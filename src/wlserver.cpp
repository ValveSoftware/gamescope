#include <xkbcommon/xkbcommon-keysyms.h>
#define _GNU_SOURCE 1

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <poll.h>

#include <linux/input-event-codes.h>

#include <X11/extensions/XTest.h>
#include <xkbcommon/xkbcommon.h>

#include <wayland-server-core.h>

extern "C" {
#define static
#define class class_
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/multi.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/util/log.h>
#include <wlr/xwayland/server.h>
#undef static
#undef class
}

#include "gamescope-xwayland-protocol.h"
#include "gamescope-pipewire-protocol.h"
#include "gamescope-tearing-control-unstable-v1-protocol.h"

#include "wlserver.hpp"
#include "drm.hpp"
#include "main.hpp"
#include "steamcompmgr.hpp"
#include "log.hpp"
#include "ime.hpp"
#include "xwayland_ctx.hpp"

#if HAVE_PIPEWIRE
#include "pipewire.hpp"
#endif

#include "gpuvis_trace_utils.h"

#include <algorithm>
#include <list>

static LogScope wl_log("wlserver");

static struct wlserver_t wlserver = {};

struct wlserver_content_override {
	struct wlr_surface *surface;
	uint32_t x11_window;
	struct wl_listener surface_destroy_listener;
};

enum wlserver_touch_click_mode g_nDefaultTouchClickMode = WLSERVER_TOUCH_CLICK_LEFT;
enum wlserver_touch_click_mode g_nTouchClickMode = g_nDefaultTouchClickMode;


static struct wl_list pending_surfaces = {0};

static void wlserver_x11_surface_info_set_wlr( struct wlserver_x11_surface_info *surf, struct wlr_surface *wlr_surf );
static wlserver_wl_surface_info *get_wl_surface_info(struct wlr_surface *wlr_surf);

extern const struct wlr_surface_role xwayland_surface_role;

std::vector<ResListEntry_t> gamescope_xwayland_server_t::retrieve_commits()
{
	std::vector<ResListEntry_t> commits;
	{
		std::lock_guard<std::mutex> lock( wayland_commit_lock );
		commits = std::move(wayland_commit_queue);
	}
	return commits;
}

void gamescope_xwayland_server_t::wayland_commit(struct wlr_surface *surf, struct wlr_buffer *buf)
{
	{
		std::lock_guard<std::mutex> lock( wayland_commit_lock );

		ResListEntry_t newEntry = {
			.surf = surf,
			.buf = buf,
		};
		wayland_commit_queue.push_back( newEntry );
	}

	nudge_steamcompmgr();
}

struct PendingCommit_t
{
	struct wlr_surface *surf;
	struct wlr_buffer *buf;
};

std::list<PendingCommit_t> g_PendingCommits;

void xwayland_surface_role_commit(struct wlr_surface *wlr_surface) {
	assert(wlr_surface->role == &xwayland_surface_role);

	uint32_t committed = wlr_surface->current.committed;
	wlr_surface->current.committed = 0;

	if (!(committed & WLR_SURFACE_STATE_BUFFER)) {
		return;
	}

	VulkanWlrTexture_t *tex = (VulkanWlrTexture_t *) wlr_surface_get_texture( wlr_surface );
	if ( tex == NULL )
	{
		return;
	}

	struct wlr_buffer *buf = wlr_buffer_lock( tex->buf );

	gpuvis_trace_printf( "xwayland_surface_role_commit wlr_surface %p", wlr_surface );

	wlserver_x11_surface_info *wlserver_x11_surface_info = get_wl_surface_info(wlr_surface)->x11_surface;
	if (wlserver_x11_surface_info)
	{
		assert(wlserver_x11_surface_info->xwayland_server);
		wlserver_x11_surface_info->xwayland_server->wayland_commit( wlr_surface, buf );
	}
	else
	{
		g_PendingCommits.push_back(PendingCommit_t{ wlr_surface, buf });
	}
}

static void xwayland_surface_role_precommit(struct wlr_surface *wlr_surface, const wlr_surface_state *wlr_surface_state) {
	assert(wlr_surface->role == &xwayland_surface_role);
}

const struct wlr_surface_role xwayland_surface_role = {
	.name = "wlr_xwayland_surface",
	.commit = xwayland_surface_role_commit,
	.precommit = xwayland_surface_role_precommit,
};

void gamescope_xwayland_server_t::on_xwayland_ready(void *data)
{
	xwayland_ready = true;

	if (!xwayland_server->options.no_touch_pointer_emulation)
		wl_log.infof("Xwayland doesn't support -noTouchPointerEmulation, touch events might get duplicated");

	dpy = XOpenDisplay( get_nested_display_name() );
}

void gamescope_xwayland_server_t::xwayland_ready_callback(struct wl_listener *listener, void *data)
{
	gamescope_xwayland_server_t *server = wl_container_of( listener, server, xwayland_ready_listener );
	server->on_xwayland_ready(data);
}

static void bump_input_counter()
{
	inputCounter++;
	nudge_steamcompmgr();
}

static void wlserver_handle_modifiers(struct wl_listener *listener, void *data)
{
	struct wlserver_keyboard *keyboard = wl_container_of( listener, keyboard, modifiers );

	wlr_seat_set_keyboard( wlserver.wlr.seat, keyboard->wlr );
	wlr_seat_keyboard_notify_modifiers( wlserver.wlr.seat, &keyboard->wlr->modifiers );

	bump_input_counter();
}

