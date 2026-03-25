#pragma once

#include "backend.h"

// clang-format off
#include "wlr_begin.hpp"
#include <wayland-client.h>
#include "wlr_end.hpp"
// clang-format on

namespace gamescope {

class CWaylandBackend;

class CWaylandFb final : public CBaseBackendFb {
  public:
	CWaylandFb(CWaylandBackend* pBackend, wl_buffer* pHostBuffer);
	~CWaylandFb();

	void OnCompositorAcquire();
	void OnCompositorRelease();

	wl_buffer* GetHostBuffer() const { return m_pHostBuffer; }
	wlr_buffer* GetClientBuffer() const { return m_pClientBuffer; }

	void Wayland_Buffer_Release(wl_buffer* pBuffer);
	static const wl_buffer_listener s_BufferListener;

  private:
	CWaylandBackend* m_pBackend = nullptr;
	wl_buffer* m_pHostBuffer = nullptr;
	wlr_buffer* m_pClientBuffer = nullptr;
	bool m_bCompositorAcquired = false;
};

} // namespace gamescope
