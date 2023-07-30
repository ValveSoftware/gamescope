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

#include "xwayland_ctx.hpp"
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/extensions/xfixeswire.h>
#include <cstdint>
#include <drm_mode.h>
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
#include <variant>

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

#include "steamcompmgr_shared.hpp"

#include "main.hpp"
#include "wlserver.hpp"
#include "drm.hpp"
#include "rendervulkan.hpp"
#include "steamcompmgr.hpp"
#include "vblankmanager.hpp"
#include "sdlwindow.hpp"
#include "log.hpp"
#include "defer.hpp"

#if HAVE_PIPEWIRE
#include "pipewire.hpp"
#endif

#if HAVE_OPENVR
#include "vr_session.hpp"
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

#define GPUVIS_TRACE_IMPLEMENTATION
#include "gpuvis_trace_utils.h"


static LogScope xwm_log("xwm");


///
// Color Mgmt
//

gamescope_color_mgmt_tracker_t g_ColorMgmt{};

static gamescope_color_mgmt_luts g_ColorMgmtLutsOverride[ EOTF_Count ];
static lut3d_t g_ColorMgmtLooks[ EOTF_Count ];


gamescope_color_mgmt_luts g_ColorMgmtLuts[ EOTF_Count ];

gamescope_color_mgmt_luts g_ScreenshotColorMgmtLuts[ EOTF_Count ];

static lut1d_t g_tmpLut1d;
static lut3d_t g_tmpLut3d;

bool g_bForceHDRSupportDebug = false;
extern float g_flInternalDisplayBrightnessNits;
extern float g_flHDRItmSdrNits;
extern float g_flHDRItmTargetNits;

extern std::atomic<uint64_t> g_lastVblank;

std::string clipboard;
std::string primarySelection;

uint64_t timespec_to_nanos(struct timespec& spec)
{
	return spec.tv_sec * 1'000'000'000ul + spec.tv_nsec;
}

static const gamescope_color_mgmt_t k_ScreenshotColorMgmt =
{
	.enabled = true,
	.displayColorimetry = displaycolorimetry_709,
	.displayEOTF = EOTF_Gamma22,
	.outputEncodingColorimetry = displaycolorimetry_709,
	.outputEncodingEOTF = EOTF_Gamma22,
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
			lut3d_t * pLook = g_ColorMgmtLooks[nInputEOTF].lutEdgeSize > 0 ? &g_ColorMgmtLooks[nInputEOTF] : nullptr;

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

			calcColorTransform( &g_tmpLut1d, s_nLutSize1d, &g_tmpLut3d, s_nLutEdgeSize3d, inputColorimetry, inputEOTF,
				outputEncodingColorimetry, newColorMgmt.outputEncodingEOTF,
				colorMapping, newColorMgmt.nightmode, tonemapping, pLook, flGain );

			// Create quantized output luts
			for ( size_t i=0, end = g_tmpLut1d.dataR.size(); i<end; ++i )
			{
				outColorMgmtLuts[nInputEOTF].lut1d[4*i+0] = drm_quantize_lut_value( g_tmpLut1d.dataR[i] );
				outColorMgmtLuts[nInputEOTF].lut1d[4*i+1] = drm_quantize_lut_value( g_tmpLut1d.dataG[i] );
				outColorMgmtLuts[nInputEOTF].lut1d[4*i+2] = drm_quantize_lut_value( g_tmpLut1d.dataB[i] );
				outColorMgmtLuts[nInputEOTF].lut1d[4*i+3] = 0;
			}

			for ( size_t i=0, end = g_tmpLut3d.data.size(); i<end; ++i )
			{
				outColorMgmtLuts[nInputEOTF].lut3d[4*i+0] = drm_quantize_lut_value( g_tmpLut3d.data[i].r );
				outColorMgmtLuts[nInputEOTF].lut3d[4*i+1] = drm_quantize_lut_value( g_tmpLut3d.data[i].g );
				outColorMgmtLuts[nInputEOTF].lut3d[4*i+2] = drm_quantize_lut_value( g_tmpLut3d.data[i].b );
				outColorMgmtLuts[nInputEOTF].lut3d[4*i+3] = 0;
			}
		}

		outColorMgmtLuts[nInputEOTF].bHasLut1D = true;
		outColorMgmtLuts[nInputEOTF].bHasLut3D = true;

		vulkan_update_luts(outColorMgmtLuts[nInputEOTF].vk_lut1d, outColorMgmtLuts[nInputEOTF].vk_lut3d, outColorMgmtLuts[nInputEOTF].lut1d, outColorMgmtLuts[nInputEOTF].lut3d);
	}
}

static void
update_color_mgmt()
{
	// update pending native display colorimetry
	if ( !BIsNested() )
	{
		drm_get_native_colorimetry( &g_DRM,
			&g_ColorMgmt.pending.displayColorimetry, &g_ColorMgmt.pending.displayEOTF,
			&g_ColorMgmt.pending.outputEncodingColorimetry, &g_ColorMgmt.pending.outputEncodingEOTF );
	}
	else
	{
		g_ColorMgmt.pending.displayColorimetry = displaycolorimetry_709;
		g_ColorMgmt.pending.displayEOTF = EOTF_Gamma22;
		g_ColorMgmt.pending.outputEncodingColorimetry = displaycolorimetry_709;
		g_ColorMgmt.pending.outputEncodingEOTF = EOTF_Gamma22;
	}

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

bool set_color_3dlut_override(const char *path)
{
	int nLutIndex = EOTF_Gamma22;
	g_ColorMgmt.pending.externalDirtyCtr++;
	g_ColorMgmtLutsOverride[nLutIndex].bHasLut3D = false;

	FILE *f = fopen(path, "rb");
	if (!f) {
		return true;
	}

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
	LoadCubeLut( &g_ColorMgmtLooks[EOTF_PQ], path );
	g_ColorMgmt.pending.externalDirtyCtr++;
	return true;
}

bool set_color_look_g22(const char *path)
{
	LoadCubeLut( &g_ColorMgmtLooks[EOTF_Gamma22], path );
	g_ColorMgmt.pending.externalDirtyCtr++;
	return true;
}

bool g_bColorSliderInUse = false;

//
//
//

const uint32_t WS_OVERLAPPED          		= 0x00000000u;
const uint32_t WS_POPUP               		= 0x80000000u;
const uint32_t WS_CHILD               		= 0x40000000u;
const uint32_t WS_MINIMIZE            		= 0x20000000u;
const uint32_t WS_VISIBLE             		= 0x10000000u;
const uint32_t WS_DISABLED            		= 0x08000000u;
const uint32_t WS_CLIPSIBLINGS        		= 0x04000000u;
const uint32_t WS_CLIPCHILDREN        		= 0x02000000u;
const uint32_t WS_MAXIMIZE            		= 0x01000000u;
const uint32_t WS_BORDER              		= 0x00800000u;
const uint32_t WS_DLGFRAME            		= 0x00400000u;
const uint32_t WS_VSCROLL             		= 0x00200000u;
const uint32_t WS_HSCROLL             		= 0x00100000u;
const uint32_t WS_SYSMENU             		= 0x00080000u;
const uint32_t WS_THICKFRAME          		= 0x00040000u;
const uint32_t WS_GROUP               		= 0x00020000u;
const uint32_t WS_TABSTOP             		= 0x00010000u;
const uint32_t WS_MINIMIZEBOX         		= 0x00020000u;
const uint32_t WS_MAXIMIZEBOX         		= 0x00010000u;
const uint32_t WS_CAPTION             		= WS_BORDER | WS_DLGFRAME;
const uint32_t WS_TILED               		= WS_OVERLAPPED;
const uint32_t WS_ICONIC              		= WS_MINIMIZE;
const uint32_t WS_SIZEBOX             		= WS_THICKFRAME;
const uint32_t WS_OVERLAPPEDWINDOW    		= WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME| WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
const uint32_t WS_POPUPWINDOW         		= WS_POPUP | WS_BORDER | WS_SYSMENU;
const uint32_t WS_CHILDWINDOW         		= WS_CHILD;
const uint32_t WS_TILEDWINDOW         		= WS_OVERLAPPEDWINDOW;

const uint32_t WS_EX_DLGMODALFRAME    		= 0x00000001u;
const uint32_t WS_EX_DRAGDETECT       		= 0x00000002u; // Undocumented
const uint32_t WS_EX_NOPARENTNOTIFY   		= 0x00000004u;
const uint32_t WS_EX_TOPMOST          		= 0x00000008u;
const uint32_t WS_EX_ACCEPTFILES      		= 0x00000010u;
const uint32_t WS_EX_TRANSPARENT      		= 0x00000020u;
const uint32_t WS_EX_MDICHILD         		= 0x00000040u;
const uint32_t WS_EX_TOOLWINDOW       		= 0x00000080u;
const uint32_t WS_EX_WINDOWEDGE       		= 0x00000100u;
const uint32_t WS_EX_CLIENTEDGE       		= 0x00000200u;
const uint32_t WS_EX_CONTEXTHELP      		= 0x00000400u;
const uint32_t WS_EX_RIGHT            		= 0x00001000u;
const uint32_t WS_EX_LEFT             		= 0x00000000u;
const uint32_t WS_EX_RTLREADING       		= 0x00002000u;
const uint32_t WS_EX_LTRREADING       		= 0x00000000u;
const uint32_t WS_EX_LEFTSCROLLBAR    		= 0x00004000u;
const uint32_t WS_EX_RIGHTSCROLLBAR   		= 0x00000000u;
const uint32_t WS_EX_CONTROLPARENT    		= 0x00010000u;
const uint32_t WS_EX_STATICEDGE       		= 0x00020000u;
const uint32_t WS_EX_APPWINDOW        		= 0x00040000u;
const uint32_t WS_EX_LAYERED          		= 0x00080000u;
const uint32_t WS_EX_NOINHERITLAYOUT  		= 0x00100000u;
const uint32_t WS_EX_NOREDIRECTIONBITMAP	= 0x00200000u;
const uint32_t WS_EX_LAYOUTRTL        		= 0x00400000u;
const uint32_t WS_EX_COMPOSITED       		= 0x02000000u;
const uint32_t WS_EX_NOACTIVATE       		= 0x08000000u;

template< typename T >
constexpr const T& clamp( const T& x, const T& min, const T& max )
{
    return x < min ? min : max < x ? max : x;
}

extern bool g_bForceRelativeMouse;
bool bSteamCompMgrGrab = false;

CommitDoneList_t g_steamcompmgr_xdg_done_commits;

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
		if (!presentation_feedbacks.empty())
		{
			wlserver_presentation_feedback_discard(surf, presentation_feedbacks);
			// presentation_feedbacks cleared by wlserver_presentation_feedback_discard
		}
		wlr_buffer_unlock( buf );
		wlserver_unlock();
    }

	GamescopeAppTextureColorspace colorspace() const
	{
		VkColorSpaceKHR colorspace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		if (feedback && vulkanTex)
			colorspace = feedback->vk_colorspace;

		if (!vulkanTex)
			return GAMESCOPE_APP_TEXTURE_COLORSPACE_LINEAR;

		return VkColorSpaceToGamescopeAppTextureColorSpace(vulkanTex->format(), colorspace);
	}

	struct wlr_buffer *buf = nullptr;
	uint32_t fb_id = 0;
	std::shared_ptr<CVulkanTexture> vulkanTex;
	uint64_t commitID = 0;
	bool done = false;
	bool async = false;
	std::optional<wlserver_vk_swapchain_feedback> feedback = std::nullopt;

	struct wlr_surface *surf = nullptr;
	std::vector<struct wl_resource*> presentation_feedbacks;
};

