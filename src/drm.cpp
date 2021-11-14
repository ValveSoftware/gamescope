// DRM output stuff

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

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

#include "cvt.hpp"
#include "drm.hpp"
#include "main.hpp"
#include "vblankmanager.hpp"
#include "wlserver.hpp"
#include "log.hpp"

#include "gpuvis_trace_utils.h"

#include <algorithm>
#include <thread>

struct drm_t g_DRM = {};

uint32_t g_nDRMFormat = DRM_FORMAT_INVALID;
bool g_bRotated = false;

bool g_bUseLayers = true;
bool g_bDebugLayers = false;
const char *g_sOutputName = nullptr;

static LogScope drm_log("drm");
static LogScope drm_verbose_log("drm", LOG_SILENT);

static std::map< uint32_t, const char * > connector_types = {
	{ DRM_MODE_CONNECTOR_Unknown, "Unknown" },
	{ DRM_MODE_CONNECTOR_VGA, "VGA" },
	{ DRM_MODE_CONNECTOR_DVII, "DVI-I" },
	{ DRM_MODE_CONNECTOR_DVID, "DVI-D" },
	{ DRM_MODE_CONNECTOR_DVIA, "DVI-A" },
	{ DRM_MODE_CONNECTOR_Composite, "Composite" },
	{ DRM_MODE_CONNECTOR_SVIDEO, "SVIDEO" },
	{ DRM_MODE_CONNECTOR_LVDS, "LVDS" },
	{ DRM_MODE_CONNECTOR_Component, "Component" },
	{ DRM_MODE_CONNECTOR_9PinDIN, "DIN" },
	{ DRM_MODE_CONNECTOR_DisplayPort, "DP" },
	{ DRM_MODE_CONNECTOR_HDMIA, "HDMI-A" },
	{ DRM_MODE_CONNECTOR_HDMIB, "HDMI-B" },
	{ DRM_MODE_CONNECTOR_TV, "TV" },
	{ DRM_MODE_CONNECTOR_eDP, "eDP" },
	{ DRM_MODE_CONNECTOR_VIRTUAL, "Virtual" },
	{ DRM_MODE_CONNECTOR_DSI, "DSI" },
	{ DRM_MODE_CONNECTOR_DPI, "DPI" },
	{ DRM_MODE_CONNECTOR_WRITEBACK, "Writeback" },
	{ DRM_MODE_CONNECTOR_SPI, "SPI" },
#ifdef DRM_MODE_CONNECTOR_USB
	{ DRM_MODE_CONNECTOR_USB, "USB" },
#endif
};

static struct fb& get_fb( struct drm_t& drm, uint32_t id )
{
	std::lock_guard<std::mutex> m( drm.fb_map_mutex );
	return drm.fb_map[ id ];
}

