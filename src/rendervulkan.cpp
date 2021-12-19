// Initialize Vulkan and composite stuff with a compute queue

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <array>

#include "rendervulkan.hpp"
#include "main.hpp"
#include "steamcompmgr.hpp"
#include "sdlwindow.hpp"
#include "log.hpp"

#include "composite.h"

bool g_bIsCompositeDebug = false;

PFN_vkGetMemoryFdKHR dyn_vkGetMemoryFdKHR;
PFN_vkGetImageDrmFormatModifierPropertiesEXT dyn_vkGetImageDrmFormatModifierPropertiesEXT;

const VkApplicationInfo appInfo = {
	.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
	.pApplicationName = "gamescope",
	.applicationVersion = VK_MAKE_VERSION(1, 0, 0),
	.pEngineName = "just some code",
	.engineVersion = VK_MAKE_VERSION(1, 0, 0),
	.apiVersion = VK_API_VERSION_1_1,
};

VkInstance instance;

#define k_nMaxSets 8 // should only need one or two per output tops

struct VulkanOutput_t
{
	VkSurfaceKHR surface;
	VkSurfaceCapabilitiesKHR surfaceCaps;
	std::vector< VkSurfaceFormatKHR > surfaceFormats;
	std::vector< VkPresentModeKHR > presentModes;
	uint32_t nSwapChainImageIndex;
	
	VkSwapchainKHR swapChain;
	std::vector< VkImage > swapChainImages;
	std::vector< VkImageView > swapChainImageViews;
	VkFence acquireFence;
	
	// If no swapchain, use our own images
	
	int nOutImage; // ping/pong between two RTs
	std::shared_ptr<CVulkanTexture> outputImage[2];

	VkFormat outputFormat;

	int nCurCmdBuffer;
	VkCommandBuffer commandBuffers[2]; // ping/pong command buffers as well

	std::array<std::shared_ptr<CVulkanTexture>, 8> pScreenshotImages;
};


VkPhysicalDevice physicalDevice;
uint32_t queueFamilyIndex;
VkQueue queue;
VkShaderModule shaderModule;
VkDevice device;
VkCommandPool commandPool;
VkDescriptorPool descriptorPool;

bool g_vulkanSupportsModifiers;

bool g_vulkanHasDrmPrimaryDevId = false;
dev_t g_vulkanDrmPrimaryDevId = 0;

static int g_drmRenderFd = -1;

VkDescriptorSetLayout descriptorSetLayout;
VkPipelineLayout pipelineLayout;
VkDescriptorSet descriptorSet;

std::array<std::array<std::array<VkPipeline, k_nMaxYcbcrMask>, 2>, k_nMaxLayers> pipelines;

VkBuffer uploadBuffer;
VkDeviceMemory uploadBufferMemory;
void *pUploadBuffer;

const uint32_t k_nScratchCmdBufferCount = 1000;

struct scratchCmdBuffer_t
{
	VkCommandBuffer cmdBuf;
	VkFence fence;
	
	std::vector<std::shared_ptr<CVulkanTexture>> refs;
	
	std::atomic<bool> haswaiter;
	std::atomic<bool> busy;
};

scratchCmdBuffer_t g_scratchCommandBuffers[ k_nScratchCmdBufferCount ];

struct VkPhysicalDeviceMemoryProperties memoryProperties;

VulkanOutput_t g_output;

struct VulkanSamplerCacheKey_t
{
	bool bNearest : 1;
	bool bUnnormalized : 1;

	VulkanSamplerCacheKey_t( void )
	{
		bNearest = false;
		bUnnormalized = false;
	}

	bool operator==( const VulkanSamplerCacheKey_t& other ) const
	{
		return this->bNearest == other.bNearest
			&& this->bUnnormalized == other.bUnnormalized;
	}
};

namespace std
{
	template <>
	struct hash<VulkanSamplerCacheKey_t>
	{
		size_t operator()( const VulkanSamplerCacheKey_t& k ) const
		{
			return k.bNearest | (k.bUnnormalized << 1);
		}
	};
}

std::unordered_map< VulkanSamplerCacheKey_t, VkSampler > g_vulkanSamplerCache;

static std::map< VkFormat, std::map< uint64_t, VkDrmFormatModifierPropertiesEXT > > DRMModifierProps = {};
static std::vector< uint32_t > sampledShmFormats{};
static struct wlr_drm_format_set sampledDRMFormats = {};

static LogScope vk_log("vulkan");

