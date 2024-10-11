#include "Script/Script.h"

#include <X11/Xlib.h>

#include <cstdio>
#include <thread>
#include <mutex>
#include <vector>
#include <cstring>
#include <string>
#if defined(__linux__)
#include <sys/capability.h>
#endif
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <getopt.h>
#include <signal.h>
#include <unistd.h>
#include <float.h>
#include <climits>

#include "main.hpp"
#include "steamcompmgr.hpp"
#include "rendervulkan.hpp"
#include "wlserver.hpp"
#include "convar.h"
#include "gpuvis_trace_utils.h"
#include "Utils/TempFiles.h"
#include "Utils/Version.h"
#include "Utils/Process.h"
#include "Utils/Defer.h"

#include "backends.h"
#include "refresh_rate.h"

#if HAVE_PIPEWIRE
#include "pipewire.hpp"
#endif

#include <wayland-client.h>

using namespace std::literals;

EStreamColorspace g_ForcedNV12ColorSpace = k_EStreamColorspace_Unknown;
extern gamescope::ConVar<bool> cv_adaptive_sync;

const char *gamescope_optstring = nullptr;
const char *g_pOriginalDisplay = nullptr;
const char *g_pOriginalWaylandDisplay = nullptr;

int g_nCursorScaleHeight = -1;

const struct option *gamescope_options = (struct option[]){
	{ "help", no_argument, nullptr, 0 },
	{ "version", no_argument, nullptr, 0 },
	{ "nested-width", required_argument, nullptr, 'w' },
	{ "nested-height", required_argument, nullptr, 'h' },
	{ "nested-refresh", required_argument, nullptr, 'r' },
	{ "max-scale", required_argument, nullptr, 'm' },
	{ "scaler", required_argument, nullptr, 'S' },
	{ "filter", required_argument, nullptr, 'F' },
	{ "output-width", required_argument, nullptr, 'W' },
	{ "output-height", required_argument, nullptr, 'H' },
	{ "sharpness", required_argument, nullptr, 0 },
	{ "fsr-sharpness", required_argument, nullptr, 0 },
	{ "rt", no_argument, nullptr, 0 },
	{ "prefer-vk-device", required_argument, 0 },
	{ "expose-wayland", no_argument, 0 },
	{ "mouse-sensitivity", required_argument, nullptr, 's' },
	{ "mangoapp", no_argument, nullptr, 0 },

	{ "backend", required_argument, nullptr, 0 },

	// nested mode options
	{ "nested-unfocused-refresh", required_argument, nullptr, 'o' },
	{ "borderless", no_argument, nullptr, 'b' },
	{ "fullscreen", no_argument, nullptr, 'f' },
	{ "grab", no_argument, nullptr, 'g' },
	{ "force-grab-cursor", no_argument, nullptr, 0 },
	{ "display-index", required_argument, nullptr, 0 },

	// embedded mode options
	{ "disable-layers", no_argument, nullptr, 0 },
	{ "debug-layers", no_argument, nullptr, 0 },
	{ "prefer-output", required_argument, nullptr, 'O' },
	{ "default-touch-mode", required_argument, nullptr, 0 },
	{ "generate-drm-mode", required_argument, nullptr, 0 },
	{ "immediate-flips", no_argument, nullptr, 0 },
	{ "adaptive-sync", no_argument, nullptr, 0 },
	{ "framerate-limit", required_argument, nullptr, 0 },

	// openvr options
#if HAVE_OPENVR
	{ "vr-overlay-key", required_argument, nullptr, 0 },
	{ "vr-overlay-explicit-name", required_argument, nullptr, 0 },
	{ "vr-overlay-default-name", required_argument, nullptr, 0 },
	{ "vr-overlay-icon", required_argument, nullptr, 0 },
	{ "vr-overlay-show-immediately", no_argument, nullptr, 0 },
	{ "vr-overlay-enable-control-bar", no_argument, nullptr, 0 },
	{ "vr-overlay-enable-control-bar-keyboard", no_argument, nullptr, 0 },
	{ "vr-overlay-enable-control-bar-close", no_argument, nullptr, 0 },
	{ "vr-overlay-modal", no_argument, nullptr, 0 },
	{ "vr-overlay-physical-width", required_argument, nullptr, 0 },
	{ "vr-overlay-physical-curvature", required_argument, nullptr, 0 },
	{ "vr-overlay-physical-pre-curve-pitch", required_argument, nullptr, 0 },
	{ "vr-scroll-speed", required_argument, nullptr, 0 },
#endif

	// wlserver options
	{ "xwayland-count", required_argument, nullptr, 0 },

	// steamcompmgr options
	{ "cursor", required_argument, nullptr, 0 },
	{ "cursor-hotspot", required_argument, nullptr, 0 },
	{ "cursor-scale-height", required_argument, nullptr, 0 },
	{ "ready-fd", required_argument, nullptr, 'R' },
	{ "stats-path", required_argument, nullptr, 'T' },
	{ "hide-cursor-delay", required_argument, nullptr, 'C' },
	{ "debug-focus", no_argument, nullptr, 0 },
	{ "synchronous-x11", no_argument, nullptr, 0 },
	{ "debug-hud", no_argument, nullptr, 'v' },
	{ "debug-events", no_argument, nullptr, 0 },
	{ "steam", no_argument, nullptr, 'e' },
	{ "force-composition", no_argument, nullptr, 'c' },
	{ "composite-debug", no_argument, nullptr, 0 },
	{ "disable-xres", no_argument, nullptr, 'x' },
	{ "fade-out-duration", required_argument, nullptr, 0 },
	{ "force-orientation", required_argument, nullptr, 0 },
	{ "force-windows-fullscreen", no_argument, nullptr, 0 },

	{ "disable-color-management", no_argument, nullptr, 0 },
	{ "sdr-gamut-wideness", required_argument, nullptr, 0 },
	{ "hdr-enabled", no_argument, nullptr, 0 },
	{ "hdr-sdr-content-nits", required_argument, nullptr, 0 },
	{ "hdr-itm-enabled", no_argument, nullptr, 0 },
	{ "hdr-itm-sdr-nits", required_argument, nullptr, 0 },
	{ "hdr-itm-target-nits", required_argument, nullptr, 0 },
	{ "hdr-debug-force-support", no_argument, nullptr, 0 },
	{ "hdr-debug-force-output", no_argument, nullptr, 0 },
	{ "hdr-debug-heatmap", no_argument, nullptr, 0 },

	{ "reshade-effect", required_argument, nullptr, 0 },
	{ "reshade-technique-idx", required_argument, nullptr, 0 },

	// Steam Deck options
	{ "mura-map", required_argument, nullptr, 0 },

	{} // keep last
};

