// DRM output stuff

#include "Script/Script.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <poll.h>

#include <atomic>
#include <cassert>
#include <cinttypes>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "backend.h"
#include "color_helpers.h"
#include "Utils/Defer.h"
#include "drm_include.h"
#include "edid.h"
#include "gamescope_shared.h"
#include "gpuvis_trace_utils.h"
#include "log.hpp"
#include "main.hpp"
#include "modegen.hpp"
#include "rendervulkan.hpp"
#include "steamcompmgr.hpp"
#include "vblankmanager.hpp"
#include "wlserver.hpp"
#include "refresh_rate.h"
#include <sys/utsname.h>

#include "wlr_begin.hpp"
#include <libliftoff.h>
#include <wlr/types/wlr_buffer.h>
#include "libdisplay-info/info.h"
#include "libdisplay-info/edid.h"
#include "libdisplay-info/cta.h"
#include "wlr_end.hpp"

#include "gamescope-control-protocol.h"

static constexpr bool k_bUseCursorPlane = false;

extern int g_nPreferredOutputWidth;
extern int g_nPreferredOutputHeight;

gamescope::ConVar<bool> cv_drm_single_plane_optimizations( "drm_single_plane_optimizations", true, "Whether or not to enable optimizations for single plane usage." );

gamescope::ConVar<bool> cv_drm_debug_disable_shaper_and_3dlut( "drm_debug_disable_shaper_and_3dlut", false, "Shaper + 3DLUT chicken bit. (Force disable/DEFAULT, no logic change)" );
gamescope::ConVar<bool> cv_drm_debug_disable_degamma_tf( "drm_debug_disable_degamma_tf", false, "Degamma chicken bit. (Forces DEGAMMA_TF to DEFAULT, does not affect other logic)" );
gamescope::ConVar<bool> cv_drm_debug_disable_regamma_tf( "drm_debug_disable_regamma_tf", false, "Regamma chicken bit. (Forces REGAMMA_TF to DEFAULT, does not affect other logic)" );
gamescope::ConVar<bool> cv_drm_debug_disable_output_tf( "drm_debug_disable_output_tf", false, "Force default (identity) output TF, affects other logic. Not a property directly." );
gamescope::ConVar<bool> cv_drm_debug_disable_blend_tf( "drm_debug_disable_blend_tf", false, "Blending chicken bit. (Forces BLEND_TF to DEFAULT, does not affect other logic)" );
gamescope::ConVar<bool> cv_drm_debug_disable_ctm( "drm_debug_disable_ctm", false, "CTM chicken bit. (Forces CTM off, does not affect other logic)" );
gamescope::ConVar<bool> cv_drm_debug_disable_color_encoding( "drm_debug_disable_color_encoding", false, "YUV Color Encoding chicken bit. (Forces COLOR_ENCODING to DEFAULT, does not affect other logic)" );
gamescope::ConVar<bool> cv_drm_debug_disable_color_range( "drm_debug_disable_color_range", false, "YUV Color Range chicken bit. (Forces COLOR_RANGE to DEFAULT, does not affect other logic)" );
gamescope::ConVar<bool> cv_drm_debug_disable_explicit_sync( "drm_debug_disable_explicit_sync", false, "Force disable explicit sync on the DRM backend." );
gamescope::ConVar<bool> cv_drm_debug_disable_in_fence_fd( "drm_debug_disable_in_fence_fd", false, "Force disable IN_FENCE_FD being set to avoid over-synchronization on the DRM backend." );

namespace gamescope
{
	std::tuple<int32_t, int32_t, int32_t> GetKernelVersion()
	{
		utsname name;
		if ( uname( &name ) != 0 )
			return std::make_tuple( 0, 0, 0 );

		std::vector<std::string_view> szVersionParts = Split( name.release, "." );

		uint32_t uVersion[3] = { 0 };
		for ( size_t i = 0; i < szVersionParts.size() && i < 3; i++ )
		{
			auto oPart = Parse<int32_t>( szVersionParts[i] );
			if ( !oPart )
				break;

			uVersion[i] = *oPart;
		}

		return std::make_tuple( uVersion[0], uVersion[1], uVersion[2] );
	}

	// Get a DRM mode in mHz
	// Taken from wlroots, but we can't access it as we don't
	// use the drm backend.
	static int32_t GetModeRefresh(const drmModeModeInfo *mode)
	{
		int32_t nRefresh = (mode->clock * 1'000'000ll / mode->htotal + mode->vtotal / 2) / mode->vtotal;

		if (mode->flags & DRM_MODE_FLAG_INTERLACE)
			nRefresh *= 2;

		if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
			nRefresh /= 2;

		if (mode->vscan > 1)
			nRefresh /= mode->vscan;

		return nRefresh;
	}

	template <typename T>
	using CAutoDeletePtr = std::unique_ptr<T, void(*)(T*)>;

	////////////////////////////////////////
	// DRM Object Wrappers + State Trackers
	////////////////////////////////////////
	struct DRMObjectRawProperty
	{
		uint32_t uPropertyId = 0ul;
		uint64_t ulValue = 0ul;
	};
	using DRMObjectRawProperties = std::unordered_map<std::string, DRMObjectRawProperty>;

	class CDRMAtomicObject
	{
	public:
		CDRMAtomicObject( uint32_t ulObjectId );
		uint32_t GetObjectId() const { return m_ulObjectId; }

		// No copy or move constructors.
		CDRMAtomicObject( const CDRMAtomicObject& ) = delete;
		CDRMAtomicObject& operator=( const CDRMAtomicObject& ) = delete;

		CDRMAtomicObject( CDRMAtomicObject&& ) = delete;
		CDRMAtomicObject& operator=( CDRMAtomicObject&& ) = delete;
	protected:
		uint32_t m_ulObjectId = 0ul;
	};

	template < uint32_t DRMObjectType >
	class CDRMAtomicTypedObject : public CDRMAtomicObject
	{
	public:
		CDRMAtomicTypedObject( uint32_t ulObjectId );
	protected:
		std::optional<DRMObjectRawProperties> GetRawProperties();
	};

	class CDRMAtomicProperty
	{
	public:
		CDRMAtomicProperty( CDRMAtomicObject *pObject, DRMObjectRawProperty rawProperty );

		static std::optional<CDRMAtomicProperty> Instantiate( const char *pszName, CDRMAtomicObject *pObject, const DRMObjectRawProperties& rawProperties );

		uint64_t GetPendingValue() const { return m_ulPendingValue; }
		uint64_t GetCurrentValue() const { return m_ulCurrentValue; }
		uint64_t GetInitialValue() const { return m_ulInitialValue; }
		int SetPendingValue( drmModeAtomicReq *pRequest, uint64_t ulValue, bool bForce );

		void OnCommit();
		void Rollback();
	private:
		CDRMAtomicObject *m_pObject = nullptr;
		uint32_t m_uPropertyId = 0u;

		uint64_t m_ulPendingValue = 0ul;
		uint64_t m_ulCurrentValue = 0ul;
		uint64_t m_ulInitialValue = 0ul;
	};

	class CDRMPlane final : public CDRMAtomicTypedObject<DRM_MODE_OBJECT_PLANE>
	{
	public:
		// Takes ownership of pPlane.
		CDRMPlane( drmModePlane *pPlane );

		void RefreshState();

		drmModePlane *GetModePlane() const { return m_pPlane.get(); }

		struct PlaneProperties
		{
			std::optional<CDRMAtomicProperty> *begin() { return &FB_ID; }
			std::optional<CDRMAtomicProperty> *end() { return &DUMMY_END; }

			std::optional<CDRMAtomicProperty> type; // Immutable
			std::optional<CDRMAtomicProperty> IN_FORMATS; // Immutable

			std::optional<CDRMAtomicProperty> FB_ID;
			std::optional<CDRMAtomicProperty> IN_FENCE_FD;
			std::optional<CDRMAtomicProperty> CRTC_ID;
			std::optional<CDRMAtomicProperty> SRC_X;
			std::optional<CDRMAtomicProperty> SRC_Y;
			std::optional<CDRMAtomicProperty> SRC_W;
			std::optional<CDRMAtomicProperty> SRC_H;
			std::optional<CDRMAtomicProperty> CRTC_X;
			std::optional<CDRMAtomicProperty> CRTC_Y;
			std::optional<CDRMAtomicProperty> CRTC_W;
			std::optional<CDRMAtomicProperty> CRTC_H;
			std::optional<CDRMAtomicProperty> zpos;
			std::optional<CDRMAtomicProperty> alpha;
			std::optional<CDRMAtomicProperty> rotation;
			std::optional<CDRMAtomicProperty> COLOR_ENCODING;
			std::optional<CDRMAtomicProperty> COLOR_RANGE;
			std::optional<CDRMAtomicProperty> AMD_PLANE_DEGAMMA_TF;
			std::optional<CDRMAtomicProperty> AMD_PLANE_DEGAMMA_LUT;
			std::optional<CDRMAtomicProperty> AMD_PLANE_CTM;
			std::optional<CDRMAtomicProperty> AMD_PLANE_HDR_MULT;
			std::optional<CDRMAtomicProperty> AMD_PLANE_SHAPER_LUT;
			std::optional<CDRMAtomicProperty> AMD_PLANE_SHAPER_TF;
			std::optional<CDRMAtomicProperty> AMD_PLANE_LUT3D;
			std::optional<CDRMAtomicProperty> AMD_PLANE_BLEND_TF;
			std::optional<CDRMAtomicProperty> AMD_PLANE_BLEND_LUT;
			std::optional<CDRMAtomicProperty> DUMMY_END;
		};
		      PlaneProperties &GetProperties()       { return m_Props; }
		const PlaneProperties &GetProperties() const { return m_Props; }
	private:
		CAutoDeletePtr<drmModePlane> m_pPlane;
		PlaneProperties m_Props;
	};

	class CDRMCRTC final : public CDRMAtomicTypedObject<DRM_MODE_OBJECT_CRTC>
	{
	public:
		// Takes ownership of pCRTC.
		CDRMCRTC( drmModeCrtc *pCRTC, uint32_t uCRTCMask );

		void RefreshState();
		uint32_t GetCRTCMask() const { return m_uCRTCMask; }

		struct CRTCProperties
		{
			std::optional<CDRMAtomicProperty> *begin() { return &ACTIVE; }
			std::optional<CDRMAtomicProperty> *end() { return &DUMMY_END; }

			std::optional<CDRMAtomicProperty> ACTIVE;
			std::optional<CDRMAtomicProperty> MODE_ID;
			std::optional<CDRMAtomicProperty> GAMMA_LUT;
			std::optional<CDRMAtomicProperty> DEGAMMA_LUT;
			std::optional<CDRMAtomicProperty> CTM;
			std::optional<CDRMAtomicProperty> VRR_ENABLED;
			std::optional<CDRMAtomicProperty> OUT_FENCE_PTR;
			std::optional<CDRMAtomicProperty> AMD_CRTC_REGAMMA_TF;
			std::optional<CDRMAtomicProperty> DUMMY_END;
		};
		      CRTCProperties &GetProperties()       { return m_Props; }
		const CRTCProperties &GetProperties() const { return m_Props; }
	private:
		CAutoDeletePtr<drmModeCrtc> m_pCRTC;
		uint32_t m_uCRTCMask = 0u;
		CRTCProperties m_Props;
	};

	class CDRMConnector final : public IBackendConnector, public CDRMAtomicTypedObject<DRM_MODE_OBJECT_CONNECTOR>
	{
	public:
		CDRMConnector( drmModeConnector *pConnector );

