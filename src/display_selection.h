#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstddef>
#include <cstdint>

namespace gamescope
{
    // Base EDID key: "{make} {model} {serial}", else model-only, else "[{connector}]". Numeric serial wins over the descriptor string.
    inline std::string BuildDisplayIdentifier( bool bInternal, const std::string &szMake, const std::string &szModel, const std::string &szConnectorName, uint32_t uEDIDSerial, const std::string &szEDIDSerial )
    {
        if ( bInternal )
            return "Internal screen";

        std::string szBase;
        if ( !szMake.empty() && !szModel.empty() )
            szBase = szMake + " " + szModel;
        else if ( !szModel.empty() )
            szBase = szModel;
        else
            return "[" + szConnectorName + "]";

        if ( uEDIDSerial != 0 )
            szBase += " " + std::to_string( uEDIDSerial );
        else if ( !szEDIDSerial.empty() )
            szBase += " " + szEDIDSerial;
        return szBase;
    }

    struct DisplaySelectionCandidate
    {
        std::string szConnectorName;
        std::string szIdentifier;
        std::string szMstPath;
    };

    // Appends a tiebreak only when the identifier collides, to disambiguate identical monitors.
    inline std::string BuildDisplaySelectionKey( const std::string &szIdentifier, const std::string &szConnectorName, bool bSharedWithOtherOutput )
    {
        if ( bSharedWithOtherOutput )
            return szIdentifier + " [" + szConnectorName + "]";
        return szIdentifier;
    }

    inline void SplitDisplaySelectionKey( const std::string &szKey, std::string &szBaseOut, std::string &szConnectorHintOut )
    {
        szBaseOut = szKey;
        szConnectorHintOut.clear();
        if ( !szKey.empty() && szKey.back() == ']' )
        {
            size_t uOpen = szKey.rfind( " [" );
            if ( uOpen != std::string::npos )
            {
                szBaseOut = szKey.substr( 0, uOpen );
                szConnectorHintOut = szKey.substr( uOpen + 2, szKey.size() - uOpen - 3 );
            }
        }
    }

    // Identifier match is primary, the connector hint only breaks ties. nullopt = no match (fall back to --prefer-output).
    inline std::optional<size_t> ResolveDisplaySelection( const std::string &szKey, const std::vector<DisplaySelectionCandidate> &candidates )
    {
        if ( szKey.empty() )
            return std::nullopt;

        // Exact identifier match wins first, so a model ending in "[...]" isn't split.
        for ( size_t i = 0; i < candidates.size(); i++ )
            if ( candidates[i].szIdentifier == szKey )
                return i;

        std::string szBase, szConnectorHint;
        SplitDisplaySelectionKey( szKey, szBase, szConnectorHint );

        std::optional<size_t> oBaseMatch;
        for ( size_t i = 0; i < candidates.size(); i++ )
        {
            if ( candidates[i].szIdentifier != szBase )
                continue;
            if ( !szConnectorHint.empty() && ( candidates[i].szConnectorName == szConnectorHint || candidates[i].szMstPath == szConnectorHint ) )
                return i;
            if ( !oBaseMatch )
                oBaseMatch = i;
        }
        return oBaseMatch;
    }
}
