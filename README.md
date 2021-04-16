## gamescope: the micro-compositor formerly known as steamcompmgr

In an embedded session usecase, gamescope does the same thing as steamcompmgr, but with less extra copies and latency:

 - It's getting game frames through Wayland by way of Xwayland, so there's no copy within X itself before it gets the frame.
 - It can use DRM/KMS to directly flip game frames to the screen, even when stretching or when notifications are up, removing another copy.
 - When it does need to composite with the GPU, it does so with async Vulkan compute, meaning you get to see your frame quick even if the game already has the GPU busy with the next frame.

It also runs on top of a regular desktop, the 'nested' usecase steamcompmgr didn't support.

 - Because the game is running in its own personal Xwayland sandbox desktop, it can't interfere with your desktop and your desktop can't interfere with it. 
 - You can spoof a virtual screen with a desired resolution and refresh rate as the only thing the game sees, and control/resize the output as needed. This can be useful in exotic display configurations like ultrawide or multi-monitor setups that involve rotation.

It runs on Mesa+AMDGPU, and could be made to run on other Mesa/DRM drivers with minimal work. Can support NVIDIA if/when they support accelerated Xwayland.

If running RadeonSI clients with older cards (GFX8 and below) or older Mesa version (21.0 and below), currently have to set `R600_DEBUG=nodcc`, or corruption will be observed until the stack picks up DRM modifiers support.

## Building

```
meson build/
ninja -C build/
build/gamescope -- <game>
```

## Keyboard shortcuts

* **Super + F** : Toggle fullscreen
* **Super + N** : Toggle integer scaling
* **Super + S** : Take screenshot (currently goes to `/tmp/gamescope_$DATE.bmp`)

## Examples

On any X11 or Wayland desktop running Mesa 20.3+, you can set the Steam launch arguments of your game as follows:

```sh
# Upscale a 720p game to 1440p with integer scaling
gamescope -w 1280 -h 720 -W 2560 -H 1440 -n -- %command%

# Limit a vsynced game to 30 FPS
gamescope -r 30 -- %command%

# Run the game at 1080p, but scale output to a fullscreen 3440×1440 pillarboxed ultrawide window
gamescope -w 1920 -h 1080 -W 3440 -H 1440 -b -- %command%
```

## Options

* `-W`, `-H`: set the resolution used by gamescope. Resizing the gamescope window will update these settings. Defaults to 1280x720.
* `-w`, `-h`: set the resolution used by the game. Defaults to the values specified in `-W` and `-H`.
* `-r`: set a frame-rate limit for the game. Specified in frames per second. Defaults to unlimited.
* `-o`: set a frame-rate limit for the game when unfocused. Specified in frames per second. Defaults to unlimited.
* `-n`: use integer scaling.
* `-b`: create a border-less window.
* `-f`: create a full-screen window.
