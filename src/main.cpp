#include <X11/Xlib.h>

#include <thread>
#include <mutex>
#include <vector>
#include <cstring>
#include <sys/capability.h>

#include <unistd.h>

#include "main.hpp"
#include "steamcompmgr.hpp"
#include "drm.hpp"
#include "rendervulkan.hpp"
#include "sdlwindow.hpp"
#include "wlserver.hpp"

int ac;
char **av;

int g_nNestedWidth = 1280;
int g_nNestedHeight = 720;
int g_nNestedRefresh = 60;

uint32_t g_nOutputWidth = 1280;
uint32_t g_nOutputHeight = 720;
int g_nOutputRefresh = 60;

bool g_bFullscreen = false;

bool g_bIsNested = false;

bool g_bFilterGameWindow = true;

bool g_bBorderlessOutputWindow = false;

bool g_bTakeScreenshot = false;

bool g_bNiceCap = false;

uint32_t g_nSubCommandArg = 0;

int BIsNested()
{
	return g_bIsNested == true;
}

int main(int argc, char **argv)
{
	// Grab the starting position of a potential command that follows "--" in argv
	// Do it before getopt can reorder anything, for use later
	for ( int i = 0; i < argc; i++ )
	{
		if ( strcmp( "--", argv[ i ] ) == 0 && i + 1 < argc )
		{
			g_nSubCommandArg = i + 1;
			break;
		}
	}

	int o;
	ac = argc;
	av = argv;
	
	bool bSleepAtStartup = false;
	
	while ((o = getopt (argc, argv, ":R:T:w:h:W:H:r:NFSvVecsdlnbf")) != -1)
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
			case 's':
				bSleepAtStartup = true;
				break;
			case 'l':
				g_bUseLayers = true;
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
			default:
				break;
		}
	}

	// If we're going to execute something monolithic, might as well set this since it'll extend to our clients
	if ( g_nSubCommandArg != 0 )
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

	cap_t caps;
	caps = cap_get_proc();
	cap_flag_value_t nicecapvalue = CAP_CLEAR;

	if ( caps != nullptr )
	{
		cap_get_flag( caps, CAP_SYS_NICE, CAP_EFFECTIVE, &nicecapvalue );

		if ( nicecapvalue == CAP_SET )
		{
			g_bNiceCap = true;
		}
	}

	if ( g_bNiceCap == false )
	{
		fprintf( stderr, "No CAP_SYS_NICE, falling back to regular-priority compute and threads.\nPerformance will be affected.\n" );
	}
	
	if ( bSleepAtStartup == true )
	{
	 	sleep( 2 );
	}
	
	XInitThreads();
	
	if ( getenv("DISPLAY") != NULL )
	{
		g_bIsNested = true;
	}

	wlserver_init(argc, argv, g_bIsNested == true );
	
	if ( initOutput() != 0 )
	{
		fprintf( stderr, "Failed to initialize output\n" );
		return 1;
	}
	
	wlserver_run();
}

void steamCompMgrThreadRun(void)
{
	steamcompmgr_main( ac, av );
}

void startSteamCompMgr(void)
{
	std::thread steamCompMgrThread( steamCompMgrThreadRun );
	steamCompMgrThread.detach();
	
	return;
}

int initOutput(void)
{
	if ( g_bIsNested == true )
	{
		return sdlwindow_init() == false;
	}
	else
	{
		return init_drm( &g_DRM, nullptr, nullptr, 0 );
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
