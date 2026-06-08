#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace gamescope
{
    template <typename T>
    inline std::optional<T> Parse( std::string_view chars )
    {
        T obj;
        auto result = std::from_chars( chars.begin(), chars.end(), obj );
        if ( result.ec == std::errc{} )
            return obj;
        else
            return std::nullopt;
    }

    template <>
    inline std::optional<bool> Parse( std::string_view chars )
    {
        std::optional<uint32_t> oNumber = Parse<uint32_t>( chars );
        if ( oNumber )
            return !!*oNumber;

        if ( chars == "true" )
            return true;
        else
            return false;
    }
}
