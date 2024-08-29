// Try to figure out when vblank is and notify steamcompmgr to render some time before it

#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>
#include <condition_variable>

#include <assert.h>
#include <fcntl.h>
#include <unistd.h>

#include "gpuvis_trace_utils.h"

#include "vblankmanager.hpp"
#include "steamcompmgr.hpp"
#include "main.hpp"
#include "refresh_rate.h"

LogScope g_VBlankLog("vblank");

namespace gamescope
{
	ConVar<bool> vblank_debug( "vblank_debug", false, "Enable vblank debug spew to stderr." );

	CVBlankTimer::CVBlankTimer()
	{
		m_ulTargetVBlank = get_time_in_nanos();
		m_ulLastVBlank = m_ulTargetVBlank;

		if ( !GetBackend()->NeedsFrameSync() )
		{
			// Majority of backends fall down this optimal
			// timerfd path, vs nudge thread.
			g_VBlankLog.infof( "Using timerfd." );
		}
		else
		{
			g_VBlankLog.infof( "Using nudge thread." );

			if ( pipe2( m_nNudgePipe, O_CLOEXEC | O_NONBLOCK ) != 0 )
			{
				g_VBlankLog.errorf_errno( "Failed to create VBlankTimer pipe." );
				abort();
			}

			std::thread vblankThread( [this]() { this->NudgeThread(); } );
			vblankThread.detach();
		}
	}

	CVBlankTimer::~CVBlankTimer()
	{
		std::unique_lock lock( m_ScheduleMutex );

		m_bRunning = false;

		m_bArmed = true;
		m_bArmed.notify_all();

		for ( int i = 0; i < 2; i++ )
		{
			if ( m_nNudgePipe[ i ] >= 0 )
			{
				close ( m_nNudgePipe[ i ] );
				m_nNudgePipe[ i ] = -1;
			}
		}
	}

	int CVBlankTimer::GetRefresh() const
	{
		return g_nNestedRefresh ? g_nNestedRefresh : g_nOutputRefresh;
	}

	uint64_t CVBlankTimer::GetLastVBlank() const
	{
		return m_ulLastVBlank;
	}

	uint64_t CVBlankTimer::GetNextVBlank( uint64_t ulOffset ) const
	{
		const uint64_t ulIntervalNSecs = mHzToRefreshCycle( GetRefresh() );
		const uint64_t ulNow = get_time_in_nanos();

		uint64_t ulTargetPoint = GetLastVBlank() + ulIntervalNSecs - ulOffset;

		while ( ulTargetPoint < ulNow )
			ulTargetPoint += ulIntervalNSecs;

		return ulTargetPoint;
	}