		void RefreshState();

		struct ConnectorProperties
		{
			std::optional<CDRMAtomicProperty> *begin() { return &CRTC_ID; }
			std::optional<CDRMAtomicProperty> *end() { return &DUMMY_END; }

			std::optional<CDRMAtomicProperty> CRTC_ID;
			std::optional<CDRMAtomicProperty> Colorspace;
			std::optional<CDRMAtomicProperty> content_type; // "content type" with space!
			std::optional<CDRMAtomicProperty> panel_orientation; // "panel orientation" with space!
			std::optional<CDRMAtomicProperty> HDR_OUTPUT_METADATA;
			std::optional<CDRMAtomicProperty> vrr_capable;
			std::optional<CDRMAtomicProperty> EDID;
			std::optional<CDRMAtomicProperty> DUMMY_END;
		};
		      ConnectorProperties &GetProperties()       { return m_Props; }
		const ConnectorProperties &GetProperties() const { return m_Props; }

		drmModeConnector *GetModeConnector() { return m_pConnector.get(); }
		const char *GetName() const override { return m_Mutable.szName; }
		const char *GetMake() const override { return m_Mutable.pszMake; }
		const char *GetModel() const override { return m_Mutable.szModel; }
		uint32_t GetPossibleCRTCMask() const { return m_Mutable.uPossibleCRTCMask; }
		std::span<const uint32_t> GetValidDynamicRefreshRates() const override { return m_Mutable.ValidDynamicRefreshRates; }
		const displaycolorimetry_t& GetDisplayColorimetry() const { return m_Mutable.DisplayColorimetry; }

		std::span<const uint8_t> GetRawEDID() const override { return std::span<const uint8_t>{ m_Mutable.EdidData.begin(), m_Mutable.EdidData.end() }; }

		bool SupportsHDR10() const
		{
			return !!GetProperties().Colorspace && !!GetProperties().HDR_OUTPUT_METADATA && GetHDRInfo().IsHDR10();
		}

		bool SupportsHDRG22() const
		{
			return GetHDRInfo().IsHDRG22();
		}

		//////////////////////////////////////
		// IBackendConnector implementation
		//////////////////////////////////////

		GamescopeScreenType GetScreenType() const override
		{
			if ( m_pConnector->connector_type == DRM_MODE_CONNECTOR_eDP ||
				 m_pConnector->connector_type == DRM_MODE_CONNECTOR_LVDS ||
				 m_pConnector->connector_type == DRM_MODE_CONNECTOR_DSI )
				return GAMESCOPE_SCREEN_TYPE_INTERNAL;

			return GAMESCOPE_SCREEN_TYPE_EXTERNAL;
		}

		GamescopePanelOrientation GetCurrentOrientation() const override
		{
			return m_ChosenOrientation;
		}

		bool SupportsHDR() const override
		{
			return SupportsHDR10() || SupportsHDRG22();
		}

		bool IsHDRActive() const override
		{
			if ( SupportsHDR10() )
			{
				return GetProperties().Colorspace->GetCurrentValue() == DRM_MODE_COLORIMETRY_BT2020_RGB;
			}
			else if ( SupportsHDRG22() )
			{
				return true;
			}

			return false;
		}

		const BackendConnectorHDRInfo &GetHDRInfo() const override { return m_Mutable.HDR; }

		virtual std::span<const BackendMode> GetModes() const override { return m_Mutable.BackendModes; }

		bool SupportsVRR() const override
		{
			return this->GetProperties().vrr_capable && !!this->GetProperties().vrr_capable->GetCurrentValue();
		}

        void GetNativeColorimetry(
			bool bHDR,
            displaycolorimetry_t *displayColorimetry, EOTF *displayEOTF,
            displaycolorimetry_t *outputEncodingColorimetry, EOTF *outputEncodingEOTF ) const override
		{
			*displayColorimetry = GetDisplayColorimetry();
			*displayEOTF = EOTF_Gamma22;

			if ( bHDR && GetHDRInfo().IsHDR10() )
			{
				// For HDR10 output, expected content colorspace != native colorspace.
				*outputEncodingColorimetry = displaycolorimetry_2020;
				*outputEncodingEOTF = GetHDRInfo().eOutputEncodingEOTF;
			}
			else
			{
				*outputEncodingColorimetry = GetDisplayColorimetry();
				*outputEncodingEOTF = EOTF_Gamma22;
			}
		}

		void UpdateEffectiveOrientation( const drmModeModeInfo *pMode );

		using DRMModeGenerator = std::function<drmModeModeInfo(const drmModeModeInfo *, int)>;
		const DRMModeGenerator &GetModeGenerator() const
		{
			return m_Mutable.fnDynamicModeGenerator;
		}

	private:
		void ParseEDID();

		CAutoDeletePtr<drmModeConnector> m_pConnector;

		struct MutableConnectorState
		{
			int nDefaultRefresh = 0;

			uint32_t uPossibleCRTCMask = 0u;
			char szName[32]{};
			char szMakePNP[4]{};
			char szModel[16]{};
			const char *pszMake = ""; // Not owned, no free. This is a pointer to pnp db or szMakePNP.
			std::vector<uint32_t> ValidDynamicRefreshRates{};
			DRMModeGenerator fnDynamicModeGenerator;
			std::vector<uint8_t> EdidData; // Raw, unmodified.
			std::vector<BackendMode> BackendModes;

			displaycolorimetry_t DisplayColorimetry = displaycolorimetry_709;
			BackendConnectorHDRInfo HDR;
		} m_Mutable;

		GamescopePanelOrientation m_ChosenOrientation = GAMESCOPE_PANEL_ORIENTATION_AUTO;

		ConnectorProperties m_Props;
	};

	class CDRMFb final : public CBaseBackendFb
	{
	public:
		CDRMFb( uint32_t uFbId );
		~CDRMFb();

		uint32_t GetFbId() const { return m_uFbId; }
	
	private:
		uint32_t m_uFbId = 0;
	};
}

struct saved_mode {
	int width;
	int height;
	int refresh;
};

struct drm_t {
	bool bUseLiftoff;

	int fd = -1;

	int preferred_width, preferred_height, preferred_refresh;

	uint64_t cursor_width, cursor_height;
	bool allow_modifiers;
	struct wlr_drm_format_set formats;

	std::vector< std::unique_ptr< gamescope::CDRMPlane > > planes;
	std::vector< std::unique_ptr< gamescope::CDRMCRTC > > crtcs;
	std::unordered_map< uint32_t, gamescope::CDRMConnector > connectors;

	gamescope::CDRMPlane *pPrimaryPlane;
	gamescope::CDRMCRTC *pCRTC;
	gamescope::CDRMConnector *pConnector;

	struct wlr_drm_format_set primary_formats;

	drmModeAtomicReq *req;
	uint32_t flags;

	struct liftoff_device *lo_device;
	struct liftoff_output *lo_output;
	struct liftoff_layer *lo_layers[ k_nMaxLayers ];

	std::shared_ptr<gamescope::BackendBlob> sdr_static_metadata;

	struct drm_state_t {
		std::shared_ptr<gamescope::BackendBlob> mode_id;
		uint32_t color_mgmt_serial;
		std::shared_ptr<gamescope::BackendBlob> lut3d_id[ EOTF_Count ];
		std::shared_ptr<gamescope::BackendBlob> shaperlut_id[ EOTF_Count ];
		amdgpu_transfer_function output_tf = AMDGPU_TRANSFER_FUNCTION_DEFAULT;
	} current, pending;

	// FBs in the atomic request, but not yet submitted to KMS
	// Accessed only on req thread
	std::vector<gamescope::Rc<gamescope::IBackendFb>> m_FbIdsInRequest;

	// FBs currently queued to go on screen.
	// May be accessed by page flip handler thread and req thread, thus mutex.
	std::mutex m_QueuedFbIdsMutex;
	std::vector<gamescope::Rc<gamescope::IBackendFb>> m_QueuedFbIds;
	// FBs currently on screen.
	// Accessed only on page flip handler thread.
	std::mutex m_mutVisibleFbIds;
	std::vector<gamescope::Rc<gamescope::IBackendFb>> m_VisibleFbIds;

	std::atomic < uint32_t > uPendingFlipCount = { 0 };

	std::atomic < bool > paused = { false };
	std::atomic < int > out_of_date = { false };
	std::atomic < bool > needs_modeset = { false };

	std::unordered_map< std::string, int > connector_priorities;

	char *device_name = nullptr;
};

void drm_drop_fbid( struct drm_t *drm, uint32_t fbid );
bool drm_set_mode( struct drm_t *drm, const drmModeModeInfo *mode );


using namespace std::literals;

struct drm_t g_DRM = {};

uint32_t g_nDRMFormat = DRM_FORMAT_INVALID;
uint32_t g_nDRMFormatOverlay = DRM_FORMAT_INVALID; // for partial composition, we may have more limited formats than base planes + alpha.
bool g_bRotated = false;
extern bool g_bDebugLayers;

struct DRMPresentCtx
{
	uint64_t ulPendingFlipCount = 0;
};

extern gamescope::ConVar<bool> cv_composite_force;
extern bool g_bColorSliderInUse;
extern bool fadingOut;
extern std::string g_reshade_effect;

#ifndef DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP
#define DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP 0x15
#endif

bool drm_update_color_mgmt(struct drm_t *drm);
bool drm_supports_color_mgmt(struct drm_t *drm);
bool drm_set_connector( struct drm_t *drm, gamescope::CDRMConnector *conn );

struct drm_color_ctm2 {
	/*
	 * Conversion matrix in S31.32 sign-magnitude
	 * (not two's complement!) format.
	 */
	__u64 matrix[12];
};

bool g_bSupportsAsyncFlips = false;
bool g_bSupportsSyncObjs = false;

extern gamescope::GamescopeModeGeneration g_eGamescopeModeGeneration;
extern GamescopePanelOrientation g_DesiredInternalOrientation;

extern bool g_bForceDisableColorMgmt;

static LogScope drm_log( "drm" );
static LogScope liftoff_log_scope( "liftoff" );

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
	45, 47, 48, 49, 
	50, 51, 53, 55, 56, 59, 
	60, 62, 64, 65, 66, 68, 
	72, 73, 76, 77, 78, 
	80, 81, 82, 84, 85, 86, 87, 88, 
	90, 
};

