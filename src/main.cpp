#include <X11/Xlib.h>

#include <cstdio>
#include <thread>
#include <mutex>
#include <vector>
#include <cstring>
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

#include "main.hpp"
#include "steamcompmgr.hpp"
#include "drm.hpp"
#include "rendervulkan.hpp"
#include "sdlwindow.hpp"
#include "wlserver.hpp"
#include "gpuvis_trace_utils.h"

#if HAVE_OPENVR
#include "vr_session.hpp"
#endif

#if HAVE_PIPEWIRE
#include "pipewire.hpp"
#endif

EStreamColorspace g_ForcedNV12ColorSpace = k_EStreamColorspace_Unknown;
static bool s_bInitialWantsVRREnabled = false;

const char *gamescope_optstring = nullptr;
const char *g_pOriginalDisplay = nullptr;

const struct option *gamescope_options = (struct option[]){
	{ "help", no_argument, nullptr, 0 },
	{ "nested-width", required_argument, nullptr, 'w' },
	{ "nested-height", required_argument, nullptr, 'h' },
	{ "nested-refresh", required_argument, nullptr, 'r' },
	{ "max-scale", required_argument, nullptr, 'm' },
	{ "integer-scale", no_argument, nullptr, 'i' },
	{ "output-width", required_argument, nullptr, 'W' },
	{ "output-height", required_argument, nullptr, 'H' },
	{ "nearest-neighbor-filter", no_argument, nullptr, 'n' },
	{ "fsr-upscaling", no_argument, nullptr, 'U' },
	{ "nis-upscaling", no_argument, nullptr, 'Y' },
	{ "sharpness", required_argument, nullptr, 0 },
	{ "fsr-sharpness", required_argument, nullptr, 0 },
	{ "rt", no_argument, nullptr, 0 },
	{ "prefer-vk-device", required_argument, 0 },
	{ "expose-wayland", no_argument, 0 },

	// nested mode options
	{ "nested-unfocused-refresh", required_argument, nullptr, 'o' },
	{ "borderless", no_argument, nullptr, 'b' },
	{ "fullscreen", no_argument, nullptr, 'f' },
	{ "grab", no_argument, nullptr, 'g' },
	{ "force-grab-cursor", no_argument, nullptr, 0 },

	// embedded mode options
	{ "disable-layers", no_argument, nullptr, 0 },
	{ "debug-layers", no_argument, nullptr, 0 },
	{ "prefer-output", required_argument, nullptr, 'O' },
	{ "default-touch-mode", required_argument, nullptr, 0 },
	{ "generate-drm-mode", required_argument, nullptr, 0 },
	{ "immediate-flips", no_argument, nullptr, 0 },
	{ "adaptive-sync", no_argument, nullptr, 0 },

	// openvr options
#if HAVE_OPENVR
	{ "openvr", no_argument, nullptr, 0 },
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
	{ "hdr-enabled", no_argument, nullptr, 0 },
	{ "hdr-sdr-content-nits", required_argument, nullptr, 0 },
	{ "hdr-wide-gammut-for-sdr", no_argument, nullptr, 0 },
	{ "hdr-itm-enable", no_argument, nullptr, 0 },
	{ "hdr-itm-sdr-nits", required_argument, nullptr, 0 },
	{ "hdr-itm-target-nits", required_argument, nullptr, 0 },
	{ "hdr-debug-force-support", no_argument, nullptr, 0 },
	{ "hdr-debug-force-output", no_argument, nullptr, 0 },
	{ "hdr-debug-heatmap", no_argument, nullptr, 0 },

	{} // keep last
};

