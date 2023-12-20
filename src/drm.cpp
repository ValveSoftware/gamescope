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

extern "C" {
#include <wlr/types/wlr_buffer.h>
}

#include "drm.hpp"
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

struct drm_t g_DRM = {};

uint32_t g_nDRMFormat = DRM_FORMAT_INVALID;
uint32_t g_nDRMFormatOverlay = DRM_FORMAT_INVALID; // for partial composition, we may have more limited formats than base planes + alpha.
bool g_bRotated = false;
bool g_bUseLayers = true;
bool g_bDebugLayers = false;
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

enum drm_mode_generation g_drmModeGeneration = DRM_MODE_GENERATE_CVT;
enum g_panel_orientation g_drmModeOrientation = PANEL_ORIENTATION_AUTO;
std::atomic<uint64_t> g_drmEffectiveOrientation[DRM_SCREEN_TYPE_COUNT]{ {DRM_MODE_ROTATE_0}, {DRM_MODE_ROTATE_0} };

bool g_bForceDisableColorMgmt = false;

static LogScope drm_log("drm");
static LogScope drm_verbose_log("drm", LOG_SILENT);

static std::map< std::string, std::string > pnps = {};

drm_screen_type drm_get_connector_type(drmModeConnector *connector);
static void drm_unset_mode( struct drm_t *drm );
static void drm_unset_connector( struct drm_t *drm );

static uint32_t steam_deck_display_rates[] =
{
	40, 41, 42, 43, 44, 45, 46, 47, 48, 49,
	50, 51, 52, 53, 54, 55, 56, 57, 58, 59,
	60,
};

static uint32_t galileo_display_rates[] =
{
	45,47,48,49,
	50,51,53,55,56,59,
	60,62,64,65,66,68,
	72,73,76,77,78,
	80,81,82,84,85,86,87,88,
	90,
};

static uint32_t get_conn_display_info_flags(struct drm_t *drm, struct connector *connector)
{
	if (!connector)
		return 0;

	uint32_t flags = 0;
	if ( drm_get_connector_type(connector->connector) == DRM_SCREEN_TYPE_INTERNAL )
		flags |= GAMESCOPE_CONTROL_DISPLAY_FLAG_INTERNAL_DISPLAY;
	if ( connector->vrr_capable )
		flags |= GAMESCOPE_CONTROL_DISPLAY_FLAG_SUPPORTS_VRR;
	if ( connector->metadata.supportsST2084 )
		flags |= GAMESCOPE_CONTROL_DISPLAY_FLAG_SUPPORTS_HDR;

	return flags;
}

