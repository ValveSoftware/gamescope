#pragma once

#include "WaylandInputThread.hpp"
#include "WaylandOutputInfo.hpp"
#include "backend.h"
#include "rendervulkan.hpp"

#include <atomic>
#include <libdecor.h>
#include <memory>
#include <span>
#include <unordered_map>

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

class CWaylandConnector;
class CWaylandPlane;
class CWaylandFb;

class CWaylandBackend : public CBaseBackend {
  public:
	CWaylandBackend();

	/////////////
	// IBackend
	/////////////

	virtual bool Init() override;
	virtual bool PostInit() override;
	virtual std::span<const char* const> GetInstanceExtensions() const override;
	virtual std::span<const char* const>
	GetDeviceExtensions(VkPhysicalDevice pVkPhysicalDevice) const override;
	virtual VkImageLayout GetPresentLayout() const override;
	virtual void GetPreferredOutputFormat(
		uint32_t* pPrimaryPlaneFormat, uint32_t* pOverlayPlaneFormat
	) const override;
	virtual bool ValidPhysicalDevice(VkPhysicalDevice pVkPhysicalDevice) const override;

	virtual void DirtyState(bool bForce = false, bool bForceModeset = false) override;
	virtual bool PollState() override;

	virtual std::shared_ptr<BackendBlob>
	CreateBackendBlob(const std::type_info& type, std::span<const uint8_t> data) override;

	virtual OwningRc<IBackendFb> ImportDmabufToBackend(wlr_dmabuf_attributes* pDmaBuf) override;
	virtual bool UsesModifiers() const override;
	virtual std::span<const uint64_t> GetSupportedModifiers(uint32_t uDrmFormat) const override;

	virtual IBackendConnector* GetCurrentConnector() override;
	virtual IBackendConnector* GetConnector(GamescopeScreenType eScreenType) override;

	virtual bool SupportsPlaneHardwareCursor() const override;

	virtual bool SupportsTearing() const override;
	virtual bool UsesVulkanSwapchain() const override;

	virtual bool IsSessionBased() const override;
	virtual bool SupportsExplicitSync() const override;

	virtual bool IsPaused() const override;
	virtual bool IsVisible() const override;

	virtual glm::uvec2 CursorSurfaceSize(glm::uvec2 uvecSize) const override;
	virtual void HackUpdatePatchedEdid() override;

	virtual bool UsesVirtualConnectors() override;
	virtual std::shared_ptr<IBackendConnector>
	CreateVirtualConnector(uint64_t ulVirtualConnectorKey) override;

  protected:
	virtual void OnBackendBlobDestroyed(BackendBlob* pBlob) override;

	wl_surface* CursorInfoToSurface(const std::shared_ptr<INestedHints::CursorInfo>& info);

	bool SupportsColorManagement() const;

	void SetCursorImage(std::shared_ptr<INestedHints::CursorInfo> info);
	void SetRelativeMouseMode(wl_surface* pSurface, bool bRelative);
	void UpdateCursor();

	friend CWaylandConnector;
	friend CWaylandPlane;
	friend CWaylandInputThread;
	friend CWaylandFb;

	wl_display* GetDisplay() const { return m_pDisplay; }
	wl_shm* GetShm() const { return m_pShm; }
	wl_compositor* GetCompositor() const { return m_pCompositor; }
	wp_single_pixel_buffer_manager_v1* GetSinglePixelBufferManager() const {
		return m_pSinglePixelBufferManager;
	}
	wl_subcompositor* GetSubcompositor() const { return m_pSubcompositor; }
	zwp_linux_dmabuf_v1* GetLinuxDmabuf() const { return m_pLinuxDmabuf; }
	xdg_wm_base* GetXDGWMBase() const { return m_pXdgWmBase; }
	wp_viewporter* GetViewporter() const { return m_pViewporter; }
	wp_presentation* GetPresentation() const { return m_pPresentation; }
	frog_color_management_factory_v1* GetFrogColorManagementFactory() const {
		return m_pFrogColorMgmtFactory;
	}
	wp_color_manager_v1* GetWPColorManager() const { return m_pWPColorManager; }
	wp_image_description_v1*
	GetWPImageDescription(GamescopeAppTextureColorspace eColorspace) const {
		return m_pWPImageDescriptions[(uint32_t)eColorspace];
	}
	wp_fractional_scale_manager_v1* GetFractionalScaleManager() const {
		return m_pFractionalScaleManager;
	}
	xdg_toplevel_icon_manager_v1* GetToplevelIconManager() const { return m_pToplevelIconManager; }
	libdecor* GetLibDecor() const { return m_pLibDecor; }

