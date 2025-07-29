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

#include "gamescope_shared.h"
#include "xwayland_ctx.hpp"
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/extensions/xfixeswire.h>
#include <X11/extensions/XInput2.h>
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
#include <filesystem>
#include <variant>
#include <unordered_set>

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/types.h>
#if defined(__linux__)
#include <sys/prctl.h>
#elif defined(__DragonFly__) || defined(__FreeBSD__)
#include <sys/procctl.h>
#endif
#include <sys/socket.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <spawn.h>
#include <signal.h>
#include <linux/input-event-codes.h>
#include <X11/Xmu/CurUtil.h>
#include "waitable.h"

#include "main.hpp"
#include "wlserver.hpp"
#include "rendervulkan.hpp"
#include "steamcompmgr.hpp"
#include "vblankmanager.hpp"
#include "log.hpp"
#include "Utils/Defer.h"
#include "win32_styles.h"
#include "edid.h"
#include "hdmi.h"
#include "convar.h"
#include "refresh_rate.h"
#include "commit.h"
#include "reshade_effect_manager.hpp"
#include "BufferMemo.h"
#include "Utils/Process.h"
#include "Utils/Algorithm.h"

#include "wlr_begin.hpp"
#include "wlr/types/wlr_pointer_constraints_v1.h"
#include "wlr_end.hpp"

#if HAVE_AVIF
#include "avif/avif.h"
#endif

static const int g_nBaseCursorScale = 36;

#if HAVE_PIPEWIRE
#include "pipewire.hpp"
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>
#include <stb_image_resize.h>

#define GPUVIS_TRACE_IMPLEMENTATION
#include "gpuvis_trace_utils.h"

#if HAVE_LIBSYSTEMD

#include <systemd/sd-bus.h>

#endif

LogScope xwm_log("xwm");
LogScope g_WaitableLog("waitable");

gamescope::ConVar<bool> cv_overlay_unmultiplied_alpha{ "overlay_unmultiplied_alpha", false };

bool g_bWasPartialComposite = false;

bool ShouldDrawCursor();

///
// Color Mgmt
//

gamescope_color_mgmt_tracker_t g_ColorMgmt{};

static gamescope_color_mgmt_luts g_ColorMgmtLutsOverride[ EOTF_Count ];
std::atomic<std::shared_ptr<lut3d_t>> g_ColorMgmtLooks[EOTF_Count];


gamescope_color_mgmt_luts g_ColorMgmtLuts[ EOTF_Count ];

gamescope_color_mgmt_luts g_ScreenshotColorMgmtLuts[ EOTF_Count ];
gamescope_color_mgmt_luts g_ScreenshotColorMgmtLutsHDR[ EOTF_Count ];

static lut1d_t g_tmpLut1d;
static lut3d_t g_tmpLut3d;

extern int g_nDynamicRefreshHz;

bool g_bForceHDRSupportDebug = false;
extern float g_flInternalDisplayBrightnessNits;
extern float g_flHDRItmSdrNits;
extern float g_flHDRItmTargetNits;

uint64_t g_lastWinSeq = 0;

static std::shared_ptr<gamescope::BackendBlob> s_scRGB709To2020Matrix;

std::string clipboard;
std::string primarySelection;

std::string g_reshade_effect{};
extern ReshadeEffectPipeline *g_pLastReshadeEffect;
uint32_t g_reshade_technique_idx = 0;

bool g_bSteamIsActiveWindow = false;
bool g_bForceInternal = false;

namespace gamescope
{
	extern std::shared_ptr<INestedHints::CursorInfo> GetX11HostCursor();
}

#if HAVE_LIBSYSTEMD
static sd_bus *g_dbus;
static std::unordered_map<std::string, uint64_t> g_vramCapacities;

static const char *unit_from_pid(pid_t pid) {
	if (!pid)
		return NULL;

	sd_bus_message *reply = NULL;
	const char *path = NULL;

	if (sd_bus_call_method(g_dbus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
								  "org.freedesktop.systemd1.Manager", "GetUnitByPID", NULL, &reply, "u", pid) < 0) {
		xwm_log.warnf("D-Bus call to get unit corresponding to pid %u failed!\n", pid);
		goto fail;
	}

	if (sd_bus_message_read(reply, "o", &path) < 0)
		xwm_log.warnf("Failed to extract unit from D-Bus reply for PID %u!\n", pid);

	path = strdup(path);
	fail:
	sd_bus_message_unref(reply);
	return path;
}

static int set_memory_low(const char *unit_path, bool focused) {
	sd_bus_message *message;
	sd_bus_message *reply;

#define CHECK_MESSAGE(expr)   \
   ret = expr;                \
   if (ret < 0) {             \
      fprintf(stderr, #expr "failed with ret %d\n", ret);                        \
      goto fail_message;      \
}


	int ret = sd_bus_message_new_method_call(g_dbus, &message, "org.freedesktop.systemd1", unit_path,
														  "org.freedesktop.systemd1.Unit", "SetProperties");
	if (ret < 0)
		return ret;
	CHECK_MESSAGE(sd_bus_message_append(message, "b", false));
	CHECK_MESSAGE(sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "(sv)"));
	{
		CHECK_MESSAGE(sd_bus_message_open_container(message, SD_BUS_TYPE_STRUCT, "sv"));
		{
			CHECK_MESSAGE(sd_bus_message_append(message, "s", "DevMemoryLow"));
			CHECK_MESSAGE(sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "a(st)"));
			{
				CHECK_MESSAGE(sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "(st)"));
				for (auto &cap: g_vramCapacities) {
					CHECK_MESSAGE(
							  sd_bus_message_append(message, "(st)", cap.first.c_str(), focused ? cap.second : 0u));
				}
				CHECK_MESSAGE(sd_bus_message_close_container(message));
			}
			CHECK_MESSAGE(sd_bus_message_close_container(message));
		}
		CHECK_MESSAGE(sd_bus_message_close_container(message));
	}
	CHECK_MESSAGE(sd_bus_message_close_container(message));

	CHECK_MESSAGE(sd_bus_call(g_dbus, message, UINT64_MAX, NULL, &reply));


	sd_bus_message_unref(reply);

	fail_message:
	sd_bus_message_unref(message);

	return ret;
}

#endif

static std::vector< steamcompmgr_win_t* > GetGlobalPossibleFocusWindows();
static bool
pick_primary_focus_and_override(
	focus_t *out,
	Window focusControlWindow,
	const std::vector<steamcompmgr_win_t*>& vecPossibleFocusWindows,
	bool globalFocus,
	const std::vector<uint32_t>& ctxFocusControlAppIDs,
	uint64_t ulVirtualFocusKey = 0,
	gamescope::VirtualConnectorStrategy eStrategy = gamescope::VirtualConnectorStrategies::PerWindow);

bool env_to_bool(const char *env)
{
	if (!env || !*env)
		return false;

	return !!atoi(env);
}

uint64_t timespec_to_nanos(struct timespec& spec)
{
	return spec.tv_sec * 1'000'000'000ul + spec.tv_nsec;
}

timespec nanos_to_timespec( uint64_t ulNanos )
{
	timespec ts =
	{
		.tv_sec = time_t( ulNanos / 1'000'000'000ul ),
		.tv_nsec = long( ulNanos % 1'000'000'000ul ),
	};
	return ts;
}

static void
update_runtime_info();

gamescope::ConVar<bool> cv_adaptive_sync( "adaptive_sync", false, "Whether or not adaptive sync is enabled if available." );
gamescope::ConVar<bool> cv_adaptive_sync_ignore_overlay( "adaptive_sync_ignore_overlay", false, "Whether or not to ignore overlay planes for pushing commits with adaptive sync." );
gamescope::ConVar<int> cv_adaptive_sync_overlay_cycles( "adaptive_sync_overlay_cycles", 1, "Number of vblank cycles to ignore overlay repaints before forcing a commit with adaptive sync." );

gamescope::ConVar<bool> cv_upscale_preemptive( "upscale_preemptive", true, "Allow pre-emptive upscaling" );
gamescope::ConVar<bool> cv_upscale_preemptive_debug_force_sync( "upscale_preemptive_debug_force_sync", false, "Force synchronize pre-emptive upscaling" );

uint64_t g_SteamCompMgrLimitedAppRefreshCycle = 16'666'666;
uint64_t g_SteamCompMgrAppRefreshCycle = 16'666'666;

static const gamescope_color_mgmt_t k_ScreenshotColorMgmt =
{
	.enabled = true,
	.displayColorimetry = displaycolorimetry_709,
	.displayEOTF = EOTF_Gamma22,
	.outputEncodingColorimetry = displaycolorimetry_709,
	.outputEncodingEOTF = EOTF_Gamma22,
};

static const gamescope_color_mgmt_t k_ScreenshotColorMgmtHDR =
{
	.enabled = true,
	.displayColorimetry = displaycolorimetry_2020,
	.displayEOTF = EOTF_PQ,
	.outputEncodingColorimetry = displaycolorimetry_2020,
	.outputEncodingEOTF = EOTF_PQ,
};

//#define COLOR_MGMT_MICROBENCH
// sudo cpupower frequency-set --governor performance

static void
create_color_mgmt_luts(const gamescope_color_mgmt_t& newColorMgmt, gamescope_color_mgmt_luts outColorMgmtLuts[ EOTF_Count ])
{
	const displaycolorimetry_t& displayColorimetry = newColorMgmt.displayColorimetry;
	const displaycolorimetry_t& outputEncodingColorimetry = newColorMgmt.outputEncodingColorimetry;

	for ( uint32_t nInputEOTF = 0; nInputEOTF < EOTF_Count; nInputEOTF++ )
	{
		if (!outColorMgmtLuts[nInputEOTF].vk_lut1d)
			outColorMgmtLuts[nInputEOTF].vk_lut1d = vulkan_create_1d_lut(s_nLutSize1d);

		if (!outColorMgmtLuts[nInputEOTF].vk_lut3d)
			outColorMgmtLuts[nInputEOTF].vk_lut3d = vulkan_create_3d_lut(s_nLutEdgeSize3d, s_nLutEdgeSize3d, s_nLutEdgeSize3d);

		if ( g_ColorMgmtLutsOverride[nInputEOTF].HasLuts() )
		{
			memcpy(g_ColorMgmtLuts[nInputEOTF].lut1d, g_ColorMgmtLutsOverride[nInputEOTF].lut1d, sizeof(g_ColorMgmtLutsOverride[nInputEOTF].lut1d));
			memcpy(g_ColorMgmtLuts[nInputEOTF].lut3d, g_ColorMgmtLutsOverride[nInputEOTF].lut3d, sizeof(g_ColorMgmtLutsOverride[nInputEOTF].lut3d));
		}
		else
		{
			displaycolorimetry_t inputColorimetry{};
			colormapping_t colorMapping{};

			tonemapping_t tonemapping{};
			tonemapping.bUseShaper = true;

			EOTF inputEOTF = static_cast<EOTF>( nInputEOTF );
			float flGain = 1.f;
			std::shared_ptr<lut3d_t> pSharedLook = g_ColorMgmtLooks[ nInputEOTF ];
			lut3d_t * pLook = pSharedLook && pSharedLook->lutEdgeSize > 0 ? pSharedLook.get() : nullptr;

			if ( inputEOTF == EOTF_Gamma22 )
			{
				flGain = newColorMgmt.flSDRInputGain;
				if ( newColorMgmt.outputEncodingEOTF == EOTF_Gamma22 )
				{
					// G22 -> G22. Does not matter what the g22 mult is
					tonemapping.g22_luminance = 1.f;
					// xwm_log.infof("G22 -> G22");
				}
				else if ( newColorMgmt.outputEncodingEOTF == EOTF_PQ )
				{
					// G22 -> PQ. SDR content going on an HDR output
					tonemapping.g22_luminance = newColorMgmt.flSDROnHDRBrightness;
					// xwm_log.infof("G22 -> PQ");
				}

				// The final display colorimetry is used to build the output mapping, as we want a gamut-aware handling
				// for sdrGamutWideness indepdendent of the output encoding (for SDR data), and when mapping SDR -> PQ output
				// we only want to utilize a portion of the gamut the actual display can reproduce
				buildSDRColorimetry( &inputColorimetry, &colorMapping, newColorMgmt.sdrGamutWideness, displayColorimetry );
			}
			else if ( inputEOTF == EOTF_PQ )
			{
				flGain = newColorMgmt.flHDRInputGain;
				if ( newColorMgmt.outputEncodingEOTF == EOTF_Gamma22 )
				{
					// PQ -> G22  Leverage the display's native brightness
					tonemapping.g22_luminance = newColorMgmt.flInternalDisplayBrightness;

					// Determine the tonemapping parameters
					// Use the external atoms if provided
					tonemap_info_t source = newColorMgmt.hdrTonemapSourceMetadata;
					tonemap_info_t dest = newColorMgmt.hdrTonemapDisplayMetadata;
					// Otherwise, rely on the Vulkan source info and the EDID
					// TODO: If source is invalid, use the provided metadata.
					// TODO: If hdrTonemapDisplayMetadata is invalid, use the one provided by the display

					// Adjust the source brightness range by the requested HDR input gain
					dest.flBlackPointNits /= flGain;
					dest.flWhitePointNits /= flGain;

					if ( source.BIsValid() && dest.BIsValid() )
					{
						tonemapping.eOperator = newColorMgmt.hdrTonemapOperator;
						tonemapping.eetf2390.init( source, newColorMgmt.hdrTonemapDisplayMetadata );
					}
					else
					{
						tonemapping.eOperator = ETonemapOperator_None;
					}
					/*
					xwm_log.infof("PQ -> 2.2  -   g22_luminance %f gain %f", tonemapping.g22_luminance, flGain );
					xwm_log.infof("source %f %f", source.flBlackPointNits, source.flWhitePointNits );
					xwm_log.infof("dest %f %f", dest.flBlackPointNits, dest.flWhitePointNits );
					xwm_log.infof("operator %d", (int) tonemapping.eOperator );*/
				}
				else if ( newColorMgmt.outputEncodingEOTF == EOTF_PQ )
				{
					// PQ -> PQ passthrough (though this does apply gain)
					// TODO: should we manipulate the output static metadata to reflect the gain factor?
					tonemapping.g22_luminance = 1.f;
					// xwm_log.infof("PQ -> PQ");
				}

				buildPQColorimetry( &inputColorimetry, &colorMapping, displayColorimetry );
			}

			calcColorTransform<s_nLutEdgeSize3d>( &g_tmpLut1d, s_nLutSize1d, &g_tmpLut3d, inputColorimetry, inputEOTF,
				outputEncodingColorimetry, newColorMgmt.outputEncodingEOTF,
				newColorMgmt.outputVirtualWhite, newColorMgmt.chromaticAdaptationMode,
				colorMapping, newColorMgmt.nightmode, tonemapping, pLook, flGain );

			// Create quantized output luts
			for ( size_t i=0, end = g_tmpLut1d.dataR.size(); i<end; ++i )
			{
				outColorMgmtLuts[nInputEOTF].lut1d[4*i+0] = quantize_lut_value_16bit( g_tmpLut1d.dataR[i] );
				outColorMgmtLuts[nInputEOTF].lut1d[4*i+1] = quantize_lut_value_16bit( g_tmpLut1d.dataG[i] );
				outColorMgmtLuts[nInputEOTF].lut1d[4*i+2] = quantize_lut_value_16bit( g_tmpLut1d.dataB[i] );
				outColorMgmtLuts[nInputEOTF].lut1d[4*i+3] = 0;
			}

			for ( size_t i=0, end = g_tmpLut3d.data.size(); i<end; ++i )
			{
				outColorMgmtLuts[nInputEOTF].lut3d[4*i+0] = quantize_lut_value_16bit( g_tmpLut3d.data[i].r );
				outColorMgmtLuts[nInputEOTF].lut3d[4*i+1] = quantize_lut_value_16bit( g_tmpLut3d.data[i].g );
				outColorMgmtLuts[nInputEOTF].lut3d[4*i+2] = quantize_lut_value_16bit( g_tmpLut3d.data[i].b );
				outColorMgmtLuts[nInputEOTF].lut3d[4*i+3] = 0;
			}
		}

		outColorMgmtLuts[nInputEOTF].bHasLut1D = true;
		outColorMgmtLuts[nInputEOTF].bHasLut3D = true;

		vulkan_update_luts(outColorMgmtLuts[nInputEOTF].vk_lut1d, outColorMgmtLuts[nInputEOTF].vk_lut3d, outColorMgmtLuts[nInputEOTF].lut1d, outColorMgmtLuts[nInputEOTF].lut3d);
	}
}

gamescope::ConVar<bool> cv_tearing_enabled{ "tearing_enabled", false, "Whether or not tearing is enabled." };
int g_nSteamMaxHeight = 0;
bool g_bVRRCapable_CachedValue = false;
bool g_bVRRInUse_CachedValue = false;
bool g_bSupportsHDR_CachedValue = false;
bool g_bForceHDR10OutputDebug = false;
gamescope::ConVar<bool> cv_hdr_enabled{ "hdr_enabled", false, "Whether or not HDR is enabled if it is available." };
bool g_bHDRItmEnable = false;
int g_nCurrentRefreshRate_CachedValue = 0;

static void
update_color_mgmt()
{
	if ( !GetBackend()->GetCurrentConnector() )
		return;

	GetBackend()->GetCurrentConnector()->GetNativeColorimetry(
		g_bOutputHDREnabled,
		&g_ColorMgmt.pending.displayColorimetry, &g_ColorMgmt.pending.displayEOTF,
		&g_ColorMgmt.pending.outputEncodingColorimetry, &g_ColorMgmt.pending.outputEncodingEOTF );

	g_ColorMgmt.pending.flInternalDisplayBrightness =
		GetBackend()->GetCurrentConnector()->GetHDRInfo().uMaxContentLightLevel;

#ifdef COLOR_MGMT_MICROBENCH
	struct timespec t0, t1;
#else
	// check if any part of our color mgmt stack is dirty
	if ( g_ColorMgmt.pending == g_ColorMgmt.current && g_ColorMgmt.serial != 0 )
		return;
#endif

#ifdef COLOR_MGMT_MICROBENCH
	clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
#endif

	if (g_ColorMgmt.pending.enabled)
	{
		create_color_mgmt_luts(g_ColorMgmt.pending, g_ColorMgmtLuts);
	}
	else
	{
		for ( uint32_t i = 0; i < EOTF_Count; i++ )
			g_ColorMgmtLuts[i].reset();
	}

#ifdef COLOR_MGMT_MICROBENCH
	clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
#endif

#ifdef COLOR_MGMT_MICROBENCH
	double delta = (timespec_to_nanos(t1) - timespec_to_nanos(t0)) / 1000000.0;

	static uint32_t iter = 0;
	static const uint32_t iter_count = 120;
	static double accum = 0;

	accum += delta;

	if (iter++ == iter_count)
	{
		printf("update_color_mgmt: %.3fms\n", accum / iter_count);

		iter = 0;
		accum = 0;
	}
#endif

	static uint32_t s_NextColorMgmtSerial = 0;

	g_ColorMgmt.serial = ++s_NextColorMgmtSerial;
	g_ColorMgmt.current = g_ColorMgmt.pending;
}

static void
update_screenshot_color_mgmt()
{
	create_color_mgmt_luts(k_ScreenshotColorMgmt, g_ScreenshotColorMgmtLuts);
	create_color_mgmt_luts(k_ScreenshotColorMgmtHDR, g_ScreenshotColorMgmtLutsHDR);
}

bool set_color_sdr_gamut_wideness( float flVal )
{
	if ( g_ColorMgmt.pending.sdrGamutWideness == flVal )
		return false;

	g_ColorMgmt.pending.sdrGamutWideness = flVal;

	return g_ColorMgmt.pending.enabled;
}

bool set_internal_display_brightness( float flVal )
{
	if ( flVal < 1.f )
	{
		flVal = 500.f;
	}

	if ( g_ColorMgmt.pending.flInternalDisplayBrightness == flVal )
		return false;

	g_ColorMgmt.pending.flInternalDisplayBrightness = flVal;
	g_flInternalDisplayBrightnessNits = flVal;

	return g_ColorMgmt.pending.enabled;
}

bool set_sdr_on_hdr_brightness( float flVal )
{
	if ( flVal < 1.f )
	{
		flVal = 203.f;
	}

	if ( g_ColorMgmt.pending.flSDROnHDRBrightness == flVal )
		return false;

	g_ColorMgmt.pending.flSDROnHDRBrightness = flVal;

	return g_ColorMgmt.pending.enabled;
}

bool set_hdr_input_gain( float flVal )
{
	if ( flVal < 0.f )
	{
		flVal = 1.f;
	}

	if ( g_ColorMgmt.pending.flHDRInputGain == flVal )
		return false;

	g_ColorMgmt.pending.flHDRInputGain = flVal;

	return g_ColorMgmt.pending.enabled;
}

bool set_sdr_input_gain( float flVal )
{
	if ( flVal < 0.f )
	{
		flVal = 1.f;
	}

	if ( g_ColorMgmt.pending.flSDRInputGain == flVal )
		return false;

	g_ColorMgmt.pending.flSDRInputGain = flVal;
	return g_ColorMgmt.pending.enabled;
}

bool set_color_nightmode( const nightmode_t &nightmode )
{
	if ( g_ColorMgmt.pending.nightmode == nightmode )
		return false;

	g_ColorMgmt.pending.nightmode = nightmode;

	return g_ColorMgmt.pending.enabled;
}
bool set_color_mgmt_enabled( bool bEnabled )
{
	if ( g_ColorMgmt.pending.enabled == bEnabled )
		return false;

	g_ColorMgmt.pending.enabled = bEnabled;

	return true;
}

static gamescope::OwningRc<CVulkanTexture> s_MuraCorrectionImage[gamescope::GAMESCOPE_SCREEN_TYPE_COUNT];
static std::shared_ptr<gamescope::BackendBlob> s_MuraCTMBlob[gamescope::GAMESCOPE_SCREEN_TYPE_COUNT];
static float g_flMuraScale = 1.0f;
static bool g_bMuraCompensationDisabled = false;

bool is_mura_correction_enabled()
{
	if ( !GetBackend()->GetCurrentConnector() )
		return false;

	return s_MuraCorrectionImage[GetBackend()->GetCurrentConnector()->GetScreenType()] != nullptr && !g_bMuraCompensationDisabled;
}

void update_mura_ctm()
{
	s_MuraCTMBlob[gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL] = nullptr;
	if (s_MuraCorrectionImage[gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL] == nullptr)
		return;

	static constexpr float kMuraMapScale = 0.0625f;
	static constexpr float kMuraOffset = -127.0f / 255.0f;

	// Mura's influence scales non-linearly with brightness, so we have an additional scale
	// on top of the scale factor for the underlying mura map.
	const float flScale = g_flMuraScale * kMuraMapScale;
	glm::mat3x4 mura_scale_offset = glm::mat3x4
	{
		flScale, 0,       0,       kMuraOffset * flScale,
		0,       flScale, 0,       kMuraOffset * flScale,
		0,       0,       0,       0, // No mura comp for blue channel.
	};
	s_MuraCTMBlob[gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL] = GetBackend()->CreateBackendBlob( mura_scale_offset );
}

bool g_bMuraDebugFullColor = false;

bool set_mura_overlay( const char *path )
{
	xwm_log.infof("[josh mura correction] Setting mura correction image to: %s", path);
	s_MuraCorrectionImage[gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL] = nullptr;
	update_mura_ctm();

	std::string red_path = std::string(path) + "_red.png";
	std::string green_path = std::string(path) + "_green.png";

	int red_w, red_h, red_comp;
	unsigned char *red_data = stbi_load(red_path.c_str(), &red_w, &red_h, &red_comp, 1);
	int green_w, green_h, green_comp;
	unsigned char *green_data = stbi_load(green_path.c_str(), &green_w, &green_h, &green_comp, 1);
	if (!red_data || !green_data || red_w != green_w || red_h != green_h || red_comp != green_comp || red_comp != 1 || green_comp != 1)
	{
		xwm_log.infof("[josh mura correction] Couldn't load mura correction image, disabling mura correction.");
		return true;
	}

	int w = red_w;
	int h = red_h;
	unsigned char *data = (unsigned char*)malloc(red_w * red_h * 4);

	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			data[(y * w * 4) + (x * 4) + 0] = g_bMuraDebugFullColor ? 255 : red_data[y * w + x];
			data[(y * w * 4) + (x * 4) + 1] = g_bMuraDebugFullColor ? 255 : green_data[y * w + x];
			data[(y * w * 4) + (x * 4) + 2] = 127; // offset of 0.
			data[(y * w * 4) + (x * 4) + 3] = 0;   // Make alpha = 0 so we act as addtive.
		}
	}
	free(red_data);
	free(green_data);

	CVulkanTexture::createFlags texCreateFlags;
	texCreateFlags.bFlippable = true;
	texCreateFlags.bSampled = true;
	s_MuraCorrectionImage[gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL] = vulkan_create_texture_from_bits(w, h, w, h, DRM_FORMAT_ABGR8888, texCreateFlags, (void*)data);
	free(data);

	xwm_log.infof("[josh mura correction] Loaded new mura correction image!");

	update_mura_ctm();

	return true;
}

bool set_mura_scale(float new_scale)
{
	bool diff = g_flMuraScale != new_scale;
	g_flMuraScale = new_scale;
	update_mura_ctm();
	return diff;
}

bool set_color_3dlut_override(const char *path)
{
	int nLutIndex = EOTF_Gamma22;
	g_ColorMgmt.pending.externalDirtyCtr++;
	g_ColorMgmtLutsOverride[nLutIndex].bHasLut3D = false;

	FILE *f = fopen(path, "rb");
	if (!f) {
		return true;
	}
	defer( fclose(f) );

	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);

	size_t elems = fsize / sizeof(uint16_t);

	if (elems == 0) {
		return true;
	}

	fread(g_ColorMgmtLutsOverride[nLutIndex].lut3d, elems, sizeof(uint16_t), f);
	g_ColorMgmtLutsOverride[nLutIndex].bHasLut3D = true;

	return true;
}

bool set_color_shaperlut_override(const char *path)
{
	int nLutIndex = EOTF_Gamma22;
	g_ColorMgmt.pending.externalDirtyCtr++;
	g_ColorMgmtLutsOverride[nLutIndex].bHasLut1D = false;

	FILE *f = fopen(path, "rb");
	if (!f) {
		return true;
	}
	defer( fclose(f) );

	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);

	size_t elems = fsize / sizeof(uint16_t);

	if (elems == 0) {
		return true;
	}

	fread(g_ColorMgmtLutsOverride[nLutIndex].lut1d, elems, sizeof(uint16_t), f);
	g_ColorMgmtLutsOverride[nLutIndex].bHasLut1D = true;

	return true;
}

bool set_color_look_pq(const char *path)
{
	bool bRaisesBlackLevelFloor = false;
	g_ColorMgmtLooks[EOTF_PQ] = LoadCubeLut( path, bRaisesBlackLevelFloor );
	cv_overlay_unmultiplied_alpha = bRaisesBlackLevelFloor;
	g_ColorMgmt.pending.externalDirtyCtr++;
	return true;
}

bool set_color_look_g22(const char *path)
{
	bool bRaisesBlackLevelFloor = false;
	g_ColorMgmtLooks[EOTF_Gamma22] = LoadCubeLut( path, bRaisesBlackLevelFloor );
	cv_overlay_unmultiplied_alpha = bRaisesBlackLevelFloor;
	g_ColorMgmt.pending.externalDirtyCtr++;
	return true;
}

bool g_bColorSliderInUse = false;

template< typename T >
constexpr const T& clamp( const T& x, const T& min, const T& max )
{
    return x < min ? min : max < x ? max : x;
}

extern bool g_bForceRelativeMouse;

CommitDoneList_t g_steamcompmgr_xdg_done_commits;

struct ignore {
	struct ignore	*next;
	unsigned long	sequence;
};

gamescope::CAsyncWaiter<gamescope::Rc<commit_t>> g_ImageWaiter{ "gamescope_img" };

gamescope::CWaiter g_SteamCompMgrWaiter;

Window x11_win(steamcompmgr_win_t *w) {
	if (w == nullptr)
		return None;
	if (w->type != steamcompmgr_win_type_t::XWAYLAND)
		return None;
	return w->xwayland().id;
}

static uint64_t s_ulFocusSerial = 0ul;
void MakeFocusDirty()
{
	s_ulFocusSerial++;
}

static inline uint64_t GetFocusSerial()
{
	return s_ulFocusSerial;
}

bool focus_t::IsDirty()
{
	return ulCurrentFocusSerial != GetFocusSerial();
}

struct global_focus_t : public focus_t
{
	steamcompmgr_win_t	  	 		*keyboardFocusWindow;
	steamcompmgr_win_t		  	 		*fadeWindow;
	MouseCursor		*cursor;

	gamescope::VirtualConnectorKey_t ulVirtualFocusKey = 0;
	std::shared_ptr<gamescope::IBackendConnector> pVirtualConnector;

	gamescope::INestedHints *GetNestedHints()
	{
		gamescope::IBackendConnector *pConnector = this->pVirtualConnector.get();
		if ( !pConnector )
			pConnector = GetBackend()->GetCurrentConnector();

		if ( pConnector )
		{
			return pConnector->GetNestedHints();
		}

		return nullptr;
	}
};

std::unordered_map<gamescope::VirtualConnectorKey_t, global_focus_t> g_VirtualConnectorFocuses;
global_focus_t *GetCurrentFocus()
{
	if ( GetBackend()->GetCurrentConnector() )
	{
		uint64_t ulKey = GetBackend()->GetCurrentConnector()->GetVirtualConnectorKey();

		auto iter = g_VirtualConnectorFocuses.find( ulKey );
		if ( iter != g_VirtualConnectorFocuses.end() )
			return &iter->second;
	}

	if ( g_VirtualConnectorFocuses.size() > 0 )
		return &g_VirtualConnectorFocuses.begin()->second;

	return nullptr;
}

uint32_t		currentOutputWidth, currentOutputHeight;
bool			currentHDROutput = false;
bool			currentHDRForce = false;

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

std::atomic<bool> hasRepaint = false;
bool			hasRepaintNonBasePlane = false;

static gamescope::ConCommand cc_debug_force_repaint( "debug_force_repaint", "Force a repaint",
[]( std::span<std::string_view> args )
{
	hasRepaint = true;
});

unsigned long	damageSequence = 0;

uint64_t		cursorHideTime = 10'000ul * 1'000'000ul;

bool			gotXError = false;

unsigned int	fadeOutStartTime = 0;

unsigned int 	g_FadeOutDuration = 0;

extern float g_flMaxWindowScale;

bool			synchronize;

std::mutex g_SteamCompMgrXWaylandServerMutex;

gamescope::VBlankTime g_SteamCompMgrVBlankTime = {};

uint64_t g_uCurrentBasePlaneCommitID = 0;
bool g_bCurrentBasePlaneIsFifo = false;

static int g_nSteamCompMgrTargetFPS = 0;
static uint64_t g_uDynamicRefreshEqualityTime = 0;
static int g_nDynamicRefreshRate[gamescope::GAMESCOPE_SCREEN_TYPE_COUNT] = { 0, 0 };
// Delay to stop modes flickering back and forth.
static const uint64_t g_uDynamicRefreshDelay = 600'000'000; // 600ms

static int g_nCombinedAppRefreshCycleOverride[gamescope::GAMESCOPE_SCREEN_TYPE_COUNT] = { 0, 0 };
bool g_nCombinedAppRefreshCycleChangeRefresh[gamescope::GAMESCOPE_SCREEN_TYPE_COUNT] = { true, true };
bool g_nCombinedAppRefreshCycleChangeFPS[gamescope::GAMESCOPE_SCREEN_TYPE_COUNT] = { true, true };

static void _update_app_target_refresh_cycle()
{
	if ( !GetBackend()->GetCurrentConnector() )
		return;

	gamescope::GamescopeScreenType type = GetBackend()->GetCurrentConnector()->GetScreenType();

	int target_fps = g_nCombinedAppRefreshCycleOverride[type];

	g_nDynamicRefreshRate[ type ] = 0;
	g_nSteamCompMgrTargetFPS = 0;

	if ( !target_fps )
	{
		return;
	}

	if ( g_nCombinedAppRefreshCycleChangeFPS[ type ] )
	{
		g_nSteamCompMgrTargetFPS = target_fps;
	}

	if ( g_nCombinedAppRefreshCycleChangeRefresh[ type ] )
	{
		auto rates = GetBackend()->GetCurrentConnector()->GetValidDynamicRefreshRates();

		// Find highest mode to do refresh doubling with.
		for ( auto rate = rates.rbegin(); rate != rates.rend(); rate++ )
		{
			if (*rate % target_fps == 0)
			{
				g_nDynamicRefreshRate[ type ] = *rate;
				return;
			}
		}
	}
}

static void update_app_target_refresh_cycle()
{
	int nPrevFPSLimit = g_nSteamCompMgrTargetFPS;
	_update_app_target_refresh_cycle();
	if ( !!g_nSteamCompMgrTargetFPS != !!nPrevFPSLimit )
		update_runtime_info();
}

void steamcompmgr_set_app_refresh_cycle_override( gamescope::GamescopeScreenType type, int override_fps, bool change_refresh, bool change_fps_cap )
{
	g_nCombinedAppRefreshCycleOverride[ type ] = override_fps;
	g_nCombinedAppRefreshCycleChangeRefresh[ type ] = change_refresh;
	g_nCombinedAppRefreshCycleChangeFPS[ type ] = change_fps_cap;
	update_app_target_refresh_cycle();
}

gamescope::ConCommand cc_debug_set_fps_limit( "debug_set_fps_limit", "Set refresh cycle (debug)",
[](std::span<std::string_view> svArgs)
{
	if ( svArgs.size() < 2 )
		return;

	// TODO: Expose all facets as args.
	std::optional<int32_t> onFps = gamescope::Parse<int32_t>( svArgs[1] );
	if ( !onFps )
	{
		console_log.errorf( "Failed to parse FPS." );
		return;
	}

	int32_t nFps = *onFps;

	steamcompmgr_set_app_refresh_cycle_override( GetBackend()->GetScreenType(), nFps, true, true );
});

static int g_nRuntimeInfoFd = -1;

bool g_bFSRActive = false;

BlurMode g_BlurMode = BLUR_MODE_OFF;
BlurMode g_BlurModeOld = BLUR_MODE_OFF;
unsigned int g_BlurFadeDuration = 0;
int g_BlurRadius = 5;
unsigned int g_BlurFadeStartTime = 0;

pid_t focusWindow_pid;
std::shared_ptr<std::string> focusWindow_engine = nullptr;

focus_t g_steamcompmgr_xdg_focus;
std::vector<std::shared_ptr<steamcompmgr_win_t>> g_steamcompmgr_xdg_wins;

static bool
window_is_steam( steamcompmgr_win_t *w )
{
	return w && ( w->isSteamLegacyBigPicture || w->appID == 769 );
}