static void vk_errorf(VkResult result, const char *fmt, ...) {
	static char buf[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	vk_log.errorf("%s (VkResult: %d)", buf, result);
}

#define MAX_DEVICE_COUNT 8
#define MAX_QUEUE_COUNT 8

#define VK_STRUCTURE_TYPE_WSI_IMAGE_CREATE_INFO_MESA (VkStructureType)1000001002
#define VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA (VkStructureType)1000001003

struct wsi_image_create_info {
	VkStructureType sType;
	const void *pNext;
	bool scanout;
	
	uint32_t modifier_count;
	const uint64_t *modifiers;
};

struct wsi_memory_allocate_info {
	VkStructureType sType;
	const void *pNext;
	bool implicit_sync;
};

struct {
	uint32_t DRMFormat;
	VkFormat vkFormat;
	VkFormat vkFormatSrgb;
	bool bNeedsSwizzle; // swap the R and the B channels
	bool bHasAlpha;
} s_DRMVKFormatTable[] = {
	{ DRM_FORMAT_ARGB8888, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, false, true },
	{ DRM_FORMAT_XRGB8888, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, false, false },
	{ DRM_FORMAT_ARGB8888, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, true, true },
	{ DRM_FORMAT_XRGB8888, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, true, false },
	{ DRM_FORMAT_NV12, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, false, false },
	{ DRM_FORMAT_INVALID, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, false, false },
};

static inline uint32_t VulkanFormatToDRM( VkFormat vkFormat )
{
	for ( int i = 0; s_DRMVKFormatTable[i].vkFormat != VK_FORMAT_UNDEFINED; i++ )
	{
		if ( s_DRMVKFormatTable[i].vkFormat == vkFormat || s_DRMVKFormatTable[i].vkFormatSrgb == vkFormat )
		{
			return s_DRMVKFormatTable[i].DRMFormat;
		}
	}
	
	return DRM_FORMAT_INVALID;
}

static inline VkFormat DRMFormatToVulkan( uint32_t nDRMFormat, bool bSrgb )
{
	for ( int i = 0; s_DRMVKFormatTable[i].vkFormat != VK_FORMAT_UNDEFINED; i++ )
	{
		if ( s_DRMVKFormatTable[i].DRMFormat == nDRMFormat )
		{
			return bSrgb ? s_DRMVKFormatTable[i].vkFormatSrgb : s_DRMVKFormatTable[i].vkFormat;
		}
	}
	
	return VK_FORMAT_UNDEFINED;
}

static inline bool DRMFormatNeedsSwizzle( uint32_t nDRMFormat )
{
	for ( int i = 0; s_DRMVKFormatTable[i].vkFormat != VK_FORMAT_UNDEFINED; i++ )
	{
		if ( s_DRMVKFormatTable[i].DRMFormat == nDRMFormat )
		{
			return s_DRMVKFormatTable[i].bNeedsSwizzle;
		}
	}
	
	return false;
}

static inline bool DRMFormatHasAlpha( uint32_t nDRMFormat )
{
	for ( int i = 0; s_DRMVKFormatTable[i].vkFormat != VK_FORMAT_UNDEFINED; i++ )
	{
		if ( s_DRMVKFormatTable[i].DRMFormat == nDRMFormat )
		{
			return s_DRMVKFormatTable[i].bHasAlpha;
		}
	}
	
	return false;
}

int32_t findMemoryType( VkMemoryPropertyFlags properties, uint32_t requiredTypeBits )
{
	for ( uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++ )
	{
		if ( ( ( 1 << i ) & requiredTypeBits ) == 0 )
			continue;
		
		if ( ( properties & memoryProperties.memoryTypes[ i ].propertyFlags ) != properties )
			continue;
		
		return i;
	}
	
	return -1;
}

static bool allDMABUFsEqual( wlr_dmabuf_attributes *pDMA )
{
	if ( pDMA->n_planes == 1 )
		return true;

	struct stat first_stat;
	if ( fstat( pDMA->fd[0], &first_stat ) != 0 )
	{
		vk_log.errorf_errno( "fstat failed" );
		return false;
	}

	for ( int i = 1; i < pDMA->n_planes; ++i )
	{
		struct stat plane_stat;
		if ( fstat( pDMA->fd[i], &plane_stat ) != 0 )
		{
			vk_log.errorf_errno( "fstat failed" );
			return false;
		}
		if ( plane_stat.st_ino != first_stat.st_ino )
			return false;
	}

	return true;
}

static VkResult getModifierProps( const VkImageCreateInfo *imageInfo, uint64_t modifier, VkExternalImageFormatProperties *externalFormatProps)
{
	VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifierFormatInfo = {};
	VkPhysicalDeviceExternalImageFormatInfo externalImageFormatInfo = {};
	VkPhysicalDeviceImageFormatInfo2 imageFormatInfo = {};
	VkImageFormatProperties2 imageProps = {};

	modifierFormatInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
	modifierFormatInfo.drmFormatModifier = modifier;
	modifierFormatInfo.sharingMode = imageInfo->sharingMode;

	externalImageFormatInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
	externalImageFormatInfo.pNext = &modifierFormatInfo;
	externalImageFormatInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

	imageFormatInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
	imageFormatInfo.pNext = &externalImageFormatInfo;
	imageFormatInfo.format = imageInfo->format;
	imageFormatInfo.type = imageInfo->imageType;
	imageFormatInfo.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	imageFormatInfo.usage = imageInfo->usage;
	imageFormatInfo.flags = imageInfo->flags;

	imageProps.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
	imageProps.pNext = externalFormatProps;

	return vkGetPhysicalDeviceImageFormatProperties2(physicalDevice, &imageFormatInfo, &imageProps);
}

bool CVulkanTexture::BInit( uint32_t width, uint32_t height, VkFormat format, createFlags flags, wlr_dmabuf_attributes *pDMA /* = nullptr */ )
{
	VkResult res = VK_ERROR_INITIALIZATION_FAILED;

	VkImageTiling tiling = (flags.bMappable || flags.bLinear) ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
	VkImageUsageFlags usage = flags.bTextureable ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_STORAGE_BIT;
	VkMemoryPropertyFlags properties;

	if ( flags.bTransferSrc == true )
	{
		usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	}

	if ( flags.bTransferDst == true )
	{
		usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	}

	if ( flags.bMappable == true )
	{
		properties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	}
	else
	{
		properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	}
	
	// Possible extensions for below
	wsi_image_create_info wsiImageCreateInfo = {};
	VkExternalMemoryImageCreateInfo externalImageCreateInfo = {};
	VkImageDrmFormatModifierExplicitCreateInfoEXT modifierInfo = {};
	VkSubresourceLayout modifierPlaneLayouts[4] = {};
	VkImageDrmFormatModifierListCreateInfoEXT modifierListInfo = {};
	
	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent.width = width;
	imageInfo.extent.height = height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.format = format;
	imageInfo.tiling = tiling;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = usage;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	if ( pDMA != nullptr )
	{
		assert( format == DRMFormatToVulkan( pDMA->format, true ) || format == DRMFormatToVulkan( pDMA->format, false ) );
	}

	if ( g_vulkanSupportsModifiers && pDMA && pDMA->modifier != DRM_FORMAT_MOD_INVALID )
	{
		VkExternalImageFormatProperties externalImageProperties = {};
		externalImageProperties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
		res = getModifierProps( &imageInfo, pDMA->modifier, &externalImageProperties );
		if ( res != VK_SUCCESS && res != VK_ERROR_FORMAT_NOT_SUPPORTED ) {
			vk_errorf( res, "getModifierProps failed" );
			return false;
		}

		if ( res == VK_SUCCESS &&
		     ( externalImageProperties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT ) )
		{
			modifierInfo.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
			modifierInfo.pNext = imageInfo.pNext;
			modifierInfo.drmFormatModifier = pDMA->modifier;
			modifierInfo.drmFormatModifierPlaneCount = pDMA->n_planes;
			modifierInfo.pPlaneLayouts = modifierPlaneLayouts;

			for ( int i = 0; i < pDMA->n_planes; ++i )
			{
				modifierPlaneLayouts[i].offset = pDMA->offset[i];
				modifierPlaneLayouts[i].rowPitch = pDMA->stride[i];
			}

			imageInfo.pNext = &modifierInfo;

			imageInfo.tiling = tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
		}
	}

	std::vector<uint64_t> modifiers = {};
	if ( flags.bFlippable == true && g_vulkanSupportsModifiers && !pDMA )
	{
		uint32_t drmFormat = VulkanFormatToDRM( format );
		assert( drmFormat != DRM_FORMAT_INVALID );

		uint64_t linear = DRM_FORMAT_MOD_LINEAR;

		const uint64_t *possibleModifiers;
		size_t numPossibleModifiers;
		if ( flags.bLinear )
		{
			possibleModifiers = &linear;
			numPossibleModifiers = 1;
		}
		else
		{
			const struct wlr_drm_format *drmFormatDesc = wlr_drm_format_set_get( &g_DRM.primary_formats, drmFormat );
			assert( drmFormatDesc != nullptr );
			possibleModifiers = drmFormatDesc->modifiers;
			numPossibleModifiers = drmFormatDesc->len;
		}

		for ( size_t i = 0; i < numPossibleModifiers; i++ )
		{
			uint64_t modifier = possibleModifiers[i];

			VkExternalImageFormatProperties externalFormatProps = {};
			externalFormatProps.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
			res = getModifierProps( &imageInfo, modifier, &externalFormatProps );
			if ( res == VK_ERROR_FORMAT_NOT_SUPPORTED )
				continue;
			else if ( res != VK_SUCCESS ) {
				vk_errorf( res, "getModifierProps failed" );
				return false;
			}

			if ( !( externalFormatProps.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT ) )
				continue;

			modifiers.push_back( modifier );
		}

		assert( modifiers.size() > 0 );

		modifierListInfo.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
		modifierListInfo.pNext = imageInfo.pNext;
		modifierListInfo.pDrmFormatModifiers = modifiers.data();
		modifierListInfo.drmFormatModifierCount = modifiers.size();

		imageInfo.pNext = &modifierListInfo;

		imageInfo.tiling = tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	}

	if ( flags.bFlippable == true && tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT )
	{
		// We want to scan-out the image
		wsiImageCreateInfo.sType = VK_STRUCTURE_TYPE_WSI_IMAGE_CREATE_INFO_MESA;
		wsiImageCreateInfo.scanout = VK_TRUE;
		wsiImageCreateInfo.pNext = imageInfo.pNext;
		
		imageInfo.pNext = &wsiImageCreateInfo;
	}
	
	if ( pDMA != nullptr )
	{
		externalImageCreateInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
		externalImageCreateInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		externalImageCreateInfo.pNext = imageInfo.pNext;
		
		imageInfo.pNext = &externalImageCreateInfo;
	}

	m_width = width;
	m_height = height;
	m_format = imageInfo.format;

	res = vkCreateImage(device, &imageInfo, nullptr, &m_vkImage);
	if (res != VK_SUCCESS) {
		vk_errorf( res, "vkCreateImage failed" );
		return false;
	}
	
	VkMemoryRequirements memRequirements;
	vkGetImageMemoryRequirements(device, m_vkImage, &memRequirements);
	
	// Possible pNexts
	wsi_memory_allocate_info wsiAllocInfo = {};
	VkImportMemoryFdInfoKHR importMemoryInfo = {};
	VkExportMemoryAllocateInfo memory_export_info = {};
	VkMemoryDedicatedAllocateInfo memory_dedicated_info = {};
	
	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = findMemoryType(properties, memRequirements.memoryTypeBits );
	
	if ( flags.bFlippable == true || pDMA != nullptr )
	{
		memory_dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
		memory_dedicated_info.image = m_vkImage;
		memory_dedicated_info.buffer = VK_NULL_HANDLE;
		memory_dedicated_info.pNext = allocInfo.pNext;
		
		allocInfo.pNext = &memory_dedicated_info;
	}
	
	if ( flags.bFlippable == true && pDMA == nullptr )
	{
		// We'll export it to DRM
		memory_export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
		memory_export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		memory_export_info.pNext = allocInfo.pNext;
		
		allocInfo.pNext = &memory_export_info;
	}
	
	if ( pDMA != nullptr )
	{
		// TODO: multi-planar DISTINCT DMA-BUFs support (see vkBindImageMemory2
		// and VkBindImagePlaneMemoryInfo)
		assert( allDMABUFsEqual( pDMA ) );

		// Importing memory from a FD transfers ownership of the FD
		int fd = dup( pDMA->fd[0] );
		if ( fd < 0 )
		{
			vk_log.errorf_errno( "dup failed" );
			return false;
		}

		// We're importing WSI buffers from GL or Vulkan, set implicit_sync
		wsiAllocInfo.sType = VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA;
		wsiAllocInfo.implicit_sync = true;
		wsiAllocInfo.pNext = allocInfo.pNext;
		
		allocInfo.pNext = &wsiAllocInfo;
		
		// Memory already provided by pDMA
		importMemoryInfo.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
		importMemoryInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		importMemoryInfo.fd = fd;
		importMemoryInfo.pNext = allocInfo.pNext;
		
		allocInfo.pNext = &importMemoryInfo;
	}
	
	res = vkAllocateMemory( device, &allocInfo, nullptr, &m_vkImageMemory );
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkAllocateMemory failed" );
		return false;
	}
	
	res = vkBindImageMemory( device, m_vkImage, m_vkImageMemory, 0 );
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkBindImageMemory failed" );
		return false;
	}

	if ( flags.bMappable == true )
	{
		assert( tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT );
		const VkImageSubresource image_subresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.mipLevel = 0,
			.arrayLayer = 0,
		};
		VkSubresourceLayout image_layout;
		vkGetImageSubresourceLayout(device, m_vkImage, &image_subresource, &image_layout);

		m_unRowPitch = image_layout.rowPitch;
	}
	
	if ( flags.bFlippable == true )
	{
		// We assume we own the memory when doing this right now.
		// We could support the import scenario as well if needed (but we
		// already have a DMA-BUF in that case).
// 		assert( bTextureable == false );
		assert( pDMA == nullptr );

		struct wlr_dmabuf_attributes dmabuf = {};
		dmabuf.width = width;
		dmabuf.height = height;
		dmabuf.format = VulkanFormatToDRM( format );
		assert( dmabuf.format != DRM_FORMAT_INVALID );

		// TODO: disjoint planes support
		const VkMemoryGetFdInfoKHR memory_get_fd_info = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
			.pNext = NULL,
			.memory = m_vkImageMemory,
			.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		};
		res = dyn_vkGetMemoryFdKHR(device, &memory_get_fd_info, &dmabuf.fd[0]);
		if ( res != VK_SUCCESS ) {
			vk_errorf( res, "vkGetMemoryFdKHR failed" );
			return false;
		}

		if ( tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT )
		{
			assert( dyn_vkGetImageDrmFormatModifierPropertiesEXT != nullptr );
			VkImageDrmFormatModifierPropertiesEXT imgModifierProps = {};
			imgModifierProps.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
			res = dyn_vkGetImageDrmFormatModifierPropertiesEXT( device, m_vkImage, &imgModifierProps );
			if ( res != VK_SUCCESS ) {
				vk_errorf( res, "vkGetImageDrmFormatModifierPropertiesEXT failed" );
				return false;
			}
			dmabuf.modifier = imgModifierProps.drmFormatModifier;

			assert( DRMModifierProps.count( format ) > 0);
			assert( DRMModifierProps[ format ].count( dmabuf.modifier ) > 0);

			dmabuf.n_planes = DRMModifierProps[ format ][ dmabuf.modifier ].drmFormatModifierPlaneCount;

			const VkImageAspectFlagBits planeAspects[] = {
				VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
				VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
				VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
				VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT,
			};
			assert( dmabuf.n_planes <= 4 );

			for ( int i = 0; i < dmabuf.n_planes; i++ )
			{
				const VkImageSubresource subresource = {
					.aspectMask = planeAspects[i],
					.mipLevel = 0,
					.arrayLayer = 0,
				};
				VkSubresourceLayout subresourceLayout = {};
				vkGetImageSubresourceLayout( device, m_vkImage, &subresource, &subresourceLayout );
				dmabuf.offset[i] = subresourceLayout.offset;
				dmabuf.stride[i] = subresourceLayout.rowPitch;
			}

			// Copy the first FD to all other planes
			for ( int i = 1; i < dmabuf.n_planes; i++ )
			{
				dmabuf.fd[i] = dup( dmabuf.fd[0] );
				if ( dmabuf.fd[i] < 0 ) {
					vk_log.errorf_errno( "dup failed" );
					return false;
				}
			}
		}
		else
		{
			const VkImageSubresource subresource = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.mipLevel = 0,
				.arrayLayer = 0,
			};
			VkSubresourceLayout subresourceLayout = {};
			vkGetImageSubresourceLayout( device, m_vkImage, &subresource, &subresourceLayout );

			dmabuf.n_planes = 1;
			dmabuf.modifier = DRM_FORMAT_MOD_INVALID;
			dmabuf.offset[0] = 0;
			dmabuf.stride[0] = subresourceLayout.rowPitch;
		}

		m_FBID = drm_fbid_from_dmabuf( &g_DRM, nullptr, &dmabuf );
		if ( m_FBID == 0 ) {
			vk_log.errorf( "drm_fbid_from_dmabuf failed" );
			return false;
		}

		wlr_dmabuf_attributes_finish( &dmabuf );
	}

	bool bSwapChannels = pDMA ? DRMFormatNeedsSwizzle( pDMA->format ) : false;
	bool bHasAlpha = pDMA ? DRMFormatHasAlpha( pDMA->format ) : true;

	if ( bSwapChannels || !bHasAlpha )
	{
		// Right now this implies no storage bit - check it now as that's incompatible with swizzle
		assert ( flags.bTextureable == true );
	}
	
	VkImageViewCreateInfo createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	createInfo.image = m_vkImage;
	createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	createInfo.format = format;
	createInfo.components.r = bSwapChannels ? VK_COMPONENT_SWIZZLE_B : VK_COMPONENT_SWIZZLE_IDENTITY;
	createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
	createInfo.components.b = bSwapChannels ? VK_COMPONENT_SWIZZLE_R : VK_COMPONENT_SWIZZLE_IDENTITY;
	createInfo.components.a = bHasAlpha ? VK_COMPONENT_SWIZZLE_IDENTITY : VK_COMPONENT_SWIZZLE_ONE;
	createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	createInfo.subresourceRange.baseMipLevel = 0;
	createInfo.subresourceRange.levelCount = 1;
	createInfo.subresourceRange.baseArrayLayer = 0;
	createInfo.subresourceRange.layerCount = 1;
	
	res = vkCreateImageView(device, &createInfo, nullptr, &m_vkImageView);
	if ( res != VK_SUCCESS ) {
		vk_errorf( res, "vkCreateImageView failed" );
		return false;
	}

	if ( flags.bMappable )
	{
		res = vkMapMemory( device, m_vkImageMemory, 0, VK_WHOLE_SIZE, 0, &m_pMappedData );
		if ( res != VK_SUCCESS )
		{
			vk_errorf( res, "vkMapMemory failed" );
			return false;
		}
	}
	
	m_bInitialized = true;
	
	return true;
}

