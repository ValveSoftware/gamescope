#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "color_helpers_impl.h"

namespace
{
constexpr int k_nEdge = rendervulkan::s_nLutEdgeSize3d;
constexpr int k_nShaperSize = rendervulkan::s_nLutSize1d;
// About one 10-bit code.
constexpr float k_flTolerance = 1e-3f;

struct Settings_t
{
	displaycolorimetry_t native = displaycolorimetry_709;
	bool bHDR10Output = false;
	EOTF eInputEOTF = EOTF_Gamma22;
	float flSDRGamutWideness = -1.f;
	float flSDRInputGain = 1.f;
	nightmode_t nightmode{};
	glm::vec2 vVirtualWhite{ 0.f };
	const lut3d_t *pLook = nullptr;
};

struct Published_t
{
	std::vector<uint16_t> lut1d;
	std::vector<uint16_t> lut3d;
};

void PushRGBX( std::vector<uint16_t> &data, const glm::vec3 &value )
{
	data.push_back( quantize_lut_value_16bit( value.r ) );
	data.push_back( quantize_lut_value_16bit( value.g ) );
	data.push_back( quantize_lut_value_16bit( value.b ) );
	data.push_back( 0 );
}

// One input EOTF index of create_color_mgmt_luts, without tonemap metadata.
Published_t Bake( const Settings_t &settings )
{
	const displaycolorimetry_t dest = settings.bHDR10Output ? displaycolorimetry_2020 : settings.native;
	const EOTF eDestEOTF = settings.bHDR10Output ? EOTF_PQ : EOTF_Gamma22;

	displaycolorimetry_t source{};
	colormapping_t mapping{};
	tonemapping_t tonemapping{};
	float flGain = 1.f;
	if ( settings.eInputEOTF == EOTF_Gamma22 )
	{
		flGain = settings.flSDRInputGain;
		tonemapping.g22_luminance = settings.bHDR10Output ? 203.f : 1.f;
		buildSDRColorimetry( &source, &mapping, settings.flSDRGamutWideness, settings.native );
	}
	else
	{
		tonemapping.g22_luminance = settings.bHDR10Output ? 1.f : 500.f;
		buildPQColorimetry( &source, &mapping, settings.native );
	}

	lut1d_t shaper;
	lut3d_t lut3d;
	calcColorTransform<rendervulkan::s_nLutEdgeSize3d>( &shaper, k_nShaperSize, &lut3d,
		source, settings.eInputEOTF, dest, eDestEOTF, settings.native,
		settings.vVirtualWhite, k_EChromaticAdapatationMethod_Bradford,
		mapping, settings.nightmode, tonemapping, settings.pLook, flGain );

	Published_t published;
	for ( int i = 0; i < k_nShaperSize; i++ )
		PushRGBX( published.lut1d, glm::vec3( shaper.dataR[i], shaper.dataG[i], shaper.dataB[i] ) );
	for ( const glm::vec3 &value : lut3d.data )
		PushRGBX( published.lut3d, value );

	return published;
}

float Unorm16( uint16_t uValue )
{
	return uValue / 65535.f;
}

// perform_1dlut, where half_texel_scale puts the linear filter at x * ( size - 1 ).
glm::vec3 SampleShaper( const std::vector<uint16_t> &lut1d, const glm::vec3 &color )
{
	glm::vec3 out;
	for ( int c = 0; c < 3; c++ )
	{
		float flCoord = std::clamp( color[c], 0.f, 1.f ) * float( k_nShaperSize - 1 );
		int nLow = std::min( int( flCoord ), k_nShaperSize - 2 );
		float flFrac = flCoord - float( nLow );
		float flLow = Unorm16( lut1d[4 * nLow + c] );
		float flHigh = Unorm16( lut1d[4 * ( nLow + 1 ) + c] );
		out[c] = flLow + ( flHigh - flLow ) * flFrac;
	}
	return out;
}

glm::vec3 Lut3DNode( const std::vector<uint16_t> &lut3d, int r, int g, int b )
{
	const uint16_t *pNode = &lut3d[4 * ( r + k_nEdge * ( g + k_nEdge * b ) )];
	return glm::vec3( Unorm16( pNode[0] ), Unorm16( pNode[1] ), Unorm16( pNode[2] ) );
}

// perform_3dlut_tetrahedral, written in RGB order.
glm::vec3 SampleLut3D( const std::vector<uint16_t> &lut3d, const glm::vec3 &color )
{
	int base[3];
	float frac[3];
	for ( int c = 0; c < 3; c++ )
	{
		float flCoord = std::clamp( color[c], 0.f, 1.f ) * float( k_nEdge - 1 );
		base[c] = std::min( int( flCoord ), k_nEdge - 2 );
		frac[c] = flCoord - float( base[c] );
	}

	auto node = [&]( int r, int g, int b ) { return Lut3DNode( lut3d, base[0] + r, base[1] + g, base[2] + b ); };
	const float fr = frac[0], fg = frac[1], fb = frac[2];
	if ( fr >= fg )
	{
		if ( fg >= fb )
			return ( 1.f - fr ) * node( 0, 0, 0 ) + ( fr - fg ) * node( 1, 0, 0 ) + ( fg - fb ) * node( 1, 1, 0 ) + fb * node( 1, 1, 1 );
		if ( fr >= fb )
			return ( 1.f - fr ) * node( 0, 0, 0 ) + ( fr - fb ) * node( 1, 0, 0 ) + ( fb - fg ) * node( 1, 0, 1 ) + fg * node( 1, 1, 1 );
		return ( 1.f - fb ) * node( 0, 0, 0 ) + ( fb - fr ) * node( 0, 0, 1 ) + ( fr - fg ) * node( 1, 0, 1 ) + fg * node( 1, 1, 1 );
	}
	if ( fb >= fg )
		return ( 1.f - fb ) * node( 0, 0, 0 ) + ( fb - fg ) * node( 0, 0, 1 ) + ( fg - fr ) * node( 0, 1, 1 ) + fr * node( 1, 1, 1 );
	if ( fb >= fr )
		return ( 1.f - fg ) * node( 0, 0, 0 ) + ( fg - fb ) * node( 0, 1, 0 ) + ( fb - fr ) * node( 0, 1, 1 ) + fr * node( 1, 1, 1 );
	return ( 1.f - fg ) * node( 0, 0, 0 ) + ( fg - fr ) * node( 0, 1, 0 ) + ( fr - fb ) * node( 1, 1, 0 ) + fb * node( 1, 1, 1 );
}

// i915 takes the S31.32 coefficient as sign, 3-bit exponent and a rounded 9-bit mantissa.
float QuantizeCSCCoeff( float flCoeff )
{
	uint64_t ulAbs = std::min<uint64_t>( uint64_t( std::fabs( double( flCoeff ) ) * 4294967296.0 ), ( 4ull << 32 ) - 1 );
	int nFracBits = 7;
	if ( ulAbs < ( 1ull << 29 ) )
		nFracBits = 12;
	else if ( ulAbs < ( 1ull << 30 ) )
		nFracBits = 11;
	else if ( ulAbs < ( 1ull << 31 ) )
		nFracBits = 10;
	else if ( ulAbs < ( 1ull << 32 ) )
		nFracBits = 9;
	else if ( ulAbs < ( 1ull << 33 ) )
		nFracBits = 8;
	uint64_t ulField = std::min<uint64_t>( ( ulAbs >> ( 32 - nFracBits - 3 ) ) + 4, 0xfff ) & 0xff8;
	return std::copysign( float( ulField >> 3 ) / float( 1u << nFracBits ), flCoeff );
}

glm::vec3 ApplyPlane( const glm::vec3 &vGain, const glm::vec3 &color )
{
	glm::vec3 out;
	for ( int c = 0; c < 3; c++ )
	{
		// The 9-bit mantissa tops out at 511/512, so a gain this close to unity is sent as 1.0.
		const float flGain = vGain[c] >= 1.f - 1.f / 1024.f ? 1.f : vGain[c];
		out[c] = std::clamp( QuantizeCSCCoeff( flGain ) * color[c], 0.f, 1.f );
	}
	return out;
}

// Scaled ramps, as the bake's float noise alone exceeds a code where one channel is near zero and another is not.
const std::vector<glm::vec3> &Samples()
{
	static const std::vector<glm::vec3> s_Samples = []
	{
		std::vector<float> ramp;
		for ( int i = 0; i <= 100; i++ )
			ramp.push_back( i / 2000.f );
		for ( int i = 6; i <= 84; i++ )
			ramp.push_back( i / 100.f );
		for ( int i = 1700; i <= 2000; i++ )
			ramp.push_back( i / 2000.f );

		const glm::vec3 directions[] = {
			{ 1.f, 1.f, 1.f },
			{ 1.f, 0.5f, 0.25f }, { 1.f, 0.25f, 0.5f },
			{ 0.5f, 1.f, 0.25f }, { 0.25f, 1.f, 0.5f },
			{ 0.5f, 0.25f, 1.f }, { 0.25f, 0.5f, 1.f },
		};

		std::vector<glm::vec3> samples;
		for ( const glm::vec3 &direction : directions )
		{
			for ( float t : ramp )
				samples.push_back( direction * t );
		}
		return samples;
	}();
	return s_Samples;
}

// A single opaque layer, so the shader's decode and blend round trips cancel around the LUTs.
float MaxScanoutError( const Published_t &published, const glm::vec3 &vGain )
{
	float flMaxError = 0.f;
	for ( const glm::vec3 &color : Samples() )
	{
		glm::vec3 composite = SampleLut3D( published.lut3d, SampleShaper( published.lut1d, color ) );
		glm::vec3 plane = ApplyPlane( vGain, color );
		for ( int c = 0; c < 3; c++ )
			flMaxError = std::max( flMaxError, std::fabs( composite[c] - plane[c] ) );
	}
	return flMaxError;
}

// The post-blend 3D LUT samples the encoded values with no shaper in front of it.
float MaxLut3DOnlyError( const Published_t &published )
{
	float flMaxError = 0.f;
	for ( const glm::vec3 &color : Samples() )
	{
		glm::vec3 composite = SampleLut3D( published.lut3d, SampleShaper( published.lut1d, color ) );
		glm::vec3 scanout = SampleLut3D( published.lut3d, color );
		for ( int c = 0; c < 3; c++ )
			flMaxError = std::max( flMaxError, std::fabs( composite[c] - scanout[c] ) );
	}
	return flMaxError;
}
}