bool g_bChangeDynamicRefreshBasedOnGameOpenRatherThanActive = false;

bool steamcompmgr_window_should_limit_fps( steamcompmgr_win_t *w )
{
	return w && !window_is_steam( w ) && !w->isOverlay && !w->isExternalOverlay;
}

static bool
steamcompmgr_user_has_any_game_open()
{
	gamescope_xwayland_server_t *server = NULL;
	for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
	{
		if (!server->ctx)
			continue;

		if (steamcompmgr_window_should_limit_fps( server->ctx->focus.focusWindow ))
			return true;
	}

	return false;
}

bool steamcompmgr_window_should_refresh_switch( steamcompmgr_win_t *w )
{
	if ( GetBackend()->GetCurrentConnector() && GetBackend()->GetCurrentConnector()->IsVRRActive() )
		return false;

	if ( g_bChangeDynamicRefreshBasedOnGameOpenRatherThanActive )
		return steamcompmgr_user_has_any_game_open();

	return steamcompmgr_window_should_limit_fps( w );
}


enum HeldCommitTypes_t
{
	HELD_COMMIT_BASE,
	HELD_COMMIT_FADE,

	HELD_COMMIT_COUNT,
};

std::array<gamescope::Rc<commit_t>, HELD_COMMIT_COUNT> g_HeldCommits;
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
extern bool		steamMode;

gamescope::ConVar<bool> cv_composite_force{ "composite_force", false, "Force composition always, never use scanout" };
static bool		useXRes = true;

namespace gamescope
{
	CScreenshotManager &CScreenshotManager::Get()
	{
		static CScreenshotManager s_Instance;
		return s_Instance;
	}

	static ConCommand cc_screenshot( "screenshot", "Take a screenshot to a given path.",
	[]( std::span<std::string_view> args )
	{
		std::string_view szPath = "/tmp/gamescope.png";
		if ( args.size() > 1 )
			szPath = args[1];

		gamescope_control_screenshot_type eScreenshotType = GAMESCOPE_CONTROL_SCREENSHOT_TYPE_BASE_PLANE_ONLY;
		if ( args.size() > 2 )
		{
			std::optional<uint32_t> oType = Parse<uint32_t>( args[2] );
			if ( oType )
				eScreenshotType = static_cast<gamescope_control_screenshot_type>( *oType );
		}

		gamescope::CScreenshotManager::Get().TakeScreenshot( gamescope::GamescopeScreenshotInfo
		{
			.szScreenshotPath      = std::string( szPath ),
			.eScreenshotType       = eScreenshotType,
			.uScreenshotFlags      = 0,
			.bX11PropertyRequested = false,
		} );
	});
}


static std::atomic<bool> g_bForceRepaint{false};

extern int g_nCursorScaleHeight;

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
	return timespec_to_nanos(ts);
}

