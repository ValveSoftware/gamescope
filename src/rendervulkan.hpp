// Initialize Vulkan and composite stuff with a compute queue

#pragma once

#include <stdint.h>

typedef uint32_t VulkanTexture_t;

#define k_nMaxLayers 4

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
		bool bBlackBorder;
	} layerBindings[ k_nMaxLayers ];
};

struct Composite_t
{
	int nLayerCount;
	int nSwapChannels;
	
	struct CompositeData_t
	{
		struct
		{
			float flScaleX, flScaleY;
			float flOffsetX, flOffsetY;
			float flOpacity;
		} layers[ k_nMaxLayers ];
	} data;
};

#include "drm.hpp"

#include <unordered_map>
#include <vector>

extern "C" {
#include <wlr/render/dmabuf.h>
}

#include <vulkan/vulkan.h>
#include <drm_fourcc.h>

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

	int m_FD = -1;
	
	int32_t nRefCount = 1;
	
	VulkanTexture_t handle = 0;

	void *m_pMappedData = nullptr;
};

extern std::vector< const char * > g_vecSDLInstanceExts;

int vulkan_init(void);

VulkanTexture_t vulkan_create_texture_from_dmabuf( struct wlr_dmabuf_attributes *pDMA );
VulkanTexture_t vulkan_create_texture_from_bits( uint32_t width, uint32_t height, VkFormat format, void *bits );

int vulkan_get_texture_fence( VulkanTexture_t vulkanTex );

uint32_t vulkan_texture_get_fbid( VulkanTexture_t vulkanTex );

void vulkan_free_texture( VulkanTexture_t vulkanTex );

bool vulkan_composite( struct Composite_t *pComposite, struct VulkanPipeline_t *pPipeline, bool bScreenshot );
uint32_t vulkan_get_last_composite_fbid( void );

void vulkan_present_to_window( void );

void vulkan_garbage_collect( void );
bool vulkan_remake_swapchain( void );
