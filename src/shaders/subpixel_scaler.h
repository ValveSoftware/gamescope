#ifndef SUBPIXEL_SCALER_H
#define SUBPIXEL_SCALER_H

// Generic subpixel-aware downscale helpers.
// Current implementation targets RGB vertical stripe layouts with a fixed 3:1 ratio.

const float kSubpixelRatioTolerance = 0.05f;
const vec2 kSubpixelRGBDownscale = vec2(3.0f, 3.0f);
const ivec2 kSubpixelRGBDownscaleInt = ivec2(3);
const float kSubpixelAlphaWeight = 1.0f / 49.0f;

const vec3 kSubpixelRGBVerticalKernel[7][7] = vec3[7][7](
    vec3[7](
        vec3(-1.0526e-02f,  3.8514e-03f,  1.1707e-02f),
        vec3(-5.1071e-02f, -5.4484e-03f, -1.5588e-02f),
        vec3(-8.5119e-02f, -5.0128e-02f, -2.6335e-02f),
        vec3(-6.1192e-02f, -8.9105e-02f, -5.8017e-02f),
        vec3(-9.1344e-03f, -5.3422e-02f, -5.6514e-02f),
        vec3( 3.5897e-03f, -6.5463e-03f, -3.8170e-02f),
        vec3( 5.5279e-03f,  4.3577e-03f, -2.2234e-02f)
    ),
    vec3[7](
        vec3(-2.4249e-03f, -4.5839e-03f, -2.0525e-02f),
        vec3( 2.6682e-02f,  1.0168e-02f,  2.2323e-02f),
        vec3( 2.3826e-02f,  2.2712e-02f, -5.5703e-03f),
        vec3( 2.4192e-02f,  2.5182e-02f,  4.2159e-02f),
        vec3( 1.0538e-02f,  2.1549e-02f,  1.9901e-03f),
        vec3( 1.9939e-03f,  1.0217e-02f,  3.5535e-02f),
        vec3(-1.1452e-03f, -3.9492e-03f, -1.5025e-02f)
    ),
    vec3[7](
        vec3(-1.9276e-03f, -3.3225e-02f, -1.5280e-02f),
        vec3( 1.1129e-01f,  1.9505e-02f, -4.1840e-02f),
        vec3( 1.5322e-01f,  1.0794e-01f,  2.8163e-02f),
        vec3( 1.1598e-01f,  1.6799e-01f,  1.2661e-01f),
        vec3( 2.3158e-02f,  1.0714e-01f,  1.7142e-01f),
        vec3(-2.1166e-02f,  1.9900e-02f,  1.0636e-01f),
        vec3(-1.9158e-02f, -3.2187e-02f, -3.2487e-03f)
    ),
    vec3[7](
        vec3(-9.8290e-03f, -5.6365e-02f, -4.0351e-02f),
        vec3( 1.7032e-01f,  2.1324e-02f,  5.7782e-03f),
        vec3( 2.3965e-01f,  1.6300e-01f,  5.7683e-02f),
        vec3( 1.8491e-01f,  2.5932e-01f,  1.6544e-01f),
        vec3( 2.8468e-02f,  1.6246e-01f,  1.8679e-01f),
        vec3(-4.0587e-02f,  2.2935e-02f,  1.5421e-01f),
        vec3(-4.3002e-02f, -5.5869e-02f,  1.2911e-02f)
    ),
    vec3[7](
        vec3(-2.0463e-03f, -3.3211e-02f, -4.0655e-03f),
        vec3( 1.1078e-01f,  1.9467e-02f, -3.8520e-02f),
        vec3( 1.5392e-01f,  1.0804e-01f,  4.1863e-02f),
        vec3( 1.1533e-01f,  1.6802e-01f,  9.2533e-02f),
        vec3( 2.3271e-02f,  1.0719e-01f,  1.6531e-01f),
        vec3(-2.1582e-02f,  1.9986e-02f,  1.0219e-01f),
        vec3(-1.7926e-02f, -3.2075e-02f,  1.5415e-02f)
    ),
    vec3[7](
        vec3(-2.5467e-03f, -4.5877e-03f, -1.0176e-02f),
        vec3( 2.6298e-02f,  1.0157e-02f,  2.1141e-02f),
        vec3( 2.4420e-02f,  2.2687e-02f,  9.2225e-03f),
        vec3( 2.3553e-02f,  2.5167e-02f,  1.2310e-02f),
        vec3( 1.0961e-02f,  2.1636e-02f,  1.4885e-03f),
        vec3( 1.5187e-03f,  1.0232e-02f,  3.3014e-02f),
        vec3(-1.6591e-04f, -3.8778e-03f, -4.7787e-04f)
    ),
    vec3[7](
        vec3(-1.0415e-02f,  3.8308e-03f,  9.5637e-03f),
        vec3(-5.1437e-02f, -5.4800e-03f, -2.3571e-02f),
        vec3(-8.4871e-02f, -5.0085e-02f, -2.7867e-02f),
        vec3(-6.2043e-02f, -8.9107e-02f, -4.9899e-02f),
        vec3(-8.7220e-03f, -5.3422e-02f, -4.5404e-02f),
        vec3( 3.4067e-03f, -6.5015e-03f, -3.7778e-02f),
        vec3( 6.0927e-03f,  4.4328e-03f, -3.0901e-02f)
    )
);

bool try_sample_subpixel_filter(uint shaderFilter, sampler2D layerSampler, vec2 coord, ivec2 texSize, vec2 scale, uint colorspace, out vec4 outColor)
{
	if (shaderFilter != filter_subpixel_rgb)
		return false;

	if (any(greaterThan(abs(scale - kSubpixelRGBDownscale), vec2(kSubpixelRatioTolerance))))
		return false;

	ivec2 outputIndex = ivec2(floor((coord - vec2(0.5f)) / kSubpixelRGBDownscale));
	ivec2 start = outputIndex * kSubpixelRGBDownscaleInt - ivec2(2);
	ivec2 maxCoord = texSize - ivec2(1);

	vec3 accum = vec3(0.0f);
	float alpha = 0.0f;

	for (int ky = 0; ky < 7; ky++) {
		int sy = clamp(start.y + ky, 0, maxCoord.y);
		for (int kx = 0; kx < 7; kx++) {
			int sx = clamp(start.x + kx, 0, maxCoord.x);
			vec4 texel = texelFetch(layerSampler, ivec2(sx, sy), 0);
			vec3 linearSample = colorspace_plane_degamma_tf(texel.rgb, colorspace);

			vec3 kernel = kSubpixelRGBVerticalKernel[ky][kx];
			accum += linearSample * kernel;
			alpha += texel.a * kSubpixelAlphaWeight;
		}
	}

	outColor = vec4(accum, clamp(alpha, 0.0f, 1.0f));
	return true;
}

#endif