CVulkanTexture::CVulkanTexture( void )
{
}

CVulkanTexture::~CVulkanTexture( void )
{
	if ( m_pMappedData != nullptr )
	{
		vkUnmapMemory( device, m_vkImageMemory );
		m_pMappedData = nullptr;
	}

	if ( m_vkImageView != VK_NULL_HANDLE )
	{
		vkDestroyImageView( device, m_vkImageView, nullptr );
		m_vkImageView = VK_NULL_HANDLE;
	}

	if ( m_FBID != 0 )
	{
		drm_drop_fbid( &g_DRM, m_FBID );
		m_FBID = 0;
	}


	if ( m_vkImage != VK_NULL_HANDLE )
	{
		vkDestroyImage( device, m_vkImage, nullptr );
		m_vkImage = VK_NULL_HANDLE;
	}


	if ( m_vkImageMemory != VK_NULL_HANDLE )
	{
		vkFreeMemory( device, m_vkImageMemory, nullptr );
		m_vkImageMemory = VK_NULL_HANDLE;
	}

	m_bInitialized = false;
}

bool vulkan_init_formats()
{
	for ( size_t i = 0; s_DRMVKFormatTable[i].DRMFormat != DRM_FORMAT_INVALID; i++ )
	{
		VkFormat format = s_DRMVKFormatTable[i].vkFormat;
		uint32_t drmFormat = s_DRMVKFormatTable[i].DRMFormat;

		// First, check whether the Vulkan format is supported
		VkPhysicalDeviceImageFormatInfo2 imageFormatInfo = {};
		imageFormatInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
		imageFormatInfo.format = format;
		imageFormatInfo.type = VK_IMAGE_TYPE_2D;
		imageFormatInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageFormatInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
		imageFormatInfo.flags = 0;
		VkImageFormatProperties2 imageFormatProps = {};
		imageFormatProps.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
		VkResult res = vkGetPhysicalDeviceImageFormatProperties2( physicalDevice, &imageFormatInfo, &imageFormatProps );
		if ( res == VK_ERROR_FORMAT_NOT_SUPPORTED )
		{
			continue;
		}
		else if ( res != VK_SUCCESS )
		{
			vk_errorf( res, "vkGetPhysicalDeviceImageFormatProperties2 failed for DRM format 0x%" PRIX32, drmFormat );
			continue;
		}

		sampledShmFormats.push_back( drmFormat );

		if ( !g_vulkanSupportsModifiers )
		{
			if ( BIsNested() == false && !wlr_drm_format_set_has( &g_DRM.formats, drmFormat, DRM_FORMAT_MOD_INVALID ) )
			{
				continue;
			}
			wlr_drm_format_set_add( &sampledDRMFormats, drmFormat, DRM_FORMAT_MOD_INVALID );
			continue;
		}

		// Then, collect the list of modifiers supported for sampled usage
		VkDrmFormatModifierPropertiesListEXT modifierPropList = {};
		modifierPropList.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
		VkFormatProperties2 formatProps = {};
		formatProps.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
		formatProps.pNext = &modifierPropList;
		vkGetPhysicalDeviceFormatProperties2( physicalDevice, format, &formatProps );

		if ( modifierPropList.drmFormatModifierCount == 0 )
		{
			vk_errorf( res, "vkGetPhysicalDeviceFormatProperties2 returned zero modifiers for DRM format 0x%" PRIX32, drmFormat );
			continue;
		}

		std::vector<VkDrmFormatModifierPropertiesEXT> modifierProps(modifierPropList.drmFormatModifierCount);
		modifierPropList.pDrmFormatModifierProperties = modifierProps.data();
		vkGetPhysicalDeviceFormatProperties2( physicalDevice, format, &formatProps );

		std::map< uint64_t, VkDrmFormatModifierPropertiesEXT > map = {};

		for ( size_t j = 0; j < modifierProps.size(); j++ )
		{
			map[ modifierProps[j].drmFormatModifier ] = modifierProps[j];

			uint64_t modifier = modifierProps[j].drmFormatModifier;

			if ( ( modifierProps[j].drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ) == 0 )
			{
				continue;
			}
			if ( BIsNested() == false && !wlr_drm_format_set_has( &g_DRM.formats, drmFormat, modifier ) )
			{
				continue;
			}
			if ( BIsNested() == false && drmFormat == DRM_FORMAT_NV12 && modifier == DRM_FORMAT_MOD_LINEAR && g_bRotated )
			{
				// If embedded and rotated, blacklist NV12 LINEAR because
				// amdgpu won't support direct scan-out. Since only pure
				// Wayland clients can submit NV12 buffers, this should only
				// affect streaming_client.
				continue;
			}
			wlr_drm_format_set_add( &sampledDRMFormats, drmFormat, modifier );
		}

		DRMModifierProps[ format ] = map;
	}

	vk_log.infof( "supported DRM formats for sampling usage:" );
	for ( size_t i = 0; i < sampledDRMFormats.len; i++ )
	{
		uint32_t fmt = sampledDRMFormats.formats[ i ]->format;
		vk_log.infof( "  0x%" PRIX32, fmt );
	}

	return true;
}

static bool is_vulkan_1_1_device(VkPhysicalDevice device)
{
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(device, &properties);

	return properties.apiVersion >= VK_API_VERSION_1_1;
}

static bool init_device()
{
	uint32_t physicalDeviceCount = 0;
	VkPhysicalDevice deviceHandles[MAX_DEVICE_COUNT];
	VkQueueFamilyProperties queueFamilyProperties[MAX_QUEUE_COUNT];
	
	vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, 0);
	physicalDeviceCount = physicalDeviceCount > MAX_DEVICE_COUNT ? MAX_DEVICE_COUNT : physicalDeviceCount;
	vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, deviceHandles);

	bool bTryComputeOnly = true;

	// In theory vkBasalt might want to filter out compute-only queue families to force our hand here
	const char *pchEnableVkBasalt = getenv( "ENABLE_VKBASALT" );
	if ( pchEnableVkBasalt != nullptr && pchEnableVkBasalt[0] == '1' )
	{
		bTryComputeOnly = false;
	}
	
