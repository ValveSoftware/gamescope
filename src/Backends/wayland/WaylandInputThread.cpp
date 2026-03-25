#include "WaylandInputThread.hpp"

#include "Utils/Defer.h"
#include "WaylandBackend.hpp"
#include "WaylandConnector.hpp"
#include "WaylandModifierIndex.hpp"
#include "callback_macro.hpp"
#include "convars.hpp"
#include "tag_identify.hpp"
#include "wlserver.hpp"
#include "xdg_log.hpp"

#include <linux/input-event-codes.h>
#include <sys/mman.h>

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

constexpr const char* WaylandModifierToXkbModifierName(WaylandModifierIndex eIndex) {
	switch (eIndex) {
	case GAMESCOPE_WAYLAND_MOD_CTRL:
		return XKB_MOD_NAME_CTRL;
	case GAMESCOPE_WAYLAND_MOD_SHIFT:
		return XKB_MOD_NAME_SHIFT;
	case GAMESCOPE_WAYLAND_MOD_ALT:
		return XKB_MOD_NAME_ALT;
	case GAMESCOPE_WAYLAND_MOD_META:
		return XKB_MOD_NAME_LOGO;
	case GAMESCOPE_WAYLAND_MOD_NUM:
		return XKB_MOD_NAME_NUM;
	case GAMESCOPE_WAYLAND_MOD_CAPS:
		return XKB_MOD_NAME_CAPS;
	default:
		return "Unknown";
	}
}

CWaylandInputThread::CWaylandInputThread() : m_Thread{[this]() { this->ThreadFunc(); }} {}

CWaylandInputThread::~CWaylandInputThread() {
	m_bInitted = true;
	m_bInitted.notify_all();

	m_Waiter.Shutdown();
	m_Thread.join();
}

bool CWaylandInputThread::Init(CWaylandBackend* pBackend) {
	m_pBackend = pBackend;

	if (!(m_pXkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS))) {
		xdg_log.errorf("Couldn't create xkb context.");
		return false;
	}

	if (!(m_pQueue = wl_display_create_queue(m_pBackend->GetDisplay()))) {
		xdg_log.errorf("Couldn't create input thread queue.");
		return false;
	}

	if (!(m_pDisplayWrapper = QueueLaunder(m_pBackend->GetDisplay()))) {
		xdg_log.errorf("Couldn't create display proxy for input thread");
		return false;
	}

	wl_registry* pRegistry;
	if (!(pRegistry = wl_display_get_registry(m_pDisplayWrapper.get()))) {
		xdg_log.errorf("Couldn't create registry for input thread");
		return false;
	}
	wl_registry_add_listener(pRegistry, &s_RegistryListener, this);

	wl_display_roundtrip_queue(pBackend->GetDisplay(), m_pQueue);
	wl_display_roundtrip_queue(pBackend->GetDisplay(), m_pQueue);

	wl_registry_destroy(pRegistry);
	pRegistry = nullptr;

	if (!m_pSeat || !m_pRelativePointerManager) {
		xdg_log.errorf("Couldn't create Wayland input objects.");
		return false;
	}

	m_bInitted = true;
	m_bInitted.notify_all();
	return true;
}

void CWaylandInputThread::ThreadFunc() {
	m_bInitted.wait(false);

	if (!m_Waiter.IsRunning())
		return;

	int nFD = wl_display_get_fd(m_pBackend->GetDisplay());
	if (nFD < 0) {
		abort();
	}

	CFunctionWaitable waitable(nFD);
	m_Waiter.AddWaitable(&waitable);

	int nRet = 0;
	while (m_Waiter.IsRunning()) {
		if ((nRet = wl_display_dispatch_queue_pending(m_pBackend->GetDisplay(), m_pQueue)) < 0) {
			abort();
		}

		if ((nRet = wl_display_prepare_read_queue(m_pBackend->GetDisplay(), m_pQueue)) < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;

			abort();
		}

		if ((nRet = m_Waiter.PollEvents()) <= 0) {
			wl_display_cancel_read(m_pBackend->GetDisplay());
			if (nRet < 0)
				abort();

			assert(nRet == 0);
			continue;
		}

		if ((nRet = wl_display_read_events(m_pBackend->GetDisplay())) < 0) {
			abort();
		}
	}
}

