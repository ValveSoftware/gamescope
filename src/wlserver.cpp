#define _GNU_SOURCE 1

#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <stdio.h> 
#include <string.h> 
#include <sys/epoll.h>

#include <map>

#include <linux/input-event-codes.h>

#include <X11/extensions/XTest.h>
#include <xkbcommon/xkbcommon.h>

#include <wayland-server-core.h>

extern "C" {
#define static
#define class class_
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/noop.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/xwayland.h>
#include <wlr/util/log.h>
#undef static
#undef class
}

#include "gamescope-xwayland-protocol.h"

#include "wlserver.hpp"
#include "drm.hpp"
#include "main.hpp"
#include "steamcompmgr.hpp"

#include "gpuvis_trace_utils.h"

struct wlserver_t wlserver;

Display *g_XWLDpy;

bool run = true;

struct wlserver_content_override {
	struct wlr_surface *surface;
	uint32_t x11_window;
	struct wl_listener surface_destroy_listener;
};

std::map<uint32_t, struct wlserver_content_override *> content_overrides;

enum wlserver_touch_click_mode g_nTouchClickMode = WLSERVER_TOUCH_CLICK_LEFT;

static struct wl_list pending_surfaces = {0};

static void wlserver_surface_set_wlr( struct wlserver_surface *surf, struct wlr_surface *wlr_surf );

void sig_handler(int signal)
{
	if ( signal == SIGUSR2 )
	{
		g_bTakeScreenshot = true;
		return;
	}

	wlr_log(WLR_DEBUG, "Received kill signal. Terminating!");
	run = false;
}

extern const struct wlr_surface_role xwayland_surface_role;

void xwayland_surface_role_commit(struct wlr_surface *wlr_surface) {
	assert(wlr_surface->role == &xwayland_surface_role);

	if ( wlr_surface->buffer == NULL )
	{
		return;
	}

	struct wlr_buffer *buf = wlr_buffer_lock( &wlr_surface->buffer->base );

	gpuvis_trace_printf( "xwayland_surface_role_commit wlr_surface %p", wlr_surface );

	wayland_commit( wlr_surface, buf );
}

static void xwayland_surface_role_precommit(struct wlr_surface *wlr_surface) {
	assert(wlr_surface->role == &xwayland_surface_role);
	struct wlr_xwayland_surface *surface = (struct wlr_xwayland_surface *) wlr_surface->role_data;
	if (surface == NULL) {
		return;
	}
}

const struct wlr_surface_role xwayland_surface_role = {
	.name = "wlr_xwayland_surface",
	.commit = xwayland_surface_role_commit,
	.precommit = xwayland_surface_role_precommit,
};

static void xwayland_ready(struct wl_listener *listener, void *data)
{
	setenv("DISPLAY", wlserver.wlr.xwayland_server->display_name, true);

	g_XWLDpy = XOpenDisplay( wlserver.wlr.xwayland_server->display_name );
	if ( g_XWLDpy == nullptr )
	{
		wlr_log( WLR_ERROR, "wlserver: failed to connect to X11 server\n" );
		exit ( 1 );
	}
	
	startSteamCompMgr();
}

struct wl_listener xwayland_ready_listener = { .notify = xwayland_ready };

static void wlserver_handle_modifiers(struct wl_listener *listener, void *data)
{
	struct wlserver_keyboard *keyboard = wl_container_of( listener, keyboard, modifiers );
	
	wlr_seat_set_keyboard( wlserver.wlr.seat, keyboard->device );
	wlr_seat_keyboard_notify_modifiers( wlserver.wlr.seat, &keyboard->device->keyboard->modifiers );
}

static void wlserver_handle_key(struct wl_listener *listener, void *data)
{
	struct wlserver_keyboard *keyboard = wl_container_of( listener, keyboard, key );
	struct wlr_event_keyboard_key *event = (struct wlr_event_keyboard_key *) data;

	xkb_keycode_t keycode = event->keycode + 8;
	xkb_keysym_t keysym = xkb_state_key_get_one_sym(keyboard->device->keyboard->xkb_state, keycode);

	if (wlserver.wlr.session && event->state == WL_KEYBOARD_KEY_STATE_PRESSED && keysym >= XKB_KEY_XF86Switch_VT_1 && keysym <= XKB_KEY_XF86Switch_VT_12) {
		unsigned vt = keysym - XKB_KEY_XF86Switch_VT_1 + 1;
		wlr_session_change_vt(wlserver.wlr.session, vt);
		return;
	}

	wlr_seat_set_keyboard( wlserver.wlr.seat, keyboard->device);
	wlr_seat_keyboard_notify_key( wlserver.wlr.seat, event->time_msec, event->keycode, event->state );
}