const char usage[] =
	"usage: gamescope [options...] -- [command...]\n"
	"\n"
	"Options:\n"
	"  --help                         show help message\n"
	"  -W, --output-width             output width\n"
	"  -H, --output-height            output height\n"
	"  -w, --nested-width             game width\n"
	"  -h, --nested-height            game height\n"
	"  -r, --nested-refresh           game refresh rate (frames per second)\n"
	"  -m, --max-scale                maximum scale factor\n"
	"  -S, --scaler                   upscaler type (auto, integer, fit, fill, stretch)\n"
	"  -F, --filter                   upscaler filter (linear, nearest, fsr, nis, pixel)\n"
	"                                     fsr => AMD FidelityFX™ Super Resolution 1.0\n"
	"                                     nis => NVIDIA Image Scaling v1.0.3\n"
	"  --sharpness, --fsr-sharpness   upscaler sharpness from 0 (max) to 20 (min)\n"
	"  --expose-wayland               support wayland clients using xdg-shell\n"
	"  -s, --mouse-sensitivity        multiply mouse movement by given decimal number\n"
	"  --backend                      select rendering backend\n"
	"                                     auto => autodetect (default)\n"
#if HAVE_DRM
	"                                     drm => use DRM backend (standalone display session)\n"
#endif
#if HAVE_SDL2
	"                                     sdl => use SDL backend\n"
#endif
#if HAVE_OPENVR
	"                                     openvr => use OpenVR backend (outputs as a VR overlay)\n"
#endif
	"                                     headless => use headless backend (no window, no DRM output)\n"
	"                                     wayland => use Wayland backend\n"
	"  --cursor                       path to default cursor image\n"
	"  -R, --ready-fd                 notify FD when ready\n"
	"  --rt                           Use realtime scheduling\n"
	"  -T, --stats-path               write statistics to path\n"
	"  -C, --hide-cursor-delay        hide cursor image after delay\n"
	"  -e, --steam                    enable Steam integration\n"
	"  --xwayland-count               create N xwayland servers\n"
	"  --prefer-vk-device             prefer Vulkan device for compositing (ex: 1002:7300)\n"
	"  --force-orientation            rotate the internal display (left, right, normal, upsidedown)\n"
	"  --force-windows-fullscreen     force windows inside of gamescope to be the size of the nested display (fullscreen)\n"
	"  --cursor-scale-height          if specified, sets a base output height to linearly scale the cursor against.\n"
	"  --hdr-enabled                  enable HDR output (needs Gamescope WSI layer enabled for support from clients)\n"
	"                                 If this is not set, and there is a HDR client, it will be tonemapped SDR.\n"
	"  --sdr-gamut-wideness           Set the 'wideness' of the gamut for SDR comment. 0 - 1.\n"
	"  --hdr-sdr-content-nits         set the luminance of SDR content in nits. Default: 400 nits.\n"
	"  --hdr-itm-enabled              enable SDR->HDR inverse tone mapping. only works for SDR input.\n"
	"  --hdr-itm-sdr-nits             set the luminance of SDR content in nits used as the input for the inverse tone mapping process.\n"
	"                                 Default: 100 nits, Max: 1000 nits\n"
	"  --hdr-itm-target-nits          set the target luminace of the inverse tone mapping process.\n"
	"                                 Default: 1000 nits, Max: 10000 nits\n"
	"  --framerate-limit              Set a simple framerate limit. Used as a divisor of the refresh rate, rounds down eg 60 / 59 -> 60fps, 60 / 25 -> 30fps. Default: 0, disabled.\n"
	"  --mangoapp                     Launch with the mangoapp (mangohud) performance overlay enabled. You should use this instead of using mangohud on the game or gamescope.\n"
	"\n"
	"Nested mode options:\n"
	"  -o, --nested-unfocused-refresh game refresh rate when unfocused\n"
	"  -b, --borderless               make the window borderless\n"
	"  -f, --fullscreen               make the window fullscreen\n"
	"  -g, --grab                     grab the keyboard\n"
	"  --force-grab-cursor            always use relative mouse mode instead of flipping dependent on cursor visibility.\n"
	"  --display-index                forces gamescope to use a specific display in nested mode."
	"\n"
	"Embedded mode options:\n"
	"  -O, --prefer-output            list of connectors in order of preference\n"
	"  --default-touch-mode           0: hover, 1: left, 2: right, 3: middle, 4: passthrough\n"
	"  --generate-drm-mode            DRM mode generation algorithm (cvt, fixed)\n"
	"  --immediate-flips              Enable immediate flips, may result in tearing\n"
	"  --adaptive-sync                Enable adaptive sync if available (variable rate refresh)\n"
	"\n"
