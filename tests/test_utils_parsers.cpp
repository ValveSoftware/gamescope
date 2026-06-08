#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <type_traits>

#include "Utils/Parsers.h"

using namespace gamescope;

TEST_CASE("Utils/Parsers") {
    SECTION("int") {
        REQUIRE(Parse<int>("42") == 42);
        REQUIRE(Parse<int>("42,foo") == 42);
        REQUIRE(Parse<int>("42 foo") == 42);
        REQUIRE(Parse<int>("foo,42") == std::nullopt);
        REQUIRE(Parse<int>("foo 42") == std::nullopt);
        REQUIRE(Parse<int>("42,35") == 42);
        REQUIRE(Parse<int>("-35") == -35);
        REQUIRE(Parse<int>("000") == 0);
        REQUIRE(Parse<int>("-00") == 0);
        REQUIRE(Parse<int>("") == std::nullopt);
    }

    SECTION("uint") {
        REQUIRE(Parse<uint>("42") == 42);
        REQUIRE(Parse<uint>("42,foo") == 42);
        REQUIRE(Parse<uint>("42 foo") == 42);
        REQUIRE(Parse<uint>("foo,42") == std::nullopt);
        REQUIRE(Parse<uint>("foo 42") == std::nullopt);
        REQUIRE(Parse<uint>("42,35") == 42);
        REQUIRE(Parse<uint>("-35") == std::nullopt);
        REQUIRE(Parse<uint>("000") == 0);
        REQUIRE(Parse<uint>("-00") == std::nullopt);
        REQUIRE(Parse<uint>("") == std::nullopt);
    }

    SECTION("float") {
        double eps = 0.0000001;
        REQUIRE_THAT(*Parse<float>("3.14159"), Catch::Matchers::WithinRel(3.14159, eps));
        REQUIRE_THAT(*Parse<float>("-42.3"), Catch::Matchers::WithinRel(-42.3, eps));
        REQUIRE_THAT(*Parse<float>("42,3"), Catch::Matchers::WithinRel(42.0, eps));
        REQUIRE_THAT(*Parse<float>("42,foo"), Catch::Matchers::WithinRel(42.0, eps));
        REQUIRE(Parse<float>("foo,42.3") == std::nullopt);
        REQUIRE(Parse<float>("foo 42.3") == std::nullopt);
        REQUIRE(Parse<float>("pi") == std::nullopt);
        REQUIRE(Parse<float>("") == std::nullopt);
    }

    SECTION("pid") {
        REQUIRE(Parse<pid_t>("42") == 42);
        REQUIRE(Parse<pid_t>("42,35,64") == 42);
        REQUIRE(Parse<pid_t>("42 35 64") == 42);
        REQUIRE(Parse<pid_t>("-42,35") == -42);
        REQUIRE(Parse<pid_t>("000") == 0);
        REQUIRE(Parse<pid_t>("-00") == 0);
        REQUIRE(Parse<pid_t>("foo,42") == std::nullopt);
        REQUIRE(Parse<pid_t>("foo 42") == std::nullopt);
        REQUIRE(Parse<pid_t>("") == std::nullopt);
    }

    SECTION("enum") {
        enum Something {
            SOME_FOO = 1,
            SOME_BAR = 2,
            SOME_BAZ = 3,
        };
        REQUIRE(Parse<std::underlying_type<Something>::type>("0") == 0);
        REQUIRE(Parse<std::underlying_type<Something>::type>("1") == SOME_FOO);
        REQUIRE(Parse<std::underlying_type<Something>::type>("2") == SOME_BAR);
        REQUIRE(Parse<std::underlying_type<Something>::type>("3") == SOME_BAZ);
        REQUIRE(Parse<std::underlying_type<Something>::type>("4") == 4);
        REQUIRE(Parse<std::underlying_type<Something>::type>("SOME_FOO") == std::nullopt);
        REQUIRE(Parse<std::underlying_type<Something>::type>("") == std::nullopt);
    }

    SECTION("bool") {
        REQUIRE(Parse<bool>("true") == true);
        //REQUIRE(Parse<bool>("TRUE") == true);
        //REQUIRE(Parse<bool>("True") == true);

        REQUIRE(Parse<bool>("false") == false);
        REQUIRE(Parse<bool>("FALSE") == false);
        REQUIRE(Parse<bool>("False") == false);

        REQUIRE(Parse<bool>("1") == true);
        REQUIRE(Parse<bool>("1.0") == true);
        REQUIRE(Parse<bool>("42") == true);
        REQUIRE(Parse<bool>("-35") == false);
        REQUIRE(Parse<bool>("0") == false);
        REQUIRE(Parse<bool>("0.0") == false);
        REQUIRE(Parse<bool>("0.1") == false);
        REQUIRE(Parse<bool>("foo") == false);
        REQUIRE(Parse<bool>("") == false);
    }
}
