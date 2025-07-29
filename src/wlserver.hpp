// Wayland stuff

#pragma once

#include <wayland-server-core.h>
#include <atomic>
#include <vector>
#include <memory>
#include <mutex>
#include <map>
#include <set>
#include <list>
#include <unordered_map>
#include <optional>

#include "WaylandServer/WaylandDecls.h"
#include "WaylandServer/WaylandServerLegacy.h"

#include <pixman-1/pixman.h>

#include "vulkan_include.h"

#include "steamcompmgr_shared.hpp"

#if HAVE_DRM
#define HAVE_SESSION 1
#endif

#define WLSERVER_BUTTON_COUNT 7

struct _XDisplay;
struct xwayland_ctx_t;

struct GamescopeAcquireTimelineState
{
	int32_t nEventFd = -1;
	bool bKnownReady = false;
};

struct ResListEntry_t {
	struct wlr_surface *surf;
	struct wlr_buffer *buf;
	bool async;
	bool fifo;
	std::shared_ptr<wlserver_vk_swapchain_feedback> feedback;
	std::vector<struct wl_resource*> presentation_feedbacks;
	std::optional<uint32_t> present_id;
	uint64_t desired_present_time;
	std::shared_ptr<gamescope::CAcquireTimelinePoint> pAcquirePoint;
	std::shared_ptr<gamescope::CReleaseTimelinePoint> pReleasePoint;
};

struct wlserver_content_override;

bool wlserver_is_lock_held(void);

class gamescope_xwayland_server_t
{
public:
	gamescope_xwayland_server_t(wl_display *display);
	~gamescope_xwayland_server_t();

	void on_xwayland_ready(void *data);
	static void xwayland_ready_callback(struct wl_listener *listener, void *data);

	bool is_xwayland_ready() const;
	const char *get_nested_display_name() const;

	void set_wl_id( struct wlserver_x11_surface_info *surf, uint32_t id );

	_XDisplay *get_xdisplay();

	std::unique_ptr<xwayland_ctx_t> ctx;

	void wayland_commit(struct wlr_surface *surf, struct wlr_buffer *buf);

	std::vector<ResListEntry_t>& retrieve_commits();

	void handle_override_window_content( struct wl_client *client, struct wl_resource *gamescope_swapchain_resource, struct wlr_surface *surface, uint32_t x11_window );
	void destroy_content_override( struct wlserver_x11_surface_info *x11_surface, struct wlr_surface *surf);
	void destroy_content_override(struct wlserver_content_override *co);

	struct wl_client *get_client();
	struct wlr_output *get_output();
	struct wlr_output_state *get_output_state();

	void update_output_info();

private:
	struct wlr_xwayland_server *xwayland_server = NULL;
	struct wl_listener xwayland_ready_listener = { .notify = xwayland_ready_callback };

	struct wlr_output *output = nullptr;
	struct wlr_output_state *output_state = nullptr;

	std::unordered_map<uint32_t, wlserver_content_override *> content_overrides;

	bool xwayland_ready = false;
	_XDisplay *dpy = NULL;

	std::mutex wayland_commit_lock;
	std::vector<ResListEntry_t> wayland_commit_queue;
};

struct wlserver_t {
	struct wl_display *display;
	struct wl_event_loop *event_loop;
	char wl_display_name[32];

	struct {
		struct wlr_backend *multi_backend;
		struct wlr_backend *headless_backend;
		struct wlr_backend *libinput_backend;

		struct wlr_renderer *renderer;
		struct wlr_compositor *compositor;
		struct wlr_session *session;
		struct wlr_seat *seat;

		// Used to simulate key events and set the keymap
		struct wlr_keyboard *virtual_keyboard_device;

		struct wlr_device *device;

		std::vector<std::unique_ptr<gamescope_xwayland_server_t>> xwayland_servers;
	} wlr;
	
	struct wlr_surface *mouse_focus_surface;
	struct wlr_surface *kb_focus_surface;
	std::unordered_map<struct wlr_surface *, std::pair<int, int>> current_dropdown_surfaces;
	double mouse_surface_cursorx = 0.0f;
	double mouse_surface_cursory = 0.0f;
	bool mouse_constraint_requires_warp = false;
	pixman_region32_t confine;
	std::atomic<struct wlr_pointer_constraint_v1 *> mouse_constraint = { nullptr };

	void SetMouseConstraint( struct wlr_pointer_constraint_v1 *pConstraint )
	{
		assert( wlserver_is_lock_held() );
		// Set by wlserver only. Read by both wlserver + steamcompmgr with no
		// need to actually be sequentially consistent.
		mouse_constraint.store( pConstraint, std::memory_order_relaxed );
	}

	struct wlr_pointer_constraint_v1 *GetCursorConstraint() const
	{
		assert( wlserver_is_lock_held() );
		return mouse_constraint.load( std::memory_order_relaxed );
	}

	bool HasMouseConstraint() const
	{
		// Does not need to be sequentially consistent.
		// Used by the steamcompmgr thread to check if there is currently a mouse constraint.
		return mouse_constraint.load( std::memory_order_relaxed ) != nullptr;
	}

	uint64_t ulLastMovedCursorTime = 0;
	bool bCursorHidden = true;
	bool bCursorHasImage = true;
	
	bool button_held[ WLSERVER_BUTTON_COUNT ];
	std::set <uint32_t> touch_down_ids;

	struct {
		char *name;
		char *description;
		int phys_width, phys_height; // millimeters
	} output_info;

