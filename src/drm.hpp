// DRM output stuff

#pragma once

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <assert.h>
#include <drm_fourcc.h>
#include <drm_mode.h>

#include <span>

#include "color_helpers.h"
#include "gamescope_shared.h"

// Josh: Okay whatever, this header isn't
// available for whatever stupid reason. :v
//#include <drm_color_mgmt.h>
enum drm_color_encoding {
	DRM_COLOR_YCBCR_BT601,
	DRM_COLOR_YCBCR_BT709,
	DRM_COLOR_YCBCR_BT2020,
	DRM_COLOR_ENCODING_MAX,
};

enum drm_color_range {
	DRM_COLOR_YCBCR_LIMITED_RANGE,
	DRM_COLOR_YCBCR_FULL_RANGE,
	DRM_COLOR_RANGE_MAX,
};

enum GamescopeAppTextureColorspace {
	GAMESCOPE_APP_TEXTURE_COLORSPACE_LINEAR = 0,
	GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB,
	GAMESCOPE_APP_TEXTURE_COLORSPACE_SCRGB,
	GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ,
	GAMESCOPE_APP_TEXTURE_COLORSPACE_PASSTHRU,
};
const uint32_t GamescopeAppTextureColorspace_Bits = 3;

inline bool ColorspaceIsHDR( GamescopeAppTextureColorspace colorspace )
{
	return colorspace == GAMESCOPE_APP_TEXTURE_COLORSPACE_SCRGB ||
		   colorspace == GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ;
}

extern "C"
{
	struct wl_resource;
}

extern struct drm_t g_DRM;
void drm_destroy_blob(struct drm_t *drm, uint32_t blob);

class drm_blob
{
public:
	drm_blob() : blob( 0 ), owned( false )
	{
	}

	drm_blob(uint32_t blob, bool owned = true)
		: blob( blob ), owned( owned )
	{
	}

	~drm_blob()
	{
		if (blob && owned)
			drm_destroy_blob( &g_DRM, blob );
	}

	// No copy constructor, because we can't duplicate the blob handle.
	drm_blob(const drm_blob&) = delete;
	drm_blob& operator=(const drm_blob&) = delete;
	// No move constructor, because we use shared_ptr anyway, but can be added if necessary.
	drm_blob(drm_blob&&) = delete;
	drm_blob& operator=(drm_blob&&) = delete;

	uint32_t blob;
	bool owned;
};

struct wlserver_hdr_metadata : drm_blob
{
	wlserver_hdr_metadata()
	{
	}

	wlserver_hdr_metadata(hdr_output_metadata* _metadata, uint32_t blob, bool owned = true)
		: drm_blob( blob, owned )
	{
		if (_metadata)
			this->metadata = *_metadata;
	}

	hdr_output_metadata metadata = {};
};

struct wlserver_ctm : drm_blob
{
	wlserver_ctm()
	{
	}

	wlserver_ctm(glm::mat3x4 ctm, uint32_t blob, bool owned = true)
		: drm_blob( blob, owned ), matrix( ctm )
	{
	}

	glm::mat3x4 matrix{};
};

#include <wayland-server-core.h>

extern "C" {
#include <libliftoff.h>
#include <wlr/render/dmabuf.h>
#include <wlr/render/drm_format_set.h>
}

#include "rendervulkan.hpp"

#include <unordered_map>
#include <utility>
#include <atomic>
#include <map>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <string>

