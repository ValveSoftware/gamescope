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

#include <X11/Xlib.h>
#include <cstdint>
#include <memory>
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
#include <queue>

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

#include "xwayland_ctx.hpp"

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

template< typename T >
constexpr const T& clamp( const T& x, const T& min, const T& max )
{
    return x < min ? min : max < x ? max : x;
}

struct ignore {
	struct ignore	*next;
	unsigned long	sequence;
};

struct commit_t
{
	commit_t()
	{
		static uint64_t maxCommmitID = 0;
		commitID = ++maxCommmitID;
	}
    ~commit_t()
    {
        if ( fb_id != 0 )
		{
			drm_unlock_fbid( &g_DRM, fb_id );
			fb_id = 0;
		}

		wlserver_lock();
		wlr_buffer_unlock( buf );
		wlserver_unlock();
    }

	struct wlr_buffer *buf = nullptr;
	uint32_t fb_id = 0;
	std::shared_ptr<CVulkanTexture> vulkanTex;
	uint64_t commitID = 0;
	bool done = false;
};

#define MWM_HINTS_FUNCTIONS   1
#define MWM_HINTS_DECORATIONS 2
#define MWM_HINTS_INPUT_MODE  4
#define MWM_HINTS_STATUS      8

#define MWM_FUNC_ALL          0x01
#define MWM_FUNC_RESIZE       0x02
#define MWM_FUNC_MOVE         0x04
#define MWM_FUNC_MINIMIZE     0x08
#define MWM_FUNC_MAXIMIZE     0x10
#define MWM_FUNC_CLOSE        0x20

#define MWM_DECOR_ALL         0x01
#define MWM_DECOR_BORDER      0x02
#define MWM_DECOR_RESIZEH     0x04
#define MWM_DECOR_TITLE       0x08
#define MWM_DECOR_MENU        0x10
#define MWM_DECOR_MINIMIZE    0x20
#define MWM_DECOR_MAXIMIZE    0x40

#define MWM_INPUT_MODELESS                  0
#define MWM_INPUT_PRIMARY_APPLICATION_MODAL 1
#define MWM_INPUT_SYSTEM_MODAL              2
#define MWM_INPUT_FULL_APPLICATION_MODAL    3
#define MWM_INPUT_APPLICATION_MODAL         1

#define MWM_TEAROFF_WINDOW 1

struct motif_hints_t
{
	unsigned long flags;
	unsigned long functions;
	unsigned long decorations;
	long input_mode;
	unsigned long status;
};

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
	bool isExternalOverlay;
	bool isFullscreen;
	bool isSysTrayIcon;
	bool sizeHintsSpecified;
	bool skipTaskbar;
	bool skipPager;
	unsigned int requestedWidth;
	unsigned int requestedHeight;
	bool is_dialog;
	bool maybe_a_dropdown;

	motif_hints_t *motif_hints;

	Window transientFor;

	bool nudged;
	bool ignoreOverrideRedirect;

	unsigned int mouseMoved;

	struct wlserver_surface surface;

	xwayland_ctx_t *ctx;

	std::vector< std::shared_ptr<commit_t> > commit_queue;
};

Window x11_win(win *w) {
	if (w == nullptr)
		return None;
	return w->id;
}

struct global_focus_t : public focus_t
{
	win	  	 		*keyboardFocusWindow;
	win	  	 		*fadeWindow;
	MouseCursor		*cursor;
} global_focus;


uint32_t		currentOutputWidth, currentOutputHeight;

bool hasFocusWindow;

std::vector< uint32_t > vecFocuscontrolAppIDs;

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

bool			synchronize;

std::mutex g_SteamCompMgrXWaylandServerMutex;

uint64_t g_SteamCompMgrVBlankTime = 0;

static int g_nSteamCompMgrTargetFPS = 0;
static uint64_t g_uDynamicRefreshEqualityTime = 0;
static int g_nDynamicRefreshRate = 0;
// Delay to stop modes flickering back and forth.
static const uint64_t g_uDynamicRefreshDelay = 600'000'000; // 600ms

static int g_nRuntimeInfoFd = -1;

bool g_bFSRActive = false;

BlurMode g_BlurMode = BLUR_MODE_OFF;
BlurMode g_BlurModeOld = BLUR_MODE_OFF;
unsigned int g_BlurFadeDuration = 0;
int g_BlurRadius = 5;
unsigned int g_BlurFadeStartTime = 0;

pid_t focusWindow_pid;

bool steamcompmgr_window_should_limit_fps( win *w )
{
	return w && !w->isSteam && w->appID != 769 && !w->isOverlay && !w->isExternalOverlay;
}


enum HeldCommitTypes_t
{
	HELD_COMMIT_BASE,
	HELD_COMMIT_FADE,

	HELD_COMMIT_COUNT,
};

std::array<std::shared_ptr<commit_t>, HELD_COMMIT_COUNT> g_HeldCommits;
bool g_bPendingFade = false;

/* opacity property name; sometime soon I'll write up an EWMH spec for it */
#define OPACITY_PROP		"_NET_WM_WINDOW_OPACITY"
#define GAME_PROP			"STEAM_GAME"
#define STEAM_PROP			"STEAM_BIGPICTURE"
#define OVERLAY_PROP		"STEAM_OVERLAY"
#define EXTERNAL_OVERLAY_PROP		"GAMESCOPE_EXTERNAL_OVERLAY"
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

struct wlr_buffer_map_entry {
	struct wl_listener listener;
	struct wlr_buffer *buf;
	std::shared_ptr<CVulkanTexture> vulkanTex;
	uint32_t fb_id;
};

static std::mutex wlr_buffer_map_lock;
static std::unordered_map<struct wlr_buffer*, wlr_buffer_map_entry> wlr_buffer_map;

static std::atomic< bool > g_bTakeScreenshot{false};
static bool g_bPropertyRequestedScreenshot;

static int g_nudgePipe[2] = {-1, -1};

static LogScope xwm_log("xwm");

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

struct WaitListEntry_t
{
	xwayland_ctx_t *ctx;
	int fence;
	// Josh: Whether or not to nudge mangoapp that we got
	// a frame as soon as we know this commit is done.
	// This could technically be out of date if we change windows
	// but for a max couple frames of inaccuracy when switching windows
	// compared to being all over the place from handling in the
	// steamcompmgr thread in handle_done_commits, it is worth it.
	bool mangoapp_nudge;
	uint64_t commitID;
};

sem waitListSem;
std::mutex waitListLock;
std::vector< WaitListEntry_t > waitList;

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
	WaitListEntry_t entry;

retry:
	{
		std::unique_lock< std::mutex > lock( waitListLock );

		if( waitList.empty() )
		{
			goto wait;
		}

		entry = waitList[ 0 ];
		bFound = true;
		waitList.erase( waitList.begin() );
	}

	assert( bFound == true );

	gpuvis_trace_begin_ctx_printf( entry.commitID, "wait fence" );
	struct pollfd fd = { entry.fence, POLLOUT, 0 };
	int ret = poll( &fd, 1, 100 );
	if ( ret < 0 )
	{
		xwm_log.errorf_errno( "failed to poll fence FD" );
	}
	gpuvis_trace_end_ctx_printf( entry.commitID, "wait fence" );

	close( entry.fence );

	uint64_t frametime;
	if ( entry.mangoapp_nudge )
	{
		uint64_t now = get_time_in_nanos();
		static uint64_t lastFrameTime = now;
		frametime = now - lastFrameTime;
		lastFrameTime = now;
	}

	{
		std::unique_lock< std::mutex > lock( entry.ctx->listCommitsDoneLock );
		entry.ctx->listCommitsDone.push_back( entry.commitID );
	}

	nudge_steamcompmgr();

	if ( entry.mangoapp_nudge )
		mangoapp_update( frametime, frametime, uint64_t(~0ull) );	

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

		if( statsEventQueue.empty() )
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
	// Kernel reports page flips with CLOCK_MONOTONIC.
	clock_gettime(CLOCK_MONOTONIC, &ts);
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
discard_ignore(xwayland_ctx_t *ctx, unsigned long sequence)
{
	while (ctx->ignore_head)
	{
		if ((long) (sequence - ctx->ignore_head->sequence) > 0)
		{
			ignore  *next = ctx->ignore_head->next;
			free(ctx->ignore_head);
			ctx->ignore_head = next;
			if (!ctx->ignore_head)
				ctx->ignore_tail = &ctx->ignore_head;
		}
		else
			break;
	}
}

static void
set_ignore(xwayland_ctx_t *ctx, unsigned long sequence)
{
	ignore  *i = (ignore *)malloc(sizeof(ignore));
	if (!i)
		return;
	i->sequence = sequence;
	i->next = NULL;
	*ctx->ignore_tail = i;
	ctx->ignore_tail = &i->next;
}

static int
should_ignore(xwayland_ctx_t *ctx, unsigned long sequence)
{
	discard_ignore(ctx, sequence);
	return ctx->ignore_head && ctx->ignore_head->sequence == sequence;
}

static bool
x_events_queued(xwayland_ctx_t* ctx)
{
	// If mode is QueuedAlready, XEventsQueued() returns the number of
	// events already in the event queue (and never performs a system call).
	return XEventsQueued(ctx->dpy, QueuedAlready) != 0;
}

static win *
find_win(xwayland_ctx_t *ctx, Window id, bool find_children = true)
{
	win	*w;

	if (id == None)
	{
		return NULL;
	}

	for (w = ctx->list; w; w = w->next)
	{
		if (w->id == id)
		{
			return w;
		}
	}

	if ( !find_children )
		return nullptr;

	// Didn't find, must be a children somewhere; try again with parent.
	Window root = None;
	Window parent = None;
	Window *children = NULL;
	unsigned int childrenCount;
	set_ignore(ctx, NextRequest(ctx->dpy));
	XQueryTree(ctx->dpy, id, &root, &parent, &children, &childrenCount);
	if (children)
		XFree(children);

	if (root == parent || parent == None)
	{
		return NULL;
	}

	return find_win(ctx, parent);
}

