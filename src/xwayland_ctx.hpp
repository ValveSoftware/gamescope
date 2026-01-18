#pragma once

#include "backend.h"
#include "waitable.h"

#include <mutex>
#include <memory>
#include <vector>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/XRes.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/xf86vmode.h>

class gamescope_xwayland_server_t;
struct ignore;
struct steamcompmgr_win_t;
class MouseCursor;

extern LogScope xwm_log;

struct focus_t
{
	steamcompmgr_win_t				*focusWindow;
	steamcompmgr_win_t				*inputFocusWindow;
	uint32_t		inputFocusMode;
	steamcompmgr_win_t				*overlayWindow;
	steamcompmgr_win_t				*externalOverlayWindow;
	steamcompmgr_win_t				*notificationWindow;
	steamcompmgr_win_t				*overrideWindow;
	bool			outdatedInteractiveFocus;
	bool			bResetToCorner = false;
	bool			bResetToCenter = false;

	uint64_t		ulCurrentFocusSerial = UINT64_MAX;

	bool IsDirty();
};

struct CommitDoneEntry_t
{
	uint64_t winSeq;
	uint64_t commitID;
	uint64_t desiredPresentTime;
	uint64_t earliestPresentTime;
	uint64_t earliestLatchTime;
	bool fifo;
};

struct CommitDoneList_t
{
	std::mutex listCommitsDoneLock;
	std::vector< CommitDoneEntry_t > listCommitsDone;
};

struct xwayland_ctx_t final : public gamescope::IWaitable
{
	gamescope_xwayland_server_t *xwayland_server;
	Display			*dpy;

	// Not used for most of steamcompmgr thread. Just to sync whenever
	// wlserver wants it.
	std::mutex list_mutex;
	steamcompmgr_win_t				*list;
	int				scr;
	Window			root;
	XserverRegion	allDamage;
	bool			clipChanged;
	int				root_height, root_width;
	int				xfixes_event, xfixes_error;
	int				damage_event, damage_error;
	int				composite_event, composite_error;
	int				render_event, render_error;
	int				xshape_event, xshape_error;
	int				composite_opcode;
	int				xinput_opcode, xinput_event, xinput_error;
	Window			ourWindow;

	focus_t 		focus;
	Window 			currentKeyboardFocusWindow;
	Window			focusControlWindow;

	std::unique_ptr<MouseCursor> cursor;

	CommitDoneList_t doneCommits;

	double accum_x = 0.0;
	double accum_y = 0.0;

	bool force_windows_fullscreen = false;

	bool bTouchPointerEmulation = false;

	std::vector< steamcompmgr_win_t* > GetPossibleFocusWindows();
	void DetermineAndApplyFocus( const std::vector< steamcompmgr_win_t* > &vecPossibleFocusWindows );

	struct {
		Atom steamAtom;
		Atom gameAtom;
		Atom overlayAtom;
		Atom externalOverlayAtom;
		Atom gamesRunningAtom;
		Atom screenZoomAtom;
		Atom screenScaleAtom;
		Atom opacityAtom;
		Atom winTypeAtom;
		Atom winDesktopAtom;
		Atom winDockAtom;
		Atom winToolbarAtom;
		Atom winMenuAtom;
		Atom winUtilAtom;
		Atom winSplashAtom;
		Atom winDialogAtom;
		Atom winNormalAtom;
		Atom sizeHintsAtom;
		Atom netWMStateFullscreenAtom;
		Atom activeWindowAtom;
		Atom netWMStateAtom;
		Atom WMTransientForAtom;
		Atom netWMStateHiddenAtom;
		Atom netWMStateFocusedAtom;
		Atom netWMStateSkipTaskbarAtom;
		Atom netWMStateSkipPagerAtom;
		Atom WLSurfaceIDAtom;
		Atom WMStateAtom;
		Atom steamInputFocusAtom;
		Atom WMChangeStateAtom;
		Atom steamTouchClickModeAtom;
		Atom utf8StringAtom;
		Atom netWMNameAtom;
		Atom netWMIcon;
		Atom netSystemTrayOpcodeAtom;
		Atom steamStreamingClientAtom;
		Atom steamStreamingClientVideoAtom;
		Atom steamGamescopeVROverlayTarget;
		Atom gamescopePid;
		Atom gamescopeVROverlayForwarding;
		Atom gamescopeFocusableAppsAtom;
		Atom gamescopeFocusableWindowsAtom;
		Atom gamescopeFocusedWindowAtom;
		Atom gamescopeFocusedAppAtom;
		Atom gamescopeFocusedAppGfxAtom;
		Atom gamescopeCtrlAppIDAtom;
		Atom gamescopeCtrlWindowAtom;
		Atom gamescopeInputCounterAtom;
		Atom gamescopeScreenShotAtom;
		Atom gamescopeDebugScreenShotAtom;

