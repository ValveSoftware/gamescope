// DRM output stuff

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <poll.h>

#include "wlr_begin.hpp"
#include <wlr/types/wlr_buffer.h>
#include "wlr_end.hpp"

#include "drm.hpp"
#include "defer.hpp"
#include "main.hpp"
#include "modegen.hpp"
#include "vblankmanager.hpp"
#include "wlserver.hpp"
#include "log.hpp"

#include "gpuvis_trace_utils.h"
#include "steamcompmgr.hpp"

#include <algorithm>
#include <thread>
#include <unordered_set>

extern "C" {
#include "libdisplay-info/info.h"
#include "libdisplay-info/edid.h"
#include "libdisplay-info/cta.h"
}

#include "gamescope-control-protocol.h"

using namespace std::literals;

struct drm_t g_DRM = {};

uint32_t g_nDRMFormat = DRM_FORMAT_INVALID;
uint32_t g_nDRMFormatOverlay = DRM_FORMAT_INVALID; // for partial composition, we may have more limited formats than base planes + alpha.
bool g_bRotated = false;
bool g_bDebugLayers = false;
enum drm_panel_type g_PanelType = PANEL_TYPE_EXTERNAL;
bool g_bPanelTypeFaked = false;
const char *g_sOutputName = nullptr;

#ifndef DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP
#define DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP 0x15
#endif

struct drm_color_ctm2 {
	/*
	 * Conversion matrix in S31.32 sign-magnitude
	 * (not two's complement!) format.
	 */
	__u64 matrix[12];
};

bool g_bSupportsAsyncFlips = false;

gamescope::GamescopeModeGeneration g_eGamescopeModeGeneration = gamescope::GAMESCOPE_MODE_GENERATE_CVT;
enum g_panel_orientation g_drmModeOrientation = PANEL_ORIENTATION_AUTO;
enum g_panel_external_orientation g_drmModeExternalOrientation = PANEL_EXTERNAL_ORIENTATION_AUTO;

std::atomic<uint64_t> g_drmEffectiveOrientation[gamescope::GAMESCOPE_SCREEN_TYPE_COUNT]{ {DRM_MODE_ROTATE_0}, {DRM_MODE_ROTATE_0} };

bool g_bForceDisableColorMgmt = false;

static LogScope drm_log("drm");
static LogScope drm_verbose_log("drm", LOG_SILENT);

static std::unordered_map< std::string, std::string > pnps = {};

static void drm_unset_mode( struct drm_t *drm );
static void drm_unset_connector( struct drm_t *drm );

static constexpr uint32_t s_kSteamDeckLCDRates[] =
{
	40, 41, 42, 43, 44, 45, 46, 47, 48, 49,
	50, 51, 52, 53, 54, 55, 56, 57, 58, 59,
	60,
};

static constexpr uint32_t s_kSteamDeckOLEDRates[] =
{
	45,47,48,49,
	50,51,53,55,56,59,
	60,62,64,65,66,68,
	72,73,76,77,78,
	80,81,82,84,85,86,87,88,
	90,
};

static uint32_t get_conn_display_info_flags( struct drm_t *drm, gamescope::CDRMConnector *pConnector )
{
	if ( !pConnector )
		return 0;

	uint32_t flags = 0;
	if ( pConnector->GetScreenType() == gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL )
		flags |= GAMESCOPE_CONTROL_DISPLAY_FLAG_INTERNAL_DISPLAY;
	if ( pConnector->IsVRRCapable() )
		flags |= GAMESCOPE_CONTROL_DISPLAY_FLAG_SUPPORTS_VRR;
	if ( pConnector->GetHDRInfo().bExposeHDRSupport )
		flags |= GAMESCOPE_CONTROL_DISPLAY_FLAG_SUPPORTS_HDR;

	return flags;
}

void drm_send_gamescope_control(wl_resource *control, struct drm_t *drm)
{
	// assumes wlserver_lock HELD!

	if ( !drm->pConnector )
		return;

	auto& conn = drm->pConnector;

	uint32_t flags = get_conn_display_info_flags( drm, drm->pConnector );

	struct wl_array display_rates;
	wl_array_init(&display_rates);
	if ( conn->GetValidDynamicRefreshRates().size() )
	{
		size_t size = conn->GetValidDynamicRefreshRates().size() * sizeof(uint32_t);
		uint32_t *ptr = (uint32_t *)wl_array_add( &display_rates, size );
		memcpy( ptr, conn->GetValidDynamicRefreshRates().data(), size );
	}
	gamescope_control_send_active_display_info( control, drm->pConnector->GetName(), drm->pConnector->GetMake(), drm->pConnector->GetModel(), flags, &display_rates );
	wl_array_release(&display_rates);
}

static void update_connector_display_info_wl(struct drm_t *drm)
{
	wlserver_lock();
	for ( const auto &control : wlserver.gamescope_controls )
	{
		drm_send_gamescope_control(control, drm);
	}
	wlserver_unlock();
}

inline uint64_t drm_calc_s31_32(float val)
{
	// S31.32 sign-magnitude
	float integral = 0.0f;
	float fractional = modf( fabsf( val ), &integral );

	union
	{
		struct
		{
			uint64_t fractional : 32;
			uint64_t integral   : 31;
			uint64_t sign_part  : 1;
		} s31_32_bits;
		uint64_t s31_32;
	} color;

	color.s31_32_bits.sign_part  = val < 0 ? 1 : 0;
	color.s31_32_bits.integral   = uint64_t( integral );
	color.s31_32_bits.fractional = uint64_t( fractional * float( 1ull << 32 ) );

	return color.s31_32;
}


static struct fb& get_fb( struct drm_t& drm, uint32_t id )
{
	std::lock_guard<std::mutex> m( drm.fb_map_mutex );
	return drm.fb_map[ id ];
}

static gamescope::CDRMCRTC *find_crtc_for_connector( struct drm_t *drm, gamescope::CDRMConnector *pConnector )
{
	for ( std::unique_ptr< gamescope::CDRMCRTC > &pCRTC : drm->crtcs )
	{
		if ( pConnector->GetPossibleCRTCMask() & pCRTC->GetCRTCMask() )
			return pCRTC.get();
	}

	return nullptr;
}

static bool get_plane_formats( struct drm_t *drm, gamescope::CDRMPlane *pPlane, struct wlr_drm_format_set *pFormatSet )
{
	for ( uint32_t i = 0; i < pPlane->GetModePlane()->count_formats; i++ )
	{
		const uint32_t uFormat = pPlane->GetModePlane()->formats[ i ];
		wlr_drm_format_set_add( pFormatSet, uFormat, DRM_FORMAT_MOD_INVALID );
	}

	if ( pPlane->GetProperties().IN_FORMATS )
	{
		const uint64_t ulBlobId = pPlane->GetProperties().IN_FORMATS->GetCurrentValue();

		drmModePropertyBlobRes *pBlob = drmModeGetPropertyBlob( drm->fd, ulBlobId );
		if ( !pBlob )
		{
			drm_log.errorf_errno("drmModeGetPropertyBlob(IN_FORMATS) failed");
			return false;
		}
		defer( drmModeFreePropertyBlob( pBlob ) );

		drm_format_modifier_blob *pModifierBlob = reinterpret_cast<drm_format_modifier_blob *>( pBlob->data );

		uint32_t *pFormats = reinterpret_cast<uint32_t *>( reinterpret_cast<uint8_t *>( pBlob->data ) + pModifierBlob->formats_offset );
		drm_format_modifier *pMods = reinterpret_cast<drm_format_modifier *>( reinterpret_cast<uint8_t *>( pBlob->data ) + pModifierBlob->modifiers_offset );

		for ( uint32_t i = 0; i < pModifierBlob->count_modifiers; i++ )
		{
			for ( uint32_t j = 0; j < 64; j++ )
			{
				if ( pMods[i].formats & ( uint64_t(1) << j ) )
					wlr_drm_format_set_add( pFormatSet, pFormats[j + pMods[i].offset], pMods[i].modifier );
			}
		}
	}

	return true;
}

static uint32_t pick_plane_format( const struct wlr_drm_format_set *formats, uint32_t Xformat, uint32_t Aformat )
{
	uint32_t result = DRM_FORMAT_INVALID;
	for ( size_t i = 0; i < formats->len; i++ ) {
		uint32_t fmt = formats->formats[i].format;
		if ( fmt == Xformat ) {
			// Prefer formats without alpha channel for main plane
			result = fmt;
		} else if ( result == DRM_FORMAT_INVALID && fmt == Aformat ) {
			result = fmt;
		}
	}
	return result;
}

/* Pick a primary plane that can be connected to the chosen CRTC. */
static gamescope::CDRMPlane *find_primary_plane(struct drm_t *drm)
{
	if ( !drm->pCRTC )
		return nullptr;

	for ( std::unique_ptr< gamescope::CDRMPlane > &pPlane : drm->planes )
	{
		if ( pPlane->GetModePlane()->possible_crtcs & drm->pCRTC->GetCRTCMask() )
		{
			if ( pPlane->GetProperties().type->GetCurrentValue() == DRM_PLANE_TYPE_PRIMARY )
				return pPlane.get();
		}
	}

	return nullptr;
}

static void drm_unlock_fb_internal( struct drm_t *drm, struct fb *fb );

std::atomic<uint64_t> g_nCompletedPageFlipCount = { 0u };

extern void mangoapp_output_update( uint64_t vblanktime );
static void page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, unsigned int crtc_id, void *data)
{
	uint64_t flipcount = (uint64_t)data;
	g_nCompletedPageFlipCount = flipcount;

	if ( !g_DRM.pCRTC )
		return;

	if ( g_DRM.pCRTC->GetObjectId() != crtc_id )
		return;

	// This is the last vblank time
	uint64_t vblanktime = sec * 1'000'000'000lu + usec * 1'000lu;
	g_VBlankTimer.MarkVBlank( vblanktime, true );

	// TODO: get the fbids_queued instance from data if we ever have more than one in flight

	drm_verbose_log.debugf("page_flip_handler %" PRIu64, flipcount);
	gpuvis_trace_printf("page_flip_handler %" PRIu64, flipcount);

	for ( uint32_t i = 0; i < g_DRM.fbids_on_screen.size(); i++ )
	{
		uint32_t previous_fbid = g_DRM.fbids_on_screen[ i ];
		assert( previous_fbid != 0 );

		struct fb &previous_fb = get_fb( g_DRM, previous_fbid );

		if ( --previous_fb.n_refs == 0 )
		{
			// we flipped away from this previous fbid, now safe to delete
			std::lock_guard<std::mutex> lock( g_DRM.free_queue_lock );

			for ( uint32_t i = 0; i < g_DRM.fbid_unlock_queue.size(); i++ )
			{
				if ( g_DRM.fbid_unlock_queue[ i ] == previous_fbid )
				{
					drm_verbose_log.debugf("deferred unlock %u", previous_fbid);

					drm_unlock_fb_internal( &g_DRM, &get_fb( g_DRM, previous_fbid ) );

					g_DRM.fbid_unlock_queue.erase( g_DRM.fbid_unlock_queue.begin() + i );
					break;
				}
			}

			for ( uint32_t i = 0; i < g_DRM.fbid_free_queue.size(); i++ )
			{
				if ( g_DRM.fbid_free_queue[ i ] == previous_fbid )
				{
					drm_verbose_log.debugf( "deferred free %u", previous_fbid );

					drm_drop_fbid( &g_DRM, previous_fbid );

					g_DRM.fbid_free_queue.erase( g_DRM.fbid_free_queue.begin() + i );
					break;
				}
			}
		}
	}

	g_DRM.fbids_on_screen = g_DRM.fbids_queued;
	g_DRM.fbids_queued.clear();

	g_DRM.flip_lock.unlock();

	mangoapp_output_update( vblanktime );
}

void flip_handler_thread_run(void)
{
	pthread_setname_np( pthread_self(), "gamescope-kms" );

	struct pollfd pollfd = {
		.fd = g_DRM.fd,
		.events = POLLIN,
	};

	while ( true )
	{
		int ret = poll( &pollfd, 1, -1 );
		if ( ret < 0 ) {
			drm_log.errorf_errno( "polling for DRM events failed" );
			break;
		}

		drmEventContext evctx = {
			.version = 3,
			.page_flip_handler2 = page_flip_handler,
		};
		drmHandleEvent(g_DRM.fd, &evctx);
	}
}

static constexpr uint32_t EDID_MAX_BLOCK_COUNT = 256;
static constexpr uint32_t EDID_BLOCK_SIZE = 128;
static constexpr uint32_t EDID_MAX_STANDARD_TIMING_COUNT = 8;
static constexpr uint32_t EDID_BYTE_DESCRIPTOR_COUNT = 4;
static constexpr uint32_t EDID_BYTE_DESCRIPTOR_SIZE = 18;
static constexpr uint32_t EDID_MAX_DESCRIPTOR_STANDARD_TIMING_COUNT = 6;
static constexpr uint32_t EDID_MAX_DESCRIPTOR_COLOR_POINT_COUNT = 2;
static constexpr uint32_t EDID_MAX_DESCRIPTOR_ESTABLISHED_TIMING_III_COUNT = 44;
static constexpr uint32_t EDID_MAX_DESCRIPTOR_CVT_TIMING_CODES_COUNT = 4;

static inline uint8_t get_bit_range(uint8_t val, size_t high, size_t low)
{
	size_t n;
	uint8_t bitmask;

	assert(high <= 7 && high >= low);

	n = high - low + 1;
	bitmask = (uint8_t) ((1 << n) - 1);
	return (uint8_t) (val >> low) & bitmask;
}

static inline void set_bit_range(uint8_t *val, size_t high, size_t low, uint8_t bits)
{
	size_t n;
	uint8_t bitmask;

	assert(high <= 7 && high >= low);

	n = high - low + 1;
	bitmask = (uint8_t) ((1 << n) - 1);
	assert((bits & ~bitmask) == 0);

	*val |= (uint8_t)(bits << low);
}


static inline void patch_edid_checksum(uint8_t* block)
{
	uint8_t sum = 0;
	for (uint32_t i = 0; i < EDID_BLOCK_SIZE - 1; i++)
		sum += block[i];

	uint8_t checksum = uint32_t(256) - uint32_t(sum);

	block[127] = checksum;
}

static bool validate_block_checksum(const uint8_t* data)
{
	uint8_t sum = 0;
	size_t i;

	for (i = 0; i < EDID_BLOCK_SIZE; i++) {
		sum += data[i];
	}

	return sum == 0;
}

const char *drm_get_patched_edid_path()
{
	return getenv("GAMESCOPE_PATCHED_EDID_FILE");
}

static uint8_t encode_max_luminance(float nits)
{
	if (nits == 0.0f)
		return 0;

	return ceilf((logf(nits / 50.0f) / logf(2.0f)) * 32.0f);
}