static win * find_win( xwayland_ctx_t *ctx, struct wlr_surface *surf )
{
	win	*w = nullptr;

	for (w = ctx->list; w; w = w->next)
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

static std::shared_ptr<commit_t>
import_commit ( struct wlr_buffer *buf )
{
	std::shared_ptr<commit_t> commit = std::make_shared<commit_t>();
	std::unique_lock<std::mutex> lock( wlr_buffer_map_lock );

	commit->buf = buf;

	auto it = wlr_buffer_map.find( buf );
	if ( it != wlr_buffer_map.end() )
	{
		commit->vulkanTex = it->second.vulkanTex;
		commit->fb_id = it->second.fb_id;

		/* Unlock here to avoid deadlock [1],
		 * drm_lock_fbid calls wlserver_lock.
		 * Map is no longer used here and the element 
		 * is no longer accessed. */
		lock.unlock();

		if (commit->fb_id)
		{
			drm_lock_fbid( &g_DRM, commit->fb_id );
		}

		return commit;
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

	commit->vulkanTex = vulkan_create_texture_from_wlr_buffer( buf );
	assert( commit->vulkanTex );

	struct wlr_dmabuf_attributes dmabuf = {0};
	if ( BIsNested() == false && wlr_buffer_get_dmabuf( buf, &dmabuf ) )
	{
		commit->fb_id = drm_fbid_from_dmabuf( &g_DRM, buf, &dmabuf );

		if ( commit->fb_id )
		{
			drm_lock_fbid( &g_DRM, commit->fb_id );
		}
	}
	else
	{
		commit->fb_id = 0;
	}

	entry.listener.notify = destroy_buffer;
	entry.buf = buf;
	entry.vulkanTex = commit->vulkanTex;
	entry.fb_id = commit->fb_id;

	wlserver_lock();
	wl_signal_add( &buf->events.destroy, &entry.listener );
	wlserver_unlock();

	return commit;
}

static int32_t
window_last_done_commit_id( win *w )
{
	int32_t lastCommit = -1;
	for ( uint32_t i = 0; i < w->commit_queue.size(); i++ )
	{
		if ( w->commit_queue[ i ]->done )
		{
			lastCommit = i;
		}
	}

	return lastCommit;
}

static bool
window_has_commits( win *w )
{
	return window_last_done_commit_id( w ) != -1;
}

static void
get_window_last_done_commit( win *w, std::shared_ptr<commit_t> &commit )
{
	int32_t lastCommit = window_last_done_commit_id( w );

	if ( lastCommit == -1 )
	{
		return;
	}

	if ( commit != w->commit_queue[ lastCommit ] )
		commit = w->commit_queue[ lastCommit ];
}

// For Steam, etc.
static bool
window_wants_no_focus_when_mouse_hidden( win *w )
{
	return w && w->appID == 769;
}

static bool
window_is_fullscreen( win *w )
{
	return w && ( w->appID == 769 || w->isFullscreen );
}

/**
 * Constructor for a cursor. It is hidden in the beginning (normally until moved by user).
 */
MouseCursor::MouseCursor(xwayland_ctx_t *ctx)
	: m_texture(0)
	, m_dirty(true)
	, m_imageEmpty(false)
	, m_hideForMovement(true)
	, m_ctx(ctx)
{
	m_lastX = g_nNestedWidth / 2;
	m_lastY = g_nNestedHeight / 2;
}

void MouseCursor::queryPositions(int &rootX, int &rootY, int &winX, int &winY)
{
	Window window, child;
	unsigned int mask;

	XQueryPointer(m_ctx->dpy, DefaultRootWindow(m_ctx->dpy), &window, &child,
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

	XQueryPointer(m_ctx->dpy, DefaultRootWindow(m_ctx->dpy), &window, &child,
				  &rootX, &rootY, &winX, &winY, &mask);
}

void MouseCursor::checkSuspension()
{
	unsigned int buttonMask;
	queryButtonMask(buttonMask);

	bool bWasHidden = m_hideForMovement;

	if (buttonMask & ( Button1Mask | Button2Mask | Button3Mask | Button4Mask | Button5Mask )) {
		m_hideForMovement = false;
		m_lastMovedTime = get_time_in_milliseconds();

		// Move the cursor back to where we left it if the window didn't want us to give
		// it hover/focus where we left it and we moved it before.
		win *window = m_ctx->focus.inputFocusWindow;
		if (window_wants_no_focus_when_mouse_hidden(window) && bWasHidden)
		{
			XWarpPointer(m_ctx->dpy, None, x11_win(m_ctx->focus.inputFocusWindow), 0, 0, 0, 0, m_lastX, m_lastY);
		}
	}

	const bool suspended = get_time_in_milliseconds() - m_lastMovedTime > cursorHideTime;
	if (!m_hideForMovement && suspended) {
		m_hideForMovement = true;

		win *window = m_ctx->focus.inputFocusWindow;

		// Rearm warp count
		if (window) {
			window->mouseMoved = 0;

			// Move the cursor to the bottom right corner, just off screen if we can
			// if the window (ie. Steam) doesn't want hover/focus events.
			if ( window_wants_no_focus_when_mouse_hidden(window) )
			{
				m_lastX = m_x;
				m_lastY = m_y;
				XWarpPointer(m_ctx->dpy, None, x11_win(m_ctx->focus.inputFocusWindow), 0, 0, 0, 0, window->a.width - 1, window->a.height - 1);
			}
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
	XWarpPointer(m_ctx->dpy, None, x11_win(m_ctx->focus.inputFocusWindow), 0, 0, 0, 0, x, y);
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
		m_ctx->dpy,
		DefaultVisual(m_ctx->dpy, DefaultScreen(m_ctx->dpy)),
		32, ZPixmap,
		0,
		data,
		w, h,
		32, 0)))
	{
		xwm_log.errorf("Failed to make ximage for cursor");
		goto error_image;
	}

	if (!(pixmap = XCreatePixmap(m_ctx->dpy, DefaultRootWindow(m_ctx->dpy), w, h, 32)))
	{
		xwm_log.errorf("Failed to make pixmap for cursor");
		goto error_pixmap;
	}

	if (!(gc = XCreateGC(m_ctx->dpy, pixmap, 0, NULL)))
	{
		xwm_log.errorf("Failed to make gc for cursor");
		goto error_gc;
	}

	XPutImage(m_ctx->dpy, pixmap, gc, ximage, 0, 0, 0, 0, w, h);

	if (!(pictformat = XRenderFindStandardFormat(m_ctx->dpy, PictStandardARGB32)))
	{
		xwm_log.errorf("Failed to create pictformat for cursor");
		goto error_pictformat;
	}

	if (!(picture = XRenderCreatePicture(m_ctx->dpy, pixmap, pictformat, 0, NULL)))
	{
		xwm_log.errorf("Failed to create picture for cursor");
		goto error_picture;
	}

	if (!(cursor = XRenderCreateCursor(m_ctx->dpy, picture, hx, hy)))
	{
		xwm_log.errorf("Failed to create cursor");
		goto error_cursor;
	}

	XDefineCursor(m_ctx->dpy, DefaultRootWindow(m_ctx->dpy), cursor);
	XFlush(m_ctx->dpy);
	setDirty();
	return true;

error_cursor:
	XRenderFreePicture(m_ctx->dpy, picture);
error_picture:
error_pictformat:
	XFreeGC(m_ctx->dpy, gc);
error_gc:
	XFreePixmap(m_ctx->dpy, pixmap);
error_pixmap:
	// XDestroyImage frees the data.
	XDestroyImage(ximage);
error_image:
	return false;
}

void MouseCursor::constrainPosition()
{
	int i;
	win *window = m_ctx->focus.inputFocusWindow;
	win *override = m_ctx->focus.overrideWindow;
	if (window == override)
		window = m_ctx->focus.focusWindow;

	// If we had barriers before, get rid of them.
	for (i = 0; i < 4; i++) {
		if (m_scaledFocusBarriers[i] != None) {
			XFixesDestroyPointerBarrier(m_ctx->dpy, m_scaledFocusBarriers[i]);
			m_scaledFocusBarriers[i] = None;
		}
	}

	auto barricade = [this](int x1, int y1, int x2, int y2) {
		return XFixesCreatePointerBarrier(m_ctx->dpy, DefaultRootWindow(m_ctx->dpy),
										  x1, y1, x2, y2, 0, 0, NULL);
	};

	int x1 = window->a.x;
	int y1 = window->a.y;
	if (override)
	{
		x1 = std::min(x1, override->a.x);
		y1 = std::min(y1, override->a.y);
	}
	int x2 = window->a.x + window->a.width;
	int y2 = window->a.y + window->a.height;
	if (override)
	{
		x2 = std::max(x2, override->a.x + override->a.width);
		y2 = std::max(y2, override->a.y + override->a.height);
	}

	// Constrain it to the window; careful, the corners will leak due to a known X server bug.
	m_scaledFocusBarriers[0] = barricade(0, y1, m_ctx->root_width, y1);
	m_scaledFocusBarriers[1] = barricade(x2, 0, x2, m_ctx->root_height);
	m_scaledFocusBarriers[2] = barricade(m_ctx->root_width, y2, 0, y2);
	m_scaledFocusBarriers[3] = barricade(x1, m_ctx->root_height, x1, 0);

	// Make sure the cursor is somewhere in our jail
	int rootX, rootY;
	queryGlobalPosition(rootX, rootY);

	if ( rootX >= x2 || rootY >= y2 || rootX < x1 || rootY < y1 ) {
		if ( window_wants_no_focus_when_mouse_hidden( window ) && m_hideForMovement )
			warp(window->a.width - 1, window->a.height - 1);
		else
			warp(window->a.width / 2, window->a.height / 2);

		m_lastX = window->a.width / 2;
		m_lastY = window->a.height / 2;
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

	win *window = m_ctx->focus.inputFocusWindow;

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
	if (!window )
		return;

	if ( ( window != global_focus.inputFocusWindow || !g_bPendingTouchMovement.exchange(false) ) && window->mouseMoved++ < 5 )
		return;

	m_lastMovedTime = get_time_in_milliseconds();
	// Move the cursor back to centre if the window didn't want us to give
	// it hover/focus where we left it.
	if ( m_hideForMovement && window_wants_no_focus_when_mouse_hidden(window) )
	{
		XWarpPointer(m_ctx->dpy, None, x11_win(m_ctx->focus.inputFocusWindow), 0, 0, 0, 0, m_lastX, m_lastY);
	}
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

	auto *image = XFixesGetCursorImage(m_ctx->dpy);

	if (!image) {
		return false;
	}

	m_hotspotX = image->xhot;
	m_hotspotY = image->yhot;

	uint32_t surfaceWidth;
	uint32_t surfaceHeight;
	if ( BIsNested() == false && alwaysComposite == false )
	{
		surfaceWidth = g_DRM.cursor_width;
		surfaceHeight = g_DRM.cursor_height;
	}
	else
	{
		surfaceWidth = image->width;
		surfaceHeight = image->height;
	}

	m_texture = nullptr;

	// Assume the cursor is fully translucent unless proven otherwise.
	bool bNoCursor = true;

	auto cursorBuffer = std::vector<uint32_t>(surfaceWidth * surfaceHeight);
	for (int i = 0; i < image->height; i++) {
		for (int j = 0; j < image->width; j++) {
			cursorBuffer[i * surfaceWidth + j] = image->pixels[i * image->width + j];

			if ( cursorBuffer[i * surfaceWidth + j] & 0xff000000 ) {
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

	m_texture = vulkan_create_texture_from_bits(surfaceWidth, surfaceHeight, image->width, image->height, DRM_FORMAT_ARGB8888, texCreateFlags, cursorBuffer.data());
	assert(m_texture);
	XFree(image);
	m_dirty = false;

	return true;
}

void MouseCursor::paint(win *window, win *fit, struct FrameInfo_t *frameInfo)
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

	uint32_t sourceWidth = window->a.width;
	uint32_t sourceHeight = window->a.height;

	if ( fit )
	{
		// If we have an override window, try to fit it in as long as it won't make our scale go below 1.0.
		sourceWidth = std::max<uint32_t>( sourceWidth, clamp<int>( fit->a.x + fit->a.width, 0, currentOutputWidth ) );
		sourceHeight = std::max<uint32_t>( sourceHeight, clamp<int>( fit->a.y + fit->a.height, 0, currentOutputHeight ) );
	}

	float XOutputRatio = currentOutputWidth / (float)g_nNestedWidth;
	float YOutputRatio = currentOutputHeight / (float)g_nNestedHeight;
	float outputScaleRatio = (XOutputRatio < YOutputRatio) ? XOutputRatio : YOutputRatio;

	float scaledX, scaledY;
	float currentScaleRatio = 1.0;
	float XRatio = (float)g_nNestedWidth / sourceWidth;
	float YRatio = (float)g_nNestedHeight / sourceHeight;
	int cursorOffsetX, cursorOffsetY;

	currentScaleRatio = (XRatio < YRatio) ? XRatio : YRatio;
	currentScaleRatio = std::min(g_flMaxWindowScale, currentScaleRatio);
	currentScaleRatio *= outputScaleRatio;
	if (g_bIntegerScale && currentScaleRatio > 1.0f)
		currentScaleRatio = floor(currentScaleRatio);

	cursorOffsetX = (currentOutputWidth - sourceWidth * currentScaleRatio * globalScaleRatio) / 2.0f;
	cursorOffsetY = (currentOutputHeight - sourceHeight * currentScaleRatio * globalScaleRatio) / 2.0f;

	// Actual point on scaled screen where the cursor hotspot should be
	scaledX = (winX - window->a.x) * currentScaleRatio * globalScaleRatio + cursorOffsetX;
	scaledY = (winY - window->a.y) * currentScaleRatio * globalScaleRatio + cursorOffsetY;

	if ( zoomScaleRatio != 1.0 )
	{
		scaledX += ((sourceWidth / 2) - winX) * currentScaleRatio * globalScaleRatio;
		scaledY += ((sourceHeight / 2) - winY) * currentScaleRatio * globalScaleRatio;
	}

	// Apply the cursor offset inside the texture using the display scale
	scaledX = scaledX - m_hotspotX;
	scaledY = scaledY - m_hotspotY;

	int curLayer = frameInfo->layerCount++;

	FrameInfo_t::Layer_t *layer = &frameInfo->layers[ curLayer ];

	layer->opacity = 1.0;

	layer->scale.x = 1.0;
	layer->scale.y = 1.0;

	layer->offset.x = -scaledX;
	layer->offset.y = -scaledY;

	layer->zpos = g_zposCursor; // cursor, on top of both bottom layers

	layer->tex = m_texture;
	layer->fbid = BIsNested() ? 0 : m_texture->fbid();

	layer->linearFilter = false;
	layer->blackBorder = false;
}

struct BaseLayerInfo_t
{
	float scale[2];
	float offset[2];
	float opacity;
};

std::array< BaseLayerInfo_t, HELD_COMMIT_COUNT > g_CachedPlanes = {};

static void
paint_cached_base_layer(const std::shared_ptr<commit_t>& commit, const BaseLayerInfo_t& base, struct FrameInfo_t *frameInfo, float flOpacityScale)
{
	int curLayer = frameInfo->layerCount++;

	FrameInfo_t::Layer_t *layer = &frameInfo->layers[ curLayer ];

	layer->scale.x = base.scale[0];
	layer->scale.y = base.scale[1];
	layer->offset.x = base.offset[0];
	layer->offset.y = base.offset[1];
	layer->opacity = base.opacity * flOpacityScale;

	layer->tex = commit->vulkanTex;
	layer->fbid = commit->fb_id;

	layer->linearFilter = true;
	layer->blackBorder = true;
}

namespace PaintWindowFlag
{
	static const uint32_t BasePlane = 1u << 0;
	static const uint32_t FadeTarget = 1u << 1;
	static const uint32_t NotificationMode = 1u << 2;
	static const uint32_t DrawBorders = 1u << 3;
	static const uint32_t NoScale = 1u << 4;
}
using PaintWindowFlags = uint32_t;

static void
paint_window(win *w, win *scaleW, struct FrameInfo_t *frameInfo,
			  MouseCursor *cursor, PaintWindowFlags flags = 0, float flOpacityScale = 1.0f, win *fit = nullptr )
{
	uint32_t sourceWidth, sourceHeight;
	int drawXOffset = 0, drawYOffset = 0;
	float currentScaleRatio = 1.0;
	std::shared_ptr<commit_t> lastCommit;
	if ( w )
		get_window_last_done_commit( w, lastCommit );

	if ( flags & PaintWindowFlag::BasePlane )
	{
		if ( !lastCommit )
		{
			// If we're the base plane and have no valid contents
			// pick up that buffer we've been holding onto if we have one.
			if ( g_HeldCommits[ HELD_COMMIT_BASE ] )
			{
				paint_cached_base_layer( g_HeldCommits[ HELD_COMMIT_BASE ], g_CachedPlanes[ HELD_COMMIT_BASE ], frameInfo, flOpacityScale );
				return;
			}
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

	// Exit out if we have no window or
	// no commit.
	//
	// We may have no commit if we're an overlay,
	// in which case, we don't want to add it,
	// or in the case of the base plane, this is our
	// first ever frame so we have no cached base layer
	// to hold on to, so we should not add a layer in that
	// instance either.
	if (!w || !lastCommit)
		return;

	// Base plane will stay as tex=0 if we don't have contents yet, which will
	// make us fall back to compositing and use the Vulkan null texture

	win *mainOverlayWindow = global_focus.overlayWindow;

	const bool notificationMode = flags & PaintWindowFlag::NotificationMode;
	if (notificationMode && !mainOverlayWindow)
		return;

	if (notificationMode)
	{
		sourceWidth = mainOverlayWindow->a.width;
		sourceHeight = mainOverlayWindow->a.height;
	}
	else if ( flags & PaintWindowFlag::NoScale )
	{
		sourceWidth = currentOutputWidth;
		sourceHeight = currentOutputHeight;
	}
	else
	{
		sourceWidth = scaleW->a.width;
		sourceHeight = scaleW->a.height;

		if ( fit )
		{
			// If we have an override window, try to fit it in as long as it won't make our scale go below 1.0.
			sourceWidth = std::max<uint32_t>( sourceWidth, clamp<int>( fit->a.x + fit->a.width, 0, currentOutputWidth ) );
			sourceHeight = std::max<uint32_t>( sourceHeight, clamp<int>( fit->a.y + fit->a.height, 0, currentOutputHeight ) );
		}
	}

	if (sourceWidth != currentOutputWidth || sourceHeight != currentOutputHeight || ( ( w->a.x || w->a.y ) && w != scaleW ) || globalScaleRatio != 1.0f)
	{
		float XOutputRatio = currentOutputWidth / (float)g_nNestedWidth;
		float YOutputRatio = currentOutputHeight / (float)g_nNestedHeight;
		float outputScaleRatio = (XOutputRatio < YOutputRatio) ? XOutputRatio : YOutputRatio;

		float XRatio = (float)g_nNestedWidth / sourceWidth;
		float YRatio = (float)g_nNestedHeight / sourceHeight;

		currentScaleRatio = (XRatio < YRatio) ? XRatio : YRatio;
		currentScaleRatio = std::min(g_flMaxWindowScale, currentScaleRatio);
		currentScaleRatio *= outputScaleRatio;
		if (g_bIntegerScale && currentScaleRatio > 1.0f)
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

	int curLayer = frameInfo->layerCount++;

	FrameInfo_t::Layer_t *layer = &frameInfo->layers[ curLayer ];

	layer->opacity = ( (w->isOverlay || w->isExternalOverlay) ? w->opacity / (float)OPAQUE : 1.0f ) * flOpacityScale;

	layer->scale.x = 1.0 / currentScaleRatio;
	layer->scale.y = 1.0 / currentScaleRatio;

	if ( w != scaleW )
	{
		layer->offset.x = -drawXOffset;
		layer->offset.y = -drawYOffset;
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

		layer->offset.x = (currentOutputWidth - xOffset - width) * -1.0f;
		layer->offset.y = (currentOutputHeight - yOffset - height) * -1.0f;
	}
	else
	{
		layer->offset.x = -drawXOffset;
		layer->offset.y = -drawYOffset;
	}

	layer->blackBorder = flags & PaintWindowFlag::DrawBorders;

	layer->zpos = g_zposBase;

	if ( w != scaleW )
	{
		layer->zpos = g_zposOverride;
	}

	if ( w->isOverlay || w->isSteamStreamingClient )
	{
		layer->zpos = g_zposOverlay;
	}
	if ( w->isExternalOverlay )
	{
		layer->zpos = g_zposExternalOverlay;
	}

	layer->tex = lastCommit->vulkanTex;
	layer->fbid = lastCommit->fb_id;

	layer->linearFilter = (w->isOverlay || w->isExternalOverlay) ? true : g_bFilterGameWindow;

	if ( flags & PaintWindowFlag::BasePlane )
	{
		BaseLayerInfo_t basePlane = {};
		basePlane.scale[0] = layer->scale.x;
		basePlane.scale[1] = layer->scale.y;
		basePlane.offset[0] = layer->offset.x;
		basePlane.offset[1] = layer->offset.y;
		basePlane.opacity = layer->opacity;

		g_CachedPlanes[ HELD_COMMIT_BASE ] = basePlane;
		if ( !(flags & PaintWindowFlag::FadeTarget) )
			g_CachedPlanes[ HELD_COMMIT_FADE ] = basePlane;
	}
}

bool g_bFirstFrame = true;

static bool is_fading_out()
{
	return fadeOutStartTime || g_bPendingFade;
}

static void update_touch_scaling( const struct FrameInfo_t *frameInfo )
{
	if ( !frameInfo->layerCount )
		return;

	focusedWindowScaleX = frameInfo->layers[ frameInfo->layerCount - 1 ].scale.x;
	focusedWindowScaleY = frameInfo->layers[ frameInfo->layerCount - 1 ].scale.y;
	focusedWindowOffsetX = frameInfo->layers[ frameInfo->layerCount - 1 ].offset.x;
	focusedWindowOffsetY = frameInfo->layers[ frameInfo->layerCount - 1 ].offset.y;
}

static void
paint_all()
{
	gamescope_xwayland_server_t *root_server = wlserver_get_xwayland_server(0);
	xwayland_ctx_t *root_ctx = root_server->ctx.get();

	static long long int paintID = 0;

	paintID++;
	gpuvis_trace_begin_ctx_printf( paintID, "paint_all" );
	win	*w;
	win	*overlay;
	win *externalOverlay;
	win	*notification;
	win	*override;
	win *input;

	unsigned int currentTime = get_time_in_milliseconds();
	bool fadingOut = ( currentTime - fadeOutStartTime < g_FadeOutDuration || g_bPendingFade ) && g_HeldCommits[HELD_COMMIT_FADE];

	w = global_focus.focusWindow;
	overlay = global_focus.overlayWindow;
	externalOverlay = global_focus.externalOverlayWindow;
	notification = global_focus.notificationWindow;
	override = global_focus.overrideWindow;
	input = global_focus.inputFocusWindow;

	if (++frameCounter == 300)
	{
		currentFrameRate = 300 * 1000.0f / (currentTime - lastSampledFrameTime);
		lastSampledFrameTime = currentTime;
		frameCounter = 0;

		stats_printf( "fps=%f\n", currentFrameRate );

		if ( w && w->isSteam )
		{
			stats_printf( "focus=steam\n" );
		}
		else
		{
			stats_printf( "focus=%i\n", w ? w->appID : 0 );
		}
	}

	struct FrameInfo_t frameInfo = {};

	// If the window we'd paint as the base layer is the streaming client,
	// find the video underlay and put it up first in the scenegraph
	if ( w )
	{
		if ( w->isSteamStreamingClient == true )
		{
			win *videow = NULL;
			bool bHasVideoUnderlay = false;

			gamescope_xwayland_server_t *server = NULL;
			for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
			{
				for ( videow = server->ctx->list; videow; videow = videow->next )
				{
					if ( videow->isSteamStreamingClientVideo == true )
					{
						// TODO: also check matching AppID so we can have several pairs
						paint_window(videow, videow, &frameInfo, global_focus.cursor, PaintWindowFlag::BasePlane | PaintWindowFlag::DrawBorders);
						bHasVideoUnderlay = true;
						break;
					}
				}
			}
			
			int nOldLayerCount = frameInfo.layerCount;

			uint32_t flags = 0;
			if ( !bHasVideoUnderlay )
				flags |= PaintWindowFlag::BasePlane;
			paint_window(w, w, &frameInfo, global_focus.cursor, flags);
			update_touch_scaling( &frameInfo );
			
			// paint UI unless it's fully hidden, which it communicates to us through opacity=0
			// we paint it to extract scaling coefficients above, then remove the layer if one was added
			if ( w->opacity == TRANSLUCENT && bHasVideoUnderlay && nOldLayerCount < frameInfo.layerCount )
				frameInfo.layerCount--;
		}
		else
		{
			if ( fadingOut )
			{
				float opacityScale = g_bPendingFade
					? 0.0f
					: ((currentTime - fadeOutStartTime) / (float)g_FadeOutDuration);
		
				paint_cached_base_layer(g_HeldCommits[HELD_COMMIT_FADE], g_CachedPlanes[HELD_COMMIT_FADE], &frameInfo, 1.0f - opacityScale);
				paint_window(w, w, &frameInfo, global_focus.cursor, PaintWindowFlag::BasePlane | PaintWindowFlag::FadeTarget | PaintWindowFlag::DrawBorders, opacityScale, override);
			}
			else
			{
				{
					if ( g_HeldCommits[HELD_COMMIT_FADE] )
					{
						g_HeldCommits[HELD_COMMIT_FADE] = nullptr;
						g_bPendingFade = false;
						fadeOutStartTime = 0;
						global_focus.fadeWindow = None;
					}
				}
				// Just draw focused window as normal, be it Steam or the game
				paint_window(w, w, &frameInfo, global_focus.cursor, PaintWindowFlag::BasePlane | PaintWindowFlag::DrawBorders, 1.0f, override);

				bool needsScaling = frameInfo.layers[0].scale.x < 1.0f && frameInfo.layers[0].scale.y < 1.0f;
				frameInfo.useFSRLayer0 = g_upscaler == GamescopeUpscaler::FSR && needsScaling;
				frameInfo.useNISLayer0 = g_upscaler == GamescopeUpscaler::NIS && needsScaling;
			}
			update_touch_scaling( &frameInfo );
		}
	}
	else
	{
		if ( g_HeldCommits[HELD_COMMIT_BASE] )
			paint_cached_base_layer(g_HeldCommits[HELD_COMMIT_BASE], g_CachedPlanes[HELD_COMMIT_BASE], &frameInfo, 1.0f);
	}

	// TODO: We want to paint this at the same scale as the normal window and probably
	// with an offset.
	// Josh: No override if we're streaming video
	// as we will have too many layers. Better to be safe than sorry.
	if ( override && w && !w->isSteamStreamingClient )
	{
		paint_window(override, w, &frameInfo, global_focus.cursor, 0, 1.0f, override);
		// Don't update touch scaling for frameInfo. We don't ever make it our
		// wlserver_mousefocus window.
		//update_touch_scaling( &frameInfo );
	}

	// If we have any layers that aren't a cursor or overlay, then we have valid contents for presentation.
	const bool bValidContents = frameInfo.layerCount > 0;

  	if (externalOverlay)
	{
		if (externalOverlay->opacity)
		{
			paint_window(externalOverlay, externalOverlay, &frameInfo, global_focus.cursor, PaintWindowFlag::NoScale);

			if ( externalOverlay == global_focus.inputFocusWindow )
				update_touch_scaling( &frameInfo );
		}
	}

	if (overlay)
	{
		if (overlay->opacity)
		{
			paint_window(overlay, overlay, &frameInfo, global_focus.cursor, PaintWindowFlag::DrawBorders);

			if ( overlay == global_focus.inputFocusWindow )
				update_touch_scaling( &frameInfo );
		}
	}

	if (notification)
	{
		if (notification->opacity)
		{
			paint_window(notification, notification, &frameInfo, global_focus.cursor, PaintWindowFlag::NotificationMode);
		}
	}

	bool bDrewCursor = false;

	// Draw cursor if we need to
	if (input) {
		int nLayerCountBefore = frameInfo.layerCount;
		global_focus.cursor->paint(
			input, w == input ? override : nullptr,
			&frameInfo);
		int nLayerCountAfter = frameInfo.layerCount;
		bDrewCursor = nLayerCountAfter > nLayerCountBefore;
	}

	if ( !bValidContents || ( BIsNested() == false && g_DRM.paused == true ) )
	{
		return;
	}

	unsigned int blurFadeTime = get_time_in_milliseconds() - g_BlurFadeStartTime;
	bool blurFading = blurFadeTime < g_BlurFadeDuration;
	BlurMode currentBlurMode = blurFading ? std::max(g_BlurMode, g_BlurModeOld) : g_BlurMode;

	if (currentBlurMode && !(frameInfo.layerCount <= 1 && currentBlurMode == BLUR_MODE_COND))
	{
		frameInfo.blurLayer0 = currentBlurMode;
		frameInfo.blurRadius = g_BlurRadius;

		if (blurFading)
		{
			float ratio = blurFadeTime / (float) g_BlurFadeDuration;
			bool fadingIn = g_BlurMode > g_BlurModeOld;

			if (!fadingIn)
				ratio = 1.0 - ratio;

			frameInfo.blurRadius = ratio * g_BlurRadius;
		}

		frameInfo.useFSRLayer0 = false;
		frameInfo.useNISLayer0 = false;
	}

	g_bFSRActive = frameInfo.useFSRLayer0;

	bool bWasFirstFrame = g_bFirstFrame;
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

	int nTargetRefresh = g_nDynamicRefreshRate && steamcompmgr_window_should_limit_fps( global_focus.focusWindow )// && !global_focus.overlayWindow
		? g_nDynamicRefreshRate
		: drm_get_default_refresh( &g_DRM );

	uint64_t now = get_time_in_nanos();

	if ( g_nOutputRefresh == nTargetRefresh )
		g_uDynamicRefreshEqualityTime = now;

	if ( !BIsNested() && g_nOutputRefresh != nTargetRefresh && g_uDynamicRefreshEqualityTime + g_uDynamicRefreshDelay < now )
		drm_set_refresh( &g_DRM, nTargetRefresh );

	bool bNeedsNearest = !g_bFilterGameWindow && frameInfo.layers[0].scale.x != 1.0f && frameInfo.layers[0].scale.y != 1.0f;

	bool bNeedsComposite = BIsNested();
	bNeedsComposite |= alwaysComposite;
	bNeedsComposite |= bCapture;
	bNeedsComposite |= bWasFirstFrame;
	bNeedsComposite |= frameInfo.useFSRLayer0;
	bNeedsComposite |= frameInfo.useNISLayer0;
	bNeedsComposite |= frameInfo.blurLayer0;
	bNeedsComposite |= bNeedsNearest;
	bNeedsComposite |= bDrewCursor;

	if ( !bNeedsComposite )
	{
		int ret = drm_prepare( &g_DRM, &frameInfo );
		if ( ret == 0 )
			bDoComposite = false;
		else if ( ret == -EACCES )
			return;
	}

	if ( bDoComposite == true )
	{
		std::shared_ptr<CVulkanTexture> pCaptureTexture = nullptr;
#if HAVE_PIPEWIRE
		if ( pw_buffer != nullptr )
		{
			pCaptureTexture = pw_buffer->texture;
		}
#endif
		if ( bCapture && pCaptureTexture == nullptr )
		{
			pCaptureTexture = vulkan_acquire_screenshot_texture(false);
		}

		bool bResult = vulkan_composite( &frameInfo, pCaptureTexture );

		if ( bResult != true )
		{
			xwm_log.errorf("vulkan_composite failed");
			return;
		}

		if ( BIsNested() == true )
		{
			vulkan_present_to_window();
			// Update the time it took us to present.
			// TODO: Use Vulkan present timing in future.
			g_uVblankDrawTimeNS = get_time_in_nanos() - g_SteamCompMgrVBlankTime;
		}
		else
		{
			frameInfo = {};

			frameInfo.layerCount = 1;
			FrameInfo_t::Layer_t *layer = &frameInfo.layers[ 0 ];
			layer->scale.x = 1.0;
			layer->scale.y = 1.0;
			layer->opacity = 1.0;

			layer->tex = vulkan_get_last_output_image();
			layer->fbid = layer->tex->fbid();

			layer->linearFilter = false;

			int ret = drm_prepare( &g_DRM, &frameInfo );

			// Happens when we're VT-switched away
			if ( ret == -EACCES )
				return;

			if ( ret != 0 )
			{
				if ( g_DRM.current.mode_id == 0 )
				{
					xwm_log.errorf("We failed our modeset and have no mode to fall back to! (Initial modeset failed?): %s", strerror(-ret));
					abort();
				}

				xwm_log.errorf("Failed to prepare 1-layer flip (%s), trying again with previous mode if modeset needed", strerror( -ret ));

				drm_rollback( &g_DRM );

				// Try once again to in case we need to fall back to another mode.
				ret = drm_prepare( &g_DRM, &frameInfo );

				// Happens when we're VT-switched away
				if ( ret == -EACCES )
					return;

				if ( ret != 0 )
				{
					xwm_log.errorf("Failed to prepare 1-layer flip entirely: %s", strerror( -ret ));
					// We should always handle a 1-layer flip
					abort();
				}
			}

			drm_commit( &g_DRM, &frameInfo );
		}

		if ( takeScreenshot )
		{
			assert( pCaptureTexture != nullptr );
			assert( pCaptureTexture->format() == VK_FORMAT_B8G8R8A8_UNORM );

			std::thread screenshotThread = std::thread([=] {
				pthread_setname_np( pthread_self(), "gamescope-scrsh" );

				const uint8_t *mappedData = reinterpret_cast<const uint8_t *>(pCaptureTexture->mappedData());

				// Make our own copy of the image to remove the alpha channel.
				auto imageData = std::vector<uint8_t>(currentOutputWidth * currentOutputHeight * 4);
				const uint32_t comp = 4;
				const uint32_t pitch = currentOutputWidth * comp;
				for (uint32_t y = 0; y < currentOutputHeight; y++)
				{
					for (uint32_t x = 0; x < currentOutputWidth; x++)
					{
						// BGR...
						imageData[y * pitch + x * comp + 0] = mappedData[y * pCaptureTexture->rowPitch() + x * comp + 2];
						imageData[y * pitch + x * comp + 1] = mappedData[y * pCaptureTexture->rowPitch() + x * comp + 1];
						imageData[y * pitch + x * comp + 2] = mappedData[y * pCaptureTexture->rowPitch() + x * comp + 0];
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

				XDeleteProperty( root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeScreenShotAtom );
			});

			screenshotThread.detach();

			takeScreenshot = false;
		}

#if HAVE_PIPEWIRE
		if ( pw_buffer != nullptr )
		{
			push_pipewire_buffer(pw_buffer);
			// TODO: make sure the pw_buffer isn't lost in one of the failure
			// code-paths above
		}
#endif
	}
	else
	{
		assert( BIsNested() == false );

		drm_commit( &g_DRM, &frameInfo );
	}

	gpuvis_trace_end_ctx_printf( paintID, "paint_all" );
	gpuvis_trace_printf( "paint_all %i layers, composite %i", (int)frameInfo.layerCount, bDoComposite );
}

/* Get prop from window
 *   not found: default
 *   otherwise the value
 */
static unsigned int
get_prop(xwayland_ctx_t *ctx, Window win, Atom prop, unsigned int def, bool *found = nullptr )
{
	Atom actual;
	int format;
	unsigned long n, left;

	unsigned char *data;
	int result = XGetWindowProperty(ctx->dpy, win, prop, 0L, 1L, false,
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
bool get_prop( xwayland_ctx_t *ctx, Window win, Atom prop, std::vector< uint32_t > &vecResult )
{
	Atom actual;
	int format;
	unsigned long n, left;

	vecResult.clear();
	uint64_t *data;
	// get up to 16 results in one go, we can add a real loop if we ever need anything beyong that
	int result = XGetWindowProperty(ctx->dpy, win, prop, 0L, 16L, false,
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
win_is_useless( win *w )
{
	// Windows that are 1x1 are pretty useless for override redirects.
	// Just ignore them.
	// Fixes the Xbox Login in Age of Empires 2: DE.
	return w->a.width == 1 && w->a.height == 1;
}

static bool
win_is_override_redirect( win *w )
{
	return w->a.override_redirect && !w->ignoreOverrideRedirect && !win_is_useless( w );
}

static bool
win_skip_taskbar_and_pager( win *w )
{
	return w->skipTaskbar && w->skipPager;
}

static bool
win_maybe_a_dropdown( win *w )
{
	// Josh:
	// Right now we don't get enough info from Wine
	// about the true nature of windows to distringuish
	// something like the Fallout 4 Options menu from the
	// Warframe language dropdown. Until we get more stuff
	// exposed for that, there is this workaround to let that work.
	if ( w->appID == 230410 && w->maybe_a_dropdown && w->transientFor && ( w->skipPager || w->skipTaskbar ) )
		return !win_is_useless( w );


	// Josh:
	// The logic here is as follows. The window will be treated as a dropdown if:
	// 
	// If this window has a fixed position on the screen + static gravity:
	//  - If the window has either skipPage or skipTaskbar
	//    - If the window isn't a dialog, always treat it as a dropdown, as it's
	//      probably meant to be some form of popup.
	//    - If the window is a dialog 
	// 		- If the window has transient for, disregard it, as it is trying to redirecting us elsewhere
	//        ie. a settings menu dialog popup or something.
	//      - If the window has both skip taskbar and pager, treat it as a dialog.
	bool valid_maybe_a_dropdown =
		w->maybe_a_dropdown && ( ( !w->is_dialog || ( !w->transientFor && win_skip_taskbar_and_pager( w ) ) ) && ( w->skipPager || w->skipTaskbar ) );
	return ( valid_maybe_a_dropdown || win_is_override_redirect( w ) ) && !win_is_useless( w );
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

	// If the window is 1x1 then prefer anything else we have.
	if ( win_is_useless( a ) != win_is_useless( b ) )
		return !win_is_useless( a );

	if ( win_maybe_a_dropdown( a ) != win_maybe_a_dropdown( b ) )
		return !win_maybe_a_dropdown( a );

	// Wine sets SKIP_TASKBAR and SKIP_PAGER hints for WS_EX_NOACTIVATE windows.
	// See https://github.com/Plagman/gamescope/issues/87
	if ( win_skip_taskbar_and_pager( a ) != win_skip_taskbar_and_pager( b ) )
		return !win_skip_taskbar_and_pager( a );

	// Prefer normal windows over dialogs
	// if we are an override redirect/dropdown window.
	if ( win_maybe_a_dropdown( a ) && win_maybe_a_dropdown( b ) &&
		a->is_dialog != b->is_dialog )
		return !a->is_dialog;

	// Attempt to tie-break dropdowns by transient-for.
	if ( win_maybe_a_dropdown( a ) && win_maybe_a_dropdown( b ) &&
		!a->transientFor != !b->transientFor )
		return !a->transientFor;

	if ( win_has_game_id( a ) && a->map_sequence != b->map_sequence )
		return a->map_sequence > b->map_sequence;

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
	if ( !focus )
		return false;

	return override != focus && override->a.x >= 0 && override->a.y >= 0;
} 

static bool
pick_primary_focus_and_override(focus_t *out, Window focusControlWindow, const std::vector<win*>& vecPossibleFocusWindows, bool globalFocus, const std::vector<uint32_t>& ctxFocusControlAppIDs)
{
	bool localGameFocused = false;
	win *focus = NULL, *override_focus = NULL;

	bool controlledFocus = focusControlWindow != None || !ctxFocusControlAppIDs.empty();
	if ( controlledFocus )
	{
		if ( focusControlWindow != None )
		{
			for ( win *focusable_window : vecPossibleFocusWindows )
			{
				if ( focusable_window->id == focusControlWindow )
				{
					focus = focusable_window;
					localGameFocused = true;
					goto found;
				}
			}
		}

		for ( auto focusable_appid : ctxFocusControlAppIDs )
		{
			for ( win *focusable_window : vecPossibleFocusWindows )
			{
				if ( focusable_window->appID == focusable_appid )
				{
					focus = focusable_window;
					localGameFocused = true;
					goto found;
				}
			}
		}

found:;
	}

	if ( !focus && ( !globalFocus || !controlledFocus ) )
	{
		if ( !vecPossibleFocusWindows.empty() )
		{
			focus = vecPossibleFocusWindows[ 0 ];
			localGameFocused = focus->appID != 0;
		}
	}

	auto resolveTransientOverrides = [&](bool maybe)
	{
		// Do some searches to find transient links to override redirects too.
		while ( true )
		{
			bool bFoundTransient = false;

			for ( win *candidate : vecPossibleFocusWindows )
			{
				bool is_dropdown = maybe ? win_maybe_a_dropdown( candidate ) : win_is_override_redirect( candidate );
				if ( ( !override_focus || candidate != override_focus ) && candidate != focus &&
					( ( !override_focus && candidate->transientFor == focus->id ) || ( override_focus && candidate->transientFor == override_focus->id ) ) &&
					 is_dropdown)
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

	if ( focus && !focusControlWindow )
	{
		// Do some searches through game windows to follow transient links if needed
		while ( true )
		{
			bool bFoundTransient = false;

			for ( win *candidate : vecPossibleFocusWindows )
			{
				if ( candidate != focus && candidate->transientFor == focus->id && !win_maybe_a_dropdown( candidate ) )
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
	}

	if ( !override_focus && focus )
	{
		if ( !ctxFocusControlAppIDs.empty() )
		{
			for ( win *override : vecPossibleFocusWindows )
			{
				if ( win_is_override_redirect(override) && is_good_override_candidate(override, focus) && override->appID == focus->appID ) {
					override_focus = override;
					break;
				}
			}
		}
		else if ( !vecPossibleFocusWindows.empty() )
		{
			for ( win *override : vecPossibleFocusWindows )
			{
				if ( win_is_override_redirect(override) && is_good_override_candidate(override, focus) ) {
					override_focus = override;
					break;
				}
			}
		}

		resolveTransientOverrides( false );
	}

	if ( focus )
	{
		if ( window_has_commits( focus ) ) 
			out->focusWindow = focus;
		else
			out->outdatedInteractiveFocus = true;

		// Always update X's idea of focus, but still dirty
		// the it being outdated so we can resolve that globally later.
		//
		// Only affecting X and not the WL idea of focus here,
		// we always want to think the window is focused.
		// but our real presenting focus and input focus can be elsewhere.
		if ( !globalFocus )
			out->focusWindow = focus;
	}

	if ( !override_focus && focus )
	{
		if ( controlledFocus )
		{
			for ( auto focusable_appid : ctxFocusControlAppIDs )
			{
				for ( win *fake_override : vecPossibleFocusWindows )
				{
					if ( fake_override->appID == focusable_appid )
					{
						if ( win_maybe_a_dropdown( fake_override ) && is_good_override_candidate( fake_override, focus ) && fake_override->appID == focus->appID )
						{
							override_focus = fake_override;
							goto found2;
						}
					}
				}
			}
		}
		else
		{
			for ( win *fake_override : vecPossibleFocusWindows )
			{
				if ( win_maybe_a_dropdown( fake_override ) && is_good_override_candidate( fake_override, focus ) )
				{
					override_focus = fake_override;
					goto found2;
				}
			}	
		}
		
		found2:;
		resolveTransientOverrides( true );
	}

	out->overrideWindow = override_focus;

	return localGameFocused;
}

static void
determine_and_apply_focus(xwayland_ctx_t *ctx, std::vector<win*>& vecGlobalPossibleFocusWindows)
{
	win *w;
	win *inputFocus = NULL;

	win *prevFocusWindow = ctx->focus.focusWindow;
	ctx->focus.overlayWindow = nullptr;
	ctx->focus.notificationWindow = nullptr;
	ctx->focus.overrideWindow = nullptr;
	ctx->focus.externalOverlayWindow = nullptr;

	unsigned int maxOpacity = 0;
	unsigned int maxOpacityExternal = 0;
	std::vector< win* > vecPossibleFocusWindows;
	for (w = ctx->list; w; w = w->next)
	{
		// Always skip system tray icons
		if ( w->isSysTrayIcon )
		{
			continue;
		}

		if ( w->a.map_state == IsViewable && w->a.c_class == InputOutput && w->isOverlay == false && w->isExternalOverlay == false &&
			( win_has_game_id( w ) || w->isSteam || w->isSteamStreamingClient ) &&
			 (w->opacity > TRANSLUCENT || w->isSteamStreamingClient == true ) )
		{
			vecPossibleFocusWindows.push_back( w );
		}

		if (w->isOverlay)
		{
			if (w->a.width > 1200 && w->opacity >= maxOpacity)
			{
				ctx->focus.overlayWindow = w;
				maxOpacity = w->opacity;
			}
			else
			{
				ctx->focus.notificationWindow = w;
			}
		}

		if (w->isExternalOverlay)
		{
			if (w->opacity > maxOpacityExternal)
			{
				ctx->focus.externalOverlayWindow = w;
				maxOpacityExternal = w->opacity;
			}
		}

		if ( w->isOverlay && w->inputFocusMode )
		{
			inputFocus = w;
		}
	}

	std::stable_sort( vecPossibleFocusWindows.begin(), vecPossibleFocusWindows.end(),
					  is_focus_priority_greater );

	vecGlobalPossibleFocusWindows.insert(vecGlobalPossibleFocusWindows.end(), vecPossibleFocusWindows.begin(), vecPossibleFocusWindows.end());

	pick_primary_focus_and_override( &ctx->focus, ctx->focusControlWindow, vecPossibleFocusWindows, false, vecFocuscontrolAppIDs );

	if ( inputFocus == NULL )
	{
		inputFocus = ctx->focus.focusWindow;
	}

	if ( !ctx->focus.focusWindow )
	{
		return;
	}

	if ( prevFocusWindow != ctx->focus.focusWindow )
	{
		/* Some games (e.g. DOOM Eternal) don't react well to being put back as
		* iconic, so never do that. Only take them out of iconic. */
		uint32_t wmState[] = { ICCCM_NORMAL_STATE, None };
		XChangeProperty(ctx->dpy, ctx->focus.focusWindow->id, ctx->atoms.WMStateAtom, ctx->atoms.WMStateAtom, 32,
					PropModeReplace, (unsigned char *)wmState,
					sizeof(wmState) / sizeof(wmState[0]));

		gpuvis_trace_printf( "determine_and_apply_focus focus %lu", ctx->focus.focusWindow->id );

		if ( debugFocus == true )
		{
			xwm_log.debugf( "determine_and_apply_focus focus %lu", ctx->focus.focusWindow->id );
			char buf[512];
			sprintf( buf,  "xwininfo -id 0x%lx; xprop -id 0x%lx; xwininfo -root -tree", ctx->focus.focusWindow->id, ctx->focus.focusWindow->id );
			system( buf );
		}
	}

	win *keyboardFocusWin = inputFocus;

	if ( inputFocus && inputFocus->inputFocusMode == 2 )
		keyboardFocusWin = ctx->focus.focusWindow;

	Window keyboardFocusWindow = keyboardFocusWin ? keyboardFocusWin->id : None;

	// If the top level parent of our current keyboard window is the same as our target (top level) input focus window
	// then keep focus on that and don't yank it away to the top level input focus window.
	// Fixes dropdowns in Steam CEF.
	if ( keyboardFocusWindow && ctx->currentKeyboardFocusWindow && find_win( ctx, ctx->currentKeyboardFocusWindow ) == keyboardFocusWin )
		keyboardFocusWindow = ctx->currentKeyboardFocusWindow;

	bool bResetToCorner = false;
	bool bResetToCenter = false;

	if ( ctx->focus.inputFocusWindow != inputFocus ||
		ctx->focus.inputFocusMode != inputFocus->inputFocusMode ||
		ctx->currentKeyboardFocusWindow != keyboardFocusWindow )
	{
		if ( debugFocus == true )
		{
			xwm_log.debugf( "determine_and_apply_focus inputFocus %lu", inputFocus->id );
		}

		if ( !ctx->focus.overrideWindow || ctx->focus.overrideWindow != keyboardFocusWin )
			XSetInputFocus(ctx->dpy, keyboardFocusWin->id, RevertToNone, CurrentTime);

		if ( ctx->focus.inputFocusWindow != inputFocus ||
			 ctx->focus.inputFocusMode != inputFocus->inputFocusMode )
		{
			// If the window doesn't want focus when hidden, move it away
			// as we are going to hide it straight after.
			// otherwise, if we switch from wanting it to not
			// (steam -> game)
			// put us back in the centre of the screen.
			if (window_wants_no_focus_when_mouse_hidden(inputFocus))
				bResetToCorner = true;
			else if ( window_wants_no_focus_when_mouse_hidden(inputFocus) != window_wants_no_focus_when_mouse_hidden(ctx->focus.inputFocusWindow) )
				bResetToCenter = true;

			// cursor is likely not interactable anymore in its original context, hide
			// don't care if we change kb focus window due to that happening when
			// going from override -> focus and we don't want to hide then as it's probably a dropdown.
			ctx->cursor->hide();
		}

		ctx->focus.inputFocusWindow = inputFocus;
		ctx->focus.inputFocusMode = inputFocus->inputFocusMode;
		ctx->currentKeyboardFocusWindow = keyboardFocusWindow;
	}

	w = ctx->focus.focusWindow;

	ctx->cursor->constrainPosition();

	if ( inputFocus == ctx->focus.focusWindow && ctx->focus.overrideWindow )
	{
		if ( ctx->list[0].id != ctx->focus.overrideWindow->id )
		{
			XRaiseWindow(ctx->dpy, ctx->focus.overrideWindow->id);
		}
	}
	else
	{
		if ( ctx->list[0].id != inputFocus->id )
		{
			XRaiseWindow(ctx->dpy, inputFocus->id);
		}
	}

	if (!ctx->focus.focusWindow->nudged)
	{
		XMoveWindow(ctx->dpy, ctx->focus.focusWindow->id, 1, 1);
		ctx->focus.focusWindow->nudged = true;
	}

	if (w->a.x != 0 || w->a.y != 0)
		XMoveWindow(ctx->dpy, ctx->focus.focusWindow->id, 0, 0);

	if ( window_is_fullscreen( ctx->focus.focusWindow ) && ( w->a.width != ctx->root_width || w->a.height != ctx->root_height || globalScaleRatio != 1.0f ) )
	{
		XResizeWindow(ctx->dpy, ctx->focus.focusWindow->id, ctx->root_width, ctx->root_height);
	}
	else if ( !window_is_fullscreen( ctx->focus.focusWindow ) && ctx->focus.focusWindow->sizeHintsSpecified &&
		((unsigned)ctx->focus.focusWindow->a.width != ctx->focus.focusWindow->requestedWidth ||
		(unsigned)ctx->focus.focusWindow->a.height != ctx->focus.focusWindow->requestedHeight))
	{
		XResizeWindow(ctx->dpy, ctx->focus.focusWindow->id, ctx->focus.focusWindow->requestedWidth, ctx->focus.focusWindow->requestedHeight);
	}

	if ( inputFocus )
	{
		// Cannot simply XWarpPointer here as we immediately go on to
		// do wlserver_mousefocus and need to update m_x and m_y of the cursor.
		if ( bResetToCorner )
		{
			inputFocus->mouseMoved = 0;
			ctx->cursor->forcePosition(inputFocus->a.width - 1, inputFocus->a.height - 1);
		}
		else if ( bResetToCenter )
		{
			inputFocus->mouseMoved = 0;
			ctx->cursor->forcePosition(inputFocus->a.width / 2, inputFocus->a.height / 2);
		}
	}

	Window	    root_return = None, parent_return = None;
	Window	    *children = NULL;
	unsigned int    nchildren = 0;
	unsigned int    i = 0;

	XQueryTree(ctx->dpy, w->id, &root_return, &parent_return, &children, &nchildren);

	while (i < nchildren)
	{
		XSelectInput( ctx->dpy, children[i], FocusChangeMask );
		i++;
	}

	XFree(children);
}

wlr_surface *win_surface(win *window)
{
	if (!window)
		return nullptr;

	return window->surface.wlr;
}

static void
determine_and_apply_focus()
{
	gamescope_xwayland_server_t *root_server = wlserver_get_xwayland_server(0);
	xwayland_ctx_t *root_ctx = root_server->ctx.get();
	global_focus_t previous_focus = global_focus;
	global_focus = global_focus_t{};
	global_focus.focusWindow = previous_focus.focusWindow;
	global_focus.cursor = root_ctx->cursor.get();
	gameFocused = false;

	std::vector< unsigned long > focusable_appids;
	std::vector< unsigned long > focusable_windows;

	// Determine local context focuses
	std::vector< win* > vecPossibleFocusWindows;
	{
		gamescope_xwayland_server_t *server = NULL;
		for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
		{
			determine_and_apply_focus(server->ctx.get(), vecPossibleFocusWindows);
		}
	}

	for ( win *focusable_window : vecPossibleFocusWindows )
	{
		// Exclude windows that are useless (1x1), skip taskbar + pager or override redirect windows
		// from the reported focusable windows to Steam.
		if ( win_is_useless( focusable_window ) ||
			win_skip_taskbar_and_pager( focusable_window ) ||
			focusable_window->a.override_redirect )
			continue;

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

	XChangeProperty( root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeFocusableAppsAtom, XA_CARDINAL, 32, PropModeReplace,
					 (unsigned char *)focusable_appids.data(), focusable_appids.size() );

	XChangeProperty( root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeFocusableWindowsAtom, XA_CARDINAL, 32, PropModeReplace,
					 (unsigned char *)focusable_windows.data(), focusable_windows.size() );

	// Determine global primary focus
	std::stable_sort( vecPossibleFocusWindows.begin(), vecPossibleFocusWindows.end(),
					is_focus_priority_greater );

	gameFocused = pick_primary_focus_and_override(&global_focus, root_ctx->focusControlWindow, vecPossibleFocusWindows, true, vecFocuscontrolAppIDs);

	// Pick overlay/notifications from root ctx
	global_focus.overlayWindow = root_ctx->focus.overlayWindow;
	global_focus.externalOverlayWindow = root_ctx->focus.externalOverlayWindow;
	global_focus.notificationWindow = root_ctx->focus.notificationWindow;

	// Pick inputFocusWindow
	if (global_focus.overlayWindow && global_focus.overlayWindow->inputFocusMode)
	{
		global_focus.inputFocusWindow = global_focus.overlayWindow;
		global_focus.keyboardFocusWindow = global_focus.overlayWindow;
	}
	else
	{
		global_focus.inputFocusWindow = global_focus.focusWindow;
		global_focus.keyboardFocusWindow = global_focus.overrideWindow ? global_focus.overrideWindow : global_focus.focusWindow;
	}

	// Pick cursor from our input focus window

	// Initially pick cursor from the ctx of our input focus.
	if (global_focus.inputFocusWindow)
		global_focus.cursor = global_focus.inputFocusWindow->ctx->cursor.get();

	if (global_focus.inputFocusWindow)
		global_focus.inputFocusMode = global_focus.inputFocusWindow->inputFocusMode;

	if ( global_focus.inputFocusMode == 2 )
	{
		global_focus.keyboardFocusWindow = global_focus.overrideWindow
			? global_focus.overrideWindow
			: global_focus.focusWindow;
	}

	// Tell wlserver about our keyboard/mouse focus.
	if ( global_focus.inputFocusWindow    != previous_focus.inputFocusWindow ||
		 global_focus.keyboardFocusWindow != previous_focus.keyboardFocusWindow )
	{
		if ( win_surface(global_focus.inputFocusWindow)    != nullptr ||
			 win_surface(global_focus.keyboardFocusWindow) != nullptr )
		{
			wlserver_lock();
			if ( win_surface(global_focus.inputFocusWindow) != nullptr )
				wlserver_mousefocus( global_focus.inputFocusWindow->surface.wlr, global_focus.cursor->x(), global_focus.cursor->y() );

			if ( win_surface(global_focus.keyboardFocusWindow) != nullptr )
				wlserver_keyboardfocus( global_focus.keyboardFocusWindow->surface.wlr );
			wlserver_unlock();
		}

		// Hide cursor on transitioning between xwaylands
		// We already do this when transitioning input focus inside of an
		// xwayland ctx.
		// don't care if we change kb focus window due to that happening when
		// going from override -> focus and we don't want to hide then as it's probably a dropdown.
		if ( global_focus.cursor && global_focus.inputFocusWindow != previous_focus.inputFocusWindow )
			global_focus.cursor->hide();
	}

	// Determine if we need to repaints
	if (previous_focus.overlayWindow         != global_focus.overlayWindow         ||
		previous_focus.externalOverlayWindow != global_focus.externalOverlayWindow ||
	    previous_focus.notificationWindow    != global_focus.notificationWindow    ||
		previous_focus.focusWindow           != global_focus.focusWindow           ||
		previous_focus.overrideWindow        != global_focus.overrideWindow)
	{
		hasRepaint = true;
	}

	// Backchannel to Steam
	unsigned long focusedWindow = 0;
	unsigned long focusedAppId = 0;
	unsigned long focusedBaseAppId = 0;
	const char *focused_display = root_ctx->xwayland_server->get_nested_display_name();
	const char *focused_keyboard_display = root_ctx->xwayland_server->get_nested_display_name();
	const char *focused_mouse_display = root_ctx->xwayland_server->get_nested_display_name();

	if ( global_focus.focusWindow )
	{
		focusedWindow = global_focus.focusWindow->id;
		focusedBaseAppId = global_focus.focusWindow->appID;
		focusedAppId = global_focus.inputFocusWindow->appID;
		focused_display = global_focus.focusWindow->ctx->xwayland_server->get_nested_display_name();
		focusWindow_pid = global_focus.focusWindow->pid;
	}

	if ( global_focus.inputFocusWindow )
	{
		focused_mouse_display = global_focus.inputFocusWindow->ctx->xwayland_server->get_nested_display_name();
	}

	if ( global_focus.keyboardFocusWindow )
	{
		focused_keyboard_display = global_focus.keyboardFocusWindow->ctx->xwayland_server->get_nested_display_name();
	}

	if ( steamMode )
	{
		XChangeProperty( root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeFocusedAppAtom, XA_CARDINAL, 32, PropModeReplace,
						(unsigned char *)&focusedAppId, focusedAppId != 0 ? 1 : 0 );

		XChangeProperty( root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeFocusedAppGfxAtom, XA_CARDINAL, 32, PropModeReplace,
						(unsigned char *)&focusedBaseAppId, focusedBaseAppId != 0 ? 1 : 0 );
	}

	XChangeProperty( root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeFocusedWindowAtom, XA_CARDINAL, 32, PropModeReplace,
					 (unsigned char *)&focusedWindow, focusedWindow != 0 ? 1 : 0 );

	XChangeProperty( root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeFocusDisplay, XA_CARDINAL, 32, PropModeReplace,
					 (unsigned char *)focused_display, strlen(focused_display) + 1 );

	XChangeProperty( root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeMouseFocusDisplay, XA_CARDINAL, 32, PropModeReplace,
					 (unsigned char *)focused_mouse_display, strlen(focused_mouse_display) + 1 );

	XChangeProperty( root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeKeyboardFocusDisplay, XA_CARDINAL, 32, PropModeReplace,
					 (unsigned char *)focused_keyboard_display, strlen(focused_keyboard_display) + 1 );

	// Sort out fading.
	if (previous_focus.focusWindow != global_focus.focusWindow)
	{
		if ( g_FadeOutDuration != 0 && !g_bFirstFrame )
		{
			if ( !g_HeldCommits[ HELD_COMMIT_FADE ] )
			{
				global_focus.fadeWindow = previous_focus.focusWindow;
				g_HeldCommits[ HELD_COMMIT_FADE ] = g_HeldCommits[ HELD_COMMIT_BASE ];
				g_bPendingFade = true;
			}
			else
			{
				// If we end up fading back to what we were going to fade to, cancel the fade.
				if ( global_focus.fadeWindow != nullptr && global_focus.focusWindow == global_focus.fadeWindow )
				{
					g_HeldCommits[ HELD_COMMIT_FADE ] = nullptr;
					g_bPendingFade = false;
					fadeOutStartTime = 0;
					global_focus.fadeWindow = nullptr;
				}
			}
		}
	}

	// Update last focus commit
	if ( global_focus.focusWindow &&
		 previous_focus.focusWindow != global_focus.focusWindow &&
		 !global_focus.focusWindow->isSteamStreamingClient )
	{
		get_window_last_done_commit( global_focus.focusWindow, g_HeldCommits[ HELD_COMMIT_BASE ] );
	}

	hasFocusWindow = global_focus.focusWindow != nullptr;

	// Set SDL window title
	if ( global_focus.focusWindow )
		sdlwindow_title( global_focus.focusWindow->title );

	sdlwindow_pushupdate();
	
	focusDirty = false;
}

static void
get_win_type(xwayland_ctx_t *ctx, win *w)
{
	w->is_dialog = !!w->transientFor;

	std::vector<unsigned int> atoms;
	if ( get_prop( ctx, w->id, ctx->atoms.winTypeAtom, atoms ) )
	{
		for ( unsigned int atom : atoms )
		{
			if ( atom == ctx->atoms.winDialogAtom )
			{
				w->is_dialog = true;
			}
			if ( atom == ctx->atoms.winNormalAtom )
			{
				w->is_dialog = false;
			}
		}
	}
}

static void
get_size_hints(xwayland_ctx_t *ctx, win *w)
{
	XSizeHints hints;
	long hintsSpecified = 0;

	XGetWMNormalHints(ctx->dpy, w->id, &hints, &hintsSpecified);

	const bool bHasPositionAndGravityHints = ( hintsSpecified & ( PPosition | PWinGravity ) ) == ( PPosition | PWinGravity );
	if ( bHasPositionAndGravityHints &&
		 hints.x && hints.y && hints.win_gravity == StaticGravity )
	{
		w->maybe_a_dropdown = true;
	}
	else
	{
		w->maybe_a_dropdown = false;
	}

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

			XQueryTree(ctx->dpy, w->id, &root_return, &parent_return, &children, &nchildren);

			if (nchildren == 1)
			{
				XWindowAttributes attribs;

				XGetWindowAttributes(ctx->dpy, children[0], &attribs);

				// If we have a unique children that isn't override-reidrect that is
				// contained inside this fullscreen window, it's probably it.
				if (attribs.override_redirect == false &&
					attribs.width <= w->a.width &&
					attribs.height <= w->a.height)
				{
					w->sizeHintsSpecified = true;

					w->requestedWidth = attribs.width;
					w->requestedHeight = attribs.height;

					XMoveWindow(ctx->dpy, children[0], 0, 0);

					w->ignoreOverrideRedirect = true;
				}
			}

			XFree(children);
		}
	}
}

static void
get_motif_hints( xwayland_ctx_t *ctx, win *w )
{
	if ( w->motif_hints )
		XFree( w->motif_hints );

	Atom actual_type;
	int actual_format;
	unsigned long nitems, bytesafter;
	XGetWindowProperty(ctx->dpy, w->id, ctx->atoms.motifWMHints, 0L, 20L, False,
		ctx->atoms.motifWMHints, &actual_type, &actual_format, &nitems,
		&bytesafter, (unsigned char **)&w->motif_hints );
}

static void
get_win_title(xwayland_ctx_t *ctx, win *w, Atom atom)
{
	assert(atom == XA_WM_NAME || atom == ctx->atoms.netWMNameAtom);

	XTextProperty tp;
	XGetTextProperty( ctx->dpy, w->id, &tp, atom );

	bool is_utf8;
	if (tp.encoding == ctx->atoms.utf8StringAtom) {
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
get_net_wm_state(xwayland_ctx_t *ctx, win *w)
{
	Atom type;
	int format;
	unsigned long nitems;
	unsigned long bytesAfter;
	unsigned char *data;
	if (XGetWindowProperty(ctx->dpy, w->id, ctx->atoms.netWMStateAtom, 0, 2048, false,
			AnyPropertyType, &type, &format, &nitems, &bytesAfter, &data) != Success) {
		return;
	}

	Atom *props = (Atom *)data;
	for (size_t i = 0; i < nitems; i++) {
		if (props[i] == ctx->atoms.netWMStateFullscreenAtom) {
			w->isFullscreen = true;
		} else if (props[i] == ctx->atoms.netWMStateSkipTaskbarAtom) {
			w->skipTaskbar = true;
		} else if (props[i] == ctx->atoms.netWMStateSkipPagerAtom) {
			w->skipPager = true;
		} else {
			xwm_log.debugf("Unhandled initial NET_WM_STATE property: %s", XGetAtomName(ctx->dpy, props[i]));
		}
	}

	XFree(data);
}

static void
map_win(xwayland_ctx_t* ctx, Window id, unsigned long sequence)
{
	win		*w = find_win(ctx, id);

	if (!w)
		return;

	w->a.map_state = IsViewable;

	/* This needs to be here or else we lose transparency messages */
	XSelectInput(ctx->dpy, id, PropertyChangeMask | SubstructureNotifyMask |
		LeaveWindowMask | FocusChangeMask);

	XFlush(ctx->dpy);

	/* This needs to be here since we don't get PropertyNotify when unmapped */
	w->opacity = get_prop(ctx, w->id, ctx->atoms.opacityAtom, OPAQUE);

	w->isSteam = get_prop(ctx, w->id, ctx->atoms.steamAtom, 0);

	/* First try to read the UTF8 title prop, then fallback to the non-UTF8 one */
	get_win_title( ctx, w, ctx->atoms.netWMNameAtom );
	get_win_title( ctx, w, XA_WM_NAME );

	w->inputFocusMode = get_prop(ctx, w->id, ctx->atoms.steamInputFocusAtom, 0);

	w->isSteamStreamingClient = get_prop(ctx, w->id, ctx->atoms.steamStreamingClientAtom, 0);
	w->isSteamStreamingClientVideo = get_prop(ctx, w->id, ctx->atoms.steamStreamingClientVideoAtom, 0);

	if ( steamMode == true )
	{
		uint32_t appID = get_prop(ctx, w->id, ctx->atoms.gameAtom, 0);

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
	w->isOverlay = get_prop(ctx, w->id, ctx->atoms.overlayAtom, 0);
	w->isExternalOverlay = get_prop(ctx, w->id, ctx->atoms.externalOverlayAtom, 0);

	get_size_hints(ctx, w);
	get_motif_hints(ctx, w);

	get_net_wm_state(ctx, w);

	XWMHints *wmHints = XGetWMHints( ctx->dpy, w->id );

	if ( wmHints != nullptr )
	{
		if ( wmHints->flags & (InputHint | StateHint ) && wmHints->input == true && wmHints->initial_state == NormalState )
		{
			XRaiseWindow( ctx->dpy, w->id );
		}

		XFree( wmHints );
	}

	Window transientFor = None;
	if ( XGetTransientForHint( ctx->dpy, w->id, &transientFor ) )
	{
		w->transientFor = transientFor;
	}
	else
	{
		w->transientFor = None;
	}

	get_win_type( ctx, w );

	w->damage_sequence = 0;
	w->map_sequence = sequence;

	focusDirty = true;
}

static void
finish_unmap_win(xwayland_ctx_t *ctx, win *w)
{
	// TODO clear done commits here?

	/* don't care about properties anymore */
	set_ignore(ctx, NextRequest(ctx->dpy));
	XSelectInput(ctx->dpy, w->id, 0);

	ctx->clipChanged = true;
}

static void
unmap_win(xwayland_ctx_t *ctx, Window id, bool fade)
{
	win *w = find_win(ctx, id);
	if (!w)
		return;
	w->a.map_state = IsUnmapped;

	focusDirty = true;

	finish_unmap_win(ctx, w);
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
get_win_pid(xwayland_ctx_t *ctx, Window id)
{
	XResClientIdSpec client_spec = {
		.client = id,
		.mask = XRES_CLIENT_ID_PID_MASK,
	};
	long num_ids = 0;
	XResClientIdValue *client_ids = NULL;
	XResQueryClientIds(ctx->dpy, 1, &client_spec, &num_ids, &client_ids);

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
add_win(xwayland_ctx_t *ctx, Window id, Window prev, unsigned long sequence)
{
	win				*new_win = new win;
	win				**p;

	if (!new_win)
		return;
	if (prev)
	{
		for (p = &ctx->list; *p; p = &(*p)->next)
			if ((*p)->id == prev)
				break;
	}
	else
		p = &ctx->list;
	new_win->id = id;
	set_ignore(ctx, NextRequest(ctx->dpy));
	if (!XGetWindowAttributes(ctx->dpy, id, &new_win->a))
	{
		delete new_win;
		return;
	}

	new_win->ctx = ctx;
	new_win->damage_sequence = 0;
	new_win->map_sequence = 0;
	if (new_win->a.c_class == InputOnly)
		new_win->damage = None;
	else
	{
		set_ignore(ctx, NextRequest(ctx->dpy));
		new_win->damage = XDamageCreate(ctx->dpy, id, XDamageReportRawRectangles);
	}
	new_win->opacity = OPAQUE;

	if ( useXRes == true )
	{
		new_win->pid = get_win_pid(ctx, id);
	}
	else
	{
		new_win->pid = -1;
	}

	new_win->isOverlay = false;
	new_win->isExternalOverlay = false;
	new_win->isSteam = false;
	new_win->isSteamStreamingClient = false;
	new_win->isSteamStreamingClientVideo = false;
	new_win->inputFocusMode = 0;
	new_win->is_dialog = false;
	new_win->maybe_a_dropdown = false;
	new_win->motif_hints = nullptr;

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
	if ( XGetTransientForHint( ctx->dpy, id, &transientFor ) )
	{
		new_win->transientFor = transientFor;
	}
	else
	{
		new_win->transientFor = None;
	}

	get_win_type( ctx, new_win );

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
		map_win(ctx, id, sequence);

	focusDirty = true;
}

static void
restack_win(xwayland_ctx_t *ctx, win *w, Window new_above)
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
		for (prev = &ctx->list; *prev; prev = &(*prev)->next)
		{
			if ((*prev) == w)
				break;
		}
		*prev = w->next;

		/* rehook */
		for (prev = &ctx->list; *prev; prev = &(*prev)->next)
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
configure_win(xwayland_ctx_t *ctx, XConfigureEvent *ce)
{
	win		    *w = find_win(ctx, ce->window);

	if (!w || w->id != ce->window)
	{
		if (ce->window == ctx->root)
		{
			ctx->root_width = ce->width;
			ctx->root_height = ce->height;
			focusDirty = true;

			gamescope_xwayland_server_t *root_server = wlserver_get_xwayland_server(0);
			xwayland_ctx_t *root_ctx = root_server->ctx.get();
			XDeleteProperty( root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeXWaylandModeControl );
			XFlush( root_ctx->dpy );
		}
		return;
	}

	w->a.x = ce->x;
	w->a.y = ce->y;
	w->a.width = ce->width;
	w->a.height = ce->height;
	w->a.border_width = ce->border_width;
	w->a.override_redirect = ce->override_redirect;
	restack_win(ctx, w, ce->above);

	focusDirty = true;
}

static void
circulate_win(xwayland_ctx_t *ctx, XCirculateEvent *ce)
{
	win	    *w = find_win(ctx, ce->window);
	Window  new_above;

	if (!w || w->id != ce->window)
		return;

	if (ce->place == PlaceOnTop)
		new_above = ctx->list->id;
	else
		new_above = None;
	restack_win(ctx, w, new_above);
	ctx->clipChanged = true;
}

static void map_request(xwayland_ctx_t *ctx, XMapRequestEvent *mapRequest)
{
	XMapWindow( ctx->dpy, mapRequest->window );
}

static void configure_request(xwayland_ctx_t *ctx, XConfigureRequestEvent *configureRequest)
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

	XConfigureWindow( ctx->dpy, configureRequest->window, configureRequest->value_mask, &changes );
}

static void circulate_request( xwayland_ctx_t *ctx, XCirculateRequestEvent *circulateRequest )
{
	XCirculateSubwindows( ctx->dpy, circulateRequest->window, circulateRequest->place );
}

static void
finish_destroy_win(xwayland_ctx_t *ctx, Window id, bool gone)
{
	win	**prev, *w;

	for (prev = &ctx->list; (w = *prev); prev = &w->next)
		if (w->id == id)
		{
			if (gone)
				finish_unmap_win (ctx, w);
			*prev = w->next;
			if (w->damage != None)
			{
				set_ignore(ctx, NextRequest(ctx->dpy));
				XDamageDestroy(ctx->dpy, w->damage);
				w->damage = None;
			}

			if (gone)
			{
				// release all commits now we are closed.
                w->commit_queue.clear();
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
destroy_win(xwayland_ctx_t *ctx, Window id, bool gone, bool fade)
{
	// Context
	if (x11_win(ctx->focus.focusWindow) == id && gone)
		ctx->focus.focusWindow = nullptr;
	if (x11_win(ctx->focus.inputFocusWindow) == id && gone)
		ctx->focus.inputFocusWindow = nullptr;
	if (x11_win(ctx->focus.overlayWindow) == id && gone)
		ctx->focus.overlayWindow = nullptr;
	if (x11_win(ctx->focus.externalOverlayWindow) == id && gone)
		ctx->focus.externalOverlayWindow = nullptr;
	if (x11_win(ctx->focus.notificationWindow) == id && gone)
		ctx->focus.notificationWindow = nullptr;
	if (x11_win(ctx->focus.overrideWindow) == id && gone)
		ctx->focus.overrideWindow = nullptr;
	if (ctx->currentKeyboardFocusWindow == id && gone)
		ctx->currentKeyboardFocusWindow = None;

	// Global Focus
	if (x11_win(global_focus.focusWindow) == id && gone)
		global_focus.focusWindow = nullptr;
	if (x11_win(global_focus.inputFocusWindow) == id && gone)
		global_focus.inputFocusWindow = nullptr;
	if (x11_win(global_focus.overlayWindow) == id && gone)
		global_focus.overlayWindow = nullptr;
	if (x11_win(global_focus.notificationWindow) == id && gone)
		global_focus.notificationWindow = nullptr;
	if (x11_win(global_focus.overrideWindow) == id && gone)
		global_focus.overrideWindow = nullptr;
	if (x11_win(global_focus.fadeWindow) == id && gone)
		global_focus.fadeWindow = nullptr;
		
	focusDirty = true;

	finish_destroy_win(ctx, id, gone);
}

static void
damage_win(xwayland_ctx_t *ctx, XDamageNotifyEvent *de)
{
	win	*w = find_win(ctx, de->drawable);
	win *focus = ctx->focus.focusWindow;

	if (!w)
		return;

	if ((w->isOverlay || w->isExternalOverlay) && !w->opacity)
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
		set_ignore(ctx, NextRequest(ctx->dpy));
		XDamageSubtract(ctx->dpy, w->damage, None, None);
	}

	gpuvis_trace_printf( "damage_win win %lx appID %u", w->id, w->appID );
}

static void
handle_wl_surface_id(xwayland_ctx_t *ctx, win *w, uint32_t surfaceID)
{
	struct wlr_surface *surface = NULL;

	wlserver_lock();

	ctx->xwayland_server->set_wl_id( &w->surface, surfaceID );

	surface = w->surface.wlr;
	if ( surface == NULL )
	{
		wlserver_unlock();
		return;
	}

	// If we already focused on our side and are handling this late,
	// let wayland know now.
	if ( w == global_focus.inputFocusWindow )
		wlserver_mousefocus( surface );

	win *keyboardFocusWindow = global_focus.inputFocusWindow;

	if ( keyboardFocusWindow && keyboardFocusWindow->inputFocusMode == 2 )
		keyboardFocusWindow = global_focus.focusWindow;

	if ( w == keyboardFocusWindow )
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
handle_net_wm_state(xwayland_ctx_t *ctx, win *w, XClientMessageEvent *ev)
{
	uint32_t action = (uint32_t)ev->data.l[0];
	Atom *props = (Atom *)&ev->data.l[1];
	for (size_t i = 0; i < 2; i++) {
		if (props[i] == ctx->atoms.netWMStateFullscreenAtom) {
			update_net_wm_state(action, &w->isFullscreen);
			focusDirty = true;
		} else if (props[i] == ctx->atoms.netWMStateSkipTaskbarAtom) {
			update_net_wm_state(action, &w->skipTaskbar);
			focusDirty = true;
		} else if (props[i] == ctx->atoms.netWMStateSkipPagerAtom) {
			update_net_wm_state(action, &w->skipPager);
			focusDirty = true;
		} else if (props[i] != None) {
			xwm_log.debugf("Unhandled NET_WM_STATE property change: %s", XGetAtomName(ctx->dpy, props[i]));
		}
	}
}

bool g_bLowLatency = false;

static void
handle_system_tray_opcode(xwayland_ctx_t *ctx, XClientMessageEvent *ev)
{
	long opcode = ev->data.l[1];

	switch (opcode) {
		case SYSTEM_TRAY_REQUEST_DOCK: {
			Window embed_id = ev->data.l[2];

			/* At this point we're supposed to initiate the XEmbed lifecycle by
			 * sending XEMBED_EMBEDDED_NOTIFY. However we don't actually need to
			 * render the systray, we just want to recognize and blacklist these
			 * icons. So for now do nothing. */

			win *w = find_win(ctx, embed_id);
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
handle_wm_change_state(xwayland_ctx_t *ctx, win *w, XClientMessageEvent *ev)
{
	long state = ev->data.l[0];

	if (state == ICCCM_ICONIC_STATE) {
		/* Wine will request iconic state and cannot ensure that the WM has
		 * agreed on it; immediately revert to normal state to avoid being
		 * stuck in a paused state. */
		xwm_log.debugf("Rejecting WM_CHANGE_STATE to ICONIC for window 0x%lx", w->id);
		uint32_t wmState[] = { ICCCM_NORMAL_STATE, None };
		XChangeProperty(ctx->dpy, w->id, ctx->atoms.WMStateAtom, ctx->atoms.WMStateAtom, 32,
			PropModeReplace, (unsigned char *)wmState,
			sizeof(wmState) / sizeof(wmState[0]));
	} else {
		xwm_log.debugf("Unhandled WM_CHANGE_STATE to %ld for window 0x%lx", state, w->id);
	}
}

static void
handle_client_message(xwayland_ctx_t *ctx, XClientMessageEvent *ev)
{
	if (ev->window == ctx->ourWindow && ev->message_type == ctx->atoms.netSystemTrayOpcodeAtom)
	{
		handle_system_tray_opcode( ctx, ev );
		return;
	}

	win *w = find_win(ctx, ev->window);
	if (w)
	{
		if (ev->message_type == ctx->atoms.WLSurfaceIDAtom)
		{
			handle_wl_surface_id( ctx, w, uint32_t(ev->data.l[0]));
		}
		else if ( ev->message_type == ctx->atoms.activeWindowAtom )
		{
			XRaiseWindow( ctx->dpy, w->id );
		}
		else if ( ev->message_type == ctx->atoms.netWMStateAtom )
		{
			handle_net_wm_state( ctx, w, ev );
		}
		else if ( ev->message_type == ctx->atoms.WMChangeStateAtom )
		{
			handle_wm_change_state( ctx, w, ev );
		}
		else if ( ev->message_type != 0 )
		{
			xwm_log.debugf( "Unhandled client message: %s", XGetAtomName( ctx->dpy, ev->message_type ) );
		}
	}
}


template<typename T, typename J>
T bit_cast(const J& src) {
	T dst;
	memcpy(&dst, &src, sizeof(T));
	return dst;
}

static void
update_runtime_info()
{
	if ( g_nRuntimeInfoFd < 0 )
		return;

	uint32_t limiter_enabled = g_nSteamCompMgrTargetFPS != 0 ? 1 : 0;
	pwrite( g_nRuntimeInfoFd, &limiter_enabled, sizeof( limiter_enabled ), 0 );
}

static void
init_runtime_info()
{
	const char *path = getenv( "GAMESCOPE_LIMITER_FILE" );
	if ( !path )
		return;

	g_nRuntimeInfoFd = open( path, O_CREAT | O_RDWR , 0644 );
	update_runtime_info();
}

static void
handle_property_notify(xwayland_ctx_t *ctx, XPropertyEvent *ev)
{
	/* check if Trans property was changed */
	if (ev->atom == ctx->atoms.opacityAtom)
	{
		/* reset mode and redraw window */
		win * w = find_win(ctx, ev->window);
		if ( w != nullptr )
		{
			unsigned int newOpacity = get_prop(ctx, w->id, ctx->atoms.opacityAtom, OPAQUE);

			if (newOpacity != w->opacity)
			{
				w->opacity = newOpacity;

				if ( gameFocused && ( w == ctx->focus.overlayWindow || w == ctx->focus.notificationWindow ) )
				{
					hasRepaint = true;
				}
				if ( w == ctx->focus.externalOverlayWindow )
				{
					hasRepaint = true;
				}
			}

			unsigned int maxOpacity = 0;
			unsigned int maxOpacityExternal = 0;

			for (w = ctx->list; w; w = w->next)
			{
				if (w->isOverlay)
				{
					if (w->a.width > 1200 && w->opacity >= maxOpacity)
					{
						ctx->focus.overlayWindow = w;
						maxOpacity = w->opacity;
					}
				}
				if (w->isExternalOverlay)
				{
					if (w->opacity >= maxOpacityExternal)
					{
						ctx->focus.externalOverlayWindow = w;
						maxOpacityExternal = w->opacity;
					}
				}
			}
		}
	}
	if (ev->atom == ctx->atoms.steamAtom)
	{
		win * w = find_win(ctx, ev->window);
		if (w)
		{
			w->isSteam = get_prop(ctx, w->id, ctx->atoms.steamAtom, 0);
			focusDirty = true;
		}
	}
	if (ev->atom == ctx->atoms.steamInputFocusAtom )
	{
		win * w = find_win(ctx, ev->window);
		if (w)
		{
			w->inputFocusMode = get_prop(ctx, w->id, ctx->atoms.steamInputFocusAtom, 0);
			focusDirty = true;
		}
	}
	if (ev->atom == ctx->atoms.steamTouchClickModeAtom )
	{
		g_nTouchClickMode = (enum wlserver_touch_click_mode) get_prop(ctx, ctx->root, ctx->atoms.steamTouchClickModeAtom, g_nDefaultTouchClickMode );
	}
	if (ev->atom == ctx->atoms.steamStreamingClientAtom)
	{
		win * w = find_win(ctx, ev->window);
		if (w)
		{
			w->isSteamStreamingClient = get_prop(ctx, w->id, ctx->atoms.steamStreamingClientAtom, 0);
			focusDirty = true;
		}
	}
	if (ev->atom == ctx->atoms.steamStreamingClientVideoAtom)
	{
		win * w = find_win(ctx, ev->window);
		if (w)
		{
			w->isSteamStreamingClientVideo = get_prop(ctx, w->id, ctx->atoms.steamStreamingClientVideoAtom, 0);
			focusDirty = true;
		}
	}
	if (ev->atom == ctx->atoms.gamescopeCtrlAppIDAtom )
	{
		get_prop( ctx, ctx->root, ctx->atoms.gamescopeCtrlAppIDAtom, vecFocuscontrolAppIDs );
		focusDirty = true;
	}
	if (ev->atom == ctx->atoms.gamescopeCtrlWindowAtom )
	{
		ctx->focusControlWindow = get_prop( ctx, ctx->root, ctx->atoms.gamescopeCtrlWindowAtom, None );
		focusDirty = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeScreenShotAtom )
	{
		if ( ev->state == PropertyNewValue )
		{
			g_bTakeScreenshot = true;
			g_bPropertyRequestedScreenshot = true;
		}
	}
	if (ev->atom == ctx->atoms.gameAtom)
	{
		win * w = find_win(ctx, ev->window);
		if (w)
		{
			uint32_t appID = get_prop(ctx, w->id, ctx->atoms.gameAtom, 0);

			if ( w->appID != 0 && appID != 0 && w->appID != appID )
			{
				xwm_log.errorf( "appid clash was %u now %u", w->appID, appID );
			}
			w->appID = appID;

			focusDirty = true;
		}
	}
	if (ev->atom == ctx->atoms.overlayAtom)
	{
		win * w = find_win(ctx, ev->window);
		if (w)
		{
			w->isOverlay = get_prop(ctx, w->id, ctx->atoms.overlayAtom, 0);
			focusDirty = true;
		}
	}
	if (ev->atom == ctx->atoms.externalOverlayAtom)
	{
		win * w = find_win(ctx, ev->window);
		if (w)
		{
			w->isExternalOverlay = get_prop(ctx, w->id, ctx->atoms.externalOverlayAtom, 0);
			focusDirty = true;
		}
	}
	if (ev->atom == ctx->atoms.winTypeAtom)
	{
		win * w = find_win(ctx, ev->window);
		if (w)
		{
			get_win_type(ctx, w);
			focusDirty = true;
		}		
	}
	if (ev->atom == ctx->atoms.sizeHintsAtom)
	{
		win * w = find_win(ctx, ev->window);
		if (w)
		{
			get_size_hints(ctx, w);
			focusDirty = true;
		}
	}
	if (ev->atom == ctx->atoms.gamesRunningAtom)
	{
		gamesRunningCount = get_prop(ctx, ctx->root, ctx->atoms.gamesRunningAtom, 0);

		focusDirty = true;
	}
	if (ev->atom == ctx->atoms.screenScaleAtom)
	{
		overscanScaleRatio = get_prop(ctx, ctx->root, ctx->atoms.screenScaleAtom, 0xFFFFFFFF) / (double)0xFFFFFFFF;

		globalScaleRatio = overscanScaleRatio * zoomScaleRatio;

		if (global_focus.focusWindow)
		{
			hasRepaint = true;
		}

		focusDirty = true;
	}
	if (ev->atom == ctx->atoms.screenZoomAtom)
	{
		zoomScaleRatio = get_prop(ctx, ctx->root, ctx->atoms.screenZoomAtom, 0xFFFF) / (double)0xFFFF;

		globalScaleRatio = overscanScaleRatio * zoomScaleRatio;

		if (global_focus.focusWindow)
		{
			hasRepaint = true;
		}

		focusDirty = true;
	}
	if (ev->atom == ctx->atoms.WMTransientForAtom)
	{
		win * w = find_win(ctx, ev->window);
		if (w)
		{
			Window transientFor = None;
			if ( XGetTransientForHint( ctx->dpy, ev->window, &transientFor ) )
			{
				w->transientFor = transientFor;
			}
			else
			{
				w->transientFor = None;
			}
			get_win_type( ctx, w );

			focusDirty = true;
		}
	}
	if (ev->atom == XA_WM_NAME || ev->atom == ctx->atoms.netWMNameAtom)
	{
		if (ev->window == x11_win(global_focus.focusWindow))
		{
			win *w = find_win(ctx, ev->window);
			if (w) {
				get_win_title(ctx, w, ev->atom);
				sdlwindow_title( w->title );
			}
		}
	}
	if (ev->atom == ctx->atoms.motifWMHints)
	{
		win *w = find_win(ctx, ev->window);
		if (w) {
			get_motif_hints(ctx, w);
			focusDirty = true;
		}
	}
	if ( ev->atom == ctx->atoms.gamescopeTuneableVBlankRedZone )
	{
		g_uVblankDrawBufferRedZoneNS = (uint64_t)get_prop( ctx, ctx->root, ctx->atoms.gamescopeTuneableVBlankRedZone, g_uDefaultVBlankRedZone );
	}
	if ( ev->atom == ctx->atoms.gamescopeTuneableRateOfDecay )
	{
		g_uVBlankRateOfDecayPercentage = (uint64_t)get_prop( ctx, ctx->root, ctx->atoms.gamescopeTuneableRateOfDecay, g_uDefaultVBlankRateOfDecayPercentage );
	}
	if ( ev->atom == ctx->atoms.gamescopeScalingFilter )
	{
		int nScalingMode = get_prop( ctx, ctx->root, ctx->atoms.gamescopeScalingFilter, 0 );
		switch ( nScalingMode )
		{
		default:
		case 0:
			g_bFilterGameWindow = true;
			g_bIntegerScale = false;
			g_upscaler = GamescopeUpscaler::BLIT;
			break;
		case 1:
			g_bFilterGameWindow = false;
			g_bIntegerScale = false;
			g_upscaler = GamescopeUpscaler::BLIT;
			break;
		case 2:
			g_bFilterGameWindow = false;
			g_bIntegerScale = true;
			g_upscaler = GamescopeUpscaler::BLIT;
			break;
		case 3:
			g_bFilterGameWindow = true;
			g_bIntegerScale = false;
			g_upscaler = GamescopeUpscaler::FSR;
			break;
		case 4:
			g_bFilterGameWindow = true;
			g_bIntegerScale = false;
			g_upscaler = GamescopeUpscaler::NIS;
			break;
		}
		hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeFSRSharpness || ev->atom == ctx->atoms.gamescopeSharpness )
	{
		g_upscalerSharpness = (int)clamp( get_prop( ctx, ctx->root, ev->atom, 2 ), 0u, 20u );
		if ( g_upscaler != GamescopeUpscaler::BLIT )
			hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeColorLinearGain )
	{
		std::vector< uint32_t > user_gains;
		bool bHasColor = get_prop( ctx, ctx->root, ctx->atoms.gamescopeColorLinearGain, user_gains );
		
		float gains[3] = { 1.0f, 1.0f, 1.0f };
		if ( bHasColor && user_gains.size() == 3 )
		{
			for (int i = 0; i < 3; i++)
				gains[i] = bit_cast<float>( user_gains[i] );
		}

		if ( drm_set_color_linear_gains( &g_DRM, gains ) )
			hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeColorGain )
	{
		std::vector< uint32_t > user_gains;
		bool bHasColor = get_prop( ctx, ctx->root, ctx->atoms.gamescopeColorGain, user_gains );
		
		float gains[3] = { 1.0f, 1.0f, 1.0f };
		if ( bHasColor && user_gains.size() == 3 )
		{
			for (int i = 0; i < 3; i++)
				gains[i] = bit_cast<float>( user_gains[i] );
		}

		if ( drm_set_color_gains( &g_DRM, gains ) )
			hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeColorMatrix )
	{
		std::vector< uint32_t > user_mtx;
		bool bHasMatrix = get_prop( ctx, ctx->root, ctx->atoms.gamescopeColorMatrix, user_mtx );
		
		// identity
		float mtx[9] =
		{
			1.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 1.0f,
		};
		if ( bHasMatrix && user_mtx.size() == 9 )
		{
			for (int i = 0; i < 9; i++)
				mtx[i] = bit_cast<float>( user_mtx[i] );
		}

		if ( drm_set_color_mtx( &g_DRM, mtx ) )
			hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeColorLinearGainBlend )
	{
		float flBlend = bit_cast<float>(get_prop(ctx, ctx->root, ctx->atoms.gamescopeColorLinearGainBlend, 0));
		if ( drm_set_color_gain_blend( &g_DRM, flBlend ) )
			hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeColorGammaExponent )
	{
		std::vector< uint32_t > user_vec;
		bool bHasVec = get_prop( ctx, ctx->root, ctx->atoms.gamescopeColorGammaExponent, user_vec );
		
		// identity
		float vec[6] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
		if ( bHasVec && user_vec.size() == 6 )
		{
			for (int i = 0; i < 6; i++)
				vec[i] = bit_cast<float>( user_vec[i] );
		}

		if ( drm_set_degamma_exponent( &g_DRM, &vec[0] ) )
			hasRepaint = true;

		if ( drm_set_gamma_exponent( &g_DRM, &vec[3] ) )
			hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeXWaylandModeControl )
	{
		std::vector< uint32_t > xwayland_mode_ctl;
		bool hasModeCtrl = get_prop( ctx, ctx->root, ctx->atoms.gamescopeXWaylandModeControl, xwayland_mode_ctl );
		if ( hasModeCtrl && xwayland_mode_ctl.size() == 4 )
		{
			size_t server_idx = size_t{ xwayland_mode_ctl[ 0 ] };
			int width = xwayland_mode_ctl[ 1 ];
			int height = xwayland_mode_ctl[ 2 ];
			bool allowSuperRes = !!xwayland_mode_ctl[ 3 ];

			if ( !allowSuperRes )
			{
				width = std::min<int>(width, currentOutputWidth);
				height = std::min<int>(height, currentOutputHeight);
			}

			gamescope_xwayland_server_t *server = wlserver_get_xwayland_server( server_idx );
			if ( server )
			{
				bool root_size_identical = server->ctx->root_width == width && server->ctx->root_height == height;

				wlserver_lock();
				wlserver_set_xwayland_server_mode( server_idx, width, height, g_nOutputRefresh );
				wlserver_unlock();

				if ( root_size_identical )
				{
					gamescope_xwayland_server_t *root_server = wlserver_get_xwayland_server(0);
					xwayland_ctx_t *root_ctx = root_server->ctx.get();
					XDeleteProperty( root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeXWaylandModeControl );
					XFlush( root_ctx->dpy );
				}
			}
		}
	}
	if ( ev->atom == ctx->atoms.gamescopeFPSLimit )
	{
		g_nSteamCompMgrTargetFPS = get_prop( ctx, ctx->root, ctx->atoms.gamescopeFPSLimit, 0 );
		update_runtime_info();
	}
	if ( ev->atom == ctx->atoms.gamescopeDynamicRefresh )
	{
		g_nDynamicRefreshRate = get_prop( ctx, ctx->root, ctx->atoms.gamescopeDynamicRefresh, 0 );
	}
	if ( ev->atom == ctx->atoms.gamescopeLowLatency )
	{
		g_bLowLatency = !!get_prop( ctx, ctx->root, ctx->atoms.gamescopeLowLatency, 0 );
	}
	if ( ev->atom == ctx->atoms.gamescopeBlurMode )
	{
		BlurMode newBlur = (BlurMode)get_prop( ctx, ctx->root, ctx->atoms.gamescopeBlurMode, 0 );
		if (newBlur < BLUR_MODE_OFF || newBlur > BLUR_MODE_ALWAYS)
			newBlur = BLUR_MODE_OFF;

		if (newBlur != g_BlurMode) {
			g_BlurFadeStartTime = get_time_in_milliseconds();
			g_BlurModeOld = g_BlurMode;
			g_BlurMode = newBlur;
			hasRepaint = true;
		}
	}
	if ( ev->atom == ctx->atoms.gamescopeBlurRadius )
	{
		unsigned int pixel = get_prop( ctx, ctx->root, ctx->atoms.gamescopeBlurRadius, 0 );
		g_BlurRadius = (int)clamp((pixel / 2) + 1, 1u, kMaxBlurRadius - 1);
		if ( g_BlurMode )
			hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeBlurFadeDuration )
	{
		g_BlurFadeDuration = get_prop( ctx, ctx->root, ctx->atoms.gamescopeBlurFadeDuration, 0 );
	}
}

static int
error(Display *dpy, XErrorEvent *ev)
{
	xwayland_ctx_t *ctx = NULL;
	// Find ctx for dpy
	{
		gamescope_xwayland_server_t *server = NULL;
		for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
		{
			if (server->ctx->dpy == dpy)
			{
				ctx = server->ctx.get();
				break;
			}
		}
	}
	if ( !ctx )
	{
		// Not for us!
		return 0;
	}
	int	    o;
	const char    *name = NULL;
	static char buffer[256];

	if (should_ignore(ctx, ev->serial))
		return 0;

	if (ev->request_code == ctx->composite_opcode &&
		ev->minor_code == X_CompositeRedirectSubwindows)
	{
		xwm_log.errorf("Another composite manager is already running");
		exit(1);
	}

	o = ev->error_code - ctx->xfixes_error;
	switch (o) {
		case BadRegion: name = "BadRegion";	break;
		default: break;
	}
	o = ev->error_code - ctx->damage_error;
	switch (o) {
		case BadDamage: name = "BadDamage";	break;
		default: break;
	}
	o = ev->error_code - ctx->render_error;
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
		XGetErrorText(ctx->dpy, ev->error_code, buffer, sizeof(buffer));
		name = buffer;
	}

	xwm_log.errorf("error %d: %s request %d minor %d serial %lu",
			 ev->error_code, (strlen(name) > 0) ? name : "unknown",
			 ev->request_code, ev->minor_code, ev->serial);

	gotXError = true;
	/*    abort();	    this is just annoying to most people */
	return 0;
}

[[noreturn]] static void
steamcompmgr_exit(void)
{
	// Clean up any commits.
	{
		gamescope_xwayland_server_t *server = NULL;
		for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
		{
			for ( win *w = server->ctx->list; w; w = w->next )
				w->commit_queue.clear();
		}
	}
	g_HeldCommits[ HELD_COMMIT_BASE ] = nullptr;
	g_HeldCommits[ HELD_COMMIT_FADE ] = nullptr;

	imageWaitThreadRun = false;
	waitListSem.signal();

	if ( statsThreadRun == true )
	{
		statsThreadRun = false;
		statsThreadSem.signal();
	}

	finish_drm( &g_DRM );

	pthread_exit(NULL);
}

static int
handle_io_error(Display *dpy)
{
	xwm_log.errorf("X11 I/O error");
	steamcompmgr_exit();
}

static bool
register_cm(xwayland_ctx_t *ctx)
{
	Window w;
	Atom a;
	static char net_wm_cm[] = "_NET_WM_CM_Sxx";

	snprintf(net_wm_cm, sizeof(net_wm_cm), "_NET_WM_CM_S%d", ctx->scr);
	a = XInternAtom(ctx->dpy, net_wm_cm, false);

	w = XGetSelectionOwner(ctx->dpy, a);
	if (w != None)
	{
		XTextProperty tp;
		char **strs;
		int count;
		Atom winNameAtom = XInternAtom(ctx->dpy, "_NET_WM_NAME", false);

		if (!XGetTextProperty(ctx->dpy, w, &tp, winNameAtom) &&
			!XGetTextProperty(ctx->dpy, w, &tp, XA_WM_NAME))
		{
			xwm_log.errorf("Another composite manager is already running (0x%lx)", (unsigned long) w);
			return false;
		}
		if (XmbTextPropertyToTextList(ctx->dpy, &tp, &strs, &count) == Success)
		{
			xwm_log.errorf("Another composite manager is already running (%s)", strs[0]);

			XFreeStringList(strs);
		}

		XFree(tp.value);

		return false;
	}

	w = XCreateSimpleWindow(ctx->dpy, RootWindow(ctx->dpy, ctx->scr), 0, 0, 1, 1, 0, None,
							 None);

	Xutf8SetWMProperties(ctx->dpy, w, "steamcompmgr", "steamcompmgr", NULL, 0, NULL, NULL,
						  NULL);

	Atom atomWmCheck = XInternAtom(ctx->dpy, "_NET_SUPPORTING_WM_CHECK", false);
	XChangeProperty(ctx->dpy, ctx->root, atomWmCheck,
					XA_WINDOW, 32, PropModeReplace, (unsigned char *)&w, 1);
	XChangeProperty(ctx->dpy, w, atomWmCheck,
					XA_WINDOW, 32, PropModeReplace, (unsigned char *)&w, 1);


	Atom supportedAtoms[] = {
		XInternAtom(ctx->dpy, "_NET_WM_STATE", false),
		XInternAtom(ctx->dpy, "_NET_WM_STATE_FULLSCREEN", false),
		XInternAtom(ctx->dpy, "_NET_WM_STATE_SKIP_TASKBAR", false),
		XInternAtom(ctx->dpy, "_NET_WM_STATE_SKIP_PAGER", false),
		XInternAtom(ctx->dpy, "_NET_ACTIVE_WINDOW", false),
	};

	XChangeProperty(ctx->dpy, ctx->root, XInternAtom(ctx->dpy, "_NET_SUPPORTED", false),
					XA_ATOM, 32, PropModeAppend, (unsigned char *)supportedAtoms,
					sizeof(supportedAtoms) / sizeof(supportedAtoms[0]));

	XSetSelectionOwner(ctx->dpy, a, w, 0);

	ctx->ourWindow = w;

	return true;
}

static void
register_systray(xwayland_ctx_t *ctx)
{
	static char net_system_tray_name[] = "_NET_SYSTEM_TRAY_Sxx";

	snprintf(net_system_tray_name, sizeof(net_system_tray_name),
			 "_NET_SYSTEM_TRAY_S%d", ctx->scr);
	Atom net_system_tray = XInternAtom(ctx->dpy, net_system_tray_name, false);

	XSetSelectionOwner(ctx->dpy, net_system_tray, ctx->ourWindow, 0);
}

void handle_done_commits( xwayland_ctx_t *ctx )
{
	std::lock_guard<std::mutex> lock( ctx->listCommitsDoneLock );

	// very fast loop yes
	for ( uint32_t i = 0; i < ctx->listCommitsDone.size(); i++ )
	{
		bool bFoundWindow = false;
		for ( win *w = ctx->list; w; w = w->next )
		{
			uint32_t j;
			for ( j = 0; j < w->commit_queue.size(); j++ )
			{
				if ( w->commit_queue[ j ]->commitID == ctx->listCommitsDone[ i ] )
				{
					gpuvis_trace_printf( "commit %lu done", w->commit_queue[ j ]->commitID );
					w->commit_queue[ j ]->done = true;
					bFoundWindow = true;

					// Window just got a new available commit, determine if that's worth a repaint

					// If this is an overlay that we're presenting, repaint
					if ( gameFocused )
					{
						if ( w == global_focus.overlayWindow && w->opacity != TRANSLUCENT )
						{
							hasRepaint = true;
						}

						if ( w == global_focus.notificationWindow && w->opacity != TRANSLUCENT )
						{
							hasRepaint = true;
						}
					}
					if ( ctx->focus.outdatedInteractiveFocus )
					{
						focusDirty = true;
						ctx->focus.outdatedInteractiveFocus = false;
					}
					// If this is an external overlay, repaint
					if ( w == ctx->focus.externalOverlayWindow && w->opacity != TRANSLUCENT )
					{
						hasRepaint = true;
					}
					// If this is the main plane, repaint
					if ( w == global_focus.focusWindow && !w->isSteamStreamingClient )
					{
						g_HeldCommits[ HELD_COMMIT_BASE ] = w->commit_queue[ j ];
						hasRepaint = true;
					}

					if ( w == global_focus.overrideWindow )
					{
						hasRepaint = true;
					}

					if ( w->isSteamStreamingClientVideo && global_focus.focusWindow && global_focus.focusWindow->isSteamStreamingClient )
					{
						g_HeldCommits[ HELD_COMMIT_BASE ] = w->commit_queue[ j ];
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
					w->commit_queue.erase( w->commit_queue.begin(), w->commit_queue.begin() + j );
				}
				break;
			}
		}
	}

	ctx->listCommitsDone.clear();
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

void check_new_wayland_res(xwayland_ctx_t *ctx)
{
	// When importing buffer, we'll potentially need to perform operations with
	// a wlserver lock (e.g. wlr_buffer_lock). We can't do this with a
	// wayland_commit_queue lock because that causes deadlocks.
	std::vector<ResListEntry_t> tmp_queue = ctx->xwayland_server->retrieve_commits();

	for ( uint32_t i = 0; i < tmp_queue.size(); i++ )
	{
		struct wlr_buffer *buf = tmp_queue[ i ].buf;

		win	*w = find_win( ctx, tmp_queue[ i ].surf );

		if ( w == nullptr )
		{
			wlserver_lock();
			wlr_buffer_unlock( buf );
			wlserver_unlock();
			xwm_log.errorf( "waylandres but no win" );
			continue;
		}

		std::shared_ptr<commit_t> newCommit = import_commit( buf );

		int fence = -1;
		if ( newCommit )
		{
			struct wlr_dmabuf_attributes dmabuf = {0};
			if ( wlr_buffer_get_dmabuf( buf, &dmabuf ) )
			{
				fence = dup( dmabuf.fd[0] );
			}
			else
			{
				fence = newCommit->vulkanTex->memoryFence();
			}

			// Whether or not to nudge mango app when this commit is done.
			const bool mango_nudge = ( w == global_focus.focusWindow && !w->isSteamStreamingClient ) ||
									 ( global_focus.focusWindow && global_focus.focusWindow->isSteamStreamingClient && w->isSteamStreamingClientVideo );

			gpuvis_trace_printf( "pushing wait for commit %lu win %lx", newCommit->commitID, w->id );
			{
				std::unique_lock< std::mutex > lock( waitListLock );
				WaitListEntry_t entry
				{
					.ctx = ctx,
					.fence = fence,
					.mangoapp_nudge = mango_nudge,
					.commitID = newCommit->commitID,
				};
				waitList.push_back( entry );
			}

			// Wake up commit wait thread if chilling
			waitListSem.signal();

			w->commit_queue.push_back( std::move(newCommit) );
		}
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
			if ( g_bRt ==  true ){
				sched_setscheduler(0, g_nOldPolicy, &g_schedOldParam);
			}
			nice( g_nOldNice - g_nNewNice );
		}

		// Restore prior rlimit in case child uses select()
		restore_fd_limit();

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
dispatch_x11( xwayland_ctx_t *ctx )
{
	MouseCursor *cursor = ctx->cursor.get();
	bool bShouldResetCursor = false;
	bool bSetFocus = false;

	while (XPending(ctx->dpy))
	{
		XEvent ev;
		int ret = XNextEvent(ctx->dpy, &ev);
		if (ret != 0)
		{
			xwm_log.errorf("XNextEvent failed");
			break;
		}
		if ((ev.type & 0x7f) != KeymapNotify)
			discard_ignore(ctx, ev.xany.serial);
		if (debugEvents)
		{
			gpuvis_trace_printf("event %d", ev.type);
			printf("event %d\n", ev.type);
		}
		switch (ev.type) {
			case CreateNotify:
				if (ev.xcreatewindow.parent == ctx->root)
					add_win(ctx, ev.xcreatewindow.window, 0, ev.xcreatewindow.serial);
				break;
			case ConfigureNotify:
				configure_win(ctx, &ev.xconfigure);
				break;
			case DestroyNotify:
			{
				win * w = find_win(ctx, ev.xdestroywindow.window);

				if (w && w->id == ev.xdestroywindow.window)
					destroy_win(ctx, ev.xdestroywindow.window, true, true);
				break;
			}
			case MapNotify:
			{
				win * w = find_win(ctx, ev.xmap.window);

				if (w && w->id == ev.xmap.window)
					map_win(ctx, ev.xmap.window, ev.xmap.serial);
				break;
			}
			case UnmapNotify:
			{
				win * w = find_win(ctx, ev.xunmap.window);

				if (w && w->id == ev.xunmap.window)
					unmap_win(ctx, ev.xunmap.window, true);
				break;
			}
			case FocusOut:
			{
				win * w = find_win( ctx, ev.xfocus.window );

				// If focus escaped the current desired keyboard focus window, check where it went
				if ( w && w->id == ctx->currentKeyboardFocusWindow )
				{
					Window newKeyboardFocus = None;
					int nRevertMode = 0;
					XGetInputFocus( ctx->dpy, &newKeyboardFocus, &nRevertMode );

					// Find window or its toplevel parent
					win *kbw = find_win( ctx, newKeyboardFocus );

					if ( kbw )
					{
						if ( kbw->id == ctx->currentKeyboardFocusWindow )
						{
							// focus went to a child, this is fine, make note of it in case we need to fix it
							ctx->currentKeyboardFocusWindow = newKeyboardFocus;
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
				if (ev.xreparent.parent == ctx->root)
					add_win(ctx, ev.xreparent.window, 0, ev.xreparent.serial);
				else
				{
					win * w = find_win(ctx, ev.xreparent.window);

					if (w && w->id == ev.xreparent.window)
					{
						destroy_win(ctx, ev.xreparent.window, false, true);
					}
					else
					{
						// If something got reparented _to_ a toplevel window,
						// go check for the fullscreen workaround again.
						w = find_win(ctx, ev.xreparent.parent);
						if (w)
						{
							get_size_hints(ctx, w);
							focusDirty = true;
						}
					}
				}
				break;
			case CirculateNotify:
				circulate_win(ctx, &ev.xcirculate);
				break;
			case MapRequest:
				map_request(ctx, &ev.xmaprequest);
				break;
			case ConfigureRequest:
				configure_request(ctx, &ev.xconfigurerequest);
				break;
			case CirculateRequest:
				circulate_request(ctx, &ev.xcirculaterequest);
				break;
			case Expose:
				break;
			case PropertyNotify:
				handle_property_notify(ctx, &ev.xproperty);
				break;
			case ClientMessage:
				handle_client_message(ctx, &ev.xclient);
				break;
			case LeaveNotify:
				if (ev.xcrossing.window == x11_win(ctx->focus.inputFocusWindow) &&
					!ctx->focus.overrideWindow)
				{
					// Josh: need to defer this as we could have a destroy later on
					// and end up submitting commands with the currentInputFocusWIndow
					bShouldResetCursor = true;
				}
				break;
			default:
				if (ev.type == ctx->damage_event + XDamageNotify)
				{
					damage_win(ctx, (XDamageNotifyEvent *) &ev);
				}
				else if (ev.type == ctx->xfixes_event + XFixesCursorNotify)
				{
					cursor->setDirty();
				}
				break;
		}
		XFlush(ctx->dpy);
	}

	if ( bShouldResetCursor )
	{
		// This shouldn't happen due to our pointer barriers,
		// but there is a known X server bug; warp to last good
		// position.
		cursor->resetPosition();
	}

	if ( bSetFocus )
	{
		XSetInputFocus(ctx->dpy, ctx->currentKeyboardFocusWindow, RevertToNone, CurrentTime);
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

		g_SteamCompMgrVBlankTime = vblanktime;
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

enum steamcompmgr_event_type {
	EVENT_VBLANK,
	EVENT_NUDGE,
	EVENT_X11,
	// Any past here are X11
};

const char* g_customCursorPath = nullptr;
int g_customCursorHotspotX = 0;
int g_customCursorHotspotY = 0;

xwayland_ctx_t g_ctx;

static bool setup_error_handlers = false;

void init_xwayland_ctx(gamescope_xwayland_server_t *xwayland_server)
{
	assert(!xwayland_server->ctx);
	xwayland_server->ctx = std::make_unique<xwayland_ctx_t>();
	xwayland_ctx_t *ctx = xwayland_server->ctx.get();
	ctx->ignore_tail = &ctx->ignore_head;

	int	composite_major, composite_minor;
	int	xres_major, xres_minor;

	ctx->xwayland_server = xwayland_server;
	ctx->dpy = xwayland_server->get_xdisplay();
	if (!ctx->dpy)
	{
		xwm_log.errorf("Can't open display");
		exit(1);
	}

	if (!setup_error_handlers)
	{
		XSetErrorHandler(error);
		XSetIOErrorHandler(handle_io_error);
		setup_error_handlers = true;
	}

	if (synchronize)
		XSynchronize(ctx->dpy, 1);

	ctx->scr = DefaultScreen(ctx->dpy);
	ctx->root = RootWindow(ctx->dpy, ctx->scr);

	if (!XRenderQueryExtension(ctx->dpy, &ctx->render_event, &ctx->render_error))
	{
		xwm_log.errorf("No render extension");
		exit(1);
	}
	if (!XQueryExtension(ctx->dpy, COMPOSITE_NAME, &ctx->composite_opcode,
		&ctx->composite_event, &ctx->composite_error))
	{
		xwm_log.errorf("No composite extension");
		exit(1);
	}
	XCompositeQueryVersion(ctx->dpy, &composite_major, &composite_minor);

	if (!XDamageQueryExtension(ctx->dpy, &ctx->damage_event, &ctx->damage_error))
	{
		xwm_log.errorf("No damage extension");
		exit(1);
	}
	if (!XFixesQueryExtension(ctx->dpy, &ctx->xfixes_event, &ctx->xfixes_error))
	{
		xwm_log.errorf("No XFixes extension");
		exit(1);
	}
	if (!XShapeQueryExtension(ctx->dpy, &ctx->xshape_event, &ctx->xshape_error))
	{
		xwm_log.errorf("No XShape extension");
		exit(1);
	}
	if (!XFixesQueryExtension(ctx->dpy, &ctx->xfixes_event, &ctx->xfixes_error))
	{
		xwm_log.errorf("No XFixes extension");
		exit(1);
	}
	if (!XResQueryVersion(ctx->dpy, &xres_major, &xres_minor))
	{
		xwm_log.errorf("No XRes extension");
		exit(1);
	}
	if (xres_major != 1 || xres_minor < 2)
	{
		xwm_log.errorf("Unsupported XRes version: have %d.%d, want 1.2", xres_major, xres_minor);
		exit(1);
	}

	if (!register_cm(ctx))
	{
		exit(1);
	}

	register_systray(ctx);

	/* get atoms */
	ctx->atoms.steamAtom = XInternAtom(ctx->dpy, STEAM_PROP, false);
	ctx->atoms.steamInputFocusAtom = XInternAtom(ctx->dpy, "STEAM_INPUT_FOCUS", false);
	ctx->atoms.steamTouchClickModeAtom = XInternAtom(ctx->dpy, "STEAM_TOUCH_CLICK_MODE", false);
	ctx->atoms.gameAtom = XInternAtom(ctx->dpy, GAME_PROP, false);
	ctx->atoms.overlayAtom = XInternAtom(ctx->dpy, OVERLAY_PROP, false);
	ctx->atoms.externalOverlayAtom = XInternAtom(ctx->dpy, EXTERNAL_OVERLAY_PROP, false);
	ctx->atoms.opacityAtom = XInternAtom(ctx->dpy, OPACITY_PROP, false);
	ctx->atoms.gamesRunningAtom = XInternAtom(ctx->dpy, GAMES_RUNNING_PROP, false);
	ctx->atoms.screenScaleAtom = XInternAtom(ctx->dpy, SCREEN_SCALE_PROP, false);
	ctx->atoms.screenZoomAtom = XInternAtom(ctx->dpy, SCREEN_MAGNIFICATION_PROP, false);
	ctx->atoms.winTypeAtom = XInternAtom(ctx->dpy, "_NET_WM_WINDOW_TYPE", false);
	ctx->atoms.winDesktopAtom = XInternAtom(ctx->dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", false);
	ctx->atoms.winDockAtom = XInternAtom(ctx->dpy, "_NET_WM_WINDOW_TYPE_DOCK", false);
	ctx->atoms.winToolbarAtom = XInternAtom(ctx->dpy, "_NET_WM_WINDOW_TYPE_TOOLBAR", false);
	ctx->atoms.winMenuAtom = XInternAtom(ctx->dpy, "_NET_WM_WINDOW_TYPE_MENU", false);
	ctx->atoms.winUtilAtom = XInternAtom(ctx->dpy, "_NET_WM_WINDOW_TYPE_UTILITY", false);
	ctx->atoms.winSplashAtom = XInternAtom(ctx->dpy, "_NET_WM_WINDOW_TYPE_SPLASH", false);
	ctx->atoms.winDialogAtom = XInternAtom(ctx->dpy, "_NET_WM_WINDOW_TYPE_DIALOG", false);
	ctx->atoms.winNormalAtom = XInternAtom(ctx->dpy, "_NET_WM_WINDOW_TYPE_NORMAL", false);
	ctx->atoms.sizeHintsAtom = XInternAtom(ctx->dpy, "WM_NORMAL_HINTS", false);
	ctx->atoms.netWMStateFullscreenAtom = XInternAtom(ctx->dpy, "_NET_WM_STATE_FULLSCREEN", false);
	ctx->atoms.activeWindowAtom = XInternAtom(ctx->dpy, "_NET_ACTIVE_WINDOW", false);
	ctx->atoms.netWMStateAtom = XInternAtom(ctx->dpy, "_NET_WM_STATE", false);
	ctx->atoms.WMTransientForAtom = XInternAtom(ctx->dpy, "WM_TRANSIENT_FOR", false);
	ctx->atoms.netWMStateHiddenAtom = XInternAtom(ctx->dpy, "_NET_WM_STATE_HIDDEN", false);
	ctx->atoms.netWMStateFocusedAtom = XInternAtom(ctx->dpy, "_NET_WM_STATE_FOCUSED", false);
	ctx->atoms.netWMStateSkipTaskbarAtom = XInternAtom(ctx->dpy, "_NET_WM_STATE_SKIP_TASKBAR", false);
	ctx->atoms.netWMStateSkipPagerAtom = XInternAtom(ctx->dpy, "_NET_WM_STATE_SKIP_PAGER", false);
	ctx->atoms.WLSurfaceIDAtom = XInternAtom(ctx->dpy, "WL_SURFACE_ID", false);
	ctx->atoms.WMStateAtom = XInternAtom(ctx->dpy, "WM_STATE", false);
	ctx->atoms.utf8StringAtom = XInternAtom(ctx->dpy, "UTF8_STRING", false);
	ctx->atoms.netWMNameAtom = XInternAtom(ctx->dpy, "_NET_WM_NAME", false);
	ctx->atoms.motifWMHints = XInternAtom(ctx->dpy, "_MOTIF_WM_HINTS", false);
	ctx->atoms.netSystemTrayOpcodeAtom = XInternAtom(ctx->dpy, "_NET_SYSTEM_TRAY_OPCODE", false);
	ctx->atoms.steamStreamingClientAtom = XInternAtom(ctx->dpy, "STEAM_STREAMING_CLIENT", false);
	ctx->atoms.steamStreamingClientVideoAtom = XInternAtom(ctx->dpy, "STEAM_STREAMING_CLIENT_VIDEO", false);
	ctx->atoms.gamescopeFocusableAppsAtom = XInternAtom(ctx->dpy, "GAMESCOPE_FOCUSABLE_APPS", false);
	ctx->atoms.gamescopeFocusableWindowsAtom = XInternAtom(ctx->dpy, "GAMESCOPE_FOCUSABLE_WINDOWS", false);
	ctx->atoms.gamescopeFocusedAppAtom = XInternAtom( ctx->dpy, "GAMESCOPE_FOCUSED_APP", false );
	ctx->atoms.gamescopeFocusedAppGfxAtom = XInternAtom( ctx->dpy, "GAMESCOPE_FOCUSED_APP_GFX", false );
	ctx->atoms.gamescopeFocusedWindowAtom = XInternAtom( ctx->dpy, "GAMESCOPE_FOCUSED_WINDOW", false );
	ctx->atoms.gamescopeCtrlAppIDAtom = XInternAtom(ctx->dpy, "GAMESCOPECTRL_BASELAYER_APPID", false);
	ctx->atoms.gamescopeCtrlWindowAtom = XInternAtom(ctx->dpy, "GAMESCOPECTRL_BASELAYER_WINDOW", false);
	ctx->atoms.WMChangeStateAtom = XInternAtom(ctx->dpy, "WM_CHANGE_STATE", false);
	ctx->atoms.gamescopeInputCounterAtom = XInternAtom(ctx->dpy, "GAMESCOPE_INPUT_COUNTER", false);
	ctx->atoms.gamescopeScreenShotAtom = XInternAtom( ctx->dpy, "GAMESCOPECTRL_REQUEST_SCREENSHOT", false );

	ctx->atoms.gamescopeFocusDisplay = XInternAtom(ctx->dpy, "GAMESCOPE_FOCUS_DISPLAY", false);
	ctx->atoms.gamescopeMouseFocusDisplay = XInternAtom(ctx->dpy, "GAMESCOPE_MOUSE_FOCUS_DISPLAY", false);
	ctx->atoms.gamescopeKeyboardFocusDisplay = XInternAtom( ctx->dpy, "GAMESCOPE_KEYBOARD_FOCUS_DISPLAY", false );

	// In nanoseconds...
	ctx->atoms.gamescopeTuneableVBlankRedZone = XInternAtom( ctx->dpy, "GAMESCOPE_TUNEABLE_VBLANK_REDZONE", false );
	ctx->atoms.gamescopeTuneableRateOfDecay = XInternAtom( ctx->dpy, "GAMESCOPE_TUNEABLE_VBLANK_RATE_OF_DECAY_PERCENTAGE", false );

	ctx->atoms.gamescopeScalingFilter = XInternAtom( ctx->dpy, "GAMESCOPE_SCALING_FILTER", false );
	ctx->atoms.gamescopeFSRSharpness = XInternAtom( ctx->dpy, "GAMESCOPE_FSR_SHARPNESS", false );
	ctx->atoms.gamescopeSharpness = XInternAtom( ctx->dpy, "GAMESCOPE_SHARPNESS", false );

	ctx->atoms.gamescopeColorLinearGain = XInternAtom( ctx->dpy, "GAMESCOPE_COLOR_LINEARGAIN", false );
	ctx->atoms.gamescopeColorGain = XInternAtom( ctx->dpy, "GAMESCOPE_COLOR_GAIN", false );
	ctx->atoms.gamescopeColorMatrix = XInternAtom( ctx->dpy, "GAMESCOPE_COLOR_MATRIX", false );
	ctx->atoms.gamescopeColorLinearGainBlend = XInternAtom( ctx->dpy, "GAMESCOPE_COLOR_LINEARGAIN_BLEND", false );

	ctx->atoms.gamescopeColorGammaExponent = XInternAtom( ctx->dpy, "GAMESCOPE_COLOR_GAMMA_EXPONENT", false );

	ctx->atoms.gamescopeXWaylandModeControl = XInternAtom( ctx->dpy, "GAMESCOPE_XWAYLAND_MODE_CONTROL", false );
	ctx->atoms.gamescopeFPSLimit = XInternAtom( ctx->dpy, "GAMESCOPE_FPS_LIMIT", false );
	ctx->atoms.gamescopeDynamicRefresh = XInternAtom( ctx->dpy, "GAMESCOPE_DYNAMIC_REFRESH", false );
	ctx->atoms.gamescopeLowLatency = XInternAtom( ctx->dpy, "GAMESCOPE_LOW_LATENCY", false );

	ctx->atoms.gamescopeFSRFeedback = XInternAtom( ctx->dpy, "GAMESCOPE_FSR_FEEDBACK", false );

	ctx->atoms.gamescopeBlurMode = XInternAtom( ctx->dpy, "GAMESCOPE_BLUR_MODE", false );
	ctx->atoms.gamescopeBlurRadius = XInternAtom( ctx->dpy, "GAMESCOPE_BLUR_RADIUS", false );
	ctx->atoms.gamescopeBlurFadeDuration = XInternAtom( ctx->dpy, "GAMESCOPE_BLUR_FADE_DURATION", false );

	ctx->root_width = DisplayWidth(ctx->dpy, ctx->scr);
	ctx->root_height = DisplayHeight(ctx->dpy, ctx->scr);

	ctx->allDamage = None;
	ctx->clipChanged = true;

	XGrabServer(ctx->dpy);

	XCompositeRedirectSubwindows(ctx->dpy, ctx->root, CompositeRedirectManual);

	Window			root_return, parent_return;
	Window			*children;
	unsigned int	nchildren;

	XSelectInput(ctx->dpy, ctx->root,
				  SubstructureNotifyMask|
				  ExposureMask|
				  StructureNotifyMask|
				  SubstructureRedirectMask|
				  FocusChangeMask|
				  PointerMotionMask|
				  LeaveWindowMask|
				  PropertyChangeMask);
	XShapeSelectInput(ctx->dpy, ctx->root, ShapeNotifyMask);
	XFixesSelectCursorInput(ctx->dpy, ctx->root, XFixesDisplayCursorNotifyMask);
	XQueryTree(ctx->dpy, ctx->root, &root_return, &parent_return, &children, &nchildren);
	for (uint32_t i = 0; i < nchildren; i++)
		add_win(ctx, children[i], i ? children[i-1] : None, 0);
	XFree(children);

	XUngrabServer(ctx->dpy);

	XF86VidModeLockModeSwitch(ctx->dpy, ctx->scr, true);

	ctx->cursor = std::make_unique<MouseCursor>(ctx);
	if (g_customCursorPath)
	{
		if (!load_mouse_cursor(ctx->cursor.get(), g_customCursorPath, g_customCursorHotspotX, g_customCursorHotspotY))
			xwm_log.errorf("Failed to load mouse cursor: %s", g_customCursorPath);
	}
}

extern int g_nPreferredOutputWidth;
extern int g_nPreferredOutputHeight;

static bool g_bWasFSRActive = false;

void
steamcompmgr_main(int argc, char **argv)
{
	int	readyPipeFD = -1;

	// Reset getopt() state
	optind = 1;

	int o;
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

	currentOutputWidth = g_nPreferredOutputWidth;
	currentOutputHeight = g_nPreferredOutputHeight;

	init_runtime_info();

	int vblankFD = vblank_init();
	assert( vblankFD >= 0 );

	std::unique_lock<std::mutex> xwayland_server_guard(g_SteamCompMgrXWaylandServerMutex);

	// Initialize any xwayland ctxs we have
	{
		gamescope_xwayland_server_t *server = NULL;
		for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
			init_xwayland_ctx(server);
	}

	gamescope_xwayland_server_t *root_server = wlserver_get_xwayland_server(0);
	xwayland_ctx_t *root_ctx = root_server->ctx.get();

	gamesRunningCount = get_prop(root_ctx, root_ctx->root, root_ctx->atoms.gamesRunningAtom, 0);
	overscanScaleRatio = get_prop(root_ctx, root_ctx->root, root_ctx->atoms.screenScaleAtom, 0xFFFFFFFF) / (double)0xFFFFFFFF;
	zoomScaleRatio = get_prop(root_ctx, root_ctx->root, root_ctx->atoms.screenZoomAtom, 0xFFFF) / (double)0xFFFF;

	globalScaleRatio = overscanScaleRatio * zoomScaleRatio;

	determine_and_apply_focus();

	if ( readyPipeFD != -1 )
	{
		dprintf( readyPipeFD, "%s %s\n", root_ctx->xwayland_server->get_nested_display_name(), wlserver_get_wl_display_name() );
		close( readyPipeFD );
		readyPipeFD = -1;
	}

	if ( subCommandArg >= 0 )
	{
		spawn_client( &argv[ subCommandArg ] );
	}

	std::thread imageWaitThread( imageWaitThreadMain );
	imageWaitThread.detach();

	std::vector<pollfd> pollfds;
	// EVENT_VBLANK
	pollfds.push_back(pollfd {
		.fd = vblankFD,
		.events = POLLIN,
	});
	// EVENT_NUDGE
	pollfds.push_back(pollfd {
			.fd = g_nudgePipe[ 0 ],
			.events = POLLIN,
	});
	// EVENT_X11
	{
		gamescope_xwayland_server_t *server = NULL;
		for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
		{
			pollfds.push_back(pollfd {
				.fd = XConnectionNumber( server->ctx->dpy ),
				.events = POLLIN,
			});
		}
	}

	for (;;)
	{
		bool vblank = false;

		{
			gamescope_xwayland_server_t *server = NULL;
			for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
			{
				assert(server);
				if (x_events_queued(server->ctx.get()))
					dispatch_x11(server->ctx.get());
			}
		}

		if ( poll( pollfds.data(), pollfds.size(), -1 ) < 0)
		{
			if ( errno == EAGAIN )
				continue;

			xwm_log.errorf_errno( "poll failed" );
			break;
		}

		for (size_t i = EVENT_X11; i < pollfds.size(); i++)
		{
			if ( pollfds[ i ].revents & POLLHUP )
			{
				xwm_log.errorf( "Lost connection to the X11 server %zd", i - EVENT_X11 );
				break;
			}
		}

		assert( !( pollfds[ EVENT_VBLANK ].revents & POLLHUP ) );
		assert( !( pollfds[ EVENT_NUDGE ].revents & POLLHUP ) );

		for (size_t i = EVENT_X11; i < pollfds.size(); i++)
		{
			if ( pollfds[ i ].revents & POLLIN )
			{
				gamescope_xwayland_server_t *server = wlserver_get_xwayland_server(i - EVENT_X11);
				assert(server);
				dispatch_x11( server->ctx.get() );
			}
		}
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
			XChangeProperty( root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeInputCounterAtom, XA_CARDINAL, 32, PropModeReplace,
							 (unsigned char *)&inputCounter, 1 );

			lastPublishedInputCounter = inputCounter;
		}

		if ( g_bFSRActive != g_bWasFSRActive )
		{
			uint32_t active = g_bFSRActive ? 1 : 0;
			XChangeProperty( root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeFSRFeedback, XA_CARDINAL, 32, PropModeReplace,
					(unsigned char *)&active, 1 );

			g_bWasFSRActive = g_bFSRActive;
		}

		if (focusDirty)
			determine_and_apply_focus();

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
				if ( steamMode && g_nXWaylandCount > 1 )
				{
					g_nNestedHeight = ( g_nNestedWidth * g_nOutputHeight ) / g_nOutputWidth;
					wlserver_lock();
					// Update only Steam, the root ctx, with the new output size for now
					wlserver_set_xwayland_server_mode( 0, g_nOutputWidth, g_nOutputHeight, g_nOutputRefresh );
					wlserver_unlock();
				}

				vulkan_remake_output_images();
			}

			currentOutputWidth = g_nOutputWidth;
			currentOutputHeight = g_nOutputHeight;

#if HAVE_PIPEWIRE
			nudge_pipewire();
#endif
		}

		{
			gamescope_xwayland_server_t *server = NULL;
			for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
				handle_done_commits(server->ctx.get());
		}

		{
			gamescope_xwayland_server_t *server = NULL;
			for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
				check_new_wayland_res(server->ctx.get());
		}

		// Handles if we got a commit for the window we want to focus
		// to switch to it for painting (outdatedInteractiveFocus)
		// Doesn't realllly matter but avoids an extra frame of being on the wrong window.
		if (focusDirty)
			determine_and_apply_focus();

		if ( ( g_bTakeScreenshot == true || hasRepaint == true || is_fading_out() ) && vblank == true )
		{
			paint_all();

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

		{
			gamescope_xwayland_server_t *server = NULL;
			for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
				server->ctx->cursor->updatePosition();
		}

		// Ask for a new surface every vblank
		if ( vblank == true )
		{
			static int vblank_idx = 0;
			{
				gamescope_xwayland_server_t *server = NULL;
				for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
				{
					for (win *w = server->ctx->list; w; w = w->next)
					{
						bool bSendCallback = w->surface.wlr != nullptr;

						int nRefresh = g_nNestedRefresh ? g_nNestedRefresh : g_nOutputRefresh;
						int nTargetFPS = g_nSteamCompMgrTargetFPS;
						if ( g_nSteamCompMgrTargetFPS && steamcompmgr_window_should_limit_fps( w ) && nRefresh > nTargetFPS )
						{
							int nVblankDivisor = nRefresh / nTargetFPS;

							if ( vblank_idx % nVblankDivisor != 0 )
								bSendCallback = false;
						}

						if ( bSendCallback )
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
			}
			vblank_idx++;
		}

		vulkan_garbage_collect();

		vblank = false;
	}

	steamcompmgr_exit();
}

void steamcompmgr_send_frame_done_to_focus_window()
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	if ( global_focus.focusWindow && global_focus.focusWindow->surface.wlr )
	{
		wlserver_lock();
		wlserver_send_frame_done( global_focus.focusWindow->surface.wlr , &now );
		wlserver_unlock();		
	}
}

gamescope_xwayland_server_t *steamcompmgr_get_focused_server()
{
	if (global_focus.inputFocusWindow != nullptr)
	{
		gamescope_xwayland_server_t *server = NULL;
		for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
		{
			if (server->ctx->focus.inputFocusWindow == global_focus.inputFocusWindow)
				return server;
		}
	}

	return wlserver_get_xwayland_server(0);
}

struct wlr_surface *steamcompmgr_get_server_input_surface( size_t idx )
{
	gamescope_xwayland_server_t *server = wlserver_get_xwayland_server( idx );
	if ( server && server->ctx && server->ctx->focus.inputFocusWindow && server->ctx->focus.inputFocusWindow->surface.wlr )
		return server->ctx->focus.inputFocusWindow->surface.wlr;
	return NULL;
}