void sleep_for_nanos(uint64_t nanos)
{
	timespec ts = nanos_to_timespec( nanos );
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

bool xwayland_ctx_t::HasQueuedEvents()
{
	// If mode is QueuedAlready, XEventsQueued() returns the number of
	// events already in the event queue (and never performs a system call).
	return XEventsQueued( dpy, QueuedAlready ) != 0;
}

static steamcompmgr_win_t *
find_win(xwayland_ctx_t *ctx, Window id, bool find_children = true)
{
	steamcompmgr_win_t	*w;

	if (id == None)
	{
		return NULL;
	}

	for (w = ctx->list; w; w = w->xwayland().next)
	{
		if (w->xwayland().id == id)
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
	XQueryTree(ctx->dpy, id, &root, &parent, &children, &childrenCount);
	if (children)
		XFree(children);

	if (root == parent || parent == None)
	{
		return NULL;
	}

	return find_win(ctx, parent);
}

static steamcompmgr_win_t * find_win( xwayland_ctx_t *ctx, struct wlr_surface *surf )
{
	steamcompmgr_win_t	*w = nullptr;

	for (w = ctx->list; w; w = w->xwayland().next)
	{
		if ( w->xwayland().surface.main_surface == surf )
			return w;

		if ( w->xwayland().surface.override_surface == surf )
			return w;
	}

	return nullptr;
}

static gamescope::CBufferMemoizer s_BufferMemos;

// This really needs cleanup, this function is so silly...
static gamescope::Rc<commit_t>
import_commit (
	steamcompmgr_win_t *w,
	struct wlr_surface *surf,
	struct wlr_buffer *buf,
	bool async,
	std::shared_ptr<wlserver_vk_swapchain_feedback> swapchain_feedback,
	std::vector<struct wl_resource*> presentation_feedbacks,
	std::optional<uint32_t> present_id,
	uint64_t desired_present_time,
	bool fifo )
{
	gamescope::Rc<commit_t> commit = new commit_t;

	commit->win_seq = w->seq;
	commit->surf = surf;
	commit->buf = buf;
	commit->async = async;
	commit->fifo = fifo;
	commit->is_steam = window_is_steam( w );
	commit->presentation_feedbacks = std::move(presentation_feedbacks);
	if (swapchain_feedback)
		commit->feedback = *swapchain_feedback;
	commit->present_id = present_id;
	commit->desired_present_time = desired_present_time;

	if ( gamescope::OwningRc<CVulkanTexture> pTexture = s_BufferMemos.LookupVulkanTexture( buf ) )
	{
		// Going from OwningRc -> Rc now.
		commit->vulkanTex = pTexture;
		return commit;
	}

	struct wlr_dmabuf_attributes dmabuf = {0};
	gamescope::OwningRc<gamescope::IBackendFb> pBackendFb;
	if ( wlr_buffer_get_dmabuf( buf, &dmabuf ) )
	{
		pBackendFb = GetBackend()->ImportDmabufToBackend( buf, &dmabuf );
	}

	gamescope::OwningRc<CVulkanTexture> pOwnedTexture = vulkan_create_texture_from_wlr_buffer( buf, std::move( pBackendFb ) );
	commit->vulkanTex = pOwnedTexture;

	s_BufferMemos.MemoizeBuffer( buf, std::move( pOwnedTexture ) );

	return commit;
}

static int32_t
window_last_done_commit_index( steamcompmgr_win_t *w )
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
window_has_commits( steamcompmgr_win_t *w )
{
	return window_last_done_commit_index( w ) != -1;
}

static void
get_window_last_done_commit( steamcompmgr_win_t *w, gamescope::Rc<commit_t> &commit )
{
	int32_t lastCommit = window_last_done_commit_index( w );

	if ( lastCommit == -1 )
	{
		return;
	}

	if ( commit != w->commit_queue[ lastCommit ] )
		commit = w->commit_queue[ lastCommit ];
}

static commit_t*
get_window_last_done_commit_peek( steamcompmgr_win_t *w )
{
	int32_t lastCommit = window_last_done_commit_index( w );

	if ( lastCommit == -1 )
	{
		return nullptr;
	}

	return w->commit_queue[ lastCommit ].get();
}

static int64_t
window_last_done_commit_id( steamcompmgr_win_t *w )
{
	if ( !w )
		return 0;

	commit_t *pCommit = get_window_last_done_commit_peek( w );
	if ( !pCommit )
		return 0;

	return pCommit->commitID;
}

// For Steam, etc.
static bool
window_wants_no_focus_when_mouse_hidden( steamcompmgr_win_t *w )
{
	return window_is_steam( w );
}

static bool
window_is_fullscreen( steamcompmgr_win_t *w )
{
	return w && ( window_is_steam( w ) || w->isFullscreen );
}

void calc_scale_factor_scaler(float &out_scale_x, float &out_scale_y, float sourceWidth, float sourceHeight)
{
	float XOutputRatio = currentOutputWidth / (float)g_nNestedWidth;
	float YOutputRatio = currentOutputHeight / (float)g_nNestedHeight;
	float outputScaleRatio = std::min(XOutputRatio, YOutputRatio);

	float XRatio = (float)g_nNestedWidth / sourceWidth;
	float YRatio = (float)g_nNestedHeight / sourceHeight;

	if (g_upscaleScaler == GamescopeUpscaleScaler::STRETCH)
	{
		out_scale_x = XRatio * XOutputRatio;
		out_scale_y = YRatio * YOutputRatio;
		return;
	}

	if (g_upscaleScaler != GamescopeUpscaleScaler::FILL)
	{
		out_scale_x = std::min(XRatio, YRatio);
		out_scale_y = std::min(XRatio, YRatio);
	}
	else
	{
		out_scale_x = std::max(XRatio, YRatio);
		out_scale_y = std::max(XRatio, YRatio);
	}

	if (g_upscaleScaler == GamescopeUpscaleScaler::AUTO)
	{
		out_scale_x = std::min(g_flMaxWindowScale, out_scale_x);
		out_scale_y = std::min(g_flMaxWindowScale, out_scale_y);
	}

	out_scale_x *= outputScaleRatio;
	out_scale_y *= outputScaleRatio;

	if (g_upscaleScaler == GamescopeUpscaleScaler::INTEGER)
	{
		if (out_scale_x > 1.0f)
		{
			// x == y here always.
			out_scale_x = out_scale_y = floor(out_scale_x);
		}
	}
}

void calc_scale_factor(float &out_scale_x, float &out_scale_y, float sourceWidth, float sourceHeight)
{
	calc_scale_factor_scaler(out_scale_x, out_scale_y, sourceWidth, sourceHeight);

	out_scale_x *= globalScaleRatio;
	out_scale_y *= globalScaleRatio;
}

/**
 * Constructor for a cursor. It is hidden in the beginning (normally until moved by user).
 */
MouseCursor::MouseCursor(xwayland_ctx_t *ctx)
	: m_texture(nullptr)
	, m_dirty(true)
	, m_imageEmpty(false)
	, m_ctx(ctx)
{
	updateCursorFeedback( true );
}

void MouseCursor::UpdatePosition()
{
	wlserver_lock();
	struct wlr_pointer_constraint_v1 *pConstraint = wlserver.GetCursorConstraint();
	if ( pConstraint && pConstraint->current.cursor_hint.enabled )
	{
		m_x = pConstraint->current.cursor_hint.x;
		m_y = pConstraint->current.cursor_hint.y;
		m_bConstrained = true;
	}
	else
	{
		m_x = wlserver.mouse_surface_cursorx;
		m_y = wlserver.mouse_surface_cursory;
		m_bConstrained = false;
	}
	wlserver_unlock();
}

void MouseCursor::checkSuspension()
{
	getTexture();

	if ( ShouldDrawCursor() )
	{
		const bool suspended = int64_t( get_time_in_nanos() ) - int64_t( wlserver.ulLastMovedCursorTime ) > int64_t( cursorHideTime );
		if (!wlserver.bCursorHidden && suspended) {
			wlserver.bCursorHidden = true;

			steamcompmgr_win_t *window = m_ctx->focus.inputFocusWindow;
			// Rearm warp count
			if (window)
			{
				// Move the cursor to the bottom right corner, just off screen if we can
				// if the window (ie. Steam) doesn't want hover/focus events.
				if ( window_wants_no_focus_when_mouse_hidden(window) )
				{
					wlserver_lock();
					wlserver_fake_mouse_pos( window->GetGeometry().nWidth - 1, window->GetGeometry().nHeight - 1 );
					wlserver_mousehide();
					wlserver_unlock();
				}
			}

			// We're hiding the cursor, force redraw if we were showing it
			if (window && !m_imageEmpty ) {
				hasRepaintNonBasePlane = true;
				nudge_steamcompmgr();
			}
		}
	}
	else
	{
		wlserver.bCursorHidden = false;
	}

	wlserver.bCursorHasImage = !m_imageEmpty;

	updateCursorFeedback();
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

bool MouseCursor::setCursorImageByName(const char *name)
{
	int index = XmuCursorNameToIndex(name);
	if (index < 0)
		return false;

	Cursor cursor = XcursorShapeLoadCursor( m_ctx->dpy, index );

	XDefineCursor(m_ctx->dpy, DefaultRootWindow(m_ctx->dpy), cursor);
	XFlush(m_ctx->dpy);
	setDirty();
	return true;
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
	uint64_t ulConnectorId = 0;
	if ( GetBackend()->GetCurrentConnector() )
		ulConnectorId = GetBackend()->GetCurrentConnector()->GetConnectorID();

	if ( ulConnectorId != m_ulLastConnectorId )
	{
		m_ulLastConnectorId = ulConnectorId;
		m_dirty = true;
	}

	if (!m_dirty) {
		return !m_imageEmpty;
	}

	auto *image = XFixesGetCursorImage(m_ctx->dpy);

	if (!image) {
		return false;
	}

	m_hotspotX = image->xhot;
	m_hotspotY = image->yhot;

	int nDesiredWidth = image->width;
	int nDesiredHeight = image->height;
	if ( g_nCursorScaleHeight > 0 )
	{
		GetDesiredSize( nDesiredWidth, nDesiredHeight );
	}

	uint32_t surfaceWidth;
	uint32_t surfaceHeight;
	glm::uvec2 surfaceSize = GetBackend()->CursorSurfaceSize( glm::uvec2{ (uint32_t)nDesiredWidth, (uint32_t)nDesiredHeight } );
	surfaceWidth = surfaceSize.x;
	surfaceHeight = surfaceSize.y;

	m_texture = nullptr;

	// Assume the cursor is fully translucent unless proven otherwise.
	bool bNoCursor = true;

	std::vector<uint32_t> cursorBuffer;

	int nContentWidth = image->width;
	int nContentHeight = image->height;

	if (image->width && image->height)
	{
		if ( nDesiredWidth < image->width || nDesiredHeight < image->height )
		{
			std::vector<uint32_t> pixels(image->width * image->height);
			for (int i = 0; i < image->height; i++)
			{
				for (int j = 0; j < image->width; j++)
				{
					pixels[i * image->width + j] = image->pixels[i * image->width + j];
				}
			} 
			std::vector<uint32_t> resizeBuffer( nDesiredWidth * nDesiredHeight );
			stbir_resize_uint8_srgb( (unsigned char *)pixels.data(),       image->width,  image->height,  0,
									 (unsigned char *)resizeBuffer.data(), nDesiredWidth, nDesiredHeight, 0,
									 4, 3, STBIR_FLAG_ALPHA_PREMULTIPLIED );

			cursorBuffer = std::vector<uint32_t>(surfaceWidth * surfaceHeight);
			for (int i = 0; i < nDesiredHeight; i++)
			{
				for (int j = 0; j < nDesiredWidth; j++)
				{
					cursorBuffer[i * surfaceWidth + j] = resizeBuffer[i * nDesiredWidth + j];
				}
			}

			m_hotspotX = ( m_hotspotX * nDesiredWidth ) / image->width;
			m_hotspotY = ( m_hotspotY * nDesiredHeight ) / image->height;

			nContentWidth = nDesiredWidth;
			nContentHeight = nDesiredHeight;
		}
		else
		{
			cursorBuffer = std::vector<uint32_t>(surfaceWidth * surfaceHeight);
			for (int i = 0; i < image->height; i++)
			{
				for (int j = 0; j < image->width; j++)
				{
					cursorBuffer[i * surfaceWidth + j] = image->pixels[i * image->width + j];
				}
			}
		}
	}

	for (uint32_t i = 0; i < surfaceHeight; i++)
	{
		for (uint32_t j = 0; j < surfaceWidth; j++)
		{
			if ( cursorBuffer[i * surfaceWidth + j] & 0xff000000 )
			{
				bNoCursor = false;
				break;
			}
		}
	}

	if (bNoCursor)
		cursorBuffer.clear();

	m_imageEmpty = bNoCursor;

	m_dirty = false;
	updateCursorFeedback();

	if (m_imageEmpty) {
		if ( GetBackend()->GetCurrentConnector() && GetBackend()->GetCurrentConnector()->GetNestedHints() )
			GetBackend()->GetCurrentConnector()->GetNestedHints()->SetCursorImage( nullptr );
		return false;
	}

	CVulkanTexture::createFlags texCreateFlags;
	texCreateFlags.bFlippable = true;
	if ( GetBackend()->SupportsPlaneHardwareCursor() )
	{
		texCreateFlags.bLinear = true; // cursor buffer needs to be linear
		// TODO: choose format & modifiers from cursor plane
	}

	m_texture = vulkan_create_texture_from_bits(surfaceWidth, surfaceHeight, nContentWidth, nContentHeight, DRM_FORMAT_ARGB8888, texCreateFlags, cursorBuffer.data());

	if ( GetBackend()->GetCurrentConnector() && GetBackend()->GetCurrentConnector()->GetNestedHints() )
	{
		auto info = std::make_shared<gamescope::INestedHints::CursorInfo>(
			gamescope::INestedHints::CursorInfo
			{
				.pPixels   = std::move( cursorBuffer ),
				.uWidth    = (uint32_t) nDesiredWidth,
				.uHeight   = (uint32_t) nDesiredHeight,
				.uXHotspot = image->xhot,
				.uYHotspot = image->yhot,
			});
		GetBackend()->GetCurrentConnector()->GetNestedHints()->SetCursorImage( std::move( info ) );
	}

	assert(m_texture);
	XFree(image);

	return true;
}

void MouseCursor::GetDesiredSize( int& nWidth, int &nHeight )
{
	int nSize = g_nBaseCursorScale;
	if ( g_nCursorScaleHeight > 0 )
	{
		nSize = nSize * floor(g_nOutputHeight / (float)g_nCursorScaleHeight);
		nSize = std::clamp( nSize, g_nBaseCursorScale, 256 );
	}

	nSize = std::min<int>( nSize, glm::compMin( GetBackend()->CursorSurfaceSize( glm::uvec2{ (uint32_t)nSize, (uint32_t)nSize } ) ) );

	nWidth = nSize;
	nHeight = nSize;
}

void MouseCursor::paint(steamcompmgr_win_t *window, steamcompmgr_win_t *fit, struct FrameInfo_t *frameInfo)
{
	if ( m_imageEmpty || wlserver.bCursorHidden )
		return;

	int winX = x();
	int winY = y();

	// Also need new texture
	if (!getTexture()) {
		return;
	}

	int32_t sourceWidth = window->GetGeometry().nWidth;
	int32_t sourceHeight = window->GetGeometry().nHeight;

	if ( fit )
	{
		// If we have an override window, try to fit it in as long as it won't make our scale go below 1.0.
		sourceWidth = std::max<int32_t>( sourceWidth, clamp<int>( fit->GetGeometry().nX + fit->GetGeometry().nWidth, 0, currentOutputWidth ) );
		sourceHeight = std::max<int32_t>( sourceHeight, clamp<int>( fit->GetGeometry().nY + fit->GetGeometry().nHeight, 0, currentOutputHeight ) );
	}

	float cursor_scale = 1.0f;
	if ( g_nCursorScaleHeight > 0 )
	{
		int nDesiredWidth, nDesiredHeight;
		GetDesiredSize( nDesiredWidth, nDesiredHeight );
		cursor_scale = nDesiredHeight / (float)m_texture->contentHeight();
	}
	cursor_scale = std::max(cursor_scale, 1.0f);

	float scaledX, scaledY;
	float currentScaleRatio_x = 1.0;
	float currentScaleRatio_y = 1.0;
	int cursorOffsetX, cursorOffsetY;

	calc_scale_factor(currentScaleRatio_x, currentScaleRatio_y, sourceWidth, sourceHeight);

	cursorOffsetX = (currentOutputWidth - sourceWidth * currentScaleRatio_x) / 2.0f;
	cursorOffsetY = (currentOutputHeight - sourceHeight * currentScaleRatio_y) / 2.0f;

	// Actual point on scaled screen where the cursor hotspot should be
	scaledX = (winX - window->GetGeometry().nX) * currentScaleRatio_x + cursorOffsetX;
	scaledY = (winY - window->GetGeometry().nY) * currentScaleRatio_y + cursorOffsetY;

	if ( zoomScaleRatio != 1.0 )
	{
		scaledX += ((sourceWidth / 2) - winX) * currentScaleRatio_x;
		scaledY += ((sourceHeight / 2) - winY) * currentScaleRatio_y;
	}

	// Apply the cursor offset inside the texture using the display scale
	scaledX = scaledX - (m_hotspotX * cursor_scale);
	scaledY = scaledY - (m_hotspotY * cursor_scale);

	int curLayer = frameInfo->layerCount++;

	FrameInfo_t::Layer_t *layer = &frameInfo->layers[ curLayer ];

	layer->opacity = 1.0;

	layer->scale.x = 1.0f / cursor_scale;
	layer->scale.y = 1.0f / cursor_scale;

	layer->offset.x = -scaledX;
	layer->offset.y = -scaledY;

	layer->zpos = g_zposCursor; // cursor, on top of both bottom layers
	layer->applyColorMgmt = false;

	layer->tex = m_texture;

	layer->filter = cursor_scale != 1.0f ? GamescopeUpscaleFilter::LINEAR : GamescopeUpscaleFilter::NEAREST;
	layer->blackBorder = false;
	layer->ctm = nullptr;
	layer->hdr_metadata_blob = nullptr;
	layer->colorspace = GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB;

	layer->eAlphaBlendingMode = cv_overlay_unmultiplied_alpha ? ALPHA_BLENDING_MODE_COVERAGE : ALPHA_BLENDING_MODE_PREMULTIPLIED;
}

void MouseCursor::updateCursorFeedback( bool bForce )
{
	// Can't resolve this until cursor is un-dirtied.
	if ( m_dirty && !bForce )
		return;

	bool bVisible = !isHidden();

	if ( m_bCursorVisibleFeedback == bVisible && !bForce )
		return;

	uint32_t value = bVisible ? 1 : 0;

	XChangeProperty(m_ctx->dpy, m_ctx->root, m_ctx->atoms.gamescopeCursorVisibleFeedback, XA_CARDINAL, 32, PropModeReplace,
		(unsigned char *)&value, 1 );

	m_bCursorVisibleFeedback = bVisible;
	m_needs_server_flush = true;
}

struct BaseLayerInfo_t
{
	float scale[2];
	float offset[2];
	float opacity;
	GamescopeUpscaleFilter filter;
	AlphaBlendingMode_t eAlphaBlendingMode = ALPHA_BLENDING_MODE_PREMULTIPLIED;
};

std::array< BaseLayerInfo_t, HELD_COMMIT_COUNT > g_CachedPlanes = {};

static void
paint_cached_base_layer(const gamescope::Rc<commit_t>& commit, const BaseLayerInfo_t& base, struct FrameInfo_t *frameInfo, float flOpacityScale, bool bOverrideOpacity )
{
	int curLayer = frameInfo->layerCount++;

	FrameInfo_t::Layer_t *layer = &frameInfo->layers[ curLayer ];

	layer->scale.x = base.scale[0];
	layer->scale.y = base.scale[1];
	layer->offset.x = base.offset[0];
	layer->offset.y = base.offset[1];
	layer->opacity = bOverrideOpacity ? flOpacityScale : base.opacity * flOpacityScale;

	layer->colorspace = commit->colorspace();
	layer->hdr_metadata_blob = nullptr;
	if (commit->feedback)
	{
		layer->hdr_metadata_blob = commit->feedback->hdr_metadata_blob;
	}
	layer->ctm = nullptr;
	if (layer->colorspace == GAMESCOPE_APP_TEXTURE_COLORSPACE_SCRGB)
		layer->ctm = s_scRGB709To2020Matrix;
	layer->tex = commit->vulkanTex;

	layer->filter = base.filter;
	layer->eAlphaBlendingMode = base.eAlphaBlendingMode;
	layer->blackBorder = true;
}

namespace PaintWindowFlag
{
	static const uint32_t BasePlane = 1u << 0;
	static const uint32_t FadeTarget = 1u << 1;
	static const uint32_t NotificationMode = 1u << 2;
	static const uint32_t DrawBorders = 1u << 3;
	static const uint32_t NoScale = 1u << 4;
	static const uint32_t NoFilter = 1u << 5;
	static const uint32_t CoverageMode = 1u << 6;
}
using PaintWindowFlags = uint32_t;

wlserver_vk_swapchain_feedback* steamcompmgr_get_base_layer_swapchain_feedback()
{
	if ( g_HeldCommits[ HELD_COMMIT_BASE ] == nullptr )
		return nullptr;

	if ( !g_HeldCommits[ HELD_COMMIT_BASE ]->feedback )
		return nullptr;

	return &(*g_HeldCommits[ HELD_COMMIT_BASE ]->feedback);
}

gamescope::ConVar<bool> cv_paint_debug_pause_base_plane( "paint_debug_pause_base_plane", false, "Pause updates to the base plane." );

static FrameInfo_t::Layer_t *
paint_window_commit( const gamescope::Rc<commit_t> &lastCommit, steamcompmgr_win_t *w, steamcompmgr_win_t *scaleW, struct FrameInfo_t *frameInfo,
			  MouseCursor *cursor, PaintWindowFlags flags = 0, float flOpacityScale = 1.0f, steamcompmgr_win_t *fit = nullptr )

{
	int32_t sourceWidth, sourceHeight;
	int32_t baseWidth, baseHeight;
	int drawXOffset = 0, drawYOffset = 0;
	float currentScaleRatio_x = 1.0;
	float currentScaleRatio_y = 1.0;
	float baseScaleRatio_x = 1.0;
	float baseScaleRatio_y = 1.0;

	// Exit out if we have no window or
	// no commit.
	//
	// We may have no commit if we're an overlay,
	// in which case, we don't want to add it,
	// or in the case of the base plane, this is our
	// first ever frame so we have no cached base layer
	// to hold on to, so we should not add a layer in that
	// instance either.
	if (!w || lastCommit == nullptr)
		return nullptr;

	// Base plane will stay as tex=0 if we don't have contents yet, which will
	// make us fall back to compositing and use the Vulkan null texture

	int curLayer = frameInfo->layerCount++;

	FrameInfo_t::Layer_t *layer = &frameInfo->layers[ curLayer ];

	layer->filter = ( flags & PaintWindowFlag::NoFilter ) ? GamescopeUpscaleFilter::LINEAR : g_upscaleFilter;

	layer->tex = lastCommit->GetTexture( layer->filter, g_upscaleScaler, layer->colorspace );

	if ( flags & PaintWindowFlag::NoScale )
	{
		sourceWidth = currentOutputWidth;
		sourceHeight = currentOutputHeight;
	}
	else
	{
		// If w == scaleW, then scale the window by the committed buffer size
		// instead of the window size.
		//
		// Some games like Halo Infinite still make swapchains that are eg.
		// 3840x2160 on a 720p window if you do borderless fullscreen.
		//
		// Typically XWayland would do a blit here to avoid that, but when we
		// are using the bypass layer, we don't get that, so we need to handle
		// this case explicitly.
		if (w == scaleW) {
			sourceWidth = layer->tex->width();
			sourceHeight = layer->tex->height();

			baseWidth = lastCommit->vulkanTex->width();
			baseHeight = lastCommit->vulkanTex->height();
		} else {
			sourceWidth = scaleW->GetGeometry().nWidth;
			sourceHeight = scaleW->GetGeometry().nHeight;

			baseWidth = sourceWidth;
			baseHeight = sourceHeight;
		}

		if ( fit )
		{
			// If we have an override window, try to fit it in as long as it won't make our scale go below 1.0.
			sourceWidth = std::max<uint32_t>( sourceWidth, clamp<int>( fit->GetGeometry().nX + fit->GetGeometry().nWidth, 0, currentOutputWidth ) );
			sourceHeight = std::max<uint32_t>( sourceHeight, clamp<int>( fit->GetGeometry().nY + fit->GetGeometry().nHeight, 0, currentOutputHeight ) );

			baseWidth = std::max<uint32_t>( baseWidth, clamp<int>( fit->GetGeometry().nX + fit->GetGeometry().nWidth, 0, currentOutputWidth ) );
			baseHeight = std::max<uint32_t>( baseHeight, clamp<int>( fit->GetGeometry().nY + fit->GetGeometry().nHeight, 0, currentOutputHeight ) );
		}
	}

	bool offset = ( ( w->GetGeometry().nX || w->GetGeometry().nY ) && w != scaleW );

	if (sourceWidth != (int32_t)currentOutputWidth || sourceHeight != (int32_t)currentOutputHeight || offset || globalScaleRatio != 1.0f)
	{
		calc_scale_factor(currentScaleRatio_x, currentScaleRatio_y, sourceWidth, sourceHeight);

		drawXOffset = ((int)currentOutputWidth - (int)sourceWidth * currentScaleRatio_x) / 2.0f;
		drawYOffset = ((int)currentOutputHeight - (int)sourceHeight * currentScaleRatio_y) / 2.0f;

		if ( w != scaleW )
		{
			drawXOffset += w->GetGeometry().nX * currentScaleRatio_x;
			drawYOffset += w->GetGeometry().nY * currentScaleRatio_y;
		}

		calc_scale_factor(baseScaleRatio_x, baseScaleRatio_y, baseWidth, baseHeight);
		if ( zoomScaleRatio != 1.0 )
		{
			drawXOffset += (((int)baseWidth / 2) - (cursor ? cursor->x() : 0)) * baseScaleRatio_x;
			drawYOffset += (((int)baseHeight / 2) - (cursor ? cursor->y() : 0)) * baseScaleRatio_y;
		}
	}

	layer->opacity = ( (w->isOverlay || w->isExternalOverlay) ? w->opacity / (float)OPAQUE : 1.0f ) * flOpacityScale;

	layer->scale.x = 1.0 / currentScaleRatio_x;
	layer->scale.y = 1.0 / currentScaleRatio_y;

	if ( w != scaleW )
	{
		layer->offset.x = -drawXOffset;
		layer->offset.y = -drawYOffset;
	}
	else
	{
		layer->offset.x = -drawXOffset;
		layer->offset.y = -drawYOffset;
	}

	layer->blackBorder = flags & PaintWindowFlag::DrawBorders;

	layer->applyColorMgmt = g_ColorMgmt.pending.enabled;
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

	layer->hdr_metadata_blob = nullptr;
	if (lastCommit->feedback)
	{
		layer->hdr_metadata_blob = lastCommit->feedback->hdr_metadata_blob;
	}
	layer->ctm = nullptr;
	if (layer->colorspace == GAMESCOPE_APP_TEXTURE_COLORSPACE_SCRGB)
		layer->ctm = s_scRGB709To2020Matrix;

	if (layer->filter == GamescopeUpscaleFilter::PIXEL)
	{
		// Don't bother doing more expensive filtering if we are sharp + integer.
		if (float_is_integer(currentScaleRatio_x) && float_is_integer(currentScaleRatio_y))
			layer->filter = GamescopeUpscaleFilter::NEAREST;
	}

	layer->eAlphaBlendingMode = ( flags & PaintWindowFlag::CoverageMode ) ? ALPHA_BLENDING_MODE_COVERAGE : ALPHA_BLENDING_MODE_PREMULTIPLIED;

	return layer;
}

static void
paint_window(steamcompmgr_win_t *w, steamcompmgr_win_t *scaleW, struct FrameInfo_t *frameInfo,
			  MouseCursor *cursor, PaintWindowFlags flags = 0, float flOpacityScale = 1.0f, steamcompmgr_win_t *fit = nullptr )
{
	gamescope::Rc<commit_t> lastCommit;
	if ( w )
		get_window_last_done_commit( w, lastCommit );

	if ( flags & PaintWindowFlag::BasePlane )
	{
		if ( lastCommit == nullptr || cv_paint_debug_pause_base_plane )
		{
			// If we're the base plane and have no valid contents
			// pick up that buffer we've been holding onto if we have one.
			if ( g_HeldCommits[ HELD_COMMIT_BASE ] != nullptr )
			{
				paint_cached_base_layer( g_HeldCommits[ HELD_COMMIT_BASE ], g_CachedPlanes[ HELD_COMMIT_BASE ], frameInfo, flOpacityScale, true );
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

	FrameInfo_t::Layer_t *layer = paint_window_commit( lastCommit, w, scaleW, frameInfo, cursor, flags, flOpacityScale, fit );

	if ( layer && ( flags & PaintWindowFlag::BasePlane ) )
	{
		BaseLayerInfo_t basePlane = {};
		basePlane.scale[0] = layer->scale.x;
		basePlane.scale[1] = layer->scale.y;
		basePlane.offset[0] = layer->offset.x;
		basePlane.offset[1] = layer->offset.y;
		basePlane.opacity = layer->opacity;
		basePlane.filter = layer->filter;
		basePlane.eAlphaBlendingMode = layer->eAlphaBlendingMode;

		g_CachedPlanes[ HELD_COMMIT_BASE ] = basePlane;
		if ( !(flags & PaintWindowFlag::FadeTarget) )
			g_CachedPlanes[ HELD_COMMIT_FADE ] = basePlane;

		g_uCurrentBasePlaneCommitID = lastCommit->commitID;
		g_bCurrentBasePlaneIsFifo = lastCommit->IsPerfOverlayFIFO();
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

#if HAVE_PIPEWIRE
static void paint_pipewire()
{
	static struct pipewire_buffer *s_pPipewireBuffer = nullptr;

	// If the stream stopped/changed, and the underlying pw_buffer was thus
	// destroyed, then destroy this buffer and grab a new one.
	if ( s_pPipewireBuffer && s_pPipewireBuffer->IsStale() )
	{
		pipewire_destroy_buffer( s_pPipewireBuffer );
		s_pPipewireBuffer = nullptr;
	}

	// Queue up a buffer with some metadata.
	if ( !s_pPipewireBuffer )
		s_pPipewireBuffer = dequeue_pipewire_buffer();

	if ( !s_pPipewireBuffer || !s_pPipewireBuffer->texture )
		return;

	struct FrameInfo_t frameInfo = {};
	frameInfo.applyOutputColorMgmt = true;
	frameInfo.outputEncodingEOTF   = EOTF_Gamma22;
	frameInfo.allowVRR             = false;
	frameInfo.bFadingOut           = false;

	// Apply screenshot-style color management.
	for ( uint32_t nInputEOTF = 0; nInputEOTF < EOTF_Count; nInputEOTF++ )
	{
		frameInfo.lut3D[nInputEOTF]     = g_ScreenshotColorMgmtLuts[nInputEOTF].vk_lut3d;
		frameInfo.shaperLut[nInputEOTF] = g_ScreenshotColorMgmtLuts[nInputEOTF].vk_lut1d;
	}

	const uint64_t ulFocusAppId = s_pPipewireBuffer->gamescope_info.focus_appid;

	focus_t *pFocus = nullptr;
	if ( ulFocusAppId )
	{
		static uint64_t s_ulLastFocusAppId = 0;

		bool bAppIdChange = ulFocusAppId != s_ulLastFocusAppId;
		if ( bAppIdChange )
		{
			xwm_log.infof( "Exposing appid %lu (%u 32-bit) focus-wise on pipewire stream.", ulFocusAppId, uint32_t( ulFocusAppId ) );
			s_ulLastFocusAppId = ulFocusAppId;
		}

		static focus_t s_PipewireFocus{};
		if ( s_PipewireFocus.IsDirty() || bAppIdChange )
		{
			std::vector<steamcompmgr_win_t *> vecPossibleFocusWindows = GetGlobalPossibleFocusWindows();

			std::vector<uint32_t> vecAppIds{ uint32_t( ulFocusAppId ) };
			pick_primary_focus_and_override( &s_PipewireFocus, None, vecPossibleFocusWindows, false, vecAppIds );
		}
		pFocus = &s_PipewireFocus;
	}
	else
	{
		pFocus = GetCurrentFocus();
	}

	if ( !pFocus->focusWindow )
		return;

	const bool bAppIdMatches = !ulFocusAppId || pFocus->focusWindow->appID == ulFocusAppId;
	if ( !bAppIdMatches )
		return;

	// If the commits are the same as they were last time, don't repaint and don't push a new buffer on the stream.
	static uint64_t s_ulLastFocusCommitId = 0;
	static uint64_t s_ulLastOverrideCommitId = 0;

	uint64_t ulFocusCommitId = window_last_done_commit_id( pFocus->focusWindow );
	uint64_t ulOverrideCommitId = window_last_done_commit_id( pFocus->overrideWindow );

	if ( ulFocusCommitId == s_ulLastFocusCommitId &&
	     ulOverrideCommitId == s_ulLastOverrideCommitId )
		return;

	s_ulLastFocusCommitId = ulFocusCommitId;
	s_ulLastOverrideCommitId = ulOverrideCommitId;

	uint32_t uWidth = s_pPipewireBuffer->texture->width();
	uint32_t uHeight = s_pPipewireBuffer->texture->height();

	const uint32_t uCompositeDebugBackup = g_uCompositeDebug;
	const uint32_t uBackupWidth = currentOutputWidth;
	const uint32_t uBackupHeight = currentOutputHeight;

	g_uCompositeDebug = 0;
	currentOutputWidth = uWidth;
	currentOutputHeight = uHeight;

	// Paint the windows we have onto the Pipewire stream.
	paint_window( pFocus->focusWindow, pFocus->focusWindow, &frameInfo, nullptr, 0, 1.0f, pFocus->overrideWindow );

	if ( pFocus->overrideWindow && !pFocus->focusWindow->isSteamStreamingClient )
		paint_window( pFocus->overrideWindow, pFocus->focusWindow, &frameInfo, nullptr, PaintWindowFlag::NoFilter, 1.0f, pFocus->overrideWindow );

	gamescope::Rc<CVulkanTexture> pRGBTexture = s_pPipewireBuffer->texture->isYcbcr()
		? vulkan_acquire_screenshot_texture( uWidth, uHeight, false, DRM_FORMAT_XRGB2101010 )
		: gamescope::Rc<CVulkanTexture>{ s_pPipewireBuffer->texture };

	gamescope::Rc<CVulkanTexture> pYUVTexture = s_pPipewireBuffer->texture->isYcbcr() ? s_pPipewireBuffer->texture : nullptr;


	std::optional<uint64_t> oPipewireSequence = vulkan_screenshot( &frameInfo, pRGBTexture, pYUVTexture );
	// If we ever want the fat compositing path, use this.
	//std::optional<uint64_t> oPipewireSequence = vulkan_composite( &frameInfo, s_pPipewireBuffer->texture, false, pRGBTexture, false );

	g_uCompositeDebug = uCompositeDebugBackup;

	currentOutputWidth = uBackupWidth;
	currentOutputHeight = uBackupHeight;

	if ( oPipewireSequence )
	{
		vulkan_wait( *oPipewireSequence, true );

		push_pipewire_buffer( s_pPipewireBuffer );
		s_pPipewireBuffer = nullptr;
	}
}
#endif

gamescope::ConVar<int> cv_cursor_composite{ "cursor_composite", 1, "0 = Never composite a cursor. 1 = Composite cursor when not nested. 2 = Always composite a cursor manually" };
bool ShouldDrawCursor()
{
	if ( cv_cursor_composite == 2 )
		return true;

	if ( cv_cursor_composite == 0 )
		return false;

	if ( g_bForceRelativeMouse )
		return true;

	global_focus_t *pFocus = GetCurrentFocus();
	if ( !pFocus )
		return false;

	return !pFocus->GetNestedHints();
}

gamescope::ConVar<bool> cv_paint_primary_plane{ "paint_primary_plane", true };
gamescope::ConVar<bool> cv_paint_override_redirect_plane{ "paint_override_redirect_plane", true };
gamescope::ConVar<bool> cv_paint_steam_overlay_plane{ "paint_steam_overlay_plane", true };
gamescope::ConVar<bool> cv_paint_external_overlay_plane{ "paint_external_overlay_plane", true };
gamescope::ConVar<bool> cv_paint_cursor_plane{ "paint_cursor_plane", true };
gamescope::ConVar<bool> cv_paint_mura_plane{ "paint_mura_plane", true };

static void
paint_all( global_focus_t *pFocus, bool async )
{
	if ( !pFocus )
		return;

	gamescope::IBackendConnector *pConnector = pFocus->pVirtualConnector.get();
	if ( !pConnector )
		pConnector = GetBackend()->GetCurrentConnector();

	gamescope_xwayland_server_t *root_server = wlserver_get_xwayland_server(0);
	xwayland_ctx_t *root_ctx = root_server->ctx.get();

	static long long int paintID = 0;

	update_color_mgmt();

	paintID++;
	gpuvis_trace_begin_ctx_printf( paintID, "paint_all" );
	steamcompmgr_win_t	*w;
	steamcompmgr_win_t	*overlay;
	steamcompmgr_win_t *externalOverlay;
	steamcompmgr_win_t	*notification;
	steamcompmgr_win_t	*override;
	steamcompmgr_win_t *input;

	unsigned int currentTime = get_time_in_milliseconds();
	bool fadingOut = ( currentTime - fadeOutStartTime < g_FadeOutDuration || g_bPendingFade ) && g_HeldCommits[HELD_COMMIT_FADE] != nullptr;

	w = pFocus->focusWindow;
	overlay = pFocus->overlayWindow;
	externalOverlay = pFocus->externalOverlayWindow;
	notification = pFocus->notificationWindow;
	override = pFocus->overrideWindow;
	input = pFocus->inputFocusWindow;

	if (++frameCounter == 300)
	{
		currentFrameRate = 300 * 1000.0f / (currentTime - lastSampledFrameTime);
		lastSampledFrameTime = currentTime;
		frameCounter = 0;

		stats_printf( "fps=%f\n", currentFrameRate );

		if ( window_is_steam( w ) )
		{
			stats_printf( "focus=steam\n" );
		}
		else
		{
			stats_printf( "focus=%i\n", w ? w->appID : 0 );
		}
	}

	struct FrameInfo_t frameInfo = {};
	frameInfo.applyOutputColorMgmt = g_ColorMgmt.pending.enabled;
	frameInfo.outputEncodingEOTF = g_ColorMgmt.pending.outputEncodingEOTF;
	frameInfo.allowVRR = cv_adaptive_sync;
	frameInfo.bFadingOut = fadingOut;

	// If the window we'd paint as the base layer is the streaming client,
	// find the video underlay and put it up first in the scenegraph
	if ( cv_paint_primary_plane )
	{
		if ( w )
		{
			if ( w->isSteamStreamingClient == true )
			{
				steamcompmgr_win_t *videow = NULL;
				bool bHasVideoUnderlay = false;

				gamescope_xwayland_server_t *server = NULL;
				for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
				{
					for ( videow = server->ctx->list; videow; videow = videow->xwayland().next )
					{
						if ( videow->isSteamStreamingClientVideo == true )
						{
							// TODO: also check matching AppID so we can have several pairs
							paint_window(videow, videow, &frameInfo, pFocus->cursor, PaintWindowFlag::BasePlane | PaintWindowFlag::DrawBorders);
							bHasVideoUnderlay = true;
							break;
						}
					}
				}
				
				int nOldLayerCount = frameInfo.layerCount;

				uint32_t flags = 0;
				if ( !bHasVideoUnderlay )
					flags |= PaintWindowFlag::BasePlane;
				paint_window(w, w, &frameInfo, pFocus->cursor, flags);
				if ( pFocus == GetCurrentFocus() )
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
			
					paint_cached_base_layer(g_HeldCommits[HELD_COMMIT_FADE], g_CachedPlanes[HELD_COMMIT_FADE], &frameInfo, 1.0f - opacityScale, false);
					paint_window(w, w, &frameInfo, pFocus->cursor, PaintWindowFlag::BasePlane | PaintWindowFlag::FadeTarget | PaintWindowFlag::DrawBorders, opacityScale, override);
				}
				else
				{
					{
						if ( g_HeldCommits[HELD_COMMIT_FADE] != nullptr )
						{
							g_HeldCommits[HELD_COMMIT_FADE] = nullptr;
							g_bPendingFade = false;
							fadeOutStartTime = 0;
							pFocus->fadeWindow = None;
						}
					}
					// Just draw focused window as normal, be it Steam or the game
					paint_window(w, w, &frameInfo, pFocus->cursor, PaintWindowFlag::BasePlane | PaintWindowFlag::DrawBorders, 1.0f, override);

					bool needsScaling = frameInfo.layers[0].scale.x < 0.999f && frameInfo.layers[0].scale.y < 0.999f;
					frameInfo.useFSRLayer0 = g_upscaleFilter == GamescopeUpscaleFilter::FSR && needsScaling;
					frameInfo.useNISLayer0 = g_upscaleFilter == GamescopeUpscaleFilter::NIS && needsScaling;
				}
				if ( pFocus == GetCurrentFocus() )
					update_touch_scaling( &frameInfo );
			}
		}
		else
		{
			if ( g_HeldCommits[HELD_COMMIT_BASE] != nullptr )
			{
				float opacityScale = 1.0f;
				if ( fadingOut )
				{
					opacityScale = g_bPendingFade
						? 0.0f
						: ((currentTime - fadeOutStartTime) / (float)g_FadeOutDuration);
				}

				paint_cached_base_layer( g_HeldCommits[HELD_COMMIT_BASE], g_CachedPlanes[HELD_COMMIT_BASE], &frameInfo, opacityScale, true );
			}
		}
	}

	// TODO: We want to paint this at the same scale as the normal window and probably
	// with an offset.
	// Josh: No override if we're streaming video
	// as we will have too many layers. Better to be safe than sorry.
	if ( override && w && !w->isSteamStreamingClient && cv_paint_override_redirect_plane )
	{
		paint_window(override, w, &frameInfo, pFocus->cursor, PaintWindowFlag::NoFilter, 1.0f, override);
		// Don't update touch scaling for frameInfo. We don't ever make it our
		// wlserver_mousefocus window.
		//update_touch_scaling( &frameInfo );
	}

	// If we have any layers that aren't a cursor or overlay, then we have valid contents for presentation.
	const bool bValidContents = frameInfo.layerCount > 0;

  	if (externalOverlay && cv_paint_external_overlay_plane )
	{
		if (externalOverlay->opacity)
		{
			paint_window(externalOverlay, externalOverlay, &frameInfo, pFocus->cursor, PaintWindowFlag::NoScale | PaintWindowFlag::NoFilter |
				( cv_overlay_unmultiplied_alpha ? PaintWindowFlag::CoverageMode : 0 ) );

			if ( externalOverlay == pFocus->inputFocusWindow && pFocus == GetCurrentFocus() )
				update_touch_scaling( &frameInfo );
		}
	}

	if ( cv_paint_steam_overlay_plane )
	{
		if (overlay && overlay->opacity )
		{
			paint_window(overlay, overlay, &frameInfo, pFocus->cursor, PaintWindowFlag::DrawBorders | PaintWindowFlag::NoFilter |
				( cv_overlay_unmultiplied_alpha ? PaintWindowFlag::CoverageMode : 0 )  );

			if ( overlay == pFocus->inputFocusWindow && pFocus == GetCurrentFocus() )
				update_touch_scaling( &frameInfo );
		}
		else if ( !GetBackend()->UsesVulkanSwapchain() && GetBackend()->IsSessionBased() )
		{
			auto tex = vulkan_get_hacky_blank_texture();
			if ( tex != nullptr )
			{
				// HACK! HACK HACK HACK
				// To avoid stutter when toggling the overlay on 
				int curLayer = frameInfo.layerCount++;

				FrameInfo_t::Layer_t *layer = &frameInfo.layers[ curLayer ];


				layer->scale.x = g_nOutputWidth == tex->width() ? 1.0f : tex->width() / (float)g_nOutputWidth;
				layer->scale.y = g_nOutputHeight == tex->height() ? 1.0f : tex->height() / (float)g_nOutputHeight;
				layer->offset.x = 0.0f;
				layer->offset.y = 0.0f;
				layer->opacity = 1.0f; // BLAH
				layer->zpos = g_zposOverlay;
				layer->applyColorMgmt = g_ColorMgmt.pending.enabled;
				layer->eAlphaBlendingMode = ALPHA_BLENDING_MODE_COVERAGE; // misyl: Always coverage and not premult in case of a Look applied.

				layer->colorspace = GAMESCOPE_APP_TEXTURE_COLORSPACE_LINEAR;
				layer->hdr_metadata_blob = nullptr;
				layer->ctm = nullptr;
				layer->tex = tex;

				layer->filter = GamescopeUpscaleFilter::NEAREST;
				layer->blackBorder = true;
			}
		}
	}
	
	if (notification)
	{
		if (notification->opacity)
		{
			paint_window(notification, notification, &frameInfo, pFocus->cursor, PaintWindowFlag::NotificationMode | PaintWindowFlag::NoFilter);
		}
	}

	if (input)
	{
		// Make sure to un-dirty the texture before we do any painting logic.
		// We determine whether we are grabbed etc this way.
		pFocus->cursor->undirty();
	}

	// Draw cursor if we need to
	if (input && ShouldDrawCursor() && cv_paint_cursor_plane) {
		pFocus->cursor->paint(
			input, w == input ? override : nullptr,
			&frameInfo);
	}

	if ( !bValidContents || GetBackend()->IsPaused() )
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
	if ( const auto& heldCommit = g_HeldCommits[HELD_COMMIT_BASE]; heldCommit && heldCommit->upscaledTexture ) {
		g_bFSRActive = ( heldCommit->upscaledTexture->eFilter == GamescopeUpscaleFilter::FSR );
	}

	g_bFirstFrame = false;

	update_app_target_refresh_cycle();

	const bool bSupportsDynamicRefresh = pConnector && !pConnector->GetValidDynamicRefreshRates().empty();
	if ( bSupportsDynamicRefresh )
	{
		auto rates = pConnector->GetValidDynamicRefreshRates();

		int nDynamicRefreshHz = g_nDynamicRefreshRate[GetBackend()->GetScreenType()];

		int nTargetRefreshHz = nDynamicRefreshHz && steamcompmgr_window_should_refresh_switch( pFocus->focusWindow )
			? nDynamicRefreshHz
			: int( rates[ rates.size() - 1 ] );

		uint64_t now = get_time_in_nanos();

		// The actual output hz generated by the mode, could be off by one either side, due to rounding.
		//
		// Check what we cached for the current dynamic refresh Hz we put in -- otherwise, convert
		// g_nOutputRefresh -> Hz rounded.
		int32_t nCurrentDynamicOutputHz = g_nDynamicRefreshHz
			? g_nDynamicRefreshHz
			: gamescope::ConvertmHzToHz( g_nOutputRefresh );

		if ( nCurrentDynamicOutputHz == nTargetRefreshHz )
		{
			g_uDynamicRefreshEqualityTime = now;
		}
		else if ( g_uDynamicRefreshEqualityTime + g_uDynamicRefreshDelay < now )
		{
			GetBackend()->HackTemporarySetDynamicRefresh( nTargetRefreshHz );
		}
	}

	bool bDoMuraCompensation = is_mura_correction_enabled() && frameInfo.layerCount && cv_paint_mura_plane;
	if ( bDoMuraCompensation )
	{
		auto& MuraCorrectionImage = s_MuraCorrectionImage[GetBackend()->GetScreenType()];
		int curLayer = frameInfo.layerCount++;

		FrameInfo_t::Layer_t *layer = &frameInfo.layers[ curLayer ];

		layer->applyColorMgmt = false;
		layer->scale = vec2_t{ 1.0f, 1.0f };
		layer->blackBorder = true;
		layer->colorspace = GAMESCOPE_APP_TEXTURE_COLORSPACE_PASSTHRU;
		layer->hdr_metadata_blob = nullptr;
		layer->opacity = 1.0f;
		layer->zpos = g_zposMuraCorrection;
		layer->filter = GamescopeUpscaleFilter::NEAREST;
		layer->tex = MuraCorrectionImage;
		layer->ctm = s_MuraCTMBlob[GetBackend()->GetScreenType()];

		// Blending needs to be done in Gamma 2.2 space for mura correction to work.
		frameInfo.applyOutputColorMgmt = false;
	}

	for (uint32_t i = 0; i < EOTF_Count; i++)
	{
		if ( g_ColorMgmtLuts[i].HasLuts() )
		{
			frameInfo.shaperLut[i] = g_ColorMgmtLuts[i].vk_lut1d;
			frameInfo.lut3D[i] = g_ColorMgmtLuts[i].vk_lut3d;
		}
	}

	if ( pConnector && pConnector->Present( &frameInfo, async ) != 0 )
	{
		return;
	}

	std::optional<gamescope::GamescopeScreenshotInfo> oScreenshotInfo =
		gamescope::CScreenshotManager::Get().ProcessPendingScreenshot();

	if ( oScreenshotInfo )
	{
		std::filesystem::path path = std::filesystem::path{ oScreenshotInfo->szScreenshotPath };

		uint32_t drmCaptureFormat = DRM_FORMAT_INVALID;

		if ( path.extension() == ".avif" )
			drmCaptureFormat = DRM_FORMAT_XRGB2101010;
		else if ( path.extension() == ".png" )
			drmCaptureFormat = DRM_FORMAT_XRGB8888;
		else if ( path.extension() == ".nv12.bin" )
			drmCaptureFormat = DRM_FORMAT_NV12;

		gamescope::Rc<CVulkanTexture> pScreenshotTexture;
		if ( drmCaptureFormat != DRM_FORMAT_INVALID )
			pScreenshotTexture = vulkan_acquire_screenshot_texture( g_nOutputWidth, g_nOutputHeight, false, drmCaptureFormat );

		if ( pScreenshotTexture )
		{
			bool bHDRScreenshot = path.extension() == ".avif" &&
								  frameInfo.layerCount > 0 &&
								  ColorspaceIsHDR( frameInfo.layers[0].colorspace ) &&
								  oScreenshotInfo->eScreenshotType != GAMESCOPE_CONTROL_SCREENSHOT_TYPE_SCREEN_BUFFER;

			if ( drmCaptureFormat == DRM_FORMAT_NV12 || oScreenshotInfo->eScreenshotType != GAMESCOPE_CONTROL_SCREENSHOT_TYPE_SCREEN_BUFFER )
			{
				// Basically no color mgmt applied for screenshots. (aside from being able to handle HDR content with LUTs)
				for ( uint32_t nInputEOTF = 0; nInputEOTF < EOTF_Count; nInputEOTF++ )
				{
					auto& luts = bHDRScreenshot ? g_ScreenshotColorMgmtLutsHDR : g_ScreenshotColorMgmtLuts;
					frameInfo.lut3D[nInputEOTF] = luts[nInputEOTF].vk_lut3d;
					frameInfo.shaperLut[nInputEOTF] = luts[nInputEOTF].vk_lut1d;
				}

				if ( oScreenshotInfo->eScreenshotType == GAMESCOPE_CONTROL_SCREENSHOT_TYPE_BASE_PLANE_ONLY )
				{
					// Remove everything but base planes from the screenshot.
					for (int i = 0; i < frameInfo.layerCount; i++)
					{
						if (frameInfo.layers[i].zpos >= (int)g_zposExternalOverlay)
						{
							frameInfo.layerCount = i;
							break;
						}
					}
				}
				else
				{
					if ( is_mura_correction_enabled() )
					{
						// Remove the last layer which is for mura...
						for (int i = 0; i < frameInfo.layerCount; i++)
						{
							if (frameInfo.layers[i].zpos >= (int)g_zposMuraCorrection)
							{
								frameInfo.layerCount = i;
								break;
							}
						}
					}
				}

				// Re-enable output color management (blending) if it was disabled by mura.
				frameInfo.applyOutputColorMgmt = true;
			}

			frameInfo.outputEncodingEOTF = bHDRScreenshot ? EOTF_PQ : EOTF_Gamma22;

			uint32_t uCompositeDebugBackup = g_uCompositeDebug;

			if ( oScreenshotInfo->eScreenshotType != GAMESCOPE_CONTROL_SCREENSHOT_TYPE_SCREEN_BUFFER )
			{
				g_uCompositeDebug = 0;
			}

			std::optional<uint64_t> oScreenshotSeq;
			if ( drmCaptureFormat == DRM_FORMAT_NV12 )
				oScreenshotSeq = vulkan_composite( &frameInfo, pScreenshotTexture, false, nullptr );
			else if ( oScreenshotInfo->eScreenshotType == GAMESCOPE_CONTROL_SCREENSHOT_TYPE_FULL_COMPOSITION ||
					  oScreenshotInfo->eScreenshotType == GAMESCOPE_CONTROL_SCREENSHOT_TYPE_SCREEN_BUFFER )
				oScreenshotSeq = vulkan_composite( &frameInfo, nullptr, false, pScreenshotTexture );
			else
				oScreenshotSeq = vulkan_screenshot( &frameInfo, pScreenshotTexture, nullptr );

			if ( oScreenshotInfo->eScreenshotType != GAMESCOPE_CONTROL_SCREENSHOT_TYPE_SCREEN_BUFFER )
			{
				g_uCompositeDebug = uCompositeDebugBackup;
			}

			if ( !oScreenshotSeq )
			{
				xwm_log.errorf("vulkan_screenshot failed");
				return;
			}

			vulkan_wait( *oScreenshotSeq, false );

			uint16_t maxCLLNits = 0;
			uint16_t maxFALLNits = 0;

			if ( bHDRScreenshot )
			{
				// Unfortunately games give us very bogus values here.
				// Thus we don't really use them.
				// Instead rely on the display it was initially tonemapped for.
				//if ( g_ColorMgmt.current.appHDRMetadata )
				//{
				//	maxCLLNits = g_ColorMgmt.current.appHDRMetadata->metadata.hdmi_metadata_type1.max_cll;
				//	maxFALLNits = g_ColorMgmt.current.appHDRMetadata->metadata.hdmi_metadata_type1.max_fall;
				//}

				if ( !maxCLLNits && !maxFALLNits )
				{
					if ( pConnector )
					{
						maxCLLNits = pConnector->GetHDRInfo().uMaxContentLightLevel;
						maxFALLNits = pConnector->GetHDRInfo().uMaxFrameAverageLuminance;
					}
				}

				if ( !maxCLLNits && !maxFALLNits )
				{
					maxCLLNits = g_ColorMgmt.pending.flInternalDisplayBrightness;
					maxFALLNits = g_ColorMgmt.pending.flInternalDisplayBrightness * 0.8f;
				}
			}

			std::thread screenshotThread = std::thread([=] {
				pthread_setname_np( pthread_self(), "gamescope-scrsh" );

				const uint8_t *mappedData = pScreenshotTexture->mappedData();

				bool bScreenshotSuccess = false;

				if ( pScreenshotTexture->format() == VK_FORMAT_A2R10G10B10_UNORM_PACK32 )
				{
					// Make our own copy of the image to remove the alpha channel.
					constexpr uint32_t kCompCnt = 3;
					auto imageData = std::vector<uint16_t>( g_nOutputWidth * g_nOutputHeight * kCompCnt );

					for (uint32_t y = 0; y < g_nOutputHeight; y++)
					{
						for (uint32_t x = 0; x < g_nOutputWidth; x++)
						{
							uint32_t *pInPixel = (uint32_t *)&mappedData[(y * pScreenshotTexture->rowPitch()) + x * (32 / 8)];
							uint32_t uInPixel = *pInPixel;

							imageData[y * g_nOutputWidth * kCompCnt + x * kCompCnt + 0] = (uInPixel & (0b1111111111 << 20)) >> 20;
							imageData[y * g_nOutputWidth * kCompCnt + x * kCompCnt + 1] = (uInPixel & (0b1111111111 << 10)) >> 10;
							imageData[y * g_nOutputWidth * kCompCnt + x * kCompCnt + 2] = (uInPixel & (0b1111111111 << 0))  >> 0;
						}
					}

					assert( HAVE_AVIF );
#if HAVE_AVIF
					avifResult avifResult = AVIF_RESULT_OK;

					avifImage *pAvifImage = avifImageCreate( g_nOutputWidth, g_nOutputHeight, 10, AVIF_PIXEL_FORMAT_YUV444 );
					defer( avifImageDestroy( pAvifImage ) );
					pAvifImage->yuvRange = AVIF_RANGE_FULL;
					pAvifImage->colorPrimaries = bHDRScreenshot ? AVIF_COLOR_PRIMARIES_BT2020 : AVIF_COLOR_PRIMARIES_BT709;
					pAvifImage->transferCharacteristics = bHDRScreenshot ? AVIF_TRANSFER_CHARACTERISTICS_SMPTE2084 : AVIF_TRANSFER_CHARACTERISTICS_SRGB;
					// We are not actually using YUV, but storing raw GBR (yes not RGB) data
					// This does not compress as well, but is always lossless!
					pAvifImage->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_IDENTITY;

					if ( oScreenshotInfo->eScreenshotType == GAMESCOPE_CONTROL_SCREENSHOT_TYPE_SCREEN_BUFFER )
					{
						// When dumping the screen output buffer for debugging,
						// mark the primaries as UNKNOWN as stuff has likely been transformed
						// to native if HDR on Deck OLED etc.
						// We want everything to be seen unadulterated by a viewer/image editor.
						pAvifImage->colorPrimaries = AVIF_COLOR_PRIMARIES_UNKNOWN;
					}

					if ( bHDRScreenshot )
					{
						pAvifImage->clli.maxCLL = maxCLLNits;
						pAvifImage->clli.maxPALL = maxFALLNits;
					}

					avifRGBImage rgbAvifImage{};
					avifRGBImageSetDefaults( &rgbAvifImage, pAvifImage );
					rgbAvifImage.format = AVIF_RGB_FORMAT_RGB;
					rgbAvifImage.ignoreAlpha = AVIF_TRUE;

					rgbAvifImage.pixels = (uint8_t *)imageData.data();
					rgbAvifImage.rowBytes = g_nOutputWidth * kCompCnt * sizeof( uint16_t );

					if ( ( avifResult = avifImageRGBToYUV( pAvifImage, &rgbAvifImage ) ) != AVIF_RESULT_OK ) // Not really! See Matrix Coefficients IDENTITY above.
					{
						xwm_log.errorf( "Failed to convert RGB to YUV: %u", avifResult );
						return;
					}

					avifEncoder *pEncoder = avifEncoderCreate();
					defer( avifEncoderDestroy( pEncoder ) );
					pEncoder->quality = AVIF_QUALITY_LOSSLESS;
					pEncoder->qualityAlpha = AVIF_QUALITY_LOSSLESS;
					pEncoder->speed = AVIF_SPEED_FASTEST;

					if ( ( avifResult = avifEncoderAddImage( pEncoder, pAvifImage, 1, AVIF_ADD_IMAGE_FLAG_SINGLE ) ) != AVIF_RESULT_OK )
					{
						xwm_log.errorf( "Failed to add image to avif encoder: %u", avifResult );
						return;
					}

					avifRWData avifOutput = AVIF_DATA_EMPTY;
					defer( avifRWDataFree( &avifOutput ) );
					if ( ( avifResult = avifEncoderFinish( pEncoder, &avifOutput ) ) != AVIF_RESULT_OK )
					{
						xwm_log.errorf( "Failed to finish encoder: %u", avifResult );
						return;
					}

					FILE *pScreenshotFile = nullptr;
					if ( ( pScreenshotFile = fopen( oScreenshotInfo->szScreenshotPath.c_str(), "wb" ) ) == nullptr )
					{
						xwm_log.errorf( "Failed to fopen file: %s", oScreenshotInfo->szScreenshotPath.c_str() );
						return;
					}

					fwrite( avifOutput.data, 1, avifOutput.size, pScreenshotFile );
					fclose( pScreenshotFile );

					xwm_log.infof( "Screenshot saved to %s", oScreenshotInfo->szScreenshotPath.c_str() );
					bScreenshotSuccess = true;
#endif
				}
				else if (pScreenshotTexture->format() == VK_FORMAT_B8G8R8A8_UNORM)
				{
					// Make our own copy of the image to remove the alpha channel.
					auto imageData = std::vector<uint8_t>(currentOutputWidth * currentOutputHeight * 4);
					const uint32_t comp = 4;
					const uint32_t pitch = currentOutputWidth * comp;
					for (uint32_t y = 0; y < currentOutputHeight; y++)
					{
						for (uint32_t x = 0; x < currentOutputWidth; x++)
						{
							// BGR...
							imageData[y * pitch + x * comp + 0] = mappedData[y * pScreenshotTexture->rowPitch() + x * comp + 2];
							imageData[y * pitch + x * comp + 1] = mappedData[y * pScreenshotTexture->rowPitch() + x * comp + 1];
							imageData[y * pitch + x * comp + 2] = mappedData[y * pScreenshotTexture->rowPitch() + x * comp + 0];
							imageData[y * pitch + x * comp + 3] = 255;
						}
					}
					if ( stbi_write_png( oScreenshotInfo->szScreenshotPath.c_str(), currentOutputWidth, currentOutputHeight, 4, imageData.data(), pitch ) )
					{
						xwm_log.infof( "Screenshot saved to %s", oScreenshotInfo->szScreenshotPath.c_str() );
						bScreenshotSuccess = true;
					}
					else
					{
						xwm_log.errorf( "Failed to save screenshot to %s", oScreenshotInfo->szScreenshotPath.c_str() );
					}
				}
				else if (pScreenshotTexture->format() == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
				{
					FILE *file = fopen( oScreenshotInfo->szScreenshotPath.c_str(), "wb" );
					if (file)
					{
						fwrite(mappedData, 1, pScreenshotTexture->totalSize(), file );
						fclose(file);

						char cmd[4096];
						sprintf(cmd, "ffmpeg -f rawvideo -pixel_format nv12 -video_size %dx%d -i %s %s_encoded.png", pScreenshotTexture->width(), pScreenshotTexture->height(), oScreenshotInfo->szScreenshotPath.c_str(), oScreenshotInfo->szScreenshotPath.c_str() );

						int ret = system(cmd);

						/* Above call may fail, ffmpeg returns 0 on success */
						if (ret) {
							xwm_log.infof("Ffmpeg call return status %i", ret);
							xwm_log.errorf( "Failed to save screenshot to %s", oScreenshotInfo->szScreenshotPath.c_str() );
						} else {
							xwm_log.infof("Screenshot saved to %s", oScreenshotInfo->szScreenshotPath.c_str());
							bScreenshotSuccess = true;
						}
					}
					else
					{
						xwm_log.errorf( "Failed to save screenshot to %s", oScreenshotInfo->szScreenshotPath.c_str() );
					}
				}

				if ( oScreenshotInfo->bX11PropertyRequested )
				{
					XDeleteProperty( root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeScreenShotAtom );
					XDeleteProperty( root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeDebugScreenShotAtom );
				}

				if ( bScreenshotSuccess && oScreenshotInfo->bWaylandRequested )
				{
						wlserver_lock();
						for ( const auto &control : wlserver.gamescope_controls )
						{
							gamescope_control_send_screenshot_taken( control, oScreenshotInfo->szScreenshotPath.c_str() );
						}
						wlserver_unlock();
				}
			});

			screenshotThread.detach();
		}
		else
		{
			xwm_log.errorf( "Oh no, we ran out of screenshot images. Not actually writing a screenshot." );
			if ( oScreenshotInfo->bX11PropertyRequested )
			{
				XDeleteProperty( root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeScreenShotAtom );
				XDeleteProperty( root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeDebugScreenShotAtom );
			}
		}
	}


	gpuvis_trace_end_ctx_printf( paintID, "paint_all" );
	gpuvis_trace_printf( "paint_all %i layers", (int)frameInfo.layerCount );
}

/* Get prop from window
 *   not found: default
 *   otherwise the value
 */
__attribute__((__no_sanitize_address__)) // x11 broken, returns format 32 even when it only malloc'ed one byte. :(
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
__attribute__((__no_sanitize_address__)) // x11 broken :(
bool get_prop( xwayland_ctx_t *ctx, Window win, Atom prop, std::vector< uint32_t > &vecResult )
{
	Atom actual;
	int format;
	unsigned long n, left;

	vecResult.clear();
	uint64_t *data;
	int result = XGetWindowProperty(ctx->dpy, win, prop, 0L, ~0UL, false,
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

std::string get_string_prop( xwayland_ctx_t *ctx, Window win, Atom prop )
{
	XTextProperty tp;
	if ( !XGetTextProperty( ctx->dpy, win, &tp, prop ) )
		return "";

	std::string value = reinterpret_cast<const char *>( tp.value );
	XFree( tp.value );
	return value;
}

void set_string_prop( xwayland_ctx_t *ctx, Atom prop, const std::string &value )
{
	XTextProperty text_property =
	{
		.value = ( unsigned char * )value.c_str(),
		.encoding = ctx->atoms.utf8StringAtom,
		.format = 8,
		.nitems = strlen( value.c_str() )
	};
	XSetTextProperty( ctx->dpy, ctx->root, &text_property, prop);
	XFlush( ctx->dpy );
}

void clear_prop( xwayland_ctx_t *ctx, Atom prop )
{
	XDeleteProperty( ctx->dpy, ctx->root, prop );
	XFlush( ctx->dpy );
}

static bool
win_has_game_id( steamcompmgr_win_t *w )
{
	return w->appID != 0;
}

static bool
win_is_useless( steamcompmgr_win_t *w )
{
	// Windows that are 1x1 are pretty useless for override redirects.
	// Just ignore them.
	// Fixes the Xbox Login in Age of Empires 2: DE.
	return w->GetGeometry().nWidth == 1 && w->GetGeometry().nHeight == 1;
}

static bool
win_is_override_redirect( steamcompmgr_win_t *w )
{
	if (w->type != steamcompmgr_win_type_t::XWAYLAND)
		return false;

	return w->xwayland().a.override_redirect && !w->ignoreOverrideRedirect && !win_is_useless( w );
}

static bool
win_skip_taskbar_and_pager( steamcompmgr_win_t *w )
{
	return w->skipTaskbar && w->skipPager;
}

static bool
win_skip_and_not_fullscreen( steamcompmgr_win_t *w )
{
	return win_skip_taskbar_and_pager( w ) && !w->isFullscreen;
}

static bool
win_maybe_a_dropdown( steamcompmgr_win_t *w )
{
	if ( w->type != steamcompmgr_win_type_t::XWAYLAND )
		return false;

	// Josh:
	// Right now we don't get enough info from Wine
	// about the true nature of windows to distringuish
	// something like the Fallout 4 Options menu from the
	// Warframe language dropdown. Until we get more stuff
	// exposed for that, there is this workaround to let that work.
	if ( w->appID == 230410 && w->maybe_a_dropdown && w->xwayland().transientFor && ( w->skipPager || w->skipTaskbar ) )
		return !win_is_useless( w );

	// Work around Antichamber splash screen until we hook up
	// the Proton window style deduction.
	if ( w->appID == 219890 )
		return false;

	// The Launcher in Witcher 2 (20920) has a clear window with WS_EX_LAYERED on top of it.
	//
	// The Age of Empires 2 Launcher also has a WS_EX_LAYERED window to separate controls
	// from its backing, which this seems to handle, although we seemingly don't handle
	// it's transparency yet, which I do not understand.
	//
	// Layered windows are windows that are meant to be transparent
	// with alpha blending + visual fx.
	// https://docs.microsoft.com/en-us/windows/win32/winmsg/window-features
	//
	// TODO: Come back to me for original Age of Empires HD launcher.
	// Does that use it? It wants blending!
	// 
	// Only do this if we have CONTROLPARENT right now. Some other apps, such as the
	// Street Fighter V (310950) Splash Screen also use LAYERED and TOOLWINDOW, and we don't
	// want that to be overlayed.
	// Ignore LAYERED if it's marked as top-level with WS_EX_APPWINDOW.
	// TODO: Find more apps using LAYERED.
	const uint32_t validLayered = WS_EX_CONTROLPARENT | WS_EX_LAYERED;
	const uint32_t invalidLayered = WS_EX_APPWINDOW;
	if ( w->hasHwndStyleEx &&
		( ( w->hwndStyleEx & validLayered   ) == validLayered ) &&
		( ( w->hwndStyleEx & invalidLayered ) == 0 ) )
		return true;

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
		w->maybe_a_dropdown && ( ( !w->is_dialog || ( !w->xwayland().transientFor && win_skip_and_not_fullscreen( w ) ) ) && ( w->skipPager || w->skipTaskbar ) );
	return ( valid_maybe_a_dropdown || win_is_override_redirect( w ) ) && !win_is_useless( w );
}

static bool
win_is_disabled( steamcompmgr_win_t *w )
{
	if ( !w->hasHwndStyle )
		return false;

	return !!(w->hwndStyle & WS_DISABLED);
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
is_focus_priority_greater( steamcompmgr_win_t *a, steamcompmgr_win_t *b )
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

	if ( win_is_disabled( a ) != win_is_disabled( b ) )
		return !win_is_disabled( a );

	// Wine sets SKIP_TASKBAR and SKIP_PAGER hints for WS_EX_NOACTIVATE windows.
	// See https://github.com/Plagman/gamescope/issues/87
	if ( win_skip_and_not_fullscreen( a ) != win_skip_and_not_fullscreen( b ) )
		return !win_skip_and_not_fullscreen( a );

	// Prefer normal windows over dialogs
	// if we are an override redirect/dropdown window.
	if ( win_maybe_a_dropdown( a ) && win_maybe_a_dropdown( b ) &&
		a->is_dialog != b->is_dialog )
		return !a->is_dialog;

	if (a->type != steamcompmgr_win_type_t::XWAYLAND)
	{
		return true;
	}

	// Attempt to tie-break dropdowns by transient-for.
	if ( win_maybe_a_dropdown( a ) && win_maybe_a_dropdown( b ) &&
		!a->xwayland().transientFor != !b->xwayland().transientFor )
		return !a->xwayland().transientFor;

	if ( win_has_game_id( a ) && a->xwayland().map_sequence != b->xwayland().map_sequence )
		return a->xwayland().map_sequence > b->xwayland().map_sequence;

	// The damage sequences are only relevant for game windows.
	if ( win_has_game_id( a ) && a->xwayland().damage_sequence != b->xwayland().damage_sequence )
		return a->xwayland().damage_sequence > b->xwayland().damage_sequence;

	return false;
}

static bool is_good_override_candidate( steamcompmgr_win_t *override, steamcompmgr_win_t* focus )
{
	// Some Chrome/Edge dropdowns (ie. FH5 xbox login) will automatically close themselves if you
	// focus them while they are meant to be offscreen (-1,-1 and 1x1) so check that the
	// override's position is on-screen.
	if ( !focus )
		return false;

	auto rect = override->GetGeometry();
	return override != focus && (rect.nX + rect.nWidth) > 0 && (rect.nY + rect.nHeight) > 0;
} 

static bool
pick_primary_focus_and_override(
	focus_t *out,
	Window focusControlWindow,
	const std::vector<steamcompmgr_win_t*>& vecPossibleFocusWindows,
	bool globalFocus,
	const std::vector<uint32_t>& ctxFocusControlAppIDs,
	uint64_t ulVirtualFocusKey,
	gamescope::VirtualConnectorStrategy eStrategy )
{
	bool localGameFocused = false;
	steamcompmgr_win_t *focus = NULL, *override_focus = NULL;

	bool controlledFocus = eStrategy != gamescope::VirtualConnectorStrategies::SingleApplication || focusControlWindow != None || !ctxFocusControlAppIDs.empty();
	if ( controlledFocus )
	{
		if ( eStrategy == gamescope::VirtualConnectorStrategies::SteamControlled )
		{
			if ( focusControlWindow != None )
			{
				for ( steamcompmgr_win_t *focusable_window : vecPossibleFocusWindows )
				{
					if ( focusable_window->type != steamcompmgr_win_type_t::XWAYLAND )
						continue;

					if ( focusable_window->xwayland().id == focusControlWindow )
					{
						focus = focusable_window;
						localGameFocused = true;
						goto found;
					}
				}
			}

			for ( auto focusable_appid : ctxFocusControlAppIDs )
			{
				for ( steamcompmgr_win_t *focusable_window : vecPossibleFocusWindows )
				{
					if ( focusable_window->appID == focusable_appid )
					{
						focus = focusable_window;
						localGameFocused = true;
						goto found;
					}
				}
			}
		}
		else
		{
			for ( steamcompmgr_win_t *focusable_window : vecPossibleFocusWindows )
			{
				if ( focusable_window->GetVirtualConnectorKey( eStrategy ) == ulVirtualFocusKey )
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
		if ( !focus || focus->type != steamcompmgr_win_type_t::XWAYLAND )
			return;

		// Do some searches to find transient links to override redirects too.
		while ( true )
		{
			bool bFoundTransient = false;

			for ( steamcompmgr_win_t *candidate : vecPossibleFocusWindows )
			{
				if ( candidate->type != steamcompmgr_win_type_t::XWAYLAND )
					continue;

				bool is_dropdown = maybe ? win_maybe_a_dropdown( candidate ) : win_is_override_redirect( candidate );
				if ( ( !override_focus || candidate != override_focus ) && candidate != focus &&
					( ( !override_focus && candidate->xwayland().transientFor == focus->xwayland().id ) || ( override_focus && candidate->xwayland().transientFor == override_focus->xwayland().id ) ) &&
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

	if ( focus && focus->type == steamcompmgr_win_type_t::XWAYLAND )
	{
		if ( !focusControlWindow )
		{
			// Do some searches through game windows to follow transient links if needed
			while ( true )
			{
				bool bFoundTransient = false;

				for ( steamcompmgr_win_t *candidate : vecPossibleFocusWindows )
				{
					if ( candidate->type != steamcompmgr_win_type_t::XWAYLAND )
						continue;

					if ( candidate != focus && candidate->xwayland().transientFor == focus->xwayland().id && !win_maybe_a_dropdown( candidate ) )
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

		if ( !override_focus )
		{
			if ( !ctxFocusControlAppIDs.empty() )
			{
				for ( steamcompmgr_win_t *override : vecPossibleFocusWindows )
				{
					if ( win_is_override_redirect(override) && is_good_override_candidate(override, focus) && override->appID == focus->appID ) {
						override_focus = override;
						break;
					}
				}
			}
			else if ( !vecPossibleFocusWindows.empty() )
			{
				for ( steamcompmgr_win_t *override : vecPossibleFocusWindows )
				{
					if ( win_is_override_redirect(override) && is_good_override_candidate(override, focus) ) {
						override_focus = override;
						break;
					}
				}
			}

			resolveTransientOverrides( false );
		}
	}

	if ( focus )
	{
		if ( window_has_commits( focus ) ) 
			out->focusWindow = focus;
		else
			focus->outdatedInteractiveFocus = true;

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
				for ( steamcompmgr_win_t *fake_override : vecPossibleFocusWindows )
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
			for ( steamcompmgr_win_t *fake_override : vecPossibleFocusWindows )
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

 std::vector< steamcompmgr_win_t* > xwayland_ctx_t::GetPossibleFocusWindows()
 {
	std::vector<steamcompmgr_win_t*> vecPossibleFocusWindows;

	for (steamcompmgr_win_t *w = this->list; w; w = w->xwayland().next)
	{
		// Always skip system tray icons and overlays
		if ( w->isSysTrayIcon || w->isOverlay || w->isExternalOverlay )
		{
			continue;
		}

		if ( w->xwayland().a.map_state == IsViewable && w->xwayland().a.c_class == InputOutput &&
			( win_has_game_id( w ) || window_is_steam( w ) || w->isSteamStreamingClient ) &&
			 (w->opacity > TRANSLUCENT || w->isSteamStreamingClient ) )
		{
			vecPossibleFocusWindows.push_back( w );
		}
	}

	std::stable_sort( vecPossibleFocusWindows.begin(), vecPossibleFocusWindows.end(), is_focus_priority_greater );

	return vecPossibleFocusWindows;
 }

static void set_wm_state( xwayland_ctx_t *ctx, Window win, uint32_t state )
{
	uint32_t wmState[] = { state, None };
	XChangeProperty(ctx->dpy, win, ctx->atoms.WMStateAtom, ctx->atoms.WMStateAtom, 32,
				PropModeReplace, (unsigned char *)wmState,
				sizeof(wmState) / sizeof(wmState[0]));
}

void xwayland_ctx_t::DetermineAndApplyFocus( const std::vector< steamcompmgr_win_t* > &vecPossibleFocusWindows )
{
	xwayland_ctx_t *ctx = this;

	steamcompmgr_win_t *inputFocus = NULL;

	steamcompmgr_win_t *prevFocusWindow = ctx->focus.focusWindow;
	ctx->focus.overlayWindow = nullptr;
	ctx->focus.notificationWindow = nullptr;
	ctx->focus.overrideWindow = nullptr;
	ctx->focus.externalOverlayWindow = nullptr;

	unsigned int maxOpacity = 0;
	unsigned int maxOpacityExternal = 0;
	for (steamcompmgr_win_t *w = ctx->list; w; w = w->xwayland().next)
	{
		if (w->isOverlay)
		{
			if (w->GetGeometry().nWidth > 1200 && w->opacity >= maxOpacity)
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

		if ( gamescope::VirtualConnectorIsSingleOutput() )
		{
			if ( w->isOverlay && w->inputFocusMode )
			{
				inputFocus = w;
			}
		}
	}

	gamescope::VirtualConnectorStrategy eStrategy = gamescope::cv_backend_virtual_connector_strategy == gamescope::VirtualConnectorStrategies::SteamControlled
		? gamescope::VirtualConnectorStrategies::SteamControlled
		: gamescope::VirtualConnectorStrategies::PerWindow;

	pick_primary_focus_and_override( &ctx->focus, ctx->focusControlWindow, vecPossibleFocusWindows, false, vecFocuscontrolAppIDs, 0, eStrategy );

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
		set_wm_state( ctx, ctx->focus.focusWindow->xwayland().id, ICCCM_NORMAL_STATE );

		gpuvis_trace_printf( "determine_and_apply_focus focus %lu", ctx->focus.focusWindow->xwayland().id );

		if ( debugFocus == true )
		{
			xwm_log.debugf( "determine_and_apply_focus focus %lu", ctx->focus.focusWindow->xwayland().id );
			char buf[512];
			sprintf( buf,  "xwininfo -id 0x%lx; xprop -id 0x%lx; xwininfo -root -tree", ctx->focus.focusWindow->xwayland().id, ctx->focus.focusWindow->xwayland().id );
			system( buf );
		}
	}

	steamcompmgr_win_t *keyboardFocusWin = inputFocus;

	if ( gamescope::VirtualConnectorIsSingleOutput() )
	{
		if ( inputFocus && inputFocus->inputFocusMode == 2 )
			keyboardFocusWin = ctx->focus.focusWindow;
	}

	Window keyboardFocusWindow = keyboardFocusWin ? keyboardFocusWin->xwayland().id : None;

	// If the top level parent of our current keyboard window is the same as our target (top level) input focus window
	// then keep focus on that and don't yank it away to the top level input focus window.
	// Fixes dropdowns in Steam CEF.
	if ( keyboardFocusWindow && ctx->currentKeyboardFocusWindow && find_win( ctx, ctx->currentKeyboardFocusWindow ) == keyboardFocusWin )
		keyboardFocusWindow = ctx->currentKeyboardFocusWindow;

	if ( ctx->focus.inputFocusWindow != inputFocus ||
		ctx->focus.inputFocusMode != inputFocus->inputFocusMode ||
		ctx->currentKeyboardFocusWindow != keyboardFocusWindow )
	{
		if ( debugFocus == true )
		{
			xwm_log.debugf( "determine_and_apply_focus inputFocus %lu", inputFocus->xwayland().id );
		}

		if ( !ctx->focus.overrideWindow || ctx->focus.overrideWindow != keyboardFocusWin )
			XSetInputFocus(ctx->dpy, keyboardFocusWin->xwayland().id, RevertToNone, CurrentTime);

		if ( ctx->focus.inputFocusWindow != inputFocus ||
			 ctx->focus.inputFocusMode != inputFocus->inputFocusMode )
		{
			// If the window doesn't want focus when hidden, move it away
			// as we are going to hide it straight after.
			// otherwise, if we switch from wanting it to not
			// (steam -> game)
			// put us back in the centre of the screen.
			if (window_wants_no_focus_when_mouse_hidden(inputFocus))
				ctx->focus.bResetToCorner = true;
			else if ( window_wants_no_focus_when_mouse_hidden(inputFocus) != window_wants_no_focus_when_mouse_hidden(ctx->focus.inputFocusWindow) )
				ctx->focus.bResetToCenter = true;

			// cursor is likely not interactable anymore in its original context, hide
			// don't care if we change kb focus window due to that happening when
			// going from override -> focus and we don't want to hide then as it's probably a dropdown.
			ctx->cursor->hide();
		}

		ctx->focus.inputFocusWindow = inputFocus;
		ctx->focus.inputFocusMode = inputFocus->inputFocusMode;
		ctx->currentKeyboardFocusWindow = keyboardFocusWindow;
	}

	steamcompmgr_win_t *w;
	w = ctx->focus.focusWindow;

	if ( inputFocus == ctx->focus.focusWindow && ctx->focus.overrideWindow )
	{
		if ( ctx->list[0].xwayland().id != ctx->focus.overrideWindow->xwayland().id )
		{
			XRaiseWindow(ctx->dpy, ctx->focus.overrideWindow->xwayland().id);
		}
	}
	else
	{
		if ( ctx->list[0].xwayland().id != inputFocus->xwayland().id )
		{
			XRaiseWindow(ctx->dpy, inputFocus->xwayland().id);
		}
	}

	if (!ctx->focus.focusWindow->nudged)
	{
		XMoveWindow(ctx->dpy, ctx->focus.focusWindow->xwayland().id, 1, 1);
		ctx->focus.focusWindow->nudged = true;
	}

	if (w->GetGeometry().nX != 0 || w->GetGeometry().nY != 0)
		XMoveWindow(ctx->dpy, ctx->focus.focusWindow->xwayland().id, 0, 0);

	if ( window_is_fullscreen( ctx->focus.focusWindow ) || ctx->force_windows_fullscreen )
	{
		bool bIsSteam = window_is_steam( ctx->focus.focusWindow );
		int fs_width  = ctx->root_width;
		int fs_height = ctx->root_height;
		if ( bIsSteam && g_nSteamMaxHeight && ctx->root_height > g_nSteamMaxHeight )
		{
			float steam_height_scale = g_nSteamMaxHeight / (float)ctx->root_height;
			fs_height = g_nSteamMaxHeight;
			fs_width  = ctx->root_width * steam_height_scale;
		}

		if ( w->GetGeometry().nWidth != fs_width || w->GetGeometry().nHeight != fs_height || globalScaleRatio != 1.0f )
			XResizeWindow(ctx->dpy, ctx->focus.focusWindow->xwayland().id, fs_width, fs_height);
	}
	else
	{
		if (ctx->focus.focusWindow->sizeHintsSpecified &&
			((unsigned)ctx->focus.focusWindow->GetGeometry().nWidth != ctx->focus.focusWindow->requestedWidth ||
			(unsigned)ctx->focus.focusWindow->GetGeometry().nHeight != ctx->focus.focusWindow->requestedHeight))
		{
			XResizeWindow(ctx->dpy, ctx->focus.focusWindow->xwayland().id, ctx->focus.focusWindow->requestedWidth, ctx->focus.focusWindow->requestedHeight);
		}
	}

	Window	    root_return = None, parent_return = None;
	Window	    *children = NULL;
	unsigned int    nchildren = 0;
	unsigned int    i = 0;

	XQueryTree(ctx->dpy, w->xwayland().id, &root_return, &parent_return, &children, &nchildren);

	while (i < nchildren)
	{
		XSelectInput( ctx->dpy, children[i], FocusChangeMask );
		i++;
	}

	XFree(children);

	ctx->focus.ulCurrentFocusSerial = GetFocusSerial();
}

wlr_surface *win_surface(steamcompmgr_win_t *window)
{
	if (!window)
		return nullptr;

	return window->main_surface();
}

const char *get_win_display_name(steamcompmgr_win_t *window)
{
	if ( window->type == steamcompmgr_win_type_t::XWAYLAND )
		return window->xwayland().ctx->xwayland_server->get_nested_display_name();
	else if ( window->type == steamcompmgr_win_type_t::XDG )
		return wlserver_get_wl_display_name();
	else
		return "";
}


static std::vector< steamcompmgr_win_t* >
steamcompmgr_xdg_get_possible_focus_windows()
{
	std::vector< steamcompmgr_win_t* > windows;
	for ( auto &win : g_steamcompmgr_xdg_wins )
	{
		// Always skip system tray icons and overlays
		if ( win->isSysTrayIcon || win->isOverlay || win->isExternalOverlay )
		{
			continue;
		}

		windows.emplace_back( win.get() );
	}
	return windows;
}

static std::vector< steamcompmgr_win_t* > GetGlobalPossibleFocusWindows()
{
	std::vector< steamcompmgr_win_t* > vecPossibleFocusWindows;

	{
		gamescope_xwayland_server_t *server = NULL;
		for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
		{
			std::vector< steamcompmgr_win_t* > vecLocalPossibleFocusWindows = server->ctx->GetPossibleFocusWindows();
			vecPossibleFocusWindows.insert( vecPossibleFocusWindows.end(), vecLocalPossibleFocusWindows.begin(), vecLocalPossibleFocusWindows.end() );
		}
	}

	{
		std::vector< steamcompmgr_win_t* > vecLocalPossibleFocusWindows = steamcompmgr_xdg_get_possible_focus_windows();
		vecPossibleFocusWindows.insert( vecPossibleFocusWindows.end(), vecLocalPossibleFocusWindows.begin(), vecLocalPossibleFocusWindows.end() );
	}

	// Determine global primary focus
	std::stable_sort( vecPossibleFocusWindows.begin(), vecPossibleFocusWindows.end(), is_focus_priority_greater );

	return vecPossibleFocusWindows;
}

static void
steamcompmgr_xdg_determine_and_apply_focus( const std::vector< steamcompmgr_win_t* > &vecPossibleFocusWindows )
{
	for ( auto &window : g_steamcompmgr_xdg_wins )
	{
		if (window->isOverlay)
			g_steamcompmgr_xdg_focus.overlayWindow = window.get();

		if (window->isExternalOverlay)
			g_steamcompmgr_xdg_focus.externalOverlayWindow = window.get();
	}

	gamescope::VirtualConnectorStrategy eStrategy = gamescope::cv_backend_virtual_connector_strategy == gamescope::VirtualConnectorStrategies::SteamControlled
		? gamescope::VirtualConnectorStrategies::SteamControlled
		: gamescope::VirtualConnectorStrategies::PerWindow;

	pick_primary_focus_and_override( &g_steamcompmgr_xdg_focus, None, vecPossibleFocusWindows, false, vecFocuscontrolAppIDs, 0, eStrategy );
}

uint32_t g_focusedBaseAppId = 0;

static void
determine_and_apply_focus( global_focus_t *pFocus )
{
	gamescope_xwayland_server_t *root_server = wlserver_get_xwayland_server(0);
	xwayland_ctx_t *root_ctx = root_server->ctx.get();
	global_focus_t previousLocalFocus = *pFocus;
	*pFocus = global_focus_t{};
	pFocus->focusWindow = previousLocalFocus.focusWindow;
	pFocus->cursor = root_ctx->cursor.get();
	pFocus->ulVirtualFocusKey = previousLocalFocus.ulVirtualFocusKey;
	pFocus->pVirtualConnector = previousLocalFocus.pVirtualConnector;
	gameFocused = false;

	std::vector< unsigned long > focusable_appids;
	std::vector< unsigned long > focusable_windows;

	// Apply focus to the XWayland contexts.
	{
		gamescope_xwayland_server_t *server = NULL;
		for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
		{
			std::vector< steamcompmgr_win_t* > vecLocalPossibleFocusWindows = server->ctx->GetPossibleFocusWindows();
			if ( server->ctx->focus.IsDirty() )
				server->ctx->DetermineAndApplyFocus( vecLocalPossibleFocusWindows );
		}
	}

	// Apply focus to XDG contexts (TODO merge me with some nice abstraction of "environments")
	{
		std::vector< steamcompmgr_win_t* > vecLocalPossibleFocusWindows = steamcompmgr_xdg_get_possible_focus_windows();
		if ( g_steamcompmgr_xdg_focus.IsDirty() )
			steamcompmgr_xdg_determine_and_apply_focus( vecLocalPossibleFocusWindows );
	}

	// Determine local context focuses
	std::vector<steamcompmgr_win_t *> vecPossibleFocusWindows = GetGlobalPossibleFocusWindows();

	for ( steamcompmgr_win_t *focusable_window : vecPossibleFocusWindows )
	{
		if ( focusable_window->type != steamcompmgr_win_type_t::XWAYLAND )
			continue;

		// Exclude windows that are useless (1x1), skip taskbar + pager or override redirect windows
		// from the reported focusable windows to Steam.
		if ( win_is_useless( focusable_window ) ||
			win_skip_and_not_fullscreen( focusable_window ) ||
			focusable_window->xwayland().a.override_redirect )
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
		focusable_windows.push_back( focusable_window->xwayland().id );
		focusable_windows.push_back( focusable_window->appID );
		focusable_windows.push_back( focusable_window->pid );
	}

	XChangeProperty( root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeFocusableAppsAtom, XA_CARDINAL, 32, PropModeReplace,
					 (unsigned char *)focusable_appids.data(), focusable_appids.size() );

	XChangeProperty( root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeFocusableWindowsAtom, XA_CARDINAL, 32, PropModeReplace,
					 (unsigned char *)focusable_windows.data(), focusable_windows.size() );

	gameFocused = pick_primary_focus_and_override( pFocus, root_ctx->focusControlWindow, vecPossibleFocusWindows, true, vecFocuscontrolAppIDs,
		pFocus->ulVirtualFocusKey,
		gamescope::cv_backend_virtual_connector_strategy );

	// Pick overlay/notifications from root ctx
	pFocus->overlayWindow = root_ctx->focus.overlayWindow;
	pFocus->externalOverlayWindow = root_ctx->focus.externalOverlayWindow;
	pFocus->notificationWindow = root_ctx->focus.notificationWindow;

	if ( !pFocus->overlayWindow )
	{
		pFocus->overlayWindow = g_steamcompmgr_xdg_focus.overlayWindow;
	}

	if ( !pFocus->externalOverlayWindow )
	{
		pFocus->externalOverlayWindow = g_steamcompmgr_xdg_focus.externalOverlayWindow;
	}

	bool bUseOverlay = gamescope::VirtualConnectorIsSingleOutput() || gamescope::VirtualConnectorKeyIsSteam( pFocus->ulVirtualFocusKey );
	if ( !bUseOverlay )
	{
		pFocus->overlayWindow = nullptr;
		pFocus->notificationWindow = nullptr;
	}

	// Pick inputFocusWindow
	if ( gamescope::VirtualConnectorIsSingleOutput() &&
	     pFocus->overlayWindow && pFocus->overlayWindow->inputFocusMode )
	{
		pFocus->inputFocusWindow = pFocus->overlayWindow;
		pFocus->keyboardFocusWindow = pFocus->overlayWindow;
	}
	else
	{
		pFocus->inputFocusWindow = pFocus->focusWindow;
		pFocus->keyboardFocusWindow = pFocus->overrideWindow ? pFocus->overrideWindow : pFocus->focusWindow;
	}

	// Pick cursor from our input focus window

	// Initially pick cursor from the ctx of our input focus.
	if (pFocus->inputFocusWindow)
	{
		if (pFocus->inputFocusWindow->type == steamcompmgr_win_type_t::XWAYLAND)
			pFocus->cursor = pFocus->inputFocusWindow->xwayland().ctx->cursor.get();
		else
		{
			// TODO XDG:
			// Implement cursor support.
			// Probably want some form of abstraction here for
			// wl cursor vs x11 cursor given we have virtual cursors.
			// wlserver should update wl cursor pos xy directly.
			static bool s_once = false;
			if (!s_once)
			{
				xwm_log.errorf("NO CURSOR IMPL XDG");
				s_once = true;
			}
		}
	}

	if (pFocus->inputFocusWindow)
		pFocus->inputFocusMode = pFocus->inputFocusWindow->inputFocusMode;

	if ( gamescope::VirtualConnectorIsSingleOutput() &&
	     pFocus->inputFocusMode == 2 )
	{
		pFocus->keyboardFocusWindow = pFocus->overrideWindow
			? pFocus->overrideWindow
			: pFocus->focusWindow;
	}

	// TODO(strategy): multi-seat on Wayland side
	if ( pFocus == GetCurrentFocus() )
	{
		static gamescope::VirtualConnectorKey_t s_ulPreviousGlobalFocusKey;

		// Tell wlserver about our keyboard/mouse focus.
		if ( pFocus->inputFocusWindow   != previousLocalFocus.inputFocusWindow ||
			pFocus->keyboardFocusWindow != previousLocalFocus.keyboardFocusWindow ||
			pFocus->overrideWindow      != previousLocalFocus.overrideWindow ||
			pFocus->ulVirtualFocusKey   != s_ulPreviousGlobalFocusKey )
		{
			if ( win_surface(pFocus->inputFocusWindow)    != nullptr ||
				 win_surface(pFocus->keyboardFocusWindow) != nullptr )
			{
				wlserver_lock();

				wlserver_clear_dropdowns();
				if ( win_surface( pFocus->overrideWindow ) != nullptr )
					wlserver_notify_dropdown( pFocus->overrideWindow->main_surface(), pFocus->overrideWindow->xwayland().a.x, pFocus->overrideWindow->xwayland().a.y );

				if ( win_surface(pFocus->inputFocusWindow) != nullptr && pFocus->cursor )
					wlserver_mousefocus( pFocus->inputFocusWindow->main_surface(), pFocus->cursor->x(), pFocus->cursor->y() );

				if ( win_surface(pFocus->keyboardFocusWindow) != nullptr )
					wlserver_keyboardfocus( pFocus->keyboardFocusWindow->main_surface() );
				wlserver_unlock();
			}

			// Hide cursor on transitioning between xwaylands
			// We already do this when transitioning input focus inside of an
			// xwayland ctx.
			// don't care if we change kb focus window due to that happening when
			// going from override -> focus and we don't want to hide then as it's probably a dropdown.
			if ( pFocus->cursor && pFocus->inputFocusWindow != previousLocalFocus.inputFocusWindow )
				pFocus->cursor->hide();
		}

		if ( pFocus->inputFocusWindow )
		{
			// Cannot simply XWarpPointer here as we immediately go on to
			// do wlserver_mousefocus and need to update m_x and m_y of the cursor.
			if ( pFocus->inputFocusWindow->GetFocus()->bResetToCorner )
			{
				wlserver_lock();
				wlserver_mousewarp( pFocus->inputFocusWindow->GetGeometry().nWidth / 2, pFocus->inputFocusWindow->GetGeometry().nHeight / 2, 0, true );
				wlserver_fake_mouse_pos( pFocus->inputFocusWindow->GetGeometry().nWidth - 1, pFocus->inputFocusWindow->GetGeometry().nHeight - 1 );
				wlserver_unlock();
			}
			else if ( pFocus->inputFocusWindow->GetFocus()->bResetToCenter )
			{
				wlserver_lock();
				wlserver_mousewarp( pFocus->inputFocusWindow->GetGeometry().nWidth / 2, pFocus->inputFocusWindow->GetGeometry().nHeight / 2, 0, true );
				wlserver_unlock();
			}

			pFocus->inputFocusWindow->GetFocus()->bResetToCorner = false;
			pFocus->inputFocusWindow->GetFocus()->bResetToCenter = false;
		}

		s_ulPreviousGlobalFocusKey = pFocus->ulVirtualFocusKey;
	}

#if HAVE_LIBSYSTEMD
	pid_t newFocusedWindowPID = pFocus->focusWindow ? pFocus->focusWindow->pid : 0;
	if (g_dbus && focusWindow_pid != newFocusedWindowPID) {
		const char *unfocusedWindowUnit = unit_from_pid(focusWindow_pid);
		const char *focusedWindowUnit = unit_from_pid(newFocusedWindowPID);
		bool sameUnit = unfocusedWindowUnit && focusedWindowUnit && !strcmp(unfocusedWindowUnit, focusedWindowUnit);

		if (unfocusedWindowUnit && !sameUnit)
			set_memory_low(unfocusedWindowUnit, false);
		if (focusedWindowUnit && !sameUnit)
			set_memory_low(focusedWindowUnit, true);
	}
#endif

	// Backchannel to Steam
	unsigned long focusedWindow = 0;
	unsigned long focusedAppId = 0;
	unsigned long focusedBaseAppId = 0;
	const char *focused_display = root_ctx->xwayland_server->get_nested_display_name();
	const char *focused_keyboard_display = root_ctx->xwayland_server->get_nested_display_name();
	const char *focused_mouse_display = root_ctx->xwayland_server->get_nested_display_name();

	if ( pFocus->focusWindow )
	{
		focusedWindow = (unsigned long)pFocus->focusWindow->id();
		focusedBaseAppId = pFocus->focusWindow->appID;
		focusedAppId = pFocus->inputFocusWindow->appID;
		focused_display = get_win_display_name(pFocus->focusWindow);
		focusWindow_pid = pFocus->focusWindow->pid;
	}

	g_focusedBaseAppId = (uint32_t)focusedAppId;

	if ( pFocus->inputFocusWindow )
	{
		focused_mouse_display = get_win_display_name(pFocus->inputFocusWindow);
	}

	if ( pFocus->keyboardFocusWindow )
	{
		focused_keyboard_display = get_win_display_name(pFocus->keyboardFocusWindow);
	}

	if ( pFocus == GetCurrentFocus() )
	{
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

		XFlush( root_ctx->dpy );
	}

	// Sort out fading.
	if (pFocus->focusWindow && previousLocalFocus.focusWindow != pFocus->focusWindow)
	{
		if ( g_FadeOutDuration != 0 && !g_bFirstFrame )
		{
			if ( g_HeldCommits[ HELD_COMMIT_FADE ] == nullptr )
			{
				pFocus->fadeWindow = previousLocalFocus.focusWindow;
				g_HeldCommits[ HELD_COMMIT_FADE ] = g_HeldCommits[ HELD_COMMIT_BASE ];
				g_bPendingFade = true;
			}
			else
			{
				// If we end up fading back to what we were going to fade to, cancel the fade.
				if ( pFocus->fadeWindow != nullptr && pFocus->focusWindow == pFocus->fadeWindow )
				{
					g_HeldCommits[ HELD_COMMIT_FADE ] = nullptr;
					g_bPendingFade = false;
					fadeOutStartTime = 0;
					pFocus->fadeWindow = nullptr;
				}
			}
		}
	}

	if ( !cv_paint_debug_pause_base_plane )
	{
		// Update last focus commit
		if ( pFocus->focusWindow &&
			previousLocalFocus.focusWindow != pFocus->focusWindow &&
			!pFocus->focusWindow->isSteamStreamingClient )
		{
			get_window_last_done_commit( pFocus->focusWindow, g_HeldCommits[ HELD_COMMIT_BASE ] );
		}
	}

	// Set SDL window title
	if ( pFocus->GetNestedHints() )
	{
		if ( pFocus->focusWindow )
		{
			pFocus->GetNestedHints()->SetVisible( true );
			if ( previousLocalFocus.focusWindow != pFocus->focusWindow )
			{
				pFocus->GetNestedHints()->SetTitle( pFocus->focusWindow->title );
				pFocus->GetNestedHints()->SetIcon( pFocus->focusWindow->icon );
			}
		}
		else
		{
			pFocus->GetNestedHints()->SetVisible( false );
		}
	}

	// Some games such as Disgaea PC (405900) don't take controller input until
	// the window is first clicked on despite it having focus.
	if ( pFocus->inputFocusWindow && pFocus->inputFocusWindow->appID == 405900 )
	{
		auto now = get_time_in_milliseconds();

		wlserver_lock();
		wlserver_touchdown( 0.5, 0.5, 0, now );
		wlserver_touchup( 0, now + 1 );
		wlserver_mousehide();
		wlserver_unlock();
	}

	pFocus->ulCurrentFocusSerial = GetFocusSerial();
}

static void
get_win_type(xwayland_ctx_t *ctx, steamcompmgr_win_t *w)
{
	w->is_dialog = !!w->xwayland().transientFor;

	std::vector<unsigned int> atoms;
	if ( get_prop( ctx, w->xwayland().id, ctx->atoms.winTypeAtom, atoms ) )
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
get_size_hints(xwayland_ctx_t *ctx, steamcompmgr_win_t *w)
{
	XSizeHints hints;
	long hintsSpecified = 0;

	XGetWMNormalHints(ctx->dpy, w->xwayland().id, &hints, &hintsSpecified);

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
		if (w->xwayland().a.override_redirect)
		{
			Window	    root_return = None, parent_return = None;
			Window	    *children = NULL;
			unsigned int    nchildren = 0;

			XQueryTree(ctx->dpy, w->xwayland().id, &root_return, &parent_return, &children, &nchildren);

			if (nchildren == 1)
			{
				XWindowAttributes attribs;

				XGetWindowAttributes(ctx->dpy, children[0], &attribs);

				// If we have a unique children that isn't override-reidrect that is
				// contained inside this fullscreen window, it's probably it.
				if (attribs.override_redirect == false &&
					attribs.width <= w->GetGeometry().nWidth &&
					attribs.height <= w->GetGeometry().nHeight)
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
get_win_title(xwayland_ctx_t *ctx, steamcompmgr_win_t *w, Atom atom)
{
	assert(atom == XA_WM_NAME || atom == ctx->atoms.netWMNameAtom);

	// Allocates a title we are meant to free,
	// let's re-use this allocation for w->title :)
	XTextProperty tp;
	XGetTextProperty( ctx->dpy, w->xwayland().id, &tp, atom );

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

	if (tp.nitems > 0) {
		// Ride off the allocation from XGetTextProperty.
		w->title = std::make_shared<std::string>((const char *)tp.value);
	} else {
		w->title = NULL;
	}
	w->utf8_title = is_utf8;
}

static void
get_net_wm_state(xwayland_ctx_t *ctx, steamcompmgr_win_t *w)
{
	Atom type;
	int format;
	unsigned long nitems;
	unsigned long bytesAfter;
	unsigned char *data;
	if (XGetWindowProperty(ctx->dpy, w->xwayland().id, ctx->atoms.netWMStateAtom, 0, 2048, false,
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
get_win_icon(xwayland_ctx_t* ctx, steamcompmgr_win_t* w)
{
	w->icon = std::make_shared<std::vector<uint32_t>>();
	get_prop(ctx, w->xwayland().id, ctx->atoms.netWMIcon, *w->icon.get());
}

static void
map_win(xwayland_ctx_t* ctx, Window id, unsigned long sequence)
{
	steamcompmgr_win_t		*w = find_win(ctx, id);

	if (!w)
		return;

	w->xwayland().a.map_state = IsViewable;

	/* This needs to be here or else we lose transparency messages */
	XSelectInput(ctx->dpy, id, PropertyChangeMask | SubstructureNotifyMask |
		LeaveWindowMask | FocusChangeMask);

	XFlush(ctx->dpy);

	/* This needs to be here since we don't get PropertyNotify when unmapped */
	w->opacity = get_prop(ctx, w->xwayland().id, ctx->atoms.opacityAtom, OPAQUE);

	w->isSteamLegacyBigPicture = get_prop(ctx, w->xwayland().id, ctx->atoms.steamAtom, 0);

	/* First try to read the UTF8 title prop, then fallback to the non-UTF8 one */
	get_win_title( ctx, w, ctx->atoms.netWMNameAtom );
	get_win_title( ctx, w, XA_WM_NAME );
	get_win_icon( ctx, w );

	w->inputFocusMode = get_prop(ctx, w->xwayland().id, ctx->atoms.steamInputFocusAtom, 0);

	w->isSteamStreamingClient = get_prop(ctx, w->xwayland().id, ctx->atoms.steamStreamingClientAtom, 0);
	w->isSteamStreamingClientVideo = get_prop(ctx, w->xwayland().id, ctx->atoms.steamStreamingClientVideoAtom, 0);

	if ( steamMode == true )
	{
		uint32_t appID = get_prop(ctx, w->xwayland().id, ctx->atoms.gameAtom, 0);

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
		w->appID = w->xwayland().id;
	}
	
	w->isOverlay = get_prop(ctx, w->xwayland().id, ctx->atoms.overlayAtom, 0);
	w->isExternalOverlay = get_prop(ctx, w->xwayland().id, ctx->atoms.externalOverlayAtom, 0);

	// misyl: Disable appID for overlay types, as parts of the code don't expect that focus-wise.
	// Fixes mangoapp usage when nested, and not in SteamOS.
	if ( w->isExternalOverlay )
		w->appID = 0;

	get_size_hints(ctx, w);

	get_net_wm_state(ctx, w);

	XWMHints *wmHints = XGetWMHints( ctx->dpy, w->xwayland().id );

	if ( wmHints != nullptr )
	{
		if ( wmHints->flags & (InputHint | StateHint ) && wmHints->input == true && wmHints->initial_state == NormalState )
		{
			XRaiseWindow( ctx->dpy, w->xwayland().id );
		}

		XFree( wmHints );
	}

	Window transientFor = None;
	if ( XGetTransientForHint( ctx->dpy, w->xwayland().id, &transientFor ) )
	{
		w->xwayland().transientFor = transientFor;
	}
	else
	{
		w->xwayland().transientFor = None;
	}

	get_win_type( ctx, w );

	w->xwayland().damage_sequence = 0;
	w->xwayland().map_sequence = sequence;

	if ( w == ctx->focus.inputFocusWindow || w->xwayland().id == ctx->currentKeyboardFocusWindow )
	{
		XSetInputFocus(ctx->dpy, w->xwayland().id, RevertToNone, CurrentTime);
	}

	MakeFocusDirty();

	set_wm_state( ctx, w->xwayland().id, ICCCM_NORMAL_STATE );
}

static void
finish_unmap_win(xwayland_ctx_t *ctx, steamcompmgr_win_t *w)
{
	// TODO clear done commits here?

	/* don't care about properties anymore */
	XSelectInput(ctx->dpy, w->xwayland().id, 0);

	ctx->clipChanged = true;
}

static void
unmap_win(xwayland_ctx_t *ctx, Window id, bool fade)
{
	steamcompmgr_win_t *w = find_win(ctx, id);
	if (!w)
		return;
	w->xwayland().a.map_state = IsUnmapped;

	MakeFocusDirty();

	finish_unmap_win(ctx, w);
	set_wm_state( ctx, w->xwayland().id, ICCCM_WITHDRAWN_STATE );
}

uint32_t
get_appid_from_pid( pid_t pid )
{
	uint32_t unFoundAppId = 0;

	char filename[256];
	pid_t next_pid = pid;

	while ( 1 )
	{
		snprintf( filename, sizeof( filename ), "/proc/%i/stat", next_pid );
		std::ifstream proc_stat_file( filename );

		if (!proc_stat_file.is_open() || proc_stat_file.bad())
			break;

		std::string proc_stat;

		std::getline( proc_stat_file, proc_stat );

		char *procName = nullptr;
		char *lastParens = nullptr;

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

		if (!lastParens)
			break;

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
	steamcompmgr_win_t				*new_win = new steamcompmgr_win_t{};
	steamcompmgr_win_t				**p;

	if (!new_win)
		return;

	new_win->seq = ++g_lastWinSeq;
	new_win->type = steamcompmgr_win_type_t::XWAYLAND;
	new_win->_window_types.emplace<steamcompmgr_xwayland_win_t>();

	if (prev)
	{
		for (p = &ctx->list; *p; p = &(*p)->xwayland().next)
			if ((*p)->xwayland().id == prev)
				break;
	}
	else
		p = &ctx->list;
	new_win->xwayland().id = id;
	if (!XGetWindowAttributes(ctx->dpy, id, &new_win->xwayland().a))
	{
		delete new_win;
		return;
	}

	new_win->xwayland().ctx = ctx;
	new_win->xwayland().damage_sequence = 0;
	new_win->xwayland().map_sequence = 0;
	if (new_win->xwayland().a.c_class == InputOnly)
		new_win->xwayland().damage = None;
	else
	{
		new_win->xwayland().damage = XDamageCreate(ctx->dpy, id, XDamageReportRawRectangles);
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
	new_win->isSteamLegacyBigPicture = false;
	new_win->isSteamStreamingClient = false;
	new_win->isSteamStreamingClientVideo = false;
	new_win->inputFocusMode = 0;
	new_win->is_dialog = false;
	new_win->maybe_a_dropdown = false;

	new_win->hasHwndStyle = false;
	new_win->hwndStyle = 0;
	new_win->hasHwndStyleEx = false;
	new_win->hwndStyleEx = 0;

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

	if ( new_win->isExternalOverlay )
		new_win->appID = 0;

	Window transientFor = None;
	if ( XGetTransientForHint( ctx->dpy, id, &transientFor ) )
	{
		new_win->xwayland().transientFor = transientFor;
	}
	else
	{
		new_win->xwayland().transientFor = None;
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

	wlserver_x11_surface_info_init( &new_win->xwayland().surface, ctx->xwayland_server, id );

	{
		std::unique_lock lock( ctx->list_mutex );
		new_win->xwayland().next = *p;
		*p = new_win;
	}
	if (new_win->xwayland().a.map_state == IsViewable)
		map_win(ctx, id, sequence);

	MakeFocusDirty();
}

static void
restack_win(xwayland_ctx_t *ctx, steamcompmgr_win_t *w, Window new_above)
{
	Window  old_above;

	if (w->xwayland().next)
		old_above = w->xwayland().next->xwayland().id;
	else
		old_above = None;
	if (old_above != new_above)
	{
		std::unique_lock lock( ctx->list_mutex );

		steamcompmgr_win_t **prev;

		/* unhook */
		for (prev = &ctx->list; *prev; prev = &(*prev)->xwayland().next)
		{
			if ((*prev) == w)
				break;
		}
		*prev = w->xwayland().next;

		/* rehook */
		for (prev = &ctx->list; *prev; prev = &(*prev)->xwayland().next)
		{
			if ((*prev)->xwayland().id == new_above)
				break;
		}

		w->xwayland().next = *prev;
		*prev = w;
		MakeFocusDirty();
	}
}

static void
configure_win(xwayland_ctx_t *ctx, XConfigureEvent *ce)
{
	steamcompmgr_win_t		    *w = find_win(ctx, ce->window);

	if (!w || w->xwayland().id != ce->window)
	{
		if (ce->window == ctx->root)
		{
			ctx->root_width = ce->width;
			ctx->root_height = ce->height;
			MakeFocusDirty();

			gamescope_xwayland_server_t *root_server = wlserver_get_xwayland_server(0);
			xwayland_ctx_t *root_ctx = root_server->ctx.get();
			XDeleteProperty( root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeXWaylandModeControl );
			XFlush( root_ctx->dpy );
		}
		return;
	}

	w->xwayland().a.x = ce->x;
	w->xwayland().a.y = ce->y;
	w->xwayland().a.width = ce->width;
	w->xwayland().a.height = ce->height;
	w->xwayland().a.border_width = ce->border_width;
	w->xwayland().a.override_redirect = ce->override_redirect;
	restack_win(ctx, w, ce->above);

	MakeFocusDirty();
}

static void
circulate_win(xwayland_ctx_t *ctx, XCirculateEvent *ce)
{
	steamcompmgr_win_t	    *w = find_win(ctx, ce->window);
	Window  new_above;

	if (!w || w->xwayland().id != ce->window)
		return;

	if (ce->place == PlaceOnTop)
		new_above = ctx->list->xwayland().id;
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
	steamcompmgr_win_t	**prev, *w;

	for (prev = &ctx->list; (w = *prev); prev = &w->xwayland().next)
		if (w->xwayland().id == id)
		{
			if (gone)
				finish_unmap_win (ctx, w);
			
			{
				std::unique_lock lock( ctx->list_mutex );
				*prev = w->xwayland().next;
			}
			if (w->xwayland().damage != None)
			{
				XDamageDestroy(ctx->dpy, w->xwayland().damage);
				w->xwayland().damage = None;
			}

			if (gone)
			{
				// release all commits now we are closed.
                w->commit_queue.clear();
			}

			wlserver_lock();
			wlserver_x11_surface_info_finish( &w->xwayland().surface );
			wlserver_unlock();
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

	for ( auto &iter : g_VirtualConnectorFocuses )
	{
		global_focus_t *pFocus = &iter.second;
		// Global Focus
		if (x11_win(pFocus->focusWindow) == id && gone)
			pFocus->focusWindow = nullptr;
		if (x11_win(pFocus->inputFocusWindow) == id && gone)
			pFocus->inputFocusWindow = nullptr;
		if (x11_win(pFocus->overlayWindow) == id && gone)
			pFocus->overlayWindow = nullptr;
		if (x11_win(pFocus->notificationWindow) == id && gone)
			pFocus->notificationWindow = nullptr;
		if (x11_win(pFocus->overrideWindow) == id && gone)
			pFocus->overrideWindow = nullptr;
		if (x11_win(pFocus->fadeWindow) == id && gone)
			pFocus->fadeWindow = nullptr;
	}
		
	MakeFocusDirty();

	finish_destroy_win(ctx, id, gone);
}

static void
damage_win(xwayland_ctx_t *ctx, XDamageNotifyEvent *de)
{
	steamcompmgr_win_t	*w = find_win(ctx, de->drawable);
	steamcompmgr_win_t *focus = ctx->focus.focusWindow;

	if (!w)
		return;

	if (w->IsAnyOverlay() && !w->opacity)
		return;

	// First damage event we get, compute focus; we only want to focus damaged
	// windows to have meaningful frames.
	if (w->appID && w->xwayland().damage_sequence == 0)
		MakeFocusDirty();

	w->xwayland().damage_sequence = damageSequence++;

	// If we just passed the focused window, we might be eliglible to take over
	if ( focus && focus != w && w->appID &&
		w->xwayland().damage_sequence > focus->xwayland().damage_sequence)
		MakeFocusDirty();

	// Josh: This will sometimes cause a BadDamage error.
	// I looked around at different compositors to see what
	// they do here and they just seem to ignore it.
	if (w->xwayland().damage)
	{
		XDamageSubtract(ctx->dpy, w->xwayland().damage, None, None);
	}

	gpuvis_trace_printf( "damage_win win %lx appID %u", w->xwayland().id, w->appID );
}

static void
handle_wl_surface_id(xwayland_ctx_t *ctx, steamcompmgr_win_t *w, uint32_t surfaceID)
{
	struct wlr_surface *current_surface = NULL;
	struct wlr_surface *main_surface = NULL;

	wlserver_lock();

	ctx->xwayland_server->set_wl_id( &w->xwayland().surface, surfaceID );

	current_surface = w->xwayland().surface.current_surface();
	main_surface = w->xwayland().surface.main_surface;
	if ( current_surface == NULL )
	{
		wlserver_unlock();
		return;
	}

	global_focus_t *pCurrentFocus = GetCurrentFocus();
	if ( pCurrentFocus )
	{
		// If we already focused on our side and are handling this late,
		// let wayland know now.
		if ( w == pCurrentFocus->inputFocusWindow )
			wlserver_mousefocus( main_surface, INT32_MAX, INT32_MAX );

		steamcompmgr_win_t *keyboardFocusWindow = pCurrentFocus->inputFocusWindow;

		if ( gamescope::VirtualConnectorIsSingleOutput() &&
		     keyboardFocusWindow && keyboardFocusWindow->inputFocusMode == 2 )
			keyboardFocusWindow = pCurrentFocus->focusWindow;

		if ( w == keyboardFocusWindow )
			wlserver_keyboardfocus( main_surface );
	}

	// Pull the first buffer out of that window, if needed
	xwayland_surface_commit( current_surface );

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
handle_net_wm_state(xwayland_ctx_t *ctx, steamcompmgr_win_t *w, XClientMessageEvent *ev)
{
	uint32_t action = (uint32_t)ev->data.l[0];
	Atom *props = (Atom *)&ev->data.l[1];
	for (size_t i = 0; i < 2; i++) {
		if (props[i] == ctx->atoms.netWMStateFullscreenAtom) {
			update_net_wm_state(action, &w->isFullscreen);
			MakeFocusDirty();
		} else if (props[i] == ctx->atoms.netWMStateSkipTaskbarAtom) {
			update_net_wm_state(action, &w->skipTaskbar);
			MakeFocusDirty();
		} else if (props[i] == ctx->atoms.netWMStateSkipPagerAtom) {
			update_net_wm_state(action, &w->skipPager);
			MakeFocusDirty();
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

			steamcompmgr_win_t *w = find_win(ctx, embed_id);
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
handle_wm_change_state(xwayland_ctx_t *ctx, steamcompmgr_win_t *w, XClientMessageEvent *ev)
{
	long state = ev->data.l[0];

	if (state == ICCCM_ICONIC_STATE) {
		xwm_log.debugf("Faking WM_CHANGE_STATE to ICONIC for window 0x%lx", w->xwayland().id);
		set_wm_state( ctx, w->xwayland().id, ICCCM_ICONIC_STATE );
	} else {
		xwm_log.debugf("Unhandled WM_CHANGE_STATE to %ld for window 0x%lx", state, w->xwayland().id);
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

	steamcompmgr_win_t *w = find_win(ctx, ev->window);
	if (w)
	{
		if (ev->message_type == ctx->atoms.WLSurfaceIDAtom)
		{
			handle_wl_surface_id( ctx, w, uint32_t(ev->data.l[0]));
		}
		else if ( ev->message_type == ctx->atoms.activeWindowAtom )
		{
			XRaiseWindow( ctx->dpy, w->xwayland().id );
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

static void x11_set_selection_owner(xwayland_ctx_t *ctx, std::string contents, GamescopeSelection eSelectionTarget)
{
	Atom target;
	if (eSelectionTarget == GAMESCOPE_SELECTION_CLIPBOARD)
	{
		target = ctx->atoms.clipboard;
	}
	else if (eSelectionTarget == GAMESCOPE_SELECTION_PRIMARY)
	{
		target = ctx->atoms.primarySelection;
	}
	else
	{
		return;
	}

	XSetSelectionOwner(ctx->dpy, target, ctx->ourWindow, CurrentTime);
}

void gamescope_set_selection(std::string contents, GamescopeSelection eSelection)
{
	if (eSelection == GAMESCOPE_SELECTION_CLIPBOARD)
	{
		clipboard = contents;
	}
	else if (eSelection == GAMESCOPE_SELECTION_PRIMARY)
	{
		primarySelection = contents;
	}

	gamescope_xwayland_server_t *server = NULL;
	for (int i = 0; (server = wlserver_get_xwayland_server(i)); i++)
	{
		x11_set_selection_owner(server->ctx.get(), contents, eSelection);
	}
}

void gamescope_set_reshade_effect(std::string effect_path)
{
	gamescope_xwayland_server_t *server = wlserver_get_xwayland_server(0);
	set_string_prop(server->ctx.get(), server->ctx->atoms.gamescopeReshadeEffect, effect_path);
}

void gamescope_clear_reshade_effect() {
	gamescope_xwayland_server_t *server = wlserver_get_xwayland_server(0);
	clear_prop(server->ctx.get(), server->ctx->atoms.gamescopeReshadeEffect);
}

static void
handle_selection_request(xwayland_ctx_t *ctx, XSelectionRequestEvent *ev)
{
	std::string *selection = ev->selection == ctx->atoms.primarySelection ? &primarySelection : &clipboard;

	const char *targetString = XGetAtomName(ctx->dpy, ev->target);

	XEvent response;
	response.xselection.type = SelectionNotify;
	response.xselection.selection = ev->selection;
	response.xselection.requestor = ev->requestor;
	response.xselection.time = ev->time;
	response.xselection.property = None;
	response.xselection.target = None;

	if (ev->requestor == ctx->ourWindow)
	{
		return;
	}

	if (ev->target == ctx->atoms.targets)
	{
		Atom targetList[] = {
			ctx->atoms.targets,
			ctx->atoms.utf8StringAtom,
		};

		XChangeProperty(ctx->dpy, ev->requestor, ev->property, XA_ATOM, 32, PropModeReplace,
				(unsigned char *)&targetList, sizeof(targetList) / sizeof(targetList[0]));
		response.xselection.property = ev->property;
		response.xselection.target = ev->target;
	}
	else if (!strcmp(targetString, "text/plain;charset=utf-8") ||
		!strcmp(targetString, "text/plain") ||
		!strcmp(targetString, "TEXT") ||
		!strcmp(targetString, "UTF8_STRING") ||
		!strcmp(targetString, "STRING"))
	{

		XChangeProperty(ctx->dpy, ev->requestor, ev->property, ev->target, 8, PropModeReplace,
				(unsigned char *)selection->c_str(), selection->length());
		response.xselection.property = ev->property;
		response.xselection.target = ev->target;
	}
	else
	{
		xwm_log.debugf("Unsupported clipboard type: %s.  Ignoring", targetString);
	}

	XSendEvent(ctx->dpy, ev->requestor, False, NoEventMask, &response);
	XFlush(ctx->dpy);
}

static void
handle_selection_notify(xwayland_ctx_t *ctx, XSelectionEvent *ev)
{
	Atom actual_type;
	int actual_format;
	unsigned long nitems;
	unsigned long bytes_after;
	unsigned char *data = NULL;

	XGetWindowProperty(ctx->dpy, ev->requestor, ev->property, 0, 0, False, AnyPropertyType,
			&actual_type, &actual_format, &nitems, &bytes_after, &data);
	if (data) {
		XFree(data);
	}

	if (actual_type == ctx->atoms.utf8StringAtom && actual_format == 8) {
		XGetWindowProperty(ctx->dpy, ev->requestor, ev->property, 0, bytes_after, False, AnyPropertyType,
				&actual_type, &actual_format, &nitems, &bytes_after, &data);
		if (data) {
			const char *contents = (const char *) data;
			auto szContents = std::make_shared<std::string>(contents);
			defer( XFree( data ); );

			gamescope::INestedHints *hints = nullptr;
			if (auto connector = GetBackend()->GetCurrentConnector())
				hints = connector->GetNestedHints();

			if (ev->selection == ctx->atoms.clipboard)
			{
				if ( hints )
				{
					hints->SetSelection( szContents, GAMESCOPE_SELECTION_CLIPBOARD );
				}
				else
				{
					gamescope_set_selection( contents, GAMESCOPE_SELECTION_CLIPBOARD );
				}
			}
			else if (ev->selection == ctx->atoms.primarySelection)
			{
				if ( hints )
				{
					hints->SetSelection( szContents, GAMESCOPE_SELECTION_PRIMARY );
				}
				else
				{
					gamescope_set_selection( contents, GAMESCOPE_SELECTION_PRIMARY );
				}
			}
			else
			{
				xwm_log.errorf( "Selection '%s' not supported.  Ignoring", XGetAtomName(ctx->dpy, ev->selection) );
			}
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
steamcompmgr_flush_frame_done( steamcompmgr_win_t *w )
{
	wlr_surface *current_surface = w->current_surface();
	if ( current_surface && w->unlockedForFrameCallback && w->receivedDoneCommit )
	{
		// TODO: Look into making this _RAW
		// wlroots, seems to just use normal MONOTONIC
		// all over so this may be problematic to just change.
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);

		wlr_surface *main_surface = w->main_surface();
		w->unlockedForFrameCallback = false;
		w->receivedDoneCommit = false;

		w->last_commit_first_latch_time = timespec_to_nanos(now);

		// Acknowledge commit once.
		wlserver_lock();

		if ( main_surface != nullptr )
		{
			wlserver_send_frame_done(main_surface, &now);
		}

		if ( current_surface != nullptr && main_surface != current_surface )
		{
			wlserver_send_frame_done(current_surface, &now);
		}

		wlserver_unlock();
	}
}

static std::optional<uint64_t> s_oLowestFPSLimitScheduleVRR;

static bool steamcompmgr_should_vblank_window( bool bShouldLimitFPS, uint64_t vblank_idx, steamcompmgr_win_t *w = nullptr, uint64_t now = 0 )
{
	bool bSendCallback = true;

	int nRefreshHz = gamescope::ConvertmHzToHz( g_nNestedRefresh ? g_nNestedRefresh : g_nOutputRefresh );
	int nTargetFPS = g_nSteamCompMgrTargetFPS;

	if ( GetBackend()->GetCurrentConnector() && GetBackend()->GetCurrentConnector()->IsVRRActive() )
	{
		bool bCloseEnough = std::abs( g_nSteamCompMgrTargetFPS - nRefreshHz ) < 2;

		if ( g_nSteamCompMgrTargetFPS && bShouldLimitFPS && w && !bCloseEnough )
		{
			uint64_t schedule = w->last_commit_first_latch_time + g_SteamCompMgrLimitedAppRefreshCycle;

			static constexpr uint64_t k_ulVRRScheduleFudge = 200'000; // 0.2ms
			if ( now + k_ulVRRScheduleFudge < schedule )
			{
				bSendCallback = false;

				if ( !s_oLowestFPSLimitScheduleVRR )
					s_oLowestFPSLimitScheduleVRR = schedule;
				else
					s_oLowestFPSLimitScheduleVRR = std::min( *s_oLowestFPSLimitScheduleVRR, schedule );
			}
		}
	}
	else
	{
		if ( g_nSteamCompMgrTargetFPS && bShouldLimitFPS && nRefreshHz > nTargetFPS )
		{
			int nVblankDivisor = nRefreshHz / nTargetFPS;

			if ( vblank_idx % nVblankDivisor != 0 )
				bSendCallback = false;
		}
	}

	return bSendCallback;
}

static bool steamcompmgr_should_vblank_window( steamcompmgr_win_t *w, uint64_t vblank_idx, uint64_t now )
{
	return steamcompmgr_should_vblank_window( steamcompmgr_window_should_limit_fps( w ), vblank_idx, w, now );
}

static void
steamcompmgr_latch_frame_done( steamcompmgr_win_t *w, uint64_t vblank_idx, uint64_t now )
{
	if ( steamcompmgr_should_vblank_window( w, vblank_idx, now ) )
	{
		w->unlockedForFrameCallback = true;
	}
}

static inline float santitize_float( float f )
{
#ifndef __FAST_MATH__
	return ( std::isfinite( f ) ? f : 0.f );
#else
	return f;
#endif
}

static void
handle_property_notify(xwayland_ctx_t *ctx, XPropertyEvent *ev)
{
	/* check if Trans property was changed */
	if (ev->atom == ctx->atoms.opacityAtom)
	{
		/* reset mode and redraw window */
		steamcompmgr_win_t * w = find_win(ctx, ev->window);
		if ( w != nullptr )
		{
			unsigned int newOpacity = get_prop(ctx, w->xwayland().id, ctx->atoms.opacityAtom, OPAQUE);

			if (newOpacity != w->opacity)
			{
				w->opacity = newOpacity;

				if ( gameFocused && ( w == ctx->focus.overlayWindow || w == ctx->focus.notificationWindow ) )
				{
					hasRepaintNonBasePlane = true;
				}
				if ( w == ctx->focus.externalOverlayWindow )
				{
					hasRepaint = true;
				}
			}

			unsigned int maxOpacity = 0;
			unsigned int maxOpacityExternal = 0;

			for (w = ctx->list; w; w = w->xwayland().next)
			{
				if (w->isOverlay)
				{
					if (w->GetGeometry().nWidth > 1200 && w->opacity >= maxOpacity)
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
		steamcompmgr_win_t * w = find_win(ctx, ev->window);
		if (w)
		{
			w->isSteamLegacyBigPicture = get_prop(ctx, w->xwayland().id, ctx->atoms.steamAtom, 0);
			MakeFocusDirty();
		}
	}
	if (ev->atom == ctx->atoms.steamInputFocusAtom )
	{
		steamcompmgr_win_t * w = find_win(ctx, ev->window);
		if (w)
		{
			w->inputFocusMode = get_prop(ctx, w->xwayland().id, ctx->atoms.steamInputFocusAtom, 0);
			MakeFocusDirty();
		}
	}
	if (ev->atom == ctx->atoms.steamTouchClickModeAtom )
	{
		gamescope::cv_touch_click_mode = (gamescope::TouchClickMode) get_prop(ctx, ctx->root, ctx->atoms.steamTouchClickModeAtom, 0u );
	}
	if (ev->atom == ctx->atoms.steamStreamingClientAtom)
	{
		steamcompmgr_win_t * w = find_win(ctx, ev->window);
		if (w)
		{
			w->isSteamStreamingClient = get_prop(ctx, w->xwayland().id, ctx->atoms.steamStreamingClientAtom, 0);
			MakeFocusDirty();
		}
	}
	if (ev->atom == ctx->atoms.steamStreamingClientVideoAtom)
	{
		steamcompmgr_win_t * w = find_win(ctx, ev->window);
		if (w)
		{
			w->isSteamStreamingClientVideo = get_prop(ctx, w->xwayland().id, ctx->atoms.steamStreamingClientVideoAtom, 0);
			MakeFocusDirty();
		}
	}
	if (ev->atom == ctx->atoms.gamescopeCtrlAppIDAtom )
	{
		get_prop( ctx, ctx->root, ctx->atoms.gamescopeCtrlAppIDAtom, vecFocuscontrolAppIDs );
		MakeFocusDirty();
	}
	if (ev->atom == ctx->atoms.gamescopeCtrlWindowAtom )
	{
		ctx->focusControlWindow = get_prop( ctx, ctx->root, ctx->atoms.gamescopeCtrlWindowAtom, None );
		MakeFocusDirty();
	}
	if ( ev->atom == ctx->atoms.gamescopeScreenShotAtom )
	{
		if ( ev->state == PropertyNewValue )
		{
			gamescope::CScreenshotManager::Get().TakeScreenshot( gamescope::GamescopeScreenshotInfo
			{
				.szScreenshotPath = "/tmp/gamescope.png",
				.eScreenshotType = (gamescope_control_screenshot_type) get_prop( ctx, ctx->root, ctx->atoms.gamescopeScreenShotAtom, None ),
				.uScreenshotFlags = 0,
				.bX11PropertyRequested = true,
			} );
		}
	}
	if ( ev->atom == ctx->atoms.gamescopeDebugScreenShotAtom )
	{
		if ( ev->state == PropertyNewValue )
		{
			gamescope::CScreenshotManager::Get().TakeScreenshot( gamescope::GamescopeScreenshotInfo
			{
				.szScreenshotPath = "/tmp/gamescope.png",
				.eScreenshotType = (gamescope_control_screenshot_type) get_prop( ctx, ctx->root, ctx->atoms.gamescopeDebugScreenShotAtom, None ),
				.uScreenshotFlags = 0,
				.bX11PropertyRequested = true,
			} );
		}
	}
	if (ev->atom == ctx->atoms.gameAtom)
	{
		steamcompmgr_win_t * w = find_win(ctx, ev->window);
		if (w)
		{
			uint32_t appID = get_prop(ctx, w->xwayland().id, ctx->atoms.gameAtom, 0);

			if ( w->appID != 0 && appID != 0 && w->appID != appID )
			{
				xwm_log.errorf( "appid clash was %u now %u", w->appID, appID );
			}
			w->appID = appID;
			if ( w->isExternalOverlay )
				w->appID = 0;

			MakeFocusDirty();
		}
	}
	if (ev->atom == ctx->atoms.overlayAtom)
	{
		steamcompmgr_win_t * w = find_win(ctx, ev->window);
		if (w)
		{
			w->isOverlay = get_prop(ctx, w->xwayland().id, ctx->atoms.overlayAtom, 0);
			if ( w->isExternalOverlay )
				w->appID = 0;
			MakeFocusDirty();
		}
	}
	if (ev->atom == ctx->atoms.externalOverlayAtom)
	{
		steamcompmgr_win_t * w = find_win(ctx, ev->window);
		if (w)
		{
			w->isExternalOverlay = get_prop(ctx, w->xwayland().id, ctx->atoms.externalOverlayAtom, 0);
			if ( w->isExternalOverlay )
				w->appID = 0;
			MakeFocusDirty();
		}
	}
	if (ev->atom == ctx->atoms.winTypeAtom)
	{
		steamcompmgr_win_t * w = find_win(ctx, ev->window);
		if (w)
		{
			get_win_type(ctx, w);
			MakeFocusDirty();
		}		
	}
	if (ev->atom == ctx->atoms.sizeHintsAtom)
	{
		steamcompmgr_win_t * w = find_win(ctx, ev->window);
		if (w)
		{
			get_size_hints(ctx, w);
			MakeFocusDirty();
		}
	}
	if (ev->atom == ctx->atoms.gamesRunningAtom)
	{
		gamesRunningCount = get_prop(ctx, ctx->root, ctx->atoms.gamesRunningAtom, 0);

		MakeFocusDirty();
	}
	if (ev->atom == ctx->atoms.screenScaleAtom)
	{
		overscanScaleRatio = get_prop(ctx, ctx->root, ctx->atoms.screenScaleAtom, 0xFFFFFFFF) / (double)0xFFFFFFFF;

		globalScaleRatio = overscanScaleRatio * zoomScaleRatio;

		for ( auto &iter : g_VirtualConnectorFocuses )
		{
			global_focus_t *pFocus = &iter.second;
			if (pFocus->focusWindow)
			{
				hasRepaint = true;
			}
		}

		MakeFocusDirty();
	}
	if (ev->atom == ctx->atoms.screenZoomAtom)
	{
		zoomScaleRatio = get_prop(ctx, ctx->root, ctx->atoms.screenZoomAtom, 0xFFFF) / (double)0xFFFF;

		globalScaleRatio = overscanScaleRatio * zoomScaleRatio;

		for ( auto &iter : g_VirtualConnectorFocuses )
		{
			global_focus_t *pFocus = &iter.second;
			if (pFocus->focusWindow)
			{
				hasRepaint = true;
			}
		}

		MakeFocusDirty();
	}
	if (ev->atom == ctx->atoms.WMTransientForAtom)
	{
		steamcompmgr_win_t * w = find_win(ctx, ev->window);
		if (w)
		{
			Window transientFor = None;
			if ( XGetTransientForHint( ctx->dpy, ev->window, &transientFor ) )
			{
				w->xwayland().transientFor = transientFor;
			}
			else
			{
				w->xwayland().transientFor = None;
			}
			get_win_type( ctx, w );

			MakeFocusDirty();
		}
	}
	if (ev->atom == XA_WM_NAME || ev->atom == ctx->atoms.netWMNameAtom)
	{
		steamcompmgr_win_t *w = find_win(ctx, ev->window);

		if (w)
		{
			get_win_title(ctx, w, ev->atom);

			for ( auto &iter : g_VirtualConnectorFocuses )
			{
				global_focus_t *pFocus = &iter.second;
				if (ev->window == x11_win(pFocus->focusWindow))
				{
					if ( pFocus->GetNestedHints() )
						pFocus->GetNestedHints()->SetTitle( w->title );
				}
			}
		}
	}
	if (ev->atom == ctx->atoms.netWMIcon)
	{
		steamcompmgr_win_t *w = find_win(ctx, ev->window);

		if (w)
		{
			get_win_icon(ctx, w);

			for ( auto &iter : g_VirtualConnectorFocuses )
			{
				global_focus_t *pFocus = &iter.second;
				if (ev->window == x11_win(pFocus->focusWindow))
				{
					if ( pFocus->GetNestedHints() )
						pFocus->GetNestedHints()->SetIcon( w->icon );
				}
			}
		}
	}
#if 0
	if ( ev->atom == ctx->atoms.gamescopeTuneableVBlankRedZone )
	{
		g_uVblankDrawBufferRedZoneNS = (uint64_t)get_prop( ctx, ctx->root, ctx->atoms.gamescopeTuneableVBlankRedZone, g_uDefaultVBlankRedZone );
	}
	if ( ev->atom == ctx->atoms.gamescopeTuneableRateOfDecay )
	{
		g_uVBlankRateOfDecayPercentage = (uint64_t)get_prop( ctx, ctx->root, ctx->atoms.gamescopeTuneableRateOfDecay, g_uDefaultVBlankRateOfDecayPercentage );
	}
#endif
	if ( ev->atom == ctx->atoms.gamescopeScalingFilter )
	{
		int nScalingMode = get_prop( ctx, ctx->root, ctx->atoms.gamescopeScalingFilter, 0 );
		switch ( nScalingMode )
		{
		default:
		case 0:
			g_wantedUpscaleScaler = GamescopeUpscaleScaler::AUTO;
			g_wantedUpscaleFilter = GamescopeUpscaleFilter::LINEAR;
			break;
		case 1:
			g_wantedUpscaleScaler = GamescopeUpscaleScaler::AUTO;
			g_wantedUpscaleFilter = GamescopeUpscaleFilter::NEAREST;
			break;
		case 2:
			g_wantedUpscaleScaler = GamescopeUpscaleScaler::INTEGER;
			g_wantedUpscaleFilter = GamescopeUpscaleFilter::NEAREST;
			break;
		case 3:
			g_wantedUpscaleScaler = GamescopeUpscaleScaler::AUTO;
			g_wantedUpscaleFilter = GamescopeUpscaleFilter::FSR;
			break;
		case 4:
			g_wantedUpscaleScaler = GamescopeUpscaleScaler::AUTO;
			g_wantedUpscaleFilter = GamescopeUpscaleFilter::NIS;
			break;
		}
		hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeFSRSharpness || ev->atom == ctx->atoms.gamescopeSharpness )
	{
		g_upscaleFilterSharpness = (int)clamp( get_prop( ctx, ctx->root, ev->atom, 2 ), 0u, 20u );
		if ( g_upscaleFilter == GamescopeUpscaleFilter::FSR || g_upscaleFilter == GamescopeUpscaleFilter::NIS )
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
	for (int i = 0; i < gamescope::GAMESCOPE_SCREEN_TYPE_COUNT; i++)
	{
		if ( ev->atom == ctx->atoms.gamescopeDynamicRefresh[i] )
		{
			g_nDynamicRefreshRate[i] = get_prop( ctx, ctx->root, ctx->atoms.gamescopeDynamicRefresh[i], 0 );
		}
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
	if ( ev->atom == ctx->atoms.gamescopeCompositeForce )
	{
		cv_composite_force = !!get_prop( ctx, ctx->root, ctx->atoms.gamescopeCompositeForce, 0 );
		hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeCompositeDebug )
	{
		cv_composite_debug = get_prop( ctx, ctx->root, ctx->atoms.gamescopeCompositeDebug, 0 );

		hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeAllowTearing )
	{
		cv_tearing_enabled = !!get_prop( ctx, ctx->root, ctx->atoms.gamescopeAllowTearing, 0 );
	}
	if ( ev->atom == ctx->atoms.gamescopeSteamMaxHeight )
	{
		g_nSteamMaxHeight = get_prop( ctx, ctx->root, ctx->atoms.gamescopeSteamMaxHeight, 0 );
		MakeFocusDirty();
	}
	if ( ev->atom == ctx->atoms.gamescopeVRREnabled )
	{
		bool enabled = !!get_prop( ctx, ctx->root, ctx->atoms.gamescopeVRREnabled, 0 );
		cv_adaptive_sync = enabled;
	}
	if ( ev->atom == ctx->atoms.gamescopeDisplayForceInternal )
	{
		g_bForceInternal = !!get_prop( ctx, ctx->root, ctx->atoms.gamescopeDisplayForceInternal, 0 );
		GetBackend()->DirtyState();
	}
	if ( ev->atom == ctx->atoms.gamescopeDisplayModeNudge )
	{
		GetBackend()->DirtyState( true );
		XDeleteProperty( ctx->dpy, ctx->root, ctx->atoms.gamescopeDisplayModeNudge );
	}
	if ( ev->atom == ctx->atoms.gamescopeNewScalingFilter )
	{
		GamescopeUpscaleFilter nScalingFilter = ( GamescopeUpscaleFilter ) get_prop( ctx, ctx->root, ctx->atoms.gamescopeNewScalingFilter, 0 );
		if (g_wantedUpscaleFilter != nScalingFilter)
		{
			g_wantedUpscaleFilter = nScalingFilter;
			hasRepaint = true;
		}
	}
	if ( ev->atom == ctx->atoms.gamescopeNewScalingScaler )
	{
		GamescopeUpscaleScaler nScalingScaler = ( GamescopeUpscaleScaler ) get_prop( ctx, ctx->root, ctx->atoms.gamescopeNewScalingScaler, 0 );
		if (g_wantedUpscaleScaler != nScalingScaler)
		{
			g_wantedUpscaleScaler = nScalingScaler;
			hasRepaint = true;
		}
	}
	if ( ev->atom == ctx->atoms.gamescopeDisplayHDREnabled )
	{
		cv_hdr_enabled = !!get_prop( ctx, ctx->root, ctx->atoms.gamescopeDisplayHDREnabled, 0 );
		hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeDebugForceHDR10Output )
	{
		g_bForceHDR10OutputDebug = !!get_prop( ctx, ctx->root, ctx->atoms.gamescopeDebugForceHDR10Output, 0 );
		hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeDebugForceHDRSupport )
	{
		g_bForceHDRSupportDebug = !!get_prop( ctx, ctx->root, ctx->atoms.gamescopeDebugForceHDRSupport, 0 );
		GetBackend()->HackUpdatePatchedEdid();
		hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeDebugHDRHeatmap )
	{
		uint32_t heatmap = get_prop( ctx, ctx->root, ctx->atoms.gamescopeDebugHDRHeatmap, 0 );
		cv_composite_debug &= ~CompositeDebugFlag::Heatmap;
		cv_composite_debug &= ~CompositeDebugFlag::Heatmap_MSWCG;
		cv_composite_debug &= ~CompositeDebugFlag::Heatmap_Hard;
		if (heatmap != 0)
			cv_composite_debug |= CompositeDebugFlag::Heatmap;
		if (heatmap == 2)
			cv_composite_debug |= CompositeDebugFlag::Heatmap_MSWCG;
		if (heatmap == 3)
			cv_composite_debug |= CompositeDebugFlag::Heatmap_Hard;
		hasRepaint = true;
	}

	if ( ev->atom == ctx->atoms.gamescopeHDRTonemapOperator )
	{
		g_ColorMgmt.pending.hdrTonemapOperator = (ETonemapOperator) get_prop( ctx, ctx->root, ctx->atoms.gamescopeHDRTonemapOperator, 0 );
		hasRepaint = true;
	}

	if ( ev->atom == ctx->atoms.gamescopeHDRTonemapDisplayMetadata )
	{
		std::vector< uint32_t > user_vec;
		if ( get_prop( ctx, ctx->root, ctx->atoms.gamescopeHDRTonemapDisplayMetadata, user_vec ) && user_vec.size() >= 2 )
		{
			g_ColorMgmt.pending.hdrTonemapDisplayMetadata.flBlackPointNits = bit_cast<float>( user_vec[0] );
			g_ColorMgmt.pending.hdrTonemapDisplayMetadata.flWhitePointNits = bit_cast<float>( user_vec[1] );
		}
		else
		{
			g_ColorMgmt.pending.hdrTonemapDisplayMetadata.reset();
		}
		hasRepaint = true;
	}

	if ( ev->atom == ctx->atoms.gamescopeHDRTonemapSourceMetadata )
	{
		std::vector< uint32_t > user_vec;
		if ( get_prop( ctx, ctx->root, ctx->atoms.gamescopeHDRTonemapSourceMetadata, user_vec ) && user_vec.size() >= 2 )
		{
			g_ColorMgmt.pending.hdrTonemapSourceMetadata.flBlackPointNits = bit_cast<float>( user_vec[0] );
			g_ColorMgmt.pending.hdrTonemapSourceMetadata.flWhitePointNits = bit_cast<float>( user_vec[1] );
		}
		else
		{
			g_ColorMgmt.pending.hdrTonemapSourceMetadata.reset();
		}
		hasRepaint = true;
	}

	if ( ev->atom == ctx->atoms.gamescopeSDROnHDRContentBrightness )
	{
		uint32_t val = get_prop( ctx, ctx->root, ctx->atoms.gamescopeSDROnHDRContentBrightness, 0 );
		if ( set_sdr_on_hdr_brightness( bit_cast<float>(val) ) )
			hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeHDRItmEnable )
	{
		g_bHDRItmEnable = !!get_prop( ctx, ctx->root, ctx->atoms.gamescopeHDRItmEnable, 0 );
		hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeHDRItmSDRNits )
	{
		g_flHDRItmSdrNits = get_prop( ctx, ctx->root, ctx->atoms.gamescopeHDRItmSDRNits, 0 );
		if ( g_flHDRItmSdrNits < 1.f )
			g_flHDRItmSdrNits = 100.f;
		else if ( g_flHDRItmSdrNits > 1000.f)
			g_flHDRItmSdrNits = 1000.f;
		hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeHDRItmTargetNits )
	{
		g_flHDRItmTargetNits = get_prop( ctx, ctx->root, ctx->atoms.gamescopeHDRItmTargetNits, 0 );
		if ( g_flHDRItmTargetNits < 1.f )
			g_flHDRItmTargetNits = 1000.f;
		else if ( g_flHDRItmTargetNits > 10000.f)
			g_flHDRItmTargetNits = 10000.f;
		hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeColorLookPQ )
	{
		std::string path = get_string_prop( ctx, ctx->root, ctx->atoms.gamescopeColorLookPQ );
		if ( set_color_look_pq( path.c_str() ) )
			hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeColorLookG22 )
	{
		std::string path = get_string_prop( ctx, ctx->root, ctx->atoms.gamescopeColorLookG22 );
		if ( set_color_look_g22( path.c_str() ) )
			hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeColorOutputVirtualWhite )
	{
		std::vector< uint32_t > user_vec;
		if ( get_prop( ctx, ctx->root, ctx->atoms.gamescopeColorOutputVirtualWhite, user_vec ) && user_vec.size() >= 2 )
		{
			g_ColorMgmt.pending.outputVirtualWhite.x = santitize_float( bit_cast<float>( user_vec[0] ) );
			g_ColorMgmt.pending.outputVirtualWhite.y = santitize_float( bit_cast<float>( user_vec[1] ) );
		}
		else
		{
			g_ColorMgmt.pending.outputVirtualWhite.x = 0.f;
			g_ColorMgmt.pending.outputVirtualWhite.y = 0.f;
		}
		hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeHDRInputGain )
	{
		uint32_t val = get_prop( ctx, ctx->root, ctx->atoms.gamescopeHDRInputGain, 0 );
		if ( set_hdr_input_gain( bit_cast<float>(val) ) )
			hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeSDRInputGain )
	{
		uint32_t val = get_prop( ctx, ctx->root, ctx->atoms.gamescopeSDRInputGain, 0 );
		if ( set_sdr_input_gain( bit_cast<float>(val) ) )
			hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeForceWindowsFullscreen )
	{
		ctx->force_windows_fullscreen = !!get_prop( ctx, ctx->root, ctx->atoms.gamescopeForceWindowsFullscreen, 0 );
		MakeFocusDirty();
	}
	if ( ev->atom == ctx->atoms.gamescopeColorLut3DOverride )
	{
		std::string path = get_string_prop( ctx, ctx->root, ctx->atoms.gamescopeColorLut3DOverride );
		if ( set_color_3dlut_override( path.c_str() ) )
			hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeColorShaperLutOverride )
	{
		std::string path = get_string_prop( ctx, ctx->root, ctx->atoms.gamescopeColorShaperLutOverride );
		if ( set_color_shaperlut_override( path.c_str() ) )
			hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeColorSDRGamutWideness )
	{
		uint32_t val = get_prop(ctx, ctx->root, ctx->atoms.gamescopeColorSDRGamutWideness, 0);
		if ( set_color_sdr_gamut_wideness( bit_cast<float>(val) ) )
			hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeColorNightMode )
	{
		std::vector< uint32_t > user_vec;
		bool bHasVec = get_prop( ctx, ctx->root, ctx->atoms.gamescopeColorNightMode, user_vec );

		// identity
		float vec[3] = { 0.0f, 0.0f, 0.0f };
		if ( bHasVec && user_vec.size() == 3 )
		{
			for (int i = 0; i < 3; i++)
				vec[i] = bit_cast<float>( user_vec[i] );
		}

		nightmode_t nightmode;
		nightmode.amount = vec[0];
		nightmode.hue = vec[1];
		nightmode.saturation = vec[2];

		if ( set_color_nightmode( nightmode ) )
			hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeColorManagementDisable )
	{
		uint32_t val = get_prop(ctx, ctx->root, ctx->atoms.gamescopeColorManagementDisable, 0);
		if ( set_color_mgmt_enabled( !val ) )
			hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeColorSliderInUse )
	{
		uint32_t val = get_prop(ctx, ctx->root, ctx->atoms.gamescopeColorSliderInUse, 0);
		g_bColorSliderInUse = !!val;
	}
	if ( ev->atom == ctx->atoms.gamescopeColorChromaticAdaptationMode )
	{
		uint32_t val = get_prop(ctx, ctx->root, ctx->atoms.gamescopeColorChromaticAdaptationMode, 0);
		g_ColorMgmt.pending.chromaticAdaptationMode = ( EChromaticAdaptationMethod ) val;
	}
	// TODO: Hook up gamescopeColorMuraCorrectionImage for external.
	if ( ev->atom == ctx->atoms.gamescopeColorMuraCorrectionImage[gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL] )
	{
		std::string path = get_string_prop( ctx, ctx->root, ctx->atoms.gamescopeColorMuraCorrectionImage[gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL] );
		if ( set_mura_overlay( path.c_str() ) )
			hasRepaint = true;
	}
	// TODO: Hook up gamescopeColorMuraScale for external.
	if ( ev->atom == ctx->atoms.gamescopeColorMuraScale[gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL] )
	{
		uint32_t val = get_prop(ctx, ctx->root, ctx->atoms.gamescopeColorMuraScale[gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL], 0);
		float new_scale = bit_cast<float>(val);
		if ( set_mura_scale( new_scale ) )
			hasRepaint = true;
	}
	// TODO: Hook up gamescopeColorMuraCorrectionDisabled for external.
	if ( ev->atom == ctx->atoms.gamescopeColorMuraCorrectionDisabled[gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL] )
	{
		bool disabled = !!get_prop(ctx, ctx->root, ctx->atoms.gamescopeColorMuraCorrectionDisabled[gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL], 0);
		if ( g_bMuraCompensationDisabled != disabled ) {
			g_bMuraCompensationDisabled = disabled;
			hasRepaint = true;
		}
	}
	if (ev->atom == ctx->atoms.gamescopeCreateXWaylandServer)
	{
		uint32_t identifier = get_prop(ctx, ctx->root, ctx->atoms.gamescopeCreateXWaylandServer, 0);
		if (identifier)
		{
			wlserver_lock();
			uint32_t server_id = (uint32_t)wlserver_make_new_xwayland_server();
			assert(server_id != ~0u);
			gamescope_xwayland_server_t *server = wlserver_get_xwayland_server(server_id);
			init_xwayland_ctx(server_id, server);
			char propertyString[256];
			snprintf(propertyString, sizeof(propertyString), "%u %u %s", identifier, server_id, server->get_nested_display_name());
			XTextProperty text_property =
			{
				.value = (unsigned char *)propertyString,
				.encoding = ctx->atoms.utf8StringAtom,
				.format = 8,
				.nitems = strlen(propertyString),
			};
			g_SteamCompMgrWaiter.AddWaitable( server->ctx.get() );
			XSetTextProperty( ctx->dpy, ctx->root, &text_property, ctx->atoms.gamescopeCreateXWaylandServerFeedback );
			wlserver_unlock();
		}
	}
	if (ev->atom == ctx->atoms.gamescopeDestroyXWaylandServer)
	{
		uint32_t server_id = get_prop(ctx, ctx->root, ctx->atoms.gamescopeDestroyXWaylandServer, 0);

		gamescope_xwayland_server_t *server = wlserver_get_xwayland_server(server_id);
		if (server)
		{
			for ( auto &iter : g_VirtualConnectorFocuses )
			{
				global_focus_t *pFocus = &iter.second;
				if (pFocus->focusWindow &&
					pFocus->focusWindow->type == steamcompmgr_win_type_t::XWAYLAND &&
					pFocus->focusWindow->xwayland().ctx == server->ctx.get())
					pFocus->focusWindow = nullptr;

				if (pFocus->inputFocusWindow &&
					pFocus->inputFocusWindow->type == steamcompmgr_win_type_t::XWAYLAND &&
					pFocus->inputFocusWindow->xwayland().ctx == server->ctx.get())
					pFocus->inputFocusWindow = nullptr;

				if (pFocus->overlayWindow &&
					pFocus->overlayWindow->type == steamcompmgr_win_type_t::XWAYLAND &&
					pFocus->overlayWindow->xwayland().ctx == server->ctx.get())
					pFocus->overlayWindow = nullptr;

				if (pFocus->externalOverlayWindow &&
					pFocus->externalOverlayWindow->type == steamcompmgr_win_type_t::XWAYLAND &&
					pFocus->externalOverlayWindow->xwayland().ctx == server->ctx.get())
					pFocus->externalOverlayWindow = nullptr;

				if (pFocus->notificationWindow &&
					pFocus->notificationWindow->type == steamcompmgr_win_type_t::XWAYLAND &&
					pFocus->notificationWindow->xwayland().ctx == server->ctx.get())
					pFocus->notificationWindow = nullptr;

				if (pFocus->overrideWindow &&
					pFocus->overrideWindow->type == steamcompmgr_win_type_t::XWAYLAND &&
					pFocus->overrideWindow->xwayland().ctx == server->ctx.get())
					pFocus->overrideWindow = nullptr;

				if (pFocus->keyboardFocusWindow &&
					pFocus->keyboardFocusWindow->type == steamcompmgr_win_type_t::XWAYLAND &&
					pFocus->keyboardFocusWindow->xwayland().ctx == server->ctx.get())
					pFocus->keyboardFocusWindow = nullptr;

				if (pFocus->fadeWindow &&
					pFocus->fadeWindow->type == steamcompmgr_win_type_t::XWAYLAND &&
					pFocus->fadeWindow->xwayland().ctx == server->ctx.get())
					pFocus->fadeWindow = nullptr;

				if (pFocus->cursor &&
					pFocus->cursor->getCtx() == server->ctx.get())
					pFocus->cursor = nullptr;
			}

			wlserver_lock();
			g_SteamCompMgrWaiter.RemoveWaitable( server->ctx.get() );
			wlserver_destroy_xwayland_server(server);
			wlserver_unlock();

			MakeFocusDirty();
		}
	}
	if (ev->atom == ctx->atoms.gamescopeReshadeTechniqueIdx)
	{
		uint32_t technique_idx = get_prop(ctx, ctx->root, ctx->atoms.gamescopeReshadeTechniqueIdx, 0);
		g_reshade_technique_idx = technique_idx;
	}
	if (ev->atom == ctx->atoms.gamescopeReshadeEffect)
	{
		std::string path = get_string_prop( ctx, ctx->root, ctx->atoms.gamescopeReshadeEffect );
		g_reshade_effect = path;
	}
	if (ev->atom == ctx->atoms.gamescopeDisplayDynamicRefreshBasedOnGamePresence)
	{
		g_bChangeDynamicRefreshBasedOnGameOpenRatherThanActive = !!get_prop(ctx, ctx->root, ctx->atoms.gamescopeDisplayDynamicRefreshBasedOnGamePresence, 0);
	}
	if (ev->atom == ctx->atoms.wineHwndStyle)
	{
		steamcompmgr_win_t * w = find_win(ctx, ev->window);
		if (w)
		{
			w->hasHwndStyle = true;
			w->hwndStyle = get_prop(ctx, w->xwayland().id, ctx->atoms.wineHwndStyle, 0);
			MakeFocusDirty();
		}
	}
	if (ev->atom == ctx->atoms.wineHwndStyleEx)
	{
		steamcompmgr_win_t * w = find_win(ctx, ev->window);
		if (w)
		{
			w->hasHwndStyleEx = true;
			w->hwndStyleEx = get_prop(ctx, w->xwayland().id, ctx->atoms.wineHwndStyleEx, 0);
			MakeFocusDirty();
		}
	}
}

static int
error(Display *dpy, XErrorEvent *ev)
{
	// Do nothing. XErrors are usually benign.
	return 0;
}

static void
steamcompmgr_exit(void)
{
	g_ImageWaiter.Shutdown();

	// Clean up any commits.
	{
		gamescope_xwayland_server_t *server = NULL;
		for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
		{
			for ( steamcompmgr_win_t *w = server->ctx->list; w; w = w->xwayland().next )
				w->commit_queue.clear();
		}
	}
	g_steamcompmgr_xdg_wins.clear();
	g_HeldCommits[ HELD_COMMIT_BASE ] = nullptr;
	g_HeldCommits[ HELD_COMMIT_FADE ] = nullptr;

	for ( auto &lut : g_ColorMgmtLuts ) lut.shutdown();
	for ( auto &lut : g_ColorMgmtLutsOverride ) lut.shutdown();
	for ( auto &lut : g_ScreenshotColorMgmtLuts ) lut.shutdown();
	for ( auto &lut : g_ScreenshotColorMgmtLutsHDR ) lut.shutdown();

	if ( statsThreadRun == true )
	{
		statsThreadRun = false;
		statsThreadSem.signal();
	}

	{
		g_ColorMgmt.pending.appHDRMetadata = nullptr;
		g_ColorMgmt.current.appHDRMetadata = nullptr;

		s_scRGB709To2020Matrix = nullptr;
		for (int i = 0; i < gamescope::GAMESCOPE_SCREEN_TYPE_COUNT; i++)
		{
			s_MuraCorrectionImage[i] = nullptr;
			s_MuraCTMBlob[i] = nullptr;
		}
	}

	g_VirtualConnectorFocuses.clear();

    gamescope::IBackend::Set( nullptr );

    wlserver_lock();
    wlserver_shutdown();
    wlserver_unlock(false);
}

[[noreturn]] static int
handle_io_error(Display *dpy)
{
	xwm_log.errorf("X11 I/O error");
	steamcompmgr_exit();

	g_SteamCompMgrXWaylandServerMutex.unlock();
	pthread_exit(NULL);
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

bool handle_done_commit( steamcompmgr_win_t *w, xwayland_ctx_t *ctx, uint64_t commitID, uint64_t earliestPresentTime, uint64_t earliestLatchTime )
{
	bool bFoundWindow = false;
	uint32_t j;
	for ( j = 0; j < w->commit_queue.size(); j++ )
	{
		if (w->commit_queue[ j ]->feedback.has_value())
			w->engineName = w->commit_queue[ j ]->feedback->vk_engine_name;

		if ( w->commit_queue[ j ]->commitID == commitID )
		{
			gpuvis_trace_printf( "commit %lu done", w->commit_queue[ j ]->commitID );
			w->commit_queue[ j ]->done = true;
			w->commit_queue[ j ]->earliest_present_time = earliestPresentTime;
			w->commit_queue[ j ]->present_margin = earliestPresentTime - earliestLatchTime;
			bFoundWindow = true;

			// Window just got a new available commit, determine if that's worth a repaint

			for ( auto &iter : g_VirtualConnectorFocuses )
			{
				global_focus_t *pFocus = &iter.second;

				// If this is an overlay that we're presenting, repaint
				if ( w == pFocus->overlayWindow && w->opacity != TRANSLUCENT )
				{
					hasRepaintNonBasePlane = true;
				}

				if ( w == pFocus->notificationWindow && w->opacity != TRANSLUCENT )
				{
					hasRepaintNonBasePlane = true;
				}

				// matt: the performance overlay in Steam will interfere with VRR if we let this repaint.
				// This has been broken since the logic for external overlay repaints was moved out of
				// outdatedInteractiveFocus. It can cause displays to jump between the focused app's
				// refresh rate and the maximum panel refresh rate when presenting any type of overlay,
				// creating noticeable VRR flicker in the process.
				// TODO: fix this properly for all overlays, including Steam notifications and QAM
				// HACK: If VRR is active, prevent external overlays, i.e. mangoapp, from repainting the base plane
				if ( ( w == pFocus->externalOverlayWindow && w->opacity != TRANSLUCENT ) &&
				     ( GetBackend()->GetCurrentConnector() && !GetBackend()->GetCurrentConnector()->IsVRRActive() ) )
				{
					hasRepaintNonBasePlane = true;
				}

				// If this is the main plane, repaint
				if ( w == pFocus->focusWindow && !w->isSteamStreamingClient )
				{
					if ( !cv_paint_debug_pause_base_plane )
						g_HeldCommits[ HELD_COMMIT_BASE ] = w->commit_queue[ j ];
					hasRepaint = true;

					focusWindow_engine = w->engineName;
				}

				if ( w == pFocus->overrideWindow )
				{
					hasRepaintNonBasePlane = true;
				}

				if ( w->isSteamStreamingClientVideo && pFocus->focusWindow && pFocus->focusWindow->isSteamStreamingClient )
				{
					if ( !cv_paint_debug_pause_base_plane )
						g_HeldCommits[ HELD_COMMIT_BASE ] = w->commit_queue[ j ];
					hasRepaint = true;
				}
			}

			if ( w->outdatedInteractiveFocus )
			{
				MakeFocusDirty();
				w->outdatedInteractiveFocus = false;
			}

			break;
		}
	}

	if ( bFoundWindow == true )
	{
		if ( j > 0 )
			w->commit_queue.erase( w->commit_queue.begin(), w->commit_queue.begin() + j );
		w->receivedDoneCommit = true;
		return true;
	}

	return false;
}

// TODO: Merge these two functions.
void handle_done_commits_xwayland( xwayland_ctx_t *ctx, bool vblank, uint64_t vblank_idx )
{
	std::lock_guard<std::mutex> lock( ctx->doneCommits.listCommitsDoneLock );

	uint64_t next_refresh_time = g_SteamCompMgrVBlankTime.schedule.ulTargetVBlank;

	// commits that were not ready to be presented based on their display timing.
	static std::vector< CommitDoneEntry_t > commits_before_their_time;
	commits_before_their_time.clear();
	commits_before_their_time.reserve( 32 );

	// windows in FIFO mode we got a new frame to present for this vblank
	static std::unordered_set< uint64_t > fifo_win_seqs;
	fifo_win_seqs.clear();
	fifo_win_seqs.reserve( 32 );

	uint64_t now = get_time_in_nanos();

	// very fast loop yes
	for ( auto& entry : ctx->doneCommits.listCommitsDone )
	{
		bool entry_vblank = vblank;

		if ( GetBackend()->GetCurrentConnector() && GetBackend()->GetCurrentConnector()->IsVRRActive() )
		{
			for ( steamcompmgr_win_t *w = ctx->list; w; w = w->xwayland().next )
			{
				if (w->seq != entry.winSeq)
					continue;

				entry_vblank = entry_vblank && steamcompmgr_should_vblank_window( true, vblank_idx, w, now );
			}
		}
		else
		{
			entry_vblank = entry_vblank && steamcompmgr_should_vblank_window( true, vblank_idx );
		}

		if (entry.fifo && (!entry_vblank || fifo_win_seqs.count(entry.winSeq) > 0))
		{
			commits_before_their_time.push_back( entry );
			continue;
		}

		if (!entry.earliestPresentTime)
		{
			entry.earliestPresentTime = next_refresh_time;
			entry.earliestLatchTime = now;
		}

		if ( entry.desiredPresentTime > next_refresh_time )
		{
			commits_before_their_time.push_back( entry );
			continue;
		}

		for ( steamcompmgr_win_t *w = ctx->list; w; w = w->xwayland().next )
		{
			if (w->seq != entry.winSeq)
				continue;
			if (handle_done_commit(w, ctx, entry.commitID, entry.earliestPresentTime, entry.earliestLatchTime))
			{
				if (entry.fifo)
					fifo_win_seqs.insert(entry.winSeq);
				break;
			}
		}
	}

	ctx->doneCommits.listCommitsDone.swap( commits_before_their_time );
}

void handle_done_commits_xdg( bool vblank, uint64_t vblank_idx )
{
	std::lock_guard<std::mutex> lock( g_steamcompmgr_xdg_done_commits.listCommitsDoneLock );

	uint64_t next_refresh_time = g_SteamCompMgrVBlankTime.schedule.ulTargetVBlank;

	// commits that were not ready to be presented based on their display timing.
	static std::vector< CommitDoneEntry_t > commits_before_their_time;
	commits_before_their_time.clear();
	commits_before_their_time.reserve( 32 );

	// windows in FIFO mode we got a new frame to present for this vblank
	static std::unordered_set< uint64_t > fifo_win_seqs;
	fifo_win_seqs.clear();
	fifo_win_seqs.reserve( 32 );

	uint64_t now = get_time_in_nanos();

	vblank = vblank && steamcompmgr_should_vblank_window( true, vblank_idx );

	// very fast loop yes
	for ( auto& entry : g_steamcompmgr_xdg_done_commits.listCommitsDone )
	{
		if (entry.fifo && (!vblank || fifo_win_seqs.count(entry.winSeq) > 0))
		{
			commits_before_their_time.push_back( entry );
			continue;
		}
		
		if (!entry.earliestPresentTime)
		{
			entry.earliestPresentTime = next_refresh_time;
			entry.earliestLatchTime = now;
		}

		if ( entry.desiredPresentTime > next_refresh_time )
		{
			commits_before_their_time.push_back( entry );
			break;
		}

		for (const auto& xdg_win : g_steamcompmgr_xdg_wins)
		{
			if (xdg_win->seq != entry.winSeq)
				continue;
			if (handle_done_commit(xdg_win.get(), nullptr, entry.commitID, entry.earliestPresentTime, entry.earliestLatchTime))
			{
				if (entry.fifo)
					fifo_win_seqs.insert(entry.winSeq);
				break;
			}
		}
	}

	g_steamcompmgr_xdg_done_commits.listCommitsDone.swap( commits_before_their_time );
}

void handle_presented_for_window( steamcompmgr_win_t* w )
{
	// wlserver_lock is held.

	uint64_t next_refresh_time = g_SteamCompMgrVBlankTime.schedule.ulTargetVBlank;

	uint64_t refresh_cycle = g_nSteamCompMgrTargetFPS && steamcompmgr_window_should_limit_fps( w )
		? g_SteamCompMgrLimitedAppRefreshCycle
		: g_SteamCompMgrAppRefreshCycle;

	commit_t *lastCommit = get_window_last_done_commit_peek(w);
	if (lastCommit)
	{
		// We might present the same commit multiple times. In these cases
		// there will be no frametime delta as the last frame was just re-used.
		uint64_t frametime_ns = lastCommit->present_time - w->last_commit_present_time;
		if ( frametime_ns > 0 )
		{
			if ( w->appID > 0 )
				wlserver_app_presented( w->appID, frametime_ns );

			w->last_commit_present_time = lastCommit->present_time;
		}

		if (!lastCommit->presentation_feedbacks.empty() || lastCommit->present_id)
		{
			if (!lastCommit->presentation_feedbacks.empty())
			{
				wlserver_presentation_feedback_presented(
					lastCommit->surf,
					lastCommit->presentation_feedbacks,
					next_refresh_time,
					refresh_cycle);
			}

			if (lastCommit->present_id)
			{
				wlserver_past_present_timing(
					lastCommit->surf,
					*lastCommit->present_id,
					lastCommit->desired_present_time,
					next_refresh_time,
					lastCommit->earliest_present_time,
					lastCommit->present_margin);
				lastCommit->present_id = std::nullopt;
			}
		}
	}

	if (struct wlr_surface *surface = w->current_surface())
	{
		auto info = get_wl_surface_info(surface);
		if (info != nullptr && info->last_refresh_cycle != refresh_cycle)
		{
			// Could have got the override set in this bubble.
			surface = w->current_surface();

			if  (info->last_refresh_cycle != refresh_cycle)
			{
				info->last_refresh_cycle = refresh_cycle;
				wlserver_refresh_cycle(surface, refresh_cycle);
			}
		}
	}
}

void handle_presented_xwayland( xwayland_ctx_t *ctx )
{
	for ( steamcompmgr_win_t *w = ctx->list; w; w = w->xwayland().next )
	{
		handle_presented_for_window(w);
	}
}

void handle_presented_xdg()
{
	for (const auto& xdg_win : g_steamcompmgr_xdg_wins)
	{
		handle_presented_for_window(xdg_win.get());
	}
}

void nudge_steamcompmgr( void )
{
	g_SteamCompMgrWaiter.Nudge();
}

void force_repaint( void )
{
	g_bForceRepaint = true;
	nudge_steamcompmgr();
}

struct TempUpscaleImage_t
{
	gamescope::OwningRc<CVulkanTexture> pTexture;
	// Timeline of upscale -> release, to be used as acquire for the commit.
	std::shared_ptr<gamescope::CTimeline> pReleaseTimeline;
	uint64_t ulLastPoint = 0ul;
};

static std::vector<TempUpscaleImage_t> g_pUpscaleImages;
void ClearUpscaleImages()
{
	g_pUpscaleImages.clear();
}

static TempUpscaleImage_t *GetTempUpscaleImage( uint32_t uWidth, uint32_t uHeight, uint32_t uDrmFormat )
{
	if ( g_pUpscaleImages.size() )
	{
		// Mixing and matching sizes to only do the min required would be nice
		// but massively complicates caching.
		if ( g_pUpscaleImages[0].pTexture->width() != uWidth ||
			 g_pUpscaleImages[0].pTexture->height() != uHeight ||
			 g_pUpscaleImages[0].pTexture->drmFormat() != uDrmFormat )
		{
			g_pUpscaleImages.clear();
		}
	}

	for ( TempUpscaleImage_t &image : g_pUpscaleImages )
	{
		if ( !image.pTexture->IsInUse() )
			return &image;
	}

	if ( g_pUpscaleImages.size() > 8 )
	{
		xwm_log.warnf( "No upscale images free!\n" );
		return {};
	}

	gamescope::OwningRc<CVulkanTexture> pTexture = new CVulkanTexture();

	std::shared_ptr<gamescope::CTimeline> pTimeline = gamescope::CTimeline::Create();
	if ( !pTimeline )
		return nullptr;

	CVulkanTexture::createFlags imageFlags;
	imageFlags.bSampled = true;
	imageFlags.bStorage = true;
	imageFlags.bFlippable = true;
	pTexture->BInit( g_nOutputWidth, g_nOutputHeight, 1, uDrmFormat, imageFlags );
	TempUpscaleImage_t &image = g_pUpscaleImages.emplace_back( std::move( pTexture ), std::move( pTimeline ) );

	return &image;
}

gamescope::ConVar<bool> cv_surface_update_force_only_current_surface( "surface_update_force_only_current_surface", false, "Force updates to apply only to the current surface, ignoring commits for other surfaces." );

void update_wayland_res(CommitDoneList_t *doneCommits, steamcompmgr_win_t *w, ResListEntry_t& reslistentry)
{
	struct wlr_buffer *buf = reslistentry.buf;

	if ( w == nullptr )
	{
		wlserver_lock();
		wlr_buffer_unlock( buf );
		wlserver_unlock();

		// Make sure to send the discarded event if we hit this
		// to ensure forward progress.
		if (!reslistentry.presentation_feedbacks.empty())
		{
			wlserver_presentation_feedback_discard( reslistentry.surf, reslistentry.presentation_feedbacks );
			// presentation_feedbacks cleared by wlserver_presentation_feedback_discard
		}

		xwm_log.errorf( "waylandres but no win" );
		return;
	}

	// If we ever use HDR on the surface, only ever accept flip commits from the WSI layer.
	if ( reslistentry.feedback && reslistentry.feedback->vk_colorspace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR )
	{
		w->bHasHadNonSRGBColorSpace = true;
	}

	// If there are random commits that are really thin/small when we have the WSI layer active ever,
	// let's just ignore these as they are probably bogus commits from glamor.
	bool bPossiblyBogus = reslistentry.buf->width <= 2 || reslistentry.buf->height <= 2;

	// If the buffer has no damage, always prefer our override surface.
	bool bHasDamage = ( reslistentry.surf->buffer_damage.extents.x2 - reslistentry.surf->buffer_damage.extents.x1 ) > 2 &&
					  ( reslistentry.surf->buffer_damage.extents.y2 - reslistentry.surf->buffer_damage.extents.y1 ) > 2;

	// If we have an override surface, make sure this commit is for the current surface
	// or if the commit is probably bogus.
	bool bOnlyCurrentSurface = w->bHasHadNonSRGBColorSpace || bPossiblyBogus || !bHasDamage || cv_surface_update_force_only_current_surface;

	bool for_current_surface = !w->override_surface() || w->current_surface() == reslistentry.surf;

	if ( !for_current_surface )
	{
		xwm_log.debugf( "Got commit not for current surface." );
	}

	if ( bOnlyCurrentSurface && !for_current_surface )
	{
		wlserver_lock();
		wlr_buffer_unlock( buf );
		wlserver_unlock();
		w->receivedDoneCommit = true;
		return;
	}

	bool already_exists = false;
	for ( const auto& existing_commit : w->commit_queue )
	{
		if (existing_commit->buf == buf)
			already_exists = true;
	}

	if ( already_exists && !reslistentry.feedback && reslistentry.presentation_feedbacks.empty() )
	{
		wlserver_lock();
		wlr_buffer_unlock( buf );
		wlserver_unlock();
		xwm_log.warnf( "got the same buffer committed twice, ignoring." );

		// If we have a duplicated commit + frame callback, ensure that is signalled.
		// This matches Mutter and Weston behavior, so it's plausible that some application relies on forward progress.
		// We're essentially discarding the commit here, so consider it complete right away.
		w->receivedDoneCommit = true;
		return;
	}

	gamescope::Rc<commit_t> newCommit = import_commit(
		w,
		reslistentry.surf,
		buf,
		reslistentry.async,
		std::move(reslistentry.feedback),
		std::move(reslistentry.presentation_feedbacks),
		reslistentry.present_id,
		reslistentry.desired_present_time,
		reslistentry.fifo );

	int fence = -1;
	if ( newCommit != nullptr )
	{
		global_focus_t *pCurrentFocus = GetCurrentFocus();

		static bool bMangoappSocketDisable = env_to_bool( getenv( "GAMESCOPE_MANGOAPP_SOCKET_DISABLE" ));

		// Whether or not to nudge mango app when this commit is done.
		const bool mango_nudge = pCurrentFocus && ( ( w == pCurrentFocus->focusWindow && !w->isSteamStreamingClient ) ||
									( pCurrentFocus->focusWindow && pCurrentFocus->focusWindow->isSteamStreamingClient && w->isSteamStreamingClientVideo ) )
									&& !bMangoappSocketDisable;

		bool bValidPreemptiveScale = reslistentry.pAcquirePoint && pCurrentFocus && w == pCurrentFocus->focusWindow && cv_upscale_preemptive;
		bool bPreemptiveUpscale = bValidPreemptiveScale && newCommit->ShouldPreemptivelyUpscale();

		bool bKnownReady = false;

		std::pair<int32_t, bool> eventFd = gamescope::CAcquireTimelinePoint::k_InvalidEvent;

		if ( bPreemptiveUpscale )
		{
			FrameInfo_t upscaledFrameInfo{};
			upscaledFrameInfo.applyOutputColorMgmt = true;
			upscaledFrameInfo.outputEncodingEOTF = ( newCommit->colorspace() == GAMESCOPE_APP_TEXTURE_COLORSPACE_LINEAR || newCommit->colorspace() == GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB )
				? EOTF_Gamma22
				: EOTF_PQ;

			float flOldGlobalScale = globalScaleRatio;
			float flOldZoomScale = zoomScaleRatio;
			float flOldOverscanScale = overscanScaleRatio;
			overscanScaleRatio = 1.0f;
			zoomScaleRatio = 1.0f;
			globalScaleRatio = 1.0f;
			paint_window_commit( newCommit, w, w, &upscaledFrameInfo, nullptr );
			upscaledFrameInfo.useFSRLayer0 = g_upscaleFilter == GamescopeUpscaleFilter::FSR;
			upscaledFrameInfo.useNISLayer0 = g_upscaleFilter == GamescopeUpscaleFilter::NIS;
			globalScaleRatio = flOldGlobalScale;
			zoomScaleRatio = flOldZoomScale;
			flOldOverscanScale = flOldOverscanScale;

			TempUpscaleImage_t *pTempImage = GetTempUpscaleImage( g_nOutputWidth, g_nOutputHeight, g_output.uOutputFormat );
			if ( pTempImage )
			{
				const uint64_t ulNextReleasePoint = ++pTempImage->ulLastPoint;

				std::unique_ptr<CVulkanCmdBuffer> pCommandBuffer = g_device.commandBuffer();
				
				pCommandBuffer->AddDependency( reslistentry.pAcquirePoint->GetTimeline()->ToVkSemaphore(), reslistentry.pAcquirePoint->GetPoint() );
				pCommandBuffer->AddSignal( pTempImage->pReleaseTimeline->ToVkSemaphore(), ulNextReleasePoint );

				static std::optional<uint64_t> s_ulLastPreemptiveUpscaleSeqNo;

				if ( s_ulLastPreemptiveUpscaleSeqNo )
				{
					vulkan_wait( *s_ulLastPreemptiveUpscaleSeqNo, true );
				}

				std::optional<uint64_t> seqNo = vulkan_composite( &upscaledFrameInfo, nullptr, false, pTempImage->pTexture, false, std::move( pCommandBuffer ) );

				if ( cv_upscale_preemptive_debug_force_sync )
				{
					vulkan_wait( *seqNo, true );
				}

				s_ulLastPreemptiveUpscaleSeqNo = seqNo;

				newCommit->upscaledTexture = std::optional<UpscaledTexture_t>
				{
					std::in_place_t{},
					g_upscaleFilter,
					g_upscaleScaler,
					g_nOutputWidth,
					g_nOutputHeight,
					pTempImage->pTexture,
					upscaledFrameInfo.outputEncodingEOTF == EOTF_Gamma22 ? VK_COLOR_SPACE_SRGB_NONLINEAR_KHR : VK_COLOR_SPACE_HDR10_ST2084_EXT,
				};

				// Manifest a new acquire timeline point with this inline work.
				eventFd = gamescope::CAcquireTimelinePoint( pTempImage->pReleaseTimeline, ulNextReleasePoint ).CreateEventFd();

				//xwm_log.infof( "Pre-emptively upscaling!" );
			}
			else
			{
				bPreemptiveUpscale = false;
			}
		}
		
		if ( !bPreemptiveUpscale )
		{
			if ( bValidPreemptiveScale )
			{
				ClearUpscaleImages();
			}

			if ( reslistentry.pAcquirePoint )
			{
				eventFd = reslistentry.pAcquirePoint->CreateEventFd();
			}
		}

		if ( gamescope::IBackendFb *pBackendFb = newCommit->vulkanTex->GetBackendFb() )
		{
			if ( reslistentry.pReleasePoint )
				pBackendFb->SetReleasePoint( reslistentry.pReleasePoint );
			else
				pBackendFb->SetBuffer( buf );
		}

		if ( eventFd != gamescope::CAcquireTimelinePoint::k_InvalidEvent )
		{
			fence = eventFd.first;
			bKnownReady = eventFd.second;
		}
		else
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
		}

		gpuvis_trace_printf( "pushing wait for commit %lu win %lx", newCommit->commitID, w->type == steamcompmgr_win_type_t::XWAYLAND ? w->xwayland().id : 0 );
		{
			newCommit->SetFence( fence, mango_nudge, doneCommits );
			if ( bKnownReady )
				newCommit->Signal();
			else
				g_ImageWaiter.AddWaitable( newCommit.get() );
		}

		w->commit_queue.push_back( std::move(newCommit) );
	}
}

void check_new_xwayland_res(xwayland_ctx_t *ctx)
{
	// When importing buffer, we'll potentially need to perform operations with
	// a wlserver lock (e.g. wlr_buffer_lock). We can't do this with a
	// wayland_commit_queue lock because that causes deadlocks.
	std::vector<ResListEntry_t>& tmp_queue = ctx->xwayland_server->retrieve_commits();

	for ( uint32_t i = 0; i < tmp_queue.size(); i++ )
	{
		steamcompmgr_win_t	*w = find_win( ctx, tmp_queue[ i ].surf );
		update_wayland_res( &ctx->doneCommits, w, tmp_queue[ i ]);
	}
}

void check_new_xdg_res()
{
	std::vector<ResListEntry_t> tmp_queue = wlserver_xdg_commit_queue();
	for ( uint32_t i = 0; i < tmp_queue.size(); i++ )
	{
		for ( const auto& xdg_win : g_steamcompmgr_xdg_wins )
		{
			if ( xdg_win->xdg().surface.main_surface == tmp_queue[ i ].surf )
			{
				update_wayland_res( &g_steamcompmgr_xdg_done_commits, xdg_win.get(), tmp_queue[ i ] );
				break;
			}
		}
	}
}


static void
handle_xfixes_selection_notify( xwayland_ctx_t *ctx, XFixesSelectionNotifyEvent *event )
{
	if (event->owner == ctx->ourWindow)
	{
		return;
	}

	XConvertSelection(ctx->dpy, event->selection, ctx->atoms.utf8StringAtom, event->selection, ctx->ourWindow, CurrentTime);
	XFlush(ctx->dpy);
}

void xwayland_ctx_t::Dispatch()
{
	xwayland_ctx_t *ctx = this;

	MouseCursor *cursor = ctx->cursor.get();
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
				steamcompmgr_win_t * w = find_win(ctx, ev.xdestroywindow.window);

				if (w && w->xwayland().id == ev.xdestroywindow.window)
					destroy_win(ctx, ev.xdestroywindow.window, true, true);
				break;
			}
			case MapNotify:
			{
				steamcompmgr_win_t * w = find_win(ctx, ev.xmap.window);

				if (w && w->xwayland().id == ev.xmap.window)
					map_win(ctx, ev.xmap.window, ev.xmap.serial);
				break;
			}
			case UnmapNotify:
			{
				steamcompmgr_win_t * w = find_win(ctx, ev.xunmap.window);

				if (w && w->xwayland().id == ev.xunmap.window)
					unmap_win(ctx, ev.xunmap.window, true);
				break;
			}
			case FocusOut:
			{
				steamcompmgr_win_t * w = find_win( ctx, ev.xfocus.window );

				// If focus escaped the current desired keyboard focus window, check where it went
				if ( w && w->xwayland().id == ctx->currentKeyboardFocusWindow )
				{
					Window newKeyboardFocus = None;
					int nRevertMode = 0;
					XGetInputFocus( ctx->dpy, &newKeyboardFocus, &nRevertMode );

					// Find window or its toplevel parent
					steamcompmgr_win_t *kbw = find_win( ctx, newKeyboardFocus );

					if ( kbw )
					{
						if ( kbw->xwayland().id == ctx->currentKeyboardFocusWindow )
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
					steamcompmgr_win_t * w = find_win(ctx, ev.xreparent.window);

					if (w && w->xwayland().id == ev.xreparent.window)
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
							MakeFocusDirty();
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
				break;
			case SelectionNotify:
				handle_selection_notify(ctx, &ev.xselection);
				break;
			case SelectionRequest:
				handle_selection_request(ctx, &ev.xselectionrequest);
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
				else if (ev.type == ctx->xfixes_event + XFixesSelectionNotify)
				{
					handle_xfixes_selection_notify(ctx, (XFixesSelectionNotifyEvent *) &ev);
				}
				break;
		}
		XFlush(ctx->dpy);
	}

	if ( bSetFocus )
	{
		XSetInputFocus(ctx->dpy, ctx->currentKeyboardFocusWindow, RevertToNone, CurrentTime);
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

const char* g_customCursorPath = nullptr;
int g_customCursorHotspotX = 0;
int g_customCursorHotspotY = 0;

xwayland_ctx_t g_ctx;

static bool setup_error_handlers = false;

void init_xwayland_ctx(uint32_t serverId, gamescope_xwayland_server_t *xwayland_server)
{
	assert(!xwayland_server->ctx);
	xwayland_server->ctx = std::make_unique<xwayland_ctx_t>();
	xwayland_ctx_t *ctx = xwayland_server->ctx.get();

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
    if (!XQueryExtension(ctx->dpy,
                        "XInputExtension",
                        &ctx->xinput_opcode,
                        &ctx->xinput_event,
                        &ctx->xinput_error))
	{
		xwm_log.errorf("No XInput extension");
		exit(1);
	}
	int xi_major = 2;
	int xi_minor = 0;
	XIQueryVersion(ctx->dpy, &xi_major, &xi_minor);

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
	ctx->atoms.netWMIcon = XInternAtom(ctx->dpy, "_NET_WM_ICON", false);
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
	ctx->atoms.gamescopeDebugScreenShotAtom = XInternAtom( ctx->dpy, "GAMESCOPECTRL_DEBUG_REQUEST_SCREENSHOT", false );

	ctx->atoms.gamescopeFocusDisplay = XInternAtom(ctx->dpy, "GAMESCOPE_FOCUS_DISPLAY", false);
	ctx->atoms.gamescopeMouseFocusDisplay = XInternAtom(ctx->dpy, "GAMESCOPE_MOUSE_FOCUS_DISPLAY", false);
	ctx->atoms.gamescopeKeyboardFocusDisplay = XInternAtom( ctx->dpy, "GAMESCOPE_KEYBOARD_FOCUS_DISPLAY", false );

	// In nanoseconds...
	ctx->atoms.gamescopeTuneableVBlankRedZone = XInternAtom( ctx->dpy, "GAMESCOPE_TUNEABLE_VBLANK_REDZONE", false );
	ctx->atoms.gamescopeTuneableRateOfDecay = XInternAtom( ctx->dpy, "GAMESCOPE_TUNEABLE_VBLANK_RATE_OF_DECAY_PERCENTAGE", false );

	ctx->atoms.gamescopeScalingFilter = XInternAtom( ctx->dpy, "GAMESCOPE_SCALING_FILTER", false );
	ctx->atoms.gamescopeFSRSharpness = XInternAtom( ctx->dpy, "GAMESCOPE_FSR_SHARPNESS", false );
	ctx->atoms.gamescopeSharpness = XInternAtom( ctx->dpy, "GAMESCOPE_SHARPNESS", false );

	ctx->atoms.gamescopeXWaylandModeControl = XInternAtom( ctx->dpy, "GAMESCOPE_XWAYLAND_MODE_CONTROL", false );
	ctx->atoms.gamescopeFPSLimit = XInternAtom( ctx->dpy, "GAMESCOPE_FPS_LIMIT", false );
	ctx->atoms.gamescopeDynamicRefresh[gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL] = XInternAtom( ctx->dpy, "GAMESCOPE_DYNAMIC_REFRESH", false );
	ctx->atoms.gamescopeDynamicRefresh[gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL] = XInternAtom( ctx->dpy, "GAMESCOPE_DYNAMIC_REFRESH_EXTERNAL", false );
	ctx->atoms.gamescopeLowLatency = XInternAtom( ctx->dpy, "GAMESCOPE_LOW_LATENCY", false );

	ctx->atoms.gamescopeFSRFeedback = XInternAtom( ctx->dpy, "GAMESCOPE_FSR_FEEDBACK", false );

	ctx->atoms.gamescopeBlurMode = XInternAtom( ctx->dpy, "GAMESCOPE_BLUR_MODE", false );
	ctx->atoms.gamescopeBlurRadius = XInternAtom( ctx->dpy, "GAMESCOPE_BLUR_RADIUS", false );
	ctx->atoms.gamescopeBlurFadeDuration = XInternAtom( ctx->dpy, "GAMESCOPE_BLUR_FADE_DURATION", false );

	ctx->atoms.gamescopeCompositeForce = XInternAtom( ctx->dpy, "GAMESCOPE_COMPOSITE_FORCE", false );
	ctx->atoms.gamescopeCompositeDebug = XInternAtom( ctx->dpy, "GAMESCOPE_COMPOSITE_DEBUG", false );

	ctx->atoms.gamescopeAllowTearing = XInternAtom( ctx->dpy, "GAMESCOPE_ALLOW_TEARING", false );

	ctx->atoms.gamescopeDisplayForceInternal = XInternAtom( ctx->dpy, "GAMESCOPE_DISPLAY_FORCE_INTERNAL", false );
	ctx->atoms.gamescopeDisplayModeNudge = XInternAtom( ctx->dpy, "GAMESCOPE_DISPLAY_MODE_NUDGE", false );

	ctx->atoms.gamescopeDisplayIsExternal = XInternAtom( ctx->dpy, "GAMESCOPE_DISPLAY_IS_EXTERNAL", false );
	ctx->atoms.gamescopeDisplayModeListExternal = XInternAtom( ctx->dpy, "GAMESCOPE_DISPLAY_MODE_LIST_EXTERNAL", false );

	ctx->atoms.gamescopeCursorVisibleFeedback = XInternAtom( ctx->dpy, "GAMESCOPE_CURSOR_VISIBLE_FEEDBACK", false );

	ctx->atoms.gamescopeSteamMaxHeight = XInternAtom( ctx->dpy, "GAMESCOPE_STEAM_MAX_HEIGHT", false );
	ctx->atoms.gamescopeVRREnabled = XInternAtom( ctx->dpy, "GAMESCOPE_VRR_ENABLED", false );
	ctx->atoms.gamescopeVRRCapable = XInternAtom( ctx->dpy, "GAMESCOPE_VRR_CAPABLE", false );
	ctx->atoms.gamescopeVRRInUse = XInternAtom( ctx->dpy, "GAMESCOPE_VRR_FEEDBACK", false );

	ctx->atoms.gamescopeNewScalingFilter = XInternAtom( ctx->dpy, "GAMESCOPE_NEW_SCALING_FILTER", false );
	ctx->atoms.gamescopeNewScalingScaler = XInternAtom( ctx->dpy, "GAMESCOPE_NEW_SCALING_SCALER", false );

	ctx->atoms.gamescopeDisplayEdidPath = XInternAtom( ctx->dpy, "GAMESCOPE_DISPLAY_EDID_PATH", false );
	ctx->atoms.gamescopeXwaylandServerId = XInternAtom( ctx->dpy, "GAMESCOPE_XWAYLAND_SERVER_ID", false );

	ctx->atoms.gamescopeDisplaySupportsHDR = XInternAtom( ctx->dpy, "GAMESCOPE_DISPLAY_SUPPORTS_HDR", false );
	ctx->atoms.gamescopeDisplayHDREnabled = XInternAtom( ctx->dpy, "GAMESCOPE_DISPLAY_HDR_ENABLED", false );
	ctx->atoms.gamescopeDebugForceHDR10Output = XInternAtom( ctx->dpy, "GAMESCOPE_DEBUG_FORCE_HDR10_PQ_OUTPUT", false );
	ctx->atoms.gamescopeDebugForceHDRSupport = XInternAtom( ctx->dpy, "GAMESCOPE_DEBUG_FORCE_HDR_SUPPORT", false );
	ctx->atoms.gamescopeDebugHDRHeatmap = XInternAtom( ctx->dpy, "GAMESCOPE_DEBUG_HDR_HEATMAP", false );
	ctx->atoms.gamescopeHDROutputFeedback = XInternAtom( ctx->dpy, "GAMESCOPE_HDR_OUTPUT_FEEDBACK", false );
	ctx->atoms.gamescopeSDROnHDRContentBrightness = XInternAtom( ctx->dpy, "GAMESCOPE_SDR_ON_HDR_CONTENT_BRIGHTNESS", false );
	ctx->atoms.gamescopeHDRInputGain = XInternAtom( ctx->dpy, "GAMESCOPE_HDR_INPUT_GAIN", false );
	ctx->atoms.gamescopeSDRInputGain = XInternAtom( ctx->dpy, "GAMESCOPE_SDR_INPUT_GAIN", false );
	ctx->atoms.gamescopeHDRItmEnable = XInternAtom( ctx->dpy, "GAMESCOPE_HDR_ITM_ENABLE", false );
	ctx->atoms.gamescopeHDRItmSDRNits = XInternAtom( ctx->dpy, "GAMESCOPE_HDR_ITM_SDR_NITS", false );
	ctx->atoms.gamescopeHDRItmTargetNits = XInternAtom( ctx->dpy, "GAMESCOPE_HDR_ITM_TARGET_NITS", false );
	ctx->atoms.gamescopeColorLookPQ = XInternAtom( ctx->dpy, "GAMESCOPE_COLOR_LOOK_PQ", false );
	ctx->atoms.gamescopeColorLookG22 = XInternAtom( ctx->dpy, "GAMESCOPE_COLOR_LOOK_G22", false );
	ctx->atoms.gamescopeColorOutputVirtualWhite = XInternAtom( ctx->dpy, "GAMESCOPE_DISPLAY_VIRTUAL_WHITE", false );
	ctx->atoms.gamescopeHDRTonemapDisplayMetadata = XInternAtom( ctx->dpy, "GAMESCOPE_HDR_TONEMAP_DISPLAY_METADATA", false );
	ctx->atoms.gamescopeHDRTonemapSourceMetadata = XInternAtom( ctx->dpy, "GAMESCOPE_HDR_TONEMAP_SOURCE_METADATA", false );
	ctx->atoms.gamescopeHDRTonemapOperator = XInternAtom( ctx->dpy, "GAMESCOPE_HDR_TONEMAP_OPERATOR", false );

	ctx->atoms.gamescopeForceWindowsFullscreen = XInternAtom( ctx->dpy, "GAMESCOPE_FORCE_WINDOWS_FULLSCREEN", false );

	ctx->atoms.gamescopeColorLut3DOverride = XInternAtom( ctx->dpy, "GAMESCOPE_COLOR_3DLUT_OVERRIDE", false );
	ctx->atoms.gamescopeColorShaperLutOverride = XInternAtom( ctx->dpy, "GAMESCOPE_COLOR_SHAPERLUT_OVERRIDE", false );
	ctx->atoms.gamescopeColorSDRGamutWideness = XInternAtom( ctx->dpy, "GAMESCOPE_COLOR_SDR_GAMUT_WIDENESS", false );
	ctx->atoms.gamescopeColorNightMode = XInternAtom( ctx->dpy, "GAMESCOPE_COLOR_NIGHT_MODE", false );
	ctx->atoms.gamescopeColorManagementDisable = XInternAtom( ctx->dpy, "GAMESCOPE_COLOR_MANAGEMENT_DISABLE", false );
	ctx->atoms.gamescopeColorAppWantsHDRFeedback = XInternAtom( ctx->dpy, "GAMESCOPE_COLOR_APP_WANTS_HDR_FEEDBACK", false );
	ctx->atoms.gamescopeColorAppHDRMetadataFeedback = XInternAtom( ctx->dpy, "GAMESCOPE_COLOR_APP_HDR_METADATA_FEEDBACK", false );
	ctx->atoms.gamescopeColorSliderInUse = XInternAtom( ctx->dpy, "GAMESCOPE_COLOR_MANAGEMENT_CHANGING_HINT", false );
	ctx->atoms.gamescopeColorChromaticAdaptationMode = XInternAtom( ctx->dpy, "GAMESCOPE_COLOR_CHROMATIC_ADAPTATION_MODE", false );
	ctx->atoms.gamescopeColorMuraCorrectionImage[gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL] = XInternAtom( ctx->dpy, "GAMESCOPE_COLOR_MURA_CORRECTION_IMAGE", false );
	ctx->atoms.gamescopeColorMuraCorrectionImage[gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL] = XInternAtom( ctx->dpy, "GAMESCOPE_COLOR_MURA_CORRECTION_IMAGE_EXTERNAL", false );
	ctx->atoms.gamescopeColorMuraScale[gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL] = XInternAtom( ctx->dpy, "GAMESCOPE_COLOR_MURA_SCALE", false );
	ctx->atoms.gamescopeColorMuraScale[gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL] = XInternAtom( ctx->dpy, "GAMESCOPE_COLOR_MURA_SCALE_EXTERNAL", false );
	ctx->atoms.gamescopeColorMuraCorrectionDisabled[gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL] = XInternAtom( ctx->dpy, "GAMESCOPE_COLOR_MURA_CORRECTION_DISABLED", false );
	ctx->atoms.gamescopeColorMuraCorrectionDisabled[gamescope::GAMESCOPE_SCREEN_TYPE_EXTERNAL] = XInternAtom( ctx->dpy, "GAMESCOPE_COLOR_MURA_CORRECTION_DISABLED_EXTERNAL", false );

	ctx->atoms.gamescopeCreateXWaylandServer = XInternAtom( ctx->dpy, "GAMESCOPE_CREATE_XWAYLAND_SERVER", false );
	ctx->atoms.gamescopeCreateXWaylandServerFeedback = XInternAtom( ctx->dpy, "GAMESCOPE_CREATE_XWAYLAND_SERVER_FEEDBACK", false );
	ctx->atoms.gamescopeDestroyXWaylandServer = XInternAtom( ctx->dpy, "GAMESCOPE_DESTROY_XWAYLAND_SERVER", false );

	ctx->atoms.gamescopeReshadeEffect = XInternAtom( ctx->dpy, "GAMESCOPE_RESHADE_EFFECT", false );
	ctx->atoms.gamescopeReshadeTechniqueIdx = XInternAtom( ctx->dpy, "GAMESCOPE_RESHADE_TECHNIQUE_IDX", false );

	ctx->atoms.gamescopeDisplayRefreshRateFeedback = XInternAtom( ctx->dpy, "GAMESCOPE_DISPLAY_REFRESH_RATE_FEEDBACK", false );
	ctx->atoms.gamescopeDisplayDynamicRefreshBasedOnGamePresence = XInternAtom( ctx->dpy, "GAMESCOPE_DISPLAY_DYNAMIC_REFRESH_BASED_ON_GAME_PRESENCE", false );

	ctx->atoms.wineHwndStyle = XInternAtom( ctx->dpy, "_WINE_HWND_STYLE", false );
	ctx->atoms.wineHwndStyleEx = XInternAtom( ctx->dpy, "_WINE_HWND_EXSTYLE", false );

	ctx->atoms.clipboard = XInternAtom(ctx->dpy, "CLIPBOARD", false);
	ctx->atoms.primarySelection = XInternAtom(ctx->dpy, "PRIMARY", false);
	ctx->atoms.targets = XInternAtom(ctx->dpy, "TARGETS", false);

	ctx->root_width = DisplayWidth(ctx->dpy, ctx->scr);
	ctx->root_height = DisplayHeight(ctx->dpy, ctx->scr);

	ctx->allDamage = None;
	ctx->clipChanged = true;

	XChangeProperty(ctx->dpy, ctx->root, ctx->atoms.gamescopeXwaylandServerId, XA_CARDINAL, 32, PropModeReplace,
		(unsigned char *)&serverId, 1 );

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
	XFixesSelectSelectionInput(ctx->dpy, ctx->root, ctx->atoms.clipboard, XFixesSetSelectionOwnerNotifyMask);
	XFixesSelectSelectionInput(ctx->dpy, ctx->root, ctx->atoms.primarySelection, XFixesSetSelectionOwnerNotifyMask);
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
	else
	{
		if ( std::shared_ptr<gamescope::INestedHints::CursorInfo> pHostCursor = gamescope::GetX11HostCursor() )
		{
			ctx->cursor->setCursorImage(
				reinterpret_cast<char *>( pHostCursor->pPixels.data() ),
				pHostCursor->uWidth,
				pHostCursor->uHeight,
				pHostCursor->uXHotspot,
				pHostCursor->uYHotspot );
		}
		else
		{
			xwm_log.infof( "Embedded, no cursor set. Using left_ptr by default." );
			if ( !ctx->cursor->setCursorImageByName( "left_ptr" ) )
				xwm_log.errorf( "Failed to load mouse cursor: left_ptr" );
		}
	}

	ctx->cursor->undirty();

	XFlush(ctx->dpy);
}

void update_vrr_atoms(xwayland_ctx_t *root_ctx, bool force, bool* needs_flush = nullptr)
{
	bool capable = GetBackend()->GetCurrentConnector() && GetBackend()->GetCurrentConnector()->SupportsVRR();
	if ( capable != g_bVRRCapable_CachedValue || force )
	{
		uint32_t capable_value = capable ? 1 : 0;
		XChangeProperty(root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeVRRCapable, XA_CARDINAL, 32, PropModeReplace,
			(unsigned char *)&capable_value, 1 );
		g_bVRRCapable_CachedValue = capable;
		if (needs_flush)
			*needs_flush = true;
	}

	bool HDR = GetBackend()->GetCurrentConnector() && GetBackend()->GetCurrentConnector()->SupportsHDR();
	if ( HDR != g_bSupportsHDR_CachedValue || force )
	{
		uint32_t hdr_value = HDR ? 1 : 0;
		XChangeProperty(root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeDisplaySupportsHDR, XA_CARDINAL, 32, PropModeReplace,
			(unsigned char *)&hdr_value, 1 );
		g_bSupportsHDR_CachedValue = HDR;
		if (needs_flush)
			*needs_flush = true;
	}

	bool in_use = GetBackend()->GetCurrentConnector() && GetBackend()->GetCurrentConnector()->IsVRRActive();
	if ( in_use != g_bVRRInUse_CachedValue || force )
	{
		uint32_t in_use_value = in_use ? 1 : 0;
		XChangeProperty(root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeVRRInUse, XA_CARDINAL, 32, PropModeReplace,
			(unsigned char *)&in_use_value, 1 );
		g_bVRRInUse_CachedValue = in_use;
		if (needs_flush)
			*needs_flush = true;
	}

	if ( g_nOutputRefresh != g_nCurrentRefreshRate_CachedValue || force )
	{
		int32_t nRefresh = gamescope::ConvertmHzToHz( g_nOutputRefresh );
		XChangeProperty(root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeDisplayRefreshRateFeedback, XA_CARDINAL, 32, PropModeReplace,
			(unsigned char *)&nRefresh, 1 );
		g_nCurrentRefreshRate_CachedValue = g_nOutputRefresh;
		if (needs_flush)
			*needs_flush = true;
	}

	// Don't update this in-sync with DRM vrr usage.
	// Keep this as a preference, starting with off.
	if ( force )
	{
        bool wants_vrr = cv_adaptive_sync;
		uint32_t enabled_value = wants_vrr ? 1 : 0;
		XChangeProperty(root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeVRREnabled, XA_CARDINAL, 32, PropModeReplace,
			(unsigned char *)&enabled_value, 1 );
		if (needs_flush)
			*needs_flush = true;
	}
}

void update_mode_atoms(xwayland_ctx_t *root_ctx, bool* needs_flush = nullptr)
{
	if (needs_flush)
		*needs_flush = true;

	if ( GetBackend()->GetCurrentConnector() && GetBackend()->GetCurrentConnector()->GetScreenType() == gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL )
	{
		XDeleteProperty(root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeDisplayModeListExternal);

		uint32_t zero = 0;
		XChangeProperty(root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeDisplayIsExternal, XA_CARDINAL, 32, PropModeReplace,
			(unsigned char *)&zero, 1 );
		return;
	}

	if ( !GetBackend()->GetCurrentConnector() )
		return;

	auto connectorModes = GetBackend()->GetCurrentConnector()->GetModes();

	char modes[4096] = "";
	int remaining_size = sizeof(modes) - 1;
	int len = 0;
	for (int i = 0; remaining_size > 0 && i < (int)connectorModes.size(); i++)
	{
		const auto& mode = connectorModes[i];
		int mode_len = snprintf(&modes[len], remaining_size, "%s%dx%d@%d",
			i == 0 ? "" : " ",
			int(mode.uWidth), int(mode.uHeight), int(mode.uRefresh));
		len += mode_len;
		remaining_size -= mode_len;
	}
	XChangeProperty(root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeDisplayModeListExternal, XA_STRING, 8, PropModeReplace,
		(unsigned char *)modes, strlen(modes) + 1 );
	
	uint32_t one = 1;
	XChangeProperty(root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeDisplayIsExternal, XA_CARDINAL, 32, PropModeReplace,
		(unsigned char *)&one, 1 );
}

extern int g_nPreferredOutputWidth;
extern int g_nPreferredOutputHeight;

static bool g_bWasFSRActive = false;

bool g_bAppWantsHDRCached = false;

void steamcompmgr_check_xdg(bool vblank, uint64_t vblank_idx)
{
	if (wlserver_xdg_dirty())
	{
		for ( auto &iter : g_VirtualConnectorFocuses )
		{
			global_focus_t *pFocus = &iter.second;
			if (pFocus->focusWindow && pFocus->focusWindow->type == steamcompmgr_win_type_t::XDG)
				pFocus->focusWindow = nullptr;
			if (pFocus->inputFocusWindow && pFocus->inputFocusWindow->type == steamcompmgr_win_type_t::XDG)
				pFocus->inputFocusWindow = nullptr;
			if (pFocus->overlayWindow && pFocus->overlayWindow->type == steamcompmgr_win_type_t::XDG)
				pFocus->overlayWindow = nullptr;
			if (pFocus->notificationWindow && pFocus->notificationWindow->type == steamcompmgr_win_type_t::XDG)
				pFocus->notificationWindow = nullptr;
			if (pFocus->overrideWindow && pFocus->overrideWindow->type == steamcompmgr_win_type_t::XDG)
				pFocus->overrideWindow = nullptr;
			if (pFocus->fadeWindow && pFocus->fadeWindow->type == steamcompmgr_win_type_t::XDG)
				pFocus->fadeWindow = nullptr;
		}
		g_steamcompmgr_xdg_wins = wlserver_get_xdg_shell_windows();
		MakeFocusDirty();
	}

	handle_done_commits_xdg( vblank, vblank_idx );

	// When we have observed both a complete commit and a VBlank, we should request a new frame.
	if (vblank)
	{
		for ( const auto& xdg_win : g_steamcompmgr_xdg_wins )
		{
			steamcompmgr_flush_frame_done(xdg_win.get());
		}

		handle_presented_xdg();
	}

	check_new_xdg_res();
}

void update_edid_prop()
{
	const char *filename = gamescope::GetPatchedEdidPath();
	if (!filename)
		return;

	gamescope_xwayland_server_t *server = NULL;
	for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
	{
		XTextProperty text_property =
		{
			.value = (unsigned char *)filename,
			.encoding = server->ctx->atoms.utf8StringAtom,
			.format = 8,
			.nitems = strlen(filename),
		};

		XSetTextProperty( server->ctx->dpy, server->ctx->root, &text_property, server->ctx->atoms.gamescopeDisplayEdidPath );
	}
}

extern bool g_bLaunchMangoapp;

extern void ShutdownGamescope();

gamescope::ConVar<bool> cv_shutdown_on_primary_child_death( "shutdown_on_primary_child_death", true, "Should gamescope shutdown when the primary application launched in it was shut down?" );
static LogScope s_LaunchLogScope( "launch" );

static std::vector<uint32_t> s_uRelativeMouseFilteredAppids;
static gamescope::ConVar<std::string> cv_mouse_relative_filter_appids( "mouse_relative_filter_appids",
"8400" /* Geometry Wars: Retro Evolved */,
"Comma separated appids to filter out using relative mouse mode for.",
[]( gamescope::ConVar<std::string> &cvar )
{
	std::vector<std::string_view> sFilterAppids = gamescope::Split( cvar, "," );
	std::vector<uint32_t> uFilterAppids;
	uFilterAppids.reserve( sFilterAppids.size() );
	for ( auto &sFilterAppid : sFilterAppids )
	{
		std::optional<uint32_t> ouFilterAppid = gamescope::Parse<uint32_t>( sFilterAppid );
		uFilterAppids.push_back( *ouFilterAppid );
	}

	s_uRelativeMouseFilteredAppids = std::move( uFilterAppids );
}, true);

void LaunchNestedChildren( char **ppPrimaryChildArgv )
{
	std::string sNewPreload;
	{
		const char *pszCurrentPreload = getenv( "LD_PRELOAD" );
		if ( pszCurrentPreload && *pszCurrentPreload )
		{
			// Remove gameoverlayrenderer.so from the child if Gamescope
			// is running with a window + Vulkan swapchain (eg. SDL2 backend)
			if ( GetBackend()->UsesVulkanSwapchain() )
			{
				std::vector<std::string_view> svLibraries = gamescope::Split( pszCurrentPreload, " :" );
				std::erase_if( svLibraries, []( std::string_view svPreload )
				{
					return svPreload.find( "gameoverlayrenderer.so" ) != std::string_view::npos;
				});

				bool bFirst = true;
				for ( std::string_view svLibrary : svLibraries )
				{
					if ( !bFirst )
					{
						sNewPreload.append( ":" );
					}
					bFirst = false;
					sNewPreload.append( svLibrary );
				}
			}
			else
			{
				sNewPreload = pszCurrentPreload;
			}
		}
	}

	// We could just run this inside the child process,
	// but we might as well just run it here at this point.
	// and affect all future child processes, without needing
	// a pre-amble inside of them.
	{
		if ( !sNewPreload.empty() )
			setenv( "LD_PRELOAD", sNewPreload.c_str(), 1 );
		else
			unsetenv( "LD_PRELOAD" );

		unsetenv( "ENABLE_VKBASALT" );
		// Enable Gamescope WSI by default for nested.
		setenv( "ENABLE_GAMESCOPE_WSI", "1", 0 );

		// Unset this to avoid it leaking to Proton apps, etc.
		unsetenv( "SDL_VIDEODRIVER" );
		// SDL3...
		unsetenv( "SDL_VIDEO_DRIVER" );
	}

	// Gamescope itself does not set itself as a subreaper anymore.
	// It launches direct children that do, and manage that they kill themselves
	// when Gamescope dies.
	// This allows us to launch stuff alongside Gamescope if we ever wanted -- rather
	// than being under it. (eg. if we wanted a drm janitor or something.)

	if ( ppPrimaryChildArgv && *ppPrimaryChildArgv )
	{
		pid_t nPrimaryChildPid = gamescope::Process::SpawnProcessInWatchdog( ppPrimaryChildArgv, false );

		std::thread waitThread([ nPrimaryChildPid ]()
		{
			pthread_setname_np( pthread_self(), "gamescope-wait" );

			gamescope::Process::WaitForChild( nPrimaryChildPid );
			s_LaunchLogScope.infof( "Primary child shut down!" );

			if ( cv_shutdown_on_primary_child_death )
				ShutdownGamescope();
		});
		waitThread.detach();
	}

	if ( g_bLaunchMangoapp )
	{
		char *ppMangoappArgv[] = { (char *)"mangoapp", NULL };
		gamescope::Process::SpawnProcessInWatchdog( ppMangoappArgv, true );
	}
}

static gamescope::CTimerFunction g_FPSLimitVRRTimer{ []
{
	// do nothing.
}};

void
steamcompmgr_main(int argc, char **argv)
{
	int	readyPipeFD = -1;

	// Reset getopt() state
	optind = 1;

	int o;
	int opt_index = -1;
	bool bForceWindowsFullscreen = false;
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
				cursorHideTime = uint64_t( atoi( optarg ) ) * 1'000'000ul;
				break;
			case 'v':
				drawDebugInfo = true;
				break;
			case 'c':
				cv_composite_force = true;
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
				} else if (strcmp(opt_name, "force-windows-fullscreen") == 0) {
					bForceWindowsFullscreen = true;
				} else if (strcmp(opt_name, "hdr-enabled") == 0 || strcmp(opt_name, "hdr-enable") == 0) {
					cv_hdr_enabled = true;
				} else if (strcmp(opt_name, "hdr-debug-force-support") == 0) {
					g_bForceHDRSupportDebug = true;
 				} else if (strcmp(opt_name, "hdr-debug-force-output") == 0) {
					g_bForceHDR10OutputDebug = true;
				} else if (strcmp(opt_name, "hdr-itm-enabled") == 0 || strcmp(opt_name, "hdr-itm-enable") == 0) {
					g_bHDRItmEnable = true;
				} else if (strcmp(opt_name, "sdr-gamut-wideness") == 0) {
					g_ColorMgmt.pending.sdrGamutWideness = atof(optarg);
				} else if (strcmp(opt_name, "hdr-sdr-content-nits") == 0) {
					g_ColorMgmt.pending.flSDROnHDRBrightness = atof(optarg);
				} else if (strcmp(opt_name, "hdr-itm-sdr-nits") == 0) {
					g_flHDRItmSdrNits = atof(optarg);
				} else if (strcmp(opt_name, "hdr-itm-target-nits") == 0) {
					g_flHDRItmTargetNits = atof(optarg);
				} else if (strcmp(opt_name, "framerate-limit") == 0) {
					g_nSteamCompMgrTargetFPS = atoi(optarg);
				} else if (strcmp(opt_name, "reshade-effect") == 0) {
					g_reshade_effect = optarg;
				} else if (strcmp(opt_name, "reshade-technique-idx") == 0) {
					g_reshade_technique_idx = atoi(optarg);
				} else if (strcmp(opt_name, "mura-map") == 0) {
					set_mura_overlay(optarg);
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

	const char *pchEnableVkBasalt = getenv( "ENABLE_VKBASALT" );
	if ( pchEnableVkBasalt != nullptr && pchEnableVkBasalt[0] == '1' )
	{
		cv_composite_force = true;
	}

	// Enable color mgmt by default.
	g_ColorMgmt.pending.enabled = true;

	currentOutputWidth = g_nPreferredOutputWidth;
	currentOutputHeight = g_nPreferredOutputHeight;

	init_runtime_info();

	std::unique_lock<std::mutex> xwayland_server_guard(g_SteamCompMgrXWaylandServerMutex);

	// Initialize any xwayland ctxs we have
	{
		gamescope_xwayland_server_t *server = NULL;
		for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
			init_xwayland_ctx(i, server);
	}

	gamescope_xwayland_server_t *root_server = wlserver_get_xwayland_server(0);
	xwayland_ctx_t *root_ctx = root_server->ctx.get();

	gamesRunningCount = get_prop(root_ctx, root_ctx->root, root_ctx->atoms.gamesRunningAtom, 0);
	overscanScaleRatio = get_prop(root_ctx, root_ctx->root, root_ctx->atoms.screenScaleAtom, 0xFFFFFFFF) / (double)0xFFFFFFFF;
	zoomScaleRatio = get_prop(root_ctx, root_ctx->root, root_ctx->atoms.screenZoomAtom, 0xFFFF) / (double)0xFFFF;

	globalScaleRatio = overscanScaleRatio * zoomScaleRatio;

	if ( gamescope::VirtualConnectorIsSingleOutput() )
	{
		// misyl: Make the virtual connector up-front if we are in a single-output mode.
		// So we don't delay in getting display/output info to the game
		static constexpr uint64_t k_unSingleOutputVirtualConnectorKey = 0;

		g_VirtualConnectorFocuses[ k_unSingleOutputVirtualConnectorKey ] = global_focus_t
		{
			.ulVirtualFocusKey = k_unSingleOutputVirtualConnectorKey,
			.pVirtualConnector = GetBackend()->UsesVirtualConnectors() ? GetBackend()->CreateVirtualConnector( k_unSingleOutputVirtualConnectorKey ) : nullptr,
		};
	}

	for ( auto &iter : g_VirtualConnectorFocuses )
	{
		global_focus_t *pFocus = &iter.second;
		if ( pFocus->IsDirty() )
			determine_and_apply_focus( pFocus );
	}

	if ( readyPipeFD != -1 )
	{
		dprintf( readyPipeFD, "%s %s\n", root_ctx->xwayland_server->get_nested_display_name(), wlserver_get_wl_display_name() );
		close( readyPipeFD );
		readyPipeFD = -1;
	}

	g_SteamCompMgrWaiter.AddWaitable( &GetVBlankTimer() );
	g_SteamCompMgrWaiter.AddWaitable( &g_FPSLimitVRRTimer );
	GetVBlankTimer().ArmNextVBlank( true );

	{
		gamescope_xwayland_server_t *pServer = NULL;
		for (size_t i = 0; (pServer = wlserver_get_xwayland_server(i)); i++)
		{
			xwayland_ctx_t *pXWaylandCtx = pServer->ctx.get();
			g_SteamCompMgrWaiter.AddWaitable( pXWaylandCtx );

			pServer->ctx->force_windows_fullscreen = bForceWindowsFullscreen;
		}
	}

	update_vrr_atoms(root_ctx, true);
	update_mode_atoms(root_ctx);
	XFlush(root_ctx->dpy);

	if ( !GetBackend()->PostInit() )
		return;

	update_edid_prop();

	update_screenshot_color_mgmt();

	LaunchNestedChildren( subCommandArg >= 0 ? &argv[ subCommandArg ] : nullptr );

	// Transpose to get this 3x3 matrix into the right state for applying as a 3x4
	// on DRM + the Vulkan side.
	// ie. color.rgb = color.rgba * u_ctm[offsetLayerIdx];
	s_scRGB709To2020Matrix = GetBackend()->CreateBackendBlob( glm::mat3x4( glm::transpose( k_2020_from_709 ) ) );

#if HAVE_LIBSYSTEMD
	int res = sd_bus_default(&g_dbus);
	if (res < 0) {
		g_dbus = NULL;
		s_LaunchLogScope.warnf(
				  "Failed to open systemd message bus, there will be no cgroup protection for focused windows.\n");
	}

	FILE *sysfs_caps = fopen("/sys/fs/cgroup/dmem.capacity", "r");
	if (sysfs_caps != nullptr) {
		char *line = NULL;
		size_t size = 0;
		while (getline(&line, &size, sysfs_caps) >= 0) {
			char *capacity = strstr(line, " ");
			if (capacity)
				++capacity;
			else
				continue;
			uint64_t vramSize = strtoull(capacity, NULL, 10);
			std::string idString = std::string(line, (capacity - 1) - line);
			g_vramCapacities.emplace(idString, vramSize);
		}
	}
#endif

	for (;;)
	{
		{
			gamescope_xwayland_server_t *server = NULL;
			for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
			{
				assert(server->ctx);
				if (server->ctx->HasQueuedEvents())
					server->ctx->Dispatch();
			}
		}

		g_SteamCompMgrWaiter.PollEvents();

		bool vblank = false;
		if ( std::optional<gamescope::VBlankTime> pendingVBlank = GetVBlankTimer().ProcessVBlank() )
		{
			g_SteamCompMgrVBlankTime = *pendingVBlank;
			vblank = true;
		}

		if ( g_bRun == false )
		{
			break;
		}

		// If this is from the timer or not.
		// Consider this to also be "is this vblank, the fastest refresh cycle after our last commit?"
		// as a question.
		const bool bIsVBlankFromTimer = vblank;

		// We can always vblank if VRR.
		const bool bVRR = GetBackend()->GetCurrentConnector() && GetBackend()->GetCurrentConnector()->IsVRRActive();
		if ( bVRR )
			vblank = true;

		bool flush_root = false;

		if ( inputCounter != lastPublishedInputCounter )
		{
			XChangeProperty( root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeInputCounterAtom, XA_CARDINAL, 32, PropModeReplace,
							 (unsigned char *)&inputCounter, 1 );

			lastPublishedInputCounter = inputCounter;
			flush_root = true;
		}

		if ( g_bFSRActive != g_bWasFSRActive )
		{
			uint32_t active = g_bFSRActive ? 1 : 0;
			XChangeProperty( root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeFSRFeedback, XA_CARDINAL, 32, PropModeReplace,
					(unsigned char *)&active, 1 );

			g_bWasFSRActive = g_bFSRActive;
			flush_root = true;
		}

		static gamescope::VirtualConnectorStrategy s_eLastVirtualConnectorStrategy = gamescope::cv_backend_virtual_connector_strategy;
		gamescope::VirtualConnectorStrategy eVirtualConnectorStrategy = gamescope::cv_backend_virtual_connector_strategy;

		if ( eVirtualConnectorStrategy != s_eLastVirtualConnectorStrategy )
		{
			// If our virtual connector strategy changes, clear out our virtual connector
			// global focuses.
			g_VirtualConnectorFocuses.clear();
			s_eLastVirtualConnectorStrategy = eVirtualConnectorStrategy;
		}

#if 0
		bool bDirtyFocuses = false;
		for ( auto &iter : g_VirtualConnectorFocuses )
		{
			global_focus_t *pFocus = &iter.second;
			if ( pFocus->IsDirty() )
			{
				bDirtyFocuses = true;
				break;
			}
		}
#endif

		// XXX: Need to look into why this doesn't work.
		//	if ( bDirtyFocuses )
		{
			// TODO(misyl): Improve this situation, it's kind of a mess.
			// We could/should make this event driven rather than solving
			// per-frame.

			if ( !gamescope::VirtualConnectorIsSingleOutput() )
			{
				std::vector<gamescope::VirtualConnectorKey_t> newKeys;

				auto focusWindows = GetGlobalPossibleFocusWindows();
				for ( steamcompmgr_win_t *pWindow : focusWindows )
				{
					gamescope::VirtualConnectorKey_t ulKey = pWindow->GetVirtualConnectorKey( eVirtualConnectorStrategy );
					if ( !gamescope::Algorithm::Contains( newKeys, ulKey ) )
						newKeys.emplace_back( ulKey );
				}
				std::sort( newKeys.begin(), newKeys.end() );

				std::vector<gamescope::VirtualConnectorKey_t> oldKeys;
				for ( const auto &iter : g_VirtualConnectorFocuses )
					oldKeys.emplace_back( iter.first );
				std::sort( oldKeys.begin(), oldKeys.end() );

				std::vector<gamescope::VirtualConnectorKey_t> diffKeys;

				std::set_symmetric_difference(oldKeys.begin(), oldKeys.end(),
									newKeys.begin(), newKeys.end(),
									std::back_inserter(diffKeys),
									[](auto& a, auto& b) { return a < b; });

				for ( gamescope::VirtualConnectorKey_t ulKey : diffKeys )	
				{
					bool bIsSteam = gamescope::VirtualConnectorKeyIsSteam( ulKey );

					if ( gamescope::Algorithm::Contains( newKeys, ulKey ) )
					{
						g_VirtualConnectorFocuses[ulKey] = global_focus_t
						{
							.ulVirtualFocusKey = ulKey,
							.pVirtualConnector = GetBackend()->UsesVirtualConnectors() ? GetBackend()->CreateVirtualConnector( ulKey ) : nullptr,
						};
					}
					else if ( !bIsSteam ) // Never remove Steam's virtual conn	ector.
					{
						g_VirtualConnectorFocuses.erase( ulKey );
					}
				}
			}

			for ( auto &iter : g_VirtualConnectorFocuses )
			{
				global_focus_t *pFocus = &iter.second;
				if ( pFocus->IsDirty() )
					determine_and_apply_focus( pFocus );
			}
		}

		// If our DRM state is out-of-date, refresh it. This might update
		// the output size.
		if ( GetBackend()->PollState() )
		{
			hasRepaint = true;

			update_mode_atoms(root_ctx, &flush_root);
		}

		g_uCompositeDebug = cv_composite_debug;

		g_bOutputHDREnabled = (g_bSupportsHDR_CachedValue || g_bForceHDR10OutputDebug) && cv_hdr_enabled;

		// Pick our width/height for this potential frame, regardless of how it might change later
		// At some point we might even add proper locking so we get real updates atomically instead
		// of whatever jumble of races the below might cause over a couple of frames
		if ( currentOutputWidth != g_nOutputWidth ||
			 currentOutputHeight != g_nOutputHeight ||
			 currentHDROutput != g_bOutputHDREnabled ||
			 currentHDRForce != g_bForceHDRSupportDebug )
		{
			if ( steamMode && g_nXWaylandCount > 1 )
			{
				g_nNestedHeight = ( g_nNestedWidth * g_nOutputHeight ) / g_nOutputWidth;
				wlserver_lock();
				// Update only Steam, the root ctx, with the new output size for now
				wlserver_set_xwayland_server_mode( 0, g_nOutputWidth, g_nOutputHeight, g_nOutputRefresh );
				wlserver_unlock();
			}

			// XXX(JoshA): Remake this. It sucks.
			if ( GetBackend()->UsesVulkanSwapchain() )
			{
				vulkan_remake_swapchain();

				while ( !acquire_next_image() )
					vulkan_remake_swapchain();
			}
			else
			{
				vulkan_remake_output_images();
			}


			{
				gamescope_xwayland_server_t *server = NULL;
				for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
				{
					uint32_t hdr_value = ( g_bOutputHDREnabled || g_bForceHDRSupportDebug ) ? 1 : 0;
					XChangeProperty(server->ctx->dpy, server->ctx->root, server->ctx->atoms.gamescopeHDROutputFeedback, XA_CARDINAL, 32, PropModeReplace,
						(unsigned char *)&hdr_value, 1 );

					server->ctx->cursor->setDirty();

					if (server->ctx.get() == root_ctx)
					{
						flush_root = true;
					}
					else
					{
						XFlush(server->ctx->dpy);
					}
				}
			}

			currentOutputWidth = g_nOutputWidth;
			currentOutputHeight = g_nOutputHeight;
			currentHDROutput = g_bOutputHDREnabled;
			currentHDRForce = g_bForceHDRSupportDebug;

#if HAVE_PIPEWIRE
			nudge_pipewire();
#endif
		}

		// Ask for a new surface every vblank
		// When we observe a new commit being complete for a surface, we ask for a new frame.
		// This ensures that FIFO works properly, since otherwise we might ask for a new frame
		// application can commit a new frame that completes before we ever displayed
		// the current pending commit.
		static uint64_t vblank_idx = 0;
		if ( vblank )
		{
			{
				uint64_t now = get_time_in_nanos();

				gamescope_xwayland_server_t *server = NULL;
				for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
				{
					for (steamcompmgr_win_t *w = server->ctx->list; w; w = w->xwayland().next)
					{
						steamcompmgr_latch_frame_done( w, vblank_idx, now );
					}
				}

				for ( const auto& xdg_win : g_steamcompmgr_xdg_wins )
				{
					steamcompmgr_latch_frame_done( xdg_win.get(), vblank_idx, now );
				}
			}
		}

		{
			gamescope_xwayland_server_t *server = NULL;
			for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
			{
				handle_done_commits_xwayland(server->ctx.get(), vblank, vblank_idx);

				// When we have observed both a complete commit and a VBlank, we should request a new frame.
				if (vblank)
				{
					for (steamcompmgr_win_t *w = server->ctx->list; w; w = w->xwayland().next)
					{
						steamcompmgr_flush_frame_done(w);
					}
				}
			}
		}

		steamcompmgr_check_xdg(vblank, vblank_idx);

		if ( s_oLowestFPSLimitScheduleVRR )
		{
			g_FPSLimitVRRTimer.ArmTimer( *s_oLowestFPSLimitScheduleVRR );
			s_oLowestFPSLimitScheduleVRR = std::nullopt;
		}

		if ( vblank )
		{
			vblank_idx++;

			int nRealRefreshmHz = g_nNestedRefresh ? g_nNestedRefresh : g_nOutputRefresh;
			g_SteamCompMgrAppRefreshCycle = gamescope::mHzToRefreshCycle( nRealRefreshmHz );
			g_SteamCompMgrLimitedAppRefreshCycle = g_SteamCompMgrAppRefreshCycle;
			if ( g_nSteamCompMgrTargetFPS )
			{
				int nRealRefreshHz = gamescope::ConvertmHzToHz( nRealRefreshmHz );
				int nTargetFPS = g_nSteamCompMgrTargetFPS;
				nTargetFPS = std::min<int>( nTargetFPS, nRealRefreshHz );

				if ( GetBackend()->GetCurrentConnector() && GetBackend()->GetCurrentConnector()->IsVRRActive() )
				{
					g_SteamCompMgrLimitedAppRefreshCycle = gamescope::mHzToRefreshCycle( gamescope::ConvertHztomHz( nTargetFPS ) );
				}
				else
				{
					int nVblankDivisor = nRealRefreshHz / nTargetFPS;

					g_SteamCompMgrLimitedAppRefreshCycle = g_SteamCompMgrAppRefreshCycle * nVblankDivisor;
				}
			}
		}

		// Handle presentation-time stuff
		//
		// Notes:
		//
		// We send the presented event just after the latest latch time possible so PresentWait in Vulkan
		// still returns pretty optimally. The extra 2ms or so can be "display latency"
		// We still provide the predicted TTL refresh time in the presented event though.
		//
		// We ignore or lie most of the flags because they aren't particularly useful for a client
		// to know anyway and it would delay us sending this at an optimal time.
		// (particularly for DXGI frame latency handles under Proton.)
		//
		// The boat is still out as to whether we should do latest latch or pageflip/ttl for the event.
		// For now, going to keep this, and if we change our minds later, it's no big deal.
		//
		// It's a little strange, but we return `presented` for any window not visible
		// and `presented` for anything visible. It's a little disingenuous because we didn't
		// actually show a window if it wasn't visible, but we could! And that is the first
		// opportunity it had. It's confusing but we need this for forward progress.

		if ( vblank )
		{
			wlserver_lock();
			gamescope_xwayland_server_t *server = NULL;
			for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
				handle_presented_xwayland( server->ctx.get() );
			wlserver_unlock();
		}

		//

		{
			gamescope_xwayland_server_t *server = NULL;
			for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
				check_new_xwayland_res(server->ctx.get());
		}


		{
			GamescopeAppTextureColorspace current_app_colorspace = GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB;
			std::shared_ptr<gamescope::BackendBlob> app_hdr_metadata = nullptr;
			if ( g_HeldCommits[HELD_COMMIT_BASE] != nullptr )
			{
				current_app_colorspace = g_HeldCommits[HELD_COMMIT_BASE]->colorspace();
				if (g_HeldCommits[HELD_COMMIT_BASE]->feedback)
					app_hdr_metadata = g_HeldCommits[HELD_COMMIT_BASE]->feedback->hdr_metadata_blob;
			}

			bool app_wants_hdr = ColorspaceIsHDR( current_app_colorspace );

			if ( app_wants_hdr != g_bAppWantsHDRCached )
			{
				uint32_t app_wants_hdr_prop = app_wants_hdr ? 1 : 0;

				XChangeProperty(root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeColorAppWantsHDRFeedback, XA_CARDINAL, 32, PropModeReplace,
						(unsigned char *)&app_wants_hdr_prop, 1 );

				g_bAppWantsHDRCached = app_wants_hdr;
				flush_root = true;
			}

			if ( app_hdr_metadata != g_ColorMgmt.pending.appHDRMetadata )
			{
				if ( app_hdr_metadata )
				{
					std::vector<uint32_t> app_hdr_metadata_blob;
					app_hdr_metadata_blob.resize((sizeof(hdr_metadata_infoframe) + (sizeof(uint32_t) - 1)) / sizeof(uint32_t));
					memset(app_hdr_metadata_blob.data(), 0, sizeof(uint32_t) * app_hdr_metadata_blob.size());
					memcpy(app_hdr_metadata_blob.data(), &app_hdr_metadata->View<hdr_output_metadata>().hdmi_metadata_type1, sizeof(hdr_metadata_infoframe));

					XChangeProperty(root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeColorAppHDRMetadataFeedback, XA_CARDINAL, 32, PropModeReplace,
							(unsigned char *)app_hdr_metadata_blob.data(), (int)app_hdr_metadata_blob.size() );
				}
				else
				{
					XDeleteProperty(root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeColorAppHDRMetadataFeedback);
				}

				g_ColorMgmt.pending.appHDRMetadata	 = app_hdr_metadata;
				flush_root = true;
			}
		}

		// Handles if we got a commit for the window we want to focus
		// to switch to it for painting (outdatedInteractiveFocus)
		// Doesn't realllly matter but avoids an extra frame of being on the wrong window.
		for ( auto &iter : g_VirtualConnectorFocuses )
		{
			global_focus_t *pFocus = &iter.second;
			if ( pFocus->IsDirty() )
				determine_and_apply_focus( pFocus );
		}

		// XXX(misyl): This is bad! We shouldnt change the upscaler like this at all!!!
		// We should move this to business logic in paint_window or something!
		if ( GetCurrentFocus() && window_is_steam( GetCurrentFocus()->focusWindow ) )
		{
			g_bSteamIsActiveWindow = true;
			g_upscaleScaler = GamescopeUpscaleScaler::FIT;
			g_upscaleFilter = GamescopeUpscaleFilter::LINEAR;
		}
		else
		{
			g_bSteamIsActiveWindow = false;
			g_upscaleScaler = g_wantedUpscaleScaler;
			g_upscaleFilter = g_wantedUpscaleFilter;
		}

		// If we're in the middle of a fade, then keep us
		// as needing a repaint.
		if ( is_fading_out() )
			hasRepaint = true;

		bool bPainted = false;

		static int nIgnoredOverlayRepaints = 0;

		if ( !hasRepaintNonBasePlane )
			nIgnoredOverlayRepaints = 0;

		if ( cv_adaptive_sync_ignore_overlay )
			nIgnoredOverlayRepaints = 0;

		for ( auto &iter : g_VirtualConnectorFocuses )
		{
			global_focus_t *pPaintFocus = &iter.second;

			if ( vblank )
			{
				if ( pPaintFocus->cursor )
					pPaintFocus->cursor->UpdatePosition();
			}

			if ( pPaintFocus->GetNestedHints() && !g_bForceRelativeMouse )
			{
				const bool bImageEmpty =
					( pPaintFocus->cursor && pPaintFocus->cursor->imageEmpty() ) &&
					( !window_is_steam( pPaintFocus->inputFocusWindow ) );

				const bool bHasPointerConstraint = pPaintFocus->cursor && pPaintFocus->cursor->IsConstrained();

				uint32_t uAppId = pPaintFocus->inputFocusWindow
					? pPaintFocus->inputFocusWindow->appID
					: 0;

				const bool bExcludedAppId = uAppId && gamescope::Algorithm::Contains( s_uRelativeMouseFilteredAppids, uAppId );

				const bool bRelativeMouseMode = bImageEmpty && bHasPointerConstraint && !bExcludedAppId;

				pPaintFocus->GetNestedHints()->SetRelativeMouseMode( bRelativeMouseMode );
			}

			// HACK: Disable tearing if we have an overlay to avoid stutters right now
			// TODO: Fix properly.
			const bool bHasOverlay = ( pPaintFocus->overlayWindow && pPaintFocus->overlayWindow->opacity ) ||
									( pPaintFocus->externalOverlayWindow && pPaintFocus->externalOverlayWindow->opacity ) ||
									( pPaintFocus->overrideWindow  && pPaintFocus->focusWindow && !pPaintFocus->focusWindow->isSteamStreamingClient && pPaintFocus->overrideWindow->opacity );

			// If we are running behind, allow tearing.

			// A false vblank value means bShouldPaint will resolve to false below, effectively ignoring this flag and losing any request
			// to force a repaint. Don't clear g_bForceRepaint unless vblank is true.
			const bool bForceRepaint = vblank && g_bForceRepaint.exchange(false);
			const bool bForceSyncFlip = bForceRepaint || is_fading_out();

			// If we are compositing, always force sync flips because we currently wait
			// for composition to finish before submitting.
			// If we want to do async + composite, we should set up syncfile stuff and have DRM wait on it.
			const bool bSurfaceWantsAsync = (g_HeldCommits[HELD_COMMIT_BASE] != nullptr && g_HeldCommits[HELD_COMMIT_BASE]->async);
			const bool bTearing = cv_tearing_enabled && GetBackend()->SupportsTearing() && bSurfaceWantsAsync;

			enum class FlipType
			{
				Normal,
				Async,
				VRR,
			};

			FlipType eFlipType = FlipType::Normal;

			if ( bForceSyncFlip )
				eFlipType = FlipType::Normal;
			else if ( bVRR )
				eFlipType = FlipType::VRR;
			else if ( bTearing )
			{
				eFlipType = FlipType::Async;

				if ( nIgnoredOverlayRepaints )
					eFlipType = FlipType::Normal;
				if ( bHasOverlay ) // Don't tear if the Steam or perf overlay is up atm.
					eFlipType = FlipType::Normal;
				if ( GetVBlankTimer().WasCompositing() )
					eFlipType = FlipType::Normal;
			}
			else
				eFlipType = FlipType::Normal;

			bool bShouldPaint = false;

			//if ( GetBackend()->IsVisible() )
			if ( true )
			{
				switch ( eFlipType )
				{
					case FlipType::Normal:
					{
						bShouldPaint = vblank && ( hasRepaint || hasRepaintNonBasePlane || bForceSyncFlip );
						break;
					}

					case FlipType::Async:
					{
						bShouldPaint = hasRepaint;

						if ( vblank && !bShouldPaint && hasRepaintNonBasePlane )
							nIgnoredOverlayRepaints++;

						break;
					}

					case FlipType::VRR:
					{
						bShouldPaint = hasRepaint;

						if ( bIsVBlankFromTimer )
						{
							if ( hasRepaintNonBasePlane )
							{
								if ( nIgnoredOverlayRepaints >= cv_adaptive_sync_overlay_cycles )
								{
									// If we hit vblank and we previously punted on drawing an overlay
									// we should go ahead and draw now.
									bShouldPaint = true;
								}
								else if ( !bShouldPaint )
								{
									// If we hit vblank (ie. fastest refresh cycle since last commit),
									// and we aren't painting and we have a pending overlay, then:
									// defer it until the next game update or next true vblank.
									if ( !cv_adaptive_sync_ignore_overlay )
										nIgnoredOverlayRepaints++;
								}
							}
						}

						// If we have a pending page flip and doing VRR, lets not do another...
						if ( GetBackend()->GetCurrentConnector()->PresentationFeedback().CurrentPresentsInFlight() != 0 )
							bShouldPaint = false;

						break;
					}
				}
			}
			else
			{
				bShouldPaint = false;
			}

			if ( bShouldPaint )
			{
				paint_all( pPaintFocus, eFlipType == FlipType::Async );

				bPainted = true;
			}
		}

		if ( bPainted )
		{
			hasRepaint = false;
			hasRepaintNonBasePlane = false;
			nIgnoredOverlayRepaints = 0;

			{
				gamescope::CScriptScopedLock script;
				script.Manager().CallHook( "OnPostPaint" );
			}
		}

		if ( bIsVBlankFromTimer )
		{
			// Pre-emptively re-arm the vblank timer if it
			// isn't already re-armed.
			//
			// Juuust in case pageflip handler doesn't happen
			// so we don't stop vblanking forever.
			GetVBlankTimer().ArmNextVBlank( true );

#if HAVE_PIPEWIRE
			if ( pipewire_is_streaming() )
				paint_pipewire();
#endif
		}

		update_vrr_atoms(root_ctx, false, &flush_root);

		if (GetCurrentFocus() && GetCurrentFocus()->cursor)
		{
			GetCurrentFocus()->cursor->checkSuspension();

			if (GetCurrentFocus()->cursor->needs_server_flush())
			{
				flush_root = true;
				GetCurrentFocus()->cursor->inform_flush();
			}
		}

		if (flush_root)
		{
			XFlush(root_ctx->dpy);
		}

		vulkan_garbage_collect();

		vblank = false;
	}

	steamcompmgr_exit();
}

struct wlr_surface *steamcompmgr_get_server_input_surface( size_t idx )
{
	gamescope_xwayland_server_t *server = wlserver_get_xwayland_server( idx );
	if ( server && server->ctx && server->ctx->focus.inputFocusWindow && server->ctx->focus.inputFocusWindow->xwayland().surface.main_surface )
		return server->ctx->focus.inputFocusWindow->xwayland().surface.main_surface;
	return NULL;
}

struct wlserver_x11_surface_info *lookup_x11_surface_info_from_xid( gamescope_xwayland_server_t *xwayland_server, uint32_t xid )
{
	if ( !xwayland_server )
		return nullptr;

	if ( !xwayland_server->ctx )
		return nullptr;

	// Lookup children too so we can get the window
	// and go back to it's top-level parent.
	// The xwayland bypass layer does this as we can have child windows
	// that cover the whole parent.
	std::unique_lock lock( xwayland_server->ctx->list_mutex );
	steamcompmgr_win_t *w = find_win( xwayland_server->ctx.get(), xid, true );
	if ( !w )
		return nullptr;

	return &w->xwayland().surface;
}

MouseCursor *steamcompmgr_get_current_cursor()
{
	global_focus_t *pFocus = GetCurrentFocus();
	if ( !pFocus )
		return nullptr;

	return pFocus->cursor;
}

MouseCursor *steamcompmgr_get_server_cursor(uint32_t idx)
{
	gamescope_xwayland_server_t *server = wlserver_get_xwayland_server( idx );
	if ( server && server->ctx )
		return  server->ctx->cursor.get();
	return nullptr;
}