template<typename T>
std::shared_ptr<T> CWaylandInputThread::QueueLaunder(T* pObject) {
	if (!pObject)
		return nullptr;

	T* pObjectWrapper = (T*)wl_proxy_create_wrapper((void*)pObject);
	if (!pObjectWrapper)
		return nullptr;
	wl_proxy_set_queue((wl_proxy*)pObjectWrapper, m_pQueue);

	return std::shared_ptr<T>{pObjectWrapper, [](T* pThing) {
								  wl_proxy_wrapper_destroy((void*)pThing);
							  }};
}

void CWaylandInputThread::SetRelativePointer(bool bRelative) {
	if (bRelative == !!m_pRelativePointer.load())
		return;
	// This constructors/destructors the display's mutex, so should be safe to do across threads.
	if (!bRelative) {
		m_pRelativePointer = nullptr;
	} else {
		zwp_relative_pointer_v1* pRelativePointer =
			zwp_relative_pointer_manager_v1_get_relative_pointer(
				m_pRelativePointerManager, m_pPointer
			);
		m_pRelativePointer = std::shared_ptr<zwp_relative_pointer_v1>{
			pRelativePointer,
			[](zwp_relative_pointer_v1* pObject) { zwp_relative_pointer_v1_destroy(pObject); }
		};
		zwp_relative_pointer_v1_add_listener(pRelativePointer, &s_RelativePointerListener, this);
	}
}

void CWaylandInputThread::HandleKey(uint32_t uKey, bool bPressed) {
	if (m_uKeyModifiers & m_uModMask[GAMESCOPE_WAYLAND_MOD_META]) {
		switch (uKey) {
		case KEY_F: {
			if (!bPressed) {
				static_cast<CWaylandConnector*>(m_pBackend->GetCurrentConnector())
					->SetFullscreen(!g_bFullscreen);
			}
			return;
		}

		case KEY_N: {
			if (!bPressed) {
				g_wantedUpscaleFilter = GamescopeUpscaleFilter::PIXEL;
			}
			return;
		}

		case KEY_B: {
			if (!bPressed) {
				g_wantedUpscaleFilter = GamescopeUpscaleFilter::LINEAR;
			}
			return;
		}

		case KEY_U: {
			if (!bPressed) {
				g_wantedUpscaleFilter = (g_wantedUpscaleFilter == GamescopeUpscaleFilter::FSR)
											? GamescopeUpscaleFilter::LINEAR
											: GamescopeUpscaleFilter::FSR;
			}
			return;
		}

		case KEY_Y: {
			if (!bPressed) {
				g_wantedUpscaleFilter = (g_wantedUpscaleFilter == GamescopeUpscaleFilter::NIS)
											? GamescopeUpscaleFilter::LINEAR
											: GamescopeUpscaleFilter::NIS;
			}
			return;
		}

		case KEY_I: {
			if (!bPressed) {
				g_upscaleFilterSharpness = std::min(20, g_upscaleFilterSharpness + 1);
			}
			return;
		}

		case KEY_O: {
			if (!bPressed) {
				g_upscaleFilterSharpness = std::max(0, g_upscaleFilterSharpness - 1);
			}
			return;
		}

		case KEY_S: {
			if (!bPressed) {
				gamescope::CScreenshotManager::Get().TakeScreenshot(true);
			}
			return;
		}

		default:
			break;
		}
	}

	wlserver_lock();
	wlserver_key(uKey, bPressed, ++m_uFakeTimestamp);
	wlserver_unlock();
}

// Registry

