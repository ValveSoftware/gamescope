#include <catch2/catch_test_macros.hpp>
#include "Utils/PresentTiming.h"

using namespace gamescope;

TEST_CASE( "Present targets retain their relative anchor", "[present_timing_schedule]" )
{
	REQUIRE( ResolvePresentTarget( 20, k_uPresentTimingRelative, 0 ) == 0 );
	REQUIRE( ResolvePresentTarget( 20, k_uPresentTimingRelative, 100 ) == 120 );
	REQUIRE( ResolvePresentTarget( 20, 0, 100 ) == 20 );
	REQUIRE( ResolvePresentTarget( 20, k_uPresentTimingRelative, UINT64_MAX - 10 ) == UINT64_MAX );
}

TEST_CASE( "Only nearest-cycle targets may present early", "[present_timing_schedule]" )
{
	REQUIRE( PresentTargetThreshold( 100'000'000, k_uPresentTimingNearest, 33'333'334 ) == 83'333'334 );
	REQUIRE( PresentTargetThreshold( 100'000'000, k_uPresentTimingNearest, 33'333'333 ) == 83'333'334 );
	REQUIRE( PresentTargetThreshold( 100'000'000, 0, 16'666'667 ) == 100'000'000 );
	REQUIRE( PresentTargetThreshold( 100, 0, 40 ) == 100 );
	REQUIRE( PresentTargetThreshold( 0, k_uPresentTimingNearest, 16'666'667 ) == 0 );
}

TEST_CASE( "Relative targets count refreshes across re-anchored vblanks", "[present_timing_schedule]" )
{
	constexpr uint64_t anchor = 1'000'000'000, cycle = 16'666'667;
	// Two refreshes after an anchor whose flip timestamp lost its sub-microsecond part.
	REQUIRE( PresentTargetReached( 1'033'333'334, 1'033'332'999, anchor, cycle, cycle ) );
	REQUIRE( PresentTargetReached( 1'033'333'334, 1'033'334'001, anchor, cycle, cycle ) );
	REQUIRE_FALSE( PresentTargetReached( 1'033'333'334, 1'016'666'000, anchor, cycle, cycle ) );
	// A target between vblanks still waits for the first one at or after it.
	REQUIRE_FALSE( PresentTargetReached( 1'025'000'000, 1'016'666'667, anchor, cycle, cycle ) );
	REQUIRE( PresentTargetReached( 1'025'000'000, 1'033'333'334, anchor, cycle, cycle ) );
	REQUIRE( PresentTargetReached( 900'000'000, 0, anchor, cycle, cycle ) );
	REQUIRE_FALSE( PresentTargetReached( UINT64_MAX, UINT64_MAX - 1, anchor, cycle, cycle ) );
}

TEST_CASE( "Counted targets keep the nearest-cycle allowance", "[present_timing_schedule]" )
{
	constexpr uint64_t anchor = 1'000'000'000, cycle = 16'666'667;
	// Two refreshes with NEAREST still need both. Half a refresh only absorbs the re-anchor.
	const uint64_t nearest = PresentTargetThreshold( anchor + 2 * cycle, k_uPresentTimingNearest, cycle );
	REQUIRE( PresentTargetReached( nearest, anchor + 2 * cycle - 335, anchor, cycle, cycle ) );
	REQUIRE_FALSE( PresentTargetReached( nearest, anchor + cycle, anchor, cycle, cycle ) );
	// Under a 30 fps limit on 60 Hz, half the limited cycle is a whole base refresh and the tie keeps the later vblank.
	const uint64_t limited = PresentTargetThreshold( anchor + 4 * cycle, k_uPresentTimingNearest, 2 * cycle );
	REQUIRE( PresentTargetReached( limited, anchor + 4 * cycle - 335, anchor, cycle, cycle ) );
	REQUIRE_FALSE( PresentTargetReached( limited, anchor + 3 * cycle - 300, anchor, cycle, cycle ) );
}

