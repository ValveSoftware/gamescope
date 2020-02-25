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
#include <vector>

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/wait.h>
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
#include <X11/extensions/shape.h>
#include <X11/extensions/xf86vmode.h>

#include "main.hpp"
#include "wlserver.h"
#include "drm.hpp"
#include "rendervulkan.hpp"
#include "steamcompmgr.hpp"
#include "vblankmanager.hpp"

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

	Bool isSteam;
	Bool isSteamPopup;
	Bool wantsUnfocus;
	unsigned long long int gameID;
	Bool isOverlay;
	Bool isFullscreen;
	Bool isHidden;
	Bool sizeHintsSpecified;
	unsigned int requestedWidth;
	unsigned int requestedHeight;
	Bool nudged;
	Bool ignoreOverrideRedirect;

	Bool mouseMoved;

	long int WLsurfaceID;
	struct wlr_surface *wlrsurface;

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
static Window	currentOverlayWindow;
static Window	currentNotificationWindow;

static Window	ourWindow;
static XEvent	nudgeEvent;

Bool			gameFocused;

unsigned int 	gamesRunningCount;

float			overscanScaleRatio = 1.0;
float			zoomScaleRatio = 1.0;
float			globalScaleRatio = 1.0f;

Bool			focusDirty = False;
bool			hasRepaint = false;

unsigned long	damageSequence = 0;

#define			CURSOR_HIDE_TIME 10000

Bool			gotXError = False;

win				fadeOutWindow;
Bool			fadeOutWindowGone;
unsigned int	fadeOutStartTime;

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
static Atom		fullscreenAtom;
static Atom		WMStateAtom;
static Atom		WMStateHiddenAtom;
static Atom		WLSurfaceIDAtom;
static Atom		steamUnfocusAtom;

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

#define			FRAME_RATE_SAMPLING_PERIOD 160

unsigned int	frameCounter;
unsigned int	lastSampledFrameTime;
float			currentFrameRate;

static Bool		doRender = True;
static Bool		drawDebugInfo = False;
static Bool		debugEvents = False;
static Bool		steamMode = False;
static Bool		alwaysComposite = False;

std::mutex wayland_commit_lock;
std::vector<ResListEntry_t> wayland_commit_queue;

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
std::vector< std::pair< uint32_t, uint64_t > > waitList;

void imageWaitThreadRun( void )
{
wait:
	waitListSem.wait();

	bool bFound = false;
	uint32_t fence;
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

	gpuvis_trace_printf( "wait fence begin_ctx=%lu\n", commitID );
	vulkan_wait_for_fence( fence );
	gpuvis_trace_printf( "wait fence end_ctx=%lu\n", commitID );

	{
		std::unique_lock< std::mutex > lock( listCommitsDoneLock );

		listCommitsDone.push_back( commitID );
	}

	static Display *threadDPY = XOpenDisplay ( wlserver_get_nested_display() );
	XSendEvent( threadDPY, ourWindow, True, SubstructureRedirectMask, &nudgeEvent );
	XFlush( threadDPY );

	goto retry;
}

sem statsThreadSem;
std::mutex statsEventQueueLock;
std::vector< std::string > statsEventQueue;

std::string statsThreadPath;
int			statsPipeFD = -1;

void statsThreadRun( void )
{
	signal(SIGPIPE, SIG_IGN);

	while ( statsPipeFD == -1 )
	{
		statsPipeFD = open( statsThreadPath.c_str(), O_WRONLY );

		if ( statsPipeFD == -1 )
		{
			sleep( 10 );
		}
	}

wait:
	statsThreadSem.wait();

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

unsigned int
get_time_in_milliseconds (void)
{
	struct timeval  tv;

	gettimeofday (&tv, NULL);
	return tv.tv_sec * 1000 + tv.tv_usec / 1000;
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
		if ( w->wlrsurface == surf )
			return w;
	}

	return nullptr;
}

