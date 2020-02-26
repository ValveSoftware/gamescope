// DRM output stuff

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/select.h>
#include <signal.h>

#include "drm.hpp"
#include "main.hpp"
#include "vblankmanager.hpp"

#include "gpuvis_trace_utils.h"

#include <thread>

struct drm_t g_DRM;

uint32_t g_nDRMFormat;
bool g_bRotated;

bool g_bUseLayers;
bool g_bDebugLayers;

static int s_drm_log = 0;

static uint32_t find_crtc_for_encoder(const drmModeRes *resources,
		const drmModeEncoder *encoder) {
	int i;

	for (i = 0; i < resources->count_crtcs; i++) {
		/* possible_crtcs is a bitmask as described here:
		 * https://dvdhrm.wordpress.com/2012/09/13/linux-drm-mode-setting-api
		 */
		const uint32_t crtc_mask = 1 << i;
		const uint32_t crtc_id = resources->crtcs[i];
		if (encoder->possible_crtcs & crtc_mask) {
			return crtc_id;
		}
	}

	/* no match found */
	return -1;
}

static uint32_t find_crtc_for_connector(const struct drm_t *drm, const drmModeRes *resources,
		const drmModeConnector *connector) {
	int i;

	for (i = 0; i < connector->count_encoders; i++) {
		const uint32_t encoder_id = connector->encoders[i];
		drmModeEncoder *encoder = drmModeGetEncoder(drm->fd, encoder_id);

		if (encoder) {
			const uint32_t crtc_id = find_crtc_for_encoder(resources, encoder);

			drmModeFreeEncoder(encoder);
			if (crtc_id != 0) {
				return crtc_id;
			}
		}
	}

	/* no match found */
	return -1;
}

static int get_resources(int fd, drmModeRes **resources)
{
	*resources = drmModeGetResources(fd);
	if (*resources == NULL)
		return -1;
	return 0;
}

#define MAX_DRM_DEVICES 64

static int find_drm_device(drmModeRes **resources)
{
	drmDevicePtr devices[MAX_DRM_DEVICES] = { NULL };
	int num_devices, fd = -1;
	
	num_devices = drmGetDevices2(0, devices, MAX_DRM_DEVICES);
	if (num_devices < 0) {
		printf("drmGetDevices2 failed: %s\n", strerror(-num_devices));
		return -1;
	}
	
	for (int i = 0; i < num_devices; i++) {
		drmDevicePtr device = devices[i];
		int ret;
		
		if (!(device->available_nodes & (1 << DRM_NODE_PRIMARY)))
			continue;
		/* OK, it's a primary device. If we can get the
		 * drmModeResources, it means it's also a
		 * KMS-capable device.
		 */
		fd = open(device->nodes[DRM_NODE_PRIMARY], O_RDWR);
		if (fd < 0)
			continue;
		ret = get_resources(fd, resources);
		if (!ret)
			break;
		close(fd);
		fd = -1;
	}
	drmFreeDevices(devices, num_devices);
	
	if (fd < 0)
		printf("no drm device found!\n");
	return fd;
}

/* Pick a plane.. something that at a minimum can be connected to
 * the chosen crtc, but prefer primary plane.
 *
 * Seems like there is some room for a drmModeObjectGetNamedProperty()
 * type helper in libdrm..
 */
