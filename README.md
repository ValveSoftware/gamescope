## gamescope: the micro-compositor formerly known as steamcompmgr

In an embedded session usecase, gamescope does the same thing as steamcompmgr, but with less extra copies and latency:

 - It's getting game frames through Wayland by way of Xwayland, so there's no copy within X itself before it gets the frame.
 - It can use DRM/KMS to directly flip game frames to the screen, even when stretching or when notifications are up, removing another copy.
 - When it does need to composite with the GPU, it does so with async Vulkan compute, meaning you get to see your frame quick even if the game already has the GPU busy with the next frame.

It also runs on top of a regular desktop, the 'nested' usecase steamcompmgr didn't support.

 - Because the game is running in its own personal Xwayland sandbox desktop, it can't interfere with your desktop and your desktop can't interfere with it. 
 - You can spoof a virtual screen with a desired resolution and refresh rate as the only thing the game sees, and control/resize the output as needed. This can be useful in exotic display configurations like ultrawide or multi-monitor setups that involve rotation.

It runs on Mesa+AMDGPU, and could be made to run on other Mesa/DRM drivers with minimal work. Can support NVIDIA if/when they support accelerated Xwayland.

If running RadeonSI clients, currently have to set R600_DEBUG=nodcc, or corruption will be observed until the stack picks up DRM modifiers support.

## Building

```
meson build/
ninja -C build/
build/gamescope -- <game>
```

## Keyboard shortcuts

**Super + F** : Toggle fullscreen

**Super + N** : Toggle integer scaling

**Super + S** : Take screenshot (currently goes to /tmp/gamescope_$DATE.bmp)

## Examples

On any X11 or Wayland desktop running Mesa 20.3+, you can set the Steam launch arguments of your game as follows:

```
// Upscale a 720p game to 1440p with integer scaling
gamescope -w 1280 -h 720 -W 2560 -H 1440 -n -- %command%
```

```
// Limit a vsynced game to 30 FPS:
gamescope -r 30 -- %command%
```

```
// Run the game at 1080p, but scale output to a fullscreen 3440×1440 pillarboxed ultrawide window
gamescope -w 1920 -h 1080 -W 3440 -H 1440 -b -- %command%
```

## Installation

### Requirements
You should install dev packages if available:
- cmake
- git
- [meson](https://pypi.org/project/meson/) >=0.54.0
- ninja
- g++
- glslang
- libcap
- libdrm
- libgbm
- libinput
- libpng
- libxcb
- libxcb-composite
- libxcb-xinput
- libxdamage
- libxkbcommon
- libxrender
- libxtst
- libxxf86vm
- libsystemd (or libelogind)
- sdl2 (libsdl2)
- libxcomposite
- opengl-driver (mesa or nvidia)
- libvulkan
- pixman
- pkg-config
- wayland
- wayland-protocols
- xwayland
- x11-xcb
- xcb-icccm

#### Arch-based
[gamescope (AUR)](https://aur.archlinux.org/packages/gamescope/)

#### Debian-based & Ubuntu-based
```
sudo apt install cmake git meson ninja-build g++ glslang-tools libcap-dev libdrm-dev libgbm-dev libinput-dev libpng-dev libxcb1-dev libxcb-composite0-dev libxcb-xinput-dev libxdamage-dev libxkbcommon-dev libxrender-dev libxtst-dev libxxf86vm-dev libsdl2-dev libxcomposite-dev libvulkan-dev libpixman-1-dev pkg-config wayland-protocols xwayland libx11-xcb-dev libxcb-icccm4-dev libsystemd-dev
```

### Build
```
git clone https://github.com/Plagman/gamescope.git
cd gamescope
meson build
ninja -C build
sudo ninja -C build install
```
