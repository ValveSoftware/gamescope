#include "colorimetry.h"

uint pseudo_random(uint seed) {
    seed ^= (seed << 13);
    seed ^= (seed >> 17);
    seed ^= (seed << 5);
    return seed * 1664525u + 1013904223u;
}

void compositing_debug(uvec2 coord) {
    uvec2 pos = coord;
    pos.x -= (u_frameId & 2) != 0 ?  128 : 0;
    pos.y -= (u_frameId & 1) != 0 ?  128 : 0;

    if (pos.x >= 40 && pos.x < 120 && pos.y >= 40 && pos.y < 120) {
        vec4 value = vec4(1.0f, 1.0f, 1.0f, 1.0f);
        if (pos.x >= 48 && pos.x < 112 && pos.y >= 48 && pos.y < 112) {
            uint random = pseudo_random(u_frameId.x + (pos.x & ~0x7) + (pos.y & ~0x7) * 50);
            vec4 time = round(unpackUnorm4x8(random)).xyzw;
            if (time.x + time.y + time.z + time.w < 2.0f)
                value = vec4(0.0f, 0.0f, 0.0f, 1.0f);
        }
        imageStore(dst, ivec2(coord), value);
    }
}

// Takes in a scRGB/Linear encoded value and applies color management
// based on the input colorspace.
//
// ie. call colorspace_plane_degamma_tf(color.rgb, colorspace) before
// input to this function.
vec3 apply_layer_color_mgmt(vec3 color, uint colorspace) {
    if (c_itm_enable)
    {
        color = bt2446a_inverse_tonemapping(color, u_itmSdrNits, u_itmTargetNits);
        colorspace = colorspace_pq;
    }

    if (checkDebugFlag(compositedebug_Heatmap))
    {
        // Debug HDR heatmap.
        color = hdr_heatmap(color, colorspace);
    }
    else
    {
        // Shaper + 3D LUT path to match DRM.

        uint plane_eotf = colorspace_to_eotf(colorspace);

        color = colorspace_plane_shaper_tf(color, colorspace);
        // The shaper TF is basically just a regamma to get into something the shaper LUT can handle.
        //
        // Despite naming, degamma + shaper TF are NOT necessarily the inverse of each other. ^^^
        // This gets the color ready to go into the shaper LUT.
        // ie. scRGB -> PQ
        //
        // We also need to do degamma here for non-linear views to blend in linear space.
        // ie. PQ -> PQ would need us to manually do bilinear here.
        color = perform_1dlut(color, s_shaperLut[plane_eotf]);
        color = perform_3dlut(color, s_lut3D[plane_eotf]);
        color = colorspace_blend_tf(color, c_output_eotf);
    }

    return color;
}


vec4 sampleLayerEx(sampler2D layerSampler, uint offsetLayerIdx, uint colorspaceLayerIdx, vec2 uv, bool unnormalized) {
    vec2 coord = ((uv + u_offset[offsetLayerIdx]) * u_scale[offsetLayerIdx]);
    vec2 texSize = textureSize(layerSampler, 0);

    if (coord.x < 0.0f       || coord.y < 0.0f ||
        coord.x >= texSize.x || coord.y >= texSize.y) {
        float border = (u_borderMask & (1u << offsetLayerIdx)) != 0 ? 1.0f : 0.0f;

        if (checkDebugFlag(compositedebug_PlaneBorders))
            return vec4(vec3(1.0f, 0.0f, 1.0f) * border, border);

        return vec4(0.0f, 0.0f, 0.0f, border);
    }

    if (!unnormalized)
        coord /= texSize;

    vec4 color = textureLod(layerSampler, coord, 0.0f);

    // TODO(Josh): If colorspace != linear, emulate bilinear ourselves to blend
    // in linear space!
    // Split this into two parts!
    uint colorspace = get_layer_colorspace(colorspaceLayerIdx);
    color.rgb = colorspace_plane_degamma_tf(color.rgb, colorspace);
    color.rgb = apply_layer_color_mgmt(color.rgb, colorspace);

    return color;
}

vec4 sampleLayer(sampler2D layerSampler, uint layerIdx, vec2 uv, bool unnormalized) {
    return sampleLayerEx(layerSampler, layerIdx, layerIdx, uv, unnormalized);
}

vec3 encodeOutputColor(vec3 value) {
    return colorspace_output_tf(value, c_output_eotf);
}
