#pragma once

namespace gamescope
{
    // Backend enum.
    enum GamescopeBackend
    {
        Auto,
        DRM,
        SDL,
        OpenVR,
        Headless,
        Wayland,
    };

    // Backend forward declarations.
    class CSDLBackend;
    class CDRMBackend;
    class COpenVRBackend;
    class CHeadlessBackend;
    class CWaylandBackend;

    // Pre-flight validation for --prefer-drm. Returns true when --prefer-drm is
    // unset or resolves to a valid DRM primary node; logs an error and returns
    // false otherwise. Lets main() reject bad input before the DRM backend and
    // Vulkan/session init are brought up.
    bool DRMBackendCheckPreferredDevice();
}
