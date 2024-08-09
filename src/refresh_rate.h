#pragma once

#include <cstdint>

namespace gamescope
{
    int32_t ConvertHztomHz( const char* refreshHz ); //function definition in main.cpp
    //function definition not included here, because having the function definition here gave me linker errors - sharkautarch

    constexpr int32_t ConvertHztomHz( int32_t nRefreshHz )
    {
        return nRefreshHz * 1'000;
    }

    constexpr int32_t ConvertmHzToHz( int32_t nRefreshmHz )
    {
        // Round to nearest when going to mHz.
        // Ceil seems to be wrong when we have 60.001 or 90.004 etc.
        // Floor seems to be bad if we have 143.99
        // So round to nearest.

        return ( nRefreshmHz + 499 ) / 1'000;
    }

    constexpr uint32_t ConvertHztomHz( uint32_t nRefreshHz )
    {
        return nRefreshHz * 1'000;
    }

    constexpr uint32_t ConvertmHzToHz( uint32_t nRefreshmHz )
    {
        return ( nRefreshmHz + 499 ) / 1'000;
    }

    constexpr float ConvertHztomHz( float flRefreshHz )
    {
        return flRefreshHz * 1000.0f;
    }

    constexpr float ConvertmHzToHz( float nRefreshmHz )
    {
        return ( nRefreshmHz ) / 1'000.0;
    }

    constexpr uint32_t RefreshCycleTomHz( int32_t nCycle )
    {
        // Round cycle to nearest.
        return ( 1'000'000'000'000ul + ( nCycle / 2 ) - 1 ) / nCycle;
    }

    constexpr uint32_t mHzToRefreshCycle( int32_t nmHz )
    {
        // Same thing.
        return RefreshCycleTomHz( nmHz );
    }
}
