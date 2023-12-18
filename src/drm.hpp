// DRM output stuff

#pragma once

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <assert.h>
#include <drm_fourcc.h>
#include <drm_mode.h>

#include <span>

#include "color_helpers.h"

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

enum drm_screen_type {
	DRM_SCREEN_TYPE_INTERNAL = 0,
	DRM_SCREEN_TYPE_EXTERNAL = 1,

	DRM_SCREEN_TYPE_COUNT
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

struct saved_mode {
	int width;
	int height;
	int refresh;
};

struct plane {
	uint32_t id;
	drmModePlane *plane;
	std::map<std::string, const drmModePropertyRes *> props;
	std::map<std::string, uint64_t> initial_prop_values;
	bool has_color_mgmt;
};

struct crtc {
	uint32_t id;
	drmModeCrtc *crtc;
	std::map<std::string, const drmModePropertyRes *> props;
	std::map<std::string, uint64_t> initial_prop_values;
	bool has_gamma_lut;
	bool has_degamma_lut;
	bool has_ctm;
	bool has_vrr_enabled;
	bool has_valve1_regamma_tf;

	struct {
		bool active;
	} current, pending;
};

struct connector_metadata_t {
   struct hdr_output_metadata defaultHdrMetadata = {};
   std::shared_ptr<wlserver_hdr_metadata> hdr10_metadata_blob;
   bool supportsST2084 = false;

   displaycolorimetry_t colorimetry = displaycolorimetry_709;
   EOTF eotf = EOTF_Gamma22;
};

struct connector {
	uint32_t id;
	char *name;
	drmModeConnector *connector;
	uint32_t possible_crtcs;
	std::map<std::string, const drmModePropertyRes *> props;
	std::map<std::string, uint64_t> initial_prop_values;

	char make_pnp[4];
	char *make;
	char *model;
	bool is_steam_deck_display;
	std::span<uint32_t> valid_display_rates{};
	uint16_t is_galileo_display;

	int target_refresh;
	bool vrr_capable;

	connector_metadata_t metadata;

	struct {
		uint32_t crtc_id;
		uint32_t colorspace;
		uint32_t content_type;
		std::shared_ptr<wlserver_hdr_metadata> hdr_output_metadata;
	} current, pending;

	std::vector<uint8_t> edid_data;

	bool has_colorspace;
	bool has_content_type;
	bool has_hdr_output_metadata;
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
	int fd;

	int preferred_width, preferred_height, preferred_refresh;

	uint64_t cursor_width, cursor_height;
	bool allow_modifiers;
	struct wlr_drm_format_set formats;

	std::vector< struct plane > planes;
	std::vector< struct crtc > crtcs;
	std::unordered_map< uint32_t, struct connector > connectors;

	std::map< uint32_t, drmModePropertyRes * > props;

	struct plane *primary;
	struct crtc *crtc;
	struct connector *connector;
	int crtc_index;
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
		uint32_t mode_id;
		uint32_t color_mgmt_serial;
		uint32_t lut3d_id[ EOTF_Count ];
		uint32_t shaperlut_id[ EOTF_Count ];
		enum drm_screen_type screen_type = DRM_SCREEN_TYPE_INTERNAL;
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

extern bool g_bUseLayers;
extern bool g_bRotated;
extern bool g_bFlipped;
extern bool g_bDebugLayers;
extern const char *g_sOutputName;

enum drm_mode_generation {
	DRM_MODE_GENERATE_CVT,
	DRM_MODE_GENERATE_FIXED,
};

enum g_panel_orientation {
	PANEL_ORIENTATION_0,	/* NORMAL */
	PANEL_ORIENTATION_270,	/* RIGHT */
	PANEL_ORIENTATION_90,	/* LEFT */
	PANEL_ORIENTATION_180,	/* UPSIDE DOWN */
	PANEL_ORIENTATION_AUTO,
};

extern enum drm_mode_generation g_drmModeGeneration;
extern enum g_panel_orientation g_drmModeOrientation;

extern std::atomic<uint64_t> g_drmEffectiveOrientation[DRM_SCREEN_TYPE_COUNT]; // DRM_MODE_ROTATE_*

extern bool g_bForceDisableColorMgmt;

bool init_drm(struct drm_t *drm, int width, int height, int refresh, bool wants_adaptive_sync);
void finish_drm(struct drm_t *drm);
int drm_commit(struct drm_t *drm, const struct FrameInfo_t *frameInfo );
int drm_prepare( struct drm_t *drm, bool async, const struct FrameInfo_t *frameInfo );
void drm_rollback( struct drm_t *drm );
bool drm_poll_state(struct drm_t *drm);
uint32_t drm_fbid_from_dmabuf( struct drm_t *drm, struct wlr_buffer *buf, struct wlr_dmabuf_attributes *dma_buf );
void drm_lock_fbid( struct drm_t *drm, uint32_t fbid );
void drm_unlock_fbid( struct drm_t *drm, uint32_t fbid );
void drm_drop_fbid( struct drm_t *drm, uint32_t fbid );
bool drm_set_connector( struct drm_t *drm, struct connector *conn );
bool drm_set_mode( struct drm_t *drm, const drmModeModeInfo *mode );
bool drm_set_refresh( struct drm_t *drm, int refresh );
bool drm_set_resolution( struct drm_t *drm, int width, int height );
bool drm_update_color_mgmt(struct drm_t *drm);
bool drm_update_vrr_state(struct drm_t *drm);
drm_screen_type drm_get_screen_type(struct drm_t *drm);

char *find_drm_node_by_devid(dev_t devid);
int drm_get_default_refresh(struct drm_t *drm);
bool drm_get_vrr_capable(struct drm_t *drm);
bool drm_supports_st2084(struct drm_t *drm);
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

std::span<uint32_t> drm_get_valid_refresh_rates( struct drm_t *drm );

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
