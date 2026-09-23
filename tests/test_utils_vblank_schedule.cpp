#include <catch2/catch_test_macros.hpp>
#include "Utils/VBlankSchedule.h"

using namespace gamescope;

namespace
{

uint64_t NextWake( uint64_t last, uint64_t interval, uint64_t offset, uint64_t now,
                   const VBlankScheduleTime &consumed, bool vrr = false )
{
	return GetNextVBlank( last, interval, offset, now, GetVBlankTargetFloor( consumed, interval, vrr ) );
}

constexpr VBlankScheduleTime consumed{ 1'004'166'667, 1'000'000'000 };

}

TEST_CASE("A shorter draw estimate cannot schedule a refresh twice", "[vblank_schedule]") {
	REQUIRE(NextWake( 1'000'000'000, 4'166'667, 4'166'667, 1'000'000'000, {} ) == 1'000'000'000);
	REQUIRE(NextWake( 1'000'000'000, 4'166'667, 4'118'333, 1'000'010'000, consumed ) == 1'004'215'001);
}

TEST_CASE("Feedback can refine an unconsumed refresh deadline", "[vblank_schedule]") {
	REQUIRE(NextWake( 1'000'020'000, 4'166'667, 4'118'333, 1'000'030'000, {} ) == 1'000'068'334);
}

TEST_CASE("Feedback phase corrections do not repeat a consumed refresh", "[vblank_schedule]") {
	REQUIRE(NextWake( 1'000'020'000, 4'166'667, 4'118'333, 1'000'030'000, consumed ) == 1'004'235'001);
	REQUIRE(NextWake( 999'980'000, 4'166'667, 4'118'333, 1'000'010'000, consumed ) == 1'004'195'001);
	// Late feedback still targets the next unconsumed cycle on the same grid.
	REQUIRE(NextWake( 995'833'333, 4'166'667, 4'118'333, 1'000'010'000, consumed ) == 1'004'215'001);
}

TEST_CASE("VRR feedback is not quantized to a fixed refresh phase", "[vblank_schedule]") {
	VBlankScheduleTime vrrConsumed{ 1'004'166'667, 1'003'866'667 };
	REQUIRE(NextWake( 1'000'020'000, 4'166'667, 300'000, 1'003'876'667, vrrConsumed, true ) == 1'003'886'667);
}

TEST_CASE("The half-cycle boundary belongs to the consumed refresh", "[vblank_schedule]") {
	REQUIRE(NextWake( 1'002'083'333, 4'166'667, 1'000'000, 1'005'000'000, consumed ) == 1'009'416'667);
	REQUIRE(NextWake( 1'002'083'334, 4'166'667, 1'000'000, 1'005'000'000, consumed ) == 1'005'250'001);
}

TEST_CASE("Refresh changes do not retain several cycles of old draw lead", "[vblank_schedule]") {
	VBlankScheduleTime slowConsumed{ 1'016'666'667, 1'000'000'000 };
	REQUIRE(NextWake( 1'000'000'000, 4'166'667, 4'166'667, 1'000'010'000, slowConsumed ) == 1'004'166'667);
	REQUIRE(NextWake( 1'000'000'000, 16'666'667, 4'650'000, 1'000'010'000, consumed ) == 1'012'016'667);
}

TEST_CASE("Small refresh corrections do not reset consumed cycles", "[vblank_schedule]") {
	REQUIRE(NextWake( 1'000'000'000, 4'166'649, 4'118'333, 1'000'010'000, consumed ) == 1'004'214'965);
	REQUIRE(NextWake( 1'000'000'000, 4'166'684, 4'118'333, 1'000'010'000, consumed ) == 1'004'215'035);
}

TEST_CASE("Missing feedback and idle periods still allow forward progress", "[vblank_schedule]") {
	VBlankScheduleTime last = consumed;
	for ( uint64_t target : { 1'008'333'334ul, 1'012'500'001ul, 1'016'666'668ul } )
	{
		uint64_t wake = NextWake( 1'000'000'000, 4'166'667, 4'118'333, last.ulScheduledWakeupPoint + 10'000, last );
		REQUIRE(wake + 4'118'333 == target);
		last = { target, wake };
	}
	REQUIRE(NextWake( 1'000'000'000, 4'166'667, 4'118'333, 2'000'000'000, last ) == 2'000'048'414);
}