static std::vector<pollfd> pollfds;

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

int g_nAsyncFlipsEnabled = 0;
int g_nSteamMaxHeight = 0;
bool g_bVRRCapable_CachedValue = false;
bool g_bVRRInUse_CachedValue = false;
bool g_bSupportsST2084_CachedValue = false;
bool g_bForceHDR10OutputDebug = false;
bool g_bHDREnabled = false;
bool g_bHDRItmEnable = false;

Window x11_win(steamcompmgr_win_t *w) {
	if (w == nullptr)
		return None;
	if (w->type != steamcompmgr_win_type_t::XWAYLAND)
		return None;
	return w->xwayland().id;
}

struct global_focus_t : public focus_t
{
	steamcompmgr_win_t	  	 		*keyboardFocusWindow;
	steamcompmgr_win_t	  	 		*fadeWindow;
	MouseCursor		*cursor;
} global_focus;


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

bool			focusDirty = false;
bool			hasRepaint = false;
bool			hasRepaintNonBasePlane = false;

unsigned long	damageSequence = 0;

unsigned int	cursorHideTime = 10'000;

bool			gotXError = false;

unsigned int	fadeOutStartTime = 0;

unsigned int 	g_FadeOutDuration = 0;

extern float g_flMaxWindowScale;

bool			synchronize;

std::mutex g_SteamCompMgrXWaylandServerMutex;

uint64_t g_SteamCompMgrVBlankTime = 0;

static int g_nSteamCompMgrTargetFPS = 0;
static uint64_t g_uDynamicRefreshEqualityTime = 0;
static int g_nDynamicRefreshRate[DRM_SCREEN_TYPE_COUNT] = { 0, 0 };
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

std::vector<std::shared_ptr<steamcompmgr_win_t>> g_steamcompmgr_xdg_wins;

static bool
window_is_steam( steamcompmgr_win_t *w )
{
	return w && ( w->isSteamLegacyBigPicture || w->appID == 769 );
}