static void wlserver_movecursor( int x, int y )
{
	wlserver.mouse_surface_cursorx += x;
	
	if ( wlserver.mouse_surface_cursorx > wlserver.mouse_focus_surface->current.width - 1 )
	{
		wlserver.mouse_surface_cursorx = wlserver.mouse_focus_surface->current.width - 1;
	}
	
	if ( wlserver.mouse_surface_cursorx < 0 )
	{
		wlserver.mouse_surface_cursorx = 0;
	}
	
	wlserver.mouse_surface_cursory += y;
	
	if ( wlserver.mouse_surface_cursory > wlserver.mouse_focus_surface->current.height - 1 )
	{
		wlserver.mouse_surface_cursory = wlserver.mouse_focus_surface->current.height - 1;
	}
	
	if ( wlserver.mouse_surface_cursory < 0 )
	{
		wlserver.mouse_surface_cursory = 0;
	}
}

static void wlserver_handle_pointer_motion(struct wl_listener *listener, void *data)
{
	struct wlserver_pointer *pointer = wl_container_of( listener, pointer, motion );
	struct wlr_event_pointer_motion *event = (struct wlr_event_pointer_motion *) data;
	
	if ( wlserver.mouse_focus_surface != NULL )
	{
		wlserver_movecursor( event->unaccel_dx, event->unaccel_dy );

		wlr_seat_pointer_notify_motion( wlserver.wlr.seat, event->time_msec, wlserver.mouse_surface_cursorx, wlserver.mouse_surface_cursory );
		wlr_seat_pointer_notify_frame( wlserver.wlr.seat );
	}
}

static void wlserver_handle_pointer_button(struct wl_listener *listener, void *data)
{
	struct wlserver_pointer *pointer = wl_container_of( listener, pointer, button );
	struct wlr_event_pointer_button *event = (struct wlr_event_pointer_button *) data;
	
	wlr_seat_pointer_notify_button( wlserver.wlr.seat, event->time_msec, event->button, event->state );
	wlr_seat_pointer_notify_frame( wlserver.wlr.seat );
}

static inline uint32_t steamcompmgr_button_to_wlserver_button( int button )
{
	switch ( button )
	{
		default:
		case 0:
			return 0;
		case 1:
			return BTN_LEFT;
		case 2:
			return BTN_RIGHT;
		case 3:
			return BTN_MIDDLE;
	}
}

static void wlserver_handle_touch_down(struct wl_listener *listener, void *data)
{
	struct wlserver_touch *touch = wl_container_of( listener, touch, down );
	struct wlr_event_touch_down *event = (struct wlr_event_touch_down *) data;
	
	if ( wlserver.mouse_focus_surface != NULL )
	{
		double x = g_bRotated ? event->y : event->x;
		double y = g_bRotated ? 1.0 - event->x : event->y;
		
		x *= g_nOutputWidth;
		y *= g_nOutputHeight;

		x += focusedWindowOffsetX;
		y += focusedWindowOffsetY;

		x *= focusedWindowScaleX;
		y *= focusedWindowScaleY;

		if ( x < 0.0f ) x = 0.0f;
		if ( y < 0.0f ) y = 0.0f;

		if ( x > wlserver.mouse_focus_surface->current.width ) x = wlserver.mouse_focus_surface->current.width;
		if ( y > wlserver.mouse_focus_surface->current.height ) y = wlserver.mouse_focus_surface->current.height;

		wlserver.mouse_surface_cursorx = x;
		wlserver.mouse_surface_cursory = y;

		if ( g_nTouchClickMode == WLSERVER_TOUCH_CLICK_PASSTHROUGH )
		{
			if ( event->touch_id >= 0 && event->touch_id < WLSERVER_TOUCH_COUNT )
			{
				wlr_seat_touch_notify_down( wlserver.wlr.seat, wlserver.mouse_focus_surface, event->time_msec, event->touch_id,
											wlserver.mouse_surface_cursorx, wlserver.mouse_surface_cursory );

				wlserver.touch_down[ event->touch_id ] = true;
			}
		}
		else
		{
			wlr_seat_pointer_notify_motion( wlserver.wlr.seat, event->time_msec, wlserver.mouse_surface_cursorx, wlserver.mouse_surface_cursory );
			wlr_seat_pointer_notify_frame( wlserver.wlr.seat );

			uint32_t button = steamcompmgr_button_to_wlserver_button( g_nTouchClickMode );

			if ( button != 0 && g_nTouchClickMode < WLSERVER_BUTTON_COUNT )
			{
				wlr_seat_pointer_notify_button( wlserver.wlr.seat, event->time_msec, button, WLR_BUTTON_PRESSED );
				wlr_seat_pointer_notify_frame( wlserver.wlr.seat );

				wlserver.button_held[ g_nTouchClickMode ] = true;
			}
		}
	}
}

