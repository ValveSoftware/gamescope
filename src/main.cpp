#include <X11/Xlib.h>

#include <cstdio>
#include <thread>
#include <mutex>
#include <vector>
#include <cstring>
#include <sys/capability.h>
#include <sys/stat.h>

#include <getopt.h>
#include <signal.h>
#include <unistd.h>
#include <float.h>

#include "main.hpp"
#include "steamcompmgr.hpp"
#include "drm.hpp"
#include "rendervulkan.hpp"
#include "sdlwindow.hpp"
#include "wlserver.hpp"
#include "gpuvis_trace_utils.h"

#if HAVE_PIPEWIRE
#include "pipewire.hpp"
#endif

const char *gamescope_optstring = nullptr;

const struct option *gamescope_options = (struct option[]){
	{ "help", no_argument, nullptr, 0 },
	{ "nested-width", required_argument, nullptr, 'w' },
	{ "nested-height", required_argument, nullptr, 'h' },
	{ "nested-refresh", required_argument, nullptr, 'r' },
	{ "max-scale", required_argument, nullptr, 'm' },
	{ "integer-scale", no_argument, nullptr, 'i' },
	{ "output-width", required_argument, nullptr, 'W' },
	{ "output-height", required_argument, nullptr, 'H' },
	{ "nearest-neighbor-filter", no_argument, nullptr, 'n' },
	{ "fsr-upscaling", no_argument, nullptr, 'U' },

	// nested mode options
	{ "nested-unfocused-refresh", required_argument, nullptr, 'o' },
	{ "borderless", no_argument, nullptr, 'b' },
	{ "fullscreen", no_argument, nullptr, 'f' },

	// embedded mode options
	{ "disable-layers", no_argument, nullptr, 0 },
	{ "debug-layers", no_argument, nullptr, 0 },
	{ "prefer-output", required_argument, nullptr, 'O' },
	{ "default-touch-mode", required_argument, nullptr, 0 },
	{ "generate-drm-mode", required_argument, nullptr, 0 },

	// wlserver options
	{ "xwayland-count", required_argument, nullptr, 0 },

	// steamcompmgr options
	{ "cursor", required_argument, nullptr, 0 },
	{ "cursor-hotspot", required_argument, nullptr, 0 },
	{ "ready-fd", required_argument, nullptr, 'R' },
	{ "stats-path", required_argument, nullptr, 'T' },
	{ "hide-cursor-delay", required_argument, nullptr, 'C' },
	{ "debug-focus", no_argument, nullptr, 0 },
	{ "synchronous-x11", no_argument, nullptr, 0 },
	{ "debug-hud", no_argument, nullptr, 'v' },
	{ "debug-events", no_argument, nullptr, 0 },
	{ "steam", no_argument, nullptr, 'e' },
	{ "force-composition", no_argument, nullptr, 'c' },
	{ "composite-debug", no_argument, nullptr, 0 },
	{ "disable-xres", no_argument, nullptr, 'x' },
	{ "fade-out-duration", required_argument, nullptr, 0 },

	{} // keep last
};

