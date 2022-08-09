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

struct drm_t g_DRM = {};

uint32_t g_nDRMFormat = DRM_FORMAT_INVALID;
bool g_bRotated = false;

bool g_bUseLayers = true;
bool g_bDebugLayers = false;
const char *g_sOutputName = nullptr;

enum drm_mode_generation g_drmModeGeneration = DRM_MODE_GENERATE_CVT;

static LogScope drm_log("drm");
static LogScope drm_verbose_log("drm", LOG_SILENT);

static std::map< std::string, std::string > pnps = {};

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

static void drm_unlock_fb_internal( struct drm_t *drm, struct fb *fb );

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, unsigned int crtc_id, void *data)
{
	uint64_t flipcount = (uint64_t)data;

	if ( g_DRM.crtc->id != crtc_id )
		return;

	// This is the last vblank time
	uint64_t vblanktime = sec * 1'000'000'000lu + usec * 1'000lu;
	vblank_mark_possible_vblank(vblanktime);

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

	if (blob->length < 128) {
		drm_log.errorf("Truncated EDID");
		return;
	}

	const uint8_t *data = (const uint8_t *) blob->data;

	// The ASCII 3-letter manufacturer PnP ID is encoded in 5-bit codes
	uint16_t id = (data[8] << 8) | data[9];
	char pnp_id[] = {
		(char)(((id >> 10) & 0x1F) + '@'),
		(char)(((id >> 5) & 0x1F) + '@'),
		(char)(((id >> 0) & 0x1F) + '@'),
		'\0',
	};
	memcpy(conn->make_pnp, pnp_id, sizeof(pnp_id));
	if (pnps.count(pnp_id) > 0) {
		conn->make = strdup(pnps[pnp_id].c_str());
	}

	for (size_t i = 72; i <= 108; i += 18) {
		uint16_t flag = (data[i] << 8) | data[i + 1];
		if (flag == 0 && data[i + 3] == 0xFC) {
			char model[14];
			snprintf(model, sizeof(model), "%.13s", &data[i + 5]);
			char *nl = strchr(model, '\n');
			if (nl) {
				*nl = '\0';
			}

			conn->model = strdup(model);
		}
	}

	drmModeFreePropertyBlob(blob);
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
		if (conn->connector != nullptr)
			drmModeFreeConnector(conn->connector);

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
			continue;

		const char *type_str = "Unknown";
		if ( connector_types.count( conn->connector->connector_type ) > 0 )
			type_str = connector_types[ conn->connector->connector_type ];

		char name[128] = {};
		snprintf(name, sizeof(name), "%s-%d", type_str, conn->connector->connector_type_id);
		conn->name = strdup(name);

		conn->possible_crtcs = get_connector_possible_crtcs(drm, conn->connector);

		drm_log.debugf("found new connector '%s'", conn->name);
	}

	for (size_t i = 0; i < drm->crtcs.size(); i++) {
		struct crtc *crtc = &drm->crtcs[i];
		if (!get_object_properties(drm, crtc->id, DRM_MODE_OBJECT_CRTC, crtc->props, crtc->initial_prop_values)) {
			return false;
		}

		crtc->has_gamma_lut = (crtc->props.find( "GAMMA_LUT" ) != crtc->props.end());
		if (!crtc->has_gamma_lut)
			drm_log.infof("CRTC %" PRIu32 " has no gamma LUT support", crtc->id);
		crtc->has_degamma_lut = (crtc->props.find( "DEGAMMA_LUT" ) != crtc->props.end());
		if (!crtc->has_degamma_lut)
			drm_log.infof("CRTC %" PRIu32 " has no degamma LUT support", crtc->id);
		crtc->has_ctm = (crtc->props.find( "CTM" ) != crtc->props.end());
		if (!crtc->has_ctm)
			drm_log.infof("CRTC %" PRIu32 " has no CTM support", crtc->id);

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
	for (auto &kv : drm->connectors) {
		struct connector *conn = &kv.second;

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

	char description[256];
	switch (best->connector->connector_type) {
	case DRM_MODE_CONNECTOR_LVDS:
	case DRM_MODE_CONNECTOR_eDP:
		snprintf(description, sizeof(description), "Internal screen");
		break;
	default:
		if (best->make && best->model) {
			snprintf(description, sizeof(description), "%s %s", best->make, best->model);
		} else if (best->model) {
			snprintf(description, sizeof(description), "%s", best->model);
		} else {
			snprintf(description, sizeof(description), "External screen");
		}
		break;
	}

	const struct wlserver_output_info wlserver_output_info = {
		.name = best->name,
		.description = description,
		.phys_width = (int) best->connector->mmWidth,
		.phys_height = (int) best->connector->mmHeight,
	};
	wlserver_set_output_info(&wlserver_output_info);

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

void load_pnps(void)
{
	// TODO: use hwdata's pkg-config file once they ship one
	const char *filename = "/usr/share/hwdata/pnp.ids";
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

bool init_drm(struct drm_t *drm, int width, int height, int refresh)
{
	load_pnps();

	drm->preferred_width = width;
	drm->preferred_height = height;
	drm->preferred_refresh = refresh;

	char *device_name = nullptr;
	dev_t dev_id = 0;
	if (vulkan_primary_dev_id(&dev_id)) {
		device_name = find_drm_node_by_devid(dev_id);
		if (device_name == nullptr) {
			drm_log.errorf("Failed to find DRM device with device ID %" PRIu64, (uint64_t)dev_id);
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
	for (const auto &kv : drm->connectors) {
		const struct connector *conn = &kv.second;

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
	}
	for ( size_t i = 0; i < drm->crtcs.size(); i++ ) {
		add_crtc_property(req, &drm->crtcs[i], "MODE_ID", 0);
		if ( drm->crtcs[i].has_gamma_lut )
			add_crtc_property(req, &drm->crtcs[i], "GAMMA_LUT", 0);
		if ( drm->crtcs[i].has_degamma_lut )
			add_crtc_property(req, &drm->crtcs[i], "DEGAMMA_LUT", 0);
		if ( drm->crtcs[i].has_ctm )
			add_crtc_property(req, &drm->crtcs[i], "CTM", 0);
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

	drm->flip_lock.lock();

	// Do it before the commit, as otherwise the pageflip handler could
	// potentially beat us to the refcount checks.
	for ( uint32_t i = 0; i < drm->fbids_in_req.size(); i++ )
	{
		struct fb &fb = get_fb( g_DRM, drm->fbids_in_req[ i ] );
		assert( fb.held_refs );
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
			drm_log.errorf( "fatal flip error, aborting" );
			drm->flip_lock.unlock();
			abort();
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
			if ( drm->pending.mode_id != drm->current.mode_id )
				drmModeDestroyPropertyBlob(drm->fd, drm->current.mode_id);
			if ( drm->pending.gamma_lut_id != drm->current.gamma_lut_id )
				drmModeDestroyPropertyBlob(drm->fd, drm->current.gamma_lut_id);
			if ( drm->pending.degamma_lut_id != drm->current.degamma_lut_id )
				drmModeDestroyPropertyBlob(drm->fd, drm->current.degamma_lut_id);
			drm->crtcs[i].current = drm->crtcs[i].pending;
		}
	}

	// Update the draw time
	// Ideally this would be updated by something right before the page flip
	// is queued and would end up being the new page flip, rather than here.
	// However, the page flip handler is called when the page flip occurs,
	// not when it is successfully queued.
	g_uVblankDrawTimeNS = get_time_in_nanos() - g_SteamCompMgrVBlankTime;

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

/* Prepares an atomic commit without using libliftoff */
static int
drm_prepare_basic( struct drm_t *drm, const struct FrameInfo_t *frameInfo )
{
	// Discard cases where our non-liftoff path is known to fail

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

	add_plane_property(req, drm->primary, "rotation", g_bRotated ? DRM_MODE_ROTATE_270 : DRM_MODE_ROTATE_0);

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

static int
drm_prepare_liftoff( struct drm_t *drm, const struct FrameInfo_t *frameInfo )
{
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

			liftoff_layer_set_property( drm->lo_layers[ i ], "zpos", frameInfo->layers[ i ].zpos );
			liftoff_layer_set_property( drm->lo_layers[ i ], "alpha", frameInfo->layers[ i ].opacity * 0xffff);

			const uint16_t srcWidth = frameInfo->layers[ i ].tex->width();
			const uint16_t srcHeight = frameInfo->layers[ i ].tex->height();

			liftoff_layer_set_property( drm->lo_layers[ i ], "SRC_X", 0);
			liftoff_layer_set_property( drm->lo_layers[ i ], "SRC_Y", 0);
			liftoff_layer_set_property( drm->lo_layers[ i ], "SRC_W", srcWidth << 16);
			liftoff_layer_set_property( drm->lo_layers[ i ], "SRC_H", srcHeight << 16);

			int32_t crtcX = -frameInfo->layers[ i ].offset.x;
			int32_t crtcY = -frameInfo->layers[ i ].offset.y;
			uint64_t crtcW = srcWidth / frameInfo->layers[ i ].scale.x;
			uint64_t crtcH = srcHeight / frameInfo->layers[ i ].scale.y;

			if (g_bRotated) {
				int64_t imageH = frameInfo->layers[ i ].tex->contentHeight() / frameInfo->layers[ i ].scale.y;

				const int32_t x = crtcX;
				const uint64_t w = crtcW;
				crtcX = g_nOutputHeight - imageH - crtcY;
				crtcY = x;
				crtcW = crtcH;
				crtcH = w;
			}

			liftoff_layer_set_property( drm->lo_layers[ i ], "rotation", g_bRotated ? DRM_MODE_ROTATE_270 : DRM_MODE_ROTATE_0);

			liftoff_layer_set_property( drm->lo_layers[ i ], "CRTC_X", crtcX);
			liftoff_layer_set_property( drm->lo_layers[ i ], "CRTC_Y", crtcY);

			liftoff_layer_set_property( drm->lo_layers[ i ], "CRTC_W", crtcW);
			liftoff_layer_set_property( drm->lo_layers[ i ], "CRTC_H", crtcH);

			if ( frameInfo->layers[i].isYcbcr() )
			{
				liftoff_layer_set_property( drm->lo_layers[ i ], "COLOR_ENCODING", drm_get_color_encoding( g_ForcedNV12ColorSpace ) );
				liftoff_layer_set_property( drm->lo_layers[ i ], "COLOR_RANGE", drm_get_color_range( g_ForcedNV12ColorSpace ) );
			}
			else
			{
				liftoff_layer_unset_property( drm->lo_layers[ i ], "COLOR_ENCODING" );
				liftoff_layer_unset_property( drm->lo_layers[ i ], "COLOR_RANGE" );
			}
		}
		else
		{
			liftoff_layer_set_property( drm->lo_layers[ i ], "FB_ID", 0 );

			liftoff_layer_unset_property( drm->lo_layers[ i ], "COLOR_ENCODING" );
			liftoff_layer_unset_property( drm->lo_layers[ i ], "COLOR_RANGE" );
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
		drm_verbose_log.debugf( "can drm present %i layers", frameInfo->layerCount );
	else
		drm_verbose_log.debugf( "can NOT drm present %i layers", frameInfo->layerCount );

	return ret;
}

/* Prepares an atomic commit for the provided scene-graph. Returns 0 on success,
 * negative errno on failure or if the scene-graph can't be presented directly. */
int drm_prepare( struct drm_t *drm, const struct FrameInfo_t *frameInfo )
{
	drm_update_gamma_lut(drm);
	drm_update_degamma_lut(drm);
	drm_update_color_mtx(drm);

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

		for ( auto &kv : drm->connectors ) {
			struct connector *conn = &kv.second;
			int ret = add_connector_property( drm->req, conn, "CRTC_ID", 0 );
			if (ret < 0)
				return ret;
		}
		for ( size_t i = 0; i < drm->crtcs.size(); i++ ) {
			struct crtc *crtc = &drm->crtcs[i];

			// We can't disable a CRTC if it's already disabled, or else the
			// kernel will error out with "requesting event but off".
			if (crtc->current.active == 0)
				continue;

			if (add_crtc_property(drm->req, crtc, "MODE_ID", 0) < 0)
				return false;
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

			int ret = add_crtc_property(drm->req, crtc, "ACTIVE", 0);
			if (ret < 0)
				return ret;
			crtc->pending.active = 0;
		}

		// Then enable the one we've picked

		int ret = add_connector_property(drm->req, drm->connector, "CRTC_ID", drm->crtc->id);
		if (ret < 0)
			return ret;

		ret = add_crtc_property(drm->req, drm->crtc, "MODE_ID", drm->pending.mode_id);
		if (ret < 0)
			return ret;

		if (drm->crtc->has_gamma_lut)
		{
			ret = add_crtc_property(drm->req, drm->crtc, "GAMMA_LUT", drm->pending.gamma_lut_id);
			if (ret < 0)
				return false;
		}

		if (drm->crtc->has_degamma_lut)
		{
			ret = add_crtc_property(drm->req, drm->crtc, "DEGAMMA_LUT", drm->pending.degamma_lut_id);
			if (ret < 0)
				return false;
		}

		if (drm->crtc->has_ctm)
		{
			ret = add_crtc_property(drm->req, drm->crtc, "CTM", drm->pending.ctm_id);
			if (ret < 0)
				return false;
		}

		ret = add_crtc_property(drm->req, drm->crtc, "ACTIVE", 1);
		if (ret < 0)
			return false;
		drm->crtc->pending.active = 1;
	}
	else
	{
		if ( drm->crtc->has_gamma_lut && drm->pending.gamma_lut_id != drm->current.gamma_lut_id )
		{
			int ret = add_crtc_property(drm->req, drm->crtc, "GAMMA_LUT", drm->pending.gamma_lut_id);
			if (ret < 0)
				return ret;
		}

		if ( drm->crtc->has_degamma_lut && drm->pending.degamma_lut_id != drm->current.degamma_lut_id )
		{
			int ret = add_crtc_property(drm->req, drm->crtc, "DEGAMMA_LUT", drm->pending.degamma_lut_id);
			if (ret < 0)
				return ret;
		}

		if ( drm->crtc->has_ctm && drm->pending.ctm_id != drm->current.ctm_id )
		{
			int ret = add_crtc_property(drm->req, drm->crtc, "CTM", drm->pending.ctm_id);
			if (ret < 0)
				return ret;
		}
	}

	drm->flags = flags;

	int ret;
	if ( g_bUseLayers == true ) {
		ret = drm_prepare_liftoff( drm, frameInfo );
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

inline float srgb_to_linear( float fVal )
{
    return ( fVal < 0.04045f ) ? fVal / 12.92f : std::pow( ( fVal + 0.055f) / 1.055f, 2.4f );
}

inline float linear_to_srgb( float fVal )
{
    return ( fVal < 0.0031308f ) ? fVal * 12.92f : std::pow( fVal, 1.0f / 2.4f ) * 1.055f - 0.055f;
}

inline int quantize( float fVal, float fMaxVal )
{
    return std::max( 0.f, std::min( fMaxVal, roundf( fVal * fMaxVal ) ) );
}

bool drm_set_color_gains(struct drm_t *drm, float *gains)
{
	for (int i = 0; i < 3; i++)
		drm->pending.color_gain[i] = gains[i];

	for (int i = 0; i < 3; i++)
	{
		if ( drm->current.color_gain[i] != drm->pending.color_gain[i] )
			return true;
	}
	return false;
}

bool drm_set_color_linear_gains(struct drm_t *drm, float *gains)
{
	for (int i = 0; i < 3; i++)
		drm->pending.color_linear_gain[i] = gains[i];

	for (int i = 0; i < 3; i++)
	{
		if ( drm->current.color_linear_gain[i] != drm->pending.color_linear_gain[i] )
			return true;
	}
	return false;
}

bool drm_set_color_mtx(struct drm_t *drm, float *mtx)
{
	for (int i = 0; i < 9; i++)
		drm->pending.color_mtx[i] = mtx[i];

	for (int i = 0; i < 9; i++)
	{
		if ( drm->current.color_mtx[i] != drm->pending.color_mtx[i] )
			return true;
	}
	return false;
}

bool drm_set_color_gain_blend(struct drm_t *drm, float blend)
{
	drm->pending.gain_blend = blend;
	if ( drm->current.gain_blend != drm->pending.gain_blend )
		return true;
	return false;
}

bool drm_set_gamma_exponent(struct drm_t *drm, float *vec)
{
	for (int i = 0; i < 3; i++)
		drm->pending.color_gamma_exponent[i] = vec[i];

	for (int i = 0; i < 3; i++)
	{
		if ( drm->current.color_gamma_exponent[i] != drm->pending.color_gamma_exponent[i] )
			return true;
	}
	return false;
}

bool drm_set_degamma_exponent(struct drm_t *drm, float *vec)
{
	for (int i = 0; i < 3; i++)
		drm->pending.color_degamma_exponent[i] = vec[i];

	for (int i = 0; i < 3; i++)
	{
		if ( drm->current.color_degamma_exponent[i] != drm->pending.color_degamma_exponent[i] )
			return true;
	}
	return false;
}

drm_screen_type drm_get_screen_type(struct drm_t *drm)
{
	if (!drm->connector || !drm->connector->connector)
		return DRM_SCREEN_TYPE_INTERNAL;

	if ( drm->connector->connector->connector_type == DRM_MODE_CONNECTOR_eDP ||
		 drm->connector->connector->connector_type == DRM_MODE_CONNECTOR_LVDS )
		 return DRM_SCREEN_TYPE_INTERNAL;

	return DRM_SCREEN_TYPE_EXTERNAL;
}

inline float lerp( float a, float b, float t )
{
    return a + t * (b - a);
}

inline uint16_t drm_quantize_lut_value( float flValue )
{
	return (uint16_t)quantize( flValue, (float)UINT16_MAX );
}

inline uint16_t drm_calc_lut_value( float input, float flLinearGain, float flGain, float flBlend )
{
    float flValue = lerp( flGain * input, linear_to_srgb( flLinearGain * srgb_to_linear( input ) ), flBlend );
	return drm_quantize_lut_value( flValue );
}

bool drm_update_color_mtx(struct drm_t *drm)
{
	if ( !drm->crtc->has_ctm )
		return true;

	static constexpr float g_identity_mtx[9] =
	{
		1.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 1.0f,
	};

	bool dirty = false;
	for (int i = 0; i < 9; i++)
	{
		if (drm->pending.color_mtx[i] != drm->current.color_mtx[i])
			dirty = true;
	}

	if (!dirty)
		return true;

	bool identity = true;
	for (int i = 0; i < 9; i++)
	{
		if (drm->pending.color_mtx[i] != g_identity_mtx[i])
			identity = false;
	}


	if (identity)
	{
		drm->pending.ctm_id = 0;
		return true;
	}

	struct drm_color_ctm drm_ctm;
	for (int i = 0; i < 9; i++)
	{
		const float val = drm->pending.color_mtx[i];

		// S31.32 sign-magnitude
		float integral;
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

		drm_ctm.matrix[i] = color.s31_32;
	}

	uint32_t blob_id = 0;	
	if (drmModeCreatePropertyBlob(drm->fd, &drm_ctm,
			sizeof(struct drm_color_ctm), &blob_id) != 0) {
		drm_log.errorf_errno("Unable to create CTM property blob");
		return false;
	}

	drm->pending.ctm_id = blob_id;
	return true;
}

static float safe_pow(float x, float y)
{
	// Avoids pow(x, 1.0f) != x.
	if (y == 1.0f)
		return x;

	return pow(x, y);
}

bool drm_update_gamma_lut(struct drm_t *drm)
{
	if ( !drm->crtc->has_gamma_lut )
		return true;

	if (drm->pending.color_gain[0] == drm->current.color_gain[0] &&
		drm->pending.color_gain[1] == drm->current.color_gain[1] &&
		drm->pending.color_gain[2] == drm->current.color_gain[2] &&
		drm->pending.color_linear_gain[0] == drm->current.color_linear_gain[0] &&
		drm->pending.color_linear_gain[1] == drm->current.color_linear_gain[1] &&
		drm->pending.color_linear_gain[2] == drm->current.color_linear_gain[2] &&
		drm->pending.color_gamma_exponent[0] == drm->current.color_gamma_exponent[0] &&
		drm->pending.color_gamma_exponent[1] == drm->current.color_gamma_exponent[1] &&
		drm->pending.color_gamma_exponent[2] == drm->current.color_gamma_exponent[2] &&
		drm->pending.gain_blend == drm->current.gain_blend )
	{
		return true;
	}

	bool color_gain_identity = drm->pending.gain_blend == 1.0f ||
		( drm->pending.color_gain[0] == 1.0f &&
		  drm->pending.color_gain[1] == 1.0f &&
		  drm->pending.color_gain[2] == 1.0f );

	bool linear_gain_identity = drm->pending.gain_blend == 0.0f ||
		( drm->pending.color_linear_gain[0] == 1.0f &&
		  drm->pending.color_linear_gain[1] == 1.0f &&
		  drm->pending.color_linear_gain[2] == 1.0f );

	bool gamma_exponent_identity =
		( drm->pending.color_gamma_exponent[0] == 1.0f &&
		  drm->pending.color_gamma_exponent[1] == 1.0f &&
		  drm->pending.color_gamma_exponent[2] == 1.0f );

	if ( color_gain_identity && linear_gain_identity && gamma_exponent_identity )
	{
		drm->pending.gamma_lut_id = 0;
		return true;
	}

	const int lut_entries = drm->crtc->initial_prop_values["GAMMA_LUT_SIZE"];
	drm_color_lut *gamma_lut = new drm_color_lut[lut_entries];
	for ( int i = 0; i < lut_entries; i++ )
	{
        float input = float(i) / float(lut_entries - 1);

		float r_exp = safe_pow( input, drm->pending.color_gamma_exponent[0] );
		float g_exp = safe_pow( input, drm->pending.color_gamma_exponent[1] );
		float b_exp = safe_pow( input, drm->pending.color_gamma_exponent[2] );

		gamma_lut[i].red   = drm_calc_lut_value( r_exp, drm->pending.color_linear_gain[0], drm->pending.color_gain[0], drm->pending.gain_blend );
		gamma_lut[i].green = drm_calc_lut_value( g_exp, drm->pending.color_linear_gain[1], drm->pending.color_gain[1], drm->pending.gain_blend );
		gamma_lut[i].blue  = drm_calc_lut_value( b_exp, drm->pending.color_linear_gain[2], drm->pending.color_gain[2], drm->pending.gain_blend );
	}

	uint32_t blob_id = 0;	
	if (drmModeCreatePropertyBlob(drm->fd, gamma_lut,
			lut_entries * sizeof(struct drm_color_lut), &blob_id) != 0) {
		drm_log.errorf_errno("Unable to create gamma LUT property blob");
		delete[] gamma_lut;
		return false;
	}
	delete[] gamma_lut;

	drm->pending.gamma_lut_id = blob_id;

	return true;
}

bool drm_update_degamma_lut(struct drm_t *drm)
{
	if ( !drm->crtc->has_degamma_lut )
		return true;

	if (drm->pending.color_degamma_exponent[0] == drm->current.color_degamma_exponent[0] &&
		drm->pending.color_degamma_exponent[1] == drm->current.color_degamma_exponent[1] &&
		drm->pending.color_degamma_exponent[2] == drm->current.color_degamma_exponent[2])
	{
		return true;
	}

	bool degamma_exponent_identity =
		( drm->pending.color_degamma_exponent[0] == 1.0f &&
		  drm->pending.color_degamma_exponent[1] == 1.0f &&
		  drm->pending.color_degamma_exponent[2] == 1.0f );

	if ( degamma_exponent_identity )
	{
		drm->pending.degamma_lut_id = 0;
		return true;
	}

	const int lut_entries = drm->crtc->initial_prop_values["DEGAMMA_LUT_SIZE"];
	drm_color_lut *degamma_lut = new drm_color_lut[lut_entries];
	for ( int i = 0; i < lut_entries; i++ )
	{
        float input = float(i) / float(lut_entries - 1);

		degamma_lut[i].red   = drm_quantize_lut_value( safe_pow( input, drm->pending.color_degamma_exponent[0] ) );
		degamma_lut[i].green = drm_quantize_lut_value( safe_pow( input, drm->pending.color_degamma_exponent[1] ) );
		degamma_lut[i].blue  = drm_quantize_lut_value( safe_pow( input, drm->pending.color_degamma_exponent[2] ) );
	}

	uint32_t blob_id = 0;	
	if (drmModeCreatePropertyBlob(drm->fd, degamma_lut,
			lut_entries * sizeof(struct drm_color_lut), &blob_id) != 0) {
		drm_log.errorf_errno("Unable to create degamma LUT property blob");
		delete[] degamma_lut;
		return false;
	}
	delete[] degamma_lut;

	drm->pending.degamma_lut_id = blob_id;

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
	int width = g_nOutputWidth;
	int height = g_nOutputHeight;
	if ( g_bRotated ) {
		int tmp = width;
		width = height;
		height = tmp;
	}

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
				const char *make_pnp = drm->connector->make_pnp;
				const char *model = drm->connector->model;
				bool is_steam_deck_display =
					(strcmp(make_pnp, "WLC") == 0 && strcmp(model, "ANX7530 U") == 0) ||
					(strcmp(make_pnp, "ANX") == 0 && strcmp(model, "ANX7530 U") == 0) ||
					(strcmp(make_pnp, "VLV") == 0 && strcmp(model, "ANX7530 U") == 0) ||
					(strcmp(make_pnp, "VLV") == 0 && strcmp(model, "Jupiter") == 0);

				const drmModeModeInfo *preferred_mode = find_mode(connector, 0, 0, 0);
				generate_fixed_mode( &mode, preferred_mode, refresh, is_steam_deck_display );
				break;
			}
		}
	}

	mode.type = DRM_MODE_TYPE_USERDEF;

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

int drm_get_default_refresh(struct drm_t *drm)
{
	if ( drm->preferred_refresh )
		return drm->preferred_refresh;

	if ( drm->connector && drm->connector->connector )
	{
		drmModeConnector *connector = drm->connector->connector;
		const drmModeModeInfo *mode = find_mode( connector, g_nOutputWidth, g_nOutputHeight, 0);
		if ( mode )
			return mode->vrefresh;
	}

	return 60;
}