static void wlserver_handle_touch_up(struct wl_listener *listener, void *data)
{
	struct wlserver_touch *touch = wl_container_of( listener, touch, up );
	struct wlr_event_touch_up *event = (struct wlr_event_touch_up *) data;

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
					wlr_seat_pointer_notify_button( wlserver.wlr.seat, event->time_msec, button, WLR_BUTTON_RELEASED );
					bReleasedAny = true;
				}

				wlserver.button_held[ i ] = false;
			}
		}

		if ( bReleasedAny == true )
		{
			wlr_seat_pointer_notify_frame( wlserver.wlr.seat );
		}

		if ( event->touch_id >= 0 && event->touch_id < WLSERVER_TOUCH_COUNT && wlserver.touch_down[ event->touch_id ] == true )
		{
			wlr_seat_touch_notify_up( wlserver.wlr.seat, event->time_msec, event->touch_id );
			wlserver.touch_down[ event->touch_id ] = false;
		}
	}
}

static void wlserver_handle_touch_motion(struct wl_listener *listener, void *data)
{
	struct wlserver_touch *touch = wl_container_of( listener, touch, motion );
	struct wlr_event_touch_motion *event = (struct wlr_event_touch_motion *) data;
	
	if ( wlserver.mouse_focus_surface != NULL )
	{
		double x = g_bRotated ? event->y : event->x;
		double y = g_bRotated ? 1.0 - event->x : event->y;

		x *= g_nOutputWidth;
		y *= g_nOutputHeight;

		x += focusedWindowOffsetX;
		y += focusedWindowOffsetY;

		x *= focusedWindowScaleX;
		y *= focusedWindowScaleY;

		if ( x < 0.0f ) x = 0.0f;
		if ( y < 0.0f ) y = 0.0f;

		if ( x > wlserver.mouse_focus_surface->current.width ) x = wlserver.mouse_focus_surface->current.width;
		if ( y > wlserver.mouse_focus_surface->current.height ) y = wlserver.mouse_focus_surface->current.height;

		wlserver.mouse_surface_cursorx = x;
		wlserver.mouse_surface_cursory = y;

		if ( g_nTouchClickMode == WLSERVER_TOUCH_CLICK_PASSTHROUGH )
		{
			wlr_seat_touch_notify_motion( wlserver.wlr.seat, event->time_msec, event->touch_id, wlserver.mouse_surface_cursorx, wlserver.mouse_surface_cursory );
		}
		else
		{
			wlr_seat_pointer_notify_motion( wlserver.wlr.seat, event->time_msec, wlserver.mouse_surface_cursorx, wlserver.mouse_surface_cursory );
			wlr_seat_pointer_notify_frame( wlserver.wlr.seat );
		}
	}
}

