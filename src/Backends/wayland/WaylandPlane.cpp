#include "WaylandPlane.hpp"

#include "WaylandBackend.hpp"
#include "WaylandConnector.hpp"
#include "WaylandFb.hpp"
#include "callback_macro.hpp"
#include "externs_gs.hpp"
#include "frac_scale.hpp"
#include "libdecor_utils.hpp"
#include "refresh_rate.h"
#include "steamcompmgr.hpp"
#include "tag_identify.hpp"
#include "tags.hpp"
#include "vblankmanager.hpp"
#include "xdg_log.hpp"

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

gamescope::ConVar<float> cv_wayland_hdr10_saturation_scale(
	"wayland_hdr10_saturation_scale", 1.0,
	"Saturation scale for HDR10 content by gamut expansion. 1.0 - 1.2 is a good range to play with."
);

CWaylandPlane::CWaylandPlane(CWaylandConnector* pConnector)
	: m_pConnector{pConnector}, m_pBackend{pConnector->GetBackend()} {}

CWaylandPlane::~CWaylandPlane() {
	std::scoped_lock lock{m_PlaneStateLock};

	m_eWindowState = LIBDECOR_WINDOW_STATE_NONE;
	m_pOutputs.clear();
	m_bNeedsDecorCommit = false;

	m_oCurrentPlaneState = std::nullopt;

	if (m_pFrame)
		libdecor_frame_unref(m_pFrame); // Ew.

	if (m_pSubsurface)
		wl_subsurface_destroy(m_pSubsurface);
	if (m_pFractionalScale)
		wp_fractional_scale_v1_destroy(m_pFractionalScale);
	if (m_pWPColorManagedSurface)
		wp_color_management_surface_v1_destroy(m_pWPColorManagedSurface);
	if (m_pWPColorManagedSurfaceFeedback)
		wp_color_management_surface_feedback_v1_destroy(m_pWPColorManagedSurfaceFeedback);
	if (m_pFrogColorManagedSurface)
		frog_color_managed_surface_destroy(m_pFrogColorManagedSurface);
	if (m_pViewport)
		wp_viewport_destroy(m_pViewport);
	if (m_pSurface)
		wl_surface_destroy(m_pSurface);
}

bool CWaylandPlane::Init(CWaylandPlane* pParent, CWaylandPlane* pSiblingBelow) {
	m_pParent = pParent;
	m_pSurface = wl_compositor_create_surface(m_pBackend->GetCompositor());
	wl_surface_set_user_data(m_pSurface, this);
	wl_surface_add_listener(m_pSurface, &s_SurfaceListener, this);

	m_pViewport = wp_viewporter_get_viewport(m_pBackend->GetViewporter(), m_pSurface);

	if (m_pBackend->GetWPColorManager()) {
		m_pWPColorManagedSurface =
			wp_color_manager_v1_get_surface(m_pBackend->GetWPColorManager(), m_pSurface);
		m_pWPColorManagedSurfaceFeedback =
			wp_color_manager_v1_get_surface_feedback(m_pBackend->GetWPColorManager(), m_pSurface);

		// Only add the listener for the toplevel to avoid useless spam.
		if (!pParent)
			wp_color_management_surface_feedback_v1_add_listener(
				m_pWPColorManagedSurfaceFeedback, &s_WPColorManagementSurfaceListener, this
			);

		UpdateWPPreferredColorManagement();
	} else if (m_pBackend->GetFrogColorManagementFactory()) {
		m_pFrogColorManagedSurface = frog_color_management_factory_v1_get_color_managed_surface(
			m_pBackend->GetFrogColorManagementFactory(), m_pSurface
		);

		// Only add the listener for the toplevel to avoid useless spam.
		if (!pParent)
			frog_color_managed_surface_add_listener(
				m_pFrogColorManagedSurface, &s_FrogColorManagedSurfaceListener, this
			);
	}

	if (m_pBackend->GetFractionalScaleManager()) {
		m_pFractionalScale = wp_fractional_scale_manager_v1_get_fractional_scale(
			m_pBackend->GetFractionalScaleManager(), m_pSurface
		);

		if (!pParent)
			wp_fractional_scale_v1_add_listener(
				m_pFractionalScale, &s_FractionalScaleListener, this
			);
	}

	if (!pParent) {
		wl_proxy_set_tag((wl_proxy*)m_pSurface, &GAMESCOPE_toplevel_tag);
		m_pFrame = libdecor_decorate(
			m_pBackend->GetLibDecor(), m_pSurface, &s_LibDecorFrameInterface, this
		);
		libdecor_frame_set_title(m_pFrame, "Gamescope");
		libdecor_frame_set_app_id(m_pFrame, "gamescope");
		libdecor_frame_map(m_pFrame);
	} else {
		wl_proxy_set_tag((wl_proxy*)m_pSurface, &GAMESCOPE_plane_tag);
		m_pSubsurface = wl_subcompositor_get_subsurface(
			m_pBackend->GetSubcompositor(), m_pSurface, pParent->GetSurface()
		);
		wl_subsurface_place_above(m_pSubsurface, pSiblingBelow->GetSurface());
		wl_subsurface_set_sync(m_pSubsurface);
		// Allow pParent to receive input while covered by subsurface planes
		wl_surface_set_input_region(m_pSurface, m_pBackend->GetEmptyRegion());
	}

	wl_surface_commit(m_pSurface);
	wl_display_roundtrip(m_pBackend->GetDisplay());

	if (m_pFrame)
		libdecor_frame_set_visibility(m_pFrame, !g_bBorderlessOutputWindow);

	return true;
}