static void wlserver_handle_key(struct wl_listener *listener, void *data)
{
	struct wlserver_keyboard *keyboard = wl_container_of( listener, keyboard, key );
	struct wlr_keyboard_key_event *event = (struct wlr_keyboard_key_event *) data;

	xkb_keycode_t keycode = event->keycode + 8;
	xkb_keysym_t keysym = xkb_state_key_get_one_sym(keyboard->wlr->xkb_state, keycode);

	if (wlserver.wlr.session && event->state == WL_KEYBOARD_KEY_STATE_PRESSED && keysym >= XKB_KEY_XF86Switch_VT_1 && keysym <= XKB_KEY_XF86Switch_VT_12) {
		unsigned vt = keysym - XKB_KEY_XF86Switch_VT_1 + 1;
		wlr_session_change_vt(wlserver.wlr.session, vt);
		return;
	}

	if ( ( event->state == WL_KEYBOARD_KEY_STATE_PRESSED || event->state == WL_KEYBOARD_KEY_STATE_RELEASED ) && ( keysym == XKB_KEY_XF86AudioLowerVolume || keysym == XKB_KEY_XF86AudioRaiseVolume ) )
	{
		// Always send volume+/- to root server only, to avoid it reaching the game.
		struct wlr_surface *old_kb_surf = wlserver.kb_focus_surface;
		struct wlr_surface *new_kb_surf = steamcompmgr_get_server_input_surface( 0 );
		if ( new_kb_surf )
		{
			wlserver_keyboardfocus( new_kb_surf );
			wlr_seat_set_keyboard( wlserver.wlr.seat, keyboard->wlr );
			wlr_seat_keyboard_notify_key( wlserver.wlr.seat, event->time_msec, event->keycode, event->state );
			wlserver_keyboardfocus( old_kb_surf );
			return;
		}
	}

	wlr_seat_set_keyboard( wlserver.wlr.seat, keyboard->wlr );
	wlr_seat_keyboard_notify_key( wlserver.wlr.seat, event->time_msec, event->keycode, event->state );

	bump_input_counter();
}

static void wlserver_handle_pointer_motion(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_motion_event *event = (struct wlr_pointer_motion_event *) data;

	// TODO: Pick the xwayland_server with active focus
	auto server = steamcompmgr_get_focused_server();
	if ( server != NULL )
	{
		server->ctx->accum_x += event->unaccel_dx;
		server->ctx->accum_y += event->unaccel_dy;

		float dx, dy;
		server->ctx->accum_x = modf(server->ctx->accum_x, &dx);
		server->ctx->accum_y = modf(server->ctx->accum_y, &dy);

		XTestFakeRelativeMotionEvent( server->get_xdisplay(), int(dx), int(dy), CurrentTime );
		XFlush( server->get_xdisplay() );
	}
}

static void wlserver_handle_pointer_button(struct wl_listener *listener, void *data)
{
	struct wlserver_pointer *pointer = wl_container_of( listener, pointer, button );
	struct wlr_pointer_button_event *event = (struct wlr_pointer_button_event *) data;

	wlr_seat_pointer_notify_button( wlserver.wlr.seat, event->time_msec, event->button, event->state );
}

static void wlserver_handle_pointer_axis(struct wl_listener *listener, void *data)
{
	struct wlserver_pointer *pointer = wl_container_of( listener, pointer, axis );
	struct wlr_pointer_axis_event *event = (struct wlr_pointer_axis_event *) data;

	wlr_seat_pointer_notify_axis( wlserver.wlr.seat, event->time_msec, event->orientation, event->delta, event->delta_discrete, event->source );
}

static void wlserver_handle_pointer_frame(struct wl_listener *listener, void *data)
{
	wlr_seat_pointer_notify_frame( wlserver.wlr.seat );

	bump_input_counter();
}

static inline uint32_t steamcompmgr_button_to_wlserver_button( int button )
{
	switch ( button )
	{
		default:
		case WLSERVER_TOUCH_CLICK_HOVER:
			return 0;
		case WLSERVER_TOUCH_CLICK_LEFT:
			return BTN_LEFT;
		case WLSERVER_TOUCH_CLICK_RIGHT:
			return BTN_RIGHT;
		case WLSERVER_TOUCH_CLICK_MIDDLE:
			return BTN_MIDDLE;
	}
}

std::atomic<bool> g_bPendingTouchMovement = { false };

static void wlserver_handle_touch_down(struct wl_listener *listener, void *data)
{
	struct wlserver_touch *touch = wl_container_of( listener, touch, down );
	struct wlr_touch_down_event *event = (struct wlr_touch_down_event *) data;

	wlserver_touchdown( event->x, event->y, event->touch_id, event->time_msec );
}

static void wlserver_handle_touch_up(struct wl_listener *listener, void *data)
{
	struct wlserver_touch *touch = wl_container_of( listener, touch, up );
	struct wlr_touch_up_event *event = (struct wlr_touch_up_event *) data;

	wlserver_touchup( event->touch_id, event->time_msec );
}

static void wlserver_handle_touch_motion(struct wl_listener *listener, void *data)
{
	struct wlserver_touch *touch = wl_container_of( listener, touch, motion );
	struct wlr_touch_motion_event *event = (struct wlr_touch_motion_event *) data;

	wlserver_touchmotion( event->x, event->y, event->touch_id, event->time_msec );
}

