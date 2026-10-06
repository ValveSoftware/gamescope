#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <vector>

#include "Utils/String.h"

using namespace gamescope;
using namespace std::string_view_literals;

TEST_CASE("Utils/String", "[string]") {
    SECTION("Split") {
        REQUIRE(Split("foo bar baz") == std::vector<std::string_view>{ "foo", "bar", "baz" });
        REQUIRE(Split("foo,bar;baz", ";,") == std::vector<std::string_view>{ "foo", "bar", "baz" });
        REQUIRE(Split("foo   bar") == std::vector<std::string_view>{ "foo", "bar" });
        REQUIRE(Split("  foo bar") == std::vector<std::string_view>{ "foo", "bar" });
        REQUIRE(Split("foo bar  ") == std::vector<std::string_view>{ "foo", "bar" });
        REQUIRE(Split("") == std::vector<std::string_view>{});
        REQUIRE(Split("   ") == std::vector<std::string_view>{});
        REQUIRE(Split("foobar") == std::vector<std::string_view>{ "foobar" });
        REQUIRE(Split("foo,;bar", ",;") == std::vector<std::string_view>{ "foo", "bar" });

        std::vector<std::string_view> tokens{ "baz" };
        Split(tokens, "foo bar"sv);
        REQUIRE(tokens == std::vector<std::string_view>{ "baz", "foo", "bar" });
    }
}
