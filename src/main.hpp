#pragma once

#define GAMESCOPE_OPTIONS ":R:T:C:w:h:W:H:r:o:NFSvVecsdLnbfx"

int initOutput(void);

void startSteamCompMgr(void);

void register_signal(void);

void wayland_commit(struct wlr_surface *surf, struct wlr_buffer *buf);

extern int g_nNestedWidth;
extern int g_nNestedHeight;
extern int g_nNestedRefresh;
extern int g_nNestedUnfocusedRefresh;

extern uint32_t g_nOutputWidth;
extern uint32_t g_nOutputHeight;
extern int g_nOutputRefresh;

extern bool g_bFullscreen;

extern bool g_bFilterGameWindow;

extern bool g_bBorderlessOutputWindow;

extern bool g_bTakeScreenshot;

extern uint32_t g_nSubCommandArg;

extern bool g_bNiceCap;
extern int g_nOldNice;
extern int g_nNewNice;

int BIsNested( void );
