#pragma once

#include <cstdint>

namespace gamescope
{
    constexpr int32_t ConvertHztomHz( int32_t nRefreshHz )
    {
        return nRefreshHz * 1'000;
    }

    constexpr int32_t ConvertmHzToHz( int32_t nRefreshmHz )
    {
        return ( nRefreshmHz + 999 ) / 1'000;
    }

    constexpr uint32_t ConvertHztomHz( uint32_t nRefreshHz )
    {
        return nRefreshHz * 1'000;
    }

    constexpr uint32_t ConvertmHzToHz( uint32_t nRefreshmHz )
    {
        return ( nRefreshmHz + 999 ) / 1'000;
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
        return ( 1'000'000'000'000ul + nCycle - 1 ) / nCycle;
    }

    constexpr uint32_t mHzToRefreshCycle( int32_t nmHz )
    {
        // Same thing.
        return RefreshCycleTomHz( nmHz );
    }
}