const char usage[] =
	"usage: gamescope [options...] -- [command...]\n"
	"\n"
	"Options:\n"
	"  --help                         show help message\n"
	"  -w, --nested-width             game width\n"
	"  -h, --nested-height            game height\n"
	"  -r, --nested-refresh           game refresh rate (frames per second)\n"
	"  -m, --max-scale                maximum scale factor\n"
	"  -i, --integer-scale            force scale factor to integer\n"
	"  -W, --output-width             output width\n"
	"  -H, --output-height            output height\n"
	"  -n, --nearest-neighbor-filter  use nearest neighbor filtering\n"
	"  -U, --fsr-upscaling            use AMD FidelityFX™ Super Resolution 1.0 for upscaling\n"
	"  -Y, --nis-upscaling            use NVIDIA Image Scaling v1.0.3 for upscaling\n"
	"  --sharpness, --fsr-sharpness   upscaler sharpness from 0 (max) to 20 (min)\n"
	"  --expose-wayland               support wayland clients using xdg-shell\n"
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
	"  --hdr-wide-gammut-for-sdr      treat SDR sRGB content as having Rec.2020 primaries. Makes colors more vivid at cost of 'correctness'.\n"
	"  --hdr-sdr-content-nits         set the luminance of SDR content in nits. Default: 400 nits.\n"
	"  --hdr-itm-enable               enable SDR->HDR inverse tone mapping. only works for SDR input.\n"
	"  --hdr-itm-sdr-nits             set the luminance of SDR content in nits used as the input for the inverse tone mapping process.\n"
	"                                 Default: 100 nits, Max: 1000 nits\n"
	"  --hdr-itm-target-nits          set the target luminace of the inverse tone mapping process.\n"
	"                                 Default: 1000 nits, Max: 10000 nits\n"
	"\n"
	"Nested mode options:\n"
	"  -o, --nested-unfocused-refresh game refresh rate when unfocused\n"
	"  -b, --borderless               make the window borderless\n"
	"  -f, --fullscreen               make the window fullscreen\n"
	"  -g, --grab                     grab the keyboard\n"
	"  --force-grab-cursor            always use relative mouse mode instead of flipping dependent on cursor visibility.\n"
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
	"  --openvr                                 Uses the openvr backend and outputs as a VR overlay\n"
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

uint32_t g_nOutputWidth = 0;
uint32_t g_nOutputHeight = 0;
int g_nOutputRefresh = 0;
bool g_bOutputHDREnabled = false;

bool g_bFullscreen = false;
bool g_bForceRelativeMouse = false;

bool g_bIsNested = false;

bool g_bGrabbed = false;

GamescopeUpscaleFilter g_upscaleFilter = GamescopeUpscaleFilter::LINEAR;
GamescopeUpscaleScaler g_upscaleScaler = GamescopeUpscaleScaler::AUTO;

GamescopeUpscaleFilter g_wantedUpscaleFilter = GamescopeUpscaleFilter::LINEAR;
GamescopeUpscaleScaler g_wantedUpscaleScaler = GamescopeUpscaleScaler::AUTO;
int g_upscaleFilterSharpness = 2;

bool g_bBorderlessOutputWindow = false;

int g_nXWaylandCount = 1;

bool g_bNiceCap = false;
int g_nOldNice = 0;
int g_nNewNice = 0;

bool g_bRt = false;
int g_nOldPolicy;
struct sched_param g_schedOldParam;

float g_flMaxWindowScale = FLT_MAX;

uint32_t g_preferVendorID = 0;
uint32_t g_preferDeviceID = 0;

pthread_t g_mainThread;

bool BIsNested()
{
	return g_bIsNested;
}

#if HAVE_OPENVR
bool g_bUseOpenVR = false;
bool BIsVRSession( void )
{
	return g_bUseOpenVR;
}
#else
bool BIsVRSession( void )
{
	return false;
}
#endif

static bool initOutput(int preferredWidth, int preferredHeight, int preferredRefresh);
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

