#pragma once

#include <cstdint>

namespace gamescope {

struct WaylandOutputInfo {
	int32_t nRefresh = 60;
	int32_t nScale = 1;
};

} // namespace gamescope