namespace gamescope
{
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
			std::optional<CDRMAtomicProperty> VALVE1_PLANE_DEGAMMA_TF;
			std::optional<CDRMAtomicProperty> VALVE1_PLANE_DEGAMMA_LUT;
			std::optional<CDRMAtomicProperty> VALVE1_PLANE_CTM;
			std::optional<CDRMAtomicProperty> VALVE1_PLANE_HDR_MULT;
			std::optional<CDRMAtomicProperty> VALVE1_PLANE_SHAPER_LUT;
			std::optional<CDRMAtomicProperty> VALVE1_PLANE_SHAPER_TF;
			std::optional<CDRMAtomicProperty> VALVE1_PLANE_LUT3D;
			std::optional<CDRMAtomicProperty> VALVE1_PLANE_BLEND_TF;
			std::optional<CDRMAtomicProperty> VALVE1_PLANE_BLEND_LUT;
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
			std::optional<CDRMAtomicProperty> VALVE1_CRTC_REGAMMA_TF;
			std::optional<CDRMAtomicProperty> DUMMY_END;
		};
		      CRTCProperties &GetProperties()       { return m_Props; }
		const CRTCProperties &GetProperties() const { return m_Props; }
	private:
		CAutoDeletePtr<drmModeCrtc> m_pCRTC;
		uint32_t m_uCRTCMask = 0u;
		CRTCProperties m_Props;
	};

	class CDRMConnector final : public CDRMAtomicTypedObject<DRM_MODE_OBJECT_CONNECTOR>
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

		struct HDRInfo
		{
			// We still want to set up HDR info for Steam Deck LCD with some good
			// target/mapping values for the display brightness for undocking from a HDR display,
			// but don't want to expose HDR there as it is not good.
			bool bExposeHDRSupport = false;

			// The output encoding to use for HDR output.
			// For typical HDR10 displays, this will be PQ.
			// For displays doing "traditional HDR" such as Steam Deck OLED, this is Gamma 2.2.
			EOTF eOutputEncodingEOTF = EOTF_Gamma22;

			uint16_t uMaxContentLightLevel = 500;     // Nits
			uint16_t uMaxFrameAverageLuminance = 500; // Nits
			uint16_t uMinContentLightLevel = 0;       // Nits / 10000
			std::shared_ptr<wlserver_hdr_metadata> pDefaultMetadataBlob;

			bool ShouldPatchEDID() const
			{
				return bExposeHDRSupport && eOutputEncodingEOTF == EOTF_Gamma22;
			}

			bool SupportsHDR() const
			{
				// Note: Different to IsHDR10, as we can expose HDR10 on G2.2 displays
				// using LUTs and CTMs.
				return bExposeHDRSupport;
			}

			bool IsHDR10() const
			{
				// PQ output encoding is always HDR10 (PQ + 2020) for us.
				// If that assumption changes, update me.
				return eOutputEncodingEOTF == EOTF_PQ;
			}
		};

		drmModeConnector *GetModeConnector() { return m_pConnector.get(); }
		const char *GetName() const { return m_Mutable.szName; }
		const char *GetMake() const { return m_Mutable.pszMake; }
		const char *GetModel() const { return m_Mutable.szModel; }
		uint32_t GetPossibleCRTCMask() const { return m_Mutable.uPossibleCRTCMask; }
		const HDRInfo &GetHDRInfo() const { return m_Mutable.HDR; }
		std::span<const uint32_t> GetValidDynamicRefreshRates() const { return m_Mutable.ValidDynamicRefreshRates; }
		GamescopeKnownDisplays GetKnownDisplayType() const { return m_Mutable.eKnownDisplay; }
		GamescopeScreenType GetScreenType() const
		{
			if ( m_pConnector->connector_type == DRM_MODE_CONNECTOR_eDP ||
				 m_pConnector->connector_type == DRM_MODE_CONNECTOR_LVDS ||
				 m_pConnector->connector_type == DRM_MODE_CONNECTOR_DSI )
				return GAMESCOPE_SCREEN_TYPE_INTERNAL;

			return GAMESCOPE_SCREEN_TYPE_EXTERNAL;
		}
		bool IsVRRCapable() const
		{
			return this->GetProperties().vrr_capable && !!this->GetProperties().vrr_capable->GetCurrentValue();
		}
		const displaycolorimetry_t& GetDisplayColorimetry() const { return m_Mutable.DisplayColorimetry; }

		const std::vector<uint8_t> &GetRawEDID() const { return m_Mutable.EdidData; }

		// TODO: Remove
		void SetBaseRefresh( int nRefresh ) { m_nBaseRefresh = nRefresh; }
		int  GetBaseRefresh() const { return m_nBaseRefresh; }
	private:
		void ParseEDID();

		static std::optional<HDRInfo> GetKnownDisplayHDRInfo( GamescopeKnownDisplays eKnownDisplay );

		CAutoDeletePtr<drmModeConnector> m_pConnector;

		struct MutableConnectorState
		{
			int nDefaultRefresh = 0;

			uint32_t uPossibleCRTCMask = 0u;
			char szName[32]{};
			char szMakePNP[4]{};
			char szModel[16]{};
			const char *pszMake = ""; // Not owned, no free. This is a pointer to pnp db or szMakePNP.
			GamescopeKnownDisplays eKnownDisplay = GAMESCOPE_KNOWN_DISPLAY_UNKNOWN;
			std::span<const uint32_t> ValidDynamicRefreshRates{};
			std::vector<uint8_t> EdidData; // Raw, unmodified.

			displaycolorimetry_t DisplayColorimetry = displaycolorimetry_709;
			HDRInfo HDR;
		} m_Mutable;

		// TODO: Remove
		int m_nBaseRefresh = 0;

		ConnectorProperties m_Props;
	};
}

