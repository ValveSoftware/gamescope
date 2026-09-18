#include <catch2/catch_test_macros.hpp>
#include "Utils/VBlankSchedule.h"

using namespace gamescope;

namespace
{

uint64_t NextWake( uint64_t last, uint64_t interval, uint64_t offset, uint64_t now,
                   const VBlankScheduleTime &consumed, bool vrr = false )
{
	return VBlank::Next({
		.ulLastVBlank = last,
		.ulInterval = interval,
		.ulOffset = offset,
		.ulNow = now,
		.bVRR = vrr,
	}, consumed).ulScheduledWakeupPoint;
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

TEST_CASE("Explicit cadence does not follow a faster host feedback grid", "[vblank_schedule]") {
	const VBlankScheduleTime previous{ 16'666'667, 14'666'667 };
	const auto next = VBlank::Next({
		.ulLastVBlank = 12'500'001,
		.ulInterval = 8'333'333,
		.ulOffset = 2'000'000,
		.ulNow = 14'676'667,
		.bRateOverride = true,
	}, previous);
	REQUIRE(next.ulTargetVBlank == 25'000'000);
	REQUIRE(next.ulScheduledWakeupPoint == 23'000'000);
}

TEST_CASE("Explicit cadence stays regular despite late or jittered feedback", "[vblank_schedule]") {
	VBlankScheduleTime previous{ 1'004'166'667, 1'000'000'000 };
	VBlank::ScheduleParams params{
		.ulInterval = 4'166'667,
		.ulOffset = 2'000'000,
		.bRateOverride = true,
	};
	for ( uint64_t feedback : { 1'000'020'000ul, 999'980'000ul, 1'008'340'000ul } )
	{
		params.ulLastVBlank = feedback;
		params.ulNow = previous.ulScheduledWakeupPoint + 10'000;
		auto next = VBlank::Next(params, previous);
		REQUIRE(next.ulTargetVBlank - previous.ulTargetVBlank == 4'166'667);
		previous = next;
	}

	params.ulNow = 2'000'000'000;
	const auto resumed = VBlank::Next(params, previous);
	REQUIRE(resumed.ulScheduledWakeupPoint >= params.ulNow);
	REQUIRE(resumed.ulScheduledWakeupPoint < params.ulNow + 4'166'667);
	REQUIRE((resumed.ulTargetVBlank - previous.ulTargetVBlank) % 4'166'667 == 0);
}

TEST_CASE("An explicit rate needs a full interval before another queued nudge", "[vblank_schedule]") {
	const VBlank::ScheduleParams params{ .ulInterval = 4'166'667, .bRateOverride = true };
	REQUIRE(VBlank::TargetFloor(consumed, params) == 1'008'333'333);
}

TEST_CASE("An explicit rate change clamps old draw lead without wedging", "[vblank_schedule]") {
	const VBlankScheduleTime previous{ 1'016'666'667, 1'000'000'000 };
	const auto next = VBlank::Next({
		.ulLastVBlank = 1'000'000'000,
		.ulInterval = 4'166'667,
		.ulOffset = 4'166'667,
		.ulNow = 1'000'010'000,
		.bRateOverride = true,
	}, previous);
	REQUIRE(next.ulTargetVBlank == 1'008'333'334);
	REQUIRE(next.ulScheduledWakeupPoint == 1'004'166'667);
}

TEST_CASE("Explicit rate selection leaves VRR feedback scheduling intact", "[vblank_schedule]") {
	const auto next = VBlank::Next({
		.ulLastVBlank = 1'000'020'000,
		.ulInterval = 4'166'667,
		.ulOffset = 300'000,
		.ulNow = 1'003'876'667,
		.bVRR = true,
		.bRateOverride = true,
	}, { 1'004'166'667, 1'003'866'667 });
	REQUIRE(next.ulTargetVBlank == 1'004'186'667);
}

TEST_CASE("An explicit cadence starts from feedback before its first accepted wake", "[vblank_schedule]") {
	const auto next = VBlank::Next({
		.ulLastVBlank = 1'000'000'000,
		.ulInterval = 8'333'333,
		.ulOffset = 2'000'000,
		.ulNow = 1'001'000'000,
		.bRateOverride = true,
	}, {});
	REQUIRE(next.ulTargetVBlank == 1'008'333'333);
	REQUIRE(next.ulScheduledWakeupPoint == 1'006'333'333);
}

TEST_CASE("Slowing an explicit cadence waits the new interval", "[vblank_schedule]") {
	const auto next = VBlank::Next({
		.ulLastVBlank = 1'002'000'000,
		.ulInterval = 16'666'667,
		.ulOffset = 2'000'000,
		.ulNow = 1'002'000'000,
		.bRateOverride = true,
	}, consumed);
	REQUIRE(next.ulTargetVBlank == 1'020'833'334);
}

TEST_CASE("Leaving VRR resumes explicit cadence without replaying missed slots", "[vblank_schedule]") {
	const auto next = VBlank::Next({
		.ulLastVBlank = 3'599'999'000'000,
		.ulInterval = 10'000'000,
		.ulOffset = 2'000'000,
		.ulNow = 3'600'000'000'000,
		.bRateOverride = true,
	}, { 20'000'000, 18'000'000 });
	REQUIRE(next.ulTargetVBlank == 3'600'010'000'000);
	REQUIRE(next.ulScheduledWakeupPoint == 3'600'008'000'000);
}

TEST_CASE("High nested rates bound a draw lead longer than one interval", "[vblank_schedule]") {
	const auto next = VBlank::Next({
		.ulLastVBlank = 1'000'000'000,
		.ulInterval = 1'000'000,
		.ulOffset = 1'650'000,
		.ulNow = 1'000'000'000,
		.bRateOverride = true,
	}, {});
	REQUIRE(next.ulTargetVBlank == 1'001'000'000);
	REQUIRE(next.ulScheduledWakeupPoint == 1'000'000'000);
}
