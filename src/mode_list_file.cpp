#include "mode_list_file.h"
#include "edid.h"
#include "log.hpp"

#include <cstdio>

namespace gamescope
{
    static LogScope modelist_log("modelist");

    static std::string GetModeListPath()
    {
        const char *pszPatchedEdidPath = GetPatchedEdidPath();
        if ( !pszPatchedEdidPath )
            return "";

        return std::string{ pszPatchedEdidPath } + ".modes";
    }

    void WriteModeListFile( std::span<const BackendMode> modes )
    {
        std::string sPath = GetModeListPath();
        if ( sPath.empty() )
            return;

        std::string sTmpPath = sPath + ".tmp";

        FILE *pFile = fopen( sTmpPath.c_str(), "wb" );
        if ( !pFile )
        {
            modelist_log.errorf( "Couldn't open file: %s", sTmpPath.c_str() );
            return;
        }

        std::string sText = EncodeModeList( modes );
        fwrite( sText.data(), 1, sText.size(), pFile );
        fflush( pFile );
        fclose( pFile );

        // Flip it over, same as WritePatchedEdid.
        rename( sTmpPath.c_str(), sPath.c_str() );
        modelist_log.infof( "Wrote %zu modes to: %s", modes.size(), sPath.c_str() );
    }

    std::vector<BackendMode> LoadModeListFile()
    {
        std::string sPath = GetModeListPath();
        if ( sPath.empty() )
            return {};

        FILE *pFile = fopen( sPath.c_str(), "rb" );
        if ( !pFile )
            return {};

        char szText[16384];
        size_t ulSize = fread( szText, 1, sizeof( szText ), pFile );
        fclose( pFile );

        // A read that fills the buffer may cut the last line, trim it so it can't parse as a bogus mode.
        if ( ulSize == sizeof( szText ) )
        {
            while ( ulSize > 0 && szText[ ulSize - 1 ] != '\n' )
                ulSize--;
        }

        return ParseModeList( std::string_view{ szText, ulSize } );
    }
}