		Atom gamescopeFocusDisplay;
		Atom gamescopeMouseFocusDisplay;
		Atom gamescopeKeyboardFocusDisplay;

		Atom gamescopeTuneableVBlankRedZone;
		Atom gamescopeTuneableRateOfDecay;

		Atom gamescopeScalingFilter;
		Atom gamescopeFSRSharpness;
		Atom gamescopeSharpness;

		Atom gamescopeXWaylandModeControl;

		Atom gamescopeFPSLimit;
		Atom gamescopeDynamicRefresh[gamescope::GAMESCOPE_SCREEN_TYPE_COUNT];
		Atom gamescopeLowLatency;

		Atom gamescopeFSRFeedback;

		Atom gamescopeBlurMode;
		Atom gamescopeBlurRadius;
		Atom gamescopeBlurFadeDuration;

		Atom gamescopeCompositeForce;
		Atom gamescopeCompositeDebug;

		Atom gamescopeAllowTearing;
		Atom gamescopeDisplayForceInternal;
		Atom gamescopeDisplayModeNudge;

		Atom gamescopeDisplayIsExternal;
		Atom gamescopeDisplayModeListExternal;

		Atom gamescopeCursorVisibleFeedback;

		Atom gamescopeSteamMaxHeight;

		Atom gamescopeVRRCapable;
		Atom gamescopeVRREnabled;
		Atom gamescopeVRRInUse;

		Atom gamescopeNewScalingFilter;
		Atom gamescopeNewScalingScaler;

		Atom gamescopeDisplayEdidPath;

		Atom gamescopeXwaylandServerId;

		Atom gamescopeDisplaySupportsHDR;
		Atom gamescopeDisplayHDREnabled;
		Atom gamescopeDebugForceHDR10Output;
		Atom gamescopeDebugForceHDRSupport;
		Atom gamescopeDebugHDRHeatmap;
		Atom gamescopeDebugHDRHeatmap_MSWCG;
		Atom gamescopeHDROutputFeedback;
		Atom gamescopeSDROnHDRContentBrightness;
		Atom gamescopeHDRInputGain;
		Atom gamescopeSDRInputGain;
		Atom gamescopeHDRItmEnable;
		Atom gamescopeHDRItmSDRNits;
		Atom gamescopeHDRItmTargetNits;
		Atom gamescopeColorLookPQ;
		Atom gamescopeColorLookG22;
		Atom gamescopeColorOutputVirtualWhite;
		Atom gamescopeHDRTonemapDisplayMetadata;
		Atom gamescopeHDRTonemapSourceMetadata;
		Atom gamescopeHDRTonemapOperator;

		Atom gamescopeForceWindowsFullscreen;

		Atom gamescopeColorLut3DOverride;
		Atom gamescopeColorShaperLutOverride;

		Atom gamescopeColorSDRGamutWideness;
		Atom gamescopeColorNightMode; // amount, hue, saturation
		Atom gamescopeColorManagementDisable;
		Atom gamescopeColorAppWantsHDRFeedback;
		Atom gamescopeColorAppHDRMetadataFeedback;
		Atom gamescopeColorSliderInUse;
		Atom gamescopeColorChromaticAdaptationMode;
		Atom gamescopeColorMuraCorrectionImage[gamescope::GAMESCOPE_SCREEN_TYPE_COUNT];
		Atom gamescopeColorMuraScale[gamescope::GAMESCOPE_SCREEN_TYPE_COUNT];
		Atom gamescopeColorMuraCorrectionDisabled[gamescope::GAMESCOPE_SCREEN_TYPE_COUNT];

		Atom gamescopeCreateXWaylandServer;
		Atom gamescopeCreateXWaylandServerFeedback;
		Atom gamescopeDestroyXWaylandServer;

		Atom gamescopeReshadeEffect;
		Atom gamescopeReshadeTechniqueIdx;

		Atom gamescopeDisplayRefreshRateFeedback;
		Atom gamescopeDisplayDynamicRefreshBasedOnGamePresence;

		Atom gamescopeMainSteamVROverlay;
		Atom steamosTouchPointerEmulation;

		Atom wineHwndStyle;
		Atom wineHwndStyleEx;

		Atom clipboard;
		Atom primarySelection;
		Atom targets;

		Atom wm_protocols;
		Atom wm_delete_window;
	} atoms;

	bool HasQueuedEvents();

	void Dispatch();

	int GetFD() final
	{
		return XConnectionNumber( dpy );
	}

	void OnPollIn() final
	{
		Dispatch();
	}

	void OnPollHangUp() final
	{
		xwm_log.errorf( "XWayland server hung up! This is fatal. Aborting..." );
		abort();
	}
};