#if HAVE_OPENVR
	"VR mode options:\n"
	"  --vr-overlay-key                         Sets the SteamVR overlay key to this string\n"
	"  --vr-overlay-explicit-name               Force the SteamVR overlay name to always be this string\n"
	"  --vr-overlay-default-name                Sets the fallback SteamVR overlay name when there is no window title\n"
	"  --vr-overlay-icon                        Sets the SteamVR overlay icon to this file\n"
	"  --vr-overlay-show-immediately            Makes our VR overlay take focus immediately\n"
	"  --vr-overlay-enable-control-bar          Enables the SteamVR control bar\n"
	"  --vr-overlay-enable-control-bar-keyboard Enables the SteamVR keyboard button on the control bar\n"
	"  --vr-overlay-enable-control-bar-close    Enables the SteamVR close button on the control bar\n"
	"  --vr-overlay-modal                       Makes our VR overlay appear as a modal\n"
	"  --vr-overlay-physical-width              Sets the physical width of our VR overlay in metres\n"
	"  --vr-overlay-physical-curvature          Sets the curvature of our VR overlay\n"
	"  --vr-overlay-physical-pre-curve-pitch    Sets the pre-curve pitch of our VR overlay\n"
	"  --vr-scrolls-speed                       Mouse scrolling speed of trackpad scroll in VR. Default: 8.0\n"
	"\n"
#endif
	"Debug options:\n"
	"  --disable-layers               disable libliftoff (hardware planes)\n"
	"  --debug-layers                 debug libliftoff\n"
	"  --debug-focus                  debug XWM focus\n"
	"  --synchronous-x11              force X11 connection synchronization\n"
	"  --debug-hud                    paint HUD with debug info\n"
	"  --debug-events                 debug X11 events\n"
	"  --force-composition            disable direct scan-out\n"
	"  --composite-debug              draw frame markers on alternating corners of the screen when compositing\n"
	"  --disable-color-management     disable color management\n"
	"  --disable-xres                 disable XRes for PID lookup\n"
	"  --hdr-debug-force-support      forces support for HDR, etc even if the display doesn't support it. HDR clients will be outputted as SDR still in that case.\n"
	"  --hdr-debug-force-output       forces support and output to HDR10 PQ even if the output does not support it (will look very wrong if it doesn't)\n"
	"  --hdr-debug-heatmap            displays a heatmap-style debug view of HDR luminence across the scene in nits."
	"\n"
	"Reshade shader options:\n"
	"  --reshade-effect               sets the name of a reshade shader to use in either /usr/share/gamescope/reshade/Shaders or ~/.local/share/gamescope/reshade/Shaders\n"
	"  --reshade-technique-idx        sets technique idx to use from the reshade effect\n"
	"\n"
	"Steam Deck options:\n"
	"  --mura-map                     Set the mura compensation map to use for the display. Takes in a path to the mura map.\n"
	"\n"
	"Keyboard shortcuts:\n"
	"  Super + F                      toggle fullscreen\n"
	"  Super + N                      toggle nearest neighbour filtering\n"
	"  Super + U                      toggle FSR upscaling\n"
	"  Super + Y                      toggle NIS upscaling\n"
	"  Super + I                      increase FSR sharpness by 1\n"
	"  Super + O                      decrease FSR sharpness by 1\n"
	"  Super + S                      take a screenshot\n"
	"  Super + G                      toggle keyboard grab\n"
	"";

std::atomic< bool > g_bRun{true};

int g_nNestedWidth = 0;
int g_nNestedHeight = 0;
int g_nNestedRefresh = 0;
int g_nNestedUnfocusedRefresh = 0;
int g_nNestedDisplayIndex = 0;

uint32_t g_nOutputWidth = 0;
uint32_t g_nOutputHeight = 0;
int g_nOutputRefresh = 0;
bool g_bOutputHDREnabled = false;

bool g_bFullscreen = false;
bool g_bForceRelativeMouse = false;

