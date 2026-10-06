#pragma once

#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>

#include <sys/types.h>

namespace gamescope
{
    // A /proc/<pid>/maps line names vrclient if the mapped file's basename
    // contains "vrclient" (vrclient.so, vrclient_x64.so, Proton builds).
    inline bool MapsLineNamesVRClient( std::string_view line )
    {
        size_t slash = line.rfind( '/' );
        if ( slash == std::string_view::npos )
            return false;

        return line.substr( slash + 1 ).find( "vrclient" ) != std::string_view::npos;
    }

    // procfs files cannot be sized up front, stream line by line.
    inline bool ProcMapsHasVRClient( const char *path )
    {
        std::ifstream maps( path );
        std::string line;
        while ( std::getline( maps, line ) )
        {
            if ( MapsLineNamesVRClient( line ) )
                return true;
        }
        return false;
    }

    inline bool ProcessHasVRClientMapped( pid_t pid )
    {
        char path[64];
        snprintf( path, sizeof( path ), "/proc/%d/maps", (int)pid );
        return ProcMapsHasVRClient( path );
    }
}
