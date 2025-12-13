#ifndef SUBPIXEL_SCALER_H
#define SUBPIXEL_SCALER_H

const vec4 kSubpixelHorizontalRGBKernel[7][7] = vec4[7][7](
	vec4[7](
		vec4( 7.1068e-03f, -5.4910e-03f, -1.7302e-03f, 0.0f),
		vec4(-4.9596e-02f,  1.4541e-03f, -8.1770e-03f, 0.0f),
		vec4(-1.1168e-01f, -3.9356e-02f,  1.0656e-02f, 0.0f),
		vec4(-3.7548e-02f, -8.4949e-02f, -6.5272e-02f, 0.0f),
		vec4( 1.8490e-02f, -4.7630e-02f, -5.5001e-02f, 0.0f),
		vec4(-2.2353e-03f,  2.1439e-03f, -4.2040e-02f, 0.0f),
		vec4(-9.0465e-04f, -3.0190e-03f, -8.3075e-03f, 0.0f)
	),
	vec4[7](
		vec4(-6.2506e-03f, -3.5835e-03f, -8.5086e-03f, 0.0f),
		vec4( 3.9232e-02f,  8.1267e-03f,  1.0763e-02f, 0.0f),
		vec4( 1.3306e-02f,  2.3506e-02f,  3.9553e-03f, 0.0f),
		vec4( 2.7662e-02f,  1.9066e-02f,  2.0329e-02f, 0.0f),
		vec4(-5.6325e-03f,  2.6609e-02f,  2.3552e-02f, 0.0f),
		vec4( 7.6947e-03f,  5.1926e-03f,  2.7644e-02f, 0.0f),
		vec4(-1.1685e-02f, -3.7455e-03f,  2.3616e-03f, 0.0f)
	),
	vec4[7](
		vec4(-5.0055e-03f, -1.6617e-02f, -1.8024e-02f, 0.0f),
		vec4( 8.8724e-02f,  2.8274e-02f,  2.3960e-04f, 0.0f),
		vec4( 1.6728e-01f,  8.3065e-02f,  3.8955e-02f, 0.0f),
		vec4( 8.9757e-02f,  1.4706e-01f,  1.0101e-01f, 0.0f),
		vec4( 1.7448e-02f,  8.5608e-02f,  1.0663e-01f, 0.0f),
		vec4(-1.6968e-02f,  2.8939e-02f,  6.7206e-02f, 0.0f),
		vec4(-5.2292e-03f, -1.8133e-02f,  2.9039e-02f, 0.0f)
	),
	vec4[7](
		vec4(-6.1220e-02f, -4.9825e-02f, -5.5558e-02f, 0.0f),
		vec4( 1.8421e-01f,  1.9944e-02f, -1.7329e-02f, 0.0f),
		vec4( 2.6878e-01f,  1.4827e-01f,  3.4439e-02f, 0.0f),
		vec4( 1.8127e-01f,  2.5076e-01f,  2.2284e-01f, 0.0f),
		vec4(-6.3037e-03f,  1.6033e-01f,  1.7886e-01f, 0.0f),
		vec4(-4.9562e-02f,  2.8671e-02f,  1.3686e-01f, 0.0f),
		vec4(-1.5705e-02f, -5.4865e-02f,  5.6473e-03f, 0.0f)
	),
	vec4[7](
		vec4(-5.4844e-03f, -1.6043e-02f, -1.6392e-02f, 0.0f),
		vec4( 8.8965e-02f,  2.8861e-02f,  2.0795e-03f, 0.0f),
		vec4( 1.6636e-01f,  8.2952e-02f,  3.8352e-02f, 0.0f),
		vec4( 8.7956e-02f,  1.4735e-01f,  1.0193e-01f, 0.0f),
		vec4( 1.8432e-02f,  8.5476e-02f,  1.0605e-01f, 0.0f),
		vec4(-1.5480e-02f,  2.9659e-02f,  6.8861e-02f, 0.0f),
		vec4(-5.6236e-03f, -1.8234e-02f,  2.9642e-02f, 0.0f)
	),
	vec4[7](
		vec4(-6.7048e-03f, -3.8673e-03f, -1.0044e-02f, 0.0f),
		vec4( 3.9388e-02f,  6.9483e-03f,  1.0357e-02f, 0.0f),
		vec4( 1.3296e-02f,  2.3415e-02f,  4.4685e-03f, 0.0f),
		vec4( 2.9055e-02f,  1.8918e-02f,  2.0069e-02f, 0.0f),
		vec4(-6.1559e-03f,  2.5721e-02f,  2.3438e-02f, 0.0f),
		vec4( 7.0322e-03f,  5.6895e-03f,  2.6485e-02f, 0.0f),
		vec4(-1.2151e-02f, -4.4146e-03f,  1.3875e-03f, 0.0f)
	),
	vec4[7](
		vec4( 7.2912e-03f, -5.3521e-03f, -1.2737e-03f, 0.0f),
		vec4(-4.9607e-02f,  1.6422e-03f, -7.8235e-03f, 0.0f),
		vec4(-1.1154e-01f, -3.9406e-02f,  1.0291e-02f, 0.0f),
		vec4(-3.7869e-02f, -8.5334e-02f, -6.5227e-02f, 0.0f),
		vec4( 1.8729e-02f, -4.6851e-02f, -5.4638e-02f, 0.0f),
		vec4(-1.2923e-03f,  1.7708e-03f, -4.1082e-02f, 0.0f),
		vec4(-2.4411e-04f, -2.8019e-03f, -8.5312e-03f, 0.0f)
	)
);

