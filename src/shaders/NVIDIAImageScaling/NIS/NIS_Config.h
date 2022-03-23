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

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#ifndef NIS_ALIGNED
#if defined(_MSC_VER)
#define NIS_ALIGNED(x) __declspec(align(x))
#else
#if defined(__GNUC__)
#define NIS_ALIGNED(x) __attribute__ ((aligned(x)))
#endif
#endif
#endif


struct NIS_ALIGNED(256) NISConfig
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

    uint32_t kInputViewportOriginX;
    uint32_t kInputViewportOriginY;
    uint32_t kInputViewportWidth;
    uint32_t kInputViewportHeight;

    uint32_t kOutputViewportOriginX;
    uint32_t kOutputViewportOriginY;
    uint32_t kOutputViewportWidth;
    uint32_t kOutputViewportHeight;

    float reserved0;
    float reserved1;
};

enum class NISHDRMode : uint32_t
{
    None = 0,
    Linear = 1,
    PQ = 2
};

enum class NISGPUArchitecture : uint32_t
{
    NVIDIA_Generic = 0,
    AMD_Generic = 1,
    Intel_Generic = 2,
    NVIDIA_Generic_fp16 = 3
};

struct NISOptimizer
{
    bool isUpscaling;
    NISGPUArchitecture gpuArch;

    constexpr NISOptimizer(bool isUpscaling = true, NISGPUArchitecture gpuArch = NISGPUArchitecture::NVIDIA_Generic)
        : isUpscaling(isUpscaling)
        , gpuArch(gpuArch)
    {}

    constexpr uint32_t GetOptimalBlockWidth()
    {
        switch (gpuArch) {
        case NISGPUArchitecture::NVIDIA_Generic:
            return 32;
        case NISGPUArchitecture::NVIDIA_Generic_fp16:
            return 32;
        case NISGPUArchitecture::AMD_Generic:
            return 32;
        case NISGPUArchitecture::Intel_Generic:
            return 32;
        }
        return 32;
    }

    constexpr uint32_t GetOptimalBlockHeight()
    {
        switch (gpuArch) {
        case NISGPUArchitecture::NVIDIA_Generic:
            return isUpscaling ? 24 : 32;
        case NISGPUArchitecture::NVIDIA_Generic_fp16:
            return isUpscaling ? 32 : 32;
        case NISGPUArchitecture::AMD_Generic:
            return isUpscaling ? 24 : 32;
        case NISGPUArchitecture::Intel_Generic:
            return isUpscaling ? 24 : 32;
        }
        return isUpscaling ? 24 : 32;
    }

    constexpr uint32_t GetOptimalThreadGroupSize()
    {
        switch (gpuArch) {
        case NISGPUArchitecture::NVIDIA_Generic:
            return 128;
        case NISGPUArchitecture::NVIDIA_Generic_fp16:
            return 128;
        case NISGPUArchitecture::AMD_Generic:
            return 256;
        case NISGPUArchitecture::Intel_Generic:
            return 256;
        }
        return 256;
    }
};


