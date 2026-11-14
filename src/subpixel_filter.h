#pragma once

#include <array>
#include <cmath>

#include "main.hpp"

struct SubpixelFilterDefinition
{
	GamescopeUpscaleFilter eFilter;
	float downscaleRatio;
	const char *pName;
};

inline constexpr std::array<SubpixelFilterDefinition, 4> g_SubpixelFilterDefinitions = {{
	{ GamescopeUpscaleFilter::SUBPIXEL_RGB,    3.0f, "horizontal RGB" },
	{ GamescopeUpscaleFilter::SUBPIXEL_OLED,   2.0f, "RG/B OLED" },
	{ GamescopeUpscaleFilter::SUBPIXEL_VBGR,   3.0f, "vertical BGR" },
	{ GamescopeUpscaleFilter::SUBPIXEL_QDOLED, 2.0f, "G/RB QD-OLED" },
}};

inline const SubpixelFilterDefinition *FindSubpixelFilterDefinition( GamescopeUpscaleFilter eFilter )
{
	for ( const auto &definition : g_SubpixelFilterDefinitions )
	{
		if ( definition.eFilter == eFilter )
			return &definition;
	}

	return nullptr;
}

inline bool SubpixelFilterSupportsScale( const SubpixelFilterDefinition &definition, float x, float y )
{
	return std::abs( x - definition.downscaleRatio ) <= 0.001f &&
	       std::abs( y - definition.downscaleRatio ) <= 0.001f;
}
