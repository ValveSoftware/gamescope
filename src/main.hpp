#pragma once

#include <getopt.h>

#include <atomic>

extern const char *gamescope_optstring;
extern const struct option *gamescope_options;

extern std::atomic< bool > g_bRun;

extern int g_nNestedWidth;
extern int g_nNestedHeight;
extern int g_nNestedRefresh; // mHz
extern int g_nNestedUnfocusedRefresh; // mHz
extern int g_nNestedDisplayIndex;

extern uint32_t g_nOutputWidth;
extern uint32_t g_nOutputHeight;
extern bool g_bForceRelativeMouse;
extern int g_nOutputRefresh; // mHz
extern bool g_bOutputHDREnabled;
extern bool g_bForceInternal;

extern bool g_bFullscreen;

extern bool g_bGrabbed;

extern float g_mouseSensitivity;
extern const char *g_sOutputName;

enum class GamescopeUpscaleFilter : uint32_t
{
    LINEAR = 0,
    NEAREST,
    FSR,
    NIS,
    PIXEL,

    FROM_VIEW = 0xF, // internal
};

enum class GamescopeUpscaleScaler : uint32_t
{
    AUTO,
    INTEGER,
    FIT,
    FILL,
    STRETCH,
};

extern GamescopeUpscaleFilter g_upscaleFilter;
extern GamescopeUpscaleScaler g_upscaleScaler;
extern GamescopeUpscaleFilter g_wantedUpscaleFilter;
extern GamescopeUpscaleScaler g_wantedUpscaleScaler;
extern int g_upscaleFilterSharpness;

extern bool g_bBorderlessOutputWindow;

extern bool g_bNiceCap;
extern int g_nOldNice;
extern int g_nNewNice;

extern bool g_bExposeWayland;

extern bool g_bRt;
extern int g_nOldPolicy;
extern struct sched_param g_schedOldParam;

extern int g_nXWaylandCount;

extern uint32_t g_preferVendorID;
extern uint32_t g_preferDeviceID;

enum class SelectedPointerType : uint32_t
{
    NONE,
    TOUCHPAD,
    MOUSE,
    ALL,
};

extern bool g_tapToClick;
extern bool g_tapAndDrag;
extern bool g_dragLock;
extern SelectedPointerType g_naturalScrolling;

void restore_fd_limit( void );