static enum drm_mode_generation parse_drm_mode_generation(const char *str)
{
	if (strcmp(str, "cvt") == 0) {
		return DRM_MODE_GENERATE_CVT;
	} else if (strcmp(str, "fixed") == 0) {
		return DRM_MODE_GENERATE_FIXED;
	} else {
		fprintf( stderr, "gamescope: invalid value for --generate-drm-mode\n" );
		exit(1);
	}
}

static enum g_panel_orientation force_orientation(const char *str)
{
	if (strcmp(str, "normal") == 0) {
		return PANEL_ORIENTATION_0;
	} else if (strcmp(str, "right") == 0) {
		return PANEL_ORIENTATION_270;
	} else if (strcmp(str, "left") == 0) {
		return PANEL_ORIENTATION_90;
	} else if (strcmp(str, "upsidedown") == 0) {
		return PANEL_ORIENTATION_180;
	} else {
		fprintf( stderr, "gamescope: invalid value for --force-orientation\n" );
		exit(1);
	}
}

static void handle_signal( int sig )
{
	switch ( sig ) {
	case SIGUSR2:
		take_screenshot();
		break;
	case SIGTERM:
	case SIGINT:
		fprintf( stderr, "gamescope: received kill signal, terminating!\n" );
		g_bRun = false;
		break;
	default:
		assert( false ); // unreachable
	}
}

static struct rlimit g_originalFdLimit;
static bool g_fdLimitRaised = false;

void restore_fd_limit( void )
{
	if (!g_fdLimitRaised) {
		return;
	}

	if ( setrlimit( RLIMIT_NOFILE, &g_originalFdLimit ) )
	{
		fprintf( stderr, "Failed to reset the maximum number of open files in child process\n" );
		fprintf( stderr, "Use of select() may fail.\n" );
	}

	g_fdLimitRaised = false;
}

