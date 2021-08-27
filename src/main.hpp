#pragma once

#include <atomic>

#define GAMESCOPE_OPTIONS ":R:T:C:w:h:W:H:m:r:o:NFSvVecsdLnbfxO:"

void wayland_commit(struct wlr_surface *surf, struct wlr_buffer *buf);

extern std::atomic< bool > g_bRun;

extern int g_nNestedWidth;
extern int g_nNestedHeight;
extern int g_nNestedRefresh; // Hz
extern int g_nNestedUnfocusedRefresh; // Hz

extern uint32_t g_nOutputWidth;
extern uint32_t g_nOutputHeight;
extern int g_nOutputRefresh; // Hz

extern bool g_bFullscreen;

extern bool g_bFilterGameWindow;

extern bool g_bBorderlessOutputWindow;

extern bool g_bTakeScreenshot;

extern bool g_bNiceCap;
extern int g_nOldNice;
extern int g_nNewNice;

int BIsNested( void );
