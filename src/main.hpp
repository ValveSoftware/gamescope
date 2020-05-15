#include <SDL.h>
#include <SDL_vulkan.h>

extern SDL_Window *window;

int initOutput(void);

void startSteamCompMgr(void);

void register_signal(void);

void wayland_commit(struct wlr_surface *surf, struct wlr_buffer *buf);

extern int g_nNestedWidth;
extern int g_nNestedHeight;
extern int g_nNestedRefresh;

extern uint32_t g_nOutputWidth;
extern uint32_t g_nOutputHeight;
extern int g_nOutputRefresh;

extern bool g_bFilterGameWindow;

extern bool g_bTakeScreenshot;

extern uint32_t g_nSubCommandArg;

int BIsNested( void );