static int get_plane_id(struct drm_t *drm)
{
	drmModePlaneResPtr plane_resources;
	uint32_t i, j;
	int ret = -EINVAL;
	int found_primary = 0;
	
	plane_resources = drmModeGetPlaneResources(drm->fd);
	if (!plane_resources) {
		printf("drmModeGetPlaneResources failed: %s\n", strerror(errno));
		return -1;
	}
	
	for (i = 0; (i < plane_resources->count_planes) && !found_primary; i++) {
		uint32_t id = plane_resources->planes[i];
		drmModePlanePtr plane = drmModeGetPlane(drm->fd, id);
		if (!plane) {
			printf("drmModeGetPlane(%u) failed: %s\n", id, strerror(errno));
			continue;
		}
		
		if (plane->possible_crtcs & (1 << drm->crtc_index)) {
			drmModeObjectPropertiesPtr props =
			drmModeObjectGetProperties(drm->fd, id, DRM_MODE_OBJECT_PLANE);
			
			/* primary or not, this plane is good enough to use: */
			ret = id;
			
			for (j = 0; j < props->count_props; j++) {
				drmModePropertyPtr p =
				drmModeGetProperty(drm->fd, props->props[j]);
				
				if ((strcmp(p->name, "type") == 0) &&
					(props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY)) {
					/* found our primary plane, lets use that: */
					
					for (uint32_t k = 0; k < plane->count_formats; k++)
					{
						uint32_t fmt = plane->formats[k];
						if (fmt == DRM_FORMAT_XRGB8888) {
							// Prefer formats without alpha channel for main plane
							g_nDRMFormat = fmt;
							break;
						} else if (fmt == DRM_FORMAT_ARGB8888) {
							g_nDRMFormat = fmt;
						}
					}
					
					found_primary = 1;
					}
					
					drmModeFreeProperty(p);
			}
			
			drmModeFreeObjectProperties(props);
		}
		
		drmModeFreePlane(plane);
	}
	
	drmModeFreePlaneResources(plane_resources);
	
	return ret;
}

static void page_flip_handler(int fd, unsigned int frame,
							  unsigned int sec, unsigned int usec, void *data)
{
	vblank_mark_possible_vblank();

	// TODO: get the fbids_in_req instance from data if we ever have more than one in flight
	
	if ( s_drm_log != 0 )
	{
		printf("page_flip_handler %p\n", data);
	}
	gpuvis_trace_printf("page_flip_handler %p\n", data);
	
	for ( uint32_t i = 0; i < g_DRM.fbids_on_screen.size(); i++ )
	{
		uint32_t previous_fbid = g_DRM.fbids_on_screen[ i ];
		assert( previous_fbid != 0 );
		assert( g_DRM.map_fbid_inflightflips[ previous_fbid ].second > 0 );
		
		g_DRM.map_fbid_inflightflips[ previous_fbid ].second--;
		
		if ( g_DRM.map_fbid_inflightflips[ previous_fbid ].second == 0 )
		{
			// we flipped away from this previous fbid, now safe to delete
			std::lock_guard<std::mutex> lock( g_DRM.free_queue_lock );
			
			for ( uint32_t i = 0; i < g_DRM.fbid_free_queue.size(); i++ )
			{
				if ( g_DRM.fbid_free_queue[ i ] == previous_fbid )
				{
					if ( s_drm_log != 0 )
					{
						printf("deferred free %u\n", previous_fbid);
					}
					drmModeRmFB( g_DRM.fd, previous_fbid );
					
					g_DRM.fbid_free_queue.erase( g_DRM.fbid_free_queue.begin() + i );
					break;
				}
			}
		}
	}
	
	g_DRM.fbids_on_screen.clear();
	
	for ( uint32_t i = 0; i < g_DRM.fbids_in_req.size(); i++ )
	{
		g_DRM.fbids_on_screen.push_back( g_DRM.fbids_in_req[ i ] );
	}
	
	g_DRM.fbids_in_req.clear();
	
	g_DRM.flip_lock.unlock();
}

void flip_handler_thread_run(void)
{
	// see wlroots xwayland startup and how wl_event_loop_add_signal works
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR1);
	sigprocmask(SIG_BLOCK, &mask, NULL);

	fd_set fds;
	int ret;
	drmEventContext evctx = {
		.version = 2,
		.page_flip_handler = page_flip_handler,
	};

	FD_ZERO(&fds);
	FD_SET(0, &fds);
	FD_SET(g_DRM.fd, &fds);
	
	while ( true )
	{
		ret = select(g_DRM.fd + 1, &fds, NULL, NULL, NULL);
		if (ret < 0) {
			break;
		}
		drmHandleEvent(g_DRM.fd, &evctx);
	}
}



