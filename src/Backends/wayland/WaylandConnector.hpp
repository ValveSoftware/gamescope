#pragma once

#include "WaylandPlane.hpp"
#include "backend.h"
#include "color_helpers.h"
#include "gamescope_shared.h"
#include "rendervulkan.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace gamescope {

class CWaylandBackend;

class CWaylandConnector final : public CBaseBackendConnector, public INestedHints {
  public:
	CWaylandConnector(CWaylandBackend* pBackend, uint64_t ulVirtualConnectorKey);
	virtual ~CWaylandConnector();

	bool UpdateEdid();
	bool Init();
	void SetFullscreen(bool bFullscreen); // Thread safe, can be called from the input thread.
	void UpdateFullscreenState();

	bool HostCompositorIsCurrentlyVRR() const { return m_bHostCompositorIsCurrentlyVRR; }
	void SetHostCompositorIsCurrentlyVRR(bool bActive) {
		m_bHostCompositorIsCurrentlyVRR = bActive;
	}
	bool CurrentDisplaySupportsVRR() const { return HostCompositorIsCurrentlyVRR(); }
	CWaylandBackend* GetBackend() const { return m_pBackend; }

	/////////////////////
	// IBackendConnector
	/////////////////////

	virtual int Present(const FrameInfo_t* pFrameInfo, bool bAsync) override;

	virtual gamescope::GamescopeScreenType GetScreenType() const override;
	virtual GamescopePanelOrientation GetCurrentOrientation() const override;
	virtual bool SupportsHDR() const override;
	virtual bool IsHDRActive() const override;
	virtual const BackendConnectorHDRInfo& GetHDRInfo() const override;
	virtual bool IsVRRActive() const override;
	virtual std::span<const BackendMode> GetModes() const override;

	virtual bool SupportsVRR() const override;

	virtual std::span<const uint8_t> GetRawEDID() const override;
	virtual std::span<const uint32_t> GetValidDynamicRefreshRates() const override;

	virtual void GetNativeColorimetry(
		bool bHDR10, displaycolorimetry_t* displayColorimetry, EOTF* displayEOTF,
		displaycolorimetry_t* outputEncodingColorimetry, EOTF* outputEncodingEOTF
	) const override;

	virtual const char* GetName() const override { return "Wayland"; }
	virtual const char* GetMake() const override { return "Gamescope"; }
	virtual const char* GetModel() const override { return "Virtual Display"; }

	virtual INestedHints* GetNestedHints() override { return this; }

	///////////////////
	// INestedHints
	///////////////////

	virtual void SetCursorImage(std::shared_ptr<INestedHints::CursorInfo> info) override;
	virtual void SetRelativeMouseMode(bool bRelative) override;
	virtual void SetVisible(bool bVisible) override;
	virtual void SetTitle(std::shared_ptr<std::string> szTitle) override;
	virtual void SetIcon(std::shared_ptr<std::vector<uint32_t>> uIconPixels) override;
	virtual void
	SetSelection(std::shared_ptr<std::string> szContents, GamescopeSelection eSelection) override;

  private:
	friend CWaylandPlane;

	BackendConnectorHDRInfo m_HDRInfo{};
	uint32_t m_uReferenceLuminance = 203;
	uint32_t m_uMaxTargetLuminance = 203;
	displaycolorimetry_t m_DisplayColorimetry = displaycolorimetry_709;
	std::vector<uint8_t> m_FakeEdid;

	CWaylandBackend* m_pBackend = nullptr;

	CWaylandPlane m_Planes[8];
	bool m_bVisible = true;
	std::atomic<bool> m_bDesiredFullscreenState = {false};

	bool m_bHostCompositorIsCurrentlyVRR = false;
};

} // namespace gamescope