static void update_connector_display_info_wl(struct drm_t *drm)
{
	wlserver_lock();
	for ( const auto &control : wlserver.gamescope_controls )
	{
		wlserver_send_gamescope_control( control );
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

extern void mangoapp_output_update( uint64_t vblanktime );
static void page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, unsigned int crtc_id, void *data)
{
	DRMPresentCtx *pCtx = reinterpret_cast<DRMPresentCtx *>( data );

	// Make this const when we move into CDRMBackend.
	GetBackend()->PresentationFeedback().m_uCompletedPresents = pCtx->ulPendingFlipCount;

	if ( !g_DRM.pCRTC )
		return;

	if ( g_DRM.pCRTC->GetObjectId() != crtc_id )
		return;

	static uint64_t ulLastVBlankTime = 0;

	// This is the last vblank time
	uint64_t vblanktime = sec * 1'000'000'000lu + usec * 1'000lu;
	GetVBlankTimer().MarkVBlank( vblanktime, true );

	// TODO: get the fbids_queued instance from data if we ever have more than one in flight

	drm_log.debugf("page_flip_handler %" PRIu64 " delta: %" PRIu64, pCtx->ulPendingFlipCount, vblanktime - ulLastVBlankTime );
	gpuvis_trace_printf("page_flip_handler %" PRIu64, pCtx->ulPendingFlipCount);

	ulLastVBlankTime = vblanktime;

	{
		std::scoped_lock lock{ g_DRM.m_QueuedFbIdsMutex, g_DRM.m_mutVisibleFbIds };
		// Swap and clear from queue -> visible to avoid allocations.
		g_DRM.m_VisibleFbIds.swap( g_DRM.m_QueuedFbIds );
		g_DRM.m_QueuedFbIds.clear();
	}

	g_DRM.uPendingFlipCount--;
	g_DRM.uPendingFlipCount.notify_all();

	mangoapp_output_update( vblanktime );

	// Nudge so that steamcompmgr releases commits.
	nudge_steamcompmgr();
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

		if ( g_bForceInternal && pConnector->GetScreenType() == gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL )
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
		mode = find_mode(best->GetModeConnector(), drm->preferred_width, drm->preferred_height, gamescope::ConvertmHzToHz( drm->preferred_refresh ));
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

	if (!drm_set_mode(drm, mode)) {
		return false;
	}

	// Don't allow rollback of mode_id after connector change
	drm->current.mode_id = drm->pending.mode_id;

	const struct wlserver_output_info wlserver_output_info = {
		.description = description,
		.phys_width = (int) best->GetModeConnector()->mmWidth,
		.phys_height = (int) best->GetModeConnector()->mmHeight,
	};
	wlserver_lock();
	wlserver_set_output_info(&wlserver_output_info);
	wlserver_unlock();

	if (!initial)
		WritePatchedEdid( best->GetRawEDID(), best->GetHDRInfo(), g_bRotated );

	update_connector_display_info_wl( drm );

	return true;
}

void load_pnps(void)
{
#ifdef HWDATA_PNP_IDS
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
#endif
}

extern bool env_to_bool(const char *env);

uint32_t g_uAlwaysSignalledSyncobj = 0;
int g_nAlwaysSignalledSyncFile = -1;

static void
gamescope_liftoff_log_handler(enum liftoff_log_priority liftoff_priority, const char *fmt, va_list args)
{
	enum LogPriority priority = LOG_DEBUG;
	
	switch ( liftoff_priority )
	{
		case LIFTOFF_ERROR:
			priority = LOG_ERROR;
			break;
		case LIFTOFF_DEBUG:
			priority = LOG_DEBUG;
			break;
		case LIFTOFF_SILENT:
			priority = LOG_SILENT;
			break;
	}

	liftoff_log_scope.vlogf(priority, fmt, args);
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

	if ( !drmIsKMS( drm->fd ) )
	{
		drm_log.errorf( "'%s' is not a KMS device", drm->device_name );
		wlsession_close_kms();
		return -1;
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
	g_bSupportsSyncObjs = drmGetCap(drm->fd, DRM_CAP_SYNCOBJ, &cap) == 0 && cap != 0;
	if ( g_bSupportsSyncObjs ) {
		int err = drmSyncobjCreate(drm->fd, DRM_SYNCOBJ_CREATE_SIGNALED, &g_uAlwaysSignalledSyncobj);
		if (err < 0) {
			drm_log.errorf("Failed to create dummy signalled syncobj");
			return false;
		}
		err = drmSyncobjExportSyncFile(drm->fd, g_uAlwaysSignalledSyncobj, &g_nAlwaysSignalledSyncFile);
		if (err < 0) {
			drm_log.errorf("Failed to create dummy signalled sync file");
			return false;
		}
	} else {
		drm_log.errorf("Syncobjs are not supported by the KMS driver");
	}

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

	std::thread flip_handler_thread( flip_handler_thread_run );
	flip_handler_thread.detach();

	// Set log priority to the max, liftoff_log_scope will filter for us.
	liftoff_log_set_priority(LIFTOFF_DEBUG);
	liftoff_log_set_handler(gamescope_liftoff_log_handler);

	hdr_output_metadata sdr_metadata;
	memset(&sdr_metadata, 0, sizeof(sdr_metadata));
	drm->sdr_static_metadata = GetBackend()->CreateBackendBlob( sdr_metadata );

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
				pConnector->GetProperties().HDR_OUTPUT_METADATA->SetPendingValue( req, drm->sdr_static_metadata->GetBlobValue(), true );
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

		if ( pCRTC->GetProperties().AMD_CRTC_REGAMMA_TF )
			pCRTC->GetProperties().AMD_CRTC_REGAMMA_TF->SetPendingValue( req, 0, true );
	}

	for ( std::unique_ptr< gamescope::CDRMPlane > &pPlane : drm->planes )
	{
		pPlane->GetProperties().FB_ID->SetPendingValue( req, 0, true );
		pPlane->GetProperties().IN_FENCE_FD->SetPendingValue( req, -1, true );
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

		if ( pPlane->GetProperties().AMD_PLANE_DEGAMMA_TF )
			pPlane->GetProperties().AMD_PLANE_DEGAMMA_TF->SetPendingValue( req, AMDGPU_TRANSFER_FUNCTION_DEFAULT, true );

		if ( pPlane->GetProperties().AMD_PLANE_DEGAMMA_LUT )
			pPlane->GetProperties().AMD_PLANE_DEGAMMA_LUT->SetPendingValue( req, 0, true );

		if ( pPlane->GetProperties().AMD_PLANE_CTM )
			pPlane->GetProperties().AMD_PLANE_CTM->SetPendingValue( req, 0, true );

		if ( pPlane->GetProperties().AMD_PLANE_HDR_MULT )
			pPlane->GetProperties().AMD_PLANE_HDR_MULT->SetPendingValue( req, 0x100000000ULL, true );

		if ( pPlane->GetProperties().AMD_PLANE_SHAPER_TF )
			pPlane->GetProperties().AMD_PLANE_SHAPER_TF->SetPendingValue( req, AMDGPU_TRANSFER_FUNCTION_DEFAULT, true );

		if ( pPlane->GetProperties().AMD_PLANE_SHAPER_LUT )
			pPlane->GetProperties().AMD_PLANE_SHAPER_LUT->SetPendingValue( req, 0, true );

		if ( pPlane->GetProperties().AMD_PLANE_LUT3D )
			pPlane->GetProperties().AMD_PLANE_LUT3D->SetPendingValue( req, 0, true );

		if ( pPlane->GetProperties().AMD_PLANE_BLEND_TF )
			pPlane->GetProperties().AMD_PLANE_BLEND_TF->SetPendingValue( req, AMDGPU_TRANSFER_FUNCTION_DEFAULT, true );

		if ( pPlane->GetProperties().AMD_PLANE_BLEND_LUT )
			pPlane->GetProperties().AMD_PLANE_BLEND_LUT->SetPendingValue( req, 0, true );
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

	wlr_drm_format_set_finish( &drm->formats );
	wlr_drm_format_set_finish( &drm->primary_formats );
	drm->m_FbIdsInRequest.clear();
	{
		std::unique_lock lock( drm->m_QueuedFbIdsMutex );
		drm->m_QueuedFbIds.clear();
	}
	{
		std::unique_lock lock( drm->m_mutVisibleFbIds );
		drm->m_VisibleFbIds.clear();
	}
	drm->sdr_static_metadata = nullptr;
	drm->current = drm_t::drm_state_t{};
	drm->pending = drm_t::drm_state_t{};
	drm->planes.clear();
	drm->crtcs.clear();
	drm->connectors.clear();



	// We can't close the DRM FD here, it might still be in use by the
	// page-flip handler thread.
}

gamescope::OwningRc<gamescope::IBackendFb> drm_fbid_from_dmabuf( struct drm_t *drm, struct wlr_buffer *buf, struct wlr_dmabuf_attributes *dma_buf )
{
	gamescope::OwningRc<gamescope::IBackendFb> pBackendFb;
	uint32_t fb_id = 0;

	if ( !wlr_drm_format_set_has( &drm->formats, dma_buf->format, dma_buf->modifier ) )
	{
		drm_log.errorf( "Cannot import FB to DRM: format 0x%" PRIX32 " and modifier 0x%" PRIX64 " not supported for scan-out", dma_buf->format, dma_buf->modifier );
		return nullptr;
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

	drm_log.debugf("make fbid %u", fb_id);

	pBackendFb = new gamescope::CDRMFb( fb_id );

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

	return pBackendFb;
}

static void update_drm_effective_orientations( struct drm_t *drm, const drmModeModeInfo *pMode )
{
	gamescope::IBackendConnector *pInternalConnector = GetBackend()->GetConnector( gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL );
	if ( pInternalConnector )
	{
		gamescope::CDRMConnector *pDRMInternalConnector = static_cast<gamescope::CDRMConnector *>( pInternalConnector );
		const drmModeModeInfo *pInternalMode = pMode;
		if ( pDRMInternalConnector != drm->pConnector )
			pInternalMode = find_mode( pDRMInternalConnector->GetModeConnector(), 0, 0, 0 );

		pDRMInternalConnector->UpdateEffectiveOrientation( pInternalMode );
	}

	gamescope::IBackendConnector *pExternalConnector = GetBackend()->GetConnector( gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL );
	if ( pExternalConnector )
	{
		gamescope::CDRMConnector *pDRMExternalConnector = static_cast<gamescope::CDRMConnector *>( pExternalConnector );
		const drmModeModeInfo *pExternalMode = pMode;
		if ( pDRMExternalConnector != drm->pConnector )
			pExternalMode = find_mode( pDRMExternalConnector->GetModeConnector(), 0, 0, 0 );

		pDRMExternalConnector->UpdateEffectiveOrientation( pExternalMode );
	}
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
		uint16_t opacity;
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
			hash_combine(hash, k.layerState[i].opacity);
			hash_combine(hash, k.layerState[i].colorEncoding);
			hash_combine(hash, k.layerState[i].colorRange);
			hash_combine(hash, k.layerState[i].colorspace);
		}

		return hash;
  	}
};


std::unordered_set<LiftoffStateCacheEntry, LiftoffStateCacheEntryKasher> g_LiftoffStateCache;

static inline amdgpu_transfer_function colorspace_to_plane_degamma_tf(GamescopeAppTextureColorspace colorspace)
{
	switch ( colorspace )
	{
		default: // Linear in this sense is SRGB. Linear = sRGB image view doing automatic sRGB -> Linear which doesn't happen on DRM side.
		case GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB:
			return AMDGPU_TRANSFER_FUNCTION_SRGB_EOTF;
		case GAMESCOPE_APP_TEXTURE_COLORSPACE_PASSTHRU:
		case GAMESCOPE_APP_TEXTURE_COLORSPACE_SCRGB:
			// Use LINEAR TF for scRGB float format as 80 nit = 1.0 in scRGB, which matches
			// what PQ TF decodes to/encodes from.
			// AMD internal format is FP16, and generally expected for 1.0 -> 80 nit.
			// which just so happens to match scRGB.
			return AMDGPU_TRANSFER_FUNCTION_IDENTITY;
		case GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ:
			return AMDGPU_TRANSFER_FUNCTION_PQ_EOTF;
	}
}

static inline amdgpu_transfer_function colorspace_to_plane_shaper_tf(GamescopeAppTextureColorspace colorspace)
{
	switch ( colorspace )
	{
		default:
		case GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB:
			return AMDGPU_TRANSFER_FUNCTION_SRGB_INV_EOTF;
		case GAMESCOPE_APP_TEXTURE_COLORSPACE_SCRGB: // scRGB Linear -> PQ for shaper + 3D LUT
		case GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ:
			return AMDGPU_TRANSFER_FUNCTION_PQ_INV_EOTF;
		case GAMESCOPE_APP_TEXTURE_COLORSPACE_PASSTHRU:
			return AMDGPU_TRANSFER_FUNCTION_DEFAULT;
	}
}

static inline amdgpu_transfer_function inverse_tf(amdgpu_transfer_function tf)
{
	switch ( tf )
	{
		default:
		case AMDGPU_TRANSFER_FUNCTION_DEFAULT:
			return AMDGPU_TRANSFER_FUNCTION_DEFAULT;
		case AMDGPU_TRANSFER_FUNCTION_IDENTITY:
			return AMDGPU_TRANSFER_FUNCTION_IDENTITY;
		case AMDGPU_TRANSFER_FUNCTION_SRGB_EOTF:
			return AMDGPU_TRANSFER_FUNCTION_SRGB_INV_EOTF;
		case AMDGPU_TRANSFER_FUNCTION_BT709_OETF:
			return AMDGPU_TRANSFER_FUNCTION_BT709_INV_OETF;
		case AMDGPU_TRANSFER_FUNCTION_PQ_EOTF:
			return AMDGPU_TRANSFER_FUNCTION_PQ_INV_EOTF;
		case AMDGPU_TRANSFER_FUNCTION_GAMMA22_EOTF:
			return AMDGPU_TRANSFER_FUNCTION_GAMMA22_INV_EOTF;
		case AMDGPU_TRANSFER_FUNCTION_GAMMA24_EOTF:
			return AMDGPU_TRANSFER_FUNCTION_GAMMA24_INV_EOTF;
		case AMDGPU_TRANSFER_FUNCTION_GAMMA26_EOTF:
			return AMDGPU_TRANSFER_FUNCTION_GAMMA26_INV_EOTF;
		case AMDGPU_TRANSFER_FUNCTION_SRGB_INV_EOTF:
			return AMDGPU_TRANSFER_FUNCTION_SRGB_EOTF;
		case AMDGPU_TRANSFER_FUNCTION_BT709_INV_OETF:
			return AMDGPU_TRANSFER_FUNCTION_BT709_OETF;
		case AMDGPU_TRANSFER_FUNCTION_PQ_INV_EOTF:
			return AMDGPU_TRANSFER_FUNCTION_PQ_EOTF;
		case AMDGPU_TRANSFER_FUNCTION_GAMMA22_INV_EOTF:
			return AMDGPU_TRANSFER_FUNCTION_GAMMA22_EOTF;
		case AMDGPU_TRANSFER_FUNCTION_GAMMA24_INV_EOTF:
			return AMDGPU_TRANSFER_FUNCTION_GAMMA24_EOTF;
		case AMDGPU_TRANSFER_FUNCTION_GAMMA26_INV_EOTF:
			return AMDGPU_TRANSFER_FUNCTION_GAMMA26_EOTF;
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
		entry.layerState[i].opacity = frameInfo->layers[i].opacity * 0xffff;
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
			m_Props.type                  = CDRMAtomicProperty::Instantiate( "type",                  this, *rawProperties );
			m_Props.IN_FORMATS            = CDRMAtomicProperty::Instantiate( "IN_FORMATS",            this, *rawProperties );

			m_Props.FB_ID                    = CDRMAtomicProperty::Instantiate( "FB_ID",                    this, *rawProperties );
			m_Props.IN_FENCE_FD              = CDRMAtomicProperty::Instantiate( "IN_FENCE_FD",              this, *rawProperties );
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
			m_Props.AMD_PLANE_DEGAMMA_TF     = CDRMAtomicProperty::Instantiate( "AMD_PLANE_DEGAMMA_TF",     this, *rawProperties );
			m_Props.AMD_PLANE_DEGAMMA_LUT    = CDRMAtomicProperty::Instantiate( "AMD_PLANE_DEGAMMA_LUT",    this, *rawProperties );
			m_Props.AMD_PLANE_CTM            = CDRMAtomicProperty::Instantiate( "AMD_PLANE_CTM",            this, *rawProperties );
			m_Props.AMD_PLANE_HDR_MULT       = CDRMAtomicProperty::Instantiate( "AMD_PLANE_HDR_MULT",       this, *rawProperties );
			m_Props.AMD_PLANE_SHAPER_LUT     = CDRMAtomicProperty::Instantiate( "AMD_PLANE_SHAPER_LUT",     this, *rawProperties );
			m_Props.AMD_PLANE_SHAPER_TF      = CDRMAtomicProperty::Instantiate( "AMD_PLANE_SHAPER_TF",      this, *rawProperties );
			m_Props.AMD_PLANE_LUT3D          = CDRMAtomicProperty::Instantiate( "AMD_PLANE_LUT3D",          this, *rawProperties );
			m_Props.AMD_PLANE_BLEND_TF       = CDRMAtomicProperty::Instantiate( "AMD_PLANE_BLEND_TF",       this, *rawProperties );
			m_Props.AMD_PLANE_BLEND_LUT      = CDRMAtomicProperty::Instantiate( "AMD_PLANE_BLEND_LUT",      this, *rawProperties );
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
			m_Props.ACTIVE              = CDRMAtomicProperty::Instantiate( "ACTIVE",              this, *rawProperties );
			m_Props.MODE_ID             = CDRMAtomicProperty::Instantiate( "MODE_ID",             this, *rawProperties );
			m_Props.GAMMA_LUT           = CDRMAtomicProperty::Instantiate( "GAMMA_LUT",           this, *rawProperties );
			m_Props.DEGAMMA_LUT         = CDRMAtomicProperty::Instantiate( "DEGAMMA_LUT",         this, *rawProperties );
			m_Props.CTM                 = CDRMAtomicProperty::Instantiate( "CTM",                 this, *rawProperties );
			m_Props.VRR_ENABLED         = CDRMAtomicProperty::Instantiate( "VRR_ENABLED",         this, *rawProperties );
			m_Props.OUT_FENCE_PTR       = CDRMAtomicProperty::Instantiate( "OUT_FENCE_PTR",       this, *rawProperties );
			m_Props.AMD_CRTC_REGAMMA_TF = CDRMAtomicProperty::Instantiate( "AMD_CRTC_REGAMMA_TF", this, *rawProperties );
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

		for ( int i = 0; i < m_pConnector->count_modes; i++ )
		{
			drmModeModeInfo *pMode = &m_pConnector->modes[i];
			m_Mutable.BackendModes.emplace_back( BackendMode
			{
				.uWidth   = pMode->hdisplay,
				.uHeight  = pMode->vdisplay,
				.uRefresh = pMode->vrefresh,
			});
		}

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

	void CDRMConnector::UpdateEffectiveOrientation( const drmModeModeInfo *pMode )
	{
		if ( this->GetScreenType() == GAMESCOPE_SCREEN_TYPE_INTERNAL && g_DesiredInternalOrientation != GAMESCOPE_PANEL_ORIENTATION_AUTO )
		{
			m_ChosenOrientation = g_DesiredInternalOrientation;
		}
		else
		{
			if ( this->GetProperties().panel_orientation )
			{
				switch ( this->GetProperties().panel_orientation->GetCurrentValue() )
				{
					case DRM_MODE_PANEL_ORIENTATION_NORMAL:
						m_ChosenOrientation = GAMESCOPE_PANEL_ORIENTATION_0;
						return;
					case DRM_MODE_PANEL_ORIENTATION_BOTTOM_UP:
						m_ChosenOrientation = GAMESCOPE_PANEL_ORIENTATION_180;
						return;
					case DRM_MODE_PANEL_ORIENTATION_LEFT_UP:
						m_ChosenOrientation = GAMESCOPE_PANEL_ORIENTATION_90;
						return;
					case DRM_MODE_PANEL_ORIENTATION_RIGHT_UP:
						m_ChosenOrientation = GAMESCOPE_PANEL_ORIENTATION_270;
						return;
					default:
						break;
				}
			}

			if ( this->GetScreenType() == gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL && pMode )
			{
				// Auto-detect portait mode for internal displays
				m_ChosenOrientation = pMode->hdisplay < pMode->vdisplay
					? GAMESCOPE_PANEL_ORIENTATION_270
					: GAMESCOPE_PANEL_ORIENTATION_0;
			}
			else
			{
				m_ChosenOrientation = GAMESCOPE_PANEL_ORIENTATION_0;
			}
		}
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

		bool bHasKnownColorimetry = false;
		bool bHasKnownHDRInfo = false;

		{
			CScriptScopedLock script;

			auto oKnownDisplay = script.Manager().Gamescope().Config.LookupDisplay( script, m_Mutable.szMakePNP, pProduct->product, m_Mutable.szModel );
			if ( oKnownDisplay )
			{
				sol::table tTable = oKnownDisplay->second;

				std::string_view psvPrettyName = tTable.get_or( "pretty_name", std::string_view{ "Untitled Display" } );
				drm_log.infof( "Got known display: %.*s (%.*s)",
					(int)oKnownDisplay->first.size(), oKnownDisplay->first.data(),
					(int)psvPrettyName.size(), psvPrettyName.data() );

				m_Mutable.fnDynamicModeGenerator = nullptr;
				m_Mutable.ValidDynamicRefreshRates.clear();

				sol::optional<sol::table> otDynamicRefreshRates = tTable["dynamic_refresh_rates"];
				sol::optional<sol::function> ofnDynamicModegen = tTable["dynamic_modegen"];

				if ( otDynamicRefreshRates && ofnDynamicModegen )
				{
					m_Mutable.ValidDynamicRefreshRates = TableToVector<uint32_t>( *otDynamicRefreshRates );

					m_Mutable.fnDynamicModeGenerator = [ fnDynamicModegen = *ofnDynamicModegen ]( const drmModeModeInfo *pBaseMode, int nRefreshHz ) -> drmModeModeInfo
					{
						CScriptScopedLock script;

						sol::table tInMode = script->create_table();
						tInMode["clock"] = pBaseMode->clock;

						tInMode["hdisplay"] = pBaseMode->hdisplay;
						tInMode["hsync_start"] = pBaseMode->hsync_start;
						tInMode["hsync_end"] = pBaseMode->hsync_end;
						tInMode["htotal"] = pBaseMode->htotal;

						tInMode["vdisplay"] = pBaseMode->vdisplay;
						tInMode["vsync_start"] = pBaseMode->vsync_start;
						tInMode["vsync_end"] = pBaseMode->vsync_end;
						tInMode["vtotal"] = pBaseMode->vtotal;

						tInMode["vrefresh"] = pBaseMode->vrefresh;

						sol::function_result ret = fnDynamicModegen(tInMode, nRefreshHz);
						if ( !ret.valid() || !ret.get<sol::table>() )
							return *pBaseMode;
						
						sol::table tOutMode = ret;

						drmModeModeInfo outMode = *pBaseMode;
						outMode.clock = tOutMode["clock"];

						outMode.hdisplay = tOutMode["hdisplay"];
						outMode.hsync_start = tOutMode["hsync_start"];
						outMode.hsync_end = tOutMode["hsync_end"];
						outMode.htotal = tOutMode["htotal"];

						outMode.vdisplay = tOutMode["vdisplay"];
						outMode.vsync_start = tOutMode["vsync_start"];
						outMode.vsync_end = tOutMode["vsync_end"];
						outMode.vtotal = tOutMode["vtotal"];

						outMode.vrefresh = tOutMode["vrefresh"];

						snprintf( outMode.name, sizeof( outMode.name ), "%dx%d@%d.00", outMode.hdisplay, outMode.vdisplay, nRefreshHz );

						return outMode;
					};
				}

				if ( sol::optional<sol::table> otColorimetry = tTable["colorimetry"] )
				{
					sol::table tColorimetry = *otColorimetry;

					// TODO: Add a vec2 + colorimetry type?
					sol::optional<sol::table> otR = tColorimetry["r"];
					sol::optional<sol::table> otG = tColorimetry["g"];
					sol::optional<sol::table> otB = tColorimetry["b"];
					sol::optional<sol::table> otW = tColorimetry["w"];

					if ( otR && otG && otB && otW )
					{
						m_Mutable.DisplayColorimetry.primaries.r = TableToVec<glm::vec2>( *otR );
						m_Mutable.DisplayColorimetry.primaries.g = TableToVec<glm::vec2>( *otG );
						m_Mutable.DisplayColorimetry.primaries.b = TableToVec<glm::vec2>( *otB );
						m_Mutable.DisplayColorimetry.white = TableToVec<glm::vec2>( *otW );

						bHasKnownColorimetry = true;
					}
				}

				if ( sol::optional<sol::table> otHDRInfo = tTable["hdr"] )
				{
					m_Mutable.HDR.bExposeHDRSupport = otHDRInfo->get_or( "supported", false );
					m_Mutable.HDR.eOutputEncodingEOTF = otHDRInfo->get_or( "eotf", EOTF_Gamma22 );
					m_Mutable.HDR.uMaxContentLightLevel = nits_to_u16( otHDRInfo->get_or( "max_content_light_level", 400.0f ) );
					m_Mutable.HDR.uMaxFrameAverageLuminance = nits_to_u16( otHDRInfo->get_or( "max_frame_average_luminance", 400.0f ) );
					m_Mutable.HDR.uMinContentLightLevel = nits_to_u16_dark( otHDRInfo->get_or( "min_content_light_level", 0.1f ) );

					bHasKnownHDRInfo = true;
				}
			}
		}

		if ( !bHasKnownColorimetry )
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
			else
			{
				// Assume 709 if we have no data at all.
				m_Mutable.DisplayColorimetry = displaycolorimetry_709;
			}
		}

		drm_log.infof( "[colorimetry]: r %f %f", m_Mutable.DisplayColorimetry.primaries.r.x, m_Mutable.DisplayColorimetry.primaries.r.y );
		drm_log.infof( "[colorimetry]: g %f %f", m_Mutable.DisplayColorimetry.primaries.g.x, m_Mutable.DisplayColorimetry.primaries.g.y );
		drm_log.infof( "[colorimetry]: b %f %f", m_Mutable.DisplayColorimetry.primaries.b.x, m_Mutable.DisplayColorimetry.primaries.b.y );
		drm_log.infof( "[colorimetry]: w %f %f", m_Mutable.DisplayColorimetry.white.x, m_Mutable.DisplayColorimetry.white.y );

		/////////////////////
		// Parse HDR stuff.
		/////////////////////
		if ( !bHasKnownHDRInfo )
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

				m_Mutable.HDR.pDefaultMetadataBlob = GetBackend()->CreateBackendBlob( defaultHDRMetadata );
			}
			else
			{
				m_Mutable.HDR.bExposeHDRSupport = false;
			}
		}
	}

	/////////////////////////
	// CDRMFb
	/////////////////////////
	CDRMFb::CDRMFb( uint32_t uFbId )
		: m_uFbId{ uFbId }
	{

	}
	CDRMFb::~CDRMFb()
	{
		// I own the fbid.
		if ( drmModeRmFB( g_DRM.fd, m_uFbId ) != 0 )
			drm_log.errorf_errno( "drmModeRmFB failed" );
		m_uFbId = 0;
	}
}