struct saved_mode {
	int width;
	int height;
	int refresh;
};

struct fb {
	uint32_t id;
	/* Client buffer, if any */
	struct wlr_buffer *buf;
	/* A FB is held if it's being used by steamcompmgr
	 * doesn't need to be atomic as it's only ever
	 * modified/read from the steamcompmgr thread */
	int held_refs;
	/* Number of page-flips using the FB */
	std::atomic< uint32_t > n_refs;
};

enum drm_valve1_transfer_function {
	DRM_VALVE1_TRANSFER_FUNCTION_DEFAULT,

	DRM_VALVE1_TRANSFER_FUNCTION_SRGB,
	DRM_VALVE1_TRANSFER_FUNCTION_BT709,
	DRM_VALVE1_TRANSFER_FUNCTION_PQ,
	DRM_VALVE1_TRANSFER_FUNCTION_LINEAR,
	DRM_VALVE1_TRANSFER_FUNCTION_UNITY,
	DRM_VALVE1_TRANSFER_FUNCTION_HLG,
	DRM_VALVE1_TRANSFER_FUNCTION_GAMMA22,
	DRM_VALVE1_TRANSFER_FUNCTION_GAMMA24,
	DRM_VALVE1_TRANSFER_FUNCTION_GAMMA26,
	DRM_VALVE1_TRANSFER_FUNCTION_MAX,
};

struct drm_t {
	bool bUseLiftoff;

	int fd;

	int preferred_width, preferred_height, preferred_refresh;

	uint64_t cursor_width, cursor_height;
	bool allow_modifiers;
	struct wlr_drm_format_set formats;

	std::vector< std::unique_ptr< gamescope::CDRMPlane > > planes;
	std::vector< std::unique_ptr< gamescope::CDRMCRTC > > crtcs;
	std::unordered_map< uint32_t, gamescope::CDRMConnector > connectors;

	std::map< uint32_t, drmModePropertyRes * > props;

	gamescope::CDRMPlane *pPrimaryPlane;
	gamescope::CDRMCRTC *pCRTC;
	gamescope::CDRMConnector *pConnector;
	int kms_in_fence_fd;
	int kms_out_fence_fd;

	struct wlr_drm_format_set primary_formats;

	drmModeAtomicReq *req;
	uint32_t flags;

	struct liftoff_device *lo_device;
	struct liftoff_output *lo_output;
	struct liftoff_layer *lo_layers[ k_nMaxLayers ];

	std::shared_ptr<wlserver_hdr_metadata> sdr_static_metadata;

	struct {
		std::shared_ptr<drm_blob> mode_id;
		uint32_t color_mgmt_serial;
		std::shared_ptr<drm_blob> lut3d_id[ EOTF_Count ];
		std::shared_ptr<drm_blob> shaperlut_id[ EOTF_Count ];
		// TODO: Remove me, this should be some generic setting.
		bool vrr_enabled = false;
		drm_valve1_transfer_function output_tf = DRM_VALVE1_TRANSFER_FUNCTION_DEFAULT;
	} current, pending;
	bool wants_vrr_enabled = false;