bool g_bGrabbed = false;

float g_mouseSensitivity = 1.0;

GamescopeUpscaleFilter g_upscaleFilter = GamescopeUpscaleFilter::LINEAR;
GamescopeUpscaleScaler g_upscaleScaler = GamescopeUpscaleScaler::AUTO;

GamescopeUpscaleFilter g_wantedUpscaleFilter = GamescopeUpscaleFilter::LINEAR;
GamescopeUpscaleScaler g_wantedUpscaleScaler = GamescopeUpscaleScaler::AUTO;
int g_upscaleFilterSharpness = 2;

gamescope::GamescopeModeGeneration g_eGamescopeModeGeneration = gamescope::GAMESCOPE_MODE_GENERATE_CVT;

bool g_bBorderlessOutputWindow = false;

int g_nXWaylandCount = 1;

float g_flMaxWindowScale = FLT_MAX;

uint32_t g_preferVendorID = 0;
uint32_t g_preferDeviceID = 0;

pthread_t g_mainThread;

static void steamCompMgrThreadRun(int argc, char **argv);

static std::string build_optstring(const struct option *options)
{
	std::string optstring;
	for (size_t i = 0; options[i].name != nullptr; i++) {
		if (!options[i].name || !options[i].val)
			continue;

		assert(optstring.find((char) options[i].val) == std::string::npos);

		char str[] = { (char) options[i].val, '\0' };
		optstring.append(str);

		if (options[i].has_arg)
			optstring.append(":");
	}
	return optstring;
}

static gamescope::GamescopeModeGeneration parse_gamescope_mode_generation( const char *str )
{
	if ( str == "cvt"sv )
	{
		return gamescope::GAMESCOPE_MODE_GENERATE_CVT;
	}
	else if ( str == "fixed"sv )
	{
		return gamescope::GAMESCOPE_MODE_GENERATE_FIXED;
	}
	else
	{
		fprintf( stderr, "gamescope: invalid value for --generate-drm-mode\n" );
		exit(1);
	}
}

GamescopePanelOrientation g_DesiredInternalOrientation = GAMESCOPE_PANEL_ORIENTATION_AUTO;
static GamescopePanelOrientation force_orientation(const char *str)
{
	if (strcmp(str, "normal") == 0) {
		return GAMESCOPE_PANEL_ORIENTATION_0;
	} else if (strcmp(str, "right") == 0) {
		return GAMESCOPE_PANEL_ORIENTATION_270;
	} else if (strcmp(str, "left") == 0) {
		return GAMESCOPE_PANEL_ORIENTATION_90;
	} else if (strcmp(str, "upsidedown") == 0) {
		return GAMESCOPE_PANEL_ORIENTATION_180;
	} else {
		fprintf( stderr, "gamescope: invalid value for --force-orientation\n" );
		exit(1);
	}
}

static enum GamescopeUpscaleScaler parse_upscaler_scaler(const char *str)
{
	if (strcmp(str, "auto") == 0) {
		return GamescopeUpscaleScaler::AUTO;
	} else if (strcmp(str, "integer") == 0) {
		return GamescopeUpscaleScaler::INTEGER;
	} else if (strcmp(str, "fit") == 0) {
		return GamescopeUpscaleScaler::FIT;
	} else if (strcmp(str, "fill") == 0) {
		return GamescopeUpscaleScaler::FILL;
	} else if (strcmp(str, "stretch") == 0) {
		return GamescopeUpscaleScaler::STRETCH;
	} else {
		fprintf( stderr, "gamescope: invalid value for --scaler\n" );
		exit(1);
	}
}

static enum GamescopeUpscaleFilter parse_upscaler_filter(const char *str)
{
	if (strcmp(str, "linear") == 0) {
		return GamescopeUpscaleFilter::LINEAR;
	} else if (strcmp(str, "nearest") == 0) {
		return GamescopeUpscaleFilter::NEAREST;
	} else if (strcmp(str, "fsr") == 0) {
		return GamescopeUpscaleFilter::FSR;
	} else if (strcmp(str, "nis") == 0) {
		return GamescopeUpscaleFilter::NIS;
	} else if (strcmp(str, "pixel") == 0) {
		return GamescopeUpscaleFilter::PIXEL;
	} else {
		fprintf( stderr, "gamescope: invalid value for --filter\n" );
		exit(1);
	}
}

static enum gamescope::GamescopeBackend parse_backend_name(const char *str)
{
	if (strcmp(str, "auto") == 0) {
		return gamescope::GamescopeBackend::Auto;
#if HAVE_DRM
	} else if (strcmp(str, "drm") == 0) {
		return gamescope::GamescopeBackend::DRM;
#endif
#if HAVE_SDL2
	} else if (strcmp(str, "sdl") == 0) {
		return gamescope::GamescopeBackend::SDL;
#endif
#if HAVE_OPENVR
	} else if (strcmp(str, "openvr") == 0) {
		return gamescope::GamescopeBackend::OpenVR;
#endif
	} else if (strcmp(str, "headless") == 0) {
		return gamescope::GamescopeBackend::Headless;
	} else if (strcmp(str, "wayland") == 0) {
		return gamescope::GamescopeBackend::Wayland;
	} else {
		fprintf( stderr, "gamescope: invalid value for --backend\n" );
		exit(1);
	}
}

