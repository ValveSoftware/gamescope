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

struct ResListEntry_t {
	struct wlr_surface *surf;
	struct wlr_buffer *buf;
};

struct _XDisplay;
struct win;

class MouseCursor
{
public:
	explicit MouseCursor(_XDisplay *display);

	int x() const;
	int y() const;

	void move(int x, int y);
	void updatePosition();
	void constrainPosition();
	void resetPosition();

	void paint(struct win *window, struct Composite_t *pComposite,
			   struct VulkanPipeline_t *pPipeline);
	void setDirty();

	// Will take ownership of data.
	bool setCursorImage(char *data, int w, int h, int hx, int hy);

	void hide() { m_lastMovedTime = 0; checkSuspension(); }

private:
	void warp(int x, int y);
	void checkSuspension();

	void queryGlobalPosition(int &x, int &y);
	void queryPositions(int &rootX, int &rootY, int &winX, int &winY);
	void queryButtonMask(unsigned int &mask);

	bool getTexture();

	int m_x = 0, m_y = 0;
	int m_hotspotX = 0, m_hotspotY = 0;
	int m_width = 0, m_height = 0;

	VulkanTexture_t m_texture = 0;
	bool m_dirty;
	bool m_imageEmpty;

	unsigned int m_lastMovedTime = 0;
	bool m_hideForMovement;

	PointerBarrier m_scaledFocusBarriers[4] = { None };

	_XDisplay *m_display;
};

extern std::mutex wayland_commit_lock;
extern std::vector<ResListEntry_t> wayland_commit_queue;

extern std::vector< wlr_surface * > wayland_surfaces_deleted;

extern bool hasFocusWindow;

// These are used for touch scaling, so it's really the window that's focused for touch
extern float focusedWindowScaleX;
extern float focusedWindowScaleY;
extern float focusedWindowOffsetX;
extern float focusedWindowOffsetY;

extern uint32_t inputCounter;

void nudge_steamcompmgr( void );
void take_screenshot( void );
void request_repaint( void );
