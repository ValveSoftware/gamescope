#pragma once

#include <cstdint>
#include <string_view>

namespace GamescopeLayerClient
{
    // GAMESCOPE_LAYER_CLIENT_FLAGS
    namespace Flag {
        static constexpr uint32_t DisableHDR = 1u << 0;
        static constexpr uint32_t ForceBypass = 1u << 1;
        static constexpr uint32_t FrameLimiterAware = 1u << 2;
    }
    using Flags = uint32_t;
}