	struct wl_listener session_active;
	struct wl_listener new_input_method;

	struct wlr_xdg_shell *xdg_shell;
	struct wlr_layer_shell_v1 *layer_shell_v1;
	struct wlr_relative_pointer_manager_v1 *relative_pointer_manager;
	struct wlr_pointer_constraints_v1 *constraints;
	struct wl_listener new_xdg_surface;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_layer_shell_surface;
	struct wl_listener new_pointer_constraint;
	std::vector<std::shared_ptr<steamcompmgr_win_t>> xdg_wins;
	std::atomic<bool> xdg_dirty;
	std::mutex xdg_commit_lock;
	std::vector<ResListEntry_t> xdg_commit_queue;

	std::vector<wl_resource*> gamescope_controls;
	std::unordered_map< uint32_t, std::vector<wl_resource*> > app_perf_requests;

	std::atomic<bool> bWaylandServerRunning = { false };

    // Share one single keymap and state between all connected physical keyboards
    struct wlr_keyboard_group *keyboard_group;
    struct wl_listener keyboard_group_modifiers;
    struct wl_listener keyboard_group_key;
};

extern struct wlserver_t wlserver;

std::vector<ResListEntry_t> wlserver_xdg_commit_queue();

struct wlserver_pointer {
	struct wlr_pointer *wlr;
	
	struct wl_listener motion;
	struct wl_listener button;
	struct wl_listener axis;
	struct wl_listener frame;
};

struct wlserver_touch {
	struct wlr_touch *wlr;
	
	struct wl_listener down;
	struct wl_listener up;
	struct wl_listener motion;
};

void xwayland_surface_commit(struct wlr_surface *wlr_surface);

bool wlsession_init( void );
int wlsession_open_kms( const char *device_name );
void wlsession_close_kms();

bool wlserver_init( void );

void wlserver_run(void);

void wlserver_lock(void);
void wlserver_unlock(bool flush = true);
bool wlserver_is_lock_held(void);

void wlserver_keyboardfocus( struct wlr_surface *surface, bool bConstrain = true );
void wlserver_key( uint32_t key, bool press, uint32_t time );

void wlserver_mousefocus( struct wlr_surface *wlrsurface, int x = 0, int y = 0 );
void wlserver_clear_dropdowns();
void wlserver_notify_dropdown( struct wlr_surface *wlrsurface, int nX, int nY );
void wlserver_mousemotion( double x, double y, uint32_t time );
void wlserver_mousehide();
void wlserver_mousewarp( double x, double y, uint32_t time, bool bSynthetic );
void wlserver_mousebutton( int button, bool press, uint32_t time );
void wlserver_mousewheel( double x, double y, uint32_t time );

void wlserver_touchmotion( double x, double y, int touch_id, uint32_t time, bool bAlwaysWarpCursor = false );
void wlserver_touchdown( double x, double y, int touch_id, uint32_t time );
void wlserver_touchup( int touch_id, uint32_t time );

void wlserver_send_frame_done( struct wlr_surface *surf, const struct timespec *when );

bool wlserver_surface_is_async( struct wlr_surface *surf );
bool wlserver_surface_is_fifo( struct wlr_surface *surf );
const std::shared_ptr<wlserver_vk_swapchain_feedback>& wlserver_surface_swapchain_feedback( struct wlr_surface *surf );

std::vector<std::shared_ptr<steamcompmgr_win_t>> wlserver_get_xdg_shell_windows();
bool wlserver_xdg_dirty();

struct wlserver_output_info {
	const char *description;
	int phys_width, phys_height; // millimeters
};

void wlserver_set_output_info( const wlserver_output_info *info );

gamescope_xwayland_server_t *wlserver_get_xwayland_server( size_t index );
const char *wlserver_get_wl_display_name( void );

void wlserver_x11_surface_info_init( struct wlserver_x11_surface_info *surf, gamescope_xwayland_server_t *server, uint32_t x11_id );
void wlserver_x11_surface_info_finish( struct wlserver_x11_surface_info *surf );

void wlserver_set_xwayland_server_mode( size_t idx, int w, int h, int refresh );

extern std::atomic<bool> g_bPendingTouchMovement;

void wlserver_open_steam_menu( bool qam );

uint32_t wlserver_make_new_xwayland_server();
void wlserver_destroy_xwayland_server(gamescope_xwayland_server_t *server);

void wlserver_presentation_feedback_presented( struct wlr_surface *surface, std::vector<struct wl_resource*>& presentation_feedbacks, uint64_t last_refresh_nsec, uint64_t refresh_cycle );
void wlserver_presentation_feedback_discard( struct wlr_surface *surface, std::vector<struct wl_resource*>& presentation_feedbacks );

void wlserver_past_present_timing( struct wlr_surface *surface, uint32_t present_id, uint64_t desired_present_time, uint64_t actual_present_time, uint64_t earliest_present_time, uint64_t present_margin );
void wlserver_refresh_cycle( struct wlr_surface *surface, uint64_t refresh_cycle );

void wlserver_app_presented( uint32_t app_id, uint64_t frametime_ns );

void wlserver_shutdown();

void wlserver_send_gamescope_control( wl_resource *control );

bool wlsession_active();

void wlserver_fake_mouse_pos( double x, double y );

void wlserver_mousewheel2( int32_t nDiscreteX, int32_t nDiscreteY, double flX, double flY, uint32_t uTime );