uint32_t CWaylandPlane::GetScale() const {
	if (m_pParent)
		return m_pParent->GetScale();

	return m_uFractionalScale;
}

void CWaylandPlane::Present(std::optional<WaylandPlaneState> oState) {
	{
		std::unique_lock lock(m_PlaneStateLock);
		m_oCurrentPlaneState = oState;
	}

	if (oState) {
		assert(oState->pBuffer);

		if (m_pFrame) {
			struct wp_presentation_feedback* pFeedback =
				wp_presentation_feedback(m_pBackend->GetPresentation(), m_pSurface);
			wp_presentation_feedback_add_listener(pFeedback, &s_PresentationFeedbackListener, this);
		}

		if (m_pWPColorManagedSurface) {
			WaylandPlaneColorState colorState = {
				.eColorspace = oState->eColorspace,
				.pHDRMetadata = oState->pHDRMetadata,
			};

			float flScale = cv_wayland_hdr10_saturation_scale;

			if (!m_ColorState || *m_ColorState != colorState ||
				m_flPreviousSaturationScale != flScale) {
				m_ColorState = colorState;
				m_flPreviousSaturationScale = flScale;

				if (m_pCurrentImageDescription) {
					wp_image_description_v1_destroy(m_pCurrentImageDescription);
					m_pCurrentImageDescription = nullptr;
				}

				if (oState->eColorspace == GAMESCOPE_APP_TEXTURE_COLORSPACE_SCRGB) {
					m_pCurrentImageDescription =
						wp_color_manager_v1_create_windows_scrgb(m_pBackend->GetWPColorManager());
				} else if (oState->eColorspace == GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ) {
					wp_image_description_creator_params_v1* pParams =
						wp_color_manager_v1_create_parametric_creator(
							m_pBackend->GetWPColorManager()
						);

					if (close_enough(flScale, 1.0f)) {
						wp_image_description_creator_params_v1_set_primaries_named(
							pParams, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020
						);
					} else {
						wp_image_description_creator_params_v1_set_primaries(
							pParams, (int32_t)(0.708 * flScale * 1'000'000.0),
							(int32_t)(0.292 / flScale * 1'000'000.0),
							(int32_t)(0.170 / flScale * 1'000'000.0),
							(int32_t)(0.797 * flScale * 1'000'000.0),
							(int32_t)(0.131 / flScale * 1'000'000.0),
							(int32_t)(0.046 / flScale * 1'000'000.0),
							(int32_t)(0.3127 * 1'000'000.0), (int32_t)(0.3290 * 1'000'000.0)
						);
					}
					wp_image_description_creator_params_v1_set_tf_named(
						pParams, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ
					);
					if (m_ColorState->pHDRMetadata) {
						const hdr_metadata_infoframe* pInfoframe =
							&m_ColorState->pHDRMetadata->View<hdr_output_metadata>()
								 .hdmi_metadata_type1;

						wp_image_description_creator_params_v1_set_mastering_display_primaries(
							pParams,
							// Rescale...
							(((int32_t)pInfoframe->display_primaries[0].x) * 1'000'000) / 0xC350,
							(((int32_t)pInfoframe->display_primaries[0].y) * 1'000'000) / 0xC350,
							(((int32_t)pInfoframe->display_primaries[1].x) * 1'000'000) / 0xC350,
							(((int32_t)pInfoframe->display_primaries[1].y) * 1'000'000) / 0xC350,
							(((int32_t)pInfoframe->display_primaries[2].x) * 1'000'000) / 0xC350,
							(((int32_t)pInfoframe->display_primaries[2].y) * 1'000'000) / 0xC350,
							(((int32_t)pInfoframe->white_point.x) * 1'000'000) / 0xC350,
							(((int32_t)pInfoframe->white_point.y) * 1'000'000) / 0xC350
						);

						wp_image_description_creator_params_v1_set_mastering_luminance(
							pParams, pInfoframe->min_display_mastering_luminance,
							pInfoframe->max_display_mastering_luminance
						);

						wp_image_description_creator_params_v1_set_max_cll(
							pParams, pInfoframe->max_cll
						);

						wp_image_description_creator_params_v1_set_max_fall(
							pParams, pInfoframe->max_fall
						);
					}
					m_pCurrentImageDescription =
						wp_image_description_creator_params_v1_create(pParams);
				}
			}

			if (m_pCurrentImageDescription) {
				wp_color_management_surface_v1_set_image_description(
					m_pWPColorManagedSurface, m_pCurrentImageDescription,
					WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL
				);
			} else {
				wp_color_management_surface_v1_unset_image_description(m_pWPColorManagedSurface);
			}
		} else if (m_pFrogColorManagedSurface) {
			frog_color_managed_surface_set_render_intent(
				m_pFrogColorManagedSurface, FROG_COLOR_MANAGED_SURFACE_RENDER_INTENT_PERCEPTUAL
			);
			switch (oState->eColorspace) {
			default:
			case GAMESCOPE_APP_TEXTURE_COLORSPACE_PASSTHRU:
				frog_color_managed_surface_set_known_container_color_volume(
					m_pFrogColorManagedSurface, FROG_COLOR_MANAGED_SURFACE_PRIMARIES_UNDEFINED
				);
				frog_color_managed_surface_set_known_container_color_volume(
					m_pFrogColorManagedSurface,
					FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_UNDEFINED
				);
				break;
			case GAMESCOPE_APP_TEXTURE_COLORSPACE_LINEAR:
			case GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB:
				frog_color_managed_surface_set_known_container_color_volume(
					m_pFrogColorManagedSurface, FROG_COLOR_MANAGED_SURFACE_PRIMARIES_REC709
				);
				frog_color_managed_surface_set_known_transfer_function(
					m_pFrogColorManagedSurface,
					FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_GAMMA_22
				);
				break;
			case GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ:
				frog_color_managed_surface_set_known_container_color_volume(
					m_pFrogColorManagedSurface, FROG_COLOR_MANAGED_SURFACE_PRIMARIES_REC2020
				);
				frog_color_managed_surface_set_known_transfer_function(
					m_pFrogColorManagedSurface,
					FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_ST2084_PQ
				);
				break;
			case GAMESCOPE_APP_TEXTURE_COLORSPACE_SCRGB:
				frog_color_managed_surface_set_known_container_color_volume(
					m_pFrogColorManagedSurface, FROG_COLOR_MANAGED_SURFACE_PRIMARIES_REC709
				);
				frog_color_managed_surface_set_known_transfer_function(
					m_pFrogColorManagedSurface,
					FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_SCRGB_LINEAR
				);
				break;
			}
		}

		// Fraction with denominator of 120 per. spec
		const uint32_t uScale = oState->uFractionalScale;

		wp_viewport_set_source(
			m_pViewport, wl_fixed_from_double(oState->flSrcX), wl_fixed_from_double(oState->flSrcY),
			wl_fixed_from_double(oState->flSrcWidth), wl_fixed_from_double(oState->flSrcHeight)
		);
		wp_viewport_set_destination(
			m_pViewport, WaylandScaleToLogical(oState->nDstWidth, uScale),
			WaylandScaleToLogical(oState->nDstHeight, uScale)
		);

		if (m_pSubsurface) {
			wl_subsurface_set_position(
				m_pSubsurface, WaylandScaleToLogical(oState->nDestX, uScale),
				WaylandScaleToLogical(oState->nDestY, uScale)
			);
		}
		// The x/y here does nothing? Why? What is it for...
		// Use the subsurface set_position thing instead.
		wl_surface_attach(m_pSurface, oState->pBuffer, 0, 0);
		wl_surface_damage(m_pSurface, 0, 0, INT32_MAX, INT32_MAX);
		wl_surface_set_opaque_region(
			m_pSurface, oState->bOpaque ? m_pBackend->GetFullRegion() : nullptr
		);
		wl_surface_set_buffer_scale(m_pSurface, 1);
	} else {
		wl_surface_attach(m_pSurface, nullptr, 0, 0);
		wl_surface_damage(m_pSurface, 0, 0, INT32_MAX, INT32_MAX);
	}
}

