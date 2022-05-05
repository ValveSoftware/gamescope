#include <stdint.h>

extern "C" {
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/wlr_texture.h>
}

extern uint32_t currentOutputWidth;
extern uint32_t currentOutputHeight;

unsigned int get_time_in_milliseconds(void);
uint64_t get_time_in_nanos();
void sleep_for_nanos(uint64_t nanos);
void sleep_until_nanos(uint64_t nanos);

void steamcompmgr_main(int argc, char **argv);

#include "rendervulkan.hpp"

#include <mutex>
#include <vector>

#include <wlr/render/dmabuf.h>

#include <X11/extensions/Xfixes.h>

struct _XDisplay;
struct win;
struct xwayland_ctx_t;
class gamescope_xwayland_server_t;

static const uint32_t g_zposBase = 0;
static const uint32_t g_zposOverride = 1;
static const uint32_t g_zposExternalOverlay = 2;
static const uint32_t g_zposOverlay = 3;
static const uint32_t g_zposCursor = 4;

class MouseCursor
{
public:
	explicit MouseCursor(xwayland_ctx_t *ctx);

	int x() const;
	int y() const;

	void move(int x, int y);
	void updatePosition();
	void constrainPosition();
	void resetPosition();

	void paint(struct win *window, struct win *fit, struct FrameInfo_t *frameInfo);
	void setDirty();

	// Will take ownership of data.
	bool setCursorImage(char *data, int w, int h, int hx, int hy);

	void hide() { m_lastMovedTime = 0; checkSuspension(); }

	bool isHidden() { return m_hideForMovement; }

	void forcePosition(int x, int y)
	{
		warp(x, y);
		m_x = x;
		m_y = y;
	}

private:
	void warp(int x, int y);
	void checkSuspension();

	void queryGlobalPosition(int &x, int &y);
	void queryPositions(int &rootX, int &rootY, int &winX, int &winY);
	void queryButtonMask(unsigned int &mask);

	bool getTexture();

	int m_x = 0, m_y = 0;
	int m_hotspotX = 0, m_hotspotY = 0;

	std::shared_ptr<CVulkanTexture> m_texture;
	bool m_dirty;
	bool m_imageEmpty;

	unsigned int m_lastMovedTime = 0;
	bool m_hideForMovement;

	PointerBarrier m_scaledFocusBarriers[4] = { None };

	xwayland_ctx_t *m_ctx;

	int m_lastX = 0;
	int m_lastY = 0;
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

void nudge_steamcompmgr( void );
void take_screenshot( void );

extern void mangoapp_update( uint64_t visible_frametime, uint64_t app_frametime_ns, uint64_t latency_ns );
gamescope_xwayland_server_t *steamcompmgr_get_focused_server();
struct wlr_surface *steamcompmgr_get_server_input_surface( size_t idx );

extern uint64_t g_SteamCompMgrVBlankTime;
extern pid_t focusWindow_pid;