void CWaylandInputThread::Wayland_Registry_Global(
	wl_registry* pRegistry, uint32_t uName, const char* pInterface, uint32_t uVersion
) {
	if (!strcmp(pInterface, wl_seat_interface.name) && uVersion >= 8u) {
		m_pSeat = (wl_seat*)wl_registry_bind(pRegistry, uName, &wl_seat_interface, 8u);
		wl_seat_add_listener(m_pSeat, &s_SeatListener, this);
	} else if (!strcmp(pInterface, zwp_relative_pointer_manager_v1_interface.name)) {
		m_pRelativePointerManager = (zwp_relative_pointer_manager_v1*)wl_registry_bind(
			pRegistry, uName, &zwp_relative_pointer_manager_v1_interface, 1u
		);
	}
}

// Seat

void CWaylandInputThread::Wayland_Seat_Capabilities(wl_seat* pSeat, uint32_t uCapabilities) {
	if (!!(uCapabilities & WL_SEAT_CAPABILITY_POINTER) != !!m_pPointer) {
		if (m_pPointer) {
			wl_pointer_release(m_pPointer);
			m_pPointer = nullptr;
		} else {
			m_pPointer = wl_seat_get_pointer(m_pSeat);
			wl_pointer_add_listener(m_pPointer, &s_PointerListener, this);
		}
	}

	if (!!(uCapabilities & WL_SEAT_CAPABILITY_KEYBOARD) != !!m_pKeyboard) {
		if (m_pKeyboard) {
			wl_keyboard_release(m_pKeyboard);
			m_pKeyboard = nullptr;
		} else {
			m_pKeyboard = wl_seat_get_keyboard(m_pSeat);
			wl_keyboard_add_listener(m_pKeyboard, &s_KeyboardListener, this);
		}
	}
}

void CWaylandInputThread::Wayland_Seat_Name(wl_seat* pSeat, const char* pName) {
	xdg_log.infof("Seat name: %s", pName);
}

// Pointer

