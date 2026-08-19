#pragma once

#include <string_view>
#include <vector>

namespace gamescope
{
	inline void Split( std::vector<std::string_view> &tokens, std::string_view string, std::string_view delims = " " )
    {
        size_t end = 0;
        for ( size_t start = 0; start < string.size() && end != std::string_view::npos; start = end + 1 )
        {
            end = string.find_first_of( delims, start );

            if ( start != end )
                tokens.emplace_back( string.substr( start, end-start ) );
        }
    }

    inline std::vector<std::string_view> Split( std::string_view string, std::string_view delims = " " )
    {
        std::vector<std::string_view> tokens;
        Split( tokens, string, delims );
        return tokens;
    }
}