const vec4 kSubpixelVerticalBGRKernel[7][7] = vec4[7][7](
	vec4[7](
		vec4(-1.3703e-03f, -2.7128e-03f, -8.9400e-03f, 0.0f),
		vec4(-1.3076e-02f, -2.9120e-03f,  2.4325e-03f, 0.0f),
		vec4(-4.6088e-03f, -1.8820e-02f,  2.6776e-02f, 0.0f),
		vec4(-1.8680e-02f, -5.5206e-02f,  2.7558e-03f, 0.0f),
		vec4(-4.2233e-03f, -1.8265e-02f,  2.8303e-02f, 0.0f),
		vec4(-1.3331e-02f, -3.5675e-03f,  1.5966e-03f, 0.0f),
		vec4(-7.5229e-04f, -2.3044e-03f, -9.3453e-03f, 0.0f)
	),
	vec4[7](
		vec4(-1.7172e-03f,  1.0680e-03f, -4.2052e-02f, 0.0f),
		vec4( 9.5862e-03f,  5.8752e-03f,  2.8153e-02f, 0.0f),
		vec4(-1.7920e-02f,  2.8416e-02f,  6.7089e-02f, 0.0f),
		vec4(-4.6624e-02f,  2.9508e-02f,  1.3613e-01f, 0.0f),
		vec4(-1.7250e-02f,  2.9594e-02f,  6.9378e-02f, 0.0f),
		vec4( 8.7751e-03f,  5.6507e-03f,  2.6455e-02f, 0.0f),
		vec4(-1.9686e-03f,  7.4416e-04f, -4.1384e-02f, 0.0f)
	),
	vec4[7](
		vec4( 1.5303e-02f, -4.8473e-02f, -5.8085e-02f, 0.0f),
		vec4(-7.1747e-03f,  2.7484e-02f,  2.3522e-02f, 0.0f),
		vec4( 1.6983e-02f,  8.5399e-02f,  1.0710e-01f, 0.0f),
		vec4(-7.9377e-03f,  1.6152e-01f,  1.8166e-01f, 0.0f),
		vec4( 1.7515e-02f,  8.5978e-02f,  1.0779e-01f, 0.0f),
		vec4(-6.5856e-03f,  2.6297e-02f,  2.2493e-02f, 0.0f),
		vec4( 1.4895e-02f, -4.8141e-02f, -5.8012e-02f, 0.0f)
	),
	vec4[7](
		vec4(-3.7719e-02f, -8.3531e-02f, -6.4092e-02f, 0.0f),
		vec4( 2.9740e-02f,  1.9817e-02f,  2.1634e-02f, 0.0f),
		vec4( 8.6868e-02f,  1.4623e-01f,  1.0123e-01f, 0.0f),
		vec4( 1.8146e-01f,  2.4817e-01f,  2.1861e-01f, 0.0f),
		vec4( 8.7783e-02f,  1.4662e-01f,  1.0216e-01f, 0.0f),
		vec4( 2.9442e-02f,  1.9655e-02f,  1.9912e-02f, 0.0f),
		vec4(-3.7865e-02f, -8.3557e-02f, -6.3263e-02f, 0.0f)
	),
	vec4[7](
		vec4(-1.1062e-01f, -3.9883e-02f,  1.0886e-02f, 0.0f),
		vec4( 1.3182e-02f,  2.3998e-02f,  3.9794e-04f, 0.0f),
		vec4( 1.6555e-01f,  8.1207e-02f,  3.6751e-02f, 0.0f),
		vec4( 2.6950e-01f,  1.4875e-01f,  3.4342e-02f, 0.0f),
		vec4( 1.6829e-01f,  8.2641e-02f,  3.5206e-02f, 0.0f),
		vec4( 1.0881e-02f,  2.3315e-02f,  3.9485e-03f, 0.0f),
		vec4(-1.0958e-01f, -3.9958e-02f,  9.7981e-03f, 0.0f)
	),
	vec4[7](
		vec4(-4.9198e-02f,  9.1998e-04f, -7.3902e-03f, 0.0f),
		vec4( 3.9535e-02f,  9.1899e-03f,  1.3642e-02f, 0.0f),
		vec4( 8.7285e-02f,  2.7588e-02f, -1.0662e-04f, 0.0f),
		vec4( 1.8349e-01f,  2.1068e-02f, -1.6866e-02f, 0.0f),
		vec4( 8.8583e-02f,  2.9153e-02f,  1.8183e-03f, 0.0f),
		vec4( 3.8491e-02f,  7.7285e-03f,  1.2305e-02f, 0.0f),
		vec4(-4.9016e-02f,  1.0721e-03f, -8.0278e-03f, 0.0f)
	),
	vec4[7](
		vec4( 6.1241e-03f, -4.8275e-03f, -9.3540e-04f, 0.0f),
		vec4(-5.4319e-03f, -2.6706e-03f, -8.6848e-03f, 0.0f),
		vec4(-6.0226e-03f, -1.7122e-02f, -1.7736e-02f, 0.0f),
		vec4(-6.1247e-02f, -4.9733e-02f, -5.5650e-02f, 0.0f),
		vec4(-4.0417e-03f, -1.6621e-02f, -1.7116e-02f, 0.0f),
		vec4(-6.7143e-03f, -3.3482e-03f, -8.2083e-03f, 0.0f),
		vec4( 6.7830e-03f, -4.3232e-03f, -7.8619e-04f, 0.0f)
	)
);

