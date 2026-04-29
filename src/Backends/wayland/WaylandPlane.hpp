#pragma once

#include "WaylandPlaneColorState.hpp"
#include "WaylandPlaneState.hpp"
#include "rendervulkan.hpp"

#include <libdecor.h>
#include <mutex>
#include <optional>
#include <vector>

// clang-format off
#include "wlr_begin.hpp"
#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>
#include <linux-dmabuf-v1-client-protocol.h>
#include <viewporter-client-protocol.h>
#include <single-pixel-buffer-v1-client-protocol.h>
#include <presentation-time-client-protocol.h>
#include <frog-color-management-v1-client-protocol.h>
#include <color-management-v1-client-protocol.h>
#include <pointer-constraints-unstable-v1-client-protocol.h>
#include <relative-pointer-unstable-v1-client-protocol.h>
#include <primary-selection-unstable-v1-client-protocol.h>
#include <fractional-scale-v1-client-protocol.h>
#include <xdg-toplevel-icon-v1-client-protocol.h>
#include "wlr_end.hpp"
// clang-format on

namespace gamescope {

class CWaylandBackend;
class CWaylandConnector;

class CWaylandPlane {
  public:
	CWaylandPlane(CWaylandConnector* pBackend);
	~CWaylandPlane();

	bool Init(CWaylandPlane* pParent, CWaylandPlane* pSiblingBelow);

	uint32_t GetScale() const;

	void Present(std::optional<WaylandPlaneState> oState);
	void Present(const FrameInfo_t::Layer_t* pLayer);

	void CommitLibDecor(libdecor_configuration* pConfiguration);
	void Commit();

	wl_surface* GetSurface() const { return m_pSurface; }
	libdecor_frame* GetFrame() const { return m_pFrame; }
	xdg_toplevel* GetXdgToplevel() const;

	std::optional<WaylandPlaneState> GetCurrentState() {
		std::unique_lock lock(m_PlaneStateLock);
		return m_oCurrentPlaneState;
	}

	void UpdateVRRRefreshRate();

  private:
	void Wayland_Surface_Enter(wl_surface* pSurface, wl_output* pOutput);
	void Wayland_Surface_Leave(wl_surface* pSurface, wl_output* pOutput);
	static const wl_surface_listener s_SurfaceListener;

	void LibDecor_Frame_Configure(libdecor_frame* pFrame, libdecor_configuration* pConfiguration);
	void LibDecor_Frame_Close(libdecor_frame* pFrame);
	void LibDecor_Frame_Commit(libdecor_frame* pFrame);
	void LibDecor_Frame_DismissPopup(libdecor_frame* pFrame, const char* pSeatName);
	static libdecor_frame_interface s_LibDecorFrameInterface;

	void Wayland_PresentationFeedback_SyncOutput(
		struct wp_presentation_feedback* pFeedback, wl_output* pOutput
	);
	void Wayland_PresentationFeedback_Presented(
		struct wp_presentation_feedback* pFeedback, uint32_t uTVSecHi, uint32_t uTVSecLo,
		uint32_t uTVNSec, uint32_t uRefresh, uint32_t uSeqHi, uint32_t uSeqLo, uint32_t uFlags
	);
	void Wayland_PresentationFeedback_Discarded(struct wp_presentation_feedback* pFeedback);
	static const wp_presentation_feedback_listener s_PresentationFeedbackListener;

	void Wayland_FrogColorManagedSurface_PreferredMetadata(
		frog_color_managed_surface* pFrogSurface, uint32_t uTransferFunction,
		uint32_t uOutputDisplayPrimaryRedX, uint32_t uOutputDisplayPrimaryRedY,
		uint32_t uOutputDisplayPrimaryGreenX, uint32_t uOutputDisplayPrimaryGreenY,
		uint32_t uOutputDisplayPrimaryBlueX, uint32_t uOutputDisplayPrimaryBlueY,
		uint32_t uOutputWhitePointX, uint32_t uOutputWhitePointY, uint32_t uMaxLuminance,
		uint32_t uMinLuminance, uint32_t uMaxFullFrameLuminance
	);
	static const frog_color_managed_surface_listener s_FrogColorManagedSurfaceListener;

