#pragma once

#include <optional>
#include "waitable.h"

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

    struct VBlankTime
    {
        VBlankScheduleTime schedule;
        // This is when we woke-up either by the timerfd poll
        // or on the nudge thread. We use this to feed-back into
        // the draw time so we automatically account for th
        //  CPU scheduler quantums.
        uint64_t ulWakeupTime = 0;
    };

    class CVBlankTimer : public ITimerWaitable
    {
    public:
        static constexpr uint64_t kMilliSecInNanoSecs = 1'000'000ul;
        // VBlank timer defaults and starting values.
        // Anything time-related is nanoseconds unless otherwise specified.
        static constexpr uint64_t kStartingVBlankDrawTime = 3'000'000ul;
        static constexpr uint64_t kDefaultMinVBlankTime = 350'000ul;
        static constexpr uint64_t kDefaultVBlankRedZone = 1'650'000ul;
        static constexpr uint64_t kDefaultVBlankDrawTimeMinCompositing = 2'400'000ul;
        static constexpr uint64_t kDefaultVBlankRateOfDecayPercentage = 980ul; // 98%
        static constexpr uint64_t kVBlankRateOfDecayMax = 1000ul; // 100%

        static constexpr uint64_t kVRRFlushingDrawTime = 1'000'000; // Could possibly be lower, like 300'000 or something.

        CVBlankTimer();
        ~CVBlankTimer();

        int GetRefresh() const;
        uint64_t GetLastVBlank() const;
        uint64_t GetNextVBlank( uint64_t ulOffset ) const;

        VBlankScheduleTime CalcNextWakeupTime( bool bPreemptive );
        void Reschedule();

        std::optional<VBlankTime> ProcessVBlank();
        void MarkVBlank( uint64_t ulNanos, bool bReArmTimer );

        bool WasCompositing() const;
        void UpdateWasCompositing( bool bCompositing );
        void UpdateLastDrawTime( uint64_t ulNanos );

        void WaitToBeArmed();
        void ArmNextVBlank( bool bPreemptive );

        bool UsingTimerFD() const;
        int GetFD() final;
        void OnPollIn() final;
    private:
        void VBlankDebugSpew( uint64_t ulOffset, uint64_t ulDrawTime, uint64_t ulRedZone );

        uint64_t m_ulTargetVBlank = 0;
        std::atomic<uint64_t> m_ulLastVBlank = { 0 };
        std::atomic<bool> m_bArmed = { false };
        std::atomic<bool> m_bRunning = { true };

        std::optional<VBlankTime> m_PendingVBlank;

        // Should have 0 contest, but just to be safe.
        // This also covers setting of m_bArmed, etc
        // so we keep in sequence.
        // m_bArmed is atomic so can still be .wait()'ed
        // on/read outside.
        // Does not cover m_ulLastVBlank, this is just atomic.
        std::mutex m_ScheduleMutex;
        VBlankScheduleTime m_TimerFDSchedule{};

        std::thread m_NudgeThread;
        int m_nNudgePipe[2] = { -1, -1 };

        /////////////////////////////
        // Scheduling bits and bobs.
        /////////////////////////////

        // Are we currently compositing? We may need
        // to push back to avoid clock feedback loops if so.
        // This is fed-back from steamcompmgr.
        std::atomic<bool> m_bCurrentlyCompositing = { false };
        // This is the last time a 'draw' took from wake-up to page flip.
        // 3ms by default to get the ball rolling.
        // This is calculated by steamcompmgr/drm and fed-back to the vblank timer.
        std::atomic<uint64_t> m_ulLastDrawTime = { kStartingVBlankDrawTime };

        //////////////////////////////////
        // VBlank timing tuneables below!
        //////////////////////////////////

        // Internal rolling peak exponential avg. draw time.
        // This is updated in CalcNextWakeupTime when not
        // doing pre-emptive timer re-arms.
        uint64_t m_ulRollingMaxDrawTime = kStartingVBlankDrawTime;

        // This accounts for some time we cannot account for (which (I think) is the drm_commit -> triggering the pageflip)
        // It would be nice to make this lower if we can find a way to track that effectively
        // Perhaps the missing time is spent elsewhere, but given we track from the pipe write
        // to after the return from `drm_commit` -- I am very doubtful.
        // 1.3ms by default. (kDefaultMinVBlankTime)
        uint64_t m_ulMinVBlankTime = kDefaultMinVBlankTime;

        // The leeway we always apply to our buffer.
        // 0.3ms by default. (kDefaultVBlankRedZone)
        uint64_t m_ulVBlankDrawBufferRedZone = kDefaultVBlankRedZone;

        // The minimum drawtime to use when we are compositing.
        // Getting closer and closer to vblank when compositing means that we can get into
        // a feedback loop with our GPU clocks. Pick a sane minimum draw time.
        // 2.4ms by default. (kDefaultVBlankDrawTimeMinCompositing)
        uint64_t m_ulVBlankDrawTimeMinCompositing = kDefaultVBlankDrawTimeMinCompositing;

        // The rate of decay (as a percentage) of the rolling average -> current draw time
        // 930 = 93%.
        // 93% by default. (kDefaultVBlankRateOfDecayPercentage)
        uint64_t m_ulVBlankRateOfDecayPercentage = kDefaultVBlankRateOfDecayPercentage;

        void NudgeThread();
    };
}

gamescope::CVBlankTimer &GetVBlankTimer();

