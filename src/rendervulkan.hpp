// Initialize Vulkan and composite stuff with a compute queue

#pragma once

#include <stdint.h>

typedef uint32_t VulkanTexture_t;

#define k_nMaxLayers 4
#define k_nMaxYcbcrMask 16

// These two structs are horrible
struct VulkanPipeline_t
{
	struct LayerBinding_t
	{
		int surfaceWidth;
		int surfaceHeight;
		
		VulkanTexture_t tex;
		uint32_t fbid;
		
		int zpos;

		// These fields below count for the sampler cache
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
	int nSwapChannels;
	int nYCBCRMask;

	struct CompositeData_t
	{
		vec2_t vScale[k_nMaxLayers];
		vec2_t vOffset[k_nMaxLayers];
		float flOpacity[k_nMaxLayers];
		float flBorderAlpha[k_nMaxLayers];
	} data;
};

#include "drm.hpp"

#include <unordered_map>
#include <vector>
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
			bTextureable = false;
			bMappable = false;
			bTransferSrc = false;
			bTransferDst = false;
			bLinear = false;
		}

		bool bFlippable : 1;
		bool bTextureable : 1;
		bool bMappable : 1;
		bool bTransferSrc : 1;
		bool bTransferDst : 1;
		bool bLinear : 1;
	};

	bool BInit( uint32_t width, uint32_t height, VkFormat format, createFlags flags, wlr_dmabuf_attributes *pDMA = nullptr );

	~CVulkanTexture( void );
	
	bool m_bInitialized = false;

	VkImage m_vkImage = VK_NULL_HANDLE;
	VkDeviceMemory m_vkImageMemory = VK_NULL_HANDLE;
	
	VkImageView m_vkImageView = VK_NULL_HANDLE;

	uint32_t m_unRowPitch = 0;
	
	uint32_t m_FBID = 0;
	
	int32_t nRefCount = 1;
	
	VulkanTexture_t handle = 0;

	void *m_pMappedData = nullptr;

	VkFormat m_format = VK_FORMAT_UNDEFINED;
};

extern bool g_vulkanSupportsModifiers;

extern bool g_vulkanHasDrmPrimaryDevId;
extern dev_t g_vulkanDrmPrimaryDevId;

bool vulkan_init(void);
bool vulkan_init_formats(void);
bool vulkan_make_output(void);

VulkanTexture_t vulkan_create_texture_from_dmabuf( struct wlr_dmabuf_attributes *pDMA );
VulkanTexture_t vulkan_create_texture_from_bits( uint32_t width, uint32_t height, VkFormat format, CVulkanTexture::createFlags texCreateFlags, void *bits );
VulkanTexture_t vulkan_create_texture_from_wlr_buffer( struct wlr_buffer *buf );

uint32_t vulkan_texture_get_fbid( VulkanTexture_t vulkanTex );
int vulkan_texture_get_fence( VulkanTexture_t vulkanTex );

void vulkan_free_texture( VulkanTexture_t vulkanTex );

bool vulkan_composite( struct Composite_t *pComposite, struct VulkanPipeline_t *pPipeline, CVulkanTexture **pScreenshotTexture );
uint32_t vulkan_get_last_composite_fbid( void );

void vulkan_present_to_window( void );

void vulkan_garbage_collect( void );
bool vulkan_remake_swapchain( void );
bool vulkan_remake_output_images( void );
bool acquire_next_image( void );

struct wlr_renderer *vulkan_renderer_create( void );
