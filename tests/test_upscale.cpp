#include <catch2/catch_test_macros.hpp>

#include "main.hpp"
#include "subpixel_filter.h"

TEST_CASE("SGSR keeps the wire value Steam writes", "[upscale]") {
	REQUIRE( uint32_t( GamescopeUpscaleFilter::SGSR ) == 5 );
}

TEST_CASE("SGSR only takes SDR RGB input", "[upscale]") {
	REQUIRE( SgsrSupportsInput( GAMESCOPE_APP_TEXTURE_COLORSPACE_LINEAR, false ) );
	REQUIRE( SgsrSupportsInput( GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB, false ) );
	REQUIRE_FALSE( SgsrSupportsInput( GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB, true ) );
	for ( auto colorspace : { GAMESCOPE_APP_TEXTURE_COLORSPACE_SCRGB, GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ, GAMESCOPE_APP_TEXTURE_COLORSPACE_PASSTHRU } )
		REQUIRE_FALSE( SgsrSupportsInput( colorspace, false ) );
}

TEST_CASE("SGSR falls back on input it cannot read", "[upscale]") {
	REQUIRE( ResolveUpscaleFilter( GamescopeUpscaleFilter::SGSR, GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB, false ) == GamescopeUpscaleFilter::SGSR );
	REQUIRE( ResolveUpscaleFilter( GamescopeUpscaleFilter::SGSR, GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ, false ) == GamescopeUpscaleFilter::FSR );
	REQUIRE( ResolveUpscaleFilter( GamescopeUpscaleFilter::SGSR, GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB, true ) == GamescopeUpscaleFilter::LINEAR );
	REQUIRE( ResolveUpscaleFilter( GamescopeUpscaleFilter::NIS, GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ, false ) == GamescopeUpscaleFilter::NIS );
}

TEST_CASE("ParseUpscaleFilter names every filter", "[upscale]") {
	REQUIRE( ParseUpscaleFilter( "linear" ) == GamescopeUpscaleFilter::LINEAR );
	REQUIRE( ParseUpscaleFilter( "nearest" ) == GamescopeUpscaleFilter::NEAREST );
	REQUIRE( ParseUpscaleFilter( "fsr" ) == GamescopeUpscaleFilter::FSR );
	REQUIRE( ParseUpscaleFilter( "nis" ) == GamescopeUpscaleFilter::NIS );
	REQUIRE( ParseUpscaleFilter( "pixel" ) == GamescopeUpscaleFilter::PIXEL );
	REQUIRE( ParseUpscaleFilter( "sgsr" ) == GamescopeUpscaleFilter::SGSR );
	REQUIRE_FALSE( ParseUpscaleFilter( "sharp" ).has_value() );
}

TEST_CASE("UpscaleFilterUsesSharpness", "[upscale]") {
	REQUIRE( UpscaleFilterUsesSharpness( GamescopeUpscaleFilter::SGSR ) );
	REQUIRE( UpscaleFilterUsesSharpness( GamescopeUpscaleFilter::FSR ) );
	REQUIRE( UpscaleFilterUsesSharpness( GamescopeUpscaleFilter::NIS ) );
	REQUIRE_FALSE( UpscaleFilterUsesSharpness( GamescopeUpscaleFilter::LINEAR ) );
	REQUIRE_FALSE( UpscaleFilterUsesSharpness( GamescopeUpscaleFilter::PIXEL ) );
}