// G/RB QD-OLED subpixel-aware downscale kernel (2:1 ratio).
const vec4 kSubpixelQDOLEDKernel[5][5] = vec4[5][5](
	vec4[5](
		vec4(-2.2206e-03f, -1.7716e-03f,  2.1434e-03f, 0.0f),
		vec4(-1.4392e-02f,  4.7502e-03f,  1.3392e-02f, 0.0f),
		vec4(-1.9470e-02f, -6.1152e-02f, -4.5265e-02f, 0.0f),
		vec4(-1.2906e-02f, -6.2232e-02f, -4.2550e-02f, 0.0f),
		vec4( 2.6931e-03f, -1.4399e-04f, -1.1285e-02f, 0.0f)
	),
	vec4[5](
		vec4( 1.6583e-02f, -3.0740e-02f, -7.2153e-03f, 0.0f),
		vec4(-5.8073e-03f,  3.0652e-02f,  1.6279e-02f, 0.0f),
		vec4(-1.1306e-01f,  1.5502e-01f,  1.4186e-02f, 0.0f),
		vec4(-3.2931e-03f,  1.4972e-01f, -3.5163e-02f, 0.0f),
		vec4( 1.4865e-02f,  8.6307e-03f,  1.2385e-02f, 0.0f)
	),
	vec4[5](
		vec4(-5.5262e-03f, -7.3926e-02f, -2.5914e-02f, 0.0f),
		vec4( 7.8208e-02f, -2.7367e-02f, -4.4307e-02f, 0.0f),
		vec4( 1.7748e-01f,  3.8761e-01f,  1.5539e-01f, 0.0f),
		vec4( 8.9775e-02f,  3.8712e-01f,  1.8995e-01f, 0.0f),
		vec4(-2.2427e-02f, -1.0061e-01f,  5.5693e-02f, 0.0f)
	),
	vec4[5](
		vec4(-1.4519e-01f, -3.1293e-02f, -3.9569e-03f, 0.0f),
		vec4( 1.7445e-01f,  1.9616e-02f, -5.5174e-02f, 0.0f),
		vec4( 5.6130e-01f,  1.5526e-01f,  1.4462e-01f, 0.0f),
		vec4( 1.7025e-01f,  1.5754e-01f,  3.1903e-01f, 0.0f),
		vec4(-1.2829e-01f, -5.1410e-03f,  8.4568e-02f, 0.0f)
	),
	vec4[5](
		vec4( 5.4248e-03f,  4.0236e-03f, -1.7954e-02f, 0.0f),
		vec4( 5.4035e-02f,  3.5357e-03f, -2.3790e-02f, 0.0f),
		vec4( 4.6402e-02f, -4.9542e-02f,  1.2279e-01f, 0.0f),
		vec4( 6.5886e-02f, -5.4416e-02f,  1.1734e-01f, 0.0f),
		vec4(-4.2627e-03f,  5.0940e-03f,  4.4279e-02f, 0.0f)
	)
);
const vec4 kSubpixelOLEDKernel[5][5] = vec4[5][5](
	vec4[5](
		vec4( 8.8942e-03f, -3.2269e-03f, -4.4989e-04f, 0.0f),
		vec4(-3.0907e-03f,  9.8704e-03f, -1.9181e-02f, 0.0f),
		vec4(-1.3623e-01f, -1.7137e-02f, -8.9794e-03f, 0.0f),
		vec4(-6.1139e-03f, -9.3100e-02f, -6.3452e-02f, 0.0f),
		vec4( 1.2717e-02f, -1.0195e-02f,  6.3495e-04f, 0.0f)
	),
	vec4[5](
		vec4(-1.4033e-02f, -1.0390e-02f,  5.3642e-03f, 0.0f),
		vec4( 7.0923e-02f, -2.0851e-02f,  1.5784e-02f, 0.0f),
		vec4( 1.5164e-01f,  9.3378e-02f, -2.6439e-02f, 0.0f),
		vec4( 6.2676e-02f,  1.6882e-01f, -9.8268e-03f, 0.0f),
		vec4(-1.4588e-02f,  5.1943e-02f,  1.3150e-02f, 0.0f)
	),
	vec4[5](
		vec4(-1.3383e-01f, -1.8797e-02f, -3.9692e-02f, 0.0f),
		vec4( 1.8129e-01f, -6.1458e-02f,  4.2175e-02f, 0.0f),
		vec4( 5.6204e-01f,  1.6187e-01f,  1.4268e-01f, 0.0f),
		vec4( 1.9378e-01f,  4.4632e-01f,  2.1392e-01f, 0.0f),
		vec4(-1.2192e-01f,  8.3973e-02f, -8.6816e-03f, 0.0f)
	),
	vec4[5](
		vec4(-1.5648e-02f, -1.0165e-02f, -6.0513e-02f, 0.0f),
		vec4( 6.6628e-02f, -2.8706e-02f, -1.8292e-02f, 0.0f),
		vec4( 1.5260e-01f,  9.4369e-02f,  2.9209e-01f, 0.0f),
		vec4( 5.9264e-02f,  1.6689e-01f,  3.2808e-01f, 0.0f),
		vec4(-1.6114e-02f,  4.3363e-02f, -8.8848e-02f, 0.0f)
	),
	vec4[5](
		vec4( 1.1299e-02f, -1.3180e-03f, -3.0147e-02f, 0.0f),
		vec4(-3.8403e-04f,  1.3891e-02f,  3.9877e-02f, 0.0f),
		vec4(-1.2776e-01f, -1.7060e-02f,  9.1108e-02f, 0.0f),
		vec4(-4.5907e-03f, -7.7945e-02f,  1.5202e-01f, 0.0f),
		vec4( 1.4749e-02f, -4.2571e-03f, -8.5528e-03f, 0.0f)
	)
);

