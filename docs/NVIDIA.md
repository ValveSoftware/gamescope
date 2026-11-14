# gamescope on NVIDIA (Blackwell / RTX 5090, driver 610)

Status and notes for running gamescope on the NVIDIA proprietary driver, from
testing on an RTX 5090 (Blackwell, sm120), driver 610.43.02, kernel 7.0.12-tkg-bore-llvm,
under COSMIC (Wayland).

## What works today

- **Builds cleanly** against the vendored wlroots 0.19.3 (no NVIDIA-specific
  build flags needed).
- **Nested mode works** (gamescope as a Wayland client of another compositor —
  e.g. COSMIC). Verified: the 5090 is selected as the render device, a flip-mode
  swapchain is created, `VK_EXT_swapchain_maintenance1` is used, runs at the
  output's refresh:

  ```
  Selected GPU 1: NVIDIA GeForce RTX 5090, type: DiscreteGpu
  [Gamescope WSI] Forcing on VK_EXT_swapchain_maintenance1.
  [Gamescope WSI] Created swapchain ... imageCount: 3 ... flip: true
  ```

- **Explicit-sync quirk is already handled**: the NVIDIA 555+ drivers advertise
  `DRM_CAP_SYNCOBJ` but reject `IN_FENCE_FD` with `EPERM`; gamescope detects this
  and disables `IN_FENCE_FD` on the first failing commit (see
  `Backends/DRMBackend.cpp`, "NVIDIA 555 series" comment).

## The real NVIDIA gaps

1. **Standalone DRM backend** (gamescope as the session compositor directly on
   KMS, not nested). This is where NVIDIA historically diverges from AMD:
   atomic plane assignment via libliftoff, modeset, and explicit sync all run
   against `nvidia-drm` instead of `amdgpu`. Needs on-hardware validation —
   use `contrib/nvidia-standalone-test.sh` (guarded, auto-reverts).

2. **HDR colour management.** gamescope's DRM HDR pipeline offloads the colour
   transform to **AMD-only** plane properties — `AMD_PLANE_CTM`,
   `AMD_PLANE_SHAPER_LUT/TF`, `AMD_PLANE_LUT3D`, `AMD_PLANE_BLEND_TF` (see
   `DRMBackend.cpp`). NVIDIA exposes none of these. The HDR10 *signalling*
   (`HDR_OUTPUT_METADATA` + `Colorspace`) NVIDIA *does* support, and gamescope's
   GPU-agnostic **Vulkan composite** colour path (`color_helpers.cpp`,
   `rendervulkan.cpp`) can perform the tone-mapping in-shader. The work is to
   confirm gamescope cleanly falls back to the Vulkan colour path on NVIDIA when
   the AMD plane props are absent, and that HDR output is correct — rather than
   silently producing a wrong/clipped image.

## Known crash: SPIR-V compiler SEGV on driver 610 (Blackwell)

On driver 610.43.02 (RTX 5090), gamescope reproducibly **SIGSEGVs while compiling
its composite/upscale shaders**, inside the NVIDIA SPIR-V→NVVM compiler. It dies
right after backend init / EDID patch, before the first swapchain; nested clients
then report `failed to read Wayland events: Broken pipe`.

Backtrace (coredumpctl):

```
Signal: 11 (SEGV)
#0  libnvidia-glvkspirv.so.610.43.02 + 0x9dd20
...
#10 _nv002nvvm (libnvidia-glvkspirv.so.610.43.02 + 0xa2816)
#11 libnvidia-eglcore.so.610.43.02 + 0xd3061d
```

Notes:
- Not the shader disk cache — purging `~/.cache/nvidia` and setting
  `__GL_SHADER_DISK_CACHE=0` does not help.
- Affects both nested and standalone (`--backend drm`) paths — same compile step.
- Next: bisect which composite/upscale shader trips the compiler, capture a
  minimal repro, file with NVIDIA, and add a gamescope-side fallback so startup
  degrades instead of crashing.

## Testing

```sh
# nested smoke test (safe, runs inside COSMIC):
WAYLAND_DISPLAY=wayland-1 gamescope -W 1280 -H 720 -b -- vkcube

# standalone + HDR (run from a bare TTY — Ctrl+Alt+F3 — never inside COSMIC):
contrib/nvidia-standalone-test.sh            # standalone SDR
HDR=1 contrib/nvidia-standalone-test.sh      # standalone HDR
```

The harness captures driver/DRM diagnostics, time-boxes the run, and has a
watchdog that SIGKILLs gamescope and `chvt`s you back if it ever wedges.