TEST_CASE( "Counting rounds a re-phased grid to the nearest refresh", "[present_timing_schedule]" )
{
	constexpr uint64_t anchor = 1'000'000'000, cycle = 16'666'667;
	// A same-rate re-phase under half a refresh releases early. Past half it holds a refresh.
	REQUIRE( PresentTargetReached( anchor + 2 * cycle, anchor + 2 * cycle - 7'000'000, anchor, cycle, cycle ) );
	REQUIRE_FALSE( PresentTargetReached( anchor + 2 * cycle, anchor + 2 * cycle - 9'000'000, anchor, cycle, cycle ) );
	// Exactly half a refresh rounds up.
	REQUIRE( PresentTargetReached( 120, 115, 100, 10, 10 ) );
	REQUIRE_FALSE( PresentTargetReached( 120, 114, 100, 10, 10 ) );
}

TEST_CASE( "Targets compare times when the anchor is off the current grid", "[present_timing_schedule]" )
{
	constexpr uint64_t anchor = 1'000'000'000;
	// Anchored at 40 Hz, now at 60 Hz: two old refreshes must not release on the third new one.
	REQUIRE_FALSE( PresentTargetReached( 1'050'000'000, 1'041'666'667, anchor, 25'000'000, 16'666'667 ) );
	REQUIRE( PresentTargetReached( 1'050'000'000, 1'058'333'334, anchor, 25'000'000, 16'666'667 ) );
	// VRR has no grid, and absolute targets have no anchor.
	REQUIRE_FALSE( PresentTargetReached( 1'033'333'334, 1'033'332'999, anchor, 16'666'667, 0 ) );
	REQUIRE_FALSE( PresentTargetReached( 1'033'333'334, 1'033'332'999, 0, 0, 16'666'667 ) );
	REQUIRE( PresentTargetReached( 1'033'333'334, 1'033'333'334, 0, 0, 16'666'667 ) );
}

TEST_CASE( "VRR prediction and wakes cannot reuse a stale vblank", "[present_timing_schedule]" )
{
	REQUIRE( PredictPresentTime( 100, 5, 10, 16 ) == 105 );
	REQUIRE( PredictPresentTime( 100, 5, 99, 16 ) == 115 );
	REQUIRE( PresentTargetWake( 120, 5 ) == 115 );
	REQUIRE_FALSE( PresentTargetWake( 3, 5 ) );
}

TEST_CASE( "Fixed prediction stays on the refresh grid after a stale vblank", "[present_timing_schedule]" )
{
	REQUIRE( PredictFixedPresentTime( 90, 100, 16 ) == 100 );
	REQUIRE( PredictFixedPresentTime( 100, 100, 16 ) == 100 );
	REQUIRE( PredictFixedPresentTime( 101, 100, 16 ) == 116 );
	REQUIRE( PredictFixedPresentTime( 132, 100, 16 ) == 132 );
	REQUIRE( PredictFixedPresentTime( 133, 100, 16 ) == 148 );
	REQUIRE( PredictFixedPresentTime( UINT64_MAX - 1, UINT64_MAX - 4, 16 ) == UINT64_MAX );
	REQUIRE( PredictFixedPresentTime( 101, 100, 0 ) == 101 );
}

TEST_CASE( "A latch after the timer wakeup misses that vblank", "[present_timing_schedule]" )
{
	// Vblank at 100 with its flip submitted at the 96 wakeup, 16 per refresh.
	REQUIRE( PredictFixedLatchPresentTime( 96, 100, 96, 16, true ) == 100 );
	REQUIRE( PredictFixedLatchPresentTime( 97, 100, 96, 16, false ) == 116 );
	REQUIRE( PredictFixedLatchPresentTime( 99, 100, 96, 16, false ) == 116 );
	REQUIRE( PredictFixedLatchPresentTime( 105, 100, 96, 16, false ) == 116 );
	REQUIRE( PredictFixedLatchPresentTime( 113, 100, 96, 16, false ) == 132 );
	REQUIRE( PredictFixedLatchPresentTime( 101, 100, 96, 16, true ) == 116 );
	REQUIRE( PredictFixedLatchPresentTime( 97, 100, 101, 16, false ) == 100 );
	REQUIRE( PredictFixedLatchPresentTime( UINT64_MAX - 1, UINT64_MAX - 4, UINT64_MAX - 8, 16, false ) == UINT64_MAX );
}