inline bool NVScalerUpdateConfig(NISConfig& config, float sharpness,
    uint32_t inputViewportOriginX, uint32_t inputViewportOriginY,
    uint32_t inputViewportWidth, uint32_t inputViewportHeight,
    uint32_t inputTextureWidth, uint32_t inputTextureHeight,
    uint32_t outputViewportOriginX, uint32_t outputViewportOriginY,
    uint32_t outputViewportWidth, uint32_t outputViewportHeight,
    uint32_t outputTextureWidth, uint32_t outputTextureHeight,
    NISHDRMode hdrMode = NISHDRMode::None)
{
    // adjust params based on value from sharpness slider
    sharpness = std::max<float>(std::min<float>(1.f, sharpness), 0.f);
    float sharpen_slider = sharpness - 0.5f;   // Map 0 to 1 to -0.5 to +0.5

    // Different range for 0 to 50% vs 50% to 100%
    // The idea is to make sure sharpness of 0% map to no-sharpening,
    // while also ensuring that sharpness of 100% doesn't cause too much over-sharpening.
    const float MaxScale = (sharpen_slider >= 0.0f) ? 1.25f : 1.75f;
    const float MinScale = (sharpen_slider >= 0.0f) ? 1.25f : 1.0f;
    const float LimitScale = (sharpen_slider >= 0.0f) ? 1.25f : 1.0f;

    float kDetectRatio = 2 * 1127.f / 1024.f;

    // Params for SDR
    float kDetectThres = 64.0f / 1024.0f;
    float kMinContrastRatio = 2.0f;
    float kMaxContrastRatio = 10.0f;

    float kSharpStartY = 0.45f;
    float kSharpEndY = 0.9f;
    float kSharpStrengthMin = std::max<float>(0.0f, 0.4f + sharpen_slider * MinScale * 1.2f);
    float kSharpStrengthMax = 1.6f + sharpen_slider * MaxScale * 1.8f;
    float kSharpLimitMin = std::max<float>(0.1f, 0.14f + sharpen_slider * LimitScale * 0.32f);
    float kSharpLimitMax = 0.5f + sharpen_slider * LimitScale * 0.6f;

    if (hdrMode == NISHDRMode::Linear || hdrMode == NISHDRMode::PQ)
    {
        kDetectThres = 32.0f / 1024.0f;

        kMinContrastRatio = 1.5f;
        kMaxContrastRatio = 5.0f;

        kSharpStrengthMin = std::max<float>(0.0f, 0.4f + sharpen_slider * MinScale * 1.1f);
        kSharpStrengthMax = 2.2f + sharpen_slider * MaxScale * 1.8f;
        kSharpLimitMin = std::max<float>(0.06f, 0.10f + sharpen_slider * LimitScale * 0.28f);
        kSharpLimitMax = 0.6f + sharpen_slider * LimitScale * 0.6f;

        if (hdrMode == NISHDRMode::PQ)
        {
            kSharpStartY = 0.35f;
            kSharpEndY = 0.55f;
        }
        else
        {
            kSharpStartY = 0.3f;
            kSharpEndY = 0.5f;
        }
    }

    float kRatioNorm = 1.0f / (kMaxContrastRatio - kMinContrastRatio);
    float kSharpScaleY = 1.0f / (kSharpEndY - kSharpStartY);
    float kSharpStrengthScale = kSharpStrengthMax - kSharpStrengthMin;
    float kSharpLimitScale = kSharpLimitMax - kSharpLimitMin;

    config.kInputViewportWidth = inputViewportWidth == 0 ? inputTextureWidth : inputViewportWidth;
    config.kInputViewportHeight = inputViewportHeight == 0 ? inputTextureHeight : inputViewportHeight;
    config.kOutputViewportWidth = outputViewportWidth == 0 ? outputTextureWidth : outputViewportWidth;
    config.kOutputViewportHeight = outputViewportHeight == 0 ? outputTextureHeight : outputViewportHeight;
    if (config.kInputViewportWidth == 0 || config.kInputViewportHeight == 0 ||
        config.kOutputViewportWidth == 0 || config.kOutputViewportHeight == 0)
        return false;

    config.kInputViewportOriginX = inputViewportOriginX;
    config.kInputViewportOriginY = inputViewportOriginY;
    config.kOutputViewportOriginX = outputViewportOriginX;
    config.kOutputViewportOriginY = outputViewportOriginY;

    config.kSrcNormX = 1.f / inputTextureWidth;
    config.kSrcNormY = 1.f / inputTextureHeight;
    config.kDstNormX = 1.f / outputTextureWidth;
    config.kDstNormY = 1.f / outputTextureHeight;
    config.kScaleX = config.kInputViewportWidth / float(config.kOutputViewportWidth);
    config.kScaleY = config.kInputViewportHeight / float(config.kOutputViewportHeight);
    config.kDetectRatio = kDetectRatio;
    config.kDetectThres = kDetectThres;
    config.kMinContrastRatio = kMinContrastRatio;
    config.kRatioNorm = kRatioNorm;
    config.kContrastBoost = 1.0f;
    config.kEps = 1.0f / 255.0f;
    config.kSharpStartY = kSharpStartY;
    config.kSharpScaleY = kSharpScaleY;
    config.kSharpStrengthMin = kSharpStrengthMin;
    config.kSharpStrengthScale = kSharpStrengthScale;
    config.kSharpLimitMin = kSharpLimitMin;
    config.kSharpLimitScale = kSharpLimitScale;

    if (config.kScaleX < 0.5f || config.kScaleX > 1.f || config.kScaleY < 0.5f || config.kScaleY > 1.f)
        return false;
    return true;
}


inline bool NVSharpenUpdateConfig(NISConfig& config, float sharpness,
    uint32_t inputViewportOriginX, uint32_t inputViewportOriginY,
    uint32_t inputViewportWidth, uint32_t inputViewportHeight,
    uint32_t inputTextureWidth, uint32_t inputTextureHeight,
    uint32_t outputViewportOriginX, uint32_t outputViewportOriginY,
    NISHDRMode hdrMode = NISHDRMode::None)
{
    return NVScalerUpdateConfig(config, sharpness,
            inputViewportOriginX, inputViewportOriginY, inputViewportWidth, inputViewportHeight, inputTextureWidth, inputTextureHeight,
            outputViewportOriginX, outputViewportOriginY, inputViewportWidth, inputViewportHeight, inputTextureWidth, inputTextureHeight,
            hdrMode);
}

namespace {
    constexpr size_t kPhaseCount = 64;
    constexpr size_t kFilterSize = 8;