struct sigaction handle_signal_action = {};

void ShutdownGamescope()
{
	g_bRun = false;

	nudge_steamcompmgr();
}

static gamescope::ConCommand cc_shutdown( "shutdown", "Cleanly shutdown gamescope",
[]( std::span<std::string_view> svArgs )
{
	console_log.infof( "Shutting down..." );
	ShutdownGamescope();
});

static void handle_signal( int sig )
{
	switch ( sig ) {
	case SIGUSR2:
		gamescope::CScreenshotManager::Get().TakeScreenshot( true );
		break;
	case SIGHUP:
	case SIGQUIT:
	case SIGTERM:
	case SIGINT:
		ShutdownGamescope();
		break;
	case SIGUSR1:
		fprintf( stderr, "gamescope: hi :3\n" );
		break;
	default:
		assert( false ); // unreachable
	}
}

static EStreamColorspace parse_colorspace_string( const char *pszStr )
{
	if ( !pszStr || !*pszStr )
		return k_EStreamColorspace_Unknown;

	if ( !strcmp( pszStr, "k_EStreamColorspace_BT601" ) )
		return k_EStreamColorspace_BT601;
	else if ( !strcmp( pszStr, "k_EStreamColorspace_BT601_Full" ) )
		return k_EStreamColorspace_BT601_Full;
	else if ( !strcmp( pszStr, "k_EStreamColorspace_BT709" ) )
		return k_EStreamColorspace_BT709;
	else if ( !strcmp( pszStr, "k_EStreamColorspace_BT709_Full" ) )
		return k_EStreamColorspace_BT709_Full;
	else
	 	return k_EStreamColorspace_Unknown;
}




static bool g_bSupportsWaylandPresentationTime = false;
static constexpr wl_registry_listener s_registryListener = {
    .global = [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
        if (interface == "wp_presentation"sv)
            g_bSupportsWaylandPresentationTime = true;
    },

    .global_remove = [](void* data, wl_registry* registry, uint32_t name) {
    },
};

static bool CheckWaylandPresentationTime()
{
	wl_display *display = wl_display_connect(g_pOriginalWaylandDisplay);
	if (!display) {
		fprintf(stderr, "Failed to connect to wayland socket: %s.\n", g_pOriginalWaylandDisplay);
        exit(1);
        return false;
	}
	wl_registry *registry = wl_display_get_registry(display);

    wl_registry_add_listener(registry, &s_registryListener, nullptr);

	wl_display_dispatch(display);
	wl_display_roundtrip(display);

	wl_registry_destroy(registry);
	wl_display_disconnect(display);

    return g_bSupportsWaylandPresentationTime;
}

#if 0
static bool IsInDebugSession()
{
	static FILE *fp;
	if ( !fp )
	{
		fp = fopen( "/proc/self/status", "r" );
	}

	char rgchLine[256]; rgchLine[0] = '\0';
	int nTracePid = 0;
	if ( fp )
	{
		const char *pszSearchString = "TracerPid:";
		const uint cchSearchString = strlen( pszSearchString );
		rewind( fp );
		fflush( fp );
		while ( fgets( rgchLine, sizeof(rgchLine), fp ) )
		{
			if ( !strncasecmp( pszSearchString, rgchLine, cchSearchString ) )
			{
				char *pszVal = rgchLine+cchSearchString+1;
				nTracePid = atoi( pszVal );
				break;
			}
		}
	}
	return nTracePid != 0;
}
#endif

bool steamMode = false;
bool g_bLaunchMangoapp = false;