void CWaylandInputThread::Wayland_Pointer_Enter(
	wl_pointer* pPointer, uint32_t uSerial, wl_surface* pSurface, wl_fixed_t fSurfaceX,
	wl_fixed_t fSurfaceY
) {
	if (!IsGamescopeToplevel(pSurface))
		return;

	m_pCurrentCursorSurface = pSurface;
	m_bMouseEntered = true;
	m_uPointerEnterSerial = uSerial;

	Wayland_Pointer_Motion(pPointer, 0, fSurfaceX, fSurfaceY);
}
void CWaylandInputThread::Wayland_Pointer_Leave(
	wl_pointer* pPointer, uint32_t uSerial, wl_surface* pSurface
) {
	if (!IsGamescopeToplevel(pSurface))
		return;

	m_pCurrentCursorSurface = nullptr;
	m_bMouseEntered = false;
}
void CWaylandInputThread::Wayland_Pointer_Motion(
	wl_pointer* pPointer, uint32_t uTime, wl_fixed_t fSurfaceX, wl_fixed_t fSurfaceY
) {
	if (!m_bMouseEntered)
		return;

	CWaylandPlane* pPlane = (CWaylandPlane*)wl_surface_get_user_data(m_pCurrentCursorSurface);

	if (!pPlane)
		return;

	if (m_pRelativePointer.load() != nullptr)
		return;

	if (!cv_wayland_mouse_warp_without_keyboard_focus && !m_bKeyboardEntered) {
		// Don't do any motion/movement stuff if we don't have kb focus
		m_ofPendingCursorX = fSurfaceX;
		m_ofPendingCursorY = fSurfaceY;
		return;
	}

	auto oState = pPlane->GetCurrentState();
	if (!oState)
		return;

	uint32_t uScale = oState->uFractionalScale;

	double flX = (wl_fixed_to_double(fSurfaceX) * uScale / 120.0 + oState->nDestX) / g_nOutputWidth;
	double flY =
		(wl_fixed_to_double(fSurfaceY) * uScale / 120.0 + oState->nDestY) / g_nOutputHeight;

	wlserver_lock();
	wlserver_touchmotion(flX, flY, 0, ++m_uFakeTimestamp);
	wlserver_unlock();
}
void CWaylandInputThread::Wayland_Pointer_Button(
	wl_pointer* pPointer, uint32_t uSerial, uint32_t uTime, uint32_t uButton, uint32_t uState
) {
	// Don't do any motion/movement stuff if we don't have kb focus
	if (!cv_wayland_mouse_warp_without_keyboard_focus && !m_bKeyboardEntered)
		return;

	wlserver_lock();
	wlserver_mousebutton(uButton, uState == WL_POINTER_BUTTON_STATE_PRESSED, ++m_uFakeTimestamp);
	wlserver_unlock();
}
void CWaylandInputThread::Wayland_Pointer_Axis(
	wl_pointer* pPointer, uint32_t uTime, uint32_t uAxis, wl_fixed_t fValue
) {}
void CWaylandInputThread::Wayland_Pointer_Axis_Source(wl_pointer* pPointer, uint32_t uAxisSource) {
	m_uAxisSource = uAxisSource;
}
void CWaylandInputThread::Wayland_Pointer_Axis_Stop(
	wl_pointer* pPointer, uint32_t uTime, uint32_t uAxis
) {}
void CWaylandInputThread::Wayland_Pointer_Axis_Discrete(
	wl_pointer* pPointer, uint32_t uAxis, int32_t nDiscrete
) {}
void CWaylandInputThread::Wayland_Pointer_Axis_Value120(
	wl_pointer* pPointer, uint32_t uAxis, int32_t nValue120
) {
	if (!cv_wayland_mouse_warp_without_keyboard_focus && !m_bKeyboardEntered)
		return;

	assert(uAxis == WL_POINTER_AXIS_VERTICAL_SCROLL || uAxis == WL_POINTER_AXIS_HORIZONTAL_SCROLL);

	// Vertical is first in the wl_pointer_axis enum, flip y,x -> x,y
	m_flScrollAccum[!uAxis] += nValue120 / 120.0;
}
void CWaylandInputThread::Wayland_Pointer_Frame(wl_pointer* pPointer) {
	defer(m_uAxisSource = WL_POINTER_AXIS_SOURCE_WHEEL);
	double flX = m_flScrollAccum[0];
	double flY = m_flScrollAccum[1];
	m_flScrollAccum[0] = 0.0;
	m_flScrollAccum[1] = 0.0;

	if (!cv_wayland_mouse_warp_without_keyboard_focus && !m_bKeyboardEntered)
		return;

	if (m_uAxisSource != WL_POINTER_AXIS_SOURCE_WHEEL)
		return;

	if (flX == 0.0 && flY == 0.0)
		return;

	wlserver_lock();
	wlserver_mousewheel(flX, flY, ++m_uFakeTimestamp);
	wlserver_unlock();
}

// Keyboard