	void Wayland_WPColorManagementSurfaceFeedback_PreferredChanged(
		wp_color_management_surface_feedback_v1* pColorManagementSurface, unsigned int data
	);
	static const wp_color_management_surface_feedback_v1_listener
		s_WPColorManagementSurfaceListener;
	void UpdateWPPreferredColorManagement();

	void Wayland_WPImageDescriptionInfo_Done(wp_image_description_info_v1* pImageDescInfo);
	void Wayland_WPImageDescriptionInfo_ICCFile(
		wp_image_description_info_v1* pImageDescInfo, int32_t nICCFd, uint32_t uICCSize
	);
	void Wayland_WPImageDescriptionInfo_Primaries(
		wp_image_description_info_v1* pImageDescInfo, int32_t nRedX, int32_t nRedY, int32_t nGreenX,
		int32_t nGreenY, int32_t nBlueX, int32_t nBlueY, int32_t nWhiteX, int32_t nWhiteY
	);
	void Wayland_WPImageDescriptionInfo_PrimariesNamed(
		wp_image_description_info_v1* pImageDescInfo, uint32_t uPrimaries
	);
	void Wayland_WPImageDescriptionInfo_TFPower(
		wp_image_description_info_v1* pImageDescInfo, uint32_t uExp
	);
	void Wayland_WPImageDescriptionInfo_TFNamed(
		wp_image_description_info_v1* pImageDescInfo, uint32_t uTF
	);
	void Wayland_WPImageDescriptionInfo_Luminances(
		wp_image_description_info_v1* pImageDescInfo, uint32_t uMinLum, uint32_t uMaxLum,
		uint32_t uRefLum
	);
	void Wayland_WPImageDescriptionInfo_TargetPrimaries(
		wp_image_description_info_v1* pImageDescInfo, int32_t nRedX, int32_t nRedY, int32_t nGreenX,
		int32_t nGreenY, int32_t nBlueX, int32_t nBlueY, int32_t nWhiteX, int32_t nWhiteY
	);
	void Wayland_WPImageDescriptionInfo_TargetLuminance(
		wp_image_description_info_v1* pImageDescInfo, uint32_t uMinLum, uint32_t uMaxLum
	);
	void Wayland_WPImageDescriptionInfo_Target_MaxCLL(
		wp_image_description_info_v1* pImageDescInfo, uint32_t uMaxCLL
	);
	void Wayland_WPImageDescriptionInfo_Target_MaxFALL(
		wp_image_description_info_v1* pImageDescInfo, uint32_t uMaxFALL
	);
	static const wp_image_description_info_v1_listener s_ImageDescriptionInfoListener;

	void Wayland_FractionalScale_PreferredScale(
		wp_fractional_scale_v1* pFractionalScale, uint32_t uScale
	);
	static const wp_fractional_scale_v1_listener s_FractionalScaleListener;

	CWaylandConnector* m_pConnector = nullptr;
	CWaylandBackend* m_pBackend = nullptr;

	CWaylandPlane* m_pParent = nullptr;
	wl_surface* m_pSurface = nullptr;
	wp_viewport* m_pViewport = nullptr;
	frog_color_managed_surface* m_pFrogColorManagedSurface = nullptr;
	wp_color_management_surface_v1* m_pWPColorManagedSurface = nullptr;
	wp_color_management_surface_feedback_v1* m_pWPColorManagedSurfaceFeedback = nullptr;
	wp_fractional_scale_v1* m_pFractionalScale = nullptr;
	wl_subsurface* m_pSubsurface = nullptr;
	libdecor_frame* m_pFrame = nullptr;
	libdecor_window_state m_eWindowState = LIBDECOR_WINDOW_STATE_NONE;
	std::vector<wl_output*> m_pOutputs;
	bool m_bNeedsDecorCommit = false;
	uint32_t m_uFractionalScale = 120;
	bool m_bHasRecievedScale = false;

	std::optional<WaylandPlaneColorState> m_ColorState{};
	float m_flPreviousSaturationScale = 1.0f;
	wp_image_description_v1* m_pCurrentImageDescription = nullptr;

	std::mutex m_PlaneStateLock;
	std::optional<WaylandPlaneState> m_oCurrentPlaneState;
};

} // namespace gamescope
