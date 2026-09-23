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
	REQUIRE( PresentTargetThreshold( 100'000'000, k_uPresentTimingNearest, 33'333'334 ) == 83'333'333 );
	REQUIRE( PresentTargetThreshold( 100'000'000, 0, 16'666'667 ) == 100'000'000 );
	REQUIRE( PresentTargetThreshold( 100, 0, 40 ) == 100 );
	REQUIRE( PresentTargetThreshold( 0, k_uPresentTimingNearest, 16'666'667 ) == 0 );
}

TEST_CASE( "VRR prediction and wakes cannot reuse a stale vblank", "[present_timing_schedule]" )
{
	REQUIRE( PredictPresentTime( 100, 5, 10, 16 ) == 105 );
	REQUIRE( PredictPresentTime( 100, 5, 99, 16 ) == 115 );
	REQUIRE( PresentTargetWake( 120, 5, 100 ) == 115 );
	REQUIRE_FALSE( PresentTargetWake( 105, 5, 100 ) );
	REQUIRE_FALSE( PresentTargetWake( 3, 5, 100 ) );
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