static void wlserver_new_input(struct wl_listener *listener, void *data)
{
	struct wlr_input_device *device = (struct wlr_input_device *) data;

	switch ( device->type )
	{
		case WLR_INPUT_DEVICE_KEYBOARD:
		{
			struct wlserver_keyboard *keyboard = (struct wlserver_keyboard *) calloc( 1, sizeof( struct wlserver_keyboard ) );

			keyboard->wlr = (struct wlr_keyboard *)device;

			struct xkb_rule_names rules = { 0 };
			struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
			rules.rules = getenv("XKB_DEFAULT_RULES");
			rules.model = getenv("XKB_DEFAULT_MODEL");
			rules.layout = getenv("XKB_DEFAULT_LAYOUT");
			rules.variant = getenv("XKB_DEFAULT_VARIANT");
			rules.options = getenv("XKB_DEFAULT_OPTIONS");
			struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, &rules,
															   XKB_KEYMAP_COMPILE_NO_FLAGS);

			wlr_keyboard_set_keymap(keyboard->wlr, keymap);
			xkb_keymap_unref(keymap);
			xkb_context_unref(context);
			wlr_keyboard_set_repeat_info(keyboard->wlr, 25, 600);

			keyboard->wlr->data = keyboard;

			keyboard->modifiers.notify = wlserver_handle_modifiers;
			wl_signal_add( &keyboard->wlr->events.modifiers, &keyboard->modifiers );

			keyboard->key.notify = wlserver_handle_key;
			wl_signal_add( &keyboard->wlr->events.key, &keyboard->key );
		}
		break;
		case WLR_INPUT_DEVICE_POINTER:
		{
			struct wlserver_pointer *pointer = (struct wlserver_pointer *) calloc( 1, sizeof( struct wlserver_pointer ) );

			pointer->wlr = (struct wlr_pointer *)device;

			pointer->motion.notify = wlserver_handle_pointer_motion;
			wl_signal_add( &pointer->wlr->events.motion, &pointer->motion );
			pointer->button.notify = wlserver_handle_pointer_button;
			wl_signal_add( &pointer->wlr->events.button, &pointer->button );
			pointer->axis.notify = wlserver_handle_pointer_axis;
			wl_signal_add( &pointer->wlr->events.axis, &pointer->axis);
			pointer->frame.notify = wlserver_handle_pointer_frame;
			wl_signal_add( &pointer->wlr->events.frame, &pointer->frame);
		}
		break;
		case WLR_INPUT_DEVICE_TOUCH:
		{
			struct wlserver_touch *touch = (struct wlserver_touch *) calloc( 1, sizeof( struct wlserver_touch ) );

			touch->wlr = (struct wlr_touch *)device;

			touch->down.notify = wlserver_handle_touch_down;
			wl_signal_add( &touch->wlr->events.down, &touch->down );
			touch->up.notify = wlserver_handle_touch_up;
			wl_signal_add( &touch->wlr->events.up, &touch->up );
			touch->motion.notify = wlserver_handle_touch_motion;
			wl_signal_add( &touch->wlr->events.motion, &touch->motion );
		}
		break;
		default:
			break;
	}
}

static struct wl_listener new_input_listener = { .notify = wlserver_new_input };

static wlserver_wl_surface_info *get_wl_surface_info(struct wlr_surface *wlr_surf)
{
	return reinterpret_cast<wlserver_wl_surface_info *>(wlr_surf->data);
}

static void handle_wl_surface_destroy( struct wl_listener *l, void *data )
{
	wlserver_wl_surface_info *surf = wl_container_of( l, surf, destroy );
	if (surf->x11_surface)
		surf->x11_surface->wlr = nullptr;

	if ( surf->wlr == wlserver.mouse_focus_surface )
		wlserver.mouse_focus_surface = nullptr;

	if ( surf->wlr == wlserver.kb_focus_surface )
		wlserver.kb_focus_surface = nullptr;

	for (auto it = g_PendingCommits.begin(); it != g_PendingCommits.end();)
	{
		if (it->surf == surf->wlr)
		{
			// We owned the buffer lock, so unlock it here.
			wlr_buffer_unlock(it->buf);
			it = g_PendingCommits.erase(it);
		}
		else
		{
			it++;
		}
	}

	surf->wlr->data = nullptr;
	delete surf;
}

static void wlserver_new_surface(struct wl_listener *l, void *data)
{
	struct wlr_surface *wlr_surf = (struct wlr_surface *)data;
	uint32_t id = wl_resource_get_id(wlr_surf->resource);

	wlserver_wl_surface_info *wl_surface_info = new wlserver_wl_surface_info;
	wl_surface_info->wlr = wlr_surf;
	wl_surface_info->destroy.notify = handle_wl_surface_destroy;
	wl_signal_add( &wlr_surf->events.destroy, &wl_surface_info->destroy );

	wlr_surf->data = wl_surface_info;

	struct wlserver_x11_surface_info *s, *tmp;
	wl_list_for_each_safe(s, tmp, &pending_surfaces, pending_link)
	{
		if (s->wl_id == id && s->wlr == nullptr)
		{
			wlserver_x11_surface_info_set_wlr( s, wlr_surf );
		}
	}
}

static struct wl_listener new_surface_listener = { .notify = wlserver_new_surface };

void gamescope_xwayland_server_t::destroy_content_override( struct wlserver_content_override *co )
{
	wl_list_remove( &co->surface_destroy_listener.link );
	content_overrides.erase( co->x11_window );
	free( co );
}

static void content_override_handle_surface_destroy( struct wl_listener *listener, void *data )
{
	struct wlserver_content_override *co = wl_container_of( listener, co, surface_destroy_listener );
	/* The protocol only works for server 0, so assume that for now.
	 * A future revision may change this and add a DISPLAY parameter or something.
	 * That would be cool! */
	gamescope_xwayland_server_t *server = wlserver_get_xwayland_server( 0 );
	assert( server );
	server->destroy_content_override( co );
}

void gamescope_xwayland_server_t::handle_override_window_content( struct wl_client *client, struct wl_resource *resource, struct wl_resource *surface_resource, uint32_t x11_window )
{
	struct wlr_surface *surface = wlr_surface_from_resource( surface_resource );

	if ( content_overrides.count( x11_window ) ) {
		destroy_content_override( content_overrides[ x11_window ] );
	}

	struct wlserver_content_override *co = (struct wlserver_content_override *)calloc(1, sizeof(*co));
	co->surface = surface;
	co->x11_window = x11_window;
	co->surface_destroy_listener.notify = content_override_handle_surface_destroy;
	wl_signal_add( &surface->events.destroy, &co->surface_destroy_listener );
	content_overrides[ x11_window ] = co;
}

struct wl_client *gamescope_xwayland_server_t::get_client()
{
	return xwayland_server->client;
}

struct wlr_output *gamescope_xwayland_server_t::get_output()
{
	return output;
}

