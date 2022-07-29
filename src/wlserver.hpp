// Wayland stuff

#pragma once

#include <wayland-server-core.h>
#include <atomic>
#include <vector>
#include <memory>
#include <mutex>
#include <map>

#define WLSERVER_BUTTON_COUNT 4
#define WLSERVER_TOUCH_COUNT 11 // Ten fingers + nose ought to be enough for anyone

struct _XDisplay;
struct xwayland_ctx_t;

struct ResListEntry_t {
	struct wlr_surface *surf;
	struct wlr_buffer *buf;
};

struct wlserver_content_override;

class gamescope_xwayland_server_t
{
public:
	gamescope_xwayland_server_t(wl_display *display);
	~gamescope_xwayland_server_t();

	void on_xwayland_ready(void *data);
	static void xwayland_ready_callback(struct wl_listener *listener, void *data);

	bool is_xwayland_ready() const;
	const char *get_nested_display_name() const;

	void set_wl_id( struct wlserver_surface *surf, uint32_t id );

	_XDisplay *get_xdisplay();

	std::unique_ptr<xwayland_ctx_t> ctx;

	void wayland_commit(struct wlr_surface *surf, struct wlr_buffer *buf);

	std::vector<ResListEntry_t> retrieve_commits();

	void handle_override_window_content( struct wl_client *client, struct wl_resource *resource, struct wl_resource *surface_resource, uint32_t x11_window );
	void destroy_content_override(struct wlserver_content_override *co);

	struct wl_client *get_client();
	struct wlr_output *get_output();

	void update_output_info();

private:
	struct wlr_xwayland_server *xwayland_server = NULL;
	struct wl_listener xwayland_ready_listener = { .notify = xwayland_ready_callback };

	struct wlr_output *output;

	std::map<uint32_t, wlserver_content_override *> content_overrides;

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
		struct wlr_input_device *virtual_keyboard_device;

		std::vector<std::unique_ptr<gamescope_xwayland_server_t>> xwayland_servers;
	} wlr;
	
	struct wlr_surface *mouse_focus_surface;
	struct wlr_surface *kb_focus_surface;
	double mouse_surface_cursorx;
	double mouse_surface_cursory;
	
	bool button_held[ WLSERVER_BUTTON_COUNT ];
	bool touch_down[ WLSERVER_TOUCH_COUNT ];

	struct {
		char *name;
		char *description;
		int phys_width, phys_height; // millimeters
	} output_info;

	struct wl_listener session_active;
	struct wl_listener new_input_method;
};

struct wlserver_keyboard {
	struct wlr_input_device *device;
	
	struct wl_listener modifiers;
	struct wl_listener key;
};

struct wlserver_pointer {
	struct wlr_input_device *device;
	
	struct wl_listener motion;
	struct wl_listener button;
	struct wl_listener axis;
	struct wl_listener frame;
};

struct wlserver_touch {
	struct wlr_input_device *device;
	
	struct wl_listener down;
	struct wl_listener up;
	struct wl_listener motion;
};

enum wlserver_touch_click_mode {
	WLSERVER_TOUCH_CLICK_HOVER = 0,
	WLSERVER_TOUCH_CLICK_LEFT = 1,
	WLSERVER_TOUCH_CLICK_RIGHT = 2,
	WLSERVER_TOUCH_CLICK_MIDDLE = 3,
	WLSERVER_TOUCH_CLICK_PASSTHROUGH = 4,
	WLSERVER_TOUCH_CLICK_DISABLED = 5,
};

extern enum wlserver_touch_click_mode g_nDefaultTouchClickMode;
extern enum wlserver_touch_click_mode g_nTouchClickMode;

void xwayland_surface_role_commit(struct wlr_surface *wlr_surface);

bool wlsession_init( void );
int wlsession_open_kms( const char *device_name );

bool wlserver_init( void );

void wlserver_run(void);

void wlserver_lock(void);
void wlserver_unlock(void);

void wlserver_keyboardfocus( struct wlr_surface *surface );
void wlserver_key( uint32_t key, bool press, uint32_t time );

void wlserver_mousefocus( struct wlr_surface *wlrsurface, int x = 0, int y = 0 );
void wlserver_mousemotion( int x, int y, uint32_t time );
void wlserver_mousebutton( int button, bool press, uint32_t time );
void wlserver_mousewheel( int x, int y, uint32_t time );

void wlserver_touchmotion( double x, double y, int touch_id, uint32_t time );
void wlserver_touchdown( double x, double y, int touch_id, uint32_t time );
void wlserver_touchup( int touch_id, uint32_t time );

void wlserver_send_frame_done( struct wlr_surface *surf, const struct timespec *when );

struct wlserver_output_info {
	const char *name;
	const char *description;
	int phys_width, phys_height; // millimeters
};

void wlserver_set_output_info( const wlserver_output_info *info );

gamescope_xwayland_server_t *wlserver_get_xwayland_server( size_t index );
const char *wlserver_get_wl_display_name( void );

struct wlserver_surface
{
	std::atomic<struct wlr_surface *> wlr;

	// owned by wlserver
	uint32_t wl_id, x11_id;
	bool overridden;
	struct wl_list pending_link;
	struct wl_listener destroy;

	gamescope_xwayland_server_t *xwayland_server;
};

void wlserver_surface_init( struct wlserver_surface *surf, gamescope_xwayland_server_t *server, uint32_t x11_id );
void wlserver_surface_finish( struct wlserver_surface *surf );

void wlserver_set_xwayland_server_mode( size_t idx, int w, int h, int refresh );

extern std::atomic<bool> g_bPendingTouchMovement;