const char usage[] =
	"usage: gamescope [options...] -- [command...]\n"
	"\n"
	"Options:\n"
	"  --help                         show help message\n"
	"  -w, --nested-width             game width\n"
	"  -h, --nested-height            game height\n"
	"  -r, --nested-refresh           game refresh rate (frames per second)\n"
	"  -m, --max-scale                maximum scale factor\n"
	"  -i, --integer-scale            force scale factor to integer\n"
	"  -W, --output-width             output width\n"
	"  -H, --output-height            output height\n"
	"  -n, --nearest-neighbor-filter  use nearest neighbor filtering\n"
	"  -U  --fsr-upscaling            use AMD FidelityFX™ Super Resolution 1.0 for upscaling"
	"  --cursor                       path to default cursor image\n"
	"  -R, --ready-fd                 notify FD when ready\n"
	"  -T, --stats-path               write statistics to path\n"
	"  -C, --hide-cursor-delay        hide cursor image after delay\n"
	"  -e, --steam                    enable Steam integration\n"
	" --xwayland-count                create N xwayland servers\n"
	"\n"
	"Nested mode options:\n"
	"  -o, --nested-unfocused-refresh game refresh rate when unfocused\n"
	"  -b, --borderless               make the window borderless\n"
	"  -f, --fullscreen               make the window fullscreen\n"
	"\n"
	"Embedded mode options:\n"
	"  -O, --prefer-output            list of connectors in order of preference\n"
	"  --default-touch-mode           0: hover, 1: left, 2: right, 3: middle, 4: passthrough\n"
	"  --generate-drm-mode            DRM mode generation algorithm (cvt, fixed)\n"
	"\n"
	"Debug options:\n"
	"  --disable-layers               disable libliftoff (hardware planes)\n"
	"  --debug-layers                 debug libliftoff\n"
	"  --debug-focus                  debug XWM focus\n"
	"  --synchronous-x11              force X11 connection synchronization\n"
	"  --debug-hud                    paint HUD with debug info\n"
	"  --debug-events                 debug X11 events\n"
	"  --force-composition            disable direct scan-out\n"
	"  --composite-debug              draw frame markers on alternating corners of the screen when compositing\n"
	"  --disable-xres                 disable XRes for PID lookup\n"
	"\n"
	"Keyboard shortcuts:\n"
	"  Super + F                      toggle fullscreen\n"
	"  Super + N                      toggle nearest neighbour filtering\n"
	"  Super + U                      toggle FSR upscaling\n"
	"  Super + S                      take a screenshot\n"
	"";

std::atomic< bool > g_bRun{true};

int g_nNestedWidth = 0;
int g_nNestedHeight = 0;
int g_nNestedRefresh = 0;
int g_nNestedUnfocusedRefresh = 0;

uint32_t g_nOutputWidth = 0;
uint32_t g_nOutputHeight = 0;
int g_nOutputRefresh = 0;

bool g_bFullscreen = false;

bool g_bIsNested = false;

bool g_bFilterGameWindow = true;
bool g_fsrUpscale = false;

bool g_bBorderlessOutputWindow = false;

int g_nXWaylandCount = 1;

bool g_bNiceCap = false;
int g_nOldNice = 0;
int g_nNewNice = 0;

float g_flMaxWindowScale = FLT_MAX;
bool g_bIntegerScale = false;

pthread_t g_mainThread;

bool BIsNested()
{
	return g_bIsNested;
}

static bool initOutput(int preferredWidth, int preferredHeight, int preferredRefresh);
static void steamCompMgrThreadRun(int argc, char **argv);

static std::string build_optstring(const struct option *options)
{
	std::string optstring;
	for (size_t i = 0; options[i].name != nullptr; i++) {
		if (!options[i].name || !options[i].val)
			continue;

		assert(optstring.find((char) options[i].val) == std::string::npos);

		char str[] = { (char) options[i].val, '\0' };
		optstring.append(str);

		if (options[i].has_arg)
			optstring.append(":");
	}
	return optstring;
}

static enum drm_mode_generation parse_drm_mode_generation(const char *str)
{
	if (strcmp(str, "cvt") == 0) {
		return DRM_MODE_GENERATE_CVT;
	} else if (strcmp(str, "fixed") == 0) {
		return DRM_MODE_GENERATE_FIXED;
	} else {
		fprintf( stderr, "gamescope: invalid value for --generate-drm-mode\n" );
		exit(1);
	}
}

static void handle_signal( int sig )
{
	switch ( sig ) {
	case SIGUSR2:
		take_screenshot();
		break;
	case SIGTERM:
	case SIGINT:
		fprintf( stderr, "gamescope: received kill signal, terminating!\n" );
		g_bRun = false;
		break;
	default:
		assert( false ); // unreachable
	}
}

