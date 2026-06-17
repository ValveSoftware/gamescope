#include <catch2/catch_test_macros.hpp>

#include "display_selection.h"

using namespace gamescope;

TEST_CASE("BuildDisplayIdentifier", "[display_selection]") {
	REQUIRE( BuildDisplayIdentifier(true, "Some", "Panel", "eDP-1", 42, "x") == "Internal screen" );

	REQUIRE( BuildDisplayIdentifier(false, "Dell Inc.", "Dell AW3423DW", "DP-3", 810309971, "ignored") == "Dell Inc. Dell AW3423DW 810309971" );
	REQUIRE( BuildDisplayIdentifier(false, "LG Electronics", "LG ULTRAGEAR+", "DP-1", 0, "ABC123") == "LG Electronics LG ULTRAGEAR+ ABC123" );
	REQUIRE( BuildDisplayIdentifier(false, "Dell Inc.", "Dell AW3423DW", "DP-3", 0, "") == "Dell Inc. Dell AW3423DW" );

	REQUIRE( BuildDisplayIdentifier(false, "", "MysteryModel", "DP-2", 0, "") == "MysteryModel" );
	REQUIRE( BuildDisplayIdentifier(false, "", "", "DP-2", 0, "") == "[DP-2]" );
	REQUIRE( BuildDisplayIdentifier(false, "Dell Inc.", "", "DP-2", 0, "") == "[DP-2]" );
}

TEST_CASE("BuildDisplayIdentifier round-trips through SplitDisplaySelectionKey", "[display_selection]") {
	std::string id = BuildDisplayIdentifier(false, "", "", "DP-2", 0, "");
	std::string base, hint;
	SplitDisplaySelectionKey(id, base, hint);
	REQUIRE( base == "[DP-2]" );
	REQUIRE( hint.empty() );
}

TEST_CASE("BuildDisplaySelectionKey", "[display_selection]") {
	REQUIRE( BuildDisplaySelectionKey("Dell AW3423DW", "DP-1", false) == "Dell AW3423DW" );
	REQUIRE( BuildDisplaySelectionKey("Dell AW3423DW", "DP-1", true) == "Dell AW3423DW [DP-1]" );
}

TEST_CASE("SplitDisplaySelectionKey", "[display_selection]") {
	std::string base, hint;

	SplitDisplaySelectionKey("Dell AW3423DW", base, hint);
	REQUIRE( base == "Dell AW3423DW" );
	REQUIRE( hint.empty() );

	SplitDisplaySelectionKey("Dell AW3423DW [DP-1]", base, hint);
	REQUIRE( base == "Dell AW3423DW" );
	REQUIRE( hint == "DP-1" );

	SplitDisplaySelectionKey("[DP-2]", base, hint);
	REQUIRE( base == "[DP-2]" );
	REQUIRE( hint.empty() );
}

TEST_CASE("ResolveDisplaySelection picks the named connector for identical twins", "[display_selection]") {
	std::vector<DisplaySelectionCandidate> twins = {
		{ "DP-1", "Dell AW3423DW" },
		{ "DP-2", "Dell AW3423DW" },
	};

	REQUIRE( ResolveDisplaySelection("Dell AW3423DW [DP-1]", twins) == 0u );
	REQUIRE( ResolveDisplaySelection("Dell AW3423DW [DP-2]", twins) == 1u );

	REQUIRE( ResolveDisplaySelection("Dell AW3423DW", twins) == 0u );

	REQUIRE( ResolveDisplaySelection("Dell AW3423DW [DP-9]", twins) == 0u );

	std::vector<DisplaySelectionCandidate> triplets = {
		{ "DP-1", "Dell AW3423DW" },
		{ "DP-2", "Dell AW3423DW" },
		{ "DP-3", "Dell AW3423DW" },
	};
	REQUIRE( ResolveDisplaySelection("Dell AW3423DW [DP-3]", triplets) == 2u );
}

TEST_CASE("ResolveDisplaySelection keeps identifier match primary over the hint", "[display_selection]") {
	std::vector<DisplaySelectionCandidate> mixed = {
		{ "DP-1", "Dell AW3423DW" },
		{ "DP-2", "LG ULTRAGEAR+ 175706" },
	};
	REQUIRE( ResolveDisplaySelection("Dell AW3423DW [DP-2]", mixed) == 0u );
}

TEST_CASE("ResolveDisplaySelection matches a unique monitor by identifier", "[display_selection]") {
	std::vector<DisplaySelectionCandidate> mixed = {
		{ "DP-1", "Dell AW3423DW" },
		{ "DP-2", "LG ULTRAGEAR+ 175706" },
	};

	REQUIRE( ResolveDisplaySelection("LG ULTRAGEAR+ 175706", mixed) == 1u );

	REQUIRE( ResolveDisplaySelection("Sony Bravia", mixed) == std::nullopt );

	REQUIRE( ResolveDisplaySelection("", mixed) == std::nullopt );
}

TEST_CASE("ResolveDisplaySelection handles a model that itself ends in brackets", "[display_selection]") {
	REQUIRE( BuildDisplayIdentifier(false, "Aperture Science", "Monitor [Pro]", "DP-1", 0, "") == "Aperture Science Monitor [Pro]" );

	std::vector<DisplaySelectionCandidate> bracketed = {
		{ "DP-1", "Aperture Science Monitor [Pro]" },
		{ "DP-2", "Aperture Science Monitor [Pro]" },
	};

	REQUIRE( ResolveDisplaySelection("Aperture Science Monitor [Pro]", bracketed) == 0u );
	REQUIRE( ResolveDisplaySelection("Aperture Science Monitor [Pro] [DP-2]", bracketed) == 1u );
}

TEST_CASE("ResolveDisplaySelection follows the MST path across a dock replug-renumber", "[display_selection]") {
	std::string key = BuildDisplaySelectionKey("Sceptre Z27", "mst:1-5", /*bShared=*/true);
	REQUIRE( key == "Sceptre Z27 [mst:1-5]" );

	std::vector<DisplaySelectionCandidate> before = {
		{ "DP-9", "Sceptre Z27", "mst:1-5" },
		{ "DP-10", "Sceptre Z27", "mst:1-6" },
	};
	REQUIRE( ResolveDisplaySelection(key, before) == 0u );

	std::vector<DisplaySelectionCandidate> after = {
		{ "DP-7", "Sceptre Z27", "mst:1-5" },
		{ "DP-12", "Sceptre Z27", "mst:1-6" },
	};
	REQUIRE( ResolveDisplaySelection(key, after) == 0u );

	REQUIRE( ResolveDisplaySelection("Sceptre Z27 [DP-10]", before) == 1u );

	std::vector<DisplaySelectionCandidate> moved = {
		{ "DP-7", "Sceptre Z27", "mst:1-9" },
		{ "DP-12", "Sceptre Z27", "mst:1-6" },
	};
	REQUIRE( ResolveDisplaySelection(key, moved) == 0u );
}