TEST_CASE( "No gain is published without night mode", "[scanout_transform]" )
{
	const int nPanel = GENERATE( 0, 1, 2 );
	const displaycolorimetry_t panels[] = { displaycolorimetry_709, displaycolorimetry_steamdeck_spec, displaycolorimetry_widegamutgeneric };
	CAPTURE( nPanel );
	Settings_t settings;
	settings.native = panels[nPanel];
	const glm::vec3 vGain = calcScanoutGain( EOTF_Gamma22, EOTF_Gamma22, settings.nightmode );
	REQUIRE( ( vGain == glm::vec3( 1.f ) ) );
	REQUIRE( MaxScanoutError( Bake( settings ), vGain ) <= k_flTolerance );
}

TEST_CASE( "Night mode on SDR output is a gain on the encoded values", "[scanout_transform]" )
{
	const float flHue = GENERATE( 0.f, 0.08f, 0.25f, 0.5f, 0.66f, 0.9f );
	const float flAmount = GENERATE( 0.001f, 0.25f, 0.5f, 1.f );
	CAPTURE( flHue, flAmount );
	Settings_t settings;
	settings.nightmode = { flAmount, flHue, 0.9f };
	const glm::vec3 vGain = calcScanoutGain( EOTF_Gamma22, EOTF_Gamma22, settings.nightmode );
	REQUIRE( ( vGain != glm::vec3( 1.f ) ) );
	REQUIRE( MaxScanoutError( Bake( settings ), vGain ) <= k_flTolerance );
}

