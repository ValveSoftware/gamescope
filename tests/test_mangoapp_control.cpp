#include <catch2/catch_test_macros.hpp>

#include "mangoapp_control.h"

static mangoapp_ctrl_msgid1_v1 ctrl(uint8_t no_display, uint8_t reload_config = 0, uint8_t log_session = 0) {
	mangoapp_ctrl_msgid1_v1 msg = {};
	msg.hdr.msg_type = 2;
	msg.hdr.ctrl_msg_type = 1;
	msg.hdr.version = 1;
	msg.no_display = no_display;
	msg.reload_config = reload_config;
	msg.log_session = log_session;
	return msg;
}

static void fold(MangoappControlRelay_t &relay, const mangoapp_ctrl_msgid1_v1 &msg) {
	mangoapp_fold_control(&msg, sizeof(msg), relay);
}

TEST_CASE("Sets are kept, the last one wins", "[mangoapp_control]") {
	MangoappControlRelay_t relay;
	fold(relay, ctrl(1));
	REQUIRE(relay.obHide == true);
	fold(relay, ctrl(2));
	REQUIRE(relay.obHide == false);
	REQUIRE_FALSE(relay.bToggle);
}

TEST_CASE("Toggles cancel in pairs and flip a set", "[mangoapp_control]") {
	MangoappControlRelay_t relay;
	fold(relay, ctrl(3));
	REQUIRE(relay.bToggle);
	fold(relay, ctrl(3));
	REQUIRE_FALSE(relay.bToggle);

	fold(relay, ctrl(1));
	fold(relay, ctrl(3));
	REQUIRE(relay.obHide == false);
	REQUIRE_FALSE(relay.bToggle);
}

TEST_CASE("A reload drops what came before it", "[mangoapp_control]") {
	MangoappControlRelay_t relay;
	fold(relay, ctrl(1));
	fold(relay, ctrl(0, 3));
	REQUIRE(relay.bReloadConfig);
	REQUIRE_FALSE(relay.obHide.has_value());
	REQUIRE_FALSE(relay.bToggle);

	// Within one message the reload lands first, then no_display on top.
	fold(relay, ctrl(1, 1));
	REQUIRE(relay.obHide == true);
}

TEST_CASE("Logging folds like visibility and a reload stops it", "[mangoapp_control]") {
	MangoappControlRelay_t relay;
	fold(relay, ctrl(0, 0, 3));
	REQUIRE(relay.bToggleLogging);
	fold(relay, ctrl(0, 0, 1));
	REQUIRE(relay.obLogging == true);
	REQUIRE_FALSE(relay.bToggleLogging);
	fold(relay, ctrl(0, 0, 3));
	REQUIRE(relay.obLogging == false);

	// Within one message the start lands first, then the reload stops it.
	fold(relay, ctrl(0, 3, 1));
	REQUIRE(relay.bReloadConfig);
	REQUIRE(relay.obLogging == false);
	fold(relay, ctrl(0, 0, 1));
	REQUIRE(relay.obLogging == true);
}

TEST_CASE("Other control types and short messages are ignored", "[mangoapp_control]") {
	MangoappControlRelay_t relay;
	mangoapp_ctrl_msgid1_v1 other = ctrl(1);
	other.hdr.ctrl_msg_type = 2;
	fold(relay, other);
	REQUIRE_FALSE(relay.obHide.has_value());

	mangoapp_ctrl_msgid1_v1 hide = ctrl(1);
	mangoapp_fold_control(&hide, sizeof(hide) - 1, relay);
	REQUIRE_FALSE(relay.obHide.has_value());
}