    constexpr float coef_scale[kPhaseCount][kFilterSize] = {
        {0.0f,     0.0f,    1.0000f, 0.0f,     0.0f,    0.0f, 0.0f, 0.0f},
        {0.0029f, -0.0127f, 1.0000f, 0.0132f, -0.0034f, 0.0f, 0.0f, 0.0f},
        {0.0063f, -0.0249f, 0.9985f, 0.0269f, -0.0068f, 0.0f, 0.0f, 0.0f},
        {0.0088f, -0.0361f, 0.9956f, 0.0415f, -0.0103f, 0.0005f, 0.0f, 0.0f},
        {0.0117f, -0.0474f, 0.9932f, 0.0562f, -0.0142f, 0.0005f, 0.0f, 0.0f},
        {0.0142f, -0.0576f, 0.9897f, 0.0713f, -0.0181f, 0.0005f, 0.0f, 0.0f},
        {0.0166f, -0.0674f, 0.9844f, 0.0874f, -0.0220f, 0.0010f, 0.0f, 0.0f},
        {0.0186f, -0.0762f, 0.9785f, 0.1040f, -0.0264f, 0.0015f, 0.0f, 0.0f},
        {0.0205f, -0.0850f, 0.9727f, 0.1206f, -0.0308f, 0.0020f, 0.0f, 0.0f},
        {0.0225f, -0.0928f, 0.9648f, 0.1382f, -0.0352f, 0.0024f, 0.0f, 0.0f},
        {0.0239f, -0.1006f, 0.9575f, 0.1558f, -0.0396f, 0.0029f, 0.0f, 0.0f},
        {0.0254f, -0.1074f, 0.9487f, 0.1738f, -0.0439f, 0.0034f, 0.0f, 0.0f},
        {0.0264f, -0.1138f, 0.9390f, 0.1929f, -0.0488f, 0.0044f, 0.0f, 0.0f},
        {0.0278f, -0.1191f, 0.9282f, 0.2119f, -0.0537f, 0.0049f, 0.0f, 0.0f},
        {0.0288f, -0.1245f, 0.9170f, 0.2310f, -0.0581f, 0.0059f, 0.0f, 0.0f},
        {0.0293f, -0.1294f, 0.9058f, 0.2510f, -0.0630f, 0.0063f, 0.0f, 0.0f},
        {0.0303f, -0.1333f, 0.8926f, 0.2710f, -0.0679f, 0.0073f, 0.0f, 0.0f},
        {0.0308f, -0.1367f, 0.8789f, 0.2915f, -0.0728f, 0.0083f, 0.0f, 0.0f},
        {0.0308f, -0.1401f, 0.8657f, 0.3120f, -0.0776f, 0.0093f, 0.0f, 0.0f},
        {0.0313f, -0.1426f, 0.8506f, 0.3330f, -0.0825f, 0.0103f, 0.0f, 0.0f},
        {0.0313f, -0.1445f, 0.8354f, 0.3540f, -0.0874f, 0.0112f, 0.0f, 0.0f},
        {0.0313f, -0.1460f, 0.8193f, 0.3755f, -0.0923f, 0.0122f, 0.0f, 0.0f},
        {0.0313f, -0.1470f, 0.8022f, 0.3965f, -0.0967f, 0.0137f, 0.0f, 0.0f},
        {0.0308f, -0.1479f, 0.7856f, 0.4185f, -0.1016f, 0.0146f, 0.0f, 0.0f},
        {0.0303f, -0.1479f, 0.7681f, 0.4399f, -0.1060f, 0.0156f, 0.0f, 0.0f},
        {0.0298f, -0.1479f, 0.7505f, 0.4614f, -0.1104f, 0.0166f, 0.0f, 0.0f},
        {0.0293f, -0.1470f, 0.7314f, 0.4829f, -0.1147f, 0.0181f, 0.0f, 0.0f},
        {0.0288f, -0.1460f, 0.7119f, 0.5049f, -0.1187f, 0.0190f, 0.0f, 0.0f},
        {0.0278f, -0.1445f, 0.6929f, 0.5264f, -0.1226f, 0.0200f, 0.0f, 0.0f},
        {0.0273f, -0.1431f, 0.6724f, 0.5479f, -0.1260f, 0.0215f, 0.0f, 0.0f},
        {0.0264f, -0.1411f, 0.6528f, 0.5693f, -0.1299f, 0.0225f, 0.0f, 0.0f},
        {0.0254f, -0.1387f, 0.6323f, 0.5903f, -0.1328f, 0.0234f, 0.0f, 0.0f},
        {0.0244f, -0.1357f, 0.6113f, 0.6113f, -0.1357f, 0.0244f, 0.0f, 0.0f},
        {0.0234f, -0.1328f, 0.5903f, 0.6323f, -0.1387f, 0.0254f, 0.0f, 0.0f},
        {0.0225f, -0.1299f, 0.5693f, 0.6528f, -0.1411f, 0.0264f, 0.0f, 0.0f},
        {0.0215f, -0.1260f, 0.5479f, 0.6724f, -0.1431f, 0.0273f, 0.0f, 0.0f},
        {0.0200f, -0.1226f, 0.5264f, 0.6929f, -0.1445f, 0.0278f, 0.0f, 0.0f},
        {0.0190f, -0.1187f, 0.5049f, 0.7119f, -0.1460f, 0.0288f, 0.0f, 0.0f},
        {0.0181f, -0.1147f, 0.4829f, 0.7314f, -0.1470f, 0.0293f, 0.0f, 0.0f},
        {0.0166f, -0.1104f, 0.4614f, 0.7505f, -0.1479f, 0.0298f, 0.0f, 0.0f},
        {0.0156f, -0.1060f, 0.4399f, 0.7681f, -0.1479f, 0.0303f, 0.0f, 0.0f},
        {0.0146f, -0.1016f, 0.4185f, 0.7856f, -0.1479f, 0.0308f, 0.0f, 0.0f},
        {0.0137f, -0.0967f, 0.3965f, 0.8022f, -0.1470f, 0.0313f, 0.0f, 0.0f},
        {0.0122f, -0.0923f, 0.3755f, 0.8193f, -0.1460f, 0.0313f, 0.0f, 0.0f},
        {0.0112f, -0.0874f, 0.3540f, 0.8354f, -0.1445f, 0.0313f, 0.0f, 0.0f},
        {0.0103f, -0.0825f, 0.3330f, 0.8506f, -0.1426f, 0.0313f, 0.0f, 0.0f},
        {0.0093f, -0.0776f, 0.3120f, 0.8657f, -0.1401f, 0.0308f, 0.0f, 0.0f},
        {0.0083f, -0.0728f, 0.2915f, 0.8789f, -0.1367f, 0.0308f, 0.0f, 0.0f},
        {0.0073f, -0.0679f, 0.2710f, 0.8926f, -0.1333f, 0.0303f, 0.0f, 0.0f},
        {0.0063f, -0.0630f, 0.2510f, 0.9058f, -0.1294f, 0.0293f, 0.0f, 0.0f},
        {0.0059f, -0.0581f, 0.2310f, 0.9170f, -0.1245f, 0.0288f, 0.0f, 0.0f},
        {0.0049f, -0.0537f, 0.2119f, 0.9282f, -0.1191f, 0.0278f, 0.0f, 0.0f},
        {0.0044f, -0.0488f, 0.1929f, 0.9390f, -0.1138f, 0.0264f, 0.0f, 0.0f},
        {0.0034f, -0.0439f, 0.1738f, 0.9487f, -0.1074f, 0.0254f, 0.0f, 0.0f},
        {0.0029f, -0.0396f, 0.1558f, 0.9575f, -0.1006f, 0.0239f, 0.0f, 0.0f},
        {0.0024f, -0.0352f, 0.1382f, 0.9648f, -0.0928f, 0.0225f, 0.0f, 0.0f},
        {0.0020f, -0.0308f, 0.1206f, 0.9727f, -0.0850f, 0.0205f, 0.0f, 0.0f},
        {0.0015f, -0.0264f, 0.1040f, 0.9785f, -0.0762f, 0.0186f, 0.0f, 0.0f},
        {0.0010f, -0.0220f, 0.0874f, 0.9844f, -0.0674f, 0.0166f, 0.0f, 0.0f},
        {0.0005f, -0.0181f, 0.0713f, 0.9897f, -0.0576f, 0.0142f, 0.0f, 0.0f},
        {0.0005f, -0.0142f, 0.0562f, 0.9932f, -0.0474f, 0.0117f, 0.0f, 0.0f},
        {0.0005f, -0.0103f, 0.0415f, 0.9956f, -0.0361f, 0.0088f, 0.0f, 0.0f},
        {0.0f, -0.0068f, 0.0269f, 0.9985f, -0.0249f, 0.0063f, 0.0f, 0.0f},
        {0.0f, -0.0034f, 0.0132f, 1.0000f, -0.0127f, 0.0029f, 0.0f, 0.0f}
    };

