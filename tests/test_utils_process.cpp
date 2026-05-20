#include <catch2/catch_test_macros.hpp>

#include <sstream>

#include "Utils/Process.h"

using namespace gamescope;

TEST_CASE("GetAppIdFromCgroup", "[appid]") {
    SECTION("steam app scope") {
        std::istringstream stream("0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-steam-app12345-9876.scope\n");
        REQUIRE(Process::GetAppIdFromCgroup(stream) == 12345u);
    }

    SECTION("no matching scope") {
        std::istringstream stream("0::/user.slice/user-1000.slice/session.scope\n");
        REQUIRE(Process::GetAppIdFromCgroup(stream) == 0u);
    }

    SECTION("multiple lines, one matching") {
        std::istringstream stream(
            "12:devices:/user.slice\n"
            "11:memory:/user.slice/user-1000.slice/session-1.scope\n"
            "0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-steam-app567-42.scope\n"
        );
        REQUIRE(Process::GetAppIdFromCgroup(stream) == 567u);
    }

    SECTION("empty stream") {
        std::istringstream stream("");
        REQUIRE(Process::GetAppIdFromCgroup(stream) == 0u);
    }

    SECTION("malformed lines") {
        std::istringstream stream("not-a-valid-cgroup-line\n");
        REQUIRE(Process::GetAppIdFromCgroup(stream) == 0u);

        stream = std::istringstream("not-a-valid:cgroup-line\n");
        REQUIRE(Process::GetAppIdFromCgroup(stream) == 0u);

        stream = std::istringstream("not-a-valid:cgroup-line:\n");
        REQUIRE(Process::GetAppIdFromCgroup(stream) == 0u);
    }
}