retry:
	for (uint32_t i = 0; i < physicalDeviceCount; ++i)
	{
		if (!is_vulkan_1_1_device(deviceHandles[i]))
			continue;

		uint32_t queueFamilyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(deviceHandles[i], &queueFamilyCount, NULL);
		queueFamilyCount = queueFamilyCount > MAX_QUEUE_COUNT ? MAX_QUEUE_COUNT : queueFamilyCount;
		vkGetPhysicalDeviceQueueFamilyProperties(deviceHandles[i], &queueFamilyCount, queueFamilyProperties);
		
		for (uint32_t j = 0; j < queueFamilyCount; ++j) {
			if ( queueFamilyProperties[j].queueFlags & VK_QUEUE_COMPUTE_BIT &&
				( bTryComputeOnly == false || !(queueFamilyProperties[j].queueFlags & VK_QUEUE_GRAPHICS_BIT ) ) &&
				( bTryComputeOnly == true || queueFamilyProperties[j].queueFlags & VK_QUEUE_GRAPHICS_BIT ) )
			{
				queueFamilyIndex = j;
				physicalDevice = deviceHandles[i];
			}
		}
		
		if (physicalDevice)
		{
			break;
		}
	}

	if ( bTryComputeOnly == true && physicalDevice == VK_NULL_HANDLE )
	{
		bTryComputeOnly = false;
		goto retry;
	}

	if (!physicalDevice)
	{
		vk_log.errorf("failed to find physical device");
		return false;
	}

	VkPhysicalDeviceProperties props = {};
	vkGetPhysicalDeviceProperties( physicalDevice, &props );
	vk_log.infof( "selecting physical device '%s'", props.deviceName );

	vkGetPhysicalDeviceMemoryProperties( physicalDevice, &memoryProperties );

	uint32_t supportedExtensionCount;
	vkEnumerateDeviceExtensionProperties( physicalDevice, NULL, &supportedExtensionCount, NULL );

	std::vector<VkExtensionProperties> vecSupportedExtensions(supportedExtensionCount);
	vkEnumerateDeviceExtensionProperties( physicalDevice, NULL, &supportedExtensionCount, vecSupportedExtensions.data() );

	g_vulkanSupportsModifiers = false;
	bool hasDrmProps = false;
	bool hasPciBusProps = false;
	bool supportsForeignQueue = false;
	for ( uint32_t i = 0; i < supportedExtensionCount; ++i )
	{
		if ( strcmp(vecSupportedExtensions[i].extensionName,
		     VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) == 0 )
			g_vulkanSupportsModifiers = true;

		if ( strcmp(vecSupportedExtensions[i].extensionName,
		            VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME) == 0 )
			hasDrmProps = true;

		if ( strcmp(vecSupportedExtensions[i].extensionName,
		            VK_EXT_PCI_BUS_INFO_EXTENSION_NAME) == 0 )
			hasPciBusProps = true;

		if ( strcmp(vecSupportedExtensions[i].extensionName,
		     VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME) == 0 )
			supportsForeignQueue = true;
	}

	vk_log.infof( "physical device %s DRM format modifiers", g_vulkanSupportsModifiers ? "supports" : "does not support" );

	if ( hasDrmProps ) {
		VkPhysicalDeviceDrmPropertiesEXT drmProps = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
		};
		VkPhysicalDeviceProperties2 props2 = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			.pNext = &drmProps,
		};
		vkGetPhysicalDeviceProperties2( physicalDevice, &props2 );

		if ( !BIsNested() && !drmProps.hasPrimary ) {
			vk_log.errorf( "physical device has no primary node" );
			return false;
		}
		if ( !drmProps.hasRender ) {
			vk_log.errorf( "physical device has no render node" );
			return false;
		}

		dev_t renderDevId = makedev( drmProps.renderMajor, drmProps.renderMinor );
		char *renderName = find_drm_node_by_devid( renderDevId );
		if ( renderName == nullptr ) {
			vk_log.errorf( "failed to find DRM node" );
			return false;
		}

		g_drmRenderFd = open( renderName, O_RDWR | O_CLOEXEC );
		if ( g_drmRenderFd < 0 ) {
			vk_log.errorf_errno( "failed to open DRM render node" );
			return false;
		}

		if ( drmProps.hasPrimary ) {
			g_vulkanHasDrmPrimaryDevId = true;
			g_vulkanDrmPrimaryDevId = makedev( drmProps.primaryMajor, drmProps.primaryMinor );
		}
	} else if ( hasPciBusProps ) {
		// TODO: drop this logic once VK_EXT_physical_device_drm support is widespread

		VkPhysicalDevicePCIBusInfoPropertiesEXT pciBusProps = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT,
		};
		VkPhysicalDeviceProperties2 props2 = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			.pNext = &pciBusProps,
		};
		vkGetPhysicalDeviceProperties2( physicalDevice, &props2 );

		drmDevice *drmDevices[32];
		int drmDevicesLen = drmGetDevices2(0, drmDevices, sizeof(drmDevices) / sizeof(drmDevices[0]));
		if (drmDevicesLen < 0) {
			vk_log.errorf_errno("drmGetDevices2 failed");
			return false;
		}

		drmDevice *match = nullptr;
		for ( int i = 0; i < drmDevicesLen; i++ ) {
			drmDevice *drmDev = drmDevices[ i ];
			if ( !( drmDev->available_nodes & ( 1 << DRM_NODE_RENDER ) ) )
				continue;
			if ( drmDev->bustype != DRM_BUS_PCI )
				continue;

			if ( pciBusProps.pciDevice == drmDev->businfo.pci->dev && pciBusProps.pciBus == drmDev->businfo.pci->bus && pciBusProps.pciDomain == drmDev->businfo.pci->domain && pciBusProps.pciFunction == drmDev->businfo.pci->func ) {
				match = drmDev;
				break;
			}
		}
		if (match == nullptr) {
			vk_log.errorf("failed to find DRM device from PCI bus info");
		}

		g_drmRenderFd = open( match->nodes[ DRM_NODE_RENDER ], O_RDWR | O_CLOEXEC );
		if ( g_drmRenderFd < 0 ) {
			vk_log.errorf_errno( "failed to open DRM render node" );
			return false;
		}

		drmFreeDevices( drmDevices, drmDevicesLen );
	} else {
		vk_log.errorf( "physical device doesn't support VK_EXT_physical_device_drm nor VK_EXT_pci_bus_info" );
		return false;
	}

	if ( g_vulkanSupportsModifiers && !supportsForeignQueue && !BIsNested() ) {
		vk_log.infof( "The vulkan driver does not support foreign queues,"
		              " disabling modifier support.");
		g_vulkanSupportsModifiers = false;
	}

	float queuePriorities = 1.0f;

	VkDeviceQueueGlobalPriorityCreateInfoEXT queueCreateInfoEXT = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_EXT,
		.pNext = nullptr,
		.globalPriority = VK_QUEUE_GLOBAL_PRIORITY_REALTIME_EXT
	};
	
	VkDeviceQueueCreateInfo queueCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.pNext = nullptr,
		.queueFamilyIndex = queueFamilyIndex,
		.queueCount = 1,
		.pQueuePriorities = &queuePriorities
	};

	if ( g_bNiceCap == true )
	{
		queueCreateInfo.pNext = &queueCreateInfoEXT;
	}
	
	std::vector< const char * > vecEnabledDeviceExtensions;
	
	if ( BIsNested() == true )
	{
		vecEnabledDeviceExtensions.push_back( VK_KHR_SWAPCHAIN_EXTENSION_NAME );
	}

	if ( g_vulkanSupportsModifiers )
	{
		vecEnabledDeviceExtensions.push_back( VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME );
		vecEnabledDeviceExtensions.push_back( VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME ); // Required.

		if ( !BIsNested() )
			vecEnabledDeviceExtensions.push_back( VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME );
	}

	vecEnabledDeviceExtensions.push_back( VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME );
	vecEnabledDeviceExtensions.push_back( VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME );

	vecEnabledDeviceExtensions.push_back( VK_KHR_SHADER_CLOCK_EXTENSION_NAME );

	vecEnabledDeviceExtensions.push_back( VK_EXT_ROBUSTNESS_2_EXTENSION_NAME );
	
	VkPhysicalDeviceFeatures2 features2 = {};
	features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	
	VkDeviceCreateInfo deviceCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &features2,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queueCreateInfo,
		.enabledLayerCount = 0,
		.ppEnabledLayerNames = 0,
		.enabledExtensionCount = (uint32_t)vecEnabledDeviceExtensions.size(),
		.ppEnabledExtensionNames = vecEnabledDeviceExtensions.data(),
		.pEnabledFeatures = 0,
	};

	VkPhysicalDeviceShaderClockFeaturesKHR clockFeatures = {};
	clockFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR;
	clockFeatures.pNext = std::exchange(features2.pNext, &clockFeatures);
	clockFeatures.shaderSubgroupClock = VK_TRUE;

	VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrFeatures = {};
	ycbcrFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
	ycbcrFeatures.pNext = std::exchange(features2.pNext, &ycbcrFeatures);
	ycbcrFeatures.samplerYcbcrConversion = VK_TRUE;

	VkPhysicalDeviceRobustness2FeaturesEXT robustness2Features = {};
	robustness2Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
	robustness2Features.pNext = std::exchange(features2.pNext, &robustness2Features);
	robustness2Features.nullDescriptor = VK_TRUE;

	VkResult res = vkCreateDevice(physicalDevice, &deviceCreateInfo, NULL, &device);
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkCreateDevice failed" );
		return false;
	}
	
	vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);
	if ( queue == VK_NULL_HANDLE )
	{
		vk_log.errorf( "vkGetDeviceQueue failed" );
		return false;
	}
	
	VkShaderModuleCreateInfo shaderModuleCreateInfo = {};
	shaderModuleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shaderModuleCreateInfo.codeSize = sizeof(composite_spv);
	shaderModuleCreateInfo.pCode = (const uint32_t*)composite_spv;
	
	res = vkCreateShaderModule( device, &shaderModuleCreateInfo, nullptr, &shaderModule );
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkCreateShaderModule failed" );
		return false;
	}
	
	VkCommandPoolCreateInfo commandPoolCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.pNext = nullptr,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = queueFamilyIndex,
	};

	res = vkCreateCommandPool(device, &commandPoolCreateInfo, 0, &commandPool);
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkCreateCommandPool failed" );
		return false;
	}

	VkDescriptorPoolSize descriptorPoolSize[] = {
		{
			VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			k_nMaxSets * 1,
		},
		{
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			2 * k_nMaxSets * k_nMaxLayers,
		},
	};
	
	VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.maxSets = k_nMaxSets,
		.poolSizeCount = sizeof(descriptorPoolSize) / sizeof(descriptorPoolSize[0]),
		.pPoolSizes = descriptorPoolSize
	};
	
	res = vkCreateDescriptorPool(device, &descriptorPoolCreateInfo, 0, &descriptorPool);
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkCreateDescriptorPool failed" );
		return false;
	}

	VkFormatProperties nv12Properties;
	vkGetPhysicalDeviceFormatProperties(physicalDevice, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, &nv12Properties);
	bool cosited = nv12Properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT;

	VkSamplerYcbcrConversionCreateInfo ycbcrSamplerConversionCreateInfo = 
	{
		.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
		.pNext = nullptr,
		.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
		.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601,
		.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW,
		.xChromaOffset = cosited ? VK_CHROMA_LOCATION_COSITED_EVEN : VK_CHROMA_LOCATION_MIDPOINT,
		.yChromaOffset = cosited ? VK_CHROMA_LOCATION_COSITED_EVEN : VK_CHROMA_LOCATION_MIDPOINT,
		.chromaFilter = VK_FILTER_LINEAR,
		.forceExplicitReconstruction = VK_FALSE,
	};

	VkSamplerYcbcrConversion ycbcrConversion;
	vkCreateSamplerYcbcrConversion( device, &ycbcrSamplerConversionCreateInfo, nullptr, &ycbcrConversion );

	VkSampler ycbcrSampler = VK_NULL_HANDLE;

	VkSamplerYcbcrConversionInfo ycbcrSamplerConversionInfo = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
		.pNext = nullptr,
		.conversion = ycbcrConversion,
	};

	VkSamplerCreateInfo ycbcrSamplerInfo = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.pNext = &ycbcrSamplerConversionInfo,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
	};
	
	vkCreateSampler( device, &ycbcrSamplerInfo, nullptr, &ycbcrSampler );

	// Create an array of our ycbcrSampler to fill up
	std::array<VkSampler, k_nMaxLayers> ycbcrSamplers;
	for (auto& sampler : ycbcrSamplers)
		sampler = ycbcrSampler;
	
	std::vector< VkDescriptorSetLayoutBinding > vecLayoutBindings;
	VkDescriptorSetLayoutBinding descriptorSetLayoutBindings =
	{
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	};
	
	vecLayoutBindings.push_back( descriptorSetLayoutBindings ); // first binding is target storage image
	
	descriptorSetLayoutBindings.binding = 1;
	descriptorSetLayoutBindings.descriptorCount = k_nMaxLayers;
	descriptorSetLayoutBindings.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

	vecLayoutBindings.push_back( descriptorSetLayoutBindings );

	descriptorSetLayoutBindings.binding = 2;
	descriptorSetLayoutBindings.descriptorCount = k_nMaxLayers;
	descriptorSetLayoutBindings.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	descriptorSetLayoutBindings.pImmutableSamplers = ycbcrSamplers.data();

	vecLayoutBindings.push_back( descriptorSetLayoutBindings );
	
	VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo =
	{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.bindingCount = (uint32_t)vecLayoutBindings.size(),
		.pBindings = vecLayoutBindings.data()
	};
	
	res = vkCreateDescriptorSetLayout(device, &descriptorSetLayoutCreateInfo, 0, &descriptorSetLayout);
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkCreateDescriptorSetLayout failed" );
		return false;
	}

	VkPushConstantRange pushConstantRange = {
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		.offset = 0,
		.size = uint32_t(sizeof(Composite_t::CompositeData_t) + sizeof(uint32_t)),
	};
	
	VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {
		VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		0,
		0,
		1,
		&descriptorSetLayout,
		1,
		&pushConstantRange
	};
	
	res = vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, 0, &pipelineLayout);
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkCreatePipelineLayout failed" );
		return false;
	}

	const std::array<VkSpecializationMapEntry, 4> specializationEntries = {{
		{
			.constantID = 0,
			.offset     = 0,
			.size       = sizeof(uint32_t)
		},
		{
			.constantID = 1,
			.offset     = sizeof(uint32_t),
			.size       = sizeof(VkBool32)
		},
		{
			.constantID = 2,
			.offset     = sizeof(uint32_t) + sizeof(uint32_t),
			.size       = sizeof(uint32_t)
		},
		{
			.constantID = 3,
			.offset     = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t),
			.size       = sizeof(uint32_t)
		},
	}};
	
	for (uint32_t layerCount = 0; layerCount < k_nMaxLayers; layerCount++) {
		for (VkBool32 swapChannels = 0; swapChannels < 2; swapChannels++) {
			for (uint32_t ycbcrMask = 0; ycbcrMask < k_nMaxYcbcrMask; ycbcrMask++) {
				struct {
					uint32_t layerCount;
					VkBool32 swapChannels;
					uint32_t ycbcrMask;
					uint32_t debug;
				} specializationData = {
					.layerCount   = layerCount + 1,
					.swapChannels = swapChannels,
					.ycbcrMask    = ycbcrMask,
					.debug        = g_bIsCompositeDebug,
				};

				VkSpecializationInfo specializationInfo = {
					.mapEntryCount = uint32_t(specializationEntries.size()),
					.pMapEntries   = specializationEntries.data(),
					.dataSize      = sizeof(specializationData),
					.pData		   = &specializationData,
				};

				VkComputePipelineCreateInfo computePipelineCreateInfo = {
					VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
					0,
					0,
					{
						VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
						0,
						0,
						VK_SHADER_STAGE_COMPUTE_BIT,
						shaderModule,
						"main",
						&specializationInfo
					},
					pipelineLayout,
					0,
					0
				};

				res = vkCreateComputePipelines(device, 0, 1, &computePipelineCreateInfo, 0, &pipelines[layerCount][swapChannels][ycbcrMask]);
				if (res != VK_SUCCESS) {
					vk_errorf( res, "vkCreateComputePipelines failed" );
					return false;
				}
			}
		}
	}
	
	VkDescriptorSetAllocateInfo descriptorSetAllocateInfo = {
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		nullptr,
		descriptorPool,
		1,
		&descriptorSetLayout
	};
	
	res = vkAllocateDescriptorSets(device, &descriptorSetAllocateInfo, &descriptorSet);
	if ( res != VK_SUCCESS || descriptorSet == VK_NULL_HANDLE )
	{
		vk_log.errorf( "vkAllocateDescriptorSets failed" );
		return false;
	}
	
	// Make and map upload buffer
	
	VkBufferCreateInfo bufferCreateInfo = {};
	bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferCreateInfo.pNext = nullptr;
	bufferCreateInfo.size = 512 * 512 * 4;
	bufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	
	res = vkCreateBuffer( device, &bufferCreateInfo, nullptr, &uploadBuffer );
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkCreateBuffer failed" );
		return false;
	}
	
	VkMemoryRequirements memRequirements;
	vkGetBufferMemoryRequirements(device, uploadBuffer, &memRequirements);
	
	int memTypeIndex =  findMemoryType(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, memRequirements.memoryTypeBits );
	if ( memTypeIndex == -1 )
	{
		vk_log.errorf( "findMemoryType failed" );
		return false;
	}
	
	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = memTypeIndex;
	
	vkAllocateMemory( device, &allocInfo, nullptr, &uploadBufferMemory);
	
	vkBindBufferMemory( device, uploadBuffer, uploadBufferMemory, 0 );

	res = vkMapMemory( device, uploadBufferMemory, 0, VK_WHOLE_SIZE, 0, (void**)&pUploadBuffer );
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkMapMemory failed" );
		return false;
	}
	
	VkCommandBufferAllocateInfo commandBufferAllocateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.pNext = nullptr,
		.commandPool = commandPool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1
	};
	
	for ( uint32_t i = 0; i < k_nScratchCmdBufferCount; i++ )
	{
		res = vkAllocateCommandBuffers( device, &commandBufferAllocateInfo, &g_scratchCommandBuffers[ i ].cmdBuf );
		if ( res != VK_SUCCESS )
		{
			vk_errorf( res, "vkAllocateCommandBuffers failed" );
			return false;
		}
		
		VkFenceCreateInfo fenceCreateInfo =
		{
			.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
		};
		
		res = vkCreateFence( device, &fenceCreateInfo, nullptr, &g_scratchCommandBuffers[ i ].fence );
		if ( res != VK_SUCCESS )
		{
			vk_errorf( res, "vkCreateFence failed" );
			return false;
		}
		
		g_scratchCommandBuffers[ i ].busy = false;
	}
	
	return true;
}