int init_drm(struct drm_t *drm, const char *device, const char *mode_str, unsigned int vrefresh)
{
	drmModeRes *resources;
	drmModeConnector *connector = NULL;
	drmModeEncoder *encoder = NULL;
	int i, ret, area;
	
	if (device) {
		drm->fd = open(device, O_RDWR);
		ret = get_resources(drm->fd, &resources);
		if (ret < 0 && errno == EOPNOTSUPP)
			printf("%s does not look like a modeset device\n", device);
	} else {
		drm->fd = find_drm_device(&resources);
	}
	
	if (drm->fd < 0) {
		printf("could not open drm device\n");
		return -1;
	}
	
	if (!resources) {
		printf("drmModeGetResources failed: %s\n", strerror(errno));
		return -1;
	}
	
	/* find a connected connector: */
	for (i = 0; i < resources->count_connectors; i++) {
		connector = drmModeGetConnector(drm->fd, resources->connectors[i]);
		if (connector->connection == DRM_MODE_CONNECTED) {
			/* it's connected, let's use this! */
			break;
		}
		drmModeFreeConnector(connector);
		connector = NULL;
	}
	
	if (!connector) {
		/* we could be fancy and listen for hotplug events and wait for
		 * a connector..
		 */
		printf("no connected connector!\n");
		return -1;
	}
	
	/* find user requested mode: */
	if (mode_str && *mode_str) {
		for (i = 0; i < connector->count_modes; i++) {
			drmModeModeInfo *current_mode = &connector->modes[i];
			
			if (strcmp(current_mode->name, mode_str) == 0) {
				if (vrefresh == 0 || current_mode->vrefresh == vrefresh) {
					drm->mode = current_mode;
					break;
				}
			}
		}
		if (!drm->mode)
			printf("requested mode not found, using default mode!\n");
	}
	
	/* find preferred mode or the highest resolution mode: */
	if (!drm->mode) {
		for (i = 0, area = 0; i < connector->count_modes; i++) {
			drmModeModeInfo *current_mode = &connector->modes[i];
			
			if (current_mode->type & DRM_MODE_TYPE_PREFERRED) {
				drm->mode = current_mode;
				break;
			}
			
			int current_area = current_mode->hdisplay * current_mode->vdisplay;
			if (current_area > area) {
				drm->mode = current_mode;
				area = current_area;
			}
		}
	}
	
	if (!drm->mode) {
		printf("could not find mode!\n");
		return -1;
	}
	
	/* find encoder: */
	for (i = 0; i < resources->count_encoders; i++) {
		encoder = drmModeGetEncoder(drm->fd, resources->encoders[i]);
		if (encoder->encoder_id == connector->encoder_id)
			break;
		drmModeFreeEncoder(encoder);
		encoder = NULL;
	}
	
	if (encoder) {
		drm->crtc_id = encoder->crtc_id;
	} else {
		uint32_t crtc_id = find_crtc_for_connector(drm, resources, connector);
		if (crtc_id == 0) {
			printf("no crtc found!\n");
			return -1;
		}
		
		drm->crtc_id = crtc_id;
	}
	
	for (i = 0; i < resources->count_crtcs; i++) {
		if (resources->crtcs[i] == drm->crtc_id) {
			drm->crtc_index = i;
			break;
		}
	}
	
	drmModeFreeResources(resources);
	
	drm->connector_id = connector->connector_id;
	
	drmSetClientCap(drm->fd, DRM_CLIENT_CAP_ATOMIC, 1);
	drmSetClientCap(drm->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	
	drm->plane_id = get_plane_id( &g_DRM );
	
	if ( drm->plane_id == 0 )
	{
		printf("could not find a suitable plane\n");
		return -1;
	}
	
	drm->plane = (struct plane*)calloc(1, sizeof(*drm->plane));
	drm->crtc = (struct crtc*)calloc(1, sizeof(*drm->crtc));
	drm->connector = (struct connector*)calloc(1, sizeof(*drm->connector));

#define get_resource(type, Type, id) do { 					\
		drm->type->type = drmModeGet##Type(drm->fd, id);			\
		if (!drm->type->type) {						\
			printf("could not get %s %i: %s\n",			\
					#type, id, strerror(errno));		\
			return -1;						\
		}								\
	} while (0)

	get_resource(plane, Plane, drm->plane_id);
	get_resource(crtc, Crtc, drm->crtc_id);
	get_resource(connector, Connector, drm->connector_id);

#define get_properties(type, TYPE, id) do {					\
		uint32_t i;							\
		drm->type->props = drmModeObjectGetProperties(drm->fd,		\
				id, DRM_MODE_OBJECT_##TYPE);			\
		if (!drm->type->props) {						\
			printf("could not get %s %u properties: %s\n", 		\
					#type, id, strerror(errno));		\
			return -1;						\
		}								\
		drm->type->props_info = (drmModePropertyRes**)calloc(drm->type->props->count_props,	\
				sizeof(*drm->type->props_info));			\
		for (i = 0; i < drm->type->props->count_props; i++) {		\
			drm->type->props_info[i] = drmModeGetProperty(drm->fd,	\
					drm->type->props->props[i]);		\
		}								\
	} while (0)

	get_properties(plane, PLANE, drm->plane_id);
	get_properties(crtc, CRTC, drm->crtc_id);
	get_properties(connector, CONNECTOR, drm->connector_id);
	
	drm->kms_in_fence_fd = -1;
	
	std::thread flip_handler_thread( flip_handler_thread_run );
	flip_handler_thread.detach();
	
	g_nOutputWidth = drm->mode->hdisplay;
	g_nOutputHeight = drm->mode->vdisplay;
	
	g_nOutputRefresh = drm->mode->vrefresh;

	if ( g_nOutputWidth < g_nOutputHeight )
	{
		// We probably don't want to be in portrait mode, rotate
		g_bRotated = true;

		g_nOutputWidth = drm->mode->vdisplay;
		g_nOutputHeight = drm->mode->hdisplay;
	}

	if (g_bUseLayers) {
		liftoff_log_init(g_bDebugLayers ? LIFTOFF_DEBUG : LIFTOFF_ERROR, NULL);
	}
	
	drm->lo_device = liftoff_device_create( drm->fd );
	drm->lo_output = liftoff_output_create( drm->lo_device, drm->crtc_id );
	
	assert( drm->lo_device && drm->lo_output );
	
	for ( int i = 0; i < k_nMaxLayers; i++ )
	{
		drm->lo_layers[ i ] = liftoff_layer_create( drm->lo_output );
		assert( drm->lo_layers[ i ] );
	}
	
	drm->flipcount = 0;
	
	return 0;
}

static int add_connector_property(struct drm_t *drm, drmModeAtomicReq *req,
								  uint32_t obj_id, const char *name,
								  uint64_t value)
{
	struct connector *obj = drm->connector;
	unsigned int i;
	int prop_id = 0;
	
	for (i = 0 ; i < obj->props->count_props ; i++) {
		if (strcmp(obj->props_info[i]->name, name) == 0) {
			prop_id = obj->props_info[i]->prop_id;
			break;
		}
	}
	
	if (prop_id < 0) {
		printf("no connector property: %s\n", name);
		return -EINVAL;
	}
	
	return drmModeAtomicAddProperty(req, obj_id, prop_id, value);
}

static int add_crtc_property(struct drm_t *drm, drmModeAtomicReq *req,
							 uint32_t obj_id, const char *name,
							 uint64_t value)
{
	struct crtc *obj = drm->crtc;
	unsigned int i;
	int prop_id = -1;
	
	for (i = 0 ; i < obj->props->count_props ; i++) {
		if (strcmp(obj->props_info[i]->name, name) == 0) {
			prop_id = obj->props_info[i]->prop_id;
			break;
		}
	}
	
	if (prop_id < 0) {
		printf("no crtc property: %s\n", name);
		return -EINVAL;
	}
	
	return drmModeAtomicAddProperty(req, obj_id, prop_id, value);
}

static int add_plane_property(struct drm_t *drm, drmModeAtomicReq *req,
                                                         uint32_t obj_id, const      char* name,
                                                         uint64_t value)
{
       struct plane *obj = drm->plane;
       unsigned int i;
       int prop_id = -1;

       for (i = 0 ; i < obj->props->count_props ; i++) {
               if (strcmp(obj->props_info[i]->name, name) == 0) {
                       prop_id = obj->props_info[i]->prop_id;
                       break;
               }
       }


       if (prop_id < 0) {
               printf("no plane property: %s\n", name);
               return -EINVAL;
       }

       return drmModeAtomicAddProperty(req, obj_id, prop_id, value);
}

int drm_atomic_commit(struct drm_t *drm, struct Composite_t *pComposite, struct VulkanPipeline_t *pPipeline )
{
	int ret;
	
	assert( drm->req != nullptr );
	
// 	if (drm->kms_in_fence_fd != -1) {
// 		add_plane_property(drm, req, plane_id, "IN_FENCE_FD", drm->kms_in_fence_fd);
// 	}
	
// 	drm->kms_out_fence_fd = -1;
	
// 	add_crtc_property(drm, req, drm->crtc_id, "OUT_FENCE_PTR",
// 					  (uint64_t)(unsigned long)&drm->kms_out_fence_fd);
	
	if ( s_drm_log != 0 ) 
	{
		printf("flipping\n");
	}
	drm->flip_lock.lock();

	// Do it before the commit, as otherwise the pageflip handler could
	// potentially beat us to the refcount checks.
	for ( uint32_t i = 0; i < drm->fbids_in_req.size(); i++ )
	{
		assert( g_DRM.map_fbid_inflightflips[ drm->fbids_in_req[ i ] ].first == true );
		g_DRM.map_fbid_inflightflips[ drm->fbids_in_req[ i ] ].second++;
	}
	
	g_DRM.flipcount++;
	gpuvis_trace_printf ( "legacy flip commit %lu\n", (uint64_t)g_DRM.flipcount );
	ret = drmModeAtomicCommit(drm->fd, drm->req, drm->flags, (void*)(uint64_t)g_DRM.flipcount );
	if (ret)
	{
		if ( ret != -EBUSY && ret != -ENOSPC )
		{
			fprintf( stderr, "flip error %d\n", ret);
			exit( 0 );
		}
		else
		{
			fprintf( stderr, "flip busy %d\n", ret);
		}

		// Undo refcount if the commit didn't actually work
		for ( uint32_t i = 0; i < drm->fbids_in_req.size(); i++ )
		{
			g_DRM.map_fbid_inflightflips[ drm->fbids_in_req[ i ] ].second--;
		}
		
		g_DRM.flipcount--;

		drm->flip_lock.unlock();

		goto out;
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

uint32_t drm_fbid_from_dmabuf( struct drm_t *drm, struct wlr_dmabuf_attributes *dma_buf )
{
	uint32_t ret = 0;
	uint32_t handles[4] = { 0 };
	drmPrimeFDToHandle( drm->fd, dma_buf->fd[0], &handles[0] );
	
	drmModeAddFB2( drm->fd, dma_buf->width, dma_buf->height, dma_buf->format, handles, dma_buf->stride, dma_buf->offset, &ret, 0 );
	
	if ( s_drm_log != 0 )
	{
		printf("make fbid %u\n", ret);
	}
	assert( drm->map_fbid_inflightflips[ ret ].first == false );

	drm->map_fbid_inflightflips[ ret ].first = true;
	drm->map_fbid_inflightflips[ ret ].second = 0;
	
	return ret;
}

void drm_free_fbid( struct drm_t *drm, uint32_t fbid )
{
	assert( drm->map_fbid_inflightflips[ fbid ].first == true );
	drm->map_fbid_inflightflips[ fbid ].first = false;

	if ( drm->map_fbid_inflightflips[ fbid ].second == 0 )
	{
		if ( s_drm_log != 0 )
		{
			printf("free fbid %u\n", fbid);
		}
		drmModeRmFB( drm->fd, fbid );
	}
	else
	{
		std::lock_guard<std::mutex> lock( drm->free_queue_lock );
		
		drm->fbid_free_queue.push_back( fbid );
	}
}

bool drm_can_avoid_composite( struct drm_t *drm, struct Composite_t *pComposite, struct VulkanPipeline_t *pPipeline )
{
	int nLayerCount = pComposite->nLayerCount;
	
	if ( g_bUseLayers == false )
	{
		// Discard cases where our non-liftoff path is known to fail
		
		// It only supports one layer
		if ( nLayerCount > 1 )
		{
			return false;
		}
		
		// Getting EINVAL trying to flip a 1x1 window, so does liftoff
		// TODO: get liftoff and/or amdgpuo bug fixed, workaround below
		if ( pPipeline->layerBindings[ 0 ].surfaceWidth < 64 ||
			 pPipeline->layerBindings[ 0 ].surfaceHeight < 64 )
		{
			return false;
		}
		
		if ( pPipeline->layerBindings[ 0 ].fbid == 0 )
		{
			return false;
		}
	}

	drm->fbids_in_req.clear();

	if ( g_bUseLayers == true )
	{

		for ( int i = 0; i < k_nMaxLayers; i++ )
		{
			if ( i < nLayerCount )
			{
				if ( pPipeline->layerBindings[ i ].fbid == 0 )
				{
					return false;
				}

				liftoff_layer_set_property( drm->lo_layers[ i ], "FB_ID", pPipeline->layerBindings[ i ].fbid);
				drm->fbids_in_req.push_back( pPipeline->layerBindings[ i ].fbid );

				liftoff_layer_set_property( drm->lo_layers[ i ], "zpos", pPipeline->layerBindings[ i ].zpos );
				liftoff_layer_set_property( drm->lo_layers[ i ], "alpha", pComposite->layers[ i ].flOpacity * 0xffff);

				if ( pPipeline->layerBindings[ i ].zpos == 0 )
				{
					assert( ( pComposite->layers[ i ].flOpacity * 0xffff ) == 0xffff );
				}

				const uint16_t srcWidth = pPipeline->layerBindings[ i ].surfaceWidth;
				const uint16_t srcHeight = pPipeline->layerBindings[ i ].surfaceHeight;

				liftoff_layer_set_property( drm->lo_layers[ i ], "SRC_X", 0);
				liftoff_layer_set_property( drm->lo_layers[ i ], "SRC_Y", 0);
				liftoff_layer_set_property( drm->lo_layers[ i ], "SRC_W", srcWidth << 16);
				liftoff_layer_set_property( drm->lo_layers[ i ], "SRC_H", srcHeight << 16);

				int32_t crtcX = -pComposite->layers[ i ].flOffsetX;
				int32_t crtcY = -pComposite->layers[ i ].flOffsetY;
				uint64_t crtcW = srcWidth / pComposite->layers[ i ].flScaleX;
				uint64_t crtcH = srcHeight / pComposite->layers[ i ].flScaleY;

				if (g_bRotated) {
					const int32_t x = crtcX;
					const uint64_t w = crtcW;
					crtcX = crtcY;
					crtcY = x;
					crtcW = crtcH;
					crtcH = w;

					liftoff_layer_set_property( drm->lo_layers[ i ], "rotation", DRM_MODE_ROTATE_270);
				}

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

	}
	
	assert( drm->req == nullptr );
	drm->req = drmModeAtomicAlloc();
	
	static bool bFirstSwap = true;
	uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK;
	uint32_t blob_id;
	
	// Temporary hack until we figure out what AMDGPU DC expects when changing the dest rect
	if ( 1 || bFirstSwap == true )
	{
		flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
		bFirstSwap = false;
	}
	
	// We do internal refcounting with these events
	flags |= DRM_MODE_PAGE_FLIP_EVENT;
	
	if (flags & DRM_MODE_ATOMIC_ALLOW_MODESET) {
		if (add_connector_property(drm, drm->req, drm->connector_id, "CRTC_ID",
			drm->crtc_id) < 0)
			return -1;
		
		if (drmModeCreatePropertyBlob(drm->fd, drm->mode, sizeof(*drm->mode),
			&blob_id) != 0)
			return -1;
		
		if (add_crtc_property(drm, drm->req, drm->crtc_id, "MODE_ID", blob_id) < 0)
			return -1;
		
		if (add_crtc_property(drm, drm->req, drm->crtc_id, "ACTIVE", 1) < 0)
			return -1;
	}
	
	drm->flags = flags;
	
	if ( g_bUseLayers == true )
	{
		if ( liftoff_output_apply( drm->lo_output, drm->req ) == true )
		{
			if ( s_drm_log != 0 )
			{
				fprintf( stderr, "can drm present %i layers\n", nLayerCount );
			}
			return true;
		}
		
		if ( s_drm_log != 0 )
		{
			fprintf( stderr, "can NOT drm present %i layers\n", nLayerCount );
		}
		
		drmModeAtomicFree( drm->req );
		drm->req = nullptr;
		
		return false;
	}
	else
	{
		drmModeAtomicReq *req = drm->req;
		uint32_t plane_id = drm->plane->plane->plane_id;
		uint32_t fb_id = pPipeline->layerBindings[ 0 ].fbid;
		
		assert( fb_id != 0 );
		drm->fbids_in_req.push_back( fb_id );
		
		if ( g_bRotated )
		{
			add_plane_property(drm, req, plane_id, "rotation", DRM_MODE_ROTATE_270);
		}
		
		add_plane_property(drm, req, plane_id, "FB_ID", fb_id);
		add_plane_property(drm, req, plane_id, "CRTC_ID", drm->crtc_id);
		add_plane_property(drm, req, plane_id, "SRC_X", 0);
		add_plane_property(drm, req, plane_id, "SRC_Y", 0);
		add_plane_property(drm, req, plane_id, "SRC_W", pPipeline->layerBindings[ 0 ].surfaceWidth << 16);
		add_plane_property(drm, req, plane_id, "SRC_H", pPipeline->layerBindings[ 0 ].surfaceHeight << 16);
		
		gpuvis_trace_printf ( "legacy flip fb_id %u src %ix%i\n", fb_id,
							 pPipeline->layerBindings[ 0 ].surfaceWidth,
							 pPipeline->layerBindings[ 0 ].surfaceHeight );
		
		if ( g_bRotated )
		{
			add_plane_property(drm, req, plane_id, "CRTC_X", pComposite->layers[ 0 ].flOffsetY * -1);
			add_plane_property(drm, req, plane_id, "CRTC_Y", pComposite->layers[ 0 ].flOffsetX * -1);
			
			add_plane_property(drm, req, plane_id, "CRTC_H", pPipeline->layerBindings[ 0 ].surfaceWidth / pComposite->layers[ 0 ].flScaleX);
			add_plane_property(drm, req, plane_id, "CRTC_W", pPipeline->layerBindings[ 0 ].surfaceHeight / pComposite->layers[ 0 ].flScaleY);
		}
		else
		{
			add_plane_property(drm, req, plane_id, "CRTC_X", pComposite->layers[ 0 ].flOffsetX * -1);
			add_plane_property(drm, req, plane_id, "CRTC_Y", pComposite->layers[ 0 ].flOffsetY * -1);
			
			add_plane_property(drm, req, plane_id, "CRTC_W", pPipeline->layerBindings[ 0 ].surfaceWidth / pComposite->layers[ 0 ].flScaleX);
			add_plane_property(drm, req, plane_id, "CRTC_H", pPipeline->layerBindings[ 0 ].surfaceHeight / pComposite->layers[ 0 ].flScaleY);
			
			gpuvis_trace_printf ( "crtc %i+%i@%ix%i\n",
								  (int)pComposite->layers[ 0 ].flOffsetX * -1, (int)pComposite->layers[ 0 ].flOffsetY * -1,
								  (int)(pPipeline->layerBindings[ 0 ].surfaceWidth / pComposite->layers[ 0 ].flScaleX),
								  (int)(pPipeline->layerBindings[ 0 ].surfaceHeight / pComposite->layers[ 0 ].flScaleX) );
		}
		
		
		return true;
	}
}
