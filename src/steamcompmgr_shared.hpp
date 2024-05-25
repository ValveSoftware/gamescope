#pragma once

#include <variant>
#include <string>
#include <utility>
#include <bit>

#include <wlr/util/box.h>
#include "xwayland_ctx.hpp"
#include "gamescope-control-protocol.h"

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

	struct wlr_surface *current_surface() const
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
	std::atomic<struct wlr_surface *> main_surface;

	struct wlr_surface *current_surface()
	{
		return main_surface;
	}

	// owned by wlserver
	struct wlr_xdg_surface *xdg_surface = nullptr;
	steamcompmgr_win_t *win = nullptr;

	std::atomic<bool> mapped = { false };

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
	uint32_t id;

	struct wlserver_xdg_surface_info surface;
	struct wlr_box geometry;
};

struct steamcompmgr_win_t {
	unsigned int	opacity;

	uint64_t seq;

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
	bool outdatedInteractiveFocus = false;

	bool hasHwndStyle;
	uint32_t hwndStyle;
	bool hasHwndStyleEx;
	uint32_t hwndStyleEx;

	motif_hints_t *motif_hints;

	bool nudged;
	bool ignoreOverrideRedirect;

	bool unlockedForFrameCallback;
	bool receivedDoneCommit;

	std::vector< gamescope::Rc<commit_t> > commit_queue;
	std::shared_ptr<std::vector< uint32_t >> icon;

	steamcompmgr_win_type_t		type;

	steamcompmgr_xwayland_win_t& xwayland() { return std::get<steamcompmgr_xwayland_win_t>(_window_types); }
	const steamcompmgr_xwayland_win_t& xwayland() const { return std::get<steamcompmgr_xwayland_win_t>(_window_types); }

	steamcompmgr_xdg_win_t& xdg() { return std::get<steamcompmgr_xdg_win_t>(_window_types); }
	const steamcompmgr_xdg_win_t& xdg() const { return std::get<steamcompmgr_xdg_win_t>(_window_types); }

	std::variant<steamcompmgr_xwayland_win_t, steamcompmgr_xdg_win_t>
		_window_types;

	const struct wlr_box* const  __attribute__((pure)) geometry() const {
		if (type == steamcompmgr_win_type_t::XWAYLAND) {
			typedef struct {
				wlr_box box;
				bool padding[ (sizeof(XWindowAttributes) - sizeof(wlr_box)) ];
			} padded_box;
			
			const padded_box* geo_padded = std::bit_cast<padded_box*, const XWindowAttributes*>(&(xwayland().a));
			const wlr_box* geo = &(geo_padded->box);
			return geo;
		} else {
			const wlr_box* geo = &(xdg().geometry);
			return geo;
		}
	}

	uint32_t id() const
	{
		if (type == steamcompmgr_win_type_t::XWAYLAND)
			return uint32_t(xwayland().id);
		else if (type == steamcompmgr_win_type_t::XDG)
			return xdg().id;
		else
			return ~(0u);
	}

	wlr_surface *main_surface() const
	{
		if (type == steamcompmgr_win_type_t::XWAYLAND)
			return xwayland().surface.main_surface;
		else if (type == steamcompmgr_win_type_t::XDG)
			return xdg().surface.main_surface;
		else
			return nullptr;
	}

	wlr_surface *current_surface() const
	{
		if (type == steamcompmgr_win_type_t::XWAYLAND)
			return xwayland().surface.current_surface();

		return main_surface();
	}

	wlr_surface *override_surface() const
	{
		if (type == steamcompmgr_win_type_t::XWAYLAND)
			return xwayland().surface.override_surface;
		else
			return nullptr;
	}
};

namespace gamescope
{
	struct GamescopeScreenshotInfo
	{
		std::string szScreenshotPath;
		gamescope_control_screenshot_type eScreenshotType = GAMESCOPE_CONTROL_SCREENSHOT_TYPE_BASE_PLANE_ONLY;
		uint32_t uScreenshotFlags = 0;
		bool bX11PropertyRequested = false;
		bool bWaylandRequested = false;
	};

	class CScreenshotManager
	{
	public:
		void TakeScreenshot( GamescopeScreenshotInfo info = GamescopeScreenshotInfo{} )
		{
			std::unique_lock lock{ m_ScreenshotInfoMutex };
			m_ScreenshotInfo = std::move( info );
		}

		void TakeScreenshot( bool bAVIF )
		{
			char szTimeBuffer[ 1024 ];
			time_t currentTime = time(0);
			struct tm *pLocalTime = localtime( &currentTime );
			strftime( szTimeBuffer, sizeof( szTimeBuffer ), bAVIF ? "/tmp/gamescope_%Y-%m-%d_%H-%M-%S.avif" : "/tmp/gamescope_%Y-%m-%d_%H-%M-%S.png", pLocalTime );

			TakeScreenshot( GamescopeScreenshotInfo
			{
				.szScreenshotPath = szTimeBuffer,
			} );
		}

		std::optional<GamescopeScreenshotInfo> ProcessPendingScreenshot()
		{
			std::unique_lock lock{ m_ScreenshotInfoMutex };
			return std::exchange( m_ScreenshotInfo, std::nullopt );
		}

		static CScreenshotManager &Get();
	private:
		std::mutex m_ScreenshotInfoMutex;
		std::optional<GamescopeScreenshotInfo> m_ScreenshotInfo;
	};

	extern CScreenshotManager g_ScreenshotMgr;
}