TEST_CASE("Subpixel filters keep their identities and focus settings", "[upscale]") {
	const struct {
		const char *name;
		GamescopeUpscaleFilter filter;
		uint32_t wireValue;
	} filters[] = {
		{ "subpixel_rgb", GamescopeUpscaleFilter::SUBPIXEL_RGB, 6 },
		{ "subpixel_oled", GamescopeUpscaleFilter::SUBPIXEL_OLED, 7 },
		{ "subpixel_vbgr", GamescopeUpscaleFilter::SUBPIXEL_VBGR, 8 },
		{ "subpixel_qdoled", GamescopeUpscaleFilter::SUBPIXEL_QDOLED, 9 },
	};
	for ( const auto &filter : filters )
	{
		CAPTURE( filter.name );
		REQUIRE( uint32_t( filter.filter ) == filter.wireValue );
		REQUIRE( ParseUpscaleFilter( filter.name ) == filter.filter );
		REQUIRE_FALSE( UpscaleFilterUsesSharpness( filter.filter ) );

		const UpscaleSettings_t game = GetUpscaleSettings(
			false, filter.filter, GamescopeUpscaleScaler::INTEGER, 7 );
		REQUIRE( game.eFilter == filter.filter );
		REQUIRE( game.eScaler == GamescopeUpscaleScaler::INTEGER );
		REQUIRE( game.nSharpness == 7 );

		const UpscaleSettings_t steam = GetUpscaleSettings(
			true, filter.filter, GamescopeUpscaleScaler::INTEGER, 7 );
		REQUIRE( steam.eFilter == GamescopeUpscaleFilter::LINEAR );
		REQUIRE( steam.eScaler == GamescopeUpscaleScaler::FIT );
		REQUIRE( steam.nSharpness == 7 );
	}
}

TEST_CASE("Subpixel kernels require their downscale ratio on both axes", "[upscale]") {
	const std::pair<GamescopeUpscaleFilter, float> filters[] = {
		{ GamescopeUpscaleFilter::SUBPIXEL_RGB, 3.0f },
		{ GamescopeUpscaleFilter::SUBPIXEL_OLED, 2.0f },
		{ GamescopeUpscaleFilter::SUBPIXEL_VBGR, 3.0f },
		{ GamescopeUpscaleFilter::SUBPIXEL_QDOLED, 2.0f },
	};
	for ( const auto &[filter, ratio] : filters )
	{
		CAPTURE( uint32_t( filter ), ratio );
		const auto *definition = FindSubpixelFilterDefinition( filter );
		REQUIRE( definition != nullptr );
		REQUIRE( SubpixelFilterSupportsScale( *definition, ratio, ratio ) );
		const float wrongRatio = ratio == 3.0f ? 2.0f : 3.0f;
		REQUIRE_FALSE( SubpixelFilterSupportsScale( *definition, wrongRatio, wrongRatio ) );
		REQUIRE_FALSE( SubpixelFilterSupportsScale( *definition, ratio, wrongRatio ) );
		REQUIRE_FALSE( SubpixelFilterSupportsScale( *definition, wrongRatio, ratio ) );
		REQUIRE_FALSE( SubpixelFilterSupportsScale( *definition, 1.0f, 1.0f ) );
		REQUIRE_FALSE( SubpixelFilterSupportsScale( *definition, 1.0f / ratio, 1.0f / ratio ) );
	}
}

TEST_CASE("GetUpscaleSettings", "[upscale]") {
	SECTION("a Steam focus window forces linear and fit") {
		const UpscaleSettings_t settings = GetUpscaleSettings(
			true, GamescopeUpscaleFilter::FSR, GamescopeUpscaleScaler::INTEGER, 7 );

		REQUIRE( settings.eFilter == GamescopeUpscaleFilter::LINEAR );
		REQUIRE( settings.eScaler == GamescopeUpscaleScaler::FIT );
		REQUIRE( settings.nSharpness == 7 );
	}

	SECTION("a non-Steam focus window keeps the wanted settings") {
		const UpscaleSettings_t settings = GetUpscaleSettings(
			false, GamescopeUpscaleFilter::FSR, GamescopeUpscaleScaler::INTEGER, 7 );

		REQUIRE( settings.eFilter == GamescopeUpscaleFilter::FSR );
		REQUIRE( settings.eScaler == GamescopeUpscaleScaler::INTEGER );
		REQUIRE( settings.nSharpness == 7 );
	}

	SECTION("passes through a different wanted filter and scaler pair") {
		const UpscaleSettings_t settings = GetUpscaleSettings(
			false, GamescopeUpscaleFilter::NEAREST, GamescopeUpscaleScaler::AUTO, 0 );

		REQUIRE( settings.eFilter == GamescopeUpscaleFilter::NEAREST );
		REQUIRE( settings.eScaler == GamescopeUpscaleScaler::AUTO );
	}
}
