#include <xkbcommon/xkbcommon-keysyms.h>
#define _GNU_SOURCE 1

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <poll.h>
#include <fcntl.h>
#include <xf86drm.h>
#include <sys/eventfd.h>

#include <linux/input-event-codes.h>

#include <X11/extensions/XTest.h>
#include <xkbcommon/xkbcommon.h>

#include "WaylandServer/WaylandResource.h"
#include "WaylandServer/WaylandProtocol.h"
#include "WaylandServer/LinuxDrmSyncobj.h"
#include "WaylandServer/Reshade.h"
#include "WaylandServer/GamescopeActionBinding.h"

#include "wlr_begin.hpp"
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#if HAVE_DRM
#include <wlr/backend/libinput.h>
#endif
#include <wlr/backend/multi.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_drm.h>
#include <wlr/util/log.h>
#include <wlr/xwayland/server.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/util/region.h>
#include "wlr_end.hpp"

#include "gamescope-xwayland-protocol.h"
#include "gamescope-pipewire-protocol.h"
#include "gamescope-control-protocol.h"
#include "gamescope-private-protocol.h"
#include "gamescope-swapchain-protocol.h"
#include "presentation-time-protocol.h"

#include "wlserver.hpp"
#include "hdmi.h"
#include "main.hpp"
#include "steamcompmgr.hpp"
#include "color_helpers.h"
#include "log.hpp"
#include "ime.hpp"
#include "xwayland_ctx.hpp"
#include "refresh_rate.h"
#include "InputEmulation.h"
#include "commit.h"
#include "Timeline.h"
#include "Utils/NonCopyable.h"

#if HAVE_PIPEWIRE
#include "pipewire.hpp"
#endif

#include "gpuvis_trace_utils.h"

#include <algorithm>
#include <list>
#include <set>

static LogScope wl_log("wlserver");

using namespace std::literals;

extern gamescope::ConVar<bool> cv_drm_debug_disable_explicit_sync;

//#define GAMESCOPE_SWAPCHAIN_DEBUG

struct wlserver_t wlserver = {
	.touch_down_ids = {}
};

struct wlserver_content_override {
	gamescope_xwayland_server_t *server;
	struct wlr_surface *surface;
	uint32_t x11_window;
	struct wl_listener surface_destroy_listener;
	struct wl_resource *gamescope_swapchain;
};

std::mutex g_wlserver_xdg_shell_windows_lock;

static struct wl_list pending_surfaces = {0};

static std::atomic<bool> g_bShutdownWLServer{ false };

static void wlserver_x11_surface_info_set_wlr( struct wlserver_x11_surface_info *surf, struct wlr_surface *wlr_surf, bool override );
wlserver_wl_surface_info *get_wl_surface_info(struct wlr_surface *wlr_surf);

static void wlserver_update_cursor_constraint();
static void handle_pointer_constraint(struct wl_listener *listener, void *data);
static void wlserver_constrain_cursor( struct wlr_pointer_constraint_v1 *pNewConstraint );
struct wlr_surface *wlserver_surface_to_main_surface( struct wlr_surface *pSurface );
bool wlserver_process_hotkeys( wlr_keyboard *keyboard, uint32_t key, bool press );

extern std::atomic<bool> hasRepaint;

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

gamescope::ConVar<bool> cv_drm_debug_syncobj_force_wait_on_commit( "drm_debug_syncobj_force_wait_on_commit", false, "Force a wait on DRM sync objects before committing buffers" );

std::optional<ResListEntry_t> PrepareCommit( struct wlr_surface *surf, struct wlr_buffer *buf )
{
	auto wl_surf = get_wl_surface_info( surf );

	const auto& pFeedback = wlserver_surface_swapchain_feedback(surf);

	bool bExplicitSync = wl_surf->pSyncobjSurface && wl_surf->pSyncobjSurface->HasExplicitSync();

	std::shared_ptr<gamescope::CAcquireTimelinePoint> pAcquirePoint;
	std::shared_ptr<gamescope::CReleaseTimelinePoint> pReleasePoint;
	if ( bExplicitSync )
	{
		pAcquirePoint = wl_surf->pSyncobjSurface->ExtractAcquireTimelinePoint();
		pReleasePoint = wl_surf->pSyncobjSurface->ExtractReleaseTimelinePoint();

		if ( cv_drm_debug_syncobj_force_wait_on_commit )
		{
			if ( !pAcquirePoint->Wait() )
				wl_log.errorf( "drmSyncobjWait failed!" );
		}
	}

	auto oNewEntry = std::optional<ResListEntry_t> {
		std::in_place_t{},
		surf,
		buf,
		wlserver_surface_is_async(surf),
		wlserver_surface_is_fifo(surf),
		pFeedback,
		std::move(wl_surf->pending_presentation_feedbacks),
		wl_surf->present_id,
		wl_surf->desired_present_time,
		std::move( pAcquirePoint ),
		std::move( pReleasePoint )
	};
	wl_surf->present_id = std::nullopt;
	wl_surf->desired_present_time = 0;
	wl_surf->pending_presentation_feedbacks.clear();
	wl_surf->oCurrentPresentMode = std::nullopt;

	struct wlr_surface *pConstraintSurface = wlserver_surface_to_main_surface( surf );

	struct wlr_pointer_constraint_v1 *pConstraint = wlserver.GetCursorConstraint();
	if ( pConstraint && pConstraint->surface == pConstraintSurface )
		wlserver_update_cursor_constraint();

	return oNewEntry;
}

void gamescope_xwayland_server_t::wayland_commit(struct wlr_surface *surf, struct wlr_buffer *buf)
{
	std::optional<ResListEntry_t> oEntry = PrepareCommit( surf, buf );
	if ( !oEntry )
		return;

	{
		std::lock_guard<std::mutex> lock( wayland_commit_lock );
		wayland_commit_queue.emplace_back( std::move( *oEntry ) );
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
	std::optional<ResListEntry_t> oEntry = PrepareCommit( surf, buf );
	if ( !oEntry )
		return;

	{
		std::lock_guard<std::mutex> lock( wlserver.xdg_commit_lock );
		wlserver.xdg_commit_queue.push_back( std::move( *oEntry ) );
	}

	nudge_steamcompmgr();
}

void xwayland_surface_commit(struct wlr_surface *wlr_surface) {
	wlr_surface->current.committed = 0;

	wlserver_x11_surface_info *wlserver_x11_surface_info = get_wl_surface_info(wlr_surface)->x11_surface;
	wlserver_xdg_surface_info *wlserver_xdg_surface_info = get_wl_surface_info(wlr_surface)->xdg_surface;

	if ( wlserver_xdg_surface_info )
	{
		if ( !wlserver_xdg_surface_info->bDoneConfigure )
		{
			if ( wlserver_xdg_surface_info->xdg_surface )
				wlr_xdg_surface_schedule_configure( wlserver_xdg_surface_info->xdg_surface );

			if ( wlserver_xdg_surface_info->layer_surface )
				wlr_layer_surface_v1_configure( wlserver_xdg_surface_info->layer_surface, g_nNestedWidth, g_nNestedHeight );

			wlserver_xdg_surface_info->bDoneConfigure = true;
		}
	}

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
	struct wlr_keyboard *keyboard = &wlserver.keyboard_group->keyboard;

	wlr_seat_set_keyboard( wlserver.wlr.seat, keyboard );
	wlr_seat_keyboard_notify_modifiers( wlserver.wlr.seat, &keyboard->modifiers );

	bump_input_counter();
}

static void wlserver_handle_key(struct wl_listener *listener, void *data)
{
	struct wlr_keyboard *keyboard = &wlserver.keyboard_group->keyboard;
	struct wlr_keyboard_key_event *event = (struct wlr_keyboard_key_event *) data;

	xkb_keycode_t keycode = event->keycode + 8;
	xkb_keysym_t keysym = xkb_state_key_get_one_sym(keyboard->xkb_state, keycode);

#if HAVE_SESSION
	if (wlserver.wlr.session && event->state == WL_KEYBOARD_KEY_STATE_PRESSED && keysym >= XKB_KEY_XF86Switch_VT_1 && keysym <= XKB_KEY_XF86Switch_VT_12) {
		unsigned vt = keysym - XKB_KEY_XF86Switch_VT_1 + 1;
		wlr_session_change_vt(wlserver.wlr.session, vt);
		return;
	}
#endif

	// TODO: Remove the below hack when Steam is shipping
	// `gamescope_action_binding_manager` in Steam Stable
	// as it can just use a keybind to grab these always.
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
			wlserver_keyboardfocus( new_kb_surf, false );
			wlr_seat_set_keyboard( wlserver.wlr.seat, keyboard );
			wlr_seat_keyboard_notify_key( wlserver.wlr.seat, event->time_msec, event->keycode, event->state );
			wlserver_keyboardfocus( old_kb_surf, false );
			return;
		}
	}
	
	if ( !wlserver_process_hotkeys( keyboard, event->keycode, event->state == WL_KEYBOARD_KEY_STATE_PRESSED ) )
	{
		wlr_seat_set_keyboard( wlserver.wlr.seat, keyboard );
		wlr_seat_keyboard_notify_key( wlserver.wlr.seat, event->time_msec, event->keycode, event->state );
	}

	bump_input_counter();
}

