#pragma once

#include <cstdint>
#include <span>

struct gbm_device;
struct wlr_dmabuf_attributes;

namespace gamescope
{
	// NVIDIA's display engine requires physically contiguous scanout memory, a
	// placement constraint Vulkan external-memory allocation cannot express.
	// GBM allocations with GBM_BO_USE_SCANOUT do satisfy it, so the DRM backend
	// allocates its scanout buffers here and Vulkan imports them instead of
	// allocating them itself.
	//
	// This can be deleted if/when nvidia-drm can reliably scan out
	// Vulkan-allocated buffers.
	class CGbmScanoutAllocator
	{
	public:
		~CGbmScanoutAllocator();

		// Creates the GBM device. Logs and returns false on failure or when
		// built without GBM support.
		bool Init( int nDrmFd );
		void Shutdown();

		bool IsAvailable() const { return m_pGbmDevice != nullptr; }

		// Allocates a scanout-capable buffer and exports it as a DMA-BUF.
		// On failure, *pDmaBuf is left zeroed and owns nothing.
		bool CreateScanoutDmabuf( uint32_t uWidth, uint32_t uHeight, uint32_t uDrmFormat,
		                          std::span<const uint64_t> ulModifiers,
		                          wlr_dmabuf_attributes *pDmaBuf );

	private:
		gbm_device *m_pGbmDevice = nullptr;
	};

	// True if the DRM fd is driven by the nvidia-drm kernel driver.
	bool DrmDeviceIsNvidia( int nDrmFd );
}