#define SAMPLE_SUBPIXEL_OLED_FILTER(layerSampler, coord, texSize, scale, colorspace, outColor) \
	do { \
		const vec2 _downscale = vec2(2.0f, 2.0f); \
		const ivec2 _downscaleInt = ivec2(2); \
		const float _alphaWeight = 1.0f / 25.0f; \
		ivec2 _outputIndex = ivec2(floor(((coord) - vec2(0.5f)) / _downscale)); \
		ivec2 _start = _outputIndex * _downscaleInt - ivec2(2); \
		ivec2 _maxCoord = (texSize) - ivec2(1); \
		vec3 _accum = vec3(0.0f); \
		float _alpha = 0.0f; \
		for (int ky = 0; ky < 5; ky++) { \
			int sy = clamp(_start.y + ky, 0, _maxCoord.y); \
			for (int kx = 0; kx < 5; kx++) { \
				int sx = clamp(_start.x + kx, 0, _maxCoord.x); \
				vec4 _texel = texelFetch((layerSampler), ivec2(sx, sy), 0); \
				vec3 _linearSample = colorspace_plane_degamma_tf(_texel.rgb, (colorspace)); \
				vec3 _kernel = kSubpixelOLEDKernel[ky][kx].rgb; \
				_accum += _linearSample * _kernel; \
				_alpha += _texel.a * _alphaWeight; \
			} \
		} \
		(outColor) = vec4(_accum, clamp(_alpha, 0.0f, 1.0f)); \
	} while (false)

