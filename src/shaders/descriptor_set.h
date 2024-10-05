#include "descriptor_set_constants.h"

layout(constant_id = 0) const int  c_layerCount   = 1;
layout(constant_id = 1) const uint c_ycbcrMask    = 0;
layout(constant_id = 2) const uint c_compositing_debug = 0;
layout(constant_id = 3) const int  c_blur_layer_count = 0;

layout(constant_id = 4) const uint c_colorspaceMask = 0;
layout(constant_id = 5) const uint c_output_eotf = 0;
layout(constant_id = 7) const bool c_itm_enable = false;

const int colorspace_linear = 0;
const int colorspace_sRGB = 1;
const int colorspace_scRGB = 2;
const int colorspace_pq = 3;
const int colorspace_passthru = 4;
const int colorspace_max_bits = 3;

const int filter_linear_emulated = 0;
const int filter_nearest = 1;
const int filter_fsr = 2;
const int filter_nis = 3;
const int filter_pixel = 4;
const int filter_bicubic = 5;
const int filter_from_view = 255;

const int EOTF_Gamma22 = 0;
const int EOTF_PQ = 1;
const int EOTF_Count = 2;

// These can be changed but also modify the ones in renderervulkan.hpp
const uint compositedebug_Markers = 1u << 0;
const uint compositedebug_PlaneBorders = 1u << 1;
const uint compositedebug_Heatmap = 1u << 2;
const uint compositedebug_Heatmap_MSWCG = 1u << 3; // If compositedebug_Heatmap is set, use the MS WCG heatmap instead of Lilium
const uint compositedebug_Heatmap_Hard = 1u << 4; // If compositedebug_Heatmap is set, use a heatmap with specialized hard flagging
const uint compositedebug_Markers_Partial = 1u << 5; // If compositedebug_Heatmap is set, use a heatmap with specialized hard flagging
//const uint compositedebug_Tonemap_Reinhard = 1u << 7; // Use Reinhard tonemapping instead of Uncharted.

bool checkDebugFlag(uint flag) {
    return (c_compositing_debug & flag) != 0;
}

uint get_layer_colorspace(uint layerIdx) {
    return bitfieldExtract(c_colorspaceMask, int(layerIdx) * colorspace_max_bits, colorspace_max_bits);
}

layout(binding = 1, rgba8) writeonly uniform image2D dst;
// alias
layout(binding = 1, rgba8) writeonly uniform image2D dst_luma;
layout(binding = 2, rgba8) writeonly uniform image2D dst_chroma;

layout(binding = 3) uniform sampler2D s_samplers[VKR_SAMPLER_SLOTS];
layout(binding = 4) uniform sampler2D s_ycbcr_samplers[VKR_SAMPLER_SLOTS];

layout(binding = 5) uniform sampler1D s_shaperLut[VKR_LUT3D_COUNT];
layout(binding = 6) uniform sampler3D s_lut3D[VKR_LUT3D_COUNT];