static void wlserver_new_input(struct wl_listener *listener, void *data)
{
	struct wlr_input_device *device = (struct wlr_input_device *) data;

	switch ( device->type )
	{
		case WLR_INPUT_DEVICE_KEYBOARD:
		{
			struct wlserver_keyboard *pKB = (struct wlserver_keyboard *) calloc( 1, sizeof( struct wlserver_keyboard ) );
			
			pKB->device = device;
			
			struct xkb_rule_names rules = { 0 };
			struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
			struct xkb_keymap *keymap = xkb_map_new_from_names(context, &rules,
															   XKB_KEYMAP_COMPILE_NO_FLAGS);
			
			wlr_keyboard_set_keymap(device->keyboard, keymap);
			xkb_keymap_unref(keymap);
			xkb_context_unref(context);
			wlr_keyboard_set_repeat_info(device->keyboard, 25, 600);
			
			pKB->modifiers.notify = wlserver_handle_modifiers;
			wl_signal_add( &device->keyboard->events.modifiers, &pKB->modifiers );
			
			pKB->key.notify = wlserver_handle_key;
			wl_signal_add( &device->keyboard->events.key, &pKB->key );
			
			wlr_seat_set_keyboard( wlserver.wlr.seat, device );
		}
		break;
		case WLR_INPUT_DEVICE_POINTER:
		{
			struct wlserver_pointer *pointer = (struct wlserver_pointer *) calloc( 1, sizeof( struct wlserver_pointer ) );
			
			pointer->device = device;

			pointer->motion.notify = wlserver_handle_pointer_motion;
			wl_signal_add( &device->pointer->events.motion, &pointer->motion );
			pointer->button.notify = wlserver_handle_pointer_button;
			wl_signal_add( &device->pointer->events.button, &pointer->button );
		}
		break;
		case WLR_INPUT_DEVICE_TOUCH:
		{
			struct wlserver_touch *touch = (struct wlserver_touch *) calloc( 1, sizeof( struct wlserver_touch ) );
			
			touch->device = device;
			
			touch->down.notify = wlserver_handle_touch_down;
			wl_signal_add( &device->touch->events.down, &touch->down );
			touch->up.notify = wlserver_handle_touch_up;
			wl_signal_add( &device->touch->events.up, &touch->up );
			touch->motion.notify = wlserver_handle_touch_motion;
			wl_signal_add( &device->touch->events.motion, &touch->motion );
		}
		break;
		default:
			break;
	}
}

struct wl_listener new_input_listener = { .notify = wlserver_new_input };

static void wlserver_new_surface(struct wl_listener *l, void *data)
{
	struct wlr_surface *wlr_surf = (struct wlr_surface *)data;
	uint32_t id = wl_resource_get_id(wlr_surf->resource);

	struct wlserver_surface *s, *tmp;
	wl_list_for_each_safe(s, tmp, &pending_surfaces, pending_link)
	{
		if (s->wl_id == id && s->wlr == nullptr)
		{
			wlserver_surface_set_wlr( s, wlr_surf );
		}
	}
}

struct wl_listener new_surface_listener = { .notify = wlserver_new_surface };

static void destroy_content_override( struct wlserver_content_override *co )
{
	wl_list_remove( &co->surface_destroy_listener.link );
	content_overrides.erase( co->x11_window );
	free( co );
}

static void content_override_handle_surface_destroy( struct wl_listener *listener, void *data )
{
	struct wlserver_content_override *co = wl_container_of( listener, co, surface_destroy_listener );
	destroy_content_override( co );
}

static void gamescope_xwayland_handle_override_window_content( struct wl_client *client, struct wl_resource *resource, struct wl_resource *surface_resource, uint32_t x11_window )
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
	wl_global_create( wlserver.wl_display, &gamescope_xwayland_interface, version, NULL, gamescope_xwayland_bind );
}

