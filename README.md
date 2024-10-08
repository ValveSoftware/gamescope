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

**Build Requirements**:
- [Git](https://git-scm.com/) – Version control system used to clone and manage the repository.
- [Meson](https://mesonbuild.com/) – Build configuration system for managing the build process.
- [Ninja](https://github.com/ninja-build/ninja) - Build system
- [CMake](https://cmake.org/) – Build system
- [glslang](https://github.com/KhronosGroup/glslang) – GLSL compiler for shader processing.
- [wayland-protocols](https://gitlab.freedesktop.org/wayland/wayland-protocols) – Extra Wayland protocols needed for communication.
- [vulkan-headers](https://github.com/KhronosGroup/Vulkan-Headers) – Vulkan API headers for graphics rendering.

**Dependencies**:
- [libei](https://gitlab.freedesktop.org/libinput/libei) Emulated input library for Wayland
- [libcap](https://sites.google.com/site/fullycapable/) – Provides capabilities for fine-grained access control.
- [wlroots](https://github.com/misyltoad/wlroots) – Fork of [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots), specifically for gamescope
- [vkroots](https://github.com/misyltoad/vkroots) - Vulkan layer framework
- [SDL2](https://github.com/libsdl-org/SDL) – Simple DirectMedia Layer
- [libx11](https://gitlab.freedesktop.org/xorg/lib/libx11) – core X11 client-side library.
- [libxmu](https://gitlab.freedesktop.org/xorg/lib/libxmu) – X11 miscellaneous utilities library.
- [libxdamage](https://gitlab.freedesktop.org/xorg/lib/libxdamage) – X11 Damage extension library.
- [libxcomposite](https://gitlab.freedesktop.org/xorg/lib/libxcomposite) – X11 Composite extension library.
- [libxrender](https://gitlab.freedesktop.org/xorg/lib/libxrender) – X11 Render extension library.
- [libxres](https://gitlab.freedesktop.org/xorg/lib/libxres) – X11 resource extension library.
- [libxtst](https://gitlab.freedesktop.org/xorg/lib/libxtst) – X11 library for XTEST & RECORD extensions
- [libxkbcommon](https://github.com/xkbcommon/libxkbcommon) – X11 library for keymap handling
- [libdrm](https://gitlab.freedesktop.org/mesa/drm) – Direct Rendering Manager library.
- [libinput](https://gitlab.freedesktop.org/libinput/libinput) – Input device handling library.
- [benchmark](https://github.com/google/benchmark) – Library for benchmarking code performance.
- [pipewire](https://pipewire.org/) – Multimedia processing framework.
- [libavif](https://github.com/AOMediaCodec/libavif) – AVIF image codec library.
- [libheif](https://github.com/strukturag/libheif) – HEIF image format library.
- [aom](https://aomedia.org/) – Open Media Video Codec for AV1 encoding.
- [rav1e](https://github.com/xiph/rav1e) – AV1 video encoder.
- [libdecor](https://gitlab.freedesktop.org/libdecor/libdecor) – Window decoration library.
- [xorg-xwayland](https://gitlab.freedesktop.org/xorg/xserver) – X server for Wayland compatibility.
- [wayland](https://gitlab.freedesktop.org/wayland/wayland) - Core Wayland window system (at least version 1.23)

If your distro packages gamescope, your package manager may already include a way to grab most build and run-time dependencies:

- `makepkg -s --nobuild` *# Arch, with PKGBUILD*
- `dnf builddep gamescope` *# dnf Fedora*
- `apt-get build-dep gamescope` *# Debian*
- `zypper source-install --build-deps-only gamescope` *# openSUSE*

**Compile**:

Clone the repository with all submodules:
```
git clone https://github.com/ValveSoftware/gamescope.git --recurse-submodules
```

Set up your build for native architecture:
```
git submodule update --init
meson setup build/
ninja -C build/
build/src/gamescope -- <game>
```

Install with:
```
meson install -C build/ --skip-subprojects
```

When using gamescope's 64-bit executable, some older game titles require a 32-bit `libVkLayer_FROG_gamescope_wsi` layer in addition to the standard 64-bit layer. This is best compiled in a separate build folder, such as `build32`. Since a gamescope 32-bit executable binary is unncessary in this case, the meson configuration options are slightly different.
```
meson -Denable_gamescope=false -Denable_gamescope_wsi_layer=true -Denable_openvr_support=false -Dpipewire=disabled build32/
ninja -C build32/
```

Install with:
```
meson install -C build32/ --skip-subprojects
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

**Embedded**

When running gamescope from a virtual terminal (TTY), you can run programs using gamescope's DRM backend. This includes the Steam client, along with any relevant Steam client launch arguments. You will want to use with `--steam/-e` as a gamescope launch argument to enable Steam integration.

```sh
# Run Steam embedded within gamescope at 120hz and native resolution with adaptive sync, mangoapp, and HDR enabled while using GamepadUI
gamescope -e -r 120 --adaptive-sync --hdr-enabled --mangoapp -- steam -gamepadui


## Options

See `gamescope --help` for a full list of options.

* `-W`, `-H`: set the resolution used by gamescope. Resizing the gamescope window will update these settings. Ignored in embedded mode. If `-H` is specified but `-W` isn't, a 16:9 aspect ratio is assumed. Defaults to 1280×720.
* `-w`, `-h`: set the resolution used by the game. If `-h` is specified but `-w` isn't, a 16:9 aspect ratio is assumed. Defaults to the values specified in `-W` and `-H`.
* `-r`: set a frame-rate limit for the game. Specified in frames per second. Defaults to unlimited.
* `-o`: set a frame-rate limit for the game when unfocused. Specified in frames per second. Defaults to unlimited.
* `-F fsr`: use AMD FidelityFX™ Super Resolution 1.0 for upscaling
* `-F nis`: use NVIDIA Image Scaling v1.0.3 for upscaling
* `-S integer`: use integer scaling.
* `-S stretch`: use stretch scaling, the game will fill the window. (e.g. 4:3 to 16:9)
* `-b`: create a border-less window.
* `-f`: create a full-screen window.
* `-e`: enable Steam client integration when running Steam embedded within gamescope

## High Dynamic Range (HDR)

Gamescope offers a wide range of configuration options for HDR support in both a nested usecase (with the Wayland backend) as well as in an embedded usecase (with the DRM backend). If your setup supports HDR, then enabling HDR support within gamescope is as easy as adding `--hdr-enabled` to your gamescope launch options. 

If you want to check if HDR is working, there are several ways you can do this. The easiest ways to check while in-game are looking for an HDR indicator in the mangoapp overlay, or on the right-side menu of Steam's GamepadUI (the Steam Deck layout) next to the sliders for volume. Gamescope's own logging will also tell you about the metadata of the HDR content that it is using.

## Gamescope utilities

There are several utilities that get installed along with the primary gamescope binary. There are two user-interactive binaries, gamescopestream and gamescopectl, while a third binary, gamescopereaper, is used by gamescope itself.

**gamescopestream**:

The first binary is gamescopestream, which can be used to capture a Pipewire stream and dma-buf from a gamescope window into a new Wayland window. It can be run without any arguments as `gamescopestream`, or you can pass along a Steam AppID. For example, Counter-Strike 2 has a Steam AppID of 730. If Counter-Strike 2 is launched with gamescope, you can use `gamescopestream app_id 730` to capture your gamescope window and export it into a new window for recording or streaming purposes. This is also how Steam's Game Recording feature works within an embedded gamescope session.

**gamescopectl**:

The second binary is gamescopectl, which is a debugging utility that can be used to set convars, execute debugging commands, and list information about active gamescope instances.

To print gamescope version and display information when filing an issue, you can run `gamescopectl` without any arguments.

```sh
gamescopectl version 3.15.9 (gcc 13.2.1)
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

Generally, convars will be set with a 0 or false for disabled, and a 1 or true for enabled. Some convars like logging will have additional values as noted in `gamescopectl help`. Keep in mind, this list is ever expanding!

```sh
# Force gamescope to always composite (i.e. never use direct scan-out)
gamescopectl composite_force 1

# Stop forcing gamescope to always composite (i.e. go back to using direct scan-out when possible)
gamescopectl composite_force false

# Print an output of gamescope backend information for debugging purposes
gamescopectl backend_info
```

Please note that some convars must be set in code before building gamescope as `gamescopectl help` lists every possible convar, not the just the ones that can be toggled via `gamescopectl`.

**gamescopereaper**:

gamescopereaper is used by the primary gamescope binary to clean up after itself by taking care of shutting down child processes after the parent process ends.

**mangoapp**:

While this is not a shipped binary with gamescope, mangoapp is the officially supported method of adding MangoHud overlays to gamescope as it is designed to integrate MangoHud and gamescope together in a way that does not adversely affect gamescope's functionality. Any other method of using MangoHud can cause unexpected issues like Vulkan or swapchain errors.

## Reshade support

Gamescope supports a subset of Reshade effects/shaders using the `--reshade-effect [path]` and `--reshade-technique-idx [idx]` command line parameters.

This provides an easy way to do shader effects (ie. CRT shader, film grain, debugging HDR with histograms, etc) on top of whatever is being displayed in Gamescope without having to hook into the underlying process.

Uniform/shader options can be modified programmatically via the `gamescope-reshade` wayland interface. Otherwise, they will just use their initializer values.

Using Reshade effects will increase latency as there will be work performed on the general gfx + compute queue as opposed to only using the realtime async compute queue which can run in tandem with the game's gfx work.

Using Reshade effects is **highly discouraged** for doing simple transformations which can be achieved with LUTs/CTMs which are possible to do in the DC (Display Core) on AMDGPU at scanout time, or with the current regular async compute composite path.
The looks system where you can specify your own 3D LUTs would be a better alternative for such transformations.

Pull requests for improving Reshade compatibility support are appreciated.

## Troubleshooting



## Status of Gamescope Packages

[![Packaging status](https://repology.org/badge/vertical-allrepos/gamescope.svg)](https://repology.org/project/gamescope/versions)
