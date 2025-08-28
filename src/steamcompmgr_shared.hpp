#pragma once

#include <variant>
#include <string>
#include <utility>

#include <wlr/util/box.h>

#include "xwayland_ctx.hpp"
#include "gamescope-control-protocol.h"

struct commit_t;
struct wlserver_vk_swapchain_feedback;

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

// not always an xdg window anymore
// wayland-y window (not xwayland)
struct wlserver_xdg_surface_info
{
	std::atomic<struct wlr_surface *> main_surface;

	struct wlr_surface *current_surface()
	{
		return main_surface;
	}

	// owned by wlserver
	struct wlr_xdg_surface *xdg_surface = nullptr;
	struct wlr_layer_surface_v1 *layer_surface = nullptr;
	steamcompmgr_win_t *win = nullptr;
	bool bDoneConfigure = false;

	std::atomic<bool> mapped = { false };

	struct wl_list link;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
};


enum class steamcompmgr_win_type_t
{
	XWAYLAND,
	XDG, // could also be layer shell
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

struct Rect
{
	int32_t nX;
	int32_t nY;

	int32_t nWidth;
	int32_t nHeight;
};

extern focus_t g_steamcompmgr_xdg_focus;

struct steamcompmgr_win_t {
	unsigned int	opacity = 0xffffffff;

	uint64_t seq = 0;

	std::shared_ptr<std::string> title;
	bool utf8_title = false;
	pid_t pid = -1;

	bool isSteamLegacyBigPicture = false;
	bool isSteamStreamingClient = false;
	bool isSteamStreamingClientVideo = false;
	uint32_t inputFocusMode = 0;
	uint32_t appID = 0;
	bool isOverlay = false;
	bool isExternalOverlay = false;

	bool IsAnyOverlay() const
	{
		return isOverlay || isExternalOverlay;
	}

	bool isFullscreen = false;
	bool isSysTrayIcon = false;
	bool sizeHintsSpecified = false;
	bool skipTaskbar = false; 
	bool skipPager = false;
	unsigned int requestedWidth = 0;
	unsigned int requestedHeight = 0;
	bool is_dialog = false;
	bool maybe_a_dropdown = false;
	bool outdatedInteractiveFocus = false;

	uint64_t last_commit_first_latch_time = 0;
	uint64_t last_commit_present_time = 0;

	bool hasHwndStyle = false;
	uint32_t hwndStyle = 0;
	bool hasHwndStyleEx = false;
	uint32_t hwndStyleEx = 0;

	bool bHasHadNonSRGBColorSpace = false;

	bool nudged = false;
	bool ignoreOverrideRedirect = false;

	bool unlockedForFrameCallback = false;
	bool receivedDoneCommit = false;

	std::shared_ptr<std::string> engineName;

	std::vector< gamescope::Rc<commit_t> > commit_queue;
	std::shared_ptr<std::vector< uint32_t >> icon;

	steamcompmgr_win_type_t		type;

	steamcompmgr_xwayland_win_t& xwayland() { return std::get<steamcompmgr_xwayland_win_t>(_window_types); }
	const steamcompmgr_xwayland_win_t& xwayland() const { return std::get<steamcompmgr_xwayland_win_t>(_window_types); }

	steamcompmgr_xdg_win_t& xdg() { return std::get<steamcompmgr_xdg_win_t>(_window_types); }
	const steamcompmgr_xdg_win_t& xdg() const { return std::get<steamcompmgr_xdg_win_t>(_window_types); }

	std::variant<steamcompmgr_xwayland_win_t, steamcompmgr_xdg_win_t>
		_window_types;

	focus_t *GetFocus() const
	{
		if (type == steamcompmgr_win_type_t::XWAYLAND)
			return &xwayland().ctx->focus;
		else if (type == steamcompmgr_win_type_t::XDG)
			return &g_steamcompmgr_xdg_focus;
		else
			return nullptr;
	}

	Rect GetGeometry() const
	{
		if (type == steamcompmgr_win_type_t::XWAYLAND)
			return Rect{ xwayland().a.x, xwayland().a.y, xwayland().a.width, xwayland().a.height };
		else if (type == steamcompmgr_win_type_t::XDG)
			return Rect{ xdg().geometry.x, xdg().geometry.y, xdg().geometry.width, xdg().geometry.height };
		else
			return Rect{};
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

	gamescope::VirtualConnectorKey_t GetVirtualConnectorKey( gamescope::VirtualConnectorStrategy eStrategy )
	{
		switch ( eStrategy )
		{
		default:
		case gamescope::VirtualConnectorStrategies::SingleApplication:
		case gamescope::VirtualConnectorStrategies::SteamControlled:
			return 0;
		case gamescope::VirtualConnectorStrategies::PerAppId:
			return static_cast<gamescope::VirtualConnectorKey_t>( this->appID );
		case gamescope::VirtualConnectorStrategies::PerWindow:
			return static_cast<gamescope::VirtualConnectorKey_t>( this->seq );
		}
	}
};

extern std::atomic<bool> hasRepaint;

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
			hasRepaint = true;
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
