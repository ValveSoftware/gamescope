#include "color_helpers_impl.h"
#include <cstdio>

//#include <glm/ext.hpp>
#include <glm/gtx/string_cast.hpp>

/*
using ns_color_tests::nLutEdgeSize3d;
const uint32_t nLutSize1d = 4096;

uint16_t lut1d[nLutSize1d*4];
uint16_t lut3d[nLutEdgeSize3d*nLutEdgeSize3d*nLutEdgeSize3d*4];

lut1d_t lut1d_float;
lut3d_t lut3d_float;

static void BenchmarkCalcColorTransform(EOTF inputEOTF, benchmark::State &state)
{
    const primaries_t primaries = { { 0.602f, 0.355f }, { 0.340f, 0.574f }, { 0.164f, 0.121f } };
    const glm::vec2 white = { 0.3070f, 0.3220f };

    displaycolorimetry_t inputColorimetry{};
    inputColorimetry.primaries = primaries;
    inputColorimetry.white = white;

    displaycolorimetry_t outputEncodingColorimetry{};
    outputEncodingColorimetry.primaries = primaries;
    outputEncodingColorimetry.white = white;

    colormapping_t colorMapping{};

    tonemapping_t tonemapping{};
    tonemapping.bUseShaper = true;

    nightmode_t nightmode{};
    float flGain = 1.0f;

    for (auto _ : state) {
        calcColorTransform<nLutEdgeSize3d>( &lut1d_float, nLutSize1d, &lut3d_float, inputColorimetry, inputEOTF,
            outputEncodingColorimetry, EOTF_Gamma22,
            colorMapping, nightmode, tonemapping, nullptr, flGain );
        for ( size_t i=0, end = lut1d_float.dataR.size(); i<end; ++i )
        {
            lut1d[4*i+0] = quantize_lut_value_16bit( lut1d_float.dataR[i] );
            lut1d[4*i+1] = quantize_lut_value_16bit( lut1d_float.dataG[i] );
            lut1d[4*i+2] = quantize_lut_value_16bit( lut1d_float.dataB[i] );
            lut1d[4*i+3] = 0;
        }
        for ( size_t i=0, end = lut3d_float.data.size(); i<end; ++i )
        {
            lut3d[4*i+0] = quantize_lut_value_16bit( lut3d_float.data[i].r );
            lut3d[4*i+1] = quantize_lut_value_16bit( lut3d_float.data[i].g );
            lut3d[4*i+2] = quantize_lut_value_16bit( lut3d_float.data[i].b );
            lut3d[4*i+3] = 0;
        }
    }
}
*/

