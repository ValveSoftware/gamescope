#include "colorimetry.h"

#include "shaderfilter.h"
#include "alphamode.h"

vec4 sampleRegular(sampler2D tex, vec2 coord, uint colorspace) {
    vec4 color = textureLod(tex, coord, 0);
    color.rgb = colorspace_plane_degamma_tf(color.rgb, colorspace);
    return color;
}

// To be considered pseudo-bandlimited, upscaling factor must be at least 2x.

const float bandlimited_PI = 3.14159265359;
const float bandlimited_PI_half = 0.5 * bandlimited_PI;
// size: resolution of sampled texture
// inv_size: inverse resolution of sampled texture
// extent: Screen-space gradient of UV in texels. Typically computed as (texture resolution) / (viewport resolution).
//   If screen is rotated by 90 or 270 degrees, the derivatives need to be computed appropriately.
//   For uniform scaling, none of this matters.
//   extent can be multiplied to achieve LOD bias.
//   extent must be at least 1.0 / 256.0.
vec4 sampleBandLimited(sampler2D samp, vec2 uv, vec2 size, vec2 inv_size, vec2 extent, uint colorspace, bool unnormalized)
{
    // Josh:
    // Clamp to behaviour like 4x scale (0.25).
    //
    // Was defaulted to 2x before (0.5), which is 1px, but gives blurry result
    // on Cave Story (480p) -> 800p on Deck.
    // TODO: Maybe make this configurable?
    const float max_extent = 0.25f;

	// Get base pixel and phase, range [0, 1).
	vec2 pixel = uv * (unnormalized ? vec2(1.0f) : size) - 0.5;
	vec2 base_pixel = floor(pixel);
	vec2 phase = pixel - base_pixel;

	// We can resolve the filter by just sampling a single 2x2 block.
	// Lerp between normal sampling at LOD 0, and bandlimited pixel filter at LOD -1.
	vec2 shift = 0.5 + 0.5 * sin(bandlimited_PI_half * clamp((phase - 0.5) / min(extent, vec2(max_extent)), -1.0, 1.0));
	uv = (base_pixel + 0.5 + shift) * (unnormalized ? vec2(1.0f) : inv_size);

	return sampleRegular(samp, uv, colorspace);
}

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
        if (checkDebugFlag(compositedebug_Markers_Partial)) {
            value = vec4(0.0f, 1.0f, 1.0f, 1.0f);
        }
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
vec3 apply_layer_color_mgmt(vec3 color, uint layer, uint colorspace) {
    if (colorspace == colorspace_passthru)
        return color;

    if (c_itm_enable)
    {
        color = bt2446a_inverse_tonemapping(color, u_itmSdrNits, u_itmTargetNits);
        colorspace = colorspace_pq;
    }

    // Shaper + 3D LUT path to match DRM.
    uint plane_eotf = colorspace_to_eotf(colorspace);

    if (layer == 0 && checkDebugFlag(compositedebug_Heatmap))
    {
        // Debug HDR heatmap.
        color = hdr_heatmap(color, colorspace);
        plane_eotf = EOTF_Gamma22;
    }


    // The shaper TF is basically just a regamma to get into something the shaper LUT can handle.
    //
    // Despite naming, degamma + shaper TF are NOT necessarily the inverse of each other. ^^^
    // This gets the color ready to go into the shaper LUT.
    // ie. scRGB -> PQ
    //
    // We also need to do degamma here for non-linear views to blend in linear space.
    // ie. PQ -> PQ would need us to manually do bilinear here.
    bool lut3d_enabled = textureQueryLevels(s_shaperLut[plane_eotf]) != 0;
    if (lut3d_enabled)
    {
        color = colorspace_plane_shaper_tf(color, colorspace);
        color = perform_1dlut(color, s_shaperLut[plane_eotf]);
        color = perform_3dlut(color, s_lut3D[plane_eotf]);
        color = colorspace_blend_tf(color, c_output_eotf);
    }

    return color;
}

vec4 sampleBilinear(sampler2D tex, vec2 coord, uint colorspace, bool unnormalized) {
    vec2 scale = unnormalized ? vec2(1.0) : vec2(textureSize(tex, 0));

    vec2 pixCoord = coord * scale - 0.5f;
    vec2 originPixCoord = floor(pixCoord);

    vec2 gatherUV = (originPixCoord * scale + 1.0f) / scale;

    vec4 red   = textureGather(tex, gatherUV, 0);
    vec4 green = textureGather(tex, gatherUV, 1);
    vec4 blue  = textureGather(tex, gatherUV, 2);
    vec4 alpha = textureGather(tex, gatherUV, 3);

    vec4 c00 = vec4(red.w, green.w, blue.w, alpha.w);
    vec4 c01 = vec4(red.x, green.x, blue.x, alpha.x);
    vec4 c11 = vec4(red.y, green.y, blue.y, alpha.y);
    vec4 c10 = vec4(red.z, green.z, blue.z, alpha.z);

    c00.rgb = colorspace_plane_degamma_tf(c00.rgb, colorspace);
    c01.rgb = colorspace_plane_degamma_tf(c01.rgb, colorspace);
    c11.rgb = colorspace_plane_degamma_tf(c11.rgb, colorspace);
    c10.rgb = colorspace_plane_degamma_tf(c10.rgb, colorspace);

    vec2 filterWeight = pixCoord - originPixCoord;

    vec4 temp0 = mix(c01, c11, filterWeight.x);
    vec4 temp1 = mix(c00, c10, filterWeight.x);
    return mix(temp1, temp0, filterWeight.y);
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

    uint colorspace = get_layer_colorspace(colorspaceLayerIdx);
    vec4 color;
    if (get_layer_shaderfilter(offsetLayerIdx) == filter_pixel) {
        vec2 output_res = texSize / u_scale[offsetLayerIdx];
        vec2 extent = max((texSize / output_res), vec2(1.0 / 256.0));
        color = sampleBandLimited(layerSampler, coord, unnormalized ? vec2(1.0f) : texSize, unnormalized ? vec2(1.0f) : vec2(1.0f) / texSize, extent, colorspace, unnormalized);
    }
    else if (get_layer_shaderfilter(offsetLayerIdx) == filter_linear_emulated) {
        color = sampleBilinear(layerSampler, coord, colorspace, unnormalized);
    }
    else {
        color = sampleRegular(layerSampler, coord, colorspace);
    }
    // JoshA: AMDGPU applies 3x4 CTM like this, where A is 1.0, but it only affects .rgb.
    color.rgb = vec4(color.rgb, 1.0f) * u_ctm[colorspaceLayerIdx];
    color.rgb = apply_layer_color_mgmt(color.rgb, offsetLayerIdx, colorspace);

    return color;
}

vec4 sampleLayer(sampler2D layerSampler, uint layerIdx, vec2 uv, bool unnormalized) {
    return sampleLayerEx(layerSampler, layerIdx, layerIdx, uv, unnormalized);
}

vec3 encodeOutputColor(vec3 value) {
    return colorspace_output_tf(value, c_output_eotf);
}
