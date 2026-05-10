layout(binding = 0, scalar)
uniform layers_t {
    vec2 u_scale[VKR_MAX_LAYERS];
    vec2 u_offset[VKR_MAX_LAYERS];
    float u_opacity[VKR_MAX_LAYERS];
    mat3x4 u_ctm[VKR_MAX_LAYERS];
    uint u_borderMask;
    uint u_frameId;
    uint u_blur_radius;

    uint u_shaderFilter;
    uint u_alphaMode;

    // hdr
    float u_linearToNits; // sdr -> hdr
    float u_nitsToLinear; // hdr -> sdr
    float u_itmSdrNits;
    float u_itmTargetNits;
};

