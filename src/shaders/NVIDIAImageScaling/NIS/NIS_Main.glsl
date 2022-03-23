// The MIT License(MIT)
//
// Copyright(c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files(the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and / or sell copies of
// the Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_shader_16bit_storage : require
#extension GL_EXT_shader_explicit_arithmetic_types : require

#define NIS_GLSL 1

#ifndef NIS_SCALER
#define NIS_SCALER 1
#endif

layout(set=0,binding=0) uniform const_buffer
{
    float kDetectRatio;
    float kDetectThres;
    float kMinContrastRatio;
    float kRatioNorm;

    float kContrastBoost;
    float kEps;
    float kSharpStartY;
    float kSharpScaleY;

    float kSharpStrengthMin;
    float kSharpStrengthScale;
    float kSharpLimitMin;
    float kSharpLimitScale;

    float kScaleX;
    float kScaleY;

    float kDstNormX;
    float kDstNormY;
    float kSrcNormX;
    float kSrcNormY;

    uint kInputViewportOriginX;
    uint kInputViewportOriginY;
    uint kInputViewportWidth;
    uint kInputViewportHeight;

    uint kOutputViewportOriginX;
    uint kOutputViewportOriginY;
    uint kOutputViewportWidth;
    uint kOutputViewportHeight;

    float reserved0;
    float reserved1;
};

layout(set=0,binding=1) uniform sampler samplerLinearClamp;
layout(set=0,binding=2) uniform texture2D in_texture;
layout(set=0,binding=3,rgba8_snorm) uniform image2D out_texture;

#if NIS_SCALER
layout(set=0,binding=4) uniform texture2D coef_scaler;
layout(set=0,binding=5) uniform texture2D coef_usm;
#endif

#include "NIS_Scaler.h"

layout(local_size_x=NIS_THREAD_GROUP_SIZE) in;
void main()
{
    #if NIS_SCALER
        NVScaler(gl_WorkGroupID.xy, gl_LocalInvocationID.x);
    #else
        NVSharpen(gl_WorkGroupID.xy, gl_LocalInvocationID.x);
    #endif
}