static void gamescope_xwayland_handle_override_window_content( struct wl_client *client, struct wl_resource *resource, struct wl_resource *surface_resource, uint32_t x11_window )
{
	// This should ideally use the surface's xwayland, but we don't know it.
	// We probably need to change our override_window_content protocol to add a
	// xwayland socket name.
	//
	// Right now, the surface -> xwayland association comes from the
	// handle_wl_id stuff from steamcompmgr.
	// However, this surface has no associated X window, and won't recieve
	// wl_id stuff as it's meant to replace another window's surface
	// which we can't do without knowing the x11_window's xwayland server
	// here for it to do that override logic in the first place.
	//
	// So... Just assume it comes from server 0 for now.
	gamescope_xwayland_server_t *server = wlserver_get_xwayland_server( 0 );
	assert( server );
	server->handle_override_window_content(client, resource, surface_resource, x11_window);
}

static void gamescope_xwayland_handle_destroy( struct wl_client *client, struct wl_resource *resource )
{
	wl_resource_destroy( resource );
}

static const struct gamescope_xwayland_interface gamescope_xwayland_impl = {
	.destroy = gamescope_xwayland_handle_destroy,
	.override_window_content = gamescope_xwayland_handle_override_window_content,
};

static void gamescope_xwayland_bind( struct wl_client *client, void *data, uint32_t version, uint32_t id )
{
	struct wl_resource *resource = wl_resource_create( client, &gamescope_xwayland_interface, version, id );
	wl_resource_set_implementation( resource, &gamescope_xwayland_impl, NULL, NULL );
}

static void create_gamescope_xwayland( void )
{
	uint32_t version = 1;
	wl_global_create( wlserver.display, &gamescope_xwayland_interface, version, NULL, gamescope_xwayland_bind );
}

#if HAVE_PIPEWIRE
static void gamescope_pipewire_handle_destroy( struct wl_client *client, struct wl_resource *resource )
{
	wl_resource_destroy( resource );
}

static const struct gamescope_pipewire_interface gamescope_pipewire_impl = {
	.destroy = gamescope_pipewire_handle_destroy,
};

static void gamescope_pipewire_bind( struct wl_client *client, void *data, uint32_t version, uint32_t id )
{
	struct wl_resource *resource = wl_resource_create( client, &gamescope_pipewire_interface, version, id );
	wl_resource_set_implementation( resource, &gamescope_pipewire_impl, NULL, NULL );

	gamescope_pipewire_send_stream_node( resource, get_pipewire_stream_node_id() );
}

static void create_gamescope_pipewire( void )
{
	uint32_t version = 1;
	wl_global_create( wlserver.display, &gamescope_pipewire_interface, version, NULL, gamescope_pipewire_bind );
}
#endif



static void gamescope_surface_tearing_set_presentation_hint( struct wl_client *client, struct wl_resource *resource, uint32_t hint )
{
	wlserver_wl_surface_info *wl_surface_info = (wlserver_wl_surface_info *)wl_resource_get_user_data( resource );

	wl_surface_info->presentation_hint = hint;
}

static void gamescope_surface_tearing_destroy( struct wl_client *client, struct wl_resource *resource )
{
	wl_resource_destroy( resource );
}

static const struct gamescope_surface_tearing_control_v1_interface surface_tearing_control_impl {
	.set_presentation_hint = gamescope_surface_tearing_set_presentation_hint,
	.destroy = gamescope_surface_tearing_destroy,
};

static void gamescope_tearing_handle_destroy( struct wl_client *client, struct wl_resource *resource )
{
	wl_resource_destroy( resource );
}

static void gamescope_tearing_get_tearing_control( struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource )
{
	struct wlr_surface *surface = wlr_surface_from_resource( surface_resource );

	struct wl_resource *surface_tearing_control_resource
		= wl_resource_create( client, &gamescope_surface_tearing_control_v1_interface, wl_resource_get_version( resource ), id );
	wl_resource_set_implementation( surface_tearing_control_resource, &surface_tearing_control_impl, get_wl_surface_info(surface), NULL );
}

static const struct gamescope_tearing_control_v1_interface tearing_control_impl = {
	.destroy			 = gamescope_tearing_handle_destroy,
	.get_tearing_control = gamescope_tearing_get_tearing_control,
};

static void gamescope_tearing_bind( struct wl_client *client, void *data, uint32_t version, uint32_t id )
{
	struct wl_resource *resource = wl_resource_create( client, &gamescope_tearing_control_v1_interface, version, id );
	wl_resource_set_implementation( resource, &tearing_control_impl, NULL, NULL );
}

static void create_gamescope_tearing( void )
{
	uint32_t version = 1;
	wl_global_create( wlserver.display, &gamescope_tearing_control_v1_interface, version, NULL, gamescope_tearing_bind );
}




static void handle_session_active( struct wl_listener *listener, void *data )
{
	if (wlserver.wlr.session->active) {
		g_DRM.out_of_date = 1;
		g_DRM.needs_modeset = 1;
	}
	g_DRM.paused = !wlserver.wlr.session->active;
	wl_log.infof( "Session %s", g_DRM.paused ? "paused" : "resumed" );
}

static void handle_wlr_log(enum wlr_log_importance importance, const char *fmt, va_list args)
{
	enum LogPriority prio;
	switch (importance) {
	case WLR_ERROR:
		prio = LOG_ERROR;
		break;
	case WLR_INFO:
		prio = LOG_INFO;
		break;
	default:
		prio = LOG_DEBUG;
		break;
	}

	wl_log.vlogf(prio, fmt, args);
}

void wlserver_set_output_info( const wlserver_output_info *info )
{
	free(wlserver.output_info.description);
	wlserver.output_info.description = strdup(info->description);
	wlserver.output_info.phys_width = info->phys_width;
	wlserver.output_info.phys_height = info->phys_height;

	if (wlserver.wlr.xwayland_servers.empty())
		return;

	gamescope_xwayland_server_t *server = wlserver.wlr.xwayland_servers[0].get();
	server->update_output_info();
}