static void
set_win_hidden (Display *dpy, win *w, Bool hidden)
{
	if (!w || w->id == None)
	{
		return;
	}

	if (w->isHidden == hidden)
	{
		return;
	}


	if (hidden == True)
	{
		XChangeProperty(dpy, w->id, WMStateAtom, XA_ATOM, 32,
						PropModeReplace, (unsigned char *)&WMStateHiddenAtom, 1);
	}
	else
	{
		XChangeProperty(dpy, w->id, WMStateAtom, XA_ATOM, 32,
						PropModeReplace, (unsigned char *)NULL, 0);
	}

	w->isHidden = hidden;
}

static void
release_commit ( commit_t &commit )
{
	if ( commit.fb_id != 0 )
	{
		drm_free_fbid( &g_DRM, commit.fb_id );
		commit.fb_id = 0;
	}

	if ( commit.vulkanTex != 0 )
	{
		vulkan_free_texture( commit.vulkanTex );
		commit.vulkanTex = 0;
	}
}

static bool
import_commit ( struct wlr_dmabuf_attributes *dmabuf, commit_t &commit )
{
	if ( BIsNested() == False )
	{
		// We'll also need a copy for Vulkan to consume below.

		int fdCopy = dup( dmabuf->fd[0] );

		if ( fdCopy == -1 )
		{
			close( dmabuf->fd[0] );
			return false;
		}

		commit.fb_id = drm_fbid_from_dmabuf( &g_DRM, dmabuf );
		assert( commit.fb_id != 0 );

		close( dmabuf->fd[0] );
		dmabuf->fd[0] = fdCopy;
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
	, m_hasPlane(false)
	, m_display(display)
{
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

	const bool suspended = get_time_in_milliseconds() - m_lastMovedTime > CURSOR_HIDE_TIME;
	if (!m_hideForMovement && suspended) {
		m_hideForMovement = true;

		win *window = find_win(m_display, currentFocusWindow);

		// Rearm warp count
		if (window) {
			window->mouseMoved = 0;
		}

		// We're hiding the cursor, force redraw if we were showing it
		if (window && gameFocused && !m_imageEmpty ) {
			hasRepaint = true;
			XSendEvent(m_display, ourWindow, true, SubstructureRedirectMask, &nudgeEvent);
		}
	}
}

void MouseCursor::warp(int x, int y)
{
	XWarpPointer(m_display, None, currentFocusWindow, 0, 0, 0, 0, x, y);
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
	win *window = find_win(m_display, currentFocusWindow);

	// If we had barriers before, get rid of them.
	for (i = 0; i < 4; i++) {
		if (m_scaledFocusBarriers[i] != None) {
			XFixesDestroyPointerBarrier(m_display, m_scaledFocusBarriers[i]);
			m_scaledFocusBarriers[i] = None;
		}
	}

	if (!gameFocused) {
		return;
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
	if (pointerX >= window->a.width || pointerY >= window->a.height) {
		warp(window->a.width / 2, window->a.height / 2);
	}
}

void MouseCursor::move(int x, int y)
{
	// Some stuff likes to warp in-place
	if (m_x == pointerX && m_y == pointerY) {
		return;
	}
	m_x = pointerX;
	m_y = pointerY;

	win *window = find_win(m_display, currentFocusWindow);

	if (window && gameFocused) {
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
	move(pointerX, pointerY);
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

	if (m_texture) {
		vulkan_free_texture(m_texture);
		m_texture = 0;
	}

	// Assume the cursor is fully translucent unless proven otherwise.
	bool bNoCursor = true;

	unsigned int cursorDataBuffer[m_width * m_height];
	for (int i = 0; i < m_width * m_height; i++) {
		cursorDataBuffer[i] = image->pixels[i];

		if ( cursorDataBuffer[i] & 0x000000ff ) {
			bNoCursor = false;
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

	m_texture = vulkan_create_texture_from_bits(m_width, m_height, VK_FORMAT_R8G8B8A8_UNORM,
												cursorDataBuffer);
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

	// We assume 'window' is as a fullscreen window always positioned at (0,0).
	const int winX = pointerX;
	const int winY = pointerY;

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

	pComposite->layers[ curLayer ].flOpacity = 1.0;

	pComposite->layers[ curLayer ].flScaleX = 1.0;
	pComposite->layers[ curLayer ].flScaleY = 1.0;

	pComposite->layers[ curLayer ].flOffsetX = -scaledX;
	pComposite->layers[ curLayer ].flOffsetY = -scaledY;

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
		currentScaleRatio *= globalScaleRatio;

		drawXOffset = (currentOutputWidth - sourceWidth * currentScaleRatio) / 2.0f;
		drawYOffset = (currentOutputHeight - sourceHeight * currentScaleRatio) / 2.0f;

		if ( zoomScaleRatio != 1.0 )
		{
			drawXOffset += ((sourceWidth / 2) - cursor->x()) * currentScaleRatio;
			drawYOffset += ((sourceHeight / 2) - cursor->y()) * currentScaleRatio;
		}
	}

	int curLayer = pComposite->nLayerCount;

	pComposite->layers[ curLayer ].flOpacity = w->isOverlay ? w->opacity / (float)OPAQUE : 1.0f;

	pComposite->layers[ curLayer ].flScaleX = 1.0 / currentScaleRatio;
	pComposite->layers[ curLayer ].flScaleY = 1.0 / currentScaleRatio;

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

		pComposite->layers[ curLayer ].flOffsetX = (currentOutputWidth - xOffset - width) * -1.0f;
		pComposite->layers[ curLayer ].flOffsetY = (currentOutputHeight - yOffset - height) * -1.0f;
	}
	else
	{
		pComposite->layers[ curLayer ].flOffsetX = -drawXOffset;
		pComposite->layers[ curLayer ].flOffsetY = -drawYOffset;
	}

	pPipeline->layerBindings[ curLayer ].surfaceWidth = w->a.width;
	pPipeline->layerBindings[ curLayer ].surfaceHeight = w->a.height;

	pPipeline->layerBindings[ curLayer ].zpos = w->isOverlay ? 1 : 0;

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
	gpuvis_trace_printf( "paint_all begin_ctx=%llu\n", paintID );
	win	*w;
	win	*overlay;
	win	*notification;

	unsigned int currentTime = get_time_in_milliseconds();
	Bool fadingOut = ((currentTime - fadeOutStartTime) < FADE_OUT_DURATION && fadeOutWindow.id != None);

	w = find_win(dpy, currentFocusWindow);
	overlay = find_win(dpy, currentOverlayWindow);
	notification = find_win(dpy, currentNotificationWindow);

	if ( !w )
	{
		return;
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
		else if ( w->isSteamPopup )
		{
			stats_printf( "focus=steampopup\n" );
		}
		else
		{
			stats_printf( "focus=%i\n", w->gameID );
		}
	}

	struct Composite_t composite = {};
	struct VulkanPipeline_t pipeline = {};

	// Fading out from previous window?
	if (fadingOut)
	{
		double newOpacity = ((currentTime - fadeOutStartTime) / (double)FADE_OUT_DURATION);

		// Draw it in the background
		fadeOutWindow.opacity = (1.0d - newOpacity) * OPAQUE;
		paint_window(dpy, &fadeOutWindow, &composite, &pipeline, False, cursor);

		w = find_win(dpy, currentFocusWindow);

		// Blend new window on top with linear crossfade
		w->opacity = newOpacity * OPAQUE;

		paint_window(dpy, w, &composite, &pipeline, False, cursor);
	}
	else
	{
		w = find_win(dpy, currentFocusWindow);
		// Just draw focused window as normal, be it Steam or the game
		paint_window(dpy, w, &composite, &pipeline, False, cursor);

		if (fadeOutWindow.id) {

			if (fadeOutWindowGone)
			{
				// This is the only reference to these resources now.
				fadeOutWindowGone = False;
			}
			fadeOutWindow.id = None;

			// Finished fading out, mark previous window hidden
			set_win_hidden(dpy, &fadeOutWindow, True);
		}
	}

	if (gamesRunningCount && overlay)
	{
		if (overlay->opacity)
		{
			paint_window(dpy, overlay, &composite, &pipeline, False, cursor);
		}
	}

	if (gamesRunningCount && notification)
	{
		if (notification->opacity)
		{
			paint_window(dpy, notification, &composite, &pipeline, True, cursor);
		}
	}

	// Draw cursor if we need to
	if (w && gameFocused) {
		cursor->paint(w, &composite, &pipeline );
	}

	if (drawDebugInfo)
		paint_debug_info(dpy);

	bool bDoComposite = true;

	if ( BIsNested() == false && alwaysComposite == False )
	{
		if ( drm_can_avoid_composite( &g_DRM, &composite, &pipeline ) == true )
		{
			bDoComposite = false;
		}
	}

	if ( bDoComposite == true )
	{
		bool bResult = vulkan_composite( &composite, &pipeline );

		if ( bResult != true )
		{
			fprintf (stderr, "composite alarm!!!\n");
		}

		if ( BIsNested() == True )
		{
			vulkan_present_to_window();
		}
		else
		{
			memset( &composite, 0, sizeof( composite ) );
			composite.nLayerCount = 1;
			composite.layers[ 0 ].flScaleX = 1.0;
			composite.layers[ 0 ].flScaleY = 1.0;
			composite.layers[ 0 ].flOpacity = 1.0;

			memset( &pipeline, 0, sizeof( pipeline ) );

			pipeline.layerBindings[ 0 ].surfaceWidth = g_nOutputWidth;
			pipeline.layerBindings[ 0 ].surfaceHeight = g_nOutputHeight;

			pipeline.layerBindings[ 0 ].fbid = vulkan_get_last_composite_fbid();
			pipeline.layerBindings[ 0 ].bFilter = false;

			bool bFlip = drm_can_avoid_composite( &g_DRM, &composite, &pipeline );

			// We should always handle a 1-layer flip
			assert( bFlip == true );

			drm_atomic_commit( &g_DRM, &composite, &pipeline );
		}
	}
	else
	{
		assert( BIsNested() == false );

		drm_atomic_commit( &g_DRM, &composite, &pipeline );
	}

	gpuvis_trace_printf( "paint_all end_ctx=%llu\n", paintID );
	gpuvis_trace_printf( "paint_all %i layers, composite %i\n", (int)composite.nLayerCount, bDoComposite );
}

static void
determine_and_apply_focus (Display *dpy, MouseCursor *cursor)
{
	win *w, *focus = NULL;
	win *steam = nullptr;
	win *steampopup = nullptr;

	gameFocused = False;

	unsigned long maxDamageSequence = 0;
	Bool usingOverrideRedirectWindow = False;

	unsigned int maxOpacity = 0;

	for (w = list; w; w = w->next)
	{
		if ( w->isSteam == True )
		{
			steam = w;
		}

		if ( w->isSteamPopup == True )
		{
			steampopup = w;
		}

		// We allow using an override redirect window in some cases, but if we have
		// a choice between two windows we always prefer the non-override redirect one.
		Bool windowIsOverrideRedirect = w->a.override_redirect && !w->ignoreOverrideRedirect;

		if (w->gameID && w->a.map_state == IsViewable && w->a.c_class == InputOutput &&
			(w->damage_sequence >= maxDamageSequence) &&
			(!windowIsOverrideRedirect || !usingOverrideRedirectWindow))
		{
			focus = w;
			gameFocused = True;
			maxDamageSequence = w->damage_sequence;

			if (windowIsOverrideRedirect)
			{
				usingOverrideRedirectWindow = True;
			}
		}

		if (w->isOverlay)
		{
			if (w->a.width == 1920 && w->opacity >= maxOpacity)
			{
				currentOverlayWindow = w->id;
				maxOpacity = w->opacity;
			}
			else
			{
				currentNotificationWindow = w->id;
			}
		}
	}

	if ( !gameFocused )
	{
		if ( steampopup && ( ( steam && steam->wantsUnfocus ) || !steam ) )
		{
			focus = steampopup;
		}

		if ( !focus && steam )
		{
			focus = steam;
		}
	}

	if (!focus)
	{
		currentFocusWindow = None;
		return;
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
	if ( currentFocusWindow != focus->id )
	{
		set_win_hidden( dpy, find_win(dpy, currentFocusWindow), True );

		if ( focus->wlrsurface != nullptr )
		{
			wlserver_lock();
			wlserver_keyboardfocus( focus->wlrsurface );
			wlserver_mousefocus( focus->wlrsurface );
			wlserver_unlock();
		}

		gpuvis_trace_printf( "determine_and_apply_focus focus %lu\n", focus->id );
	}

	currentFocusWindow = focus->id;

	w = focus;

	set_win_hidden(dpy, w, False);

	cursor->constrainPosition();

	if (gameFocused || (!gamesRunningCount && list[0].id != focus->id))
	{
		XRaiseWindow(dpy, focus->id);
	}

	XSetInputFocus(dpy, focus->id, RevertToNone, CurrentTime);

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
		XSelectInput(dpy, children[i], PointerMotionMask);
		i++;
	}

	XFree (children);
}

/* Get prop from window
 *   not found: default
 *   otherwise the value
 */
static unsigned int
get_prop(Display *dpy, Window win, Atom prop, unsigned int def)
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
		return i;
	}
	return def;
}

static void
get_size_hints(Display *dpy, win *w)
{
	XSizeHints hints;
	long hintsSpecified;

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
map_win (Display *dpy, Window id, unsigned long sequence)
{
	win		*w = find_win (dpy, id);

	if (!w)
		return;

	w->a.map_state = IsViewable;

	/* This needs to be here or else we lose transparency messages */
	XSelectInput (dpy, id, PropertyChangeMask | SubstructureNotifyMask |
	PointerMotionMask | LeaveWindowMask);

	/* This needs to be here since we don't get PropertyNotify when unmapped */
	w->opacity = get_prop (dpy, w->id, opacityAtom, TRANSLUCENT);

	w->isSteam = get_prop (dpy, w->id, steamAtom, 0);

	XTextProperty tp;
	XGetTextProperty ( dpy, id, &tp, XA_WM_NAME );

	if ( tp.value && strcmp( (const char*)tp.value, "SP" ) == 0 )
	{
		w->isSteamPopup = True;
	}
	else
	{
		w->isSteamPopup = False;
	}

	w->wantsUnfocus = get_prop(dpy, w->id, steamUnfocusAtom, 1);

	if ( steamMode == True )
	{
		w->gameID = get_prop (dpy, w->id, gameAtom, 0);
	}
	else
	{
		w->gameID = w->id;
	}
	w->isOverlay = get_prop (dpy, w->id, overlayAtom, 0);

	get_size_hints(dpy, w);

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
		free (new_win);
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
	new_win->opacity = TRANSLUCENT;

	new_win->isOverlay = False;
	new_win->isSteam = False;
	new_win->isSteamPopup = False;
	new_win->wantsUnfocus = True;

	if ( steamMode == True )
	{
		new_win->gameID = 0;
	}
	else
	{
		new_win->gameID = id;
	}
	new_win->isFullscreen = False;
	new_win->isHidden = False;
	new_win->sizeHintsSpecified = False;
	new_win->requestedWidth = 0;
	new_win->requestedHeight = 0;
	new_win->nudged = False;
	new_win->ignoreOverrideRedirect = False;

	new_win->mouseMoved = False;

	new_win->WLsurfaceID = 0;
	new_win->wlrsurface = NULL;

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
			free (w);
			break;
		}
}

static void
destroy_win (Display *dpy, Window id, Bool gone, Bool fade)
{
	if (currentFocusWindow == id && gone)
		currentFocusWindow = None;
	if (currentOverlayWindow == id && gone)
		currentOverlayWindow = None;
	if (currentNotificationWindow == id && gone)
		currentNotificationWindow = None;
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
	if (w->gameID && w->damage_sequence == 0)
		focusDirty = True;

	w->damage_sequence = damageSequence++;

	// If we just passed the focused window, we might be eliglible to take over
	if (focus && focus != w && w->gameID &&
		w->damage_sequence > focus->damage_sequence)
		focusDirty = True;

	if (w->damage)
		XDamageSubtract(dpy, w->damage, None, None);

	gpuvis_trace_printf( "damage_win win %lx gameID %llu\n", w->id, w->gameID );
}

static void
handle_wl_surface_id(win *w, long surfaceID)
{
	struct wlr_surface *surface = NULL;

	wlserver_lock();

	surface = wlserver_get_surface( surfaceID );

	if ( surface == NULL )
	{
		// We'll retry next time
		w->WLsurfaceID = surfaceID;

		wlserver_unlock();
		return;
	}

	// If we already focused on our side and are handling this late,
	// let wayland know now.
	if ( w->id == currentFocusWindow )
	{
		wlserver_keyboardfocus( surface );
		wlserver_mousefocus( surface );
	}

	// Pull the first buffer out of that window, if needed
	xwayland_surface_role_commit( surface );

	wlserver_unlock();

	w->wlrsurface = surface;
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


	Atom fullScreenSupported = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
	XChangeProperty(dpy, root, XInternAtom(dpy, "_NET_SUPPORTED", False),
					XA_ATOM, 32, PropModeAppend, (unsigned char *)&fullScreenSupported, 1);

	XSetSelectionOwner (dpy, a, w, 0);

	ourWindow = w;

	nudgeEvent.xclient.type = ClientMessage;
	nudgeEvent.xclient.window = ourWindow;
	nudgeEvent.xclient.format = 32;

	return True;
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
					gpuvis_trace_printf( "commit %lu done\n", w->commit_queue[ j ].commitID );
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

void check_new_wayland_res( void )
{
	std::lock_guard<std::mutex> lock( wayland_commit_lock );

	for ( uint32_t i = 0; i < wayland_commit_queue.size(); i++ )
	{
		win	*w = find_win( wayland_commit_queue[ i ].surf );

		assert( wayland_commit_queue[ i ].attribs.fd[0] != -1 );

		if ( w == nullptr )
		{
			close( wayland_commit_queue[ i ].attribs.fd[0] );
			fprintf (stderr, "waylandres but no win\n");
			continue;
		}

		commit_t newCommit = {};

		bool bSuccess = import_commit( &wayland_commit_queue[ i ].attribs, newCommit );

		if ( bSuccess == true )
		{
			newCommit.commitID = ++maxCommmitID;
			w->commit_queue.push_back( newCommit );
		}

		uint32_t fence = vulkan_get_texture_fence( newCommit.vulkanTex );

		gpuvis_trace_printf( "pushing wait for commit %lu\n", newCommit.commitID );
		{
			std::unique_lock< std::mutex > lock( waitListLock );

			waitList.push_back( std::make_pair( fence, newCommit.commitID ) );
		}

		// Wake up commit wait thread if chilling
		waitListSem.signal();
	}

	wayland_commit_queue.clear();
}

int
steamcompmgr_main (int argc, char **argv)
{
	Display	   *dpy;
	XEvent	    ev;
	Window	    root_return, parent_return;
	Window	    *children;
	unsigned int    nchildren;
	int		    composite_major, composite_minor;
	int		    o;
	int			readyPipeFD = -1;

	// :/
	optind = 1;

	while ((o = getopt (argc, argv, ":R:T:w:h:W:H:r:NSvVecslnb")) != -1)
	{
		switch (o) {
			case 'R':
				readyPipeFD = open( optarg, O_WRONLY );
				break;
			case 'T':
				statsThreadPath = optarg;
				{
					std::thread statsThreads( statsThreadRun );
					statsThreads.detach();
				}
				break;
			case 'N':
				doRender = False;
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
			default:
				break;
		}
	}

	dpy = XOpenDisplay ( wlserver_get_nested_display() );
	if (!dpy)
	{
		fprintf (stderr, "Can't open display\n");
		exit (1);
	}
	XSetErrorHandler (error);
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

	if (!register_cm(dpy))
	{
		exit (1);
	}

	/* get atoms */
	steamAtom = XInternAtom (dpy, STEAM_PROP, False);
	steamUnfocusAtom = XInternAtom (dpy, "STEAM_UNFOCUS", False);
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
	fullscreenAtom = XInternAtom (dpy, "_NET_WM_STATE_FULLSCREEN", False);
	WMStateAtom = XInternAtom (dpy, "_NET_WM_STATE", False);
	WMStateHiddenAtom = XInternAtom (dpy, "_NET_WM_STATE_HIDDEN", False);
	WLSurfaceIDAtom = XInternAtom (dpy, "WL_SURFACE_ID", False);

	root_width = DisplayWidth (dpy, scr);
	root_height = DisplayHeight (dpy, scr);

	allDamage = None;
	clipChanged = True;

	if ( vulkan_init() != True )
	{
		fprintf (stderr, "alarm!!!\n");
	}

	vblank_init();

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
		dprintf( readyPipeFD, "%s\n", wlserver_get_nested_display() );
		close( readyPipeFD );
		readyPipeFD = -1;
	}

	if ( g_nSubCommandArg != 0 )
	{
		pid_t pid;

		sigset_t fullset;
		sigfillset( &fullset );

		posix_spawnattr_t attr;

		posix_spawnattr_init( &attr );
		posix_spawnattr_setflags( &attr, POSIX_SPAWN_SETSIGDEF );
		posix_spawnattr_setsigdefault( &attr, &fullset );

		// (Don't Lose) The Children
		prctl( PR_SET_CHILD_SUBREAPER );

		posix_spawnp( &pid, argv[ g_nSubCommandArg ], NULL, &attr, &argv[ g_nSubCommandArg ], environ );

		std::thread waitThread([](){
			while( wait( nullptr ) >= 0 )
			{
				;
			}

			run = false;
		});

		waitThread.detach();
	}

	std::thread imageWaitThread( imageWaitThreadRun );
	imageWaitThread.detach();

	for (;;)
	{
		focusDirty = False;
		bool vblank = false;

		do {
			XNextEvent (dpy, &ev);
			if ((ev.type & 0x7f) != KeymapNotify)
				discard_ignore (dpy, ev.xany.serial);
			if (debugEvents)
			{
				gpuvis_trace_printf ("event %d\n", ev.type);
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
					/* check if Trans property was changed */
					if (ev.xproperty.atom == opacityAtom)
					{
						/* reset mode and redraw window */
						win * w = find_win(dpy, ev.xproperty.window);
						if (w && w->isOverlay)
						{
							unsigned int newOpacity = get_prop(dpy, w->id, opacityAtom, TRANSLUCENT);

							if (newOpacity != w->opacity)
							{
								w->opacity = newOpacity;

								if ( gameFocused && ( w->id == currentOverlayWindow || w->id == currentNotificationWindow ) )
								{
									hasRepaint = true;
								}
							}

							if (w->isOverlay)
							{
								set_win_hidden(dpy, w, w->opacity == TRANSLUCENT);
							}

							unsigned int maxOpacity = 0;

							for (w = list; w; w = w->next)
							{
								if (w->isOverlay)
								{
									if (w->a.width == 1920 && w->opacity >= maxOpacity)
									{
										currentOverlayWindow = w->id;
										maxOpacity = w->opacity;
									}
								}
							}
						}
					}
					if (ev.xproperty.atom == steamAtom)
					{
						win * w = find_win(dpy, ev.xproperty.window);
						if (w)
						{
							w->isSteam = get_prop(dpy, w->id, steamAtom, 0);
							focusDirty = True;
						}
					}
					if (ev.xproperty.atom == steamUnfocusAtom )
					{
						win * w = find_win(dpy, ev.xproperty.window);
						if (w)
						{
							w->wantsUnfocus = get_prop(dpy, w->id, steamUnfocusAtom, 1);
							focusDirty = True;
						}
					}
					if (ev.xproperty.atom == gameAtom)
					{
						win * w = find_win(dpy, ev.xproperty.window);
						if (w)
						{
							w->gameID = get_prop(dpy, w->id, gameAtom, 0);
							focusDirty = True;
						}
					}
					if (ev.xproperty.atom == overlayAtom)
					{
						win * w = find_win(dpy, ev.xproperty.window);
						if (w)
						{
							w->isOverlay = get_prop(dpy, w->id, overlayAtom, 0);
							focusDirty = True;
						}
					}
					if (ev.xproperty.atom == sizeHintsAtom)
					{
						win * w = find_win(dpy, ev.xproperty.window);
						if (w)
						{
							get_size_hints(dpy, w);
							focusDirty = True;
						}
					}
					if (ev.xproperty.atom == gamesRunningAtom)
					{
						gamesRunningCount = get_prop(dpy, root, gamesRunningAtom, 0);

						focusDirty = True;
					}
					if (ev.xproperty.atom == screenScaleAtom)
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
					if (ev.xproperty.atom == screenZoomAtom)
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
					break;
					case ClientMessage:
					{
						win * w = find_win(dpy, ev.xclient.window);
						if (w)
						{
							if (ev.xclient.message_type == WLSurfaceIDAtom)
							{
								handle_wl_surface_id( w, ev.xclient.data.l[0]);
							}
							else
							{
								if ((unsigned)ev.xclient.data.l[1] == fullscreenAtom)
								{
									w->isFullscreen = ev.xclient.data.l[0];

									focusDirty = True;
								}
							}
						}

						if ( ev.xclient.data.l[0] == 24 && ev.xclient.data.l[1] == 8 )
						{
							// Message from vblankmanager
							gpuvis_trace_printf( "got vblank\n" );
							vblank = true;
						}

						break;
					}
					case LeaveNotify:
						if (ev.xcrossing.window == currentFocusWindow)
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
						if (w && w->id == currentFocusWindow)
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
		} while (QLength (dpy));

		if (focusDirty == True)
			determine_and_apply_focus(dpy, cursor.get());

		if (doRender)
		{
			// Pick our width/height for this potential frame, regardless of how it might change later
			// At some point we might even add proper locking so we get real updates atomically instead
			// of whatever jumble of races the below might cause over a couple of frames
			if ( currentOutputWidth != g_nOutputWidth ||
				 currentOutputHeight != g_nOutputHeight )
			{
				if ( BIsNested() == true )
				{
					bool bRet = vulkan_remake_swapchain();

					assert( bRet == true );
				}
				else
				{
					// Remake output images if we ever care about resizing there
				}

				currentOutputWidth = g_nOutputWidth;
				currentOutputHeight = g_nOutputHeight;
			}
			// See if we have surfaceIDs we need to handle late
			for (win *w = list; w; w = w->next)
			{
				if ( w->wlrsurface == NULL && w->WLsurfaceID != 0 )
				{
					handle_wl_surface_id( w, w->WLsurfaceID );

					if ( w->wlrsurface != NULL )
					{
						// Got it now.
						w->WLsurfaceID = 0;

						fprintf ( stderr, "handled late WLSurfaceID\n" );
					}
				}
			}

			handle_done_commits();

			check_new_wayland_res();

			if ( hasRepaint == true && vblank == true )
			{
				paint_all(dpy, cursor.get());

				// Consumed the need to repaint here
				hasRepaint = false;
			}

			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);

			// If we're in the middle of a fade, pump an event into the loop to
			// make sure we keep pushing frames even if the app isn't updating.
			if (fadeOutWindow.id)
				XSendEvent(dpy, ourWindow, True, SubstructureRedirectMask, &nudgeEvent);

			cursor->updatePosition();

			// Ask for a new surface every vblank
			if ( vblank == true )
			{
				for (win *w = list; w; w = w->next)
				{
					if ( w->wlrsurface )
					{
						// Acknowledge commit once.
						wlserver_lock();
						wlserver_send_frame_done(w->wlrsurface, &now);
						wlserver_unlock();
					}
				}
			}

			vulkan_garbage_collect();

			vblank = false;
		}
	}
}