static void raise_fd_limit( void )
{
	struct rlimit newFdLimit;

	memset(&g_originalFdLimit, 0, sizeof(g_originalFdLimit));
	if ( getrlimit( RLIMIT_NOFILE, &g_originalFdLimit ) != 0 )
	{
		fprintf( stderr, "Could not query maximum number of open files. Leaving at default value.\n" );
		return;
	}

	if ( g_originalFdLimit.rlim_cur >= g_originalFdLimit.rlim_max )
	{
		return;
	}

	memcpy(&newFdLimit, &g_originalFdLimit, sizeof(newFdLimit));
	newFdLimit.rlim_cur = newFdLimit.rlim_max;

	if ( setrlimit( RLIMIT_NOFILE, &newFdLimit ) )
	{
		fprintf( stderr, "Failed to raise the maximum number of open files. Leaving at default value.\n" );
	}

	g_fdLimitRaised = true;
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

int g_nPreferredOutputWidth = 0;
int g_nPreferredOutputHeight = 0;

int main(int argc, char **argv)
{
	// Force disable this horrible broken layer.
	setenv("DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1", "1", 1);

	bool bExposeWayland = false;

	static std::string optstring = build_optstring(gamescope_options);
	gamescope_optstring = optstring.c_str();

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
				g_nNestedRefresh = atoi( optarg );
				break;
			case 'W':
				g_nPreferredOutputWidth = atoi( optarg );
				break;
			case 'H':
				g_nPreferredOutputHeight = atoi( optarg );
				break;
			case 'o':
				g_nNestedUnfocusedRefresh = atoi( optarg );
				break;
			case 'm':
				g_flMaxWindowScale = atof( optarg );
				break;
			case 'i':
				g_wantedUpscaleScaler = GamescopeUpscaleScaler::INTEGER;
				break;
			case 'n':
				g_wantedUpscaleFilter = GamescopeUpscaleFilter::NEAREST;
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
			case 'U':
				g_wantedUpscaleFilter = GamescopeUpscaleFilter::FSR;
				break;
			case 'Y':
				g_wantedUpscaleFilter = GamescopeUpscaleFilter::NIS;
				break;
			case 'g':
				g_bGrabbed = true;
				break;
			case 0: // long options without a short option
				opt_name = gamescope_options[opt_index].name;
				if (strcmp(opt_name, "help") == 0) {
					fprintf(stderr, "%s", usage);
					return 0;
				} else if (strcmp(opt_name, "disable-layers") == 0) {
					g_bUseLayers = false;
				} else if (strcmp(opt_name, "debug-layers") == 0) {
					g_bDebugLayers = true;
				} else if (strcmp(opt_name, "disable-color-management") == 0) {
					g_bForceDisableColorMgmt = true;
				} else if (strcmp(opt_name, "xwayland-count") == 0) {
					g_nXWaylandCount = atoi( optarg );
				} else if (strcmp(opt_name, "composite-debug") == 0) {
					g_uCompositeDebug |= CompositeDebugFlag::Markers;
					g_uCompositeDebug |= CompositeDebugFlag::PlaneBorders;
				} else if (strcmp(opt_name, "hdr-debug-heatmap") == 0) {
					g_uCompositeDebug |= CompositeDebugFlag::Heatmap;
				} else if (strcmp(opt_name, "default-touch-mode") == 0) {
					g_nDefaultTouchClickMode = (enum wlserver_touch_click_mode) atoi( optarg );
					g_nTouchClickMode = g_nDefaultTouchClickMode;
				} else if (strcmp(opt_name, "generate-drm-mode") == 0) {
					g_drmModeGeneration = parse_drm_mode_generation( optarg );
				} else if (strcmp(opt_name, "force-orientation") == 0) {
					g_drmModeOrientation = force_orientation( optarg );
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
					g_nAsyncFlipsEnabled = 1;
				} else if (strcmp(opt_name, "force-grab-cursor") == 0) {
					g_bForceRelativeMouse = true;
				} else if (strcmp(opt_name, "adaptive-sync") == 0) {
					s_bInitialWantsVRREnabled = true;
				} else if (strcmp(opt_name, "expose-wayland") == 0) {
					bExposeWayland = true;
				}
#if HAVE_OPENVR
				else if (strcmp(opt_name, "openvr") == 0) {
					g_bUseOpenVR = true;
					g_bIsNested = true;
				}
#endif
				break;
			case '?':
				fprintf( stderr, "See --help for a list of options.\n" );
				return 1;
		}
	}

#if defined(__linux__)
	cap_t caps = cap_get_proc();
	if ( caps != nullptr )
	{
		cap_flag_value_t nicecapvalue = CAP_CLEAR;
		cap_get_flag( caps, CAP_SYS_NICE, CAP_EFFECTIVE, &nicecapvalue );

		if ( nicecapvalue == CAP_SET )
		{
			g_bNiceCap = true;

			errno = 0;
			int nOldNice = nice( 0 );
			if ( nOldNice != -1 && errno == 0 )
			{
				g_nOldNice = nOldNice;
			}

			errno = 0;
			int nNewNice = nice( -20 );
			if ( nNewNice != -1 && errno == 0 )
			{
				g_nNewNice = nNewNice;
			}
			if ( g_bRt )
			{
				struct sched_param sched;
				sched_getparam(0, &sched);
				sched.sched_priority = sched_get_priority_min(SCHED_RR);

				if (pthread_getschedparam(pthread_self(), &g_nOldPolicy, &g_schedOldParam)) {
					fprintf(stderr, "Failed to get old scheduling parameters: %s", strerror(errno));
					exit(1);
				}
				if (sched_setscheduler(0, SCHED_RR, &sched))
					fprintf(stderr, "Failed to set realtime: %s", strerror(errno));
			}
		}
	}

	if ( g_bNiceCap == false )
	{
		fprintf( stderr, "No CAP_SYS_NICE, falling back to regular-priority compute and threads.\nPerformance will be affected.\n" );
	}