bool steamcompmgr_window_should_limit_fps( steamcompmgr_win_t *w )
{
	return w && !window_is_steam( w ) && !w->isOverlay && !w->isExternalOverlay;
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

static std::atomic<bool> g_bForceRepaint{false};

static int g_nudgePipe[2] = {-1, -1};

static int g_nCursorScaleHeight = -1;

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
	CommitDoneList_t *doneCommits;
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
	struct pollfd fd = { entry.fence, POLLIN, 0 };
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
		std::unique_lock< std::mutex > lock( entry.doneCommits->listCommitsDoneLock );
		entry.doneCommits->listCommitsDone.push_back( entry.commitID );
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
	return timespec_to_nanos(ts);
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
import_commit ( struct wlr_surface *surf, struct wlr_buffer *buf, bool async, std::shared_ptr<wlserver_vk_swapchain_feedback> swapchain_feedback, std::vector<struct wl_resource*> presentation_feedbacks )
{
	std::shared_ptr<commit_t> commit = std::make_shared<commit_t>();
	std::unique_lock<std::mutex> lock( wlr_buffer_map_lock );

	commit->surf = surf;
	commit->buf = buf;
	commit->async = async;
	commit->presentation_feedbacks = std::move(presentation_feedbacks);
	if (swapchain_feedback)
		commit->feedback = *swapchain_feedback;

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
window_last_done_commit_id( steamcompmgr_win_t *w )
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
	return window_last_done_commit_id( w ) != -1;
}

static void
get_window_last_done_commit( steamcompmgr_win_t *w, std::shared_ptr<commit_t> &commit )
{
	int32_t lastCommit = window_last_done_commit_id( w );

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
	int32_t lastCommit = window_last_done_commit_id( w );

	if ( lastCommit == -1 )
	{
		return nullptr;
	}

	return w->commit_queue[ lastCommit ].get();
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
	: m_texture(0)
	, m_dirty(true)
	, m_imageEmpty(false)
	, m_hideForMovement(true)
	, m_ctx(ctx)
{
	m_lastX = g_nNestedWidth / 2;
	m_lastY = g_nNestedHeight / 2;
	updateCursorFeedback( true );
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

	steamcompmgr_win_t *window = m_ctx->focus.inputFocusWindow;
	if (window && window->ignoreNextClickForVisibility)
	{
		window->ignoreNextClickForVisibility--;
		m_hideForMovement = true;
		return;
	}
	else
	{
		if (buttonMask & ( Button1Mask | Button2Mask | Button3Mask | Button4Mask | Button5Mask )) {
			m_hideForMovement = false;
			m_lastMovedTime = get_time_in_milliseconds();

			// Move the cursor back to where we left it if the window didn't want us to give
			// it hover/focus where we left it and we moved it before.
			if (window_wants_no_focus_when_mouse_hidden(window) && bWasHidden)
			{
				XWarpPointer(m_ctx->dpy, None, x11_win(m_ctx->focus.inputFocusWindow), 0, 0, 0, 0, m_lastX, m_lastY);
			}
		}
	}

	const bool suspended = get_time_in_milliseconds() - m_lastMovedTime > cursorHideTime;
	if (!m_hideForMovement && suspended) {
		m_hideForMovement = true;

		// Rearm warp count
		if (window) {
			window->mouseMoved = 0;

			// Move the cursor to the bottom right corner, just off screen if we can
			// if the window (ie. Steam) doesn't want hover/focus events.
			if ( window_wants_no_focus_when_mouse_hidden(window) )
			{
				m_lastX = m_x;
				m_lastY = m_y;
				XWarpPointer(m_ctx->dpy, None, x11_win(m_ctx->focus.inputFocusWindow), 0, 0, 0, 0, window->xwayland().a.width - 1, window->xwayland().a.height - 1);
			}
		}

		// We're hiding the cursor, force redraw if we were showing it
		if (window && !m_imageEmpty ) {
			hasRepaintNonBasePlane = true;
			nudge_steamcompmgr();
		}
	}

	updateCursorFeedback();
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

bool MouseCursor::setCursorImageByName(const char *name)
{
	int screen = DefaultScreen(m_ctx->dpy);

	XColor fg;
	fg.pixel = WhitePixel(m_ctx->dpy, screen);
	XQueryColor(m_ctx->dpy, DefaultColormap(m_ctx->dpy, screen), &fg);

	XColor bg;
	bg.pixel = BlackPixel(m_ctx->dpy, screen);
	XQueryColor(m_ctx->dpy, DefaultColormap(m_ctx->dpy, screen), &bg);

	int index = XmuCursorNameToIndex(name);
	if (index < 0)
		return false;

	Font font = XLoadFont(m_ctx->dpy, "cursor");
	if (!font)
		return false;
	defer( XUnloadFont(m_ctx->dpy, font) );

	Cursor cursor = XCreateGlyphCursor(m_ctx->dpy, font, font, index, index + 1, &fg, &bg);
	if ( !cursor )
		return false;
	defer( XFreeCursor(m_ctx->dpy, cursor) );

	XDefineCursor(m_ctx->dpy, DefaultRootWindow(m_ctx->dpy), cursor);
	XFlush(m_ctx->dpy);
	setDirty();
	return true;
}

void MouseCursor::constrainPosition()
{
	int i;
	steamcompmgr_win_t *window = m_ctx->focus.inputFocusWindow;
	steamcompmgr_win_t *override = m_ctx->focus.overrideWindow;
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

	int x1 = window->xwayland().a.x;
	int y1 = window->xwayland().a.y;
	if (override)
	{
		x1 = std::min(x1, override->xwayland().a.x);
		y1 = std::min(y1, override->xwayland().a.y);
	}
	int x2 = window->xwayland().a.x + window->xwayland().a.width;
	int y2 = window->xwayland().a.y + window->xwayland().a.height;
	if (override)
	{
		x2 = std::max(x2, override->xwayland().a.x + override->xwayland().a.width);
		y2 = std::max(y2, override->xwayland().a.y + override->xwayland().a.height);
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
			warp(window->xwayland().a.width - 1, window->xwayland().a.height - 1);
		else
			warp(window->xwayland().a.width / 2, window->xwayland().a.height / 2);

		m_lastX = window->xwayland().a.width / 2;
		m_lastY = window->xwayland().a.height / 2;
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

	steamcompmgr_win_t *window = m_ctx->focus.inputFocusWindow;

	if (window) {
		// If mouse moved and we're on the hook for showing the cursor, repaint
		if (!m_hideForMovement && !m_imageEmpty) {
			hasRepaintNonBasePlane = true;
		}

		// If mouse moved and screen is magnified, repaint
		if ( zoomScaleRatio != 1.0 )
		{
			hasRepaintNonBasePlane = true;
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
	updateCursorFeedback();
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

	std::shared_ptr<std::vector<uint32_t>> cursorBuffer = nullptr;

	if (image->width && image->height)
	{
		cursorBuffer = std::make_shared<std::vector<uint32_t>>(surfaceWidth * surfaceHeight);
		for (int i = 0; i < image->height; i++) {
			for (int j = 0; j < image->width; j++) {
				(*cursorBuffer)[i * surfaceWidth + j] = image->pixels[i * image->width + j];

				if ( (*cursorBuffer)[i * surfaceWidth + j] & 0xff000000 ) {
					bNoCursor = false;
				}
			}
		}
	}

	if (bNoCursor)
		cursorBuffer = nullptr;

	m_imageEmpty = bNoCursor;

	if ( !g_bForceRelativeMouse )
	{
		sdlwindow_grab( m_imageEmpty );
		bSteamCompMgrGrab = BIsNested() && m_imageEmpty;
	}

	m_dirty = false;
	updateCursorFeedback();

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

	m_texture = vulkan_create_texture_from_bits(surfaceWidth, surfaceHeight, image->width, image->height, DRM_FORMAT_ARGB8888, texCreateFlags, cursorBuffer->data());
	sdlwindow_cursor(std::move(cursorBuffer), image->width, image->height, image->xhot, image->yhot);
	assert(m_texture);
	XFree(image);

	return true;
}

void MouseCursor::paint(steamcompmgr_win_t *window, steamcompmgr_win_t *fit, struct FrameInfo_t *frameInfo)
{
	if ( m_hideForMovement || m_imageEmpty ) {
		return;
	}

	int rootX, rootY, winX, winY;
	queryPositions(rootX, rootY, winX, winY);
	move(rootX, rootY);

	// Also need new texture
	if (!getTexture()) {
		return;
	}

	uint32_t sourceWidth = window->xwayland().a.width;
	uint32_t sourceHeight = window->xwayland().a.height;

	if ( fit )
	{
		// If we have an override window, try to fit it in as long as it won't make our scale go below 1.0.
		sourceWidth = std::max<uint32_t>( sourceWidth, clamp<int>( fit->xwayland().a.x + fit->xwayland().a.width, 0, currentOutputWidth ) );
		sourceHeight = std::max<uint32_t>( sourceHeight, clamp<int>( fit->xwayland().a.y + fit->xwayland().a.height, 0, currentOutputHeight ) );
	}

	float cursor_scale = 1.0f;
	if ( g_nCursorScaleHeight > 0 )
	{
		cursor_scale = floor(currentOutputHeight / (float)g_nCursorScaleHeight);
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
	scaledX = (winX - window->xwayland().a.x) * currentScaleRatio_x + cursorOffsetX;
	scaledY = (winY - window->xwayland().a.y) * currentScaleRatio_y + cursorOffsetY;

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
	layer->fbid = BIsNested() ? 0 : m_texture->fbid();

	layer->linearFilter = cursor_scale != 1.0f;
	layer->blackBorder = false;
	layer->colorspace = GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB;
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

	layer->colorspace = commit->colorspace();
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

wlserver_vk_swapchain_feedback* steamcompmgr_get_base_layer_swapchain_feedback()
{
	if ( !g_HeldCommits[ HELD_COMMIT_BASE ] )
		return nullptr;

	if ( !g_HeldCommits[ HELD_COMMIT_BASE ]->feedback )
		return nullptr;

	return &(*g_HeldCommits[ HELD_COMMIT_BASE ]->feedback);
}

static void
paint_window(steamcompmgr_win_t *w, steamcompmgr_win_t *scaleW, struct FrameInfo_t *frameInfo,
			  MouseCursor *cursor, PaintWindowFlags flags = 0, float flOpacityScale = 1.0f, steamcompmgr_win_t *fit = nullptr )
{
	uint32_t sourceWidth, sourceHeight;
	int drawXOffset = 0, drawYOffset = 0;
	float currentScaleRatio_x = 1.0;
	float currentScaleRatio_y = 1.0;
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

	steamcompmgr_win_t *mainOverlayWindow = global_focus.overlayWindow;

	const bool notificationMode = flags & PaintWindowFlag::NotificationMode;
	if (notificationMode && !mainOverlayWindow)
		return;

	if (notificationMode)
	{
		sourceWidth = mainOverlayWindow->xwayland().a.width;
		sourceHeight = mainOverlayWindow->xwayland().a.height;
	}
	else if ( flags & PaintWindowFlag::NoScale )
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
			sourceWidth = lastCommit->vulkanTex->width();
			sourceHeight = lastCommit->vulkanTex->height();
		} else {
			sourceWidth = scaleW->xwayland().a.width;
			sourceHeight = scaleW->xwayland().a.height;
		}

		if ( fit )
		{
			// If we have an override window, try to fit it in as long as it won't make our scale go below 1.0.
			sourceWidth = std::max<uint32_t>( sourceWidth, clamp<int>( fit->xwayland().a.x + fit->xwayland().a.width, 0, currentOutputWidth ) );
			sourceHeight = std::max<uint32_t>( sourceHeight, clamp<int>( fit->xwayland().a.y + fit->xwayland().a.height, 0, currentOutputHeight ) );
		}
	}

	bool offset = false;
	if ( w->type == steamcompmgr_win_type_t::XWAYLAND )
		offset = ( ( w->xwayland().a.x || w->xwayland().a.y ) && w != scaleW );

	if (sourceWidth != currentOutputWidth || sourceHeight != currentOutputHeight || offset || globalScaleRatio != 1.0f)
	{
		calc_scale_factor(currentScaleRatio_x, currentScaleRatio_y, sourceWidth, sourceHeight);

		drawXOffset = ((int)currentOutputWidth - (int)sourceWidth * currentScaleRatio_x) / 2.0f;
		drawYOffset = ((int)currentOutputHeight - (int)sourceHeight * currentScaleRatio_y) / 2.0f;

		if ( w->type == steamcompmgr_win_type_t::XWAYLAND && w != scaleW )
		{
			drawXOffset += w->xwayland().a.x * currentScaleRatio_x;
			drawYOffset += w->xwayland().a.y * currentScaleRatio_y;
		}

		if ( zoomScaleRatio != 1.0 )
		{
			drawXOffset += (((int)sourceWidth / 2) - cursor->x()) * currentScaleRatio_x;
			drawYOffset += (((int)sourceHeight / 2) - cursor->y()) * currentScaleRatio_y;
		}
	}

	int curLayer = frameInfo->layerCount++;

	FrameInfo_t::Layer_t *layer = &frameInfo->layers[ curLayer ];

	layer->opacity = ( (w->isOverlay || w->isExternalOverlay) ? w->opacity / (float)OPAQUE : 1.0f ) * flOpacityScale;

	layer->scale.x = 1.0 / currentScaleRatio_x;
	layer->scale.y = 1.0 / currentScaleRatio_y;

	if ( w != scaleW )
	{
		layer->offset.x = -drawXOffset;
		layer->offset.y = -drawYOffset;
	}
	else if (notificationMode)
	{
		int xOffset = 0, yOffset = 0;

		int width = w->xwayland().a.width * currentScaleRatio_x;
		int height = w->xwayland().a.height * currentScaleRatio_y;

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

	layer->applyColorMgmt = true;
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

	layer->linearFilter = (w->isOverlay || w->isExternalOverlay) ? true : g_upscaleFilter != GamescopeUpscaleFilter::NEAREST;
	layer->colorspace = lastCommit->colorspace();

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
paint_all(bool async)
{
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
	frameInfo.applyOutputColorMgmt = true;

	// If the window we'd paint as the base layer is the streaming client,
	// find the video underlay and put it up first in the scenegraph
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

				bool needsScaling = frameInfo.layers[0].scale.x < 0.999f && frameInfo.layers[0].scale.y < 0.999f;
				frameInfo.useFSRLayer0 = g_upscaleFilter == GamescopeUpscaleFilter::FSR && needsScaling;
				frameInfo.useNISLayer0 = g_upscaleFilter == GamescopeUpscaleFilter::NIS && needsScaling;
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

	if (input)
	{
		// Make sure to un-dirty the texture before we do any painting logic.
		// We determine whether we are grabbed etc this way.
		global_focus.cursor->undirty();
	}

	bool bForceHideCursor = BIsSDLSession() && !bSteamCompMgrGrab;

	bool bDrewCursor = false;

	// Draw cursor if we need to
	if (input && !bForceHideCursor) {
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

	int nDynamicRefresh = g_nDynamicRefreshRate[drm_get_screen_type( &g_DRM )];

	int nTargetRefresh = nDynamicRefresh && steamcompmgr_window_should_limit_fps( global_focus.focusWindow )// && !global_focus.overlayWindow
		? nDynamicRefresh
		: drm_get_default_refresh( &g_DRM );

	uint64_t now = get_time_in_nanos();

	if ( g_nOutputRefresh == nTargetRefresh )
		g_uDynamicRefreshEqualityTime = now;

	if ( !BIsNested() && g_nOutputRefresh != nTargetRefresh && g_uDynamicRefreshEqualityTime + g_uDynamicRefreshDelay < now )
		drm_set_refresh( &g_DRM, nTargetRefresh );

	bool bNeedsNearest = g_upscaleFilter == GamescopeUpscaleFilter::NEAREST && frameInfo.layers[0].scale.x != 1.0f && frameInfo.layers[0].scale.y != 1.0f;


	bool bNeedsComposite = BIsNested();
	bNeedsComposite |= alwaysComposite;
	bNeedsComposite |= pw_buffer != nullptr;
	bNeedsComposite |= bWasFirstFrame;
	bNeedsComposite |= frameInfo.useFSRLayer0;
	bNeedsComposite |= frameInfo.useNISLayer0;
	bNeedsComposite |= frameInfo.blurLayer0;
	bNeedsComposite |= bNeedsNearest;
	bNeedsComposite |= bDrewCursor;
	bNeedsComposite |= g_bColorSliderInUse;

	for (uint32_t i = 0; i < EOTF_Count; i++)
	{
		if (g_ColorMgmtLuts[i].HasLuts())
		{
			frameInfo.shaperLut[i] = g_ColorMgmtLuts[i].vk_lut1d;
			frameInfo.lut3D[i] = g_ColorMgmtLuts[i].vk_lut3d;
		}
	}

	if ( !BIsNested() && g_bOutputHDREnabled )
	{
		bNeedsComposite |= g_bHDRItmEnable;
		if ( !drm_supports_color_mgmt(&g_DRM) )
			bNeedsComposite |= ( frameInfo.layerCount > 1 || frameInfo.layers[0].colorspace != GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ );
	}
	bNeedsComposite |= !!(g_uCompositeDebug & CompositeDebugFlag::Heatmap);

	if ( !bNeedsComposite )
	{
		int ret = drm_prepare( &g_DRM, async, &frameInfo );
		if ( ret == 0 )
			bDoComposite = false;
		else if ( ret == -EACCES )
			return;
	}

	// Update to let the vblank manager know we are currently compositing.
	g_bCurrentlyCompositing = bDoComposite;

	if ( bDoComposite == true )
	{
		std::shared_ptr<CVulkanTexture> pPipewireTexture = nullptr;
#if HAVE_PIPEWIRE
		if ( pw_buffer != nullptr )
		{
			pPipewireTexture = pw_buffer->texture;
		}
#endif
		bool bResult = vulkan_composite( &frameInfo, pPipewireTexture );

		if ( bResult != true )
		{
			xwm_log.errorf("vulkan_composite failed");
			return;
		}

		if ( BIsNested() == true )
		{
#if HAVE_OPENVR
			if ( BIsVRSession() )
			{
				vulkan_present_to_openvr();
			}
			else if ( BIsSDLSession() )
#endif
			{
				vulkan_present_to_window();
			}
			// Update the time it took us to present.
			// TODO: Use Vulkan present timing in future.
			g_uVblankDrawTimeNS = get_time_in_nanos() - g_SteamCompMgrVBlankTime;
		}
		else
		{
			struct FrameInfo_t compositeFrameInfo = {};
			compositeFrameInfo = {};
			compositeFrameInfo.applyOutputColorMgmt = false;
			compositeFrameInfo.layerCount = 1;
			FrameInfo_t::Layer_t *layer = &compositeFrameInfo.layers[ 0 ];
			layer->scale.x = 1.0;
			layer->scale.y = 1.0;
			layer->opacity = 1.0;

			layer->tex = vulkan_get_last_output_image();
			layer->fbid = layer->tex->fbid();
			layer->applyColorMgmt = false;

			layer->linearFilter = false;
			layer->colorspace = g_bOutputHDREnabled ? GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ : GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB;

			int ret = drm_prepare( &g_DRM, async, &compositeFrameInfo );

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
				ret = drm_prepare( &g_DRM, async, &compositeFrameInfo );

				// Happens when we're VT-switched away
				if ( ret == -EACCES )
					return;

				if ( ret != 0 )
				{
					xwm_log.errorf("Failed to prepare 1-layer flip entirely: %s", strerror( -ret ));
					// We should always handle a 1-layer flip, this used to abort,
					// but lets be more friendly and just avoid a commit and try again later.
					// Let's re-poll our state, and force grab the best connector again.
					//
					// Some intense connector hotplugging could be occuring and the
					// connector could become destroyed before we had a chance to use it
					// as we hadn't reffed it in a commit yet.
					g_DRM.out_of_date = 2;
					drm_poll_state( &g_DRM );
					return;
				}
			}

			drm_commit( &g_DRM, &compositeFrameInfo );
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

	if ( takeScreenshot )
	{
		constexpr bool bHackForceNV12DumpScreenshot = false;

		uint32_t drmCaptureFormat = bHackForceNV12DumpScreenshot
			? DRM_FORMAT_NV12
			: DRM_FORMAT_XRGB8888;

		std::shared_ptr<CVulkanTexture> pScreenshotTexture = vulkan_acquire_screenshot_texture(g_nOutputWidth, g_nOutputHeight, false, drmCaptureFormat);

		assert( pScreenshotTexture != nullptr );

		// Basically no color mgmt applied for screenshots. (aside from being able to handle HDR content with LUTs)
		for ( uint32_t nInputEOTF = 0; nInputEOTF < EOTF_Count; nInputEOTF++ )
		{
			frameInfo.lut3D[nInputEOTF] = g_ScreenshotColorMgmtLuts[nInputEOTF].vk_lut3d;
			frameInfo.shaperLut[nInputEOTF] = g_ScreenshotColorMgmtLuts[nInputEOTF].vk_lut1d;
		}
		// Remove everything but base planes from the screenshot.
		for (int i = 0; i < frameInfo.layerCount; i++)
		{
			if (frameInfo.layers[i].zpos >= (int)g_zposExternalOverlay)
			{
				frameInfo.layerCount = i;
				break;
			}
		}

		bool bResult = vulkan_screenshot( &frameInfo, pScreenshotTexture );
		if ( bResult != true )
		{
			xwm_log.errorf("vulkan_screenshot failed");
			return;
		}

		std::thread screenshotThread = std::thread([=] {
			pthread_setname_np( pthread_self(), "gamescope-scrsh" );

			const uint8_t *mappedData = pScreenshotTexture->mappedData();

			if (pScreenshotTexture->format() == VK_FORMAT_B8G8R8A8_UNORM)
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
			}
			else if (pScreenshotTexture->format() == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
			{
				char pTimeBuffer[1024] = "/tmp/gamescope.raw";

				if ( !propertyRequestedScreenshot )
				{
					time_t currentTime = time(0);
					struct tm *localTime = localtime( &currentTime );
					strftime( pTimeBuffer, sizeof( pTimeBuffer ), "/tmp/gamescope_%Y-%m-%d_%H-%M-%S.raw", localTime );
				}

				FILE *file = fopen(pTimeBuffer, "wb");
				if (file)
				{
					fwrite(mappedData, 1, pScreenshotTexture->totalSize(), file );
					fclose(file);

					char cmd[4096];
					sprintf(cmd, "ffmpeg -f rawvideo -pixel_format nv12 -video_size %dx%d -i %s %s_encoded.png", pScreenshotTexture->width(), pScreenshotTexture->height(), pTimeBuffer, pTimeBuffer);

					int ret = system(cmd);

					/* Above call may fail, ffmpeg returns 0 on success */
					if (ret) {
						xwm_log.infof("Ffmpeg call return status %i", ret);
						xwm_log.errorf( "Failed to save screenshot to %s", pTimeBuffer );
					} else {
						xwm_log.infof("Screenshot saved to %s", pTimeBuffer);
					}
				}
				else
				{
					xwm_log.errorf( "Failed to save screenshot to %s", pTimeBuffer );
				}
			}

			XDeleteProperty( root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeScreenShotAtom );
		});

		screenshotThread.detach();

		takeScreenshot = false;
	}

	gpuvis_trace_end_ctx_printf( paintID, "paint_all" );
	gpuvis_trace_printf( "paint_all %i layers, composite %i", (int)frameInfo.layerCount, bDoComposite );
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

static bool
win_has_game_id( steamcompmgr_win_t *w )
{
	return w->appID != 0;
}

static bool
win_is_useless( steamcompmgr_win_t *w )
{
	if (w->type != steamcompmgr_win_type_t::XWAYLAND)
		return false;

	// Windows that are 1x1 are pretty useless for override redirects.
	// Just ignore them.
	// Fixes the Xbox Login in Age of Empires 2: DE.
	return w->xwayland().a.width == 1 && w->xwayland().a.height == 1;
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

	return override != focus && override->xwayland().a.x >= 0 && override->xwayland().a.y >= 0;
} 

static bool
pick_primary_focus_and_override(focus_t *out, Window focusControlWindow, const std::vector<steamcompmgr_win_t*>& vecPossibleFocusWindows, bool globalFocus, const std::vector<uint32_t>& ctxFocusControlAppIDs)
{
	bool localGameFocused = false;
	steamcompmgr_win_t *focus = NULL, *override_focus = NULL;

	bool controlledFocus = focusControlWindow != None || !ctxFocusControlAppIDs.empty();
	if ( controlledFocus )
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
				// HACK: Bring any Wayland windows to global focus for now
				// until appID stuff is plumbed for them.
				if ( focusable_window->type == steamcompmgr_win_type_t::XDG )
				{
					focus = focusable_window;
					localGameFocused = true;
					goto found;
				}

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

static void
determine_and_apply_focus(xwayland_ctx_t *ctx, std::vector<steamcompmgr_win_t*>& vecGlobalPossibleFocusWindows)
{
	steamcompmgr_win_t *w;
	steamcompmgr_win_t *inputFocus = NULL;

	steamcompmgr_win_t *prevFocusWindow = ctx->focus.focusWindow;
	ctx->focus.overlayWindow = nullptr;
	ctx->focus.notificationWindow = nullptr;
	ctx->focus.overrideWindow = nullptr;
	ctx->focus.externalOverlayWindow = nullptr;

	unsigned int maxOpacity = 0;
	unsigned int maxOpacityExternal = 0;
	std::vector< steamcompmgr_win_t* > vecPossibleFocusWindows;
	for (w = ctx->list; w; w = w->xwayland().next)
	{
		// Always skip system tray icons
		if ( w->isSysTrayIcon )
		{
			continue;
		}

		if ( w->xwayland().a.map_state == IsViewable && w->xwayland().a.c_class == InputOutput && w->isOverlay == false && w->isExternalOverlay == false &&
			( win_has_game_id( w ) || window_is_steam( w ) || w->isSteamStreamingClient ) &&
			 (w->opacity > TRANSLUCENT || w->isSteamStreamingClient == true ) )
		{
			vecPossibleFocusWindows.push_back( w );
		}

		if (w->isOverlay)
		{
			if (w->xwayland().a.width > 1200 && w->opacity >= maxOpacity)
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
		XChangeProperty(ctx->dpy, ctx->focus.focusWindow->xwayland().id, ctx->atoms.WMStateAtom, ctx->atoms.WMStateAtom, 32,
					PropModeReplace, (unsigned char *)wmState,
					sizeof(wmState) / sizeof(wmState[0]));

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

	if ( inputFocus && inputFocus->inputFocusMode == 2 )
		keyboardFocusWin = ctx->focus.focusWindow;

	Window keyboardFocusWindow = keyboardFocusWin ? keyboardFocusWin->xwayland().id : None;

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

	if (w->xwayland().a.x != 0 || w->xwayland().a.y != 0)
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

		if ( w->xwayland().a.width != fs_width || w->xwayland().a.height != fs_height || globalScaleRatio != 1.0f )
			XResizeWindow(ctx->dpy, ctx->focus.focusWindow->xwayland().id, fs_width, fs_height);
	}
	else
	{
		if (ctx->focus.focusWindow->sizeHintsSpecified &&
			((unsigned)ctx->focus.focusWindow->xwayland().a.width != ctx->focus.focusWindow->requestedWidth ||
			(unsigned)ctx->focus.focusWindow->xwayland().a.height != ctx->focus.focusWindow->requestedHeight))
		{
			XResizeWindow(ctx->dpy, ctx->focus.focusWindow->xwayland().id, ctx->focus.focusWindow->requestedWidth, ctx->focus.focusWindow->requestedHeight);
		}
	}

	if ( inputFocus )
	{
		// Cannot simply XWarpPointer here as we immediately go on to
		// do wlserver_mousefocus and need to update m_x and m_y of the cursor.
		if ( bResetToCorner )
		{
			inputFocus->mouseMoved = 0;
			ctx->cursor->forcePosition(inputFocus->xwayland().a.width - 1, inputFocus->xwayland().a.height - 1);
		}
		else if ( bResetToCenter )
		{
			inputFocus->mouseMoved = 0;
			ctx->cursor->forcePosition(inputFocus->xwayland().a.width / 2, inputFocus->xwayland().a.height / 2);
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
	std::vector< steamcompmgr_win_t* > vecPossibleFocusWindows;
	{
		gamescope_xwayland_server_t *server = NULL;
		for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
		{
			determine_and_apply_focus(server->ctx.get(), vecPossibleFocusWindows);
		}
	}

	for ( const auto& xdg_win : g_steamcompmgr_xdg_wins )
	{
		if ( xdg_win->xdg().surface.mapped )
			vecPossibleFocusWindows.push_back( xdg_win.get() );
	}

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
	{
		if (global_focus.inputFocusWindow->type == steamcompmgr_win_type_t::XWAYLAND)
			global_focus.cursor = global_focus.inputFocusWindow->xwayland().ctx->cursor.get();
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
			if ( win_surface(global_focus.inputFocusWindow) != nullptr && global_focus.cursor )
				wlserver_mousefocus( global_focus.inputFocusWindow->main_surface(), global_focus.cursor->x(), global_focus.cursor->y() );

			if ( win_surface(global_focus.keyboardFocusWindow) != nullptr )
				wlserver_keyboardfocus( global_focus.keyboardFocusWindow->main_surface() );
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
		previous_focus.overrideWindow        != global_focus.overrideWindow)
	{
		hasRepaintNonBasePlane = true;
	}

	if (previous_focus.focusWindow           != global_focus.focusWindow)
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
		focusedWindow = (unsigned long)global_focus.focusWindow->id();
		focusedBaseAppId = global_focus.focusWindow->appID;
		focusedAppId = global_focus.inputFocusWindow->appID;
		focused_display = get_win_display_name(global_focus.focusWindow);
		focusWindow_pid = global_focus.focusWindow->pid;
	}

	if ( global_focus.inputFocusWindow )
	{
		focused_mouse_display = get_win_display_name(global_focus.inputFocusWindow);
	}

	if ( global_focus.keyboardFocusWindow )
	{
		focused_keyboard_display = get_win_display_name(global_focus.keyboardFocusWindow);
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

	XFlush( root_ctx->dpy );

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

	// Set SDL window title
	if ( global_focus.focusWindow )
	{
#if HAVE_OPENVR
		if ( BIsVRSession() )
		{
			const char *title = global_focus.focusWindow->title
				? global_focus.focusWindow->title->c_str()
				: nullptr;
			vrsession_title( title, global_focus.focusWindow->icon );
		}
#endif

		if ( BIsSDLSession() )
		{
			sdlwindow_title( global_focus.focusWindow->title, global_focus.focusWindow->icon );
		}
	}

#if HAVE_OPENVR
	if ( BIsVRSession() )
	{
		vrsession_set_dashboard_visible( global_focus.focusWindow != nullptr );
	}
	else
#endif
	{
		if ( BIsSDLSession() )
		{
			sdlwindow_visible( global_focus.focusWindow != nullptr );
		}
	}

	// Some games such as Disgaea PC (405900) don't take controller input until
	// the window is first clicked on despite it having focus.
	if ( global_focus.inputFocusWindow && global_focus.inputFocusWindow->appID == 405900 )
	{
		global_focus.inputFocusWindow->mouseMoved = 0;
		global_focus.inputFocusWindow->ignoreNextClickForVisibility = 2;

		auto now = get_time_in_milliseconds();

		wlserver_lock();
		wlserver_touchdown( 0.5, 0.5, 0, now );
		wlserver_touchup( 0, now + 1 );
		wlserver_unlock();
	}

	focusDirty = false;
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
					attribs.width <= w->xwayland().a.width &&
					attribs.height <= w->xwayland().a.height)
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
get_motif_hints( xwayland_ctx_t *ctx, steamcompmgr_win_t *w )
{
	if ( w->motif_hints )
		XFree( w->motif_hints );

	Atom actual_type;
	int actual_format;
	unsigned long nitems, bytesafter;
	XGetWindowProperty(ctx->dpy, w->xwayland().id, ctx->atoms.motifWMHints, 0L, 20L, False,
		ctx->atoms.motifWMHints, &actual_type, &actual_format, &nitems,
		&bytesafter, (unsigned char **)&w->motif_hints );
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

	get_size_hints(ctx, w);
	get_motif_hints(ctx, w);

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

	focusDirty = true;
}

static void
finish_unmap_win(xwayland_ctx_t *ctx, steamcompmgr_win_t *w)
{
	// TODO clear done commits here?

	/* don't care about properties anymore */
	set_ignore(ctx, NextRequest(ctx->dpy));
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
	set_ignore(ctx, NextRequest(ctx->dpy));
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
		set_ignore(ctx, NextRequest(ctx->dpy));
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
	new_win->motif_hints = nullptr;

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

	new_win->mouseMoved = 0;

	wlserver_x11_surface_info_init( &new_win->xwayland().surface, ctx->xwayland_server, id );

	new_win->xwayland().next = *p;
	*p = new_win;
	if (new_win->xwayland().a.map_state == IsViewable)
		map_win(ctx, id, sequence);

	focusDirty = true;
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

		focusDirty = true;
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
			focusDirty = true;

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

	focusDirty = true;
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
			*prev = w->xwayland().next;
			if (w->xwayland().damage != None)
			{
				set_ignore(ctx, NextRequest(ctx->dpy));
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
	steamcompmgr_win_t	*w = find_win(ctx, de->drawable);
	steamcompmgr_win_t *focus = ctx->focus.focusWindow;

	if (!w)
		return;

	if ((w->isOverlay || w->isExternalOverlay) && !w->opacity)
		return;

	// First damage event we get, compute focus; we only want to focus damaged
	// windows to have meaningful frames.
	if (w->appID && w->xwayland().damage_sequence == 0)
		focusDirty = true;

	w->xwayland().damage_sequence = damageSequence++;

	// If we just passed the focused window, we might be eliglible to take over
	if ( focus && focus != w && w->appID &&
		w->xwayland().damage_sequence > focus->xwayland().damage_sequence)
		focusDirty = true;

	// Josh: This will sometimes cause a BadDamage error.
	// I looked around at different compositors to see what
	// they do here and they just seem to ignore it.
	if (w->xwayland().damage)
	{
		set_ignore(ctx, NextRequest(ctx->dpy));
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

	// If we already focused on our side and are handling this late,
	// let wayland know now.
	if ( w == global_focus.inputFocusWindow )
		wlserver_mousefocus( main_surface );

	steamcompmgr_win_t *keyboardFocusWindow = global_focus.inputFocusWindow;

	if ( keyboardFocusWindow && keyboardFocusWindow->inputFocusMode == 2 )
		keyboardFocusWindow = global_focus.focusWindow;

	if ( w == keyboardFocusWindow )
		wlserver_keyboardfocus( main_surface );

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
		/* Wine will request iconic state and cannot ensure that the WM has
		 * agreed on it; immediately revert to normal state to avoid being
		 * stuck in a paused state. */
		xwm_log.debugf("Rejecting WM_CHANGE_STATE to ICONIC for window 0x%lx", w->xwayland().id);
		uint32_t wmState[] = { ICCCM_NORMAL_STATE, None };
		XChangeProperty(ctx->dpy, w->xwayland().id, ctx->atoms.WMStateAtom, ctx->atoms.WMStateAtom, 32,
			PropModeReplace, (unsigned char *)wmState,
			sizeof(wmState) / sizeof(wmState[0]));
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

static void x11_set_selection(xwayland_ctx_t *ctx, std::string contents, int selectionTarget)
{
	Atom target;
	if (selectionTarget == CLIPBOARD)
	{
		target = ctx->atoms.clipboard;
	}
	else if (selectionTarget == PRIMARYSELECTION)
	{
		target = ctx->atoms.primarySelection;
	}
	else
	{
		return;
	}

	XSetSelectionOwner(ctx->dpy, target, ctx->ourWindow, CurrentTime);
	XChangeProperty(ctx->dpy, ctx->ourWindow, target, ctx->atoms.utf8StringAtom, 8, PropModeReplace,
			(unsigned char *)contents.c_str(), contents.length());
	XFlush(ctx->dpy);
}

void gamescope_set_selection(std::string contents, int selection)
{
	if (selection == CLIPBOARD)
	{
		clipboard = contents;
	}
	else if (selection == PRIMARYSELECTION)
	{
		primarySelection = contents;
	}

	for (int i = 0; i < g_nXWaylandCount; i++)
	{
		gamescope_xwayland_server_t *server = wlserver_get_xwayland_server(i);
		xwayland_ctx_t *ctx = server->ctx.get();
		x11_set_selection(ctx, contents, selection);
	}
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
			XA_STRING,
		};

		XChangeProperty(ctx->dpy, ev->requestor, ev->property, XA_ATOM, 32, PropModeReplace,
				(unsigned char *)&targetList, 2);
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
	int selection;

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

			if (ev->selection == ctx->atoms.clipboard)
			{
				selection = CLIPBOARD;
			}
			else if (ev->selection == ctx->atoms.primarySelection)
			{
				selection = PRIMARYSELECTION;
			}
			else
			{
				xwm_log.errorf( "Selection '%s' not supported.  Ignoring", XGetAtomName(ctx->dpy, ev->selection) );
				goto done;
			}

			if (BIsNested())
			{
				/*
				 * gamescope_set_selection() doesn't need to be called here.
				 * sdlwindow_set_selection triggers a clipboard update, which
				 * then indirectly ccalls gamescope_set_selection()
				*/
				sdlwindow_set_selection(contents, selection);
			}
			else
			{
				gamescope_set_selection(contents, selection);
			}

done:
			XFree(data);
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

static void
steamcompmgr_latch_frame_done( steamcompmgr_win_t *w, uint64_t vblank_idx )
{
	bool bSendCallback = true;

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
		w->unlockedForFrameCallback = true;
	}
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
					if (w->xwayland().a.width > 1200 && w->opacity >= maxOpacity)
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
			focusDirty = true;
		}
	}
	if (ev->atom == ctx->atoms.steamInputFocusAtom )
	{
		steamcompmgr_win_t * w = find_win(ctx, ev->window);
		if (w)
		{
			w->inputFocusMode = get_prop(ctx, w->xwayland().id, ctx->atoms.steamInputFocusAtom, 0);
			focusDirty = true;
		}
	}
	if (ev->atom == ctx->atoms.steamTouchClickModeAtom )
	{
		g_nTouchClickMode = (enum wlserver_touch_click_mode) get_prop(ctx, ctx->root, ctx->atoms.steamTouchClickModeAtom, g_nDefaultTouchClickMode );
#if HAVE_OPENVR
		if (BIsVRSession())
			vrsession_update_touch_mode();
#endif
	}
	if (ev->atom == ctx->atoms.steamStreamingClientAtom)
	{
		steamcompmgr_win_t * w = find_win(ctx, ev->window);
		if (w)
		{
			w->isSteamStreamingClient = get_prop(ctx, w->xwayland().id, ctx->atoms.steamStreamingClientAtom, 0);
			focusDirty = true;
		}
	}
	if (ev->atom == ctx->atoms.steamStreamingClientVideoAtom)
	{
		steamcompmgr_win_t * w = find_win(ctx, ev->window);
		if (w)
		{
			w->isSteamStreamingClientVideo = get_prop(ctx, w->xwayland().id, ctx->atoms.steamStreamingClientVideoAtom, 0);
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
		steamcompmgr_win_t * w = find_win(ctx, ev->window);
		if (w)
		{
			uint32_t appID = get_prop(ctx, w->xwayland().id, ctx->atoms.gameAtom, 0);

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
		steamcompmgr_win_t * w = find_win(ctx, ev->window);
		if (w)
		{
			w->isOverlay = get_prop(ctx, w->xwayland().id, ctx->atoms.overlayAtom, 0);
			focusDirty = true;
		}
	}
	if (ev->atom == ctx->atoms.externalOverlayAtom)
	{
		steamcompmgr_win_t * w = find_win(ctx, ev->window);
		if (w)
		{
			w->isExternalOverlay = get_prop(ctx, w->xwayland().id, ctx->atoms.externalOverlayAtom, 0);
			focusDirty = true;
		}
	}
	if (ev->atom == ctx->atoms.winTypeAtom)
	{
		steamcompmgr_win_t * w = find_win(ctx, ev->window);
		if (w)
		{
			get_win_type(ctx, w);
			focusDirty = true;
		}		
	}
	if (ev->atom == ctx->atoms.sizeHintsAtom)
	{
		steamcompmgr_win_t * w = find_win(ctx, ev->window);
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

			focusDirty = true;
		}
	}
	if (ev->atom == XA_WM_NAME || ev->atom == ctx->atoms.netWMNameAtom)
	{
		steamcompmgr_win_t *w = find_win(ctx, ev->window);

		if (w)
		{
			get_win_title(ctx, w, ev->atom);

			if (ev->window == x11_win(global_focus.focusWindow))
			{
				sdlwindow_title( w->title, w->icon );
			}
		}
	}
	if (ev->atom == ctx->atoms.netWMIcon)
	{
		steamcompmgr_win_t *w = find_win(ctx, ev->window);

		if (w)
		{
			get_win_icon(ctx, w);

			if (ev->window == x11_win(global_focus.focusWindow))
			{
				sdlwindow_title( w->title, w->icon );
			}
		}
	}
	if (ev->atom == ctx->atoms.motifWMHints)
	{
		steamcompmgr_win_t *w = find_win(ctx, ev->window);
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
	for (int i = 0; i < DRM_SCREEN_TYPE_COUNT; i++)
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
		alwaysComposite = !!get_prop( ctx, ctx->root, ctx->atoms.gamescopeCompositeForce, 0 );
		hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeCompositeDebug )
	{
		g_uCompositeDebug = get_prop( ctx, ctx->root, ctx->atoms.gamescopeCompositeDebug, 0 );

		hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeAllowTearing )
	{
		g_nAsyncFlipsEnabled = get_prop( ctx, ctx->root, ctx->atoms.gamescopeAllowTearing, 0 );
	}
	if ( ev->atom == ctx->atoms.gamescopeSteamMaxHeight )
	{
		g_nSteamMaxHeight = get_prop( ctx, ctx->root, ctx->atoms.gamescopeSteamMaxHeight, 0 );
		focusDirty = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeVRREnabled )
	{
		bool enabled = !!get_prop( ctx, ctx->root, ctx->atoms.gamescopeVRREnabled, 0 );
		drm_set_vrr_enabled( &g_DRM, enabled );
	}
	if ( ev->atom == ctx->atoms.gamescopeDisplayForceInternal )
	{
		if ( !BIsNested() )
		{
			g_DRM.force_internal = !!get_prop( ctx, ctx->root, ctx->atoms.gamescopeDisplayForceInternal, 0 );
			g_DRM.out_of_date = 1;
		}
	}
	if ( ev->atom == ctx->atoms.gamescopeDisplayModeNudge )
	{
		if ( !BIsNested() )
		{
			g_DRM.out_of_date = 2;
			XDeleteProperty( ctx->dpy, ctx->root, ctx->atoms.gamescopeDisplayModeNudge );
		}
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
		g_bHDREnabled = !!get_prop( ctx, ctx->root, ctx->atoms.gamescopeDisplayHDREnabled, 0 );
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
		drm_update_patched_edid(&g_DRM);
		hasRepaint = true;
	}
	if ( ev->atom == ctx->atoms.gamescopeDebugHDRHeatmap )
	{
		uint32_t heatmap = get_prop( ctx, ctx->root, ctx->atoms.gamescopeDebugHDRHeatmap, 0 );
		g_uCompositeDebug &= ~CompositeDebugFlag::Heatmap;
		g_uCompositeDebug &= ~CompositeDebugFlag::Heatmap_MSWCG;
		if (heatmap != 0)
			g_uCompositeDebug |= CompositeDebugFlag::Heatmap;
		if (heatmap == 2)
			g_uCompositeDebug |= CompositeDebugFlag::Heatmap_MSWCG;
		if (heatmap == 3)
			g_uCompositeDebug |= CompositeDebugFlag::Heatmap_Hard;
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
	if ( ev->atom == ctx->atoms.gamescopeInternalDisplayBrightness )
	{
		uint32_t val = get_prop( ctx, ctx->root, ctx->atoms.gamescopeInternalDisplayBrightness, 0 );
		if ( set_internal_display_brightness( bit_cast<float>(val) ) )
		{
			drm_update_patched_edid(&g_DRM);
			hasRepaint = true;
		}
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
		focusDirty = true;
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
			pollfds.push_back(pollfd {
				.fd = XConnectionNumber( server->ctx->dpy ),
				.events = POLLIN,
			});
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
			if (global_focus.focusWindow &&
				global_focus.focusWindow->type == steamcompmgr_win_type_t::XWAYLAND &&
				global_focus.focusWindow->xwayland().ctx == server->ctx.get())
				global_focus.focusWindow = nullptr;

			if (global_focus.inputFocusWindow &&
				global_focus.inputFocusWindow->type == steamcompmgr_win_type_t::XWAYLAND &&
				global_focus.inputFocusWindow->xwayland().ctx == server->ctx.get())
				global_focus.inputFocusWindow = nullptr;

			if (global_focus.overlayWindow &&
				global_focus.overlayWindow->type == steamcompmgr_win_type_t::XWAYLAND &&
				global_focus.overlayWindow->xwayland().ctx == server->ctx.get())
				global_focus.overlayWindow = nullptr;

			if (global_focus.externalOverlayWindow &&
				global_focus.externalOverlayWindow->type == steamcompmgr_win_type_t::XWAYLAND &&
				global_focus.externalOverlayWindow->xwayland().ctx == server->ctx.get())
				global_focus.externalOverlayWindow = nullptr;

			if (global_focus.notificationWindow &&
				global_focus.notificationWindow->type == steamcompmgr_win_type_t::XWAYLAND &&
				global_focus.notificationWindow->xwayland().ctx == server->ctx.get())
				global_focus.notificationWindow = nullptr;

			if (global_focus.overrideWindow &&
				global_focus.overrideWindow->type == steamcompmgr_win_type_t::XWAYLAND &&
				global_focus.overrideWindow->xwayland().ctx == server->ctx.get())
				global_focus.overrideWindow = nullptr;

			if (global_focus.keyboardFocusWindow &&
				global_focus.keyboardFocusWindow->type == steamcompmgr_win_type_t::XWAYLAND &&
				global_focus.keyboardFocusWindow->xwayland().ctx == server->ctx.get())
				global_focus.keyboardFocusWindow = nullptr;

			if (global_focus.fadeWindow &&
				global_focus.fadeWindow->type == steamcompmgr_win_type_t::XWAYLAND &&
				global_focus.fadeWindow->xwayland().ctx == server->ctx.get())
				global_focus.fadeWindow = nullptr;

			if (global_focus.cursor &&
				global_focus.cursor->getCtx() == server->ctx.get())
				global_focus.cursor = nullptr;

			wlserver_lock();
			std::erase_if(pollfds, [=](const auto& other){
				return other.fd == XConnectionNumber( server->ctx->dpy );
			});
			wlserver_destroy_xwayland_server(server);
			wlserver_unlock();

			focusDirty = true;
		}
	}
	if (ev->atom == ctx->atoms.wineHwndStyle)
	{
		steamcompmgr_win_t * w = find_win(ctx, ev->window);
		if (w)
		{
			w->hasHwndStyle = true;
			w->hwndStyle = get_prop(ctx, w->xwayland().id, ctx->atoms.wineHwndStyle, 0);
			focusDirty = true;
		}
	}
	if (ev->atom == ctx->atoms.wineHwndStyleEx)
	{
		steamcompmgr_win_t * w = find_win(ctx, ev->window);
		if (w)
		{
			w->hasHwndStyleEx = true;
			w->hwndStyleEx = get_prop(ctx, w->xwayland().id, ctx->atoms.wineHwndStyleEx, 0);
			focusDirty = true;
		}
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
			for ( steamcompmgr_win_t *w = server->ctx->list; w; w = w->xwayland().next )
				w->commit_queue.clear();
		}
	}
	g_steamcompmgr_xdg_wins.clear();
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

bool handle_done_commit( steamcompmgr_win_t *w, xwayland_ctx_t *ctx, uint64_t commitID )
{
	bool bFoundWindow = false;
	uint32_t j;
	for ( j = 0; j < w->commit_queue.size(); j++ )
	{
		if ( w->commit_queue[ j ]->commitID == commitID )
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
					hasRepaintNonBasePlane = true;
				}

				if ( w == global_focus.notificationWindow && w->opacity != TRANSLUCENT )
				{
					hasRepaintNonBasePlane = true;
				}
			}
			if ( ctx )
			{
				if ( ctx->focus.outdatedInteractiveFocus )
				{
					focusDirty = true;
					ctx->focus.outdatedInteractiveFocus = false;
				}
			}
			if ( global_focus.outdatedInteractiveFocus )
			{
				focusDirty = true;
				global_focus.outdatedInteractiveFocus = false;

				// If this is an external overlay, repaint
				if ( w == global_focus.externalOverlayWindow && w->opacity != TRANSLUCENT )
				{
					hasRepaintNonBasePlane = true;
				}
			}
			// If this is the main plane, repaint
			if ( w == global_focus.focusWindow && !w->isSteamStreamingClient )
			{
				g_HeldCommits[ HELD_COMMIT_BASE ] = w->commit_queue[ j ];
				hasRepaint = true;
			}

			if ( w == global_focus.overrideWindow )
			{
				hasRepaintNonBasePlane = true;
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
		w->receivedDoneCommit = true;
		return true;
	}

	return false;
}

void handle_done_commits_xwayland( xwayland_ctx_t *ctx )
{
	std::lock_guard<std::mutex> lock( ctx->doneCommits.listCommitsDoneLock );

	// very fast loop yes
	for ( uint32_t i = 0; i < ctx->doneCommits.listCommitsDone.size(); i++ )
	{
		for ( steamcompmgr_win_t *w = ctx->list; w; w = w->xwayland().next )
		{
			if (handle_done_commit(w, ctx, ctx->doneCommits.listCommitsDone[i]))
				break;
		}
	}

	ctx->doneCommits.listCommitsDone.clear();
}

void handle_done_commits_xdg()
{
	std::lock_guard<std::mutex> lock( g_steamcompmgr_xdg_done_commits.listCommitsDoneLock );

	// very fast loop yes
	for ( uint32_t i = 0; i < g_steamcompmgr_xdg_done_commits.listCommitsDone.size(); i++ )
	{
		for (const auto& xdg_win : g_steamcompmgr_xdg_wins)
		{
			if (handle_done_commit(xdg_win.get(), nullptr, g_steamcompmgr_xdg_done_commits.listCommitsDone[i]))
				break;
		}
	}

	g_steamcompmgr_xdg_done_commits.listCommitsDone.clear();
}

void handle_presented_for_window( steamcompmgr_win_t* w )
{
	commit_t *lastCommit = get_window_last_done_commit_peek(w);
	if (lastCommit && !lastCommit->presentation_feedbacks.empty())
	{
		wlserver_lock();

		int nRefresh = g_nNestedRefresh ? g_nNestedRefresh : g_nOutputRefresh;

		wlserver_presentation_feedback_presented(
			lastCommit->surf,
			lastCommit->presentation_feedbacks,
			g_lastVblank.load(),
			nRefresh);

		wlserver_unlock();
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
	if ( write( g_nudgePipe[ 1 ], "\n", 1 ) < 0 )
		xwm_log.errorf_errno( "nudge_steamcompmgr: write failed" );
}

void take_screenshot( void )
{
	g_bTakeScreenshot = true;
	nudge_steamcompmgr();
}

void force_repaint( void )
{
	g_bForceRepaint = true;
	nudge_steamcompmgr();
}

void update_wayland_res(CommitDoneList_t *doneCommits, steamcompmgr_win_t *w, ResListEntry_t& reslistentry)
{
	struct wlr_buffer *buf = reslistentry.buf;

	if ( w == nullptr )
	{
		wlserver_lock();
		wlr_buffer_unlock( buf );
		wlserver_unlock();
		xwm_log.errorf( "waylandres but no win" );
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
		xwm_log.errorf( "got the same buffer commited twice, ignoring." );

		// If we have a duplicated commit + frame callback, ensure that is signalled.
		// This matches Mutter and Weston behavior, so it's plausible that some application relies on forward progress.
		// We're essentially discarding the commit here, so consider it complete right away.
		w->receivedDoneCommit = true;
		return;
	}

	std::shared_ptr<commit_t> newCommit = import_commit( reslistentry.surf, buf, reslistentry.async, std::move(reslistentry.feedback), std::move(reslistentry.presentation_feedbacks) );

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

		gpuvis_trace_printf( "pushing wait for commit %lu win %lx", newCommit->commitID, w->type == steamcompmgr_win_type_t::XWAYLAND ? w->xwayland().id : 0 );
		{
			std::unique_lock< std::mutex > lock( waitListLock );
			WaitListEntry_t entry
			{
				.doneCommits = doneCommits,
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

void check_new_xwayland_res(xwayland_ctx_t *ctx)
{
	// When importing buffer, we'll potentially need to perform operations with
	// a wlserver lock (e.g. wlr_buffer_lock). We can't do this with a
	// wayland_commit_queue lock because that causes deadlocks.
	std::vector<ResListEntry_t> tmp_queue = ctx->xwayland_server->retrieve_commits();

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
spawn_client( char **argv )
{
#if defined(__linux__)
	// (Don't Lose) The Children
	prctl( PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0 );
#elif defined(__DragonFly__) || defined(__FreeBSD__)
	procctl(P_PID, getpid(), PROC_REAP_ACQUIRE, NULL);
#else
#warning "Changing reaper process for children is not supported on this platform"
#endif

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
handle_xfixes_selection_notify( xwayland_ctx_t *ctx, XFixesSelectionNotifyEvent *event )
{
	if (event->owner == ctx->ourWindow)
	{
		return;
	}

	XConvertSelection(ctx->dpy, event->selection, ctx->atoms.utf8StringAtom, event->selection, ctx->ourWindow, CurrentTime);
	XFlush(ctx->dpy);
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

static bool
load_host_cursor( MouseCursor *cursor )
{
	extern const char *g_pOriginalDisplay;

	if ( !g_pOriginalDisplay )
		return false;

	Display *display = XOpenDisplay( g_pOriginalDisplay );
	if ( !display )
		return false;
	defer( XCloseDisplay( display ) );

	int xfixes_event, xfixes_error;
	if (!XFixesQueryExtension(display, &xfixes_event, &xfixes_error))
	{
		xwm_log.errorf("No XFixes extension on current compositor");
		return false;
	}

	XFixesCursorImage *image = XFixesGetCursorImage( display );
	if ( !image )
		return false;
	defer( XFree( image ) );

	// image->pixels is `unsigned long*` :/
	// Thanks X11.
	std::vector<uint32_t> cursorData;
	for (uint32_t y = 0; y < image->height; y++)
	{
		for (uint32_t x = 0; x < image->width; x++)
		{
			cursorData.push_back((uint32_t)image->pixels[image->height * y + x]);
		}
	}

	cursor->setCursorImage((char *)cursorData.data(), image->width, image->height, image->xhot, image->yhot);
	return true;
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

void init_xwayland_ctx(uint32_t serverId, gamescope_xwayland_server_t *xwayland_server)
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
	ctx->atoms.netWMIcon = XInternAtom(ctx->dpy, "_NET_WM_ICON", false);
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

	ctx->atoms.gamescopeXWaylandModeControl = XInternAtom( ctx->dpy, "GAMESCOPE_XWAYLAND_MODE_CONTROL", false );
	ctx->atoms.gamescopeFPSLimit = XInternAtom( ctx->dpy, "GAMESCOPE_FPS_LIMIT", false );
	ctx->atoms.gamescopeDynamicRefresh[DRM_SCREEN_TYPE_INTERNAL] = XInternAtom( ctx->dpy, "GAMESCOPE_DYNAMIC_REFRESH", false );
	ctx->atoms.gamescopeDynamicRefresh[DRM_SCREEN_TYPE_EXTERNAL] = XInternAtom( ctx->dpy, "GAMESCOPE_DYNAMIC_REFRESH_EXTERNAL", false );
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
	ctx->atoms.gamescopeInternalDisplayBrightness = XInternAtom( ctx->dpy, "GAMESCOPE_INTERNAL_DISPLAY_BRIGHTNESS", false );
	ctx->atoms.gamescopeHDRInputGain = XInternAtom( ctx->dpy, "GAMESCOPE_HDR_INPUT_GAIN", false );
	ctx->atoms.gamescopeSDRInputGain = XInternAtom( ctx->dpy, "GAMESCOPE_SDR_INPUT_GAIN", false );
	ctx->atoms.gamescopeHDRItmEnable = XInternAtom( ctx->dpy, "GAMESCOPE_HDR_ITM_ENABLE", false );
	ctx->atoms.gamescopeHDRItmSDRNits = XInternAtom( ctx->dpy, "GAMESCOPE_HDR_ITM_SDR_NITS", false );
	ctx->atoms.gamescopeHDRItmTargetNits = XInternAtom( ctx->dpy, "GAMESCOPE_HDR_ITM_TARGET_NITS", false );
	ctx->atoms.gamescopeColorLookPQ = XInternAtom( ctx->dpy, "GAMESCOPE_COLOR_LOOK_PQ", false );
	ctx->atoms.gamescopeColorLookG22 = XInternAtom( ctx->dpy, "GAMESCOPE_COLOR_LOOK_G22", false );
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

	ctx->atoms.gamescopeCreateXWaylandServer = XInternAtom( ctx->dpy, "GAMESCOPE_CREATE_XWAYLAND_SERVER", false );
	ctx->atoms.gamescopeCreateXWaylandServerFeedback = XInternAtom( ctx->dpy, "GAMESCOPE_CREATE_XWAYLAND_SERVER_FEEDBACK", false );
	ctx->atoms.gamescopeDestroyXWaylandServer = XInternAtom( ctx->dpy, "GAMESCOPE_DESTROY_XWAYLAND_SERVER", false );

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
		if ( BIsNested() )
		{
			if ( !load_host_cursor( ctx->cursor.get() ) )
			{
				xwm_log.errorf("Failed to load host cursor. Falling back to left_ptr.");
				if (!ctx->cursor->setCursorImageByName("left_ptr"))
					xwm_log.errorf("Failed to load mouse cursor: left_ptr");
			}
		}
		else
		{
			xwm_log.infof("Embedded, no cursor set. Using left_ptr by default.");
			if (!ctx->cursor->setCursorImageByName("left_ptr"))
				xwm_log.errorf("Failed to load mouse cursor: left_ptr");
		}
	}

	XFlush(ctx->dpy);
}

void update_vrr_atoms(xwayland_ctx_t *root_ctx, bool force, bool* needs_flush = nullptr)
{
	bool capable = drm_get_vrr_capable( &g_DRM );
	if ( capable != g_bVRRCapable_CachedValue || force )
	{
		uint32_t capable_value = capable ? 1 : 0;
		XChangeProperty(root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeVRRCapable, XA_CARDINAL, 32, PropModeReplace,
			(unsigned char *)&capable_value, 1 );
		g_bVRRCapable_CachedValue = capable;
		if (needs_flush)
			*needs_flush = true;
	}

	bool st2084 = drm_supports_st2084( &g_DRM );
	if ( st2084 != g_bSupportsST2084_CachedValue || force )
	{
		uint32_t hdr_value = st2084 ? 1 : 0;
		XChangeProperty(root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeDisplaySupportsHDR, XA_CARDINAL, 32, PropModeReplace,
			(unsigned char *)&hdr_value, 1 );
		g_bSupportsST2084_CachedValue = st2084;
		if (needs_flush)
			*needs_flush = true;
	}

	bool in_use = drm_get_vrr_in_use( &g_DRM );
	if ( in_use != g_bVRRInUse_CachedValue || force )
	{
		uint32_t in_use_value = in_use ? 1 : 0;
		XChangeProperty(root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeVRRInUse, XA_CARDINAL, 32, PropModeReplace,
			(unsigned char *)&in_use_value, 1 );
		g_bVRRInUse_CachedValue = in_use;
		if (needs_flush)
			*needs_flush = true;
	}

	// Don't update this in-sync with DRM vrr usage.
	// Keep this as a preference, starting with off.
	if ( force )
	{
        bool wants_vrr = g_DRM.wants_vrr_enabled;
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

	if ( drm_get_screen_type(&g_DRM) == DRM_SCREEN_TYPE_INTERNAL )
	{
		XDeleteProperty(root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeDisplayModeListExternal);

		uint32_t zero = 0;
		XChangeProperty(root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeDisplayIsExternal, XA_CARDINAL, 32, PropModeReplace,
			(unsigned char *)&zero, 1 );
		return;
	}

	char modes[4096] = "";
	int remaining_size = sizeof(modes) - 1;
	int len = 0;
	for (int i = 0; remaining_size > 0 && i < g_DRM.connector->connector->count_modes; i++)
	{
		const auto& mode = g_DRM.connector->connector->modes[i];
		int mode_len = snprintf(&modes[len], remaining_size, "%s%dx%d@%d",
			i == 0 ? "" : " ",
			int(mode.hdisplay), int(mode.vdisplay), int(mode.vrefresh));
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

extern std::atomic<uint64_t> g_nCompletedPageFlipCount;

void steamcompmgr_check_xdg(bool vblank)
{
	if (wlserver_xdg_dirty())
	{
		g_steamcompmgr_xdg_wins = wlserver_get_xdg_shell_windows();
		focusDirty = true;
	}

	handle_done_commits_xdg();

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
	if ( !BIsNested() )
	{
		const char *filename = drm_get_patched_edid_path();
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
}

void
steamcompmgr_main(int argc, char **argv)
{
	int	readyPipeFD = -1;

	// Reset getopt() state
	optind = 1;

	bSteamCompMgrGrab = BIsNested() && g_bForceRelativeMouse;

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
				} else if (strcmp(opt_name, "force-windows-fullscreen") == 0) {
					bForceWindowsFullscreen = true;
				} else if (strcmp(opt_name, "cursor-scale-height") == 0) {
					g_nCursorScaleHeight = atoi(optarg);
				} else if (strcmp(opt_name, "hdr-enabled") == 0) {
					g_bHDREnabled = true;
				} else if (strcmp(opt_name, "hdr-debug-force-support") == 0) {
					g_bForceHDRSupportDebug = true;
 				} else if (strcmp(opt_name, "hdr-debug-force-output") == 0) {
					g_bForceHDR10OutputDebug = true;
				} else if (strcmp(opt_name, "hdr-itm-enable") == 0) {
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

	// Enable color mgmt by default.
	g_ColorMgmt.pending.enabled = true;

	currentOutputWidth = g_nPreferredOutputWidth;
	currentOutputHeight = g_nPreferredOutputHeight;

	init_runtime_info();
#if HAVE_OPENVR
	if ( BIsVRSession() )
		vrsession_steam_mode( steamMode );
#endif

	int vblankFD = vblank_init();
	assert( vblankFD >= 0 );

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

			server->ctx->force_windows_fullscreen = bForceWindowsFullscreen;
		}
	}

	update_vrr_atoms(root_ctx, true);
	update_mode_atoms(root_ctx);
	XFlush(root_ctx->dpy);

	if ( !BIsNested() )
	{
		drm_update_patched_edid(&g_DRM);
		update_edid_prop();
	}

	update_screenshot_color_mgmt();

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

		if (focusDirty)
			determine_and_apply_focus();

		// If our DRM state is out-of-date, refresh it. This might update
		// the output size.
		if ( BIsNested() == false )
		{
			if ( drm_poll_state( &g_DRM ) )
			{
				hasRepaint = true;

				update_mode_atoms(root_ctx, &flush_root);
			}
		}

		g_bOutputHDREnabled = (g_bSupportsST2084_CachedValue || g_bForceHDR10OutputDebug) && g_bHDREnabled;

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

			if ( BIsSDLSession() )
			{
				vulkan_remake_swapchain();

				while ( !acquire_next_image() )
					vulkan_remake_swapchain();
			}
			else
			{
				if ( !BIsNested() )
				{
					if (g_bOutputHDREnabled != currentHDROutput)
					{
						drm_set_hdr_state(&g_DRM, g_bOutputHDREnabled);
					}
				}

				vulkan_remake_output_images();
			}


			{
				gamescope_xwayland_server_t *server = NULL;
				for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
				{
					uint32_t hdr_value = ( g_bOutputHDREnabled || g_bForceHDRSupportDebug ) ? 1 : 0;
					XChangeProperty(server->ctx->dpy, server->ctx->root, server->ctx->atoms.gamescopeHDROutputFeedback, XA_CARDINAL, 32, PropModeReplace,
						(unsigned char *)&hdr_value, 1 );

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
		if ( vblank == true )
		{
			static int vblank_idx = 0;
			{
				gamescope_xwayland_server_t *server = NULL;
				for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
				{
					for (steamcompmgr_win_t *w = server->ctx->list; w; w = w->xwayland().next)
					{
						steamcompmgr_latch_frame_done( w, vblank_idx );
					}
				}

				for ( const auto& xdg_win : g_steamcompmgr_xdg_wins )
				{
					steamcompmgr_latch_frame_done( xdg_win.get(), vblank_idx );
				}
			}
			vblank_idx++;
		}

		{
			gamescope_xwayland_server_t *server = NULL;
			for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
			{
				handle_done_commits_xwayland(server->ctx.get());

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
			gamescope_xwayland_server_t *server = NULL;
			for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
				handle_presented_xwayland( server->ctx.get() );
		}

		//

		{
			gamescope_xwayland_server_t *server = NULL;
			for (size_t i = 0; (server = wlserver_get_xwayland_server(i)); i++)
				check_new_xwayland_res(server->ctx.get());
		}


		{
			GamescopeAppTextureColorspace current_app_colorspace = GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB;
			std::shared_ptr<wlserver_hdr_metadata> app_hdr_metadata = nullptr;
			if ( g_HeldCommits[HELD_COMMIT_BASE] )
			{
				current_app_colorspace = g_HeldCommits[HELD_COMMIT_BASE]->colorspace();
				if (g_HeldCommits[HELD_COMMIT_BASE]->feedback)
					app_hdr_metadata = g_HeldCommits[HELD_COMMIT_BASE]->feedback->hdr_metadata_blob;
			}

			bool app_wants_hdr = ColorspaceIsHDR( current_app_colorspace );

			static bool s_bAppWantsHDRCached = false;

			if ( app_wants_hdr != s_bAppWantsHDRCached )
			{
				uint32_t app_wants_hdr_prop = app_wants_hdr ? 1 : 0;

				XChangeProperty(root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeColorAppWantsHDRFeedback, XA_CARDINAL, 32, PropModeReplace,
						(unsigned char *)&app_wants_hdr_prop, 1 );

				s_bAppWantsHDRCached = app_wants_hdr;
				flush_root = true;
			}

			if ( app_hdr_metadata != g_ColorMgmt.pending.appHDRMetadata )
			{
				if ( app_hdr_metadata )
				{
					std::vector<uint32_t> app_hdr_metadata_blob;
					app_hdr_metadata_blob.resize((sizeof(hdr_metadata_infoframe) + (sizeof(uint32_t) - 1)) / sizeof(uint32_t));
					memset(app_hdr_metadata_blob.data(), 0, sizeof(uint32_t) * app_hdr_metadata_blob.size());
					memcpy(app_hdr_metadata_blob.data(), &app_hdr_metadata->metadata, sizeof(hdr_metadata_infoframe));

					XChangeProperty(root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeColorAppHDRMetadataFeedback, XA_CARDINAL, 32, PropModeReplace,
							(unsigned char *)app_hdr_metadata_blob.data(), (int)app_hdr_metadata_blob.size() );
				}
				else
				{
					XDeleteProperty(root_ctx->dpy, root_ctx->root, root_ctx->atoms.gamescopeColorAppHDRMetadataFeedback);
				}

				g_ColorMgmt.pending.appHDRMetadata = app_hdr_metadata;
				flush_root = true;
			}
		}

		steamcompmgr_check_xdg(vblank);

		// Handles if we got a commit for the window we want to focus
		// to switch to it for painting (outdatedInteractiveFocus)
		// Doesn't realllly matter but avoids an extra frame of being on the wrong window.
		if (focusDirty)
			determine_and_apply_focus();

		if ( window_is_steam( global_focus.focusWindow ) )
		{
			g_upscaleScaler = GamescopeUpscaleScaler::FIT;
			g_upscaleFilter = GamescopeUpscaleFilter::LINEAR;
		}
		else
		{
			g_upscaleScaler = g_wantedUpscaleScaler;
			g_upscaleFilter = g_wantedUpscaleFilter;
		}

		static int nIgnoredOverlayRepaints = 0;

		const bool bVRR = drm_get_vrr_in_use( &g_DRM );

		// HACK: Disable tearing if we have an overlay to avoid stutters right now
		// TODO: Fix properly.
		static bool bHasOverlay = ( global_focus.overlayWindow && global_focus.overlayWindow->opacity ) ||
								( global_focus.externalOverlayWindow && global_focus.externalOverlayWindow->opacity ) ||
								( global_focus.overrideWindow  && global_focus.focusWindow && !global_focus.focusWindow->isSteamStreamingClient && global_focus.overrideWindow->opacity );

		const bool bSteamOverlayOpen  = global_focus.overlayWindow && global_focus.overlayWindow->opacity;
		// If we are running behind, allow tearing.
		const bool bSurfaceWantsAsync = (g_HeldCommits[HELD_COMMIT_BASE] && g_HeldCommits[HELD_COMMIT_BASE]->async);

		const bool bForceRepaint = g_bForceRepaint.exchange(false);
		const bool bForceSyncFlip = bForceRepaint || g_bTakeScreenshot || is_fading_out();
		// If we are compositing, always force sync flips because we currently wait
		// for composition to finish before submitting.
		// If we want to do async + composite, we should set up syncfile stuff and have DRM wait on it.
		const bool bNeedsSyncFlip = bForceSyncFlip || g_bCurrentlyCompositing || nIgnoredOverlayRepaints;
		const bool bDoAsyncFlip   = ( ((g_nAsyncFlipsEnabled >= 1) && g_bSupportsAsyncFlips && bSurfaceWantsAsync && !bHasOverlay) || bVRR ) && !bSteamOverlayOpen && !bNeedsSyncFlip;

		bool bShouldPaint = false;
		if ( bDoAsyncFlip )
		{
			if ( hasRepaint && !g_bCurrentlyCompositing )
				bShouldPaint = true;
		}
		else
		{
			bShouldPaint = vblank && ( hasRepaint || hasRepaintNonBasePlane || bForceSyncFlip );
		}

		// If we have a pending page flip and doing VRR, lets not do another...
		if ( bVRR && g_nCompletedPageFlipCount != g_DRM.flipcount )
			bShouldPaint = false;

		if ( !bShouldPaint && hasRepaintNonBasePlane && vblank )
			nIgnoredOverlayRepaints++;

#if HAVE_OPENVR
		if ( BIsVRSession() && !vrsession_visible() )
			bShouldPaint = false;
#endif

		if ( bShouldPaint )
		{
			paint_all( !vblank && !bVRR );

			hasRepaint = false;
			hasRepaintNonBasePlane = false;
			nIgnoredOverlayRepaints = 0;

			// If we're in the middle of a fade, pump an event into the loop to
			// make sure we keep pushing frames even if the app isn't updating.
			if ( is_fading_out() )
			{
				nudge_steamcompmgr();
			}
		}

		update_vrr_atoms(root_ctx, false, &flush_root);

		if (global_focus.cursor)
		{
			global_focus.cursor->updatePosition();

			if (global_focus.cursor->needs_server_flush())
			{
				flush_root = true;
				global_focus.cursor->inform_flush();
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

void steamcompmgr_send_frame_done_to_focus_window()
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	if ( global_focus.focusWindow && global_focus.focusWindow->xwayland().surface.main_surface )
	{
		wlserver_lock();
		wlserver_send_frame_done( global_focus.focusWindow->xwayland().surface.main_surface , &now );
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
	steamcompmgr_win_t *w = find_win( xwayland_server->ctx.get(), xid, true );
	if ( !w )
		return nullptr;

	return &w->xwayland().surface;
}
