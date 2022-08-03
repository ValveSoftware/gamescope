// Initialize Vulkan and composite stuff with a compute queue

#pragma once

#include <atomic>
#include <stdint.h>
#include <memory>

// 1: Fade Plane (Fade outs between switching focus)
// 2: Video Underlay (The actual video)
// 3: Video Streaming UI (Game, App)
// 4: External Overlay (Mangoapp, etc)
// 5: Primary Overlay (Steam Overlay)
// 6: Cursor

// or

// 1: Fade Plane (Fade outs between switching focus)
// 2: Base Plane (Game, App)
// 3: Override Plane (Dropdowns, etc)
// 4: External Overlay (Mangoapp, etc)
// 5: Primary Overlay (Steam Overlay)
// 6: Cursor
#define k_nMaxLayers 6
#define k_nMaxYcbcrMask 16

#define k_nMaxBlurLayers 2

#define kMaxBlurRadius (37u / 2 + 1)

enum BlurMode {
    BLUR_MODE_OFF = 0,
    BLUR_MODE_COND = 1,
    BLUR_MODE_ALWAYS = 2,
};

enum EStreamColorspace : int
{
	k_EStreamColorspace_Unknown = 0,
	k_EStreamColorspace_BT601 = 1,
	k_EStreamColorspace_BT601_Full = 2,
	k_EStreamColorspace_BT709 = 3,
	k_EStreamColorspace_BT709_Full = 4
};

#include "drm.hpp"

#include <memory>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <wayland-server-core.h>

extern "C" {
#define static
#include <wlr/render/dmabuf.h>
#include <wlr/render/interface.h>
#undef static
}

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <drm_fourcc.h>

struct VulkanRenderer_t
{
	struct wlr_renderer base;
};

struct VulkanWlrTexture_t
{
	struct wlr_texture base;
	struct wlr_buffer *buf;
};

class CVulkanTexture
{
public:
	struct createFlags {

		createFlags( void )
		{
			bFlippable = false;
			bMappable = false;
			bSampled = false;
			bStorage = false;
			bTransferSrc = false;
			bTransferDst = false;
			bLinear = false;
			bExportable = false;
		}

		bool bFlippable : 1;
		bool bMappable : 1;
		bool bSampled : 1;
		bool bStorage : 1;
		bool bTransferSrc : 1;
		bool bTransferDst : 1;
		bool bLinear : 1;
		bool bExportable : 1;
	};

	bool BInit( uint32_t width, uint32_t height, uint32_t drmFormat, createFlags flags, wlr_dmabuf_attributes *pDMA = nullptr, uint32_t contentWidth = 0, uint32_t contentHeight = 0 );
	bool BInitFromSwapchain( VkImage image, uint32_t width, uint32_t height, VkFormat format );

	inline VkImageView view( bool linear ) { return linear ? m_linearView : m_srgbView; }
	inline VkImageView linearView() { return m_linearView; }
	inline VkImageView srgbView() { return m_srgbView; }
	inline uint32_t width() { return m_width; }
	inline uint32_t height() { return m_height; }
	inline uint32_t contentWidth() {return m_contentWidth; }
	inline uint32_t contentHeight() {return m_contentHeight; }
	inline uint32_t rowPitch() { return m_unRowPitch; }
	inline uint32_t fbid() { return m_FBID; }
	inline void *mappedData() { return m_pMappedData; }
	inline VkFormat format() { return m_format; }
	inline const struct wlr_dmabuf_attributes& dmabuf() { return m_dmabuf; }
	inline VkImage vkImage() { return m_vkImage; }
	inline bool swapchainImage() { return m_vkImageMemory == VK_NULL_HANDLE; }
	inline bool externalImage() { return m_bExternal; }

	int memoryFence();

	CVulkanTexture( void );
	~CVulkanTexture( void );

private:
	bool m_bInitialized = false;
	bool m_bExternal = false;

	VkImage m_vkImage = VK_NULL_HANDLE;
	VkDeviceMemory m_vkImageMemory = VK_NULL_HANDLE;
	
	VkImageView m_srgbView = VK_NULL_HANDLE;
	VkImageView m_linearView = VK_NULL_HANDLE;

	uint32_t m_width = 0;
	uint32_t m_height = 0;

	uint32_t m_contentWidth = 0;
	uint32_t m_contentHeight = 0;

	uint32_t m_unRowPitch = 0;
	
	uint32_t m_FBID = 0;

	void *m_pMappedData = nullptr;

	VkFormat m_format = VK_FORMAT_UNDEFINED;

	struct wlr_dmabuf_attributes m_dmabuf = {};
};

struct vec2_t
{
	float x, y;
};

struct FrameInfo_t
{
	bool useFSRLayer0;
	bool useNISLayer0;
	BlurMode blurLayer0;
	int blurRadius;


	int layerCount;
	struct Layer_t
	{
		std::shared_ptr<CVulkanTexture> tex;
		uint32_t fbid; // TODO pretty sure we can just move this into tex
		int zpos;

		vec2_t offset;
		vec2_t scale;

		float opacity;

		bool blackBorder;
		bool linearFilter;

		bool isYcbcr() const
		{
			if ( !tex )
				return false;

			return tex->format() == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
		}

		uint32_t integerWidth() const { return tex->width() / scale.x; }
		uint32_t integerHeight() const { return tex->height() / scale.y; }
		vec2_t offsetPixelCenter() const
		{
			float x = offset.x + 0.5f / scale.x;
			float y = offset.y + 0.5f / scale.y;
			return { x, y };
		}
	} layers[ k_nMaxLayers ];

	uint32_t borderMask() const {
		uint32_t result = 0;
		for (int i = 0; i < layerCount; i++)
		{
			if (layers[ i ].blackBorder)
				result |= 1 << i;
		}
		return result;
	}
	uint32_t ycbcrMask() const {
		uint32_t result = 0;
		for (int i = 0; i < layerCount; i++)
		{
			if (layers[ i ].isYcbcr())
				result |= 1 << i;
		}
		return result;
	}
};

extern bool g_bIsCompositeDebug;

bool vulkan_init(void);
bool vulkan_init_formats(void);
bool vulkan_make_output(void);

std::shared_ptr<CVulkanTexture> vulkan_create_texture_from_dmabuf( struct wlr_dmabuf_attributes *pDMA );
std::shared_ptr<CVulkanTexture> vulkan_create_texture_from_bits( uint32_t width, uint32_t height, uint32_t contentWidth, uint32_t contentHeight, uint32_t drmFormat, CVulkanTexture::createFlags texCreateFlags, void *bits );
std::shared_ptr<CVulkanTexture> vulkan_create_texture_from_wlr_buffer( struct wlr_buffer *buf );

bool vulkan_composite( const struct FrameInfo_t *frameInfo, std::shared_ptr<CVulkanTexture> pScreenshotTexture );
std::shared_ptr<CVulkanTexture> vulkan_get_last_output_image( void );
std::shared_ptr<CVulkanTexture> vulkan_acquire_screenshot_texture(bool exportable);

void vulkan_present_to_window( void );

void vulkan_garbage_collect( void );
bool vulkan_remake_swapchain( void );
bool vulkan_remake_output_images( void );
bool acquire_next_image( void );

bool vulkan_primary_dev_id(dev_t *id);
bool vulkan_supports_modifiers(void);

struct wlr_renderer *vulkan_renderer_create( void );
