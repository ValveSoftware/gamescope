#include "WaylandBackend.hpp"

#include "callback_macro.hpp"

namespace gamescope {

const wl_registry_listener CWaylandBackend::s_RegistryListener = {
	.global = WAYLAND_USERDATA_TO_THIS(CWaylandBackend, Wayland_Registry_Global),
	.global_remove = WAYLAND_NULL(),
};
const wl_output_listener CWaylandBackend::s_OutputListener = {
	.geometry = WAYLAND_USERDATA_TO_THIS(CWaylandBackend, Wayland_Output_Geometry),
	.mode = WAYLAND_USERDATA_TO_THIS(CWaylandBackend, Wayland_Output_Mode),
	.done = WAYLAND_USERDATA_TO_THIS(CWaylandBackend, Wayland_Output_Done),
	.scale = WAYLAND_USERDATA_TO_THIS(CWaylandBackend, Wayland_Output_Scale),
	.name = WAYLAND_USERDATA_TO_THIS(CWaylandBackend, Wayland_Output_Name),
	.description = WAYLAND_USERDATA_TO_THIS(CWaylandBackend, Wayland_Output_Description),
};
const wl_seat_listener CWaylandBackend::s_SeatListener = {
	.capabilities = WAYLAND_USERDATA_TO_THIS(CWaylandBackend, Wayland_Seat_Capabilities),
	.name = WAYLAND_NULL(),
};
const wl_pointer_listener CWaylandBackend::s_PointerListener = {
	.enter = WAYLAND_USERDATA_TO_THIS(CWaylandBackend, Wayland_Pointer_Enter),
	.leave = WAYLAND_USERDATA_TO_THIS(CWaylandBackend, Wayland_Pointer_Leave),
	.motion = WAYLAND_NULL(),
	.button = WAYLAND_NULL(),
	.axis = WAYLAND_NULL(),
	.frame = WAYLAND_NULL(),
	.axis_source = WAYLAND_NULL(),
	.axis_stop = WAYLAND_NULL(),
	.axis_discrete = WAYLAND_NULL(),
	.axis_value120 = WAYLAND_NULL(),
};
const wl_keyboard_listener CWaylandBackend::s_KeyboardListener = {
	.keymap = WAYLAND_NULL(),
	.enter = WAYLAND_USERDATA_TO_THIS(CWaylandBackend, Wayland_Keyboard_Enter),
	.leave = WAYLAND_USERDATA_TO_THIS(CWaylandBackend, Wayland_Keyboard_Leave),
	.key = WAYLAND_NULL(),
	.modifiers = WAYLAND_NULL(),
	.repeat_info = WAYLAND_NULL(),
};
const zwp_locked_pointer_v1_listener CWaylandBackend::s_LockedPointerListener = {
	.locked = WAYLAND_USERDATA_TO_THIS(CWaylandBackend, Wayland_LockedPointer_Locked),
	.unlocked = WAYLAND_USERDATA_TO_THIS(CWaylandBackend, Wayland_LockedPointer_Unlocked),
};
const wp_color_manager_v1_listener CWaylandBackend::s_WPColorManagerListener{
	.supported_intent =
		WAYLAND_USERDATA_TO_THIS(CWaylandBackend, Wayland_WPColorManager_SupportedIntent),
	.supported_feature =
		WAYLAND_USERDATA_TO_THIS(CWaylandBackend, Wayland_WPColorManager_SupportedFeature),
	.supported_tf_named =
		WAYLAND_USERDATA_TO_THIS(CWaylandBackend, Wayland_WPColorManager_SupportedTFNamed),
	.supported_primaries_named =
		WAYLAND_USERDATA_TO_THIS(CWaylandBackend, Wayland_WPColorManager_SupportedPrimariesNamed),
	.done = WAYLAND_USERDATA_TO_THIS(CWaylandBackend, Wayland_WPColorManager_ColorManagerDone),
};
const wl_data_source_listener CWaylandBackend::s_DataSourceListener = {
	.target = WAYLAND_NULL(),
	.send = WAYLAND_USERDATA_TO_THIS(CWaylandBackend, Wayland_DataSource_Send),
	.cancelled = WAYLAND_USERDATA_TO_THIS(CWaylandBackend, Wayland_DataSource_Cancelled),
	.dnd_drop_performed = WAYLAND_NULL(),
	.dnd_finished = WAYLAND_NULL(),
	.action = WAYLAND_NULL(),
};
const zwp_primary_selection_source_v1_listener CWaylandBackend::s_PrimarySelectionSourceListener = {
	.send = WAYLAND_USERDATA_TO_THIS(CWaylandBackend, Wayland_PrimarySelectionSource_Send),
	.cancelled =
		WAYLAND_USERDATA_TO_THIS(CWaylandBackend, Wayland_PrimarySelectionSource_Cancelled),
};

} // namespace gamescope