void fini_device()
{
	vkDestroyDevice(device, 0);
}

bool acquire_next_image( void )
{
	VkResult res = vkAcquireNextImageKHR( device, g_output.swapChain, UINT64_MAX, VK_NULL_HANDLE, g_output.acquireFence, &g_output.nSwapChainImageIndex );
	if ( res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR )
		return false;
	if ( vkWaitForFences( device, 1, &g_output.acquireFence, false, UINT64_MAX ) != VK_SUCCESS )
		return false;
	return vkResetFences( device, 1, &g_output.acquireFence ) == VK_SUCCESS;
}

void vulkan_present_to_window( void )
{
	VkPresentInfoKHR presentInfo = {};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	
// 	presentInfo.waitSemaphoreCount = 1;
// 	presentInfo.pWaitSemaphores = signalSemaphores;
	
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = &g_output.swapChain;
	
	presentInfo.pImageIndices = &g_output.nSwapChainImageIndex;
	
	if ( vkQueuePresentKHR( queue, &presentInfo ) != VK_SUCCESS )
		vulkan_remake_swapchain();
	
	while ( !acquire_next_image() )
		vulkan_remake_swapchain();
}

bool vulkan_make_swapchain( VulkanOutput_t *pOutput )
{
	uint32_t imageCount = pOutput->surfaceCaps.minImageCount + 1;
	uint32_t surfaceFormat = 0;
	uint32_t formatCount = pOutput->surfaceFormats.size();
	VkResult result = VK_SUCCESS;

	for ( surfaceFormat = 0; surfaceFormat < formatCount; surfaceFormat++ )
	{
		if ( pOutput->surfaceFormats[ surfaceFormat ].format == VK_FORMAT_B8G8R8A8_UNORM )
			break;
	}
	
	if ( surfaceFormat == formatCount )
		return false;

	pOutput->outputFormat = pOutput->surfaceFormats[ surfaceFormat ].format;
	
	VkSwapchainCreateInfoKHR createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	createInfo.surface = pOutput->surface;
	
	createInfo.minImageCount = imageCount;
	createInfo.imageFormat = pOutput->outputFormat;
	createInfo.imageColorSpace = pOutput->surfaceFormats[surfaceFormat ].colorSpace;
	createInfo.imageExtent = { g_nOutputWidth, g_nOutputHeight };
	createInfo.imageArrayLayers = 1;
	createInfo.imageUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	
	createInfo.preTransform = pOutput->surfaceCaps.currentTransform;
	createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	createInfo.clipped = VK_TRUE;
	
	createInfo.oldSwapchain = VK_NULL_HANDLE;
	
	if (vkCreateSwapchainKHR( device, &createInfo, nullptr, &pOutput->swapChain) != VK_SUCCESS ) {
		return 0;
	}
	
	vkGetSwapchainImagesKHR( device, pOutput->swapChain, &imageCount, nullptr );
	pOutput->swapChainImages.resize( imageCount );
	pOutput->swapChainImageViews.resize( imageCount );
	vkGetSwapchainImagesKHR( device, pOutput->swapChain, &imageCount, pOutput->swapChainImages.data() );
	
	for ( uint32_t i = 0; i < pOutput->swapChainImages.size(); i++ )
	{
		VkImageViewCreateInfo createInfo = {};
		createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		createInfo.image = pOutput->swapChainImages[ i ];
		createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		createInfo.format = pOutput->outputFormat;
		createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
		createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
		createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
		createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
		createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		createInfo.subresourceRange.baseMipLevel = 0;
		createInfo.subresourceRange.levelCount = 1;
		createInfo.subresourceRange.baseArrayLayer = 0;
		createInfo.subresourceRange.layerCount = 1;
		
		result = vkCreateImageView(device, &createInfo, nullptr, &pOutput->swapChainImageViews[ i ]);
		
		if ( result != VK_SUCCESS )
			return false;
	}
	
	VkFenceCreateInfo fenceInfo = {};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

	vkCreateFence( device, &fenceInfo, nullptr, &pOutput->acquireFence );

	return true;
}

