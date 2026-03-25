#include "WaylandInputThread.hpp"

#include "callback_macro.hpp"

namespace gamescope {

const wl_registry_listener CWaylandInputThread::s_RegistryListener = {
	.global = WAYLAND_USERDATA_TO_THIS(CWaylandInputThread, Wayland_Registry_Global),
	.global_remove = WAYLAND_NULL(),
};
const wl_seat_listener CWaylandInputThread::s_SeatListener = {
	.capabilities = WAYLAND_USERDATA_TO_THIS(CWaylandInputThread, Wayland_Seat_Capabilities),
	.name = WAYLAND_USERDATA_TO_THIS(CWaylandInputThread, Wayland_Seat_Name),
};
const wl_pointer_listener CWaylandInputThread::s_PointerListener = {
	.enter = WAYLAND_USERDATA_TO_THIS(CWaylandInputThread, Wayland_Pointer_Enter),
	.leave = WAYLAND_USERDATA_TO_THIS(CWaylandInputThread, Wayland_Pointer_Leave),
	.motion = WAYLAND_USERDATA_TO_THIS(CWaylandInputThread, Wayland_Pointer_Motion),
	.button = WAYLAND_USERDATA_TO_THIS(CWaylandInputThread, Wayland_Pointer_Button),
	.axis = WAYLAND_USERDATA_TO_THIS(CWaylandInputThread, Wayland_Pointer_Axis),
	.frame = WAYLAND_USERDATA_TO_THIS(CWaylandInputThread, Wayland_Pointer_Frame),
	.axis_source = WAYLAND_USERDATA_TO_THIS(CWaylandInputThread, Wayland_Pointer_Axis_Source),
	.axis_stop = WAYLAND_USERDATA_TO_THIS(CWaylandInputThread, Wayland_Pointer_Axis_Stop),
	.axis_discrete = WAYLAND_USERDATA_TO_THIS(CWaylandInputThread, Wayland_Pointer_Axis_Discrete),
	.axis_value120 = WAYLAND_USERDATA_TO_THIS(CWaylandInputThread, Wayland_Pointer_Axis_Value120),
};
const wl_keyboard_listener CWaylandInputThread::s_KeyboardListener = {
	.keymap = WAYLAND_USERDATA_TO_THIS(CWaylandInputThread, Wayland_Keyboard_Keymap),
	.enter = WAYLAND_USERDATA_TO_THIS(CWaylandInputThread, Wayland_Keyboard_Enter),
	.leave = WAYLAND_USERDATA_TO_THIS(CWaylandInputThread, Wayland_Keyboard_Leave),
	.key = WAYLAND_USERDATA_TO_THIS(CWaylandInputThread, Wayland_Keyboard_Key),
	.modifiers = WAYLAND_USERDATA_TO_THIS(CWaylandInputThread, Wayland_Keyboard_Modifiers),
	.repeat_info = WAYLAND_USERDATA_TO_THIS(CWaylandInputThread, Wayland_Keyboard_RepeatInfo),
};
const zwp_relative_pointer_v1_listener CWaylandInputThread::s_RelativePointerListener = {
	.relative_motion =
		WAYLAND_USERDATA_TO_THIS(CWaylandInputThread, Wayland_RelativePointer_RelativeMotion),
};

} // namespace gamescope
