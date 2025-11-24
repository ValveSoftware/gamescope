#ifndef SUBPIXEL_SCALER_H
#define SUBPIXEL_SCALER_H

// Generic subpixel-aware downscale helpers.
// Current implementations:
//  - horizontal RGB subpixel-aware downscale filter with a fixed 3:1 ratio.
//  - RG/B OLED subpixel-aware downscale filter with a fixed 2:1 ratio.

const float kSubpixelRatioTolerance = 0.05f;
const vec2 kSubpixelRGBDownscale = vec2(3.0f, 3.0f);
const ivec2 kSubpixelRGBDownscaleInt = ivec2(3);
const float kSubpixelRGBAlphaWeight = 1.0f / 49.0f;

const vec2 kSubpixelOLEDDownscale = vec2(2.0f, 2.0f);
const ivec2 kSubpixelOLEDDownscaleInt = ivec2(2);
const float kSubpixelOLEDAlphaWeight = 1.0f / 25.0f;

const vec3 kSubpixelHorizontalRGBKernel[7][7] = vec3[7][7](
    vec3[7](
        vec3( 7.1068e-03f, -5.4910e-03f, -1.7302e-03f),
        vec3(-4.9596e-02f,  1.4541e-03f, -8.1770e-03f),
        vec3(-1.1168e-01f, -3.9356e-02f,  1.0656e-02f),
        vec3(-3.7548e-02f, -8.4949e-02f, -6.5272e-02f),
        vec3( 1.8490e-02f, -4.7630e-02f, -5.5001e-02f),
        vec3(-2.2353e-03f,  2.1439e-03f, -4.2040e-02f),
        vec3(-9.0465e-04f, -3.0190e-03f, -8.3075e-03f)
    ),
    vec3[7](
        vec3(-6.2506e-03f, -3.5835e-03f, -8.5086e-03f),
        vec3( 3.9232e-02f,  8.1267e-03f,  1.0763e-02f),
        vec3( 1.3306e-02f,  2.3506e-02f,  3.9553e-03f),
        vec3( 2.7662e-02f,  1.9066e-02f,  2.0329e-02f),
        vec3(-5.6325e-03f,  2.6609e-02f,  2.3552e-02f),
        vec3( 7.6947e-03f,  5.1926e-03f,  2.7644e-02f),
        vec3(-1.1685e-02f, -3.7455e-03f,  2.3616e-03f)
    ),
    vec3[7](
        vec3(-5.0055e-03f, -1.6617e-02f, -1.8024e-02f),
        vec3( 8.8724e-02f,  2.8274e-02f,  2.3960e-04f),
        vec3( 1.6728e-01f,  8.3065e-02f,  3.8955e-02f),
        vec3( 8.9757e-02f,  1.4706e-01f,  1.0101e-01f),
        vec3( 1.7448e-02f,  8.5608e-02f,  1.0663e-01f),
        vec3(-1.6968e-02f,  2.8939e-02f,  6.7206e-02f),
        vec3(-5.2292e-03f, -1.8133e-02f,  2.9039e-02f)
    ),
    vec3[7](
        vec3(-6.1220e-02f, -4.9825e-02f, -5.5558e-02f),
        vec3( 1.8421e-01f,  1.9944e-02f, -1.7329e-02f),
        vec3( 2.6878e-01f,  1.4827e-01f,  3.4439e-02f),
        vec3( 1.8127e-01f,  2.5076e-01f,  2.2284e-01f),
        vec3(-6.3037e-03f,  1.6033e-01f,  1.7886e-01f),
        vec3(-4.9562e-02f,  2.8671e-02f,  1.3686e-01f),
        vec3(-1.5705e-02f, -5.4865e-02f,  5.6473e-03f)
    ),
    vec3[7](
        vec3(-5.4844e-03f, -1.6043e-02f, -1.6392e-02f),
        vec3( 8.8965e-02f,  2.8861e-02f,  2.0795e-03f),
        vec3( 1.6636e-01f,  8.2952e-02f,  3.8352e-02f),
        vec3( 8.7956e-02f,  1.4735e-01f,  1.0193e-01f),
        vec3( 1.8432e-02f,  8.5476e-02f,  1.0605e-01f),
        vec3(-1.5480e-02f,  2.9659e-02f,  6.8861e-02f),
        vec3(-5.6236e-03f, -1.8234e-02f,  2.9642e-02f)
    ),
    vec3[7](
        vec3(-6.7048e-03f, -3.8673e-03f, -1.0044e-02f),
        vec3( 3.9388e-02f,  6.9483e-03f,  1.0357e-02f),
        vec3( 1.3296e-02f,  2.3415e-02f,  4.4685e-03f),
        vec3( 2.9055e-02f,  1.8918e-02f,  2.0069e-02f),
        vec3(-6.1559e-03f,  2.5721e-02f,  2.3438e-02f),
        vec3( 7.0322e-03f,  5.6895e-03f,  2.6485e-02f),
        vec3(-1.2151e-02f, -4.4146e-03f,  1.3875e-03f)
    ),
    vec3[7](
        vec3( 7.2912e-03f, -5.3521e-03f, -1.2737e-03f),
        vec3(-4.9607e-02f,  1.6422e-03f, -7.8235e-03f),
        vec3(-1.1154e-01f, -3.9406e-02f,  1.0291e-02f),
        vec3(-3.7869e-02f, -8.5334e-02f, -6.5227e-02f),
        vec3( 1.8729e-02f, -4.6851e-02f, -5.4638e-02f),
        vec3(-1.2923e-03f,  1.7708e-03f, -4.1082e-02f),
        vec3(-2.4411e-04f, -2.8019e-03f, -8.5312e-03f)
    )
);

