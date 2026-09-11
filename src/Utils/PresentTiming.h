#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace gamescope
{
    constexpr uint32_t k_uPresentTimingRelative = 1;
    constexpr uint32_t k_uPresentTimingNearest = 2;

    inline uint64_t SaturatingPresentAdd( uint64_t a, uint64_t b )
    {
        return a > UINT64_MAX - b ? UINT64_MAX : a + b;
    }

    inline uint64_t ResolvePresentTarget( uint64_t target, uint32_t flags, uint64_t previous )
    {
        if ( flags & k_uPresentTimingRelative )
            return previous ? SaturatingPresentAdd( previous, target ) : 0;
        return target;
    }

    inline uint64_t PresentTargetThreshold( uint64_t target, uint32_t flags, uint64_t cycle )
    {
        uint64_t tolerance = flags & k_uPresentTimingNearest ? cycle / 2 : 0;
        return target > tolerance ? target - tolerance : 0;
    }

    inline uint64_t PredictFixedPresentTime( uint64_t now, uint64_t target, uint64_t interval )
    {
        if ( target >= now )
            return target;
        if ( !interval )
            return now;
        uint64_t remainder = ( now - target ) % interval;
        return SaturatingPresentAdd( now, remainder ? interval - remainder : 0 );
    }

    inline uint64_t PredictPresentTime( uint64_t now, uint64_t offset, uint64_t last, uint64_t interval )
    {
        return std::max( SaturatingPresentAdd( now, offset ), SaturatingPresentAdd( last, interval ) );
    }

    inline std::optional<uint64_t> PresentTargetWake( uint64_t target, uint64_t offset, uint64_t now )
    {
        if ( target > offset && target - offset > now )
            return target - offset;
        return std::nullopt;
    }
}