void drm_send_gamescope_control(wl_resource *control, struct drm_t *drm)
{
	// assumes wlserver_lock HELD!

	if ( !drm->connector )
		return;

	auto& conn = drm->connector;

	uint32_t flags = get_conn_display_info_flags( drm, drm->connector );

	struct wl_array display_rates;
	wl_array_init(&display_rates);
	if ( conn->valid_display_rates.size() )
	{
		size_t size = conn->valid_display_rates.size() * sizeof(uint32_t);
		uint32_t *ptr = (uint32_t *)wl_array_add( &display_rates, size );
		memcpy( ptr, conn->valid_display_rates.data(), size );
	}
	gamescope_control_send_active_display_info( control, drm->connector->name, drm->connector->make, drm->connector->model, flags, &display_rates );
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

static struct crtc *find_crtc_for_connector(struct drm_t *drm, const struct connector *connector) {
	for (size_t i = 0; i < drm->crtcs.size(); i++) {
		uint32_t crtc_mask = 1 << i;
		if (connector->possible_crtcs & crtc_mask)
			return &drm->crtcs[i];
	}

	return nullptr;
}

static bool get_plane_formats(struct drm_t *drm, struct plane *plane, struct wlr_drm_format_set *formats) {
	for (uint32_t k = 0; k < plane->plane->count_formats; k++) {
		uint32_t fmt = plane->plane->formats[k];
		wlr_drm_format_set_add(formats, fmt, DRM_FORMAT_MOD_INVALID);
	}

	if (plane->props.count("IN_FORMATS") > 0) {
		uint64_t blob_id = plane->initial_prop_values["IN_FORMATS"];

		drmModePropertyBlobRes *blob = drmModeGetPropertyBlob(drm->fd, blob_id);
		if (!blob) {
			drm_log.errorf_errno("drmModeGetPropertyBlob(IN_FORMATS) failed");
			return false;
		}

		struct drm_format_modifier_blob *data =
			(struct drm_format_modifier_blob *)blob->data;
		uint32_t *fmts = (uint32_t *)((char *)data + data->formats_offset);
		struct drm_format_modifier *mods = (struct drm_format_modifier *)
			((char *)data + data->modifiers_offset);
		for (uint32_t i = 0; i < data->count_modifiers; ++i) {
			for (int j = 0; j < 64; ++j) {
				if (mods[i].formats & ((uint64_t)1 << j)) {
					wlr_drm_format_set_add(formats,
						fmts[j + mods[i].offset], mods[i].modifier);
				}
			}
		}

		drmModeFreePropertyBlob(blob);
	}

	return true;
}

static const char *get_enum_name(const drmModePropertyRes *prop, uint64_t value)
{
	for (int i = 0; i < prop->count_enums; i++) {
		if (prop->enums[i].value == value)
			return prop->enums[i].name;
	}
	return nullptr;
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
static struct plane *find_primary_plane(struct drm_t *drm)
{
	struct plane *primary = nullptr;

	for (size_t i = 0; i < drm->planes.size(); i++) {
		struct plane *plane = &drm->planes[i];

		if (!(plane->plane->possible_crtcs & (1 << drm->crtc_index)))
			continue;

		uint64_t plane_type = drm->planes[i].initial_prop_values["type"];
		if (plane_type == DRM_PLANE_TYPE_PRIMARY) {
			primary = plane;
			break;
		}
	}

	if (primary == nullptr)
		return nullptr;

	return primary;
}

static void drm_unlock_fb_internal( struct drm_t *drm, struct fb *fb );

std::atomic<uint64_t> g_nCompletedPageFlipCount = { 0u };

extern void mangoapp_output_update( uint64_t vblanktime );
static void page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, unsigned int crtc_id, void *data)
{
	uint64_t flipcount = (uint64_t)data;
	g_nCompletedPageFlipCount = flipcount;

	if ( g_DRM.crtc->id != crtc_id )
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

static const drmModePropertyRes *get_prop(struct drm_t *drm, uint32_t prop_id)
{
	if (drm->props.count(prop_id) > 0) {
		return drm->props[prop_id];
	}

	drmModePropertyRes *prop = drmModeGetProperty(drm->fd, prop_id);
	if (!prop) {
		drm_log.errorf_errno("drmModeGetProperty failed");
		return nullptr;
	}

	drm->props[prop_id] = prop;
	return prop;
}

static bool get_object_properties(struct drm_t *drm, uint32_t obj_id, uint32_t obj_type, std::map<std::string, const drmModePropertyRes *> &map, std::map<std::string, uint64_t> &values)
{
	drmModeObjectProperties *props = drmModeObjectGetProperties(drm->fd, obj_id, obj_type);
	if (!props) {
		drm_log.errorf_errno("drmModeObjectGetProperties failed");
		return false;
	}

	map = {};
	values = {};

	for (uint32_t i = 0; i < props->count_props; i++) {
		const drmModePropertyRes *prop = get_prop(drm, props->props[i]);
		if (!prop) {
			return false;
		}
		map[prop->name] = prop;
		values[prop->name] = props->prop_values[i];
	}

	drmModeFreeObjectProperties(props);
	return true;
}

static bool compare_modes( drmModeModeInfo mode1, drmModeModeInfo mode2 )
{
	bool goodRefresh1 = mode1.vrefresh >= 60;
	bool goodRefresh2 = mode2.vrefresh >= 60;
	if (goodRefresh1 != goodRefresh2)
		return goodRefresh1;

	bool preferred1 = mode1.type & DRM_MODE_TYPE_PREFERRED;
	bool preferred2 = mode2.type & DRM_MODE_TYPE_PREFERRED;
	if (preferred1 != preferred2)
		return preferred1;

	int area1 = mode1.hdisplay * mode1.vdisplay;
	int area2 = mode2.hdisplay * mode2.vdisplay;
	if (area1 != area2)
		return area1 > area2;

	return mode1.vrefresh > mode2.vrefresh;
}

static void
drm_hdr_parse_edid(drm_t *drm, struct connector *connector, const struct di_edid *edid)
{
	struct connector_metadata_t *metadata = &connector->metadata;

	const struct di_edid_chromaticity_coords* chroma = di_edid_get_chromaticity_coords(edid);
	const struct di_cta_hdr_static_metadata_block* hdr_static_metadata = NULL;
	const struct di_cta_colorimetry_block* colorimetry = NULL;

	const struct di_edid_cta* cta = NULL;
	const struct di_edid_ext* const* exts = di_edid_get_extensions(edid);
	for (; *exts != NULL; exts++) {
		if ((cta = di_edid_ext_get_cta(*exts)))
			break;
	}

	if (cta) {
		const struct di_cta_data_block* const* blocks = di_edid_cta_get_data_blocks(cta);
		for (; *blocks != NULL; blocks++) {
			if (!hdr_static_metadata && (hdr_static_metadata = di_cta_data_block_get_hdr_static_metadata(*blocks)))
				continue;
			if (!colorimetry && (colorimetry = di_cta_data_block_get_colorimetry(*blocks)))
				continue;
		}
	}

	struct hdr_metadata_infoframe *infoframe = &metadata->defaultHdrMetadata.hdmi_metadata_type1;

	if (chroma) {
		infoframe->display_primaries[0].x = color_xy_to_u16(chroma->red_x);
		infoframe->display_primaries[0].y = color_xy_to_u16(chroma->red_y);
		infoframe->display_primaries[1].x = color_xy_to_u16(chroma->green_x);
		infoframe->display_primaries[1].y = color_xy_to_u16(chroma->green_y);
		infoframe->display_primaries[2].x = color_xy_to_u16(chroma->blue_x);
		infoframe->display_primaries[2].y = color_xy_to_u16(chroma->blue_y);
		infoframe->white_point.x = color_xy_to_u16(chroma->white_x);
		infoframe->white_point.y = color_xy_to_u16(chroma->white_y);
	}

	/* Some sane defaults for SDR for displays with missing data... */
	infoframe->max_display_mastering_luminance = nits_to_u16(1000.0f);
	infoframe->min_display_mastering_luminance = nits_to_u16_dark(0.0f);
	infoframe->max_cll = nits_to_u16(400.0f);
	infoframe->max_fall = nits_to_u16(400.0f);

	if (hdr_static_metadata) {
		if (hdr_static_metadata->desired_content_max_luminance)
			infoframe->max_display_mastering_luminance = nits_to_u16(hdr_static_metadata->desired_content_max_luminance);
		if (hdr_static_metadata->desired_content_min_luminance)
			infoframe->min_display_mastering_luminance = nits_to_u16_dark(hdr_static_metadata->desired_content_min_luminance);
		/* To be filled in by the app based on the scene, default to desired_content_max_luminance.
		 *
		 * Using display's max_fall for the default metadata max_cll to avoid displays
		 * overcompensating with tonemapping for SDR content.
		 */
		float default_max_fall = hdr_static_metadata->desired_content_max_frame_avg_luminance
			? hdr_static_metadata->desired_content_max_frame_avg_luminance
			: hdr_static_metadata->desired_content_max_luminance;

		if (default_max_fall) {
			infoframe->max_cll = nits_to_u16(default_max_fall);
			infoframe->max_fall = nits_to_u16(default_max_fall);
		}
	}

	metadata->supportsST2084 =
		chroma &&
		colorimetry && colorimetry->bt2020_rgb &&
		hdr_static_metadata && hdr_static_metadata->eotfs && hdr_static_metadata->eotfs->pq;

	if (metadata->supportsST2084) {
		metadata->defaultHdrMetadata.metadata_type = 0;
		infoframe->metadata_type = 0;
		infoframe->eotf = HDMI_EOTF_ST2084;

		metadata->hdr10_metadata_blob = drm_create_hdr_metadata_blob(drm, &metadata->defaultHdrMetadata);

		if (metadata->hdr10_metadata_blob == nullptr) {
			fprintf(stderr, "Failed to create blob for HDR_OUTPUT_METADATA. Falling back to null blob.\n");
		}
	}

	const char *coloroverride = getenv( "GAMESCOPE_INTERNAL_COLORIMETRY_OVERRIDE" );
	if (coloroverride && drm_get_connector_type(connector->connector) == DRM_SCREEN_TYPE_INTERNAL)
	{
		if (sscanf( coloroverride, "%f %f %f %f %f %f %f %f",
			&metadata->colorimetry.primaries.r.x, &metadata->colorimetry.primaries.r.y,
			&metadata->colorimetry.primaries.g.x, &metadata->colorimetry.primaries.g.y,
			&metadata->colorimetry.primaries.b.x, &metadata->colorimetry.primaries.b.y,
			&metadata->colorimetry.white.x, &metadata->colorimetry.white.y ) == 8 )
		{
			drm_log.infof("[colorimetry]: GAMESCOPE_INTERNAL_COLORIMETRY_OVERRIDE detected");
		}
		else
		{
			drm_log.errorf("[colorimetry]: GAMESCOPE_INTERNAL_COLORIMETRY_OVERRIDE specified, but could not parse \"rx ry gx gy bx by wx wy\"");
		}
	}
	else if (connector->is_steam_deck_display && !connector->is_galileo_display)
	{
		drm_log.infof("[colorimetry]: Steam Deck (internal display) detected.");

		// Hardcode Steam Deck display info to support
		// BIOSes with missing info for this in EDID.
		drm_log.infof("[colorimetry]: using default steamdeck colorimetry");
		metadata->colorimetry = displaycolorimetry_steamdeck_measured;
		metadata->eotf = EOTF_Gamma22;
	}
	else if (chroma && chroma->red_x != 0.0f)
	{
		drm_log.infof("[colorimetry]: EDID with colorimetry detected. Using it");
		metadata->colorimetry.primaries = { { chroma->red_x, chroma->red_y }, { chroma->green_x, chroma->green_y }, { chroma->blue_x, chroma->blue_y } };
		metadata->colorimetry.white = { chroma->white_x, chroma->white_y };
		metadata->eotf = infoframe->eotf == HDMI_EOTF_ST2084 ? EOTF_PQ : EOTF_Gamma22;
	}
	else
	{
		// No valid chroma data in the EDID, fill it in ourselves.
		if (infoframe->eotf == HDMI_EOTF_ST2084)
		{
			drm_log.infof("[colorimetry]: EDID does not define colorimetry. Assuming rec2020 based on HDMI_EOTF_ST2084 support");
			// Fallback to 2020 primaries for HDR
			metadata->colorimetry = displaycolorimetry_2020;
			metadata->eotf = EOTF_PQ;
		}
		else
		{
			// Fallback to 709 primaries for SDR
			drm_log.infof("[colorimetry]: EDID does not define colorimetry. Assuming rec709 / gamma 2.2");
			metadata->colorimetry = displaycolorimetry_709;
			metadata->eotf = EOTF_Gamma22;
		}
	}

	drm_log.infof("[colorimetry]: r %f %f", metadata->colorimetry.primaries.r.x, metadata->colorimetry.primaries.r.y);
	drm_log.infof("[colorimetry]: g %f %f", metadata->colorimetry.primaries.g.x, metadata->colorimetry.primaries.g.y);
	drm_log.infof("[colorimetry]: b %f %f", metadata->colorimetry.primaries.b.x, metadata->colorimetry.primaries.b.y);
	drm_log.infof("[colorimetry]: w %f %f", metadata->colorimetry.white.x, metadata->colorimetry.white.y);
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

static void create_patched_edid( const uint8_t *orig_data, size_t orig_size, drm_t *drm, struct connector *conn )
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
	if ( g_bForceHDRSupportDebug && !conn->metadata.supportsST2084 )
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
	if (!drm || !drm->connector)
		return;

	create_patched_edid(drm->connector->edid_data.data(), drm->connector->edid_data.size(), drm, drm->connector);
}

#define GALILEO_SDC_PID 0x3003
#define GALILEO_BOE_PID 0x3004

static void parse_edid( drm_t *drm, struct connector *conn)
{
	memset(conn->make_pnp, 0, sizeof(conn->make_pnp));
	free(conn->make);
	conn->make = NULL;
	free(conn->model);
	conn->model = NULL;

	if (conn->props.count("EDID") == 0) {
		return;
	}

	uint64_t blob_id = conn->initial_prop_values["EDID"];
	if (blob_id == 0) {
		return;
	}

	drmModePropertyBlobRes *blob = drmModeGetPropertyBlob(drm->fd, blob_id);
	if (!blob) {
		drm_log.errorf_errno("drmModeGetPropertyBlob(EDID) failed");
		return;
	}

	struct di_info *info = di_info_parse_edid(blob->data, blob->length);
	if (!info) {
		drmModeFreePropertyBlob(blob);
		drm_log.errorf("Failed to parse edid");
		return;
	}

	conn->edid_data = std::vector<uint8_t>((const uint8_t*)blob->data, ((const uint8_t*)(blob->data)) + blob->length);

	drmModeFreePropertyBlob(blob);

	const struct di_edid *edid = di_info_get_edid(info);

	const struct di_edid_vendor_product *vendor_product = di_edid_get_vendor_product(edid);
	char pnp_id[] = {
		vendor_product->manufacturer[0],
		vendor_product->manufacturer[1],
		vendor_product->manufacturer[2],
		'\0',
	};
	memcpy(conn->make_pnp, pnp_id, sizeof(pnp_id));
	if (pnps.count(pnp_id) > 0) {
		conn->make = strdup(pnps[pnp_id].c_str());
	}
	else {
		// Some vendors like AOC, don't have a PNP id listed,
		// but their name is literally just "AOC", so just
		// use the PNP name directly.
		conn->make = strdup(pnp_id);
	}

	const struct di_edid_display_descriptor *const *descriptors = di_edid_get_display_descriptors(edid);
	for (size_t i = 0; descriptors[i] != NULL; i++) {
		const struct di_edid_display_descriptor *desc = descriptors[i];
		if (di_edid_display_descriptor_get_tag(desc) == DI_EDID_DISPLAY_DESCRIPTOR_PRODUCT_NAME) {
			conn->model = strdup(di_edid_display_descriptor_get_string(desc));
		}
	}

	drm_log.infof("Connector make %s model %s", conn->make_pnp, conn->model );

	conn->is_steam_deck_display =
		(strcmp(conn->make_pnp, "WLC") == 0 && strcmp(conn->model, "ANX7530 U") == 0) ||
		(strcmp(conn->make_pnp, "ANX") == 0 && strcmp(conn->model, "ANX7530 U") == 0) ||
		(strcmp(conn->make_pnp, "VLV") == 0 && strcmp(conn->model, "ANX7530 U") == 0) ||
		(strcmp(conn->make_pnp, "VLV") == 0 && strcmp(conn->model, "Jupiter") == 0);

	if ((vendor_product->product == GALILEO_SDC_PID) || (vendor_product->product == GALILEO_BOE_PID)) {
		conn->is_galileo_display = vendor_product->product;
		conn->valid_display_rates = std::span(galileo_display_rates);
	} else {
		conn->is_galileo_display = 0;
		if ( conn->is_steam_deck_display )
			conn->valid_display_rates = std::span(steam_deck_display_rates);
	}

	drm_hdr_parse_edid(drm, conn, edid);

	di_info_destroy(info);
}

static bool refresh_state( drm_t *drm )
{
	drmModeRes *resources = drmModeGetResources(drm->fd);
	if (resources == nullptr) {
		drm_log.errorf_errno("drmModeGetResources failed");
		return false;
	}

	// Add connectors which appeared
	for (int i = 0; i < resources->count_connectors; i++) {
		uint32_t conn_id = resources->connectors[i];

		if (drm->connectors.count(conn_id) == 0) {
			struct connector conn = { .id = conn_id };
			drm->connectors[conn_id] = conn;
		}
	}

	// Remove connectors which disappeared
	auto it = drm->connectors.begin();
	while (it != drm->connectors.end()) {
		struct connector *conn = &it->second;

		bool found = false;
		for (int j = 0; j < resources->count_connectors; j++) {
			if (resources->connectors[j] == conn->id) {
				found = true;
				break;
			}
		}

		if (!found) {
			drm_log.debugf("connector '%s' disappeared", conn->name);

			if (drm->connector == conn) {
				drm_log.infof("current connector '%s' disappeared", conn->name);
				drm->connector = nullptr;
			}

			free(conn->name);
			conn->name = nullptr;
			conn->metadata.hdr10_metadata_blob = nullptr;
			drmModeFreeConnector(conn->connector);
			it = drm->connectors.erase(it);
		} else {
			it++;
		}
	}

	drmModeFreeResources(resources);

	// Re-probe connectors props and status
	for (auto &kv : drm->connectors) {
		struct connector *conn = &kv.second;
		if (conn->connector != nullptr) {
			conn->metadata.hdr10_metadata_blob = nullptr;
			drmModeFreeConnector(conn->connector);
		}

		conn->connector = drmModeGetConnector(drm->fd, conn->id);
		if (conn->connector == nullptr) {
			drm_log.errorf_errno("drmModeGetConnector failed");
			return false;
		}

		if (!get_object_properties(drm, conn->id, DRM_MODE_OBJECT_CONNECTOR, conn->props, conn->initial_prop_values)) {
			return false;
		}

		/* sort modes by preference: preferred flag, then highest area, then
		 * highest refresh rate */
		std::stable_sort(conn->connector->modes, conn->connector->modes + conn->connector->count_modes, compare_modes);

		parse_edid(drm, conn);

		if ( conn->name != nullptr )
		{
			free(conn->name);
			conn->name = nullptr;
		}

		const char *type_str = drmModeGetConnectorTypeName(conn->connector->connector_type);
		if (!type_str)
			type_str = "Unknown";

		char name[128] = {};
		snprintf(name, sizeof(name), "%s-%d", type_str, conn->connector->connector_type_id);
		conn->name = strdup(name);

		conn->possible_crtcs = drmModeConnectorGetPossibleCrtcs(drm->fd, conn->connector);
		if (!conn->possible_crtcs)
			drm_log.errorf_errno("drmModeConnectorGetPossibleCrtcs failed");

		conn->has_colorspace = conn->props.contains( "Colorspace" );
		conn->has_hdr_output_metadata = conn->props.contains( "HDR_OUTPUT_METADATA" );
		conn->has_content_type = conn->props.contains( "content type" );

		conn->current.crtc_id = conn->initial_prop_values["CRTC_ID"];
		if (conn->has_colorspace)
			conn->current.colorspace = conn->initial_prop_values["Colorspace"];
		if (conn->has_hdr_output_metadata)
			conn->current.hdr_output_metadata = std::make_shared<wlserver_hdr_metadata>(nullptr, conn->initial_prop_values["HDR_OUTPUT_METADATA"], false);
		if (conn->has_content_type)
			conn->current.content_type = conn->initial_prop_values["content type"];

		conn->target_refresh = 0;

		conn->vrr_capable = !!conn->initial_prop_values["vrr_capable"];

		drm_log.debugf("found new connector '%s'", conn->name);
	}

	for (size_t i = 0; i < drm->crtcs.size(); i++) {
		struct crtc *crtc = &drm->crtcs[i];
		if (!get_object_properties(drm, crtc->id, DRM_MODE_OBJECT_CRTC, crtc->props, crtc->initial_prop_values)) {
			return false;
		}

		crtc->has_gamma_lut = crtc->props.contains( "GAMMA_LUT" );
		if (!crtc->has_gamma_lut)
			drm_log.infof("CRTC %" PRIu32 " has no gamma LUT support", crtc->id);
		crtc->has_degamma_lut = crtc->props.contains( "DEGAMMA_LUT" );
		if (!crtc->has_degamma_lut)
			drm_log.infof("CRTC %" PRIu32 " has no degamma LUT support", crtc->id);
		crtc->has_ctm = crtc->props.contains( "CTM" );
		if (!crtc->has_ctm)
			drm_log.infof("CRTC %" PRIu32 " has no CTM support", crtc->id);
		crtc->has_vrr_enabled = crtc->props.contains( "VRR_ENABLED" );
		if (!crtc->has_vrr_enabled)
			drm_log.infof("CRTC %" PRIu32 " has no VRR_ENABLED support", crtc->id);
		crtc->has_valve1_regamma_tf = crtc->props.contains( "VALVE1_CRTC_REGAMMA_TF" );
		if (!crtc->has_valve1_regamma_tf)
			drm_log.infof("CRTC %" PRIu32 " has no VALVE1_CRTC_REGAMMA_TF support", crtc->id);

		crtc->current.active = crtc->initial_prop_values["ACTIVE"];
		if (crtc->has_vrr_enabled)
			drm->current.vrr_enabled = crtc->initial_prop_values["VRR_ENABLED"];
		if (crtc->has_valve1_regamma_tf)
			drm->current.output_tf = (drm_valve1_transfer_function) crtc->initial_prop_values["VALVE1_CRTC_REGAMMA_TF"];
	}

	for (size_t i = 0; i < drm->planes.size(); i++) {
		struct plane *plane = &drm->planes[i];
		if (!get_object_properties(drm, plane->id, DRM_MODE_OBJECT_PLANE, plane->props, plane->initial_prop_values)) {
			return false;
		}
		plane->has_color_mgmt = plane->props.contains( "VALVE1_PLANE_BLEND_TF" );
	}

	return true;
}

static bool get_resources(struct drm_t *drm)
{
	drmModeRes *resources = drmModeGetResources(drm->fd);
	if (resources == nullptr) {
		drm_log.errorf_errno("drmModeGetResources failed");
		return false;
	}

	for (int i = 0; i < resources->count_crtcs; i++) {
		struct crtc crtc = { .id = resources->crtcs[i] };

		crtc.crtc = drmModeGetCrtc(drm->fd, crtc.id);
		if (crtc.crtc == nullptr) {
			drm_log.errorf_errno("drmModeGetCrtc failed");
			return false;
		}

		drm->crtcs.push_back(crtc);
	}

	drmModeFreeResources(resources);

	drmModePlaneRes *plane_resources = drmModeGetPlaneResources(drm->fd);
	if (!plane_resources) {
		drm_log.errorf_errno("drmModeGetPlaneResources failed");
		return false;
	}

	for (uint32_t i = 0; i < plane_resources->count_planes; i++) {
		struct plane plane = { .id = plane_resources->planes[i] };

		plane.plane = drmModeGetPlane(drm->fd, plane.id);
		if (plane.plane == nullptr) {
			drm_log.errorf_errno("drmModeGetPlane failed");
			return false;
		}

		drm->planes.push_back(plane);
	}

	drmModeFreePlaneResources(plane_resources);

	if (!refresh_state(drm))
		return false;

	for (size_t i = 0; i < drm->crtcs.size(); i++) {
		struct crtc *crtc = &drm->crtcs[i];
		crtc->pending = crtc->current;
	}

	return true;
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
	if (drm->connector && drm->connector->connector->connection != DRM_MODE_CONNECTED) {
		drm_log.infof("current connector '%s' disconnected", drm->connector->name);
		drm->connector = nullptr;
	}

	struct connector *best = nullptr;
	int best_priority = INT_MAX;
	for (auto &kv : drm->connectors) {
		struct connector *conn = &kv.second;

		if (conn->connector->connection != DRM_MODE_CONNECTED)
			continue;

		if (drm->force_internal && drm_get_connector_type(conn->connector) == DRM_SCREEN_TYPE_EXTERNAL)
			continue;

		int priority = get_connector_priority(drm, conn->name);
		if (priority < best_priority) {
			best = conn;
			best_priority = priority;
		}
	}

	if (!force) {
		if ((!best && drm->connector) || (best && best == drm->connector)) {
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
	if (drm_get_connector_type(best->connector) == DRM_SCREEN_TYPE_INTERNAL) {
		snprintf(description, sizeof(description), "Internal screen");
	} else if (best->make && best->model) {
		snprintf(description, sizeof(description), "%s %s", best->make, best->model);
	} else if (best->model) {
		snprintf(description, sizeof(description), "%s", best->model);
	} else {
		snprintf(description, sizeof(description), "External screen");
	}

	const drmModeModeInfo *mode = nullptr;
	if ( drm->preferred_width != 0 || drm->preferred_height != 0 || drm->preferred_refresh != 0 )
	{
		mode = find_mode(best->connector, drm->preferred_width, drm->preferred_height, drm->preferred_refresh);
	}

	if (!mode && drm_get_connector_type(best->connector) == DRM_SCREEN_TYPE_EXTERNAL) {
		saved_mode mode_info;
		if (get_saved_mode(description, mode_info))
			mode = find_mode(best->connector, mode_info.width, mode_info.height, mode_info.refresh);
	}

	if (!mode) {
		mode = find_mode(best->connector, 0, 0, 0);
	}

	if (!mode) {
		drm_log.errorf("could not find mode!");
		return false;
	}

	best->target_refresh = mode->vrefresh;

	if (!drm_set_mode(drm, mode)) {
		return false;
	}

	const struct wlserver_output_info wlserver_output_info = {
		.description = description,
		.phys_width = (int) best->connector->mmWidth,
		.phys_height = (int) best->connector->mmHeight,
	};
	wlserver_lock();
	wlserver_set_output_info(&wlserver_output_info);
	wlserver_unlock();

	if (!initial)
		create_patched_edid(best->edid_data.data(), best->edid_data.size(), drm, best);

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

bool init_drm(struct drm_t *drm, int width, int height, int refresh, bool wants_adaptive_sync)
{
	load_pnps();

	drm->wants_vrr_enabled = wants_adaptive_sync;
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
	for (const auto &kv : drm->connectors) {
		const struct connector *conn = &kv.second;

		const char *status_str = "disconnected";
		if ( conn->connector->connection == DRM_MODE_CONNECTED )
			status_str = "connected";

		drm_log.infof("  %s (%s)", conn->name, status_str);
	}

	drm->connector_priorities = parse_connector_priorities( g_sOutputName );

	if (!setup_best_connector(drm, true, true)) {
		return false;
	}

	// Fetch formats which can be scanned out
	for (size_t i = 0; i < drm->planes.size(); i++) {
		struct plane *plane = &drm->planes[i];
		if (!get_plane_formats(drm, plane, &drm->formats))
			return false;
	}

	// TODO: intersect primary planes formats instead
	struct plane *primary_plane = drm->primary;
	if (primary_plane == nullptr) {
		primary_plane = find_primary_plane(drm);
	}
	if (primary_plane == nullptr) {
		drm_log.errorf("Failed to find a primary plane");
		return false;
	}

	if (!get_plane_formats(drm, primary_plane, &drm->primary_formats)) {
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

	if (g_bUseLayers) {
		liftoff_log_set_priority(g_bDebugLayers ? LIFTOFF_DEBUG : LIFTOFF_ERROR);
	}

	hdr_output_metadata sdr_metadata;
	memset(&sdr_metadata, 0, sizeof(sdr_metadata));
	drm->sdr_static_metadata = drm_create_hdr_metadata_blob(drm, &sdr_metadata);

	drm->flipcount = 0;
	drm->needs_modeset = true;

	return true;
}

static int add_property(drmModeAtomicReq *req, uint32_t obj_id, std::map<std::string, const drmModePropertyRes *> &props, const char *name, uint64_t value)
{
	if ( props.count( name ) == 0 )
	{
		drm_log.errorf("no property %s on object %u", name, obj_id);
		return -ENOENT;
	}

	const drmModePropertyRes *prop = props[ name ];

	int ret = drmModeAtomicAddProperty(req, obj_id, prop->prop_id, value);
	if ( ret < 0 )
	{
		drm_log.errorf_errno( "drmModeAtomicAddProperty failed" );
	}
	return ret;
}

static int add_connector_property(drmModeAtomicReq *req, struct connector *conn, const char *name, uint64_t value)
{
	return add_property(req, conn->id, conn->props, name, value);
}

static int add_crtc_property(drmModeAtomicReq *req, struct crtc *crtc, const char *name, uint64_t value)
{
	return add_property(req, crtc->id, crtc->props, name, value);
}

static int add_plane_property(drmModeAtomicReq *req, struct plane *plane, const char *name, uint64_t value)
{
	return add_property(req, plane->id, plane->props, name, value);
}

static std::shared_ptr<wlserver_hdr_metadata> get_default_hdr_metadata(struct drm_t *drm, struct connector *connector)
{
	if ( !connector->has_hdr_output_metadata )
		return nullptr;
	if ( !connector->metadata.supportsST2084 )
		return nullptr;
	return drm->sdr_static_metadata;
}

void finish_drm(struct drm_t *drm)
{
	// Disable all connectors, CRTCs and planes. This is necessary to leave a
	// clean KMS state behind. Some other KMS clients might not support all of
	// the properties we use, e.g. "rotation" and Xorg don't play well
	// together.

	drmModeAtomicReq *req = drmModeAtomicAlloc();
	for ( auto &kv : drm->connectors ) {
		struct connector *conn = &kv.second;
		add_connector_property(req, conn, "CRTC_ID", 0);
		if (conn->has_colorspace)
			add_connector_property(req, conn, "Colorspace", 0);
		// HACK HACK: Setting to 0 doesn't disable HDR properly.
		// Set an SDR metadata blob.
		if (conn->has_hdr_output_metadata)
		{
			auto metadata = get_default_hdr_metadata( drm, conn );
			add_connector_property(req, conn, "HDR_OUTPUT_METADATA", metadata ? metadata->blob : 0);
		}
		if (conn->has_content_type)
			add_connector_property(req, conn, "content type", 0);
	}
	for ( size_t i = 0; i < drm->crtcs.size(); i++ ) {
		add_crtc_property(req, &drm->crtcs[i], "MODE_ID", 0);
		if ( drm->crtcs[i].has_gamma_lut )
			add_crtc_property(req, &drm->crtcs[i], "GAMMA_LUT", 0);
		if ( drm->crtcs[i].has_degamma_lut )
			add_crtc_property(req, &drm->crtcs[i], "DEGAMMA_LUT", 0);
		if ( drm->crtcs[i].has_ctm )
			add_crtc_property(req, &drm->crtcs[i], "CTM", 0);
		if ( drm->crtcs[i].has_vrr_enabled )
			add_crtc_property(req, &drm->crtcs[i], "VRR_ENABLED", 0);
		if ( drm->crtcs[i].has_valve1_regamma_tf )
			add_crtc_property(req, &drm->crtcs[i], "VALVE1_CRTC_REGAMMA_TF", 0);
		add_crtc_property(req, &drm->crtcs[i], "ACTIVE", 0);
	}
	for ( size_t i = 0; i < drm->planes.size(); i++ ) {
		struct plane *plane = &drm->planes[i];
		add_plane_property(req, plane, "FB_ID", 0);
		add_plane_property(req, plane, "CRTC_ID", 0);
		add_plane_property(req, plane, "SRC_X", 0);
		add_plane_property(req, plane, "SRC_Y", 0);
		add_plane_property(req, plane, "SRC_W", 0);
		add_plane_property(req, plane, "SRC_H", 0);
		add_plane_property(req, plane, "CRTC_X", 0);
		add_plane_property(req, plane, "CRTC_Y", 0);
		add_plane_property(req, plane, "CRTC_W", 0);
		add_plane_property(req, plane, "CRTC_H", 0);
		if (plane->props.count("rotation") > 0)
			add_plane_property(req, plane, "rotation", DRM_MODE_ROTATE_0);
		if (plane->props.count("alpha") > 0)
			add_plane_property(req, plane, "alpha", 0xFFFF);
		if (plane->props.count("VALVE1_PLANE_DEGAMMA_TF") > 0)
			add_plane_property(req, plane, "VALVE1_PLANE_DEGAMMA_TF", DRM_VALVE1_TRANSFER_FUNCTION_DEFAULT );
		if (plane->props.count("VALVE1_PLANE_HDR_MULT") > 0)
			add_plane_property(req, plane, "VALVE1_PLANE_HDR_MULT", 0x100000000ULL);
		if (plane->props.count("VALVE1_PLANE_SHAPER_TF") > 0)
			add_plane_property(req, plane, "VALVE1_PLANE_SHAPER_TF", DRM_VALVE1_TRANSFER_FUNCTION_DEFAULT );
		if (plane->props.count("VALVE1_PLANE_SHAPER_LUT") > 0)
			add_plane_property(req, plane, "VALVE1_PLANE_SHAPER_LUT", 0 );
		if (plane->props.count("VALVE1_PLANE_LUT3D") > 0)
			add_plane_property(req, plane, "VALVE1_PLANE_LUT3D", 0 );
		if (plane->props.count("VALVE1_PLANE_BLEND_TF") > 0)
			add_plane_property(req, plane, "VALVE1_PLANE_BLEND_TF", DRM_VALVE1_TRANSFER_FUNCTION_DEFAULT );
		if (plane->props.count("VALVE1_PLANE_BLEND_LUT") > 0)
			add_plane_property(req, plane, "VALVE1_PLANE_BLEND_LUT", 0 );
		if (plane->props.count("VALVE1_PLANE_CTM") > 0)
			add_plane_property(req, plane, "VALVE1_PLANE_CTM", 0 );
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

		for ( size_t i = 0; i < drm->crtcs.size(); i++ )
		{
			drm->crtcs[i].pending = drm->crtcs[i].current;
		}

		for (auto &kv : drm->connectors) {
			struct connector *conn = &kv.second;
			conn->pending = conn->current;
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

		for (auto & crtc : drm->crtcs)
		{
			crtc.current = crtc.pending;
		}

		for (auto &kv : drm->connectors) {
			struct connector *conn = &kv.second;
			conn->current = conn->pending;
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

static uint64_t determine_drm_orientation(struct drm_t *drm, struct connector *conn, const drmModeModeInfo *mode)
{
	drm_screen_type screenType = drm_get_connector_type(conn->connector);

	if (conn && conn->props.count("panel orientation") > 0)
	{
		const char *orientation = get_enum_name(conn->props["panel orientation"], conn->initial_prop_values["panel orientation"]);

		if (strcmp(orientation, "Normal") == 0)
		{
			return DRM_MODE_ROTATE_0;
		}
		else if (strcmp(orientation, "Left Side Up") == 0)
		{
			return DRM_MODE_ROTATE_90;
		}
		else if (strcmp(orientation, "Upside Down") == 0)
		{
			return DRM_MODE_ROTATE_180;
		}
		else if (strcmp(orientation, "Right Side Up") == 0)
		{
			return DRM_MODE_ROTATE_270;
		}
	}
	else
	{
		if (screenType == DRM_SCREEN_TYPE_INTERNAL && mode)
		{
			// Auto-detect portait mode for internal displays
			return mode->hdisplay < mode->vdisplay ? DRM_MODE_ROTATE_270 : DRM_MODE_ROTATE_0;
		}
		else
		{
			return DRM_MODE_ROTATE_0;
		}
	}

	return DRM_MODE_ROTATE_0;
}

/* Handle the orientation of the display */
static void update_drm_effective_orientation(struct drm_t *drm, struct connector *conn, const drmModeModeInfo *mode)
{
	drm_screen_type screenType = drm_get_connector_type(conn->connector);

	if (screenType == DRM_SCREEN_TYPE_INTERNAL)
	{
		switch ( g_drmModeOrientation )
		{
			case PANEL_ORIENTATION_0:
				g_drmEffectiveOrientation[screenType] = DRM_MODE_ROTATE_0;
				break;
			case PANEL_ORIENTATION_90:
				g_drmEffectiveOrientation[screenType] = DRM_MODE_ROTATE_90;
				break;
			case PANEL_ORIENTATION_180:
				g_drmEffectiveOrientation[screenType] = DRM_MODE_ROTATE_180;
				break;
			case PANEL_ORIENTATION_270:
				g_drmEffectiveOrientation[screenType] = DRM_MODE_ROTATE_270;
				break;
			case PANEL_ORIENTATION_AUTO:
				g_drmEffectiveOrientation[screenType] = determine_drm_orientation(drm, conn, mode);
				break;
		}
	}
	else
	{
		g_drmEffectiveOrientation[screenType] = determine_drm_orientation(drm, conn, mode);
	}
}

static void update_drm_effective_orientations(struct drm_t *drm, struct connector *conn, const drmModeModeInfo *mode)
{
	drm_screen_type screenType = drm_get_connector_type(conn->connector);
	if (screenType == DRM_SCREEN_TYPE_INTERNAL)
	{
		update_drm_effective_orientation(drm, conn, mode);
		return;
	}
	else if (screenType == DRM_SCREEN_TYPE_EXTERNAL)
	{
		update_drm_effective_orientation(drm, conn, mode);

		struct connector *internal_conn = nullptr;
		for ( auto &kv : drm->connectors ) {
			struct connector *kv_con = &kv.second;
			if (kv_con->connector)
			{
				drm_screen_type kv_screentype = drm_get_connector_type(kv_con->connector);
				if (kv_screentype == DRM_SCREEN_TYPE_INTERNAL)
				{
					internal_conn = kv_con;
					break;
				}
			}
		}

		if (internal_conn)
		{
			const drmModeModeInfo *default_internal_mode = find_mode(internal_conn->connector, 0, 0, 0);
			update_drm_effective_orientation(drm, internal_conn, default_internal_mode);
		}
	}
}

/* Prepares an atomic commit without using libliftoff */
static int
drm_prepare_basic( struct drm_t *drm, const struct FrameInfo_t *frameInfo )
{
	// Discard cases where our non-liftoff path is known to fail

	drm_screen_type screenType = drm_get_screen_type(drm);

	// It only supports one layer
	if ( frameInfo->layerCount > 1 )
	{
		drm_verbose_log.errorf("drm_prepare_basic: cannot handle %d layers", frameInfo->layerCount);
		return -EINVAL;
	}

	if ( frameInfo->layers[ 0 ].fbid == 0 )
	{
		drm_verbose_log.errorf("drm_prepare_basic: layer has no FB");
		return -EINVAL;
	}

	drmModeAtomicReq *req = drm->req;
	uint32_t fb_id = frameInfo->layers[ 0 ].fbid;

	drm->fbids_in_req.push_back( fb_id );

	add_plane_property(req, drm->primary, "rotation", g_drmEffectiveOrientation[screenType] );

	add_plane_property(req, drm->primary, "FB_ID", fb_id);
	add_plane_property(req, drm->primary, "CRTC_ID", drm->crtc->id);
	add_plane_property(req, drm->primary, "SRC_X", 0);
	add_plane_property(req, drm->primary, "SRC_Y", 0);

	const uint16_t srcWidth = frameInfo->layers[ 0 ].tex->width();
	const uint16_t srcHeight = frameInfo->layers[ 0 ].tex->height();

	add_plane_property(req, drm->primary, "SRC_W", srcWidth << 16);
	add_plane_property(req, drm->primary, "SRC_H", srcHeight << 16);

	gpuvis_trace_printf ( "legacy flip fb_id %u src %ix%i", fb_id,
						 srcWidth, srcHeight );

	int64_t crtcX = frameInfo->layers[ 0 ].offset.x * -1;
	int64_t crtcY = frameInfo->layers[ 0 ].offset.y * -1;
	int64_t crtcW = srcWidth / frameInfo->layers[ 0 ].scale.x;
	int64_t crtcH = srcHeight / frameInfo->layers[ 0 ].scale.y;

	if ( g_bRotated )
	{
		int64_t imageH = frameInfo->layers[ 0 ].tex->contentHeight() / frameInfo->layers[ 0 ].scale.y;

		int64_t tmp = crtcX;
		crtcX = g_nOutputHeight - imageH - crtcY;
		crtcY = tmp;

		tmp = crtcW;
		crtcW = crtcH;
		crtcH = tmp;
	}

	add_plane_property(req, drm->primary, "CRTC_X", crtcX);
	add_plane_property(req, drm->primary, "CRTC_Y", crtcY);
	add_plane_property(req, drm->primary, "CRTC_W", crtcW);
	add_plane_property(req, drm->primary, "CRTC_H", crtcH);

	gpuvis_trace_printf ( "crtc %li,%li %lix%li", crtcX, crtcY, crtcW, crtcH );

	// TODO: disable all planes except drm->primary

	unsigned test_flags = (drm->flags & DRM_MODE_ATOMIC_ALLOW_MODESET) | DRM_MODE_ATOMIC_TEST_ONLY;
	int ret = drmModeAtomicCommit( drm->fd, drm->req, test_flags, NULL );

	if ( ret != 0 && ret != -EINVAL && ret != -ERANGE ) {
		drm_log.errorf_errno( "drmModeAtomicCommit failed" );
	}

	return ret;
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

static int
drm_prepare_liftoff( struct drm_t *drm, const struct FrameInfo_t *frameInfo, bool needs_modeset )
{
	drm_screen_type screenType = drm_get_screen_type(drm);
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
	drm->pending.screen_type = drm_get_screen_type(drm);

	drm_update_vrr_state(drm);
	drm_update_color_mgmt(drm);

	drm->fbids_in_req.clear();

	bool needs_modeset = drm->needs_modeset.exchange(false);

	assert( drm->req == nullptr );
	drm->req = drmModeAtomicAlloc();

	bool bConnectorSupportsHDR = drm->connector->metadata.supportsST2084;
	bool bConnectorHDR = g_bOutputHDREnabled && bConnectorSupportsHDR;

	if (drm->connector != nullptr) {
		if (drm->connector->has_colorspace) {
			drm->connector->pending.colorspace = ( bConnectorHDR ) ? DRM_MODE_COLORIMETRY_BT2020_RGB : DRM_MODE_COLORIMETRY_DEFAULT;
		}

		if (drm->connector->has_content_type) {
			drm->connector->pending.content_type = DRM_MODE_CONTENT_TYPE_GAME;
		}

		if ( bConnectorHDR )
		{
			if (drm->connector->has_hdr_output_metadata) {
				auto hdr_output_metadata = get_default_hdr_metadata( drm, drm->connector );

				if ( drm->connector->metadata.hdr10_metadata_blob )
					hdr_output_metadata = drm->connector->metadata.hdr10_metadata_blob;

				auto feedback = steamcompmgr_get_base_layer_swapchain_feedback();
				if (feedback && feedback->hdr_metadata_blob)
					hdr_output_metadata = feedback->hdr_metadata_blob;

				drm->connector->pending.hdr_output_metadata = hdr_output_metadata;
			}
		}
		else
		{
			if (drm->connector->has_hdr_output_metadata && bConnectorSupportsHDR)
			{
				drm->connector->pending.hdr_output_metadata = drm->sdr_static_metadata;
			}
			else
			{
				drm->connector->pending.hdr_output_metadata = nullptr;
			}
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
	if ( drm->crtc != nullptr )
		flags |= DRM_MODE_PAGE_FLIP_EVENT;

	if ( async || g_bForceAsyncFlips )
		flags |= DRM_MODE_PAGE_FLIP_ASYNC;

	if ( needs_modeset ) {
		flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;

		// Disable all connectors and CRTCs

		for ( auto &kv : drm->connectors ) {
			struct connector *conn = &kv.second;

			if ( conn->current.crtc_id == 0 )
				continue;

			conn->pending.crtc_id = 0;
			int ret = add_connector_property( drm->req, conn, "CRTC_ID", 0 );
			if (ret < 0)
				return ret;

			if (conn->has_colorspace) {
				ret = add_connector_property( drm->req, conn, "Colorspace", 0 );
				if (ret < 0)
					return ret;
			}

			if (conn->has_hdr_output_metadata) {
				ret = add_connector_property( drm->req, conn, "HDR_OUTPUT_METADATA", 0 );
				if (ret < 0)
					return ret;
			}

			if (conn->has_content_type) {
				ret = add_connector_property( drm->req, conn, "content type", 0 );
				if (ret < 0)
					return ret;
			}
		}
		for ( size_t i = 0; i < drm->crtcs.size(); i++ ) {
			struct crtc *crtc = &drm->crtcs[i];

			// We can't disable a CRTC if it's already disabled, or else the
			// kernel will error out with "requesting event but off".
			if (crtc->current.active == 0)
				continue;

			int ret = add_crtc_property(drm->req, crtc, "MODE_ID", 0);
			if (ret < 0)
				return ret;
			if (crtc->has_gamma_lut)
			{
				int ret = add_crtc_property(drm->req, crtc, "GAMMA_LUT", 0);
				if (ret < 0)
					return ret;
			}
			if (crtc->has_degamma_lut)
			{
				int ret = add_crtc_property(drm->req, crtc, "DEGAMMA_LUT", 0);
				if (ret < 0)
					return ret;
			}
			if (crtc->has_ctm)
			{
				int ret = add_crtc_property(drm->req, crtc, "CTM", 0);
				if (ret < 0)
					return ret;
			}
			if (crtc->has_vrr_enabled)
			{
				int ret = add_crtc_property(drm->req, crtc, "VRR_ENABLED", 0);
				if (ret < 0)
					return ret;
			}
			if (crtc->has_valve1_regamma_tf)
			{
				int ret = add_crtc_property(drm->req, crtc, "VALVE1_CRTC_REGAMMA_TF", 0);
				if (ret < 0)
					return ret;
			}

			ret = add_crtc_property(drm->req, crtc, "ACTIVE", 0);
			if (ret < 0)
				return ret;
			crtc->pending.active = 0;
		}

		// Then enable the one we've picked
		int ret = 0;
		if (drm->connector != nullptr) {
			// Always set our CRTC_ID for the modeset, especially
			// as we zero-ed it above.
			drm->connector->pending.crtc_id = drm->crtc->id;
			ret = add_connector_property(drm->req, drm->connector, "CRTC_ID", drm->crtc->id);
			if (ret < 0)
				return ret;

			if (drm->connector->has_colorspace) {
				ret = add_connector_property(drm->req, drm->connector, "Colorspace", drm->connector->pending.colorspace);
				if (ret < 0)
					return ret;
			}

			if (drm->connector->has_hdr_output_metadata) {
				uint32_t value = drm->connector->pending.hdr_output_metadata ? drm->connector->pending.hdr_output_metadata->blob : 0;
				ret = add_connector_property(drm->req, drm->connector, "HDR_OUTPUT_METADATA", value);
				if (ret < 0)
					return ret;
			}

			if (drm->connector->has_content_type) {
				ret = add_connector_property(drm->req, drm->connector, "content type", drm->connector->pending.content_type);
				if (ret < 0)
					return ret;
			}

			ret = add_crtc_property(drm->req, drm->crtc, "MODE_ID", drm->pending.mode_id->blob);
			if (ret < 0)
				return ret;

			if (drm->crtc->has_vrr_enabled)
			{
				ret = add_crtc_property(drm->req, drm->crtc, "VRR_ENABLED", drm->pending.vrr_enabled);
				if (ret < 0)
					return ret;
			}

			if (drm->crtc->has_valve1_regamma_tf)
			{
				ret = add_crtc_property(drm->req, drm->crtc, "VALVE1_CRTC_REGAMMA_TF", drm->pending.output_tf);
				if (ret < 0)
					return ret;
			}

			ret = add_crtc_property(drm->req, drm->crtc, "ACTIVE", 1);
			if (ret < 0)
				return ret;
			drm->crtc->pending.active = 1;
		}
	}
	else
	{
		if (drm->connector != nullptr) {
			if (drm->connector->has_colorspace && drm->connector->pending.colorspace != drm->connector->current.colorspace) {
				int ret = add_connector_property(drm->req, drm->connector, "Colorspace", drm->connector->pending.colorspace);
				if (ret < 0)
					return ret;
			}

			if (drm->connector->has_hdr_output_metadata && drm->connector->pending.hdr_output_metadata != drm->connector->current.hdr_output_metadata) {
				uint32_t value = drm->connector->pending.hdr_output_metadata ? drm->connector->pending.hdr_output_metadata->blob : 0;
				int ret = add_connector_property(drm->req, drm->connector, "HDR_OUTPUT_METADATA", value);
				if (ret < 0)
					return ret;
			}

			if (drm->connector->has_content_type && drm->connector->pending.content_type != drm->connector->current.content_type) {
				int ret = add_connector_property(drm->req, drm->connector, "content type", drm->connector->pending.content_type);
				if (ret < 0)
					return ret;
			}
		}

		if (drm->crtc != nullptr) {
			if ( drm->crtc->has_vrr_enabled && drm->pending.vrr_enabled != drm->current.vrr_enabled )
			{
				int ret = add_crtc_property(drm->req, drm->crtc, "VRR_ENABLED", drm->pending.vrr_enabled );
				if (ret < 0)
					return ret;
			}

			if ( drm->crtc->has_valve1_regamma_tf && drm->pending.output_tf != drm->current.output_tf )
			{
				int ret = add_crtc_property(drm->req, drm->crtc, "VALVE1_CRTC_REGAMMA_TF", drm->pending.output_tf );
				if (ret < 0)
					return ret;
			}
		}
	}

	drm->flags = flags;

	int ret;
	if ( drm->crtc == nullptr ) {
		ret = 0;
	} else if ( g_bUseLayers == true ) {
		ret = drm_prepare_liftoff( drm, frameInfo, needs_modeset );
	} else {
		ret = drm_prepare_basic( drm, frameInfo );
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

void drm_rollback( struct drm_t *drm )
{
	drm->pending = drm->current;

	for ( size_t i = 0; i < drm->crtcs.size(); i++ )
	{
		drm->crtcs[i].pending = drm->crtcs[i].current;
	}

	for (auto &kv : drm->connectors) {
		struct connector *conn = &kv.second;
		conn->pending = conn->current;
	}
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

static bool drm_set_crtc( struct drm_t *drm, struct crtc *crtc )
{
	drm->crtc = crtc;
	drm->needs_modeset = true;

	for (size_t i = 0; i < drm->crtcs.size(); i++) {
		if (drm->crtcs[i].id == drm->crtc->id) {
			drm->crtc_index = i;
			break;
		}
	}

	drm->primary = find_primary_plane( drm );
	if ( drm->primary == nullptr ) {
		drm_log.errorf("could not find a suitable primary plane");
		return false;
	}

	struct liftoff_output *lo_output = liftoff_output_create( drm->lo_device, crtc->id );
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

bool drm_set_connector( struct drm_t *drm, struct connector *conn )
{
	drm_log.infof("selecting connector %s", conn->name);

	struct crtc *crtc = find_crtc_for_connector(drm, conn);
	if (crtc == nullptr) {
		drm_log.errorf("no CRTC found!");
		return false;
	}

	if (!drm_set_crtc(drm, crtc)) {
		return false;
	}

	drm->connector = conn;
	drm->needs_modeset = true;

	return true;
}

static void drm_unset_connector( struct drm_t *drm )
{
	drm->crtc = nullptr;
	drm->primary = nullptr;

	for ( int i = 0; i < k_nMaxLayers; i++ )
	{
		liftoff_layer_destroy( drm->lo_layers[ i ] );
		drm->lo_layers[ i ] = nullptr;
	}

	liftoff_output_destroy(drm->lo_output);
	drm->lo_output = nullptr;

	drm->connector = nullptr;
	drm->needs_modeset = true;
}

void drm_set_vrr_enabled(struct drm_t *drm, bool enabled)
{
	drm->wants_vrr_enabled = enabled;
}

bool drm_get_vrr_in_use(struct drm_t *drm)
{
	return drm->current.vrr_enabled;
}

drm_screen_type drm_get_connector_type(drmModeConnector *connector)
{
	if (connector->connector_type == DRM_MODE_CONNECTOR_eDP ||
		connector->connector_type == DRM_MODE_CONNECTOR_LVDS ||
		connector->connector_type == DRM_MODE_CONNECTOR_DSI)
		return DRM_SCREEN_TYPE_INTERNAL;

	return DRM_SCREEN_TYPE_EXTERNAL;
}

drm_screen_type drm_get_screen_type(struct drm_t *drm)
{
	if (!drm->connector || !drm->connector->connector)
		return DRM_SCREEN_TYPE_INTERNAL;

	return drm_get_connector_type(drm->connector->connector);
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

bool drm_update_vrr_state(struct drm_t *drm)
{
	drm->pending.vrr_enabled = false;

	if ( drm->connector && drm->crtc && drm->crtc->has_vrr_enabled )
	{
		if ( drm->wants_vrr_enabled && drm->connector->vrr_capable )
			drm->pending.vrr_enabled = true;
	}

	if (drm->pending.vrr_enabled != drm->current.vrr_enabled)
		drm->needs_modeset = true;

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

	g_drmEffectiveOrientation[DRM_SCREEN_TYPE_INTERNAL] = DRM_MODE_ROTATE_0;
	g_drmEffectiveOrientation[DRM_SCREEN_TYPE_EXTERNAL] = DRM_MODE_ROTATE_0;
	g_bRotated = false;
}

bool drm_set_mode( struct drm_t *drm, const drmModeModeInfo *mode )
{
	if (!drm->connector || !drm->connector->connector)
		return false;

	uint32_t mode_id = 0;
	if (drmModeCreatePropertyBlob(drm->fd, mode, sizeof(*mode), &mode_id) != 0)
		return false;

	drm_screen_type screenType = drm_get_screen_type(drm);

	drm_log.infof("selecting mode %dx%d@%uHz", mode->hdisplay, mode->vdisplay, mode->vrefresh);

	drm->pending.mode_id = std::make_shared<drm_blob>(mode_id);
	drm->needs_modeset = true;

	g_nOutputRefresh = mode->vrefresh;

	update_drm_effective_orientations(drm, drm->connector, mode);

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
	if (!drm->connector || !drm->connector->connector)
		return false;

	drmModeConnector *connector = drm->connector->connector;
	const drmModeModeInfo *existing_mode = find_mode(connector, width, height, refresh);
	drmModeModeInfo mode = {0};
	if ( existing_mode )
	{
		mode = *existing_mode;
	}
	else
	{
		/* TODO: check refresh is within the EDID limits */
		switch ( g_drmModeGeneration )
		{
		case DRM_MODE_GENERATE_CVT:
			generate_cvt_mode( &mode, width, height, refresh, true, false );
			break;
		case DRM_MODE_GENERATE_FIXED:
			{
				const drmModeModeInfo *preferred_mode = find_mode(connector, 0, 0, 0);
				generate_fixed_mode( &mode, preferred_mode, refresh, drm->connector->is_steam_deck_display, drm->connector->is_galileo_display );
				break;
			}
		}
	}

	mode.type = DRM_MODE_TYPE_USERDEF;

	return drm_set_mode(drm, &mode);
}

bool drm_set_resolution( struct drm_t *drm, int width, int height )
{
	if (!drm->connector || !drm->connector->connector)
		return false;

	drmModeConnector *connector = drm->connector->connector;
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

	if ( drm->connector && drm->connector->target_refresh )
		return drm->connector->target_refresh;

	if ( drm->connector && drm->connector->connector )
	{
		int width = g_nOutputWidth;
		int height = g_nOutputHeight;
		if ( g_bRotated ) {
			int tmp = width;
			width = height;
			height = tmp;
		}

		drmModeConnector *connector = drm->connector->connector;
		const drmModeModeInfo *mode = find_mode( connector, width, height, 0);
		if ( mode )
			return mode->vrefresh;
	}

	return 60;
}

bool drm_get_vrr_capable(struct drm_t *drm)
{
	if ( drm->connector )
		return drm->connector->vrr_capable;

	return false;
}

bool drm_supports_st2084(struct drm_t *drm)
{
	if ( drm->connector )
		return drm->connector->metadata.supportsST2084;

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
	if ( !drm->connector )
		return nullptr;

	return drm->connector->name;
}

const char *drm_get_device_name(struct drm_t *drm)
{
	return drm->device_name;
}

std::pair<uint32_t, uint32_t> drm_get_connector_identifier(struct drm_t *drm)
{
	if ( !drm->connector )
		return { 0u, 0u };

	return std::make_pair(drm->connector->connector->connector_type, drm->connector->connector->connector_type_id);
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
	if (g_bForceDisableColorMgmt)
		return false;

	if (!drm->primary)
		return false;

	return drm->primary->has_color_mgmt;
}

void drm_get_native_colorimetry( struct drm_t *drm,
	displaycolorimetry_t *displayColorimetry, EOTF *displayEOTF,
	displaycolorimetry_t *outputEncodingColorimetry, EOTF *outputEncodingEOTF )
{
	if ( !drm || !drm->connector )
	{
		*displayColorimetry = displaycolorimetry_709;
		*displayEOTF = EOTF_Gamma22;
		*outputEncodingColorimetry = displaycolorimetry_709;
		*outputEncodingEOTF = EOTF_Gamma22;
		return;
	}

	*displayColorimetry = drm->connector->metadata.colorimetry;
	*displayEOTF = drm->connector->metadata.eotf;

	// For HDR output, expected content colorspace != native colorspace.
	if (drm->connector->metadata.supportsST2084 && g_bOutputHDREnabled)
	{
		*outputEncodingColorimetry = displaycolorimetry_2020;
		*outputEncodingEOTF = EOTF_PQ;
	}
	else
	{
		*outputEncodingColorimetry = drm->connector->metadata.colorimetry;
		*outputEncodingEOTF = drm->connector->metadata.eotf;
	}

	if (!g_bOutputHDREnabled)
	{
		*displayEOTF = EOTF_Gamma22;
		*outputEncodingEOTF = EOTF_Gamma22;
	}
}


std::span<uint32_t> drm_get_valid_refresh_rates( struct drm_t *drm )
{
	if (drm && drm->connector)
		return drm->connector->valid_display_rates;

	return std::span<uint32_t>{};
}
