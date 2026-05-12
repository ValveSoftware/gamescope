#include <array>
#include <benchmark/benchmark.h>

#include <algorithm>
#include "Utils/Algorithm.h"

#include "color_helpers_impl.h"

using color_bench::nLutEdgeSize3d;
using color_bench::nLutSize1d;

uint16_t lut1d[ nLutSize1d * 4 ];
uint16_t lut3d[ nLutEdgeSize3d * nLutEdgeSize3d * nLutEdgeSize3d * 4 ];

lut1d_t lut1d_float;
lut3d_t lut3d_float;

static void
BenchmarkCalcColorTransform( EOTF inputEOTF, benchmark::State &state )
{
    const primaries_t primaries        = { { 0.602f, 0.355f },
                                           { 0.340f, 0.574f },
                                           { 0.164f, 0.121f } };
    const glm::vec2   white            = { 0.3070f, 0.3220f };
    const glm::vec2   destVirtualWhite = { 0.f, 0.f };

    displaycolorimetry_t inputColorimetry{};
    inputColorimetry.primaries = primaries;
    inputColorimetry.white     = white;

    displaycolorimetry_t outputEncodingColorimetry{};
    outputEncodingColorimetry.primaries = primaries;
    outputEncodingColorimetry.white     = white;

    colormapping_t colorMapping{};

    tonemapping_t tonemapping{};
    tonemapping.bUseShaper = true;

    nightmode_t nightmode{};
    float       flGain = 1.0f;

    for ( auto _ : state )
    {
        calcColorTransform<nLutEdgeSize3d>(
            &lut1d_float,
            nLutSize1d,
            &lut3d_float,
            inputColorimetry,
            inputEOTF,
            outputEncodingColorimetry,
            EOTF_Gamma22,
            destVirtualWhite,
            k_EChromaticAdapatationMethod_XYZ,
            colorMapping,
            nightmode,
            tonemapping,
            nullptr,
            flGain );
        for ( size_t i = 0, end = lut1d_float.dataR.size( ); i < end; ++i )
        {
            lut1d[ 4 * i + 0 ] =
                quantize_lut_value_16bit( lut1d_float.dataR[ i ] );
            lut1d[ 4 * i + 1 ] =
                quantize_lut_value_16bit( lut1d_float.dataG[ i ] );
            lut1d[ 4 * i + 2 ] =
                quantize_lut_value_16bit( lut1d_float.dataB[ i ] );
            lut1d[ 4 * i + 3 ] = 0;
        }
        for ( size_t i = 0, end = lut3d_float.data.size( ); i < end; ++i )
        {
            lut3d[ 4 * i + 0 ] =
                quantize_lut_value_16bit( lut3d_float.data[ i ].r );
            lut3d[ 4 * i + 1 ] =
                quantize_lut_value_16bit( lut3d_float.data[ i ].g );
            lut3d[ 4 * i + 2 ] =
                quantize_lut_value_16bit( lut3d_float.data[ i ].b );
            lut3d[ 4 * i + 3 ] = 0;
        }
    }
}

static void BenchmarkCalcColorTransforms_G22( benchmark::State &state )
{ BenchmarkCalcColorTransform( EOTF_Gamma22, state ); }
BENCHMARK( BenchmarkCalcColorTransforms_G22 );

static void BenchmarkCalcColorTransforms_PQ( benchmark::State &state )
{ BenchmarkCalcColorTransform( EOTF_PQ, state ); }
BENCHMARK( BenchmarkCalcColorTransforms_PQ );

static void BenchmarkCalcColorTransforms( benchmark::State &state )
{
    for ( uint32_t nInputEOTF = 0; nInputEOTF < EOTF_Count; nInputEOTF++ )
        BenchmarkCalcColorTransform( ( EOTF )nInputEOTF, state );
}
BENCHMARK( BenchmarkCalcColorTransforms );

static constexpr uint32_t k_uFindTestValueCountLarge  = 524288;
static constexpr uint32_t k_uFindTestValueCountMedium = 16;
static constexpr uint32_t k_uFindTestValueCountSmall  = 5;

template<uint32_t uSize>
static __attribute__(( noinline )) std::array<int, uSize> GetFindTestValues( )
{
    static std::array<int, uSize> s_Values = []( )
    {
        std::array<int, uSize> values;
        for ( uint32_t i = 0; i < uSize; i++ )
            values[ i ] = rand( ) % 255;

        return values;
    }( );

    return s_Values;
}

