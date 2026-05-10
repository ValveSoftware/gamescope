#pragma once

#include <cstdint>

namespace gamescope
{
	class BackendBlob;

	enum GamescopeModeGeneration
	{
		GAMESCOPE_MODE_GENERATE_CVT,
		GAMESCOPE_MODE_GENERATE_FIXED,
	};

	enum GamescopeScreenType
	{
		GAMESCOPE_SCREEN_TYPE_INTERNAL,
		GAMESCOPE_SCREEN_TYPE_EXTERNAL,

		GAMESCOPE_SCREEN_TYPE_COUNT
	};
}

enum GamescopeAppTextureColorspace
{
	GAMESCOPE_APP_TEXTURE_COLORSPACE_LINEAR = 0,
	GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB,
	GAMESCOPE_APP_TEXTURE_COLORSPACE_SCRGB,
	GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ,
	GAMESCOPE_APP_TEXTURE_COLORSPACE_PASSTHRU,
};
const uint32_t GamescopeAppTextureColorspace_Count = 5;
const uint32_t GamescopeAppTextureColorspace_Bits = 3;

inline bool ColorspaceIsHDR( GamescopeAppTextureColorspace colorspace )
{
	return colorspace == GAMESCOPE_APP_TEXTURE_COLORSPACE_SCRGB ||
		colorspace == GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ;
}

enum GamescopeSelection
{
	GAMESCOPE_SELECTION_CLIPBOARD,
	GAMESCOPE_SELECTION_PRIMARY,

	GAMESCOPE_SELECTION_COUNT,
};

enum GamescopePanelOrientation
{
	GAMESCOPE_PANEL_ORIENTATION_0,   // normal
	GAMESCOPE_PANEL_ORIENTATION_270, // right
	GAMESCOPE_PANEL_ORIENTATION_90,  // left
	GAMESCOPE_PANEL_ORIENTATION_180, // upside down

	GAMESCOPE_PANEL_ORIENTATION_AUTO,
};

// Disable partial composition for now until we get
// composite priorities working in libliftoff + also
// use the proper libliftoff composite plane system.
static constexpr bool kDisablePartialComposition = true;
