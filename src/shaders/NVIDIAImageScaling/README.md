# NVIDIA Image Scaling SDK v1.0.2

The MIT License(MIT)

Copyright(c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files(the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and / or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions :

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.



## Introduction

The NVIDIA Image Scaling SDK provides a single spatial scaling and sharpening algorithm
for cross-platform support. The scaling algorithm uses a 6-tap scaling filter combined
with 4 directional scaling and adaptive sharpening filters, which creates nice smooth images
and sharp edges. In addition, the SDK provides a state-of-the-art adaptive directional sharpening algorithm for use in
applications where no scaling is required.\
The directional scaling and sharpening algorithm is named NVScaler while the adaptive-directional-sharpening-only
algorithm is named NVSharpen. Both algorithms are provided as compute shaders and
developers are free to integrate them in their applications. Note that if you integrate NVScaler, you
should NOT integrate NVSharpen, as NVScaler already includes a sharpening pass



## Pipeline Placement

The call into the NVIDIA Image Scaling shaders must occur during the post-processing phase after tone-mapping.
Applying the scaling in linear HDR in-game color-space may result in a sharpening effect that is
either not visible or too strong. Since sharpening algorithms can enhance noisy or grainy regions, it is recommended
that certain effects such as film grain should occur after NVScaler or NVSharpen. Low-pass filters such as motion blur or
light bloom are recommended to be applied before NVScaler or NVSharpen to avoid sharpening attenuation.



## Color Space and Ranges

NVIDIA Image Scaling shaders can process color textures stored as either LDR or HDR with the following
restrictions:

1) LDR
   - The range of color values must be in the [0, 1] range
   - The input color texture must be in display-referred color-space after tone mapping and OETF (gamma-correction)
     has been applied
2) HDR PQ
   - The range of color values must be in the [0, 1] range
   - The input color texture must be in display-referred color-space after tone mapping with Rec.2020 PQ OETF applied
3) HDR Linear
   - The recommended range of color values is [0, 12.5], where luminance value (as per BT. 709) of
     1.0 maps to brightness value of 80nits (sRGB peak) and 12.5 maps to 1000nits
   - The input color texture may have luminance values that are either linear and scene-referred or
     linear and display-referred (after tone mapping)

If the input color texture sent to NVScaler/NVSharpen is in HDR format set NIS_HDR_MODE define to either NIS_HDR_MODE_LINEAR (1) or NIS_HDR_MODE_PQ (2).



## Supported Texture Formats

### Input and output formats:

Input and output formats are expected to be in the rages defined in previous section and should be
specified using non-integer data types such as DXGI_FORMAT_R8G8B8A8_UNORM.

### Coefficients formats:

The scaler coefficients and USM coefficients format should be specified using float4 type such as
DXGI_FORMAT_R32G32B32A32_FLOAT or DXGI_FORMAT_R16G16B16A16_FLOAT.
The coefficients are included in NIS_Config.h file:

fp32 format: coef_scaler, coef_USM

fp16 format: coef_scaler_fp16, coef_USM_fp16


### Resource States, Buffers, and Sampler:

The game or application calling NVIDIA Image Scaling SDK shaders must ensure that the textures are in
the correct state.

- Input color textures must be in pixel shader read state. Shader Resource View (SRV) in DirectX
- The output texture must be in read/write state. Unordered Access View (UAV) in DirectX
- The coefficients texture for NVScaler must be in read state. Shader Resource View (SRV) in DirectX
- The configuration variables must be passed as constant buffer. Constant Buffer View (CBV) in DirectX
- The sampler for texture pixel sampling must use linear filter interpolation and clamp to edge addressing mode



## Adding NVIDIA Image Scaling SDK to a Project