static void wlserver_perform_rel_pointer_motion(double unaccel_dx, double unaccel_dy)
{
	assert( wlserver_is_lock_held() );

	wlr_relative_pointer_manager_v1_send_relative_motion( wlserver.relative_pointer_manager, wlserver.wlr.seat, 0, unaccel_dx, unaccel_dy, unaccel_dx, unaccel_dy );
}

static void wlserver_handle_pointer_motion(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_motion_event *event = (struct wlr_pointer_motion_event *) data;

	wlserver_mousemotion(event->unaccel_dx, event->unaccel_dy, event->time_msec);
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

	wlr_seat_pointer_notify_axis( wlserver.wlr.seat, event->time_msec, event->orientation, event->delta, event->delta_discrete, event->source, event->relative_direction );
}

static void wlserver_handle_pointer_frame(struct wl_listener *listener, void *data)
{
	wlr_seat_pointer_notify_frame( wlserver.wlr.seat );

	bump_input_counter();
}

static inline uint32_t TouchClickModeToLinuxButton( gamescope::TouchClickMode eTouchClickMode )
{
	switch ( eTouchClickMode )
	{
		default:
		case gamescope::TouchClickModes::Hover:
			return 0;
		case gamescope::TouchClickModes::Trackpad:
		case gamescope::TouchClickModes::Left:
			return BTN_LEFT;
		case gamescope::TouchClickModes::Right:
			return BTN_RIGHT;
		case gamescope::TouchClickModes::Middle:
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
			struct wlr_keyboard *keyboard = wlr_keyboard_from_input_device(device);
			wlr_keyboard_set_keymap(keyboard, wlserver.keyboard_group->keyboard.keymap);
			if (!wlr_keyboard_group_add_keyboard(wlserver.keyboard_group, keyboard)) {
				wl_log.errorf("failed to add physical keyboard %s", device->name);
				break;
			}
			// Sync the state of the modifiers and the state of the LEDs
			struct wlr_keyboard_modifiers mods = wlserver.keyboard_group->keyboard.modifiers;
			if (mods.depressed != keyboard->modifiers.depressed ||
				mods.latched != keyboard->modifiers.latched ||
				mods.locked != keyboard->modifiers.locked ||
				mods.group != keyboard->modifiers.group) {
				wlr_keyboard_notify_modifiers(keyboard,
					mods.depressed, mods.latched, mods.locked, mods.group);
			}
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

	wlserver.current_dropdown_surfaces.erase( surf->wlr );

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
	{
		wp_presentation_feedback_send_discarded(feedback);
		wl_resource_destroy(feedback);
	}
	surf->pending_presentation_feedbacks.clear();

	surf->wlr->data = nullptr;

	for ( wl_resource *pSwapchain : surf->gamescope_swapchains )
	{
		wl_resource_set_user_data( pSwapchain, nullptr );
	}

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
#ifdef GAMESCOPE_SWAPCHAIN_DEBUG
	wl_log.infof( "destroy_content_override REAL: co: %p co->surface: %p co->x11_window: 0x%x co->gamescope_swapchain: %p", co, co->surface, co->x11_window, co->gamescope_swapchain );
#endif
	if ( co->surface )
	{
		wlserver_wl_surface_info *wl_surface_info = get_wl_surface_info( co->surface );
		if ( wl_surface_info )
			wl_surface_info->x11_surface = nullptr;
	}

	if ( co->gamescope_swapchain )
	{
		gamescope_swapchain_send_retired(co->gamescope_swapchain);
	}
	wl_list_remove( &co->surface_destroy_listener.link );
	content_overrides.erase( co->x11_window );
	free( co );
}
void gamescope_xwayland_server_t::destroy_content_override( struct wlserver_x11_surface_info *x11_surface, struct wlr_surface *surf )
{
#ifdef GAMESCOPE_SWAPCHAIN_DEBUG
	wl_log.infof( "destroy_content_override LOOKUP: x11_surface: %p x11_window: 0x%x surf: %p", x11_surface, x11_surface->x11_id, surf );
#endif
	auto iter = content_overrides.find( x11_surface->x11_id );
	if (iter == content_overrides.end())
		return;

	if ( x11_surface->override_surface == surf )
		x11_surface->override_surface = nullptr;

	struct wlserver_content_override *co = iter->second;

#ifdef GAMESCOPE_SWAPCHAIN_DEBUG
	wl_log.infof( "destroy_content_override LOOKUP FOUND: x11_surface: %p x11_window: 0x%x surf: %p co: %p co->surface: %p", x11_surface, x11_surface->x11_id, surf, co, co->surface );
#endif

	co->gamescope_swapchain = nullptr;
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

static void gamescope_swapchain_destroy_co( struct wl_resource *resource );

void gamescope_xwayland_server_t::handle_override_window_content( struct wl_client *client, struct wl_resource *gamescope_swapchain_resource, struct wlr_surface *surface, uint32_t x11_window )
{
	wlserver_x11_surface_info *x11_surface = lookup_x11_surface_info_from_xid( this, x11_window );
	// If we found an x11_surface, go back up to our parent.
	if ( x11_surface )
		x11_window = x11_surface->x11_id;

#ifdef GAMESCOPE_SWAPCHAIN_DEBUG
	wl_log.infof( "handle_override_window_content: (1) x11_window: 0x%x swapchain_resource: %p surface: %p", x11_window, gamescope_swapchain_resource, surface );
#endif

	if ( content_overrides.count( x11_window ) ) {
		if ( content_overrides[x11_window]->gamescope_swapchain == gamescope_swapchain_resource )
			return;
#ifdef GAMESCOPE_SWAPCHAIN_DEBUG
		wl_log.infof( "handle_override_window_content: (2) DESTROYING x11_window: 0x%x old_swapchain: %p new_swapchain: %p", x11_window, content_overrides[x11_window]->gamescope_swapchain, gamescope_swapchain_resource );
#endif
		destroy_content_override( content_overrides[ x11_window ] );
	}

	if ( gamescope_swapchain_resource )
	{
		gamescope_swapchain_destroy_co( gamescope_swapchain_resource );
	}

	struct wlserver_content_override *co = (struct wlserver_content_override *)calloc(1, sizeof(*co));
	co->server = this;
	co->surface = surface;
	co->x11_window = x11_window;
	co->gamescope_swapchain = gamescope_swapchain_resource;
	co->surface_destroy_listener.notify = content_override_handle_surface_destroy;
	wl_signal_add( &surface->events.destroy, &co->surface_destroy_listener );
	content_overrides[ x11_window ] = co;

#ifdef GAMESCOPE_SWAPCHAIN_DEBUG
	wl_log.infof( "handle_override_window_content: (3) x11_window: 0x%x swapchain_resource: %p surface: %p co: %p", x11_window, gamescope_swapchain_resource, surface, co );
#endif

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

struct wlr_output_state *gamescope_xwayland_server_t::get_output_state()
{
	return output_state;
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
	server->handle_override_window_content(client, nullptr, surface, x11_window);
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









static void gamescope_swapchain_destroy_co( struct wl_resource *resource )
{
#ifdef GAMESCOPE_SWAPCHAIN_DEBUG
	wl_log.infof( "gamescope_swapchain_destroy_co swapchain: %p", resource );
#endif
	wlserver_wl_surface_info *wl_surface_info = (wlserver_wl_surface_info *)wl_resource_get_user_data( resource );
	if ( wl_surface_info )
	{
#ifdef GAMESCOPE_SWAPCHAIN_DEBUG
		wl_log.infof( "gamescope_swapchain_destroy_co swapchain: %p GOT WAYLAND SURFACE", resource );
#endif
		wlserver_x11_surface_info *x11_surface = wl_surface_info->x11_surface;
		if (x11_surface)
		{
#ifdef GAMESCOPE_SWAPCHAIN_DEBUG
			wl_log.infof( "gamescope_swapchain_destroy_co swapchain: %p GOT X11 SURFACE", resource );
#endif
			x11_surface->xwayland_server->destroy_content_override( x11_surface, wl_surface_info->wlr );
		}
	}
}

static void gamescope_swapchain_handle_resource_destroy( struct wl_resource *resource )
{
#ifdef GAMESCOPE_SWAPCHAIN_DEBUG
	wl_log.infof( "gamescope_swapchain_handle_resource_destroy swapchain: %p", resource );
#endif
	wlserver_wl_surface_info *wl_surface_info = (wlserver_wl_surface_info *)wl_resource_get_user_data( resource );
	if ( wl_surface_info )
	{
		gamescope_swapchain_destroy_co( resource );
		std::erase(wl_surface_info->gamescope_swapchains, resource);
	}
}

static void gamescope_swapchain_destroy( struct wl_client *client, struct wl_resource *resource )
{
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
	uint32_t vk_clipped,
	const char *vk_engine_name)
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
			.vk_clipped = VkBool32(vk_clipped),
			.vk_engine_name = std::make_shared<std::string>(vk_engine_name),
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

		wl_info->swapchain_feedback->hdr_metadata_blob = GetBackend()->CreateBackendBlob( metadata );
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

static void gamescope_swapchain_set_present_mode( struct wl_client *client, struct wl_resource *resource, uint32_t present_mode )
{
	wlserver_wl_surface_info *wl_info = (wlserver_wl_surface_info *)wl_resource_get_user_data( resource );

	if ( wl_info )
	{
		wl_info->oCurrentPresentMode = VkPresentModeKHR( present_mode );
	}
}

static const struct gamescope_swapchain_interface gamescope_swapchain_impl = {
	.destroy = gamescope_swapchain_destroy,
	.override_window_content = gamescope_swapchain_override_window_content,
	.swapchain_feedback = gamescope_swapchain_swapchain_feedback,
	.set_present_mode = gamescope_swapchain_set_present_mode,
	.set_hdr_metadata = gamescope_swapchain_set_hdr_metadata,
	.set_present_time = gamescope_swapchain_set_present_time,
};

static void gamescope_swapchain_factory_v2_destroy( struct wl_client *client, struct wl_resource *resource )
{
	wl_resource_destroy( resource );
}

static void gamescope_swapchain_factory_v2_create_swapchain( struct wl_client *client, struct wl_resource *resource, struct wl_resource *surface_resource, uint32_t id )
{
	struct wlr_surface *surface = wlr_surface_from_resource( surface_resource );

	wlserver_wl_surface_info *wl_surface_info = get_wl_surface_info(surface);

	struct wl_resource *gamescope_swapchain_resource
		= wl_resource_create( client, &gamescope_swapchain_interface, wl_resource_get_version( resource ), id );
	wl_resource_set_implementation( gamescope_swapchain_resource, &gamescope_swapchain_impl, wl_surface_info, gamescope_swapchain_handle_resource_destroy );

	if (wl_surface_info->gamescope_swapchains.size())
		wl_log.errorf("create_swapchain: Surface already had a gamescope_swapchain! Warning!");

	wl_surface_info->gamescope_swapchains.emplace_back( gamescope_swapchain_resource );
}

static const struct gamescope_swapchain_factory_v2_interface gamescope_swapchain_factory_v2_impl = {
	.destroy = gamescope_swapchain_factory_v2_destroy,
	.create_swapchain = gamescope_swapchain_factory_v2_create_swapchain,
};

static void gamescope_swapchain_factory_v2_bind( struct wl_client *client, void *data, uint32_t version, uint32_t id )
{
	struct wl_resource *resource = wl_resource_create( client, &gamescope_swapchain_factory_v2_interface, version, id );
	wl_resource_set_implementation( resource, &gamescope_swapchain_factory_v2_impl, NULL, NULL );
}

static void create_gamescope_swapchain_factory_v2( void )
{
	uint32_t version = 1;
	wl_global_create( wlserver.display, &gamescope_swapchain_factory_v2_interface, version, NULL, gamescope_swapchain_factory_v2_bind );
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

	steamcompmgr_set_app_refresh_cycle_override(
		display_type,
		fps,
		!!( flags & GAMESCOPE_CONTROL_TARGET_REFRESH_CYCLE_FLAG_ALLOW_REFRESH_SWITCHING ),
		!( flags & GAMESCOPE_CONTROL_TARGET_REFRESH_CYCLE_FLAG_ONLY_CHANGE_REFRESH_RATE ) );
}

static void gamescope_control_take_screenshot( struct wl_client *client, struct wl_resource *resource, const char *path, uint32_t type, uint32_t flags )
{
	gamescope::CScreenshotManager::Get().TakeScreenshot( gamescope::GamescopeScreenshotInfo
	{
		.szScreenshotPath = path,
		.eScreenshotType  = (gamescope_control_screenshot_type)type,
		.uScreenshotFlags = flags,
		.bWaylandRequested = true,
	} );
}

void drm_sleep_screen( gamescope::GamescopeScreenType eType, bool bSleep );

static void gamescope_control_display_sleep( struct wl_client *client, struct wl_resource *resource, uint32_t display_type_flags, uint32_t flags )
{
	if ( flags & ( GAMESCOPE_CONTROL_DISPLAY_SLEEP_FLAGS_SLEEP | GAMESCOPE_CONTROL_DISPLAY_SLEEP_FLAGS_WAKE ) )
	{
		const bool sleep = !!( flags & GAMESCOPE_CONTROL_DISPLAY_SLEEP_FLAGS_SLEEP );
		if ( display_type_flags & GAMESCOPE_CONTROL_DISPLAY_TYPE_FLAGS_EXTERNAL_DISPLAY )
			drm_sleep_screen( gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL, sleep );

		if ( display_type_flags & GAMESCOPE_CONTROL_DISPLAY_TYPE_FLAGS_INTERNAL_DISPLAY )
			drm_sleep_screen( gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL, sleep );
	}
}

extern gamescope::ConVar<bool> cv_overlay_unmultiplied_alpha;
extern std::atomic<std::shared_ptr<lut3d_t>> g_ColorMgmtLooks[EOTF_Count];

static gamescope::ConCommand cc_set_look("set_look", "Set a look for a specific EOTF. Eg. set_look mylook.cube (g22 only), set_look pq mylook.cube, set_look mylook_g22.cube mylook_pq.cube",
[]( std::span<std::string_view> args )
{
	if ( args.size() == 2 )
	{
		std::string arg1 = std::string{ args[1] };
		bool bRaisesBlackLevelFloor = false;
		g_ColorMgmtLooks[ EOTF_Gamma22 ] = LoadCubeLut( arg1.c_str(), bRaisesBlackLevelFloor );
		cv_overlay_unmultiplied_alpha = bRaisesBlackLevelFloor;
		g_ColorMgmt.pending.externalDirtyCtr++;
		hasRepaint = true;
	}
	else if ( args.size() == 3 )
	{
		std::string arg2 = std::string{ args[2] };

		bool bRaisesBlackLevelFloor = false;
		if ( args[1] == "g22" || args[1] == "G22")
		{
			g_ColorMgmtLooks[ EOTF_Gamma22 ] = LoadCubeLut( arg2.c_str(), bRaisesBlackLevelFloor );
		}
		else if ( args[1] == "pq" || args[1] == "PQ" )
		{
			g_ColorMgmtLooks[ EOTF_PQ ] = LoadCubeLut( arg2.c_str(), bRaisesBlackLevelFloor );
		}
		else
		{
			std::string arg1 = std::string{ args[1] };

			std::shared_ptr<lut3d_t> pG22LUT;
			std::shared_ptr<lut3d_t> pPQLUT;

			bool bDummy = false;
			pG22LUT = LoadCubeLut( arg1.c_str(), bRaisesBlackLevelFloor );
			pPQLUT = LoadCubeLut( arg2.c_str(), bDummy );

			g_ColorMgmtLooks[ EOTF_Gamma22 ] = pG22LUT;
			g_ColorMgmtLooks[ EOTF_PQ ] = pPQLUT;
		}
		cv_overlay_unmultiplied_alpha = bRaisesBlackLevelFloor;
		g_ColorMgmt.pending.externalDirtyCtr++;
		hasRepaint = true;
	}
	else
	{
		cv_overlay_unmultiplied_alpha = false;
		g_ColorMgmtLooks[ EOTF_Gamma22 ] = nullptr;
		g_ColorMgmtLooks[ EOTF_PQ ] = nullptr;
		g_ColorMgmt.pending.externalDirtyCtr++;
		hasRepaint = true;
	}
});

static void gamescope_control_set_look( struct wl_client *client, struct wl_resource *resource, int g22_fd, int pq_fd, uint32_t flags )
{
	std::shared_ptr<lut3d_t> pG22LUT;
	std::shared_ptr<lut3d_t> pPQLUT;
	bool bRaisesBlackLevelFloor = false;

	if ( g22_fd >= 0 )
	{
		// takes ownership of FD.
		FILE *pG22 = fdopen( g22_fd, "r" );
		if ( pG22 )
		{
			pG22LUT = LoadCubeLut( pG22, bRaisesBlackLevelFloor );
			fclose( pG22 );
		}
		else
		{
			wl_log.errorf_errno( "gamescope_control_set_look error opening g22 lut" );
			close( g22_fd );
		}
		g22_fd = -1;
	}

	if ( pq_fd >= 0 )
	{
		// takes ownership of FD.
		FILE *pPQ = fdopen( pq_fd, "r" );
		if ( pPQ )
		{
			bool bDummy = false;
			pPQLUT = LoadCubeLut( pPQ, bDummy );
			fclose( pPQ );
		}
		else
		{
			wl_log.errorf_errno( "gamescope_control_set_look error opening pq lut" );
			close( pq_fd );
		}
		pq_fd = -1;
	}

	cv_overlay_unmultiplied_alpha = bRaisesBlackLevelFloor;
	g_ColorMgmtLooks[ EOTF_Gamma22 ] = pG22LUT;
	g_ColorMgmtLooks[ EOTF_PQ ] = pPQLUT;
	g_ColorMgmt.pending.externalDirtyCtr++;
	hasRepaint = true;
}

static void gamescope_control_unset_look( struct wl_client *client, struct wl_resource *resource )
{
	cv_overlay_unmultiplied_alpha = false;
	g_ColorMgmtLooks[ EOTF_Gamma22 ] = nullptr;
	g_ColorMgmtLooks[ EOTF_PQ ] = nullptr;
	g_ColorMgmt.pending.externalDirtyCtr++;
	hasRepaint = true;
}

static void gamescope_control_handle_destroy( struct wl_client *client, struct wl_resource *resource )
{
	wl_resource_destroy( resource );
}

static void gamescope_control_request_app_performance_stats( struct wl_client *client, struct wl_resource *resource, uint32_t app_id )
{
	assert( wlserver_is_lock_held() );
	wlserver.app_perf_requests[ app_id ].push_back( resource );
}

void wlserver_app_presented( uint32_t app_id, uint64_t frametime_ns )
{
	assert( wlserver_is_lock_held() );

	auto it = wlserver.app_perf_requests.find( app_id );
	if ( it == wlserver.app_perf_requests.end() )
		return;

	// Fire the response to everyone that requested it
	for ( wl_resource *resource : it->second )
	{
		uint32_t frametime_hi = frametime_ns >> 32;
		uint32_t frametime_lo = frametime_ns & 0xffffffff;

		gamescope_control_send_app_performance_stats( resource, app_id, frametime_lo, frametime_hi );
	}

	// Event fired, clear the request until we get the next one
	wlserver.app_perf_requests.erase( it );
}

static const struct gamescope_control_interface gamescope_control_impl = {
	.destroy = gamescope_control_handle_destroy,
	.set_app_target_refresh_cycle = gamescope_control_set_app_target_refresh_cycle,
	.take_screenshot = gamescope_control_take_screenshot,
	.display_sleep = gamescope_control_display_sleep,
	.set_look = gamescope_control_set_look,
	.unset_look = gamescope_control_unset_look,
	.request_app_performance_stats = gamescope_control_request_app_performance_stats,
};

static uint32_t get_conn_display_info_flags()
{
	gamescope::IBackendConnector *pConn = GetBackend()->GetCurrentConnector();

	if ( !pConn )
		return 0;

	uint32_t flags = 0;
	if ( pConn->GetScreenType() == gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL )
		flags |= GAMESCOPE_CONTROL_DISPLAY_FLAG_INTERNAL_DISPLAY;
	if ( pConn->SupportsVRR() )
		flags |= GAMESCOPE_CONTROL_DISPLAY_FLAG_SUPPORTS_VRR;
	if ( pConn->GetHDRInfo().bExposeHDRSupport )
		flags |= GAMESCOPE_CONTROL_DISPLAY_FLAG_SUPPORTS_HDR;

	return flags;
}

void wlserver_send_gamescope_control( wl_resource *control )
{
	assert( wlserver_is_lock_held() );

	gamescope::IBackendConnector *pConn = GetBackend()->GetCurrentConnector();
	if ( !pConn )
		return;

	uint32_t flags = get_conn_display_info_flags();

	struct wl_array display_rates;
	wl_array_init(&display_rates);
	if ( pConn->GetValidDynamicRefreshRates().size() )
	{
		for ( uint32_t uRateHz : pConn->GetValidDynamicRefreshRates() )
		{
			uint32_t *ptr = (uint32_t *)wl_array_add( &display_rates, sizeof( uint32_t ) );
			*ptr = uRateHz;
		}
	}
	else if ( g_nOutputRefresh > 0 )
	{
		uint32_t *ptr = (uint32_t *)wl_array_add( &display_rates, sizeof(uint32_t) );
		*ptr = (uint32_t)gamescope::ConvertmHzToHz( g_nOutputRefresh );
	}
	gamescope_control_send_active_display_info( control, pConn->GetName(), pConn->GetMake(), pConn->GetModel(), flags, &display_rates );
	wl_array_release(&display_rates);
}

static void gamescope_control_bind( struct wl_client *client, void *data, uint32_t version, uint32_t id )
{
	struct wl_resource *resource = wl_resource_create( client, &gamescope_control_interface, version, id );
	wl_resource_set_implementation( resource, &gamescope_control_impl, NULL,
	[](struct wl_resource *resource)
	{
		std::erase_if(wlserver.gamescope_controls, [=](struct wl_resource *control) { return control == resource; });
		for ( auto &[ app_id, resources] : wlserver.app_perf_requests )
		{
			std::erase_if( resources, [=]( struct wl_resource *control ) { return control == resource; } );
		}
	});

	// Send feature support
	gamescope_control_send_feature_support( resource, GAMESCOPE_CONTROL_FEATURE_RESHADE_SHADERS, 1, 0 );
	gamescope_control_send_feature_support( resource, GAMESCOPE_CONTROL_FEATURE_DISPLAY_INFO, 1, 0 );
	gamescope_control_send_feature_support( resource, GAMESCOPE_CONTROL_FEATURE_PIXEL_FILTER, 1, 0 );
	gamescope_control_send_feature_support( resource, GAMESCOPE_CONTROL_FEATURE_REFRESH_CYCLE_ONLY_CHANGE_REFRESH_RATE, 1, 0 );
	gamescope_control_send_feature_support( resource, GAMESCOPE_CONTROL_FEATURE_MURA_CORRECTION, 1, 0 );
	gamescope_control_send_feature_support( resource, GAMESCOPE_CONTROL_FEATURE_LOOK, 1, 0 );
	gamescope_control_send_feature_support( resource, GAMESCOPE_CONTROL_FEATURE_PERF_QUERY, 1, 0 );
	gamescope_control_send_feature_support( resource, GAMESCOPE_CONTROL_FEATURE_DONE, 0, 0 );

	wlserver_send_gamescope_control( resource );

	wlserver.gamescope_controls.push_back(resource);
}

static void create_gamescope_control( void )
{
	uint32_t version = 6;
	wl_global_create( wlserver.display, &gamescope_control_interface, version, NULL, gamescope_control_bind );
}

////////////////////////
// gamescope_private
////////////////////////

static void gamescope_private_execute( struct wl_client *client, struct wl_resource *resource, const char *cvar_name, const char *value )
{
	std::vector<std::string_view> args;
	args.emplace_back( cvar_name );
	args.emplace_back( value );
	if ( gamescope::ConCommand::Exec( std::span<std::string_view>{ args } ) )
		gamescope_private_send_command_executed( resource );
}

static void gamescope_private_handle_destroy( struct wl_client *client, struct wl_resource *resource )
{
	wl_resource_destroy( resource );
}

static const struct gamescope_private_interface gamescope_private_impl = {
	.destroy = gamescope_private_handle_destroy,
	.execute = gamescope_private_execute,
};

static void gamescope_private_bind( struct wl_client *client, void *data, uint32_t version, uint32_t id )
{
	struct wl_resource *resource = wl_resource_create( client, &gamescope_private_interface, version, id );
	console_log.m_LoggingListeners[(uintptr_t)resource] = [ resource ]( LogPriority ePriority, std::string_view psvScope, std::string_view psvText )
	{
		if ( !wlserver_is_lock_held() )
			return;
		
		// Can't send a length with string on Wayland api currently... :(
		std::string sText = std::string{ psvText };
		gamescope_private_send_log( resource, sText.c_str() );
	};
	wl_resource_set_implementation( resource, &gamescope_private_impl, NULL,
		[](struct wl_resource *resource)
	{
		console_log.m_LoggingListeners.erase( (uintptr_t)resource );
	});

}

static void create_gamescope_private( void )
{
	uint32_t version = 1;
	wl_global_create( wlserver.display, &gamescope_private_interface, version, NULL, gamescope_private_bind );
}

static void create_explicit_sync()
{
	new gamescope::WaylandServer::CLinuxDrmSyncobj( wlserver.display );
}

static void create_reshade()
{
	new gamescope::WaylandServer::CReshade( wlserver.display );
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
		wl_resource_destroy(feedback);
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

	for (auto& swapchain : wl_info->gamescope_swapchains) {
		gamescope_swapchain_send_past_present_timing(
			swapchain,
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
}

void wlserver_refresh_cycle( struct wlr_surface *surface, uint64_t refresh_cycle )
{
	wlserver_wl_surface_info *wl_info = get_wl_surface_info( surface );
	if ( !wl_info )
		return;

	for (auto& swapchain : wl_info->gamescope_swapchains) {
		gamescope_swapchain_send_refresh_cycle(
			swapchain,
			refresh_cycle >> 32,
			refresh_cycle & 0xffffffff);
	}
}

///////////////////////

#if HAVE_SESSION
bool wlsession_active()
{
	return wlserver.wlr.session->active;
}

static void handle_session_active( struct wl_listener *listener, void *data )
{
	GetBackend()->DirtyState( wlserver.wlr.session->active, wlserver.wlr.session->active );
	wl_log.infof( "Session %s", wlserver.wlr.session->active ? "resumed" : "paused" );
}
#endif

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

	if ( cv_drm_debug_disable_explicit_sync && iface->name == "wp_linux_drm_syncobj_manager_v1"sv )
		return false;

	if (strcmp(iface->name, wl_output_interface.name) != 0)
		return true;

	struct wlr_output *output = (struct wlr_output *) wl_global_get_user_data(global);
	if (!output)
	{
		/* Can happen if the output has been destroyed, but the client hasn't
		 * received the wl_registry.global_remove event yet. */
		return false;
	}

	if (g_bShutdownWLServer)
		return false;

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
	wlserver.event_loop = wl_display_get_event_loop( wlserver.display );
	wlserver.wlr.headless_backend = wlr_headless_backend_create( wlserver.event_loop );

	wl_display_set_global_filter(wlserver.display, filter_global, nullptr);

	const struct wlserver_output_info output_info = {
		.description = "Virtual gamescope output",
	};
	wlserver_set_output_info( &output_info );

#if HAVE_SESSION
	if ( !GetBackend()->IsSessionBased() )
		return true;

	wlserver.wlr.session = wlr_session_create( wlserver.event_loop );
	if ( wlserver.wlr.session == nullptr )
	{
		wl_log.errorf( "Failed to create session" );
		return false;
	}

	wlserver.session_active.notify = handle_session_active;
	wl_signal_add( &wlserver.wlr.session->events.active, &wlserver.session_active );
#endif

	return true;
}

#if HAVE_SESSION

static void kms_device_handle_change( struct wl_listener *listener, void *data )
{
	GetBackend()->DirtyState();
	wl_log.infof( "Got change event for KMS device" );

	nudge_steamcompmgr();
}

int wlsession_open_kms( const char *device_name ) {
	if ( device_name != nullptr )
	{
		wlserver.wlr.device = wlr_session_open_file( wlserver.wlr.session, device_name );
		if ( wlserver.wlr.device == nullptr )
			return -1;
	}
	else
	{
		ssize_t n = wlr_session_find_gpus( wlserver.wlr.session, 1, &wlserver.wlr.device );
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
	wl_signal_add( &wlserver.wlr.device->events.change, listener );

	return wlserver.wlr.device->fd;
}

void wlsession_close_kms()
{
	wlr_session_close_file( wlserver.wlr.session, wlserver.wlr.device );
}

#endif

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
	output_state = new wlr_output_state;
	wlr_output_state_init(output_state);

	output->make = strdup("gamescope");  // freed by wlroots
	output->model = strdup("gamescope"); // freed by wlroots
	wlr_output_set_name(output, "gamescope");

	int refresh = g_nNestedRefresh;
	if (refresh == 0) {
		refresh = g_nOutputRefresh;
	}

	wlr_output_state_set_enabled(output_state, true);
	wlr_output_state_set_custom_mode(output_state, g_nNestedWidth, g_nNestedHeight, refresh);
	if (!wlr_output_commit_state(output, output_state))
	{
		wl_log.errorf("Failed to commit headless output");
		abort();
	}

	update_output_info();

	wlr_output_create_global(output, wlserver.display);
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
	delete output_state;	
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

static void xdg_surface_map(struct wl_listener *listener, void *data) {
	struct wlserver_xdg_surface_info* info =
		wl_container_of(listener, info, map);

	info->mapped = true;
	wlserver.xdg_dirty = true;
}

static void xdg_surface_unmap(struct wl_listener *listener, void *data) {
	struct wlserver_xdg_surface_info* info =
		wl_container_of(listener, info, unmap);

	info->mapped = false;
	wlserver.xdg_dirty = true;
}

static void waylandy_surface_destroy(struct wl_listener *listener, void *data) {
	struct wlserver_xdg_surface_info* info =
		wl_container_of(listener, info, destroy);

	wlserver_wl_surface_info *wlserver_surface = get_wl_surface_info(info->main_surface);
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
	info->xdg_surface = nullptr;
	info->layer_surface = nullptr;
	info->mapped = false;

	wl_list_remove(&info->map.link);
	wl_list_remove(&info->unmap.link);
	wl_list_remove(&info->destroy.link);

	wlserver_surface->xdg_surface = nullptr;
}

void xdg_toplevel_new(struct wl_listener *listener, void *data)
{
}

uint32_t get_appid_from_pid( pid_t pid );

wlserver_xdg_surface_info* waylandy_type_surface_new(struct wl_client *client, struct wlr_surface *surface)
{
	wlserver_wl_surface_info *wlserver_surface = get_wl_surface_info(surface);
	if (!wlserver_surface)
	{
		wl_log.infof("No base surface info. (new)");
		return nullptr;
	}

	auto window = std::make_shared<steamcompmgr_win_t>();
	{
		std::unique_lock lock(g_wlserver_xdg_shell_windows_lock);
		wlserver.xdg_wins.emplace_back(window);
	}

	window->seq = ++g_lastWinSeq;
	window->type = steamcompmgr_win_type_t::XDG;
	if ( client )
	{
		pid_t nPid = 0;
		wl_client_get_credentials( client, &nPid, nullptr, nullptr );
		window->appID = get_appid_from_pid( nPid );
	}
	window->_window_types.emplace<steamcompmgr_xdg_win_t>();

	static uint32_t s_window_serial = 0;
	window->xdg().id = ++s_window_serial;

	wlserver_xdg_surface_info* xdg_surface_info = &window->xdg().surface;
	xdg_surface_info->main_surface = surface;
	xdg_surface_info->win = window.get();

	wlserver_surface->xdg_surface = xdg_surface_info;

	xdg_surface_info->map.notify = xdg_surface_map;
	wl_signal_add(&surface->events.map, &xdg_surface_info->map);
	xdg_surface_info->unmap.notify = xdg_surface_unmap;
	wl_signal_add(&surface->events.unmap, &xdg_surface_info->unmap);

	for (auto it = g_PendingCommits.begin(); it != g_PendingCommits.end();)
	{
		if (it->surf == surface)
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

	return xdg_surface_info;
}

void xdg_surface_new(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_surface *xdg_surface = (struct wlr_xdg_surface *)data;

	wlserver_xdg_surface_info *surface_info = waylandy_type_surface_new(xdg_surface->client->client, xdg_surface->surface);
	surface_info->destroy.notify = waylandy_surface_destroy;
	wl_signal_add(&xdg_surface->events.destroy, &surface_info->destroy);

	surface_info->xdg_surface = xdg_surface;
}


void layer_shell_surface_new(struct wl_listener *listener, void *data)
{
	struct wlr_layer_surface_v1 *layer_surface = (struct wlr_layer_surface_v1 *)data;

	wlserver_xdg_surface_info *surface_info = waylandy_type_surface_new(nullptr, layer_surface->surface);
	surface_info->destroy.notify = waylandy_surface_destroy;
	wl_signal_add(&layer_surface->events.destroy, &surface_info->destroy);

	surface_info->layer_surface = layer_surface;

	surface_info->win->isExternalOverlay = true;
}

#if HAVE_LIBEIS
static gamescope::CAsyncWaiter g_LibEisWaiter( "gamescope-eis" );
// TODO: Move me into some ownership of eg. wlserver.
static std::unique_ptr<gamescope::GamescopeInputServer> g_InputServer;
#endif

bool wlserver_init( void ) {
	assert( wlserver.display != nullptr );

	wl_list_init(&pending_surfaces);

	wlserver.wlr.multi_backend = wlr_multi_backend_create( wlserver.event_loop );
	wlr_multi_backend_add( wlserver.wlr.multi_backend, wlserver.wlr.headless_backend );

	assert( wlserver.event_loop && wlserver.wlr.multi_backend );

	wl_signal_add( &wlserver.wlr.multi_backend->events.new_input, &new_input_listener );

	if ( GetBackend()->IsSessionBased() )
	{
#if HAVE_DRM
		wlserver.wlr.libinput_backend = wlr_libinput_backend_create( wlserver.wlr.session );
		if ( wlserver.wlr.libinput_backend == NULL)
		{
			return false;
		}
		wlr_multi_backend_add( wlserver.wlr.multi_backend, wlserver.wlr.libinput_backend );
#endif
	}

	// Create a stub wlr_keyboard only used to set the keymap
	// We need to wait for the backend to be started before adding the device
	struct wlr_keyboard *kbd = (struct wlr_keyboard *) calloc(1, sizeof(*kbd));
	wlr_keyboard_init(kbd, nullptr, "virtual");
	wlserver.wlr.virtual_keyboard_device = kbd;

	// Create a keyboard group to keep all externally connected keyboards
	// in sync (one single layout and a shared state)
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_rule_names rules = { 0 };
	rules.rules = getenv("XKB_DEFAULT_RULES");
	rules.model = getenv("XKB_DEFAULT_MODEL");
	rules.layout = getenv("XKB_DEFAULT_LAYOUT");
	rules.variant = getenv("XKB_DEFAULT_VARIANT");
	rules.options = getenv("XKB_DEFAULT_OPTIONS");
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
	wlserver.keyboard_group = wlr_keyboard_group_create();
	struct wlr_keyboard *keyboard = &wlserver.keyboard_group->keyboard;
	wlr_keyboard_set_repeat_info(keyboard, 25, 600);
	wlr_keyboard_set_keymap(keyboard, keymap);
	wlserver.keyboard_group_modifiers.notify = wlserver_handle_modifiers;
	wl_signal_add(&keyboard->events.modifiers, &wlserver.keyboard_group_modifiers);
	wlserver.keyboard_group_key.notify = wlserver_handle_key;
	wl_signal_add(&keyboard->events.key, &wlserver.keyboard_group_key);

	wlserver.wlr.renderer = vulkan_renderer_create();

	wlr_renderer_init_wl_display(wlserver.wlr.renderer, wlserver.display);

	wlserver.wlr.compositor = wlr_compositor_create(wlserver.display, 5, wlserver.wlr.renderer);

	wl_signal_add( &wlserver.wlr.compositor->events.new_surface, &new_surface_listener );

	create_ime_manager( &wlserver );

	create_reshade();

	new gamescope::WaylandServer::CGamescopeActionBindingProtocol( wlserver.display );

	create_gamescope_xwayland();

	create_gamescope_swapchain_factory_v2();

#if HAVE_PIPEWIRE
	create_gamescope_pipewire();
#endif

	create_gamescope_control();

	create_gamescope_private();

	create_presentation_time();

	// Have to make this old ancient thing for compat with older XWayland.
	// Someday, he will be purged.
	wlr_drm_create(wlserver.display, wlserver.wlr.renderer);

	if ( GetBackend()->SupportsExplicitSync() )
	{
		create_explicit_sync();
		wl_log.infof( "Using explicit sync when available" );
	}

	wlserver.relative_pointer_manager = wlr_relative_pointer_manager_v1_create(wlserver.display);
	if ( !wlserver.relative_pointer_manager )
	{
		wl_log.errorf( "Failed to create relative pointer manager" );
		return false;
	}
	wlserver.constraints = wlr_pointer_constraints_v1_create(wlserver.display);
	if ( !wlserver.constraints )
	{
		wl_log.errorf( "Failed to create pointer constraints" );
		return false;
	}
	wlserver.new_pointer_constraint.notify = handle_pointer_constraint;
	wl_signal_add(&wlserver.constraints->events.new_constraint, &wlserver.new_pointer_constraint);

	wlserver.xdg_shell = wlr_xdg_shell_create(wlserver.display, 3);
	if (!wlserver.xdg_shell)
	{
		wl_log.infof("Unable to create XDG shell interface");
		return false;
	}
	wlserver.new_xdg_surface.notify = xdg_surface_new;
	wlserver.new_xdg_toplevel.notify = xdg_toplevel_new;
	wl_signal_add(&wlserver.xdg_shell->events.new_surface, &wlserver.new_xdg_surface);
	wl_signal_add(&wlserver.xdg_shell->events.new_toplevel, &wlserver.new_xdg_toplevel);

	wlserver.layer_shell_v1 = wlr_layer_shell_v1_create(wlserver.display, 4);
	if (!wlserver.layer_shell_v1)
	{
		wl_log.infof("Unable to create layer shell interface");
		return false;
	}
	wlserver.new_layer_shell_surface.notify = layer_shell_surface_new;
	wl_signal_add(&wlserver.layer_shell_v1->events.new_surface, &wlserver.new_layer_shell_surface);

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

#if HAVE_LIBEIS
	{
		char szEISocket[ 64 ];
		snprintf( szEISocket, sizeof( szEISocket ), "%s-ei", wlserver.wl_display_name );

		std::unique_ptr<gamescope::GamescopeInputServer> pInputServer = std::make_unique<gamescope::GamescopeInputServer>();
		if ( pInputServer->Init( szEISocket ) )
		{
			g_InputServer = std::move( pInputServer );

			setenv( "LIBEI_SOCKET", szEISocket, 1 );
			g_LibEisWaiter.AddWaitable( g_InputServer.get() );
			wl_log.infof( "Successfully initialized libei for input emulation!" );
		}
		else
		{
			wl_log.errorf( "Initializing libei failed, XTEST will not be available!" );
		}
	}
#else
	wl_log.errorf( "Gamescope built without libei, XTEST will not be available!" );
#endif

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

	wlserver.bWaylandServerRunning = true;
	wlserver.bWaylandServerRunning.notify_all();

	while ( !g_bShutdownWLServer )
	{
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
				wlserver_unlock();
				break;
			}

			wlserver_unlock();
		}
	}

	wlserver.bWaylandServerRunning = false;
	wlserver.bWaylandServerRunning.notify_all();

	{
		std::unique_lock lock3(wlserver.xdg_commit_lock);
		wlserver.xdg_commit_queue.clear();
	}

	{
		std::unique_lock lock2(g_wlserver_xdg_shell_windows_lock);
		wlserver.xdg_wins.clear();
	}

#if HAVE_LIBEIS
	g_InputServer = nullptr;
#endif

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

void wlserver_shutdown()
{
    assert( wlserver_is_lock_held() );

	g_bShutdownWLServer = true;

    if (wlserver.display)
    {
        if ( write( g_wlserverNudgePipe[ 1 ], "\n", 1 ) < 0 )
            wl_log.errorf_errno( "wlserver_force_shutdown: write failed" );
    }
}

void wlserver_keyboardfocus( struct wlr_surface *surface, bool bConstrain )
{
	assert( wlserver_is_lock_held() );

	if (wlserver.kb_focus_surface != surface) {
		auto old_wl_surf = get_wl_surface_info( wlserver.kb_focus_surface );
		if (old_wl_surf && old_wl_surf->xdg_surface && old_wl_surf->xdg_surface->xdg_surface && old_wl_surf->xdg_surface->xdg_surface->toplevel)
			wlr_xdg_toplevel_set_activated(old_wl_surf->xdg_surface->xdg_surface->toplevel, false);

		auto new_wl_surf = get_wl_surface_info( surface );
		if (new_wl_surf && new_wl_surf->xdg_surface && new_wl_surf->xdg_surface->xdg_surface && new_wl_surf->xdg_surface->xdg_surface->toplevel)
			wlr_xdg_toplevel_set_activated(new_wl_surf->xdg_surface->xdg_surface->toplevel, true);
	}

	assert( wlserver.wlr.virtual_keyboard_device != nullptr );
	wlr_seat_set_keyboard( wlserver.wlr.seat, wlserver.wlr.virtual_keyboard_device );

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard( wlserver.wlr.seat );
	if ( keyboard == nullptr )
		wlr_seat_keyboard_notify_enter( wlserver.wlr.seat, surface, nullptr, 0, nullptr);
	else
		wlr_seat_keyboard_notify_enter( wlserver.wlr.seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);

	wlserver.kb_focus_surface = surface;

	if ( bConstrain )
	{
		struct wlr_pointer_constraint_v1 *constraint = wlr_pointer_constraints_v1_constraint_for_surface( wlserver.constraints, surface, wlserver.wlr.seat );
		wlserver_constrain_cursor( constraint );
	}
}

bool wlserver_process_hotkeys( wlr_keyboard *keyboard, uint32_t key, bool press )
{
	xkb_keycode_t keycode = key + 8;
	xkb_keysym_t keysym = xkb_state_key_get_one_sym( keyboard->xkb_state, keycode );

	keysym = NormalizeKeysymForHotkey( keysym );

	static std::unordered_set<xkb_keysym_t> s_setPressedKeySyms;
	if ( press )
	{
		s_setPressedKeySyms.emplace( keysym );
	}
	else
	{
		s_setPressedKeySyms.erase( keysym );
	}

	if ( log_binding.Enabled( LOG_DEBUG ) )
	{
		std::string sPressedKeySymsDebugName = ComputeDebugName( s_setPressedKeySyms );
		log_binding.debugf( "Looking for: [%s].", sPressedKeySymsDebugName.c_str() );
	}

	{
		using namespace gamescope::WaylandServer;

		std::span<CGamescopeActionBinding *> ppBindings = CGamescopeActionBinding::GetBindings();

		for ( CGamescopeActionBinding *pBinding : ppBindings )
		{
			if ( !pBinding->IsArmed() )
				continue;

			std::span<Keybind_t> pKeybinds = pBinding->GetKeyboardTriggers();
			for ( const Keybind_t &keybind : pKeybinds )
			{
				if ( !pBinding->IsArmed() )
					break;

				if ( s_setPressedKeySyms != keybind.setKeySyms )
					continue;

				if ( pBinding->Execute() )
					return true;
			}
		}
	}

	return false;
}

void wlserver_key( uint32_t key, bool press, uint32_t time )
{
	assert( wlserver_is_lock_held() );

	wlr_keyboard *keyboard = wlserver.wlr.virtual_keyboard_device;

	if ( !wlserver_process_hotkeys( keyboard, key, press ) )
	{
		assert( keyboard != nullptr );
		wlr_seat_set_keyboard( wlserver.wlr.seat, keyboard );
		wlr_seat_keyboard_notify_key( wlserver.wlr.seat, time, key, press );
	}

	bump_input_counter();
}

struct wlr_surface *wlserver_surface_to_main_surface( struct wlr_surface *pSurface )
{
	if ( !pSurface )
		return nullptr;

	wlserver_wl_surface_info *pInfo = get_wl_surface_info( pSurface );
	if ( !pInfo )
		return nullptr;

	if ( pInfo->x11_surface )
	{
		wlr_surface *pMain = pInfo->x11_surface->main_surface.load();
		if ( pMain )
			pSurface = pMain;
	}

	return pSurface;
}

struct wlr_surface *wlserver_surface_to_override_surface( struct wlr_surface *pSurface )
{
	if ( !pSurface )
		return nullptr;

	wlserver_wl_surface_info *pInfo = get_wl_surface_info( pSurface );
	if ( !pInfo )
		return nullptr;

	if ( pInfo->x11_surface )
	{
		wlr_surface *pOverride = pInfo->x11_surface->override_surface.load();
		if ( pOverride )
			pSurface = pOverride;
	}

	return pSurface;
}

std::pair<int, int> wlserver_get_surface_extent( struct wlr_surface *pSurface )
{
	assert( wlserver_is_lock_held() );

	pSurface = wlserver_surface_to_override_surface( pSurface );

	if ( !pSurface )
		return std::make_pair( g_nNestedWidth, g_nNestedHeight );

	return std::make_pair( pSurface->current.width, pSurface->current.height );
}

bool ShouldDrawCursor();
void wlserver_oncursorevent()
{
	// Don't repaint if we would use a nested cursor.
	if ( !ShouldDrawCursor() )
		return;

	if ( !wlserver.bCursorHidden && wlserver.bCursorHasImage )
	{
		hasRepaint = true;
	}
}

static std::pair<int, int> wlserver_get_cursor_bounds()
{
	auto [nWidth, nHeight] = wlserver_get_surface_extent( wlserver.mouse_focus_surface );
	for ( auto iter : wlserver.current_dropdown_surfaces )
	{
		auto [nDropdownX, nDropdownY] = iter.second;
		auto [nDropdownWidth, nDropdownHeight] = wlserver_get_surface_extent( iter.first );

		nWidth = std::max( nWidth, nDropdownX + nDropdownWidth );
		nHeight = std::max( nHeight, nDropdownY + nDropdownHeight );
	}

	return std::make_pair( nWidth, nHeight );
}

static void wlserver_clampcursor()
{
	auto [nWidth, nHeight] = wlserver_get_cursor_bounds();
	wlserver.mouse_surface_cursorx = std::clamp( wlserver.mouse_surface_cursorx, 0.0, double( std::max( nWidth - 1, 0 ) ) );
	wlserver.mouse_surface_cursory = std::clamp( wlserver.mouse_surface_cursory, 0.0, double( std::max( nHeight - 1, 0 ) ) );
}

void wlserver_mousefocus( struct wlr_surface *wlrsurface, int x /* = 0 */, int y /* = 0 */ )
{
	assert( wlserver_is_lock_held() );

	if ( wlserver.mouse_focus_surface == wlrsurface )
	{
		wlserver_clampcursor();
	}
	else
	{
		wlserver.mouse_focus_surface = wlrsurface;

		auto [nWidth, nHeight] = wlserver_get_surface_extent( wlrsurface );

		if ( x < nWidth && y < nHeight )
		{
			wlserver.mouse_surface_cursorx = x;
			wlserver.mouse_surface_cursory = y;
		}
		else
		{
			wlserver.mouse_surface_cursorx = nWidth / 2.0;
			wlserver.mouse_surface_cursory = nHeight / 2.0;
		}
	}

	wlserver_oncursorevent();
	wlr_seat_pointer_warp( wlserver.wlr.seat, wlserver.mouse_surface_cursorx, wlserver.mouse_surface_cursory );
	wlr_seat_pointer_notify_enter( wlserver.wlr.seat, wlrsurface, wlserver.mouse_surface_cursorx, wlserver.mouse_surface_cursory );
}

void wlserver_clear_dropdowns()
{
	wlserver.current_dropdown_surfaces.clear();
}

void wlserver_notify_dropdown( struct wlr_surface *wlrsurface, int nX, int nY )
{
	wlserver.current_dropdown_surfaces[wlrsurface] = std::make_pair( nX, nY );
}

void wlserver_mousehide()
{
	wlserver.ulLastMovedCursorTime = 0;
	if ( wlserver.bCursorHidden != true )
	{
		wlserver.bCursorHidden = true;
		hasRepaint = true;
	}
}

struct GamescopePointerConstraint
{
	struct wlr_pointer_constraint_v1 *pConstraint = nullptr;

	struct wl_listener set_region{};
	struct wl_listener destroy{};
};

static void wlserver_warp_to_constraint_hint()
{
	struct wlr_pointer_constraint_v1 *pConstraint = wlserver.GetCursorConstraint();
	
	if (pConstraint->current.cursor_hint.enabled)
	{
		double sx = pConstraint->current.cursor_hint.x;
		double sy = pConstraint->current.cursor_hint.y;

		if ( wlserver.mouse_surface_cursorx == sx && wlserver.mouse_surface_cursory == sy )
			return;

		wlserver.mouse_surface_cursorx = sx;
		wlserver.mouse_surface_cursory = sy;
		wlr_seat_pointer_warp( wlserver.wlr.seat, sx, sy );

		uint64_t ulNow = get_time_in_nanos();
		static uint32_t s_unSyntheticMoveCount = 0;

		// Add a heuristic for whether the cursor is continually moving, or if
		// this is just a simple warp to the saame place.
		bool bSynthetic = true;
		if ( wlserver.ulLastMovedCursorTime + 200'000'000 >= ulNow )
		{
			if ( s_unSyntheticMoveCount++ >= 3 )
			{
				bSynthetic = false;
			}
		}
		else
		{
			s_unSyntheticMoveCount = 0;
		}

		wlserver.ulLastMovedCursorTime = ulNow;

		if ( !bSynthetic )
			wlserver.bCursorHidden = !wlserver.bCursorHasImage;
	}
}

static void wlserver_update_cursor_constraint()
{
	struct wlr_pointer_constraint_v1 *pConstraint = wlserver.GetCursorConstraint();
	pixman_region32_t *pRegion = &pConstraint->region;

	if ( wlserver.mouse_constraint_requires_warp && pConstraint->surface )
	{
		wlserver.mouse_constraint_requires_warp = false;

		wlserver_warp_to_constraint_hint();

		if (!pixman_region32_contains_point(pRegion, floor(wlserver.mouse_surface_cursorx), floor(wlserver.mouse_surface_cursory), NULL))
		{
			int nboxes;
			pixman_box32_t *boxes = pixman_region32_rectangles(pRegion, &nboxes);
			if ( nboxes )
			{
				wlserver.mouse_surface_cursorx = std::clamp<double>( wlserver.mouse_surface_cursorx, boxes[0].x1, boxes[0].x2);
				wlserver.mouse_surface_cursory = std::clamp<double>( wlserver.mouse_surface_cursory, boxes[0].y1, boxes[0].y2);

				wlr_seat_pointer_warp( wlserver.wlr.seat, wlserver.mouse_surface_cursorx, wlserver.mouse_surface_cursory );
			}
		}
	}

	if (pConstraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED)
		pixman_region32_copy(&wlserver.confine, pRegion);
	else
		pixman_region32_clear(&wlserver.confine);
}

static void wlserver_constrain_cursor( struct wlr_pointer_constraint_v1 *pNewConstraint )
{
	struct wlr_pointer_constraint_v1 *pOldConstraint = wlserver.GetCursorConstraint();

	if ( pOldConstraint == pNewConstraint )
		return;

	if ( pOldConstraint )
	{
		if ( !pNewConstraint )
			wlserver_warp_to_constraint_hint();

		wlr_pointer_constraint_v1_send_deactivated( pOldConstraint );
	}

	wlserver.SetMouseConstraint( pNewConstraint );

	if ( !pNewConstraint )
		return;

	wlserver.mouse_constraint_requires_warp = true;

	wlserver_update_cursor_constraint();

	wlr_pointer_constraint_v1_send_activated( pNewConstraint );
}

static void handle_pointer_constraint_set_region(struct wl_listener *listener, void *data)
{
	GamescopePointerConstraint *pGamescopeConstraint = wl_container_of(listener, pGamescopeConstraint, set_region);

	// If the region has been updated, we might need to warp again next commit.
	wlserver.mouse_constraint_requires_warp = true;
}

void handle_constraint_destroy(struct wl_listener *listener, void *data)
{
	GamescopePointerConstraint *pGamescopeConstraint = wl_container_of(listener, pGamescopeConstraint, destroy);

	wl_list_remove(&pGamescopeConstraint->set_region.link);
	wl_list_remove(&pGamescopeConstraint->destroy.link);

	struct wlr_pointer_constraint_v1 *pCurrentConstraint = wlserver.GetCursorConstraint();
	if ( pCurrentConstraint == pGamescopeConstraint->pConstraint )
	{
		wlserver_warp_to_constraint_hint();

		wlserver.SetMouseConstraint( nullptr );
	}

	delete pGamescopeConstraint;
}

static void handle_pointer_constraint(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_constraint_v1 *pConstraint = (struct wlr_pointer_constraint_v1 *) data;

	GamescopePointerConstraint *pGamescopeConstraint = new GamescopePointerConstraint;
	pGamescopeConstraint->pConstraint = pConstraint;

	pGamescopeConstraint->set_region.notify = handle_pointer_constraint_set_region;
	wl_signal_add(&pConstraint->events.set_region, &pGamescopeConstraint->set_region);

	pGamescopeConstraint->destroy.notify = handle_constraint_destroy;
	wl_signal_add(&pConstraint->events.destroy, &pGamescopeConstraint->destroy);

	if ( wlserver.kb_focus_surface && wlserver.kb_focus_surface == pConstraint->surface )
		wlserver_constrain_cursor(pConstraint);
}

static bool wlserver_apply_constraint( double *dx, double *dy )
{
	struct wlr_pointer_constraint_v1 *pConstraint = wlserver.GetCursorConstraint();

	if ( pConstraint )
	{
		if ( pConstraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED )
			return false;

		double sx = wlserver.mouse_surface_cursorx;
		double sy = wlserver.mouse_surface_cursory;

		double sx_confined, sy_confined;
		if ( !wlr_region_confine( &wlserver.confine, sx, sy, sx + *dx, sy + *dy, &sx_confined, &sy_confined ) )
			return false;

		*dx = sx_confined - sx;
		*dy = sy_confined - sy;

		if ( *dx == 0.0 && *dy == 0.0 )
			return false;
	}

	return true;
}

void wlserver_mousemotion( double dx, double dy, uint32_t time )
{
	assert( wlserver_is_lock_held() );

	dx *= g_mouseSensitivity;
	dy *= g_mouseSensitivity;

	wlserver_perform_rel_pointer_motion( dx, dy );

	if ( !wlserver_apply_constraint( &dx, &dy ) )
	{
		wlr_seat_pointer_notify_frame( wlserver.wlr.seat );
		return;
	}

	wlserver.ulLastMovedCursorTime = get_time_in_nanos();
	wlserver.bCursorHidden = !wlserver.bCursorHasImage;

	wlserver.mouse_surface_cursorx += dx;
	wlserver.mouse_surface_cursory += dy;

	wlserver_clampcursor();

	wlserver_oncursorevent();

	wlr_seat_pointer_notify_motion( wlserver.wlr.seat, time, wlserver.mouse_surface_cursorx, wlserver.mouse_surface_cursory );
	wlr_seat_pointer_notify_frame( wlserver.wlr.seat );
}

void wlserver_mousewarp( double x, double y, uint32_t time, bool bSynthetic )
{
	assert( wlserver_is_lock_held() );

	wlserver.mouse_surface_cursorx = x;
	wlserver.mouse_surface_cursory = y;

	wlserver_clampcursor();

	wlserver.ulLastMovedCursorTime = get_time_in_nanos();
	if ( !bSynthetic )
		wlserver.bCursorHidden = !wlserver.bCursorHasImage;

	wlserver_oncursorevent();

	wlr_seat_pointer_notify_motion( wlserver.wlr.seat, time, wlserver.mouse_surface_cursorx, wlserver.mouse_surface_cursory );
	wlr_seat_pointer_notify_frame( wlserver.wlr.seat );
}

void wlserver_fake_mouse_pos( double x, double y )
{
	// Fake a pos for eg. hiding true cursor state from Steam.
	wlr_seat_pointer_notify_motion( wlserver.wlr.seat, 0, x, y );
	wlr_seat_pointer_notify_frame( wlserver.wlr.seat );
}

void wlserver_mousebutton( int button, bool press, uint32_t time )
{
	assert( wlserver_is_lock_held() );

	wlserver.bCursorHidden = !wlserver.bCursorHasImage;

	wlserver_oncursorevent();

	wlr_seat_pointer_notify_button( wlserver.wlr.seat, time, button, press ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED );
	wlr_seat_pointer_notify_frame( wlserver.wlr.seat );
}

void wlserver_mousewheel( double flX, double flY, uint32_t time )
{
	assert( wlserver_is_lock_held() );

	wlr_seat_pointer_notify_axis( wlserver.wlr.seat, time, WL_POINTER_AXIS_HORIZONTAL_SCROLL, flX, flX * WLR_POINTER_AXIS_DISCRETE_STEP, WL_POINTER_AXIS_SOURCE_WHEEL, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL );
	wlr_seat_pointer_notify_axis( wlserver.wlr.seat, time, WL_POINTER_AXIS_VERTICAL_SCROLL, flY, flY * WLR_POINTER_AXIS_DISCRETE_STEP, WL_POINTER_AXIS_SOURCE_WHEEL, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL );
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
	if ( wl_surf->oCurrentPresentMode )
	{
		return wl_surf->oCurrentPresentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ||
			   wl_surf->oCurrentPresentMode == VK_PRESENT_MODE_MAILBOX_KHR;
	}

	return false;
}

bool wlserver_surface_is_fifo( struct wlr_surface *surf )
{
	assert( wlserver_is_lock_held() );

	auto wl_surf = get_wl_surface_info( surf );
	if ( !wl_surf )
		return false;

	if ( wl_surf->oCurrentPresentMode )
	{
		return wl_surf->oCurrentPresentMode == VK_PRESENT_MODE_FIFO_KHR;
	}

	return false;
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
	switch ( GetBackend()->GetConnector( gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL )->GetCurrentOrientation() )
	{
		default:
		case GAMESCOPE_PANEL_ORIENTATION_AUTO:
		case GAMESCOPE_PANEL_ORIENTATION_0:
			tx = *x;
			ty = *y;
			break;
		case GAMESCOPE_PANEL_ORIENTATION_90:
			tx = 1.0 - *y;
			ty = *x;
			break;
		case GAMESCOPE_PANEL_ORIENTATION_180:
			tx = 1.0 - *x;
			ty = 1.0 - *y;
			break;
		case GAMESCOPE_PANEL_ORIENTATION_270:
			tx = *y;
			ty = 1.0 - *x;
			break;
	}

	*x = tx;
	*y = ty;
}

void wlserver_touchmotion( double x, double y, int touch_id, uint32_t time, bool bAlwaysWarpCursor )
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

		auto [nWidth, nHeight] = wlserver_get_cursor_bounds();
		tx = clamp( tx, 0.0, nWidth - 0.1 );
		ty = clamp( ty, 0.0, nHeight - 0.1 );

		double trackpad_dx, trackpad_dy;

		trackpad_dx = tx - wlserver.mouse_surface_cursorx;
		trackpad_dy = ty - wlserver.mouse_surface_cursory;

		gamescope::TouchClickMode eMode = GetBackend()->GetTouchClickMode();

		if ( eMode == gamescope::TouchClickModes::Passthrough )
		{
			wlr_seat_touch_notify_motion( wlserver.wlr.seat, time, touch_id, tx, ty );

			if ( bAlwaysWarpCursor )
				wlserver_mousewarp( tx, ty, time, false );
		}
		else if ( eMode == gamescope::TouchClickModes::Disabled )
		{
			return;
		}
		else if ( eMode == gamescope::TouchClickModes::Trackpad )
		{
			wlserver_mousemotion( trackpad_dx, trackpad_dy, time );
		}
		else
		{
			g_bPendingTouchMovement = true;

			wlserver_mousewarp( tx, ty, time, false );
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

		gamescope::TouchClickMode eMode = GetBackend()->GetTouchClickMode();

		if ( eMode == gamescope::TouchClickModes::Passthrough )
		{
			wlr_seat_touch_notify_down( wlserver.wlr.seat, wlserver.mouse_focus_surface, time, touch_id,
										tx, ty );

			wlserver.touch_down_ids.insert( touch_id );
		}
		else if ( eMode == gamescope::TouchClickModes::Disabled )
		{
			return;
		}
		else
		{
			g_bPendingTouchMovement = true;

			if ( eMode != gamescope::TouchClickModes::Trackpad )
			{
				wlserver_mousewarp( tx, ty, time, false );
			}

			uint32_t button = TouchClickModeToLinuxButton( eMode );

			if ( button != 0 && eMode < WLSERVER_BUTTON_COUNT )
			{
				wlr_seat_pointer_notify_button( wlserver.wlr.seat, time, button, WL_POINTER_BUTTON_STATE_PRESSED );
				wlr_seat_pointer_notify_frame( wlserver.wlr.seat );

				wlserver.button_held[ eMode ] = true;
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
				uint32_t button = TouchClickModeToLinuxButton( (gamescope::TouchClickMode) i );

				if ( button != 0 )
				{
					wlr_seat_pointer_notify_button( wlserver.wlr.seat, time, button, WL_POINTER_BUTTON_STATE_RELEASED );
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
		surf->xwayland_server->destroy_content_override( surf, surf->main_surface );

		wlserver_wl_surface_info *wl_info = get_wl_surface_info(surf->main_surface);
		if (wl_info)
			wl_info->x11_surface = nullptr;
	}

	if (surf->override_surface)
	{
		surf->xwayland_server->destroy_content_override( surf, surf->override_surface );

		wlserver_wl_surface_info *wl_info = get_wl_surface_info(surf->override_surface);
		if (wl_info)
			wl_info->x11_surface = nullptr;
	}

	surf->wl_id = 0;
	surf->main_surface = nullptr;
	surf->override_surface = nullptr;
	wl_list_remove( &surf->pending_link );
}

void wlserver_set_xwayland_server_mode( size_t idx, int w, int h, int nRefreshmHz )
{
	assert( wlserver_is_lock_held() );

	gamescope_xwayland_server_t *server = wlserver_get_xwayland_server( idx );
	if ( !server )
		return;

	struct wlr_output *output = server->get_output();
	struct wlr_output_state *output_state = server->get_output_state();
	wlr_output_state_set_custom_mode(output_state, w, h, nRefreshmHz );
	if (!wlr_output_commit_state(output, output_state))
	{
		wl_log.errorf("Failed to commit headless output");
		abort();
	}

	wl_log.infof("Updating mode for xwayland server #%zu: %dx%d@%d", idx, w, h, gamescope::ConvertmHzToHz( nRefreshmHz ) );
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
