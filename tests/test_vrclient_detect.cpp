#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <cstring>
#include <string>

#include <unistd.h>

#include "vrclient_detect.h"

TEST_CASE("maps lines naming vrclient are detected", "[vrclient_detect]") {
	// Native SteamVR, 64-bit and arm64, the latter captured verbatim from a Steam Frame.
	REQUIRE(gamescope::MapsLineNamesVRClient(
		"7f0e8c000000-7f0e8c021000 r-xp 00000000 103:02 1234 /home/user/.steam/steam/steamapps/common/SteamVR/bin/linux64/vrclient.so" ));
	REQUIRE(gamescope::MapsLineNamesVRClient(
		"ffff64200000-ffff647f8000 r-xp 00000000 00:1c 2751                       /opt/steamvr/bin/linuxarm64/vrclient.so" ));
	// Proton's unixlib builds.
	REQUIRE(gamescope::MapsLineNamesVRClient(
		"7f0e8c000000-7f0e8c021000 r-xp 00000000 103:02 1234 /home/user/.steam/steam/steamapps/common/Proton 11.0/files/lib/wine/x86_64-unix/vrclient_x64.so" ));
	// Proton's PE builds, Wine maps the .dll itself. Paths carry spaces and parens.
	REQUIRE(gamescope::MapsLineNamesVRClient(
		"7f0e8c000000-7f0e8c021000 r-xp 00000000 103:02 1234 /home/user/.steam/steam/steamapps/common/Proton - Experimental/files/lib/wine/x86_64-windows/vrclient_x64.dll" ));
	REQUIRE(gamescope::MapsLineNamesVRClient(
		"7f0e8c000000-7f0e8c021000 r-xp 00000000 103:02 1234 /home/user/.steam/steam/steamapps/common/Proton 9.0 (Beta)/files/lib/wine/i386-windows/vrclient.dll" ));
	// Unlinked while mapped.
	REQUIRE(gamescope::MapsLineNamesVRClient(
		"7f0e8c000000-7f0e8c021000 r-xp 00000000 103:02 1234 /path/vrclient.so (deleted)" ));
}

TEST_CASE("non-vrclient maps lines are not detected", "[vrclient_detect]") {
	REQUIRE(!gamescope::MapsLineNamesVRClient(
		"7f0e8c000000-7f0e8c021000 r-xp 00000000 103:02 1234 /usr/lib/libvulkan.so.1" ));
	// vrclient only in a directory component, not the mapped file.
	REQUIRE(!gamescope::MapsLineNamesVRClient(
		"7f0e8c000000-7f0e8c021000 r-xp 00000000 103:02 1234 /home/user/vrclient/game.so" ));
	// Anonymous and pseudo mappings.
	REQUIRE(!gamescope::MapsLineNamesVRClient(
		"7f0e8c000000-7f0e8c021000 rw-p 00000000 00:00 0" ));
	REQUIRE(!gamescope::MapsLineNamesVRClient( "[heap]" ));
	REQUIRE(!gamescope::MapsLineNamesVRClient( "" ));
}

static std::string write_fixture( const char *contents ) {
	char path[] = "/tmp/vrclient_detect_test_XXXXXX";
	int fd = mkstemp( path );
	REQUIRE(fd >= 0);
	REQUIRE(write( fd, contents, strlen( contents ) ) == (ssize_t)strlen( contents ));
	close( fd );
	return path;
}

TEST_CASE("maps files scan positive and negative", "[vrclient_detect]") {
	std::string positive = write_fixture(
		"7f0e8b000000-7f0e8b021000 r-xp 00000000 103:02 1233 /usr/lib/libc.so.6\n"
		"7f0e8c000000-7f0e8c021000 r-xp 00000000 103:02 1234 /path/bin/linux64/vrclient_x64.so\n"
		"7f0e8d000000-7f0e8d021000 rw-p 00000000 00:00 0\n" );
	REQUIRE(gamescope::ProcMapsHasVRClient( positive.c_str() ));
	unlink( positive.c_str() );

	// Final line without a trailing newline still scans.
	std::string positiveNoNewline = write_fixture(
		"7f0e8b000000-7f0e8b021000 r-xp 00000000 103:02 1233 /usr/lib/libc.so.6\n"
		"7f0e8c000000-7f0e8c021000 r-xp 00000000 103:02 1234 /path/bin/linux64/vrclient.so" );
	REQUIRE(gamescope::ProcMapsHasVRClient( positiveNoNewline.c_str() ));
	unlink( positiveNoNewline.c_str() );

	std::string negative = write_fixture(
		"7f0e8b000000-7f0e8b021000 r-xp 00000000 103:02 1233 /usr/lib/libc.so.6\n"
		"7f0e8d000000-7f0e8d021000 rw-p 00000000 00:00 0\n" );
	REQUIRE(!gamescope::ProcMapsHasVRClient( negative.c_str() ));
	unlink( negative.c_str() );
}

TEST_CASE("processes without vrclient scan as false", "[vrclient_detect]") {
	// The test binary never loads vrclient.
	REQUIRE(!gamescope::ProcessHasVRClientMapped( getpid() ));
	// Nonexistent pid: unreadable maps degrade to false.
	REQUIRE(!gamescope::ProcessHasVRClientMapped( -1 ));
}
