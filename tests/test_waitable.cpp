#include <catch2/catch_test_macros.hpp>

#include "waitable.h"

#include <poll.h>

LogScope g_WaitableLog{ "waitable" };

timespec nanos_to_timespec( uint64_t nanos )
{
	return { .tv_sec = time_t( nanos / 1'000'000'000 ), .tv_nsec = long( nanos % 1'000'000'000 ) };
}

TEST_CASE("Timer expirations are consumed once", "[waitable]") {
	gamescope::ITimerWaitable timer;
	timer.ArmTimer( 1 );
	pollfd fd{ .fd = timer.GetFD(), .events = POLLIN };
	REQUIRE(poll( &fd, 1, 1000 ) == 1);
	REQUIRE(timer.ReadExpirations() == 1);
	REQUIRE(timer.ReadExpirations() == 0);
}

TEST_CASE("Rearming invalidates queued timer readiness", "[waitable]") {
	gamescope::ITimerWaitable timer;
	timer.ArmTimer( 1 );
	pollfd fd{ .fd = timer.GetFD(), .events = POLLIN };
	REQUIRE(poll( &fd, 1, 1000 ) == 1);

	timespec now;
	REQUIRE(clock_gettime( CLOCK_MONOTONIC, &now ) == 0);
	timer.ArmTimer( uint64_t( now.tv_sec ) * 1'000'000'000 + now.tv_nsec + 60'000'000'000 );
	REQUIRE(timer.ReadExpirations() == 0);

	itimerspec remaining;
	REQUIRE(timerfd_gettime( fd.fd, &remaining ) == 0);
	REQUIRE(remaining.it_value.tv_sec > 0);
}