	void UpdateFullscreenState();

	WaylandOutputInfo* GetOutputInfo(wl_output* pOutput) {
		auto iter = m_pOutputs.find(pOutput);
		if (iter == m_pOutputs.end())
			return nullptr;

		return &iter->second;
	}

	wl_region* GetEmptyRegion() const { return m_pEmptyRegion; }
	wl_region* GetFullRegion() const { return m_pFullRegion; }
	CWaylandFb* GetBlackFb() const { return m_BlackFb.get(); }

	void OnConnectorDestroyed(CWaylandConnector* pConnector) {
		m_pFocusConnector.compare_exchange_strong(pConnector, nullptr);
	}

  private:
	void Wayland_Registry_Global(
		wl_registry* pRegistry, uint32_t uName, const char* pInterface, uint32_t uVersion
	);
	static const wl_registry_listener s_RegistryListener;

	void Wayland_Modifier(
		zwp_linux_dmabuf_v1* pDmabuf, uint32_t uFormat, uint32_t uModifierHi, uint32_t uModifierLo
	);

	void Wayland_Output_Geometry(
		wl_output* pOutput, int32_t nX, int32_t nY, int32_t nPhysicalWidth, int32_t nPhysicalHeight,
		int32_t nSubpixel, const char* pMake, const char* pModel, int32_t nTransform
	);
	void Wayland_Output_Mode(
		wl_output* pOutput, uint32_t uFlags, int32_t nWidth, int32_t nHeight, int32_t nRefresh
	);
	void Wayland_Output_Done(wl_output* pOutput);
	void Wayland_Output_Scale(wl_output* pOutput, int32_t nFactor);
	void Wayland_Output_Name(wl_output* pOutput, const char* pName);
	void Wayland_Output_Description(wl_output* pOutput, const char* pDescription);
	static const wl_output_listener s_OutputListener;

	void Wayland_Seat_Capabilities(wl_seat* pSeat, uint32_t uCapabilities);
	static const wl_seat_listener s_SeatListener;

	void Wayland_Pointer_Enter(
		wl_pointer* pPointer, uint32_t uSerial, wl_surface* pSurface, wl_fixed_t fSurfaceX,
		wl_fixed_t fSurfaceY
	);
	void Wayland_Pointer_Leave(wl_pointer* pPointer, uint32_t uSerial, wl_surface* pSurface);
	static const wl_pointer_listener s_PointerListener;

	void Wayland_Keyboard_Enter(
		wl_keyboard* pKeyboard, uint32_t uSerial, wl_surface* pSurface, wl_array* pKeys
	);
	void Wayland_Keyboard_Leave(wl_keyboard* pKeyboard, uint32_t uSerial, wl_surface* pSurface);
	static const wl_keyboard_listener s_KeyboardListener;

	void Wayland_LockedPointer_Locked(zwp_locked_pointer_v1* pLockedPointer);
	void Wayland_LockedPointer_Unlocked(zwp_locked_pointer_v1* pLockedPointer);
	static const zwp_locked_pointer_v1_listener s_LockedPointerListener;

	void Wayland_WPColorManager_SupportedIntent(
		wp_color_manager_v1* pWPColorManager, uint32_t uRenderIntent
	);
	void Wayland_WPColorManager_SupportedFeature(
		wp_color_manager_v1* pWPColorManager, uint32_t uFeature
	);
	void
	Wayland_WPColorManager_SupportedTFNamed(wp_color_manager_v1* pWPColorManager, uint32_t uTF);
	void Wayland_WPColorManager_SupportedPrimariesNamed(
		wp_color_manager_v1* pWPColorManager, uint32_t uPrimaries
	);
	void Wayland_WPColorManager_ColorManagerDone(wp_color_manager_v1* pWPColorManager);
	static const wp_color_manager_v1_listener s_WPColorManagerListener;

	void Wayland_DataSource_Send(struct wl_data_source* pSource, const char* pMime, int nFd);
	void Wayland_DataSource_Cancelled(struct wl_data_source* pSource);
	static const wl_data_source_listener s_DataSourceListener;