void CWaylandPlane::CommitLibDecor(libdecor_configuration* pConfiguration) {
	int32_t uScale = GetScale();
	libdecor_state* pState = libdecor_state_new(
		WaylandScaleToLogical(g_nOutputWidth, uScale),
		WaylandScaleToLogical(g_nOutputHeight, uScale)
	);
	libdecor_frame_commit(m_pFrame, pState, pConfiguration);
	libdecor_state_free(pState);
}

void CWaylandPlane::Commit() {
	if (m_bNeedsDecorCommit) {
		CommitLibDecor(nullptr);
		m_bNeedsDecorCommit = false;
	}

	wl_surface_commit(m_pSurface);
}

xdg_toplevel* CWaylandPlane::GetXdgToplevel() const {
	if (!m_pFrame)
		return nullptr;

	return libdecor_frame_get_xdg_toplevel(m_pFrame);
}

void CWaylandPlane::Present(const FrameInfo_t::Layer_t* pLayer) {
	CWaylandFb* pWaylandFb =
		pLayer && pLayer->tex != nullptr
			? static_cast<CWaylandFb*>(pLayer->tex->GetBackendFb()->EnsureImported())
			: nullptr;
	wl_buffer* pBuffer = pWaylandFb ? pWaylandFb->GetHostBuffer() : nullptr;

	if (pBuffer) {
		pWaylandFb->OnCompositorAcquire();

		Present(ClipPlane(
			WaylandPlaneState{
				.pBuffer = pBuffer,
				.nDestX = int32_t(-pLayer->offset.x),
				.nDestY = int32_t(-pLayer->offset.y),
				.flSrcX = 0.0,
				.flSrcY = 0.0,
				.flSrcWidth = double(pLayer->tex->width()),
				.flSrcHeight = double(pLayer->tex->height()),
				.nDstWidth = int32_t(ceil(pLayer->tex->width() / double(pLayer->scale.x))),
				.nDstHeight = int32_t(ceil(pLayer->tex->height() / double(pLayer->scale.y))),
				.eColorspace = pLayer->colorspace,
				.pHDRMetadata = pLayer->hdr_metadata_blob,
				.bOpaque = pLayer->zpos == g_zposBase,
				.uFractionalScale = GetScale(),
			}
		));
	} else {
		Present(std::nullopt);
	}
}

