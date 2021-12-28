vec3 srgbToLinear(vec3 color) {
    bvec3 isLo = lessThanEqual(color, vec3(0.04045f));

    vec3 loPart = color / 12.92f;
    vec3 hiPart = pow((color + 0.055f) / 1.055f, vec3(12.0f / 5.0f));
    return mix(hiPart, loPart, isLo);
}

vec4 srgbToLinear(vec4 color) {
  return vec4(srgbToLinear(color.rgb), color.a);
}

vec3 linearToSrgb(vec3 color) {
    bvec3 isLo = lessThanEqual(color, vec3(0.0031308f));

    vec3 loPart = color * 12.92f;
    vec3 hiPart = pow(color, vec3(5.0f / 12.0f)) * 1.055f - 0.055f;
    return mix(hiPart, loPart, isLo);
}

vec4 linearToSrgb(vec4 color) {
  return vec4(linearToSrgb(color.rgb), color.a);
}

void compositing_debug(uvec2 size, uvec2 coord) {
    uvec2 pos = coord;
    pos.x -= (u_frameId & 2) != 0 ? /* size.x - 160 */ 128 : 0;
    pos.y -= (u_frameId & 1) != 0 ? /* size.y - 160 */ 128 : 0;

    if (pos.x >= 40 && pos.x < 120 && pos.y >= 40 && pos.y < 120) {
        vec4 value = vec4(1.0f, 1.0f, 1.0f, 1.0f);
        if (pos.x >= 48 && pos.x < 112 && pos.y >= 48 && pos.y < 112) {
            vec4 time = round(unpackUnorm4x8(clock2x32ARB().x * 1664525u + 1013904223u)).xyzw;
            if (time.x + time.y + time.z + time.w < 2.0f)
                value = vec4(0.0f, 0.0f, 0.0f, 1.0f);
        }
        imageStore(dst, ivec2(coord), value);
    }
}

vec4 sampleLayer(sampler2D layerSampler, uint layerIdx, vec2 uv, bool unnormalized) {
    vec2 coord = ((uv + u_offset[layerIdx]) * u_scale[layerIdx]);
    vec2 texSize = textureSize(layerSampler, 0);

    if (coord.x < 0.0f       || coord.y < 0.0f ||
        coord.x >= texSize.x || coord.y >= texSize.y) {
        float border = (u_borderMask & (1u << layerIdx)) != 0 ? 1.0f : 0.0f;
        return vec4(0.0f, 0.0f, 0.0f, border);
    }

    if (!unnormalized)
        coord /= texSize;

    return textureLod(layerSampler, coord, 0.0f);
}