// Large

static void Benchmark_Find_Large_Gamescope( benchmark::State &state )
{
    std::array<int, k_uFindTestValueCountLarge> values =
        GetFindTestValues<k_uFindTestValueCountLarge>( );

    for ( auto _ : state )
    {
        auto iter = gamescope::Algorithm::Find(
            values.begin( ), values.end( ), 765678478 );
        benchmark::DoNotOptimize( iter );
    }
}
BENCHMARK( Benchmark_Find_Large_Gamescope );

static void Benchmark_Find_Large_Std( benchmark::State &state )
{
    std::array<int, k_uFindTestValueCountLarge> values =
        GetFindTestValues<k_uFindTestValueCountLarge>( );

    for ( auto _ : state )
    {
        auto iter = std::find( values.begin( ), values.end( ), 765678478 );
        benchmark::DoNotOptimize( iter );
    }
}
BENCHMARK( Benchmark_Find_Large_Std );

static void Benchmark_Contains_Large_Gamescope( benchmark::State &state )
{
    std::array<int, k_uFindTestValueCountLarge> values =
        GetFindTestValues<k_uFindTestValueCountLarge>( );

    for ( auto _ : state )
    {
        bool bContains = gamescope::Algorithm::ContainsNoShortcut(
            values.begin( ), values.end( ), 765678478 );
        benchmark::DoNotOptimize( bContains );
    }
}
BENCHMARK( Benchmark_Contains_Large_Gamescope );

//

static void Benchmark_Find_Medium_Gamescope( benchmark::State &state )
{
    std::array<int, k_uFindTestValueCountMedium> values =
        GetFindTestValues<k_uFindTestValueCountMedium>( );

    for ( auto _ : state )
    {
        auto iter = gamescope::Algorithm::Find(
            values.begin( ), values.end( ), 765678478 );
        benchmark::DoNotOptimize( iter );
    }
}
BENCHMARK( Benchmark_Find_Medium_Gamescope );

static void Benchmark_Find_Medium_Std( benchmark::State &state )
{
    std::array<int, k_uFindTestValueCountMedium> values =
        GetFindTestValues<k_uFindTestValueCountMedium>( );

    for ( auto _ : state )
    {
        auto iter = std::find( values.begin( ), values.end( ), 765678478 );
        benchmark::DoNotOptimize( iter );
    }
}
BENCHMARK( Benchmark_Find_Medium_Std );

static void Benchmark_Contains_Medium_Gamescope( benchmark::State &state )
{
    std::array<int, k_uFindTestValueCountMedium> values =
        GetFindTestValues<k_uFindTestValueCountMedium>( );

    for ( auto _ : state )
    {
        bool bContains = gamescope::Algorithm::ContainsNoShortcut(
            values.begin( ), values.end( ), 765678478 );
        benchmark::DoNotOptimize( bContains );
    }
}
BENCHMARK( Benchmark_Contains_Medium_Gamescope );

//

static void Benchmark_Find_Small_Gamescope( benchmark::State &state )
{
    std::array<int, k_uFindTestValueCountSmall> values =
        GetFindTestValues<k_uFindTestValueCountSmall>( );

    for ( auto _ : state )
    {
        auto iter = gamescope::Algorithm::Find(
            values.begin( ), values.end( ), 765678478 );
        benchmark::DoNotOptimize( iter );
    }
}
BENCHMARK( Benchmark_Find_Small_Gamescope );

static void Benchmark_Find_Small_Std( benchmark::State &state )
{
    std::array<int, k_uFindTestValueCountSmall> values =
        GetFindTestValues<k_uFindTestValueCountSmall>( );

    for ( auto _ : state )
    {
        auto iter = std::find( values.begin( ), values.end( ), 765678478 );
        benchmark::DoNotOptimize( iter );
    }
}
BENCHMARK( Benchmark_Find_Small_Std );

static void Benchmark_Contains_Small_Gamescope( benchmark::State &state )
{
    std::array<int, k_uFindTestValueCountSmall> values =
        GetFindTestValues<k_uFindTestValueCountSmall>( );

    for ( auto _ : state )
    {
        bool bContains = gamescope::Algorithm::ContainsNoShortcut(
            values.begin( ), values.end( ), 765678478 );
        benchmark::DoNotOptimize( bContains );
    }
}
BENCHMARK( Benchmark_Contains_Small_Gamescope );

BENCHMARK_MAIN( );