static void UpdateCompatEnvVars()
{
	// Legacy env vars for compat.
	if ( steamMode )
	{
		// We have NIS support.
		setenv( "STEAM_GAMESCOPE_NIS_SUPPORTED", "1", 0 );
		// Have SteamRT's xdg-open send http:// and https:// URLs to Steam
		setenv( "SRT_URLOPEN_PREFER_STEAM", "1", 0 );
		if ( g_nXWaylandCount > 1 )
		{
			setenv( "STEAM_MULTIPLE_XWAYLANDS", "1", 0 );
		}
		// If the backend exposes tearing, expose that to Steam.
		if ( GetBackend()->SupportsTearing() )
		{
			setenv( "STEAM_GAMESCOPE_TEARING_SUPPORTED", "1", 0 );
			setenv( "STEAM_GAMESCOPE_HAS_TEARING_SUPPORT", "1", 0 );
		}
		// We always support VRR (but not necessarily on every connector, etc.)
		setenv( "STEAM_GAMESCOPE_VRR_SUPPORTED", "1", 0 );
		// We no longer need to set GAMESCOPE_EXTERNAL_OVERLAY from steam, mangoapp now does it itself
		setenv( "STEAM_DISABLE_MANGOAPP_ATOM_WORKAROUND", "1", 0 );
		// Enable horizontal mangoapp bar
		setenv( "STEAM_MANGOAPP_HORIZONTAL_SUPPORTED", "1", 0 );
		// Scaling support
		setenv( "STEAM_GAMESCOPE_FANCY_SCALING_SUPPORT", "1", 0 );
		// We support HDR.
		setenv( "STEAM_GAMESCOPE_HDR_SUPPORTED", "1", 0 );
		// Gamescope WSI layer implements this.
		setenv( "STEAM_GAMESCOPE_DYNAMIC_FPSLIMITER", "1", 0 );

		// Set input method modules for Qt/GTK that will show the Steam keyboard
		// These are mostly SteamOS specific, and are set by our Gamescope session,
		// but might be useful for you.
		//setenv( "QT_IM_MODULE", "steam", 1 );
		//setenv( "GTK_IM_MODULE", "Steam", 1 );
		//setenv( "QT_QPA_PLATFORM_THEME", "kde", 1 );

		// Maybe we should expose a backend check for this...
		// STEAM_GAMESCOPE_COLOR_MANAGED
		// STEAM_GAMESCOPE_VIRTUAL_WHITE

		// STEAM_USE_DYNAMIC_VRS is RADV specific, so don't expose this right now.

		setenv( "STEAM_MANGOAPP_PRESETS_SUPPORTED", "1", 0 );
		setenv( "STEAM_USE_MANGOAPP", "1", 0 );
	}

	// Always set this to false, we never want buffers to be waited on by Mesa.
	// That is our job!
	setenv( "vk_xwayland_wait_ready", "false", 1 );
	if ( g_nCursorScaleHeight > 0 )
	{
		// We always want the biggest cursor size so we can scale it.
		setenv( "XCURSOR_SIZE", "256", 1 );
	}

	// Legacy support for SteamOS.
	setenv( "XWAYLAND_FORCE_ENABLE_EXTRA_MODES", "1", 1 );

	// Don't minimise stuff on focus loss with SDL.
	setenv( "SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS", "0", 1 );

	const char *pszMangoConfigPath = getenv( "MANGOHUD_CONFIGFILE" );
	if ( (g_bLaunchMangoapp && steamMode) && ( !pszMangoConfigPath || !*pszMangoConfigPath ) )
	{
		char szMangoConfigPath[ PATH_MAX ];
		FILE *pMangoConfigFile = gamescope::MakeTempFile( szMangoConfigPath, gamescope::k_szGamescopeTempMangoappTemplate, "w", true );
		if ( pMangoConfigFile )
		{
			setenv( "MANGOHUD_CONFIGFILE", szMangoConfigPath, 1 );

			if ( steamMode )
			{
				const char szDefaultConfig[] = "no_display";
				fwrite( szDefaultConfig, 1, sizeof( szDefaultConfig ), pMangoConfigFile );
			}
			fclose( pMangoConfigFile );
		}
	}

	const char *pszLimiterFile = getenv( "GAMESCOPE_LIMITER_FILE" );
	if ( !pszLimiterFile || !*pszLimiterFile )
	{
		char szLimiterPath[ PATH_MAX ];
		int nLimiterFd = gamescope::MakeTempFile( szLimiterPath, gamescope::k_szGamescopeTempLimiterTemplate, true );
		if ( nLimiterFd >= 0 )
		{
			setenv( "GAMESCOPE_LIMITER_FILE", szLimiterPath, 1 );
			gamescope::Process::CloseFd( nLimiterFd );
		}
	}
}

int g_nPreferredOutputWidth = 0;
int g_nPreferredOutputHeight = 0;
bool g_bExposeWayland = false;
const char *g_sOutputName = nullptr;
bool g_bDebugLayers = false;
bool g_bForceDisableColorMgmt = false;
bool g_bRt = false;

// This will go away when we remove the getopt stuff from vr session.
// For now...
int g_argc;
char **g_argv;