static bool filter_global(const struct wl_client *client, const struct wl_global *global, void *data)
{
	const struct wl_interface *iface = wl_global_get_interface(global);
	if (strcmp(iface->name, wl_output_interface.name) != 0)
		return true;

	struct wlr_output *output = (struct wlr_output *) wl_global_get_user_data(global);
	if (!output)
	{
		/* Can happen if the output has been destroyed, but the client hasn't
		 * received the wl_registry.global_remove event yet. */
		return false;
	}

	/* We create one wl_output global per Xwayland server, to easily have
	 * per-server output configuration. Only expose the wl_output belonging to
	 * the server. */
	for (size_t i = 0; i < wlserver.wlr.xwayland_servers.size(); i++) {
		gamescope_xwayland_server_t *server = wlserver.wlr.xwayland_servers[i].get();
		if (server->get_client() == client)
			return server->get_output() == output;
	}

	if (wlserver.wlr.xwayland_servers.empty())
		return false;

	gamescope_xwayland_server_t *server = wlserver.wlr.xwayland_servers[0].get();
	/* If we aren't an xwayland server, then only expose the first wl_output
	 * that's associated with from server 0. */
	return server->get_output() == output;
}

bool wlsession_init( void ) {
	wlr_log_init(WLR_DEBUG, handle_wlr_log);

	wlserver.display = wl_display_create();
	wlserver.wlr.headless_backend = wlr_headless_backend_create( wlserver.display );

	wl_display_set_global_filter(wlserver.display, filter_global, nullptr);

	const struct wlserver_output_info output_info = {
		.description = "Virtual gamescope output",
	};
	wlserver_set_output_info( &output_info );

	if ( BIsNested() )
		return true;

	wlserver.wlr.session = wlr_session_create( wlserver.display );
	if ( wlserver.wlr.session == nullptr )
	{
		wl_log.errorf( "Failed to create session" );
		return false;
	}

	wlserver.session_active.notify = handle_session_active;
	wl_signal_add( &wlserver.wlr.session->events.active, &wlserver.session_active );

	return true;
}

static void kms_device_handle_change( struct wl_listener *listener, void *data )
{
	g_DRM.out_of_date = 1;
	wl_log.infof( "Got change event for KMS device" );

	nudge_steamcompmgr();
}

int wlsession_open_kms( const char *device_name ) {
	struct wlr_device *device = nullptr;
	if ( device_name != nullptr )
	{
		device = wlr_session_open_file( wlserver.wlr.session, device_name );
		if ( device == nullptr )
			return -1;
		if ( !drmIsKMS( device->fd ) )
		{
			wl_log.errorf( "'%s' is not a KMS device", device_name );
			wlr_session_close_file( wlserver.wlr.session, device );
			return -1;
		}
	}
	else
	{
		ssize_t n = wlr_session_find_gpus( wlserver.wlr.session, 1, &device );
		if ( n < 0 )
		{
			wl_log.errorf( "Failed to list GPUs" );
			return -1;
		}
		if ( n == 0 )
		{
			wl_log.errorf( "No GPU detected" );
			return -1;
		}
	}

	struct wl_listener *listener = new wl_listener();
	listener->notify = kms_device_handle_change;
	wl_signal_add( &device->events.change, listener );

	return device->fd;
}

gamescope_xwayland_server_t::gamescope_xwayland_server_t(wl_display *display)
{
	struct wlr_xwayland_server_options xwayland_options = {
		.lazy = false,
		.enable_wm = false,
		.no_touch_pointer_emulation = true,
	};
	xwayland_server = wlr_xwayland_server_create(display, &xwayland_options);
	wl_signal_add(&xwayland_server->events.ready, &xwayland_ready_listener);

	output = wlr_headless_add_output(wlserver.wlr.headless_backend, 1280, 720);
	output->make = strdup("gamescope");  // freed by wlroots
	output->model = strdup("gamescope"); // freed by wlroots
	wlr_output_set_name(output, "gamescope");

	int refresh = g_nNestedRefresh;
	if (refresh == 0) {
		refresh = g_nOutputRefresh;
	}

	wlr_output_enable(output, true);
	wlr_output_set_custom_mode(output, g_nNestedWidth, g_nNestedHeight, refresh * 1000);
	if (!wlr_output_commit(output))
	{
		wl_log.errorf("Failed to commit headless output");
		abort();
	}

	update_output_info();

	wlr_output_create_global(output);
}

gamescope_xwayland_server_t::~gamescope_xwayland_server_t()
{
	wlr_xwayland_server_destroy(xwayland_server);
	wlr_output_destroy(output);
}

void gamescope_xwayland_server_t::update_output_info()
{
	const auto *info = &wlserver.output_info;

	output->phys_width = info->phys_width;
	output->phys_height = info->phys_height;
	struct wl_resource *resource;
	wl_resource_for_each(resource, &output->resources) {
		wl_output_send_geometry(resource, 0, 0,
			output->phys_width, output->phys_height, output->subpixel,
			output->make, output->model, output->transform);
	}
	wlr_output_schedule_done(output);

	wlr_output_set_description(output, info->description);
}

