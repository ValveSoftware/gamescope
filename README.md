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

Dependencies first (**Debian/Debian-based**):
```
apt install meson ninja-build pkg-config cmake libpipewire-0.3-dev hwdata libx11-dev libwayland-dev libvulkan-dev wayland-protocols libx11-xcb-dev libxdamage-dev libxcomposite-dev libxcursor-dev libxxf86vm-dev libxtst-dev libxres-dev libxmu-dev libxkbcommon-dev libcap-dev libsdl2-dev libavif-dev libpixman-1-dev liblcms2-dev libseat-dev libinput-dev xwayland libxcb-composite0-dev libxcb-ewmh-dev libxcb-icccm4-dev libxcb-res0-dev glslang-tools libluajit-5.1-dev libcatch2-dev
```

Note: Debian stable (13 "trixie") packages some libraries older than Gamescope
and its bundled wlroots require. If `meson setup` complains about the versions
below, install newer ones first:

- **Vulkan-Headers >= 1.4.335** (build from source):

  ```
  git clone --depth 1 --branch v1.4.341 https://github.com/KhronosGroup/Vulkan-Headers /tmp/Vulkan-Headers
  cmake -B /tmp/Vulkan-Headers/build -S /tmp/Vulkan-Headers -DCMAKE_INSTALL_PREFIX=/usr/local
  sudo cmake --install /tmp/Vulkan-Headers/build
  ```
- **wayland >= 1.24.0** (from trixie-backports):

  ```
  echo "deb http://deb.debian.org/debian trixie-backports main contrib non-free non-free-firmware" | sudo tee /etc/apt/sources.list.d/backports.list
  sudo apt update
  sudo apt install -t trixie-backports libwayland-dev libwayland-server0 libwayland-client0 libwayland-cursor0 libwayland-egl1
  ```
- **libdrm >= 2.4.129** (build from source):

  ```
  sudo apt install libpciaccess-dev
  git clone --depth 1 --branch libdrm-2.4.129 https://gitlab.freedesktop.org/mesa/libdrm.git /tmp/libdrm
  meson setup /tmp/libdrm/build /tmp/libdrm --prefix=/usr/local -Dintel=disabled -Dradeon=disabled -Damdgpu=enabled -Dnouveau=disabled -Dvmwgfx=disabled -Dtests=false
  ninja -C /tmp/libdrm/build
  sudo ninja -C /tmp/libdrm/build install
  sudo ldconfig
  ```

Dependencies first (**Fedora/Fedora-based**):
```
dnf install meson ninja-build gcc-c++ pkgconf-pkg-config cmake pipewire-devel hwdata libX11-devel wayland-devel vulkan-headers vulkan-loader-devel wayland-protocols-devel libXdamage-devel libXcomposite-devel libXcursor-devel libXxf86vm-devel libXtst-devel libXres-devel libXmu-devel libxkbcommon-devel libcap-devel SDL2-devel libavif-devel pixman-devel lcms2-devel libseat-devel libinput-devel xorg-x11-server-Xwayland-devel libxcb-devel xcb-util-wm-devel glslang-devel luajit-devel libdecor-devel google-benchmark-devel libdrm-devel
```

Gamescope's tests need Catch2 v3 (`catch2-with-main`), but Fedora's
`catch2-devel` is v2, so build v3 from source:

```
git clone --depth 1 --branch v3.7.1 https://github.com/catchorg/Catch2.git /tmp/Catch2
cmake -B /tmp/Catch2/build -S /tmp/Catch2 -DCMAKE_INSTALL_PREFIX=/usr/local -DBUILD_TESTING=OFF
cmake --build /tmp/Catch2/build -j$(nproc)
sudo cmake --install /tmp/Catch2/build
```

Then, on any distribution, build with:

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

## Keyboard shortcuts

* **Super + F** : Toggle fullscreen
* **Super + N** : Toggle nearest neighbour filtering
* **Super + B** : Toggle bilinear filtering
* **Super + U** : Toggle FSR upscaling
* **Super + Y** : Toggle NIS upscaling
* **Super + T** : Toggle SGSR upscaling
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
* `-F sgsr`: use Snapdragon™ Game Super Resolution 1 with RCAS sharpening for upscaling
* `-S integer`: use integer scaling.
* `-S stretch`: use stretch scaling, the game will fill the window. (e.g. 4:3 to 16:9)
* `-b`: create a border-less window.
* `-f`: create a full-screen window.

## Reshade support

Gamescope supports a subset of Reshade effects/shaders using the `--reshade-effect [path]` and `--reshade-technique-idx [idx]` command line parameters.

This provides an easy way to do shader effects (ie. CRT shader, film grain, debugging HDR with histograms, etc) on top of whatever is being displayed in Gamescope without having to hook into the underlying process.

Uniform/shader options can be modified programmatically via the `gamescope-reshade` wayland interface. Otherwise, they will just use their initializer values.

Using Reshade effects will increase latency as there will be work performed on the general gfx + compute queue as opposed to only using the realtime async compute queue which can run in tandem with the game's gfx work.

Using Reshade effects is **highly discouraged** for doing simple transformations which can be achieved with LUTs/CTMs which are possible to do in the DC (Display Core) on AMDGPU at scanout time, or with the current regular async compute composite path.
The looks system where you can specify your own 3D LUTs would be a better alternative for such transformations.

Pull requests for improving Reshade compatibility support are appreciated.

## Status of Gamescope Packages

[![Packaging status](https://repology.org/badge/vertical-allrepos/gamescope.svg?exclude_unsupported=1)](https://repology.org/project/gamescope/versions)
