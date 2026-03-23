#pragma once

#include <rendervulkan.hpp>

#define WL_FRACTIONAL_SCALE_DENOMINATOR 120

static inline uint32_t WaylandScaleToPhysical(uint32_t pValue, uint32_t pFactor) {
	return pValue * pFactor / WL_FRACTIONAL_SCALE_DENOMINATOR;
}

static inline uint32_t WaylandScaleToLogical(uint32_t pValue, uint32_t pFactor) {
	return div_roundup(pValue * WL_FRACTIONAL_SCALE_DENOMINATOR, pFactor);
}