static int
drm_prepare_liftoff( struct drm_t *drm, const struct FrameInfo_t *frameInfo, bool needs_modeset )
{
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

	bool bSinglePlane = frameInfo->layerCount < 2 && cv_drm_single_plane_optimizations;

	for ( int i = 0; i < k_nMaxLayers; i++ )
	{
		if ( i < frameInfo->layerCount )
		{
			const FrameInfo_t::Layer_t *pLayer = &frameInfo->layers[ i ];
			gamescope::CDRMFb *pDrmFb = static_cast<gamescope::CDRMFb *>( pLayer->tex ? pLayer->tex->GetBackendFb() : nullptr );

			if ( pDrmFb == nullptr )
			{
				drm_log.debugf("drm_prepare_liftoff: layer %d has no FB", i );
				return -EINVAL;
			}

			const int nFence = cv_drm_debug_disable_in_fence_fd ? -1 : g_nAlwaysSignalledSyncFile;


			liftoff_layer_set_property( drm->lo_layers[ i ], "FB_ID", pDrmFb->GetFbId());
			liftoff_layer_set_property( drm->lo_layers[ i ], "IN_FENCE_FD", nFence );
			drm->m_FbIdsInRequest.emplace_back( pDrmFb );

			liftoff_layer_set_property( drm->lo_layers[ i ], "zpos", entry.layerState[i].zpos );
			liftoff_layer_set_property( drm->lo_layers[ i ], "alpha", frameInfo->layers[ i ].opacity * 0xffff);

			liftoff_layer_set_property( drm->lo_layers[ i ], "SRC_X", 0);
			liftoff_layer_set_property( drm->lo_layers[ i ], "SRC_Y", 0);
			liftoff_layer_set_property( drm->lo_layers[ i ], "SRC_W", entry.layerState[i].srcW );
			liftoff_layer_set_property( drm->lo_layers[ i ], "SRC_H", entry.layerState[i].srcH );

			uint64_t ulOrientation = DRM_MODE_ROTATE_0;
			switch ( drm->pConnector->GetCurrentOrientation() )
			{
			default:
			case GAMESCOPE_PANEL_ORIENTATION_0:
				ulOrientation = DRM_MODE_ROTATE_0;
				break;
			case GAMESCOPE_PANEL_ORIENTATION_270:
				ulOrientation = DRM_MODE_ROTATE_270;
				break;
			case GAMESCOPE_PANEL_ORIENTATION_90:
				ulOrientation = DRM_MODE_ROTATE_90;
				break;
			case GAMESCOPE_PANEL_ORIENTATION_180:
				ulOrientation = DRM_MODE_ROTATE_180;
				break;
			}
			liftoff_layer_set_property( drm->lo_layers[ i ], "rotation", ulOrientation );

			liftoff_layer_set_property( drm->lo_layers[ i ], "CRTC_X", entry.layerState[i].crtcX);
			liftoff_layer_set_property( drm->lo_layers[ i ], "CRTC_Y", entry.layerState[i].crtcY);

			liftoff_layer_set_property( drm->lo_layers[ i ], "CRTC_W", entry.layerState[i].crtcW);
			liftoff_layer_set_property( drm->lo_layers[ i ], "CRTC_H", entry.layerState[i].crtcH);

			if ( frameInfo->layers[i].applyColorMgmt )
			{
				bool bYCbCr = entry.layerState[i].ycbcr;

				if ( !cv_drm_debug_disable_color_encoding && bYCbCr )
				{
					liftoff_layer_set_property( drm->lo_layers[ i ], "COLOR_ENCODING", entry.layerState[i].colorEncoding );
				}
				else
				{
					liftoff_layer_unset_property( drm->lo_layers[ i ], "COLOR_ENCODING" );
				}

				if ( !cv_drm_debug_disable_color_range && bYCbCr )
				{
					liftoff_layer_set_property( drm->lo_layers[ i ], "COLOR_RANGE",    entry.layerState[i].colorRange );
				}
				else
				{
					liftoff_layer_unset_property( drm->lo_layers[ i ], "COLOR_RANGE" );
				}

				if ( drm_supports_color_mgmt( drm ) )
				{
					amdgpu_transfer_function degamma_tf = colorspace_to_plane_degamma_tf( entry.layerState[i].colorspace );
					amdgpu_transfer_function shaper_tf = colorspace_to_plane_shaper_tf( entry.layerState[i].colorspace );

					if ( bYCbCr )
					{
						// JoshA: Based on the Steam In-Home Streaming Shader,
						// it looks like Y is actually sRGB, not HDTV G2.4
						//
						// Matching BT709 for degamma -> regamma on shaper TF here
						// is identity and works on YUV NV12 planes to preserve this.
						//
						// Doing LINEAR/DEFAULT here introduces banding so... this is the best way.
						// (sRGB DEGAMMA does NOT work on YUV planes!)
						degamma_tf = AMDGPU_TRANSFER_FUNCTION_BT709_INV_OETF;
						shaper_tf = AMDGPU_TRANSFER_FUNCTION_BT709_OETF;
					}

					bool bUseDegamma = !cv_drm_debug_disable_degamma_tf;
					if ( bUseDegamma )
						liftoff_layer_set_property( drm->lo_layers[ i ], "AMD_PLANE_DEGAMMA_TF", degamma_tf );
					else
						liftoff_layer_set_property( drm->lo_layers[ i ], "AMD_PLANE_DEGAMMA_TF", 0 );

					bool bUseShaperAnd3DLUT = !cv_drm_debug_disable_shaper_and_3dlut;
					if ( bUseShaperAnd3DLUT )
					{
						liftoff_layer_set_property( drm->lo_layers[ i ], "AMD_PLANE_SHAPER_LUT", drm->pending.shaperlut_id[ ColorSpaceToEOTFIndex( entry.layerState[i].colorspace ) ]->GetBlobValue() );
						liftoff_layer_set_property( drm->lo_layers[ i ], "AMD_PLANE_SHAPER_TF", shaper_tf );
						liftoff_layer_set_property( drm->lo_layers[ i ], "AMD_PLANE_LUT3D", drm->pending.lut3d_id[ ColorSpaceToEOTFIndex( entry.layerState[i].colorspace ) ]->GetBlobValue() );
						// Josh: See shaders/colorimetry.h colorspace_blend_tf if you have questions as to why we start doing sRGB for BLEND_TF despite potentially working in Gamma 2.2 space prior.
					}
					else
					{
						liftoff_layer_set_property( drm->lo_layers[ i ], "AMD_PLANE_SHAPER_LUT", 0 );
						liftoff_layer_set_property( drm->lo_layers[ i ], "AMD_PLANE_SHAPER_TF", 0 );
						liftoff_layer_set_property( drm->lo_layers[ i ], "AMD_PLANE_LUT3D", 0 );
					}
				}
			}
			else
			{
				if ( drm_supports_color_mgmt( drm ) )
				{
					liftoff_layer_set_property( drm->lo_layers[ i ], "AMD_PLANE_DEGAMMA_TF", AMDGPU_TRANSFER_FUNCTION_DEFAULT );
					liftoff_layer_set_property( drm->lo_layers[ i ], "AMD_PLANE_SHAPER_LUT", 0 );
					liftoff_layer_set_property( drm->lo_layers[ i ], "AMD_PLANE_SHAPER_TF", 0 );
					liftoff_layer_set_property( drm->lo_layers[ i ], "AMD_PLANE_LUT3D", 0 );
					liftoff_layer_set_property( drm->lo_layers[ i ], "AMD_PLANE_CTM", 0 );
				}
			}

			if ( drm_supports_color_mgmt( drm ) )
			{
				if (!cv_drm_debug_disable_blend_tf && !bSinglePlane)
					liftoff_layer_set_property( drm->lo_layers[ i ], "AMD_PLANE_BLEND_TF", drm->pending.output_tf );
				else
					liftoff_layer_set_property( drm->lo_layers[ i ], "AMD_PLANE_BLEND_TF", AMDGPU_TRANSFER_FUNCTION_DEFAULT );

				if (!cv_drm_debug_disable_ctm && frameInfo->layers[i].ctm != nullptr)
					liftoff_layer_set_property( drm->lo_layers[ i ], "AMD_PLANE_CTM", frameInfo->layers[i].ctm->GetBlobValue() );
				else
					liftoff_layer_set_property( drm->lo_layers[ i ], "AMD_PLANE_CTM", 0 );
			}
		}
		else
		{
			liftoff_layer_set_property( drm->lo_layers[ i ], "FB_ID", 0 );
			liftoff_layer_set_property( drm->lo_layers[ i ], "IN_FENCE_FD", -1 );

			liftoff_layer_unset_property( drm->lo_layers[ i ], "COLOR_ENCODING" );
			liftoff_layer_unset_property( drm->lo_layers[ i ], "COLOR_RANGE" );

			if ( drm_supports_color_mgmt( drm ) )
			{
				liftoff_layer_set_property( drm->lo_layers[ i ], "AMD_PLANE_DEGAMMA_TF", AMDGPU_TRANSFER_FUNCTION_DEFAULT );
				liftoff_layer_set_property( drm->lo_layers[ i ], "AMD_PLANE_SHAPER_LUT", 0 );
				liftoff_layer_set_property( drm->lo_layers[ i ], "AMD_PLANE_SHAPER_TF", 0 );
				liftoff_layer_set_property( drm->lo_layers[ i ], "AMD_PLANE_LUT3D", 0 );
				liftoff_layer_set_property( drm->lo_layers[ i ], "AMD_PLANE_BLEND_TF", AMDGPU_TRANSFER_FUNCTION_DEFAULT );
				liftoff_layer_set_property( drm->lo_layers[ i ], "AMD_PLANE_CTM", 0 );
			}
		}
	}

	struct liftoff_output_apply_options lo_options = {
		.timeout_ns = std::numeric_limits<int64_t>::max()
	};

	int ret = liftoff_output_apply( drm->lo_output, drm->req, drm->flags, &lo_options);

	// The NVIDIA 555 series drivers started advertising DRM_CAP_SYNCOBJ, but do
	// not support IN_FENCE_FD. However, there is no way to hide the IN_FENCE_FD
	// property in a DRM-KMS driver, so the driver returns EPERM when an
	// application sets IN_FENCE_FD. To work around this, the first time a
	// commit fails with -EPERM, try it again with the IN_FENCE_FD property
	// reset to its default value. If this succeeds, disable use of the
	// IN_FENCE_FD property.
	static bool attempted_in_fence_fallback = false;
	if ( ret == -EPERM && !attempted_in_fence_fallback && !cv_drm_debug_disable_in_fence_fd )
	{
		attempted_in_fence_fallback = true;
		for ( int i = 0; i < frameInfo->layerCount; i++ )
		{
			liftoff_layer_set_property( drm->lo_layers[ i ], "IN_FENCE_FD", -1 );
		}

		ret = liftoff_output_apply( drm->lo_output, drm->req, drm->flags, &lo_options );

		if ( ret == 0 )
		{
			// IN_FENCE_FD isn't actually supported. Avoid it in the future.
			cv_drm_debug_disable_in_fence_fd  = true;
		}
	}

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
		drm_log.debugf( "can drm present %i layers", frameInfo->layerCount );
	else
		drm_log.debugf( "can NOT drm present %i layers", frameInfo->layerCount );

	return ret;
}