TEST_CASE( "The gain alone does not reproduce a gamut setting", "[scanout_transform]" )
{
	Settings_t settings;
	settings.flSDRGamutWideness = 0.5f;
	REQUIRE( MaxScanoutError( Bake( settings ), glm::vec3( 1.f ) ) > k_flTolerance );
}

TEST_CASE( "No gain is published for PQ content or PQ output", "[scanout_transform]" )
{
	const nightmode_t nightmode = { 0.5f, 0.08f, 0.9f };
	REQUIRE( ( calcScanoutGain( EOTF_PQ, EOTF_PQ, nightmode ) == glm::vec3( 1.f ) ) );
	REQUIRE( ( calcScanoutGain( EOTF_PQ, EOTF_Gamma22, nightmode ) == glm::vec3( 1.f ) ) );
	REQUIRE( ( calcScanoutGain( EOTF_Gamma22, EOTF_PQ, nightmode ) == glm::vec3( 1.f ) ) );
}

TEST_CASE( "The 3D LUT alone carries the transform when the shaper is the identity", "[scanout_transform]" )
{
	const bool bHDR10Output = GENERATE( false, true );
	CAPTURE( bHDR10Output );
	Settings_t settings;
	settings.native = displaycolorimetry_widegamutgeneric;
	settings.bHDR10Output = bHDR10Output;
	settings.eInputEOTF = bHDR10Output ? EOTF_PQ : EOTF_Gamma22;
	settings.flSDRGamutWideness = 0.5f;
	settings.nightmode = { 0.5f, 0.08f, 0.9f };
	REQUIRE( BIsShaperIdentity( settings.eInputEOTF, bHDR10Output ? EOTF_PQ : EOTF_Gamma22, settings.flSDRInputGain ) );
	REQUIRE( MaxLut3DOnlyError( Bake( settings ) ) <= k_flTolerance );
}

TEST_CASE( "The 3D LUT alone misses an input gain", "[scanout_transform]" )
{
	Settings_t settings;
	settings.flSDRInputGain = 1.5f;
	REQUIRE( !BIsShaperIdentity( EOTF_Gamma22, EOTF_Gamma22, settings.flSDRInputGain ) );
	REQUIRE( MaxLut3DOnlyError( Bake( settings ) ) > k_flTolerance );
}