Include NIS_Scaler.h directly in your application or alternative use the provided NIS_Main.hlsl or NIS_Main.glsl shader files.
Use NIS_Config.h to get the ideal shader dispatch values for your platform, to configure the algorithm constant
values (NVScalerUpdateConfig, and NVSharpenUpdateConfig), and to access the algorithm coefficients (coef_scale, coef_USM, coef_scale_fp16, coef_USM_fp16).

- Device\
  NIS_Scaler.h    : HLSL shader file\
  NIS_Main.hlsl   : Main HLSL shader example (can be replaced by your own) \
  NIS_Main.glsl   : Main GLSL shader example (can ge replaced by your own)

- Host Configuration\
  NIS_Config.h    : Configuration structure


### Defines:

**NIS_SCALER**: default (**1**) NVScaler, (0) fast NVSharpen only, no upscaling\
**NIS_HDR_MODE**: default(**0**) disabled, (1) Linear, (2) PQ\
**NIS_BLOCK_WIDTH**: pixels per block width. Use GetOptimalBlockWidth query for your platform\
**NIS_BLOCK_HEIGHT**: pixels per block height. Use GetOptimalBlockHeight query for your platform\
**NIS_THREAD_GROUP_SIZE**: number of threads per group. Use GetOptimalThreadGroupSize query for your platform\
**NIS_USE_HALF_PRECISION**: default(**0**) disabled, (1) enable half pression computation\
**NIS_HLSL**: default (**1**) enabled, (0) disabled\
**NIS_HLSL_6_2**: default (**0**) HLSL v5, (1) HLSL v6.2 forces NIS_HLSL=1\
**NIS_GLSL**: default (**0**) disabled, (1) enabled\
**NIS_VIEWPORT_SUPPORT**: default(**0**) disabled, (1) enable input/output viewport support\


*Default NVScaler shader constants:*

[**NIS_BLOCK_WIDTH**, **NIS_BLOCK_HEIGHT**, **NIS_THREAD_GROUP_SIZE**] = [32, 24, 256]

*Default NVSharpen shader constants:*

[**NIS_BLOCK_WIDTH**, **NIS_BLOCK_HEIGHT**, **NIS_THREAD_GROUP_SIZE**] = [32, 32, 256]


*Defines for HLSL with DXC bindings:*

**NIS_DXC**: (0) disabled, (1) enable HLSL DXC Vulkan support

## Optimal shader settings

To get optimal performance of NVScaler and NVSharpen for current and future hardware, it is recommended that the following API is used to obtain the values for NIS_BLOCK_WIDTH, NIS_BLOCK_HEIGHT, and NIS_THREAD_GROUP_SIZE. These values can be used to compile permutations of NVScaler and NVSharpen offline.

```
enum class NISGPUArchitecture : uint32_t
{
    NVIDIA_Generic = 0,
    AMD_Generic = 1,
    Intel_Generic = 2,
    NVIDIA_Generic_fp16 = 3,
};
```

```
struct NISOptimizer
{
    bool isUpscaling;
    NISGPUArchitecture gpuArch;

    NISOptimizer(bool isUpscaling = true,
                 NISGPUArchitecture gpuArch = NISGPUArchitecture::NVIDIA_Generic);
    uint32_t GetOptimalBlockWidth();
    uint32_t GetOptimalBlockHeight();
    uint32_t GetOptimalThreadGroupSize();
};
```



## HDR shader settings

Use the following enum values for setting NIS_HDR_MODE

```
enum class NISHDRMode : uint32_t
{
    None = 0,
    Linear = 1,
    PQ = 2
};
```



## Integration of NVScaler
The integration instructions in this section can be applied with minimal changes to your own DX11, DX12, or Vulkan application, using HLSL or GLSL.

### Compile the NIS_Main.hlsl shader

NIS_SCALER should be set to 1, and the isUpscaling argument should set to true.

