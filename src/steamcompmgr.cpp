/*
 * Based on xcompmgr by Keith Packard et al.
 * http://cgit.freedesktop.org/xorg/app/xcompmgr/
 * Original xcompmgr legal notices follow:
 *
 * Copyright © 2003 Keith Packard
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of Keith Packard not be used in
 * advertising or publicity pertaining to distribution of the software without
 * specific, written prior permission.  Keith Packard makes no
 * representations about the suitability of this software for any purpose.  It
 * is provided "as is" without express or implied warranty.
 *
 * KEITH PACKARD DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL KEITH PACKARD BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */


/* Modified by Matthew Hawn. I don't know what to say here so follow what it
 *   says above. Not that I can really do anything about it
 */

#include <thread>
#include <condition_variable>
#include <mutex>
#include <atomic>
#include <vector>
#include <algorithm>
#include <array>
#include <iostream>
#include <fstream>
#include <string>

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <spawn.h>
#include <signal.h>
#include <linux/input-event-codes.h>


#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/XRes.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/xf86vmode.h>

#include "main.hpp"
#include "wlserver.hpp"
#include "drm.hpp"
#include "rendervulkan.hpp"
#include "steamcompmgr.hpp"
#include "vblankmanager.hpp"
#include "sdlwindow.hpp"
#include "log.hpp"

#if HAVE_PIPEWIRE
#include "pipewire.hpp"
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

#define GPUVIS_TRACE_IMPLEMENTATION
#include "gpuvis_trace_utils.h"

typedef struct _ignore {
	struct _ignore	*next;
	unsigned long	sequence;
} ignore;

uint64_t maxCommmitID;

struct commit_t
{
	struct wlr_buffer *buf;
	uint32_t fb_id;
	VulkanTexture_t vulkanTex;
	uint64_t commitID;
	bool done;
};

std::mutex listCommitsDoneLock;
std::vector< uint64_t > listCommitsDone;

struct win {
	struct win		*next;
	Window		id;
	XWindowAttributes	a;
	int			mode;
	Damage		damage;
	unsigned int	opacity;
	unsigned long	map_sequence;
	unsigned long	damage_sequence;

	char *title;
	bool utf8_title;
	pid_t pid;

	bool isSteam;
	bool isSteamStreamingClient;
	bool isSteamStreamingClientVideo;
	uint32_t inputFocusMode;
	uint32_t appID;
	bool isOverlay;
	bool isFullscreen;
	bool isSysTrayIcon;
	bool sizeHintsSpecified;
	bool skipTaskbar;
	bool skipPager;
	unsigned int requestedWidth;
	unsigned int requestedHeight;

	Window transientFor;

	bool nudged;
	bool ignoreOverrideRedirect;

	unsigned int mouseMoved;

	struct wlserver_surface surface;

	std::vector< commit_t > commit_queue;
};

static win		*list;
static int		scr;
static Window		root;
static XserverRegion	allDamage;
static bool		clipChanged;
static int		root_height, root_width;
static ignore		*ignore_head, **ignore_tail = &ignore_head;
static int		xfixes_event, xfixes_error;
static int		damage_event, damage_error;
static int		composite_event, composite_error;
static int		render_event, render_error;
static int		xshape_event, xshape_error;
static bool		synchronize;
static int		composite_opcode;

uint32_t		currentOutputWidth, currentOutputHeight;

static Window	currentFocusWindow;
static win*		currentFocusWin;
static Window	currentInputFocusWindow;
uint32_t		currentInputFocusMode;
static Window 	currentKeyboardFocusWindow;
static Window	currentOverlayWindow;
static Window	currentNotificationWindow;
static Window	currentOverrideWindow;
static Window   currentFadeWindow;

bool hasFocusWindow;

std::vector< uint32_t > vecFocuscontrolAppIDs;
static Window focusControlWindow;

static Window	ourWindow;

bool			gameFocused;

unsigned int 	gamesRunningCount;

float			overscanScaleRatio = 1.0;
float			zoomScaleRatio = 1.0;
float			globalScaleRatio = 1.0f;

float			focusedWindowScaleX = 1.0f;
float			focusedWindowScaleY = 1.0f;
float			focusedWindowOffsetX = 0.0f;
float			focusedWindowOffsetY = 0.0f;

uint32_t		inputCounter;
uint32_t		lastPublishedInputCounter;

bool			focusDirty = false;
bool			hasRepaint = false;

unsigned long	damageSequence = 0;

unsigned int	cursorHideTime = 10'000;

bool			gotXError = false;

unsigned int	fadeOutStartTime = 0;

unsigned int 	g_FadeOutDuration = 0;

extern float g_flMaxWindowScale;
extern bool g_bIntegerScale;

/* find these once and be done with it */
static Atom		steamAtom;
static Atom		gameAtom;
static Atom		overlayAtom;
static Atom		gamesRunningAtom;
static Atom		screenZoomAtom;
static Atom		screenScaleAtom;
static Atom		opacityAtom;
static Atom		winTypeAtom;
static Atom		winDesktopAtom;
static Atom		winDockAtom;
static Atom		winToolbarAtom;
static Atom		winMenuAtom;
static Atom		winUtilAtom;
static Atom		winSplashAtom;
static Atom		winDialogAtom;
static Atom		winNormalAtom;
static Atom		sizeHintsAtom;
static Atom		netWMStateFullscreenAtom;
static Atom		activeWindowAtom;
static Atom		netWMStateAtom;
static Atom		WMTransientForAtom;
static Atom		netWMStateHiddenAtom;
static Atom		netWMStateFocusedAtom;
static Atom		netWMStateSkipTaskbarAtom;
static Atom		netWMStateSkipPagerAtom;
static Atom		WLSurfaceIDAtom;
static Atom		WMStateAtom;
static Atom		steamInputFocusAtom;
static Atom		WMChangeStateAtom;
static Atom		steamTouchClickModeAtom;
static Atom		utf8StringAtom;
static Atom		netWMNameAtom;
static Atom		netSystemTrayOpcodeAtom;
static Atom		steamStreamingClientAtom;
static Atom		steamStreamingClientVideoAtom;
static Atom		gamescopeFocusableAppsAtom;
static Atom		gamescopeFocusableWindowsAtom;
static Atom		gamescopeFocusedWindowAtom;
static Atom		gamescopeFocusedAppAtom;
static Atom		gamescopeCtrlAppIDAtom;
static Atom		gamescopeCtrlWindowAtom;
static Atom		gamescopeInputCounterAtom;
static Atom		gamescopeScreenShotAtom;

enum HeldCommitTypes_t
{
	HELD_COMMIT_BASE,
	HELD_COMMIT_FADE,

	HELD_COMMIT_COUNT,
};

std::array<commit_t, HELD_COMMIT_COUNT> g_HeldCommits;
bool g_bPendingFade = false;

/* opacity property name; sometime soon I'll write up an EWMH spec for it */
#define OPACITY_PROP		"_NET_WM_WINDOW_OPACITY"
#define GAME_PROP			"STEAM_GAME"
#define STEAM_PROP			"STEAM_BIGPICTURE"
#define OVERLAY_PROP		"STEAM_OVERLAY"
#define GAMES_RUNNING_PROP 	"STEAM_GAMES_RUNNING"
#define SCREEN_SCALE_PROP	"STEAM_SCREEN_SCALE"
#define SCREEN_MAGNIFICATION_PROP	"STEAM_SCREEN_MAGNIFICATION"

#define TRANSLUCENT	0x00000000
#define OPAQUE		0xffffffff

#define ICCCM_WITHDRAWN_STATE 0
#define ICCCM_NORMAL_STATE 1
#define ICCCM_ICONIC_STATE 3

#define NET_WM_STATE_REMOVE 0
#define NET_WM_STATE_ADD 1
#define NET_WM_STATE_TOGGLE 2

#define SYSTEM_TRAY_REQUEST_DOCK 0
#define SYSTEM_TRAY_BEGIN_MESSAGE 1
#define SYSTEM_TRAY_CANCEL_MESSAGE 2

#define			FRAME_RATE_SAMPLING_PERIOD 160

unsigned int	frameCounter;
unsigned int	lastSampledFrameTime;
float			currentFrameRate;

static bool		debugFocus = false;
static bool		drawDebugInfo = false;
static bool		debugEvents = false;
bool			steamMode = false;
static bool		alwaysComposite = false;
static bool		useXRes = true;

std::mutex wayland_commit_lock;
std::vector<ResListEntry_t> wayland_commit_queue;

struct wlr_buffer_map_entry {
	struct wl_listener listener;
	struct wlr_buffer *buf;
	VulkanTexture_t vulkanTex;
	uint32_t fb_id;
};

static std::mutex wlr_buffer_map_lock;
static std::unordered_map<struct wlr_buffer*, wlr_buffer_map_entry> wlr_buffer_map;

static std::atomic< bool > g_bTakeScreenshot{false};
static bool g_bPropertyRequestedScreenshot;

static int g_nudgePipe[2] = {-1, -1};

static LogScope xwm_log("xwm");

// Right now there's a suuuper duper rare bug where if we change the zposes
// from these values the overlay can render behind the base layer
// or the override can render above the overlay which is very very wrong.
// So for now, if we have both an override, just force composition
// and keep these zpos values the same.
#define WORKAROUND_ZPOS_BUG

#ifdef WORKAROUND_ZPOS_BUG
static const uint32_t g_zposBase = 0;
static const uint32_t g_zposOverride = 0;
static const uint32_t g_zposOverlay = 1;
static const uint32_t g_zposCursor = 2;
#else
static const uint32_t g_zposBase = 0;
static const uint32_t g_zposOverride = 1;
static const uint32_t g_zposOverlay = 2;
static const uint32_t g_zposCursor = 3;
#endif

// poor man's semaphore
class sem
{
public:
	void wait( void )
	{
		std::unique_lock<std::mutex> lock(mtx);

		while(count == 0){
			cv.wait(lock);
		}
		count--;
	}

	void signal( void )
	{
		std::unique_lock<std::mutex> lock(mtx);
		count++;
		cv.notify_one();
	}

private:
	std::mutex mtx;
	std::condition_variable cv;
	int count = 0;
};

sem waitListSem;
std::mutex waitListLock;
std::vector< std::pair< int, uint64_t > > waitList;

bool imageWaitThreadRun = true;

void imageWaitThreadMain( void )
{
	pthread_setname_np( pthread_self(), "gamescope-img" );

wait:
	waitListSem.wait();

	if ( imageWaitThreadRun == false )
	{
		return;
	}

	bool bFound = false;
	int fence;
	uint64_t commitID;

retry:
	{
		std::unique_lock< std::mutex > lock( waitListLock );

		if( waitList.size() == 0 )
		{
			goto wait;
		}

		fence = waitList[ 0 ].first;
		commitID = waitList[ 0 ].second;
		bFound = true;
		waitList.erase( waitList.begin() );
	}

	assert( bFound == true );

	gpuvis_trace_begin_ctx_printf( commitID, "wait fence" );
	struct pollfd fd = { fence, POLLOUT, 0 };
	int ret = poll( &fd, 1, 100 );
	if ( ret < 0 )
	{
		xwm_log.errorf_errno( "failed to poll fence FD" );
	}
	gpuvis_trace_end_ctx_printf( commitID, "wait fence" );

	close( fence );

	{
		std::unique_lock< std::mutex > lock( listCommitsDoneLock );

		listCommitsDone.push_back( commitID );
	}

	nudge_steamcompmgr();

	goto retry;
}

sem statsThreadSem;
std::mutex statsEventQueueLock;
std::vector< std::string > statsEventQueue;

std::string statsThreadPath;
int			statsPipeFD = -1;

bool statsThreadRun;

void statsThreadMain( void )
{
	pthread_setname_np( pthread_self(), "gamescope-stats" );
	signal(SIGPIPE, SIG_IGN);

	while ( statsPipeFD == -1 )
	{
		statsPipeFD = open( statsThreadPath.c_str(), O_WRONLY | O_CLOEXEC );

		if ( statsPipeFD == -1 )
		{
			sleep( 10 );
		}
	}

wait:
	statsThreadSem.wait();

	if ( statsThreadRun == false )
	{
		return;
	}

	std::string event;

retry:
	{
		std::unique_lock< std::mutex > lock( statsEventQueueLock );

		if( statsEventQueue.size() == 0 )
		{
			goto wait;
		}

		event = statsEventQueue[ 0 ];
		statsEventQueue.erase( statsEventQueue.begin() );
	}

	dprintf( statsPipeFD, "%s", event.c_str() );

	goto retry;
}

static inline void stats_printf( const char* format, ...)
{
	static char buffer[256];
	static std::string eventstr;

	va_list args;
	va_start(args, format);
	vsprintf(buffer,format, args);
	va_end(args);

	eventstr = buffer;

	{
		{
			std::unique_lock< std::mutex > lock( statsEventQueueLock );

			if( statsEventQueue.size() > 50 )
			{
				// overflow, drop event
				return;
			}

			statsEventQueue.push_back( eventstr );

			statsThreadSem.signal();
		}
	}
}

uint64_t get_time_in_nanos()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return ts.tv_sec * 1'000'000'000ul + ts.tv_nsec;
}

