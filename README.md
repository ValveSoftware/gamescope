## gamescope: the micro-compositor formerly known as steamcompmgr

In an embedded session usecase, gamescope does the same thing as steamcompmgr, but with less extra copies and latency:

 - It's getting game frames through Wayland by way of Xwayland, so there's no copy within X itself before it gets the frame.
 - It can use DRM/KMS to directly flip game frames to the screen, even when stretching or when notifications are up, removing another copy.
 - When it does need to composite with the GPU, it does so with async Vulkan compute, meaning you get to see your frame quick even if the game already has the GPU busy with the next frame.

It also runs on top of a regular desktop, the 'nested' usecase steamcompmgr didn't support.

 - Because the game is running in its own personal Xwayland sandbox desktop, it can't interfere with your desktop and your desktop can't interfere with it.
 - You can spoof a virtual screen with a desired resolution and refresh rate as the only thing the game sees, and control/resize the output as needed. This can be useful in exotic display configurations like ultrawide or multi-monitor setups that involve rotation.

It runs on Mesa + AMD or Intel, and could be made to run on other Mesa/DRM drivers with minimal work. AMD requires Mesa 20.3+, Intel requires Mesa 21.2+. For NVIDIA's proprietary driver, version 515.43.04+ is required (make sure the `nvidia-drm.modeset=1` kernel parameter is set).

If running RadeonSI clients with older cards (GFX8 and below), currently have to set `R600_DEBUG=nodcc`, or corruption will be observed until the stack picks up DRM modifiers support.

