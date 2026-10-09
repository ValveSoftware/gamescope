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
        // Strictly under half a cycle, so a target exactly between two vblanks keeps the later one.
        uint64_t tolerance = flags & k_uPresentTimingNearest && cycle ? ( cycle - 1 ) / 2 : 0;
        return target > tolerance ? target - tolerance : 0;
    }

    // Each flip re-anchors the vblank grid, so count refreshes from an anchor on the same grid.
    inline bool PresentTargetReached( uint64_t threshold, uint64_t predicted, uint64_t anchor, uint64_t anchorGrid, uint64_t grid )
    {
        if ( !anchor || !grid || anchorGrid != grid )
            return threshold <= predicted;
        if ( threshold <= anchor )
            return true;
        const uint64_t span = threshold - anchor;
        const uint64_t required = span / grid + ( span % grid != 0 );
        const uint64_t elapsed = predicted > anchor ? predicted - anchor : 0;
        return elapsed / grid + ( elapsed % grid >= ( grid + 1 ) / 2 ) >= required;
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

    // A latch outside the pass that flips for target must also beat its wakeup.
    inline uint64_t PredictFixedLatchPresentTime( uint64_t now, uint64_t target, uint64_t wakeup, uint64_t interval, bool bInPass )
    {
        uint64_t lead = !bInPass && target > wakeup ? target - wakeup : 0;
        return PredictFixedPresentTime( SaturatingPresentAdd( now, lead ), target, interval );
    }

    inline uint64_t PredictPresentTime( uint64_t now, uint64_t offset, uint64_t last, uint64_t interval )
    {
        return std::max( SaturatingPresentAdd( now, offset ), SaturatingPresentAdd( last, interval ) );
    }

    // A hold was predicted before its pass ran, so a wake already due must still fire to re-predict.
    inline std::optional<uint64_t> PresentTargetWake( uint64_t target, uint64_t offset )
    {
        if ( target > offset )
            return target - offset;
        return std::nullopt;
    }
}
