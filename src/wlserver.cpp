#include <xkbcommon/xkbcommon-keysyms.h>
#define _GNU_SOURCE 1

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <poll.h>
#include <fcntl.h>

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
#include <wlr/types/wlr_xdg_shell.h>
#undef static
#undef class
}

#include "gamescope-xwayland-protocol.h"
#include "gamescope-pipewire-protocol.h"
#include "gamescope-control-protocol.h"
#include "gamescope-swapchain-protocol.h"
#include "gamescope-tearing-control-unstable-v1-protocol.h"
#include "gamescope-commit-queue-v1-protocol.h"
#include "presentation-time-protocol.h"

#include "wlserver.hpp"
#include "drm.hpp"
#include "main.hpp"
#include "steamcompmgr.hpp"
#include "log.hpp"
#include "ime.hpp"
#include "xwayland_ctx.hpp"
#include "sdlwindow.hpp"

#if HAVE_PIPEWIRE
#include "pipewire.hpp"
#endif

#include "gpuvis_trace_utils.h"

#include <algorithm>
#include <list>
#include <set>

static LogScope wl_log("wlserver");

struct wlserver_t wlserver = {
	.touch_down_ids = {}
};

struct wlserver_content_override {
	gamescope_xwayland_server_t *server;
	struct wlr_surface *surface;
	uint32_t x11_window;
	struct wl_listener surface_destroy_listener;
};

enum wlserver_touch_click_mode g_nDefaultTouchClickMode = WLSERVER_TOUCH_CLICK_LEFT;
enum wlserver_touch_click_mode g_nTouchClickMode = g_nDefaultTouchClickMode;


std::mutex g_wlserver_xdg_shell_windows_lock;

static struct wl_list pending_surfaces = {0};

static void wlserver_x11_surface_info_set_wlr( struct wlserver_x11_surface_info *surf, struct wlr_surface *wlr_surf, bool override );
wlserver_wl_surface_info *get_wl_surface_info(struct wlr_surface *wlr_surf);
static enum gamescope_commit_queue_v1_queue_mode gamescope_commit_queue_v1_get_surface_mode(struct wlr_surface *surface);

std::vector<ResListEntry_t>& gamescope_xwayland_server_t::retrieve_commits()
{
	static std::vector<ResListEntry_t> commits;
	commits.clear();
	commits.reserve(16);

	{
		std::lock_guard<std::mutex> lock( wayland_commit_lock );
		commits.swap(wayland_commit_queue);
	}
	return commits;
}

