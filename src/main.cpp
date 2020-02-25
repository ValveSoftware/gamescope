#include <X11/Xlib.h>

#include <thread>
#include <mutex>
#include <vector>

#include <unistd.h>

#include "wlserver.h"

#include "main.hpp"
#include "steamcompmgr.hpp"
#include "drm.hpp"
#include "rendervulkan.hpp"
#include "inputsdl.hpp"

int ac;
char **av;

SDL_Window *window;

int g_nNestedWidth = 1280;
int g_nNestedHeight = 720;
int g_nNestedRefresh = 60;

uint32_t g_nOutputWidth = 1280;
uint32_t g_nOutputHeight = 720;
int g_nOutputRefresh = 60;

bool g_bIsNested = false;

bool g_bFilterGameWindow = true;

bool g_bBorderlessOutputWindow = false;

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
	
	while ((o = getopt (argc, argv, ":R:T:w:h:W:H:r:NSvVecsdlnb")) != -1)
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
			default:
				break;
		}
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
	
	initOutput();
	
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

void initOutput(void)
{
	if ( g_bIsNested == true )
	{
		inputsdl_init();
		
		uint32_t nSDLWindowFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
		
		if ( g_bBorderlessOutputWindow == true )
		{
			nSDLWindowFlags |= SDL_WINDOW_BORDERLESS;
		}

		window = SDL_CreateWindow( "gamescope",
								   SDL_WINDOWPOS_UNDEFINED,
								   SDL_WINDOWPOS_UNDEFINED,
								   g_nOutputWidth,
								   g_nOutputHeight,
								   nSDLWindowFlags );
		
		
		unsigned int extCount;
		SDL_Vulkan_GetInstanceExtensions( window, &extCount, nullptr );
		
		g_vecSDLInstanceExts.resize( extCount );
		
		SDL_Vulkan_GetInstanceExtensions( window, &extCount, g_vecSDLInstanceExts.data() );
	}
	else
	{
		init_drm( &g_DRM, nullptr, nullptr, 0 );
	}
}

void wayland_commit(struct wlr_surface *surf, struct wlr_dmabuf_attributes *attribs)
{
	{
		std::lock_guard<std::mutex> lock( wayland_commit_lock );
		
		ResListEntry_t newEntry = {
			.surf = surf,
			.attribs = *attribs
		};
		wayland_commit_queue.push_back( newEntry );
	}
	
	nudge_steamcompmgr();
}
