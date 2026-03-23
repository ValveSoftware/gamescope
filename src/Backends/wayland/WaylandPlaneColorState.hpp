#pragma once

#include "gamescope_shared.h"

#include <memory>

namespace gamescope {

struct WaylandPlaneColorState {
	GamescopeAppTextureColorspace eColorspace;
	std::shared_ptr<gamescope::BackendBlob> pHDRMetadata;

	bool operator==(const WaylandPlaneColorState& other) const = default;
	bool operator!=(const WaylandPlaneColorState& other) const = default;
};

} // namespace gamescope