int color_tests()
{
#if 0
    {
        // Test normalized primary matrix
        primaries_t primaries = { { 0.602f, 0.355f }, { 0.340f, 0.574f }, { 0.164f, 0.121f } };
        glm::vec2 white = { 0.3070f, 0.3220f };

        glm::mat3x3 rgb_to_xyz = normalised_primary_matrix( primaries, white, 1.f );
        printf("normalised_primary_matrix rgb_to_xyz %s\n", glm::to_string(rgb_to_xyz).c_str() );

        glm::vec3 redXYZ = rgb_to_xyz * glm::vec3(1,0,0);
        glm::vec2 redxy = XYZ_to_xy( redXYZ );
        glm::vec3 whiteXYZ = rgb_to_xyz * glm::vec3(1);
        glm::vec2 whitexy = XYZ_to_xy( whiteXYZ );

        printf("r xy %s == %s \n", glm::to_string(primaries.r).c_str(), glm::to_string(redxy).c_str() );
        printf("w xy %s == %s \n", glm::to_string(white).c_str(), glm::to_string(whitexy).c_str() );
    }
#endif


#if 1
    {
        // chromatic adapatation
        glm::vec3 d50XYZ = glm::vec3(0.96422f, 1.00000f, 0.82521f );
        glm::vec3 d65XYZ = glm::vec3(0.95047f, 1.00000f, 1.08883f );
        printf("d50XYZ %s\n", glm::to_string(d50XYZ).c_str() );
        printf("d65XYZ %s\n", glm::to_string(d65XYZ).c_str() );

        glm::mat3x3 d65FromF50_reference_bradford( 0.9555766, -0.0282895, 0.0122982,
            -0.0230393, 1.0099416, -0.0204830,
            0.0631636, 0.0210077,  1.3299098 );
        printf("d65FromF50_reference_bradford %s\n", glm::to_string(d65FromF50_reference_bradford).c_str() );
        
        {
            
            glm::mat3x3 d65From50 = chromatic_adaptation_matrix( d50XYZ, d65XYZ, k_EChromaticAdapatationMethod_Bradford );
            printf("bradford d65From50 %s\n", glm::to_string(d65From50).c_str() );
            glm::vec3 d65_2 = d65From50 * d50XYZ;
            printf("bradford d65_2 %s\n", glm::to_string(d65_2).c_str() );
        }
        {
            glm::mat3x3 d65From50 = chromatic_adaptation_matrix( d50XYZ, d65XYZ, k_EChromaticAdapatationMethod_XYZ );
            printf("xyzscaling d65From50 %s\n", glm::to_string(d65From50).c_str() );
            glm::vec3 d65_2 = d65From50 * d50XYZ;
            printf("xyzscaling d65_2 %s\n", glm::to_string(d65_2).c_str() );
        }

    }
#endif

#if 0
    {

        int nLut3DSize = 4;
        float flScale = 1.f / ( (float) nLut3DSize - 1.f );
        for (int nBlue = 0; nBlue<nLut3DSize; ++nBlue )
        {
            for (int nGreen = 0; nGreen<nLut3DSize; ++nGreen )
            {
                for (int nRed = 0; nRed<nLut3DSize; ++nRed )
                {
                    glm::vec3 rgb1( nRed * flScale, nGreen * flScale, nBlue * flScale );
                    glm::vec3 hsv = rgb_to_hsv( rgb1 );
                    glm::vec3 rgb2 = hsv_to_rgb( hsv );
                    if ( !approxEqual(rgb1, rgb2 ) )
                    {
                        printf("****");
                    }
                    printf("%s %s %s\n", glm::to_string(rgb1).c_str(), glm::to_string(hsv).c_str(), glm::to_string(rgb2).c_str()  );
                }
            }
        }
    }
#endif

#if 0
    // Generate a 1d lut
    {
        int nLutsize = 9;
        lut1d_t lut;
        lut.resize( nLutsize );
        float flScale = 1.f / ( (float) nLutsize - 1.f );
        for ( int i = 0; i<nLutsize; ++i )
        {
            float f = flScale * (float) i;

            lut.dataR[i] = f * f;
            lut.dataG[i] = f * f;
            lut.dataB[i] = f * f;
        }
        lut.finalize();

        glm::vec3 rgb1 = ApplyLut1D_Linear( lut, glm::vec3( -1.f, 0.5f, 2.f ) );
        printf("%s\n", glm::to_string(rgb1).c_str() );

        glm::vec3 rgb2 = ApplyLut1D_Inverse_Linear( lut, rgb1 );
        printf("%s\n", glm::to_string(rgb2).c_str() );
    }
#endif

#if 0
    // Generate a 1d lut with a flat spot
    {
        int nLutsize = 9;
        lut1d_t lut;
        lut.resize( nLutsize );
        float flScale = 1.f / ( (float) nLutsize - 1.f );
        printf("LUT\n");
        for ( int i = 0; i<nLutsize; ++i )
        {
            float f = std::max( 0.25f, flScale * (float) i );
            lut.dataR[i] = f * f;
            lut.dataG[i] = f * f;
            lut.dataB[i] = f * f;
            printf("%f %d %f\n", f, i, lut.dataG[i] );
        }
        lut.finalize();

        printf("\n");
        nLutsize = 21;
        flScale = 1.f / ( (float) nLutsize - 1.f );
        for (int i=0; i<21; ++i)
        {
            float f = std::max( 0.25f, flScale * (float) i );
            glm::vec3 rgb1 = ApplyLut1D_Linear( lut, glm::vec3( f, f, f ) );
            glm::vec3 rgb2 = ApplyLut1D_Inverse_Linear( lut, rgb1 );
            printf("%f %f %f\n", f, rgb1.g, rgb2.g );
        }
        /*
        printf("%s\n", glm::to_string(rgb1).c_str() );
        glm::vec3 rgb2 = ApplyLut1D_Inverse_Linear( lut, rgb1 );
        printf("%s\n", glm::to_string(rgb2).c_str() );
        */
    }
#endif
   return 0;
}

void test_eetf2390_mono()
{
    printf("%s\n", __func__  );
    float vLumaLevels[] = { 0.0, 0.001, 0.01, 0.1, 1.0, 10.0, 100.0, 500.0, 1000.0, 5000.0, 10000.0, 15000.0 };

    //  map 0.01 -  10,000 -> 0.1 - 1000
    float sourceBlackNits = 0.01f;
    float sourceWhiteNits = 5000.0f;
    float sourceBlackPQ = nits_to_pq( sourceBlackNits );
    float sourceWhitePQ = nits_to_pq( sourceWhiteNits );

    printf("source\t%0.02f - %0.01f\t\tPQ10: %0.1f %0.1f \n", sourceBlackNits, sourceWhiteNits, sourceBlackPQ * 1023.f, sourceWhitePQ * 1023.f );

    float destBlackNits = 0.1f;
    float destWhiteNits = 1000.0f;
    float destBlackPQ = nits_to_pq( destBlackNits );
    float destWhitePQ = nits_to_pq( destWhiteNits );
    printf("dest\t%0.02f - %0.01f\t\tPQ10: %0.1f %0.1f\n", destBlackNits, destWhiteNits, destBlackPQ * 1023.f, destWhitePQ * 1023.f );
    printf("\n");

    eetf_2390_t eetf;
    eetf.init_pq( sourceBlackPQ, sourceWhitePQ, destBlackPQ, destWhitePQ );

    for ( size_t nLevel=0; nLevel < 12; ++nLevel )
    {
        float flInputNits = vLumaLevels[nLevel];
        float inputPQ = nits_to_pq( flInputNits );
        float tonemappedOutputPQ = eetf.apply_pq( inputPQ );
        float tonemappedOutputNits = pq_to_nits( tonemappedOutputPQ );
        printf("value\t%0.03f -> %0.03f\tPQ10: %0.1f -> %0.1f\n", flInputNits, tonemappedOutputNits, inputPQ * 1023.f, tonemappedOutputPQ * 1023.f );
    }
}

int main(int argc, char* argv[])
{
    printf("color_tests\n");
    // test_eetf2390_mono();
    color_tests();
    return 0;
}