## Building
**Build Dependencies**:
- [Git](https://git-scm.com/)
- [Meson](https://mesonbuild.com/) - (version 0.58 or newer)
- [Ninja](https://github.com/ninja-build/ninja)
- [CMake](https://cmake.org/)
- [glslang](https://github.com/KhronosGroup/glslang)
- [wayland-protocols](https://gitlab.freedesktop.org/wayland/wayland-protocols) - (version 1.17 or newer)
- [vulkan-headers](https://github.com/KhronosGroup/Vulkan-Headers)
- [benchmark](https://github.com/google/benchmark)

**Requirements**:
- [libei](https://gitlab.freedesktop.org/libinput/libei)
- [libcap](https://sites.google.com/site/fullycapable/)
- [SDL2](https://github.com/libsdl-org/SDL)
- [libx11](https://gitlab.freedesktop.org/xorg/lib/libx11)
- [libxmu](https://gitlab.freedesktop.org/xorg/lib/libxmu)
- [libxdamage](https://gitlab.freedesktop.org/xorg/lib/libxdamage)
- [libxcomposite](https://gitlab.freedesktop.org/xorg/lib/libxcomposite)
- [libxrender](https://gitlab.freedesktop.org/xorg/lib/libxrender)
- [libxres](https://gitlab.freedesktop.org/xorg/lib/libxres)
- [libxtst](https://gitlab.freedesktop.org/xorg/lib/libxtst)
- [libxkbcommon](https://github.com/xkbcommon/libxkbcommon)
- [libdrm](https://gitlab.freedesktop.org/mesa/drm) - (version 2.4.113 or newer)
- [libinput](https://gitlab.freedesktop.org/libinput/libinput)
- [pipewire](https://pipewire.org/)
- [libavif](https://github.com/AOMediaCodec/libavif) - (version 1.0.0 or newer)
- [libheif](https://github.com/strukturag/libheif)
- [aom](https://aomedia.org/)
- [rav1e](https://github.com/xiph/rav1e)
- [libdecor](https://gitlab.freedesktop.org/libdecor/libdecor)
- [xorg-xwayland](https://gitlab.freedesktop.org/xorg/xserver)
- [wayland](https://gitlab.freedesktop.org/wayland/wayland) - (version 1.23.0 or newer)
- [luajit](https://luajit.org/)

Clone the repository with all submodules:

```sh
git clone https://github.com/ValveSoftware/gamescope.git --recurse-submodules
```

Set up your build for native architecture:

```sh
git submodule update --init
meson setup build/
ninja -C build/
```

Install with:

```sh
meson install -C build/ --skip-subprojects
```

By default, meson will install to `/usr/local`. If you want to install to your system directory, you can set the prefix to `--prefix=/usr` during meson build configuration.

Uninstall with:

```sh
sudo ninja -C build/ uninstall
```

## Keyboard shortcuts

* **Super + F** : Toggle fullscreen
* **Super + N** : Toggle nearest neighbour filtering
* **Super + U** : Toggle FSR upscaling
* **Super + Y** : Toggle NIS upscaling
* **Super + I** : Increase FSR sharpness by 1
* **Super + O** : Decrease FSR sharpness by 1
* **Super + S** : Take screenshot (currently goes to `/tmp/gamescope_$DATE.png`)
* **Super + G** : Toggle keyboard grab

## Examples
**Nested**

On any X11 or Wayland desktop, you can set the Steam launch arguments of your game as follows:

```sh
# Upscale a 720p game to 1440p with integer scaling
gamescope -h 720 -H 1440 -S integer -- %command%

# Limit a vsynced game to 30 FPS
gamescope -r 30 -- %command%

# Run the game at 1080p, but scale output to a fullscreen 3440×1440 pillarboxed ultrawide window
gamescope -w 1920 -h 1080 -W 3440 -H 1440 -b -- %command%
```

From a desktop terminal, you can also run gamescope with programs other than Steam:

```sh
# Run vkcube in gamescope
gamescope -- vkcube

# Run glxgears in fullscreen 2160p, with a frame rate of 24 FPS
gamescope -r 24 -w 3840 -h 2160 -- glxgears
```

> [!WARNING]
> Trying to use launch arguments without a `--` separator before `%command%` or a program being run will not start gamescope correctly.

**Embedded**

When launching from a Virtual Terminal, gamescope's DRM backend can act as a standalone session. The easiest way to run games with this backend is by running the entire Steam client within gamescope. 

To enable Steam client integration in gamescope, add `-e` or `--steam` to your launch arguments for gamescope while launching the Steam client. Running gamescope from a VT will default to your monitor's native resolution and refresh rate unless otherwise specified.

```sh
# Run Steam embedded within gamescope at 7680x2160p with adaptive sync, mangoapp, and HDR enabled while using GamepadUI
gamescope -e -w 7680 -h 2160  --adaptive-sync --hdr-enabled --mangoapp -- steam -gamepadui
```

## Options

See `gamescope --help` for a full list of options.

* `-W`, `-H`: set the resolution used by gamescope. Resizing the gamescope window will update these settings. Ignored in embedded mode. If `-H` is specified but `-W` isn't, a 16:9 aspect ratio is assumed. Defaults to 1280×720.
* `-w`, `-h`: set the resolution used by the game. If `-h` is specified but `-w` isn't, a 16:9 aspect ratio is assumed. Defaults to the values specified in `-W` and `-H`.
* `-r`: set a frame-rate limit for the game. Specified in frames per second. Defaults to unlimited.
* `-o`: set a frame-rate limit for the game when unfocused. Specified in frames per second. Defaults to unlimited.
* `-F fsr`: use AMD FidelityFX™ Super Resolution 1.0 for upscaling.
* `-F nis`: use NVIDIA Image Scaling v1.0.3 for upscaling.
* `-S integer`: use integer scaling.
* `-S stretch`: use stretch scaling, the game will fill the window. (e.g. 4:3 to 16:9)
* `-b`: create a border-less window.
* `-f`: create a full-screen window.
* `-e`: enable Steam client integration when running Steam embedded within gamescope.

## High Dynamic Range (HDR)

Gamescope has HDR support in both a nested usecase with the Wayland backend, or an embedded usecase with the DRM backend. If your setup supports HDR, enabling HDR within gamescope is as simple as adding `--hdr-enabled` to your gamescope launch arguments. 

Gamescope's logging reports the metadata of the HDR content it presents with [Gamescope WSI], and some MangoHud overlay presets include HDR status. The Steam Deck UI will also show an HDR badge on the right-side menu, next to the volume slider, when HDR is working both in-game and in gamescope.

## Utilities

Several utilities are built and installed along with the gamescope binary. Two binaries, gamescopestream and gamescopectl, are designed to be user-interactive while a third binary, gamescopereaper, is used for child process cleanup.

**gamescopestream**:

Gamescopestream is used to capture a Pipewire stream from a gamescope window and export it into a new window. It can be run alongside gamescope as `gamescopestream` in a separate terminal, which will capture the most recent window launched within gamescope.

In embedded gamescope with Steam integration, you can pass along a Steam AppID to capture a specific window when multiple applications are launched. For example: Counter-Strike 2 has a Steam AppID of 730 and Portal 2 has a Steam AppID of 620. If both games are running simultaneeously within the same gamescope-session, you can use `gamescopestream 730` to capture only the Counter-Strike 2 window and export it into a new gamescopestream window.

**gamescopectl**:

Gamescopectl is a debugging utility that can be used to set convars, execute debugging commands, and list information about active gamescope instances.

To print your current gamescope version and display panel information when filing an issue, you can run `gamescopectl` without any arguments while gamescope is running.

```sh
gamescopectl version 3.15.13 (gcc 13.2.1)
gamescope_control info:
  - Connector Name: eDP-1
  - Display Make: Valve Corporation
  - Display Model: ANX7530 U
  - Display Flags: 0x3
  - ValidRefreshRates: 45, 47, 48, 49, 50, 51, 53, 55, 56, 59, 60, 62, 64, 65, 66, 68, 72, 73, 76, 77, 78, 80, 81, 82, 84, 85, 86, 87, 88, 90
  Features:
  - Reshade Shaders (1) - Version: 1 - Flags: 0x0
  - Display Info (2) - Version: 1 - Flags: 0x0
  - Pixel Filter (3) - Version: 1 - Flags: 0x0
  - Refresh Cycle Only Change Refresh Rate (4) - Version: 1 - Flags: 0x0
  - Mura Correction (5) - Version: 1 - Flags: 0x0
You can execute any debug command in Gamescope using this tool.
For a list of commands and convars, use 'gamescopectl help'
```

Generally, convars can be set with 0 or false for disabled, and 1 or true for enabled.

```sh
# Force gamescope to always composite (i.e. never use direct scan-out)
gamescopectl composite_force 1

# Stop forcing gamescope to always composite (i.e. go back to using direct scan-out when possible)
gamescopectl composite_force false

# Print an output of gamescope backend information for debugging purposes
gamescopectl backend_info
```

Some convars, like logging levels, have additional values noted in `gamescopectl help`.

**mangoapp**:

Mangoapp is not a binary that is included with gamescope, but using [--mangoapp](https://github.com/flightlessmango/MangoHud?tab=readme-ov-file#gamescope) as a gamescope launch argument is the only officially supported method of adding MangoHud overlays to gamescope. It is designed to integrate MangoHud and gamescope in a way that does not adversely affect gamescope's presentation capabilities.

Any other method of using MangoHud overlays, such as global overrides or `mangohud gamescope`, is not supported as they can cause Vulkan errors or swapchain issues.

## Scripts
>  ⚠️ Experimental Feature ⚠️

Gamescope supports adding per-display configurations for display panels in `.lua` format. These configurations are used by the DRM backend as display profiles for known panels. Chromaticity coordinates can provide custom colorimetry to be used at scan-out, and custom refresh rate timings can be defined to customize the frame limiter within `gamescope-session`. 

Details about scripts can be found inside of the [`README.md`](https://github.com/ValveSoftware/gamescope/tree/master/scripts#readme) within the scripts subdirectory.

## Reshade support

Gamescope supports a subset of Reshade effects/shaders using the `--reshade-effect [path]` and `--reshade-technique-idx [idx]` command line parameters.

This provides an easy way to do shader effects (ie. CRT shader, film grain, debugging HDR with histograms, etc) on top of whatever is being displayed in Gamescope without having to hook into the underlying process.

Uniform/shader options can be modified programmatically via the `gamescope-reshade` wayland interface. Otherwise, they will just use their initializer values.

Using Reshade effects will increase latency as there will be work performed on the general gfx + compute queue as opposed to only using the realtime async compute queue which can run in tandem with the game's gfx work.

Using Reshade effects is **highly discouraged** for doing simple transformations which can be achieved with LUTs/CTMs which are possible to do in the DC (Display Core) on AMDGPU at scanout time, or with the current regular async compute composite path.
The looks system where you can specify your own 3D LUTs would be a better alternative for such transformations.

Pull requests for improving Reshade compatibility support are appreciated.

## Status of Gamescope Packages

[![Packaging status](https://repology.org/badge/vertical-allrepos/gamescope.svg)](https://repology.org/project/gamescope/versions)