bool wlserver_init( void ) {
	assert( wlserver.display != nullptr );

	bool bIsDRM = !BIsNested();

	wl_list_init(&pending_surfaces);

	wlserver.event_loop = wl_display_get_event_loop(wlserver.display);

	wlserver.wlr.multi_backend = wlr_multi_backend_create(wlserver.display);
	wlr_multi_backend_add( wlserver.wlr.multi_backend, wlserver.wlr.headless_backend );

	assert( wlserver.event_loop && wlserver.wlr.multi_backend );

	wl_signal_add( &wlserver.wlr.multi_backend->events.new_input, &new_input_listener );

	if ( bIsDRM == True )
	{
		wlserver.wlr.libinput_backend = wlr_libinput_backend_create( wlserver.display, wlserver.wlr.session );
		if ( wlserver.wlr.libinput_backend == NULL)
		{
			return false;
		}
		wlr_multi_backend_add( wlserver.wlr.multi_backend, wlserver.wlr.libinput_backend );
	}

	// Create a stub wlr_keyboard only used to set the keymap
	// We need to wait for the backend to be started before adding the device
	struct wlr_keyboard *kbd = (struct wlr_keyboard *) calloc(1, sizeof(*kbd));
	wlr_keyboard_init(kbd, nullptr, "virtual");

	wlserver.wlr.virtual_keyboard_device = kbd;

	wlserver.wlr.renderer = vulkan_renderer_create();

	wlr_renderer_init_wl_display(wlserver.wlr.renderer, wlserver.display);

	wlserver.wlr.compositor = wlr_compositor_create(wlserver.display, wlserver.wlr.renderer);

	wl_signal_add( &wlserver.wlr.compositor->events.new_surface, &new_surface_listener );

	create_ime_manager( &wlserver );

	create_gamescope_xwayland();

#if HAVE_PIPEWIRE
	create_gamescope_pipewire();
#endif

	create_gamescope_tearing();

	int result = -1;
	int display_slot = 0;

	while ( result != 0 && display_slot < 128 )
	{
		sprintf( wlserver.wl_display_name, "gamescope-%d", display_slot );
		result = wl_display_add_socket( wlserver.display, wlserver.wl_display_name );
		display_slot++;
	}

	if ( result != 0 )
	{
		wl_log.errorf_errno("Unable to open wayland socket");
		wlr_backend_destroy( wlserver.wlr.multi_backend );
		return false;
	}

	wlserver.wlr.seat = wlr_seat_create(wlserver.display, "seat0");
	wlr_seat_set_capabilities( wlserver.wlr.seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_TOUCH );

	wl_log.infof("Running compositor on wayland display '%s'", wlserver.wl_display_name);

	if (!wlr_backend_start( wlserver.wlr.multi_backend ))
	{
		wl_log.errorf("Failed to start backend");
		wlr_backend_destroy( wlserver.wlr.multi_backend );
		wl_display_destroy(wlserver.display);
		return false;
	}

	wl_signal_emit( &wlserver.wlr.multi_backend->events.new_input, kbd );

	for (int i = 0; i < g_nXWaylandCount; i++)
	{
		auto server = std::make_unique<gamescope_xwayland_server_t>(wlserver.display);
		wlserver.wlr.xwayland_servers.emplace_back(std::move(server));
	}

	for (size_t i = 0; i < wlserver.wlr.xwayland_servers.size(); i++)
	{
		while (!wlserver.wlr.xwayland_servers[i]->is_xwayland_ready()) {
			wl_display_flush_clients(wlserver.display);
			if (wl_event_loop_dispatch(wlserver.event_loop, -1) < 0) {
				wl_log.errorf("wl_event_loop_dispatch failed\n");
				return false;
			}
		}
	}

	return true;
}

pthread_mutex_t waylock = PTHREAD_MUTEX_INITIALIZER;

void wlserver_lock(void)
{
	pthread_mutex_lock(&waylock);
}

void wlserver_unlock(void)
{
	wl_display_flush_clients(wlserver.display);
	pthread_mutex_unlock(&waylock);
}

extern std::mutex g_SteamCompMgrXWaylandServerMutex;

void wlserver_run(void)
{
	pthread_setname_np( pthread_self(), "gamescope-wl" );

	struct pollfd pollfd = {
		.fd = wl_event_loop_get_fd( wlserver.event_loop ),
		.events = POLLIN,
	};
	while ( g_bRun ) {
		int ret = poll( &pollfd, 1, -1 );
		if ( ret < 0 ) {
			if ( errno == EINTR )
				continue;
			wl_log.errorf_errno( "poll failed" );
			break;
		}

		if ( pollfd.revents & (POLLHUP | POLLERR) ) {
			wl_log.errorf( "socket %s", ( pollfd.revents & POLLERR ) ? "error" : "closed" );
			break;
		}

		if ( pollfd.revents & POLLIN ) {
			// We have wayland stuff to do, do it while locked
			wlserver_lock();

			wl_display_flush_clients(wlserver.display);
			int ret = wl_event_loop_dispatch(wlserver.event_loop, 0);
			if (ret < 0) {
				break;
			}

			wlserver_unlock();
		}
	}

	// Released when steamcompmgr closes.
	std::unique_lock<std::mutex> xwayland_server_guard(g_SteamCompMgrXWaylandServerMutex);
	// We need to shutdown Xwayland before disconnecting all clients, otherwise
	// wlroots will restart it automatically.
	wlserver.wlr.xwayland_servers.clear();
	wl_display_destroy_clients(wlserver.display);
	wl_display_destroy(wlserver.display);
}

void wlserver_keyboardfocus( struct wlr_surface *surface )
{
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard( wlserver.wlr.seat );
	if ( keyboard == nullptr )
		wlr_seat_keyboard_notify_enter( wlserver.wlr.seat, surface, nullptr, 0, nullptr);
	else
		wlr_seat_keyboard_notify_enter( wlserver.wlr.seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);

	wlserver.kb_focus_surface = surface;
}

void wlserver_key( uint32_t key, bool press, uint32_t time )
{
	assert( wlserver.wlr.virtual_keyboard_device != nullptr );
	wlr_seat_set_keyboard( wlserver.wlr.seat, wlserver.wlr.virtual_keyboard_device );
	wlr_seat_keyboard_notify_key( wlserver.wlr.seat, time, key, press );

	bump_input_counter();
}

void wlserver_mousefocus( struct wlr_surface *wlrsurface, int x /* = 0 */, int y /* = 0 */ )
{
	wlserver.mouse_focus_surface = wlrsurface;

	if ( x < wlrsurface->current.width && y < wlrsurface->current.height )
	{
		wlserver.mouse_surface_cursorx = x;
		wlserver.mouse_surface_cursory = y;
	}
	else
	{
		wlserver.mouse_surface_cursorx = wlrsurface->current.width / 2.0;
		wlserver.mouse_surface_cursory = wlrsurface->current.height / 2.0;
	}
	wlr_seat_pointer_notify_enter( wlserver.wlr.seat, wlrsurface, wlserver.mouse_surface_cursorx, wlserver.mouse_surface_cursory );
}