    constexpr float coef_usm[kPhaseCount][kFilterSize] = {
        {0.0f,      -0.6001f, 1.2002f, -0.6001f,  0.0f,  0.0f, 0.0f, 0.0f},
        {0.0029f, -0.6084f, 1.1987f, -0.5903f, -0.0029f, 0.0f, 0.0f, 0.0f},
        {0.0049f, -0.6147f, 1.1958f, -0.5791f, -0.0068f, 0.0005f, 0.0f, 0.0f},
        {0.0073f, -0.6196f, 1.1890f, -0.5659f, -0.0103f, 0.0f, 0.0f, 0.0f},
        {0.0093f, -0.6235f, 1.1802f, -0.5513f, -0.0151f, 0.0f, 0.0f, 0.0f},
        {0.0112f, -0.6265f, 1.1699f, -0.5352f, -0.0195f, 0.0005f, 0.0f, 0.0f},
        {0.0122f, -0.6270f, 1.1582f, -0.5181f, -0.0259f, 0.0005f, 0.0f, 0.0f},
        {0.0142f, -0.6284f, 1.1455f, -0.5005f, -0.0317f, 0.0005f, 0.0f, 0.0f},
        {0.0156f, -0.6265f, 1.1274f, -0.4790f, -0.0386f, 0.0005f, 0.0f, 0.0f},
        {0.0166f, -0.6235f, 1.1089f, -0.4570f, -0.0454f, 0.0010f, 0.0f, 0.0f},
        {0.0176f, -0.6187f, 1.0879f, -0.4346f, -0.0532f, 0.0010f, 0.0f, 0.0f},
        {0.0181f, -0.6138f, 1.0659f, -0.4102f, -0.0615f, 0.0015f, 0.0f, 0.0f},
        {0.0190f, -0.6069f, 1.0405f, -0.3843f, -0.0698f, 0.0015f, 0.0f, 0.0f},
        {0.0195f, -0.6006f, 1.0161f, -0.3574f, -0.0796f, 0.0020f, 0.0f, 0.0f},
        {0.0200f, -0.5928f, 0.9893f, -0.3286f, -0.0898f, 0.0024f, 0.0f, 0.0f},
        {0.0200f, -0.5820f, 0.9580f, -0.2988f, -0.1001f, 0.0029f, 0.0f, 0.0f},
        {0.0200f, -0.5728f, 0.9292f, -0.2690f, -0.1104f, 0.0034f, 0.0f, 0.0f},
        {0.0200f, -0.5620f, 0.8975f, -0.2368f, -0.1226f, 0.0039f, 0.0f, 0.0f},
        {0.0205f, -0.5498f, 0.8643f, -0.2046f, -0.1343f, 0.0044f, 0.0f, 0.0f},
        {0.0200f, -0.5371f, 0.8301f, -0.1709f, -0.1465f, 0.0049f, 0.0f, 0.0f},
        {0.0195f, -0.5239f, 0.7944f, -0.1367f, -0.1587f, 0.0054f, 0.0f, 0.0f},
        {0.0195f, -0.5107f, 0.7598f, -0.1021f, -0.1724f, 0.0059f, 0.0f, 0.0f},
        {0.0190f, -0.4966f, 0.7231f, -0.0649f, -0.1865f, 0.0063f, 0.0f, 0.0f},
        {0.0186f, -0.4819f, 0.6846f, -0.0288f, -0.1997f, 0.0068f, 0.0f, 0.0f},
        {0.0186f, -0.4668f, 0.6460f, 0.0093f, -0.2144f, 0.0073f, 0.0f, 0.0f},
        {0.0176f, -0.4507f, 0.6055f, 0.0479f, -0.2290f, 0.0083f, 0.0f, 0.0f},
        {0.0171f, -0.4370f, 0.5693f, 0.0859f, -0.2446f, 0.0088f, 0.0f, 0.0f},
        {0.0161f, -0.4199f, 0.5283f, 0.1255f, -0.2598f, 0.0098f, 0.0f, 0.0f},
        {0.0161f, -0.4048f, 0.4883f, 0.1655f, -0.2754f, 0.0103f, 0.0f, 0.0f},
        {0.0151f, -0.3887f, 0.4497f, 0.2041f, -0.2910f, 0.0107f, 0.0f, 0.0f},
        {0.0142f, -0.3711f, 0.4072f, 0.2446f, -0.3066f, 0.0117f, 0.0f, 0.0f},
        {0.0137f, -0.3555f, 0.3672f, 0.2852f, -0.3228f, 0.0122f, 0.0f, 0.0f},
        {0.0132f, -0.3394f, 0.3262f, 0.3262f, -0.3394f, 0.0132f, 0.0f, 0.0f},
        {0.0122f, -0.3228f, 0.2852f, 0.3672f, -0.3555f, 0.0137f, 0.0f, 0.0f},
        {0.0117f, -0.3066f, 0.2446f, 0.4072f, -0.3711f, 0.0142f, 0.0f, 0.0f},
        {0.0107f, -0.2910f, 0.2041f, 0.4497f, -0.3887f, 0.0151f, 0.0f, 0.0f},
        {0.0103f, -0.2754f, 0.1655f, 0.4883f, -0.4048f, 0.0161f, 0.0f, 0.0f},
        {0.0098f, -0.2598f, 0.1255f, 0.5283f, -0.4199f, 0.0161f, 0.0f, 0.0f},
        {0.0088f, -0.2446f, 0.0859f, 0.5693f, -0.4370f, 0.0171f, 0.0f, 0.0f},
        {0.0083f, -0.2290f, 0.0479f, 0.6055f, -0.4507f, 0.0176f, 0.0f, 0.0f},
        {0.0073f, -0.2144f, 0.0093f, 0.6460f, -0.4668f, 0.0186f, 0.0f, 0.0f},
        {0.0068f, -0.1997f, -0.0288f, 0.6846f, -0.4819f, 0.0186f, 0.0f, 0.0f},
        {0.0063f, -0.1865f, -0.0649f, 0.7231f, -0.4966f, 0.0190f, 0.0f, 0.0f},
        {0.0059f, -0.1724f, -0.1021f, 0.7598f, -0.5107f, 0.0195f, 0.0f, 0.0f},
        {0.0054f, -0.1587f, -0.1367f, 0.7944f, -0.5239f, 0.0195f, 0.0f, 0.0f},
        {0.0049f, -0.1465f, -0.1709f, 0.8301f, -0.5371f, 0.0200f, 0.0f, 0.0f},
        {0.0044f, -0.1343f, -0.2046f, 0.8643f, -0.5498f, 0.0205f, 0.0f, 0.0f},
        {0.0039f, -0.1226f, -0.2368f, 0.8975f, -0.5620f, 0.0200f, 0.0f, 0.0f},
        {0.0034f, -0.1104f, -0.2690f, 0.9292f, -0.5728f, 0.0200f, 0.0f, 0.0f},
        {0.0029f, -0.1001f, -0.2988f, 0.9580f, -0.5820f, 0.0200f, 0.0f, 0.0f},
        {0.0024f, -0.0898f, -0.3286f, 0.9893f, -0.5928f, 0.0200f, 0.0f, 0.0f},
        {0.0020f, -0.0796f, -0.3574f, 1.0161f, -0.6006f, 0.0195f, 0.0f, 0.0f},
        {0.0015f, -0.0698f, -0.3843f, 1.0405f, -0.6069f, 0.0190f, 0.0f, 0.0f},
        {0.0015f, -0.0615f, -0.4102f, 1.0659f, -0.6138f, 0.0181f, 0.0f, 0.0f},
        {0.0010f, -0.0532f, -0.4346f, 1.0879f, -0.6187f, 0.0176f, 0.0f, 0.0f},
        {0.0010f, -0.0454f, -0.4570f, 1.1089f, -0.6235f, 0.0166f, 0.0f, 0.0f},
        {0.0005f, -0.0386f, -0.4790f, 1.1274f, -0.6265f, 0.0156f, 0.0f, 0.0f},
        {0.0005f, -0.0317f, -0.5005f, 1.1455f, -0.6284f, 0.0142f, 0.0f, 0.0f},
        {0.0005f, -0.0259f, -0.5181f, 1.1582f, -0.6270f, 0.0122f, 0.0f, 0.0f},
        {0.0005f, -0.0195f, -0.5352f, 1.1699f, -0.6265f, 0.0112f, 0.0f, 0.0f},
        {0.0f, -0.0151f, -0.5513f, 1.1802f, -0.6235f, 0.0093f, 0.0f, 0.0f},
        {0.0f, -0.0103f, -0.5659f, 1.1890f, -0.6196f, 0.0073f, 0.0f, 0.0f},
        {0.0005f, -0.0068f, -0.5791f, 1.1958f, -0.6147f, 0.0049f, 0.0f, 0.0f},
        {0.0f, -0.0029f, -0.5903f, 1.1987f, -0.6084f, 0.0029f, 0.0f, 0.0f}
    };

