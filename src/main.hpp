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

extern bool g_bFullscreen;

extern bool g_bFilterGameWindow;

extern bool g_fsrUpscale;
extern int g_fsrSharpness;

extern bool g_bBorderlessOutputWindow;

extern bool g_bNiceCap;
extern int g_nOldNice;
extern int g_nNewNice;

extern int g_nXWaylandCount;

bool BIsNested( void );
