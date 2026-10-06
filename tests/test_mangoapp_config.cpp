#include <catch2/catch_test_macros.hpp>

#include "mangoapp_config.h"

TEST_CASE("An empty config shows the overlay", "[mangoapp_config]") {
	REQUIRE(mangoapp_config_visible(""));
}

TEST_CASE("The session's boot config hides it", "[mangoapp_config]") {
	REQUIRE_FALSE(mangoapp_config_visible("no_display"));
	REQUIRE_FALSE(mangoapp_config_visible("no_display\n"));
	REQUIRE_FALSE(mangoapp_config_visible("  no_display  \r\n"));
}

TEST_CASE("Steam's overlay-off config hides it", "[mangoapp_config]") {
	REQUIRE_FALSE(mangoapp_config_visible("control=mangohud\nfsr_steam_sharpness=5\nnis_steam_sharpness=10\nno_display\n"));
}

TEST_CASE("Steam's overlay-on config shows it", "[mangoapp_config]") {
	REQUIRE(mangoapp_config_visible("control=mangohud\nfsr_steam_sharpness=5\nnis_steam_sharpness=10\npreset=2\n"));
}

TEST_CASE("no_display takes a value", "[mangoapp_config]") {
	REQUIRE(mangoapp_config_visible("no_display=0"));
	REQUIRE_FALSE(mangoapp_config_visible("no_display=1"));
	REQUIRE_FALSE(mangoapp_config_visible("no_display = 1"));
}

TEST_CASE("Preset 0 hides, only the first preset counts", "[mangoapp_config]") {
	REQUIRE_FALSE(mangoapp_config_visible("preset=0"));
	REQUIRE_FALSE(mangoapp_config_visible("preset=0,2,3"));
	REQUIRE(mangoapp_config_visible("preset=2,0"));
	REQUIRE(mangoapp_config_visible("preset=4"));
}

TEST_CASE("An explicit no_display beats the preset", "[mangoapp_config]") {
	REQUIRE(mangoapp_config_visible("preset=0\nno_display=0"));
	REQUIRE_FALSE(mangoapp_config_visible("preset=2\nno_display"));
}

TEST_CASE("Comments are ignored", "[mangoapp_config]") {
	REQUIRE(mangoapp_config_visible("# no_display\npreset=2"));
	REQUIRE_FALSE(mangoapp_config_visible("no_display # hidden for now"));
}

TEST_CASE("MANGOHUD_CONFIG replaces the file unless it sets read_cfg", "[mangoapp_config]") {
	REQUIRE(mangoapp_config_visible("no_display", "fps_limit=60"));
	REQUIRE_FALSE(mangoapp_config_visible("no_display", "read_cfg,fps_limit=60"));
}
