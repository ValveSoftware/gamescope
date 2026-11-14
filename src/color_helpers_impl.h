#pragma once

#include "color_helpers.h"

namespace rendervulkan {
  static constexpr uint32_t s_nLutEdgeSize3d = 17;
  static constexpr uint32_t s_nLutSize1d = 4096;
}

namespace color_bench {
  static constexpr uint32_t nLutEdgeSize3d = 17;
  static constexpr uint32_t nLutSize1d = 4096;
}

namespace ns_color_tests {
	[[maybe_unused]] static constexpr uint32_t nLutEdgeSize3d = 17;
}

#ifdef COLOR_HELPERS_CPP
REGISTER_LUT_EDGE_SIZE(rendervulkan::s_nLutEdgeSize3d);
#endif