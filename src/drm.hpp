// DRM output stuff

#pragma once

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <assert.h>
#include <drm_fourcc.h>

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
};

struct crtc {
	uint32_t id;
	drmModeCrtc *crtc;
	std::map<std::string, const drmModePropertyRes *> props;
	std::map<std::string, uint64_t> initial_prop_values;
	bool has_gamma_lut;
	bool has_degamma_lut;
	bool has_ctm;

	struct {
		bool active;
	} current, pending;
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

	int target_refresh;

	struct {
		uint32_t crtc_id;
	} current, pending;
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

	struct {
		uint32_t mode_id;
		uint32_t gamma_lut_id;
		uint32_t degamma_lut_id;
		uint32_t ctm_id;
		float color_gain[3] = { 1.0f, 1.0f, 1.0f };
		float color_linear_gain[3] = { 1.0f, 1.0f, 1.0f };
		float color_gamma_exponent[DRM_SCREEN_TYPE_COUNT][3]   = { { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f } };
		float color_degamma_exponent[DRM_SCREEN_TYPE_COUNT][3] = { { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f } };
		float color_mtx[DRM_SCREEN_TYPE_COUNT][9] =
		{
			{
				1.0f, 0.0f, 0.0f,
				0.0f, 1.0f, 0.0f,
				0.0f, 0.0f, 1.0f,
			},

			{
				1.0f, 0.0f, 0.0f,
				0.0f, 1.0f, 0.0f,
				0.0f, 0.0f, 1.0f,
			},
		};
		float gain_blend = 0.0f;
		enum drm_screen_type screen_type = DRM_SCREEN_TYPE_INTERNAL;
	} current, pending;

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
};

extern struct drm_t g_DRM;

extern uint32_t g_nDRMFormat;

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
extern uint64_t g_drmEffectiveOrientation;

bool init_drm(struct drm_t *drm, int width, int height, int refresh);
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
bool drm_set_color_linear_gains(struct drm_t *drm, float *gains);
bool drm_set_color_gains(struct drm_t *drm, float *gains);
bool drm_set_color_mtx(struct drm_t *drm, float *mtx, enum drm_screen_type screen_type);
bool drm_set_color_gain_blend(struct drm_t *drm, float blend);
bool drm_update_gamma_lut(struct drm_t *drm);
bool drm_update_degamma_lut(struct drm_t *drm);
bool drm_update_color_mtx(struct drm_t *drm);
bool drm_set_gamma_exponent(struct drm_t *drm, float *vec, enum drm_screen_type screen_type);
bool drm_set_degamma_exponent(struct drm_t *drm, float *vec, enum drm_screen_type screen_type);
drm_screen_type drm_get_screen_type(struct drm_t *drm);

char *find_drm_node_by_devid(dev_t devid);
int drm_get_default_refresh(struct drm_t *drm);

extern bool g_bSupportsAsyncFlips;