void CWaylandPlane::UpdateVRRRefreshRate() {
	if (m_pParent)
		return;

	if (!m_pConnector->HostCompositorIsCurrentlyVRR())
		return;

	if (m_pOutputs.empty())
		return;

	int32_t nLargestRefreshRateMhz = 0;
	for (wl_output* pOutput : m_pOutputs) {
		WaylandOutputInfo* pOutputInfo = m_pBackend->GetOutputInfo(pOutput);
		if (!pOutputInfo)
			continue;

		nLargestRefreshRateMhz = std::max(nLargestRefreshRateMhz, pOutputInfo->nRefresh);
	}

	if (nLargestRefreshRateMhz && nLargestRefreshRateMhz != g_nOutputRefresh) {
		// TODO(strategy): We should pick the largest refresh rate.
		xdg_log.infof("Changed refresh to: %.3fhz", ConvertmHzToHz((float)nLargestRefreshRateMhz));
		g_nOutputRefresh = nLargestRefreshRateMhz;
	}
}

void CWaylandPlane::Wayland_Surface_Enter(wl_surface* pSurface, wl_output* pOutput) {
	if (!IsGamescopeToplevel(pSurface))
		return;

	m_pOutputs.emplace_back(pOutput);

	UpdateVRRRefreshRate();
}
void CWaylandPlane::Wayland_Surface_Leave(wl_surface* pSurface, wl_output* pOutput) {
	if (!IsGamescopeToplevel(pSurface))
		return;

	std::erase(m_pOutputs, pOutput);

	UpdateVRRRefreshRate();
}