bool vulkan_remake_swapchain( void )
{
	VulkanOutput_t *pOutput = &g_output;
	vkQueueWaitIdle( queue );

	for ( uint32_t i = 0; i < pOutput->swapChainImages.size(); i++ )
	{
		vkDestroyImageView( device, pOutput->swapChainImageViews[ i ], nullptr );
		
		pOutput->swapChainImageViews[ i ] = VK_NULL_HANDLE;
		pOutput->swapChainImages[ i ] = VK_NULL_HANDLE;
	}
	
	vkDestroySwapchainKHR( device, pOutput->swapChain, nullptr );
	
	pOutput->nSwapChainImageIndex = 0;

	// Delete screenshot image to be remade if needed
	for (auto& pScreenshotImage : pOutput->pScreenshotImages)
		pScreenshotImage = nullptr;
	
	bool bRet = vulkan_make_swapchain( pOutput );
	assert( bRet ); // Something has gone horribly wrong!
	return bRet;
}

static bool vulkan_make_output_images( VulkanOutput_t *pOutput )
{
	CVulkanTexture::createFlags outputImageflags;
	outputImageflags.bFlippable = true;
	outputImageflags.bTransferSrc = true; // for screenshots

	pOutput->outputImage[0] = nullptr;
	pOutput->outputImage[1] = nullptr;

	pOutput->outputImage[0] = std::make_shared<CVulkanTexture>();
	bool bSuccess = pOutput->outputImage[0]->BInit( g_nOutputWidth, g_nOutputHeight, pOutput->outputFormat, outputImageflags );
	if ( bSuccess != true )
	{
		vk_log.errorf( "failed to allocate buffer for KMS" );
		return false;
	}

	pOutput->outputImage[1] = std::make_shared<CVulkanTexture>();
	bSuccess = pOutput->outputImage[1]->BInit( g_nOutputWidth, g_nOutputHeight, pOutput->outputFormat, outputImageflags );
	if ( bSuccess != true )
	{
		vk_log.errorf( "failed to allocate buffer for KMS" );
		return false;
	}

	return true;
}

bool vulkan_remake_output_images( void )
{
	VulkanOutput_t *pOutput = &g_output;
	vkQueueWaitIdle( queue );

	pOutput->nOutImage = 0;

	// Delete screenshot image to be remade if needed
	for (auto& pScreenshotImage : pOutput->pScreenshotImages)
		pScreenshotImage = nullptr;

	bool bRet = vulkan_make_output_images( pOutput );
	assert( bRet );
	return bRet;
}

bool vulkan_make_output( void )
{
	VulkanOutput_t *pOutput = &g_output;

	VkResult result;
	
	if ( BIsNested() == true )
	{
		if ( !SDL_Vulkan_CreateSurface( g_SDLWindow, instance, &pOutput->surface ) )
		{
			vk_log.errorf( "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError() );
			return false;
		}

		// TODO: check this when selecting the physical device and queue family
		VkBool32 canPresent = false;
		vkGetPhysicalDeviceSurfaceSupportKHR( physicalDevice, queueFamilyIndex, pOutput->surface, &canPresent );
		if ( !canPresent )
		{
			vk_log.errorf( "physical device queue doesn't support presenting on our surface" );
			return false;
		}

		result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR( physicalDevice, pOutput->surface, &pOutput->surfaceCaps );
		if ( result != VK_SUCCESS )
		{
			vk_errorf( result, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed" );
			return false;
		}
		
		uint32_t formatCount = 0;
		result = vkGetPhysicalDeviceSurfaceFormatsKHR( physicalDevice, pOutput->surface, &formatCount, nullptr );
		if ( result != VK_SUCCESS )
		{
			vk_errorf( result, "vkGetPhysicalDeviceSurfaceFormatsKHR failed" );
			return false;
		}
		
		if ( formatCount != 0 ) {
			pOutput->surfaceFormats.resize( formatCount );
			vkGetPhysicalDeviceSurfaceFormatsKHR( physicalDevice, pOutput->surface, &formatCount, pOutput->surfaceFormats.data() );
			if ( result != VK_SUCCESS )
			{
				vk_errorf( result, "vkGetPhysicalDeviceSurfaceFormatsKHR failed" );
				return false;
			}
		}
		
		uint32_t presentModeCount = false;
		result = vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, pOutput->surface, &presentModeCount, nullptr );
		if ( result != VK_SUCCESS )
		{
			vk_errorf( result, "vkGetPhysicalDeviceSurfacePresentModesKHR failed" );
			return false;
		}
		
		if ( presentModeCount != 0 ) {
			pOutput->presentModes.resize(presentModeCount);
			result = vkGetPhysicalDeviceSurfacePresentModesKHR( physicalDevice, pOutput->surface, &presentModeCount, pOutput->presentModes.data() );
			if ( result != VK_SUCCESS )
			{
				vk_errorf( result, "vkGetPhysicalDeviceSurfacePresentModesKHR failed" );
				return false;
			}
		}
		
		if ( !vulkan_make_swapchain( pOutput ) )
			return false;

		while ( !acquire_next_image() )
			vulkan_remake_swapchain();
	}
	else
	{
		pOutput->outputFormat = DRMFormatToVulkan( g_nDRMFormat, false );
		
		if ( pOutput->outputFormat == VK_FORMAT_UNDEFINED )
		{
			vk_log.errorf( "failed to find Vulkan format suitable for KMS" );
			return false;
		}

		if ( !vulkan_make_output_images( pOutput ) )
			return false;
	}

	// Make command buffers
	
	VkCommandBufferAllocateInfo commandBufferAllocateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.pNext = nullptr,
		.commandPool = commandPool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 2
	};
	
	result = vkAllocateCommandBuffers(device, &commandBufferAllocateInfo, pOutput->commandBuffers);
	if ( result != VK_SUCCESS )
	{
		vk_errorf( result, "vkAllocateCommandBuffers failed" );
		return false;
	}
	
	pOutput->nCurCmdBuffer = 0;
	
	return true;
}

bool vulkan_init( void )
{
	VkResult result = VK_ERROR_INITIALIZATION_FAILED;

	std::vector< const char * > vecEnabledInstanceExtensions;
	if ( BIsNested() )
	{
		if ( SDL_Vulkan_LoadLibrary( nullptr ) != 0 )
		{
			fprintf(stderr, "SDL_Vulkan_LoadLibrary failed: %s\n", SDL_GetError());
			return false;
		}

		unsigned int extCount = 0;
		SDL_Vulkan_GetInstanceExtensions( nullptr, &extCount, nullptr );
		vecEnabledInstanceExtensions.resize( extCount );
		SDL_Vulkan_GetInstanceExtensions( nullptr, &extCount, vecEnabledInstanceExtensions.data() );
	}

	const VkInstanceCreateInfo createInfo = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &appInfo,
		.enabledExtensionCount = (uint32_t)vecEnabledInstanceExtensions.size(),
		.ppEnabledExtensionNames = vecEnabledInstanceExtensions.data(),
	};

	result = vkCreateInstance(&createInfo, 0, &instance);
	if ( result != VK_SUCCESS )
	{
		vk_errorf( result, "vkCreateInstance failed" );
		return false;
	}
	
	if ( !init_device() )
		return false;
	
	dyn_vkGetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr( device, "vkGetMemoryFdKHR" );
	if ( dyn_vkGetMemoryFdKHR == nullptr )
		return false;

	dyn_vkGetImageDrmFormatModifierPropertiesEXT = (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr( device, "vkGetImageDrmFormatModifierPropertiesEXT" );
	
	return true;
}

static inline uint32_t get_command_buffer( VkCommandBuffer &cmdBuf, VkFence *pFence )
{
	for ( uint32_t i = 0; i < k_nScratchCmdBufferCount; i++ )
	{
		if ( g_scratchCommandBuffers[ i ].busy == false )
		{
			cmdBuf = g_scratchCommandBuffers[ i ].cmdBuf;
			if ( pFence != nullptr )
			{
				*pFence = g_scratchCommandBuffers[ i ].fence;
			}
			
			VkCommandBufferBeginInfo commandBufferBeginInfo = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
			};
			
			VkResult res = vkBeginCommandBuffer( cmdBuf, &commandBufferBeginInfo);
			
			if ( res != VK_SUCCESS )
			{
				break;
			}
			
			g_scratchCommandBuffers[ i ].refs.clear();
			
			g_scratchCommandBuffers[ i ].haswaiter = pFence != nullptr;
			
			g_scratchCommandBuffers[ i ].busy = true;
			
			return i;
		}
	}
	
	assert( 0 );
	return 0;
}

static inline void submit_command_buffer( uint32_t handle, std::vector<std::shared_ptr<CVulkanTexture>> &vecRefs )
{
	VkCommandBuffer cmdBuf = g_scratchCommandBuffers[ handle ].cmdBuf;
	VkFence fence = g_scratchCommandBuffers[ handle ].fence;
	
	VkResult res = vkEndCommandBuffer( cmdBuf );
	
	if ( res != VK_SUCCESS )
	{
		assert( 0 );
	}
	
	VkSubmitInfo submitInfo = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		. commandBufferCount = 1,
		.pCommandBuffers = &cmdBuf,
	};
	
	res = vkQueueSubmit( queue, 1, &submitInfo, fence );
	
	if ( res != VK_SUCCESS )
	{
		assert( 0 );
	}
	
	for( uint32_t i = 0; i < vecRefs.size(); i++ )
		g_scratchCommandBuffers[ handle ].refs.push_back( std::move(vecRefs[ i ]) );
}

void vulkan_garbage_collect( void )
{
	// If we ever made anything calling get_command_buffer() multi-threaded we'd have to rethink this
	// Probably just differentiate "busy" and "submitted"
	for ( uint32_t i = 0; i < k_nScratchCmdBufferCount; i++ )
	{
		if ( g_scratchCommandBuffers[ i ].busy == true &&
			g_scratchCommandBuffers[ i ].haswaiter == false )
		{
			VkResult res = vkGetFenceStatus( device, g_scratchCommandBuffers[ i ].fence );
			
			if ( res == VK_SUCCESS )
			{
				vkResetCommandBuffer( g_scratchCommandBuffers[ i ].cmdBuf, 0 );
				vkResetFences( device, 1, &g_scratchCommandBuffers[ i ].fence );
				
				g_scratchCommandBuffers[ i ].refs.clear();
				g_scratchCommandBuffers[ i ].busy = false;
			}
		}
	}
}