int wlserver_init(int argc, char **argv, bool bIsNested) {
	bool bIsDRM = bIsNested == false;

	wl_list_init(&pending_surfaces);

	wlr_log_init(WLR_DEBUG, NULL);
	wlserver.wl_display = wl_display_create();

	signal(SIGTERM, sig_handler);
	signal(SIGINT, sig_handler);
	signal(SIGUSR2, sig_handler);

	wlserver.wlr.session = ( bIsDRM == True ) ? wlr_session_create(wlserver.wl_display) : NULL;

	wlserver.wl_event_loop = wl_display_get_event_loop(wlserver.wl_display);
	wlserver.wl_event_loop_fd = wl_event_loop_get_fd( wlserver.wl_event_loop );

	wlserver.wlr.multi_backend = wlr_multi_backend_create(wlserver.wl_display);

	assert( wlserver.wl_display && wlserver.wl_event_loop && wlserver.wlr.multi_backend );
	assert( !bIsDRM || wlserver.wlr.session );

	wl_signal_add( &wlserver.wlr.multi_backend->events.new_input, &new_input_listener );

	wlserver.wlr.headless_backend = wlr_headless_backend_create( wlserver.wl_display );
	if ( wlserver.wlr.headless_backend == NULL )
	{
		return 1;
	}
	wlr_multi_backend_add( wlserver.wlr.multi_backend, wlserver.wlr.headless_backend );

	wlserver.wlr.noop_backend = wlr_noop_backend_create( wlserver.wl_display );
	wlr_multi_backend_add( wlserver.wlr.multi_backend, wlserver.wlr.noop_backend );

	wlserver.wlr.output = wlr_noop_add_output( wlserver.wlr.noop_backend );

	if ( bIsDRM == True )
	{
		wlserver.wlr.libinput_backend = wlr_libinput_backend_create( wlserver.wl_display, wlserver.wlr.session );
		if ( wlserver.wlr.libinput_backend == NULL)
		{
			return 1;
		}
		wlr_multi_backend_add( wlserver.wlr.multi_backend, wlserver.wlr.libinput_backend );
	}
	else if ( wlr_backend_is_headless(wlserver.wlr.headless_backend) )
	{
		wlr_headless_add_input_device( wlserver.wlr.headless_backend, WLR_INPUT_DEVICE_KEYBOARD );
	}

	struct wlr_renderer *headless_renderer = wlr_backend_get_renderer( wlserver.wlr.multi_backend );
	assert( headless_renderer );
	wlserver.wlr.renderer = vulkan_renderer_create( headless_renderer );

	wlr_renderer_init_wl_display(wlserver.wlr.renderer, wlserver.wl_display);

	wlserver.wlr.compositor = wlr_compositor_create(wlserver.wl_display, wlserver.wlr.renderer);

	wl_signal_add( &wlserver.wlr.compositor->events.new_surface, &new_surface_listener );

	create_gamescope_xwayland();
	
	struct wlr_xwayland_server_options xwayland_options = {
		.lazy = false,
		.enable_wm = false,
	};
	wlserver.wlr.xwayland_server = wlr_xwayland_server_create(wlserver.wl_display, &xwayland_options);
	wl_signal_add(&wlserver.wlr.xwayland_server->events.ready, &xwayland_ready_listener);

	int result = -1;
	int display_slot = 0;

	while ( result != 0 && display_slot < 128 )
	{
		sprintf( wlserver.wl_display_name, "gamescope-%d", display_slot );
		result = wl_display_add_socket( wlserver.wl_display, wlserver.wl_display_name );
		display_slot++;
	}

	if ( result != 0 )
	{
		wlr_log_errno(WLR_ERROR, "Unable to open wayland socket");
		wlr_backend_destroy( wlserver.wlr.multi_backend );
		return 1;
	}

	wlserver.wlr.seat = wlr_seat_create(wlserver.wl_display, "seat0");
	wlr_seat_set_capabilities( wlserver.wlr.seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_TOUCH );

	wlr_log(WLR_INFO, "Running compositor on wayland display '%s'", wlserver.wl_display_name);

	setenv("GAMESCOPE_WAYLAND_DISPLAY", wlserver.wl_display_name, 1);

	if (!wlr_backend_start( wlserver.wlr.multi_backend ))
	{
		wlr_log(WLR_ERROR, "Failed to start backend");
		wlr_backend_destroy( wlserver.wlr.multi_backend );
		wl_display_destroy(wlserver.wl_display);
		return 1;
	}

	wlr_output_enable( wlserver.wlr.output, true );
	wlr_output_set_custom_mode( wlserver.wlr.output, g_nNestedWidth, g_nNestedHeight, g_nOutputRefresh * 1000 );
	wlr_output_commit( wlserver.wlr.output );

	wlr_output_create_global( wlserver.wlr.output );

	return 0;
}

pthread_mutex_t waylock = PTHREAD_MUTEX_INITIALIZER;

void wlserver_lock(void)
{
	pthread_mutex_lock(&waylock);
}

void wlserver_unlock( bool bFlush )
{
	if ( bFlush )
		wl_display_flush_clients(wlserver.wl_display);
	pthread_mutex_unlock(&waylock);
}

int wlserver_run(void)
{
	int epoll_fd = epoll_create( 1 );
	struct epoll_event ev;
	struct epoll_event events[128];
	int n;
	
	ev.events = EPOLLIN;
	
	if ( epoll_fd == -1 ||
		epoll_ctl( epoll_fd, EPOLL_CTL_ADD, wlserver.wl_event_loop_fd, &ev ) == -1 )
	{
		return 1;
	}

	while ( run )
	{
		n = epoll_wait( epoll_fd, events, 128, -1 );
		if ( n == -1 )
		{
			if ( errno == EINTR )
			{
				continue;
			}
			else
			{
				break;
			}
		}

		// We have wayland stuff to do, do it while locked
		wlserver_lock();
		
		for ( int i = 0; i < n; i++ )
		{
			wl_display_flush_clients(wlserver.wl_display);
			int ret = wl_event_loop_dispatch(wlserver.wl_event_loop, 0);
			if (ret < 0) {
				break;
			}
		}

		wlserver_unlock();
	}

	// We need to shutdown Xwayland before disconnecting all clients, otherwise
	// wlroots will restart it automatically.
	wlr_xwayland_server_destroy(wlserver.wlr.xwayland_server);
	wl_display_destroy_clients(wlserver.wl_display);
	wl_display_destroy(wlserver.wl_display);
	return 0;
}

