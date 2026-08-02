#include "WaylandFb.hpp"

#include "callback_macro.hpp"
#include "xdg_log.hpp"

namespace gamescope {

const wl_buffer_listener CWaylandFb::s_BufferListener = {
	.release = WAYLAND_USERDATA_TO_THIS(CWaylandFb, Wayland_Buffer_Release),
};

CWaylandFb::CWaylandFb(CWaylandBackend* pBackend, wl_buffer* pHostBuffer)
	: CBaseBackendFb(), m_pBackend{pBackend}, m_pHostBuffer{pHostBuffer} {
	wl_buffer_add_listener(pHostBuffer, &s_BufferListener, this);
}

CWaylandFb::~CWaylandFb() {
	// I own the pHostBuffer.
	wl_buffer_destroy(m_pHostBuffer);
	m_pHostBuffer = nullptr;
}

void CWaylandFb::OnCompositorAcquire() {
	// If the compositor has acquired us, track that
	// and increment the ref count.
	if (!m_bCompositorAcquired) {
		m_bCompositorAcquired = true;
		IncRef();
	}
}

void CWaylandFb::OnCompositorRelease() {
	// Compositor has released us, decrement rc.
	// assert( m_bCompositorAcquired );

	if (m_bCompositorAcquired) {
		m_bCompositorAcquired = false;
		DecRef();
	} else {
		xdg_log.errorf("Compositor released us but we were not acquired. Oh no.");
	}
}

void CWaylandFb::Wayland_Buffer_Release(wl_buffer* pBuffer) {
	assert(m_pHostBuffer);
	assert(m_pHostBuffer == pBuffer);

	xdg_log.debugf("buffer_release: %p", pBuffer);

	OnCompositorRelease();
}

} // namespace gamescope