static uint32_t get_connector_possible_crtcs(struct drm_t *drm, const drmModeConnector *connector) {
	uint32_t possible_crtcs = 0;

	for (int i = 0; i < connector->count_encoders; i++) {
		uint32_t encoder_id = connector->encoders[i];

		drmModeEncoder *encoder = drmModeGetEncoder(drm->fd, encoder_id);
		if (encoder == nullptr) {
			drm_log.errorf_errno("drmModeGetEncoder failed");
			continue;
		}

		possible_crtcs |= encoder->possible_crtcs;

		drmModeFreeEncoder(encoder);
	}

	return possible_crtcs;
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

static uint32_t pick_plane_format( const struct wlr_drm_format_set *formats )
{
	uint32_t result = DRM_FORMAT_INVALID;
	for ( size_t i = 0; i < formats->len; i++ ) {
		uint32_t fmt = formats->formats[i]->format;
		if ( fmt == DRM_FORMAT_XRGB8888 ) {
			// Prefer formats without alpha channel for main plane
			result = fmt;
		} else if ( result == DRM_FORMAT_INVALID && fmt == DRM_FORMAT_ARGB8888 ) {
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

static void drm_free_fb( struct drm_t *drm, struct fb *fb );

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, unsigned int crtc_id, void *data)
{
	uint64_t flipcount = (uint64_t)data;

	if ( g_DRM.crtc->id != crtc_id )
		return;

	vblank_mark_possible_vblank();

	// TODO: get the fbids_queued instance from data if we ever have more than one in flight

	drm_verbose_log.debugf("page_flip_handler %" PRIu64, flipcount);
	gpuvis_trace_printf("page_flip_handler %" PRIu64, flipcount);

	for ( uint32_t i = 0; i < g_DRM.fbids_on_screen.size(); i++ )
	{
		uint32_t previous_fbid = g_DRM.fbids_on_screen[ i ];
		assert( previous_fbid != 0 );

		struct fb &previous_fb = get_fb( g_DRM, previous_fbid );
		assert( previous_fb.n_refs > 0 );

		previous_fb.n_refs--;

		if ( previous_fb.n_refs == 0 )
		{
			// we flipped away from this previous fbid, now safe to delete
			std::lock_guard<std::mutex> lock( g_DRM.free_queue_lock );

			for ( uint32_t i = 0; i < g_DRM.fbid_free_queue.size(); i++ )
			{
				if ( g_DRM.fbid_free_queue[ i ] == previous_fbid )
				{
					drm_verbose_log.debugf("deferred free %u", previous_fbid);

					drm_free_fb( &g_DRM, &previous_fb );

					g_DRM.fbid_free_queue.erase( g_DRM.fbid_free_queue.begin() + i );
					break;
				}
			}
		}
	}

	g_DRM.fbids_on_screen = g_DRM.fbids_queued;
	g_DRM.fbids_queued.clear();

	g_DRM.flip_lock.unlock();
}

void flip_handler_thread_run(void)
{
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
	if (mode1.type & DRM_MODE_TYPE_PREFERRED)
		return true;
	if (mode2.type & DRM_MODE_TYPE_PREFERRED)
		return false;

	int area1 = mode1.hdisplay * mode1.vdisplay;
	int area2 = mode2.hdisplay * mode2.vdisplay;
	if (area1 != area2)
		return area1 > area2;

	return mode1.vrefresh > mode2.vrefresh;
}

static bool refresh_state( drm_t *drm )
{
	// TODO: refresh list of connectors for DP-MST

	for (size_t i = 0; i < drm->connectors.size(); i++) {
		struct connector *conn = &drm->connectors[i];
		if (!get_object_properties(drm, conn->id, DRM_MODE_OBJECT_CONNECTOR, conn->props, conn->initial_prop_values)) {
			return false;
		}

		if (conn->connector != nullptr)
			drmModeFreeConnector(conn->connector);

		conn->connector = drmModeGetConnector(drm->fd, conn->id);
		if (conn->connector == nullptr) {
			drm_log.errorf_errno("drmModeGetConnector failed");
			return false;
		}

		/* sort modes by preference: preferred flag, then highest area, then
		 * highest refresh rate */
		std::stable_sort(conn->connector->modes, conn->connector->modes + conn->connector->count_modes, compare_modes);
	}

	for (size_t i = 0; i < drm->crtcs.size(); i++) {
		struct crtc *crtc = &drm->crtcs[i];
		if (!get_object_properties(drm, crtc->id, DRM_MODE_OBJECT_CRTC, crtc->props, crtc->initial_prop_values)) {
			return false;
		}

		crtc->current.active = crtc->initial_prop_values["ACTIVE"];
	}

	for (size_t i = 0; i < drm->planes.size(); i++) {
		struct plane *plane = &drm->planes[i];
		if (!get_object_properties(drm, plane->id, DRM_MODE_OBJECT_PLANE, plane->props, plane->initial_prop_values)) {
			return false;
		}
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

	for (int i = 0; i < resources->count_connectors; i++) {
		struct connector conn = { .id = resources->connectors[i] };
		drm->connectors.push_back(conn);
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

	for (size_t i = 0; i < drm->connectors.size(); i++) {
		struct connector *conn = &drm->connectors[i];

		const char *type_str = "Unknown";
		if ( connector_types.count( conn->connector->connector_type ) > 0 )
			type_str = connector_types[ conn->connector->connector_type ];

		char name[128] = {};
		snprintf(name, sizeof(name), "%s-%d", type_str, conn->connector->connector_type_id);

		conn->name = strdup(name);

		conn->possible_crtcs = get_connector_possible_crtcs(drm, conn->connector);
	}

	for (size_t i = 0; i < drm->crtcs.size(); i++) {
		struct crtc *crtc = &drm->crtcs[i];
		crtc->pending = crtc->current;
	}

	return true;
}

static const drmModeModeInfo *find_mode( const drmModeConnector *connector, int hdisplay, int vdisplay, uint32_t vrefresh )
{
	for (int i = 0; i < connector->count_modes; i++) {
		const drmModeModeInfo *mode = &connector->modes[i];

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

static bool setup_best_connector(struct drm_t *drm)
{
	if (drm->connector && drm->connector->connector->connection != DRM_MODE_CONNECTED) {
		drm_log.infof("current connector '%s' disconnected", drm->connector->name);
		drm->connector = nullptr;
	}

	struct connector *best = nullptr;
	int best_priority = INT_MAX;
	for (size_t i = 0; i < drm->connectors.size(); i++) {
		struct connector *conn = &drm->connectors[i];

		if (conn->connector->connection != DRM_MODE_CONNECTED)
			continue;

		int priority = get_connector_priority(drm, conn->name);
		if (priority < best_priority) {
			best = conn;
			best_priority = priority;
		}
	}

	if ((!best && drm->connector) || (best && best == drm->connector)) {
		// Let's keep our current connector
		return true;
	}

	if ( best == nullptr ) {
		/* we could be fancy and listen for hotplug events and wait for
		 * a connector.. */
		drm_log.errorf("cannot find any connector!");
		return false;
	}

	if (!drm_set_connector(drm, best)) {
		return false;
	}

	const drmModeModeInfo *mode = nullptr;
	if ( drm->preferred_width != 0 || drm->preferred_height != 0 || drm->preferred_refresh != 0 )
	{
		mode = find_mode(best->connector, drm->preferred_width, drm->preferred_height, drm->preferred_refresh);
	}

	if (!mode) {
		mode = find_mode(best->connector, 0, 0, 0);
	}

	if (!mode) {
		drm_log.errorf("could not find mode!");
		return false;
	}

	if (!drm_set_mode(drm, mode)) {
		return false;
	}

	return true;
}

char *find_drm_node_by_devid(dev_t devid)
{
	// TODO: replace all of this with drmGetDeviceFromDevId once it's available

	drmDevice *devices[32];
	int devices_len = drmGetDevices2(0, devices, sizeof(devices) / sizeof(devices[0]));
	if (devices_len < 0) {
		drm_log.errorf_errno("drmGetDevices2 failed");
		return nullptr;
	}

	char *name = nullptr;
	for (int i = 0; i < devices_len; i++) {
		drmDevice *dev = devices[i];

		const int node_types[] = { DRM_NODE_PRIMARY, DRM_NODE_RENDER };
		for (size_t j = 0; j < sizeof(node_types) / sizeof(node_types[0]); j++) {
			int node = node_types[j];

			if (!(dev->available_nodes & (1 << node)))
				continue;

			struct stat dev_stat = {};
			if (stat(dev->nodes[node], &dev_stat) != 0) {
				drm_log.errorf_errno("stat(%s) failed", dev->nodes[node]);
				continue;
			}

			if (dev_stat.st_rdev == devid) {
				name = strdup(dev->nodes[node]);
				break;
			}
		}

		if (name != nullptr)
			break;
	}

	drmFreeDevices(devices, devices_len);
	return name;
}

bool init_drm(struct drm_t *drm, int width, int height, int refresh)
{
	drm->preferred_width = width;
	drm->preferred_height = height;
	drm->preferred_refresh = refresh;

	char *device_name = nullptr;
	if (g_vulkanHasDrmPrimaryDevId) {
		device_name = find_drm_node_by_devid(g_vulkanDrmPrimaryDevId);
		if (device_name == nullptr) {
			drm_log.errorf("Failed to find DRM device with device ID %" PRIu64, (uint64_t)g_vulkanDrmPrimaryDevId);
			return false;
		}
		drm_log.infof("opening DRM node '%s'", device_name);
	}
	else
	{
		drm_log.infof("warning: picking an arbitrary DRM device");
	}

	drm->fd = wlsession_open_kms( device_name );
	free(device_name);
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

	if (!get_resources(drm)) {
		return false;
	}

	drm->lo_device = liftoff_device_create( drm->fd );
	if ( drm->lo_device == nullptr )
		return false;
	if ( liftoff_device_register_all_planes( drm->lo_device ) < 0 )
		return false;

	drm_log.infof("Connectors:");
	for (size_t i = 0; i < drm->connectors.size(); i++) {
		struct connector *conn = &drm->connectors[i];

		const char *status_str = "disconnected";
		if ( conn->connector->connection == DRM_MODE_CONNECTED )
			status_str = "connected";

		drm_log.infof("  %s (%s)", conn->name, status_str);
	}

	drm->connector_priorities = parse_connector_priorities( g_sOutputName );

	if (!setup_best_connector(drm)) {
		return false;
	}

	// Fetch formats which can be scanned out
	for (size_t i = 0; i < drm->planes.size(); i++) {
		struct plane *plane = &drm->planes[i];
		if (!get_plane_formats(drm, plane, &drm->formats))
			return false;
	}

	if (!get_plane_formats(drm, drm->primary, &drm->primary_formats)) {
		return false;
	}

	g_nDRMFormat = pick_plane_format(&drm->primary_formats);
	if ( g_nDRMFormat == DRM_FORMAT_INVALID ) {
		drm_log.errorf("Primary plane doesn't support XRGB8888 nor ARGB8888");
		return false;
	}

	drm->kms_in_fence_fd = -1;

	std::thread flip_handler_thread( flip_handler_thread_run );
	flip_handler_thread.detach();

	if (g_bUseLayers) {
		liftoff_log_set_priority(g_bDebugLayers ? LIFTOFF_DEBUG : LIFTOFF_ERROR);
	}

	drm->flipcount = 0;
	drm->needs_modeset = true;

	return true;
}

static int add_property(drmModeAtomicReq *req, uint32_t obj_id, std::map<std::string, const drmModePropertyRes *> &props, const char *name, uint64_t value)
{
	if ( props.count( name ) == 0 )
	{
		drm_log.errorf("no property %s on object %u", name, obj_id);
		return -EINVAL;
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

void finish_drm(struct drm_t *drm)
{
	// Disable all connectors, CRTCs and planes. This is necessary to leave a
	// clean KMS state behind. Some other KMS clients might not support all of
	// the properties we use, e.g. "rotation" and Xorg don't play well
	// together.

	drmModeAtomicReq *req = drmModeAtomicAlloc();
	for ( size_t i = 0; i < drm->connectors.size(); i++ ) {
		add_connector_property(req, &drm->connectors[i], "CRTC_ID", 0);
	}
	for ( size_t i = 0; i < drm->crtcs.size(); i++ ) {
		add_crtc_property(req, &drm->crtcs[i], "MODE_ID", 0);
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
	}
	// We can't do a non-blocking commit here or else risk EBUSY in case the
	// previous page-flip is still in flight.
	uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
	int ret = drmModeAtomicCommit( drm->fd, req, flags, nullptr );
	if ( ret != 0 ) {
		drm_log.errorf_errno( "finish_drm: drmModeAtomicCommit failed" );
	}
	drmModeAtomicFree(req);

	// We can't close the DRM FD here, it might still be in use by the
	// page-flip handler thread.
}

int drm_commit(struct drm_t *drm, struct Composite_t *pComposite, struct VulkanPipeline_t *pPipeline )
{
	int ret;

	assert( drm->req != nullptr );

// 	if (drm->kms_in_fence_fd != -1) {
// 		add_plane_property(req, plane_id, "IN_FENCE_FD", drm->kms_in_fence_fd);
// 	}

// 	drm->kms_out_fence_fd = -1;

// 	add_crtc_property(req, drm->crtc_id, "OUT_FENCE_PTR",
// 					  (uint64_t)(unsigned long)&drm->kms_out_fence_fd);

	drm->flip_lock.lock();

	// Do it before the commit, as otherwise the pageflip handler could
	// potentially beat us to the refcount checks.
	for ( uint32_t i = 0; i < drm->fbids_in_req.size(); i++ )
	{
		struct fb &fb = get_fb( g_DRM, drm->fbids_in_req[ i ] );
		assert( fb.held == true );
		fb.n_refs++;
	}

	assert( drm->fbids_queued.size() == 0 );
	drm->fbids_queued = drm->fbids_in_req;

	g_DRM.flipcount++;

	drm_verbose_log.debugf("flip commit %" PRIu64, (uint64_t)g_DRM.flipcount);
	gpuvis_trace_printf( "flip commit %" PRIu64, (uint64_t)g_DRM.flipcount );

	ret = drmModeAtomicCommit(drm->fd, drm->req, drm->flags, (void*)(uint64_t)g_DRM.flipcount );
	if ( ret != 0 )
	{
		drm_log.errorf_errno( "flip error" );

		if ( ret != -EBUSY && ret != -EACCES )
		{
			exit( 1 );
		}

		drm->pending = drm->current;

		for ( size_t i = 0; i < drm->crtcs.size(); i++ )
		{
			drm->crtcs[i].pending = drm->crtcs[i].current;
		}

		// Undo refcount if the commit didn't actually work
		for ( uint32_t i = 0; i < drm->fbids_in_req.size(); i++ )
		{
			get_fb( g_DRM, drm->fbids_in_req[ i ] ).n_refs--;
		}

		drm->fbids_queued.clear();

		g_DRM.flipcount--;

		drm->flip_lock.unlock();

		goto out;
	} else {
		drm->fbids_in_req.clear();

		drm->current = drm->pending;

		for ( size_t i = 0; i < drm->crtcs.size(); i++ )
		{
			drm->crtcs[i].current = drm->crtcs[i].pending;
		}
	}

	// Wait for flip handler to unlock
	drm->flip_lock.lock();
	drm->flip_lock.unlock();

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

	if ( buf != nullptr )
	{
		wlserver_lock();
		buf = wlr_buffer_lock( buf );
		wlserver_unlock();
	}

	/* Nested scope so fb doesn't end up in the out: label */
	{
		struct fb &fb = get_fb( *drm, fb_id );
		assert( fb.held == false );
		fb.id = fb_id;
		fb.buf = buf;
		fb.held = true;
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

static void drm_free_fb( struct drm_t *drm, struct fb *fb )
{
	assert( !fb->held );
	assert( fb->n_refs == 0 );

	if ( drmModeRmFB( drm->fd, fb->id ) != 0 )
	{
		drm_log.errorf_errno( "drmModeRmFB failed" );
	}

	if ( fb->buf != nullptr )
	{
		wlserver_lock();
		wlr_buffer_unlock( fb->buf );
		wlserver_unlock();
	}

	fb = {};
}

void drm_drop_fbid( struct drm_t *drm, uint32_t fbid )
{
	struct fb &fb = get_fb( *drm, fbid );
	
	assert( fb.held == true );
	fb.held = false;

	if ( fb.n_refs == 0 )
	{
		/* FB isn't being used in any page-flip, free it immediately */
		drm_verbose_log.debugf("free fbid %u", fbid);
		drm_free_fb( drm, &fb );
	}
	else
	{
		std::lock_guard<std::mutex> lock( drm->free_queue_lock );

		drm->fbid_free_queue.push_back( fbid );
	}
}

/* Prepares an atomic commit without using libliftoff */
static int
drm_prepare_basic( struct drm_t *drm, const struct Composite_t *pComposite, const struct VulkanPipeline_t *pPipeline )
{
	// Discard cases where our non-liftoff path is known to fail

	// It only supports one layer
	if ( pComposite->nLayerCount > 1 )
	{
		drm_verbose_log.errorf("drm_prepare_basic: cannot handle %d layers", pComposite->nLayerCount);
		return -EINVAL;
	}

	if ( pPipeline->layerBindings[ 0 ].fbid == 0 )
	{
		drm_verbose_log.errorf("drm_prepare_basic: layer has no FB");
		return -EINVAL;
	}

	drmModeAtomicReq *req = drm->req;
	uint32_t fb_id = pPipeline->layerBindings[ 0 ].fbid;

	drm->fbids_in_req.push_back( fb_id );

	add_plane_property(req, drm->primary, "rotation", g_bRotated ? DRM_MODE_ROTATE_270 : DRM_MODE_ROTATE_0);

	add_plane_property(req, drm->primary, "FB_ID", fb_id);
	add_plane_property(req, drm->primary, "CRTC_ID", drm->crtc->id);
	add_plane_property(req, drm->primary, "SRC_X", 0);
	add_plane_property(req, drm->primary, "SRC_Y", 0);
	add_plane_property(req, drm->primary, "SRC_W", pPipeline->layerBindings[ 0 ].surfaceWidth << 16);
	add_plane_property(req, drm->primary, "SRC_H", pPipeline->layerBindings[ 0 ].surfaceHeight << 16);

	gpuvis_trace_printf ( "legacy flip fb_id %u src %ix%i", fb_id,
						 pPipeline->layerBindings[ 0 ].surfaceWidth,
						 pPipeline->layerBindings[ 0 ].surfaceHeight );

	int64_t crtcX = pComposite->data.vOffset[ 0 ].x * -1;
	int64_t crtcY = pComposite->data.vOffset[ 0 ].y * -1;
	int64_t crtcW = pPipeline->layerBindings[ 0 ].surfaceWidth / pComposite->data.vScale[ 0 ].x;
	int64_t crtcH = pPipeline->layerBindings[ 0 ].surfaceHeight / pComposite->data.vScale[ 0 ].y;

	if ( g_bRotated )
	{
		int64_t tmp = crtcX;
		crtcX = crtcY;
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

static int
drm_prepare_liftoff( struct drm_t *drm, const struct Composite_t *pComposite, const struct VulkanPipeline_t *pPipeline )
{
	for ( int i = 0; i < k_nMaxLayers; i++ )
	{
		if ( i < pComposite->nLayerCount )
		{
			if ( pPipeline->layerBindings[ i ].fbid == 0 )
			{
				drm_verbose_log.errorf("drm_prepare_liftoff: layer %d has no FB", i );
				return -EINVAL;
			}

			liftoff_layer_set_property( drm->lo_layers[ i ], "FB_ID", pPipeline->layerBindings[ i ].fbid);
			drm->fbids_in_req.push_back( pPipeline->layerBindings[ i ].fbid );

			liftoff_layer_set_property( drm->lo_layers[ i ], "zpos", pPipeline->layerBindings[ i ].zpos );
			liftoff_layer_set_property( drm->lo_layers[ i ], "alpha", pComposite->data.flOpacity[ i ] * 0xffff);

			if ( pPipeline->layerBindings[ i ].zpos == 0 )
			{
				assert( ( pComposite->data.flOpacity[ i ] * 0xffff ) == 0xffff );
			}

			const uint16_t srcWidth = pPipeline->layerBindings[ i ].surfaceWidth;
			const uint16_t srcHeight = pPipeline->layerBindings[ i ].surfaceHeight;

			liftoff_layer_set_property( drm->lo_layers[ i ], "SRC_X", 0);
			liftoff_layer_set_property( drm->lo_layers[ i ], "SRC_Y", 0);
			liftoff_layer_set_property( drm->lo_layers[ i ], "SRC_W", srcWidth << 16);
			liftoff_layer_set_property( drm->lo_layers[ i ], "SRC_H", srcHeight << 16);

			int32_t crtcX = -pComposite->data.vOffset[ i ].x;
			int32_t crtcY = -pComposite->data.vOffset[ i ].y;
			uint64_t crtcW = srcWidth / pComposite->data.vScale[ i ].x;
			uint64_t crtcH = srcHeight / pComposite->data.vScale[ i ].y;

			if (g_bRotated) {
				const int32_t x = crtcX;
				const uint64_t w = crtcW;
				crtcX = crtcY;
				crtcY = x;
				crtcW = crtcH;
				crtcH = w;
			}

			liftoff_layer_set_property( drm->lo_layers[ i ], "rotation", g_bRotated ? DRM_MODE_ROTATE_270 : DRM_MODE_ROTATE_0);

			liftoff_layer_set_property( drm->lo_layers[ i ], "CRTC_X", crtcX);
			liftoff_layer_set_property( drm->lo_layers[ i ], "CRTC_Y", crtcY);

			liftoff_layer_set_property( drm->lo_layers[ i ], "CRTC_W", crtcW);
			liftoff_layer_set_property( drm->lo_layers[ i ], "CRTC_H", crtcH);
		}
		else
		{
			liftoff_layer_set_property( drm->lo_layers[ i ], "FB_ID", 0 );
		}
	}

	int ret = liftoff_output_apply( drm->lo_output, drm->req, drm->flags );

	if ( ret == 0 )
	{
		// We don't support partial composition yet
		if ( liftoff_output_needs_composition( drm->lo_output ) )
			ret = -EINVAL;
	}

	if ( ret == 0 )
		drm_verbose_log.debugf( "can drm present %i layers", pComposite->nLayerCount );
	else
		drm_verbose_log.debugf( "can NOT drm present %i layers", pComposite->nLayerCount );

	return ret;
}

/* Prepares an atomic commit for the provided scene-graph. Returns false on
 * error or if the scene-graph can't be presented directly. */
int drm_prepare( struct drm_t *drm, const struct Composite_t *pComposite, const struct VulkanPipeline_t *pPipeline )
{
	drm->fbids_in_req.clear();

	bool needs_modeset = drm->needs_modeset.exchange(false);

	assert( drm->req == nullptr );
	drm->req = drmModeAtomicAlloc();

	uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK;

	// We do internal refcounting with these events
	flags |= DRM_MODE_PAGE_FLIP_EVENT;

	if ( needs_modeset ) {
		flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;

		// Disable all connectors and CRTCs

		for ( size_t i = 0; i < drm->connectors.size(); i++ ) {
			if ( add_connector_property( drm->req, &drm->connectors[i], "CRTC_ID", 0 ) < 0 )
				return false;
		}
		for ( size_t i = 0; i < drm->crtcs.size(); i++ ) {
			// We can't disable a CRTC if it's already disabled, or else the
			// kernel will error out with "requesting event but off".
			if (drm->crtcs[i].current.active == 0)
				continue;

			if (add_crtc_property(drm->req, &drm->crtcs[i], "MODE_ID", 0) < 0)
				return false;
			if (add_crtc_property(drm->req, &drm->crtcs[i], "ACTIVE", 0) < 0)
				return false;
			drm->crtcs[i].pending.active = 0;
		}

		// Then enable the one we've picked

		if (add_connector_property(drm->req, drm->connector, "CRTC_ID", drm->crtc->id) < 0)
			return false;

		if (add_crtc_property(drm->req, drm->crtc, "MODE_ID", drm->pending.mode_id) < 0)
			return false;
		if (add_crtc_property(drm->req, drm->crtc, "ACTIVE", 1) < 0)
			return false;
		drm->crtc->pending.active = 1;
	}

	drm->flags = flags;

	int ret;
	if ( g_bUseLayers == true ) {
		ret = drm_prepare_liftoff( drm, pComposite, pPipeline );
	} else {
		ret = drm_prepare_basic( drm, pComposite, pPipeline );
	}

	if ( ret != 0 ) {
		drmModeAtomicFree( drm->req );
		drm->req = nullptr;

		drm->fbids_in_req.clear();

		drm->pending = drm->current;

		for ( size_t i = 0; i < drm->crtcs.size(); i++ )
		{
			drm->crtcs[i].pending = drm->crtcs[i].current;
		}
	}

	return ret;
}

bool drm_poll_state( struct drm_t *drm )
{
	bool out_of_date = drm->out_of_date.exchange(false);
	if ( !out_of_date )
		return false;

	refresh_state( drm );

	setup_best_connector(drm);

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

bool drm_set_mode( struct drm_t *drm, const drmModeModeInfo *mode )
{
	uint32_t mode_id = 0;
	if (drmModeCreatePropertyBlob(drm->fd, mode, sizeof(*mode), &mode_id) != 0)
		return false;

	drm_log.infof("selecting mode %dx%d@%uHz", mode->hdisplay, mode->vdisplay, mode->vrefresh);

	drm->pending.mode_id = mode_id;
	drm->needs_modeset = true;

	g_nOutputWidth = mode->hdisplay;
	g_nOutputHeight = mode->vdisplay;
	g_nOutputRefresh = mode->vrefresh;

	// Auto-detect portrait mode
	g_bRotated = g_nOutputWidth < g_nOutputHeight;

	if ( g_bRotated )
	{
		g_nOutputWidth = mode->vdisplay;
		g_nOutputHeight = mode->hdisplay;
	}

	return true;
}

bool drm_set_refresh( struct drm_t *drm, int refresh )
{
	drmModeConnector *connector = drm->connector->connector;
	const drmModeModeInfo *existing_mode = find_mode(connector, g_nOutputWidth, g_nOutputHeight, refresh);
	drmModeModeInfo mode;
	if ( existing_mode )
	{
		mode = *existing_mode;
	}
	else
	{
		/* TODO: check refresh is within the EDID limits */
		generate_cvt_mode( &mode, g_nOutputWidth, g_nOutputHeight, refresh, true, false );
	}

	return drm_set_mode(drm, &mode);
}

bool drm_set_resolution( struct drm_t *drm, int width, int height )
{
	drmModeConnector *connector = drm->connector->connector;
	const drmModeModeInfo *mode = find_mode(connector, width, height, 0);
	if ( !mode )
	{
		return false;
	}

	return drm_set_mode(drm, mode);
}