const vec3 kSubpixelOLEDKernel[5][5] = vec3[5][5](
    vec3[5](
        vec3( 8.8942e-03f, -3.2269e-03f, -4.4989e-04f),
        vec3(-3.0907e-03f,  9.8704e-03f, -1.9181e-02f),
        vec3(-1.3623e-01f, -1.7137e-02f, -8.9794e-03f),
        vec3(-6.1139e-03f, -9.3100e-02f, -6.3452e-02f),
        vec3( 1.2717e-02f, -1.0195e-02f,  6.3495e-04f)
    ),
    vec3[5](
        vec3(-1.4033e-02f, -1.0390e-02f,  5.3642e-03f),
        vec3( 7.0923e-02f, -2.0851e-02f,  1.5784e-02f),
        vec3( 1.5164e-01f,  9.3378e-02f, -2.6439e-02f),
        vec3( 6.2676e-02f,  1.6882e-01f, -9.8268e-03f),
        vec3(-1.4588e-02f,  5.1943e-02f,  1.3150e-02f)
    ),
    vec3[5](
        vec3(-1.3383e-01f, -1.8797e-02f, -3.9692e-02f),
        vec3( 1.8129e-01f, -6.1458e-02f,  4.2175e-02f),
        vec3( 5.6204e-01f,  1.6187e-01f,  1.4268e-01f),
        vec3( 1.9378e-01f,  4.4632e-01f,  2.1392e-01f),
        vec3(-1.2192e-01f,  8.3973e-02f, -8.6816e-03f)
    ),
    vec3[5](
        vec3(-1.5648e-02f, -1.0165e-02f, -6.0513e-02f),
        vec3( 6.6628e-02f, -2.8706e-02f, -1.8292e-02f),
        vec3( 1.5260e-01f,  9.4369e-02f,  2.9209e-01f),
        vec3( 5.9264e-02f,  1.6689e-01f,  3.2808e-01f),
        vec3(-1.6114e-02f,  4.3363e-02f, -8.8848e-02f)
    ),
    vec3[5](
        vec3( 1.1299e-02f, -1.3180e-03f, -3.0147e-02f),
        vec3(-3.8403e-04f,  1.3891e-02f,  3.9877e-02f),
        vec3(-1.2776e-01f, -1.7060e-02f,  9.1108e-02f),
        vec3(-4.5907e-03f, -7.7945e-02f,  1.5202e-01f),
        vec3( 1.4749e-02f, -4.2571e-03f, -8.5528e-03f)
    )
);

// Explicit sRGB decode for subpixel sampling when source is sRGB/linear.
// Avoids depending on colorspace state for OLED path.
vec3 SubpixelSRGBToLinear(vec3 c)
{
    // If the source is already linear, skip decode.
    return c;
}

bool try_sample_subpixel_filter(uint shaderFilter, sampler2D layerSampler, vec2 coord, ivec2 texSize, vec2 scale, uint colorspace, out vec4 outColor)
{
	if (shaderFilter != filter_subpixel_rgb && shaderFilter != filter_subpixel_oled)
		return false;

	vec2 ratio = shaderFilter == filter_subpixel_oled ? kSubpixelOLEDDownscale : kSubpixelRGBDownscale;
	ivec2 ratioInt = shaderFilter == filter_subpixel_oled ? kSubpixelOLEDDownscaleInt : kSubpixelRGBDownscaleInt;
	float alphaWeight = shaderFilter == filter_subpixel_oled ? kSubpixelOLEDAlphaWeight : kSubpixelRGBAlphaWeight;

	if (any(greaterThan(abs(scale - ratio), vec2(kSubpixelRatioTolerance))))
		return false;

	ivec2 outputIndex = ivec2(floor((coord - vec2(0.5f)) / ratio));
	ivec2 start = outputIndex * ratioInt - ivec2(2);
	ivec2 maxCoord = texSize - ivec2(1);

	vec3 accum = vec3(0.0f);
	float alpha = 0.0f;

	if (shaderFilter == filter_subpixel_oled) {
		for (int ky = 0; ky < 5; ky++) {
			int sy = clamp(start.y + ky, 0, maxCoord.y);
			for (int kx = 0; kx < 5; kx++) {
				int sx = clamp(start.x + kx, 0, maxCoord.x);
				vec4 texel = texelFetch(layerSampler, ivec2(sx, sy), 0);
				vec3 linearSample = SubpixelSRGBToLinear(texel.rgb);

				vec3 kernel = kSubpixelOLEDKernel[ky][kx];
				accum += linearSample * kernel;
				alpha += texel.a * alphaWeight;
			}
		}
	} else {
		for (int ky = 0; ky < 7; ky++) {
			int sy = clamp(start.y + ky, 0, maxCoord.y);
			for (int kx = 0; kx < 7; kx++) {
				int sx = clamp(start.x + kx, 0, maxCoord.x);
				// Shift green footprint by +2 taps horizontally (tuned for RGB path).
				int sxG = clamp(start.x + 2 + kx, 0, maxCoord.x);

				vec4 texel = texelFetch(layerSampler, ivec2(sx, sy), 0);
				vec4 texelG = texelFetch(layerSampler, ivec2(sxG, sy), 0);

				vec3 linearSample = colorspace_plane_degamma_tf(texel.rgb, colorspace);
				linearSample.g = colorspace_plane_degamma_tf(texelG.rgb, colorspace).g;

				// Apply kernel with channel permutation r->r, g->b, b->g.
				vec3 kernel = kSubpixelHorizontalRGBKernel[ky][kx].rbg;
				accum += linearSample * kernel;
				alpha += texel.a * alphaWeight;
			}
		}
	}

	outColor = vec4(accum, clamp(alpha, 0.0f, 1.0f));
	return true;
}

#endif