void gamescope_xwayland_server_t::wayland_commit(struct wlr_surface *surf, struct wlr_buffer *buf)
{
	{
		std::lock_guard<std::mutex> lock( wayland_commit_lock );

		auto wl_surf = get_wl_surface_info( surf );

		auto queue_mode = gamescope_commit_queue_v1_get_surface_mode(surf);

		ResListEntry_t newEntry = {
			.surf = surf,
			.buf = buf,
			.async = wlserver_surface_is_async(surf),
			.fifo = queue_mode == GAMESCOPE_COMMIT_QUEUE_V1_QUEUE_MODE_FIFO,
			.feedback = wlserver_surface_swapchain_feedback(surf),
			.presentation_feedbacks = std::move(wl_surf->pending_presentation_feedbacks),
			.present_id = wl_surf->present_id,
			.desired_present_time = wl_surf->desired_present_time,
		};
		wl_surf->present_id = std::nullopt;
		wl_surf->desired_present_time = 0;
		wl_surf->pending_presentation_feedbacks.clear();

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

void wlserver_xdg_commit(struct wlr_surface *surf, struct wlr_buffer *buf)
{
	{
		std::lock_guard<std::mutex> lock( wlserver.xdg_commit_lock );

		auto wl_surf = get_wl_surface_info( surf );

		ResListEntry_t newEntry = {
			.surf = surf,
			.buf = buf,
			.async = wlserver_surface_is_async(surf),
			.feedback = wlserver_surface_swapchain_feedback(surf),
			.presentation_feedbacks = std::move(wl_surf->pending_presentation_feedbacks),
		};
		wl_surf->pending_presentation_feedbacks.clear();
		wlserver.xdg_commit_queue.push_back( newEntry );
	}

	nudge_steamcompmgr();
}

void xwayland_surface_commit(struct wlr_surface *wlr_surface) {
	wlr_surface->current.committed = 0;

	// Committing without buffer state is valid and commits the same buffer again.
	// Mutter and Weston have forward progress on the frame callback in this situation,
	// so let the commit go through. It will be duplication-eliminated later.

	VulkanWlrTexture_t *tex = (VulkanWlrTexture_t *) wlr_surface_get_texture( wlr_surface );
	if ( tex == NULL )
	{
		return;
	}

	struct wlr_buffer *buf = wlr_buffer_lock( tex->buf );

	gpuvis_trace_printf( "xwayland_surface_commit wlr_surface %p", wlr_surface );

	wlserver_x11_surface_info *wlserver_x11_surface_info = get_wl_surface_info(wlr_surface)->x11_surface;
	wlserver_xdg_surface_info *wlserver_xdg_surface_info = get_wl_surface_info(wlr_surface)->xdg_surface;
	if (wlserver_x11_surface_info)
	{
		assert(wlserver_x11_surface_info->xwayland_server);
		wlserver_x11_surface_info->xwayland_server->wayland_commit( wlr_surface, buf );
	}
	else if (wlserver_xdg_surface_info)
	{
		wlserver_xdg_commit(wlr_surface, buf);
	}
	else
	{
		g_PendingCommits.push_back(PendingCommit_t{ wlr_surface, buf });
	}
}

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

	bool forbidden_key =
		keysym == XKB_KEY_XF86AudioLowerVolume ||
		keysym == XKB_KEY_XF86AudioRaiseVolume ||
		keysym == XKB_KEY_XF86PowerOff;
	if ( ( event->state == WL_KEYBOARD_KEY_STATE_PRESSED || event->state == WL_KEYBOARD_KEY_STATE_RELEASED ) && forbidden_key )
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

static void wlserver_perform_rel_pointer_motion(double unaccel_dx, double unaccel_dy)
{
	auto server = steamcompmgr_get_focused_server();
	if ( server != NULL )
	{
		server->ctx->accum_x += unaccel_dx;
		server->ctx->accum_y += unaccel_dy;

		float dx, dy;
		server->ctx->accum_x = modf(server->ctx->accum_x, &dx);
		server->ctx->accum_y = modf(server->ctx->accum_y, &dy);

		XTestFakeRelativeMotionEvent( server->get_xdisplay(), int(dx), int(dy), CurrentTime );
		XFlush( server->get_xdisplay() );
	}
}

static void wlserver_handle_pointer_motion(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_motion_event *event = (struct wlr_pointer_motion_event *) data;

	wlserver_perform_rel_pointer_motion(event->unaccel_dx, event->unaccel_dy);
}

void wlserver_open_steam_menu( bool qam )
{
	gamescope_xwayland_server_t *server = wlserver_get_xwayland_server( 0 );
	if (!server)
		return;

	uint32_t keycode = qam ? XK_2 : XK_1;

	XTestFakeKeyEvent(server->get_xdisplay(), XKeysymToKeycode( server->get_xdisplay(), XK_Control_L ), True, CurrentTime);
	XTestFakeKeyEvent(server->get_xdisplay(), XKeysymToKeycode( server->get_xdisplay(), keycode ), True, CurrentTime);

	XTestFakeKeyEvent(server->get_xdisplay(), XKeysymToKeycode( server->get_xdisplay(), keycode ), False, CurrentTime);
	XTestFakeKeyEvent(server->get_xdisplay(), XKeysymToKeycode( server->get_xdisplay(), XK_Control_L ), False, CurrentTime);
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
		case WLSERVER_TOUCH_CLICK_TRACKPAD:
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

wlserver_wl_surface_info *get_wl_surface_info(struct wlr_surface *wlr_surf)
{
	if (!wlr_surf)
		return NULL;
	return reinterpret_cast<wlserver_wl_surface_info *>(wlr_surf->data);
}

static void handle_wl_surface_commit( struct wl_listener *l, void *data )
{
	wlserver_wl_surface_info *surf = wl_container_of( l, surf, commit );
	xwayland_surface_commit(surf->wlr);
}

static void handle_wl_surface_destroy( struct wl_listener *l, void *data )
{
	wlserver_wl_surface_info *surf = wl_container_of( l, surf, destroy );
	if (surf->x11_surface)
	{
		wlserver_x11_surface_info *x11_surface = surf->x11_surface;

		x11_surface->xwayland_server->destroy_content_override( x11_surface, surf->wlr );

		wlserver_x11_surface_info_finish(x11_surface);
		// Re-init it so it can be destroyed for good on the x11 side.
		// This just clears it out from the main wl surface mainly.
		//
		// wl_list_remove leaves stuff in a weird state, so we need to call
		// this to re-init the list to avoid a crash.
		wlserver_x11_surface_info_init(x11_surface, x11_surface->xwayland_server, x11_surface->x11_id);
	}

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

	for (auto& feedback : surf->pending_presentation_feedbacks)
		wp_presentation_feedback_send_discarded(feedback);
	surf->pending_presentation_feedbacks.clear();

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

	wl_surface_info->commit.notify = handle_wl_surface_commit;
	wl_signal_add( &wlr_surf->events.commit, &wl_surface_info->commit );

	wlr_surf->data = wl_surface_info;

	struct wlserver_x11_surface_info *s, *tmp;
	wl_list_for_each_safe(s, tmp, &pending_surfaces, pending_link)
	{
		if (s->wl_id == id && s->main_surface == nullptr)
		{
			wlserver_x11_surface_info_set_wlr( s, wlr_surf, false );
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
void gamescope_xwayland_server_t::destroy_content_override( struct wlserver_x11_surface_info *x11_surface, struct wlr_surface *surf )
{
	auto iter = content_overrides.find( x11_surface->x11_id );
	if (iter == content_overrides.end())
		return;

	if ( x11_surface->override_surface == surf )
		x11_surface->override_surface = nullptr;

	struct wlserver_content_override *co = iter->second;
	if (co->surface == surf)
		destroy_content_override(iter->second);
}


static void content_override_handle_surface_destroy( struct wl_listener *listener, void *data )
{
	struct wlserver_content_override *co = wl_container_of( listener, co, surface_destroy_listener );
	gamescope_xwayland_server_t *server = co->server;
	assert( server );
	server->destroy_content_override( co );
}

void gamescope_xwayland_server_t::handle_override_window_content( struct wl_client *client, struct wl_resource *resource, struct wlr_surface *surface, uint32_t x11_window )
{
	if ( content_overrides.count( x11_window ) ) {
		destroy_content_override( content_overrides[ x11_window ] );
	}
	wlserver_x11_surface_info *x11_surface = lookup_x11_surface_info_from_xid( this, x11_window );
	// If we found an x11_surface, go back up to our parent.
	if ( x11_surface )
		x11_window = x11_surface->x11_id;

	struct wlserver_content_override *co = (struct wlserver_content_override *)calloc(1, sizeof(*co));
	co->server = this;
	co->surface = surface;
	co->x11_window = x11_window;
	co->surface_destroy_listener.notify = content_override_handle_surface_destroy;
	wl_signal_add( &surface->events.destroy, &co->surface_destroy_listener );
	content_overrides[ x11_window ] = co;

	if ( x11_surface )
		wlserver_x11_surface_info_set_wlr( x11_surface, surface, true );

    if ( x11_surface )
    {
        for (auto it = g_PendingCommits.begin(); it != g_PendingCommits.end();)
        {
            if (it->surf == surface)
            {
                PendingCommit_t pending = *it;

                // Still have the buffer lock from before...
                assert(x11_surface);
                assert(x11_surface->xwayland_server);
                x11_surface->xwayland_server->wayland_commit( pending.surf, pending.buf );

                it = g_PendingCommits.erase(it);
            }
            else
            {
                it++;
            }
        }
    }
}

struct wl_client *gamescope_xwayland_server_t::get_client()
{
	if (!xwayland_server)
		return nullptr;

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
	struct wlr_surface *surface = wlr_surface_from_resource( surface_resource );
	server->handle_override_window_content(client, resource, surface, x11_window);
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













static void gamescope_swapchain_destroy( struct wl_client *client, struct wl_resource *resource )
{
	wlserver_wl_surface_info *wl_surface_info = (wlserver_wl_surface_info *)wl_resource_get_user_data( resource );

	wlserver_x11_surface_info *x11_surface = wl_surface_info->x11_surface;
	if (x11_surface)
		x11_surface->xwayland_server->destroy_content_override( x11_surface, wl_surface_info->wlr );

	if (wl_surface_info->gamescope_swapchain == resource)
		wl_surface_info->gamescope_swapchain = nullptr;

	wl_resource_destroy( resource );
}

static void gamescope_swapchain_override_window_content( struct wl_client *client, struct wl_resource *resource, uint32_t server_id, uint32_t x11_window )
{
	wlserver_wl_surface_info *wl_surface_info = (wlserver_wl_surface_info *)wl_resource_get_user_data( resource );

	gamescope_xwayland_server_t *server = wlserver_get_xwayland_server( server_id );
	assert( server );
	server->handle_override_window_content(client, resource, wl_surface_info->wlr, x11_window);
}

static void gamescope_swapchain_swapchain_feedback( struct wl_client *client, struct wl_resource *resource,
	uint32_t image_count,
	uint32_t vk_format,
	uint32_t vk_colorspace,
	uint32_t vk_composite_alpha,
	uint32_t vk_pre_transform,
	uint32_t vk_present_mode,
	uint32_t vk_clipped)
{
	wlserver_wl_surface_info *wl_info = (wlserver_wl_surface_info *)wl_resource_get_user_data( resource );
	if ( wl_info )
	{
		wl_info->swapchain_feedback = std::make_unique<wlserver_vk_swapchain_feedback>(wlserver_vk_swapchain_feedback{
			.image_count = image_count,
			.vk_format = VkFormat(vk_format),
			.vk_colorspace = VkColorSpaceKHR(vk_colorspace),
			.vk_composite_alpha = VkCompositeAlphaFlagBitsKHR(vk_composite_alpha),
			.vk_pre_transform = VkSurfaceTransformFlagBitsKHR(vk_pre_transform),
			.vk_present_mode = VkPresentModeKHR(vk_present_mode),
			.vk_clipped = VkBool32(vk_clipped),
			.hdr_metadata_blob = nullptr,
		});
	}
}

static void gamescope_swapchain_set_hdr_metadata( struct wl_client *client, struct wl_resource *resource,
	uint32_t display_primary_red_x,
	uint32_t display_primary_red_y,
	uint32_t display_primary_green_x,
	uint32_t display_primary_green_y,
	uint32_t display_primary_blue_x,
	uint32_t display_primary_blue_y,
	uint32_t white_point_x,
	uint32_t white_point_y,
	uint32_t max_display_mastering_luminance,
	uint32_t min_display_mastering_luminance,
	uint32_t max_cll,
	uint32_t max_fall)
{
	wlserver_wl_surface_info *wl_info = (wlserver_wl_surface_info *)wl_resource_get_user_data( resource );

	if ( wl_info )
	{
		if ( !wl_info->swapchain_feedback ) {
			wl_log.errorf("set_hdr_metadata with no swapchain_feedback.");
			return;
		}

		// Check validity of this metadata,
		// if it's garbage, just toss it...
		if (!max_cll || !max_fall || (!white_point_x && !white_point_y))
			return;

		hdr_output_metadata metadata = {};
		metadata.metadata_type = 0;

		hdr_metadata_infoframe& infoframe = metadata.hdmi_metadata_type1;
		infoframe.eotf = HDMI_EOTF_ST2084;
		infoframe.metadata_type = 0;
		infoframe.display_primaries[0].x = display_primary_red_x;
		infoframe.display_primaries[0].y = display_primary_red_y;
		infoframe.display_primaries[1].x = display_primary_green_x;
		infoframe.display_primaries[1].y = display_primary_green_y;
		infoframe.display_primaries[2].x = display_primary_blue_x;
		infoframe.display_primaries[2].y = display_primary_blue_y;
		infoframe.white_point.x = white_point_x;
		infoframe.white_point.y = white_point_y;
		infoframe.max_display_mastering_luminance = max_display_mastering_luminance;
		infoframe.min_display_mastering_luminance = min_display_mastering_luminance;
		infoframe.max_cll = max_cll;
		infoframe.max_fall = max_fall;

		wl_info->swapchain_feedback->hdr_metadata_blob =
			drm_create_hdr_metadata_blob( &g_DRM, &metadata );
	}
}

static void gamescope_swapchain_set_present_time( struct wl_client *client, struct wl_resource *resource,
	uint32_t present_id,
	uint32_t desired_present_time_hi,
	uint32_t desired_present_time_lo)
{
	wlserver_wl_surface_info *wl_info = (wlserver_wl_surface_info *)wl_resource_get_user_data( resource );

	if ( wl_info )
	{
		wl_info->present_id = present_id;
		wl_info->desired_present_time = (uint64_t(desired_present_time_hi) << 32) | desired_present_time_lo;
	}
}

static const struct gamescope_swapchain_interface gamescope_swapchain_impl = {
	.destroy = gamescope_swapchain_destroy,
	.override_window_content = gamescope_swapchain_override_window_content,
	.swapchain_feedback = gamescope_swapchain_swapchain_feedback,
	.set_hdr_metadata = gamescope_swapchain_set_hdr_metadata,
	.set_present_time = gamescope_swapchain_set_present_time,
};

static void gamescope_swapchain_factory_destroy( struct wl_client *client, struct wl_resource *resource )
{
	wl_resource_destroy( resource );
}

static void gamescope_swapchain_factory_create_swapchain( struct wl_client *client, struct wl_resource *resource, struct wl_resource *surface_resource, uint32_t id )
{
	struct wlr_surface *surface = wlr_surface_from_resource( surface_resource );

	wlserver_wl_surface_info *wl_surface_info = get_wl_surface_info(surface);

	struct wl_resource *gamescope_swapchain_resource
		= wl_resource_create( client, &gamescope_swapchain_interface, wl_resource_get_version( resource ), id );
	wl_resource_set_implementation( gamescope_swapchain_resource, &gamescope_swapchain_impl, wl_surface_info, NULL );

	if (wl_surface_info->gamescope_swapchain != nullptr)
		wl_log.errorf("create_swapchain: Surface already had a gamescope_swapchain! Overriding.");

	wl_surface_info->gamescope_swapchain = gamescope_swapchain_resource;
}

static const struct gamescope_swapchain_factory_interface gamescope_swapchain_factory_impl = {
	.destroy = gamescope_swapchain_factory_destroy,
	.create_swapchain = gamescope_swapchain_factory_create_swapchain,
};

static void gamescope_swapchain_factory_bind( struct wl_client *client, void *data, uint32_t version, uint32_t id )
{
	struct wl_resource *resource = wl_resource_create( client, &gamescope_swapchain_factory_interface, version, id );
	wl_resource_set_implementation( resource, &gamescope_swapchain_factory_impl, NULL, NULL );
}

static void create_gamescope_swapchain_factory( void )
{
	uint32_t version = 1;
	wl_global_create( wlserver.display, &gamescope_swapchain_factory_interface, version, NULL, gamescope_swapchain_factory_bind );
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

//

static void gamescope_control_set_app_target_refresh_cycle( struct wl_client *client, struct wl_resource *resource, uint32_t fps, uint32_t flags )
{
	auto display_type = gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL;
	if ( flags & GAMESCOPE_CONTROL_TARGET_REFRESH_CYCLE_FLAG_INTERNAL_DISPLAY )
		display_type = gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL;

	steamcompmgr_set_app_refresh_cycle_override( display_type, fps );
}

static void gamescope_control_handle_destroy( struct wl_client *client, struct wl_resource *resource )
{
	wl_resource_destroy( resource );
}

static const struct gamescope_control_interface gamescope_control_impl = {
	.destroy = gamescope_control_handle_destroy,
	.set_app_target_refresh_cycle = gamescope_control_set_app_target_refresh_cycle,
};

static void gamescope_control_bind( struct wl_client *client, void *data, uint32_t version, uint32_t id )
{
	struct wl_resource *resource = wl_resource_create( client, &gamescope_control_interface, version, id );
	wl_resource_set_implementation( resource, &gamescope_control_impl, NULL,
	[](struct wl_resource *resource)
	{
		std::erase_if(wlserver.gamescope_controls, [=](struct wl_resource *control) { return control == resource; });
	});

	// Send feature support
	gamescope_control_send_feature_support( resource, GAMESCOPE_CONTROL_FEATURE_RESHADE_SHADERS, 1, 0 );
	gamescope_control_send_feature_support( resource, GAMESCOPE_CONTROL_FEATURE_DISPLAY_INFO, 1, 0 );
	gamescope_control_send_feature_support( resource, GAMESCOPE_CONTROL_FEATURE_PIXEL_FILTER, 1, 0 );
	gamescope_control_send_feature_support( resource, GAMESCOPE_CONTROL_FEATURE_DONE, 0, 0 );

	if ( !BIsNested() )
	{
		drm_send_gamescope_control( resource, &g_DRM );
	}

	wlserver.gamescope_controls.push_back(resource);
}

static void create_gamescope_control( void )
{
	uint32_t version = 2;
	wl_global_create( wlserver.display, &gamescope_control_interface, version, NULL, gamescope_control_bind );
}

//

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


struct gamescope_commit_queue_v1 {
	struct wl_resource *resource;
	struct wlr_surface *surface;

	struct {
		enum gamescope_commit_queue_v1_queue_mode mode;
	} current, pending;

	struct wlr_addon surface_addon;
	struct wl_listener surface_commit;
};

extern const struct gamescope_commit_queue_v1_interface queue_impl;

// Returns NULL if inert
static struct gamescope_commit_queue_v1 *queue_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&gamescope_commit_queue_v1_interface, &queue_impl));
	return (struct gamescope_commit_queue_v1 *) wl_resource_get_user_data(resource);
}

static void resource_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void queue_destroy(struct gamescope_commit_queue_v1 *queue) {
	if (queue == NULL) {
		return;
	}
	wl_list_remove(&queue->surface_commit.link);
	wlr_addon_finish(&queue->surface_addon);
	wl_resource_set_user_data(queue->resource, NULL); // make inert
	free(queue);
}

static void surface_addon_handle_destroy(struct wlr_addon *addon) {
	struct gamescope_commit_queue_v1 *queue = wl_container_of(addon, queue, surface_addon);
	queue_destroy(queue);
}

static const struct wlr_addon_interface surface_addon_impl = {
	.name = "gamescope_commit_queue_v1",
	.destroy = surface_addon_handle_destroy,
};

static void queue_handle_set_queue_mode(struct wl_client *client,
		struct wl_resource *resource, uint32_t mode) {
	struct gamescope_commit_queue_v1 *queue = queue_from_resource(resource);

	if (mode > GAMESCOPE_COMMIT_QUEUE_V1_QUEUE_MODE_FIFO) {
		wl_resource_post_error(resource, GAMESCOPE_COMMIT_QUEUE_V1_ERROR_INVALID_QUEUE_MODE,
			"Invalid queue mode");
		return;
	}

	queue->pending.mode = (enum gamescope_commit_queue_v1_queue_mode) mode;
}

const struct gamescope_commit_queue_v1_interface queue_impl = {
	.set_queue_mode = queue_handle_set_queue_mode,
	.destroy = resource_handle_destroy,
};

static void queue_handle_surface_commit(struct wl_listener *listener, void *data) {
	struct gamescope_commit_queue_v1 *queue = wl_container_of(listener, queue, surface_commit);
	queue->current = queue->pending;
}

static void queue_handle_resource_destroy(struct wl_resource *resource) {
	struct gamescope_commit_queue_v1 *queue = queue_from_resource(resource);
	queue_destroy(queue);
}

static void manager_handle_get_queue_controller(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		struct wl_resource *surface_resource) {
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);

	if (wlr_addon_find(&surface->addons, NULL, &surface_addon_impl) != NULL) {
		wl_resource_post_error(manager_resource,
			GAMESCOPE_COMMIT_QUEUE_MANAGER_V1_ERROR_QUEUE_CONTROLLER_ALREADY_EXISTS,
			"A gamescope_commit_queue_v1 object already exists for this surface");
		return;
	}

	struct gamescope_commit_queue_v1 *queue = (struct gamescope_commit_queue_v1 *) calloc(1, sizeof(*queue));
	if (queue == NULL) {
		wl_resource_post_no_memory(manager_resource);
		return;
	}

	queue->surface = surface;

	uint32_t version = wl_resource_get_version(manager_resource);
	queue->resource = wl_resource_create(client,
		&gamescope_commit_queue_v1_interface, version, id);
	if (queue->resource == NULL) {
		free(queue);
		wl_resource_post_no_memory(manager_resource);
		return;
	}
	wl_resource_set_implementation(queue->resource,
		&queue_impl, queue, queue_handle_resource_destroy);

	wlr_addon_init(&queue->surface_addon, &surface->addons, NULL, &surface_addon_impl);

	queue->surface_commit.notify = queue_handle_surface_commit;
	wl_signal_add(&surface->events.commit, &queue->surface_commit);
}

static const struct gamescope_commit_queue_manager_v1_interface manager_impl = {
	.destroy = resource_handle_destroy,
	.get_queue_controller = manager_handle_get_queue_controller,
};

static void commit_queue_manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client,
		&gamescope_commit_queue_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

static void commit_queue_manager_v1_create(struct wl_display *display) {
	wl_global_create(display, &gamescope_commit_queue_manager_v1_interface, 1, NULL, commit_queue_manager_bind);
}

static enum gamescope_commit_queue_v1_queue_mode gamescope_commit_queue_v1_get_surface_mode(struct wlr_surface *surface) {
	struct wlr_addon *addon =
		wlr_addon_find(&surface->addons, NULL, &surface_addon_impl);
	if (addon == NULL) {
		return GAMESCOPE_COMMIT_QUEUE_V1_QUEUE_MODE_MAILBOX;
	}
	struct gamescope_commit_queue_v1 *queue = wl_container_of(addon, queue, surface_addon);
	return queue->current.mode;
}


////////////////////////
// presentation-time
////////////////////////

static void presentation_time_destroy( struct wl_client *client, struct wl_resource *resource )
{
	wl_resource_destroy( resource );
}

static void presentation_time_feedback( struct wl_client *client, struct wl_resource *resource, struct wl_resource *surface_resource, uint32_t id )
{
	struct wlr_surface *surface = wlr_surface_from_resource( surface_resource );

	wlserver_wl_surface_info *wl_surface_info = get_wl_surface_info(surface);

	struct wl_resource *presentation_feedback_resource
		= wl_resource_create( client, &wp_presentation_feedback_interface, wl_resource_get_version( resource ), id );
	wl_resource_set_implementation( presentation_feedback_resource, NULL, wl_surface_info, NULL );

	wl_surface_info->pending_presentation_feedbacks.emplace_back(presentation_feedback_resource);
}

static const struct wp_presentation_interface presentation_time_impl = {
	.destroy = presentation_time_destroy,
	.feedback = presentation_time_feedback,
};

static void presentation_time_bind( struct wl_client *client, void *data, uint32_t version, uint32_t id )
{
	struct wl_resource *resource = wl_resource_create( client, &wp_presentation_interface, version, id );
	wl_resource_set_implementation( resource, &presentation_time_impl, NULL, NULL );

	wp_presentation_send_clock_id( resource, CLOCK_MONOTONIC );
}

static void create_presentation_time( void )
{
	uint32_t version = 1;
	wl_global_create( wlserver.display, &wp_presentation_interface, version, NULL, presentation_time_bind );
}

void wlserver_presentation_feedback_presented( struct wlr_surface *surface, std::vector<struct wl_resource*>& presentation_feedbacks, uint64_t last_refresh_nsec, uint64_t refresh_cycle )
{
	wlserver_wl_surface_info *wl_surface_info = get_wl_surface_info(surface);

	if ( !wl_surface_info )
		return;

	uint32_t flags = 0;

	// Don't know when we want to send this.
	// Not useful for an app to know.
	flags |= WP_PRESENTATION_FEEDBACK_KIND_VSYNC;

	// We set HW clock because it's based on the HW clock of the last page flip.
	flags |= WP_PRESENTATION_FEEDBACK_KIND_HW_CLOCK;

	// We don't set HW_COMPLETION because we actually kinda are using a timer here.
	// We want to signal this event at latest latch time, but with the info of the refresh
	// cadence we want to track.

	// Probably. Not delaying sending this to find out.
	// Not useful for an app to know.
	flags |= WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY;

	wl_surface_info->sequence++;

	for (auto& feedback : presentation_feedbacks)
	{
		timespec last_refresh_ts;
		last_refresh_ts.tv_sec = time_t(last_refresh_nsec / 1'000'000'000ul);
		last_refresh_ts.tv_nsec = long(last_refresh_nsec % 1'000'000'000ul);

		wp_presentation_feedback_send_presented(
			feedback,
			last_refresh_ts.tv_sec >> 32,
			last_refresh_ts.tv_sec & 0xffffffff,
			last_refresh_ts.tv_nsec,
			uint32_t(refresh_cycle),
			wl_surface_info->sequence >> 32,
			wl_surface_info->sequence & 0xffffffff,
			flags);
	}

	presentation_feedbacks.clear();
}

void wlserver_presentation_feedback_discard( struct wlr_surface *surface, std::vector<struct wl_resource*>& presentation_feedbacks )
{
	wlserver_wl_surface_info *wl_surface_info = get_wl_surface_info(surface);

	if ( !wl_surface_info )
		return;

	wl_surface_info->sequence++;

	for (auto& feedback : presentation_feedbacks)
	{
		wp_presentation_feedback_send_discarded(feedback);
	}
	presentation_feedbacks.clear();
}

///////////////////////


void wlserver_past_present_timing( struct wlr_surface *surface, uint32_t present_id, uint64_t desired_present_time, uint64_t actual_present_time, uint64_t earliest_present_time, uint64_t present_margin )
{
	wlserver_wl_surface_info *wl_info = get_wl_surface_info( surface );
	if ( !wl_info )
		return;

	gamescope_swapchain_send_past_present_timing(
		wl_info->gamescope_swapchain,
		present_id,
		desired_present_time >> 32,
		desired_present_time & 0xffffffff,
		actual_present_time >> 32,
		actual_present_time & 0xffffffff,
		earliest_present_time >> 32,
		earliest_present_time & 0xffffffff,
		present_margin >> 32,
		present_margin & 0xffffffff);
}

void wlserver_refresh_cycle( struct wlr_surface *surface, uint64_t refresh_cycle )
{
	wlserver_wl_surface_info *wl_info = get_wl_surface_info( surface );
	if ( !wl_info )
		return;

	gamescope_swapchain_send_refresh_cycle(
		wl_info->gamescope_swapchain,
		refresh_cycle >> 32,
		refresh_cycle & 0xffffffff);
}

///////////////////////

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
		if (server && server->get_client() == client)
			return server->get_output() == output;
	}

	if (wlserver.wlr.xwayland_servers.empty())
		return false;

	gamescope_xwayland_server_t *server = wlserver.wlr.xwayland_servers[0].get();
	/* If we aren't an xwayland server, then only expose the first wl_output
	 * that's associated with from server 0. */
	return server && server->get_output() == output;
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
		.force_xrandr_emulation = true,
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
	/* Clear content overrides */
	for (auto& co : content_overrides)
	{
		wl_list_remove( &co.second->surface_destroy_listener.link );
		free( co.second );
	}
	content_overrides.clear();

	wlr_xwayland_server_destroy(xwayland_server);
	xwayland_server = nullptr;

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

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	struct wlserver_xdg_surface_info* info =
		wl_container_of(listener, info, map);

	info->mapped = true;
	wlserver.xdg_dirty = true;
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	struct wlserver_xdg_surface_info* info =
		wl_container_of(listener, info, unmap);

	info->mapped = false;
	wlserver.xdg_dirty = true;
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	struct wlserver_xdg_surface_info* info =
		wl_container_of(listener, info, destroy);

	wlserver_wl_surface_info *wlserver_surface = get_wl_surface_info(info->xdg_toplevel->base->surface);
	if (!wlserver_surface)
	{
		wl_log.infof("No base surface info. (destroy)");
		return;
	}

	{
		std::unique_lock lock(g_wlserver_xdg_shell_windows_lock);
		std::erase_if(wlserver.xdg_wins, [=](auto win) { return win.get() == info->win; });
	}
	info->main_surface = nullptr;
	info->win = nullptr;
	info->xdg_toplevel = nullptr;
	info->mapped = false;

	wl_list_remove(&info->map.link);
	wl_list_remove(&info->unmap.link);
	wl_list_remove(&info->destroy.link);

	wlserver_surface->xdg_surface = nullptr;
}

void xdg_surface_new(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_surface *xdg_surface = (struct wlr_xdg_surface *)data;

	if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL)
	{
		wl_log.infof("Not top level surface.");
		return;
	}

	wlserver_wl_surface_info *wlserver_surface = get_wl_surface_info(xdg_surface->surface);
	if (!wlserver_surface)
	{
		wl_log.infof("No base surface info. (new)");
		return;
	}

	auto window = std::make_shared<steamcompmgr_win_t>();
	{
		std::unique_lock lock(g_wlserver_xdg_shell_windows_lock);
		wlserver.xdg_wins.emplace_back(window);
	}

	window->seq = ++g_lastWinSeq;
	window->type = steamcompmgr_win_type_t::XDG;
	window->_window_types.emplace<steamcompmgr_xdg_win_t>();

	static uint32_t s_window_serial = 0;
	window->xdg().id = ++s_window_serial;

	wlserver_xdg_surface_info* xdg_surface_info = &window->xdg().surface;
	xdg_surface_info->main_surface = xdg_surface->surface;
	xdg_surface_info->win = window.get();
	xdg_surface_info->xdg_toplevel = xdg_surface->toplevel;

	wlserver_surface->xdg_surface = xdg_surface_info;

	xdg_surface_info->map.notify = xdg_toplevel_map;
	wl_signal_add(&xdg_surface->surface->events.map, &xdg_surface_info->map);
	xdg_surface_info->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&xdg_surface->surface->events.unmap, &xdg_surface_info->unmap);
	xdg_surface_info->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_surface->events.destroy, &xdg_surface_info->destroy);

	for (auto it = g_PendingCommits.begin(); it != g_PendingCommits.end();)
	{
		if (it->surf == xdg_surface->surface)
		{
			PendingCommit_t pending = *it;

			wlserver_xdg_commit(pending.surf, pending.buf);

			it = g_PendingCommits.erase(it);
		}
		else
		{
			it++;
		}
	}
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

	wlserver.wlr.compositor = wlr_compositor_create(wlserver.display, 5, wlserver.wlr.renderer);

	wl_signal_add( &wlserver.wlr.compositor->events.new_surface, &new_surface_listener );

	create_ime_manager( &wlserver );

	create_gamescope_xwayland();

	create_gamescope_swapchain_factory();

#if HAVE_PIPEWIRE
	create_gamescope_pipewire();
#endif

	create_gamescope_control();

	create_gamescope_tearing();

	create_presentation_time();

	commit_queue_manager_v1_create(wlserver.display);

	wlserver.xdg_shell = wlr_xdg_shell_create(wlserver.display, 3);
	if (!wlserver.xdg_shell)
	{
		wl_log.infof("Unable to create XDG shell interface");
		return false;
	}
	wlserver.new_xdg_surface.notify = xdg_surface_new;
	wl_signal_add(&wlserver.xdg_shell->events.new_surface, &wlserver.new_xdg_surface);

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

bool wlserver_is_lock_held(void)
{
	int err = pthread_mutex_trylock(&waylock);
	if (err == 0)
	{
		pthread_mutex_unlock(&waylock);
		return false;
	}
	return true;
}

void wlserver_lock(void)
{
	pthread_mutex_lock(&waylock);
}

void wlserver_unlock(bool flush)
{
    if (flush)
	    wl_display_flush_clients(wlserver.display);
	pthread_mutex_unlock(&waylock);
}

extern std::mutex g_SteamCompMgrXWaylandServerMutex;

static int g_wlserverNudgePipe[2] = {-1, -1};

void wlserver_run(void)
{
	pthread_setname_np( pthread_self(), "gamescope-wl" );

	if ( pipe2( g_wlserverNudgePipe, O_CLOEXEC | O_NONBLOCK ) != 0 )
	{
		wl_log.errorf_errno( "wlserver: pipe2 failed" );
		exit( 1 );
	}

	struct pollfd pollfds[2] = {
        {
            .fd = wl_event_loop_get_fd( wlserver.event_loop ),
            .events = POLLIN,
	    },
        {
			.fd = g_wlserverNudgePipe[ 0 ],
			.events = POLLIN,
        },
    };

	while ( g_bRun ) {
		int ret = poll( pollfds, 2, -1 );

		if ( ret < 0 ) {
			if ( errno == EINTR || errno == EAGAIN )
				continue;
			wl_log.errorf_errno( "poll failed" );
			break;
		}

		if ( pollfds[ 0 ].revents & (POLLHUP | POLLERR) ) {
			wl_log.errorf( "socket %s", ( pollfds[ 0 ].revents & POLLERR ) ? "error" : "closed" );
			break;
		}

		if ( pollfds[ 0 ].revents & POLLIN ) {
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

	{
		std::unique_lock lock3(wlserver.xdg_commit_lock);
		wlserver.xdg_commit_queue.clear();
	}

	{
		std::unique_lock lock2(g_wlserver_xdg_shell_windows_lock);
		wlserver.xdg_wins.clear();
	}

	// Released when steamcompmgr closes.
	std::unique_lock<std::mutex> xwayland_server_guard(g_SteamCompMgrXWaylandServerMutex);
	// We need to shutdown Xwayland before disconnecting all clients, otherwise
	// wlroots will restart it automatically.
	wlserver_lock();
	wlserver.wlr.xwayland_servers.clear();
	wl_display_destroy_clients(wlserver.display);
	wl_display_destroy(wlserver.display);
    wlserver.display = NULL;
	wlserver_unlock(false);
}

void wlserver_force_shutdown()
{
    assert( wlserver_is_lock_held() );

    if (wlserver.display)
    {
        if ( write( g_wlserverNudgePipe[ 1 ], "\n", 1 ) < 0 )
            wl_log.errorf_errno( "wlserver_force_shutdown: write failed" );
    }
}

void wlserver_keyboardfocus( struct wlr_surface *surface )
{
	assert( wlserver_is_lock_held() );

	if (wlserver.kb_focus_surface != surface) {
		auto old_wl_surf = get_wl_surface_info( wlserver.kb_focus_surface );
		if (old_wl_surf && old_wl_surf->xdg_surface && old_wl_surf->xdg_surface->xdg_toplevel)
			wlr_xdg_toplevel_set_activated(old_wl_surf->xdg_surface->xdg_toplevel, false);

		auto new_wl_surf = get_wl_surface_info( surface );
		if (new_wl_surf && new_wl_surf->xdg_surface && new_wl_surf->xdg_surface->xdg_toplevel)
			wlr_xdg_toplevel_set_activated(new_wl_surf->xdg_surface->xdg_toplevel, true);
	}

	assert( wlserver.wlr.virtual_keyboard_device != nullptr );
	wlr_seat_set_keyboard( wlserver.wlr.seat, wlserver.wlr.virtual_keyboard_device );

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard( wlserver.wlr.seat );
	if ( keyboard == nullptr )
		wlr_seat_keyboard_notify_enter( wlserver.wlr.seat, surface, nullptr, 0, nullptr);
	else
		wlr_seat_keyboard_notify_enter( wlserver.wlr.seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);

	wlserver.kb_focus_surface = surface;
}

void wlserver_key( uint32_t key, bool press, uint32_t time )
{
	assert( wlserver_is_lock_held() );

	assert( wlserver.wlr.virtual_keyboard_device != nullptr );
	wlr_seat_set_keyboard( wlserver.wlr.seat, wlserver.wlr.virtual_keyboard_device );
	wlr_seat_keyboard_notify_key( wlserver.wlr.seat, time, key, press );

	bump_input_counter();
}

void wlserver_mousefocus( struct wlr_surface *wlrsurface, int x /* = 0 */, int y /* = 0 */ )
{
	assert( wlserver_is_lock_held() );

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
	assert( wlserver_is_lock_held() );

	// TODO: Pick the xwayland_server with active focus
	auto server = steamcompmgr_get_focused_server();
	if ( server != NULL )
	{
		XTestFakeRelativeMotionEvent( server->get_xdisplay(), x, y, CurrentTime );
		XFlush( server->get_xdisplay() );
	}
}

void wlserver_mousewarp( int x, int y, uint32_t time )
{
	assert( wlserver_is_lock_held() );

	// TODO: Pick the xwayland_server with active focus
	auto server = steamcompmgr_get_focused_server();
	if ( server != NULL )
	{
		XTestFakeMotionEvent( server->get_xdisplay(), 0, x, y, CurrentTime );
		XFlush( server->get_xdisplay() );
	}
}

void wlserver_mousebutton( int button, bool press, uint32_t time )
{
	assert( wlserver_is_lock_held() );

	wlr_seat_pointer_notify_button( wlserver.wlr.seat, time, button, press ? WLR_BUTTON_PRESSED : WLR_BUTTON_RELEASED );
	wlr_seat_pointer_notify_frame( wlserver.wlr.seat );
}

void wlserver_mousewheel( int x, int y, uint32_t time )
{
	assert( wlserver_is_lock_held() );

	if ( x != 0 )
	{
		wlr_seat_pointer_notify_axis( wlserver.wlr.seat, time, WLR_AXIS_ORIENTATION_HORIZONTAL, x, x * WLR_POINTER_AXIS_DISCRETE_STEP, WLR_AXIS_SOURCE_WHEEL );
	}
	if ( y != 0 )
	{
		wlr_seat_pointer_notify_axis( wlserver.wlr.seat, time, WLR_AXIS_ORIENTATION_VERTICAL, y, y * WLR_POINTER_AXIS_DISCRETE_STEP, WLR_AXIS_SOURCE_WHEEL );
	}
	wlr_seat_pointer_notify_frame( wlserver.wlr.seat );
}

void wlserver_send_frame_done( struct wlr_surface *surf, const struct timespec *when )
{
	assert( wlserver_is_lock_held() );

	wlr_surface_send_frame_done( surf, when );
}

bool wlserver_surface_is_async( struct wlr_surface *surf )
{
	assert( wlserver_is_lock_held() );

	auto wl_surf = get_wl_surface_info( surf );
	if ( !wl_surf )
		return false;

	// If we are using the Gamescope WSI layer, treat both immediate and mailbox as
	// "async", this is because we have a global tearing override for games.
	// When that is enabled we want anything not FIFO or explicitly vsynced to
	// have tearing.
	if ( wl_surf->swapchain_feedback )
	{
		return wl_surf->swapchain_feedback->vk_present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR ||
			   wl_surf->swapchain_feedback->vk_present_mode == VK_PRESENT_MODE_MAILBOX_KHR;
	}

	return wl_surf->presentation_hint != 0;
}

static std::shared_ptr<wlserver_vk_swapchain_feedback> s_NullFeedback;

const std::shared_ptr<wlserver_vk_swapchain_feedback>& wlserver_surface_swapchain_feedback( struct wlr_surface *surf )
{
	assert( wlserver_is_lock_held() );

	auto wl_surf = get_wl_surface_info( surf );
	if ( !wl_surf )
		return s_NullFeedback;

	return wl_surf->swapchain_feedback;
}

/* Handle the orientation of the touch inputs */
static void apply_touchscreen_orientation(double *x, double *y )
{
	double tx = 0;
	double ty = 0;

	// Use internal screen always for orientation purposes.
	switch ( g_drmEffectiveOrientation[gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL] )
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

bool g_bTrackpadTouchExternalDisplay = false;

int get_effective_touch_mode()
{
	if (!BIsNested() && g_bTrackpadTouchExternalDisplay)
	{
		gamescope::GamescopeScreenType screenType = drm_get_screen_type(&g_DRM);
		if ( screenType == gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL && g_nTouchClickMode == WLSERVER_TOUCH_CLICK_PASSTHROUGH )
			return WLSERVER_TOUCH_CLICK_TRACKPAD;
	}

	return g_nTouchClickMode;
}

void wlserver_touchmotion( double x, double y, int touch_id, uint32_t time )
{
	assert( wlserver_is_lock_held() );

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

		double trackpad_dx, trackpad_dy;

		trackpad_dx = tx - wlserver.mouse_surface_cursorx;
		trackpad_dy = ty - wlserver.mouse_surface_cursory;

		wlserver.mouse_surface_cursorx = tx;
		wlserver.mouse_surface_cursory = ty;

		if ( get_effective_touch_mode() == WLSERVER_TOUCH_CLICK_PASSTHROUGH )
		{
			wlr_seat_touch_notify_motion( wlserver.wlr.seat, time, touch_id, wlserver.mouse_surface_cursorx, wlserver.mouse_surface_cursory );
		}
		else if ( get_effective_touch_mode() == WLSERVER_TOUCH_CLICK_DISABLED )
		{
			return;
		}
		else if ( get_effective_touch_mode() == WLSERVER_TOUCH_CLICK_TRACKPAD )
		{
			wlserver_perform_rel_pointer_motion(trackpad_dx, trackpad_dy);
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
	assert( wlserver_is_lock_held() );

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

		if ( get_effective_touch_mode() == WLSERVER_TOUCH_CLICK_PASSTHROUGH )
		{
			wlr_seat_touch_notify_down( wlserver.wlr.seat, wlserver.mouse_focus_surface, time, touch_id,
										wlserver.mouse_surface_cursorx, wlserver.mouse_surface_cursory );

			wlserver.touch_down_ids.insert( touch_id );
		}
		else if ( get_effective_touch_mode() == WLSERVER_TOUCH_CLICK_DISABLED )
		{
			return;
		}
		else
		{
			g_bPendingTouchMovement = true;

			if ( get_effective_touch_mode() != WLSERVER_TOUCH_CLICK_TRACKPAD )
			{
				wlr_seat_pointer_notify_motion( wlserver.wlr.seat, time, wlserver.mouse_surface_cursorx, wlserver.mouse_surface_cursory );
				wlr_seat_pointer_notify_frame( wlserver.wlr.seat );
			}

			uint32_t button = steamcompmgr_button_to_wlserver_button( get_effective_touch_mode() );

			if ( button != 0 && get_effective_touch_mode() < WLSERVER_BUTTON_COUNT )
			{
				wlr_seat_pointer_notify_button( wlserver.wlr.seat, time, button, WLR_BUTTON_PRESSED );
				wlr_seat_pointer_notify_frame( wlserver.wlr.seat );

				wlserver.button_held[ get_effective_touch_mode() ] = true;
			}
		}
	}

	bump_input_counter();
}

void wlserver_touchup( int touch_id, uint32_t time )
{
	assert( wlserver_is_lock_held() );

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

		if ( wlserver.touch_down_ids.count( touch_id ) > 0 )
		{
			wlr_seat_touch_notify_up( wlserver.wlr.seat, time, touch_id );
			wlserver.touch_down_ids.erase( touch_id );
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

static void wlserver_x11_surface_info_set_wlr( struct wlserver_x11_surface_info *surf, struct wlr_surface *wlr_surf, bool override )
{
	assert( wlserver_is_lock_held() );

	if (!override)
	{
		wl_list_remove( &surf->pending_link );
		wl_list_init( &surf->pending_link );
	}

	wlserver_wl_surface_info *wl_surf_info = get_wl_surface_info(wlr_surf);
	if (override)
	{
		if ( surf->override_surface )
		{
			wlserver_wl_surface_info *wl_info = get_wl_surface_info(surf->override_surface);
			if (wl_info)
				wl_info->x11_surface = nullptr;
		}

		surf->override_surface = wlr_surf;
	}
	else
	{
		if ( surf->main_surface )
		{
			wlserver_wl_surface_info *wl_info = get_wl_surface_info(surf->main_surface);
			if (wl_info)
				wl_info->x11_surface = nullptr;
		}

		surf->main_surface = wlr_surf;
	}
	wl_surf_info->x11_surface = surf;

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
	surf->main_surface = nullptr;
	surf->override_surface = nullptr;
	surf->xwayland_server = server;
	wl_list_init( &surf->pending_link );
}

void gamescope_xwayland_server_t::set_wl_id( struct wlserver_x11_surface_info *surf, uint32_t id )
{
	if (surf->wl_id)
	{
		if (surf->main_surface)
		{
			struct wl_resource *old_resource = wl_client_get_object( xwayland_server->client, surf->wl_id );
			if (!old_resource)
			{
				wl_log.errorf("wlserver_x11_surface_info had bad wl_id? Oh no! %d", surf->wl_id);
				return;
			}
			wlr_surface *old_wlr_surf = wlr_surface_from_resource( old_resource );
			if (!old_wlr_surf)
			{
				wl_log.errorf("wlserver_x11_surface_info wl_id's was not a wlr_surf?! Oh no! %d", surf->wl_id);
				return;
			}

			wlserver_wl_surface_info *old_surface_info = get_wl_surface_info(old_wlr_surf);
			old_surface_info->x11_surface = nullptr;
		}
		else
		{
			wl_list_remove( &surf->pending_link );
			wl_list_init( &surf->pending_link );
		}
	}

	surf->wl_id = id;
	surf->main_surface = nullptr;
	surf->xwayland_server = this;

	wl_list_insert( &pending_surfaces, &surf->pending_link );

	struct wlr_surface *wlr_override_surf = nullptr;
	struct wlr_surface *wlr_surf = nullptr;
	if ( content_overrides.count( surf->x11_id ) )
	{
		wlr_override_surf = content_overrides[ surf->x11_id ]->surface;
	}

	struct wl_resource *resource = wl_client_get_object( xwayland_server->client, id );
	if ( resource != nullptr )
		wlr_surf = wlr_surface_from_resource( resource );

	if ( wlr_surf != nullptr )
		wlserver_x11_surface_info_set_wlr( surf, wlr_surf, false );

	if ( wlr_override_surf != nullptr )
		wlserver_x11_surface_info_set_wlr( surf, wlr_override_surf, true );
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
	assert( wlserver_is_lock_held() );

	if (surf->main_surface)
	{
		wlserver_wl_surface_info *wl_info = get_wl_surface_info(surf->main_surface);
		if (wl_info)
			wl_info->x11_surface = nullptr;
	}

	if (surf->override_surface)
	{
		wlserver_wl_surface_info *wl_info = get_wl_surface_info(surf->override_surface);
		if (wl_info)
			wl_info->x11_surface = nullptr;
	}

	surf->wl_id = 0;
	surf->main_surface = nullptr;
	surf->override_surface = nullptr;
	wl_list_remove( &surf->pending_link );
}

void wlserver_set_xwayland_server_mode( size_t idx, int w, int h, int refresh )
{
	assert( wlserver_is_lock_held() );

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

// Definitely not very efficient if we end up with
// a lot of Wayland windows in the future.
//
// Lots of atomic reference stuff will happen whenever
// this is called with a lot of windows.
// May want to change this down the line.
//
// Given we are only expecting like 1-2 xdg-shell
// windows for our current usecases (Waydroid, etc)
// I think this is reasonable for now.
std::vector<std::shared_ptr<steamcompmgr_win_t>> wlserver_get_xdg_shell_windows()
{
	std::unique_lock lock(g_wlserver_xdg_shell_windows_lock);
	return wlserver.xdg_wins;
}

bool wlserver_xdg_dirty()
{
	return wlserver.xdg_dirty.exchange(false);
}

std::vector<ResListEntry_t> wlserver_xdg_commit_queue()
{
	std::vector<ResListEntry_t> commits;
	{
		std::lock_guard<std::mutex> lock( wlserver.xdg_commit_lock );
		commits = std::move(wlserver.xdg_commit_queue);
	}
	return commits;
}

uint32_t wlserver_make_new_xwayland_server()
{
	assert( wlserver_is_lock_held() );

	auto& server = wlserver.wlr.xwayland_servers.emplace_back(std::make_unique<gamescope_xwayland_server_t>(wlserver.display));

	while (!server->is_xwayland_ready()) {
		wl_display_flush_clients(wlserver.display);
		if (wl_event_loop_dispatch(wlserver.event_loop, -1) < 0) {
			wl_log.errorf("wl_event_loop_dispatch failed\n");
			return ~0u;
		}
	}

	return uint32_t(wlserver.wlr.xwayland_servers.size() - 1);
}

void wlserver_destroy_xwayland_server(gamescope_xwayland_server_t *server)
{
	assert( wlserver_is_lock_held() );

	std::erase_if(wlserver.wlr.xwayland_servers, [=](const auto& other) { return other.get() == server; });
}