static void create_patched_edid( const uint8_t *orig_data, size_t orig_size, drm_t *drm, gamescope::CDRMConnector *conn )
{
	// A zero length indicates that the edid parsing failed.
	if (orig_size == 0) {
		return;
	}

	std::vector<uint8_t> edid(orig_data, orig_data + orig_size);

	if ( g_bRotated )
	{
		// Patch width, height.
		drm_log.infof("[patched edid] Patching dims %ux%u -> %ux%u", edid[0x15], edid[0x16], edid[0x16], edid[0x15]);
		std::swap(edid[0x15], edid[0x16]);

		for (uint32_t i = 0; i < EDID_BYTE_DESCRIPTOR_COUNT; i++)
		{
			uint8_t *byte_desc_data = &edid[0x36 + i * EDID_BYTE_DESCRIPTOR_SIZE];
			if (byte_desc_data[0] || byte_desc_data[1])
			{
				uint32_t horiz = (get_bit_range(byte_desc_data[4], 7, 4) << 8) | byte_desc_data[2];
				uint32_t vert  = (get_bit_range(byte_desc_data[7], 7, 4) << 8) | byte_desc_data[5];
				drm_log.infof("[patched edid] Patching res %ux%u -> %ux%u", horiz, vert, vert, horiz);
				std::swap(byte_desc_data[4], byte_desc_data[7]);
				std::swap(byte_desc_data[2], byte_desc_data[5]);
				break;
			}
		}

		patch_edid_checksum(&edid[0]);
	}

	// If we are debugging HDR support lazily on a regular Deck,
	// just hotpatch the edid for the game so we get values we want as if we had
	// an external display attached.
	// (Allows for debugging undocked fallback without undocking/redocking)
	if ( conn->GetHDRInfo().ShouldPatchEDID() )
	{
		// TODO: Allow for override of min luminance
		float flMaxPeakLuminance = g_ColorMgmt.pending.hdrTonemapDisplayMetadata.BIsValid() ? 
			g_ColorMgmt.pending.hdrTonemapDisplayMetadata.flWhitePointNits :
			g_ColorMgmt.pending.flInternalDisplayBrightness;
		drm_log.infof("[edid] Patching HDR static metadata. max peak luminance/max frame avg luminance = %f nits", flMaxPeakLuminance );
		const uint8_t new_hdr_static_metadata_block[]
		{
			(1 << HDMI_EOTF_SDR) | (1 << HDMI_EOTF_TRADITIONAL_HDR) | (1 << HDMI_EOTF_ST2084), /* supported eotfs */
			1, /* type 1 */
			encode_max_luminance(flMaxPeakLuminance), /* desired content max peak luminance */
			encode_max_luminance(flMaxPeakLuminance * 0.8f), /* desired content max frame avg luminance */
			0, /* desired content min luminance -- 0 is technically "undefined" */
		};

		int ext_count = int(edid.size() / EDID_BLOCK_SIZE) - 1;
		assert(ext_count == edid[0x7E]);
		bool has_cta_block = false;
		bool has_hdr_metadata_block = false;

		for (int i = 0; i < ext_count; i++)
		{
			uint8_t *ext_data = &edid[EDID_BLOCK_SIZE + i * EDID_BLOCK_SIZE];
			uint8_t tag = ext_data[0];
			if (tag == DI_EDID_EXT_CEA)
			{
				has_cta_block = true;
				uint8_t dtd_start = ext_data[2];
				uint8_t flags = ext_data[3];
				if (dtd_start == 0)
				{
					drm_log.infof("[josh edid] Hmmmm.... dtd start is 0. Interesting... Not going further! :-(");
					continue;
				}
				if (flags != 0)
				{
					drm_log.infof("[josh edid] Hmmmm.... non-zero CTA flags. Interesting... Not going further! :-(");
					continue;
				}

				const int CTA_HEADER_SIZE = 4;
				int j = CTA_HEADER_SIZE;
				while (j < dtd_start)
				{
					uint8_t data_block_header = ext_data[j];
					uint8_t data_block_tag = get_bit_range(data_block_header, 7, 5);
					uint8_t data_block_size = get_bit_range(data_block_header, 4, 0);

					if (j + 1 + data_block_size > dtd_start)
					{
						drm_log.infof("[josh edid] Hmmmm.... CTA malformatted. Interesting... Not going further! :-(");
						break;
					}

					uint8_t *data_block = &ext_data[j + 1];
					if (data_block_tag == 7) // extended
					{
						uint8_t extended_tag = data_block[0];
						uint8_t *extended_block = &data_block[1];
						uint8_t extended_block_size = data_block_size - 1;

						if (extended_tag == 6) // hdr static
						{
							if (extended_block_size >= sizeof(new_hdr_static_metadata_block))
							{
								drm_log.infof("[josh edid] Patching existing HDR Metadata with our own!");
								memcpy(extended_block, new_hdr_static_metadata_block, sizeof(new_hdr_static_metadata_block));
								has_hdr_metadata_block = true;
							}
						}
					}

					j += 1 + data_block_size; // account for header size.
				}

				if (!has_hdr_metadata_block)
				{
					const int hdr_metadata_block_size_plus_headers = sizeof(new_hdr_static_metadata_block) + 2; // +1 for header, +1 for extended header -> +2
					drm_log.infof("[josh edid] No HDR metadata block to patch... Trying to insert one.");

					// Assert that the end of the data blocks == dtd_start
					if (dtd_start != j)
					{
						drm_log.infof("[josh edid] dtd_start != end of blocks. Giving up patching. I'm too scared to attempt it.");
					}

					// Move back the dtd to make way for our block at the end.
					uint8_t *dtd = &ext_data[dtd_start];
					memmove(dtd + hdr_metadata_block_size_plus_headers, dtd, hdr_metadata_block_size_plus_headers);
					dtd_start += hdr_metadata_block_size_plus_headers;

					// Data block is where the dtd was.
					uint8_t *data_block = dtd;

					// header
					data_block[0] = 0;
					set_bit_range(&data_block[0], 7, 5, 7); // extended tag
					set_bit_range(&data_block[0], 4, 0, sizeof(new_hdr_static_metadata_block) + 1); // size (+1 for extended header, does not include normal header)

					// extended header
					data_block[1] = 6; // hdr metadata extended tag
					memcpy(&data_block[2], new_hdr_static_metadata_block, sizeof(new_hdr_static_metadata_block));
				}

				patch_edid_checksum(ext_data);
				bool sum_valid = validate_block_checksum(ext_data);
				drm_log.infof("[josh edid] CTA Checksum valid? %s", sum_valid ? "Y" : "N");
			}
		}

		if (!has_cta_block)
		{
			drm_log.infof("[josh edid] Couldn't patch for HDR metadata as we had no CTA block! Womp womp =c");
		}
	}

	bool sum_valid = validate_block_checksum(&edid[0]);
	drm_log.infof("[josh edid] BASE Checksum valid? %s", sum_valid ? "Y" : "N");

	// Write it out then flip it over atomically.

	const char *filename = drm_get_patched_edid_path();
	if (!filename)
	{
		drm_log.errorf("[josh edid] Couldn't write patched edid. No Path.");
		return;
	}

	char filename_tmp[PATH_MAX];
	snprintf(filename_tmp, sizeof(filename_tmp), "%s.tmp", filename);

	FILE *file = fopen(filename_tmp, "wb");
	if (!file)
	{
		drm_log.errorf("[josh edid] Couldn't open file: %s", filename_tmp);
		return;
	}

	fwrite(edid.data(), 1, edid.size(), file);
	fflush(file);
	fclose(file);

	rename(filename_tmp, filename);
	drm_log.infof("[josh edid] Wrote new edid to: %s", filename);
}

void drm_update_patched_edid( drm_t *drm )
{
	if (!drm || !drm->pConnector)
		return;

	create_patched_edid(drm->pConnector->GetRawEDID().data(), drm->pConnector->GetRawEDID().size(), drm, drm->pConnector);
}

static bool refresh_state( drm_t *drm )
{
	drmModeRes *pResources = drmModeGetResources( drm->fd );
	if ( pResources == nullptr )
	{
		drm_log.errorf_errno( "drmModeGetResources failed" );
		return false;
	}
	defer( drmModeFreeResources( pResources ) );

	// Add connectors which appeared
	for ( int i = 0; i < pResources->count_connectors; i++ )
	{
		uint32_t uConnectorId = pResources->connectors[i];

		drmModeConnector *pConnector = drmModeGetConnector( drm->fd, uConnectorId );
		if ( !pConnector )
			continue;

		if ( !drm->connectors.contains( uConnectorId ) )
		{
			drm->connectors.emplace(
				std::piecewise_construct,
				std::forward_as_tuple( uConnectorId ),
				std::forward_as_tuple( pConnector ) );
		}
	}

	// Remove connectors which disappeared
	for ( auto iter = drm->connectors.begin(); iter != drm->connectors.end(); )
	{
		gamescope::CDRMConnector *pConnector = &iter->second;

		const bool bFound = std::any_of(
			pResources->connectors,
			pResources->connectors + pResources->count_connectors,
			std::bind_front( std::equal_to{}, pConnector->GetObjectId() ) );

		if ( !bFound )
		{
			drm_log.debugf( "Connector '%s' disappeared.", pConnector->GetName() );

			if ( drm->pConnector == pConnector )
			{
				drm_log.infof( "Current connector '%s' disappeared.", pConnector->GetName() );
				drm->pConnector = nullptr;
			}

			iter = drm->connectors.erase( iter );
		}
		else
			iter++;
	}

	// Re-probe connectors props and status)
	for ( auto &iter : drm->connectors )
	{
		gamescope::CDRMConnector *pConnector = &iter.second;
		pConnector->RefreshState();
	}

	for ( std::unique_ptr< gamescope::CDRMCRTC > &pCRTC : drm->crtcs )
		pCRTC->RefreshState();

	for ( std::unique_ptr< gamescope::CDRMPlane > &pPlane : drm->planes )
		pPlane->RefreshState();

	return true;
}

static bool get_resources(struct drm_t *drm)
{
	{
		drmModeRes *pResources = drmModeGetResources( drm->fd );
		if ( !pResources )
		{
			drm_log.errorf_errno( "drmModeGetResources failed" );
			return false;
		}
		defer( drmModeFreeResources( pResources ) );

		for ( int i = 0; i < pResources->count_crtcs; i++ )
		{
			drmModeCrtc *pCRTC = drmModeGetCrtc( drm->fd, pResources->crtcs[ i ] );
			if ( pCRTC )
				drm->crtcs.emplace_back( std::make_unique<gamescope::CDRMCRTC>( pCRTC, 1u << i ) );
		}
	}

	{
		drmModePlaneRes *pPlaneResources = drmModeGetPlaneResources( drm->fd );
		if ( !pPlaneResources )
		{
			drm_log.errorf_errno( "drmModeGetPlaneResources failed" );
			return false;
		}
		defer( drmModeFreePlaneResources( pPlaneResources ) );

		for ( uint32_t i = 0; i < pPlaneResources->count_planes; i++ )
		{
			drmModePlane *pPlane = drmModeGetPlane( drm->fd, pPlaneResources->planes[ i ] );
			if ( pPlane )
				drm->planes.emplace_back( std::make_unique<gamescope::CDRMPlane>( pPlane ) );
		}
	}

	return refresh_state( drm );
}

struct mode_blocklist_entry
{
	uint32_t width, height, refresh;
};

// Filter out reporting some modes that are required for
// certain certifications, but are completely useless,
// and probably don't fit the display pixel size.
static mode_blocklist_entry g_badModes[] =
{
	{ 4096, 2160, 0 },
};

static const drmModeModeInfo *find_mode( const drmModeConnector *connector, int hdisplay, int vdisplay, uint32_t vrefresh )
{
	for (int i = 0; i < connector->count_modes; i++) {
		const drmModeModeInfo *mode = &connector->modes[i];

		bool bad = false;
		for (const auto& badMode : g_badModes) {
			bad |= (badMode.width   == 0 || mode->hdisplay == badMode.width)
				&& (badMode.height  == 0 || mode->vdisplay == badMode.height)
				&& (badMode.refresh == 0 || mode->vrefresh == badMode.refresh);
		}

		if (bad)
			continue;

		if (hdisplay != 0 && hdisplay != mode->hdisplay)
			continue;
		if (vdisplay != 0 && vdisplay != mode->vdisplay)
			continue;
		if (vrefresh != 0 && vrefresh != mode->vrefresh)
			continue;

		return mode;
	}

	return NULL;
}

static std::unordered_map<std::string, int> parse_connector_priorities(const char *str)
{
	std::unordered_map<std::string, int> priorities{};
	if (!str) {
		return priorities;
	}
	int i = 0;
	char *buf = strdup(str);
	char *name = strtok(buf, ",");
	while (name) {
		priorities[name] = i;
		i++;
		name = strtok(nullptr, ",");
	}
	free(buf);
	return priorities;
}

static int get_connector_priority(struct drm_t *drm, const char *name)
{
	if (drm->connector_priorities.count(name) > 0) {
		return drm->connector_priorities[name];
	}
	if (drm->connector_priorities.count("*") > 0) {
		return drm->connector_priorities["*"];
	}
	return drm->connector_priorities.size();
}

static bool get_saved_mode(const char *description, saved_mode &mode_info)
{
	const char *mode_file = getenv("GAMESCOPE_MODE_SAVE_FILE");
	if (!mode_file || !*mode_file)
		return false;

	FILE *file = fopen(mode_file, "r");
	if (!file)
		return false;

	char line[256];
    while (fgets(line, sizeof(line), file))
	{
		char saved_description[256];
        bool valid = sscanf(line, "%255[^:]:%dx%d@%d", saved_description, &mode_info.width, &mode_info.height, &mode_info.refresh) == 4;

		if (valid && !strcmp(saved_description, description))
		{
			fclose(file);
			return true;
		}
    }
	fclose(file);
	return false;
}

static bool setup_best_connector(struct drm_t *drm, bool force, bool initial)
{
	if (drm->pConnector && drm->pConnector->GetModeConnector()->connection != DRM_MODE_CONNECTED) {
		drm_log.infof("current connector '%s' disconnected", drm->pConnector->GetName());
		drm->pConnector = nullptr;
	}

	gamescope::CDRMConnector *best = nullptr;
	int nBestPriority = INT_MAX;
	for ( auto &iter : drm->connectors )
	{
		gamescope::CDRMConnector *pConnector = &iter.second;

		if ( pConnector->GetModeConnector()->connection != DRM_MODE_CONNECTED )
			continue;

		if ( drm->force_internal && pConnector->GetScreenType() == gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL )
			continue;

		int nPriority = get_connector_priority( drm, pConnector->GetName() );
		if ( nPriority < nBestPriority )
		{
			best = pConnector;
			nBestPriority = nPriority;
		}
	}

	if (!force) {
		if ((!best && drm->pConnector) || (best && best == drm->pConnector)) {
			// Let's keep our current connector
			return true;
		}
	}

	if (best == nullptr) {
		drm_log.infof("cannot find any connected connector!");
		drm_unset_connector(drm);
		drm_unset_mode(drm);
		const struct wlserver_output_info wlserver_output_info = {
			.description = "Virtual screen",
		};
		wlserver_lock();
		wlserver_set_output_info(&wlserver_output_info);
		wlserver_unlock();
		return true;
	}

	if (!drm_set_connector(drm, best)) {
		return false;
	}

	char description[256];
	if (best->GetScreenType() == gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL) {
		snprintf(description, sizeof(description), "Internal screen");
	} else if (best->GetMake() && best->GetModel()) {
		snprintf(description, sizeof(description), "%s %s", best->GetMake(), best->GetModel());
	} else if (best->GetModel()) {
		snprintf(description, sizeof(description), "%s", best->GetModel());
	} else {
		snprintf(description, sizeof(description), "External screen");
	}

	const drmModeModeInfo *mode = nullptr;
	if ( drm->preferred_width != 0 || drm->preferred_height != 0 || drm->preferred_refresh != 0 )
	{
		mode = find_mode(best->GetModeConnector(), drm->preferred_width, drm->preferred_height, drm->preferred_refresh);
	}

	if (!mode && best->GetScreenType() == gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL) {
		saved_mode mode_info;
		if (get_saved_mode(description, mode_info))
			mode = find_mode(best->GetModeConnector(), mode_info.width, mode_info.height, mode_info.refresh);
	}

	if (!mode) {
		mode = find_mode(best->GetModeConnector(), 0, 0, 0);
	}

	if (!mode) {
		drm_log.errorf("could not find mode!");
		return false;
	}

	best->SetBaseRefresh( mode->vrefresh );

	if (!drm_set_mode(drm, mode)) {
		return false;
	}

	const struct wlserver_output_info wlserver_output_info = {
		.description = description,
		.phys_width = (int) best->GetModeConnector()->mmWidth,
		.phys_height = (int) best->GetModeConnector()->mmHeight,
	};
	wlserver_lock();
	wlserver_set_output_info(&wlserver_output_info);
	wlserver_unlock();

	if (!initial)
		create_patched_edid(best->GetRawEDID().data(), best->GetRawEDID().size(), drm, best);

	update_connector_display_info_wl( drm );

	return true;
}