void CWaylandPlane::LibDecor_Frame_Configure(
	libdecor_frame* pFrame, libdecor_configuration* pConfiguration
) {
	if (!libdecor_configuration_get_window_state(pConfiguration, &m_eWindowState))
		m_eWindowState = LIBDECOR_WINDOW_STATE_NONE;

	int32_t uScale = GetScale();

	int nWidth, nHeight;
	if (!libdecor_configuration_get_content_size(pConfiguration, m_pFrame, &nWidth, &nHeight)) {
		// XXX(virtual connector): Move g_nOutputWidth etc to connector.
		// Right now we are doubling this up when we should not be.
		//
		// Which is causing problems.
		nWidth = WaylandScaleToLogical(g_nOutputWidth, uScale);
		nHeight = WaylandScaleToLogical(g_nOutputHeight, uScale);
	}
	g_nOutputWidth = WaylandScaleToPhysical(nWidth, uScale);
	g_nOutputHeight = WaylandScaleToPhysical(nHeight, uScale);

	CommitLibDecor(pConfiguration);

	force_repaint();
}
void CWaylandPlane::LibDecor_Frame_Close(libdecor_frame* pFrame) { raise(SIGTERM); }
void CWaylandPlane::LibDecor_Frame_Commit(libdecor_frame* pFrame) {
	m_bNeedsDecorCommit = true;
	force_repaint();
}
void CWaylandPlane::LibDecor_Frame_DismissPopup(libdecor_frame* pFrame, const char* pSeatName) {}