    constexpr uint16_t coef_scale_fp16[kPhaseCount][kFilterSize] = {
       { 0, 0, 15360, 0, 0, 0, 0, 0 },
       { 6640, 41601, 15360, 8898, 39671, 0, 0, 0 },
       { 7796, 42592, 15357, 9955, 40695, 0, 0, 0 },
       { 8321, 43167, 15351, 10576, 41286, 4121, 0, 0 },
       { 8702, 43537, 15346, 11058, 41797, 4121, 0, 0 },
       { 9029, 43871, 15339, 11408, 42146, 4121, 0, 0 },
       { 9280, 44112, 15328, 11672, 42402, 5145, 0, 0 },
       { 9411, 44256, 15316, 11944, 42690, 5669, 0, 0 },
       { 9535, 44401, 15304, 12216, 42979, 6169, 0, 0 },
       { 9667, 44528, 15288, 12396, 43137, 6378, 0, 0 },
       { 9758, 44656, 15273, 12540, 43282, 6640, 0, 0 },
       { 9857, 44768, 15255, 12688, 43423, 6903, 0, 0 },
       { 9922, 44872, 15235, 12844, 43583, 7297, 0, 0 },
       { 10014, 44959, 15213, 13000, 43744, 7429, 0, 0 },
       { 10079, 45048, 15190, 13156, 43888, 7691, 0, 0 },
       { 10112, 45092, 15167, 13316, 44040, 7796, 0, 0 },
       { 10178, 45124, 15140, 13398, 44120, 8058, 0, 0 },
       { 10211, 45152, 15112, 13482, 44201, 8256, 0, 0 },
       { 10211, 45180, 15085, 13566, 44279, 8387, 0, 0 },
       { 10242, 45200, 15054, 13652, 44360, 8518, 0, 0 },
       { 10242, 45216, 15023, 13738, 44440, 8636, 0, 0 },
       { 10242, 45228, 14990, 13826, 44520, 8767, 0, 0 },
       { 10242, 45236, 14955, 13912, 44592, 8964, 0, 0 },
       { 10211, 45244, 14921, 14002, 44673, 9082, 0, 0 },
       { 10178, 45244, 14885, 14090, 44745, 9213, 0, 0 },
       { 10145, 45244, 14849, 14178, 44817, 9280, 0, 0 },
       { 10112, 45236, 14810, 14266, 44887, 9378, 0, 0 },
       { 10079, 45228, 14770, 14346, 44953, 9437, 0, 0 },
       { 10014, 45216, 14731, 14390, 45017, 9503, 0, 0 },
       { 9981, 45204, 14689, 14434, 45064, 9601, 0, 0 },
       { 9922, 45188, 14649, 14478, 45096, 9667, 0, 0 },
       { 9857, 45168, 14607, 14521, 45120, 9726, 0, 0 },
       { 9791, 45144, 14564, 14564, 45144, 9791, 0, 0 },
       { 9726, 45120, 14521, 14607, 45168, 9857, 0, 0 },
       { 9667, 45096, 14478, 14649, 45188, 9922, 0, 0 },
       { 9601, 45064, 14434, 14689, 45204, 9981, 0, 0 },
       { 9503, 45017, 14390, 14731, 45216, 10014, 0, 0 },
       { 9437, 44953, 14346, 14770, 45228, 10079, 0, 0 },
       { 9378, 44887, 14266, 14810, 45236, 10112, 0, 0 },
       { 9280, 44817, 14178, 14849, 45244, 10145, 0, 0 },
       { 9213, 44745, 14090, 14885, 45244, 10178, 0, 0 },
       { 9082, 44673, 14002, 14921, 45244, 10211, 0, 0 },
       { 8964, 44592, 13912, 14955, 45236, 10242, 0, 0 },
       { 8767, 44520, 13826, 14990, 45228, 10242, 0, 0 },
       { 8636, 44440, 13738, 15023, 45216, 10242, 0, 0 },
       { 8518, 44360, 13652, 15054, 45200, 10242, 0, 0 },
       { 8387, 44279, 13566, 15085, 45180, 10211, 0, 0 },
       { 8256, 44201, 13482, 15112, 45152, 10211, 0, 0 },
       { 8058, 44120, 13398, 15140, 45124, 10178, 0, 0 },
       { 7796, 44040, 13316, 15167, 45092, 10112, 0, 0 },
       { 7691, 43888, 13156, 15190, 45048, 10079, 0, 0 },
       { 7429, 43744, 13000, 15213, 44959, 10014, 0, 0 },
       { 7297, 43583, 12844, 15235, 44872, 9922, 0, 0 },
       { 6903, 43423, 12688, 15255, 44768, 9857, 0, 0 },
       { 6640, 43282, 12540, 15273, 44656, 9758, 0, 0 },
       { 6378, 43137, 12396, 15288, 44528, 9667, 0, 0 },
       { 6169, 42979, 12216, 15304, 44401, 9535, 0, 0 },
       { 5669, 42690, 11944, 15316, 44256, 9411, 0, 0 },
       { 5145, 42402, 11672, 15328, 44112, 9280, 0, 0 },
       { 4121, 42146, 11408, 15339, 43871, 9029, 0, 0 },
       { 4121, 41797, 11058, 15346, 43537, 8702, 0, 0 },
       { 4121, 41286, 10576, 15351, 43167, 8321, 0, 0 },
       { 0, 40695, 9955, 15357, 42592, 7796, 0, 0 },
       { 0, 39671, 8898, 15360, 41601, 6640, 0, 0 },
    };