#endif

	setenv( "XWAYLAND_FORCE_ENABLE_EXTRA_MODES", "1", 1 );

	raise_fd_limit();

	if ( gpuvis_trace_init() != -1 )
	{
		fprintf( stderr, "Tracing is enabled\n");
	}

	XInitThreads();
	g_mainThread = pthread_self();

	if ( getenv("DISPLAY") != NULL || getenv("WAYLAND_DISPLAY") != NULL )
	{
		g_bIsNested = true;
		g_pOriginalDisplay = getenv("DISPLAY");
	}

	if ( !wlsession_init() )
	{
		fprintf( stderr, "Failed to initialize Wayland session\n" );
		return 1;
	}

	if ( !BIsNested() )
	{
		g_bForceRelativeMouse = false;
	}

#if HAVE_OPENVR
	if ( BIsVRSession() )
	{
		if ( !vr_init( argc, argv ) )
		{
			fprintf( stderr, "Failed to initialize OpenVR runtime\n" );
			return 1;
		}
	}
	else
#endif
	if ( BIsNested() )
	{
		if ( SDL_Init( SDL_INIT_VIDEO | SDL_INIT_EVENTS ) != 0 )
		{
			fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
			return 1;
		}
	}

	g_ForcedNV12ColorSpace = parse_colorspace_string( getenv( "GAMESCOPE_NV12_COLORSPACE" ) );

	if ( !vulkan_init() )
	{
		fprintf( stderr, "Failed to initialize Vulkan\n" );
		return 1;
	}

	if ( !initOutput( g_nPreferredOutputWidth, g_nPreferredOutputHeight, g_nNestedRefresh ) )
	{
		fprintf( stderr, "Failed to initialize output\n" );
		return 1;
	}

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

#if HAVE_OPENVR
	if ( BIsVRSession() )
	{
		vrsession_ime_init();
	}
#endif

	gamescope_xwayland_server_t *base_server = wlserver_get_xwayland_server(0);

	setenv("DISPLAY", base_server->get_nested_display_name(), 1);
	if ( bExposeWayland )
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
	if ( bExposeWayland )
		setenv("WAYLAND_DISPLAY", wlserver_get_wl_display_name(), 1);

#if HAVE_PIPEWIRE
	if ( !init_pipewire() )
	{
		fprintf( stderr, "Warning: failed to setup PipeWire, screen capture won't be available\n" );
	}
#endif

	std::thread steamCompMgrThread( steamCompMgrThreadRun, argc, argv );

	signal( SIGTERM, handle_signal );
	signal( SIGINT, handle_signal );
	signal( SIGUSR2, handle_signal );

	wlserver_run();

	steamCompMgrThread.join();
}

static void steamCompMgrThreadRun(int argc, char **argv)
{
	pthread_setname_np( pthread_self(), "gamescope-xwm" );

	steamcompmgr_main( argc, argv );

	pthread_kill( g_mainThread, SIGINT );
}

static bool initOutput( int preferredWidth, int preferredHeight, int preferredRefresh )
{
	if ( g_bIsNested == true )
	{
		g_nOutputWidth = preferredWidth;
		g_nOutputHeight = preferredHeight;
		g_nOutputRefresh = preferredRefresh;

		if ( g_nOutputHeight == 0 )
		{
			if ( g_nOutputWidth != 0 )
			{
				fprintf( stderr, "Cannot specify -W without -H\n" );
				return 1;
			}
			g_nOutputHeight = 720;
		}
		if ( g_nOutputWidth == 0 )
			g_nOutputWidth = g_nOutputHeight * 16 / 9;
		if ( g_nOutputRefresh == 0 )
			g_nOutputRefresh = 60;

#if HAVE_OPENVR
		if ( BIsVRSession() )
		{
			return vrsession_init();
		}
		else
#endif
		{
			return sdlwindow_init();
		}
	}
	else
	{
		return init_drm( &g_DRM, preferredWidth, preferredHeight, preferredRefresh, s_bInitialWantsVRREnabled );
	}
}
