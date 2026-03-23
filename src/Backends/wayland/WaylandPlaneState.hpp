#pragma once

#include "gamescope_shared.h"
#include "main.hpp"

#include <memory>
#include <wayland-client.h>

namespace gamescope {

struct WaylandPlaneState {
	wl_buffer* pBuffer;
	int32_t nDestX;
	int32_t nDestY;
	double flSrcX;
	double flSrcY;
	double flSrcWidth;
	double flSrcHeight;
	int32_t nDstWidth;
	int32_t nDstHeight;
	GamescopeAppTextureColorspace eColorspace;
	std::shared_ptr<gamescope::BackendBlob> pHDRMetadata;
	bool bOpaque;
	uint32_t uFractionalScale;
};

inline WaylandPlaneState ClipPlane(const WaylandPlaneState& state) {
	int32_t nClippedDstWidth =
		std::min<int32_t>(g_nOutputWidth, state.nDstWidth + state.nDestX) - state.nDestX;
	int32_t nClippedDstHeight =
		std::min<int32_t>(g_nOutputHeight, state.nDstHeight + state.nDestY) - state.nDestY;
	double flClippedSrcWidth = state.flSrcWidth * (nClippedDstWidth / double(state.nDstWidth));
	double flClippedSrcHeight = state.flSrcHeight * (nClippedDstHeight / double(state.nDstHeight));

	WaylandPlaneState outState = state;
	outState.nDstWidth = nClippedDstWidth;
	outState.nDstHeight = nClippedDstHeight;
	outState.flSrcWidth = flClippedSrcWidth;
	outState.flSrcHeight = flClippedSrcHeight;
	return outState;
}

} // namespace gamescope
