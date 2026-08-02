#include "convars.hpp"

namespace gamescope {

ConVar<bool> cv_wayland_mouse_warp_without_keyboard_focus(
	"wayland_mouse_warp_without_keyboard_focus", true,
	"Should we only forward mouse warps to the app when we have keyboard focus?"
);
ConVar<bool> cv_wayland_mouse_relmotion_without_keyboard_focus(
	"wayland_mouse_relmotion_without_keyboard_focus", false,
	"Should we only forward mouse relative motion to the app when we have keyboard focus?"
);
ConVar<bool> cv_wayland_use_modifiers("wayland_use_modifiers", true, "Use DMA-BUF modifiers?");

} // namespace gamescope