void sleep_for_nanos(uint64_t nanos)
{
	timespec ts;
	ts.tv_sec = time_t(nanos / 1'000'000'000ul);
	ts.tv_nsec = long(nanos % 1'000'000'000ul);
	nanosleep(&ts, nullptr);
}

void sleep_until_nanos(uint64_t nanos)
{
	uint64_t now = get_time_in_nanos();
	if (now >= nanos)
		return;
	sleep_for_nanos(nanos - now);
}

unsigned int
get_time_in_milliseconds(void)
{
	return (unsigned int)(get_time_in_nanos() / 1'000'000ul);
}

static void
discard_ignore(Display *dpy, unsigned long sequence)
{
	while (ignore_head)
	{
		if ((long) (sequence - ignore_head->sequence) > 0)
		{
			ignore  *next = ignore_head->next;
			free(ignore_head);
			ignore_head = next;
			if (!ignore_head)
				ignore_tail = &ignore_head;
		}
		else
			break;
	}
}

static void
set_ignore(Display *dpy, unsigned long sequence)
{
	ignore  *i = (ignore *)malloc(sizeof(ignore));
	if (!i)
		return;
	i->sequence = sequence;
	i->next = NULL;
	*ignore_tail = i;
	ignore_tail = &i->next;
}

static int
should_ignore(Display *dpy, unsigned long sequence)
{
	discard_ignore(dpy, sequence);
	return ignore_head && ignore_head->sequence == sequence;
}

static win *
find_win(Display *dpy, Window id)
{
	win	*w;

	if (id == None)
	{
		return NULL;
	}

	for (w = list; w; w = w->next)
	{
		if (w->id == id)
		{
			return w;
		}
	}

	if ( dpy == nullptr )
		return nullptr;

	// Didn't find, must be a children somewhere; try again with parent.
	Window root = None;
	Window parent = None;
	Window *children = NULL;
	unsigned int childrenCount;
	set_ignore(dpy, NextRequest(dpy));
	XQueryTree(dpy, id, &root, &parent, &children, &childrenCount);
	if (children)
		XFree(children);

	if (root == parent || parent == None)
	{
		return NULL;
	}

	return find_win(dpy, parent);
}

static win * find_win( struct wlr_surface *surf )
{
	win	*w = nullptr;

	for (w = list; w; w = w->next)
	{
		if ( w->surface.wlr == surf )
			return w;
	}

	return nullptr;
}

#ifdef COMMIT_REF_DEBUG
static int buffer_refs = 0;
#endif

static void
destroy_buffer( struct wl_listener *listener, void * )
{
	std::lock_guard<std::mutex> lock( wlr_buffer_map_lock );
	wlr_buffer_map_entry *entry = wl_container_of( listener, entry, listener );

	if ( entry->vulkanTex != 0 )
	{
		vulkan_free_texture( entry->vulkanTex );
	}

	if ( entry->fb_id != 0 )
	{
		drm_drop_fbid( &g_DRM, entry->fb_id );
	}

	wl_list_remove( &entry->listener.link );

#ifdef COMMIT_REF_DEBUG
	fprintf(stderr, "destroy_buffer - refs: %d\n", --buffer_refs);
#endif

	/* Has to be the last thing we do as this deletes *entry. */
	wlr_buffer_map.erase( wlr_buffer_map.find( entry->buf ) );
}

static void
ref_commit( commit_t &commit )
{
	vulkan_ref_commit( commit.vulkanTex );
}

static void
release_commit( commit_t &commit )
{
	if ( !vulkan_free_commit( commit.vulkanTex ) )
	{
		if ( commit.fb_id != 0 )
		{
			drm_unlock_fbid( &g_DRM, commit.fb_id );
			commit.fb_id = 0;
		}

		wlserver_lock();
		wlr_buffer_unlock( commit.buf );
		wlserver_unlock();
	}
}

static bool
import_commit ( struct wlr_buffer *buf, commit_t &commit )
{
	std::unique_lock<std::mutex> lock( wlr_buffer_map_lock );

	commit.buf = buf;

	auto it = wlr_buffer_map.find( buf );
	if ( it != wlr_buffer_map.end() )
	{
		commit.vulkanTex = it->second.vulkanTex;
		commit.fb_id = it->second.fb_id;

		/* Unlock here to avoid deadlock [1],
		 * drm_lock_fbid calls wlserver_lock.
		 * Map is no longer used here and the element 
		 * is no longer accessed. */
		lock.unlock();

		if (commit.fb_id)
		{
			drm_lock_fbid( &g_DRM, commit.fb_id );
		}

		return true;
	}

#ifdef COMMIT_REF_DEBUG
	fprintf(stderr, "import_commit - refs %d\n", ++buffer_refs);
#endif

	wlr_buffer_map_entry& entry = wlr_buffer_map[buf];
	/* [1]
	 * Need to unlock the wlr_buffer_map_lock to avoid
	 * a deadlock on destroy_buffer if it owns the wlserver_lock.
	 * This is safe for a few reasons:
	 * - 1: All accesses to wlr_buffer_map are done before this lock.
	 * - 2: destroy_buffer cannot be called from this buffer before now
	 *      as it only happens because of the signal added below.
	 * - 3: "References to elements in the unordered_map container remain
	 *		 valid in all cases, even after a rehash." */
	lock.unlock();

	commit.vulkanTex = vulkan_create_texture_from_wlr_buffer( buf );
	assert( commit.vulkanTex != 0 );

	struct wlr_dmabuf_attributes dmabuf = {0};
	if ( BIsNested() == false && wlr_buffer_get_dmabuf( buf, &dmabuf ) )
	{
		commit.fb_id = drm_fbid_from_dmabuf( &g_DRM, buf, &dmabuf );

		if ( commit.fb_id )
		{
			drm_lock_fbid( &g_DRM, commit.fb_id );
		}
	}
	else
	{
		commit.fb_id = 0;
	}

	entry.listener.notify = destroy_buffer;
	entry.buf = buf;
	entry.vulkanTex = commit.vulkanTex;
	entry.fb_id = commit.fb_id;

	wlserver_lock();
	wl_signal_add( &buf->events.destroy, &entry.listener );
	wlserver_unlock();

	return true;
}

static bool
get_window_last_done_commit( win *w, commit_t &commit )
{
	int32_t lastCommit = -1;
	for ( uint32_t i = 0; i < w->commit_queue.size(); i++ )
	{
		if ( w->commit_queue[ i ].done == true )
		{
			lastCommit = i;
		}
	}

	if ( lastCommit == -1 )
	{
		return false;
	}

	commit = w->commit_queue[ lastCommit ];
	return true;
}

/**
 * Constructor for a cursor. It is hidden in the beginning (normally until moved by user).
 */
MouseCursor::MouseCursor(_XDisplay *display)
	: m_texture(0)
	, m_dirty(true)
	, m_imageEmpty(false)
	, m_hideForMovement(true)
	, m_display(display)
{
}

void MouseCursor::queryPositions(int &rootX, int &rootY, int &winX, int &winY)
{
	Window window, child;
	unsigned int mask;

	XQueryPointer(m_display, DefaultRootWindow(m_display), &window, &child,
				  &rootX, &rootY, &winX, &winY, &mask);

}

void MouseCursor::queryGlobalPosition(int &x, int &y)
{
	int winX, winY;
	queryPositions(x, y, winX, winY);
}

void MouseCursor::queryButtonMask(unsigned int &mask)
{
	Window window, child;
	int rootX, rootY, winX, winY;

	XQueryPointer(m_display, DefaultRootWindow(m_display), &window, &child,
				  &rootX, &rootY, &winX, &winY, &mask);
}

void MouseCursor::checkSuspension()
{
	unsigned int buttonMask;
	queryButtonMask(buttonMask);

	if (buttonMask & ( Button1Mask | Button2Mask | Button3Mask | Button4Mask | Button5Mask )) {
		m_hideForMovement = false;
		m_lastMovedTime = get_time_in_milliseconds();
	}

	const bool suspended = get_time_in_milliseconds() - m_lastMovedTime > cursorHideTime;
	if (!m_hideForMovement && suspended) {
		m_hideForMovement = true;

		win *window = find_win(m_display, currentInputFocusWindow);

		// Rearm warp count
		if (window) {
			window->mouseMoved = 0;
		}

		// We're hiding the cursor, force redraw if we were showing it
		if (window && !m_imageEmpty ) {
			hasRepaint = true;
			nudge_steamcompmgr();
		}
	}
}

void MouseCursor::warp(int x, int y)
{
	XWarpPointer(m_display, None, currentInputFocusWindow, 0, 0, 0, 0, x, y);
}

void MouseCursor::resetPosition()
{
	warp(m_x, m_y);
}

void MouseCursor::setDirty()
{
	// We can't prove it's empty until checking again
	m_imageEmpty = false;
	m_dirty = true;
}

bool MouseCursor::setCursorImage(char *data, int w, int h, int hx, int hy)
{
	XRenderPictFormat *pictformat;
	Picture picture;
	XImage* ximage;
	Pixmap pixmap;
	Cursor cursor;
	GC gc;

	if (!(ximage = XCreateImage(
		m_display,
		DefaultVisual(m_display, DefaultScreen(m_display)),
		32, ZPixmap,
		0,
		data,
		w, h,
		32, 0)))
	{
		xwm_log.errorf("Failed to make ximage for cursor");
		goto error_image;
	}

	if (!(pixmap = XCreatePixmap(m_display, DefaultRootWindow(m_display), w, h, 32)))
	{
		xwm_log.errorf("Failed to make pixmap for cursor");
		goto error_pixmap;
	}

	if (!(gc = XCreateGC(m_display, pixmap, 0, NULL)))
	{
		xwm_log.errorf("Failed to make gc for cursor");
		goto error_gc;
	}

	XPutImage(m_display, pixmap, gc, ximage, 0, 0, 0, 0, w, h);

	if (!(pictformat = XRenderFindStandardFormat(m_display, PictStandardARGB32)))
	{
		xwm_log.errorf("Failed to create pictformat for cursor");
		goto error_pictformat;
	}

	if (!(picture = XRenderCreatePicture(m_display, pixmap, pictformat, 0, NULL)))
	{
		xwm_log.errorf("Failed to create picture for cursor");
		goto error_picture;
	}

	if (!(cursor = XRenderCreateCursor(m_display, picture, hx, hy)))
	{
		xwm_log.errorf("Failed to create cursor");
		goto error_cursor;
	}

	XDefineCursor(m_display, DefaultRootWindow(m_display), cursor);
	XFlush(m_display);
	setDirty();
	return true;

error_cursor:
	XRenderFreePicture(m_display, picture);
error_picture:
error_pictformat:
	XFreeGC(m_display, gc);
error_gc:
	XFreePixmap(m_display, pixmap);
error_pixmap:
	// XDestroyImage frees the data.
	XDestroyImage(ximage);
error_image:
	return false;
}

void MouseCursor::constrainPosition()
{
	int i;
	win *window = find_win(m_display, currentInputFocusWindow);

	// If we had barriers before, get rid of them.
	for (i = 0; i < 4; i++) {
		if (m_scaledFocusBarriers[i] != None) {
			XFixesDestroyPointerBarrier(m_display, m_scaledFocusBarriers[i]);
			m_scaledFocusBarriers[i] = None;
		}
	}

	auto barricade = [this](int x1, int y1, int x2, int y2) {
		return XFixesCreatePointerBarrier(m_display, DefaultRootWindow(m_display),
										  x1, y1, x2, y2, 0, 0, NULL);
	};

	// Constrain it to the window; careful, the corners will leak due to a known X server bug.
	m_scaledFocusBarriers[0] = barricade(0, window->a.y, root_width, window->a.y);

	m_scaledFocusBarriers[1] = barricade(window->a.x + window->a.width, 0,
										 window->a.x + window->a.width, root_height);
	m_scaledFocusBarriers[2] = barricade(root_width, window->a.y + window->a.height,
										 0, window->a.y + window->a.height);
	m_scaledFocusBarriers[3] = barricade(window->a.x, root_height, window->a.x, 0);

	// Make sure the cursor is somewhere in our jail
	int rootX, rootY;
	queryGlobalPosition(rootX, rootY);

	if (rootX - window->a.x >= window->a.width || rootY - window->a.y >= window->a.height ||
		rootX - window->a.x < 0 || rootY - window->a.y < 0 ) {
		warp(window->a.width / 2, window->a.height / 2);
	}
}

void MouseCursor::move(int x, int y)
{
	// Some stuff likes to warp in-place
	if (m_x == x && m_y == y) {
		return;
	}
	m_x = x;
	m_y = y;

	win *window = find_win(m_display, currentInputFocusWindow);

	if (window) {
		// If mouse moved and we're on the hook for showing the cursor, repaint
		if (!m_hideForMovement && !m_imageEmpty) {
			hasRepaint = true;
		}

		// If mouse moved and screen is magnified, repaint
		if ( zoomScaleRatio != 1.0 )
		{
			hasRepaint = true;
		}
	}

	// Ignore the first events as it's likely to be non-user-initiated warps
	if (!window || window->mouseMoved++ < 5 )
		return;

	m_lastMovedTime = get_time_in_milliseconds();
	m_hideForMovement = false;
}

void MouseCursor::updatePosition()
{
	int x,y;
	queryGlobalPosition(x, y);
	move(x, y);
	checkSuspension();
}

int MouseCursor::x() const
{
	return m_x;
}

int MouseCursor::y() const
{
	return m_y;
}

bool MouseCursor::getTexture()
{
	if (!m_dirty) {
		return !m_imageEmpty;
	}

	auto *image = XFixesGetCursorImage(m_display);

	if (!image) {
		return false;
	}

	m_hotspotX = image->xhot;
	m_hotspotY = image->yhot;

	m_width = image->width;
	m_height = image->height;
	if ( BIsNested() == false && alwaysComposite == false )
	{
		m_width = g_DRM.cursor_width;
		m_height = g_DRM.cursor_height;
	}

	if (m_texture) {
		vulkan_free_texture(m_texture);
		m_texture = 0;
	}

	// Assume the cursor is fully translucent unless proven otherwise.
	bool bNoCursor = true;

	auto cursorBuffer = std::vector<uint32_t>(m_width * m_height);
	for (int i = 0; i < image->height; i++) {
		for (int j = 0; j < image->width; j++) {
			cursorBuffer[i * m_width + j] = image->pixels[i * image->width + j];

			if ( cursorBuffer[i * m_width + j] & 0xff000000 ) {
				bNoCursor = false;
			}
		}
	}

	if (bNoCursor != m_imageEmpty) {
		m_imageEmpty = bNoCursor;

		if (m_imageEmpty) {
// 				fprintf( stderr, "grab?\n" );
		}
	}

	if (m_imageEmpty) {
		return false;
	}

	CVulkanTexture::createFlags texCreateFlags;
	if ( BIsNested() == false )
	{
		texCreateFlags.bFlippable = true;
		texCreateFlags.bLinear = true; // cursor buffer needs to be linear
		// TODO: choose format & modifiers from cursor plane
	}

	m_texture = vulkan_create_texture_from_bits(m_width, m_height, VK_FORMAT_B8G8R8A8_UNORM, texCreateFlags, cursorBuffer.data());
	assert(m_texture);
	XFree(image);
	m_dirty = false;

	return true;
}

void MouseCursor::paint(win *window, struct Composite_t *pComposite,
						struct VulkanPipeline_t *pPipeline)
{
	if (m_hideForMovement || m_imageEmpty) {
		return;
	}

	int rootX, rootY, winX, winY;
	queryPositions(rootX, rootY, winX, winY);
	move(rootX, rootY);

	// Also need new texture
	if (!getTexture()) {
		return;
	}

	float scaledX, scaledY;
	float currentScaleRatio = 1.0;
	float XRatio = (float)currentOutputWidth / window->a.width;
	float YRatio = (float)currentOutputHeight / window->a.height;
	int cursorOffsetX, cursorOffsetY;

	currentScaleRatio = (XRatio < YRatio) ? XRatio : YRatio;
	currentScaleRatio = std::min(g_flMaxWindowScale, currentScaleRatio);
	if (g_bIntegerScale)
		currentScaleRatio = floor(currentScaleRatio);

	cursorOffsetX = (currentOutputWidth - window->a.width * currentScaleRatio * globalScaleRatio) / 2.0f;
	cursorOffsetY = (currentOutputHeight - window->a.height * currentScaleRatio * globalScaleRatio) / 2.0f;

	// Actual point on scaled screen where the cursor hotspot should be
	scaledX = (winX - window->a.x) * currentScaleRatio * globalScaleRatio + cursorOffsetX;
	scaledY = (winY - window->a.y) * currentScaleRatio * globalScaleRatio + cursorOffsetY;

	if ( zoomScaleRatio != 1.0 )
	{
		scaledX += ((window->a.width / 2) - winX) * currentScaleRatio * globalScaleRatio;
		scaledY += ((window->a.height / 2) - winY) * currentScaleRatio * globalScaleRatio;
	}

	// Apply the cursor offset inside the texture using the display scale
	scaledX = scaledX - m_hotspotX;
	scaledY = scaledY - m_hotspotY;

	int curLayer = pComposite->nLayerCount;

	pComposite->data.flOpacity[ curLayer ] = 1.0;

	pComposite->data.vScale[ curLayer ].x = 1.0;
	pComposite->data.vScale[ curLayer ].y = 1.0;

	pComposite->data.vOffset[ curLayer ].x = -scaledX;
	pComposite->data.vOffset[ curLayer ].y = -scaledY;

	pComposite->data.flBorderAlpha[ curLayer ] = 0.0f;

	pPipeline->layerBindings[ curLayer ].surfaceWidth = m_width;
	pPipeline->layerBindings[ curLayer ].surfaceHeight = m_height;

	pPipeline->layerBindings[ curLayer ].zpos = g_zposCursor; // cursor, on top of both bottom layers

	pPipeline->layerBindings[ curLayer ].tex = m_texture;
	pPipeline->layerBindings[ curLayer ].fbid = BIsNested() ? 0 :
															  vulkan_texture_get_fbid(m_texture);

	pPipeline->layerBindings[ curLayer ].bFilter = false;

	pComposite->nLayerCount += 1;
}

struct BaseLayerInfo_t
{
	float scale[2];
	float offset[2];
	float opacity;
};

std::array< BaseLayerInfo_t, HELD_COMMIT_COUNT > g_CachedPlanes = {};

static bool
paint_cached_base_layer(const commit_t& commit, const BaseLayerInfo_t& base, struct Composite_t *pComposite, struct VulkanPipeline_t *pPipeline, float flOpacityScale)
{
	if (!commit.done)
		return false;
	int curLayer = pComposite->nLayerCount;

	pComposite->data.vScale[ curLayer ].x = base.scale[0];
	pComposite->data.vScale[ curLayer ].y = base.scale[1];
	pComposite->data.vOffset[ curLayer ].x = base.offset[0];
	pComposite->data.vOffset[ curLayer ].y = base.offset[1];
	pComposite->data.flOpacity[ curLayer ] = base.opacity * flOpacityScale;

	pPipeline->layerBindings[ curLayer ].tex = commit.vulkanTex;
	pPipeline->layerBindings[ curLayer ].fbid = commit.fb_id;
	pPipeline->layerBindings[ curLayer ].bFilter = true;

	pComposite->nLayerCount++;

	return true;
}

static bool
paint_window(Display *dpy, win *w, win *scaleW, struct Composite_t *pComposite,
			  struct VulkanPipeline_t *pPipeline, bool notificationMode, MouseCursor *cursor, bool bBasePlane = false, float flOpacityScale = 1.0f, bool bFadeTarget = false)
{
	uint32_t sourceWidth, sourceHeight;
	int drawXOffset = 0, drawYOffset = 0;
	float currentScaleRatio = 1.0;
	commit_t lastCommit = {};
	bool validContents = w ? get_window_last_done_commit( w, lastCommit ) : false;

	if ( bBasePlane )
	{
		if ( !validContents )
		{
			// If we're the base plane and have no valid contents
			// pick up that buffer we've been holding onto if we have one.
			if ( g_HeldCommits[ HELD_COMMIT_BASE ].done )
				return paint_cached_base_layer( g_HeldCommits[ HELD_COMMIT_BASE ], g_CachedPlanes[ HELD_COMMIT_BASE ], pComposite, pPipeline, flOpacityScale );
		}
		else
		{
			if ( g_bPendingFade )
			{
				fadeOutStartTime = get_time_in_milliseconds();
				g_bPendingFade = false;
			}
		}
	}

	if (!w)
		return false;

	// Don't add a layer at all if it's an overlay without contents
	if (w->isOverlay && !validContents)
		return false;

	// Base plane will stay as tex=0 if we don't have contents yet, which will
	// make us fall back to compositing and use the Vulkan null texture

	win *mainOverlayWindow = find_win(dpy, currentOverlayWindow);

	if (notificationMode && !mainOverlayWindow)
		return false;

	if (notificationMode)
	{
		sourceWidth = mainOverlayWindow->a.width;
		sourceHeight = mainOverlayWindow->a.height;
	}
	else
	{
		sourceWidth = scaleW->a.width;
		sourceHeight = scaleW->a.height;
	}

	if (sourceWidth != currentOutputWidth || sourceHeight != currentOutputHeight || globalScaleRatio != 1.0f)
	{
		float XRatio = (float)currentOutputWidth / sourceWidth;
		float YRatio = (float)currentOutputHeight / sourceHeight;

		currentScaleRatio = (XRatio < YRatio) ? XRatio : YRatio;
		currentScaleRatio = std::min(g_flMaxWindowScale, currentScaleRatio);
		if (g_bIntegerScale)
			currentScaleRatio = floor(currentScaleRatio);
		currentScaleRatio *= globalScaleRatio;

		drawXOffset = ((int)currentOutputWidth - (int)sourceWidth * currentScaleRatio) / 2.0f;
		drawYOffset = ((int)currentOutputHeight - (int)sourceHeight * currentScaleRatio) / 2.0f;

		if (w != scaleW)
		{
			drawXOffset += w->a.x * currentScaleRatio;
			drawYOffset += w->a.y * currentScaleRatio;
		}

		if ( zoomScaleRatio != 1.0 )
		{
			drawXOffset += (((int)sourceWidth / 2) - cursor->x()) * currentScaleRatio;
			drawYOffset += (((int)sourceHeight / 2) - cursor->y()) * currentScaleRatio;
		}
	}

	int curLayer = pComposite->nLayerCount;

	pComposite->data.flOpacity[ curLayer ] = ( w->isOverlay ? w->opacity / (float)OPAQUE : 1.0f ) * flOpacityScale;

	pComposite->data.vScale[ curLayer ].x = 1.0 / currentScaleRatio;
	pComposite->data.vScale[ curLayer ].y = 1.0 / currentScaleRatio;

	if ( w != scaleW )
	{
		pComposite->data.vOffset[ curLayer ].x = -drawXOffset;
		pComposite->data.vOffset[ curLayer ].y = -drawYOffset;

		pComposite->data.flBorderAlpha[ curLayer ] = 0.0f;
	}
	else if (notificationMode)
	{
		int xOffset = 0, yOffset = 0;

		int width = w->a.width * currentScaleRatio;
		int height = w->a.height * currentScaleRatio;

		if (globalScaleRatio != 1.0f)
		{
			xOffset = (currentOutputWidth - currentOutputWidth * globalScaleRatio) / 2.0;
			yOffset = (currentOutputHeight - currentOutputHeight * globalScaleRatio) / 2.0;
		}

		pComposite->data.vOffset[ curLayer ].x = (currentOutputWidth - xOffset - width) * -1.0f;
		pComposite->data.vOffset[ curLayer ].y = (currentOutputHeight - yOffset - height) * -1.0f;

		pComposite->data.flBorderAlpha[ curLayer ] = 0.0f;
	}
	else
	{
		pComposite->data.vOffset[ curLayer ].x = -drawXOffset;
		pComposite->data.vOffset[ curLayer ].y = -drawYOffset;

		pComposite->data.flBorderAlpha[ curLayer ] = 1.0f;
	}

	pPipeline->layerBindings[ curLayer ].surfaceWidth = w->a.width;
	pPipeline->layerBindings[ curLayer ].surfaceHeight = w->a.height;

	pPipeline->layerBindings[ curLayer ].zpos = g_zposBase;

	if ( w != scaleW )
	{
		pPipeline->layerBindings[ curLayer ].zpos = g_zposOverride;
	}

	if ( w->isOverlay || w->isSteamStreamingClient )
	{
		pPipeline->layerBindings[ curLayer ].zpos = g_zposOverlay;
	}

	pPipeline->layerBindings[ curLayer ].tex = lastCommit.vulkanTex;
	pPipeline->layerBindings[ curLayer ].fbid = lastCommit.fb_id;

	pPipeline->layerBindings[ curLayer ].bFilter = w->isOverlay ? true : g_bFilterGameWindow;

	if ( bBasePlane )
	{
		BaseLayerInfo_t basePlane = {};
		basePlane.scale[0] = pComposite->data.vScale[ curLayer ].x;
		basePlane.scale[1] = pComposite->data.vScale[ curLayer ].y;
		basePlane.offset[0] = pComposite->data.vOffset[ curLayer ].x;
		basePlane.offset[1] = pComposite->data.vOffset[ curLayer ].y;
		basePlane.opacity = pComposite->data.flOpacity[ curLayer ];

		g_CachedPlanes[ HELD_COMMIT_BASE ] = basePlane;
		if ( !bFadeTarget )
			g_CachedPlanes[ HELD_COMMIT_FADE ] = basePlane;
	}

	pComposite->nLayerCount += 1;

	return validContents;
}

static void
paint_message(const char *message, int Y, float r, float g, float b)
{

}

static void
paint_debug_info(Display *dpy)
{
	int Y = 100;

// 	glBindTexture(GL_TEXTURE_2D, 0);

	char messageBuffer[256];

	sprintf(messageBuffer, "Compositing at %.1f FPS", currentFrameRate);

	float textYMax = 0.0f;

	paint_message(messageBuffer, Y, 1.0f, 1.0f, 1.0f); Y += textYMax;
	if (find_win(dpy, currentFocusWindow))
	{
		if (gameFocused)
		{
			sprintf(messageBuffer, "Presenting game window %x", (unsigned int)currentFocusWindow);
			paint_message(messageBuffer, Y, 0.0f, 1.0f, 0.0f); Y += textYMax;
		}
		else
		{
			// must be Steam
			paint_message("Presenting Steam", Y, 1.0f, 1.0f, 0.0f); Y += textYMax;
		}
	}

	win *overlay = find_win(dpy, currentOverlayWindow);
	win *notification = find_win(dpy, currentNotificationWindow);

	if (overlay && gamesRunningCount && overlay->opacity)
	{
		sprintf(messageBuffer, "Compositing overlay at opacity %f", overlay->opacity / (float)OPAQUE);
		paint_message(messageBuffer, Y, 1.0f, 0.0f, 1.0f); Y += textYMax;
	}

	if (notification && gamesRunningCount && notification->opacity)
	{
		sprintf(messageBuffer, "Compositing notification at opacity %f", notification->opacity / (float)OPAQUE);
		paint_message(messageBuffer, Y, 1.0f, 0.0f, 1.0f); Y += textYMax;
	}

	if (gotXError) {
		paint_message("Encountered X11 error", Y, 1.0f, 0.0f, 0.0f); Y += textYMax;
	}
}

bool g_bFirstFrame = true;

static bool is_fading_out()
{
	return fadeOutStartTime || g_bPendingFade;
}

static void
paint_all(Display *dpy, MouseCursor *cursor)
{
	static long long int paintID = 0;

	paintID++;
	gpuvis_trace_begin_ctx_printf( paintID, "paint_all" );
	win	*w;
	win	*overlay;
	win	*notification;
	win	*override;
	win *input;

	unsigned int currentTime = get_time_in_milliseconds();
	bool fadingOut = ( currentTime - fadeOutStartTime < g_FadeOutDuration || g_bPendingFade ) && g_HeldCommits[HELD_COMMIT_FADE].done;

	w = find_win(dpy, currentFocusWindow);
	overlay = find_win(dpy, currentOverlayWindow);
	notification = find_win(dpy, currentNotificationWindow);
	override = find_win(dpy, currentOverrideWindow);
	input = find_win(dpy, currentInputFocusWindow);

	if ( !w )
	{
		return;
	}

	bool inGame = false;

	if ( gamesRunningCount || w->appID != 0 )
	{
		inGame = true;
	}

	frameCounter++;

	if (frameCounter == 300)
	{
		currentFrameRate = 300 * 1000.0f / (currentTime - lastSampledFrameTime);
		lastSampledFrameTime = currentTime;
		frameCounter = 0;

		stats_printf( "fps=%f\n", currentFrameRate );

		if ( w->isSteam )
		{
			stats_printf( "focus=steam\n" );
		}
		else
		{
			stats_printf( "focus=%i\n", w->appID );
		}
	}

	struct Composite_t composite = {};
	struct VulkanPipeline_t pipeline = {};
	bool bValidContents = false;

	// If the window we'd paint as the base layer is the streaming client,
	// find the video underlay and put it up first in the scenegraph
	if ( w->isSteamStreamingClient == true )
	{
		win *videow = NULL;

		for ( videow = list; videow; videow = videow->next )
		{
			if ( videow->isSteamStreamingClientVideo == true )
			{
				// TODO: also check matching AppID so we can have several pairs
				bValidContents |= paint_window(dpy, videow, videow, &composite, &pipeline, false, cursor);
				break;
			}
		}

		// paint UI unless it's fully hidden, which it communicates to us through opacity=0
		if ( w->opacity > TRANSLUCENT )
		{
			bValidContents |= paint_window(dpy, w, w, &composite, &pipeline, false, cursor);
		}
	}
	else
	{
		if ( fadingOut )
		{
			float opacityScale = g_bPendingFade
				? 0.0f
				: ((currentTime - fadeOutStartTime) / (float)g_FadeOutDuration);
	
			bValidContents |= paint_cached_base_layer(g_HeldCommits[HELD_COMMIT_FADE], g_CachedPlanes[HELD_COMMIT_FADE], &composite, &pipeline, 1.0f - opacityScale);
			bValidContents |= paint_window(dpy, w, w, &composite, &pipeline, false, cursor, true, opacityScale, true);
		}
		else
		{
			{
				if ( g_HeldCommits[HELD_COMMIT_FADE].done )
				{
					release_commit(g_HeldCommits[HELD_COMMIT_FADE]);
					g_HeldCommits[HELD_COMMIT_FADE] = commit_t{};
					g_bPendingFade = false;
					fadeOutStartTime = 0;
					currentFadeWindow = None;
				}
			}
			// Just draw focused window as normal, be it Steam or the game
			bValidContents |= paint_window(dpy, w, w, &composite, &pipeline, false, cursor, true);
		}
	}

	// TODO: We want to paint this at the same scale as the normal window and probably
	// with an offset.
	if (override)
		bValidContents |= paint_window(dpy, override, w, &composite, &pipeline, false, cursor);

	int touchInputFocusLayer = composite.nLayerCount - 1;

	if (inGame && overlay)
	{
		if (overlay->opacity)
		{
			bValidContents |= paint_window(dpy, overlay, overlay, &composite, &pipeline, false, cursor);

			if ( overlay->id == currentInputFocusWindow )
				touchInputFocusLayer = composite.nLayerCount - 1;
		}
	}

	if ( touchInputFocusLayer >= 0 )
	{
		focusedWindowScaleX = composite.data.vScale[ touchInputFocusLayer ].x;
		focusedWindowScaleY = composite.data.vScale[ touchInputFocusLayer ].y;
		focusedWindowOffsetX = composite.data.vOffset[ touchInputFocusLayer ].x;
		focusedWindowOffsetY = composite.data.vOffset[ touchInputFocusLayer ].y;
	}

	if (inGame && notification)
	{
		if (notification->opacity)
		{
			bValidContents |= paint_window(dpy, notification, notification, &composite, &pipeline, true, cursor);
		}
	}

	// Draw cursor if we need to
	if (input) {
		cursor->paint(override == input ? w : input, &composite, &pipeline);
	}

	if (drawDebugInfo)
		paint_debug_info(dpy);

	// If this is our first frame,and we have no valid contents, then toss it.
	// We don't want to do this on any future frames as we can end up in the following scenario:
	// -> Game crashes
	// -> Stuck on the last frame of the game
	// -> User brings up overlay
	// -> User closes overlay
	// -> Overlay is still visible :( but not interactable
	bool bDiscard = g_bFirstFrame && !bValidContents;

	if ( bDiscard || ( BIsNested() == false && g_DRM.paused == true ) )
	{
		return;
	}

	g_bFirstFrame = false;

	bool bDoComposite = true;

	// Handoff from whatever thread to this one since we check ours twice
	bool takeScreenshot = g_bTakeScreenshot.exchange(false);
	bool propertyRequestedScreenshot = g_bPropertyRequestedScreenshot;
	g_bPropertyRequestedScreenshot = false;

	struct pipewire_buffer *pw_buffer = nullptr;
#if HAVE_PIPEWIRE
	pw_buffer = dequeue_pipewire_buffer();
#endif

	bool bCapture = takeScreenshot || pw_buffer != nullptr;
	
#ifdef WORKAROUND_ZPOS_BUG
	const bool bOverrideCompositeHack = override != nullptr;
#else
	const bool bOverrideCompositeHack = false;
#endif

	if ( BIsNested() == false && alwaysComposite == false && bCapture == false && bOverrideCompositeHack == false )
	{
		int ret = drm_prepare( &g_DRM, &composite, &pipeline );
		if ( ret == 0 )
			bDoComposite = false;
		else if ( ret == -EACCES )
			return;
	}

	if ( bDoComposite == true )
	{
		std::shared_ptr<CVulkanTexture> pCaptureTexture = nullptr;

		bool bResult = vulkan_composite( &composite, &pipeline, bCapture ? &pCaptureTexture : nullptr );

		if ( bResult != true )
		{
			xwm_log.errorf("vulkan_composite failed");
			return;
		}

		if ( BIsNested() == true )
		{
			vulkan_present_to_window();
		}
		else
		{
			memset( &composite, 0, sizeof( composite ) );
			composite.nLayerCount = 1;
			composite.data.vScale[ 0 ].x = 1.0;
			composite.data.vScale[ 0 ].y = 1.0;
			composite.data.flOpacity[ 0 ] = 1.0;

			memset( &pipeline, 0, sizeof( pipeline ) );

			pipeline.layerBindings[ 0 ].surfaceWidth = g_nOutputWidth;
			pipeline.layerBindings[ 0 ].surfaceHeight = g_nOutputHeight;

			pipeline.layerBindings[ 0 ].fbid = vulkan_get_last_composite_fbid();
			pipeline.layerBindings[ 0 ].bFilter = false;

			int ret = drm_prepare( &g_DRM, &composite, &pipeline );

			// Happens when we're VT-switched away
			if ( ret == -EACCES )
				return;

			if ( ret != 0 )
			{
				xwm_log.errorf( "Failed to prepare 1-layer flip: %s", strerror( -ret ) );
				// We should always handle a 1-layer flip
				abort();
			}

			drm_commit( &g_DRM, &composite, &pipeline );
		}

		if ( takeScreenshot )
		{
			assert( pCaptureTexture != nullptr );
			assert( pCaptureTexture->m_format == VK_FORMAT_B8G8R8A8_UNORM );

			std::thread screenshotThread = std::thread([=] {
				pthread_setname_np( pthread_self(), "gamescope-scrsh" );

				const uint8_t *mappedData = reinterpret_cast<const uint8_t *>(pCaptureTexture->m_pMappedData);

				// Make our own copy of the image to remove the alpha channel.
				auto imageData = std::vector<uint8_t>(currentOutputWidth * currentOutputHeight * 4);
				const uint32_t comp = 4;
				const uint32_t pitch = currentOutputWidth * comp;
				for (uint32_t y = 0; y < currentOutputHeight; y++)
				{
					for (uint32_t x = 0; x < currentOutputWidth; x++)
					{
						// BGR...
						imageData[y * pitch + x * comp + 0] = mappedData[y * pCaptureTexture->m_unRowPitch + x * comp + 2];
						imageData[y * pitch + x * comp + 1] = mappedData[y * pCaptureTexture->m_unRowPitch + x * comp + 1];
						imageData[y * pitch + x * comp + 2] = mappedData[y * pCaptureTexture->m_unRowPitch + x * comp + 0];
						imageData[y * pitch + x * comp + 3] = 255;
					}
				}

				char pTimeBuffer[1024] = "/tmp/gamescope.png";

				if ( !propertyRequestedScreenshot )
				{
					time_t currentTime = time(0);
					struct tm *localTime = localtime( &currentTime );
					strftime( pTimeBuffer, sizeof( pTimeBuffer ), "/tmp/gamescope_%Y-%m-%d_%H-%M-%S.png", localTime );
				}

				if ( stbi_write_png(pTimeBuffer, currentOutputWidth, currentOutputHeight, 4, imageData.data(), pitch) )
				{
					xwm_log.infof("Screenshot saved to %s", pTimeBuffer);
				}
				else
				{
					xwm_log.errorf( "Failed to save screenshot to %s", pTimeBuffer );
				}
			});

			screenshotThread.detach();

			takeScreenshot = false;
		}

#if HAVE_PIPEWIRE
		if ( pw_buffer != nullptr )
		{
			assert( pCaptureTexture != nullptr );
			assert( pw_buffer->texture == nullptr );

			pw_buffer->texture = pCaptureTexture;

			push_pipewire_buffer(pw_buffer);
			// TODO: make sure the buffer isn't lost in one of the failure
			// code-paths above
		}
#endif
	}
	else
	{
		assert( BIsNested() == false );

		drm_commit( &g_DRM, &composite, &pipeline );
	}

	gpuvis_trace_end_ctx_printf( paintID, "paint_all" );
	gpuvis_trace_printf( "paint_all %i layers, composite %i", (int)composite.nLayerCount, bDoComposite );
}

/* Get prop from window
 *   not found: default
 *   otherwise the value
 */
static unsigned int
get_prop(Display *dpy, Window win, Atom prop, unsigned int def, bool *found = nullptr )
{
	Atom actual;
	int format;
	unsigned long n, left;

	unsigned char *data;
	int result = XGetWindowProperty(dpy, win, prop, 0L, 1L, false,
									XA_CARDINAL, &actual, &format,
								 &n, &left, &data);
	if (result == Success && data != NULL)
	{
		unsigned int i;
		memcpy(&i, data, sizeof(unsigned int));
		XFree((void *) data);
		if ( found != nullptr )
		{
			*found = true;
		}
		return i;
	}
	if ( found != nullptr )
	{
		*found = false;
	}
	return def;
}

// vectored version, return value is whether anything was found
bool get_prop( Display *dpy, Window win, Atom prop, std::vector< uint32_t > &vecResult )
{
	Atom actual;
	int format;
	unsigned long n, left;

	vecResult.clear();
	uint64_t *data;
	// get up to 16 results in one go, we can add a real loop if we ever need anything beyong that
	int result = XGetWindowProperty(dpy, win, prop, 0L, 16L, false,
									XA_CARDINAL, &actual, &format,
									&n, &left, ( unsigned char** )&data);
	if (result == Success && data != NULL)
	{
		for ( uint32_t i = 0; i < n; i++ )
		{
			vecResult.push_back( data[ i ] );
		}
		XFree((void *) data);
		return true;
	}
	return false;
}

static bool
win_has_game_id( win *w )
{
	return w->appID != 0;
}

static bool
win_is_override_redirect( win *w )
{
	return w->a.override_redirect && !w->ignoreOverrideRedirect;
}

static bool
win_skip_taskbar_and_pager( win *w )
{
	return w->skipTaskbar && w->skipPager;
}

/* Returns true if a's focus priority > b's.
 *
 * This function establishes a list of criteria to decide which window should
 * have focus. The first criteria has higher priority. If the first criteria
 * is a tie, fallback to the second one, then the third, and so on.
 *
 * The general workflow is:
 *
 *     if ( windows don't have the same criteria value )
 *         return true if a should be focused;
 *     // This is a tie, fallback to the next criteria
 */
static bool
is_focus_priority_greater( win *a, win *b )
{
	if ( win_has_game_id( a ) != win_has_game_id( b ) )
		return win_has_game_id( a );

	// We allow using an override redirect window in some cases, but if we have
	// a choice between two windows we always prefer the non-override redirect
	// one.
	if ( win_is_override_redirect( a ) != win_is_override_redirect( b ) )
		return !win_is_override_redirect( a );

	// Wine sets SKIP_TASKBAR and SKIP_PAGER hints for WS_EX_NOACTIVATE windows.
	// See https://github.com/Plagman/gamescope/issues/87
	if ( win_skip_taskbar_and_pager( a ) != win_skip_taskbar_and_pager( b ) )
		return !win_skip_taskbar_and_pager( a );

	// The damage sequences are only relevant for game windows.
	if ( win_has_game_id( a ) && a->damage_sequence != b->damage_sequence )
		return a->damage_sequence > b->damage_sequence;

	return false;
}

static bool is_good_override_candidate( win *override, win* focus )
{
	// Some Chrome/Edge dropdowns (ie. FH5 xbox login) will automatically close themselves if you
	// focus them while they are meant to be offscreen (-1,-1 and 1x1) so check that the
	// override's position is on-screen.
	return win_is_override_redirect(override) && override != focus && override->a.x > 0 && override->a.y > 0;
} 

static void
determine_and_apply_focus(Display *dpy, MouseCursor *cursor)
{
	win *w, *focus = NULL, *override_focus = NULL;
	win *inputFocus = NULL;

	gameFocused = false;

	Window prevFocusWindow = currentFocusWindow;
	Window prevOverlayWindow = currentOverlayWindow;
	Window prevNotificationWindow = currentNotificationWindow;
	Window prevOverrideWindow = currentOverrideWindow;
	currentFocusWindow = None;
	currentFocusWin = nullptr;
	currentOverlayWindow = None;
	currentNotificationWindow = None;
	currentOverrideWindow = None;

	unsigned int maxOpacity = 0;
	std::vector< win* > vecPossibleFocusWindows;
	for (w = list; w; w = w->next)
	{
		// Always skip system tray icons
		if ( w->isSysTrayIcon )
		{
			continue;
		}

		if ( w->a.map_state == IsViewable && w->a.c_class == InputOutput && w->isOverlay == false &&
			( win_has_game_id( w ) || w->isSteam || w->isSteamStreamingClient ) &&
			 (w->opacity > TRANSLUCENT || w->isSteamStreamingClient == true ) )
		{
			vecPossibleFocusWindows.push_back( w );
		}

		if (w->isOverlay)
		{
			if (w->a.width > 1200 && w->opacity >= maxOpacity)
			{
				currentOverlayWindow = w->id;
				maxOpacity = w->opacity;
			}
			else
			{
				currentNotificationWindow = w->id;
			}
		}

		if ( w->isOverlay && w->inputFocusMode )
		{
			inputFocus = w;
		}
	}

	if ( prevOverlayWindow != currentOverlayWindow ||
	     prevNotificationWindow != currentNotificationWindow )
	{
		hasRepaint = true;
	}

	std::vector< unsigned long > focusable_appids;
	std::vector< unsigned long > focusable_windows;

	for ( win *focusable_window : vecPossibleFocusWindows )
	{
		unsigned int unAppID = focusable_window->appID;
		if ( unAppID != 0 )
		{
			unsigned long j;
			for( j = 0; j < focusable_appids.size(); j++ )
			{
				if ( focusable_appids[ j ] == unAppID )
				{
					break;
				}
			}
			if ( j == focusable_appids.size() )
			{
				focusable_appids.push_back( unAppID );
			}
		}

		// list of [window, appid, pid] triplets
		focusable_windows.push_back( focusable_window->id );
		focusable_windows.push_back( focusable_window->appID );
		focusable_windows.push_back( focusable_window->pid );
	}

	XChangeProperty( dpy, root, gamescopeFocusableAppsAtom, XA_CARDINAL, 32, PropModeReplace,
					 (unsigned char *)focusable_appids.data(), focusable_appids.size() );

	XChangeProperty( dpy, root, gamescopeFocusableWindowsAtom, XA_CARDINAL, 32, PropModeReplace,
					 (unsigned char *)focusable_windows.data(), focusable_windows.size() );

	std::stable_sort( vecPossibleFocusWindows.begin(), vecPossibleFocusWindows.end(),
					  is_focus_priority_greater );

	if ( focusControlWindow != None || vecFocuscontrolAppIDs.size() > 0 )
	{
		if ( focusControlWindow != None )
		{
			for ( win *focusable_window : vecPossibleFocusWindows )
			{
				if ( focusable_window->id == focusControlWindow )
				{
					focus = focusable_window;
					goto found;
				}
			}
		}

		for ( auto focusable_appid : vecFocuscontrolAppIDs )
		{
			for ( win *focusable_window : vecPossibleFocusWindows )
			{
				if ( focusable_window->appID == focusable_appid )
				{
					focus = focusable_window;
					goto found;
				}
			}
		}
found:
		gameFocused = true;
	}
	else if ( vecPossibleFocusWindows.size() > 0 )
	{
		focus = vecPossibleFocusWindows[ 0 ];
		gameFocused = focus->appID != 0;
	}

	auto resolveTransientOverrides = [&]()
	{
		// Do some searches to find transient links to override redirects too.
		while ( true )
		{
			bool bFoundTransient = false;

			for ( win *candidate : vecPossibleFocusWindows )
			{
				if ( ( !override_focus || candidate != override_focus ) && candidate != focus &&
					( ( !override_focus && candidate->transientFor == focus->id ) || ( override_focus && candidate->transientFor == override_focus->id ) ) &&
					candidate->a.override_redirect )
				{
					bFoundTransient = true;
					override_focus = candidate;
					break;
				}
			}

			// Hopefully we can't have transient cycles or we'll have to maintain a list of visited windows here
			if ( bFoundTransient == false )
				break;
		}
	};

	if ( gameFocused && focus )
	{
		// Do some searches through game windows to follow transient links if needed
		while ( true )
		{
			bool bFoundTransient = false;

			for ( win *candidate : vecPossibleFocusWindows )
			{
				if ( candidate != focus && candidate->transientFor == focus->id && !candidate->a.override_redirect )
				{
					bFoundTransient = true;
					focus = candidate;
					break;
				}
			}

			// Hopefully we can't have transient cycles or we'll have to maintain a list of visited windows here
			if ( bFoundTransient == false )
				break;
		}

		resolveTransientOverrides();
	}

	if ( !override_focus && focus )
	{
		if ( vecFocuscontrolAppIDs.size() > 0 )
		{
			for ( win *override : vecPossibleFocusWindows )
			{
				if ( is_good_override_candidate(override, focus) && override->appID == focus->appID ) {
					override_focus = override;
					break;
				}
			}
		}
		else if ( vecPossibleFocusWindows.size() > 0 )
		{
			for ( win *override : vecPossibleFocusWindows )
			{
				if ( is_good_override_candidate(override, focus) ) {
					override_focus = override;
					break;
				}
			}
		}

		resolveTransientOverrides();
	}


	currentOverrideWindow = override_focus ? override_focus->id : None;

	if ( currentOverrideWindow != prevOverrideWindow )
		hasRepaint = true;

	unsigned long focusedWindow = 0;
	unsigned long focusedAppId = 0;

	if ( inputFocus == NULL )
	{
		inputFocus = override_focus ? override_focus : focus;
	}

	if ( focus )
	{
		focusedWindow = focus->id;
		focusedAppId = inputFocus->appID;
	}

	XChangeProperty( dpy, root, gamescopeFocusedAppAtom, XA_CARDINAL, 32, PropModeReplace,
					 (unsigned char *)&focusedAppId, focusedAppId != 0 ? 1 : 0 );

	XChangeProperty( dpy, root, gamescopeFocusedWindowAtom, XA_CARDINAL, 32, PropModeReplace,
					 (unsigned char *)&focusedWindow, focusedWindow != 0 ? 1 : 0 );

	if (!focus)
	{
		return;
	}

	if ( prevFocusWindow != focus->id )
	{
		if ( g_FadeOutDuration != 0 && !g_bFirstFrame )
		{
			if ( !g_HeldCommits[ HELD_COMMIT_FADE ].done )
			{
				currentFadeWindow = prevFocusWindow;
				ref_commit( g_HeldCommits[ HELD_COMMIT_BASE ] );
				g_HeldCommits[ HELD_COMMIT_FADE ] = g_HeldCommits[ HELD_COMMIT_BASE ];
				g_bPendingFade = true;
			}
			else
			{
				// If we end up fading back to what we were going to fade to, cancel the fade.
				if ( currentFadeWindow != None && focus->id == currentFadeWindow )
				{
					release_commit( g_HeldCommits[ HELD_COMMIT_FADE ] );
					g_HeldCommits[ HELD_COMMIT_FADE ] = commit_t{};
					g_bPendingFade = false;
					fadeOutStartTime = 0;
					currentFadeWindow = None;
				}
			}
		}

		hasRepaint = true;

		/* Some games (e.g. DOOM Eternal) don't react well to being put back as
		* iconic, so never do that. Only take them out of iconic. */
		uint32_t wmState[] = { ICCCM_NORMAL_STATE, None };
		XChangeProperty(dpy, focus->id, WMStateAtom, WMStateAtom, 32,
					PropModeReplace, (unsigned char *)wmState,
					sizeof(wmState) / sizeof(wmState[0]));

		gpuvis_trace_printf( "determine_and_apply_focus focus %lu", focus->id );

		if ( debugFocus == true )
		{
			xwm_log.debugf( "determine_and_apply_focus focus %lu", focus->id );
			char buf[512];
			sprintf( buf,  "xwininfo -id 0x%lx; xprop -id 0x%lx; xwininfo -root -tree", focus->id, focus->id );
			system( buf );
		}
	}

	currentFocusWindow = focus->id;
	currentFocusWin = focus;

	if ( currentInputFocusWindow != inputFocus->id ||
		currentInputFocusMode != inputFocus->inputFocusMode )
	{
		win *keyboardFocusWin = inputFocus;

		if ( debugFocus == true )
		{
			xwm_log.debugf( "determine_and_apply_focus inputFocus %lu", inputFocus->id );
		}

		if ( inputFocus->inputFocusMode )
			keyboardFocusWin = override_focus ? override_focus : focus;

		if ( inputFocus->surface.wlr != nullptr || keyboardFocusWin->surface.wlr != nullptr )
		{
			wlserver_lock();

			if ( inputFocus->surface.wlr != nullptr )
			{
				// Instantly stop pressing left mouse before transitioning to a new window.
				// for focus.
				// Fixes dropdowns not working.
				wlserver_mousebutton( BTN_LEFT, false, 0 );
				wlserver_mousefocus( inputFocus->surface.wlr, cursor->x(), cursor->y() );
			}

			if ( keyboardFocusWin->surface.wlr != nullptr )
				wlserver_keyboardfocus( keyboardFocusWin->surface.wlr );

			wlserver_unlock();
		}

		if ( !override_focus || override_focus != keyboardFocusWin )
			XSetInputFocus(dpy, keyboardFocusWin->id, RevertToNone, CurrentTime);

		currentInputFocusWindow = inputFocus->id;
		currentInputFocusMode = inputFocus->inputFocusMode;
		currentKeyboardFocusWindow = keyboardFocusWin->id;

		// cursor is likely not interactable anymore in its original context, hide
		cursor->hide();
	}

	w = focus;

	cursor->constrainPosition();

	if ( list[0].id != inputFocus->id )
	{
		XRaiseWindow(dpy, inputFocus->id);
	}

	if (!focus->nudged)
	{
		XMoveWindow(dpy, focus->id, 1, 1);
		focus->nudged = true;
	}

	if (w->a.x != 0 || w->a.y != 0)
		XMoveWindow(dpy, focus->id, 0, 0);

	if ( focus->isFullscreen && ( w->a.width != root_width || w->a.height != root_height || globalScaleRatio != 1.0f ) )
	{
		XResizeWindow(dpy, focus->id, root_width, root_height);
	}
	else if (!focus->isFullscreen && focus->sizeHintsSpecified &&
		((unsigned)focus->a.width != focus->requestedWidth ||
		(unsigned)focus->a.height != focus->requestedHeight))
	{
		XResizeWindow(dpy, focus->id, focus->requestedWidth, focus->requestedHeight);
	}

	Window	    root_return = None, parent_return = None;
	Window	    *children = NULL;
	unsigned int    nchildren = 0;
	unsigned int    i = 0;

	XQueryTree(dpy, w->id, &root_return, &parent_return, &children, &nchildren);

	while (i < nchildren)
	{
		XSelectInput( dpy, children[i], PointerMotionMask | FocusChangeMask );
		i++;
	}

	XFree(children);
}

static void
get_size_hints(Display *dpy, win *w)
{
	XSizeHints hints;
	long hintsSpecified = 0;

	XGetWMNormalHints(dpy, w->id, &hints, &hintsSpecified);

	if (hintsSpecified & (PMaxSize | PMinSize) &&
		hints.max_width && hints.max_height && hints.min_width && hints.min_height &&
		hints.max_width == hints.min_width && hints.min_height == hints.max_height)
	{
		w->requestedWidth = hints.max_width;
		w->requestedHeight = hints.max_height;

		w->sizeHintsSpecified = true;
	}
	else
	{
		w->sizeHintsSpecified = false;

		// Below block checks for a pattern that matches old SDL fullscreen applications;
		// SDL creates a fullscreen overrride-redirect window and reparents the game
		// window under it, centered. We get rid of the modeswitch and also want that
		// black border gone.
		if (w->a.override_redirect)
		{
			Window	    root_return = None, parent_return = None;
			Window	    *children = NULL;
			unsigned int    nchildren = 0;

			XQueryTree(dpy, w->id, &root_return, &parent_return, &children, &nchildren);

			if (nchildren == 1)
			{
				XWindowAttributes attribs;

				XGetWindowAttributes(dpy, children[0], &attribs);

				// If we have a unique children that isn't override-reidrect that is
				// contained inside this fullscreen window, it's probably it.
				if (attribs.override_redirect == false &&
					attribs.width <= w->a.width &&
					attribs.height <= w->a.height)
				{
					w->sizeHintsSpecified = true;

					w->requestedWidth = attribs.width;
					w->requestedHeight = attribs.height;

					XMoveWindow(dpy, children[0], 0, 0);

					w->ignoreOverrideRedirect = true;
				}
			}

			XFree(children);
		}
	}
}

static void
get_win_title(Display *dpy, win *w, Atom atom)
{
	assert(atom == XA_WM_NAME || atom == netWMNameAtom);

	XTextProperty tp;
	XGetTextProperty( dpy, w->id, &tp, atom );

	bool is_utf8;
	if (tp.encoding == utf8StringAtom) {
		is_utf8 = true;
	} else if (tp.encoding == XA_STRING) {
		is_utf8 = false;
	} else {
		return;
	}

	if (!is_utf8 && w->utf8_title) {
		/* Clients usually set both the non-UTF8 title and the UTF8 title
		 * properties. If the client has set the UTF8 title prop, ignore the
		 * non-UTF8 one. */
		return;
	}

	free(w->title);
	if (tp.nitems > 0) {
		w->title = strndup((char *)tp.value, tp.nitems);
	} else {
		w->title = NULL;
	}
	w->utf8_title = is_utf8;
}

static void
get_net_wm_state(Display *dpy, win *w)
{
	Atom type;
	int format;
	unsigned long nitems;
	unsigned long bytesAfter;
	unsigned char *data;
	if (XGetWindowProperty(dpy, w->id, netWMStateAtom, 0, 2048, false,
			AnyPropertyType, &type, &format, &nitems, &bytesAfter, &data) != Success) {
		return;
	}

	Atom *props = (Atom *)data;
	for (size_t i = 0; i < nitems; i++) {
		if (props[i] == netWMStateFullscreenAtom) {
			w->isFullscreen = true;
		} else if (props[i] == netWMStateSkipTaskbarAtom) {
			w->skipTaskbar = true;
		} else if (props[i] == netWMStateSkipPagerAtom) {
			w->skipPager = true;
		} else {
			xwm_log.debugf("Unhandled initial NET_WM_STATE property: %s", XGetAtomName(dpy, props[i]));
		}
	}

	XFree(data);
}

static void
map_win(Display *dpy, Window id, unsigned long sequence)
{
	win		*w = find_win(dpy, id);

	if (!w)
		return;

	w->a.map_state = IsViewable;

	/* This needs to be here or else we lose transparency messages */
	XSelectInput(dpy, id, PropertyChangeMask | SubstructureNotifyMask |
		PointerMotionMask | LeaveWindowMask | FocusChangeMask);

	XFlush(dpy);

	/* This needs to be here since we don't get PropertyNotify when unmapped */
	w->opacity = get_prop(dpy, w->id, opacityAtom, OPAQUE);

	w->isSteam = get_prop(dpy, w->id, steamAtom, 0);

	/* First try to read the UTF8 title prop, then fallback to the non-UTF8 one */
	get_win_title( dpy, w, netWMNameAtom );
	get_win_title( dpy, w, XA_WM_NAME );

	w->inputFocusMode = get_prop(dpy, w->id, steamInputFocusAtom, 0);

	w->isSteamStreamingClient = get_prop(dpy, w->id, steamStreamingClientAtom, 0);
	w->isSteamStreamingClientVideo = get_prop(dpy, w->id, steamStreamingClientVideoAtom, 0);

	if ( steamMode == true )
	{
		uint32_t appID = get_prop(dpy, w->id, gameAtom, 0);

		if ( w->appID != 0 && appID != 0 && w->appID != appID )
		{
			xwm_log.errorf( "appid clash was %u now %u", w->appID, appID );
		}
		// Let the appID property be authoritative for now
		if ( appID != 0 )
		{
			w->appID = appID;
		}
	}
	else
	{
		w->appID = w->id;
	}
	w->isOverlay = get_prop(dpy, w->id, overlayAtom, 0);

	get_size_hints(dpy, w);

	get_net_wm_state(dpy, w);

	XWMHints *wmHints = XGetWMHints( dpy, w->id );

	if ( wmHints != nullptr )
	{
		if ( wmHints->flags & (InputHint | StateHint ) && wmHints->input == true && wmHints->initial_state == NormalState )
		{
			XRaiseWindow( dpy, w->id );
		}

		XFree( wmHints );
	}

	Window transientFor = None;
	if ( XGetTransientForHint( dpy, w->id, &transientFor ) )
	{
		w->transientFor = transientFor;
	}
	else
	{
		w->transientFor = None;
	}

	w->damage_sequence = 0;
	w->map_sequence = sequence;

	focusDirty = true;
}

static void
finish_unmap_win(Display *dpy, win *w)
{
	// TODO clear done commits here?

	/* don't care about properties anymore */
	set_ignore(dpy, NextRequest(dpy));
	XSelectInput(dpy, w->id, 0);

	clipChanged = true;
}

static void
unmap_win(Display *dpy, Window id, bool fade)
{
	win *w = find_win(dpy, id);
	if (!w)
		return;
	w->a.map_state = IsUnmapped;

	focusDirty = true;

	finish_unmap_win(dpy, w);
}

static uint32_t
get_appid_from_pid( pid_t pid )
{
	uint32_t unFoundAppId = 0;

	char filename[256];
	pid_t next_pid = pid;

	while ( 1 )
	{
		snprintf( filename, sizeof( filename ), "/proc/%i/stat", next_pid );
		std::ifstream proc_stat_file( filename );
		std::string proc_stat;

		std::getline( proc_stat_file, proc_stat );

		char *procName = nullptr;
		char *lastParens;

		for ( uint32_t i = 0; i < proc_stat.length(); i++ )
		{
			if ( procName == nullptr && proc_stat[ i ] == '(' )
			{
				procName = &proc_stat[ i + 1 ];
			}

			if ( proc_stat[ i ] == ')' )
			{
				lastParens = &proc_stat[ i ];
			}
		}

		*lastParens = '\0';
		char state;
		int parent_pid = -1;

		sscanf( lastParens + 1, " %c %d", &state, &parent_pid );

		if ( strcmp( "reaper", procName ) == 0 )
		{
			snprintf( filename, sizeof( filename ), "/proc/%i/cmdline", next_pid );
			std::ifstream proc_cmdline_file( filename );
			std::string proc_cmdline;

			bool bSteamLaunch = false;
			uint32_t unAppId = 0;

			std::getline( proc_cmdline_file, proc_cmdline );

			for ( uint32_t j = 0; j < proc_cmdline.length(); j++ )
			{
				if ( proc_cmdline[ j ] == '\0' && j + 1 < proc_cmdline.length() )
				{
					if ( strcmp( "SteamLaunch", &proc_cmdline[ j + 1 ] ) == 0 )
					{
						bSteamLaunch = true;
					}
					else if ( sscanf( &proc_cmdline[ j + 1 ], "AppId=%u", &unAppId ) == 1 && unAppId != 0 )
					{
						if ( bSteamLaunch == true )
						{
							unFoundAppId = unAppId;
						}
					}
					else if ( strcmp( "--", &proc_cmdline[ j + 1 ] ) == 0 )
					{
						break;
					}
				}
			}
		}

		if ( parent_pid == -1 || parent_pid == 0 )
		{
			break;
		}
		else
		{
			next_pid = parent_pid;
		}
	}

	return unFoundAppId;
}

static pid_t
get_win_pid(Display *dpy, Window id)
{
	XResClientIdSpec client_spec = {
		.client = id,
		.mask = XRES_CLIENT_ID_PID_MASK,
	};
	long num_ids = 0;
	XResClientIdValue *client_ids = NULL;
	XResQueryClientIds(dpy, 1, &client_spec, &num_ids, &client_ids);

	pid_t pid = -1;
	for (long i = 0; i < num_ids; i++) {
		pid = XResGetClientPid(&client_ids[i]);
		if (pid > 0)
			break;
	}
	XResClientIdsDestroy(num_ids, client_ids);
	if (pid <= 0)
		xwm_log.errorf("Failed to find PID for window 0x%lx", id);
	return pid;
}

static void
add_win(Display *dpy, Window id, Window prev, unsigned long sequence)
{
	win				*new_win = new win;
	win				**p;

	if (!new_win)
		return;
	if (prev)
	{
		for (p = &list; *p; p = &(*p)->next)
			if ((*p)->id == prev)
				break;
	}
	else
		p = &list;
	new_win->id = id;
	set_ignore(dpy, NextRequest(dpy));
	if (!XGetWindowAttributes(dpy, id, &new_win->a))
	{
		delete new_win;
		return;
	}

	new_win->damage_sequence = 0;
	new_win->map_sequence = 0;
	if (new_win->a.c_class == InputOnly)
		new_win->damage = None;
	else
	{
		set_ignore(dpy, NextRequest(dpy));
		new_win->damage = XDamageCreate(dpy, id, XDamageReportRawRectangles);
	}
	new_win->opacity = OPAQUE;

	if ( useXRes == true )
	{
		new_win->pid = get_win_pid(dpy, id);
	}
	else
	{
		new_win->pid = -1;
	}

	new_win->isOverlay = false;
	new_win->isSteam = false;
	new_win->isSteamStreamingClient = false;
	new_win->isSteamStreamingClientVideo = false;
	new_win->inputFocusMode = 0;

	if ( steamMode == true )
	{
		if ( new_win->pid != -1 )
		{
			new_win->appID = get_appid_from_pid( new_win->pid );
		}
		else
		{
			new_win->appID = 0;
		}
	}
	else
	{
		new_win->appID = id;
	}

	Window transientFor = None;
	if ( XGetTransientForHint( dpy, id, &transientFor ) )
	{
		new_win->transientFor = transientFor;
	}
	else
	{
		new_win->transientFor = None;
	}

	new_win->title = NULL;
	new_win->utf8_title = false;

	new_win->isFullscreen = false;
	new_win->isSysTrayIcon = false;
	new_win->sizeHintsSpecified = false;
	new_win->skipTaskbar = false;
	new_win->skipPager = false;
	new_win->requestedWidth = 0;
	new_win->requestedHeight = 0;
	new_win->nudged = false;
	new_win->ignoreOverrideRedirect = false;

	new_win->mouseMoved = 0;

	wlserver_surface_init( &new_win->surface, id );

	new_win->next = *p;
	*p = new_win;
	if (new_win->a.map_state == IsViewable)
		map_win(dpy, id, sequence);

	focusDirty = true;
}

static void
restack_win(Display *dpy, win *w, Window new_above)
{
	Window  old_above;

	if (w->next)
		old_above = w->next->id;
	else
		old_above = None;
	if (old_above != new_above)
	{
		win **prev;

		/* unhook */
		for (prev = &list; *prev; prev = &(*prev)->next)
		{
			if ((*prev) == w)
				break;
		}
		*prev = w->next;

		/* rehook */
		for (prev = &list; *prev; prev = &(*prev)->next)
		{
			if ((*prev)->id == new_above)
				break;
		}
		w->next = *prev;
		*prev = w;

		focusDirty = true;
	}
}

static void
configure_win(Display *dpy, XConfigureEvent *ce)
{
	win		    *w = find_win(dpy, ce->window);

	if (!w || w->id != ce->window)
	{
		if (ce->window == root)
		{
			root_width = ce->width;
			root_height = ce->height;
		}
		return;
	}

	w->a.x = ce->x;
	w->a.y = ce->y;
	w->a.width = ce->width;
	w->a.height = ce->height;
	w->a.border_width = ce->border_width;
	w->a.override_redirect = ce->override_redirect;
	restack_win(dpy, w, ce->above);

	focusDirty = true;
}

static void
circulate_win(Display *dpy, XCirculateEvent *ce)
{
	win	    *w = find_win(dpy, ce->window);
	Window  new_above;

	if (!w || w->id != ce->window)
		return;

	if (ce->place == PlaceOnTop)
		new_above = list->id;
	else
		new_above = None;
	restack_win(dpy, w, new_above);
	clipChanged = true;
}

static void map_request(Display *dpy, XMapRequestEvent *mapRequest)
{
	XMapWindow( dpy, mapRequest->window );
}

static void configure_request(Display *dpy, XConfigureRequestEvent *configureRequest)
{
	XWindowChanges changes =
	{
		.x = configureRequest->x,
		.y = configureRequest->y,
		.width = configureRequest->width,
		.height = configureRequest->height,
		.border_width = configureRequest->border_width,
		.sibling = configureRequest->above,
		.stack_mode = configureRequest->detail
	};

	XConfigureWindow( dpy, configureRequest->window, configureRequest->value_mask, &changes );
}

static void circulate_request( Display *dpy, XCirculateRequestEvent *circulateRequest )
{
	XCirculateSubwindows( dpy, circulateRequest->window, circulateRequest->place );
}

static void
finish_destroy_win(Display *dpy, Window id, bool gone)
{
	win	**prev, *w;

	for (prev = &list; (w = *prev); prev = &w->next)
		if (w->id == id)
		{
			if (gone)
				finish_unmap_win (dpy, w);
			*prev = w->next;
			if (w->damage != None)
			{
				set_ignore(dpy, NextRequest(dpy));
				XDamageDestroy(dpy, w->damage);
				w->damage = None;
			}

			if (gone)
			{
				// release all commits now we are closed.
				for ( commit_t& commit : w->commit_queue )
					release_commit( commit );
			}

			wlserver_lock();
			wlserver_surface_finish( &w->surface );
			wlserver_unlock();

			free(w->title);
			delete w;
			break;
		}
}

static void
destroy_win(Display *dpy, Window id, bool gone, bool fade)
{
	if (currentFocusWindow == id && gone)
	{
		currentFocusWindow = None;
		currentFocusWin = nullptr;
	}
	if (currentFadeWindow == id && gone)
		currentFadeWindow = None;
	if (currentInputFocusWindow == id && gone)
		currentInputFocusWindow = None;
	if (currentOverlayWindow == id && gone)
		currentOverlayWindow = None;
	if (currentNotificationWindow == id && gone)
		currentNotificationWindow = None;
	if (currentOverrideWindow == id && gone)
		currentOverrideWindow = None;
	if (currentKeyboardFocusWindow == id && gone)
		currentKeyboardFocusWindow = None;
	focusDirty = true;

	finish_destroy_win(dpy, id, gone);
}

static void
damage_win(Display *dpy, XDamageNotifyEvent *de)
{
	win	*w = find_win(dpy, de->drawable);
	win *focus = find_win(dpy, currentFocusWindow);

	if (!w)
		return;

	if (w->isOverlay && !w->opacity)
		return;

	// First damage event we get, compute focus; we only want to focus damaged
	// windows to have meaningful frames.
	if (w->appID && w->damage_sequence == 0)
		focusDirty = true;

	w->damage_sequence = damageSequence++;

	// If we just passed the focused window, we might be eliglible to take over
	if ( focus && focus != w && w->appID &&
		w->damage_sequence > focus->damage_sequence)
		focusDirty = true;

	// Josh: This will sometimes cause a BadDamage error.
	// I looked around at different compositors to see what
	// they do here and they just seem to ignore it.
	if (w->damage)
	{
		set_ignore(dpy, NextRequest(dpy));
		XDamageSubtract(dpy, w->damage, None, None);
	}

	gpuvis_trace_printf( "damage_win win %lx appID %u", w->id, w->appID );
}

static void
handle_wl_surface_id(win *w, long surfaceID)
{
	struct wlr_surface *surface = NULL;

	wlserver_lock();

	wlserver_surface_set_wl_id( &w->surface, surfaceID );

	surface = w->surface.wlr;
	if ( surface == NULL )
	{
		wlserver_unlock();
		return;
	}

	// If we already focused on our side and are handling this late,
	// let wayland know now.
	if ( w->id == currentInputFocusWindow )
		wlserver_mousefocus( surface );

	win *inputFocusWin = find_win( nullptr, currentInputFocusWindow );
	Window keyboardFocusWindow = currentInputFocusWindow;

	if ( inputFocusWin && inputFocusWin->inputFocusMode )
		keyboardFocusWindow = currentFocusWindow;

	if ( w->id == keyboardFocusWindow )
		wlserver_keyboardfocus( surface );

	// Pull the first buffer out of that window, if needed
	xwayland_surface_role_commit( surface );

	wlserver_unlock();
}

static void
update_net_wm_state(uint32_t action, bool *value)
{
	switch (action) {
	case NET_WM_STATE_REMOVE:
		*value = false;
		break;
	case NET_WM_STATE_ADD:
		*value = true;
		break;
	case NET_WM_STATE_TOGGLE:
		*value = !*value;
		break;
	default:
		xwm_log.debugf("Unknown NET_WM_STATE action: %" PRIu32, action);
	}
}

static void
handle_net_wm_state(Display *dpy, win *w, XClientMessageEvent *ev)
{
	uint32_t action = (uint32_t)ev->data.l[0];
	Atom *props = (Atom *)&ev->data.l[1];
	for (size_t i = 0; i < 2; i++) {
		if (props[i] == netWMStateFullscreenAtom) {
			update_net_wm_state(action, &w->isFullscreen);
			focusDirty = true;
		} else if (props[i] == netWMStateSkipTaskbarAtom) {
			update_net_wm_state(action, &w->skipTaskbar);
			focusDirty = true;
		} else if (props[i] == netWMStateSkipPagerAtom) {
			update_net_wm_state(action, &w->skipPager);
			focusDirty = true;
		} else if (props[i] != None) {
			xwm_log.debugf("Unhandled NET_WM_STATE property change: %s", XGetAtomName(dpy, props[i]));
		}
	}
}

static void
handle_system_tray_opcode(Display *dpy, XClientMessageEvent *ev)
{
	long opcode = ev->data.l[1];

	switch (opcode) {
		case SYSTEM_TRAY_REQUEST_DOCK: {
			Window embed_id = ev->data.l[2];

			/* At this point we're supposed to initiate the XEmbed lifecycle by
			 * sending XEMBED_EMBEDDED_NOTIFY. However we don't actually need to
			 * render the systray, we just want to recognize and blacklist these
			 * icons. So for now do nothing. */

			win *w = find_win(dpy, embed_id);
			if (w) {
				w->isSysTrayIcon = true;
			}
			break;
		}
		default:
			xwm_log.debugf("Unhandled _NET_SYSTEM_TRAY_OPCODE %ld", opcode);
	}
}

/* See http://tronche.com/gui/x/icccm/sec-4.html#s-4.1.4 */
static void
handle_wm_change_state(Display *dpy, win *w, XClientMessageEvent *ev)
{
	long state = ev->data.l[0];

	if (state == ICCCM_ICONIC_STATE) {
		/* Wine will request iconic state and cannot ensure that the WM has
		 * agreed on it; immediately revert to normal state to avoid being
		 * stuck in a paused state. */
		xwm_log.debugf("Rejecting WM_CHANGE_STATE to ICONIC for window 0x%lx", w->id);
		uint32_t wmState[] = { ICCCM_NORMAL_STATE, None };
		XChangeProperty(dpy, w->id, WMStateAtom, WMStateAtom, 32,
			PropModeReplace, (unsigned char *)wmState,
			sizeof(wmState) / sizeof(wmState[0]));
	} else {
		xwm_log.debugf("Unhandled WM_CHANGE_STATE to %ld for window 0x%lx", state, w->id);
	}
}

static void
handle_client_message(Display *dpy, XClientMessageEvent *ev)
{
	if (ev->window == ourWindow && ev->message_type == netSystemTrayOpcodeAtom)
	{
		handle_system_tray_opcode( dpy, ev );
		return;
	}

	win *w = find_win(dpy, ev->window);
	if (w)
	{
		if (ev->message_type == WLSurfaceIDAtom)
		{
			handle_wl_surface_id( w, ev->data.l[0]);
		}
		else if ( ev->message_type == activeWindowAtom )
		{
			XRaiseWindow( dpy, w->id );
		}
		else if ( ev->message_type == netWMStateAtom )
		{
			handle_net_wm_state( dpy, w, ev );
		}
		else if ( ev->message_type == WMChangeStateAtom )
		{
			handle_wm_change_state( dpy, w, ev );
		}
		else if ( ev->message_type != 0 )
		{
			xwm_log.debugf( "Unhandled client message: %s", XGetAtomName( dpy, ev->message_type ) );
		}
	}
}

static void
handle_property_notify(Display *dpy, XPropertyEvent *ev)
{
	/* check if Trans property was changed */
	if (ev->atom == opacityAtom)
	{
		/* reset mode and redraw window */
		win * w = find_win(dpy, ev->window);
		if ( w != nullptr )
		{
			unsigned int newOpacity = get_prop(dpy, w->id, opacityAtom, OPAQUE);

			if (newOpacity != w->opacity)
			{
				w->opacity = newOpacity;

				if ( gameFocused && ( w->id == currentOverlayWindow || w->id == currentNotificationWindow ) )
				{
					hasRepaint = true;
				}
			}

			unsigned int maxOpacity = 0;

			for (w = list; w; w = w->next)
			{
				if (w->isOverlay)
				{
					if (w->a.width > 1200 && w->opacity >= maxOpacity)
					{
						currentOverlayWindow = w->id;
						maxOpacity = w->opacity;
					}
				}
			}
		}
	}
	if (ev->atom == steamAtom)
	{
		win * w = find_win(dpy, ev->window);
		if (w)
		{
			w->isSteam = get_prop(dpy, w->id, steamAtom, 0);
			focusDirty = true;
		}
	}
	if (ev->atom == steamInputFocusAtom )
	{
		win * w = find_win(dpy, ev->window);
		if (w)
		{
			w->inputFocusMode = get_prop(dpy, w->id, steamInputFocusAtom, 0);
			focusDirty = true;
		}
	}
	if (ev->atom == steamTouchClickModeAtom )
	{
		g_nTouchClickMode = (enum wlserver_touch_click_mode) get_prop(dpy, root, steamTouchClickModeAtom, g_nDefaultTouchClickMode );
	}
	if (ev->atom == steamStreamingClientAtom)
	{
		win * w = find_win(dpy, ev->window);
		if (w)
		{
			w->isSteamStreamingClient = get_prop(dpy, w->id, steamStreamingClientAtom, 0);
			focusDirty = true;
		}
	}
	if (ev->atom == steamStreamingClientVideoAtom)
	{
		win * w = find_win(dpy, ev->window);
		if (w)
		{
			w->isSteamStreamingClientVideo = get_prop(dpy, w->id, steamStreamingClientVideoAtom, 0);
			focusDirty = true;
		}
	}
	if (ev->atom == gamescopeCtrlAppIDAtom )
	{
		get_prop( dpy, root, gamescopeCtrlAppIDAtom, vecFocuscontrolAppIDs );
		focusDirty = true;
	}
	if (ev->atom == gamescopeCtrlWindowAtom )
	{
		focusControlWindow = get_prop( dpy, root, gamescopeCtrlWindowAtom, None );
		focusDirty = true;
	}
	if ( ev->atom == gamescopeScreenShotAtom )
	{
		if ( ev->state == PropertyNewValue )
		{
			g_bTakeScreenshot = true;
			g_bPropertyRequestedScreenshot = true;
			XDeleteProperty( dpy, root, gamescopeScreenShotAtom );
		}
	}
	if (ev->atom == gameAtom)
	{
		win * w = find_win(dpy, ev->window);
		if (w)
		{
			uint32_t appID = get_prop(dpy, w->id, gameAtom, 0);

			if ( w->appID != 0 && appID != 0 && w->appID != appID )
			{
				xwm_log.errorf( "appid clash was %u now %u", w->appID, appID );
			}
			w->appID = appID;

			focusDirty = true;
		}
	}
	if (ev->atom == overlayAtom)
	{
		win * w = find_win(dpy, ev->window);
		if (w)
		{
			w->isOverlay = get_prop(dpy, w->id, overlayAtom, 0);
			focusDirty = true;
		}
	}
	if (ev->atom == sizeHintsAtom)
	{
		win * w = find_win(dpy, ev->window);
		if (w)
		{
			get_size_hints(dpy, w);
			focusDirty = true;
		}
	}
	if (ev->atom == gamesRunningAtom)
	{
		gamesRunningCount = get_prop(dpy, root, gamesRunningAtom, 0);

		focusDirty = true;
	}
	if (ev->atom == screenScaleAtom)
	{
		overscanScaleRatio = get_prop(dpy, root, screenScaleAtom, 0xFFFFFFFF) / (double)0xFFFFFFFF;

		globalScaleRatio = overscanScaleRatio * zoomScaleRatio;

		win *w;

		if ((w = find_win(dpy, currentFocusWindow)))
		{
			hasRepaint = true;
		}

		focusDirty = true;
	}
	if (ev->atom == screenZoomAtom)
	{
		zoomScaleRatio = get_prop(dpy, root, screenZoomAtom, 0xFFFF) / (double)0xFFFF;

		globalScaleRatio = overscanScaleRatio * zoomScaleRatio;

		win *w;

		if ((w = find_win(dpy, currentFocusWindow)))
		{
			hasRepaint = true;
		}

		focusDirty = true;
	}
	if (ev->atom == WMTransientForAtom)
	{
		win * w = find_win(dpy, ev->window);
		if (w)
		{
			Window transientFor = None;
			if ( XGetTransientForHint( dpy, ev->window, &transientFor ) )
			{
				w->transientFor = transientFor;
			}
			else
			{
				w->transientFor = None;
			}
			focusDirty = true;
		}
	}
	if (ev->atom == XA_WM_NAME || ev->atom == netWMNameAtom)
	{
		win *w = find_win(dpy, ev->window);
		if (w) {
			get_win_title(dpy, w, ev->atom);
		}
	}
}

static int
error(Display *dpy, XErrorEvent *ev)
{
	int	    o;
	const char    *name = NULL;
	static char buffer[256];

	if (should_ignore(dpy, ev->serial))
		return 0;

	if (ev->request_code == composite_opcode &&
		ev->minor_code == X_CompositeRedirectSubwindows)
	{
		xwm_log.errorf("Another composite manager is already running");
		exit(1);
	}

	o = ev->error_code - xfixes_error;
	switch (o) {
		case BadRegion: name = "BadRegion";	break;
		default: break;
	}
	o = ev->error_code - damage_error;
	switch (o) {
		case BadDamage: name = "BadDamage";	break;
		default: break;
	}
	o = ev->error_code - render_error;
	switch (o) {
		case BadPictFormat: name ="BadPictFormat"; break;
		case BadPicture: name ="BadPicture"; break;
		case BadPictOp: name ="BadPictOp"; break;
		case BadGlyphSet: name ="BadGlyphSet"; break;
		case BadGlyph: name ="BadGlyph"; break;
		default: break;
	}

	if (name == NULL)
	{
		buffer[0] = '\0';
		XGetErrorText(dpy, ev->error_code, buffer, sizeof(buffer));
		name = buffer;
	}

	xwm_log.errorf("error %d: %s request %d minor %d serial %lu",
			 ev->error_code, (strlen(name) > 0) ? name : "unknown",
			 ev->request_code, ev->minor_code, ev->serial);

	gotXError = true;
	/*    abort();	    this is just annoying to most people */
	return 0;
}

static int
handle_io_error(Display *dpy)
{
	xwm_log.errorf("X11 I/O error");

	imageWaitThreadRun = false;
	waitListSem.signal();

	if ( statsThreadRun == true )
	{
		statsThreadRun = false;
		statsThreadSem.signal();
	}

	pthread_exit(NULL);
}

static bool
register_cm(Display *dpy)
{
	Window w;
	Atom a;
	static char net_wm_cm[] = "_NET_WM_CM_Sxx";

	snprintf(net_wm_cm, sizeof(net_wm_cm), "_NET_WM_CM_S%d", scr);
	a = XInternAtom(dpy, net_wm_cm, false);

	w = XGetSelectionOwner(dpy, a);
	if (w != None)
	{
		XTextProperty tp;
		char **strs;
		int count;
		Atom winNameAtom = XInternAtom(dpy, "_NET_WM_NAME", false);

		if (!XGetTextProperty(dpy, w, &tp, winNameAtom) &&
			!XGetTextProperty(dpy, w, &tp, XA_WM_NAME))
		{
			xwm_log.errorf("Another composite manager is already running (0x%lx)", (unsigned long) w);
			return false;
		}
		if (XmbTextPropertyToTextList(dpy, &tp, &strs, &count) == Success)
		{
			xwm_log.errorf("Another composite manager is already running (%s)", strs[0]);

			XFreeStringList(strs);
		}

		XFree(tp.value);

		return false;
	}

	w = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 0, 0, 1, 1, 0, None,
							 None);

	Xutf8SetWMProperties(dpy, w, "steamcompmgr", "steamcompmgr", NULL, 0, NULL, NULL,
						  NULL);

	Atom atomWmCheck = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", false);
	XChangeProperty(dpy, root, atomWmCheck,
					XA_WINDOW, 32, PropModeReplace, (unsigned char *)&w, 1);
	XChangeProperty(dpy, w, atomWmCheck,
					XA_WINDOW, 32, PropModeReplace, (unsigned char *)&w, 1);


	Atom supportedAtoms[] = {
		XInternAtom(dpy, "_NET_WM_STATE", false),
		XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", false),
		XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", false),
		XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", false),
		XInternAtom(dpy, "_NET_ACTIVE_WINDOW", false),
	};

	XChangeProperty(dpy, root, XInternAtom(dpy, "_NET_SUPPORTED", false),
					XA_ATOM, 32, PropModeAppend, (unsigned char *)supportedAtoms,
					sizeof(supportedAtoms) / sizeof(supportedAtoms[0]));

	XSetSelectionOwner(dpy, a, w, 0);

	ourWindow = w;

	return true;
}

static void
register_systray(Display *dpy)
{
	static char net_system_tray_name[] = "_NET_SYSTEM_TRAY_Sxx";

	snprintf(net_system_tray_name, sizeof(net_system_tray_name),
			 "_NET_SYSTEM_TRAY_S%d", scr);
	Atom net_system_tray = XInternAtom(dpy, net_system_tray_name, false);

	XSetSelectionOwner(dpy, net_system_tray, ourWindow, 0);
}

void handle_done_commits( void )
{
	std::lock_guard<std::mutex> lock( listCommitsDoneLock );

	// very fast loop yes
	for ( uint32_t i = 0; i < listCommitsDone.size(); i++ )
	{
		bool bFoundWindow = false;
		for ( win *w = list; w; w = w->next )
		{
			uint32_t j;
			for ( j = 0; j < w->commit_queue.size(); j++ )
			{
				if ( w->commit_queue[ j ].commitID == listCommitsDone[ i ] )
				{
					gpuvis_trace_printf( "commit %lu done", w->commit_queue[ j ].commitID );
					w->commit_queue[ j ].done = true;
					bFoundWindow = true;

					// Window just got a new available commit, determine if that's worth a repaint

					// If this is an overlay that we're presenting, repaint
					if ( gameFocused )
					{
						if ( w->id == currentOverlayWindow && w->opacity != TRANSLUCENT )
						{
							hasRepaint = true;
						}

						if ( w->id == currentNotificationWindow && w->opacity != TRANSLUCENT )
						{
							hasRepaint = true;
						}
					}

					// If this is the main plane, repaint
					if ( w->id == currentFocusWindow )
					{
						if ( g_HeldCommits[ HELD_COMMIT_BASE ].done )
							release_commit( g_HeldCommits[ HELD_COMMIT_BASE ] );
					
						ref_commit( w->commit_queue[ j ] );
						g_HeldCommits[ HELD_COMMIT_BASE ] = w->commit_queue[ j ];
						hasRepaint = true;
					}

					if ( w->id == currentOverrideWindow )
					{
						frame_timing();
						hasRepaint = true;
					}

					if ( w->isSteamStreamingClientVideo && currentFocusWin && currentFocusWin->isSteamStreamingClient )
					{
						hasRepaint = true;
					}

					break;
				}
			}

			if ( bFoundWindow == true )
			{
				if ( j > 0 )
				{
					// we can release all commits prior to done ones
					for ( uint32_t k = 0; k < j; k++ )
					{
						release_commit( w->commit_queue[ k ] );
					}
					w->commit_queue.erase( w->commit_queue.begin(), w->commit_queue.begin() + j );
				}
				break;
			}
		}
	}

	listCommitsDone.clear();
}

void nudge_steamcompmgr( void )
{
	if ( write( g_nudgePipe[ 1 ], "\n", 1 ) < 0 )
		xwm_log.errorf_errno( "nudge_steamcompmgr: write failed" );
}

void take_screenshot( void )
{
	g_bTakeScreenshot = true;
	nudge_steamcompmgr();
}

void check_new_wayland_res( void )
{
	// When importing buffer, we'll potentially need to perform operations with
	// a wlserver lock (e.g. wlr_buffer_lock). We can't do this with a
	// wayland_commit_queue lock because that causes deadlocks.
	std::vector<ResListEntry_t> tmp_queue;
	{
		std::lock_guard<std::mutex> lock( wayland_commit_lock );
		tmp_queue = wayland_commit_queue;
		wayland_commit_queue.clear();
	}

	for ( uint32_t i = 0; i < tmp_queue.size(); i++ )
	{
		struct wlr_buffer *buf = tmp_queue[ i ].buf;

		win	*w = find_win( tmp_queue[ i ].surf );

		if ( w == nullptr )
		{
			wlserver_lock();
			wlr_buffer_unlock( buf );
			wlserver_unlock();
			xwm_log.errorf( "waylandres but no win" );
			continue;
		}

		commit_t newCommit = {};
		bool bSuccess = import_commit( buf, newCommit );

		int fence = -1;
		if ( bSuccess == true )
		{
			struct wlr_dmabuf_attributes dmabuf = {0};
			if ( wlr_buffer_get_dmabuf( buf, &dmabuf ) )
			{
				fence = dup( dmabuf.fd[0] );
			}
			else
			{
				fence = vulkan_texture_get_fence( newCommit.vulkanTex );
			}

			newCommit.commitID = ++maxCommmitID;
			ref_commit( newCommit );
			w->commit_queue.push_back( newCommit );
		}

		gpuvis_trace_printf( "pushing wait for commit %lu win %lx", newCommit.commitID, w->id );
		{
			std::unique_lock< std::mutex > lock( waitListLock );

			waitList.push_back( std::make_pair( fence, newCommit.commitID ) );
		}

		// Wake up commit wait thread if chilling
		waitListSem.signal();
	}
}

static void
spawn_client( char **argv )
{
	// (Don't Lose) The Children
	prctl( PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0 );

	std::string strNewPreload;
	char *pchPreloadCopy = nullptr;
	const char *pchCurrentPreload = getenv( "LD_PRELOAD" );
	bool bFirst = true;

	if ( pchCurrentPreload != nullptr )
	{
		pchPreloadCopy = strdup( pchCurrentPreload );

		// First replace all the separators in our copy with terminators
		for ( uint32_t i = 0; i < strlen( pchCurrentPreload ); i++ )
		{
			if ( pchPreloadCopy[ i ] == ' ' || pchPreloadCopy[ i ] == ':' )
			{
				pchPreloadCopy[ i ] = '\0';
			}
		}

		// Then walk it again and find all the substrings
		uint32_t i = 0;
		while ( i < strlen( pchCurrentPreload ) )
		{
			// If there's a string and it's not gameoverlayrenderer, append it to our new LD_PRELOAD
			if ( pchPreloadCopy[ i ] != '\0' )
			{
				if ( strstr( pchPreloadCopy + i, "gameoverlayrenderer.so" ) == nullptr )
				{
					if ( bFirst == false )
					{
						strNewPreload.append( ":" );
					}
					else
					{
						bFirst = false;
					}

					strNewPreload.append( pchPreloadCopy + i );
				}

				i += strlen( pchPreloadCopy + i );
			}
			else
			{
				i++;
			}
		}

		free( pchPreloadCopy );
	}

	pid_t pid = fork();

	if ( pid < 0 )
		xwm_log.errorf_errno( "fork failed" );

	// Are we in the child?
	if ( pid == 0 )
	{
		// Try to snap back to old priority
		if ( g_bNiceCap == true )
		{
			nice( g_nOldNice - g_nNewNice );
		}

		// Set modified LD_PRELOAD if needed
		if ( pchCurrentPreload != nullptr )
		{
			if ( strNewPreload.empty() == false )
			{
				setenv( "LD_PRELOAD", strNewPreload.c_str(), 1 );
			}
			else
			{
				unsetenv( "LD_PRELOAD" );
			}
		}

		unsetenv( "ENABLE_VKBASALT" );

		execvp( argv[ 0 ], argv );

		xwm_log.errorf_errno( "execvp failed" );
		_exit( 1 );
	}

	std::thread waitThread([]() {
		pthread_setname_np( pthread_self(), "gamescope-wait" );

		// Because we've set PR_SET_CHILD_SUBREAPER above, we'll get process
		// status notifications for all of our child processes, even if our
		// direct child exits. Wait until all have exited.
		while ( true )
		{
			if ( wait( nullptr ) < 0 )
			{
				if ( errno == EINTR )
					continue;
				if ( errno != ECHILD )
					xwm_log.errorf_errno( "steamcompmgr: wait failed" );
				break;
			}
		}

		g_bRun = false;
		nudge_steamcompmgr();
	});

	waitThread.detach();
}

static void
dispatch_x11( Display *dpy, MouseCursor *cursor )
{
	bool bShouldResetCursor = false;
	bool bSetFocus = false;

	do {
		XEvent ev;
		int ret = XNextEvent(dpy, &ev);
		if (ret != 0)
		{
			xwm_log.errorf("XNextEvent failed");
			break;
		}
		if ((ev.type & 0x7f) != KeymapNotify)
			discard_ignore(dpy, ev.xany.serial);
		if (debugEvents)
		{
			gpuvis_trace_printf("event %d", ev.type);
			printf("event %d\n", ev.type);
		}
		switch (ev.type) {
			case CreateNotify:
				if (ev.xcreatewindow.parent == root)
					add_win(dpy, ev.xcreatewindow.window, 0, ev.xcreatewindow.serial);
				break;
			case ConfigureNotify:
				configure_win(dpy, &ev.xconfigure);
				break;
			case DestroyNotify:
			{
				win * w = find_win(dpy, ev.xdestroywindow.window);

				if (w && w->id == ev.xdestroywindow.window)
					destroy_win(dpy, ev.xdestroywindow.window, true, true);
				break;
			}
			case MapNotify:
			{
				win * w = find_win(dpy, ev.xmap.window);

				if (w && w->id == ev.xmap.window)
					map_win(dpy, ev.xmap.window, ev.xmap.serial);
				break;
			}
			case UnmapNotify:
			{
				win * w = find_win(dpy, ev.xunmap.window);

				if (w && w->id == ev.xunmap.window)
					unmap_win(dpy, ev.xunmap.window, true);
				break;
			}
			case FocusOut:
			{
				win * w = find_win( dpy, ev.xfocus.window );

				// If focus escaped the current desired keyboard focus window, check where it went
				if ( w && w->id == currentKeyboardFocusWindow )
				{
					Window newKeyboardFocus = None;
					int nRevertMode = 0;
					XGetInputFocus( dpy, &newKeyboardFocus, &nRevertMode );

					// Find window or its toplevel parent
					win *kbw = find_win( dpy, newKeyboardFocus );

					if ( kbw )
					{
						if ( kbw->id == currentKeyboardFocusWindow )
						{
							// focus went to a child, this is fine, make note of it in case we need to fix it
							currentKeyboardFocusWindow = newKeyboardFocus;
						}
						else
						{
							// focus went elsewhere, correct it
							bSetFocus = true;
						}
					}
				}

				break;
			}
			case ReparentNotify:
				if (ev.xreparent.parent == root)
					add_win(dpy, ev.xreparent.window, 0, ev.xreparent.serial);
				else
				{
					win * w = find_win(dpy, ev.xreparent.window);

					if (w && w->id == ev.xreparent.window)
					{
						destroy_win(dpy, ev.xreparent.window, false, true);
					}
					else
					{
						// If something got reparented _to_ a toplevel window,
						// go check for the fullscreen workaround again.
						w = find_win(dpy, ev.xreparent.parent);
						if (w)
						{
							get_size_hints(dpy, w);
							focusDirty = true;
						}
					}
				}
				break;
			case CirculateNotify:
				circulate_win(dpy, &ev.xcirculate);
				break;
			case MapRequest:
				map_request(dpy, &ev.xmaprequest);
				break;
			case ConfigureRequest:
				configure_request(dpy, &ev.xconfigurerequest);
				break;
			case CirculateRequest:
				circulate_request(dpy, &ev.xcirculaterequest);
				break;
			case Expose:
				break;
			case PropertyNotify:
				handle_property_notify(dpy, &ev.xproperty);
				break;
			case ClientMessage:
				handle_client_message(dpy, &ev.xclient);
				break;
			case LeaveNotify:
				if (ev.xcrossing.window == currentInputFocusWindow)
				{
					// Josh: need to defer this as we could have a destroy later on
					// and end up submitting commands with the currentInputFocusWIndow
					bShouldResetCursor = true;
				}
				break;
			case MotionNotify:
				{
					win * w = find_win(dpy, ev.xmotion.window);
					if (w && w->id == currentInputFocusWindow)
					{
						cursor->move(ev.xmotion.x, ev.xmotion.y);
					}
					break;
				}
			default:
				if (ev.type == damage_event + XDamageNotify)
				{
					damage_win(dpy, (XDamageNotifyEvent *) &ev);
				}
				else if (ev.type == xfixes_event + XFixesCursorNotify)
				{
					cursor->setDirty();
				}
				break;
		}
		XFlush(dpy);
	} while (XPending(dpy));

	if ( bShouldResetCursor )
	{
		// This shouldn't happen due to our pointer barriers,
		// but there is a known X server bug; warp to last good
		// position.
		cursor->resetPosition();
	}

	if ( bSetFocus )
	{
		XSetInputFocus(dpy, currentKeyboardFocusWindow, RevertToNone, CurrentTime);
	}
}

static bool
dispatch_vblank( int fd )
{
	bool vblank = false;
	for (;;)
	{
		uint64_t vblanktime = 0;
		ssize_t ret = read( fd, &vblanktime, sizeof( vblanktime ) );
		if ( ret < 0 )
		{
			if ( errno == EAGAIN )
				break;

			xwm_log.errorf_errno( "steamcompmgr: dispatch_vblank: read failed" );
			break;
		}

		uint64_t diff = get_time_in_nanos() - vblanktime;

		// give it 1 ms of slack.. maybe too long
		if ( diff > 1'000'000ul )
		{
			gpuvis_trace_printf( "ignored stale vblank" );
		}
		else
		{
			gpuvis_trace_printf( "got vblank" );
			vblank = true;
		}
	}
	return vblank;
}

static void
dispatch_nudge( int fd )
{
	for (;;)
	{
		static char buf[1024];
		if ( read( fd, buf, sizeof(buf) ) < 0 )
		{
			if ( errno != EAGAIN )
				xwm_log.errorf_errno(" steamcompmgr: dispatch_nudge: read failed" );
			break;
		}
	}
}

struct rgba_t
{
	uint8_t r,g,b,a;
};

static bool
load_mouse_cursor( MouseCursor *cursor, const char *path, int hx, int hy )
{
	int w, h, channels;
	rgba_t *data = (rgba_t *) stbi_load(path, &w, &h, &channels, STBI_rgb_alpha);
	if (!data)
	{
		xwm_log.errorf("Failed to open/load cursor file");
		return false;
	}

	std::transform(data, data + w * h, data, [](rgba_t x) {
		if (x.a == 0)
			return rgba_t{};
		return rgba_t{
			uint8_t((x.b * x.a) / 255),
			uint8_t((x.g * x.a) / 255),
			uint8_t((x.r * x.a) / 255),
			x.a };
	});

	// Data is freed by XDestroyImage in setCursorImage.
	return cursor->setCursorImage((char *)data, w, h, hx, hy);
}

enum event_type {
	EVENT_X11,
	EVENT_VBLANK,
	EVENT_NUDGE,
	EVENT_COUNT // keep last
};

const char* g_customCursorPath = nullptr;
int g_customCursorHotspotX = 0;
int g_customCursorHotspotY = 0;

void
steamcompmgr_main(int argc, char **argv)
{
	Display	   *dpy;
	Window	    root_return, parent_return;
	Window	    *children;
	unsigned int    nchildren;
	int		    composite_major, composite_minor;
	int			xres_major, xres_minor;
	int		    o;
	int			readyPipeFD = -1;

	// Reset getopt() state
	optind = 1;

	int opt_index = -1;
	while ((o = getopt_long(argc, argv, gamescope_optstring, gamescope_options, &opt_index)) != -1)
	{
		const char *opt_name;
		switch (o) {
			case 'R':
				readyPipeFD = open( optarg, O_WRONLY | O_CLOEXEC );
				break;
			case 'T':
				statsThreadPath = optarg;
				{
					statsThreadRun = true;
					std::thread statsThreads( statsThreadMain );
					statsThreads.detach();
				}
				break;
			case 'C':
				cursorHideTime = atoi( optarg );
				break;
			case 'v':
				drawDebugInfo = true;
				break;
			case 'e':
				steamMode = true;
				break;
			case 'c':
				alwaysComposite = true;
				break;
			case 'x':
				useXRes = false;
				break;
			case 0: // long options without a short option
				opt_name = gamescope_options[opt_index].name;
				if (strcmp(opt_name, "debug-focus") == 0) {
					debugFocus = true;
				} else if (strcmp(opt_name, "synchronous-x11") == 0) {
					synchronize = true;
				} else if (strcmp(opt_name, "debug-events") == 0) {
					debugEvents = true;
				} else if (strcmp(opt_name, "cursor") == 0) {
					g_customCursorPath = optarg;
				} else if (strcmp(opt_name, "cursor-hotspot") == 0) {
					sscanf(optarg, "%d,%d", &g_customCursorHotspotX, &g_customCursorHotspotY);
				} else if (strcmp(opt_name, "fade-out-duration") == 0) {
					g_FadeOutDuration = atoi(optarg);
				}
				break;
			case '?':
				assert(false); // unreachable
		}
	}

	int subCommandArg = -1;
	if ( optind < argc )
	{
		subCommandArg = optind;
	}

	if ( pipe2( g_nudgePipe, O_CLOEXEC | O_NONBLOCK ) != 0 )
	{
		xwm_log.errorf_errno( "steamcompmgr: pipe2 failed" );
		exit( 1 );
	}

	const char *pchEnableVkBasalt = getenv( "ENABLE_VKBASALT" );
	if ( pchEnableVkBasalt != nullptr && pchEnableVkBasalt[0] == '1' )
	{
		alwaysComposite = true;
	}

	dpy = XOpenDisplay( wlserver_get_nested_display_name() );
	if (!dpy)
	{
		xwm_log.errorf("Can't open display");
		exit(1);
	}
	XSetErrorHandler(error);
	XSetIOErrorHandler(handle_io_error);
	if (synchronize)
		XSynchronize(dpy, 1);
	scr = DefaultScreen(dpy);
	root = RootWindow(dpy, scr);

	if (!XRenderQueryExtension(dpy, &render_event, &render_error))
	{
		xwm_log.errorf("No render extension");
		exit(1);
	}
	if (!XQueryExtension(dpy, COMPOSITE_NAME, &composite_opcode,
		&composite_event, &composite_error))
	{
		xwm_log.errorf("No composite extension");
		exit(1);
	}
	XCompositeQueryVersion(dpy, &composite_major, &composite_minor);

	if (!XDamageQueryExtension(dpy, &damage_event, &damage_error))
	{
		xwm_log.errorf("No damage extension");
		exit(1);
	}
	if (!XFixesQueryExtension(dpy, &xfixes_event, &xfixes_error))
	{
		xwm_log.errorf("No XFixes extension");
		exit(1);
	}
	if (!XShapeQueryExtension(dpy, &xshape_event, &xshape_error))
	{
		xwm_log.errorf("No XShape extension");
		exit(1);
	}
	if (!XFixesQueryExtension(dpy, &xfixes_event, &xfixes_error))
	{
		xwm_log.errorf("No XFixes extension");
		exit(1);
	}
	if (!XResQueryVersion(dpy, &xres_major, &xres_minor))
	{
		xwm_log.errorf("No XRes extension");
		exit(1);
	}
	if (xres_major != 1 || xres_minor < 2)
	{
		xwm_log.errorf("Unsupported XRes version: have %d.%d, want 1.2", xres_major, xres_minor);
		exit(1);
	}

	if (!register_cm(dpy))
	{
		exit(1);
	}

	register_systray(dpy);

	/* get atoms */
	steamAtom = XInternAtom(dpy, STEAM_PROP, false);
	steamInputFocusAtom = XInternAtom(dpy, "STEAM_INPUT_FOCUS", false);
	steamTouchClickModeAtom = XInternAtom(dpy, "STEAM_TOUCH_CLICK_MODE", false);
	gameAtom = XInternAtom(dpy, GAME_PROP, false);
	overlayAtom = XInternAtom(dpy, OVERLAY_PROP, false);
	opacityAtom = XInternAtom(dpy, OPACITY_PROP, false);
	gamesRunningAtom = XInternAtom(dpy, GAMES_RUNNING_PROP, false);
	screenScaleAtom = XInternAtom(dpy, SCREEN_SCALE_PROP, false);
	screenZoomAtom = XInternAtom(dpy, SCREEN_MAGNIFICATION_PROP, false);
	winTypeAtom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", false);
	winDesktopAtom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", false);
	winDockAtom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", false);
	winToolbarAtom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_TOOLBAR", false);
	winMenuAtom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_MENU", false);
	winUtilAtom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", false);
	winSplashAtom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_SPLASH", false);
	winDialogAtom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", false);
	winNormalAtom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NORMAL", false);
	sizeHintsAtom = XInternAtom(dpy, "WM_NORMAL_HINTS", false);
	netWMStateFullscreenAtom = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", false);
	activeWindowAtom = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", false);
	netWMStateAtom = XInternAtom(dpy, "_NET_WM_STATE", false);
	WMTransientForAtom = XInternAtom(dpy, "WM_TRANSIENT_FOR", false);
	netWMStateHiddenAtom = XInternAtom(dpy, "_NET_WM_STATE_HIDDEN", false);
	netWMStateFocusedAtom = XInternAtom(dpy, "_NET_WM_STATE_FOCUSED", false);
	netWMStateSkipTaskbarAtom = XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", false);
	netWMStateSkipPagerAtom = XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", false);
	WLSurfaceIDAtom = XInternAtom(dpy, "WL_SURFACE_ID", false);
	WMStateAtom = XInternAtom(dpy, "WM_STATE", false);
	utf8StringAtom = XInternAtom(dpy, "UTF8_STRING", false);
	netWMNameAtom = XInternAtom(dpy, "_NET_WM_NAME", false);
	netSystemTrayOpcodeAtom = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", false);
	steamStreamingClientAtom = XInternAtom(dpy, "STEAM_STREAMING_CLIENT", false);
	steamStreamingClientVideoAtom = XInternAtom(dpy, "STEAM_STREAMING_CLIENT_VIDEO", false);
	gamescopeFocusableAppsAtom = XInternAtom(dpy, "GAMESCOPE_FOCUSABLE_APPS", false);
	gamescopeFocusableWindowsAtom = XInternAtom(dpy, "GAMESCOPE_FOCUSABLE_WINDOWS", false);
	gamescopeFocusedAppAtom = XInternAtom( dpy, "GAMESCOPE_FOCUSED_APP", false );
	gamescopeFocusedWindowAtom = XInternAtom( dpy, "GAMESCOPE_FOCUSED_WINDOW", false );
	gamescopeCtrlAppIDAtom = XInternAtom(dpy, "GAMESCOPECTRL_BASELAYER_APPID", false);
	gamescopeCtrlWindowAtom = XInternAtom(dpy, "GAMESCOPECTRL_BASELAYER_WINDOW", false);
	WMChangeStateAtom = XInternAtom(dpy, "WM_CHANGE_STATE", false);
	gamescopeInputCounterAtom = XInternAtom(dpy, "GAMESCOPE_INPUT_COUNTER", false);
	gamescopeScreenShotAtom = XInternAtom( dpy, "GAMESCOPECTRL_REQUEST_SCREENSHOT", false );

	root_width = DisplayWidth(dpy, scr);
	root_height = DisplayHeight(dpy, scr);

	allDamage = None;
	clipChanged = true;

	int vblankFD = vblank_init();
	assert( vblankFD >= 0 );

	currentOutputWidth = g_nOutputWidth;
	currentOutputHeight = g_nOutputHeight;

	XGrabServer(dpy);

	XCompositeRedirectSubwindows(dpy, root, CompositeRedirectManual);

	XSelectInput(dpy, root,
				  SubstructureNotifyMask|
				  ExposureMask|
				  StructureNotifyMask|
				  SubstructureRedirectMask|
				  FocusChangeMask|
				  PointerMotionMask|
				  LeaveWindowMask|
				  PropertyChangeMask);
	XShapeSelectInput(dpy, root, ShapeNotifyMask);
	XFixesSelectCursorInput(dpy, root, XFixesDisplayCursorNotifyMask);
	XQueryTree(dpy, root, &root_return, &parent_return, &children, &nchildren);
	for (uint32_t i = 0; i < nchildren; i++)
		add_win(dpy, children[i], i ? children[i-1] : None, 0);
	XFree(children);

	XUngrabServer(dpy);

	XF86VidModeLockModeSwitch(dpy, scr, true);

	std::unique_ptr<MouseCursor> cursor(new MouseCursor(dpy));
	if (g_customCursorPath)
	{
		if (!load_mouse_cursor(cursor.get(), g_customCursorPath, g_customCursorHotspotX, g_customCursorHotspotY))
			xwm_log.errorf("Failed to load mouse cursor: %s", g_customCursorPath);
	}

	gamesRunningCount = get_prop(dpy, root, gamesRunningAtom, 0);
	overscanScaleRatio = get_prop(dpy, root, screenScaleAtom, 0xFFFFFFFF) / (double)0xFFFFFFFF;
	zoomScaleRatio = get_prop(dpy, root, screenZoomAtom, 0xFFFF) / (double)0xFFFF;

	globalScaleRatio = overscanScaleRatio * zoomScaleRatio;

	determine_and_apply_focus(dpy, cursor.get());

	if ( readyPipeFD != -1 )
	{
		dprintf( readyPipeFD, "%s %s\n", wlserver_get_nested_display_name(), wlserver_get_wl_display_name() );
		close( readyPipeFD );
		readyPipeFD = -1;
	}

	if ( subCommandArg >= 0 )
	{
		spawn_client( &argv[ subCommandArg ] );
	}

	std::thread imageWaitThread( imageWaitThreadMain );
	imageWaitThread.detach();

	struct pollfd pollfds[] = {
		[ EVENT_X11 ] = {
			.fd = XConnectionNumber( dpy ),
			.events = POLLIN,
		},
		[ EVENT_VBLANK ] = {
			.fd = vblankFD,
			.events = POLLIN,
		},
		[ EVENT_NUDGE ] = {
			.fd = g_nudgePipe[ 0 ],
			.events = POLLIN,
		},
	};

	for (;;)
	{
		focusDirty = false;
		bool vblank = false;

		if ( poll( pollfds, EVENT_COUNT, -1 ) < 0)
		{
			if ( errno == EAGAIN )
				continue;

			xwm_log.errorf_errno( "poll failed" );
			break;
		}

		if ( pollfds[ EVENT_X11 ].revents & POLLHUP )
		{
			xwm_log.errorf( "Lost connection to the X11 server" );
			break;
		}

		assert( !( pollfds[ EVENT_VBLANK ].revents & POLLHUP ) );
		assert( !( pollfds[ EVENT_NUDGE ].revents & POLLHUP ) );

		if ( pollfds[ EVENT_X11 ].revents & POLLIN )
			dispatch_x11( dpy, cursor.get() );
		if ( pollfds[ EVENT_VBLANK ].revents & POLLIN )
			vblank = dispatch_vblank( vblankFD );
		if ( pollfds[ EVENT_NUDGE ].revents & POLLIN )
			dispatch_nudge( g_nudgePipe[ 0 ] );

		if ( g_bRun == false )
		{
			break;
		}

		if ( inputCounter != lastPublishedInputCounter )
		{
			XChangeProperty( dpy, root, gamescopeInputCounterAtom, XA_CARDINAL, 32, PropModeReplace,
							 (unsigned char *)&inputCounter, 1 );

			lastPublishedInputCounter = inputCounter;
		}

		if (focusDirty == true)
		{
			determine_and_apply_focus(dpy, cursor.get());

			hasFocusWindow = currentFocusWindow != None;

			sdlwindow_pushupdate();
		}

		// If our DRM state is out-of-date, refresh it. This might update
		// the output size.
		if ( BIsNested() == false )
		{
			if ( drm_poll_state( &g_DRM ) )
				hasRepaint = true;
		}

		// Pick our width/height for this potential frame, regardless of how it might change later
		// At some point we might even add proper locking so we get real updates atomically instead
		// of whatever jumble of races the below might cause over a couple of frames
		if ( currentOutputWidth != g_nOutputWidth ||
			 currentOutputHeight != g_nOutputHeight )
		{
			if ( BIsNested() == true )
			{
				vulkan_remake_swapchain();

				while ( !acquire_next_image() )
					vulkan_remake_swapchain();
			}
			else
			{
				vulkan_remake_output_images();
			}

			currentOutputWidth = g_nOutputWidth;
			currentOutputHeight = g_nOutputHeight;

#if HAVE_PIPEWIRE
			nudge_pipewire();
#endif
		}

		handle_done_commits();

		check_new_wayland_res();

		if ( ( g_bTakeScreenshot == true || hasRepaint == true || is_fading_out() ) && vblank == true )
		{
			paint_all(dpy, cursor.get());

			// Consumed the need to repaint here
			hasRepaint = false;

			// If we're in the middle of a fade, pump an event into the loop to
			// make sure we keep pushing frames even if the app isn't updating.
			if ( is_fading_out() )
			{
				nudge_steamcompmgr();
			}
		}

		// TODO: Look into making this _RAW
		// wlroots, seems to just use normal MONOTONIC
		// all over so this may be problematic to just change.
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);

		cursor->updatePosition();

		// Ask for a new surface every vblank
		if ( vblank == true )
		{
			for (win *w = list; w; w = w->next)
			{
				if ( w->surface.wlr != nullptr )
				{
					// Acknowledge commit once.
					wlserver_lock();

					if ( w->surface.wlr != nullptr )
					{
						wlserver_send_frame_done(w->surface.wlr, &now);
					}

					wlserver_unlock();
				}
			}
		}

		vulkan_garbage_collect();

		vblank = false;
	}

	// Clean up any commits.

	for ( win *w = list; w; w = w->next )
	{
		for ( commit_t& commit : w->commit_queue )
			release_commit( commit );
	}

	if ( g_HeldCommits[ HELD_COMMIT_BASE ].done )
		release_commit( g_HeldCommits[ HELD_COMMIT_BASE ] );

	if ( g_HeldCommits[ HELD_COMMIT_FADE ].done )
		release_commit( g_HeldCommits[ HELD_COMMIT_FADE ] );

	imageWaitThreadRun = false;
	waitListSem.signal();

	if ( statsThreadRun == true )
	{
		statsThreadRun = false;
		statsThreadSem.signal();
	}

	finish_drm( &g_DRM );
}
