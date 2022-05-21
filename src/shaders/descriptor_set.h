#include "descriptor_set_constants.h"

layout(constant_id = 0) const int  c_layerCount   = 1;
layout(constant_id = 1) const uint c_ycbcrMask    = 0;
layout(constant_id = 2) const bool c_compositing_debug = false;
layout(constant_id = 3) const uint c_blur_radius = 11;
layout(constant_id = 4) const int  c_blur_layer_count = 0;

layout(binding = 0, rgba8) writeonly uniform image2D dst;
layout(binding = 1) uniform sampler2D s_samplers[VKR_SAMPLER_SLOTS];
layout(binding = 2) uniform sampler2D s_ycbcr_samplers[VKR_SAMPLER_SLOTS];
