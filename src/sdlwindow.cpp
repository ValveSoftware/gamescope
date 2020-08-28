// For the nested case, reads input from the SDL window and send to wayland

#include <thread>
#include <mutex>

#include <signal.h>
#include <linux/input-event-codes.h>

#include "main.hpp"
#include "wlserver.hpp"
#include "sdlwindow.hpp"
#include "rendervulkan.hpp"

#include "sdlscancodetable.hpp"

bool g_bSDLInitOK = false;
std::mutex g_SDLInitLock;

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
	// see wlroots xwayland startup and how wl_event_loop_add_signal works
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR1);
	sigprocmask(SIG_BLOCK, &mask, NULL);

	SDL_Event event;
	SDL_Keymod mod;
	uint32_t key;
	
	SDL_Init( SDL_INIT_VIDEO | SDL_INIT_EVENTS );

	g_unSDLUserEventID = SDL_RegisterEvents( 1 );

	g_SDLUserEvent.type = g_unSDLUserEventID;
	g_SDLUserEvent.user.code = 32;

	uint32_t nSDLWindowFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;

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
		g_SDLInitLock.unlock();
		return;
	}

	unsigned int extCount = 0;
	SDL_Vulkan_GetInstanceExtensions( g_SDLWindow, &extCount, nullptr );

	g_vecSDLInstanceExts.resize( extCount );

	SDL_Vulkan_GetInstanceExtensions( g_SDLWindow, &extCount, g_vecSDLInstanceExts.data() );

	SDL_SetRelativeMouseMode(SDL_TRUE);
	
	g_bSDLInitOK = true;
	g_SDLInitLock.unlock();
	
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
				}
				break;
			default:
				break;
		}
	}
}

bool sdlwindow_init( void )
{
	g_SDLInitLock.lock();

	std::thread inputSDLThread( inputSDLThreadRun );
	inputSDLThread.detach();
	
	// When this returns SDL_Init should be over
	g_SDLInitLock.lock();

	return g_bSDLInitOK;
}

void sdlwindow_update( void )
{
	SDL_PushEvent( &g_SDLUserEvent );
}