std::shared_ptr<CVulkanTexture> vulkan_create_texture_from_dmabuf( struct wlr_dmabuf_attributes *pDMA )
{
	std::shared_ptr<CVulkanTexture> pTex = std::make_shared<CVulkanTexture>();

	CVulkanTexture::createFlags texCreateFlags;
	texCreateFlags.bTextureable = true;
	
	if ( pTex->BInit( pDMA->width, pDMA->height, DRMFormatToVulkan( pDMA->format, true ), texCreateFlags, pDMA ) == false )
		return nullptr;
	
	return pTex;
}

std::shared_ptr<CVulkanTexture> vulkan_create_texture_from_bits( uint32_t width, uint32_t height, VkFormat format, CVulkanTexture::createFlags texCreateFlags, void *bits )
{
	std::shared_ptr<CVulkanTexture> pTex = std::make_shared<CVulkanTexture>();

	texCreateFlags.bTextureable = true;
	texCreateFlags.bTransferDst = true;

	if ( pTex->BInit( width, height, format, texCreateFlags ) == false )
		return nullptr;
	
	memcpy( pUploadBuffer, bits, width * height * 4 );
	
	VkCommandBuffer commandBuffer;
	uint32_t handle = get_command_buffer( commandBuffer, nullptr );
	
	VkBufferImageCopy region = {};
	
	region.imageSubresource = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.layerCount = 1
	};
	
	region.imageExtent = {
		.width = width,
		.height = height,
		.depth = 1
	};
	
	vkCmdCopyBufferToImage( commandBuffer, uploadBuffer, pTex->m_vkImage, VK_IMAGE_LAYOUT_GENERAL, 1, &region );
	
	std::vector<std::shared_ptr<CVulkanTexture>> refs;
	refs.push_back( pTex );
	
	submit_command_buffer( handle, refs );
	
	return pTex;
}

VkSampler vulkan_make_sampler( VulkanSamplerCacheKey_t key )
{
	if ( g_vulkanSamplerCache.count(key) != 0 )
		return g_vulkanSamplerCache[key];

	VkSampler ret = VK_NULL_HANDLE;

	VkSamplerCreateInfo samplerCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.pNext = nullptr,
		.magFilter = key.bNearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
		.minFilter = key.bNearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
		.unnormalizedCoordinates = key.bUnnormalized,
	};

	vkCreateSampler( device, &samplerCreateInfo, nullptr, &ret );

	g_vulkanSamplerCache[key] = ret;

	return ret;
}

bool float_is_integer(float x)
{
	return fabsf(ceilf(x) - x) <= 0.0001f;
}

static uint32_t s_frameId = 0;

void vulkan_update_descriptor( struct Composite_t *pComposite, struct VulkanPipeline_t *pPipeline, int nYCBCRMask )
{
	{
		VkImageView targetImageView;
		
		if ( BIsNested() == true )
		{
			targetImageView = g_output.swapChainImageViews[ g_output.nSwapChainImageIndex ];
		}
		else
		{
			targetImageView = g_output.outputImage[ g_output.nOutImage ]->m_vkImageView;
		}

		VkDescriptorImageInfo imageInfo = {
			.imageView = targetImageView,
			.imageLayout = VK_IMAGE_LAYOUT_GENERAL
		};
		
		VkWriteDescriptorSet writeDescriptorSet = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.pNext = nullptr,
			.dstSet = descriptorSet,
			.dstBinding = 0,
			.dstArrayElement = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			.pImageInfo = &imageInfo,
			.pBufferInfo = nullptr,
			.pTexelBufferView = nullptr,
		};
		
		vkUpdateDescriptorSets(device, 1, &writeDescriptorSet, 0, nullptr);
	}
	
	std::array< VkDescriptorImageInfo, k_nMaxLayers > imageDescriptors = {};
	for ( uint32_t i = 0; i < k_nMaxLayers; i++ )
	{
		bool bForceNearest = pComposite->data.vScale[i].x == 1.0f &&
							 pComposite->data.vScale[i].y == 1.0f &&
							 float_is_integer(pComposite->data.vOffset[i].x);
							 float_is_integer(pComposite->data.vOffset[i].y);

		VkImageView imageView = pPipeline->layerBindings[ i ].tex
			? pPipeline->layerBindings[ i ].tex->m_vkImageView
			: VK_NULL_HANDLE;

		VulkanSamplerCacheKey_t samplerKey;
		samplerKey.bNearest = bForceNearest || !pPipeline->layerBindings[i].bFilter;
		samplerKey.bUnnormalized = true;

		VkSampler sampler = vulkan_make_sampler(samplerKey);
		assert ( sampler != VK_NULL_HANDLE );

		{
			VkDescriptorImageInfo imageInfo = {
				.sampler = sampler,
				.imageView = imageView,
				.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
			};

			imageDescriptors[ i ] = imageInfo;
		}
	}

	// Duplicate image descriptors for ycbcr.
	std::array< VkDescriptorImageInfo, k_nMaxLayers > ycbcrImageDescriptors = {};

	for (uint32_t i = 0; i < k_nMaxLayers; i++)
	{
		if ( nYCBCRMask & ( 1u << i ) )
		{
			ycbcrImageDescriptors[i] = imageDescriptors[i];
			// We use immutable samplers.
			ycbcrImageDescriptors[i].sampler = VK_NULL_HANDLE;
			// The ycbcr image might not be usable as a rgb image
			// and the shader doesn't use it anyway.
			imageDescriptors[i].imageView =  VK_NULL_HANDLE;
		}
	}

	std::array< VkWriteDescriptorSet, 2 > writeDescriptorSets;

	writeDescriptorSets[0] = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.pNext = nullptr,
		.dstSet = descriptorSet,
		.dstBinding = 1,
		.dstArrayElement = 0,
		.descriptorCount = imageDescriptors.size(),
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = imageDescriptors.data(),
		.pBufferInfo = nullptr,
		.pTexelBufferView = nullptr,
	};

	writeDescriptorSets[1] = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.pNext = nullptr,
		.dstSet = descriptorSet,
		.dstBinding = 2,
		.dstArrayElement = 0,
		.descriptorCount = ycbcrImageDescriptors.size(),
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = ycbcrImageDescriptors.data(),
		.pBufferInfo = nullptr,
		.pTexelBufferView = nullptr,
	};

	vkUpdateDescriptorSets(device, writeDescriptorSets.size(), writeDescriptorSets.data(), 0, nullptr);
}

bool vulkan_composite( struct Composite_t *pComposite, struct VulkanPipeline_t *pPipeline, std::shared_ptr<CVulkanTexture> *pScreenshotTexture )
{
	VkImage compositeImage;

	if ( BIsNested() == true )
	{
		compositeImage = g_output.swapChainImages[ g_output.nSwapChainImageIndex ];
	}
	else
	{
		compositeImage = g_output.outputImage[ g_output.nOutImage ]->m_vkImage;

		if ( DRMFormatNeedsSwizzle( g_nDRMFormat ) )
		{
			pComposite->nSwapChannels = 1;
		}
	}
	
	pComposite->nYCBCRMask = 0;
	for (uint32_t i = 0; i < k_nMaxLayers; i++)
	{
		if ( pPipeline->layerBindings[ i ].tex != 0 )
		{
			const auto& pTex = pPipeline->layerBindings[ i ].tex;
			if (pTex->m_format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
				pComposite->nYCBCRMask |= 1 << i;
		}
	}

	vulkan_update_descriptor( pComposite, pPipeline, pComposite->nYCBCRMask );

	for ( int i = 0; i < pComposite->nLayerCount; i++ )
	{
		pComposite->data.vOffset[ i ].x += pComposite->data.vScale[ i ].x / 2.0f;
		pComposite->data.vOffset[ i ].y += pComposite->data.vScale[ i ].y / 2.0f;
	}
	
	VkCommandBuffer curCommandBuffer = g_output.commandBuffers[ g_output.nCurCmdBuffer ];
	
	VkCommandBufferBeginInfo commandBufferBeginInfo = {
		VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		0,
		VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		0
	};
	
	VkResult res = vkResetCommandBuffer( curCommandBuffer, 0 );
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}
	
	res = vkBeginCommandBuffer( curCommandBuffer, &commandBufferBeginInfo);
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}
	
	VkImageSubresourceRange subResRange =
	{
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.levelCount = 1,
		.layerCount = 1
	};
	
	/* We don't need a queue ownership transfer even with the foreign path
	   because we don't care about the content of the image.
	   The spec says:
	   "If memory dependencies are correctly expressed between uses of such
	    a resource between two queues in different families, but no ownership
	    transfer is defined, the contents of that resource are undefined for
	    any read accesses performed by the second queue family." */
	VkImageMemoryBarrier memoryBarrier =
	{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = compositeImage,
		.subresourceRange = subResRange
	};
	
	vkCmdPipelineBarrier( curCommandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			      		0, 0, nullptr, 0, nullptr, 1, &memoryBarrier );
	
	vkCmdBindPipeline(curCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[pComposite->nLayerCount - 1][pComposite->nSwapChannels][pComposite->nYCBCRMask]);
	
	vkCmdBindDescriptorSets(curCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
							pipelineLayout, 0, 1, &descriptorSet, 0, 0);

	vkCmdPushConstants(curCommandBuffer, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pComposite->data), &pComposite->data);
	if (g_bIsCompositeDebug) {
		vkCmdPushConstants(curCommandBuffer, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof(pComposite->data), sizeof(uint32_t), &s_frameId);
		s_frameId++;
	}
	
	uint32_t nGroupCountX = currentOutputWidth % 8 ? currentOutputWidth / 8 + 1: currentOutputWidth / 8;
	uint32_t nGroupCountY = currentOutputHeight % 8 ? currentOutputHeight / 8 + 1: currentOutputHeight / 8;
	
	vkCmdDispatch( curCommandBuffer, nGroupCountX, nGroupCountY, 1 );
	
	if ( pScreenshotTexture != nullptr )
	{
		std::shared_ptr<CVulkanTexture> pFoundScreenshotImage = nullptr;

		for (auto& pScreenshotImage : g_output.pScreenshotImages)
		{
			if (pScreenshotImage == nullptr)
			{
				pScreenshotImage = std::make_shared<CVulkanTexture>();

				CVulkanTexture::createFlags screenshotImageFlags;
				screenshotImageFlags.bMappable = true;
				screenshotImageFlags.bTransferDst = true;

				bool bSuccess = pScreenshotImage->BInit( currentOutputWidth, currentOutputHeight, g_output.outputFormat, screenshotImageFlags );

				assert( bSuccess );

				// Transition it to GENERAL
				VkImageSubresourceRange subResRange =
				{
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.levelCount = 1,
					.layerCount = 1
				};

				VkImageMemoryBarrier memoryBarrier =
				{
					.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					.srcAccessMask = 0,
					.dstAccessMask = 0,
					.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
					.newLayout = VK_IMAGE_LAYOUT_GENERAL,
					.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.image = pScreenshotImage->m_vkImage,
					.subresourceRange = subResRange
				};

				vkCmdPipelineBarrier( curCommandBuffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
									0, 0, nullptr, 0, nullptr, 1, &memoryBarrier );
			}

			if (pScreenshotImage.use_count() > 1)
				continue;

			pFoundScreenshotImage = pScreenshotImage;
			break;
		}

		VkImageMemoryBarrier memoryBarrier =
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = compositeImage,
			.subresourceRange = subResRange
		};
	
		vkCmdPipelineBarrier( curCommandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
				      0, 0, nullptr, 0, nullptr, 1, &memoryBarrier );

		VkImageCopy region = {};

		region.srcSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.layerCount = 1
		};

		region.dstSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.layerCount = 1
		};

		region.extent = {
			.width = currentOutputWidth,
			.height = currentOutputHeight,
			.depth = 1
		};

		vkCmdCopyImage( curCommandBuffer, compositeImage, VK_IMAGE_LAYOUT_GENERAL, pFoundScreenshotImage->m_vkImage, VK_IMAGE_LAYOUT_GENERAL, 1, &region );

		*pScreenshotTexture = pFoundScreenshotImage;
	}

	bool useForeignQueue = !BIsNested() && g_vulkanSupportsModifiers;

	memoryBarrier =
	{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = pScreenshotTexture ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = useForeignQueue ? (VkAccessFlagBits)0 : VK_ACCESS_MEMORY_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = BIsNested() ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = queueFamilyIndex,
		.dstQueueFamilyIndex = useForeignQueue
								? VK_QUEUE_FAMILY_FOREIGN_EXT
						    	: queueFamilyIndex,
		.image = compositeImage,
		.subresourceRange = subResRange
	};
	
	vkCmdPipelineBarrier( curCommandBuffer, pScreenshotTexture ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			      useForeignQueue ? 0 : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			      0, 0, nullptr, 0, nullptr, 1, &memoryBarrier );
	
	res = vkEndCommandBuffer( curCommandBuffer );
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}
	
	VkSubmitInfo submitInfo = {
		VK_STRUCTURE_TYPE_SUBMIT_INFO,
		0,
		0,
		0,
		0,
		1,
		&curCommandBuffer,
		0,
		0
	};
	
	res = vkQueueSubmit( queue, 1, &submitInfo, 0 );
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}
	
	vkQueueWaitIdle( queue );

	if ( BIsNested() == false )
	{
		g_output.nOutImage = !g_output.nOutImage;
	}
	
	g_output.nCurCmdBuffer = !g_output.nCurCmdBuffer;
	
	return true;
}