	VBlankScheduleTime CVBlankTimer::CalcNextWakeupTime( bool bPreemptive )
	{
		const GamescopeScreenType eScreenType = GetBackend()->GetScreenType();

		const int nRefreshRate = GetRefresh();
		const uint64_t ulRefreshInterval = mHzToRefreshCycle( nRefreshRate );

		bool bVRR = GetBackend()->GetCurrentConnector() && GetBackend()->GetCurrentConnector()->IsVRRActive();
		uint64_t ulOffset = 0;
		if ( !bVRR )
		{
			// The redzone is relative to 60Hz for external displays.
			// Scale it by our target refresh so we don't miss submitting for
			// vblank in DRM.
			// (This fixes wonky frame-pacing on 4K@30Hz screens)
			//
			// TODO(Josh): Is this fudging still needed with our SteamOS kernel patches
			// to not account for vertical front porch when dealing with the vblank
			// drm_commit is going to target?
			// Need to re-test that.
			const uint64_t ulRedZone = eScreenType == GAMESCOPE_SCREEN_TYPE_INTERNAL
				? m_ulVBlankDrawBufferRedZone
				: std::min<uint64_t>( m_ulVBlankDrawBufferRedZone, ( m_ulVBlankDrawBufferRedZone * 60'000 * nRefreshRate ) / 60'000 );

			const uint64_t ulDecayAlpha = m_ulVBlankRateOfDecayPercentage; // eg. 980 = 98%

			uint64_t ulDrawTime = m_ulLastDrawTime;
			/// See comment of m_ulVBlankDrawTimeMinCompositing.
			if ( m_bCurrentlyCompositing )
				ulDrawTime = std::max( ulDrawTime, m_ulVBlankDrawTimeMinCompositing );

			uint64_t ulNewRollingDrawTime;
			// This is a rolling average when ulDrawTime < m_ulRollingMaxDrawTime,
			// and a maximum when ulDrawTime > m_ulRollingMaxDrawTime.
			//
			// This allows us to deal with spikes in the draw buffer time very easily.
			// eg. if we suddenly spike up (eg. because of test commits taking a stupid long time),
			// we will then be able to deal with spikes in the long term, even if several commits after
			// we get back into a good state and then regress again.

			// If we go over half of our deadzone, be more defensive about things and
			// spike up back to our current drawtime (sawtooth).
			if ( int64_t( ulDrawTime ) - int64_t( ulRedZone / 2 ) > int64_t( m_ulRollingMaxDrawTime ) )
				ulNewRollingDrawTime = ulDrawTime;
			else
				ulNewRollingDrawTime = ( ( ulDecayAlpha * m_ulRollingMaxDrawTime ) + ( kVBlankRateOfDecayMax - ulDecayAlpha ) * ulDrawTime ) / kVBlankRateOfDecayMax;

			// If we need to offset for our draw more than half of our vblank, something is very wrong.
			// Clamp our max time to half of the vblank if we can.
			ulNewRollingDrawTime = std::min( ulNewRollingDrawTime, ulRefreshInterval - ulRedZone );

			// If this is not a pre-emptive re-arming, then update
			// the rolling internal max draw time for next time.
			if ( !bPreemptive )
				m_ulRollingMaxDrawTime = ulNewRollingDrawTime;

			ulOffset = ulNewRollingDrawTime + ulRedZone;

			if ( vblank_debug && !bPreemptive )
				VBlankDebugSpew( ulOffset, ulDrawTime, ulRedZone );
		}
		else
		{
			// See above.
			if ( !bPreemptive )
			{
				// Reset the max draw time to default, it is unused for VRR.
				m_ulRollingMaxDrawTime = kStartingVBlankDrawTime;
			}

			uint64_t ulRedZone = kVRRFlushingTime;

			uint64_t ulDrawTime = 0;
			/// See comment of m_ulVBlankDrawTimeMinCompositing.
			if ( m_bCurrentlyCompositing )
				ulDrawTime = std::max( ulDrawTime, m_ulVBlankDrawTimeMinCompositing );

			ulOffset = ulDrawTime + ulRedZone;

			if ( vblank_debug && !bPreemptive )
				VBlankDebugSpew( ulOffset, ulDrawTime, ulRedZone );
		}

		const uint64_t ulScheduledWakeupPoint = GetNextVBlank( ulOffset );
		const uint64_t ulTargetVBlank = ulScheduledWakeupPoint + ulOffset;

		VBlankScheduleTime schedule =
		{
			.ulTargetVBlank = ulTargetVBlank,
			.ulScheduledWakeupPoint = ulScheduledWakeupPoint,
		};
		return schedule;
	}

	std::optional<VBlankTime> CVBlankTimer::ProcessVBlank()
	{
		return std::exchange( m_PendingVBlank, std::nullopt );
	}

	void CVBlankTimer::MarkVBlank( uint64_t ulNanos, bool bReArmTimer )
	{
		m_ulLastVBlank = ulNanos;
		if ( bReArmTimer )
		{
			// Force timer re-arm with the new vblank timings.
			ArmNextVBlank( false );
		}
	}

	bool CVBlankTimer::WasCompositing() const
	{
		return m_bCurrentlyCompositing;
	}

	void CVBlankTimer::UpdateWasCompositing( bool bCompositing )
	{
		m_bCurrentlyCompositing = bCompositing;
	}

	void CVBlankTimer::UpdateLastDrawTime( uint64_t ulNanos )
	{
		m_ulLastDrawTime = ulNanos;
	}

	void CVBlankTimer::WaitToBeArmed()
	{
		// Wait for m_bArmed to change *from* false.
		m_bArmed.wait( false );
	}

	void CVBlankTimer::ArmNextVBlank( bool bPreemptive )
	{
		std::unique_lock lock( m_ScheduleMutex );

		// If we're pre-emptively re-arming, don't
		// do anything if we are already armed.
		if ( bPreemptive && m_bArmed )
			return;

		m_bArmed = true;
		m_bArmed.notify_all();

		if ( UsingTimerFD() )
		{
			m_TimerFDSchedule = CalcNextWakeupTime( bPreemptive );

			ITimerWaitable::ArmTimer( m_TimerFDSchedule.ulScheduledWakeupPoint );
		}
	}

	bool CVBlankTimer::UsingTimerFD() const
	{
		return m_nNudgePipe[ 0 ] < 0;
	}

	int CVBlankTimer::GetFD()
	{
		return UsingTimerFD() ? ITimerWaitable::GetFD() : m_nNudgePipe[ 0 ];
	}

	void CVBlankTimer::OnPollIn()
	{
		if ( UsingTimerFD() )
		{
			std::unique_lock lock( m_ScheduleMutex );

			// Disarm the timer if it was armed.
			if ( !m_bArmed.exchange( false ) )
				return;


			m_PendingVBlank = VBlankTime
			{
				.schedule = m_TimerFDSchedule,
				// One might think this should just be 'now', however consider the fact
				// that the effective draw-time should also include the scheduling quantums
				// and any work before we reached this poll.
				// The old path used to be be on its own thread, simply awaking from sleep
				// then writing to a pipe and going back to sleep, the wakeup time was before we
				// did the write, so we included the quantum of pipe nudge -> wakeup.
				// Doing this aims to include that, like we were before, but with timerfd.
				.ulWakeupTime = m_TimerFDSchedule.ulScheduledWakeupPoint,
			};

			gpuvis_trace_printf( "vblank timerfd wakeup" );

			ITimerWaitable::DisarmTimer();
		}
		else
		{
			VBlankTime time{};
			for ( ;; )
			{
				ssize_t ret = read( m_nNudgePipe[ 0 ], &time, sizeof( time ) );

				if ( ret < 0 )
				{
					if ( errno == EAGAIN )
						continue;

					g_VBlankLog.errorf_errno( "Failed to read nudge pipe. Pre-emptively re-arming." );
					ArmNextVBlank( true );
					return;
				}
				else if ( ret != sizeof( VBlankTime ) )
				{
					g_VBlankLog.errorf( "Nudge pipe had less data than sizeof( VBlankTime ). Pre-emptively re-arming." );
					ArmNextVBlank( true );
					return;
				}
				else
				{
					break;
				}
			}

			uint64_t ulDiff = get_time_in_nanos() - time.ulWakeupTime;
			if ( ulDiff > 1'000'000ul )
			{
				gpuvis_trace_printf( "Ignoring stale vblank... Pre-emptively re-arming." );
				ArmNextVBlank( true );
				return;
			}

			gpuvis_trace_printf( "got vblank" );
			m_PendingVBlank = time;
		}
	}

	void CVBlankTimer::VBlankDebugSpew( uint64_t ulOffset, uint64_t ulDrawTime, uint64_t ulRedZone )
	{
		static uint64_t s_ulVBlankID = 0;
		static uint64_t s_ulLastDrawTime = kStartingVBlankDrawTime;
		static uint64_t s_ulLastOffset = kStartingVBlankDrawTime + ulRedZone;

		if ( s_ulVBlankID++ % 300 == 0 || ulDrawTime > s_ulLastOffset )
		{
			if ( ulDrawTime > s_ulLastOffset )
				g_VBlankLog.infof( " !! missed vblank " );

			g_VBlankLog.infof( "redZone: %.2fms decayRate: %lu%% - rollingMaxDrawTime: %.2fms lastDrawTime: %.2fms lastOffset: %.2fms - drawTime: %.2fms offset: %.2fms",
				ulRedZone / 1'000'000.0,
				m_ulVBlankRateOfDecayPercentage,
				m_ulRollingMaxDrawTime / 1'000'000.0,
				s_ulLastDrawTime / 1'000'000.0,
				s_ulLastOffset / 1'000'000.0,
				ulDrawTime / 1'000'000.0,
				ulOffset / 1'000'000.0 );
		}

		s_ulLastDrawTime = ulDrawTime;
		s_ulLastOffset = ulOffset;
	}

	void CVBlankTimer::NudgeThread()
	{
		pthread_setname_np( pthread_self(), "gamescope-vblk" );

		for ( ;; )
		{
			WaitToBeArmed();

			if ( !m_bRunning )
				return;

			VBlankScheduleTime schedule;
			if ( GetBackend()->GetCurrentConnector() )
			{
				schedule = GetBackend()->GetCurrentConnector()->FrameSync();
			}
			else
			{
				// If we don't currently have a connector, make up some dummy refresh cycle.
				sleep_for_nanos( mHzToRefreshCycle( g_nNestedRefresh ? g_nNestedRefresh : g_nOutputRefresh ) );
        		uint64_t ulNow = get_time_in_nanos();
				schedule = VBlankScheduleTime
				{
					.ulTargetVBlank  = ulNow + 3'000'000,
					.ulScheduledWakeupPoint = ulNow,
				};
			}

			const uint64_t ulWakeupTime = get_time_in_nanos();
			{
				std::unique_lock lock( m_ScheduleMutex );

				// Unarm, we are processing now!
				m_bArmed = false;

				VBlankTime timeInfo =
				{
					.schedule = schedule,
					.ulWakeupTime = ulWakeupTime,
				};

				ssize_t ret = write( m_nNudgePipe[ 1 ], &timeInfo, sizeof( timeInfo ) );
				if ( ret <= 0 )
				{
					g_VBlankLog.errorf_errno( "Nudge write failed" );
				}
				else
				{
					gpuvis_trace_printf( "sent vblank (nudge thread)" );
				}
			}
		}
	}
}

gamescope::CVBlankTimer &GetVBlankTimer()
{
    static gamescope::CVBlankTimer s_VBlankTimer;
    return s_VBlankTimer;
}