    constexpr uint16_t coef_usm_fp16[kPhaseCount][kFilterSize] = {
        { 0, 47309, 15565, 47309, 0, 0, 0, 0 },
        { 6640, 47326, 15563, 47289, 39408, 0, 0, 0 },
        { 7429, 47339, 15560, 47266, 40695, 4121, 0, 0 },
        { 8058, 47349, 15554, 47239, 41286, 0, 0, 0 },
        { 8387, 47357, 15545, 47209, 41915, 0, 0, 0 },
        { 8636, 47363, 15534, 47176, 42238, 4121, 0, 0 },
        { 8767, 47364, 15522, 47141, 42657, 4121, 0, 0 },
        { 9029, 47367, 15509, 47105, 43023, 4121, 0, 0 },
        { 9213, 47363, 15490, 47018, 43249, 4121, 0, 0 },
        { 9280, 47357, 15472, 46928, 43472, 5145, 0, 0 },
        { 9345, 47347, 15450, 46836, 43727, 5145, 0, 0 },
        { 9378, 47337, 15427, 46736, 43999, 5669, 0, 0 },
        { 9437, 47323, 15401, 46630, 44152, 5669, 0, 0 },
        { 9470, 47310, 15376, 46520, 44312, 6169, 0, 0 },
        { 9503, 47294, 15338, 46402, 44479, 6378, 0, 0 },
        { 9503, 47272, 15274, 46280, 44648, 6640, 0, 0 },
        { 9503, 47253, 15215, 46158, 44817, 6903, 0, 0 },
        { 9503, 47231, 15150, 45972, 45017, 7165, 0, 0 },
        { 9535, 47206, 15082, 45708, 45132, 7297, 0, 0 },
        { 9503, 47180, 15012, 45432, 45232, 7429, 0, 0 },
        { 9470, 47153, 14939, 45152, 45332, 7560, 0, 0 },
        { 9470, 47126, 14868, 44681, 45444, 7691, 0, 0 },
        { 9437, 47090, 14793, 44071, 45560, 7796, 0, 0 },
        { 9411, 47030, 14714, 42847, 45668, 7927, 0, 0 },
        { 9411, 46968, 14635, 8387, 45788, 8058, 0, 0 },
        { 9345, 46902, 14552, 10786, 45908, 8256, 0, 0 },
        { 9313, 46846, 14478, 11647, 46036, 8321, 0, 0 },
        { 9247, 46776, 14394, 12292, 46120, 8453, 0, 0 },
        { 9247, 46714, 14288, 12620, 46184, 8518, 0, 0 },
        { 9147, 46648, 14130, 12936, 46248, 8570, 0, 0 },
        { 9029, 46576, 13956, 13268, 46312, 8702, 0, 0 },
        { 8964, 46512, 13792, 13456, 46378, 8767, 0, 0 },
        { 8898, 46446, 13624, 13624, 46446, 8898, 0, 0 },
        { 8767, 46378, 13456, 13792, 46512, 8964, 0, 0 },
        { 8702, 46312, 13268, 13956, 46576, 9029, 0, 0 },
        { 8570, 46248, 12936, 14130, 46648, 9147, 0, 0 },
        { 8518, 46184, 12620, 14288, 46714, 9247, 0, 0 },
        { 8453, 46120, 12292, 14394, 46776, 9247, 0, 0 },
        { 8321, 46036, 11647, 14478, 46846, 9313, 0, 0 },
        { 8256, 45908, 10786, 14552, 46902, 9345, 0, 0 },
        { 8058, 45788, 8387, 14635, 46968, 9411, 0, 0 },
        { 7927, 45668, 42847, 14714, 47030, 9411, 0, 0 },
        { 7796, 45560, 44071, 14793, 47090, 9437, 0, 0 },
        { 7691, 45444, 44681, 14868, 47126, 9470, 0, 0 },
        { 7560, 45332, 45152, 14939, 47153, 9470, 0, 0 },
        { 7429, 45232, 45432, 15012, 47180, 9503, 0, 0 },
        { 7297, 45132, 45708, 15082, 47206, 9535, 0, 0 },
        { 7165, 45017, 45972, 15150, 47231, 9503, 0, 0 },
        { 6903, 44817, 46158, 15215, 47253, 9503, 0, 0 },
        { 6640, 44648, 46280, 15274, 47272, 9503, 0, 0 },
        { 6378, 44479, 46402, 15338, 47294, 9503, 0, 0 },
        { 6169, 44312, 46520, 15376, 47310, 9470, 0, 0 },
        { 5669, 44152, 46630, 15401, 47323, 9437, 0, 0 },
        { 5669, 43999, 46736, 15427, 47337, 9378, 0, 0 },
        { 5145, 43727, 46836, 15450, 47347, 9345, 0, 0 },
        { 5145, 43472, 46928, 15472, 47357, 9280, 0, 0 },
        { 4121, 43249, 47018, 15490, 47363, 9213, 0, 0 },
        { 4121, 43023, 47105, 15509, 47367, 9029, 0, 0 },
        { 4121, 42657, 47141, 15522, 47364, 8767, 0, 0 },
        { 4121, 42238, 47176, 15534, 47363, 8636, 0, 0 },
        { 0, 41915, 47209, 15545, 47357, 8387, 0, 0 },
        { 0, 41286, 47239, 15554, 47349, 8058, 0, 0 },
        { 4121, 40695, 47266, 15560, 47339, 7429, 0, 0 },
        { 0, 39408, 47289, 15563, 47326, 6640, 0, 0 },
    };
}