void wlserver_mousemotion( int x, int y, uint32_t time )
{
	// TODO: Pick the xwayland_server with active focus
	auto server = steamcompmgr_get_focused_server();
	if ( server != NULL )
	{
		XTestFakeRelativeMotionEvent( server->get_xdisplay(), x, y, CurrentTime );
		XFlush( server->get_xdisplay() );
	}
}

void wlserver_mousebutton( int button, bool press, uint32_t time )
{
	wlr_seat_pointer_notify_button( wlserver.wlr.seat, time, button, press ? WLR_BUTTON_PRESSED : WLR_BUTTON_RELEASED );
	wlr_seat_pointer_notify_frame( wlserver.wlr.seat );
}

void wlserver_mousewheel( int x, int y, uint32_t time )
{
	if ( x != 0 )
	{
		wlr_seat_pointer_notify_axis( wlserver.wlr.seat, time, WLR_AXIS_ORIENTATION_HORIZONTAL, x, x, WLR_AXIS_SOURCE_WHEEL );
	}
	if ( y != 0 )
	{
		wlr_seat_pointer_notify_axis( wlserver.wlr.seat, time, WLR_AXIS_ORIENTATION_VERTICAL, y, y, WLR_AXIS_SOURCE_WHEEL );
	}
	wlr_seat_pointer_notify_frame( wlserver.wlr.seat );
}

void wlserver_send_frame_done( struct wlr_surface *surf, const struct timespec *when )
{
	wlr_surface_send_frame_done( surf, when );
}

bool wlserver_surface_is_async( struct wlr_surface *surf )
{
	return get_wl_surface_info( surf )->presentation_hint != 0;
}

/* Handle the orientation of the touch inputs */
static void apply_touchscreen_orientation(double *x, double *y )
{
	double tx = 0;
	double ty = 0;

	switch ( g_drmEffectiveOrientation )
	{
		default:
		case DRM_MODE_ROTATE_0:
			tx = *x;
			ty = *y;
			break;
		case DRM_MODE_ROTATE_90:
			tx = 1.0 - *y;
			ty = *x;
			break;
		case DRM_MODE_ROTATE_180:
			tx = 1.0 - *x;
			ty = 1.0 - *y;
			break;
		case DRM_MODE_ROTATE_270:
			tx = *y;
			ty = 1.0 - *x;
			break;
	}

	*x = tx;
	*y = ty;
}

void wlserver_touchmotion( double x, double y, int touch_id, uint32_t time )
{
	if ( wlserver.mouse_focus_surface != NULL )
	{
		double tx = x;
		double ty = y;

		apply_touchscreen_orientation(&tx, &ty);

		tx *= g_nOutputWidth;
		ty *= g_nOutputHeight;
		tx += focusedWindowOffsetX;
		ty += focusedWindowOffsetY;
		tx *= focusedWindowScaleX;
		ty *= focusedWindowScaleY;
		wlserver.mouse_surface_cursorx = tx;
		wlserver.mouse_surface_cursory = ty;

		if ( g_nTouchClickMode == WLSERVER_TOUCH_CLICK_PASSTHROUGH )
		{
			wlr_seat_touch_notify_motion( wlserver.wlr.seat, time, touch_id, wlserver.mouse_surface_cursorx, wlserver.mouse_surface_cursory );
		}
		else if ( g_nTouchClickMode == WLSERVER_TOUCH_CLICK_DISABLED )
		{
			return;
		}
		else
		{
			g_bPendingTouchMovement = true;

			wlr_seat_pointer_notify_motion( wlserver.wlr.seat, time, wlserver.mouse_surface_cursorx, wlserver.mouse_surface_cursory );
			wlr_seat_pointer_notify_frame( wlserver.wlr.seat );
		}
	}

	bump_input_counter();
}

void wlserver_touchdown( double x, double y, int touch_id, uint32_t time )
{
	if ( wlserver.mouse_focus_surface != NULL )
	{
		double tx = x;
		double ty = y;

		apply_touchscreen_orientation(&tx, &ty);

		tx *= g_nOutputWidth;
		ty *= g_nOutputHeight;
		tx += focusedWindowOffsetX;
		ty += focusedWindowOffsetY;
		tx *= focusedWindowScaleX;
		ty *= focusedWindowScaleY;
		wlserver.mouse_surface_cursorx = tx;
		wlserver.mouse_surface_cursory = ty;

		if ( g_nTouchClickMode == WLSERVER_TOUCH_CLICK_PASSTHROUGH )
		{
			if ( touch_id >= 0 && touch_id < WLSERVER_TOUCH_COUNT )
			{
				wlr_seat_touch_notify_down( wlserver.wlr.seat, wlserver.mouse_focus_surface, time, touch_id,
											wlserver.mouse_surface_cursorx, wlserver.mouse_surface_cursory );

				wlserver.touch_down[ touch_id ] = true;
			}
		}
		else if ( g_nTouchClickMode == WLSERVER_TOUCH_CLICK_DISABLED )
		{
			return;
		}
		else
		{
			g_bPendingTouchMovement = true;

			wlr_seat_pointer_notify_motion( wlserver.wlr.seat, time, wlserver.mouse_surface_cursorx, wlserver.mouse_surface_cursory );
			wlr_seat_pointer_notify_frame( wlserver.wlr.seat );

			uint32_t button = steamcompmgr_button_to_wlserver_button( g_nTouchClickMode );

			if ( button != 0 && g_nTouchClickMode < WLSERVER_BUTTON_COUNT )
			{
				wlr_seat_pointer_notify_button( wlserver.wlr.seat, time, button, WLR_BUTTON_PRESSED );
				wlr_seat_pointer_notify_frame( wlserver.wlr.seat );

				wlserver.button_held[ g_nTouchClickMode ] = true;
			}
		}
	}

	bump_input_counter();
}

