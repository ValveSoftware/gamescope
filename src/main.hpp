#pragma once

#include <getopt.h>

#include <atomic>

extern const char *gamescope_optstring;
extern const struct option *gamescope_options;

extern std::atomic< bool > g_bRun;

extern int g_nNestedWidth;
extern int g_nNestedHeight;
extern int g_nNestedRefresh; // Hz
extern int g_nNestedUnfocusedRefresh; // Hz

extern uint32_t g_nOutputWidth;
extern uint32_t g_nOutputHeight;
extern int g_nOutputRefresh; // Hz
extern bool g_bOutputHDREnabled;

extern bool g_bFullscreen;

extern bool g_bGrabbed;

enum class GamescopeUpscaleFilter : uint32_t
{
    LINEAR = 0,
    NEAREST,
    FSR,
    NIS,

    _FIRST = LINEAR,
    _LAST = NIS
};

enum class GamescopeUpscaleScaler : uint32_t
{
    AUTO,
    INTEGER,
    FIT,
    FILL,
    STRETCH,

    _FIRST = AUTO,
    _LAST = STRETCH
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

extern bool g_bRt;
extern int g_nOldPolicy;
extern struct sched_param g_schedOldParam;

extern int g_nXWaylandCount;

extern uint32_t g_preferVendorID;
extern uint32_t g_preferDeviceID;

void restore_fd_limit( void );
bool BIsNested( void );
bool BIsVRSession( void );
