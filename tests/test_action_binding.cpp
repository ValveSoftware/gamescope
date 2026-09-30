#include <catch2/catch_test_macros.hpp>

#include "action_binding.h"
#include "convar.h"
#include "Script/Script.h"

#include <xkbcommon/xkbcommon-keysyms.h>

using namespace gamescope;

TEST_CASE("ParseHotkeyStringToKeysyms", "[action_binding]") {
	SECTION("Single key parsing") {
		auto optF = ParseHotkeyStringToKeysyms("f");
		REQUIRE(optF.has_value());
		REQUIRE(optF->size() == 1);
		REQUIRE(optF->contains(XKB_KEY_F));

		auto optUpperF = ParseHotkeyStringToKeysyms("F");
		REQUIRE(optUpperF.has_value());
		REQUIRE(optUpperF->size() == 1);
		REQUIRE(optUpperF->contains(XKB_KEY_F));

		auto optReturn = ParseHotkeyStringToKeysyms("Return");
		REQUIRE(optReturn.has_value());
		REQUIRE(optReturn->size() == 1);
		REQUIRE(optReturn->contains(XKB_KEY_Return));

		auto optEnter = ParseHotkeyStringToKeysyms("Enter");
		REQUIRE(optEnter.has_value());
		REQUIRE(optEnter->size() == 1);
		REQUIRE(optEnter->contains(XKB_KEY_Return));

		auto optEsc = ParseHotkeyStringToKeysyms("esc");
		REQUIRE(optEsc.has_value());
		REQUIRE(optEsc->size() == 1);
		REQUIRE(optEsc->contains(XKB_KEY_Escape));

		auto optSpace = ParseHotkeyStringToKeysyms("space");
		REQUIRE(optSpace.has_value());
		REQUIRE(optSpace->size() == 1);
		REQUIRE(optSpace->contains(XKB_KEY_space));
	}

	SECTION("Modifier parsing and normalization") {
		auto optSuper = ParseHotkeyStringToKeysyms("Super");
		REQUIRE(optSuper.has_value());
		REQUIRE(optSuper->contains(XKB_KEY_Super_L));

		auto optSuperR = ParseHotkeyStringToKeysyms("Super_R");
		REQUIRE(optSuperR.has_value());
		REQUIRE(optSuperR->contains(XKB_KEY_Super_R));

		auto optCtrl = ParseHotkeyStringToKeysyms("Ctrl");
		REQUIRE(optCtrl.has_value());
		REQUIRE(optCtrl->contains(XKB_KEY_Control_L));

		auto optControl = ParseHotkeyStringToKeysyms("Control");
		REQUIRE(optControl.has_value());
		REQUIRE(optControl->contains(XKB_KEY_Control_L));

		auto optControlR = ParseHotkeyStringToKeysyms("Control_R");
		REQUIRE(optControlR.has_value());
		REQUIRE(optControlR->contains(XKB_KEY_Control_R));

		auto optAlt = ParseHotkeyStringToKeysyms("Alt");
		REQUIRE(optAlt.has_value());
		REQUIRE(optAlt->contains(XKB_KEY_Alt_L));

		auto optAltR = ParseHotkeyStringToKeysyms("Alt_R");
		REQUIRE(optAltR.has_value());
		REQUIRE(optAltR->contains(XKB_KEY_Alt_R));

		auto optAltGr = ParseHotkeyStringToKeysyms("AltGr");
		REQUIRE(optAltGr.has_value());
		REQUIRE(optAltGr->contains(XKB_KEY_Alt_R));

		auto optShift = ParseHotkeyStringToKeysyms("Shift");
		REQUIRE(optShift.has_value());
		REQUIRE(optShift->contains(XKB_KEY_Shift_L));

		auto optShiftR = ParseHotkeyStringToKeysyms("Shift_R");
		REQUIRE(optShiftR.has_value());
		REQUIRE(optShiftR->contains(XKB_KEY_Shift_R));
	}

	SECTION("Combinations") {
		auto optSuperF = ParseHotkeyStringToKeysyms("Super+F");
		REQUIRE(optSuperF.has_value());
		REQUIRE(optSuperF->size() == 2);
		REQUIRE(optSuperF->contains(XKB_KEY_Super_L));
		REQUIRE(optSuperF->contains(XKB_KEY_F));

		auto optSpaces = ParseHotkeyStringToKeysyms("  super  +   f  ");
		REQUIRE(optSpaces.has_value());
		REQUIRE(optSpaces->size() == 2);
		REQUIRE(optSpaces->contains(XKB_KEY_Super_L));
		REQUIRE(optSpaces->contains(XKB_KEY_F));

		auto optTriple = ParseHotkeyStringToKeysyms("Ctrl+Alt+Delete");
		REQUIRE(optTriple.has_value());
		REQUIRE(optTriple->size() == 3);
		REQUIRE(optTriple->contains(XKB_KEY_Control_L));
		REQUIRE(optTriple->contains(XKB_KEY_Alt_L));
		REQUIRE(optTriple->contains(XKB_KEY_Delete));
	}

	SECTION("Invalid combinations") {
		auto optEmpty = ParseHotkeyStringToKeysyms("");
		REQUIRE(!optEmpty.has_value());

		auto optInvalid = ParseHotkeyStringToKeysyms("Super+TotallyInvalidKeyName123");
		REQUIRE(!optInvalid.has_value());
	}
}