```
bool isUpscaling = true;
// Note: NISOptimizer is optional and these values can be cached offline
NISOptimizer opt(isUpscaling, NISGPUArchitecture::NVIDIA_Generic);
uint32_t blockWidth = opt.GetOptimalBlockWidth();
uint32_t blockHeight = opt.GetOptimalBlockHeight();
uint32_t threadGroupSize = opt.GetOptimalThreadGroupSize();

Defines defines;
defines.add("NIS_SCALER", isUpscaling);
defines.add("NIS_HDR_MODE", hdrMode);
defines.add("NIS_BLOCK_WIDTH", blockWidth);
defines.add("NIS_BLOCK_HEIGHT", blockHeight);
defines.add("NIS_THREAD_GROUP_SIZE", threadGroupSize);
NVScalerCS = CompileComputeShader(device, "NIS_Main.hlsl”, &defines);
```

### Create NVIDIA Image Scaling SDK configuration constant buffer

```
struct NISConfig
{
    float kDetectRatio;
    float kDetectThres;
    float kMinContrastRatio;
    float kRatioNorm;
    ...
};

NISConfig config;
createConstBuffer(&config, &csBuffer);
```

### Create SRV textures for the scaler and USM phase coefficients

```
const int rowPitch = kFilterSize * sizeof(float);  // use for fp32: float, fp16: uint16_t
const int coeffSize = rowPitch * kPhaseCount;

// Since we are using RGBA format the texture width = kFilterSize / 4
createTexture2D(kFilterSize / 4, kPhaseCount, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_USAGE_DEFAULT, coef_scaler, rowPitch, coeffSize, &scalerTex);
createTexture2D(kFilterSize / 4, kPhaseCount, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_USAGE_DEFAULT, coef_usm, rowPitch, coeffSize, &usmTex);

createSRV(scalerTex.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, &scalerSRV);
createSRV(usmTex.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, &usmSRV);
```

### Create Sampler

```
createLinearClampSampler(&linearClampSampler);
```

### Update NVIDIA Image Scaling SDK NVScaler configuration and constant buffer

Use the following API call to update the NVIDIA Image Scaling SDK configuration

```
void NVScalerUpdateConfig(NISConfig& config,
    float sharpness,
    uint32_t inputViewportOriginX, uint32_t inputViewportOriginY,
    uint32_t inputViewportWidth, uint32_t inputViewportHeight,
    uint32_t inputTextureWidth, uint32_t inputTextureHeight,
    uint32_t outputViewportOriginX, uint32_t outputViewportOriginY,
    uint32_t outputViewportWidth, uint32_t outputViewportHeight,
    uint32_t outputTextureWidth, uint32_t outputTextureHeight,
    NISHDRMode hdrMode = NISHDRMode::None
);
```

Update the constant buffer whenever the input size, sharpness, or scale changes

```
NVScalerUpdateConfig(m_config, sharpness,
                0, 0, inputWidth, inputHeight, inputWidth, inputHeight,
                0, 0, outputWidth, outputHeight, outputWidth, outputHeight,
                NISHDRMode::None);

updateConstBuffer(&config, csBuffer.Get());
```

### A simple DX11 NVScaler dispatch example

```
context->CSSetShaderResources(0, 1, input); // SRV
context->CSSetShaderResource (1, 1, scalerSRV.GetAddressOf());
context->CSSetShaderResource (2, 1, usmSRV.GetAddressOf());
context->CSSetUnorderedAccessViews(0, 1, output, nullptr);
context->CSSetSamplers(0, 1, linearClampSampler.GetAddressOf());
context->CSSetConstantBuffers(0, 1, csBuffer.GetAddressOf());
context->CSSetShader(NVScalerCS.Get(), nullptr, 0);

context->Dispatch(UINT(std::ceil(outputWidth / float(blockWidth))),
                  UINT(std::ceil(outputHeight / float(blockHeight))), 1);
```



## Integration of NVSharpen

If your application requires upscaling and sharpening do not use NVSharpen use NVScaler instead. Since NVScaler performs both operations, upscaling and sharpening, in one step, it performs faster and produces better image quality.

### Compile the NIS_Main.hlsl shader