void load_pnps(void)
{
	const char *filename = HWDATA_PNP_IDS;
	FILE *f = fopen(filename, "r");
	if (!f) {
		drm_log.infof("failed to open PNP IDs file at '%s'", filename);
		return;
	}

	char *line = NULL;
	size_t line_size = 0;
	while (getline(&line, &line_size, f) >= 0) {
		char *nl = strchr(line, '\n');
		if (nl) {
			*nl = '\0';
		}

		char *sep = strchr(line, '\t');
		if (!sep) {
			continue;
		}
		*sep = '\0';

		std::string id(line);
		std::string name(sep + 1);
		pnps[id] = name;
	}

	free(line);
	fclose(f);
}

bool env_to_bool(const char *env)
{
	if (!env || !*env)
		return false;

	return !!atoi(env);
}

bool init_drm(struct drm_t *drm, int width, int height, int refresh)
{
	load_pnps();

	drm->bUseLiftoff = true;

	drm->preferred_width = width;
	drm->preferred_height = height;
	drm->preferred_refresh = refresh;

	drm->device_name = nullptr;
	dev_t dev_id = 0;
	if (vulkan_primary_dev_id(&dev_id)) {
		drmDevice *drm_dev = nullptr;
		if (drmGetDeviceFromDevId(dev_id, 0, &drm_dev) != 0) {
			drm_log.errorf("Failed to find DRM device with device ID %" PRIu64, (uint64_t)dev_id);
			return false;
		}
		assert(drm_dev->available_nodes & (1 << DRM_NODE_PRIMARY));
		drm->device_name = strdup(drm_dev->nodes[DRM_NODE_PRIMARY]);
		drm_log.infof("opening DRM node '%s'", drm->device_name);
	}
	else
	{
		drm_log.infof("warning: picking an arbitrary DRM device");
	}

	drm->fd = wlsession_open_kms( drm->device_name );
	if ( drm->fd < 0 )
	{
		drm_log.errorf("Could not open KMS device");
		return false;
	}

	if (drmSetClientCap(drm->fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
		drm_log.errorf("drmSetClientCap(ATOMIC) failed");
		return false;
	}

	if (drmGetCap(drm->fd, DRM_CAP_CURSOR_WIDTH, &drm->cursor_width) != 0) {
		drm->cursor_width = 64;
	}
	if (drmGetCap(drm->fd, DRM_CAP_CURSOR_HEIGHT, &drm->cursor_height) != 0) {
		drm->cursor_height = 64;
	}

	uint64_t cap;
	if (drmGetCap(drm->fd, DRM_CAP_ADDFB2_MODIFIERS, &cap) == 0 && cap != 0) {
		drm->allow_modifiers = true;
	}

	g_bSupportsAsyncFlips = drmGetCap(drm->fd, DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP, &cap) == 0 && cap != 0;
	if (!g_bSupportsAsyncFlips)
		drm_log.errorf("Immediate flips are not supported by the KMS driver");

	static bool async_disabled = env_to_bool(getenv("GAMESCOPE_DISABLE_ASYNC_FLIPS"));

	if ( async_disabled )
	{
		g_bSupportsAsyncFlips = false;
		drm_log.errorf("Immediate flips disabled from environment");
	}

	if (!get_resources(drm)) {
		return false;
	}

	drm->lo_device = liftoff_device_create( drm->fd );
	if ( drm->lo_device == nullptr )
		return false;
	if ( liftoff_device_register_all_planes( drm->lo_device ) < 0 )
		return false;

	drm_log.infof("Connectors:");
	for ( auto &iter : drm->connectors )
	{
		gamescope::CDRMConnector *pConnector = &iter.second;

		const char *status_str = "disconnected";
		if ( pConnector->GetModeConnector()->connection == DRM_MODE_CONNECTED )
			status_str = "connected";

		drm_log.infof("  %s (%s)", pConnector->GetName(), status_str);
	}

	drm->connector_priorities = parse_connector_priorities( g_sOutputName );

	if (!setup_best_connector(drm, true, true)) {
		return false;
	}

	// Fetch formats which can be scanned out
	for ( std::unique_ptr< gamescope::CDRMPlane > &pPlane : drm->planes )
	{
		if ( !get_plane_formats( drm, pPlane.get(), &drm->formats ) )
			return false;
	}

	// TODO: intersect primary planes formats instead
	if ( !drm->pPrimaryPlane )
		drm->pPrimaryPlane = find_primary_plane( drm );

	if ( !drm->pPrimaryPlane )
	{
		drm_log.errorf("Failed to find a primary plane");
		return false;
	}

	if ( !get_plane_formats( drm, drm->pPrimaryPlane, &drm->primary_formats ) )
	{
		return false;
	}

	// Pick a 10-bit format at first for our composition buffer, for a couple of reasons:
	//
	// 1. Many game engines automatically render to 10-bit formats such as UE4 which means
	// that when we have to composite, we can keep the same HW dithering that we would get if
	// we just scanned them out directly.
	//
	// 2. When compositing HDR content as a fallback when we undock, it avoids introducing
	// a bunch of horrible banding when going to G2.2 curve.
	// It ensures that we can dither that.
	g_nDRMFormat = pick_plane_format(&drm->primary_formats, DRM_FORMAT_XRGB2101010, DRM_FORMAT_ARGB2101010);
	if ( g_nDRMFormat == DRM_FORMAT_INVALID ) {
		g_nDRMFormat = pick_plane_format(&drm->primary_formats, DRM_FORMAT_XBGR2101010, DRM_FORMAT_ABGR2101010);
		if ( g_nDRMFormat == DRM_FORMAT_INVALID ) {
			g_nDRMFormat = pick_plane_format(&drm->primary_formats, DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888);
			if ( g_nDRMFormat == DRM_FORMAT_INVALID ) {
				drm_log.errorf("Primary plane doesn't support any formats >= 8888");
				return false;
			}
		}
	}

	// ARGB8888 is the Xformat and AFormat here in this function as we want transparent overlay
	g_nDRMFormatOverlay = pick_plane_format(&drm->primary_formats, DRM_FORMAT_ARGB2101010, DRM_FORMAT_ARGB2101010);
	if ( g_nDRMFormatOverlay == DRM_FORMAT_INVALID ) {
		g_nDRMFormatOverlay = pick_plane_format(&drm->primary_formats, DRM_FORMAT_ABGR2101010, DRM_FORMAT_ABGR2101010);
		if ( g_nDRMFormatOverlay == DRM_FORMAT_INVALID ) {
			g_nDRMFormatOverlay = pick_plane_format(&drm->primary_formats, DRM_FORMAT_ARGB8888, DRM_FORMAT_ARGB8888);
			if ( g_nDRMFormatOverlay == DRM_FORMAT_INVALID ) {
				drm_log.errorf("Overlay plane doesn't support any formats >= 8888");
				return false;
			}
		}
	}

	drm->kms_in_fence_fd = -1;

	std::thread flip_handler_thread( flip_handler_thread_run );
	flip_handler_thread.detach();

	if ( drm->bUseLiftoff )
		liftoff_log_set_priority(g_bDebugLayers ? LIFTOFF_DEBUG : LIFTOFF_ERROR);

	hdr_output_metadata sdr_metadata;
	memset(&sdr_metadata, 0, sizeof(sdr_metadata));
	drm->sdr_static_metadata = drm_create_hdr_metadata_blob(drm, &sdr_metadata);

	drm->flipcount = 0;
	drm->needs_modeset = true;

	return true;
}

void finish_drm(struct drm_t *drm)
{
	// Disable all connectors, CRTCs and planes. This is necessary to leave a
	// clean KMS state behind. Some other KMS clients might not support all of
	// the properties we use, e.g. "rotation" and Xorg don't play well
	// together.

	drmModeAtomicReq *req = drmModeAtomicAlloc();

	for ( auto &iter : drm->connectors )
	{
		gamescope::CDRMConnector *pConnector = &iter.second;

		pConnector->GetProperties().CRTC_ID->SetPendingValue( req, 0, true );

		if ( pConnector->GetProperties().Colorspace )
			pConnector->GetProperties().Colorspace->SetPendingValue( req, 0, true );

		if ( pConnector->GetProperties().HDR_OUTPUT_METADATA )
		{
			if ( drm->sdr_static_metadata && pConnector->GetHDRInfo().IsHDR10() )
				pConnector->GetProperties().HDR_OUTPUT_METADATA->SetPendingValue( req, drm->sdr_static_metadata->blob, true );
			else
				pConnector->GetProperties().HDR_OUTPUT_METADATA->SetPendingValue( req, 0, true );
		}

		if ( pConnector->GetProperties().content_type )
			pConnector->GetProperties().content_type->SetPendingValue( req, 0, true );
	}

	for ( std::unique_ptr< gamescope::CDRMCRTC > &pCRTC : drm->crtcs )
	{
		pCRTC->GetProperties().ACTIVE->SetPendingValue( req, 0, true );
		pCRTC->GetProperties().MODE_ID->SetPendingValue( req, 0, true );

		if ( pCRTC->GetProperties().GAMMA_LUT )
			pCRTC->GetProperties().GAMMA_LUT->SetPendingValue( req, 0, true );

		if ( pCRTC->GetProperties().DEGAMMA_LUT )
			pCRTC->GetProperties().DEGAMMA_LUT->SetPendingValue( req, 0, true );

		if ( pCRTC->GetProperties().CTM )
			pCRTC->GetProperties().CTM->SetPendingValue( req, 0, true );

		if ( pCRTC->GetProperties().VRR_ENABLED )
			pCRTC->GetProperties().VRR_ENABLED->SetPendingValue( req, 0, true );

		if ( pCRTC->GetProperties().OUT_FENCE_PTR )
			pCRTC->GetProperties().OUT_FENCE_PTR->SetPendingValue( req, 0, true );

		if ( pCRTC->GetProperties().VALVE1_CRTC_REGAMMA_TF )
			pCRTC->GetProperties().VALVE1_CRTC_REGAMMA_TF->SetPendingValue( req, 0, true );
	}

	for ( std::unique_ptr< gamescope::CDRMPlane > &pPlane : drm->planes )
	{
		pPlane->GetProperties().FB_ID->SetPendingValue( req, 0, true );
		pPlane->GetProperties().CRTC_ID->SetPendingValue( req, 0, true );
		pPlane->GetProperties().SRC_X->SetPendingValue( req, 0, true );
		pPlane->GetProperties().SRC_Y->SetPendingValue( req, 0, true );
		pPlane->GetProperties().SRC_W->SetPendingValue( req, 0, true );
		pPlane->GetProperties().SRC_H->SetPendingValue( req, 0, true );
		pPlane->GetProperties().CRTC_X->SetPendingValue( req, 0, true );
		pPlane->GetProperties().CRTC_Y->SetPendingValue( req, 0, true );
		pPlane->GetProperties().CRTC_W->SetPendingValue( req, 0, true );
		pPlane->GetProperties().CRTC_H->SetPendingValue( req, 0, true );

		if ( pPlane->GetProperties().rotation )
			pPlane->GetProperties().rotation->SetPendingValue( req, DRM_MODE_ROTATE_0, true );

		if ( pPlane->GetProperties().alpha )
			pPlane->GetProperties().alpha->SetPendingValue( req, 0xFFFF, true );

		//if ( pPlane->GetProperties().zpos )
		//	pPlane->GetProperties().zpos->SetPendingValue( req, , true );

		if ( pPlane->GetProperties().VALVE1_PLANE_DEGAMMA_TF )
			pPlane->GetProperties().VALVE1_PLANE_DEGAMMA_TF->SetPendingValue( req, DRM_VALVE1_TRANSFER_FUNCTION_DEFAULT, true );

		if ( pPlane->GetProperties().VALVE1_PLANE_DEGAMMA_LUT )
			pPlane->GetProperties().VALVE1_PLANE_DEGAMMA_LUT->SetPendingValue( req, 0, true );

		if ( pPlane->GetProperties().VALVE1_PLANE_CTM )
			pPlane->GetProperties().VALVE1_PLANE_CTM->SetPendingValue( req, 0, true );

		if ( pPlane->GetProperties().VALVE1_PLANE_HDR_MULT )
			pPlane->GetProperties().VALVE1_PLANE_HDR_MULT->SetPendingValue( req, 0x100000000ULL, true );

		if ( pPlane->GetProperties().VALVE1_PLANE_SHAPER_TF )
			pPlane->GetProperties().VALVE1_PLANE_SHAPER_TF->SetPendingValue( req, DRM_VALVE1_TRANSFER_FUNCTION_DEFAULT, true );

		if ( pPlane->GetProperties().VALVE1_PLANE_SHAPER_LUT )
			pPlane->GetProperties().VALVE1_PLANE_SHAPER_LUT->SetPendingValue( req, 0, true );

		if ( pPlane->GetProperties().VALVE1_PLANE_LUT3D )
			pPlane->GetProperties().VALVE1_PLANE_LUT3D->SetPendingValue( req, 0, true );

		if ( pPlane->GetProperties().VALVE1_PLANE_BLEND_TF )
			pPlane->GetProperties().VALVE1_PLANE_BLEND_TF->SetPendingValue( req, DRM_VALVE1_TRANSFER_FUNCTION_DEFAULT, true );

		if ( pPlane->GetProperties().VALVE1_PLANE_BLEND_LUT )
			pPlane->GetProperties().VALVE1_PLANE_BLEND_LUT->SetPendingValue( req, 0, true );
	}

	// We can't do a non-blocking commit here or else risk EBUSY in case the
	// previous page-flip is still in flight.
	uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
	int ret = drmModeAtomicCommit( drm->fd, req, flags, nullptr );
	if ( ret != 0 ) {
		drm_log.errorf_errno( "finish_drm: drmModeAtomicCommit failed" );
	}
	drmModeAtomicFree(req);

	free(drm->device_name);

	// We can't close the DRM FD here, it might still be in use by the
	// page-flip handler thread.
}

int drm_commit(struct drm_t *drm, const struct FrameInfo_t *frameInfo )
{
	int ret;

	assert( drm->req != nullptr );

// 	if (drm->kms_in_fence_fd != -1) {
// 		add_plane_property(req, plane_id, "IN_FENCE_FD", drm->kms_in_fence_fd);
// 	}

// 	drm->kms_out_fence_fd = -1;

// 	add_crtc_property(req, drm->crtc_id, "OUT_FENCE_PTR",
// 					  (uint64_t)(unsigned long)&drm->kms_out_fence_fd);


	assert( drm->fbids_queued.size() == 0 );

	bool isPageFlip = drm->flags & DRM_MODE_PAGE_FLIP_EVENT;

	if ( isPageFlip ) {
		drm->flip_lock.lock();

		// Do it before the commit, as otherwise the pageflip handler could
		// potentially beat us to the refcount checks.
		for ( uint32_t i = 0; i < drm->fbids_in_req.size(); i++ )
		{
			struct fb &fb = get_fb( g_DRM, drm->fbids_in_req[ i ] );
			assert( fb.held_refs );
			fb.n_refs++;
		}

		drm->fbids_queued = drm->fbids_in_req;
	}

	g_DRM.flipcount++;

	drm_verbose_log.debugf("flip commit %" PRIu64, (uint64_t)g_DRM.flipcount);
	gpuvis_trace_printf( "flip commit %" PRIu64, (uint64_t)g_DRM.flipcount );

	ret = drmModeAtomicCommit(drm->fd, drm->req, drm->flags, (void*)(uint64_t)g_DRM.flipcount );
	if ( ret != 0 )
	{
		drm_log.errorf_errno( "flip error" );

		if ( ret != -EBUSY && ret != -EACCES )
		{
			drm_log.errorf( "fatal flip error, aborting" );
			if ( isPageFlip )
				drm->flip_lock.unlock();
			abort();
		}

		drm->pending = drm->current;

		for ( std::unique_ptr< gamescope::CDRMCRTC > &pCRTC : drm->crtcs )
		{
			for ( std::optional<gamescope::CDRMAtomicProperty> &oProperty : pCRTC->GetProperties() )
			{
				if ( oProperty )
					oProperty->Rollback();
			}
		}

		for ( std::unique_ptr< gamescope::CDRMPlane > &pPlane : drm->planes )
		{
			for ( std::optional<gamescope::CDRMAtomicProperty> &oProperty : pPlane->GetProperties() )
			{
				if ( oProperty )
					oProperty->Rollback();
			}
		}

		for ( auto &iter : drm->connectors )
		{
			gamescope::CDRMConnector *pConnector = &iter.second;
			for ( std::optional<gamescope::CDRMAtomicProperty> &oProperty : pConnector->GetProperties() )
			{
				if ( oProperty )
					oProperty->Rollback();
			}
		}

		// Undo refcount if the commit didn't actually work
		for ( uint32_t i = 0; i < drm->fbids_in_req.size(); i++ )
		{
			get_fb( g_DRM, drm->fbids_in_req[ i ] ).n_refs--;
		}

		drm->fbids_queued.clear();

		g_DRM.flipcount--;

		if ( isPageFlip )
			drm->flip_lock.unlock();

		goto out;
	} else {
		drm->fbids_in_req.clear();

		drm->current = drm->pending;

		for ( std::unique_ptr< gamescope::CDRMCRTC > &pCRTC : drm->crtcs )
		{
			for ( std::optional<gamescope::CDRMAtomicProperty> &oProperty : pCRTC->GetProperties() )
			{
				if ( oProperty )
					oProperty->OnCommit();
			}
		}

		for ( std::unique_ptr< gamescope::CDRMPlane > &pPlane : drm->planes )
		{
			for ( std::optional<gamescope::CDRMAtomicProperty> &oProperty : pPlane->GetProperties() )
			{
				if ( oProperty )
					oProperty->OnCommit();
			}
		}

		for ( auto &iter : drm->connectors )
		{
			gamescope::CDRMConnector *pConnector = &iter.second;
			for ( std::optional<gamescope::CDRMAtomicProperty> &oProperty : pConnector->GetProperties() )
			{
				if ( oProperty )
					oProperty->OnCommit();
			}
		}
	}

	// Update the draw time
	// Ideally this would be updated by something right before the page flip
	// is queued and would end up being the new page flip, rather than here.
	// However, the page flip handler is called when the page flip occurs,
	// not when it is successfully queued.
	g_VBlankTimer.UpdateLastDrawTime( get_time_in_nanos() - g_SteamCompMgrVBlankTime.ulWakeupTime );

	if ( isPageFlip ) {
		// Wait for flip handler to unlock
		drm->flip_lock.lock();
		drm->flip_lock.unlock();
	}

// 	if (drm->kms_in_fence_fd != -1) {
// 		close(drm->kms_in_fence_fd);
// 		drm->kms_in_fence_fd = -1;
// 	}
//
// 	drm->kms_in_fence_fd = drm->kms_out_fence_fd;

out:
	drmModeAtomicFree( drm->req );
	drm->req = nullptr;

	return ret;
}

uint32_t drm_fbid_from_dmabuf( struct drm_t *drm, struct wlr_buffer *buf, struct wlr_dmabuf_attributes *dma_buf )
{
	uint32_t fb_id = 0;

	if ( !wlr_drm_format_set_has( &drm->formats, dma_buf->format, dma_buf->modifier ) )
	{
		drm_verbose_log.errorf( "Cannot import FB to DRM: format 0x%" PRIX32 " and modifier 0x%" PRIX64 " not supported for scan-out", dma_buf->format, dma_buf->modifier );
		return 0;
	}

	uint32_t handles[4] = {0};
	uint64_t modifiers[4] = {0};
	for ( int i = 0; i < dma_buf->n_planes; i++ ) {
		if ( drmPrimeFDToHandle( drm->fd, dma_buf->fd[i], &handles[i] ) != 0 )
		{
			drm_log.errorf_errno("drmPrimeFDToHandle failed");
			goto out;
		}

		/* KMS requires all planes to have the same modifier */
		modifiers[i] = dma_buf->modifier;
	}

	if ( dma_buf->modifier != DRM_FORMAT_MOD_INVALID )
	{
		if ( !drm->allow_modifiers )
		{
			drm_log.errorf("Cannot import DMA-BUF: has a modifier (0x%" PRIX64 "), but KMS doesn't support them", dma_buf->modifier);
			goto out;
		}

		if ( drmModeAddFB2WithModifiers( drm->fd, dma_buf->width, dma_buf->height, dma_buf->format, handles, dma_buf->stride, dma_buf->offset, modifiers, &fb_id, DRM_MODE_FB_MODIFIERS ) != 0 )
		{
			drm_log.errorf_errno("drmModeAddFB2WithModifiers failed");
			goto out;
		}
	}
	else
	{
		if ( drmModeAddFB2( drm->fd, dma_buf->width, dma_buf->height, dma_buf->format, handles, dma_buf->stride, dma_buf->offset, &fb_id, 0 ) != 0 )
		{
			drm_log.errorf_errno("drmModeAddFB2 failed");
			goto out;
		}
	}

	drm_verbose_log.debugf("make fbid %u", fb_id);

	/* Nested scope so fb doesn't end up in the out: label */
	{
		struct fb &fb = get_fb( *drm, fb_id );
		assert( fb.held_refs == 0 );
		fb.id = fb_id;
		fb.buf = buf;
		if (!buf)
			fb.held_refs++;
		fb.n_refs = 0;
	}

out:
	for ( int i = 0; i < dma_buf->n_planes; i++ ) {
		if ( handles[i] == 0 )
			continue;

		// GEM handles aren't ref'counted by the kernel. Two DMA-BUFs may
		// return the same GEM handle, we need to be careful not to
		// double-close them.
		bool already_closed = false;
		for ( int j = 0; j < i; j++ ) {
			if ( handles[i] == handles[j] )
				already_closed = true;
		}
		if ( already_closed )
			continue;

		struct drm_gem_close args = { .handle = handles[i] };
		if ( drmIoctl( drm->fd, DRM_IOCTL_GEM_CLOSE, &args ) != 0 ) {
			drm_log.errorf_errno( "drmIoctl(GEM_CLOSE) failed" );
		}
	}

	return fb_id;
}

void drm_drop_fbid( struct drm_t *drm, uint32_t fbid )
{
	struct fb &fb = get_fb( *drm, fbid );
	assert( fb.held_refs == 0 ||
	        fb.buf == nullptr );

	fb.held_refs = 0;

	if ( fb.n_refs != 0 )
	{
		std::lock_guard<std::mutex> lock( drm->free_queue_lock );
		drm->fbid_free_queue.push_back( fbid );
		return;
	}

	if (drmModeRmFB( drm->fd, fbid ) != 0 )
	{
		drm_log.errorf_errno( "drmModeRmFB failed" );
	}
}

static void drm_unlock_fb_internal( struct drm_t *drm, struct fb *fb )
{
	assert( fb->held_refs == 0 );
	assert( fb->n_refs == 0 );

	if ( fb->buf != nullptr )
	{
		wlserver_lock();
		wlr_buffer_unlock( fb->buf );
		wlserver_unlock();
	}
}

void drm_lock_fbid( struct drm_t *drm, uint32_t fbid )
{
	struct fb &fb = get_fb( *drm, fbid );
	assert( fb.n_refs == 0 );

	if ( fb.held_refs++ == 0 )
	{
		if ( fb.buf != nullptr )
		{
			wlserver_lock();
			wlr_buffer_lock( fb.buf );
			wlserver_unlock();
		}
	}
}

void drm_unlock_fbid( struct drm_t *drm, uint32_t fbid )
{
	struct fb &fb = get_fb( *drm, fbid );

	assert( fb.held_refs > 0 );
	if ( --fb.held_refs != 0 )
		return;

	if ( fb.n_refs != 0 )
	{
		std::lock_guard<std::mutex> lock( drm->free_queue_lock );
		drm->fbid_unlock_queue.push_back( fbid );
		return;
	}

	/* FB isn't being used in any page-flip, free it immediately */
	drm_verbose_log.debugf("free fbid %u", fbid);
	drm_unlock_fb_internal( drm, &fb );
}

static uint64_t determine_drm_orientation(struct drm_t *drm, gamescope::CDRMConnector *pConnector, const drmModeModeInfo *mode)
{
	if ( pConnector && pConnector->GetProperties().panel_orientation )
	{
		switch ( pConnector->GetProperties().panel_orientation->GetCurrentValue() )
		{
			case DRM_MODE_PANEL_ORIENTATION_NORMAL:
				return DRM_MODE_ROTATE_0;
			case DRM_MODE_PANEL_ORIENTATION_BOTTOM_UP:
				return DRM_MODE_ROTATE_180;
			case DRM_MODE_PANEL_ORIENTATION_LEFT_UP:
				return DRM_MODE_ROTATE_90;
			case DRM_MODE_PANEL_ORIENTATION_RIGHT_UP:
				return DRM_MODE_ROTATE_270;
		}
	}

	if ( pConnector->GetScreenType() == gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL && mode )
	{
		// Auto-detect portait mode for internal displays
		return mode->hdisplay < mode->vdisplay ? DRM_MODE_ROTATE_270 : DRM_MODE_ROTATE_0;
	}

	return DRM_MODE_ROTATE_0;
}

/* Handle the orientation of the display */
static void update_drm_effective_orientation(struct drm_t *drm, gamescope::CDRMConnector *pConnector, const drmModeModeInfo *mode, bool fakedExternal)
{
	gamescope::GamescopeScreenType eScreenType = pConnector->GetScreenType();

    if ( (eScreenType == gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL) || (eScreenType == gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL && fakedExternal))
	{
		// We are rotating an internal display or an internal display that is faked as external
        switch (g_drmModeOrientation)
        {
        case PANEL_ORIENTATION_0:
            g_drmEffectiveOrientation[eScreenType] = DRM_MODE_ROTATE_0;
            break;
        case PANEL_ORIENTATION_90:
            g_drmEffectiveOrientation[eScreenType] = DRM_MODE_ROTATE_90;
            break;
        case PANEL_ORIENTATION_180:
            g_drmEffectiveOrientation[eScreenType] = DRM_MODE_ROTATE_180;
            break;
        case PANEL_ORIENTATION_270:
            g_drmEffectiveOrientation[eScreenType] = DRM_MODE_ROTATE_270;
            break;
        case PANEL_ORIENTATION_AUTO:
            g_drmEffectiveOrientation[eScreenType] = determine_drm_orientation(drm, pConnector, mode);
            break;
        }
    }
    else if (eScreenType == gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL)
    {
		// We are rotating a real external dislay
		drm_log.infof("Rotating external display orientation");
        switch (g_drmModeExternalOrientation)
        {
        default:
        case PANEL_EXTERNAL_ORIENTATION_0:
            g_drmEffectiveOrientation[eScreenType] = DRM_MODE_ROTATE_0;
            break;
        case PANEL_EXTERNAL_ORIENTATION_90:
            g_drmEffectiveOrientation[eScreenType] = DRM_MODE_ROTATE_90;
            break;
        case PANEL_EXTERNAL_ORIENTATION_180:
            g_drmEffectiveOrientation[eScreenType] = DRM_MODE_ROTATE_180;
            break;
        case PANEL_EXTERNAL_ORIENTATION_270:
            g_drmEffectiveOrientation[eScreenType] = DRM_MODE_ROTATE_270;
            break;
        }
    }
	else
	{
		g_drmEffectiveOrientation[eScreenType] = determine_drm_orientation( drm, pConnector, mode );
	}
}

static void update_drm_effective_orientations( struct drm_t *drm, const drmModeModeInfo *pMode, bool panel_flag )
{
	update_drm_effective_orientation( drm, drm->pConnector, pMode, panel_flag );
}

// Only used for NV12 buffers
static drm_color_encoding drm_get_color_encoding(EStreamColorspace colorspace)
{
	switch (colorspace)
	{
		default:
		case k_EStreamColorspace_Unknown:
			return DRM_COLOR_YCBCR_BT709;

		case k_EStreamColorspace_BT601:
			return DRM_COLOR_YCBCR_BT601;
		case k_EStreamColorspace_BT601_Full:
			return DRM_COLOR_YCBCR_BT601;

		case k_EStreamColorspace_BT709:
			return DRM_COLOR_YCBCR_BT709;
		case k_EStreamColorspace_BT709_Full:
			return DRM_COLOR_YCBCR_BT709;
	}
}

static drm_color_range drm_get_color_range(EStreamColorspace colorspace)
{
	switch (colorspace)
	{
		default:
		case k_EStreamColorspace_Unknown:
			return DRM_COLOR_YCBCR_FULL_RANGE;

		case k_EStreamColorspace_BT601:
			return DRM_COLOR_YCBCR_LIMITED_RANGE;
		case k_EStreamColorspace_BT601_Full:
			return DRM_COLOR_YCBCR_FULL_RANGE;

		case k_EStreamColorspace_BT709:
			return DRM_COLOR_YCBCR_LIMITED_RANGE;
		case k_EStreamColorspace_BT709_Full:
			return DRM_COLOR_YCBCR_FULL_RANGE;
	}
}

template <typename T>
void hash_combine(size_t& s, const T& v)
{
	std::hash<T> h;
	s^= h(v) + 0x9e3779b9 + (s<< 6) + (s>> 2);
}

struct LiftoffStateCacheEntry
{
	LiftoffStateCacheEntry()
	{
		memset(this, 0, sizeof(LiftoffStateCacheEntry));
	}

    int nLayerCount;

	struct LiftoffLayerState_t
	{
		bool ycbcr;
		uint32_t zpos;
		uint32_t srcW, srcH;
		uint32_t crtcX, crtcY, crtcW, crtcH;
		drm_color_encoding colorEncoding;
		drm_color_range    colorRange;
		GamescopeAppTextureColorspace colorspace;
	} layerState[ k_nMaxLayers ];

	bool operator == (const LiftoffStateCacheEntry& entry) const
	{
		return !memcmp(this, &entry, sizeof(LiftoffStateCacheEntry));
	}
};

struct LiftoffStateCacheEntryKasher
{
	size_t operator()(const LiftoffStateCacheEntry& k) const
	{
		size_t hash = 0;
		hash_combine(hash, k.nLayerCount);
		for ( int i = 0; i < k.nLayerCount; i++ )
		{
			hash_combine(hash, k.layerState[i].ycbcr);
			hash_combine(hash, k.layerState[i].zpos);
			hash_combine(hash, k.layerState[i].srcW);
			hash_combine(hash, k.layerState[i].srcH);
			hash_combine(hash, k.layerState[i].crtcX);
			hash_combine(hash, k.layerState[i].crtcY);
			hash_combine(hash, k.layerState[i].crtcW);
			hash_combine(hash, k.layerState[i].crtcH);
			hash_combine(hash, k.layerState[i].colorEncoding);
			hash_combine(hash, k.layerState[i].colorRange);
			hash_combine(hash, k.layerState[i].colorspace);
		}

		return hash;
  	}
};


std::unordered_set<LiftoffStateCacheEntry, LiftoffStateCacheEntryKasher> g_LiftoffStateCache;

static inline drm_valve1_transfer_function colorspace_to_plane_degamma_tf(GamescopeAppTextureColorspace colorspace)
{
	switch ( colorspace )
	{
		default: // Linear in this sense is SRGB. Linear = sRGB image view doing automatic sRGB -> Linear which doesn't happen on DRM side.
		case GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB:
			return DRM_VALVE1_TRANSFER_FUNCTION_SRGB;
		case GAMESCOPE_APP_TEXTURE_COLORSPACE_PASSTHRU:
		case GAMESCOPE_APP_TEXTURE_COLORSPACE_SCRGB:
			// Use LINEAR TF for scRGB float format as 80 nit = 1.0 in scRGB, which matches
			// what PQ TF decodes to/encodes from.
			// AMD internal format is FP16, and generally expected for 1.0 -> 80 nit.
			// which just so happens to match scRGB.
			return DRM_VALVE1_TRANSFER_FUNCTION_LINEAR;
		case GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ:
			return DRM_VALVE1_TRANSFER_FUNCTION_PQ;
	}
}

static inline drm_valve1_transfer_function colorspace_to_plane_shaper_tf(GamescopeAppTextureColorspace colorspace)
{
	switch ( colorspace )
	{
		default:
		case GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB:
			return DRM_VALVE1_TRANSFER_FUNCTION_SRGB;
		case GAMESCOPE_APP_TEXTURE_COLORSPACE_SCRGB: // scRGB Linear -> PQ for shaper + 3D LUT
		case GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ:
			return DRM_VALVE1_TRANSFER_FUNCTION_PQ;
		case GAMESCOPE_APP_TEXTURE_COLORSPACE_PASSTHRU:
			return DRM_VALVE1_TRANSFER_FUNCTION_DEFAULT;
	}
}

static inline uint32_t ColorSpaceToEOTFIndex( GamescopeAppTextureColorspace colorspace )
{
	switch ( colorspace )
	{
		default:
		case GAMESCOPE_APP_TEXTURE_COLORSPACE_LINEAR: // Not actually linear, just Linear vs sRGB image views in Vulkan. Still viewed as sRGB on the DRM side.
		case GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB:
			// SDR sRGB content treated as native Gamma 22 curve. No need to do sRGB -> 2.2 or whatever.
			return EOTF_Gamma22;
		case GAMESCOPE_APP_TEXTURE_COLORSPACE_SCRGB:
			// Okay, so this is WEIRD right? OKAY Let me explain it to you.
			// The plan for scRGB content is to go from scRGB -> PQ in a SHAPER_TF
			// before indexing into the shaper. (input from colorspace_to_plane_regamma_tf!)
			return EOTF_PQ;
		case GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ:
			return EOTF_PQ;
	}
}


LiftoffStateCacheEntry FrameInfoToLiftoffStateCacheEntry( struct drm_t *drm, const FrameInfo_t *frameInfo )
{
	LiftoffStateCacheEntry entry{};

	entry.nLayerCount = frameInfo->layerCount;
	for ( int i = 0; i < entry.nLayerCount; i++ )
	{
		const uint16_t srcWidth  = frameInfo->layers[ i ].tex->width();
		const uint16_t srcHeight = frameInfo->layers[ i ].tex->height();

		int32_t crtcX = -frameInfo->layers[ i ].offset.x;
		int32_t crtcY = -frameInfo->layers[ i ].offset.y;
		uint64_t crtcW = srcWidth / frameInfo->layers[ i ].scale.x;
		uint64_t crtcH = srcHeight / frameInfo->layers[ i ].scale.y;

		if (g_bRotated)
		{
			int64_t imageH = frameInfo->layers[ i ].tex->contentHeight() / frameInfo->layers[ i ].scale.y;

			const int32_t x = crtcX;
			const uint64_t w = crtcW;
			crtcX = g_nOutputHeight - imageH - crtcY;
			crtcY = x;
			crtcW = crtcH;
			crtcH = w;
		}

		entry.layerState[i].zpos  = frameInfo->layers[ i ].zpos;
		entry.layerState[i].srcW  = srcWidth  << 16;
		entry.layerState[i].srcH  = srcHeight << 16;
		entry.layerState[i].crtcX = crtcX;
		entry.layerState[i].crtcY = crtcY;
		entry.layerState[i].crtcW = crtcW;
		entry.layerState[i].crtcH = crtcH;
		entry.layerState[i].ycbcr = frameInfo->layers[i].isYcbcr();
		if ( entry.layerState[i].ycbcr )
		{
			entry.layerState[i].colorEncoding = drm_get_color_encoding( g_ForcedNV12ColorSpace );
			entry.layerState[i].colorRange    = drm_get_color_range( g_ForcedNV12ColorSpace );
			entry.layerState[i].colorspace = GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB;
		}
		else
		{
			entry.layerState[i].colorspace = frameInfo->layers[ i ].colorspace;
		}
	}

	return entry;
}

static bool is_liftoff_caching_enabled()
{
	static bool disabled = env_to_bool(getenv("GAMESCOPE_LIFTOFF_CACHE_DISABLE"));
	return !disabled;
}

bool g_bDisableShaperAnd3DLUT = false;
bool g_bDisableDegamma = false;
bool g_bDisableRegamma = false;
bool g_bDisableBlendTF = false;

bool g_bSinglePlaneOptimizations = true;

namespace gamescope
{
	////////////////////
	// CDRMAtomicObject
	////////////////////
	CDRMAtomicObject::CDRMAtomicObject( uint32_t ulObjectId )
		: m_ulObjectId{ ulObjectId }
	{
	}


	/////////////////////////
	// CDRMAtomicTypedObject
	/////////////////////////
	template < uint32_t DRMObjectType >
	CDRMAtomicTypedObject<DRMObjectType>::CDRMAtomicTypedObject( uint32_t ulObjectId )
		: CDRMAtomicObject{ ulObjectId }
	{
	}

	template < uint32_t DRMObjectType >
	std::optional<DRMObjectRawProperties> CDRMAtomicTypedObject<DRMObjectType>::GetRawProperties()
	{
		drmModeObjectProperties *pProperties = drmModeObjectGetProperties( g_DRM.fd, m_ulObjectId, DRMObjectType );
		if ( !pProperties )
		{
			drm_log.errorf_errno( "drmModeObjectGetProperties failed" );
			return std::nullopt;
		}
		defer( drmModeFreeObjectProperties( pProperties ) );

		DRMObjectRawProperties rawProperties;
		for ( uint32_t i = 0; i < pProperties->count_props; i++ )
		{
			drmModePropertyRes *pProperty = drmModeGetProperty( g_DRM.fd, pProperties->props[ i ] );
			if ( !pProperty )
				continue;
			defer( drmModeFreeProperty( pProperty ) );

			rawProperties[ pProperty->name ] = DRMObjectRawProperty{ pProperty->prop_id, pProperties->prop_values[ i ] };
		}

		return rawProperties;
	}


	/////////////////////////
	// CDRMAtomicProperty
	/////////////////////////
	CDRMAtomicProperty::CDRMAtomicProperty( CDRMAtomicObject *pObject, DRMObjectRawProperty rawProperty )
		: m_pObject{ pObject }
		, m_uPropertyId{ rawProperty.uPropertyId }
		, m_ulPendingValue{ rawProperty.ulValue }
		, m_ulCurrentValue{ rawProperty.ulValue }
		, m_ulInitialValue{ rawProperty.ulValue }
	{
	}

	/*static*/ std::optional<CDRMAtomicProperty> CDRMAtomicProperty::Instantiate( const char *pszName, CDRMAtomicObject *pObject, const DRMObjectRawProperties& rawProperties )
	{
		auto iter = rawProperties.find( pszName );
		if ( iter == rawProperties.end() )
			return std::nullopt;

		return CDRMAtomicProperty{ pObject, iter->second };
	}

	int CDRMAtomicProperty::SetPendingValue( drmModeAtomicReq *pRequest, uint64_t ulValue, bool bForce /*= false*/ )
	{
		// In instances where we rolled back due to -EINVAL, or we want to ensure a value from an unclean state
		// eg. from an unclean or other initial state, you can force an update in the request with bForce.

		if ( ulValue == m_ulPendingValue && !bForce )
			return 0;

		int ret = drmModeAtomicAddProperty( pRequest, m_pObject->GetObjectId(), m_uPropertyId, ulValue );
		if ( ret < 0 )
			return ret;

		m_ulPendingValue = ulValue;
		return ret;
	}

	void CDRMAtomicProperty::OnCommit()
	{
		m_ulCurrentValue = m_ulPendingValue;
	}

	void CDRMAtomicProperty::Rollback()
	{
		m_ulPendingValue = m_ulCurrentValue;
	}

	/////////////////////////
	// CDRMPlane
	/////////////////////////
	CDRMPlane::CDRMPlane( drmModePlane *pPlane )
		: CDRMAtomicTypedObject<DRM_MODE_OBJECT_PLANE>( pPlane->plane_id )
		, m_pPlane{ pPlane, []( drmModePlane *pPlane ){ drmModeFreePlane( pPlane ); } }
	{
		RefreshState();
	}

	void CDRMPlane::RefreshState()
	{
		auto rawProperties = GetRawProperties();
		if ( rawProperties )
		{
			m_Props.type                     = CDRMAtomicProperty::Instantiate( "type",                     this, *rawProperties );
			m_Props.IN_FORMATS               = CDRMAtomicProperty::Instantiate( "IN_FORMATS",               this, *rawProperties );

			m_Props.FB_ID                    = CDRMAtomicProperty::Instantiate( "FB_ID",                    this, *rawProperties );
			m_Props.CRTC_ID                  = CDRMAtomicProperty::Instantiate( "CRTC_ID",                  this, *rawProperties );
			m_Props.SRC_X                    = CDRMAtomicProperty::Instantiate( "SRC_X",                    this, *rawProperties );
			m_Props.SRC_Y                    = CDRMAtomicProperty::Instantiate( "SRC_Y",                    this, *rawProperties );
			m_Props.SRC_W                    = CDRMAtomicProperty::Instantiate( "SRC_W",                    this, *rawProperties );
			m_Props.SRC_H                    = CDRMAtomicProperty::Instantiate( "SRC_H",                    this, *rawProperties );
			m_Props.CRTC_X                   = CDRMAtomicProperty::Instantiate( "CRTC_X",                   this, *rawProperties );
			m_Props.CRTC_Y                   = CDRMAtomicProperty::Instantiate( "CRTC_Y",                   this, *rawProperties );
			m_Props.CRTC_W                   = CDRMAtomicProperty::Instantiate( "CRTC_W",                   this, *rawProperties );
			m_Props.CRTC_H                   = CDRMAtomicProperty::Instantiate( "CRTC_H",                   this, *rawProperties );
			m_Props.zpos                     = CDRMAtomicProperty::Instantiate( "zpos",                     this, *rawProperties );
			m_Props.alpha                    = CDRMAtomicProperty::Instantiate( "alpha",                    this, *rawProperties );
			m_Props.rotation                 = CDRMAtomicProperty::Instantiate( "rotation",                 this, *rawProperties );
			m_Props.COLOR_ENCODING           = CDRMAtomicProperty::Instantiate( "COLOR_ENCODING",           this, *rawProperties );
			m_Props.COLOR_RANGE              = CDRMAtomicProperty::Instantiate( "COLOR_RANGE",              this, *rawProperties );
			m_Props.VALVE1_PLANE_DEGAMMA_TF  = CDRMAtomicProperty::Instantiate( "VALVE1_PLANE_DEGAMMA_TF",  this, *rawProperties );
			m_Props.VALVE1_PLANE_DEGAMMA_LUT = CDRMAtomicProperty::Instantiate( "VALVE1_PLANE_DEGAMMA_LUT", this, *rawProperties );
			m_Props.VALVE1_PLANE_CTM         = CDRMAtomicProperty::Instantiate( "VALVE1_PLANE_CTM",         this, *rawProperties );
			m_Props.VALVE1_PLANE_HDR_MULT    = CDRMAtomicProperty::Instantiate( "VALVE1_PLANE_HDR_MULT",    this, *rawProperties );
			m_Props.VALVE1_PLANE_SHAPER_LUT  = CDRMAtomicProperty::Instantiate( "VALVE1_PLANE_SHAPER_LUT",  this, *rawProperties );
			m_Props.VALVE1_PLANE_SHAPER_TF   = CDRMAtomicProperty::Instantiate( "VALVE1_PLANE_SHAPER_TF",   this, *rawProperties );
			m_Props.VALVE1_PLANE_LUT3D       = CDRMAtomicProperty::Instantiate( "VALVE1_PLANE_LUT3D",       this, *rawProperties );
			m_Props.VALVE1_PLANE_BLEND_TF    = CDRMAtomicProperty::Instantiate( "VALVE1_PLANE_BLEND_TF",    this, *rawProperties );
			m_Props.VALVE1_PLANE_BLEND_LUT   = CDRMAtomicProperty::Instantiate( "VALVE1_PLANE_BLEND_LUT",   this, *rawProperties );
		}
	}

	/////////////////////////
	// CDRMCRTC
	/////////////////////////
	CDRMCRTC::CDRMCRTC( drmModeCrtc *pCRTC, uint32_t uCRTCMask )
		: CDRMAtomicTypedObject<DRM_MODE_OBJECT_CRTC>( pCRTC->crtc_id )
		, m_pCRTC{ pCRTC, []( drmModeCrtc *pCRTC ){ drmModeFreeCrtc( pCRTC ); } }
		, m_uCRTCMask{ uCRTCMask }
	{
		RefreshState();
	}

	void CDRMCRTC::RefreshState()
	{
		auto rawProperties = GetRawProperties();
		if ( rawProperties )
		{
			m_Props.ACTIVE                   = CDRMAtomicProperty::Instantiate( "ACTIVE",                 this, *rawProperties );
			m_Props.MODE_ID                  = CDRMAtomicProperty::Instantiate( "MODE_ID",                this, *rawProperties );
			m_Props.GAMMA_LUT                = CDRMAtomicProperty::Instantiate( "GAMMA_LUT",              this, *rawProperties );
			m_Props.DEGAMMA_LUT              = CDRMAtomicProperty::Instantiate( "DEGAMMA_LUT",            this, *rawProperties );
			m_Props.CTM                      = CDRMAtomicProperty::Instantiate( "CTM",                    this, *rawProperties );
			m_Props.VRR_ENABLED              = CDRMAtomicProperty::Instantiate( "VRR_ENABLED",            this, *rawProperties );
			m_Props.OUT_FENCE_PTR            = CDRMAtomicProperty::Instantiate( "OUT_FENCE_PTR",          this, *rawProperties );
			m_Props.VALVE1_CRTC_REGAMMA_TF   = CDRMAtomicProperty::Instantiate( "VALVE1_CRTC_REGAMMA_TF", this, *rawProperties );
		}
	}

	/////////////////////////
	// CDRMConnector
	/////////////////////////
	CDRMConnector::CDRMConnector( drmModeConnector *pConnector )
		: CDRMAtomicTypedObject<DRM_MODE_OBJECT_CONNECTOR>( pConnector->connector_id )
		, m_pConnector{ pConnector, []( drmModeConnector *pConnector ){ drmModeFreeConnector( pConnector ); } }
	{
		RefreshState();
	}

	void CDRMConnector::RefreshState()
	{
		// For the connector re-poll the drmModeConnector to get new modes, etc.
		// This isn't needed for CRTC/Planes in which the state is immutable for their lifetimes.
		// Connectors can be re-plugged.

		// TODO: Clean this up.
		m_pConnector = CAutoDeletePtr< drmModeConnector >
		{
			drmModeGetConnector( g_DRM.fd, m_pConnector->connector_id ),
			[]( drmModeConnector *pConnector ){ drmModeFreeConnector( pConnector ); }
		};

		// Sort the modes to our preference.
		std::stable_sort( m_pConnector->modes, m_pConnector->modes + m_pConnector->count_modes, []( const drmModeModeInfo &a, const drmModeModeInfo &b )
		{
			bool bGoodRefreshA = a.vrefresh >= 60;
			bool bGoodRefreshB = b.vrefresh >= 60;
			if (bGoodRefreshA != bGoodRefreshB)
				return bGoodRefreshA;

			bool bPreferredA = a.type & DRM_MODE_TYPE_PREFERRED;
			bool bPreferredB = b.type & DRM_MODE_TYPE_PREFERRED;
			if (bPreferredA != bPreferredB)
				return bPreferredA;

			int nAreaA = a.hdisplay * a.vdisplay;
			int nAreaB = b.hdisplay * b.vdisplay;
			if (nAreaA != nAreaB)
				return nAreaA > nAreaB;

			return a.vrefresh > b.vrefresh;
		} );

		// Clear this information out.
		m_Mutable = MutableConnectorState{};

		m_Mutable.uPossibleCRTCMask = drmModeConnectorGetPossibleCrtcs( g_DRM.fd, GetModeConnector() );

		// These are string constants from libdrm, no free.
		const char *pszTypeStr = drmModeGetConnectorTypeName( GetModeConnector()->connector_type );
		if ( !pszTypeStr )
			pszTypeStr = "Unknown";

		snprintf( m_Mutable.szName, sizeof( m_Mutable.szName ), "%s-%d", pszTypeStr, GetModeConnector()->connector_type_id );
		m_Mutable.szName[ sizeof( m_Mutable.szName ) - 1 ] = '\0';

		auto rawProperties = GetRawProperties();
		if ( rawProperties )
		{
			m_Props.CRTC_ID                  = CDRMAtomicProperty::Instantiate( "CRTC_ID",                this, *rawProperties );
			m_Props.Colorspace               = CDRMAtomicProperty::Instantiate( "Colorspace",             this, *rawProperties );
			m_Props.content_type             = CDRMAtomicProperty::Instantiate( "content type",           this, *rawProperties );
			m_Props.panel_orientation        = CDRMAtomicProperty::Instantiate( "panel orientation",      this, *rawProperties );
			m_Props.HDR_OUTPUT_METADATA      = CDRMAtomicProperty::Instantiate( "HDR_OUTPUT_METADATA",    this, *rawProperties );
			m_Props.vrr_capable              = CDRMAtomicProperty::Instantiate( "vrr_capable",            this, *rawProperties );
			m_Props.EDID                     = CDRMAtomicProperty::Instantiate( "EDID",                   this, *rawProperties );
		}

		ParseEDID();
	}

	void CDRMConnector::ParseEDID()
	{
		if ( !GetProperties().EDID )
			return;

		uint64_t ulBlobId = GetProperties().EDID->GetCurrentValue();
		if ( !ulBlobId )
			return;

		drmModePropertyBlobRes *pBlob = drmModeGetPropertyBlob( g_DRM.fd, ulBlobId );
		if ( !pBlob )
			return;
		defer( drmModeFreePropertyBlob( pBlob ) );

		const uint8_t *pDataPointer = reinterpret_cast<const uint8_t *>( pBlob->data );
		m_Mutable.EdidData = std::vector<uint8_t>{ pDataPointer, pDataPointer + pBlob->length };

		di_info *pInfo = di_info_parse_edid( m_Mutable.EdidData.data(), m_Mutable.EdidData.size() );
		if ( !pInfo )
		{
			drm_log.errorf( "Failed to parse edid for connector: %s", m_Mutable.szName );
			return;
		}
		defer( di_info_destroy( pInfo ) );

		const di_edid *pEdid = di_info_get_edid( pInfo );

		const di_edid_vendor_product *pProduct = di_edid_get_vendor_product( pEdid );
		m_Mutable.szMakePNP[0] = pProduct->manufacturer[0];
		m_Mutable.szMakePNP[1] = pProduct->manufacturer[1];
		m_Mutable.szMakePNP[2] = pProduct->manufacturer[2];
		m_Mutable.szMakePNP[3] = '\0';

		m_Mutable.pszMake = m_Mutable.szMakePNP;
		auto pnpIter = pnps.find( m_Mutable.szMakePNP );
		if ( pnpIter != pnps.end() )
			m_Mutable.pszMake = pnpIter->second.c_str();

		const di_edid_display_descriptor *const *pDescriptors = di_edid_get_display_descriptors( pEdid );
		for ( size_t i = 0; pDescriptors[i] != nullptr; i++ )
		{
			const di_edid_display_descriptor *pDesc = pDescriptors[i];
			if ( di_edid_display_descriptor_get_tag( pDesc ) == DI_EDID_DISPLAY_DESCRIPTOR_PRODUCT_NAME )
			{
				// Max length of di_edid_display_descriptor_get_string is 14
				// m_szModel is 16 bytes.
				const char *pszModel = di_edid_display_descriptor_get_string( pDesc );
				strncpy( m_Mutable.szModel, pszModel, sizeof( m_Mutable.szModel ) );
			}
		}

		drm_log.infof("Connector %s -> %s - %s", m_Mutable.szName, m_Mutable.szMakePNP, m_Mutable.szModel );

		const bool bSteamDeckDisplay =
			( m_Mutable.szMakePNP == "WLC"sv && m_Mutable.szModel == "ANX7530 U"sv ) ||
			( m_Mutable.szMakePNP == "ANX"sv && m_Mutable.szModel == "ANX7530 U"sv ) ||
			( m_Mutable.szMakePNP == "VLV"sv && m_Mutable.szModel == "ANX7530 U"sv ) ||
			( m_Mutable.szMakePNP == "VLV"sv && m_Mutable.szModel == "Jupiter"sv ) ||
			( m_Mutable.szMakePNP == "VLV"sv && m_Mutable.szModel == "Galileo"sv );

		if ( bSteamDeckDisplay )
		{
			static constexpr uint32_t kPIDGalileoSDC = 0x3003;
			static constexpr uint32_t kPIDGalileoBOE = 0x3004;

			if ( pProduct->product == kPIDGalileoSDC )
			{
				m_Mutable.eKnownDisplay = GAMESCOPE_KNOWN_DISPLAY_STEAM_DECK_OLED_SDC;
				m_Mutable.ValidDynamicRefreshRates = std::span( s_kSteamDeckOLEDRates );
			}
			else if ( pProduct->product == kPIDGalileoBOE )
			{
				m_Mutable.eKnownDisplay = GAMESCOPE_KNOWN_DISPLAY_STEAM_DECK_OLED_BOE;
				m_Mutable.ValidDynamicRefreshRates = std::span( s_kSteamDeckOLEDRates );
			}
			else
			{
				m_Mutable.eKnownDisplay = GAMESCOPE_KNOWN_DISPLAY_STEAM_DECK_LCD;
				m_Mutable.ValidDynamicRefreshRates = std::span( s_kSteamDeckLCDRates );
			}
		}

		// Colorimetry
		const char *pszColorOverride = getenv( "GAMESCOPE_INTERNAL_COLORIMETRY_OVERRIDE" );
		if ( pszColorOverride && *pszColorOverride && GetScreenType() == GAMESCOPE_SCREEN_TYPE_INTERNAL )
		{
			if ( sscanf( pszColorOverride, "%f %f %f %f %f %f %f %f",
				&m_Mutable.DisplayColorimetry.primaries.r.x, &m_Mutable.DisplayColorimetry.primaries.r.y,
				&m_Mutable.DisplayColorimetry.primaries.g.x, &m_Mutable.DisplayColorimetry.primaries.g.y,
				&m_Mutable.DisplayColorimetry.primaries.b.x, &m_Mutable.DisplayColorimetry.primaries.b.y,
				&m_Mutable.DisplayColorimetry.white.x, &m_Mutable.DisplayColorimetry.white.y ) == 8 )
			{
				drm_log.infof( "[colorimetry]: GAMESCOPE_INTERNAL_COLORIMETRY_OVERRIDE detected" );
			}
			else
			{
				drm_log.errorf( "[colorimetry]: GAMESCOPE_INTERNAL_COLORIMETRY_OVERRIDE specified, but could not parse \"rx ry gx gy bx by wx wy\"" );
			}
		}
		else if ( m_Mutable.eKnownDisplay == GAMESCOPE_KNOWN_DISPLAY_STEAM_DECK_LCD )
		{
			drm_log.infof( "[colorimetry]: Steam Deck LCD detected. Using known colorimetry" );
			m_Mutable.DisplayColorimetry = displaycolorimetry_steamdeck_measured;
		}
		else
		{
			// Steam Deck OLED has calibrated chromaticity coordinates in the EDID
			// for each unit.
			// Other external displays probably have this too.

			const di_edid_chromaticity_coords *pChroma = di_edid_get_chromaticity_coords( pEdid );
			if ( pChroma && pChroma->red_x != 0.0f )
			{
				drm_log.infof( "[colorimetry]: EDID with colorimetry detected. Using it" );
				m_Mutable.DisplayColorimetry = displaycolorimetry_t
				{
					.primaries = { { pChroma->red_x, pChroma->red_y }, { pChroma->green_x, pChroma->green_y }, { pChroma->blue_x, pChroma->blue_y } },
					.white = { pChroma->white_x, pChroma->white_y },
				};
			}
		}

		drm_log.infof( "[colorimetry]: r %f %f", m_Mutable.DisplayColorimetry.primaries.r.x, m_Mutable.DisplayColorimetry.primaries.r.y );
		drm_log.infof( "[colorimetry]: g %f %f", m_Mutable.DisplayColorimetry.primaries.g.x, m_Mutable.DisplayColorimetry.primaries.g.y );
		drm_log.infof( "[colorimetry]: b %f %f", m_Mutable.DisplayColorimetry.primaries.b.x, m_Mutable.DisplayColorimetry.primaries.b.y );
		drm_log.infof( "[colorimetry]: w %f %f", m_Mutable.DisplayColorimetry.white.x, m_Mutable.DisplayColorimetry.white.y );

		/////////////////////
		// Parse HDR stuff.
		/////////////////////
		std::optional<CDRMConnector::HDRInfo> oKnownHDRInfo = GetKnownDisplayHDRInfo( m_Mutable.eKnownDisplay );
		if ( oKnownHDRInfo )
		{
			m_Mutable.HDR = *oKnownHDRInfo;
		}
		else
		{
			const di_cta_hdr_static_metadata_block *pHDRStaticMetadata = nullptr;
			const di_cta_colorimetry_block *pColorimetry = nullptr;

			const di_edid_cta* pCTA = NULL;
			const di_edid_ext *const *ppExts = di_edid_get_extensions( pEdid );
			for ( ; *ppExts != nullptr; ppExts++ )
			{
				if ( ( pCTA = di_edid_ext_get_cta( *ppExts ) ) )
					break;
			}

			if ( pCTA )
			{
				const di_cta_data_block *const *ppBlocks = di_edid_cta_get_data_blocks( pCTA );
				for ( ; *ppBlocks != nullptr; ppBlocks++ )
				{
					if ( di_cta_data_block_get_tag( *ppBlocks ) == DI_CTA_DATA_BLOCK_HDR_STATIC_METADATA )
					{
						pHDRStaticMetadata = di_cta_data_block_get_hdr_static_metadata( *ppBlocks );
						continue;
					}

					if ( di_cta_data_block_get_tag( *ppBlocks ) == DI_CTA_DATA_BLOCK_COLORIMETRY )
					{
						pColorimetry = di_cta_data_block_get_colorimetry( *ppBlocks );
						continue;
					}
				}
			}

			if ( pColorimetry && pColorimetry->bt2020_rgb &&
				 pHDRStaticMetadata && pHDRStaticMetadata->eotfs && pHDRStaticMetadata->eotfs->pq )
			{
				m_Mutable.HDR.bExposeHDRSupport = true;
				m_Mutable.HDR.eOutputEncodingEOTF = EOTF_PQ;
				m_Mutable.HDR.uMaxContentLightLevel =
					pHDRStaticMetadata->desired_content_max_luminance
					? nits_to_u16( pHDRStaticMetadata->desired_content_max_luminance )
					: nits_to_u16( 1499.0f );
				m_Mutable.HDR.uMaxFrameAverageLuminance =
					pHDRStaticMetadata->desired_content_max_frame_avg_luminance
					? nits_to_u16( pHDRStaticMetadata->desired_content_max_frame_avg_luminance )
					: nits_to_u16( std::min( 799.f, nits_from_u16( m_Mutable.HDR.uMaxContentLightLevel ) ) );
				m_Mutable.HDR.uMinContentLightLevel =
					pHDRStaticMetadata->desired_content_min_luminance
					? nits_to_u16_dark( pHDRStaticMetadata->desired_content_min_luminance )
					: nits_to_u16_dark( 0.0f );

				// Generate a default HDR10 infoframe.
				hdr_output_metadata defaultHDRMetadata{};
				hdr_metadata_infoframe *pInfoframe = &defaultHDRMetadata.hdmi_metadata_type1;

				// To be filled in by the app based on the scene, default to desired_content_max_luminance
				//
		 		// Using display's max_fall for the default metadata max_cll to avoid displays
		 		// overcompensating with tonemapping for SDR content.
				uint16_t uDefaultInfoframeLuminances = m_Mutable.HDR.uMaxFrameAverageLuminance;

				pInfoframe->display_primaries[0].x = color_xy_to_u16( m_Mutable.DisplayColorimetry.primaries.r.x );
				pInfoframe->display_primaries[0].y = color_xy_to_u16( m_Mutable.DisplayColorimetry.primaries.r.y );
				pInfoframe->display_primaries[1].x = color_xy_to_u16( m_Mutable.DisplayColorimetry.primaries.g.x );
				pInfoframe->display_primaries[1].y = color_xy_to_u16( m_Mutable.DisplayColorimetry.primaries.g.y );
				pInfoframe->display_primaries[2].x = color_xy_to_u16( m_Mutable.DisplayColorimetry.primaries.b.x );
				pInfoframe->display_primaries[2].y = color_xy_to_u16( m_Mutable.DisplayColorimetry.primaries.b.y );
				pInfoframe->white_point.x = color_xy_to_u16( m_Mutable.DisplayColorimetry.white.x );
				pInfoframe->white_point.y = color_xy_to_u16( m_Mutable.DisplayColorimetry.white.y );
				pInfoframe->max_display_mastering_luminance = uDefaultInfoframeLuminances;
				pInfoframe->min_display_mastering_luminance = m_Mutable.HDR.uMinContentLightLevel;
				pInfoframe->max_cll = uDefaultInfoframeLuminances;
				pInfoframe->max_fall = uDefaultInfoframeLuminances;
				pInfoframe->eotf = HDMI_EOTF_ST2084;

				m_Mutable.HDR.pDefaultMetadataBlob = drm_create_hdr_metadata_blob( &g_DRM, &defaultHDRMetadata );
			}
			else
			{
				m_Mutable.HDR.bExposeHDRSupport = false;
			}
		}
	}

	/*static*/ std::optional<CDRMConnector::HDRInfo> CDRMConnector::GetKnownDisplayHDRInfo( GamescopeKnownDisplays eKnownDisplay )
	{
		if ( eKnownDisplay == GAMESCOPE_KNOWN_DISPLAY_STEAM_DECK_OLED_BOE || eKnownDisplay == GAMESCOPE_KNOWN_DISPLAY_STEAM_DECK_OLED_SDC )
		{
			// The stuff in the EDID for the HDR metadata does not fully
			// reflect what we can achieve on the display by poking at more
			// things out-of-band.
			return HDRInfo
			{
				.bExposeHDRSupport = true,
				.eOutputEncodingEOTF = EOTF_Gamma22,
				.uMaxContentLightLevel = nits_to_u16( 1000.0f ),
				.uMaxFrameAverageLuminance = nits_to_u16( 800.0f ), // Full-frame sustained.
				.uMinContentLightLevel = nits_to_u16_dark( 0 ),
			};
		}
		else if ( eKnownDisplay == GAMESCOPE_KNOWN_DISPLAY_STEAM_DECK_LCD )
		{
			// Set up some HDR fallbacks for undocking
			return HDRInfo
			{
				.bExposeHDRSupport = false,
				.eOutputEncodingEOTF = EOTF_Gamma22,
				.uMaxContentLightLevel = nits_to_u16( 500.0f ),
				.uMaxFrameAverageLuminance = nits_to_u16( 500.0f ),
				.uMinContentLightLevel = nits_to_u16_dark( 0.5f ),
			};
		}

		return std::nullopt;
	}
}

static int
drm_prepare_liftoff( struct drm_t *drm, const struct FrameInfo_t *frameInfo, bool needs_modeset )
{
	gamescope::GamescopeScreenType screenType = drm_get_screen_type(drm);
	auto entry = FrameInfoToLiftoffStateCacheEntry( drm, frameInfo );

	// If we are modesetting, reset the state cache, we might
	// move to another CRTC or whatever which might have differing caps.
	// (same with different modes)
	if (needs_modeset)
		g_LiftoffStateCache.clear();

	if (is_liftoff_caching_enabled())
	{
		if (g_LiftoffStateCache.count(entry) != 0)
			return -EINVAL;
	}

	bool bSinglePlane = frameInfo->layerCount < 2 && g_bSinglePlaneOptimizations;

	for ( int i = 0; i < k_nMaxLayers; i++ )
	{
		if ( i < frameInfo->layerCount )
		{
			if ( frameInfo->layers[ i ].fbid == 0 )
			{
				drm_verbose_log.errorf("drm_prepare_liftoff: layer %d has no FB", i );
				return -EINVAL;
			}

			liftoff_layer_set_property( drm->lo_layers[ i ], "FB_ID", frameInfo->layers[ i ].fbid);
			drm->fbids_in_req.push_back( frameInfo->layers[ i ].fbid );

			liftoff_layer_set_property( drm->lo_layers[ i ], "zpos", entry.layerState[i].zpos );
			liftoff_layer_set_property( drm->lo_layers[ i ], "alpha", frameInfo->layers[ i ].opacity * 0xffff);

			liftoff_layer_set_property( drm->lo_layers[ i ], "SRC_X", 0);
			liftoff_layer_set_property( drm->lo_layers[ i ], "SRC_Y", 0);
			liftoff_layer_set_property( drm->lo_layers[ i ], "SRC_W", entry.layerState[i].srcW );
			liftoff_layer_set_property( drm->lo_layers[ i ], "SRC_H", entry.layerState[i].srcH );

			liftoff_layer_set_property( drm->lo_layers[ i ], "rotation", g_drmEffectiveOrientation[screenType] );

			liftoff_layer_set_property( drm->lo_layers[ i ], "CRTC_X", entry.layerState[i].crtcX);
			liftoff_layer_set_property( drm->lo_layers[ i ], "CRTC_Y", entry.layerState[i].crtcY);

			liftoff_layer_set_property( drm->lo_layers[ i ], "CRTC_W", entry.layerState[i].crtcW);
			liftoff_layer_set_property( drm->lo_layers[ i ], "CRTC_H", entry.layerState[i].crtcH);

			if ( frameInfo->layers[i].applyColorMgmt )
			{
				if ( entry.layerState[i].ycbcr )
				{
					liftoff_layer_set_property( drm->lo_layers[ i ], "COLOR_ENCODING", entry.layerState[i].colorEncoding );
					liftoff_layer_set_property( drm->lo_layers[ i ], "COLOR_RANGE",    entry.layerState[i].colorRange );
				}
				else
				{
					liftoff_layer_unset_property( drm->lo_layers[ i ], "COLOR_ENCODING" );
					liftoff_layer_unset_property( drm->lo_layers[ i ], "COLOR_RANGE" );
				}

				if ( drm_supports_color_mgmt( drm ) )
				{
					drm_valve1_transfer_function degamma_tf = colorspace_to_plane_degamma_tf( entry.layerState[i].colorspace );
					drm_valve1_transfer_function shaper_tf = colorspace_to_plane_shaper_tf( entry.layerState[i].colorspace );

					if ( entry.layerState[i].ycbcr )
					{
						// JoshA: Based on the Steam In-Home Streaming Shader,
						// it looks like Y is actually sRGB, not HDTV G2.4
						//
						// Matching BT709 for degamma -> regamma on shaper TF here
						// is identity and works on YUV NV12 planes to preserve this.
						//
						// Doing LINEAR/DEFAULT here introduces banding so... this is the best way.
						// (sRGB DEGAMMA does NOT work on YUV planes!)
						degamma_tf = DRM_VALVE1_TRANSFER_FUNCTION_BT709;
						shaper_tf = DRM_VALVE1_TRANSFER_FUNCTION_BT709;
					}

					if (!g_bDisableDegamma)
						liftoff_layer_set_property( drm->lo_layers[ i ], "VALVE1_PLANE_DEGAMMA_TF", degamma_tf );
					else
						liftoff_layer_set_property( drm->lo_layers[ i ], "VALVE1_PLANE_DEGAMMA_TF", 0 );

					if ( !g_bDisableShaperAnd3DLUT )
					{
						liftoff_layer_set_property( drm->lo_layers[ i ], "VALVE1_PLANE_SHAPER_LUT", drm->pending.shaperlut_id[ ColorSpaceToEOTFIndex( entry.layerState[i].colorspace ) ]->blob );
						liftoff_layer_set_property( drm->lo_layers[ i ], "VALVE1_PLANE_SHAPER_TF", shaper_tf );
						liftoff_layer_set_property( drm->lo_layers[ i ], "VALVE1_PLANE_LUT3D", drm->pending.lut3d_id[ ColorSpaceToEOTFIndex( entry.layerState[i].colorspace ) ]->blob );
						// Josh: See shaders/colorimetry.h colorspace_blend_tf if you have questions as to why we start doing sRGB for BLEND_TF despite potentially working in Gamma 2.2 space prior.
					}
					else
					{
						liftoff_layer_set_property( drm->lo_layers[ i ], "VALVE1_PLANE_SHAPER_LUT", 0 );
						liftoff_layer_set_property( drm->lo_layers[ i ], "VALVE1_PLANE_SHAPER_TF", 0 );
						liftoff_layer_set_property( drm->lo_layers[ i ], "VALVE1_PLANE_LUT3D", 0 );
					}
				}
			}
			else
			{
				if ( drm_supports_color_mgmt( drm ) )
				{
					liftoff_layer_set_property( drm->lo_layers[ i ], "VALVE1_PLANE_DEGAMMA_TF", DRM_VALVE1_TRANSFER_FUNCTION_DEFAULT );
					liftoff_layer_set_property( drm->lo_layers[ i ], "VALVE1_PLANE_SHAPER_LUT", 0 );
					liftoff_layer_set_property( drm->lo_layers[ i ], "VALVE1_PLANE_SHAPER_TF", 0 );
					liftoff_layer_set_property( drm->lo_layers[ i ], "VALVE1_PLANE_LUT3D", 0 );
					liftoff_layer_set_property( drm->lo_layers[ i ], "VALVE1_PLANE_CTM", 0 );
				}
			}

			if ( drm_supports_color_mgmt( drm ) )
			{
				if (!g_bDisableBlendTF && !bSinglePlane)
					liftoff_layer_set_property( drm->lo_layers[ i ], "VALVE1_PLANE_BLEND_TF", drm->pending.output_tf );
				else
					liftoff_layer_set_property( drm->lo_layers[ i ], "VALVE1_PLANE_BLEND_TF", DRM_VALVE1_TRANSFER_FUNCTION_DEFAULT );

				if (frameInfo->layers[i].ctm != nullptr)
					liftoff_layer_set_property( drm->lo_layers[ i ], "VALVE1_PLANE_CTM", frameInfo->layers[i].ctm->blob );
				else
					liftoff_layer_set_property( drm->lo_layers[ i ], "VALVE1_PLANE_CTM", 0 );
			}
		}
		else
		{
			liftoff_layer_set_property( drm->lo_layers[ i ], "FB_ID", 0 );

			liftoff_layer_unset_property( drm->lo_layers[ i ], "COLOR_ENCODING" );
			liftoff_layer_unset_property( drm->lo_layers[ i ], "COLOR_RANGE" );

			if ( drm_supports_color_mgmt( drm ) )
			{
				liftoff_layer_set_property( drm->lo_layers[ i ], "VALVE1_PLANE_DEGAMMA_TF", DRM_VALVE1_TRANSFER_FUNCTION_DEFAULT );
				liftoff_layer_set_property( drm->lo_layers[ i ], "VALVE1_PLANE_SHAPER_LUT", 0 );
				liftoff_layer_set_property( drm->lo_layers[ i ], "VALVE1_PLANE_SHAPER_TF", 0 );
				liftoff_layer_set_property( drm->lo_layers[ i ], "VALVE1_PLANE_LUT3D", 0 );
				liftoff_layer_set_property( drm->lo_layers[ i ], "VALVE1_PLANE_BLEND_TF", DRM_VALVE1_TRANSFER_FUNCTION_DEFAULT );
				liftoff_layer_set_property( drm->lo_layers[ i ], "VALVE1_PLANE_CTM", 0 );
			}
		}
	}

	int ret = liftoff_output_apply( drm->lo_output, drm->req, drm->flags );

	if ( ret == 0 )
	{
		// We don't support partial composition yet
		if ( liftoff_output_needs_composition( drm->lo_output ) )
			ret = -EINVAL;
	}

	// If we aren't modesetting and we got -EINVAL, that means that we
	// probably can't do this layout, so add it to our state cache so we don't
	// try it again.
	if (!needs_modeset)
	{
		if (ret == -EINVAL)
			g_LiftoffStateCache.insert(entry);
	}

	if ( ret == 0 )
		drm_verbose_log.debugf( "can drm present %i layers", frameInfo->layerCount );
	else
		drm_verbose_log.debugf( "can NOT drm present %i layers", frameInfo->layerCount );

	return ret;
}

bool g_bForceAsyncFlips = false;

/* Prepares an atomic commit for the provided scene-graph. Returns 0 on success,
 * negative errno on failure or if the scene-graph can't be presented directly. */
int drm_prepare( struct drm_t *drm, bool async, const struct FrameInfo_t *frameInfo )
{
	drm_update_color_mgmt(drm);

	const bool bVRRCapable = drm->pConnector && drm->pConnector->GetProperties().vrr_capable &&
							 drm->pCRTC && drm->pCRTC->GetProperties().VRR_ENABLED;
	const bool bVRREnabled = bVRRCapable && frameInfo->allowVRR;
	if ( bVRRCapable )
	{
		if ( bVRREnabled != !!drm->pCRTC->GetProperties().VRR_ENABLED->GetCurrentValue() )
			drm->needs_modeset = true;
	}

	drm->fbids_in_req.clear();

	bool needs_modeset = drm->needs_modeset.exchange(false);

	assert( drm->req == nullptr );
	drm->req = drmModeAtomicAlloc();

	wlserver_hdr_metadata *pHDRMetadata = nullptr;
	if ( drm->pConnector && drm->pConnector->GetHDRInfo().IsHDR10() )
	{
		if ( g_bOutputHDREnabled )
		{
			wlserver_vk_swapchain_feedback* pFeedback = steamcompmgr_get_base_layer_swapchain_feedback();
			pHDRMetadata = pFeedback ? pFeedback->hdr_metadata_blob.get() : drm->pConnector->GetHDRInfo().pDefaultMetadataBlob.get();
		}
		else
		{
			pHDRMetadata = drm->sdr_static_metadata.get();
		}
	}

	bool bSinglePlane = frameInfo->layerCount < 2 && g_bSinglePlaneOptimizations;

	if ( drm_supports_color_mgmt( &g_DRM ) && frameInfo->applyOutputColorMgmt )
	{
		if ( !g_bDisableRegamma && !bSinglePlane )
		{
			drm->pending.output_tf = g_bOutputHDREnabled
				? DRM_VALVE1_TRANSFER_FUNCTION_PQ
				: DRM_VALVE1_TRANSFER_FUNCTION_SRGB;
		}
		else
		{
			drm->pending.output_tf = DRM_VALVE1_TRANSFER_FUNCTION_DEFAULT;
		}
	}
	else
	{
		drm->pending.output_tf = DRM_VALVE1_TRANSFER_FUNCTION_DEFAULT;
	}

	uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK;

	// We do internal refcounting with these events
	if ( drm->pCRTC != nullptr )
		flags |= DRM_MODE_PAGE_FLIP_EVENT;

	if ( async || g_bForceAsyncFlips )
		flags |= DRM_MODE_PAGE_FLIP_ASYNC;

	bool bForceInRequest = needs_modeset;

	if ( needs_modeset )
	{
		flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;

		// Disable all connectors and CRTCs

		for ( auto &iter : drm->connectors )
		{
			gamescope::CDRMConnector *pConnector = &iter.second;
			if ( pConnector->GetProperties().CRTC_ID->GetCurrentValue() == 0 )
				continue;

			pConnector->GetProperties().CRTC_ID->SetPendingValue( drm->req, 0, bForceInRequest );

			if ( pConnector->GetProperties().Colorspace )
				pConnector->GetProperties().Colorspace->SetPendingValue( drm->req, 0, bForceInRequest );

			if ( pConnector->GetProperties().HDR_OUTPUT_METADATA )
				pConnector->GetProperties().HDR_OUTPUT_METADATA->SetPendingValue( drm->req, 0, bForceInRequest );

			if ( pConnector->GetProperties().content_type )
				pConnector->GetProperties().content_type->SetPendingValue( drm->req, 0, bForceInRequest );
		}

		for ( std::unique_ptr< gamescope::CDRMCRTC > &pCRTC : drm->crtcs )
		{
			// We can't disable a CRTC if it's already disabled, or else the
			// kernel will error out with "requesting event but off".
			if ( pCRTC->GetProperties().ACTIVE->GetCurrentValue() == 0 )
				continue;

			pCRTC->GetProperties().ACTIVE->SetPendingValue( drm->req, 0, bForceInRequest );
			pCRTC->GetProperties().MODE_ID->SetPendingValue( drm->req, 0, bForceInRequest );

			if ( pCRTC->GetProperties().GAMMA_LUT )
				pCRTC->GetProperties().GAMMA_LUT->SetPendingValue( drm->req, 0, bForceInRequest );

			if ( pCRTC->GetProperties().DEGAMMA_LUT )
				pCRTC->GetProperties().DEGAMMA_LUT->SetPendingValue( drm->req, 0, bForceInRequest );

			if ( pCRTC->GetProperties().CTM )
				pCRTC->GetProperties().CTM->SetPendingValue( drm->req, 0, bForceInRequest );

			if ( pCRTC->GetProperties().VRR_ENABLED )
				pCRTC->GetProperties().VRR_ENABLED->SetPendingValue( drm->req, 0, bForceInRequest );

			if ( pCRTC->GetProperties().OUT_FENCE_PTR )
				pCRTC->GetProperties().OUT_FENCE_PTR->SetPendingValue( drm->req, 0, bForceInRequest );

			if ( pCRTC->GetProperties().VALVE1_CRTC_REGAMMA_TF )
				pCRTC->GetProperties().VALVE1_CRTC_REGAMMA_TF->SetPendingValue( drm->req, 0, bForceInRequest );
		}

		if ( drm->pConnector )
		{
			// Always set our CRTC_ID for the modeset, especially
			// as we zero-ed it above.
			drm->pConnector->GetProperties().CRTC_ID->SetPendingValue( drm->req, drm->pCRTC->GetObjectId(), bForceInRequest );

			if ( drm->pConnector->GetProperties().Colorspace )
			{
				uint32_t uColorimetry = g_bOutputHDREnabled && drm->pConnector->GetHDRInfo().IsHDR10()
					? DRM_MODE_COLORIMETRY_BT2020_RGB
					: DRM_MODE_COLORIMETRY_DEFAULT;
				drm->pConnector->GetProperties().Colorspace->SetPendingValue( drm->req, uColorimetry, bForceInRequest );
			}
		}

		if ( drm->pCRTC )
		{
			drm->pCRTC->GetProperties().ACTIVE->SetPendingValue( drm->req, 1u, true );
			drm->pCRTC->GetProperties().MODE_ID->SetPendingValue( drm->req, drm->pending.mode_id ? drm->pending.mode_id->blob : 0lu, true );

			if ( drm->pCRTC->GetProperties().VRR_ENABLED )
				drm->pCRTC->GetProperties().VRR_ENABLED->SetPendingValue( drm->req, bVRREnabled, true );
		}
	}

	if ( drm->pConnector )
	{
		if ( drm->pConnector->GetProperties().HDR_OUTPUT_METADATA )
			drm->pConnector->GetProperties().HDR_OUTPUT_METADATA->SetPendingValue( drm->req, pHDRMetadata ? pHDRMetadata->blob : 0lu, bForceInRequest );

		if ( drm->pConnector->GetProperties().content_type )
			drm->pConnector->GetProperties().content_type->SetPendingValue( drm->req, DRM_MODE_CONTENT_TYPE_GAME, bForceInRequest );
	}

	if ( drm->pCRTC )
	{
		if ( drm->pCRTC->GetProperties().VALVE1_CRTC_REGAMMA_TF )
			drm->pCRTC->GetProperties().VALVE1_CRTC_REGAMMA_TF->SetPendingValue( drm->req, drm->pending.output_tf, bForceInRequest );
	}

	drm->flags = flags;

	int ret;
	if ( drm->pCRTC == nullptr ) {
		ret = 0;
	} else if ( drm->bUseLiftoff ) {
		ret = drm_prepare_liftoff( drm, frameInfo, needs_modeset );
	} else {
		ret = 0;
	}

	if ( ret != 0 ) {
		drmModeAtomicFree( drm->req );
		drm->req = nullptr;

		drm->fbids_in_req.clear();

		if ( needs_modeset )
			drm->needs_modeset = true;
	}

	return ret;
}

bool drm_poll_state( struct drm_t *drm )
{
	int out_of_date = drm->out_of_date.exchange(false);
	if ( !out_of_date )
		return false;

	refresh_state( drm );

	setup_best_connector(drm, out_of_date >= 2, false);

	return true;
}

static bool drm_set_crtc( struct drm_t *drm, gamescope::CDRMCRTC *pCRTC )
{
	drm->pCRTC = pCRTC;
	drm->needs_modeset = true;

	drm->pPrimaryPlane = find_primary_plane( drm );
	if ( drm->pPrimaryPlane == nullptr ) {
		drm_log.errorf("could not find a suitable primary plane");
		return false;
	}

	struct liftoff_output *lo_output = liftoff_output_create( drm->lo_device, pCRTC->GetObjectId() );
	if ( lo_output == nullptr )
		return false;

	for ( int i = 0; i < k_nMaxLayers; i++ )
	{
		liftoff_layer_destroy( drm->lo_layers[ i ] );
		drm->lo_layers[ i ] = liftoff_layer_create( lo_output );
		if ( drm->lo_layers[ i ] == nullptr )
			return false;
	}

	liftoff_output_destroy( drm->lo_output );
	drm->lo_output = lo_output;

	return true;
}

bool drm_set_connector( struct drm_t *drm, gamescope::CDRMConnector *conn )
{
	drm_log.infof("selecting connector %s", conn->GetName());

	gamescope::CDRMCRTC *pCRTC = find_crtc_for_connector(drm, conn);
	if (pCRTC == nullptr)
	{
		drm_log.errorf("no CRTC found!");
		return false;
	}

	if (!drm_set_crtc(drm, pCRTC)) {
		return false;
	}

	drm->pConnector = conn;
	drm->needs_modeset = true;

	return true;
}

static void drm_unset_connector( struct drm_t *drm )
{
	drm->pCRTC = nullptr;
	drm->pPrimaryPlane = nullptr;

	for ( int i = 0; i < k_nMaxLayers; i++ )
	{
		liftoff_layer_destroy( drm->lo_layers[ i ] );
		drm->lo_layers[ i ] = nullptr;
	}

	liftoff_output_destroy(drm->lo_output);
	drm->lo_output = nullptr;

	drm->pConnector = nullptr;
	drm->needs_modeset = true;
}

bool drm_get_vrr_in_use(struct drm_t *drm)
{
	if ( !drm->pCRTC || !drm->pCRTC->GetProperties().VRR_ENABLED )
		return false;

	return !!drm->pCRTC->GetProperties().VRR_ENABLED->GetCurrentValue();
}

gamescope::GamescopeScreenType drm_get_screen_type(struct drm_t *drm)
{
	if ( !drm->pConnector )
		return gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL;

	if (g_PanelType == PANEL_TYPE_INTERNAL)
	{
		drm->pConnector->drm_force_panel_type(drm->pConnector->PANEL_TYPE_INTERNAL);
	}

	if (g_PanelType == PANEL_TYPE_EXTERNAL)
	{
		drm->pConnector->drm_force_panel_type(drm->pConnector->PANEL_TYPE_EXTERNAL);
	}
	return drm->pConnector->GetScreenType();
}

bool drm_update_color_mgmt(struct drm_t *drm)
{
	if ( !drm_supports_color_mgmt( drm ) )
		return true;

	if ( g_ColorMgmt.serial == drm->current.color_mgmt_serial )
		return true;

	drm->pending.color_mgmt_serial = g_ColorMgmt.serial;

	for ( uint32_t i = 0; i < EOTF_Count; i++ )
	{
		drm->pending.shaperlut_id[ i ] = 0;
		drm->pending.lut3d_id[ i ] = 0;
	}

	for ( uint32_t i = 0; i < EOTF_Count; i++ )
	{
		if ( !g_ColorMgmtLuts[i].HasLuts() )
			continue;

		uint32_t shaper_blob_id = 0;
		if (drmModeCreatePropertyBlob(drm->fd, g_ColorMgmtLuts[i].lut1d, sizeof(g_ColorMgmtLuts[i].lut1d), &shaper_blob_id) != 0) {
			drm_log.errorf_errno("Unable to create SHAPERLUT property blob");
			return false;
		}
		drm->pending.shaperlut_id[ i ] = std::make_shared<drm_blob>( shaper_blob_id );

		uint32_t lut3d_blob_id = 0;
		if (drmModeCreatePropertyBlob(drm->fd, g_ColorMgmtLuts[i].lut3d, sizeof(g_ColorMgmtLuts[i].lut3d), &lut3d_blob_id) != 0) {
			drm_log.errorf_errno("Unable to create LUT3D property blob");
			return false;
		}
		drm->pending.lut3d_id[ i ] = std::make_shared<drm_blob>( lut3d_blob_id );
	}

	return true;
}

static void drm_unset_mode( struct drm_t *drm )
{
	drm->pending.mode_id = 0;
	drm->needs_modeset = true;

	g_nOutputWidth = drm->preferred_width;
	g_nOutputHeight = drm->preferred_height;
	if (g_nOutputHeight == 0)
		g_nOutputHeight = 720;
	if (g_nOutputWidth == 0)
		g_nOutputWidth = g_nOutputHeight * 16 / 9;

	g_nOutputRefresh = drm->preferred_refresh;
	if (g_nOutputRefresh == 0)
		g_nOutputRefresh = 60;

	g_drmEffectiveOrientation[gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL] = DRM_MODE_ROTATE_0;
	g_drmEffectiveOrientation[gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL] = DRM_MODE_ROTATE_0;
	g_bRotated = false;
}

void drm_set_orientation( struct drm_t *drm, bool isRotated)
{
	int width = g_nOutputWidth;
	int height = g_nOutputHeight;
	g_bRotated = isRotated;
	if ( g_bRotated ) {
		int tmp = width;
		width = height;
		height = tmp;
	}

	if (!drm->pConnector || !drm->pConnector->GetModeConnector())
		return;

	drmModeConnector *connector = drm->pConnector->GetModeConnector();
	const drmModeModeInfo *mode = find_mode(connector, width, height, 0);
	update_drm_effective_orientations(drm, mode, g_bPanelTypeFaked);
}

bool drm_set_mode( struct drm_t *drm, const drmModeModeInfo *mode )
{
	if (!drm->pConnector || !drm->pConnector->GetModeConnector())
		return false;

	uint32_t mode_id = 0;
	if (drmModeCreatePropertyBlob(drm->fd, mode, sizeof(*mode), &mode_id) != 0)
		return false;

	gamescope::GamescopeScreenType screenType = drm_get_screen_type(drm);

	drm_log.infof("selecting mode %dx%d@%uHz", mode->hdisplay, mode->vdisplay, mode->vrefresh);

	drm->pending.mode_id = std::make_shared<drm_blob>(mode_id);
	drm->needs_modeset = true;

	g_nOutputRefresh = mode->vrefresh;

	update_drm_effective_orientations(drm, mode, g_bPanelTypeFaked);

	switch ( g_drmEffectiveOrientation[screenType] )
	{
	case DRM_MODE_ROTATE_0:
	case DRM_MODE_ROTATE_180:
		g_bRotated = false;
		g_nOutputWidth = mode->hdisplay;
		g_nOutputHeight = mode->vdisplay;
		break;
	case DRM_MODE_ROTATE_90:
	case DRM_MODE_ROTATE_270:
		g_bRotated = true;
		g_nOutputWidth = mode->vdisplay;
		g_nOutputHeight = mode->hdisplay;
		break;
	}

	return true;
}

bool drm_set_refresh( struct drm_t *drm, int refresh )
{
	int width = g_nOutputWidth;
	int height = g_nOutputHeight;

	if ( g_bRotated ) {
		int tmp = width;
		width = height;
		height = tmp;
	}
	if (!drm->pConnector || !drm->pConnector->GetModeConnector())
		return false;

	drmModeConnector *connector = drm->pConnector->GetModeConnector();
	const drmModeModeInfo *existing_mode = find_mode(connector, width, height, refresh);
	drmModeModeInfo mode = {0};
	if ( existing_mode )
	{
		mode = *existing_mode;
	}
	else
	{
		/* TODO: check refresh is within the EDID limits */
		switch ( g_eGamescopeModeGeneration )
		{
		case gamescope::GAMESCOPE_MODE_GENERATE_CVT:
			generate_cvt_mode( &mode, width, height, refresh, true, false );
			break;
		case gamescope::GAMESCOPE_MODE_GENERATE_FIXED:
			{
				const drmModeModeInfo *preferred_mode = find_mode(connector, 0, 0, 0);
				generate_fixed_mode( &mode, preferred_mode, refresh, drm->pConnector->GetKnownDisplayType() );
				break;
			}
		}
	}

	mode.type = DRM_MODE_TYPE_USERDEF;

	return drm_set_mode(drm, &mode);
}

bool drm_set_resolution( struct drm_t *drm, int width, int height )
{
	if (!drm->pConnector || !drm->pConnector->GetModeConnector())
		return false;

	drmModeConnector *connector = drm->pConnector->GetModeConnector();
	const drmModeModeInfo *mode = find_mode(connector, width, height, 0);
	if ( !mode )
	{
		return false;
	}

	return drm_set_mode(drm, mode);
}

int drm_get_default_refresh(struct drm_t *drm)
{
	if ( drm->preferred_refresh )
		return drm->preferred_refresh;

	if ( drm->pConnector && drm->pConnector->GetBaseRefresh() )
		return drm->pConnector->GetBaseRefresh();

	if ( drm->pConnector && drm->pConnector->GetModeConnector() )
	{
		int width = g_nOutputWidth;
		int height = g_nOutputHeight;
		if ( g_bRotated ) {
			int tmp = width;
			width = height;
			height = tmp;
		}

		drmModeConnector *connector = drm->pConnector->GetModeConnector();
		const drmModeModeInfo *mode = find_mode( connector, width, height, 0);
		if ( mode )
			return mode->vrefresh;
	}

	return 60;
}

bool drm_get_vrr_capable(struct drm_t *drm)
{
	if ( drm->pConnector )
		return drm->pConnector->IsVRRCapable();

	return false;
}

bool drm_supports_hdr( struct drm_t *drm, uint16_t *maxCLL, uint16_t *maxFALL )
{
	if ( drm->pConnector && drm->pConnector->GetHDRInfo().SupportsHDR() )
	{
		if ( maxCLL )
			*maxCLL = drm->pConnector->GetHDRInfo().uMaxContentLightLevel;
		if ( maxFALL )
			*maxFALL = drm->pConnector->GetHDRInfo().uMaxFrameAverageLuminance;
		return true;
	}

	return false;
}

void drm_set_hdr_state(struct drm_t *drm, bool enabled) {
	if (drm->enable_hdr != enabled) {
		drm->needs_modeset = true;
		drm->enable_hdr = enabled;
	}
}

const char *drm_get_connector_name(struct drm_t *drm)
{
	if ( !drm->pConnector )
		return nullptr;

	return drm->pConnector->GetName();
}

const char *drm_get_device_name(struct drm_t *drm)
{
	return drm->device_name;
}

std::pair<uint32_t, uint32_t> drm_get_connector_identifier(struct drm_t *drm)
{
	if ( !drm->pConnector )
		return { 0u, 0u };

	return std::make_pair(drm->pConnector->GetModeConnector()->connector_type, drm->pConnector->GetModeConnector()->connector_type_id);
}

std::shared_ptr<wlserver_hdr_metadata> drm_create_hdr_metadata_blob(struct drm_t *drm, hdr_output_metadata *metadata)
{
	uint32_t blob = 0;
	if (!BIsNested())
	{
		int ret = drmModeCreatePropertyBlob(drm->fd, metadata, sizeof(*metadata), &blob);

		if (ret != 0) {
			drm_log.errorf("Failed to create blob for HDR_OUTPUT_METADATA. (%s) Falling back to null blob.", strerror(-ret));
			blob = 0;
		}


		if (!blob)
			return nullptr;
	}

	return std::make_shared<wlserver_hdr_metadata>(metadata, blob);
}
void drm_destroy_blob(struct drm_t *drm, uint32_t blob)
{
	drmModeDestroyPropertyBlob(drm->fd, blob);
}

std::shared_ptr<wlserver_ctm> drm_create_ctm(struct drm_t *drm, glm::mat3x4 ctm)
{
	uint32_t blob = 0;
	if (!BIsNested())
	{
		drm_color_ctm2 ctm2;
		for (uint32_t i = 0; i < 12; i++)
		{
			float *data = (float*)&ctm;
			ctm2.matrix[i] = drm_calc_s31_32(data[i]);
		}

		int ret = drmModeCreatePropertyBlob(drm->fd, &ctm2, sizeof(ctm2), &blob);

		if (ret != 0) {
			drm_log.errorf("Failed to create blob for CTM. (%s) Falling back to null blob.", strerror(-ret));
			blob = 0;
		}

		if (!blob)
			return nullptr;
	}

	return std::make_shared<wlserver_ctm>(ctm, blob);
}


bool drm_supports_color_mgmt(struct drm_t *drm)
{
	if ( g_bForceDisableColorMgmt )
		return false;

	if ( !drm->pPrimaryPlane )
		return false;

	return drm->pPrimaryPlane->GetProperties().VALVE1_PLANE_CTM.has_value();
}

void drm_get_native_colorimetry( struct drm_t *drm,
	displaycolorimetry_t *displayColorimetry, EOTF *displayEOTF,
	displaycolorimetry_t *outputEncodingColorimetry, EOTF *outputEncodingEOTF )
{
	if ( !drm || !drm->pConnector )
	{
		*displayColorimetry = displaycolorimetry_709;
		*displayEOTF = EOTF_Gamma22;
		*outputEncodingColorimetry = displaycolorimetry_709;
		*outputEncodingEOTF = EOTF_Gamma22;
		return;
	}

	*displayColorimetry = drm->pConnector->GetDisplayColorimetry();
	*displayEOTF = EOTF_Gamma22;

	// For HDR10 output, expected content colorspace != native colorspace.
	if ( g_bOutputHDREnabled && drm->pConnector->GetHDRInfo().IsHDR10() )
	{
		*outputEncodingColorimetry = displaycolorimetry_2020;
		*outputEncodingEOTF = drm->pConnector->GetHDRInfo().eOutputEncodingEOTF;
	}
	else
	{
		*outputEncodingColorimetry = drm->pConnector->GetDisplayColorimetry();
		*outputEncodingEOTF = EOTF_Gamma22;
	}
}


std::span<const uint32_t> drm_get_valid_refresh_rates( struct drm_t *drm )
{
	if ( drm && drm->pConnector )
		return drm->pConnector->GetValidDynamicRefreshRates();

	return std::span<const uint32_t>{};
}
