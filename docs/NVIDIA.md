# gamescope on NVIDIA (Blackwell / RTX 5090, driver 610)

Status and notes for running gamescope on the NVIDIA proprietary driver, from
testing on an RTX 5090 (Blackwell, sm120), driver 610.43.02, kernel 6.19-rc,
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