NIS_SCALER should be set to 0 and the optimizer isUpscaling argument should be set to false.

```
bool isUpscaling = false;
// Note: NISOptimizer is optional and these values can be cached offline
NISOptimizer opt(isUpscaling, NISGPUArchitecture::NVIDIA_Generic);
uint32_t blockWidth = opt.GetOptimalBlockWidth();
uint32_t blockHeight = opt.GetOptimalBlockHeight();
uint32_t threadGroupSize = opt.GetOptimalThreadGroupSize();

Defines defines;
defines.add("NIS_DIRSCALER", isUpscaling);
defines.add("NIS_HDR_MODE", hdrMode);
defines.add("NIS_BLOCK_WIDTH", blockWidth);
defines.add("NIS_BLOCK_HEIGHT", blockHeight);
defines.add("NIS_THREAD_GROUP_SIZE", threadGroupSize);
NVSharpenCS = CompileComputeShader(device, "NIS_Main.hlsl”, &defines);
```

### Create NVIDIA Image Scaling SDK NVSharpen configuration constant buffer

```
struct NISConfig
{
    float kDetectRatio;
    float kDetectThres;
    float kMinContrastRatio;
    float kRatioNorm;
    ...
};

NISConfig config;
createConstBuffer(&config, &csBuffer);
```

### Create Sampler

```
createLinearClampSampler(&linearClampSampler);
```

### Update NVIDIA Image Scaling SDK NVSharpen configuration and constant buffer

Use the following API call to update the NVIDIA Image Scaling SDK configuration. Since NVSharpen is a sharpening algorithm only the sharpness and input size are required. For upscaling with sharpening use NVScaler since it performs both operations at the same time.

```
void NVSharpenUpdateConfig(NISConfig& config, float sharpness,
    uint32_t inputViewportOriginX, uint32_t inputViewportOriginY,
    uint32_t inputViewportWidth, uint32_t inputViewportHeight,
    uint32_t inputTextureWidth, uint32_t inputTextureHeight,
    uint32_t outputViewportOriginX, uint32_t outputViewportOriginY,
    NISHDRMode hdrMode = NISHDRMode::None
);

```

Update the constant buffer whenever the input size or sharpness changes.

```
NVSharpenUpdateConfig(m_config, sharpness,
                      0, 0, inputWidth, inputHeight, inputWidth, inputHeight,
                      0, 0, NISHDRMode::None);

updateConstBuffer(&config, csBuffer.Get());
```

### A simple DX11 NVSharpen dispatch example

```
context->CSSetShaderResources(0, 1, input);
context->CSSetUnorderedAccessViews(0, 1, output, nullptr);
context->CSSetSamplers(0, 1, linearClampSampler.GetAddressOf());
context->CSSetConstantBuffers(0, 1, csBuffer.GetAddressOf());
context->CSSetShader(NVSharpenCS.Get(), nullptr, 0);

context->Dispatch(UINT(std::ceil(outputWidth / float(blockWidth))),
                  UINT(std::ceil(outputHeight / float(blockHeight))), 1);
```



## Samples

### Dependencies

- Visual Studio 2019 : https://visualstudio.microsoft.com/downloads/
- Windows 10 SDK : https://developer.microsoft.com/en-us/windows/downloads/windows-10-sdk
- CMake 3.16 : https://cmake.org/download/

for building the Vulkan sample:
- Vulkan SDK 1.2.189.2 : https://vulkan.lunarg.com/

### Build

For the DirectX11 and DirectX12 samples use the following:

```
$> cd samples
$> mkdir build
$> cd build
$> cmake ..
```

for building the Vulkan sample:

```
$> cd samples
$> mkdir build
$> cd build
$> cmake .. -DNIS_VK_SAMPLE=ON
```

Open the solution with Visual Studio 2019. Right-click the sample project and select "Set as Startup Project" before building the project. For Linux, only the VK sample will be generated.