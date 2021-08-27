#include <X11/Xlib.h>

#include <thread>
#include <mutex>
#include <vector>
#include <cstring>
#include <sys/capability.h>
#include <sys/stat.h>

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

std::atomic< bool > g_bRun{true};

int g_nNestedWidth = 0;
int g_nNestedHeight = 0;
int g_nNestedRefresh = 0;
int g_nNestedUnfocusedRefresh = 0;

uint32_t g_nOutputWidth = 0;
uint32_t g_nOutputHeight = 0;
int g_nOutputRefresh = 60;

bool g_bFullscreen = false;

bool g_bIsNested = false;

bool g_bFilterGameWindow = true;

bool g_bBorderlessOutputWindow = false;

bool g_bTakeScreenshot = false;

bool g_bNiceCap = false;
int g_nOldNice = 0;
int g_nNewNice = 0;

float g_flMaxWindowScale = FLT_MAX;

pthread_t g_mainThread;

int BIsNested()
{
	return g_bIsNested == true;
}

static int initOutput(void);
static void steamCompMgrThreadRun(int argc, char **argv);

int main(int argc, char **argv)
{
	int o;
	bool bSleepAtStartup = false;
	while ((o = getopt(argc, argv, GAMESCOPE_OPTIONS)) != -1)
	{
		switch (o) {
			case 'w':
				g_nNestedWidth = atoi( optarg );
				break;
			case 'h':
				g_nNestedHeight = atoi( optarg );
				break;
			case 'W':
				g_nOutputWidth = atoi( optarg );
				break;
			case 'H':
				g_nOutputHeight = atoi( optarg );
				break;
			case 'r':
				g_nNestedRefresh = atoi( optarg );
				break;
			case 'o':
				g_nNestedUnfocusedRefresh = atoi( optarg );
				break;
			case 'm':
				g_flMaxWindowScale = atof( optarg );
				break;
			case 's':
				bSleepAtStartup = true;
				break;
			case 'L':
				g_bUseLayers = false;
				break;
			case 'd':
				g_bDebugLayers = true;
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
			default:
				break;
		}
	}

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

	cap_t caps;
	caps = cap_get_proc();
	cap_flag_value_t nicecapvalue = CAP_CLEAR;

	if ( caps != nullptr )
	{
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

	if ( bSleepAtStartup == true )
	{
	 	sleep( 2 );
	}

	XInitThreads();
	g_mainThread = pthread_self();

	if ( getenv("DISPLAY") != NULL || getenv("WAYLAND_DISPLAY") != NULL )
	{
		g_bIsNested = true;
	}

	if ( wlsession_init() != 0 )
	{
		fprintf( stderr, "Failed to initialize Wayland session\n" );
		return 1;
	}

	if ( initOutput() != 0 )
	{
		fprintf( stderr, "Failed to initialize output\n" );
		return 1;
	}

	if ( !vulkan_init() )
	{
		fprintf( stderr, "Failed to initialize Vulkan\n" );
		return 1;
	}

	// TODO: get the DRM device from the Vulkan device
	if ( g_bIsNested == false && g_vulkanHasDrmDevId )
	{
		struct stat drmStat = {};
		if ( fstat( g_DRM.fd, &drmStat ) != 0 )
		{
			perror( "fstat failed on DRM FD" );
			return 1;
		}

		if ( drmStat.st_rdev != g_vulkanDrmDevId )
		{
			fprintf( stderr, "Mismatch between DRM and Vulkan devices\n" );
			return 1;
		}
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

	if ( wlserver_init(argc, argv, g_bIsNested == true ) != 0 )
	{
		fprintf( stderr, "Failed to initialize wlserver\n" );
		return 1;
	}

	setenv("DISPLAY", wlserver_get_nested_display_name(), 1);
	setenv("GAMESCOPE_WAYLAND_DISPLAY", wlserver_get_wl_display_name(), 1);

	std::thread steamCompMgrThread( steamCompMgrThreadRun, argc, argv );
	steamCompMgrThread.detach();

	wlserver_run();
}

static void steamCompMgrThreadRun(int argc, char **argv)
{
	steamcompmgr_main( argc, argv );

	pthread_kill( g_mainThread, SIGINT );
}

static int initOutput(void)
{
	if ( g_bIsNested == true )
	{
		return sdlwindow_init() == false;
	}
	else
	{
		return init_drm( &g_DRM, nullptr );
	}
}

void wayland_commit(struct wlr_surface *surf, struct wlr_buffer *buf)
{
	{
		std::lock_guard<std::mutex> lock( wayland_commit_lock );
		
		ResListEntry_t newEntry = {
			.surf = surf,
			.buf = buf,
		};
		wayland_commit_queue.push_back( newEntry );
	}
	
	nudge_steamcompmgr();
}
