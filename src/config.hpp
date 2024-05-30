#pragma once

#include <span>
#include <stdint.h>

namespace gamescope
{
    // Read all configurations files
    int config_read();

    // Get the list of valid refresh rates for the specified display,
    // or an empty std::span if they are not found.
    std::span<const uint32_t> config_get_refresh_rates(const char *vendor_id, uint16_t product_id);
}
