
layout(constant_id = 0) const int  c_layerCount   = 1;
layout(constant_id = 1) const uint c_ycbcrMask    = 0;
layout(constant_id = 2) const bool c_compositing_debug = false;
layout(constant_id = 3) const uint c_blur_radius = 11;
layout(constant_id = 4) const int  c_blur_layer_count = 0;

const int MaxLayers = 6;

layout(binding = 0, rgba8) writeonly uniform image2D dst;
layout(binding = 1) uniform sampler2D s_samplers[MaxLayers];
layout(binding = 2) uniform sampler2D s_ycbcr_samplers[MaxLayers];
layout(binding = 3) uniform sampler2D s_sampler_extra;

#if defined(NIS_SCALER)
layout(binding = 4) uniform sampler2D coef_scaler;
layout(binding = 5) uniform sampler2D coef_usm;
#endif
