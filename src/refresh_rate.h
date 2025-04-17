#pragma once

#include <cstdint>

namespace gamescope
{
    static inline int32_t ConvertHztomHz( char* refreshHz )
    {
        std::vector svParts = Split(refreshHz, ".,");

        const uint64_t ulWholeNumLen = svParts[0].size();
        if (svParts.size() > 1)
            refreshHz[ulWholeNumLen] = '\0'; //replace the decimal separator with null terminator, so that atol can be used
        const uint64_t ulWholeNum = (uint64_t) atol(refreshHz);

	const auto [ulDecNum, ulDecNumLen] = [&svParts]() -> const std::array<uint64_t, 2> {
	    if (svParts.size()>1)
		return {(uint64_t)atol(svParts[1].data()), (uint64_t)svParts[1].size()};
	    return {0ul, 1ul};
	}();

	const uint64_t ulDecDenom = (uint64_t)lrintf(powf(10.0f, (int32_t)ulDecNumLen));

        //don't divide the decimal part by its denominator(/number of decimal places) first,
        //instead multiply the wholeNum by the number of decimal places, and then divide the result of adding the multiplied wholeNum & decimal parts.
        //This, along with doing all intermediate calculations w/ 64-bit unsigned ints, ensures there's no precision loss.
        const uint64_t ulMultiplied = ulWholeNum * 1'000lu * ulDecDenom + ulDecNum * 1'000lu;
        return (int32_t)(ulMultiplied/ulDecDenom);
    }

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
