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
        // Refresh period the target was stepped with, zero when unknown.
        uint64_t ulRefreshCycle = 0;
    };

    namespace VBlank
    {
        struct ScheduleParams
        {
            uint64_t ulLastVBlank = 0;
            uint64_t ulInterval = 0;
            uint64_t ulOffset = 0;
            uint64_t ulNow = 0;
            bool bVRR = false;
            bool bRateOverride = false;
        };

        inline uint64_t LastTarget( const VBlankScheduleTime &lastSchedule, uint64_t ulInterval )
        {
            // Do not retain several cycles of old draw lead when the rate increases.
            return std::min( lastSchedule.ulTargetVBlank, lastSchedule.ulScheduledWakeupPoint + ulInterval );
        }

        inline uint64_t TargetFloor( const VBlankScheduleTime &lastSchedule, const ScheduleParams &params )
        {
            // VRR wakes have no fixed phase and can follow a newly ready frame.
            if ( params.bVRR || !lastSchedule.ulTargetVBlank )
                return 0;

            const uint64_t ulSeparation = params.bRateOverride ? params.ulInterval - 1 : params.ulInterval / 2;
            return LastTarget( lastSchedule, params.ulInterval ) + ulSeparation;
        }

        inline VBlankScheduleTime Next( const ScheduleParams &params, const VBlankScheduleTime &lastSchedule )
        {
            uint64_t ulAnchor = params.ulLastVBlank;
            uint64_t ulOffset = params.ulOffset;
            if ( params.bRateOverride && !params.bVRR )
            {
                // An explicit rate owns its cadence, independent of host feedback.
                if ( lastSchedule.ulTargetVBlank )
                    ulAnchor = LastTarget( lastSchedule, params.ulInterval );
                ulOffset = std::min( ulOffset, params.ulInterval );
            }

            const uint64_t ulTargetFloor = TargetFloor( lastSchedule, params );
            uint64_t ulWake = ulAnchor + params.ulInterval - ulOffset;
            uint64_t ulEarliestWake = params.ulNow;
            if ( ulTargetFloor >= ulOffset )
                ulEarliestWake = std::max( ulEarliestWake, ulTargetFloor - ulOffset + 1 );
            // Step in one go, a headless session never marks a vblank so the gap grows with uptime.
            if ( ulWake < ulEarliestWake )
            {
                const uint64_t ulDelta = ulEarliestWake - ulWake;
                ulWake += ( ulDelta / params.ulInterval + ( ulDelta % params.ulInterval != 0 ) ) * params.ulInterval;
            }
            return { .ulTargetVBlank = ulWake + ulOffset, .ulScheduledWakeupPoint = ulWake, .ulRefreshCycle = params.ulInterval };
        }
    }
}
