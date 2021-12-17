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

class CVulkanTexture;

// These two structs are horrible
struct VulkanPipeline_t
{
	struct LayerBinding_t
	{
		int surfaceWidth;
		int surfaceHeight;
		
		std::shared_ptr<CVulkanTexture> tex;
		uint32_t fbid;
		
		int zpos;

		bool bFilter;
	} layerBindings[ k_nMaxLayers ];
};

struct vec2_t
{
	float x, y;
};

struct Composite_t
{
	int nLayerCount;
	int nYCBCRMask;

	struct CompositeData_t
	{
		vec2_t vScale[k_nMaxLayers];
		vec2_t vOffset[k_nMaxLayers];
		float flOpacity[k_nMaxLayers];
		uint32_t nBorderMask;
	} data;

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
#include <wlr/render/gles2.h>
#include <wlr/render/interface.h>
#include <wlr/render/egl.h>
#undef static
}

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
		}

		bool bFlippable : 1;
		bool bMappable : 1;
		bool bSampled : 1;
		bool bStorage : 1;
		bool bTransferSrc : 1;
		bool bTransferDst : 1;
		bool bLinear : 1;
	};

	bool BInit( uint32_t width, uint32_t height, VkFormat format, createFlags flags, wlr_dmabuf_attributes *pDMA = nullptr );

	CVulkanTexture( void );
	~CVulkanTexture( void );
	
	bool m_bInitialized = false;

	VkImage m_vkImage = VK_NULL_HANDLE;
	VkDeviceMemory m_vkImageMemory = VK_NULL_HANDLE;
	
	VkImageView m_vkImageView = VK_NULL_HANDLE;

	uint32_t m_width = 0, m_height = 0;
	uint32_t m_unRowPitch = 0;
	
	uint32_t m_FBID = 0;

	void *m_pMappedData = nullptr;

	VkFormat m_format = VK_FORMAT_UNDEFINED;
};

extern bool g_vulkanSupportsModifiers;

extern bool g_vulkanHasDrmPrimaryDevId;
extern dev_t g_vulkanDrmPrimaryDevId;

extern bool g_bIsCompositeDebug;

bool vulkan_init(void);
bool vulkan_init_formats(void);
bool vulkan_make_output(void);

std::shared_ptr<CVulkanTexture> vulkan_create_texture_from_dmabuf( struct wlr_dmabuf_attributes *pDMA );
std::shared_ptr<CVulkanTexture> vulkan_create_texture_from_bits( uint32_t width, uint32_t height, VkFormat format, CVulkanTexture::createFlags texCreateFlags, void *bits );
std::shared_ptr<CVulkanTexture> vulkan_create_texture_from_wlr_buffer( struct wlr_buffer *buf );

uint32_t vulkan_texture_get_fbid( const std::shared_ptr<CVulkanTexture>& vulkanTex );
int vulkan_texture_get_fence( const std::shared_ptr<CVulkanTexture>& vulkanTex );

bool vulkan_composite( struct Composite_t *pComposite, struct VulkanPipeline_t *pPipeline, std::shared_ptr<CVulkanTexture> *pScreenshotTexture );
uint32_t vulkan_get_last_composite_fbid( void );

void vulkan_present_to_window( void );

void vulkan_garbage_collect( void );
bool vulkan_remake_swapchain( void );
bool vulkan_remake_output_images( void );
bool acquire_next_image( void );

struct wlr_renderer *vulkan_renderer_create( void );