#define SAMPLE_SUBPIXEL_QDOLED_FILTER(layerSampler, coord, texSize, scale, colorspace, outColor) \
	do { \
		const vec2 _downscale = vec2(2.0f, 2.0f); \
		const ivec2 _downscaleInt = ivec2(2); \
		const float _alphaWeight = 1.0f / 25.0f; \
		ivec2 _outputIndex = ivec2(floor(((coord) - vec2(0.5f)) / _downscale)); \
		ivec2 _start = _outputIndex * _downscaleInt - ivec2(2); \
		ivec2 _maxCoord = (texSize) - ivec2(1); \
		vec3 _accum = vec3(0.0f); \
		float _alpha = 0.0f; \
		for (int ky = 0; ky < 5; ky++) { \
			int sy = clamp(_start.y + ky, 0, _maxCoord.y); \
			for (int kx = 0; kx < 5; kx++) { \
				int sx = clamp(_start.x + kx, 0, _maxCoord.x); \
				vec4 _texel = texelFetch((layerSampler), ivec2(sx, sy), 0); \
				vec3 _linearSample = colorspace_plane_degamma_tf(_texel.rgb, (colorspace)); \
				vec3 _kernel = kSubpixelQDOLEDKernel[ky][kx].rgb; \
				_accum += _linearSample * _kernel; \
				_alpha += _texel.a * _alphaWeight; \
			} \
		} \
		(outColor) = vec4(_accum, clamp(_alpha, 0.0f, 1.0f)); \
	} while (false)

#define SAMPLE_SUBPIXEL_RGB_FILTER(layerSampler, coord, texSize, scale, colorspace, outColor) \
	do { \
		const vec2 _downscale = vec2(3.0f, 3.0f); \
		const ivec2 _downscaleInt = ivec2(3); \
		const float _alphaWeight = 1.0f / 49.0f; \
		ivec2 _outputIndex = ivec2(floor(((coord) - vec2(0.5f)) / _downscale)); \
		ivec2 _start = _outputIndex * _downscaleInt - ivec2(2); \
		ivec2 _maxCoord = (texSize) - ivec2(1); \
		vec3 _accum = vec3(0.0f); \
		float _alpha = 0.0f; \
		for (int ky = 0; ky < 7; ky++) { \
			int sy = clamp(_start.y + ky, 0, _maxCoord.y); \
			for (int kx = 0; kx < 7; kx++) { \
				int sx = clamp(_start.x + kx, 0, _maxCoord.x); \
				vec4 _texel = texelFetch((layerSampler), ivec2(sx, sy), 0); \
				vec3 _linearSample = colorspace_plane_degamma_tf(_texel.rgb, (colorspace)); \
				vec3 _kernel = kSubpixelHorizontalRGBKernel[ky][kx].rgb; \
				_accum += _linearSample * _kernel; \
				_alpha += _texel.a * _alphaWeight; \
			} \
		} \
		(outColor) = vec4(_accum, clamp(_alpha, 0.0f, 1.0f)); \
	} while (false)

#define SAMPLE_SUBPIXEL_VBGR_FILTER(layerSampler, coord, texSize, scale, colorspace, outColor) \
	do { \
		const vec2 _downscale = vec2(3.0f, 3.0f); \
		const ivec2 _downscaleInt = ivec2(3); \
		const float _alphaWeight = 1.0f / 49.0f; \
		ivec2 _outputIndex = ivec2(floor(((coord) - vec2(0.5f)) / _downscale)); \
		ivec2 _start = _outputIndex * _downscaleInt - ivec2(2); \
		ivec2 _maxCoord = (texSize) - ivec2(1); \
		vec3 _accum = vec3(0.0f); \
		float _alpha = 0.0f; \
		for (int ky = 0; ky < 7; ky++) { \
			int sy = clamp(_start.y + ky, 0, _maxCoord.y); \
			for (int kx = 0; kx < 7; kx++) { \
				int sx = clamp(_start.x + kx, 0, _maxCoord.x); \
				vec4 _texel = texelFetch((layerSampler), ivec2(sx, sy), 0); \
				vec3 _linearSample = colorspace_plane_degamma_tf(_texel.rgb, (colorspace)); \
				vec3 _kernel = kSubpixelVerticalBGRKernel[ky][kx].rgb; \
				_accum += _linearSample * _kernel; \
				_alpha += _texel.a * _alphaWeight; \
			} \
		} \
		(outColor) = vec4(_accum, clamp(_alpha, 0.0f, 1.0f)); \
	} while (false)

#endif
