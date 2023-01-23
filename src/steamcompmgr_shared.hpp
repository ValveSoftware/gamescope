#pragma once

#include "xwayland_ctx.hpp"
#include <variant>

struct commit_t;
struct wlserver_vk_swapchain_feedback;

struct motif_hints_t
{
	unsigned long flags;
	unsigned long functions;
	unsigned long decorations;
	long input_mode;
	unsigned long status;
};

struct wlserver_x11_surface_info
{
	std::atomic<struct wlr_surface *> override_surface;
	std::atomic<struct wlr_surface *> main_surface;

	struct wlr_surface *current_surface()
	{
		if ( override_surface )
			return override_surface;
		return main_surface;
	}

	// owned by wlserver
	uint32_t wl_id, x11_id;
	struct wl_list pending_link;

	gamescope_xwayland_server_t *xwayland_server;
};

struct wlserver_xdg_surface_info
{
	struct wlr_xdg_toplevel *xdg_toplevel = nullptr;

	struct wl_list link;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
};


enum class steamcompmgr_win_type_t
{
	XWAYLAND,
	XDG,
};

struct steamcompmgr_xwayland_win_t
{
	steamcompmgr_win_t		*next;

	Window		id;
	XWindowAttributes	a;
	Damage		damage;
	unsigned long	map_sequence;
	unsigned long	damage_sequence;

	Window transientFor;

	struct wlserver_x11_surface_info surface;

	xwayland_ctx_t *ctx;
};

struct steamcompmgr_xdg_win_t
{
	struct wlserver_xdg_surface_info surface;
};

struct steamcompmgr_win_t {
	unsigned int	opacity;

	std::shared_ptr<std::string> title;
	bool utf8_title;
	pid_t pid;

	bool isSteamLegacyBigPicture;
	bool isSteamStreamingClient;
	bool isSteamStreamingClientVideo;
	uint32_t inputFocusMode;
	uint32_t appID;
	bool isOverlay;
	bool isExternalOverlay;
	bool isFullscreen;
	bool isSysTrayIcon;
	bool sizeHintsSpecified;
	bool skipTaskbar;
	bool skipPager;
	unsigned int requestedWidth;
	unsigned int requestedHeight;
	bool is_dialog;
	bool maybe_a_dropdown;

	bool hasHwndStyle;
	uint32_t hwndStyle;
	bool hasHwndStyleEx;
	uint32_t hwndStyleEx;

	motif_hints_t *motif_hints;

	bool nudged;
	bool ignoreOverrideRedirect;

	unsigned int mouseMoved;

	std::vector< std::shared_ptr<commit_t> > commit_queue;
	std::shared_ptr<std::vector< uint32_t >> icon;

	steamcompmgr_win_type_t		type;

	steamcompmgr_xwayland_win_t& xwayland() { return std::get<steamcompmgr_xwayland_win_t>(_window_types); }
	const steamcompmgr_xwayland_win_t& xwayland() const { return std::get<steamcompmgr_xwayland_win_t>(_window_types); }

	steamcompmgr_xdg_win_t& xdg() { return std::get<steamcompmgr_xdg_win_t>(_window_types); }
	const steamcompmgr_xdg_win_t& xdg() const { return std::get<steamcompmgr_xdg_win_t>(_window_types); }

	std::variant<steamcompmgr_xwayland_win_t, steamcompmgr_xdg_win_t>
		_window_types;
};