TEST_CASE("CNativeActionBinding Execution", "[action_binding]") {
	SECTION("Command execution") {
		static bool s_bExecuted = false;
		static ConCommand cc_test("test_hotkey_action_cmd", "test command", []( std::span<std::string_view> ) {
			s_bExecuted = true;
		});

		s_bExecuted = false;
		CNativeActionBinding binding("Test Command", "test_hotkey_action_cmd");

		// When not armed, Execute should fail
		REQUIRE(binding.IsArmed() == false);
		REQUIRE(binding.Execute() == false);
		REQUIRE(s_bExecuted == false);

		// Arm and execute
		binding.Arm(0);
		REQUIRE(binding.IsArmed() == true);
		REQUIRE(binding.Execute() == true);
		REQUIRE(s_bExecuted == true);
	}

	SECTION("C++ Callback execution") {
		bool bCalled = false;
		CNativeActionBinding binding("Test Callback", [&]() {
			bCalled = true;
		});

		binding.Arm(0);
		REQUIRE(binding.Execute() == true);
		REQUIRE(bCalled == true);
	}

	SECTION("OneShot arming flag") {
		int nCallCount = 0;
		CNativeActionBinding binding("Test OneShot", [&]() {
			nCallCount++;
		});

		binding.Arm(ActionBindingArmFlag_OneShot);
		REQUIRE(binding.IsArmed() == true);

		REQUIRE(binding.Execute() == true);
		REQUIRE(nCallCount == 1);
		// After one shot, it should be disarmed
		REQUIRE(binding.IsArmed() == false);
		REQUIRE(binding.Execute() == false);
		REQUIRE(nCallCount == 1);
	}
}

TEST_CASE("RegisterHotkeysFromScript", "[action_binding]") {
	static bool s_bLuaCallbackRan = false;
	{
		CScriptScopedLock script;

		auto configTable = script.Manager().Gamescope().Config.Base;

		sol::object inputObj = configTable["input"];
		sol::table inputTable;
		if (!inputObj.is<sol::table>()) {
			inputTable = script->create_table();
			configTable["input"] = inputTable;
		} else {
			inputTable = inputObj.as<sol::table>();
		}

		sol::table hotkeysTable = script->create_table();
		inputTable["hotkeys"] = hotkeysTable;

		hotkeysTable["Super+F"] = "test_fs_action";

		hotkeysTable["Ctrl+1"] = [&]() {
			s_bLuaCallbackRan = true;
		};

		// Table syntax with array of keys
		sol::table comboTable = script->create_table();
		sol::table keysArray = script->create_table();
		keysArray[1] = "Super";
		keysArray[2] = "G";
		comboTable["keys"] = keysArray;
		comboTable["action"] = "test_fs_action";
		comboTable["description"] = "Custom Grab";
		hotkeysTable["CustomGrab"] = comboTable;
	}

	static bool s_bLuaFsRan = false;
	static ConCommand cc_test_fs("test_fs_action", "test fs", []( std::span<std::string_view> ) {
		s_bLuaFsRan = true;
	});

	RegisterHotkeysFromScript();

	auto bindings = CServerActionBinding::GetBindings();
	bool bFoundSuperF = false;
	bool bFoundCtrl1 = false;
	bool bFoundCustomGrab = false;

	for (auto *pBinding : bindings) {
		if (pBinding->GetDescription() == "Super+F") {
			bFoundSuperF = true;
			auto triggers = pBinding->GetKeyboardTriggers();
			REQUIRE(triggers.size() == 1);
			REQUIRE(triggers[0].setKeySyms.contains(XKB_KEY_Super_L));
			REQUIRE(triggers[0].setKeySyms.contains(XKB_KEY_F));

			s_bLuaFsRan = false;
			REQUIRE(pBinding->Execute() == true);
			REQUIRE(s_bLuaFsRan == true);
		} else if (pBinding->GetDescription() == "Ctrl+1") {
			bFoundCtrl1 = true;
			auto triggers = pBinding->GetKeyboardTriggers();
			REQUIRE(triggers.size() == 1);
			REQUIRE(triggers[0].setKeySyms.contains(XKB_KEY_Control_L));
			REQUIRE(triggers[0].setKeySyms.contains(XKB_KEY_1));

			s_bLuaCallbackRan = false;
			REQUIRE(pBinding->Execute() == true);
			REQUIRE(s_bLuaCallbackRan == true);
		} else if (pBinding->GetDescription() == "Custom Grab") {
			bFoundCustomGrab = true;
			auto triggers = pBinding->GetKeyboardTriggers();
			REQUIRE(triggers.size() == 1);
			REQUIRE(triggers[0].setKeySyms.contains(XKB_KEY_Super_L));
			REQUIRE(triggers[0].setKeySyms.contains(XKB_KEY_G));

			s_bLuaFsRan = false;
			REQUIRE(pBinding->Execute() == true);
			REQUIRE(s_bLuaFsRan == true);
		}
	}
	REQUIRE(bFoundSuperF == true);
	REQUIRE(bFoundCtrl1 == true);
	REQUIRE(bFoundCustomGrab == true);

	// Test unbinding: setting to nil removes the binding on next reload
	{
		CScriptScopedLock script;
		sol::table hotkeysTable = script.Manager().Gamescope().Config.Base["input"]["hotkeys"];
		hotkeysTable["Super+F"] = sol::lua_nil;
	}

	RegisterHotkeysFromScript();

	bindings = CServerActionBinding::GetBindings();
	bFoundSuperF = false;
	for (auto *pBinding : bindings) {
		if (pBinding->GetDescription() == "Super+F") {
			bFoundSuperF = true;
		}
	}
	REQUIRE(bFoundSuperF == false);
}
