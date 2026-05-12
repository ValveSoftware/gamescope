#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace gamescope
{
    struct StringHash
    {
        using is_transparent = void;
        [[nodiscard]] size_t operator( )( const char *string ) const
        { return std::hash<std::string_view>{}( string ); }
        [[nodiscard]] size_t operator( )( std::string_view string ) const
        { return std::hash<std::string_view>{}( string ); }
        [[nodiscard]] size_t operator( )( const std::string &string ) const
        { return std::hash<std::string>{}( string ); }
    };

    template<typename T>
    using Dict =
        std::unordered_map<std::string, T, StringHash, std::equal_to<>>;

    template<typename T>
    using MultiDict =
        std::unordered_multimap<std::string, T, StringHash, std::equal_to<>>;
} // namespace gamescope