bool g_bForceAsyncFlips = false;

void drm_rollback( struct drm_t *drm )
{
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
}

/* Prepares an atomic commit for the provided scene-graph. Returns 0 on success,
 * negative errno on failure or if the scene-graph can't be presented directly. */
int drm_prepare( struct drm_t *drm, bool async, const struct FrameInfo_t *frameInfo )
{
	drm_update_color_mgmt(drm);

	const bool bIsVRRCapable = drm->pConnector && drm->pConnector->GetProperties().vrr_capable && !!drm->pConnector->GetProperties().vrr_capable->GetCurrentValue();
	const bool bHasVRREnable = drm->pCRTC && drm->pCRTC->GetProperties().VRR_ENABLED;

	const bool bVRREnabled = bIsVRRCapable && bHasVRREnable && frameInfo->allowVRR;
	if ( bIsVRRCapable )
	{
		if ( bVRREnabled != !!drm->pCRTC->GetProperties().VRR_ENABLED->GetCurrentValue() )
			drm->needs_modeset = true;
	}

	drm_colorspace uColorimetry = DRM_MODE_COLORIMETRY_DEFAULT;

	const bool bWantsHDR10 = g_bOutputHDREnabled && frameInfo->outputEncodingEOTF == EOTF_PQ;
	gamescope::BackendBlob *pHDRMetadata = nullptr;
	if ( drm->pConnector && drm->pConnector->SupportsHDR10() )
	{
		if ( bWantsHDR10 )
		{
			pHDRMetadata = drm->pConnector->GetHDRInfo().pDefaultMetadataBlob.get();

			wlserver_vk_swapchain_feedback* pFeedback = steamcompmgr_get_base_layer_swapchain_feedback();
			if ( pFeedback && pFeedback->hdr_metadata_blob != nullptr )
				pHDRMetadata = pFeedback->hdr_metadata_blob.get();
			uColorimetry = DRM_MODE_COLORIMETRY_BT2020_RGB;
		}
		else
		{
			pHDRMetadata = drm->sdr_static_metadata.get();
			uColorimetry = DRM_MODE_COLORIMETRY_DEFAULT;
		}

		if ( uColorimetry != drm->pConnector->GetProperties().Colorspace->GetCurrentValue() )
			drm->needs_modeset = true;
	}

	drm->m_FbIdsInRequest.clear();

	bool needs_modeset = drm->needs_modeset.exchange(false);

	assert( drm->req == nullptr );
	drm->req = drmModeAtomicAlloc();

	bool bSinglePlane = frameInfo->layerCount < 2 && cv_drm_single_plane_optimizations;

	if ( drm_supports_color_mgmt( &g_DRM ) && frameInfo->applyOutputColorMgmt )
	{
		if ( !cv_drm_debug_disable_output_tf && !bSinglePlane )
		{
			drm->pending.output_tf = g_bOutputHDREnabled
				? AMDGPU_TRANSFER_FUNCTION_PQ_EOTF
				: AMDGPU_TRANSFER_FUNCTION_SRGB_EOTF;
		}
		else
		{
			drm->pending.output_tf = AMDGPU_TRANSFER_FUNCTION_DEFAULT;
		}
	}
	else
	{
		drm->pending.output_tf = AMDGPU_TRANSFER_FUNCTION_DEFAULT;
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

			if ( pCRTC->GetProperties().AMD_CRTC_REGAMMA_TF )
				pCRTC->GetProperties().AMD_CRTC_REGAMMA_TF->SetPendingValue( drm->req, 0, bForceInRequest );
		}

		if ( drm->pConnector )
		{
			// Always set our CRTC_ID for the modeset, especially
			// as we zero-ed it above.
			drm->pConnector->GetProperties().CRTC_ID->SetPendingValue( drm->req, drm->pCRTC->GetObjectId(), bForceInRequest );

			if ( drm->pConnector->GetProperties().Colorspace )
				drm->pConnector->GetProperties().Colorspace->SetPendingValue( drm->req, uColorimetry, bForceInRequest );
		}

		if ( drm->pCRTC )
		{
			drm->pCRTC->GetProperties().ACTIVE->SetPendingValue( drm->req, 1u, true );
			drm->pCRTC->GetProperties().MODE_ID->SetPendingValue( drm->req, drm->pending.mode_id ? drm->pending.mode_id->GetBlobValue() : 0lu, true );

			if ( drm->pCRTC->GetProperties().VRR_ENABLED )
				drm->pCRTC->GetProperties().VRR_ENABLED->SetPendingValue( drm->req, bVRREnabled, true );
		}
	}

	if ( drm->pConnector )
	{
		if ( drm->pConnector->GetProperties().HDR_OUTPUT_METADATA )
			drm->pConnector->GetProperties().HDR_OUTPUT_METADATA->SetPendingValue( drm->req, pHDRMetadata ? pHDRMetadata->GetBlobValue() : 0lu, bForceInRequest );

		if ( drm->pConnector->GetProperties().content_type )
			drm->pConnector->GetProperties().content_type->SetPendingValue( drm->req, DRM_MODE_CONTENT_TYPE_GAME, bForceInRequest );
	}

	if ( drm->pCRTC )
	{
		if ( drm->pCRTC->GetProperties().AMD_CRTC_REGAMMA_TF )
		{
			if ( !cv_drm_debug_disable_regamma_tf )
				drm->pCRTC->GetProperties().AMD_CRTC_REGAMMA_TF->SetPendingValue( drm->req, inverse_tf( drm->pending.output_tf ), bForceInRequest );
			else
				drm->pCRTC->GetProperties().AMD_CRTC_REGAMMA_TF->SetPendingValue( drm->req, AMDGPU_TRANSFER_FUNCTION_DEFAULT, bForceInRequest );
		}
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
		drm_rollback( drm );

		drmModeAtomicFree( drm->req );
		drm->req = nullptr;

		drm->m_FbIdsInRequest.clear();

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

	// If we are changing connector, zero out the current and pending mode IDs.
	// So we don't try to use one mode from the old connector on the new one if we roll back.
	drm->pending.mode_id = nullptr;
	drm->current.mode_id = nullptr;

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

		drm->pending.shaperlut_id[ i ] = GetBackend()->CreateBackendBlob( g_ColorMgmtLuts[i].lut1d );
		drm->pending.lut3d_id[ i ] = GetBackend()->CreateBackendBlob( g_ColorMgmtLuts[i].lut3d );
	}

	return true;
}

