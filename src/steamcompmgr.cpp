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

#if HAVE_PIPEWIRE
#include "pipewire.hpp"
#endif

#define GPUVIS_TRACE_IMPLEMENTATION
#include "gpuvis_trace_utils.h"

extern char **environ;

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

typedef struct _win {
	struct _win		*next;
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

	Bool isSteam;
	Bool isSteamStreamingClient;
	Bool isSteamStreamingClientVideo;
	uint32_t inputFocusMode;
	uint32_t appID;
	Bool isOverlay;
	Bool isFullscreen;
	Bool isSysTrayIcon;
	Bool sizeHintsSpecified;
	Bool skipTaskbar;
	Bool skipPager;
	unsigned int requestedWidth;
	unsigned int requestedHeight;
	
	Window transientFor;
	
	Bool nudged;
	Bool ignoreOverrideRedirect;

	Bool mouseMoved;

	struct wlserver_surface surface;

	std::vector< commit_t > commit_queue;
} win;

static win		*list;
static int		scr;
static Window		root;
static XserverRegion	allDamage;
static Bool		clipChanged;
static int		root_height, root_width;
static ignore		*ignore_head, **ignore_tail = &ignore_head;
static int		xfixes_event, xfixes_error;
static int		damage_event, damage_error;
static int		composite_event, composite_error;
static int		render_event, render_error;
static int		xshape_event, xshape_error;
static Bool		synchronize;
static int		composite_opcode;

uint32_t		currentOutputWidth, currentOutputHeight;

static Window	currentFocusWindow;
static win*		currentFocusWin;
static Window	currentInputFocusWindow;
uint32_t		currentInputFocusMode;
static Window 	currentKeyboardFocusWindow;
static Window	currentOverlayWindow;
static Window	currentNotificationWindow;

bool hasFocusWindow;

bool focusControlled;
std::vector< uint32_t > vecFocuscontrolAppIDs;

static Window	ourWindow;

Bool			gameFocused;

unsigned int 	gamesRunningCount;

float			overscanScaleRatio = 1.0;
float			zoomScaleRatio = 1.0;
float			globalScaleRatio = 1.0f;

float			focusedWindowScaleX = 1.0f;
float			focusedWindowScaleY = 1.0f;
float			focusedWindowOffsetX = 0.0f;
float			focusedWindowOffsetY = 0.0f;

Bool			focusDirty = False;
bool			hasRepaint = false;

unsigned long	damageSequence = 0;

unsigned int	cursorHideTime = 10'000;

Bool			gotXError = False;

win				fadeOutWindow;
Bool			fadeOutWindowGone;
unsigned int	fadeOutStartTime;

extern float g_flMaxWindowScale;
extern bool g_bIntegerScale;

#define			FADE_OUT_DURATION 200

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
static Atom		gamescopeCtrlAppIDAtom;

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

static Bool		doRender = True;
static Bool		debugFocus = False;
static Bool		drawDebugInfo = False;
static Bool		debugEvents = False;
static Bool		steamMode = False;
static Bool		alwaysComposite = False;
static Bool		useXRes = True;

std::mutex wayland_commit_lock;
std::vector<ResListEntry_t> wayland_commit_queue;

static std::atomic< bool > g_bTakeScreenshot{false};

static int g_nudgePipe[2];

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
		perror( "failed to poll fence FD" );
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
	va_start (args, format);
	vsprintf (buffer,format, args);
	va_end (args);

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
get_time_in_milliseconds (void)
{
	return (unsigned int)(get_time_in_nanos() / 1'000'000ul);
}

static void
discard_ignore (Display *dpy, unsigned long sequence)
{
	while (ignore_head)
	{
		if ((long) (sequence - ignore_head->sequence) > 0)
		{
			ignore  *next = ignore_head->next;
			free (ignore_head);
			ignore_head = next;
			if (!ignore_head)
				ignore_tail = &ignore_head;
		}
		else
			break;
	}
}

static void
set_ignore (Display *dpy, unsigned long sequence)
{
	ignore  *i = (ignore *)malloc (sizeof (ignore));
	if (!i)
		return;
	i->sequence = sequence;
	i->next = NULL;
	*ignore_tail = i;
	ignore_tail = &i->next;
}

static int
should_ignore (Display *dpy, unsigned long sequence)
{
	discard_ignore (dpy, sequence);
	return ignore_head && ignore_head->sequence == sequence;
}

static win *
find_win (Display *dpy, Window id)
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
	set_ignore (dpy, NextRequest (dpy));
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

static void
release_commit ( commit_t &commit )
{
	if ( commit.fb_id != 0 )
	{
		drm_drop_fbid( &g_DRM, commit.fb_id );
		commit.fb_id = 0;
	}

	if ( commit.vulkanTex != 0 )
	{
		vulkan_free_texture( commit.vulkanTex );
		commit.vulkanTex = 0;
	}

	wlserver_lock();
	wlr_buffer_unlock( commit.buf );
	wlserver_unlock();
}

