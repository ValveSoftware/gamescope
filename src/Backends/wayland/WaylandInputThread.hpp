#pragma once

#include "WaylandModifierIndex.hpp"
#include "waitable.h"

#include <memory>
#include <optional>
#include <thread>
#include <unordered_set>
#include <xkbcommon/xkbcommon.h>

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

class CWaylandInputThread {
  public:
	CWaylandInputThread();
	~CWaylandInputThread();

	bool Init(CWaylandBackend* pBackend);

	void ThreadFunc();

	// This is only shared_ptr because it works nicely
	// with std::atomic, std::any and such and makes it very easy.
	//
	// It could be a std::unique_ptr if you added a mutex,
	// but I didn't seem it worth it.
	template<typename T>
	std::shared_ptr<T> QueueLaunder(T* pObject);

	void SetRelativePointer(bool bRelative);

  private:
	void HandleKey(uint32_t uKey, bool bPressed);

	CWaylandBackend* m_pBackend = nullptr;

	CWaiter<4> m_Waiter;

	std::thread m_Thread;
	std::atomic<bool> m_bInitted = {false};

	uint32_t m_uPointerEnterSerial = 0;
	bool m_bMouseEntered = false;
	bool m_bKeyboardEntered = false;

	wl_event_queue* m_pQueue = nullptr;
	std::shared_ptr<wl_display> m_pDisplayWrapper;

	wl_seat* m_pSeat = nullptr;
	wl_keyboard* m_pKeyboard = nullptr;
	wl_pointer* m_pPointer = nullptr;
	wl_touch* m_pTouch = nullptr;
	zwp_relative_pointer_manager_v1* m_pRelativePointerManager = nullptr;

	uint32_t m_uFakeTimestamp = 0;

	xkb_context* m_pXkbContext = nullptr;
	xkb_keymap* m_pXkbKeymap = nullptr;

	uint32_t m_uKeyModifiers = 0;
	uint32_t m_uModMask[GAMESCOPE_WAYLAND_MOD_COUNT];

	double m_flScrollAccum[2] = {0.0, 0.0};
	uint32_t m_uAxisSource = WL_POINTER_AXIS_SOURCE_WHEEL;

	wl_surface* m_pCurrentCursorSurface = nullptr;

	std::optional<wl_fixed_t> m_ofPendingCursorX;
	std::optional<wl_fixed_t> m_ofPendingCursorY;

	std::atomic<std::shared_ptr<zwp_relative_pointer_v1>> m_pRelativePointer = nullptr;
	std::unordered_set<uint32_t> m_uScancodesHeld;

	void Wayland_Registry_Global(
		wl_registry* pRegistry, uint32_t uName, const char* pInterface, uint32_t uVersion
	);
	static const wl_registry_listener s_RegistryListener;

	void Wayland_Seat_Capabilities(wl_seat* pSeat, uint32_t uCapabilities);
	void Wayland_Seat_Name(wl_seat* pSeat, const char* pName);
	static const wl_seat_listener s_SeatListener;

	void Wayland_Pointer_Enter(
		wl_pointer* pPointer, uint32_t uSerial, wl_surface* pSurface, wl_fixed_t fSurfaceX,
		wl_fixed_t fSurfaceY
	);
	void Wayland_Pointer_Leave(wl_pointer* pPointer, uint32_t uSerial, wl_surface* pSurface);
	void Wayland_Pointer_Motion(
		wl_pointer* pPointer, uint32_t uTime, wl_fixed_t fSurfaceX, wl_fixed_t fSurfaceY
	);
	void Wayland_Pointer_Button(
		wl_pointer* pPointer, uint32_t uSerial, uint32_t uTime, uint32_t uButton, uint32_t uState
	);
	void
	Wayland_Pointer_Axis(wl_pointer* pPointer, uint32_t uTime, uint32_t uAxis, wl_fixed_t fValue);
	void Wayland_Pointer_Axis_Source(wl_pointer* pPointer, uint32_t uAxisSource);
	void Wayland_Pointer_Axis_Stop(wl_pointer* pPointer, uint32_t uTime, uint32_t uAxis);
	void Wayland_Pointer_Axis_Discrete(wl_pointer* pPointer, uint32_t uAxis, int32_t nDiscrete);
	void Wayland_Pointer_Axis_Value120(wl_pointer* pPointer, uint32_t uAxis, int32_t nValue120);
	void Wayland_Pointer_Frame(wl_pointer* pPointer);
	static const wl_pointer_listener s_PointerListener;

	void
	Wayland_Keyboard_Keymap(wl_keyboard* pKeyboard, uint32_t uFormat, int32_t nFd, uint32_t uSize);
	void Wayland_Keyboard_Enter(
		wl_keyboard* pKeyboard, uint32_t uSerial, wl_surface* pSurface, wl_array* pKeys
	);
	void Wayland_Keyboard_Leave(wl_keyboard* pKeyboard, uint32_t uSerial, wl_surface* pSurface);
	void Wayland_Keyboard_Key(
		wl_keyboard* pKeyboard, uint32_t uSerial, uint32_t uTime, uint32_t uKey, uint32_t uState
	);
	void Wayland_Keyboard_Modifiers(
		wl_keyboard* pKeyboard, uint32_t uSerial, uint32_t uModsDepressed, uint32_t uModsLatched,
		uint32_t uModsLocked, uint32_t uGroup
	);
	void Wayland_Keyboard_RepeatInfo(wl_keyboard* pKeyboard, int32_t nRate, int32_t nDelay);
	static const wl_keyboard_listener s_KeyboardListener;

	void Wayland_RelativePointer_RelativeMotion(
		zwp_relative_pointer_v1* pRelativePointer, uint32_t uTimeHi, uint32_t uTimeLo,
		wl_fixed_t fDx, wl_fixed_t fDy, wl_fixed_t fDxUnaccel, wl_fixed_t fDyUnaccel
	);
	static const zwp_relative_pointer_v1_listener s_RelativePointerListener;
};

} // namespace gamescope
