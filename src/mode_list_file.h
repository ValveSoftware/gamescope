#pragma once

#include "gamescope_shared.h"

#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gamescope
{
    // Sidecar text format: one "WxH@R" per line, connector preference order.
    inline std::string EncodeModeList( std::span<const BackendMode> modes )
    {
        std::string sText;
        for ( const BackendMode &mode : modes )
        {
            char szLine[64];
            snprintf( szLine, sizeof( szLine ), "%ux%u@%u\n", mode.uWidth, mode.uHeight, mode.uRefresh );
            sText += szLine;
        }
        return sText;
    }

    inline std::vector<BackendMode> ParseModeList( std::string_view text )
    {
        std::vector<BackendMode> modes;
        size_t ulPos = 0;
        while ( ulPos < text.size() )
        {
            size_t ulEnd = text.find( '\n', ulPos );
            if ( ulEnd == std::string_view::npos )
                ulEnd = text.size();

            std::string line{ text.substr( ulPos, ulEnd - ulPos ) };
            ulPos = ulEnd + 1;

            BackendMode mode{};
            if ( sscanf( line.c_str(), "%ux%u@%u", &mode.uWidth, &mode.uHeight, &mode.uRefresh ) != 3 )
                continue;
            if ( !mode.uWidth || !mode.uHeight || !mode.uRefresh )
                continue;
            // Catches "%u" swallowing negatives as huge values, and other garbage.
            if ( mode.uWidth > 16384 || mode.uHeight > 16384 || mode.uRefresh > 1000 )
                continue;

            modes.push_back( mode );
        }
        return modes;
    }

    void WriteModeListFile( std::span<const BackendMode> modes );
    std::vector<BackendMode> LoadModeListFile();
}
