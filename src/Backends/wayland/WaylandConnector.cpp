#include "WaylandConnector.hpp"

#include "CreateShmBuffer.hpp"
#include "Utils/Defer.h"
#include "WaylandBackend.hpp"
#include "WaylandFb.hpp"
#include "edid.h"
#include "externs_core.hpp"
#include "externs_gs.hpp"
#include "externs_std.hpp"
#include "steamcompmgr.hpp"
#include "vblankmanager.hpp"
#include "xdg_log.hpp"

namespace gamescope {

CWaylandConnector::CWaylandConnector(CWaylandBackend* pBackend, uint64_t ulVirtualConnectorKey)
	: CBaseBackendConnector{ulVirtualConnectorKey}, m_pBackend(pBackend),
	  m_Planes{this, this, this, this, this, this, this, this} {
	m_HDRInfo.bAlwaysPatchEdid = true;
}

CWaylandConnector::~CWaylandConnector() { m_pBackend->OnConnectorDestroyed(this); }

bool CWaylandConnector::UpdateEdid() {
	m_FakeEdid = GenerateSimpleEdid(g_nNestedWidth, g_nNestedHeight);

	return true;
}

bool CWaylandConnector::Init() {
	for (uint32_t i = 0; i < 8; i++) {
		bool bSuccess =
			m_Planes[i].Init(i == 0 ? nullptr : &m_Planes[0], i == 0 ? nullptr : &m_Planes[i - 1]);
		if (!bSuccess)
			return false;
	}

	if (g_bFullscreen) {
		m_bDesiredFullscreenState = true;
		g_bFullscreen = false;
		UpdateFullscreenState();
	}

	UpdateEdid();
	m_pBackend->HackUpdatePatchedEdid();

	if (g_bForceRelativeMouse)
		this->SetRelativeMouseMode(true);

	return true;
}

void CWaylandConnector::SetFullscreen(bool bFullscreen) { m_bDesiredFullscreenState = bFullscreen; }

void CWaylandConnector::UpdateFullscreenState() {
	if (!m_bVisible)
		g_bFullscreen = false;

	if (m_bDesiredFullscreenState != g_bFullscreen && m_bVisible) {
		if (m_bDesiredFullscreenState)
			libdecor_frame_set_fullscreen(m_Planes[0].GetFrame(), nullptr);
		else
			libdecor_frame_unset_fullscreen(m_Planes[0].GetFrame());

		g_bFullscreen = m_bDesiredFullscreenState;
	}
}

int CWaylandConnector::Present(const FrameInfo_t* pFrameInfo, bool bAsync) {
	UpdateFullscreenState();

	bool bNeedsFullComposite = false;

	if (!m_bVisible) {
		uint32_t uCurrentPlane = 0;
		for (int i = 0; i < 8 && uCurrentPlane < 8; i++)
			m_Planes[uCurrentPlane++].Present(nullptr);
	} else {
		// TODO: Dedupe some of this composite check code between us and drm.cpp
		bool bLayer0ScreenSize = close_enough(pFrameInfo->layers[0].scale.x, 1.0f) &&
								 close_enough(pFrameInfo->layers[0].scale.y, 1.0f);

		bool bNeedsCompositeFromFilter = (g_upscaleFilter == GamescopeUpscaleFilter::NEAREST ||
										  g_upscaleFilter == GamescopeUpscaleFilter::PIXEL) &&
										 !bLayer0ScreenSize;

		bNeedsFullComposite |= cv_composite_force;
		bNeedsFullComposite |= pFrameInfo->useFSRLayer0;
		bNeedsFullComposite |= pFrameInfo->useNISLayer0;
		bNeedsFullComposite |= pFrameInfo->blurLayer0;
		bNeedsFullComposite |= bNeedsCompositeFromFilter;
		bNeedsFullComposite |= g_bColorSliderInUse;
		bNeedsFullComposite |= pFrameInfo->bFadingOut;
		bNeedsFullComposite |= !g_reshade_effect.empty();

		if (g_bOutputHDREnabled)
			bNeedsFullComposite |= g_bHDRItmEnable;

		if (!m_pBackend->SupportsColorManagement())
			bNeedsFullComposite |= ColorspaceIsHDR(pFrameInfo->layers[0].colorspace);

		bNeedsFullComposite |= !!(g_uCompositeDebug & CompositeDebugFlag::Heatmap);

		if (!bNeedsFullComposite) {
			bool bNeedsBacking = true;
			if (pFrameInfo->layerCount >= 1) {
				if (pFrameInfo->layers[0].isScreenSize() && !pFrameInfo->layers[0].hasAlpha())
					bNeedsBacking = false;
			}

			uint32_t uCurrentPlane = 0;
			if (bNeedsBacking) {
				m_pBackend->GetBlackFb()->OnCompositorAcquire();

				CWaylandPlane* pPlane = &m_Planes[uCurrentPlane++];
				pPlane->Present(
					WaylandPlaneState{
						.pBuffer = m_pBackend->GetBlackFb()->GetHostBuffer(),
						.flSrcWidth = 1.0,
						.flSrcHeight = 1.0,
						.nDstWidth = int32_t(g_nOutputWidth),
						.nDstHeight = int32_t(g_nOutputHeight),
						.eColorspace = GAMESCOPE_APP_TEXTURE_COLORSPACE_PASSTHRU,
						.bOpaque = true,
						.uFractionalScale = pPlane->GetScale(),
					}
				);
			}

			for (int i = 0; i < 8 && uCurrentPlane < 8; i++)
				m_Planes[uCurrentPlane++].Present(
					i < pFrameInfo->layerCount ? &pFrameInfo->layers[i] : nullptr
				);
		} else {
			std::optional oCompositeResult =
				vulkan_composite((FrameInfo_t*)pFrameInfo, nullptr, false);

			if (!oCompositeResult) {
				xdg_log.errorf("vulkan_composite failed");
				return -EINVAL;
			}

			vulkan_wait(*oCompositeResult, true);

			FrameInfo_t::Layer_t compositeLayer{};
			compositeLayer.scale.x = 1.0;
			compositeLayer.scale.y = 1.0;
			compositeLayer.opacity = 1.0;
			compositeLayer.zpos = g_zposBase;

			compositeLayer.tex = vulkan_get_last_output_image(false, false);
			compositeLayer.applyColorMgmt = false;

			compositeLayer.filter = GamescopeUpscaleFilter::NEAREST;
			compositeLayer.ctm = nullptr;
			compositeLayer.colorspace = pFrameInfo->outputEncodingEOTF == EOTF_PQ
											? GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ
											: GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB;

			m_Planes[0].Present(&compositeLayer);

			for (int i = 1; i < 8; i++)
				m_Planes[i].Present(nullptr);
		}
	}

	for (int i = 7; i >= 0; i--)
		m_Planes[i].Commit();

	wl_display_flush(m_pBackend->GetDisplay());

	GetVBlankTimer().UpdateWasCompositing(bNeedsFullComposite);
	GetVBlankTimer().UpdateLastDrawTime(
		get_time_in_nanos() - g_SteamCompMgrVBlankTime.ulWakeupTime
	);

	m_pBackend->PollState();

	return 0;
}

GamescopeScreenType CWaylandConnector::GetScreenType() const {
	return gamescope::GAMESCOPE_SCREEN_TYPE_INTERNAL;
}
GamescopePanelOrientation CWaylandConnector::GetCurrentOrientation() const {
	return GAMESCOPE_PANEL_ORIENTATION_0;
}
bool CWaylandConnector::SupportsHDR() const { return GetHDRInfo().IsHDR10(); }
bool CWaylandConnector::IsHDRActive() const {
	// XXX: blah
	return false;
}
const BackendConnectorHDRInfo& CWaylandConnector::GetHDRInfo() const { return m_HDRInfo; }
bool CWaylandConnector::IsVRRActive() const {
	return cv_adaptive_sync && m_bHostCompositorIsCurrentlyVRR;
}
std::span<const BackendMode> CWaylandConnector::GetModes() const {
	return std::span<const BackendMode>{};
}

bool CWaylandConnector::SupportsVRR() const { return CurrentDisplaySupportsVRR(); }

std::span<const uint8_t> CWaylandConnector::GetRawEDID() const {
	return std::span<const uint8_t>{m_FakeEdid.begin(), m_FakeEdid.end()};
}
std::span<const uint32_t> CWaylandConnector::GetValidDynamicRefreshRates() const {
	return std::span<const uint32_t>{};
}

void CWaylandConnector::GetNativeColorimetry(
	bool bHDR10, displaycolorimetry_t* displayColorimetry, EOTF* displayEOTF,
	displaycolorimetry_t* outputEncodingColorimetry, EOTF* outputEncodingEOTF
) const {
	*displayColorimetry = m_DisplayColorimetry;
	*displayEOTF = EOTF_Gamma22;

	if (bHDR10 && GetHDRInfo().IsHDR10()) {
		// For HDR10 output, expected content colorspace != native colorspace.
		*outputEncodingColorimetry = displaycolorimetry_2020;
		*outputEncodingEOTF = GetHDRInfo().eOutputEncodingEOTF;
	} else {
		// We always use default 'perceptual' intent, so
		// this should be correct for SDR content.
		*outputEncodingColorimetry = m_DisplayColorimetry;
		*outputEncodingEOTF = EOTF_Gamma22;
	}
}

void CWaylandConnector::SetCursorImage(std::shared_ptr<INestedHints::CursorInfo> info) {
	m_pBackend->SetCursorImage(std::move(info));
}
void CWaylandConnector::SetRelativeMouseMode(bool bRelative) {
	// TODO: Do more tracking across multiple connectors, and activity here if we ever want to use
	// this.
	m_pBackend->SetRelativeMouseMode(m_Planes[0].GetSurface(), bRelative);
}
void CWaylandConnector::SetVisible(bool bVisible) {
	if (m_bVisible == bVisible)
		return;

	m_bVisible = bVisible;
	force_repaint();
}
void CWaylandConnector::SetTitle(std::shared_ptr<std::string> pAppTitle) {
	std::string szTitle = pAppTitle ? *pAppTitle : "gamescope";
	if (g_bGrabbed)
		szTitle += " (grabbed)";
	libdecor_frame_set_title(m_Planes[0].GetFrame(), szTitle.c_str());
}
void CWaylandConnector::SetIcon(std::shared_ptr<std::vector<uint32_t>> uIconPixels) {
	if (!m_pBackend->GetToplevelIconManager())
		return;

	if (uIconPixels && uIconPixels->size() >= 3) {
		xdg_toplevel_icon_v1* pIcon =
			xdg_toplevel_icon_manager_v1_create_icon(m_pBackend->GetToplevelIconManager());
		if (!pIcon) {
			xdg_log.errorf("Failed to create xdg_toplevel_icon_v1");
			return;
		}
		defer(xdg_toplevel_icon_v1_destroy(pIcon));

		const uint32_t uWidth = (*uIconPixels)[0];
		const uint32_t uHeight = (*uIconPixels)[1];

		const uint32_t uStride = uWidth * 4;
		const uint32_t uSize = uStride * uHeight;
		int32_t nFd = CreateShmBuffer(uSize, &(*uIconPixels)[2]);
		if (nFd < 0) {
			xdg_log.errorf("Failed to create/map shm buffer");
			return;
		}
		defer(close(nFd));

		wl_shm_pool* pPool = wl_shm_create_pool(m_pBackend->GetShm(), nFd, uSize);
		defer(wl_shm_pool_destroy(pPool));

		wl_buffer* pBuffer =
			wl_shm_pool_create_buffer(pPool, 0, uWidth, uHeight, uStride, WL_SHM_FORMAT_ARGB8888);
		defer(wl_buffer_destroy(pBuffer));

		xdg_toplevel_icon_v1_add_buffer(pIcon, pBuffer, 1);

		xdg_toplevel_icon_manager_v1_set_icon(
			m_pBackend->GetToplevelIconManager(), m_Planes[0].GetXdgToplevel(), pIcon
		);
	} else {
		xdg_toplevel_icon_manager_v1_set_icon(
			m_pBackend->GetToplevelIconManager(), m_Planes[0].GetXdgToplevel(), nullptr
		);
	}
}

void CWaylandConnector::SetSelection(
	std::shared_ptr<std::string> szContents, GamescopeSelection eSelection
) {
	if (m_pBackend->m_pDataDeviceManager && !m_pBackend->m_pDataDevice)
		m_pBackend->m_pDataDevice = wl_data_device_manager_get_data_device(
			m_pBackend->m_pDataDeviceManager, m_pBackend->m_pSeat
		);

	if (m_pBackend->m_pPrimarySelectionDeviceManager && !m_pBackend->m_pPrimarySelectionDevice)
		m_pBackend->m_pPrimarySelectionDevice = zwp_primary_selection_device_manager_v1_get_device(
			m_pBackend->m_pPrimarySelectionDeviceManager, m_pBackend->m_pSeat
		);

	if (eSelection == GAMESCOPE_SELECTION_CLIPBOARD && m_pBackend->m_pDataDevice) {
		m_pBackend->m_pClipboard = szContents;
		wl_data_source* source =
			wl_data_device_manager_create_data_source(m_pBackend->m_pDataDeviceManager);
		wl_data_source_add_listener(source, &m_pBackend->s_DataSourceListener, m_pBackend);
		wl_data_source_offer(source, "text/plain");
		wl_data_source_offer(source, "text/plain;charset=utf-8");
		wl_data_source_offer(source, "TEXT");
		wl_data_source_offer(source, "STRING");
		wl_data_source_offer(source, "UTF8_STRING");
		wl_data_device_set_selection(
			m_pBackend->m_pDataDevice, source, m_pBackend->m_uKeyboardEnterSerial
		);
	} else if (eSelection == GAMESCOPE_SELECTION_PRIMARY && m_pBackend->m_pPrimarySelectionDevice) {
		m_pBackend->m_pPrimarySelection = szContents;
		zwp_primary_selection_source_v1* source =
			zwp_primary_selection_device_manager_v1_create_source(
				m_pBackend->m_pPrimarySelectionDeviceManager
			);
		zwp_primary_selection_source_v1_add_listener(
			source, &m_pBackend->s_PrimarySelectionSourceListener, m_pBackend
		);
		zwp_primary_selection_source_v1_offer(source, "text/plain");
		zwp_primary_selection_source_v1_offer(source, "text/plain;charset=utf-8");
		zwp_primary_selection_source_v1_offer(source, "TEXT");
		zwp_primary_selection_source_v1_offer(source, "STRING");
		zwp_primary_selection_source_v1_offer(source, "UTF8_STRING");
		zwp_primary_selection_device_v1_set_selection(
			m_pBackend->m_pPrimarySelectionDevice, source, m_pBackend->m_uPointerEnterSerial
		);
	}
}

} // namespace gamescope