	/* FBs in the atomic request, but not yet submitted to KMS */
	std::vector < uint32_t > fbids_in_req;
	/* FBs submitted to KMS, but not yet displayed on screen */
	std::vector < uint32_t > fbids_queued;
	/* FBs currently on screen */
	std::vector < uint32_t > fbids_on_screen;

	std::unordered_map< uint32_t, struct fb > fb_map;
	std::mutex fb_map_mutex;

	std::mutex free_queue_lock;
	std::vector< uint32_t > fbid_unlock_queue;
	std::vector< uint32_t > fbid_free_queue;

	std::mutex flip_lock;

	std::atomic < uint64_t > flipcount;

	std::atomic < bool > paused;
	std::atomic < int > out_of_date;
	std::atomic < bool > needs_modeset;

	std::unordered_map< std::string, int > connector_priorities;

	bool force_internal = false;
	bool enable_hdr = false;

	char *device_name = nullptr;
};

extern struct drm_t g_DRM;

extern uint32_t g_nDRMFormat;
extern uint32_t g_nDRMFormatOverlay;

extern bool g_bRotated;
extern bool g_bFlipped;
extern bool g_bDebugLayers;
extern const char *g_sOutputName;

enum g_panel_orientation {
	PANEL_ORIENTATION_0,	/* NORMAL */
	PANEL_ORIENTATION_270,	/* RIGHT */
	PANEL_ORIENTATION_90,	/* LEFT */
	PANEL_ORIENTATION_180,	/* UPSIDE DOWN */
	PANEL_ORIENTATION_AUTO,
};

enum drm_panel_orientation {
	DRM_MODE_PANEL_ORIENTATION_UNKNOWN = -1,
	DRM_MODE_PANEL_ORIENTATION_NORMAL = 0,
	DRM_MODE_PANEL_ORIENTATION_BOTTOM_UP,
	DRM_MODE_PANEL_ORIENTATION_LEFT_UP,
	DRM_MODE_PANEL_ORIENTATION_RIGHT_UP,
};

extern gamescope::GamescopeModeGeneration g_eGamescopeModeGeneration;
extern enum g_panel_orientation g_drmModeOrientation;

extern std::atomic<uint64_t> g_drmEffectiveOrientation[gamescope::GAMESCOPE_SCREEN_TYPE_COUNT]; // DRM_MODE_ROTATE_*

extern bool g_bForceDisableColorMgmt;

bool init_drm(struct drm_t *drm, int width, int height, int refresh, bool wants_adaptive_sync);
void finish_drm(struct drm_t *drm);
int drm_commit(struct drm_t *drm, const struct FrameInfo_t *frameInfo );
int drm_prepare( struct drm_t *drm, bool async, const struct FrameInfo_t *frameInfo );
bool drm_poll_state(struct drm_t *drm);
uint32_t drm_fbid_from_dmabuf( struct drm_t *drm, struct wlr_buffer *buf, struct wlr_dmabuf_attributes *dma_buf );
void drm_lock_fbid( struct drm_t *drm, uint32_t fbid );
void drm_unlock_fbid( struct drm_t *drm, uint32_t fbid );
void drm_drop_fbid( struct drm_t *drm, uint32_t fbid );
bool drm_set_connector( struct drm_t *drm, gamescope::CDRMConnector *conn );
bool drm_set_mode( struct drm_t *drm, const drmModeModeInfo *mode );
bool drm_set_refresh( struct drm_t *drm, int refresh );
bool drm_set_resolution( struct drm_t *drm, int width, int height );
bool drm_update_color_mgmt(struct drm_t *drm);
bool drm_update_vrr_state(struct drm_t *drm);
gamescope::GamescopeScreenType drm_get_screen_type(struct drm_t *drm);

