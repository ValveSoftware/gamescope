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

```
git submodule update --init
meson build/
ninja -C build/
build/src/gamescope -- <game>
```

Install with:

```
meson install -C build/ --skip-subprojects
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

On any X11 or Wayland desktop, you can set the Steam launch arguments of your game as follows:

```sh
# Upscale a 720p game to 1440p with integer scaling
gamescope -h 720 -H 1440 -S integer -- %command%

# Limit a vsynced game to 30 FPS
gamescope -r 30 -- %command%

# Run the game at 1080p, but scale output to a fullscreen 3440×1440 pillarboxed ultrawide window
gamescope -w 1920 -h 1080 -W 3440 -H 1440 -b -- %command%
```


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

If your display supports HDR and it is currently enabled, gamescope will automatically enable HDR support for its own output. You can use gamescope in an embedded session running with the DRM backened for HDR output, or if your desktop environment supports HDR and runs a Wayland session, you can use the Wayland backend. HDR can be disabled with `--hdr-disabled` as a gamescope launch option or as a convar.

If you want to check if HDR is working properly, the mangoapp overlay has an HDR indicator and the right side menu of Steam's GamepadUI will have an HDR badge next to the brightness and volume sliders. 

## Gamescope utilities

There are several utilities that get installed along with the primary gamescope binary. There are two user-interactive binaries, gamescopestream and gamescopectl, while a third binary, gamescopereaper, is used by gamescope itself.

**gamescopestream**:

The first binary is gamescopestream, which can be used to capture a Pipewire stream and dma-buf from a gamescope window into a new Wayland window. It can be run without any arguments as `gamescopestream`, or you can pass along a Steam AppID. For example, Counter-Strike 2 has a Steam AppID of 730. If Counter-Strike 2 is launched with gamescope, you can use `gamescopestream app_id 730` to capture your gamescope window and export it into a new window for recording or streaming purposes. This is also how Steam's Game Recording feature works within an embedded gamescope session.

**gamescopectl**:

The second binary is gamescopectl, which is a debugging utility that can be used to set convars, execute debugging commands, and list information about active gamescope instances.

To print gamescope version and display information when filing an issue, you can run `gamescopectl` without any arguments.

```sh
gamescopectl version 3.14.23
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

For a full list of convars and debugging commands, you can run `gamescopectl help` while a gamescope instance is open. Generally, convars will be set with a 0 for disabled or a 1 for enabled, although some convars will have additional values as noted in `gamescopectl help`

```sh
# Force gamescope to always composite (i.e. never use direct scan-out)
gamescopectl composite_force 1

# Stop forcing gamescope to always composite (i.e. go back to using direct scan-out when possible)
gamescopectl composite_force 0

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

There is currently no way to set the value of uniforms/options, they will just be their initializer values currently.

Using Reshade effects will increase latency as there will be work performed on the general gfx + compute queue as opposed to only using the realtime async compute queue which can run in tandem with the game's gfx work.

Using Reshade effects is **highly discouraged** for doing simple transformations which can be achieved with LUTs/CTMs which are possible to do in the DC (Display Core) on AMDGPU at scanout time, or with the current regular async compute composite path.
The looks system where you can specify your own 3D LUTs would be a better alternative for such transformations.

Pull requests for improving Reshade compatibility support are appreciated.

## Status of Gamescope Packages

[![Packaging status](https://repology.org/badge/vertical-allrepos/gamescope.svg)](https://repology.org/project/gamescope/versions)