int main(int argc, char **argv)
{
	g_argc = argc;
	g_argv = argv;

	// Force disable this horrible broken layer.
	setenv("DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1", "1", 1);

	static std::string optstring = build_optstring(gamescope_options);
	gamescope_optstring = optstring.c_str();

	gamescope::GamescopeBackend eCurrentBackend = gamescope::GamescopeBackend::Auto;

	gamescope::PrintVersion();

	int o;
	int opt_index = -1;
	while ((o = getopt_long(argc, argv, gamescope_optstring, gamescope_options, &opt_index)) != -1)
	{
		const char *opt_name;
		switch (o) {
			case 'w':
				g_nNestedWidth = atoi( optarg );
				break;
			case 'h':
				g_nNestedHeight = atoi( optarg );
				break;
			case 'r':
				g_nNestedRefresh = gamescope::ConvertHztomHz( atoi( optarg ) );
				break;
			case 'W':
				g_nPreferredOutputWidth = atoi( optarg );
				break;
			case 'H':
				g_nPreferredOutputHeight = atoi( optarg );
				break;
			case 'o':
				g_nNestedUnfocusedRefresh = gamescope::ConvertHztomHz( atoi( optarg ) );
				break;
			case 'm':
				g_flMaxWindowScale = atof( optarg );
				break;
			case 'S':
				g_wantedUpscaleScaler = parse_upscaler_scaler(optarg);
				break;
			case 'F':
				g_wantedUpscaleFilter = parse_upscaler_filter(optarg);
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
			case 'g':
				g_bGrabbed = true;
				break;
			case 's':
				g_mouseSensitivity = atof( optarg );
				break;
			case 'e':
				steamMode = true;
				break;
			case 0: // long options without a short option
				opt_name = gamescope_options[opt_index].name;
				if (strcmp(opt_name, "help") == 0) {
					fprintf(stderr, "%s", usage);
					return 0;
				} else if (strcmp(opt_name, "version") == 0) {
					// We always print the version to stderr anyway.
					return 0;
				} else if (strcmp(opt_name, "debug-layers") == 0) {
					g_bDebugLayers = true;
				} else if (strcmp(opt_name, "disable-color-management") == 0) {
					g_bForceDisableColorMgmt = true;
				} else if (strcmp(opt_name, "xwayland-count") == 0) {
					g_nXWaylandCount = atoi( optarg );
				} else if (strcmp(opt_name, "composite-debug") == 0) {
					cv_composite_debug |= CompositeDebugFlag::Markers;
					cv_composite_debug |= CompositeDebugFlag::PlaneBorders;
				} else if (strcmp(opt_name, "hdr-debug-heatmap") == 0) {
					cv_composite_debug |= CompositeDebugFlag::Heatmap;
				} else if (strcmp(opt_name, "default-touch-mode") == 0) {
					gamescope::cv_touch_click_mode = (gamescope::TouchClickMode) atoi( optarg );
				} else if (strcmp(opt_name, "generate-drm-mode") == 0) {
					g_eGamescopeModeGeneration = parse_gamescope_mode_generation( optarg );
				} else if (strcmp(opt_name, "force-orientation") == 0) {
					g_DesiredInternalOrientation = force_orientation( optarg );
				} else if (strcmp(opt_name, "sharpness") == 0 ||
						   strcmp(opt_name, "fsr-sharpness") == 0) {
					g_upscaleFilterSharpness = atoi( optarg );
				} else if (strcmp(opt_name, "rt") == 0) {
					g_bRt = true;
				} else if (strcmp(opt_name, "prefer-vk-device") == 0) {
					unsigned vendorID;
					unsigned deviceID;
					sscanf( optarg, "%X:%X", &vendorID, &deviceID );
					g_preferVendorID = vendorID;
					g_preferDeviceID = deviceID;
				} else if (strcmp(opt_name, "immediate-flips") == 0) {
					cv_tearing_enabled = true;
				} else if (strcmp(opt_name, "force-grab-cursor") == 0) {
					g_bForceRelativeMouse = true;
				} else if (strcmp(opt_name, "display-index") == 0) {
					g_nNestedDisplayIndex = atoi( optarg );
				} else if (strcmp(opt_name, "adaptive-sync") == 0) {
					cv_adaptive_sync = true;
				} else if (strcmp(opt_name, "expose-wayland") == 0) {
					g_bExposeWayland = true;
				} else if (strcmp(opt_name, "backend") == 0) {
					eCurrentBackend = parse_backend_name( optarg );
				} else if (strcmp(opt_name, "cursor-scale-height") == 0) {
					g_nCursorScaleHeight = atoi(optarg);
				} else if (strcmp(opt_name, "mangoapp") == 0) {
					g_bLaunchMangoapp = true;
				}
				break;
			case '?':
				fprintf( stderr, "See --help for a list of options.\n" );
				return 1;
		}
	}

	if ( gamescope::Process::HasCapSysNice() )
	{
		gamescope::Process::SetNice( -20 );

		if ( g_bRt )
			gamescope::Process::SetRealtime();
	}
	else
	{
		fprintf( stderr, "No CAP_SYS_NICE, falling back to regular-priority compute and threads.\nPerformance will be affected.\n" );
	}

#if 0
	while( !IsInDebugSession() )
	{
		usleep( 100 );
	}
#endif

	gamescope::Process::RaiseFdLimit();

	if ( gpuvis_trace_init() != -1 )
	{
		fprintf( stderr, "Tracing is enabled\n");
	}

	{
		gamescope::CScriptScopedLock script;
		script.Manager().RunDefaultScripts();
	}

	XInitThreads();
	g_mainThread = pthread_self();

	g_pOriginalDisplay = getenv("DISPLAY");
	g_pOriginalWaylandDisplay = getenv("WAYLAND_DISPLAY");

	if ( eCurrentBackend == gamescope::GamescopeBackend::Auto )
	{
		if ( g_pOriginalWaylandDisplay != NULL )
			eCurrentBackend = gamescope::GamescopeBackend::Wayland;
		else if ( g_pOriginalDisplay != NULL )
			eCurrentBackend = gamescope::GamescopeBackend::SDL;
		else
			eCurrentBackend = gamescope::GamescopeBackend::DRM;
	}

	if ( g_pOriginalWaylandDisplay != NULL )
	{
        if (CheckWaylandPresentationTime())
        {
            // Default to SDL_VIDEODRIVER wayland under Wayland and force enable vk_khr_present_wait
            // (not enabled by default in Mesa because instance does not know if Wayland
            //  compositor supports wp_presentation, but we can check that ourselves.)
            setenv("vk_khr_present_wait", "true", 0);
            setenv("SDL_VIDEODRIVER", "wayland", 0);
        }
        else
        {
            fprintf(stderr,
                "Your Wayland compositor does NOT support wp_presentation/presentation-time which is required for VK_KHR_present_wait and VK_KHR_present_id.\n"
                "Please complain to your compositor vendor for support. Falling back to X11 window with less accurate present wait.\n");
            setenv("SDL_VIDEODRIVER", "x11", 1);
        }
	}

	g_ForcedNV12ColorSpace = parse_colorspace_string( getenv( "GAMESCOPE_NV12_COLORSPACE" ) );

	switch ( eCurrentBackend )
	{
#if HAVE_DRM
		case gamescope::GamescopeBackend::DRM:
			gamescope::IBackend::Set<gamescope::CDRMBackend>();
			break;
#endif
#if HAVE_SDL2
		case gamescope::GamescopeBackend::SDL:
			gamescope::IBackend::Set<gamescope::CSDLBackend>();
			break;
#endif
#if HAVE_OPENVR
		case gamescope::GamescopeBackend::OpenVR:
			gamescope::IBackend::Set<gamescope::COpenVRBackend>();
			break;
#endif
		case gamescope::GamescopeBackend::Headless:
			gamescope::IBackend::Set<gamescope::CHeadlessBackend>();
			break;

		case gamescope::GamescopeBackend::Wayland:
			gamescope::IBackend::Set<gamescope::CWaylandBackend>();
#if HAVE_SDL2
			if ( !GetBackend() )
				gamescope::IBackend::Set<gamescope::CSDLBackend>();
#endif
			break;
		default:
			abort();
	}

	if ( !GetBackend() )
	{
		fprintf( stderr, "Failed to create backend.\n" );
		return 1;
	}

	UpdateCompatEnvVars();

	if ( !vulkan_init_formats() )
	{
		fprintf( stderr, "vulkan_init_formats failed\n" );
		return 1;
	}

	if ( !vulkan_make_output() )
	{
		fprintf( stderr, "vulkan_make_output failed\n" );
		return 1;
	}

	// Prevent our clients from connecting to the parent compositor
	unsetenv("WAYLAND_DISPLAY");

	// If DRM format modifiers aren't supported, prevent our clients from using
	// DCC, as this can cause tiling artifacts.
	if ( !vulkan_supports_modifiers() )
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

	if ( !wlserver_init() )
	{
		fprintf( stderr, "Failed to initialize wlserver\n" );
		return 1;
	}

	gamescope_xwayland_server_t *base_server = wlserver_get_xwayland_server(0);

	setenv("DISPLAY", base_server->get_nested_display_name(), 1);
	if ( g_bExposeWayland )
		setenv("XDG_SESSION_TYPE", "wayland", 1);
	else
		setenv("XDG_SESSION_TYPE", "x11", 1);
	setenv("XDG_CURRENT_DESKTOP", "gamescope", 1);
	if (g_nXWaylandCount > 1)
	{
		for (int i = 1; i < g_nXWaylandCount; i++)
		{
			char env_name[64];
			snprintf(env_name, sizeof(env_name), "STEAM_GAME_DISPLAY_%d", i - 1);
			gamescope_xwayland_server_t *server = wlserver_get_xwayland_server(i);
			setenv(env_name, server->get_nested_display_name(), 1);
		}
	}
	else
	{
		setenv("STEAM_GAME_DISPLAY_0", base_server->get_nested_display_name(), 1);
	}
	setenv("GAMESCOPE_WAYLAND_DISPLAY", wlserver_get_wl_display_name(), 1);
	if ( g_bExposeWayland )
		setenv("WAYLAND_DISPLAY", wlserver_get_wl_display_name(), 1);

#if HAVE_PIPEWIRE
	if ( !init_pipewire() )
	{
		fprintf( stderr, "Warning: failed to setup PipeWire, screen capture won't be available\n" );
	}
#endif

	std::thread steamCompMgrThread( steamCompMgrThreadRun, argc, argv );

	handle_signal_action.sa_handler = handle_signal;
	sigaction(SIGHUP, &handle_signal_action, nullptr);
	sigaction(SIGINT, &handle_signal_action, nullptr);
	sigaction(SIGQUIT, &handle_signal_action, nullptr);
	sigaction(SIGTERM, &handle_signal_action, nullptr);
	sigaction(SIGUSR1, &handle_signal_action, nullptr);
	sigaction(SIGUSR2, &handle_signal_action, nullptr);

	wlserver_run();

	steamCompMgrThread.join();

	gamescope::Process::KillAllChildren( getpid(), SIGTERM );
	gamescope::Process::WaitForAllChildren();
}

static void steamCompMgrThreadRun(int argc, char **argv)
{
	pthread_setname_np( pthread_self(), "gamescope-xwm" );

	steamcompmgr_main( argc, argv );

	pthread_kill( g_mainThread, SIGINT );
}