void CWaylandInputThread::Wayland_Keyboard_Keymap(
	wl_keyboard* pKeyboard, uint32_t uFormat, int32_t nFd, uint32_t uSize
) {
	// We are not doing much with the keymap, we pass keycodes thru.
	// Ideally we'd use this to influence our keymap to clients, eg. x server.

	defer(close(nFd));
	if (uFormat != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
		return;

	char* pMap = (char*)mmap(nullptr, uSize, PROT_READ, MAP_PRIVATE, nFd, 0);
	if (!pMap || pMap == MAP_FAILED) {
		xdg_log.errorf("Failed to map keymap fd.");
		return;
	}
	defer(munmap(pMap, uSize));

	xkb_keymap* pKeymap = xkb_keymap_new_from_string(
		m_pXkbContext, pMap, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS
	);
	if (!pKeymap) {
		xdg_log.errorf("Failed to create xkb_keymap");
		return;
	}

	xkb_keymap_unref(m_pXkbKeymap);
	m_pXkbKeymap = pKeymap;

	for (uint32_t i = 0; i < GAMESCOPE_WAYLAND_MOD_COUNT; i++)
		m_uModMask[i] = 1u << xkb_keymap_mod_get_index(
							m_pXkbKeymap, WaylandModifierToXkbModifierName((WaylandModifierIndex)i)
						);
}
void CWaylandInputThread::Wayland_Keyboard_Enter(
	wl_keyboard* pKeyboard, uint32_t uSerial, wl_surface* pSurface, wl_array* pKeys
) {
	if (!IsGamescopeToplevel(pSurface))
		return;

	m_bKeyboardEntered = true;
	m_uScancodesHeld.clear();

	const uint32_t* pBegin = (uint32_t*)pKeys->data;
	const uint32_t* pEnd = pBegin + (pKeys->size / sizeof(uint32_t));
	std::span<const uint32_t> keys{pBegin, pEnd};
	for (uint32_t uKey : keys) {
		HandleKey(uKey, true);
		m_uScancodesHeld.insert(uKey);
	}

	if (m_ofPendingCursorX) {
		assert(m_ofPendingCursorY.has_value());

		Wayland_Pointer_Motion(m_pPointer, 0, *m_ofPendingCursorX, *m_ofPendingCursorY);
		m_ofPendingCursorX = std::nullopt;
		m_ofPendingCursorY = std::nullopt;
	}
}
void CWaylandInputThread::Wayland_Keyboard_Leave(
	wl_keyboard* pKeyboard, uint32_t uSerial, wl_surface* pSurface
) {
	if (!IsGamescopeToplevel(pSurface))
		return;

	m_bKeyboardEntered = false;
	m_uKeyModifiers = 0;

	for (uint32_t uKey : m_uScancodesHeld)
		HandleKey(uKey, false);

	m_uScancodesHeld.clear();
}
void CWaylandInputThread::Wayland_Keyboard_Key(
	wl_keyboard* pKeyboard, uint32_t uSerial, uint32_t uTime, uint32_t uKey, uint32_t uState
) {
	if (!m_bKeyboardEntered)
		return;

	const bool bPressed = uState == WL_KEYBOARD_KEY_STATE_PRESSED;
	const bool bWasPressed = m_uScancodesHeld.contains(uKey);
	if (bWasPressed == bPressed)
		return;

	HandleKey(uKey, bPressed);

	if (bWasPressed)
		m_uScancodesHeld.erase(uKey);
	else
		m_uScancodesHeld.emplace(uKey);
}
void CWaylandInputThread::Wayland_Keyboard_Modifiers(
	wl_keyboard* pKeyboard, uint32_t uSerial, uint32_t uModsDepressed, uint32_t uModsLatched,
	uint32_t uModsLocked, uint32_t uGroup
) {
	m_uKeyModifiers = uModsDepressed | uModsLatched | uModsLocked;
}
void CWaylandInputThread::Wayland_Keyboard_RepeatInfo(
	wl_keyboard* pKeyboard, int32_t nRate, int32_t nDelay
) {}

// Relative Pointer

void CWaylandInputThread::Wayland_RelativePointer_RelativeMotion(
	zwp_relative_pointer_v1* pRelativePointer, uint32_t uTimeHi, uint32_t uTimeLo, wl_fixed_t fDx,
	wl_fixed_t fDy, wl_fixed_t fDxUnaccel, wl_fixed_t fDyUnaccel
) {
	// Don't do any motion/movement stuff if we don't have kb focus
	if (!m_pBackend->m_bPointerLocked ||
		(!cv_wayland_mouse_relmotion_without_keyboard_focus && !m_bKeyboardEntered))
		return;

	wlserver_lock();
	wlserver_mousemotion(
		wl_fixed_to_double(fDxUnaccel), wl_fixed_to_double(fDyUnaccel), ++m_uFakeTimestamp
	);
	wlserver_unlock();
}

} // namespace gamescope
