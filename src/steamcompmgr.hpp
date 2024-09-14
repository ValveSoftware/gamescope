#include <stdint.h>

#include "wlr_begin.hpp"
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/render/dmabuf.h>
#include "wlr_end.hpp"

extern uint32_t currentOutputWidth;
extern uint32_t currentOutputHeight;

unsigned int get_time_in_milliseconds(void);
uint64_t get_time_in_nanos();
void sleep_for_nanos(uint64_t nanos);
void sleep_until_nanos(uint64_t nanos);
timespec nanos_to_timespec( uint64_t ulNanos );

void steamcompmgr_main(int argc, char **argv);

#include "rendervulkan.hpp"
#include "wlserver.hpp"
#include "vblankmanager.hpp"

#include <mutex>
#include <vector>

#include <X11/extensions/Xfixes.h>

struct _XDisplay;
struct steamcompmgr_win_t;
struct xwayland_ctx_t;
class gamescope_xwayland_server_t;

static const uint32_t g_zposBase = 0;
static const uint32_t g_zposOverride = 1;
static const uint32_t g_zposExternalOverlay = 2;
static const uint32_t g_zposOverlay = 3;
static const uint32_t g_zposCursor = 4;
static const uint32_t g_zposMuraCorrection = 5;

extern bool g_bHDRItmEnable;
extern bool g_bForceHDRSupportDebug;

extern EStreamColorspace g_ForcedNV12ColorSpace;

struct CursorBarrierInfo
{
	int x1 = 0;
	int y1 = 0;
	int x2 = 0;
	int y2 = 0;
};

struct CursorBarrier
{
	PointerBarrier obj = None;
	CursorBarrierInfo info = {};
};

class MouseCursor
{
public:
	explicit MouseCursor(xwayland_ctx_t *ctx);

	int x() const;
	int y() const;

	void paint(steamcompmgr_win_t *window, steamcompmgr_win_t *fit, FrameInfo_t *frameInfo);
	void setDirty();

	// Will take ownership of data.
	bool setCursorImage(char *data, int w, int h, int hx, int hy);
	bool setCursorImageByName(const char *name);

	void hide()
	{
		wlserver_lock();
		wlserver_mousehide();
		wlserver_unlock( false );
		checkSuspension();
	}

	void UpdatePosition();

	bool isHidden() { return wlserver.bCursorHidden || m_imageEmpty; }
	bool imageEmpty() const { return m_imageEmpty; }

	void undirty() { getTexture(); }

	xwayland_ctx_t *getCtx() const { return m_ctx; }

	bool needs_server_flush() const { return m_needs_server_flush; }
	void inform_flush() { m_needs_server_flush = false; }

	void GetDesiredSize( int& nWidth, int &nHeight );

	void checkSuspension();

	bool IsConstrained() const { return m_bConstrained; }
private:

	bool getTexture();

	void updateCursorFeedback( bool bForce = false );

	int m_x = 0, m_y = 0;
	bool m_bConstrained = false;
	int m_hotspotX = 0, m_hotspotY = 0;

	gamescope::OwningRc<CVulkanTexture> m_texture;
	bool m_dirty;
	bool m_imageEmpty;

	xwayland_ctx_t *m_ctx;

	bool m_bCursorVisibleFeedback = false;
	bool m_needs_server_flush = false;
};

extern std::vector< wlr_surface * > wayland_surfaces_deleted;

extern bool hasFocusWindow;

// These are used for touch scaling, so it's really the window that's focused for touch
extern float focusedWindowScaleX;
extern float focusedWindowScaleY;
extern float focusedWindowOffsetX;
extern float focusedWindowOffsetY;

extern bool g_bFSRActive;

extern uint32_t inputCounter;
extern uint64_t g_lastWinSeq;

void nudge_steamcompmgr( void );
void force_repaint( void );

extern void mangoapp_update( uint64_t visible_frametime, uint64_t app_frametime_ns, uint64_t latency_ns );
gamescope_xwayland_server_t *steamcompmgr_get_focused_server();
struct wlr_surface *steamcompmgr_get_server_input_surface( size_t idx );
wlserver_vk_swapchain_feedback* steamcompmgr_get_base_layer_swapchain_feedback();

struct wlserver_x11_surface_info *lookup_x11_surface_info_from_xid( gamescope_xwayland_server_t *xwayland_server, uint32_t xid );

extern gamescope::VBlankTime g_SteamCompMgrVBlankTime;
extern pid_t focusWindow_pid;

void init_xwayland_ctx(uint32_t serverId, gamescope_xwayland_server_t *xwayland_server);
void gamescope_set_selection(std::string contents, GamescopeSelection eSelection);
void gamescope_set_reshade_effect(std::string effect_path);
void gamescope_clear_reshade_effect();

MouseCursor *steamcompmgr_get_current_cursor();
MouseCursor *steamcompmgr_get_server_cursor(uint32_t serverId);

extern gamescope::ConVar<bool> cv_tearing_enabled;

extern void steamcompmgr_set_app_refresh_cycle_override( gamescope::GamescopeScreenType type, int override_fps, bool change_refresh, bool change_fps_cap );
