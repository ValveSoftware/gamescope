// For the nested case, reads input from the SDL window and send to wayland

#include <thread>
#include <mutex>

#include <linux/input-event-codes.h>

#include "main.hpp"
#include "wlserver.hpp"
#include "sdlwindow.hpp"
#include "rendervulkan.hpp"
#include "steamcompmgr.hpp"

#include "sdlscancodetable.hpp"

#include <condition_variable>

int g_iSDLInitOK = 0;
std::mutex g_SDLInitLock;
std::condition_variable g_SDLCondVar;

bool g_bWindowShown = false;

int g_nOldNestedRefresh = 0;
bool g_bWindowFocused = true;

SDL_Window *g_SDLWindow;
uint32_t g_unSDLUserEventID;
SDL_Event g_SDLUserEvent;

//-----------------------------------------------------------------------------
// Purpose: Convert from the remote scancode to a Linux event keycode
//-----------------------------------------------------------------------------
static inline uint32_t SDLScancodeToLinuxKey( uint32_t nScancode )
{
	if ( nScancode < sizeof( s_ScancodeTable ) / sizeof( s_ScancodeTable[0] ) )
	{
		return s_ScancodeTable[ nScancode ];
	}
	return KEY_RESERVED;
}

static inline int SDLButtonToLinuxButton( int SDLButton )
{
	switch ( SDLButton )
	{
		case SDL_BUTTON_LEFT: return BTN_LEFT;
		case SDL_BUTTON_MIDDLE: return BTN_MIDDLE;
		case SDL_BUTTON_RIGHT: return BTN_RIGHT;
		case SDL_BUTTON_X1: return BTN_FORWARD;
		case SDL_BUTTON_X2: return BTN_BACK;
		default: return 0;
	}
}

void updateOutputRefresh( void )
{
	int display_index = 0;
	SDL_DisplayMode mode = { SDL_PIXELFORMAT_UNKNOWN, 0, 0, 0, 0 };
	
	display_index = SDL_GetWindowDisplayIndex( g_SDLWindow );
	if ( SDL_GetDesktopDisplayMode( display_index, &mode ) == 0 )
	{
		g_nOutputRefresh = mode.refresh_rate;
	}
}

void inputSDLThreadRun( void )
{
	g_SDLInitLock.lock();
	SDL_Event event;
	SDL_Keymod mod;
	uint32_t key;

	if ( SDL_Init( SDL_INIT_VIDEO | SDL_INIT_EVENTS ) != 0 )
	{
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		g_iSDLInitOK = -1;
		g_SDLInitLock.unlock();
		g_SDLCondVar.notify_one();
		return;
	}

	g_unSDLUserEventID = SDL_RegisterEvents( 1 );

	g_SDLUserEvent.type = g_unSDLUserEventID;
	g_SDLUserEvent.user.code = 32;

	uint32_t nSDLWindowFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN;

	if ( g_bBorderlessOutputWindow == true )
	{
		nSDLWindowFlags |= SDL_WINDOW_BORDERLESS;
	}

	if ( g_bFullscreen == true )
	{
		nSDLWindowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
	}

	g_SDLWindow = SDL_CreateWindow( "gamescope",
							   SDL_WINDOWPOS_UNDEFINED,
							SDL_WINDOWPOS_UNDEFINED,
							g_nOutputWidth,
							g_nOutputHeight,
							nSDLWindowFlags );

	if ( g_SDLWindow == nullptr )
	{
		fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
		g_iSDLInitOK = -1;
		g_SDLInitLock.unlock();
		g_SDLCondVar.notify_one();
		return;
	}

	unsigned int extCount = 0;
	SDL_Vulkan_GetInstanceExtensions( g_SDLWindow, &extCount, nullptr );

	g_vecSDLInstanceExts.resize( extCount );

	SDL_Vulkan_GetInstanceExtensions( g_SDLWindow, &extCount, g_vecSDLInstanceExts.data() );

	SDL_SetRelativeMouseMode(SDL_TRUE);

	g_nOldNestedRefresh = g_nNestedRefresh;
	
	g_iSDLInitOK = 1;
	g_SDLInitLock.unlock();
	g_SDLCondVar.notify_one();
	
	while( SDL_WaitEvent( &event ) )
	{
		switch( event.type )
		{
			case SDL_MOUSEMOTION:
				wlserver_lock();
				wlserver_mousemotion( event.motion.xrel, event.motion.yrel, event.motion.timestamp );
				wlserver_unlock();
				break;
			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP:
				wlserver_lock();
				wlserver_mousebutton( SDLButtonToLinuxButton( event.button.button ),
									  event.button.state == SDL_PRESSED,
									  event.button.timestamp );
				wlserver_unlock();
				break;
			case SDL_MOUSEWHEEL:
				wlserver_lock();
				wlserver_mousewheel( -event.wheel.x, -event.wheel.y, event.wheel.timestamp );
				wlserver_unlock();
				break;
			case SDL_KEYDOWN:
			case SDL_KEYUP:
				mod = SDL_GetModState();
				key = SDLScancodeToLinuxKey( event.key.keysym.scancode );
				
				if ( event.type == SDL_KEYUP && mod & KMOD_LGUI )
				{
					switch ( key )
					{
						case KEY_F:
							g_bFullscreen = !g_bFullscreen;
							SDL_SetWindowFullscreen( g_SDLWindow, g_bFullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0 );
							break;
						case KEY_N:
							g_bFilterGameWindow = !g_bFilterGameWindow;
							break;
						case KEY_S:
							g_bTakeScreenshot = true;
							break;
						default:
							goto client;
						
					}
					break;
				}
client:
				wlserver_lock();
				wlserver_key( key, event.type == SDL_KEYDOWN, event.key.timestamp );
				wlserver_unlock();
				break;
			case SDL_WINDOWEVENT:
				switch( event.window.event )
				{
					default:
						break;
					case SDL_WINDOWEVENT_MOVED:
					case SDL_WINDOWEVENT_SHOWN:
						updateOutputRefresh();
						break;
					case SDL_WINDOWEVENT_SIZE_CHANGED:
						g_nOutputWidth = event.window.data1;
						g_nOutputHeight = event.window.data2;
						
						updateOutputRefresh();
						
						break;
					case SDL_WINDOWEVENT_FOCUS_LOST:
						g_nNestedRefresh = g_nNestedUnfocusedRefresh;
						g_bWindowFocused = false;
						break;
					case SDL_WINDOWEVENT_FOCUS_GAINED:
						g_nNestedRefresh = g_nOldNestedRefresh;
						g_bWindowFocused = true;
						break;
				}
				break;
			default:
				if ( event.type == g_unSDLUserEventID )
				{
					sdlwindow_update();
				}
				break;
		}
	}
}

bool sdlwindow_init( void )
{
	std::unique_lock lock(g_SDLInitLock);

	std::thread inputSDLThread( inputSDLThreadRun );
	inputSDLThread.detach();

	g_SDLCondVar.wait(lock, []{ return g_iSDLInitOK != 0; });
	
	return g_iSDLInitOK == 1;
}

void sdlwindow_update( void )
{
	if ( g_bWindowShown != hasFocusWindow )
	{
		g_bWindowShown = hasFocusWindow;

		if ( g_bWindowShown )
		{
			SDL_ShowWindow( g_SDLWindow );
		}
		else
		{
			SDL_HideWindow( g_SDLWindow );
		}
	}
}

void sdlwindow_pushupdate( void )
{
	SDL_PushEvent( &g_SDLUserEvent );
}
