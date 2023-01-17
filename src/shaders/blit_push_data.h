layout(push_constant)
uniform layers_t {
    vec2 u_scale[VKR_MAX_LAYERS];
    vec2 u_offset[VKR_MAX_LAYERS];
    float u_opacity[VKR_MAX_LAYERS];
    uint u_borderMask;
    uint u_frameId;
    uint u_blur_radius;

    // hdr
    float u_linearToNits; // sdr -> hdr
    float u_nitsToLinear; // hdr -> sdr
    float u_itmSdrNits;
    float u_itmTargetNits;
};