void wlserver_keyboardfocus( struct wlr_surface *surface )
{
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard( wlserver.wlr.seat );
	if ( keyboard != NULL )
	{
		wlr_seat_keyboard_notify_enter( wlserver.wlr.seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}
}

void wlserver_key( uint32_t key, bool press, uint32_t time )
{
	wlr_seat_keyboard_notify_key( wlserver.wlr.seat, time, key, press );
}

void wlserver_mousefocus( struct wlr_surface *wlrsurface )
{
	wlserver.mouse_focus_surface = wlrsurface;
	wlserver.mouse_surface_cursorx = wlrsurface->current.width / 2.0;
	wlserver.mouse_surface_cursory = wlrsurface->current.height / 2.0;
	wlr_seat_pointer_notify_enter( wlserver.wlr.seat, wlrsurface, wlserver.mouse_surface_cursorx, wlserver.mouse_surface_cursory );
}

void wlserver_mousemotion( int x, int y, uint32_t time )
{
	if ( g_XWLDpy != NULL )
	{
		XTestFakeRelativeMotionEvent( g_XWLDpy, x, y, CurrentTime );
		XFlush( g_XWLDpy );
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

const char *wlserver_get_nested_display_name( void )
{
	return wlserver.wlr.xwayland_server->display_name;
}

const char *wlserver_get_wl_display_name( void )
{
	return wlserver.wl_display_name;
}

static void handle_surface_destroy( struct wl_listener *l, void *data )
{
	struct wlserver_surface *surf = wl_container_of( l, surf, destroy );
	wlserver_surface_finish( surf );
	wlserver_surface_init( surf, surf->x11_id );
}

static void wlserver_surface_set_wlr( struct wlserver_surface *surf, struct wlr_surface *wlr_surf )
{
	assert( surf->wlr == nullptr );

	wl_list_remove( &surf->pending_link );
	wl_list_init( &surf->pending_link );

	surf->destroy.notify = handle_surface_destroy;
	wl_signal_add( &wlr_surf->events.destroy, &surf->destroy );

	surf->wlr = wlr_surf;

	if ( !wlr_surface_set_role(wlr_surf, &xwayland_surface_role, NULL, NULL, 0 ) )
	{
		fprintf (stderr, "Failed to set xwayland surface role");
	}
}

void wlserver_surface_init( struct wlserver_surface *surf, long x11_id )
{
	surf->wl_id = 0;
	surf->x11_id = x11_id;
	surf->wlr = nullptr;
	wl_list_init( &surf->pending_link );
	wl_list_init( &surf->destroy.link );
}

void wlserver_surface_set_wl_id( struct wlserver_surface *surf, long id )
{
	if ( surf->wl_id != 0 )
	{
		fprintf( stderr, "surf->wl_id already set, was %lu, set %lu\n", surf->wl_id, id );
		return;
	}

	surf->wl_id = id;
	surf->wlr = nullptr;

	wl_list_insert( &pending_surfaces, &surf->pending_link );
	wl_list_init( &surf->destroy.link );

	struct wlr_surface *wlr_surf = nullptr;
	if ( content_overrides.count( surf->x11_id ) )
	{
		wlr_surf = content_overrides[ surf->x11_id ]->surface;
	}
	else
	{
		struct wl_resource *resource = wl_client_get_object( wlserver.wlr.xwayland_server->client, id );
		if ( resource != nullptr )
			wlr_surf = wlr_surface_from_resource( resource );
	}

	if ( wlr_surf != nullptr )
		wlserver_surface_set_wlr( surf, wlr_surf );
}

void wlserver_surface_finish( struct wlserver_surface *surf )
{
	if ( surf->wlr == wlserver.mouse_focus_surface )
	{
		wlserver.mouse_focus_surface = nullptr;
	}

	surf->wl_id = 0;
	surf->wlr = nullptr;
	wl_list_remove( &surf->pending_link );
	wl_list_remove( &surf->destroy.link );
}