void CWaylandPlane::Wayland_PresentationFeedback_SyncOutput(
	struct wp_presentation_feedback* pFeedback, wl_output* pOutput
) {}
void CWaylandPlane::Wayland_PresentationFeedback_Presented(
	struct wp_presentation_feedback* pFeedback, uint32_t uTVSecHi, uint32_t uTVSecLo,
	uint32_t uTVNSec, uint32_t uRefreshCycle, uint32_t uSeqHi, uint32_t uSeqLo, uint32_t uFlags
) {
	uint64_t ulTime =
		(((uint64_t(uTVSecHi) << 32ul) | uTVSecLo) * 1'000'000'000lu) + (uint64_t(uTVNSec));

	if (uRefreshCycle) {
		int32_t nRefresh = RefreshCycleTomHz(uRefreshCycle);
		if (nRefresh && nRefresh != g_nOutputRefresh) {
			xdg_log.infof("Changed refresh to: %.3fhz", ConvertmHzToHz((float)nRefresh));
			g_nOutputRefresh = nRefresh;
		}

		m_pConnector->SetHostCompositorIsCurrentlyVRR(false);
	} else {
		m_pConnector->SetHostCompositorIsCurrentlyVRR(true);

		UpdateVRRRefreshRate();
	}

	GetVBlankTimer().MarkVBlank(ulTime, true);
	wp_presentation_feedback_destroy(pFeedback);

	// Nudge so that steamcompmgr releases commits.
	nudge_steamcompmgr();
}
void CWaylandPlane::Wayland_PresentationFeedback_Discarded(
	struct wp_presentation_feedback* pFeedback
) {
	wp_presentation_feedback_destroy(pFeedback);

	// Nudge so that steamcompmgr releases commits.
	nudge_steamcompmgr();
}

void CWaylandPlane::Wayland_FrogColorManagedSurface_PreferredMetadata(
	frog_color_managed_surface* pFrogSurface, uint32_t uTransferFunction,
	uint32_t uOutputDisplayPrimaryRedX, uint32_t uOutputDisplayPrimaryRedY,
	uint32_t uOutputDisplayPrimaryGreenX, uint32_t uOutputDisplayPrimaryGreenY,
	uint32_t uOutputDisplayPrimaryBlueX, uint32_t uOutputDisplayPrimaryBlueY,
	uint32_t uOutputWhitePointX, uint32_t uOutputWhitePointY, uint32_t uMaxLuminance,
	uint32_t uMinLuminance, uint32_t uMaxFullFrameLuminance
) {
	auto* pHDRInfo = &m_pConnector->m_HDRInfo;
	pHDRInfo->bExposeHDRSupport =
		(cv_hdr_enabled &&
		 uTransferFunction == FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_ST2084_PQ);
	pHDRInfo->eOutputEncodingEOTF =
		(cv_hdr_enabled &&
		 uTransferFunction == FROG_COLOR_MANAGED_SURFACE_TRANSFER_FUNCTION_ST2084_PQ)
			? EOTF_PQ
			: EOTF_Gamma22;
	pHDRInfo->uMaxContentLightLevel = uMaxLuminance;
	pHDRInfo->uMaxFrameAverageLuminance = uMaxFullFrameLuminance;
	pHDRInfo->uMinContentLightLevel = uMinLuminance;

	auto* pDisplayColorimetry = &m_pConnector->m_DisplayColorimetry;
	pDisplayColorimetry->primaries.r =
		glm::vec2{uOutputDisplayPrimaryRedX * 0.00002f, uOutputDisplayPrimaryRedY * 0.00002f};
	pDisplayColorimetry->primaries.g =
		glm::vec2{uOutputDisplayPrimaryGreenX * 0.00002f, uOutputDisplayPrimaryGreenY * 0.00002f};
	pDisplayColorimetry->primaries.b =
		glm::vec2{uOutputDisplayPrimaryBlueX * 0.00002f, uOutputDisplayPrimaryBlueY * 0.00002f};
	pDisplayColorimetry->white =
		glm::vec2{uOutputWhitePointX * 0.00002f, uOutputWhitePointY * 0.00002f};

	xdg_log.infof(
		"PreferredMetadata: Red: %g %g, Green: %g %g, Blue: %g %g, White: %g %g, Max Luminance: %u "
		"nits, Min Luminance: %g nits, Max Full Frame Luminance: %u nits",
		uOutputDisplayPrimaryRedX * 0.00002, uOutputDisplayPrimaryRedY * 0.00002,
		uOutputDisplayPrimaryGreenX * 0.00002, uOutputDisplayPrimaryGreenY * 0.00002,
		uOutputDisplayPrimaryBlueX * 0.00002, uOutputDisplayPrimaryBlueY * 0.00002,
		uOutputWhitePointX * 0.00002, uOutputWhitePointY * 0.00002, uint32_t(uMaxLuminance),
		uMinLuminance * 0.0001, uint32_t(uMaxFullFrameLuminance)
	);
}

//

void CWaylandPlane::Wayland_WPColorManagementSurfaceFeedback_PreferredChanged(
	wp_color_management_surface_feedback_v1* pColorManagementSurface, unsigned int data
) {
	UpdateWPPreferredColorManagement();
}

void CWaylandPlane::UpdateWPPreferredColorManagement() {
	if (m_pParent)
		return;

	wp_image_description_v1* pImageDescription =
		wp_color_management_surface_feedback_v1_get_preferred(m_pWPColorManagedSurfaceFeedback);
	wp_image_description_info_v1* pImageDescInfo =
		wp_image_description_v1_get_information(pImageDescription);
	wp_image_description_info_v1_add_listener(
		pImageDescInfo, &s_ImageDescriptionInfoListener, this
	);
	wl_display_roundtrip(m_pBackend->GetDisplay());

	wp_image_description_info_v1_destroy(pImageDescInfo);
	wp_image_description_v1_destroy(pImageDescription);
}

void CWaylandPlane::Wayland_WPImageDescriptionInfo_Done(
	wp_image_description_info_v1* pImageDescInfo
) {
	auto* pHDRInfo = &m_pConnector->m_HDRInfo;
	if (m_pBackend->SupportsColorManagement()) {
		pHDRInfo->bExposeHDRSupport =
			(cv_hdr_enabled &&
			 m_pConnector->m_uMaxTargetLuminance > m_pConnector->m_uReferenceLuminance);
		pHDRInfo->eOutputEncodingEOTF = pHDRInfo->bExposeHDRSupport ? EOTF_PQ : EOTF_Gamma22;
	}

	xdg_log.infof("HDR INFO");
	xdg_log.infof("  cv_hdr_enabled: %s", cv_hdr_enabled ? "true" : "false");
	xdg_log.infof(
		"  uMaxLum: %u, uRefLum: %u", m_pConnector->m_uMaxTargetLuminance,
		m_pConnector->m_uReferenceLuminance
	);
	xdg_log.infof("  bExposeHDRSupport: %s", pHDRInfo->bExposeHDRSupport ? "true" : "false");
}
void CWaylandPlane::Wayland_WPImageDescriptionInfo_ICCFile(
	wp_image_description_info_v1* pImageDescInfo, int32_t nICCFd, uint32_t uICCSize
) {
	if (nICCFd >= 0)
		close(nICCFd);
}
void CWaylandPlane::Wayland_WPImageDescriptionInfo_Primaries(
	wp_image_description_info_v1* pImageDescInfo, int32_t nRedX, int32_t nRedY, int32_t nGreenX,
	int32_t nGreenY, int32_t nBlueX, int32_t nBlueY, int32_t nWhiteX, int32_t nWhiteY
) {}
void CWaylandPlane::Wayland_WPImageDescriptionInfo_PrimariesNamed(
	wp_image_description_info_v1* pImageDescInfo, uint32_t uPrimaries
) {}
void CWaylandPlane::Wayland_WPImageDescriptionInfo_TFPower(
	wp_image_description_info_v1* pImageDescInfo, uint32_t uExp
) {}
void CWaylandPlane::Wayland_WPImageDescriptionInfo_TFNamed(
	wp_image_description_info_v1* pImageDescInfo, uint32_t uTF
) {}
void CWaylandPlane::Wayland_WPImageDescriptionInfo_Luminances(
	wp_image_description_info_v1* pImageDescInfo, uint32_t uMinLum, uint32_t uMaxLum,
	uint32_t uRefLum
) {
	m_pConnector->m_uReferenceLuminance = uRefLum;
}
void CWaylandPlane::Wayland_WPImageDescriptionInfo_TargetPrimaries(
	wp_image_description_info_v1* pImageDescInfo, int32_t nRedX, int32_t nRedY, int32_t nGreenX,
	int32_t nGreenY, int32_t nBlueX, int32_t nBlueY, int32_t nWhiteX, int32_t nWhiteY
) {
	auto* pDisplayColorimetry = &m_pConnector->m_DisplayColorimetry;
	pDisplayColorimetry->primaries.r = glm::vec2{nRedX / 1000000.0f, nRedY / 1000000.0f};
	pDisplayColorimetry->primaries.g = glm::vec2{nGreenX / 1000000.0f, nGreenY / 1000000.0f};
	pDisplayColorimetry->primaries.b = glm::vec2{nBlueX / 1000000.0f, nBlueY / 1000000.0f};
	pDisplayColorimetry->white = glm::vec2{nWhiteX / 1000000.0f, nWhiteY / 1000000.0f};
}
void CWaylandPlane::Wayland_WPImageDescriptionInfo_TargetLuminance(
	wp_image_description_info_v1* pImageDescInfo, uint32_t uMinLum, uint32_t uMaxLum
) {
	m_pConnector->m_uMaxTargetLuminance = uMaxLum;
}
void CWaylandPlane::Wayland_WPImageDescriptionInfo_Target_MaxCLL(
	wp_image_description_info_v1* pImageDescInfo, uint32_t uMaxCLL
) {
	auto* pHDRInfo = &m_pConnector->m_HDRInfo;
	pHDRInfo->uMaxContentLightLevel = uMaxCLL;
	xdg_log.infof("uMaxContentLightLevel: %u", uMaxCLL);
}
void CWaylandPlane::Wayland_WPImageDescriptionInfo_Target_MaxFALL(
	wp_image_description_info_v1* pImageDescInfo, uint32_t uMaxFALL
) {
	auto* pHDRInfo = &m_pConnector->m_HDRInfo;
	pHDRInfo->uMaxFrameAverageLuminance = uMaxFALL;
}

//

void CWaylandPlane::Wayland_FractionalScale_PreferredScale(
	wp_fractional_scale_v1* pFractionalScale, uint32_t uScale
) {
	bool bDirty = false;

	static uint32_t s_uGlobalFractionalScale = 120;
	if (s_uGlobalFractionalScale != uScale) {
		if (m_bHasRecievedScale) {
			g_nOutputWidth = (g_nOutputWidth * uScale) / m_uFractionalScale;
			g_nOutputHeight = (g_nOutputHeight * uScale) / m_uFractionalScale;
		}

		s_uGlobalFractionalScale = uScale;
		bDirty = true;
	}

	if (m_uFractionalScale != uScale) {
		m_uFractionalScale = uScale;
		bDirty = true;
	}

	m_bHasRecievedScale = true;

	if (bDirty)
		force_repaint();
}

} // namespace gamescope
