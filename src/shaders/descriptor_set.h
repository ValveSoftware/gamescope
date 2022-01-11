
const int MaxLayers = 6;

layout(binding = 0, rgba8) writeonly uniform image2D dst;

layout(binding = 1) uniform sampler2D s_samplers[MaxLayers];

layout(binding = 2) uniform sampler2D s_ycbcr_samplers[MaxLayers];