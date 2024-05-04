#pragma once

namespace gamescope
{
    // Backend enum.
    enum GamescopeBackend
    {
        Auto,
        DRM,
        SDL,
        Headless,
        Wayland,
    };

    // Backend forward declarations.
    class CSDLBackend;
    class CDRMBackend;
    class CHeadlessBackend;
    class CWaylandBackend;
}
