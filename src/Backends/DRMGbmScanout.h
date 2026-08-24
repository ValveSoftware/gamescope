#pragma once

#include <cstdint>
#include <span>

struct gbm_device;
struct wlr_dmabuf_attributes;

namespace gamescope
{
	// NVIDIA's display engine requires physically contiguous scanout memory,
	// which Vulkan external-memory allocation cannot guarantee but GBM can.
	// Delete this if/when nvidia-drm can scan out Vulkan-allocated buffers.
	class CGbmScanoutAllocator
	{
	public:
		~CGbmScanoutAllocator();

		bool Init( int nDrmFd ); // no-op unless nvidia-drm
		void Shutdown();

		bool IsAvailable() const { return m_pGbmDevice != nullptr; }

		// On failure, *pDmaBuf is left zeroed and owns nothing.
		bool CreateScanoutDmabuf( uint32_t uWidth, uint32_t uHeight, uint32_t uDrmFormat,
		                          std::span<const uint64_t> ulModifiers,
		                          wlr_dmabuf_attributes *pDmaBuf );

	private:
		gbm_device *m_pGbmDevice = nullptr;
	};
}