int main(int argc, char **argv)
{
	int nPreferredOutputWidth = 0;
	int nPreferredOutputHeight = 0;

	static std::string optstring = build_optstring(gamescope_options);
	gamescope_optstring = optstring.c_str();

	int o;
	int opt_index = -1;
	while ((o = getopt_long(argc, argv, gamescope_optstring, gamescope_options, &opt_index)) != -1)
	{
		const char *opt_name;
		switch (o) {
			case 'w':
				g_nNestedWidth = atoi( optarg );
				break;
			case 'h':
				g_nNestedHeight = atoi( optarg );
				break;
			case 'r':
				g_nNestedRefresh = atoi( optarg );
				break;
			case 'W':
				nPreferredOutputWidth = atoi( optarg );
				break;
			case 'H':
				nPreferredOutputHeight = atoi( optarg );
				break;
			case 'o':
				g_nNestedUnfocusedRefresh = atoi( optarg );
				break;
			case 'm':
				g_flMaxWindowScale = atof( optarg );
				break;
			case 'i':
				g_bIntegerScale = true;
				break;
			case 'n':
				g_bFilterGameWindow = false;
				break;
			case 'b':
				g_bBorderlessOutputWindow = true;
				break;
			case 'f':
				g_bFullscreen = true;
				break;
			case 'O':
				g_sOutputName = optarg;
				break;
			case 'U':
				g_fsrUpscale = true;
				break;
			case 0: // long options without a short option
				opt_name = gamescope_options[opt_index].name;
				if (strcmp(opt_name, "help") == 0) {
					fprintf(stderr, "%s", usage);
					return 0;
				} else if (strcmp(opt_name, "disable-layers") == 0) {
					g_bUseLayers = false;
				} else if (strcmp(opt_name, "debug-layers") == 0) {
					g_bDebugLayers = true;
				} else if (strcmp(opt_name, "xwayland-count") == 0) {
					g_nXWaylandCount = atoi( optarg );
				} else if (strcmp(opt_name, "composite-debug") == 0) {
					g_bIsCompositeDebug = true;
				} else if (strcmp(opt_name, "default-touch-mode") == 0) {
					g_nDefaultTouchClickMode = (enum wlserver_touch_click_mode) atoi( optarg );
					g_nTouchClickMode = g_nDefaultTouchClickMode;
				} else if (strcmp(opt_name, "generate-drm-mode") == 0) {
					g_drmModeGeneration = parse_drm_mode_generation( optarg );
				}
				break;
			case '?':
				fprintf( stderr, "See --help for a list of options.\n" );
				return 1;
		}
	}

	cap_t caps = cap_get_proc();
	if ( caps != nullptr )
	{
		cap_flag_value_t nicecapvalue = CAP_CLEAR;
		cap_get_flag( caps, CAP_SYS_NICE, CAP_EFFECTIVE, &nicecapvalue );

		if ( nicecapvalue == CAP_SET )
		{
			g_bNiceCap = true;

			errno = 0;
			int nOldNice = nice( 0 );
			if ( nOldNice != -1 && errno == 0 )
			{
				g_nOldNice = nOldNice;
			}

			errno = 0;
			int nNewNice = nice( -20 );
			if ( nNewNice != -1 && errno == 0 )
			{
				g_nNewNice = nNewNice;
			}
		}
	}

	if ( g_bNiceCap == false )
	{
		fprintf( stderr, "No CAP_SYS_NICE, falling back to regular-priority compute and threads.\nPerformance will be affected.\n" );
	}

	if ( gpuvis_trace_init() != -1 )
	{
		fprintf( stderr, "Tracing is enabled\n");
	}

	XInitThreads();
	g_mainThread = pthread_self();

	if ( getenv("DISPLAY") != NULL || getenv("WAYLAND_DISPLAY") != NULL )
	{
		g_bIsNested = true;
	}

	if ( !wlsession_init() )
	{
		fprintf( stderr, "Failed to initialize Wayland session\n" );
		return 1;
	}

	if ( BIsNested() )
	{
		if ( SDL_Init( SDL_INIT_VIDEO | SDL_INIT_EVENTS ) != 0 )
		{
			fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
			return 1;
		}
	}

	if ( !vulkan_init() )
	{
		fprintf( stderr, "Failed to initialize Vulkan\n" );
		return 1;
	}

	if ( !initOutput( nPreferredOutputWidth, nPreferredOutputHeight, g_nNestedRefresh ) )
	{
		fprintf( stderr, "Failed to initialize output\n" );
		return 1;
	}

	if ( !vulkan_init_formats() )
	{
		fprintf( stderr, "vulkan_init_formats failed\n" );
		return 1;
	}

	if ( !vulkan_make_output() )
	{
		fprintf( stderr, "vulkan_make_output failed\n" );
		return 1;
	}

	// Prevent our clients from connecting to the parent compositor
	unsetenv("WAYLAND_DISPLAY");

	// If DRM format modifiers aren't supported, prevent our clients from using
	// DCC, as this can cause tiling artifacts.
	if ( !g_vulkanSupportsModifiers )
	{
		const char *pchR600Debug = getenv( "R600_DEBUG" );

		if ( pchR600Debug == nullptr )
		{
			setenv( "R600_DEBUG", "nodcc", 1 );
		}
		else if ( strstr( pchR600Debug, "nodcc" ) == nullptr )
		{
			std::string strPreviousR600Debug = pchR600Debug;
			strPreviousR600Debug.append( ",nodcc" );
			setenv( "R600_DEBUG", strPreviousR600Debug.c_str(), 1 );
		}
	}

	if ( g_nNestedHeight == 0 )
	{
		if ( g_nNestedWidth != 0 )
		{
			fprintf( stderr, "Cannot specify -w without -h\n" );
			return 1;
		}
		g_nNestedWidth = g_nOutputWidth;
		g_nNestedHeight = g_nOutputHeight;
	}
	if ( g_nNestedWidth == 0 )
		g_nNestedWidth = g_nNestedHeight * 16 / 9;

	if ( !wlserver_init() )
	{
		fprintf( stderr, "Failed to initialize wlserver\n" );
		return 1;
	}
	
	gamescope_xwayland_server_t *base_server = wlserver_get_xwayland_server(0);

	setenv("DISPLAY", base_server->get_nested_display_name(), 1);
	if (g_nXWaylandCount > 1)
	{
		for (int i = 1; i < g_nXWaylandCount; i++)
		{
			char env_name[64];
			snprintf(env_name, sizeof(env_name), "STEAM_GAME_DISPLAY_%d", i - 1);
			gamescope_xwayland_server_t *server = wlserver_get_xwayland_server(i);
			setenv(env_name, server->get_nested_display_name(), 1);
		}
	}
	else
	{
		setenv("STEAM_GAME_DISPLAY_0", base_server->get_nested_display_name(), 1);
	}
	setenv("GAMESCOPE_WAYLAND_DISPLAY", wlserver_get_wl_display_name(), 1);

#if HAVE_PIPEWIRE
	if ( !init_pipewire() )
	{
		fprintf( stderr, "Warning: failed to setup PipeWire, screen capture won't be available\n" );
	}
#endif

	std::thread steamCompMgrThread( steamCompMgrThreadRun, argc, argv );

	signal( SIGTERM, handle_signal );
	signal( SIGINT, handle_signal );
	signal( SIGUSR2, handle_signal );

	wlserver_run();

	steamCompMgrThread.join();
}

static void steamCompMgrThreadRun(int argc, char **argv)
{
	pthread_setname_np( pthread_self(), "gamescope-xwm" );

	steamcompmgr_main( argc, argv );

	pthread_kill( g_mainThread, SIGINT );
}

static bool initOutput( int preferredWidth, int preferredHeight, int preferredRefresh )
{
	if ( g_bIsNested == true )
	{
		g_nOutputWidth = preferredWidth;
		g_nOutputHeight = preferredHeight;
		g_nOutputRefresh = preferredRefresh;

		if ( g_nOutputHeight == 0 )
		{
			if ( g_nOutputWidth != 0 )
			{
				fprintf( stderr, "Cannot specify -W without -H\n" );
				return 1;
			}
			g_nOutputHeight = 720;
		}
		if ( g_nOutputWidth == 0 )
			g_nOutputWidth = g_nOutputHeight * 16 / 9;
		if ( g_nOutputRefresh == 0 )
			g_nOutputRefresh = 60;

		return sdlwindow_init();
	}
	else
	{
		return init_drm( &g_DRM, preferredWidth, preferredHeight, preferredRefresh );
	}
}
