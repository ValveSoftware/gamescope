#include "descriptor_set_constants.h"

layout(constant_id = 0) const int  c_layerCount   = 1;
layout(constant_id = 1) const uint c_ycbcrMask    = 0;
layout(constant_id = 2) const uint c_compositing_debug = 0;
layout(constant_id = 3) const int  c_blur_layer_count = 0;

layout(constant_id = 4) const uint c_colorspaceMask = 0;
layout(constant_id = 5) const bool c_st2084Output = false;
layout(constant_id = 6) const bool c_forceWideGammut = false;
layout(constant_id = 7) const bool c_itmEnable = false;

const int colorspace_linear = 0;
const int colorspace_sRGB = 1;
const int colorspace_scRGB = 2;
const int colorspace_pq = 3;
const int colorspace_reserved = 3;
const int colorspace_max_bits = 2;

const uint compositedebug_Markers = 1u << 0;
const uint compositedebug_PlaneBorders = 1u << 1;
const uint compositedebug_Heatmap = 1u << 2;
const uint compositedebug_Heatmap_MSWCG = 1u << 3; // If compositedebug_Heatmap is set, use the MS WCG heatmap instead of Lilium
const uint compositedebug_Tonemap_Reinhard = 1u << 4; // Use Reinhard tonemapping instead of Uncharted.

bool checkDebugFlag(uint flag) {
    return (c_compositing_debug & flag) != 0;
}

uint get_layer_colorspace(uint layerIdx) {
    return bitfieldExtract(c_colorspaceMask, int(layerIdx) * colorspace_max_bits, colorspace_max_bits);
}

layout(binding = 0, rgba8) writeonly uniform image2D dst;
// alias
layout(binding = 0, rgba8) writeonly uniform image2D dst_luma;
layout(binding = 1, rgba8) writeonly uniform image2D dst_chroma;

layout(binding = 2) uniform sampler2D s_samplers[VKR_SAMPLER_SLOTS];
layout(binding = 3) uniform sampler2D s_ycbcr_samplers[VKR_SAMPLER_SLOTS];