	void Wayland_PrimarySelectionSource_Send(
		struct zwp_primary_selection_source_v1* pSource, const char* pMime, int nFd
	);
	void Wayland_PrimarySelectionSource_Cancelled(struct zwp_primary_selection_source_v1* pSource);
	static const zwp_primary_selection_source_v1_listener s_PrimarySelectionSourceListener;

	CWaylandInputThread m_InputThread;

	wl_display* m_pDisplay = nullptr;
	wl_shm* m_pShm = nullptr;
	wl_compositor* m_pCompositor = nullptr;
	wp_single_pixel_buffer_manager_v1* m_pSinglePixelBufferManager = nullptr;
	wl_subcompositor* m_pSubcompositor = nullptr;
	zwp_linux_dmabuf_v1* m_pLinuxDmabuf = nullptr;
	xdg_wm_base* m_pXdgWmBase = nullptr;
	wp_viewporter* m_pViewporter = nullptr;
	wl_region* m_pEmptyRegion = nullptr;
	wl_region* m_pFullRegion = nullptr;
	Rc<CWaylandFb> m_BlackFb;
	OwningRc<CWaylandFb> m_pOwnedBlackFb;
	OwningRc<CVulkanTexture> m_pBlackTexture;
	wp_presentation* m_pPresentation = nullptr;
	frog_color_management_factory_v1* m_pFrogColorMgmtFactory = nullptr;
	wp_color_manager_v1* m_pWPColorManager = nullptr;
	wp_image_description_v1* m_pWPImageDescriptions[GamescopeAppTextureColorspace_Count]{};
	zwp_pointer_constraints_v1* m_pPointerConstraints = nullptr;
	zwp_relative_pointer_manager_v1* m_pRelativePointerManager = nullptr;
	wp_fractional_scale_manager_v1* m_pFractionalScaleManager = nullptr;
	xdg_toplevel_icon_manager_v1* m_pToplevelIconManager = nullptr;

	// TODO: Restructure and remove the need for this.
	std::atomic<CWaylandConnector*> m_pFocusConnector;

	wl_data_device_manager* m_pDataDeviceManager = nullptr;
	wl_data_device* m_pDataDevice = nullptr;
	std::shared_ptr<std::string> m_pClipboard = nullptr;

	zwp_primary_selection_device_manager_v1* m_pPrimarySelectionDeviceManager = nullptr;
	zwp_primary_selection_device_v1* m_pPrimarySelectionDevice = nullptr;
	std::shared_ptr<std::string> m_pPrimarySelection = nullptr;

	struct {
		std::vector<wp_color_manager_v1_primaries> ePrimaries;
		std::vector<wp_color_manager_v1_transfer_function> eTransferFunctions;
		std::vector<wp_color_manager_v1_render_intent> eRenderIntents;
		std::vector<wp_color_manager_v1_feature> eFeatures;

		bool bSupportsGamescopeColorManagement = false; // Has everything we want and need?
	} m_WPColorManagerFeatures;

	std::unordered_map<wl_output*, WaylandOutputInfo> m_pOutputs;

	libdecor* m_pLibDecor = nullptr;

	wl_seat* m_pSeat = nullptr;
	wl_keyboard* m_pKeyboard = nullptr;
	wl_pointer* m_pPointer = nullptr;
	wl_touch* m_pTouch = nullptr;
	zwp_locked_pointer_v1* m_pLockedPointer = nullptr;
	bool m_bPointerLocked = false;
	wl_surface* m_pLockedSurface = nullptr;
	zwp_relative_pointer_v1* m_pRelativePointer = nullptr;

	bool m_bCanUseModifiers = false;
	std::unordered_map<uint32_t, std::vector<uint64_t>> m_FormatModifiers;
	std::unordered_map<uint32_t, wl_buffer*> m_ImportedFbs;

	uint32_t m_uPointerEnterSerial = 0;
	bool m_bMouseEntered = false;
	uint32_t m_uKeyboardEnterSerial = 0;
	bool m_bKeyboardEntered = false;

	std::shared_ptr<INestedHints::CursorInfo> m_pCursorInfo;
	wl_surface* m_pCursorSurface = nullptr;
	std::shared_ptr<INestedHints::CursorInfo> m_pDefaultCursorInfo;
	wl_surface* m_pDefaultCursorSurface = nullptr;
};
} // namespace gamescope