static bool
import_commit ( struct wlr_buffer *buf, struct wlr_dmabuf_attributes *dmabuf, commit_t &commit )
{
	commit.buf = buf;

	if ( BIsNested() == False )
	{
		commit.fb_id = drm_fbid_from_dmabuf( &g_DRM, buf, dmabuf );
	}

	commit.vulkanTex = vulkan_create_texture_from_dmabuf( dmabuf );
	assert( commit.vulkanTex != 0 );

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

	if (rootX >= window->a.width || rootY >= window->a.height) {
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
	// Account for one warp from us, one warp from the app and one warp from
	// the toolkit.
	if (!window || window->mouseMoved++ < 3 )
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
	if ( BIsNested() == false && alwaysComposite == False )
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

			if ( cursorBuffer[i * m_width + j] & 0x000000ff ) {
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

	pPipeline->layerBindings[ curLayer ].surfaceWidth = m_width;
	pPipeline->layerBindings[ curLayer ].surfaceHeight = m_height;

	pPipeline->layerBindings[ curLayer ].zpos = 2; // cursor, on top of both bottom layers

	pPipeline->layerBindings[ curLayer ].tex = m_texture;
	pPipeline->layerBindings[ curLayer ].fbid = BIsNested() ? 0 :
															  vulkan_texture_get_fbid(m_texture);

	pPipeline->layerBindings[ curLayer ].bFilter = false;
	pPipeline->layerBindings[ curLayer ].bBlackBorder = false;

	pComposite->nLayerCount += 1;
}

static void
paint_window (Display *dpy, win *w, struct Composite_t *pComposite,
			  struct VulkanPipeline_t *pPipeline, Bool notificationMode, MouseCursor *cursor)
{
	uint32_t sourceWidth, sourceHeight;
	int drawXOffset = 0, drawYOffset = 0;
	float currentScaleRatio = 1.0;
	commit_t lastCommit = {};
	bool validContents = get_window_last_done_commit( w, lastCommit );

	if (!w)
		return;

	// Don't add a layer at all if it's an overlay without contents
	if (w->isOverlay && !validContents)
		return;

	// Base plane will stay as tex=0 if we don't have contents yet, which will
	// make us fall back to compositing and use the Vulkan null texture

	win *mainOverlayWindow = find_win(dpy, currentOverlayWindow);

	if (notificationMode && !mainOverlayWindow)
		return;

	if (notificationMode)
	{
		sourceWidth = mainOverlayWindow->a.width;
		sourceHeight = mainOverlayWindow->a.height;
	}
	else
	{
		sourceWidth = w->a.width;
		sourceHeight = w->a.height;
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

		if ( zoomScaleRatio != 1.0 )
		{
			drawXOffset += (((int)sourceWidth / 2) - cursor->x()) * currentScaleRatio;
			drawYOffset += (((int)sourceHeight / 2) - cursor->y()) * currentScaleRatio;
		}
	}

	int curLayer = pComposite->nLayerCount;

	pComposite->data.flOpacity[ curLayer ] = w->isOverlay ? w->opacity / (float)OPAQUE : 1.0f;

	pComposite->data.vScale[ curLayer ].x = 1.0 / currentScaleRatio;
	pComposite->data.vScale[ curLayer ].y = 1.0 / currentScaleRatio;

	if (notificationMode)
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
	}
	else
	{
		pComposite->data.vOffset[ curLayer ].x = -drawXOffset;
		pComposite->data.vOffset[ curLayer ].y = -drawYOffset;
	}

	pPipeline->layerBindings[ curLayer ].surfaceWidth = w->a.width;
	pPipeline->layerBindings[ curLayer ].surfaceHeight = w->a.height;

	pPipeline->layerBindings[ curLayer ].zpos = 0;

	if ( w->isOverlay || w->isSteamStreamingClient )
	{
		pPipeline->layerBindings[ curLayer ].zpos = 1;
	}

	pPipeline->layerBindings[ curLayer ].tex = lastCommit.vulkanTex;
	pPipeline->layerBindings[ curLayer ].fbid = lastCommit.fb_id;

	pPipeline->layerBindings[ curLayer ].bFilter = w->isOverlay ? true : g_bFilterGameWindow;
	pPipeline->layerBindings[ curLayer ].bBlackBorder = notificationMode ? false : true;

	pComposite->nLayerCount += 1;
}

static void
paint_message (const char *message, int Y, float r, float g, float b)
{

}

static void
paint_debug_info (Display *dpy)
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

static void
paint_all(Display *dpy, MouseCursor *cursor)
{
	static long long int paintID = 0;

	paintID++;
	gpuvis_trace_begin_ctx_printf( paintID, "paint_all" );
	win	*w;
	win	*overlay;
	win	*notification;
	win *input;

	unsigned int currentTime = get_time_in_milliseconds();
	Bool fadingOut = ((currentTime - fadeOutStartTime) < FADE_OUT_DURATION && fadeOutWindow.id != None);

	w = find_win(dpy, currentFocusWindow);
	overlay = find_win(dpy, currentOverlayWindow);
	notification = find_win(dpy, currentNotificationWindow);
	input = find_win(dpy, currentInputFocusWindow);

	if ( !w )
	{
		return;
	}
	
	Bool inGame = False;
	
	if ( gamesRunningCount || w->appID != 0 )
	{
		inGame = True;
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

	// Fading out from previous window?
	if (fadingOut)
	{
		double newOpacity = ((currentTime - fadeOutStartTime) / (double)FADE_OUT_DURATION);

		// Draw it in the background
		fadeOutWindow.opacity = (1.0 - newOpacity) * OPAQUE;
		paint_window(dpy, &fadeOutWindow, &composite, &pipeline, False, cursor);

		// Blend new window on top with linear crossfade
		w->opacity = newOpacity * OPAQUE;

		paint_window(dpy, w, &composite, &pipeline, False, cursor);
	}
	else
	{
		// If the window we'd paint as the base layer is the streaming client,
		// find the video underlay and put it up first in the scenegraph
		if ( w->isSteamStreamingClient == True )
		{
			win *videow = NULL;

			for ( videow = list; videow; videow = videow->next )
			{
				if ( videow->isSteamStreamingClientVideo == True )
				{
					// TODO: also check matching AppID so we can have several pairs
					paint_window(dpy, videow, &composite, &pipeline, False, cursor);
					break;
				}
			}
			
			// paint UI unless it's fully hidden, which it communicates to us through opacity=0
			if ( w->opacity > TRANSLUCENT )
			{
				paint_window(dpy, w, &composite, &pipeline, False, cursor);
			}
		}
		else
		{
			// Just draw focused window as normal, be it Steam or the game
			paint_window(dpy, w, &composite, &pipeline, False, cursor);
		}

		if (fadeOutWindow.id) {

			if (fadeOutWindowGone)
			{
				// This is the only reference to these resources now.
				fadeOutWindowGone = False;
			}
			fadeOutWindow.id = None;
		}
	}

	int touchInputFocusLayer = composite.nLayerCount - 1;

	if (inGame && overlay)
	{
		if (overlay->opacity)
		{
			paint_window(dpy, overlay, &composite, &pipeline, False, cursor);

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
			paint_window(dpy, notification, &composite, &pipeline, True, cursor);
		}
	}

	// Draw cursor if we need to
	if (input) {
		cursor->paint(input, &composite, &pipeline );
	}

	if (drawDebugInfo)
		paint_debug_info(dpy);

	if ( BIsNested() == false && g_DRM.paused == true )
	{
		return;
	}

	bool bDoComposite = true;

	// Handoff from whatever thread to this one since we check ours twice
	bool takeScreenshot = g_bTakeScreenshot.exchange(false);

	struct pipewire_buffer *pw_buffer = nullptr;
#if HAVE_PIPEWIRE
	pw_buffer = dequeue_pipewire_buffer();
#endif

	bool bCapture = takeScreenshot || pw_buffer != nullptr;

	if ( BIsNested() == false && alwaysComposite == False && bCapture == false )
	{
		int ret = drm_prepare( &g_DRM, &composite, &pipeline );
		if ( ret == 0 )
			bDoComposite = false;
		else if ( ret == -EACCES )
			return;
	}

	if ( bDoComposite == true )
	{
		CVulkanTexture *pCaptureTexture = nullptr;

		bool bResult = vulkan_composite( &composite, &pipeline, bCapture ? &pCaptureTexture : nullptr );

		if ( bResult != true )
		{
			fprintf( stderr, "composite alarm!!!\n" );
			return;
		}

		if ( BIsNested() == True )
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
				fprintf( stderr, "Failed to prepare 1-layer flip: %s\n", strerror(-ret) );

			// We should always handle a 1-layer flip
			assert( ret == 0 );

			drm_commit( &g_DRM, &composite, &pipeline );
		}

		if ( takeScreenshot )
		{
			assert( pCaptureTexture != nullptr );
			assert( pCaptureTexture->m_format == VK_FORMAT_B8G8R8A8_UNORM );

			uint32_t redMask = 0x00ff0000;
			uint32_t greenMask = 0x0000ff00;
			uint32_t blueMask = 0x000000ff;
			uint32_t alphaMask = 0;

			SDL_Surface *pSDLSurface = SDL_CreateRGBSurfaceFrom( pCaptureTexture->m_pMappedData, currentOutputWidth, currentOutputHeight, 32, pCaptureTexture->m_unRowPitch, redMask, greenMask, blueMask, alphaMask );

			static char pTimeBuffer[1024];

			time_t currentTime = time(0);
			struct tm *localTime = localtime( &currentTime );
			strftime( pTimeBuffer, sizeof( pTimeBuffer ), "/tmp/gamescope_%Y-%m-%d_%H-%M-%S.bmp", localTime );

			SDL_SaveBMP( pSDLSurface, pTimeBuffer );

			SDL_FreeSurface( pSDLSurface );

			fprintf(stderr, "Screenshot saved to %s\n", pTimeBuffer);
			takeScreenshot = False;
		}

#if HAVE_PIPEWIRE
		if ( pw_buffer != nullptr )
		{
			assert( pCaptureTexture != nullptr );
			assert( pCaptureTexture->m_format == VK_FORMAT_B8G8R8A8_UNORM );

			if ( pw_buffer->video_info.size.width != currentOutputWidth || pw_buffer->video_info.size.height != currentOutputHeight )
			{
				// Push black frames until the PipeWire thread realizes the stream size has changed
				memset( pw_buffer->data, 0, pw_buffer->stride * pw_buffer->video_info.size.height );
			}
			else
			{
				// TODO: avoid this memcpy by using multiple capture textures
				int bpp = 4;
				for ( unsigned int i = 0; i < currentOutputHeight; i++ )
				{
					memcpy( pw_buffer->data + i * pw_buffer->stride, (uint8_t *) pCaptureTexture->m_pMappedData + i * pCaptureTexture->m_unRowPitch, bpp * currentOutputWidth );
				}
			}

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
	int result = XGetWindowProperty(dpy, win, prop, 0L, 1L, False,
									XA_CARDINAL, &actual, &format,
								 &n, &left, &data);
	if (result == Success && data != NULL)
	{
		unsigned int i;
		memcpy (&i, data, sizeof (unsigned int));
		XFree( (void *) data);
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
	
	uint64_t *data;
	// get up to 16 results in one go, we can add a real loop if we ever need anything beyong that
	int result = XGetWindowProperty(dpy, win, prop, 0L, 16L, False,
									XA_CARDINAL, &actual, &format,
									&n, &left, ( unsigned char** )&data);
	if (result == Success && data != NULL)
	{
		vecResult.clear();
		
		for ( uint32_t i = 0; i < n; i++ )
		{
			vecResult.push_back( data[ i ] );
		}
		XFree( (void *) data);
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
	if ( win_has_game_id( a ) != win_has_game_id ( b ) )
		return win_has_game_id( a );

	// We allow using an override redirect window in some cases, but if we have
	// a choice between two windows we always prefer the non-override redirect
	// one.
	if ( win_is_override_redirect( a ) != win_is_override_redirect( b ) )
		return !win_is_override_redirect( a );

	// Wine sets SKIP_TASKBAR and SKIP_PAGER hints for WS_EX_NOACTIVATE windows.
	// See https://github.com/Plagman/gamescope/issues/87
	if ( win_skip_taskbar_and_pager( a ) != win_skip_taskbar_and_pager ( b ) )
		return !win_skip_taskbar_and_pager( a );

	// The damage sequences are only relevant for game windows.
	if ( win_has_game_id( a ) && a->damage_sequence != b->damage_sequence )
		return a->damage_sequence > b->damage_sequence;

	return false;
}

static void
determine_and_apply_focus (Display *dpy, MouseCursor *cursor)
{
	win *w, *focus = NULL;
	win *inputFocus = NULL;

	gameFocused = False;
	
	Window prevFocusWindow = currentFocusWindow;
	currentFocusWindow = None;
	currentFocusWin = nullptr;
	currentOverlayWindow = None;
	currentNotificationWindow = None;

	unsigned int maxOpacity = 0;
	std::vector< win* > vecPossibleFocusWindows;
	for (w = list; w; w = w->next)
	{
		// Always skip system tray icons
		if ( w->isSysTrayIcon )
		{
			continue;
		}

		if ( w->a.map_state == IsViewable && w->a.c_class == InputOutput && w->isOverlay == False && 
			 (w->opacity > TRANSLUCENT || w->isSteamStreamingClient == True ) )
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
	
	std::vector< unsigned long > focusable_appids;
	
	for ( unsigned long i = 0; i < vecPossibleFocusWindows.size(); i++ )
	{
		unsigned int unAppID = vecPossibleFocusWindows[ i ]->appID;
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
	}
	
	XChangeProperty( dpy, root, XInternAtom( dpy, "GAMESCOPE_FOCUSABLE_APPS", False ),
					 XA_CARDINAL, 32, PropModeReplace, (unsigned char *)focusable_appids.data(),
					 focusable_appids.size() );

	std::stable_sort( vecPossibleFocusWindows.begin(), vecPossibleFocusWindows.end(),
					  is_focus_priority_greater );

	if ( focusControlled == true )
	{
		for ( unsigned long i = 0; i < vecFocuscontrolAppIDs.size(); i++ )
		{
			for ( unsigned long j = 0; j < vecPossibleFocusWindows.size(); j++ )
			{
				if ( vecPossibleFocusWindows[ j ]->appID == vecFocuscontrolAppIDs[ i ] )
				{
					focus = vecPossibleFocusWindows[ j ];
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
	
	unsigned long focusedWindow = 0;
	unsigned long focusedAppId = 0;
	
	if ( inputFocus == NULL )
	{
		inputFocus = focus;
	}

	if ( focus )
	{
		focusedWindow = focus->id;
		focusedAppId = inputFocus->appID;
	}
	
	XChangeProperty( dpy, root, XInternAtom( dpy, "GAMESCOPE_FOCUSED_WINDOW", False ),
					 XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&focusedWindow,
					 focusedWindow != 0 ? 1 : 0 );
	
	XChangeProperty( dpy, root, XInternAtom( dpy, "GAMESCOPE_FOCUSED_APP", False ),
					 XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&focusedAppId,
					 focusedAppId != 0 ? 1 : 0 );

	if (!focus)
	{
		return;
	}

	if ( gameFocused )
	{
		// Do some searches through game windows to follow transient links if needed
		while ( true )
		{
			bool bFoundTransient = false;
			
			for ( uint32_t i = 0; i < vecPossibleFocusWindows.size(); i++ )
			{
				win *candidate = vecPossibleFocusWindows[ i ];
				
				if ( candidate != focus && candidate->transientFor == focus->id )
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

// 	if (fadeOutWindow.id == None && currentFocusWindow != focus->id)
// 	{
// 		// Initiate fade out if switching focus
// 		w = find_win(dpy, currentFocusWindow);
//
// 		if (w)
// 		{
// 			ensure_win_resources(dpy, w);
// 			fadeOutWindow = *w;
// 			fadeOutStartTime = get_time_in_milliseconds();
// 		}
// 	}

// 	if (fadeOutWindow.id && currentFocusWindow != focus->id)
	if ( prevFocusWindow != focus->id )
	{
		/* Some games (e.g. DOOM Eternal) don't react well to being put back as
		* iconic, so never do that. Only take them out of iconic. */
		uint32_t wmState[] = { ICCCM_NORMAL_STATE, None };
		XChangeProperty(dpy, focus->id, WMStateAtom, WMStateAtom, 32,
					PropModeReplace, (unsigned char *)wmState,
					sizeof(wmState) / sizeof(wmState[0]));

		gpuvis_trace_printf( "determine_and_apply_focus focus %lu", focus->id );

		if ( debugFocus == True )
		{
			fprintf( stderr, "determine_and_apply_focus focus %lu\n", focus->id );
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
		
		if ( inputFocus->inputFocusMode == 2 )
			keyboardFocusWin = focus;

		if ( inputFocus->surface.wlr != nullptr || keyboardFocusWin->surface.wlr != nullptr )
		{
			wlserver_lock();

			if ( inputFocus->surface.wlr != nullptr )
				wlserver_mousefocus( inputFocus->surface.wlr );
		
			if ( keyboardFocusWin->surface.wlr != nullptr )
				wlserver_keyboardfocus( keyboardFocusWin->surface.wlr );

			wlserver_unlock();
		}
		
		XSetInputFocus(dpy, keyboardFocusWin->id, RevertToNone, CurrentTime);

		currentInputFocusWindow = inputFocus->id;
		currentInputFocusMode = inputFocus->inputFocusMode;
		currentKeyboardFocusWindow = keyboardFocusWin->id;
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
		focus->nudged = True;
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

	XQueryTree (dpy, w->id, &root_return, &parent_return, &children, &nchildren);

	while (i < nchildren)
	{
		XSelectInput( dpy, children[i], PointerMotionMask | FocusChangeMask );
		i++;
	}

	XFree (children);
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

		w->sizeHintsSpecified = True;
	}
	else
	{
		w->sizeHintsSpecified = False;

		// Below block checks for a pattern that matches old SDL fullscreen applications;
		// SDL creates a fullscreen overrride-redirect window and reparents the game
		// window under it, centered. We get rid of the modeswitch and also want that
		// black border gone.
		if (w->a.override_redirect)
		{
			Window	    root_return = None, parent_return = None;
			Window	    *children = NULL;
			unsigned int    nchildren = 0;

			XQueryTree (dpy, w->id, &root_return, &parent_return, &children, &nchildren);

			if (nchildren == 1)
			{
				XWindowAttributes attribs;

				XGetWindowAttributes (dpy, children[0], &attribs);

				// If we have a unique children that isn't override-reidrect that is
				// contained inside this fullscreen window, it's probably it.
				if (attribs.override_redirect == False &&
					attribs.width <= w->a.width &&
					attribs.height <= w->a.height)
				{
					w->sizeHintsSpecified = True;

					w->requestedWidth = attribs.width;
					w->requestedHeight = attribs.height;

					XMoveWindow(dpy, children[0], 0, 0);

					w->ignoreOverrideRedirect = True;
				}
			}

			XFree (children);
		}
	}
}

static void
get_win_title(Display *dpy, win *w, Atom atom)
{
	assert(atom == XA_WM_NAME || atom == netWMNameAtom);

	XTextProperty tp;
	XGetTextProperty ( dpy, w->id, &tp, atom );

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
	if (XGetWindowProperty(dpy, w->id, netWMStateAtom, 0, 2048, False,
			AnyPropertyType, &type, &format, &nitems, &bytesAfter, &data) != Success) {
		return;
	}

	Atom *props = (Atom *)data;
	for (size_t i = 0; i < nitems; i++) {
		if (props[i] == netWMStateFullscreenAtom) {
			w->isFullscreen = True;
		} else if (props[i] == netWMStateSkipTaskbarAtom) {
			w->skipTaskbar = True;
		} else if (props[i] == netWMStateSkipPagerAtom) {
			w->skipPager = True;
		} else {
			fprintf(stderr, "Unhandled initial NET_WM_STATE property: %s\n", XGetAtomName(dpy, props[i]));
		}
	}

	XFree(data);
}

static void
map_win (Display *dpy, Window id, unsigned long sequence)
{
	win		*w = find_win (dpy, id);

	if (!w)
		return;

	w->a.map_state = IsViewable;

	/* This needs to be here or else we lose transparency messages */
	XSelectInput (dpy, id, PropertyChangeMask | SubstructureNotifyMask |
		PointerMotionMask | LeaveWindowMask | FocusChangeMask);

	/* This needs to be here since we don't get PropertyNotify when unmapped */
	w->opacity = get_prop (dpy, w->id, opacityAtom, OPAQUE);

	w->isSteam = get_prop (dpy, w->id, steamAtom, 0);

	/* First try to read the UTF8 title prop, then fallback to the non-UTF8 one */
	get_win_title( dpy, w, netWMNameAtom );
	get_win_title( dpy, w, XA_WM_NAME );

	w->inputFocusMode = get_prop(dpy, w->id, steamInputFocusAtom, 0);

	w->isSteamStreamingClient = get_prop(dpy, w->id, steamStreamingClientAtom, 0);
	w->isSteamStreamingClientVideo = get_prop(dpy, w->id, steamStreamingClientVideoAtom, 0);

	if ( steamMode == True )
	{
		uint32_t appID = get_prop (dpy, w->id, gameAtom, 0);
		
		if ( w->appID != 0 && appID != 0 && w->appID != appID )
		{
			fprintf( stderr, "appid clash was %u now %u\n", w->appID, appID );
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
	w->isOverlay = get_prop (dpy, w->id, overlayAtom, 0);

	get_size_hints(dpy, w);

	get_net_wm_state(dpy, w);

	XWMHints *wmHints = XGetWMHints( dpy, w->id );

	if ( wmHints != nullptr )
	{
		if ( wmHints->flags & (InputHint | StateHint ) && wmHints->input == True && wmHints->initial_state == NormalState )
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

	focusDirty = True;
}

static void
finish_unmap_win (Display *dpy, win *w)
{
	// TODO clear done commits here?
// 	if (fadeOutWindow.id != w->id)
// 	{
// 		teardown_win_resources( w );
// 	}

	if (fadeOutWindow.id == w->id)
	{
		fadeOutWindowGone = True;
	}

	/* don't care about properties anymore */
	set_ignore (dpy, NextRequest (dpy));
	XSelectInput(dpy, w->id, 0);

	clipChanged = True;
}

static void
unmap_win (Display *dpy, Window id, Bool fade)
{
	win *w = find_win (dpy, id);
	if (!w)
		return;
	w->a.map_state = IsUnmapped;

	focusDirty = True;

	finish_unmap_win (dpy, w);
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
get_win_pid (Display *dpy, Window id)
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
		fprintf(stderr, "Failed to find PID for window 0x%lx\n", id);
	return pid;
}

static void
add_win (Display *dpy, Window id, Window prev, unsigned long sequence)
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
	set_ignore (dpy, NextRequest (dpy));
	if (!XGetWindowAttributes (dpy, id, &new_win->a))
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
		new_win->damage = XDamageCreate (dpy, id, XDamageReportRawRectangles);
	}
	new_win->opacity = OPAQUE;

	if ( useXRes == True )
	{
		new_win->pid = get_win_pid (dpy, id);
	}
	else
	{
		new_win->pid = -1;
	}

	new_win->isOverlay = False;
	new_win->isSteam = False;
	new_win->isSteamStreamingClient = False;
	new_win->isSteamStreamingClientVideo = False;
	new_win->inputFocusMode = 0;

	if ( steamMode == True )
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
	new_win->utf8_title = False;
	
	new_win->isFullscreen = False;
	new_win->isSysTrayIcon = False;
	new_win->sizeHintsSpecified = False;
	new_win->skipTaskbar = False;
	new_win->skipPager = False;
	new_win->requestedWidth = 0;
	new_win->requestedHeight = 0;
	new_win->nudged = False;
	new_win->ignoreOverrideRedirect = False;

	new_win->mouseMoved = False;

	wlserver_surface_init( &new_win->surface, id );

	new_win->next = *p;
	*p = new_win;
	if (new_win->a.map_state == IsViewable)
		map_win (dpy, id, sequence);

	focusDirty = True;
}

static void
restack_win (Display *dpy, win *w, Window new_above)
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

		focusDirty = True;
	}
}

static void
configure_win (Display *dpy, XConfigureEvent *ce)
{
	win		    *w = find_win (dpy, ce->window);

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
	restack_win (dpy, w, ce->above);

	focusDirty = True;
}

static void
circulate_win (Display *dpy, XCirculateEvent *ce)
{
	win	    *w = find_win (dpy, ce->window);
	Window  new_above;

	if (!w || w->id != ce->window)
		return;

	if (ce->place == PlaceOnTop)
		new_above = list->id;
	else
		new_above = None;
	restack_win (dpy, w, new_above);
	clipChanged = True;
}

static void map_request (Display *dpy, XMapRequestEvent *mapRequest)
{
	XMapWindow( dpy, mapRequest->window );
}

static void configure_request (Display *dpy, XConfigureRequestEvent *configureRequest)
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

static void circulate_request ( Display *dpy, XCirculateRequestEvent *circulateRequest )
{
	XCirculateSubwindows( dpy, circulateRequest->window, circulateRequest->place );
}

static void
finish_destroy_win (Display *dpy, Window id, Bool gone)
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
				set_ignore (dpy, NextRequest (dpy));
				XDamageDestroy (dpy, w->damage);
				w->damage = None;
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
destroy_win (Display *dpy, Window id, Bool gone, Bool fade)
{
	if (currentFocusWindow == id && gone)
	{
		currentFocusWindow = None;
		currentFocusWin = nullptr;
	}
	if (currentInputFocusWindow == id && gone)
		currentInputFocusWindow = None;
	if (currentOverlayWindow == id && gone)
		currentOverlayWindow = None;
	if (currentNotificationWindow == id && gone)
		currentNotificationWindow = None;
	if (currentKeyboardFocusWindow == id && gone)
		currentKeyboardFocusWindow = None;
	focusDirty = True;

	finish_destroy_win (dpy, id, gone);
}

static void
damage_win (Display *dpy, XDamageNotifyEvent *de)
{
	win	*w = find_win (dpy, de->drawable);
	win *focus = find_win(dpy, currentFocusWindow);

	if (!w)
		return;

	if (w->isOverlay && !w->opacity)
		return;

	// First damage event we get, compute focus; we only want to focus damaged
	// windows to have meaningful frames.
	if (w->appID && w->damage_sequence == 0)
		focusDirty = True;

	w->damage_sequence = damageSequence++;

	// If we just passed the focused window, we might be eliglible to take over
	if ( !focusControlled && focus && focus != w && w->appID &&
		w->damage_sequence > focus->damage_sequence)
		focusDirty = True;

	if (w->damage)
		XDamageSubtract(dpy, w->damage, None, None);

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
	
	if ( inputFocusWin && inputFocusWin->inputFocusMode == 2 )
		keyboardFocusWindow = currentFocusWindow;

	if ( w->id == keyboardFocusWindow )
		wlserver_keyboardfocus( surface );

	// Pull the first buffer out of that window, if needed
	xwayland_surface_role_commit( surface );

	wlserver_unlock();
}

static void
update_net_wm_state(uint32_t action, Bool *value)
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
		fprintf(stderr, "Unknown NET_WM_STATE action: %" PRIu32 "\n", action);
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
			focusDirty = True;
		} else if (props[i] == netWMStateSkipTaskbarAtom) {
			update_net_wm_state(action, &w->skipTaskbar);
			focusDirty = True;
		} else if (props[i] == netWMStateSkipPagerAtom) {
			update_net_wm_state(action, &w->skipPager);
			focusDirty = True;
		} else if (props[i] != None) {
			fprintf(stderr, "Unhandled NET_WM_STATE property change: %s\n", XGetAtomName(dpy, props[i]));
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
				w->isSysTrayIcon = True;
			}
			break;
		}
		default:
			fprintf(stderr, "Unhandled _NET_SYSTEM_TRAY_OPCODE %ld\n", opcode);
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
		fprintf(stderr, "Rejecting WM_CHANGE_STATE to ICONIC for window 0x%lx\n", w->id);
		uint32_t wmState[] = { ICCCM_NORMAL_STATE, None };
		XChangeProperty(dpy, w->id, WMStateAtom, WMStateAtom, 32,
			PropModeReplace, (unsigned char *)wmState,
			sizeof(wmState) / sizeof(wmState[0]));
	} else {
		fprintf(stderr, "Unhandled WM_CHANGE_STATE to %ld for window 0x%lx\n", state, w->id);
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
			fprintf( stderr, "Unhandled client message: %s\n", XGetAtomName( dpy, ev->message_type ) );
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
			focusDirty = True;
		}
	}
	if (ev->atom == steamInputFocusAtom )
	{
		win * w = find_win(dpy, ev->window);
		if (w)
		{
			w->inputFocusMode = get_prop(dpy, w->id, steamInputFocusAtom, 0);
			focusDirty = True;
		}
	}
	if (ev->atom == steamTouchClickModeAtom )
	{
		// Default to 1, left click
		g_nTouchClickMode = (enum wlserver_touch_click_mode) get_prop(dpy, root, steamTouchClickModeAtom, 1 );
	}
	if (ev->atom == steamStreamingClientAtom)
	{
		win * w = find_win(dpy, ev->window);
		if (w)
		{
			w->isSteamStreamingClient = get_prop(dpy, w->id, steamStreamingClientAtom, 0);
			focusDirty = True;
		}
	}
	if (ev->atom == steamStreamingClientVideoAtom)
	{
		win * w = find_win(dpy, ev->window);
		if (w)
		{
			w->isSteamStreamingClientVideo = get_prop(dpy, w->id, steamStreamingClientVideoAtom, 0);
			focusDirty = True;
		}
	}
	if (ev->atom == gamescopeCtrlAppIDAtom )
	{
		focusControlled = get_prop( dpy, root, gamescopeCtrlAppIDAtom, vecFocuscontrolAppIDs );
		focusDirty = True;
	}
	if (ev->atom == gameAtom)
	{
		win * w = find_win(dpy, ev->window);
		if (w)
		{
			uint32_t appID = get_prop (dpy, w->id, gameAtom, 0);
			
			if ( w->appID != 0 && appID != 0 && w->appID != appID )
			{
				fprintf( stderr, "appid clash was %u now %u\n", w->appID, appID );
			}
			w->appID = appID;

			focusDirty = True;
		}
	}
	if (ev->atom == overlayAtom)
	{
		win * w = find_win(dpy, ev->window);
		if (w)
		{
			w->isOverlay = get_prop(dpy, w->id, overlayAtom, 0);
			focusDirty = True;
		}
	}
	if (ev->atom == sizeHintsAtom)
	{
		win * w = find_win(dpy, ev->window);
		if (w)
		{
			get_size_hints(dpy, w);
			focusDirty = True;
		}
	}
	if (ev->atom == gamesRunningAtom)
	{
		gamesRunningCount = get_prop(dpy, root, gamesRunningAtom, 0);

		focusDirty = True;
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

		focusDirty = True;
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

		focusDirty = True;
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
			focusDirty = True;
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
error (Display *dpy, XErrorEvent *ev)
{
	int	    o;
	const char    *name = NULL;
	static char buffer[256];

	if (should_ignore (dpy, ev->serial))
		return 0;

	if (ev->request_code == composite_opcode &&
		ev->minor_code == X_CompositeRedirectSubwindows)
	{
		fprintf (stderr, "Another composite manager is already running\n");
		exit (1);
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
		XGetErrorText (dpy, ev->error_code, buffer, sizeof (buffer));
		name = buffer;
	}

	fprintf (stderr, "error %d: %s request %d minor %d serial %lu\n",
			 ev->error_code, (strlen (name) > 0) ? name : "unknown",
			 ev->request_code, ev->minor_code, ev->serial);

	gotXError = True;
	/*    abort ();	    this is just annoying to most people */
	return 0;
}

static int
handle_io_error(Display *dpy)
{
	fprintf(stderr, "X11 I/O error\n");

	imageWaitThreadRun = false;
	waitListSem.signal();

	if ( statsThreadRun == true )
	{
		statsThreadRun = false;
		statsThreadSem.signal();
	}

	pthread_exit(NULL);
}

static Bool
register_cm (Display *dpy)
{
	Window w;
	Atom a;
	static char net_wm_cm[] = "_NET_WM_CM_Sxx";

	snprintf (net_wm_cm, sizeof (net_wm_cm), "_NET_WM_CM_S%d", scr);
	a = XInternAtom (dpy, net_wm_cm, False);

	w = XGetSelectionOwner (dpy, a);
	if (w != None)
	{
		XTextProperty tp;
		char **strs;
		int count;
		Atom winNameAtom = XInternAtom (dpy, "_NET_WM_NAME", False);

		if (!XGetTextProperty (dpy, w, &tp, winNameAtom) &&
			!XGetTextProperty (dpy, w, &tp, XA_WM_NAME))
		{
			fprintf (stderr,
					 "Another composite manager is already running (0x%lx)\n",
					 (unsigned long) w);
			return False;
		}
		if (XmbTextPropertyToTextList (dpy, &tp, &strs, &count) == Success)
		{
			fprintf (stderr,
					 "Another composite manager is already running (%s)\n",
					 strs[0]);

			XFreeStringList (strs);
		}

		XFree (tp.value);

		return False;
	}

	w = XCreateSimpleWindow (dpy, RootWindow (dpy, scr), 0, 0, 1, 1, 0, None,
							 None);

	Xutf8SetWMProperties (dpy, w, "steamcompmgr", "steamcompmgr", NULL, 0, NULL, NULL,
						  NULL);

	Atom atomWmCheck = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
	XChangeProperty(dpy, root, atomWmCheck,
					XA_WINDOW, 32, PropModeReplace, (unsigned char *)&w, 1);
	XChangeProperty(dpy, w, atomWmCheck,
					XA_WINDOW, 32, PropModeReplace, (unsigned char *)&w, 1);


	Atom supportedAtoms[] = {
		XInternAtom(dpy, "_NET_WM_STATE", False),
		XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False),
		XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False),
		XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False),
		XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False),
	};

	XChangeProperty(dpy, root, XInternAtom(dpy, "_NET_SUPPORTED", False),
					XA_ATOM, 32, PropModeAppend, (unsigned char *)supportedAtoms,
					sizeof(supportedAtoms) / sizeof(supportedAtoms[0]));

	XSetSelectionOwner (dpy, a, w, 0);

	ourWindow = w;

	return True;
}

static void
register_systray(Display *dpy)
{
	static char net_system_tray_name[] = "_NET_SYSTEM_TRAY_Sxx";

	snprintf(net_system_tray_name, sizeof(net_system_tray_name),
			 "_NET_SYSTEM_TRAY_S%d", scr);
	Atom net_system_tray = XInternAtom(dpy, net_system_tray_name, False);

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
		perror( "nudge_steamcompmgr: write failed" );
}

void take_screenshot( void )
{
	g_bTakeScreenshot = true;
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
			fprintf (stderr, "waylandres but no win\n");
			continue;
		}

		struct wlr_dmabuf_attributes dmabuf = {};
		bool result = False;
		if ( wlr_buffer_get_dmabuf( buf, &dmabuf ) ) {
			result = true;
			for ( int i = 0; i < dmabuf.n_planes; i++ ) {
				dmabuf.fd[i] = dup( dmabuf.fd[i] );
				assert( dmabuf.fd[i] >= 0 );
			}
		} else {
			struct wlr_client_buffer *client_buf = (struct wlr_client_buffer *) buf;
			result = wlr_texture_to_dmabuf( client_buf->texture, &dmabuf );
		}
		assert( result == true );

		commit_t newCommit = {};
		int fence = dup( dmabuf.fd[ 0 ] );
		assert( fence >= 0 );
		bool bSuccess = import_commit( buf, &dmabuf, newCommit );
		wlr_dmabuf_attributes_finish( &dmabuf );

		if ( bSuccess == true )
		{
			newCommit.commitID = ++maxCommmitID;
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

				i += strlen ( pchPreloadCopy + i );
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
		perror( "fork failed" );

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

		perror( "execvp failed" );
		_exit( 1 );
	}

	std::thread waitThread([]() {
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
					perror( "steamcompmgr: wait failed" );
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
	do {
		XEvent ev;
		XNextEvent (dpy, &ev);
		if ((ev.type & 0x7f) != KeymapNotify)
			discard_ignore (dpy, ev.xany.serial);
		if (debugEvents)
		{
			gpuvis_trace_printf ("event %d", ev.type);
			printf ("event %d\n", ev.type);
		}
		switch (ev.type) {
			case CreateNotify:
				if (ev.xcreatewindow.parent == root)
					add_win (dpy, ev.xcreatewindow.window, 0, ev.xcreatewindow.serial);
				break;
			case ConfigureNotify:
				configure_win (dpy, &ev.xconfigure);
				break;
			case DestroyNotify:
			{
				win * w = find_win(dpy, ev.xdestroywindow.window);

				if (w && w->id == ev.xdestroywindow.window)
					destroy_win (dpy, ev.xdestroywindow.window, True, True);
				break;
			}
			case MapNotify:
			{
				win * w = find_win(dpy, ev.xmap.window);

				if (w && w->id == ev.xmap.window)
					map_win (dpy, ev.xmap.window, ev.xmap.serial);
				break;
			}
			case UnmapNotify:
			{
				win * w = find_win(dpy, ev.xunmap.window);

				if (w && w->id == ev.xunmap.window)
					unmap_win (dpy, ev.xunmap.window, True);
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
							XSetInputFocus(dpy, currentKeyboardFocusWindow, RevertToNone, CurrentTime);
						}
					}
				}
				
				break;
			}
			case ReparentNotify:
				if (ev.xreparent.parent == root)
					add_win (dpy, ev.xreparent.window, 0, ev.xreparent.serial);
				else
				{
					win * w = find_win(dpy, ev.xreparent.window);

					if (w && w->id == ev.xreparent.window)
					{
						destroy_win (dpy, ev.xreparent.window, False, True);
					}
					else
					{
						// If something got reparented _to_ a toplevel window,
						// go check for the fullscreen workaround again.
						w = find_win(dpy, ev.xreparent.parent);
						if (w)
						{
							get_size_hints(dpy, w);
							focusDirty = True;
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
					// This shouldn't happen due to our pointer barriers,
					// but there is a known X server bug; warp to last good
					// position.
					cursor->resetPosition();
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
					damage_win (dpy, (XDamageNotifyEvent *) &ev);
				}
				else if (ev.type == xfixes_event + XFixesCursorNotify)
				{
					cursor->setDirty();
				}
				break;
		}
		XFlush(dpy);
	} while (XPending (dpy));
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

			perror( "steamcompmgr: dispatch_vblank: read failed" );
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
				perror(" steamcompmgr: dispatch_nudge: read failed" );
			break;
		}
	}
}

enum event_type {
	EVENT_X11,
	EVENT_VBLANK,
	EVENT_NUDGE,
	EVENT_COUNT // keep last
};

void
steamcompmgr_main (int argc, char **argv)
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
			case 'N':
				doRender = False;
				break;
			case 'F':
				debugFocus = True;
				break;
			case 'S':
				synchronize = True;
				break;
			case 'v':
				drawDebugInfo = True;
				break;
			case 'V':
				debugEvents = True;
				break;
			case 'e':
				steamMode = True;
				break;
			case 'c':
				alwaysComposite = True;
				break;
			case 'x':
				useXRes = False;
				break;
			default:
				break;
		}
	}

	int subCommandArg = -1;
	if ( optind < argc )
	{
		subCommandArg = optind;
	}

	if ( pipe2( g_nudgePipe, O_CLOEXEC | O_NONBLOCK ) != 0 )
	{
		perror( "steamcompmgr: pipe failed" );
		exit( 1 );
	}

	const char *pchEnableVkBasalt = getenv( "ENABLE_VKBASALT" );
	if ( pchEnableVkBasalt != nullptr && pchEnableVkBasalt[0] == '1' )
	{
		alwaysComposite = True;
	}

	dpy = XOpenDisplay ( wlserver_get_nested_display_name() );
	if (!dpy)
	{
		fprintf (stderr, "Can't open display\n");
		exit (1);
	}
	XSetErrorHandler (error);
	XSetIOErrorHandler (handle_io_error);
	if (synchronize)
		XSynchronize (dpy, 1);
	scr = DefaultScreen (dpy);
	root = RootWindow (dpy, scr);

	if (!XRenderQueryExtension (dpy, &render_event, &render_error))
	{
		fprintf (stderr, "No render extension\n");
		exit (1);
	}
	if (!XQueryExtension (dpy, COMPOSITE_NAME, &composite_opcode,
		&composite_event, &composite_error))
	{
		fprintf (stderr, "No composite extension\n");
		exit (1);
	}
	XCompositeQueryVersion (dpy, &composite_major, &composite_minor);

	if (!XDamageQueryExtension (dpy, &damage_event, &damage_error))
	{
		fprintf (stderr, "No damage extension\n");
		exit (1);
	}
	if (!XFixesQueryExtension (dpy, &xfixes_event, &xfixes_error))
	{
		fprintf (stderr, "No XFixes extension\n");
		exit (1);
	}
	if (!XShapeQueryExtension (dpy, &xshape_event, &xshape_error))
	{
		fprintf (stderr, "No XShape extension\n");
		exit (1);
	}
	if (!XFixesQueryExtension (dpy, &xfixes_event, &xfixes_error))
	{
		fprintf (stderr, "No XFixes extension\n");
		exit (1);
	}
	if (!XResQueryVersion (dpy, &xres_major, &xres_minor))
	{
		fprintf (stderr, "No XRes extension\n");
		exit (1);
	}
	if (xres_major != 1 || xres_minor < 2)
	{
		fprintf (stderr, "Unsupported XRes version: have %d.%d, want 1.2\n", xres_major, xres_minor);
		exit (1);
	}

	if (!register_cm(dpy))
	{
		exit (1);
	}

	register_systray(dpy);

	/* get atoms */
	steamAtom = XInternAtom (dpy, STEAM_PROP, False);
	steamInputFocusAtom = XInternAtom (dpy, "STEAM_INPUT_FOCUS", False);
	steamTouchClickModeAtom = XInternAtom (dpy, "STEAM_TOUCH_CLICK_MODE", False);
	gameAtom = XInternAtom (dpy, GAME_PROP, False);
	overlayAtom = XInternAtom (dpy, OVERLAY_PROP, False);
	opacityAtom = XInternAtom (dpy, OPACITY_PROP, False);
	gamesRunningAtom = XInternAtom (dpy, GAMES_RUNNING_PROP, False);
	screenScaleAtom = XInternAtom (dpy, SCREEN_SCALE_PROP, False);
	screenZoomAtom = XInternAtom (dpy, SCREEN_MAGNIFICATION_PROP, False);
	winTypeAtom = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE", False);
	winDesktopAtom = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
	winDockAtom = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
	winToolbarAtom = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE_TOOLBAR", False);
	winMenuAtom = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE_MENU", False);
	winUtilAtom = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);
	winSplashAtom = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE_SPLASH", False);
	winDialogAtom = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
	winNormalAtom = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);
	sizeHintsAtom = XInternAtom (dpy, "WM_NORMAL_HINTS", False);
	netWMStateFullscreenAtom = XInternAtom (dpy, "_NET_WM_STATE_FULLSCREEN", False);
	activeWindowAtom = XInternAtom (dpy, "_NET_ACTIVE_WINDOW", False);
	netWMStateAtom = XInternAtom (dpy, "_NET_WM_STATE", False);
	WMTransientForAtom = XInternAtom (dpy, "WM_TRANSIENT_FOR", False);
	netWMStateHiddenAtom = XInternAtom (dpy, "_NET_WM_STATE_HIDDEN", False);
	netWMStateFocusedAtom = XInternAtom (dpy, "_NET_WM_STATE_FOCUSED", False);
	netWMStateSkipTaskbarAtom = XInternAtom (dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
	netWMStateSkipPagerAtom = XInternAtom (dpy, "_NET_WM_STATE_SKIP_PAGER", False);
	WLSurfaceIDAtom = XInternAtom (dpy, "WL_SURFACE_ID", False);
	WMStateAtom = XInternAtom (dpy, "WM_STATE", False);
	utf8StringAtom = XInternAtom (dpy, "UTF8_STRING", False);
	netWMNameAtom = XInternAtom (dpy, "_NET_WM_NAME", False);
	netSystemTrayOpcodeAtom = XInternAtom (dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
	steamStreamingClientAtom = XInternAtom (dpy, "STEAM_STREAMING_CLIENT", False);
	steamStreamingClientVideoAtom = XInternAtom (dpy, "STEAM_STREAMING_CLIENT_VIDEO", False);
	gamescopeCtrlAppIDAtom = XInternAtom (dpy, "GAMESCOPECTRL_BASELAYER_APPID", False);
	WMChangeStateAtom = XInternAtom (dpy, "WM_CHANGE_STATE", False);

	root_width = DisplayWidth (dpy, scr);
	root_height = DisplayHeight (dpy, scr);

	allDamage = None;
	clipChanged = True;

	int vblankFD = vblank_init();
	assert( vblankFD >= 0 );

	currentOutputWidth = g_nOutputWidth;
	currentOutputHeight = g_nOutputHeight;

	XGrabServer (dpy);

	if (doRender)
	{
		XCompositeRedirectSubwindows (dpy, root, CompositeRedirectManual);
	}
	XSelectInput (dpy, root,
				  SubstructureNotifyMask|
				  ExposureMask|
				  StructureNotifyMask|
				  SubstructureRedirectMask|
				  FocusChangeMask|
				  PointerMotionMask|
				  LeaveWindowMask|
				  PropertyChangeMask);
	XShapeSelectInput (dpy, root, ShapeNotifyMask);
	XFixesSelectCursorInput(dpy, root, XFixesDisplayCursorNotifyMask);
	XQueryTree (dpy, root, &root_return, &parent_return, &children, &nchildren);
	for (uint32_t i = 0; i < nchildren; i++)
		add_win (dpy, children[i], i ? children[i-1] : None, 0);
	XFree (children);

	XUngrabServer (dpy);

	XF86VidModeLockModeSwitch(dpy, scr, True);

	std::unique_ptr<MouseCursor> cursor(new MouseCursor(dpy));

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
		focusDirty = False;
		bool vblank = false;

		if ( poll( pollfds, EVENT_COUNT, -1 ) < 0)
		{
			if ( errno == EAGAIN )
				continue;

			perror( "poll failed" );
			break;
		}

		if ( pollfds[ EVENT_X11 ].revents & POLLHUP )
		{
			fprintf( stderr, "Lost connection to the X11 server\n" );
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

		if (focusDirty == True)
		{
			determine_and_apply_focus(dpy, cursor.get());

			hasFocusWindow = currentFocusWindow != None;

			sdlwindow_pushupdate();
		}

		if (doRender)
		{
			// If our DRM state is out-of-date, refresh it. This might update
			// the output size.
			if ( BIsNested() == false )
				drm_poll_state( &g_DRM );

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

			if ( hasRepaint == true && vblank == true )
			{
				paint_all(dpy, cursor.get());

				// Consumed the need to repaint here
				hasRepaint = false;
			}

			// TODO: Look into making this _RAW
			// wlroots, seems to just use normal MONOTONIC
			// all over so this may be problematic to just change.
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);

			// If we're in the middle of a fade, pump an event into the loop to
			// make sure we keep pushing frames even if the app isn't updating.
			if (fadeOutWindow.id)
			{
				nudge_steamcompmgr();
			}

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
	}

	imageWaitThreadRun = false;
	waitListSem.signal();

	if ( statsThreadRun == true )
	{
		statsThreadRun = false;
		statsThreadSem.signal();
	}
}