void wlserver_touchup( int touch_id, uint32_t time )
{
	if ( wlserver.mouse_focus_surface != NULL )
	{
		bool bReleasedAny = false;
		for ( int i = 0; i < WLSERVER_BUTTON_COUNT; i++ )
		{
			if ( wlserver.button_held[ i ] == true )
			{
				uint32_t button = steamcompmgr_button_to_wlserver_button( i );

				if ( button != 0 )
				{
					wlr_seat_pointer_notify_button( wlserver.wlr.seat, time, button, WLR_BUTTON_RELEASED );
					bReleasedAny = true;
				}

				wlserver.button_held[ i ] = false;
			}
		}

		if ( bReleasedAny == true )
		{
			wlr_seat_pointer_notify_frame( wlserver.wlr.seat );
		}

		if ( touch_id >= 0 && touch_id < WLSERVER_TOUCH_COUNT && wlserver.touch_down[ touch_id ] == true )
		{
			wlr_seat_touch_notify_up( wlserver.wlr.seat, time, touch_id );
			wlserver.touch_down[ touch_id ] = false;
		}
	}

	bump_input_counter();
}

gamescope_xwayland_server_t *wlserver_get_xwayland_server( size_t index )
{
	if (index >= wlserver.wlr.xwayland_servers.size() )
		return NULL;
	return wlserver.wlr.xwayland_servers[index].get();
}

const char *wlserver_get_wl_display_name( void )
{
	return wlserver.wl_display_name;
}

static void handle_x11_surface_destroy( struct wl_listener *l, void *data )
{
	struct wlserver_x11_surface_info *surf = wl_container_of( l, surf, destroy );
	wlserver_x11_surface_info_finish( surf );
	wlserver_x11_surface_info_init( surf, surf->xwayland_server, surf->x11_id );
}

static void wlserver_x11_surface_info_set_wlr( struct wlserver_x11_surface_info *surf, struct wlr_surface *wlr_surf )
{
	assert( surf->wlr == nullptr );

	wl_list_remove( &surf->pending_link );
	wl_list_init( &surf->pending_link );

	surf->destroy.notify = handle_x11_surface_destroy;
	wl_signal_add( &wlr_surf->events.destroy, &surf->destroy );

	surf->wlr = wlr_surf;
	get_wl_surface_info(wlr_surf)->x11_surface = surf;

	if ( !wlr_surface_set_role(wlr_surf, &xwayland_surface_role, NULL, NULL, 0 ) )
	{
		wl_log.errorf("Failed to set xwayland surface role");
	}

	for (auto it = g_PendingCommits.begin(); it != g_PendingCommits.end();)
	{
		if (it->surf == wlr_surf)
		{
			PendingCommit_t pending = *it;

			// Still have the buffer lock from before...
			wlserver_x11_surface_info *wlserver_x11_surface_info = get_wl_surface_info(wlr_surf)->x11_surface;
			assert(wlserver_x11_surface_info);
			assert(wlserver_x11_surface_info->xwayland_server);
			wlserver_x11_surface_info->xwayland_server->wayland_commit( pending.surf, pending.buf );

			it = g_PendingCommits.erase(it);
		}
		else
		{
			it++;
		}
	}
}

void wlserver_x11_surface_info_init( struct wlserver_x11_surface_info *surf, gamescope_xwayland_server_t *server, uint32_t x11_id )
{
	surf->wl_id = 0;
	surf->x11_id = x11_id;
	surf->wlr = nullptr;
	surf->xwayland_server = server;
	wl_list_init( &surf->pending_link );
	wl_list_init( &surf->destroy.link );
}

void gamescope_xwayland_server_t::set_wl_id( struct wlserver_x11_surface_info *surf, uint32_t id )
{
	if ( surf->wl_id != 0 )
	{
		wl_log.errorf( "surf->wl_id already set, was %u, set %u", surf->wl_id, id );
		return;
	}

	surf->wl_id = id;
	surf->wlr = nullptr;
	surf->xwayland_server = this;

	wl_list_insert( &pending_surfaces, &surf->pending_link );
	wl_list_init( &surf->destroy.link );

	struct wlr_surface *wlr_surf = nullptr;
	if ( content_overrides.count( surf->x11_id ) )
	{
		wlr_surf = content_overrides[ surf->x11_id ]->surface;
	}
	else
	{
		struct wl_resource *resource = wl_client_get_object( xwayland_server->client, id );
		if ( resource != nullptr )
			wlr_surf = wlr_surface_from_resource( resource );
	}

	if ( wlr_surf != nullptr )
		wlserver_x11_surface_info_set_wlr( surf, wlr_surf );
}

bool gamescope_xwayland_server_t::is_xwayland_ready() const
{
	return xwayland_ready;
}

_XDisplay *gamescope_xwayland_server_t::get_xdisplay()
{
	return dpy;
}

const char *gamescope_xwayland_server_t::get_nested_display_name() const
{
	return xwayland_server->display_name;
}

void wlserver_x11_surface_info_finish( struct wlserver_x11_surface_info *surf )
{
	if (surf->wlr)
	{
		wlserver_wl_surface_info *wl_info = get_wl_surface_info(surf->wlr);
		if (wl_info)
			wl_info->x11_surface = nullptr;
	}

	surf->wl_id = 0;
	surf->wlr = nullptr;
	wl_list_remove( &surf->pending_link );
	wl_list_remove( &surf->destroy.link );
}

void wlserver_set_xwayland_server_mode( size_t idx, int w, int h, int refresh )
{
	gamescope_xwayland_server_t *server = wlserver_get_xwayland_server( idx );
	if ( !server )
		return;

	struct wlr_output *output = server->get_output();
	wlr_output_set_custom_mode(output, w, h, refresh * 1000);
	if (!wlr_output_commit(output))
	{
		wl_log.errorf("Failed to commit headless output");
		abort();
	}

	wl_log.infof("Updating mode for xwayland server #%zu: %dx%d@%d", idx, w, h, refresh);
}
