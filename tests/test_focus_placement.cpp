#include <catch2/catch_test_macros.hpp>

#include "focus_placement.h"

TEST_CASE("A screen-sized window belongs at the origin", "[focus_placement]") {
	const auto place = focus_placement(1013, 1066, 1920, 1080, 1920, 1080);
	REQUIRE(place.x == 0);
	REQUIRE(place.y == 0);
}

TEST_CASE("A window that fits and hangs off is clamped onto the screen", "[focus_placement]") {
	const auto place = focus_placement(1201, 878, 1127, 1064, 1920, 1080);
	REQUIRE(place.x == 793);
	REQUIRE(place.y == 16);
}

TEST_CASE("A window that fits at a negative origin is clamped to the near edge", "[focus_placement]") {
	const auto place = focus_placement(-236, -50, 1762, 833, 1920, 1080);
	REQUIRE(place.x == 0);
	REQUIRE(place.y == 0);
}

TEST_CASE("A window inside the screen keeps its position", "[focus_placement]") {
	const auto place = focus_placement(240, 90, 1440, 900, 1920, 1080);
	REQUIRE(place.x == 240);
	REQUIRE(place.y == 90);
}

TEST_CASE("An oversized window keeps its centering", "[focus_placement]") {
	const auto place = focus_placement(-241, -16, 1762, 833, 1280, 800);
	REQUIRE(place.x == -241);
	REQUIRE(place.y == -16);
}

TEST_CASE("Each axis is decided on its own", "[focus_placement]") {
	const auto place = focus_placement(200, -30, 2000, 700, 1920, 1080);
	REQUIRE(place.x == 200);
	REQUIRE(place.y == 0);
}
