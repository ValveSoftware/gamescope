#include "WaylandPlane.hpp"

#include "callback_macro.hpp"
#include "libdecor_utils.hpp"

namespace gamescope {

// Can't be const, libdecor api bad...
libdecor_frame_interface CWaylandPlane::s_LibDecorFrameInterface = {
	.configure = LIBDECOR_USERDATA_TO_THIS(CWaylandPlane, LibDecor_Frame_Configure),
	.close = LIBDECOR_USERDATA_TO_THIS(CWaylandPlane, LibDecor_Frame_Close),
	.commit = LIBDECOR_USERDATA_TO_THIS(CWaylandPlane, LibDecor_Frame_Commit),
	.dismiss_popup = LIBDECOR_USERDATA_TO_THIS(CWaylandPlane, LibDecor_Frame_DismissPopup),
};
const wl_surface_listener CWaylandPlane::s_SurfaceListener = {
	.enter = WAYLAND_USERDATA_TO_THIS(CWaylandPlane, Wayland_Surface_Enter),
	.leave = WAYLAND_USERDATA_TO_THIS(CWaylandPlane, Wayland_Surface_Leave),
	.preferred_buffer_scale = WAYLAND_NULL(),
	.preferred_buffer_transform = WAYLAND_NULL(),
};
const wp_presentation_feedback_listener CWaylandPlane::s_PresentationFeedbackListener = {
	.sync_output = WAYLAND_USERDATA_TO_THIS(CWaylandPlane, Wayland_PresentationFeedback_SyncOutput),
	.presented = WAYLAND_USERDATA_TO_THIS(CWaylandPlane, Wayland_PresentationFeedback_Presented),
	.discarded = WAYLAND_USERDATA_TO_THIS(CWaylandPlane, Wayland_PresentationFeedback_Discarded),
};
const frog_color_managed_surface_listener CWaylandPlane::s_FrogColorManagedSurfaceListener = {
	.preferred_metadata =
		WAYLAND_USERDATA_TO_THIS(CWaylandPlane, Wayland_FrogColorManagedSurface_PreferredMetadata),
};
const wp_color_management_surface_feedback_v1_listener
	CWaylandPlane::s_WPColorManagementSurfaceListener = {
		.preferred_changed = WAYLAND_USERDATA_TO_THIS(
			CWaylandPlane, Wayland_WPColorManagementSurfaceFeedback_PreferredChanged
		),
};
const wp_image_description_info_v1_listener CWaylandPlane::s_ImageDescriptionInfoListener = {
	.done = WAYLAND_USERDATA_TO_THIS(CWaylandPlane, Wayland_WPImageDescriptionInfo_Done),
	.icc_file = WAYLAND_USERDATA_TO_THIS(CWaylandPlane, Wayland_WPImageDescriptionInfo_ICCFile),
	.primaries = WAYLAND_USERDATA_TO_THIS(CWaylandPlane, Wayland_WPImageDescriptionInfo_Primaries),
	.primaries_named =
		WAYLAND_USERDATA_TO_THIS(CWaylandPlane, Wayland_WPImageDescriptionInfo_PrimariesNamed),
	.tf_power = WAYLAND_USERDATA_TO_THIS(CWaylandPlane, Wayland_WPImageDescriptionInfo_TFPower),
	.tf_named = WAYLAND_USERDATA_TO_THIS(CWaylandPlane, Wayland_WPImageDescriptionInfo_TFNamed),
	.luminances =
		WAYLAND_USERDATA_TO_THIS(CWaylandPlane, Wayland_WPImageDescriptionInfo_Luminances),
	.target_primaries =
		WAYLAND_USERDATA_TO_THIS(CWaylandPlane, Wayland_WPImageDescriptionInfo_TargetPrimaries),
	.target_luminance =
		WAYLAND_USERDATA_TO_THIS(CWaylandPlane, Wayland_WPImageDescriptionInfo_TargetLuminance),
	.target_max_cll =
		WAYLAND_USERDATA_TO_THIS(CWaylandPlane, Wayland_WPImageDescriptionInfo_Target_MaxCLL),
	.target_max_fall =
		WAYLAND_USERDATA_TO_THIS(CWaylandPlane, Wayland_WPImageDescriptionInfo_Target_MaxFALL),
};
const wp_fractional_scale_v1_listener CWaylandPlane::s_FractionalScaleListener = {
	.preferred_scale =
		WAYLAND_USERDATA_TO_THIS(CWaylandPlane, Wayland_FractionalScale_PreferredScale),
};

} // namespace gamescope