uint32_t vulkan_get_last_composite_fbid( void )
{
	return g_output.outputImage[ !g_output.nOutImage ]->m_FBID;
}

uint32_t vulkan_texture_get_fbid( const std::shared_ptr<CVulkanTexture>& vulkanTex )
{
	if ( !vulkanTex )
		return 0;

	return vulkanTex->m_FBID;
}

int vulkan_texture_get_fence( const std::shared_ptr<CVulkanTexture>& vulkanTex )
{
	if ( !vulkanTex )
		return -1;

	const VkMemoryGetFdInfoKHR memory_get_fd_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
		.pNext = NULL,
		.memory = vulkanTex->m_vkImageMemory,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	int fence = -1;
	VkResult res = dyn_vkGetMemoryFdKHR(device, &memory_get_fd_info, &fence);
	if ( res != VK_SUCCESS ) {
		fprintf( stderr, "vkGetMemoryFdKHR failed\n" );
	}

	return fence;
}

static void texture_destroy( struct wlr_texture *wlr_texture )
{
	VulkanWlrTexture_t *tex = (VulkanWlrTexture_t *)wlr_texture;
	wlr_buffer_unlock( tex->buf );
	delete tex;
}

static const struct wlr_texture_impl texture_impl = {
	.destroy = texture_destroy,
};

static uint32_t renderer_get_render_buffer_caps( struct wlr_renderer *renderer )
{
	return 0;
}

static void renderer_begin( struct wlr_renderer *renderer, uint32_t width, uint32_t height )
{
	abort(); // unreachable
}

static void renderer_end( struct wlr_renderer *renderer )
{
	abort(); // unreachable
}

static void renderer_clear( struct wlr_renderer *renderer, const float color[4] )
{
	abort(); // unreachable
}

static void renderer_scissor( struct wlr_renderer *renderer, struct wlr_box *box )
{
	abort(); // unreachable
}

static bool renderer_render_subtexture_with_matrix( struct wlr_renderer *renderer, struct wlr_texture *texture, const struct wlr_fbox *box, const float matrix[9], float alpha )
{
	abort(); // unreachable
}

static void renderer_render_quad_with_matrix( struct wlr_renderer *renderer, const float color[4], const float matrix[9] )
{
	abort(); // unreachable
}

static const uint32_t *renderer_get_shm_texture_formats( struct wlr_renderer *wlr_renderer, size_t *len
 )
{
	*len = sampledShmFormats.size();
	return sampledShmFormats.data();
}

static const struct wlr_drm_format_set *renderer_get_dmabuf_texture_formats( struct wlr_renderer *wlr_renderer )
{
	return &sampledDRMFormats;
}

static int renderer_get_drm_fd( struct wlr_renderer *wlr_renderer )
{
	return g_drmRenderFd;
}

static struct wlr_texture *renderer_texture_from_buffer( struct wlr_renderer *wlr_renderer, struct wlr_buffer *buf )
{
	VulkanWlrTexture_t *tex = new VulkanWlrTexture_t();
	wlr_texture_init( &tex->base, &texture_impl, buf->width, buf->height );
	tex->buf = wlr_buffer_lock( buf );
	// TODO: check format/modifier
	// TODO: if DMA-BUF, try importing it into Vulkan
	return &tex->base;
}

static const struct wlr_renderer_impl renderer_impl = {
	.begin = renderer_begin,
	.end = renderer_end,
	.clear = renderer_clear,
	.scissor = renderer_scissor,
	.render_subtexture_with_matrix = renderer_render_subtexture_with_matrix,
	.render_quad_with_matrix = renderer_render_quad_with_matrix,
	.get_shm_texture_formats = renderer_get_shm_texture_formats,
	.get_dmabuf_texture_formats = renderer_get_dmabuf_texture_formats,
	.get_drm_fd = renderer_get_drm_fd,
	.get_render_buffer_caps = renderer_get_render_buffer_caps,
	.texture_from_buffer = renderer_texture_from_buffer,
};

struct wlr_renderer *vulkan_renderer_create( void )
{
	VulkanRenderer_t *renderer = new VulkanRenderer_t();
	wlr_renderer_init(&renderer->base, &renderer_impl);
	return &renderer->base;
}

std::shared_ptr<CVulkanTexture> vulkan_create_texture_from_wlr_buffer( struct wlr_buffer *buf )
{

	struct wlr_dmabuf_attributes dmabuf = {0};
	if ( wlr_buffer_get_dmabuf( buf, &dmabuf ) )
	{
		return vulkan_create_texture_from_dmabuf( &dmabuf );
	}

	VkResult result;

	void *src;
	uint32_t drmFormat;
	size_t stride;
	if ( wlr_buffer_begin_data_ptr_access( buf, WLR_BUFFER_DATA_PTR_ACCESS_READ, &src, &drmFormat, &stride ) )
	{
		return 0;
	}

	VkFormat format = DRMFormatToVulkan( drmFormat, true );
	uint32_t width = buf->width;
	uint32_t height = buf->height;

	VkBufferCreateInfo bufferCreateInfo = {};
	bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferCreateInfo.size = stride * height;
	bufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VkBuffer buffer;
	result = vkCreateBuffer( device, &bufferCreateInfo, nullptr, &buffer );
	if ( result != VK_SUCCESS )
	{
		wlr_buffer_end_data_ptr_access( buf );
		return 0;
	}

	VkMemoryRequirements memRequirements;
	vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

	int memTypeIndex =  findMemoryType(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, memRequirements.memoryTypeBits );
	if ( memTypeIndex == -1 )
	{
		wlr_buffer_end_data_ptr_access( buf );
		return 0;
	}

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = memTypeIndex;

	VkDeviceMemory bufferMemory;
	result = vkAllocateMemory( device, &allocInfo, nullptr, &bufferMemory);
	if ( result != VK_SUCCESS )
	{
		wlr_buffer_end_data_ptr_access( buf );
		return 0;
	}

	result = vkBindBufferMemory( device, buffer, bufferMemory, 0 );
	if ( result != VK_SUCCESS )
	{
		wlr_buffer_end_data_ptr_access( buf );
		return 0;
	}

	void *dst;
	result = vkMapMemory( device, bufferMemory, 0, VK_WHOLE_SIZE, 0, &dst );
	if ( result != VK_SUCCESS )
	{
		wlr_buffer_end_data_ptr_access( buf );
		return 0;
	}

	memcpy( dst, src, stride * height );

	vkUnmapMemory( device, bufferMemory );

	wlr_buffer_end_data_ptr_access( buf );

	std::shared_ptr<CVulkanTexture> pTex = std::make_shared<CVulkanTexture>();
	CVulkanTexture::createFlags texCreateFlags = {};
	texCreateFlags.bTextureable = true;
	texCreateFlags.bTransferDst = true;
	if ( pTex->BInit( width, height, format, texCreateFlags ) == false )
		return nullptr;

	VkCommandBuffer commandBuffer;
	uint32_t handle = get_command_buffer( commandBuffer, nullptr );

	VkBufferImageCopy region = {};
	region.imageSubresource = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.layerCount = 1
	};
	region.imageExtent = {
		.width = width,
		.height = height,
		.depth = 1
	};
	vkCmdCopyBufferToImage( commandBuffer, buffer, pTex->m_vkImage, VK_IMAGE_LAYOUT_GENERAL, 1, &region );

	std::vector<std::shared_ptr<CVulkanTexture>> refs;
	refs.push_back( pTex );

	submit_command_buffer( handle, refs );

	return pTex;
}
