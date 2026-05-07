#include <catch2/catch_test_macros.hpp>

#include "convar.h"
#include "Script/Script.h"

using namespace gamescope;

TEST_CASE("ConVar", "[convar]") {
	ConVar<bool> cv_bool_var {"foobar", true, "switch on and off"};
	REQUIRE(cv_bool_var.GetName() == "foobar");
	REQUIRE(cv_bool_var.GetDescription() == "switch on and off");
	REQUIRE(cv_bool_var.Get() == true);
	cv_bool_var.SetValue(false);
	REQUIRE(cv_bool_var.Get() == false);

	{
		auto proxy = CScriptScopedLock().Manager().Gamescope().Convars.Base["foobar"];
		REQUIRE(proxy.valid() == true);
		ConVar<bool>* cv = proxy;
		REQUIRE(cv->GetName() == "foobar");
		REQUIRE(cv->GetDescription() == "switch on and off");
		REQUIRE(cv->Get() == false);
		cv->SetValue(true);
		REQUIRE(cv_bool_var.Get() == true);
	}

	{
		auto proxy = CScriptScopedLock().Manager().Gamescope().Convars.Base["nobar"];
		REQUIRE(proxy.valid() == false);
	}

	{
		auto proxy = CScriptScopedLock()->script("return gamescope.convars.foobar");
		REQUIRE(proxy.valid() == true);
		ConVar<bool>* cv = proxy;
		REQUIRE(cv->GetName() == "foobar");
		REQUIRE(cv->GetDescription() == "switch on and off");
		REQUIRE(cv->Get() == true);
		cv->SetValue(false);
		REQUIRE(cv_bool_var.Get() == false);
	}

	{
		auto proxy = CScriptScopedLock()->script("return gamescope.convars.nobar");
		REQUIRE(proxy.valid() == true);
		ConVar<bool>* cv = proxy;
		REQUIRE(cv == nullptr);
	}
}