int g_nDynamicRefreshHz = 0;

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
		g_nOutputRefresh = gamescope::ConvertHztomHz( 60 );
	g_nDynamicRefreshHz = 0;

	g_bRotated = false;
}

bool drm_set_mode( struct drm_t *drm, const drmModeModeInfo *mode )
{
	if (!drm->pConnector || !drm->pConnector->GetModeConnector())
		return false;

	drm_log.infof("selecting mode %dx%d@%uHz", mode->hdisplay, mode->vdisplay, mode->vrefresh);

	drm->pending.mode_id = GetBackend()->CreateBackendBlob( *mode );
	drm->needs_modeset = true;

	g_nOutputRefresh = gamescope::GetModeRefresh( mode );
	g_nDynamicRefreshHz = 0;

	update_drm_effective_orientations(drm, mode);

	switch ( drm->pConnector->GetCurrentOrientation() )
	{
	default:
	case GAMESCOPE_PANEL_ORIENTATION_0:
	case GAMESCOPE_PANEL_ORIENTATION_180:
		g_bRotated = false;
		g_nOutputWidth = mode->hdisplay;
		g_nOutputHeight = mode->vdisplay;
		break;
	case GAMESCOPE_PANEL_ORIENTATION_90:
	case GAMESCOPE_PANEL_ORIENTATION_270:
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
		if ( g_DRM.pConnector && g_DRM.pConnector->GetModeGenerator() )
		{
			const drmModeModeInfo *preferred_mode = find_mode(connector, 0, 0, 0);
			mode = g_DRM.pConnector->GetModeGenerator()( preferred_mode, refresh );
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
					generate_fixed_mode( &mode, preferred_mode, refresh );
					break;
				}
			}
		}
	}

	mode.type = DRM_MODE_TYPE_USERDEF;

	bool bSuccess = drm_set_mode(drm, &mode);
	if ( !bSuccess )
		return false;

	g_nDynamicRefreshHz = refresh;

	return true;
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

