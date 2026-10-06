/*
Copyright (c) 2023, 2025, Qualcomm Innovation Center, Inc. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

SPDX-License-Identifier: BSD-3-Clause
*/

// Adapted from Qualcomm SGSR1, d926f074bcb9d714e179f1ce0fcb9ee2eeb5074e.
// sgsr/v1/include/glsl/sgsr1_shader_mobile.frag, with the departures noted below.

mediump float sgsrFastLanczos2(mediump float x)
{
	// The polynomial turns positive again past 4, the narrower kernel can reach it.
	x = min(x, 4.0);
	mediump float wA = x-4.0;
	mediump float wB = x*wA-wA;
	wA *= wA;
	return wB*wA;
}
// Above the reference 0.55 to narrow the kernel, so far taps cannot outvote a lone dark texel.
const float sgsrKernelWidth = 0.8;

// Outlier rejection fades in with tap distance, so the texel covering the pixel always votes.
mediump float sgsrWeight(mediump float dx, mediump float dy, mediump float g, mediump float std)
{
	mediump float d2 = (dx*dx)+(dy*dy);
	mediump float outlier = clamp(abs(g)*std, 0.0, 1.0) * min(d2 * 0.5, 1.0);
	return sgsrFastLanczos2(d2 * sgsrKernelWidth + outlier);
}
// Clamped to its own nearest 2x2, so shared weights cannot pull a channel out of range.
mediump float sgsrChannel(mediump float w[12], mediump vec4 ud, mediump vec4 l, mediump vec4 r, mediump float invSum)
{
	mediump float acc = dot(vec4(w[0], w[1], w[2], w[3]), ud) + dot(vec4(w[4], w[5], w[6], w[7]), l) + dot(vec4(w[8], w[9], w[10], w[11]), r);
	mediump float lo = min(min(l.y, l.z), min(r.x, r.w));
	mediump float hi = max(max(l.y, l.z), max(r.x, r.w));
	return clamp(acc * invSum, lo, hi);
}
mediump float sgsrMean(mediump vec4 l, mediump vec4 r)
{
	return (l.y+l.z+r.x+r.w)*0.25;
}

vec4 sampleSgsr1(sampler2D tex, vec2 uv)
{
	const float edgeThreshold = 8.0 / 255.0;
	mediump vec4 color = textureLod(tex, uv, 0.0);

	vec2 size = vec2(textureSize(tex, 0));
	vec4 viewport = vec4(1.0 / size, size);
	{
		highp vec2 imgCoord = ((uv*viewport.zw)+vec2(-0.5,0.5));
		highp vec2 imgCoordPixel = floor(imgCoord);
		highp vec2 coord = (imgCoordPixel*viewport.xy);
		mediump vec2 pl = (imgCoord+(-imgCoordPixel));
		mediump vec4 leftG = textureGather(tex,coord, 1);

		mediump float edgeVote = abs(leftG.z - leftG.y) + abs(color.g - leftG.y)  + abs(color.g - leftG.z) ;
		if(edgeVote > edgeThreshold)
		{
			// Red and blue wait on green's weights so twelve gathers are never live at once.
			highp vec2 leftCoord = coord;
			coord.x += viewport.x;
			highp vec2 rightCoord = coord + vec2(viewport.x, 0.0);
			highp vec2 upCoord = coord + vec2(0.0, -viewport.y);
			highp vec2 downCoord = coord + vec2(0.0, viewport.y);
			mediump vec4 rightG = textureGather(tex,rightCoord, 1);
			mediump vec4 upDownG;
			upDownG.xy = textureGather(tex,upCoord, 1).wz;
			upDownG.zw = textureGather(tex,downCoord, 1).yx;

			mediump float meanG = sgsrMean(leftG, rightG);
			leftG -= vec4(meanG); rightG -= vec4(meanG); upDownG -= vec4(meanG);

			mediump float sum = (((((abs(leftG.x)+abs(leftG.y))+abs(leftG.z))+abs(leftG.w))+(((abs(rightG.x)+abs(rightG.y))+abs(rightG.z))+abs(rightG.w)))+(((abs(upDownG.x)+abs(upDownG.y))+abs(upDownG.z))+abs(upDownG.w)));
			mediump float std = 2.181818/sum;

			mediump float w[12];
			w[0]  = sgsrWeight(pl.x,     pl.y+1.0, upDownG.x, std);
			w[1]  = sgsrWeight(pl.x-1.0, pl.y+1.0, upDownG.y, std);
			w[2]  = sgsrWeight(pl.x-1.0, pl.y-2.0, upDownG.z, std);
			w[3]  = sgsrWeight(pl.x,     pl.y-2.0, upDownG.w, std);
			w[4]  = sgsrWeight(pl.x+1.0, pl.y-1.0, leftG.x, std);
			w[5]  = sgsrWeight(pl.x,     pl.y-1.0, leftG.y, std);
			w[6]  = sgsrWeight(pl.x,     pl.y,     leftG.z, std);
			w[7]  = sgsrWeight(pl.x+1.0, pl.y,     leftG.w, std);
			w[8]  = sgsrWeight(pl.x-1.0, pl.y-1.0, rightG.x, std);
			w[9]  = sgsrWeight(pl.x-2.0, pl.y-1.0, rightG.y, std);
			w[10] = sgsrWeight(pl.x-2.0, pl.y,     rightG.z, std);
			w[11] = sgsrWeight(pl.x-1.0, pl.y,     rightG.w, std);
			mediump float invSum = 1.0 / (((w[0]+w[1])+(w[2]+w[3]))+((w[4]+w[5])+(w[6]+w[7]))+((w[8]+w[9])+(w[10]+w[11])));

			// Green is centered on its mean, so its clamp window is centered too.
			color.g = meanG + sgsrChannel(w, upDownG, leftG, rightG, invSum);
			{
				mediump vec4 l = textureGather(tex,leftCoord, 0);
				mediump vec4 r = textureGather(tex,rightCoord, 0);
				mediump vec4 ud;
				ud.xy = textureGather(tex,upCoord, 0).wz;
				ud.zw = textureGather(tex,downCoord, 0).yx;
				color.r = sgsrChannel(w, ud, l, r, invSum);
			}
			{
				mediump vec4 l = textureGather(tex,leftCoord, 2);
				mediump vec4 r = textureGather(tex,rightCoord, 2);
				mediump vec4 ud;
				ud.xy = textureGather(tex,upCoord, 2).wz;
				ud.zw = textureGather(tex,downCoord, 2).yx;
				color.b = sgsrChannel(w, ud, l, r, invSum);
			}
		}
	}

	return color;
}
