#pragma once

#include <algorithm>
#include <cstdint>

namespace gamescope
{
    struct VBlankScheduleTime
    {
        // The expected time for the vblank we want to target.
        uint64_t ulTargetVBlank = 0;
        // The vblank offset by the redzone/scheduling calculation.
        // This is when we want to wake-up by to meet that vblank time above.
        uint64_t ulScheduledWakeupPoint = 0;
    };

    inline uint64_t GetVBlankTargetFloor( const VBlankScheduleTime &last, uint64_t ulInterval, bool bVRR )
    {
        // VRR wakes have no fixed refresh phase and can follow a newly ready frame.
        if ( bVRR || !last.ulTargetVBlank )
            return 0;

        // A shorter refresh interval must not retain several cycles of old draw lead.
        uint64_t ulLastTarget = std::min( last.ulTargetVBlank, last.ulScheduledWakeupPoint + ulInterval );
        // Treat the nearest refresh as consumed even if feedback moves its phase later.
        return ulLastTarget + ulInterval / 2;
    }

    inline uint64_t GetNextVBlank( uint64_t ulLastVBlank, uint64_t ulInterval, uint64_t ulOffset,
                                  uint64_t ulNow, uint64_t ulTargetFloor )
    {
        uint64_t ulTargetPoint = ulLastVBlank + ulInterval - ulOffset;
        while ( ulTargetPoint < ulNow || ulTargetPoint + ulOffset <= ulTargetFloor )
            ulTargetPoint += ulInterval;
        return ulTargetPoint;
    }
}