bool drm_get_vrr_capable(struct drm_t *drm)
{
	if ( drm->pConnector )
		return drm->pConnector->SupportsVRR();

	return false;
}

bool drm_supports_hdr( struct drm_t *drm, uint16_t *maxCLL, uint16_t *maxFALL )
{
	if ( drm->pConnector && drm->pConnector->SupportsHDR() )
	{
		if ( maxCLL )
			*maxCLL = drm->pConnector->GetHDRInfo().uMaxContentLightLevel;
		if ( maxFALL )
			*maxFALL = drm->pConnector->GetHDRInfo().uMaxFrameAverageLuminance;
		return true;
	}

	return false;
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

bool drm_supports_color_mgmt(struct drm_t *drm)
{
	if ( g_bForceDisableColorMgmt )
		return false;

	if ( !drm->pPrimaryPlane )
		return false;

	return drm->pPrimaryPlane->GetProperties().AMD_PLANE_CTM.has_value() && drm->pPrimaryPlane->GetProperties().AMD_PLANE_BLEND_TF.has_value();
}

std::span<const uint32_t> drm_get_valid_refresh_rates( struct drm_t *drm )
{
	if ( drm && drm->pConnector )
		return drm->pConnector->GetValidDynamicRefreshRates();

	return std::span<const uint32_t>{};
}

namespace gamescope
{
	class CDRMBackend;

	class CDRMBackend final : public CBaseBackend
	{
	public:
		CDRMBackend()
		{
		}

		virtual ~CDRMBackend()
		{
			if ( g_DRM.fd != -1 )
				finish_drm( &g_DRM );
		}

		virtual bool Init() override
		{
			if ( !vulkan_init( vulkan_get_instance(), VK_NULL_HANDLE ) )
			{
				fprintf( stderr, "Failed to initialize Vulkan\n" );
				return false;
			}

			if ( !wlsession_init() )
			{
				fprintf( stderr, "Failed to initialize Wayland session\n" );
				return false;
			}

			return init_drm( &g_DRM, g_nPreferredOutputWidth, g_nPreferredOutputHeight, g_nNestedRefresh );
		}

		virtual bool PostInit() override
		{
			if ( g_DRM.pConnector )
				WritePatchedEdid( g_DRM.pConnector->GetRawEDID(), g_DRM.pConnector->GetHDRInfo(), g_bRotated );
			return true;
		}

        virtual std::span<const char *const> GetInstanceExtensions() const override
		{
			return std::span<const char *const>{};
		}
        virtual std::span<const char *const> GetDeviceExtensions( VkPhysicalDevice pVkPhysicalDevice ) const override
		{
			return std::span<const char *const>{};
		}
        virtual VkImageLayout GetPresentLayout() const override
		{
			// Does not matter, as this has a queue family transition
			// to VK_QUEUE_FAMILY_FOREIGN_EXT queue,
			// thus: newLayout is ignored.
			return VK_IMAGE_LAYOUT_GENERAL;
		}
        virtual void GetPreferredOutputFormat( uint32_t *pPrimaryPlaneFormat, uint32_t *pOverlayPlaneFormat ) const override
        {
			*pPrimaryPlaneFormat = g_nDRMFormat;
			*pOverlayPlaneFormat = g_nDRMFormatOverlay;
        }
		virtual bool ValidPhysicalDevice( VkPhysicalDevice pVkPhysicalDevice ) const override
		{
			return true;
		}

		virtual int Present( const FrameInfo_t *pFrameInfo, bool bAsync ) override
		{
			bool bWantsPartialComposite = pFrameInfo->layerCount >= 3 && !kDisablePartialComposition;

			static bool s_bWasFirstFrame = true;
			bool bWasFirstFrame = s_bWasFirstFrame;
			s_bWasFirstFrame = false;

			bool bDrewCursor = false;
			for ( uint32_t i = 0; i < k_nMaxLayers; i++ )
			{
				if ( pFrameInfo->layers[i].zpos == g_zposCursor )
				{
					bDrewCursor = true;
					break;
				}
			}

			bool bLayer0ScreenSize = close_enough(pFrameInfo->layers[0].scale.x, 1.0f) && close_enough(pFrameInfo->layers[0].scale.y, 1.0f);

			bool bNeedsCompositeFromFilter = (g_upscaleFilter == GamescopeUpscaleFilter::NEAREST || g_upscaleFilter == GamescopeUpscaleFilter::PIXEL) && !bLayer0ScreenSize;

			bool bNeedsFullComposite = false;
			bNeedsFullComposite |= cv_composite_force;
			bNeedsFullComposite |= bWasFirstFrame;
			bNeedsFullComposite |= pFrameInfo->useFSRLayer0;
			bNeedsFullComposite |= pFrameInfo->useNISLayer0;
			bNeedsFullComposite |= pFrameInfo->blurLayer0;
			bNeedsFullComposite |= bNeedsCompositeFromFilter;
			bNeedsFullComposite |= !k_bUseCursorPlane && bDrewCursor;
			bNeedsFullComposite |= g_bColorSliderInUse;
			bNeedsFullComposite |= pFrameInfo->bFadingOut;
			bNeedsFullComposite |= !g_reshade_effect.empty();

			if ( g_bOutputHDREnabled )
			{
				bNeedsFullComposite |= g_bHDRItmEnable;
				if ( !SupportsColorManagement() )
					bNeedsFullComposite |= ( pFrameInfo->layerCount > 1 || pFrameInfo->layers[0].colorspace != GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ );
			}
			else
			{
				if ( !SupportsColorManagement() )
					bNeedsFullComposite |= ColorspaceIsHDR( pFrameInfo->layers[0].colorspace );
			}

			bNeedsFullComposite |= !!(g_uCompositeDebug & CompositeDebugFlag::Heatmap);

			bool bDoComposite = true;
			if ( !bNeedsFullComposite && !bWantsPartialComposite )
			{
				int ret = drm_prepare( &g_DRM, bAsync, pFrameInfo );
				if ( ret == 0 )
					bDoComposite = false;
				else if ( ret == -EACCES )
					return 0;
			}

			// Update to let the vblank manager know we are currently compositing.
			GetVBlankTimer().UpdateWasCompositing( bDoComposite );

			if ( !bDoComposite )
			{
				// Scanout + Planes Path
				m_bWasPartialCompsiting = false;
				m_bWasCompositing = false;
				if ( pFrameInfo->layerCount == 2 )
					m_nLastSingleOverlayZPos = pFrameInfo->layers[1].zpos;

				return Commit( pFrameInfo );
			}

			// Composition Path
			if ( kDisablePartialComposition )
				bNeedsFullComposite = true;

			FrameInfo_t compositeFrameInfo = *pFrameInfo;

			if ( compositeFrameInfo.layerCount == 1 )
			{
				// If we failed to flip a single plane then
				// we definitely need to composite for some reason...
				bNeedsFullComposite = true;
			}

			if ( !bNeedsFullComposite )
			{
				// If we want to partial composite, fallback to full
				// composite if we have mismatching colorspaces in our overlays.
				// This is 2, and we do i-1 so 1...layerCount. So AFTER we have removed baseplane.
				// Overlays only.
				//
				// Josh:
				// We could handle mismatching colorspaces for partial composition
				// but I want to keep overlay -> partial composition promotion as simple
				// as possible, using the same 3D + SHAPER LUTs + BLEND in DRM
				// as changing them is incredibly expensive!! It takes forever.
				// We can't just point it to random BDA or whatever, it has to be uploaded slowly
				// thru registers which is SUPER SLOW.
				// This avoids stutter.
				for ( int i = 2; i < compositeFrameInfo.layerCount; i++ )
				{
					if ( pFrameInfo->layers[i - 1].colorspace != pFrameInfo->layers[i].colorspace )
					{
						bNeedsFullComposite = true;
						break;
					}
				}
			}

			// If we ever promoted from partial -> full, for the first frame
			// do NOT defer this partial composition.
			// We were already stalling for the full composition before, so it's not an issue
			// for latency, we just need to make sure we get 1 partial frame that isn't deferred
			// in time so we don't lose layers.
			bool bDefer = !bNeedsFullComposite && ( !m_bWasCompositing || m_bWasPartialCompsiting );

			// If doing a partial composition then remove the baseplane
			// from our frameinfo to composite.
			if ( !bNeedsFullComposite )
			{
				for ( int i = 1; i < compositeFrameInfo.layerCount; i++ )
					compositeFrameInfo.layers[i - 1] = compositeFrameInfo.layers[i];
				compositeFrameInfo.layerCount -= 1;

				// When doing partial composition, apply the shaper + 3D LUT stuff
				// at scanout.
				for ( uint32_t nEOTF = 0; nEOTF < EOTF_Count; nEOTF++ ) {
					compositeFrameInfo.shaperLut[ nEOTF ] = nullptr;
					compositeFrameInfo.lut3D[ nEOTF ] = nullptr;
				}
			}

			// If using composite debug markers, make sure we mark them as partial
			// so we know!
			if ( bDefer && !!( g_uCompositeDebug & CompositeDebugFlag::Markers ) )
				g_uCompositeDebug |= CompositeDebugFlag::Markers_Partial;

			std::optional oCompositeResult = vulkan_composite( &compositeFrameInfo, nullptr, !bNeedsFullComposite );

			m_bWasCompositing = true;

			g_uCompositeDebug &= ~CompositeDebugFlag::Markers_Partial;

			if ( !oCompositeResult )
			{
				xwm_log.errorf("vulkan_composite failed");
				return -EINVAL;
			}

			vulkan_wait( *oCompositeResult, true );

			FrameInfo_t presentCompFrameInfo = {};
			presentCompFrameInfo.allowVRR = pFrameInfo->allowVRR;
			presentCompFrameInfo.outputEncodingEOTF = pFrameInfo->outputEncodingEOTF;

			if ( bNeedsFullComposite )
			{
				presentCompFrameInfo.applyOutputColorMgmt = false;
				presentCompFrameInfo.layerCount = 1;

				FrameInfo_t::Layer_t *baseLayer = &presentCompFrameInfo.layers[ 0 ];
				baseLayer->scale.x = 1.0;
				baseLayer->scale.y = 1.0;
				baseLayer->opacity = 1.0;
				baseLayer->zpos = g_zposBase;

				baseLayer->tex = vulkan_get_last_output_image( false, false );
				baseLayer->applyColorMgmt = false;

				baseLayer->filter = GamescopeUpscaleFilter::NEAREST;
				baseLayer->ctm = nullptr;
				baseLayer->colorspace = pFrameInfo->outputEncodingEOTF == EOTF_PQ ? GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ : GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB;

				m_bWasPartialCompsiting = false;
			}
			else
			{
				if ( m_bWasPartialCompsiting || !bDefer )
				{
					presentCompFrameInfo.applyOutputColorMgmt = g_ColorMgmt.pending.enabled;
					presentCompFrameInfo.layerCount = 2;

					presentCompFrameInfo.layers[ 0 ] = pFrameInfo->layers[ 0 ];
					presentCompFrameInfo.layers[ 0 ].zpos = g_zposBase;

					FrameInfo_t::Layer_t *overlayLayer = &presentCompFrameInfo.layers[ 1 ];
					overlayLayer->scale.x = 1.0;
					overlayLayer->scale.y = 1.0;
					overlayLayer->opacity = 1.0;
					overlayLayer->zpos = g_zposOverlay;

					overlayLayer->tex = vulkan_get_last_output_image( true, bDefer );
					overlayLayer->applyColorMgmt = g_ColorMgmt.pending.enabled;

					overlayLayer->filter = GamescopeUpscaleFilter::NEAREST;
					// Partial composition stuff has the same colorspace.
					// So read that from the composite frame info
					overlayLayer->ctm = nullptr;
					overlayLayer->colorspace = compositeFrameInfo.layers[0].colorspace;
				}
				else
				{
					// Use whatever overlay we had last while waiting for the
					// partial composition to have anything queued.
					presentCompFrameInfo.applyOutputColorMgmt = g_ColorMgmt.pending.enabled;
					presentCompFrameInfo.layerCount = 1;

					presentCompFrameInfo.layers[ 0 ] = pFrameInfo->layers[ 0 ];
					presentCompFrameInfo.layers[ 0 ].zpos = g_zposBase;

					const FrameInfo_t::Layer_t *lastPresentedOverlayLayer = nullptr;
					for (int i = 0; i < pFrameInfo->layerCount; i++)
					{
						if ( pFrameInfo->layers[i].zpos == m_nLastSingleOverlayZPos )
						{
							lastPresentedOverlayLayer = &pFrameInfo->layers[i];
							break;
						}
					}

					if ( lastPresentedOverlayLayer )
					{
						FrameInfo_t::Layer_t *overlayLayer = &presentCompFrameInfo.layers[ 1 ];
						*overlayLayer = *lastPresentedOverlayLayer;
						overlayLayer->zpos = g_zposOverlay;

						presentCompFrameInfo.layerCount = 2;
					}
				}

				m_bWasPartialCompsiting = true;
			}

			int ret = drm_prepare( &g_DRM, bAsync, &presentCompFrameInfo );

			// Happens when we're VT-switched away
			if ( ret == -EACCES )
				return 0;

			if ( ret != 0 )
			{
				if ( g_DRM.current.mode_id == 0 )
				{
					xwm_log.errorf("We failed our modeset and have no mode to fall back to! (Initial modeset failed?): %s", strerror(-ret));
					abort();
				}

				xwm_log.errorf("Failed to prepare 1-layer flip (%s), trying again with previous mode if modeset needed", strerror( -ret ));

				// Try once again to in case we need to fall back to another mode.
				ret = drm_prepare( &g_DRM, bAsync, &compositeFrameInfo );

				// Happens when we're VT-switched away
				if ( ret == -EACCES )
					return 0;

				if ( ret != 0 )
				{
					xwm_log.errorf("Failed to prepare 1-layer flip entirely: %s", strerror( -ret ));
					// We should always handle a 1-layer flip, this used to abort,
					// but lets be more friendly and just avoid a commit and try again later.
					// Let's re-poll our state, and force grab the best connector again.
					//
					// Some intense connector hotplugging could be occuring and the
					// connector could become destroyed before we had a chance to use it
					// as we hadn't reffed it in a commit yet.
					this->DirtyState( true, false );
					this->PollState();
					return ret;
				}
			}

			return Commit( &compositeFrameInfo );
		}

		virtual void DirtyState( bool bForce, bool bForceModeset ) override
		{
			if ( bForceModeset )
				g_DRM.needs_modeset = true;
			g_DRM.out_of_date = std::max<int>( g_DRM.out_of_date, bForce ? 2 : 1 );
			g_DRM.paused = !wlsession_active();
		}

		virtual bool PollState() override
		{
			return drm_poll_state( &g_DRM );
		}

		virtual std::shared_ptr<BackendBlob> CreateBackendBlob( const std::type_info &type, std::span<const uint8_t> data ) override
		{
			uint32_t uBlob = 0;
			if ( type == typeid( glm::mat3x4 ) )
			{
				assert( data.size() == sizeof( glm::mat3x4 ) );

				drm_color_ctm2 ctm2;
				const float *pData = reinterpret_cast<const float *>( data.data() );
				for ( uint32_t i = 0; i < 12; i++ )
					ctm2.matrix[i] = drm_calc_s31_32( pData[i] );

				if ( drmModeCreatePropertyBlob( g_DRM.fd, reinterpret_cast<const void *>( &ctm2 ), sizeof( ctm2 ), &uBlob ) != 0 )
					return nullptr;
			}
			else
			{
				if ( drmModeCreatePropertyBlob( g_DRM.fd, data.data(), data.size(), &uBlob ) != 0 )
					return nullptr;
			}

			return std::make_shared<BackendBlob>( data, uBlob, true );
		}

		virtual OwningRc<IBackendFb> ImportDmabufToBackend( wlr_buffer *pBuffer, wlr_dmabuf_attributes *pDmaBuf ) override
		{
			return drm_fbid_from_dmabuf( &g_DRM, pBuffer, pDmaBuf );
		}

		virtual bool UsesModifiers() const override
		{
			return g_DRM.allow_modifiers;
		}
		virtual std::span<const uint64_t> GetSupportedModifiers( uint32_t uDrmFormat ) const override
		{
			const wlr_drm_format *pFormat = wlr_drm_format_set_get( &g_DRM.formats, uDrmFormat );
			if ( !pFormat )
				return std::span<const uint64_t>{};

			return std::span<const uint64_t>{ pFormat->modifiers, pFormat->modifiers + pFormat->len };
		}

		virtual IBackendConnector *GetCurrentConnector() override
		{
			return g_DRM.pConnector;
		}

		virtual IBackendConnector *GetConnector( GamescopeScreenType eScreenType ) override
		{
			if ( GetCurrentConnector() && GetCurrentConnector()->GetScreenType() == eScreenType )
				return GetCurrentConnector();

			if ( eScreenType == GAMESCOPE_SCREEN_TYPE_INTERNAL )
			{
				for ( auto &iter : g_DRM.connectors )
				{
					gamescope::CDRMConnector *pConnector = &iter.second;
					if ( pConnector->GetScreenType() == GAMESCOPE_SCREEN_TYPE_INTERNAL )
						return pConnector;
				}
			}

			return nullptr;
		}

		virtual bool IsVRRActive() const override
		{
			if ( !g_DRM.pCRTC || !g_DRM.pCRTC->GetProperties().VRR_ENABLED )
				return false;

			return !!g_DRM.pCRTC->GetProperties().VRR_ENABLED->GetCurrentValue();
		}

		virtual bool SupportsPlaneHardwareCursor() const override
		{
			return true;
		}

		virtual bool SupportsTearing() const override
		{
			return g_bSupportsAsyncFlips;
		}

		virtual bool UsesVulkanSwapchain() const override
		{
			return false;
		}

        virtual bool IsSessionBased() const override
		{
			return true;
		}

		virtual bool SupportsExplicitSync() const override
		{
#if __linux__
			auto [nMajor, nMinor, nPatch] = GetKernelVersion();
			
			// Only expose support on 6.8+ for eventfd fixes.
			if ( nMajor < 6 )
				return false;

			if ( nMajor == 6 && nMinor < 8 )
				return false;
#else
			// I don't know about this for FreeBSD, etc.
			return false;
#endif

			return g_bSupportsSyncObjs && !cv_drm_debug_disable_explicit_sync;
		}

		virtual bool IsVisible() const override
		{
			return !g_DRM.paused;
		}

		virtual glm::uvec2 CursorSurfaceSize( glm::uvec2 uvecSize ) const override
		{
			if ( !k_bUseCursorPlane )
				return uvecSize;

			return glm::uvec2{ g_DRM.cursor_width, g_DRM.cursor_height };
		}

		virtual bool HackTemporarySetDynamicRefresh( int nRefresh ) override
		{
			return drm_set_refresh( &g_DRM, nRefresh );
		}

		virtual void HackUpdatePatchedEdid() override
		{
			if ( !GetCurrentConnector() )
				return;

			WritePatchedEdid( GetCurrentConnector()->GetRawEDID(), GetCurrentConnector()->GetHDRInfo(), g_bRotated );
		}

	protected:

		virtual void OnBackendBlobDestroyed( BackendBlob *pBlob ) override
		{
			if ( pBlob->GetBlobValue() )
				drmModeDestroyPropertyBlob( g_DRM.fd, pBlob->GetBlobValue() );
		}

	private:
		bool m_bWasCompositing = false;
		bool m_bWasPartialCompsiting = false;
		int m_nLastSingleOverlayZPos = 0;

		uint32_t m_uNextPresentCtx = 0;
		DRMPresentCtx m_PresentCtxs[3];

		bool SupportsColorManagement() const
		{
			return drm_supports_color_mgmt( &g_DRM );
		}

		int Commit( const FrameInfo_t *pFrameInfo )
		{
			drm_t *drm = &g_DRM;
			int ret = 0;

			assert( drm->req != nullptr );

			defer( if ( drm->req != nullptr ) { drmModeAtomicFree( drm->req ); drm->req = nullptr; } );

			bool isPageFlip = drm->flags & DRM_MODE_PAGE_FLIP_EVENT;
			uint32_t uNewPendingFlipCount = 0;

			if ( isPageFlip )
			{
				uNewPendingFlipCount = ++drm->uPendingFlipCount;

				// Do it before the commit, as otherwise the pageflip handler could
				// potentially beat us to the refcount checks.

				// Swap over request FDs -> Queue
				std::unique_lock lock( drm->m_QueuedFbIdsMutex );
				drm->m_QueuedFbIds.swap( drm->m_FbIdsInRequest );
			}

			m_PresentFeedback.m_uQueuedPresents++;

			uint32_t uCurrentPresentCtx = m_uNextPresentCtx;
			m_uNextPresentCtx = ( m_uNextPresentCtx + 1 ) % 3;
			m_PresentCtxs[uCurrentPresentCtx].ulPendingFlipCount = m_PresentFeedback.m_uQueuedPresents;

			drm_log.debugf("flip commit %" PRIu64, (uint64_t)m_PresentFeedback.m_uQueuedPresents);
			gpuvis_trace_printf( "flip commit %" PRIu64, (uint64_t)m_PresentFeedback.m_uQueuedPresents );

			ret = drmModeAtomicCommit(drm->fd, drm->req, drm->flags, &m_PresentCtxs[uCurrentPresentCtx] );
			if ( ret != 0 )
			{
				drm_log.errorf_errno( "flip error" );

				if ( ret != -EBUSY && ret != -EACCES )
				{
					drm_log.errorf( "fatal flip error, aborting" );
					if ( isPageFlip )
						drm->uPendingFlipCount--;
					abort();
				}

				drm_rollback( drm );

				// Swap back over to what was previously queued (probably nothing)
				// if this commit failed.
				{
					std::unique_lock lock( drm->m_QueuedFbIdsMutex );
					drm->m_QueuedFbIds.swap( drm->m_FbIdsInRequest );
				}
				// Clear our refs.
				drm->m_FbIdsInRequest.clear();

				m_PresentFeedback.m_uQueuedPresents--;

				if ( isPageFlip )
					drm->uPendingFlipCount--;

				return ret;
			} else {
				// Our request went through!
				// Clear what we swapped with (what was previously queued)
				drm->m_FbIdsInRequest.clear();

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
			GetVBlankTimer().UpdateLastDrawTime( get_time_in_nanos() - g_SteamCompMgrVBlankTime.ulWakeupTime );

			if ( isPageFlip )
			{
				// Wait for bPendingFlip to change from true -> false.
				drm->uPendingFlipCount.wait( uNewPendingFlipCount );
				assert( drm->uPendingFlipCount == 0 );
			}

			return ret;
		}

	};

	/////////////////////////
	// Backend Instantiator
	/////////////////////////

	template <>
	bool IBackend::Set<CDRMBackend>()
	{
		return Set( new CDRMBackend{} );
	}
}