char *find_drm_node_by_devid(dev_t devid);
int drm_get_default_refresh(struct drm_t *drm);
bool drm_get_vrr_capable(struct drm_t *drm);
bool drm_supports_hdr(struct drm_t *drm, uint16_t *maxCLL = nullptr, uint16_t *maxFALL = nullptr);
void drm_set_vrr_enabled(struct drm_t *drm, bool enabled);
bool drm_get_vrr_in_use(struct drm_t *drm);
bool drm_supports_color_mgmt(struct drm_t *drm);
std::shared_ptr<wlserver_hdr_metadata> drm_create_hdr_metadata_blob(struct drm_t *drm, hdr_output_metadata *metadata);
std::shared_ptr<wlserver_ctm> drm_create_ctm(struct drm_t *drm, glm::mat3x4 ctm);
void drm_destroy_blob(struct drm_t *drm, uint32_t blob);

const char *drm_get_connector_name(struct drm_t *drm);
const char *drm_get_device_name(struct drm_t *drm);

std::pair<uint32_t, uint32_t> drm_get_connector_identifier(struct drm_t *drm);
void drm_set_hdr_state(struct drm_t *drm, bool enabled);

void drm_get_native_colorimetry( struct drm_t *drm,
	displaycolorimetry_t *displayColorimetry, EOTF *displayEOTF,
	displaycolorimetry_t *outputEncodingColorimetry, EOTF *outputEncodingEOTF );

std::span<const uint32_t> drm_get_valid_refresh_rates( struct drm_t *drm );

extern bool g_bSupportsAsyncFlips;

/* from CTA-861-G */
#define HDMI_EOTF_SDR 0
#define HDMI_EOTF_TRADITIONAL_HDR 1
#define HDMI_EOTF_ST2084 2
#define HDMI_EOTF_HLG 3

/* For Default case, driver will set the colorspace */
#define DRM_MODE_COLORIMETRY_DEFAULT			0
/* CEA 861 Normal Colorimetry options */
#define DRM_MODE_COLORIMETRY_NO_DATA			0
#define DRM_MODE_COLORIMETRY_SMPTE_170M_YCC		1
#define DRM_MODE_COLORIMETRY_BT709_YCC			2
/* CEA 861 Extended Colorimetry Options */
#define DRM_MODE_COLORIMETRY_XVYCC_601			3
#define DRM_MODE_COLORIMETRY_XVYCC_709			4
#define DRM_MODE_COLORIMETRY_SYCC_601			5
#define DRM_MODE_COLORIMETRY_OPYCC_601			6
#define DRM_MODE_COLORIMETRY_OPRGB			7
#define DRM_MODE_COLORIMETRY_BT2020_CYCC		8
#define DRM_MODE_COLORIMETRY_BT2020_RGB			9
#define DRM_MODE_COLORIMETRY_BT2020_YCC			10
/* Additional Colorimetry extension added as part of CTA 861.G */
#define DRM_MODE_COLORIMETRY_DCI_P3_RGB_D65		11
#define DRM_MODE_COLORIMETRY_DCI_P3_RGB_THEATER		12
/* Additional Colorimetry Options added for DP 1.4a VSC Colorimetry Format */
#define DRM_MODE_COLORIMETRY_RGB_WIDE_FIXED		13
#define DRM_MODE_COLORIMETRY_RGB_WIDE_FLOAT		14
#define DRM_MODE_COLORIMETRY_BT601_YCC			15

/* Content type options */
#define DRM_MODE_CONTENT_TYPE_NO_DATA		0
#define DRM_MODE_CONTENT_TYPE_GRAPHICS		1
#define DRM_MODE_CONTENT_TYPE_PHOTO		2
#define DRM_MODE_CONTENT_TYPE_CINEMA		3
#define DRM_MODE_CONTENT_TYPE_GAME		4

const char* drm_get_patched_edid_path();
void drm_update_patched_edid(drm_t *drm);

void drm_send_gamescope_control(wl_resource *control, struct drm_t *drm);
