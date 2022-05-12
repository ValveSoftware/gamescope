// Initialize Vulkan and composite stuff with a compute queue

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <algorithm>
#include <array>
#include <thread>

// Used to remove the config struct alignment specified by the NIS header
#define NIS_ALIGNED(x)
// NIS_Config needs to be included before the X11 headers because of conflicting defines introduced by X11
#include "shaders/NVIDIAImageScaling/NIS/NIS_Config.h"

#include "rendervulkan.hpp"
#include "main.hpp"
#include "steamcompmgr.hpp"
#include "sdlwindow.hpp"
#include "log.hpp"

#include "cs_composite_blit.h"
#include "cs_composite_blur.h"
#include "cs_composite_blur_cond.h"
#include "cs_composite_rcas.h"
#include "cs_easu.h"
#include "cs_easu_fp16.h"
#include "cs_gaussian_blur_horizontal.h"
#include "cs_nis.h"
#include "cs_nis_fp16.h"


#define A_CPU
#include "shaders/ffx_a.h"
#include "shaders/ffx_fsr1.h"

bool g_bIsCompositeDebug = false;

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

	// NIS and FSR
	std::shared_ptr<CVulkanTexture> tmpOutput;

	// NIS
	VkSampler nisSampler;
	VkImage nisScalerImage;
	VkImageView nisScalerView;
	VkImage nisUsmImage;
	VkImageView nisUsmView;
	VkDeviceMemory nisMemory;
};


enum ShaderType {
	SHADER_TYPE_BLIT = 0,
	SHADER_TYPE_BLUR,
	SHADER_TYPE_BLUR_COND,
	SHADER_TYPE_BLUR_FIRST_PASS,
	SHADER_TYPE_EASU,
	SHADER_TYPE_RCAS,
	SHADER_TYPE_NIS,

	SHADER_TYPE_COUNT
};

const uint32_t k_nScratchCmdBufferCount = 1000;

VulkanOutput_t g_output;

// Prototype to use init_nis_data in vulkan_init
static bool init_nis_data();

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

// Needed for use in init_nis_data
VkSampler vulkan_make_sampler( VulkanSamplerCacheKey_t key );

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

struct PipelineInfo_t
{
	ShaderType shaderType;

	uint32_t layerCount;
	uint32_t ycbcrMask;
	uint32_t blurRadius;
	uint32_t blurLayerCount;

	bool operator==(const PipelineInfo_t& o) const {
		return shaderType == o.shaderType && layerCount == o.layerCount && ycbcrMask == o.ycbcrMask && blurRadius == o.blurRadius && blurLayerCount == o.blurLayerCount;
	}
};


static inline uint32_t hash_combine(uint32_t old_hash, uint32_t new_hash) {
    return old_hash ^ (new_hash + 0x9e3779b9 + (old_hash << 6) + (old_hash >> 2));
}

namespace std
{
	template <>
	struct hash<PipelineInfo_t>
	{
		size_t operator()( const PipelineInfo_t& k ) const
		{
			uint32_t hash = k.shaderType;
			hash = hash_combine(hash, k.layerCount);
			hash = hash_combine(hash, k.ycbcrMask);
			hash = hash_combine(hash, k.blurRadius);
			hash = hash_combine(hash, k.blurLayerCount);
			return hash;
		}
	};
}

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

template<typename Target, typename Base>
Target *pNextFind(const Base *base, VkStructureType sType)
{
	for ( ; base; base = (const Base *)base->pNext )
	{
		if (base->sType == sType)
			return (Target *) base;
	}
	return nullptr;
}

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
	bool bHasAlpha;
} s_DRMVKFormatTable[] = {
	{ DRM_FORMAT_ARGB8888, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, true },
	{ DRM_FORMAT_XRGB8888, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, false },
	{ DRM_FORMAT_NV12, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, false },
	{ DRM_FORMAT_INVALID, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, false },
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

struct scratchCmdBuffer_t
{
	VkCommandBuffer cmdBuf;
	
	std::vector<std::shared_ptr<CVulkanTexture>> refs;
	
	std::atomic<uint64_t> seqNo = { 0 };
};

#define VULKAN_INSTANCE_FUNCTIONS \
	VK_FUNC(CreateDevice) \
	VK_FUNC(EnumeratePhysicalDevices) \
	VK_FUNC(GetDeviceProcAddr) \
	VK_FUNC(GetPhysicalDeviceFeatures2) \
	VK_FUNC(GetPhysicalDeviceFormatProperties) \
	VK_FUNC(GetPhysicalDeviceMemoryProperties) \
	VK_FUNC(GetPhysicalDeviceQueueFamilyProperties) \
	VK_FUNC(GetPhysicalDeviceProperties) \
	VK_FUNC(GetPhysicalDeviceProperties2)

#define VULKAN_DEVICE_FUNCTIONS \
	VK_FUNC(AllocateCommandBuffers) \
	VK_FUNC(AllocateDescriptorSets) \
	VK_FUNC(AllocateMemory) \
	VK_FUNC(BeginCommandBuffer) \
	VK_FUNC(BindBufferMemory) \
	VK_FUNC(CreateBuffer) \
	VK_FUNC(CreateCommandPool) \
	VK_FUNC(CreateComputePipelines) \
	VK_FUNC(CreateDescriptorPool) \
	VK_FUNC(CreateDescriptorSetLayout) \
	VK_FUNC(CreatePipelineLayout) \
	VK_FUNC(CreateSampler) \
	VK_FUNC(CreateSamplerYcbcrConversion) \
	VK_FUNC(CreateSemaphore) \
	VK_FUNC(CreateShaderModule) \
	VK_FUNC(CmdDispatch) \
	VK_FUNC(DestroyPipeline) \
	VK_FUNC(EndCommandBuffer) \
	VK_FUNC(GetBufferMemoryRequirements) \
	VK_FUNC(GetDeviceQueue) \
	VK_FUNC(GetImageDrmFormatModifierPropertiesEXT) \
	VK_FUNC(GetMemoryFdKHR) \
	VK_FUNC(GetSemaphoreCounterValue) \
	VK_FUNC(MapMemory) \
	VK_FUNC(QueueSubmit) \
	VK_FUNC(ResetCommandBuffer) \

class CVulkanDevice
{
public:
	bool BInit();

	VkSampler sampler(VulkanSamplerCacheKey_t key);
	VkPipeline pipeline(ShaderType type, uint32_t layerCount = 1, uint32_t ycbcrMask = 0, uint32_t radius = 0, uint32_t blur_layers = 0);
	int32_t findMemoryType( VkMemoryPropertyFlags properties, uint32_t requiredTypeBits );
	uint32_t commandBuffer( VkCommandBuffer &cmdBuf );
	void submitCommandBuffer( uint32_t handle, std::vector<std::shared_ptr<CVulkanTexture>> &vecRefs );
	void garbageCollect();
	inline VkDescriptorSet descriptorSet()
	{
		VkDescriptorSet ret = m_descriptorSets[m_currentDescriptorSet];
		m_currentDescriptorSet = (m_currentDescriptorSet + 1) % m_descriptorSets.size();
		return ret;
	}

	inline VkDevice device() { return m_device; }
	inline VkPhysicalDevice physDev() {return m_physDev; }
	inline VkInstance instance() { return m_instance; }
	inline VkQueue queue() {return m_queue;}
	inline VkCommandPool commandPool() {return m_commandPool;}
	inline uint32_t queueFamily() {return m_queueFamily;}
	inline VkBuffer uploadBuffer() {return m_uploadBuffer;}
	inline VkPipelineLayout pipelineLayout() {return m_pipelineLayout;}
	inline void *uploadBufferData() {return m_uploadBufferData;}
	inline int drmRenderFd() {return m_drmRendererFd;}
	inline bool supportsModifiers() {return m_bSupportsModifiers;}
	inline bool hasDrmPrimaryDevId() {return m_bHasDrmPrimaryDevId;}
	inline dev_t primaryDevId() {return m_drmPrimaryDevId;}
	inline bool supportsFp16() {return m_bSupportsFp16;}

	#define VK_FUNC(x) PFN_vk##x x = nullptr;
	struct
	{
		VULKAN_INSTANCE_FUNCTIONS
		VULKAN_DEVICE_FUNCTIONS
	} vk;
	#undef VK_FUNC


private:
	bool createInstance();
	bool selectPhysDev();
	bool createDevice();
	bool createLayouts();
	bool createPools();
	bool createShaders();
	bool createScratchResources();
	VkPipeline compilePipeline(uint32_t layerCount, uint32_t ycbcrMask, uint32_t radius, ShaderType type, uint32_t blur_layer_count);
	void compileAllPipelines();

	VkDevice m_device = nullptr;
	VkPhysicalDevice m_physDev = nullptr;
	VkInstance m_instance = nullptr;
	VkQueue m_queue = nullptr;
	VkSamplerYcbcrConversion m_ycbcrConversion = VK_NULL_HANDLE;
	VkSampler m_ycbcrSampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
	VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
	VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
	VkCommandPool m_commandPool = VK_NULL_HANDLE;

	uint32_t m_queueFamily = -1;

	int m_drmRendererFd = -1;
	dev_t m_drmPrimaryDevId = 0;

	bool m_bSupportsFp16 = false;
	bool m_bHasDrmPrimaryDevId = false;
	bool m_bSupportsModifiers = false;
	bool m_bInitialized = false;


	VkPhysicalDeviceMemoryProperties m_memoryProperties;

	std::unordered_map< VulkanSamplerCacheKey_t, VkSampler > m_samplerCache;
	std::array<VkShaderModule, SHADER_TYPE_COUNT> m_shaderModules;
	std::unordered_map<PipelineInfo_t, VkPipeline> m_pipelineMap;
	std::mutex m_pipelineMutex;

	// currently just one set, no need to double buffer because we
	// vkQueueWaitIdle after each submit.
	// should be moved to the output if we are going to support multiple outputs
	std::array<VkDescriptorSet, 3> m_descriptorSets;
	uint32_t m_currentDescriptorSet = 0;

	VkBuffer m_uploadBuffer;
	VkDeviceMemory m_uploadBufferMemory;
	void *m_uploadBufferData;


	VkSemaphore m_scratchTimelineSemaphore;
	std::atomic<uint64_t> m_submissionSeqNo = { 0 };
	scratchCmdBuffer_t m_scratchCommandBuffers[ k_nScratchCmdBufferCount ];
};

bool CVulkanDevice::BInit()
{
	assert(!m_bInitialized);

	if (!createInstance())
		return false;
	if (!selectPhysDev())
		return false;
	if (!createDevice())
		return false;
	if (!createLayouts())
		return false;
	if (!createPools())
		return false;
	if (!createShaders())
		return false;
	if (!createScratchResources())
		return false;

	m_bInitialized = true;

	std::thread piplelineThread([this](){compileAllPipelines();});
	piplelineThread.detach();

	return true;
}

bool CVulkanDevice::createInstance()
{
	VkResult result = VK_ERROR_INITIALIZATION_FAILED;

	std::vector< const char * > sdlExtensions;
	if ( BIsNested() )
	{
		if ( SDL_Vulkan_LoadLibrary( nullptr ) != 0 )
		{
			fprintf(stderr, "SDL_Vulkan_LoadLibrary failed: %s\n", SDL_GetError());
			return false;
		}

		unsigned int extCount = 0;
		SDL_Vulkan_GetInstanceExtensions( nullptr, &extCount, nullptr );
		sdlExtensions.resize( extCount );
		SDL_Vulkan_GetInstanceExtensions( nullptr, &extCount, sdlExtensions.data() );
	}

	const VkApplicationInfo appInfo = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "gamescope",
		.applicationVersion = VK_MAKE_VERSION(1, 0, 0),
		.pEngineName = "hopefully not just some code",
		.engineVersion = VK_MAKE_VERSION(1, 0, 0),
		.apiVersion = VK_API_VERSION_1_2,
	};

	const VkInstanceCreateInfo createInfo = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &appInfo,
		.enabledExtensionCount = (uint32_t)sdlExtensions.size(),
		.ppEnabledExtensionNames = sdlExtensions.data(),
	};

	result = vkCreateInstance(&createInfo, 0, &m_instance);
	if ( result != VK_SUCCESS )
	{
		vk_errorf( result, "vkCreateInstance failed" );
		return false;
	}

	#define VK_FUNC(x) vk.x = (PFN_vk##x) vkGetInstanceProcAddr(instance(), "vk"#x);
	VULKAN_INSTANCE_FUNCTIONS
	#undef VK_FUNC

	return true;
}

bool CVulkanDevice::selectPhysDev()
{
	uint32_t deviceCount = 0;
	vk.EnumeratePhysicalDevices(instance(), &deviceCount, nullptr);
	std::vector<VkPhysicalDevice> physDevs(deviceCount);
	vk.EnumeratePhysicalDevices(instance(), &deviceCount, physDevs.data());
	if (deviceCount < physDevs.size())
		physDevs.resize(deviceCount);

	bool bTryComputeOnly = true;

	// In theory vkBasalt might want to filter out compute-only queue families to force our hand here
	const char *pchEnableVkBasalt = getenv( "ENABLE_VKBASALT" );
	if ( pchEnableVkBasalt != nullptr && pchEnableVkBasalt[0] == '1' )
	{
		bTryComputeOnly = false;
	}

	for (auto cphysDev : physDevs)
	{
		VkPhysicalDeviceProperties deviceProperties;
		vk.GetPhysicalDeviceProperties(cphysDev, &deviceProperties);

		if (deviceProperties.apiVersion < VK_API_VERSION_1_2)
			continue;

		uint32_t queueFamilyCount = 0;
		vk.GetPhysicalDeviceQueueFamilyProperties(cphysDev, &queueFamilyCount, nullptr);
		std::vector<VkQueueFamilyProperties> queueFamilyProperties(queueFamilyCount);
		vk.GetPhysicalDeviceQueueFamilyProperties(cphysDev, &queueFamilyCount, queueFamilyProperties.data());

		uint32_t generalIndex = ~0u;
		uint32_t computeOnlyIndex = ~0u;
		for (uint32_t i = 0; i < queueFamilyCount; ++i) {
			const VkQueueFlags generalBits = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT;
			if ((queueFamilyProperties[i].queueFlags & generalBits) == generalBits )
				generalIndex = std::min(generalIndex, i);
			else if (bTryComputeOnly && queueFamilyProperties[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
				computeOnlyIndex = std::min(computeOnlyIndex, i);
		}

		if (generalIndex != ~0u || computeOnlyIndex != ~0u)
		{
			m_queueFamily = computeOnlyIndex == ~0u ? generalIndex : computeOnlyIndex;
			m_physDev = cphysDev;
			break;
		}
	}

	if (!m_physDev)
	{
		vk_log.errorf("failed to find physical device");
		return false;
	}

	VkPhysicalDeviceProperties props;
	vk.GetPhysicalDeviceProperties( m_physDev, &props );
	vk_log.infof( "selecting physical device '%s': queue family %x", props.deviceName, m_queueFamily );

	return true;
}

bool CVulkanDevice::createDevice()
{
	vk.GetPhysicalDeviceMemoryProperties( physDev(), &m_memoryProperties );

	uint32_t supportedExtensionCount;
	vkEnumerateDeviceExtensionProperties( physDev(), NULL, &supportedExtensionCount, NULL );

	std::vector<VkExtensionProperties> supportedExts(supportedExtensionCount);
	vkEnumerateDeviceExtensionProperties( physDev(), NULL, &supportedExtensionCount, supportedExts.data() );

	bool hasDrmProps = false;
	bool supportsForeignQueue = false;
	for ( uint32_t i = 0; i < supportedExtensionCount; ++i )
	{
		if ( strcmp(supportedExts[i].extensionName,
		     VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) == 0 )
			m_bSupportsModifiers = true;

		if ( strcmp(supportedExts[i].extensionName,
		            VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME) == 0 )
			hasDrmProps = true;

		if ( strcmp(supportedExts[i].extensionName,
		     VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME) == 0 )
			supportsForeignQueue = true;
	}

	vk_log.infof( "physical device %s DRM format modifiers", m_bSupportsModifiers ? "supports" : "does not support" );

	if ( hasDrmProps ) {
		VkPhysicalDeviceDrmPropertiesEXT drmProps = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
		};
		VkPhysicalDeviceProperties2 props2 = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			.pNext = &drmProps,
		};
		vk.GetPhysicalDeviceProperties2( physDev(), &props2 );

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

		m_drmRendererFd = open( renderName, O_RDWR | O_CLOEXEC );
		if ( m_drmRendererFd < 0 ) {
			vk_log.errorf_errno( "failed to open DRM render node" );
			return false;
		}

		if ( drmProps.hasPrimary ) {
			m_bHasDrmPrimaryDevId = true;
			m_drmPrimaryDevId = makedev( drmProps.primaryMajor, drmProps.primaryMinor );
		}
	} else {
		vk_log.errorf( "physical device doesn't support VK_EXT_physical_device_drm" );
		return false;
	}

	if ( m_bSupportsModifiers && !supportsForeignQueue ) {
		vk_log.infof( "The vulkan driver does not support foreign queues,"
		              " disabling modifier support.");
		m_bSupportsModifiers = false;
	}

	{
		VkPhysicalDeviceVulkan12Features vulkan12Features = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		};
		VkPhysicalDeviceFeatures2 features2 = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
			.pNext = &vulkan12Features,
		};
		vk.GetPhysicalDeviceFeatures2( physDev(), &features2 );

		m_bSupportsFp16 = vulkan12Features.shaderFloat16 && features2.features.shaderInt16;
	}

	float queuePriorities = 1.0f;

	VkDeviceQueueGlobalPriorityCreateInfoEXT queueCreateInfoEXT = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_EXT,
		.pNext = nullptr,
		.globalPriority = VK_QUEUE_GLOBAL_PRIORITY_REALTIME_EXT
	};

	VkDeviceQueueCreateInfo queueCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.pNext = g_bNiceCap ? &queueCreateInfoEXT : nullptr,
		.queueFamilyIndex = m_queueFamily,
		.queueCount = 1,
		.pQueuePriorities = &queuePriorities
	};

	std::vector< const char * > enabledExtensions;

	if ( BIsNested() == true )
	{
		enabledExtensions.push_back( VK_KHR_SWAPCHAIN_EXTENSION_NAME );
	}

	if ( m_bSupportsModifiers )
	{
		enabledExtensions.push_back( VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME );
		enabledExtensions.push_back( VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME );
	}

	enabledExtensions.push_back( VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME );
	enabledExtensions.push_back( VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME );

	enabledExtensions.push_back( VK_EXT_ROBUSTNESS_2_EXTENSION_NAME );

	VkPhysicalDeviceFeatures2 features2 = {};
	features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features2.features.shaderInt16 = m_bSupportsFp16;

	VkDeviceCreateInfo deviceCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &features2,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queueCreateInfo,
		.enabledExtensionCount = (uint32_t)enabledExtensions.size(),
		.ppEnabledExtensionNames = enabledExtensions.data(),
	};

	VkPhysicalDeviceVulkan12Features vulkan12Features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.pNext = std::exchange(features2.pNext, &vulkan12Features),
		.shaderFloat16 = m_bSupportsFp16,
		.timelineSemaphore = VK_TRUE,
	};

	VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrFeatures = {};
	ycbcrFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
	ycbcrFeatures.pNext = std::exchange(features2.pNext, &ycbcrFeatures);
	ycbcrFeatures.samplerYcbcrConversion = VK_TRUE;

	VkPhysicalDeviceRobustness2FeaturesEXT robustness2Features = {};
	robustness2Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
	robustness2Features.pNext = std::exchange(features2.pNext, &robustness2Features);
	robustness2Features.nullDescriptor = VK_TRUE;

	VkResult res = vk.CreateDevice(physDev(), &deviceCreateInfo, nullptr, &m_device);
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkCreateDevice failed" );
		return false;
	}

	#define VK_FUNC(x) vk.x = (PFN_vk##x) vk.GetDeviceProcAddr(device(), "vk"#x);
	VULKAN_DEVICE_FUNCTIONS
	#undef VK_FUNC

	vk.GetDeviceQueue(device(), m_queueFamily, 0, &m_queue);

	return true;
}

bool CVulkanDevice::createLayouts()
{
	VkFormatProperties nv12Properties;
	vk.GetPhysicalDeviceFormatProperties(physDev(), VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, &nv12Properties);
	bool cosited = nv12Properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT;

	VkSamplerYcbcrConversionCreateInfo ycbcrSamplerConversionCreateInfo = 
	{
		.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
		.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
		.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
		.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL,
		.xChromaOffset = cosited ? VK_CHROMA_LOCATION_COSITED_EVEN : VK_CHROMA_LOCATION_MIDPOINT,
		.yChromaOffset = cosited ? VK_CHROMA_LOCATION_COSITED_EVEN : VK_CHROMA_LOCATION_MIDPOINT,
		.chromaFilter = VK_FILTER_LINEAR,
		.forceExplicitReconstruction = VK_FALSE,
	};

	vk.CreateSamplerYcbcrConversion( device(), &ycbcrSamplerConversionCreateInfo, nullptr, &m_ycbcrConversion );

	VkSamplerYcbcrConversionInfo ycbcrSamplerConversionInfo = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
		.conversion = m_ycbcrConversion,
	};

	VkSamplerCreateInfo ycbcrSamplerInfo = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.pNext = &ycbcrSamplerConversionInfo,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
	};
	
	vk.CreateSampler( device(), &ycbcrSamplerInfo, nullptr, &m_ycbcrSampler );

	// Create an array of our ycbcrSampler to fill up
	std::array<VkSampler, k_nMaxLayers> ycbcrSamplers;
	for (auto& sampler : ycbcrSamplers)
		sampler = m_ycbcrSampler;
	
	std::vector< VkDescriptorSetLayoutBinding > layoutBindings;
	VkDescriptorSetLayoutBinding descriptorBinding =
	{
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	};
	
	layoutBindings.push_back( descriptorBinding );
	
	descriptorBinding.binding = 1;
	descriptorBinding.descriptorCount = k_nMaxLayers;
	descriptorBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

	layoutBindings.push_back( descriptorBinding );

	descriptorBinding.binding = 2;
	descriptorBinding.descriptorCount = k_nMaxLayers;
	descriptorBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	descriptorBinding.pImmutableSamplers = ycbcrSamplers.data();

	layoutBindings.push_back( descriptorBinding );

	descriptorBinding.binding = 3;
	descriptorBinding.descriptorCount = 1;
	descriptorBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	descriptorBinding.pImmutableSamplers = nullptr;

	layoutBindings.push_back( descriptorBinding );

	descriptorBinding.binding = 4;
	descriptorBinding.descriptorCount = 1;
	descriptorBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	descriptorBinding.pImmutableSamplers = nullptr;

	layoutBindings.push_back( descriptorBinding );

	descriptorBinding.binding = 5;
	descriptorBinding.descriptorCount = 1;
	descriptorBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	descriptorBinding.pImmutableSamplers = nullptr;

	layoutBindings.push_back( descriptorBinding );

	VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo =
	{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = (uint32_t)layoutBindings.size(),
		.pBindings = layoutBindings.data()
	};

	VkResult res = vk.CreateDescriptorSetLayout(device(), &descriptorSetLayoutCreateInfo, 0, &m_descriptorSetLayout);
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkCreateDescriptorSetLayout failed" );
		return false;
	}

	VkPushConstantRange pushConstantRange = {
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		.offset = 0,
		.size = 128,
	};
	
	VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &m_descriptorSetLayout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &pushConstantRange,
	};
	
	res = vk.CreatePipelineLayout(device(), &pipelineLayoutCreateInfo, nullptr, &m_pipelineLayout);
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkCreatePipelineLayout failed" );
		return false;
	}

	return true;
}

bool CVulkanDevice::createPools()
{
	VkCommandPoolCreateInfo commandPoolCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = m_queueFamily,
	};

	VkResult res = vk.CreateCommandPool(device(), &commandPoolCreateInfo, nullptr, &m_commandPool);
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkCreateCommandPool failed" );
		return false;
	}

	VkDescriptorPoolSize poolSizes[2] {
		{
			VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			uint32_t(m_descriptorSets.size()) * 1,
		},
		{
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			uint32_t(m_descriptorSets.size()) * (2 * k_nMaxLayers + 3),
		},
	};
	
	VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = uint32_t(m_descriptorSets.size()),
		.poolSizeCount = sizeof(poolSizes) / sizeof(poolSizes[0]),
		.pPoolSizes = poolSizes,
	};
	
	res = vk.CreateDescriptorPool(device(), &descriptorPoolCreateInfo, nullptr, &m_descriptorPool);
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkCreateDescriptorPool failed" );
		return false;
	}

	return true;
}

bool CVulkanDevice::createShaders()
{
	struct ShaderInfo_t
	{
		const uint32_t* spirv;
		uint32_t size;
	};

	std::array<ShaderInfo_t, SHADER_TYPE_COUNT> shaderInfos;
#define SHADER(type, array) shaderInfos[SHADER_TYPE_##type] = {array , sizeof(array)}
	SHADER(BLIT, cs_composite_blit);
	SHADER(BLUR, cs_composite_blur);
	SHADER(BLUR_COND, cs_composite_blur_cond);
	SHADER(BLUR_FIRST_PASS, cs_gaussian_blur_horizontal);
	SHADER(RCAS, cs_composite_rcas);
	if (m_bSupportsFp16)
	{
		SHADER(EASU, cs_easu_fp16);
		SHADER(NIS, cs_nis_fp16);
	}
	else
	{
		SHADER(EASU, cs_easu);
		SHADER(NIS, cs_nis);
	}
#undef SHADER

	for (uint32_t i = 0; i < shaderInfos.size(); i++)
	{
		VkShaderModuleCreateInfo shaderCreateInfo = {
			.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.codeSize = shaderInfos[i].size,
			.pCode = shaderInfos[i].spirv,
		};

		VkResult res = vk.CreateShaderModule(device(), &shaderCreateInfo, nullptr, &m_shaderModules[i]);
		if ( res != VK_SUCCESS )
		{
			vk_errorf( res, "vkCreateShaderModule failed" );
			return false;
		}
	}

	return true;
}

bool CVulkanDevice::createScratchResources()
{
	std::vector<VkDescriptorSetLayout> descriptorSetLayouts(m_descriptorSets.size(), m_descriptorSetLayout);
	
	VkDescriptorSetAllocateInfo descriptorSetAllocateInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = m_descriptorPool,
		.descriptorSetCount = (uint32_t)descriptorSetLayouts.size(),
		.pSetLayouts = descriptorSetLayouts.data(),
	};
	
	VkResult res = vk.AllocateDescriptorSets(device(), &descriptorSetAllocateInfo, m_descriptorSets.data());
	if ( res != VK_SUCCESS )
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
	
	res = vk.CreateBuffer( device(), &bufferCreateInfo, nullptr, &m_uploadBuffer );
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkCreateBuffer failed" );
		return false;
	}
	
	VkMemoryRequirements memRequirements;
	vk.GetBufferMemoryRequirements(device(), m_uploadBuffer, &memRequirements);
	
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
	
	vk.AllocateMemory( device(), &allocInfo, nullptr, &m_uploadBufferMemory);
	
	vk.BindBufferMemory( device(), m_uploadBuffer, m_uploadBufferMemory, 0 );

	res = vk.MapMemory( device(), m_uploadBufferMemory, 0, VK_WHOLE_SIZE, 0, (void**)&m_uploadBufferData );
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkMapMemory failed" );
		return false;
	}
	
	VkCommandBufferAllocateInfo commandBufferAllocateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.pNext = nullptr,
		.commandPool = m_commandPool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1
	};
	
	for ( uint32_t i = 0; i < k_nScratchCmdBufferCount; i++ )
	{
		res = vk.AllocateCommandBuffers( device(), &commandBufferAllocateInfo, &m_scratchCommandBuffers[ i ].cmdBuf );
		if ( res != VK_SUCCESS )
		{
			vk_errorf( res, "vkAllocateCommandBuffers failed" );
			return false;
		}
	}
	
	VkSemaphoreTypeCreateInfo timelineCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
	};

	VkSemaphoreCreateInfo semCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		.pNext = &timelineCreateInfo,
	};

	res = vk.CreateSemaphore( device(), &semCreateInfo, NULL, &m_scratchTimelineSemaphore );
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkCreateSemaphore failed" );
		return false;
	}

	return true;
}

VkSampler CVulkanDevice::sampler( VulkanSamplerCacheKey_t key )
{
	if ( m_samplerCache.count(key) != 0 )
		return m_samplerCache[key];

	VkSampler ret = VK_NULL_HANDLE;

	VkSamplerCreateInfo samplerCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = key.bNearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
		.minFilter = key.bNearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
		.unnormalizedCoordinates = key.bUnnormalized,
	};

	vk.CreateSampler( device(), &samplerCreateInfo, nullptr, &ret );

	m_samplerCache[key] = ret;

	return ret;
}

VkPipeline CVulkanDevice::compilePipeline(uint32_t layerCount, uint32_t ycbcrMask, uint32_t radius, ShaderType type, uint32_t blur_layer_count)
{
	const std::array<VkSpecializationMapEntry, 5> specializationEntries = {{
		{
			.constantID = 0,
			.offset     = sizeof(uint32_t) * 0,
			.size       = sizeof(uint32_t)
		},
		{
			.constantID = 1,
			.offset     = sizeof(uint32_t) * 1,
			.size       = sizeof(uint32_t)
		},
		{
			.constantID = 2,
			.offset     = sizeof(uint32_t) * 2,
			.size       = sizeof(uint32_t)
		},
		{
			.constantID = 3,
			.offset     = sizeof(uint32_t) * 3,
			.size       = sizeof(uint32_t)
		},
		{
			.constantID = 4,
			.offset     = sizeof(uint32_t) * 4,
			.size       = sizeof(uint32_t)
		},
	}};

	struct {
		uint32_t layerCount;
		uint32_t ycbcrMask;
		uint32_t debug;
		uint32_t radius;
		uint32_t blur_layer_count;
	} specializationData = {
		.layerCount   = layerCount,
		.ycbcrMask    = ycbcrMask,
		.debug        = g_bIsCompositeDebug,
		.radius       = radius ? (radius * 2) - 1 : 0,
		.blur_layer_count = blur_layer_count,
	};

	VkSpecializationInfo specializationInfo = {
		.mapEntryCount = uint32_t(specializationEntries.size()),
		.pMapEntries   = specializationEntries.data(),
		.dataSize      = sizeof(specializationData),
		.pData		   = &specializationData,
	};

	VkComputePipelineCreateInfo computePipelineCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = m_shaderModules[type],
			.pName = "main",
			.pSpecializationInfo = &specializationInfo
		},
		.layout = m_pipelineLayout,
	};

	VkPipeline result;

	VkResult res = vk.CreateComputePipelines(device(), VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &result);
	if (res != VK_SUCCESS) {
		vk_errorf( res, "vkCreateComputePipelines failed" );
		return VK_NULL_HANDLE;
	}

	return result;
}

void CVulkanDevice::compileAllPipelines()
{


	std::array<PipelineInfo_t, SHADER_TYPE_COUNT> pipelineInfos;
#define SHADER(type, layer_count, max_ycbcr, max_radius, blur_layers) pipelineInfos[SHADER_TYPE_##type] = {SHADER_TYPE_##type, layer_count, max_ycbcr, max_radius, blur_layers}
	SHADER(BLIT, k_nMaxLayers, k_nMaxYcbcrMask, 1, 1);
	SHADER(BLUR, k_nMaxLayers, k_nMaxYcbcrMask, kMaxBlurRadius, k_nMaxBlurLayers);
	SHADER(BLUR_COND, k_nMaxLayers, k_nMaxYcbcrMask, kMaxBlurRadius, k_nMaxBlurLayers);
	SHADER(BLUR_FIRST_PASS, 1, 2, kMaxBlurRadius, 1);
	SHADER(RCAS, k_nMaxLayers, k_nMaxYcbcrMask, 1, 1);
	SHADER(EASU, 1, 1, 1, 1);
	SHADER(NIS, 1, 1, 1, 1);
#undef SHADER

	for (auto& info : pipelineInfos) {
		for (uint32_t layerCount = 1; layerCount <= info.layerCount; layerCount++) {
			for (uint32_t ycbcrMask = 0; ycbcrMask < info.ycbcrMask; ycbcrMask++) {
				for (uint32_t radius = 0; radius < info.blurRadius; radius++) {
					for (uint32_t blur_layers = 0; blur_layers < info.blurLayerCount; blur_layers++) {
						if (ycbcrMask >= (1u << (layerCount + 1)))
							continue;
						if (blur_layers > layerCount)
							continue;

						VkPipeline newPipeline = compilePipeline(layerCount, ycbcrMask, radius, info.shaderType, blur_layers);
						{
							std::lock_guard<std::mutex> lock(m_pipelineMutex);
							PipelineInfo_t key = {info.shaderType, layerCount, ycbcrMask, radius, blur_layers};
							auto result = m_pipelineMap.emplace(std::make_pair(key, newPipeline));
							if (!result.second)
								vk.DestroyPipeline(device(), newPipeline, nullptr);
						}
					}
				}
			}
		}
	}
}

VkPipeline CVulkanDevice::pipeline(ShaderType type, uint32_t layerCount, uint32_t ycbcrMask, uint32_t radius, uint32_t blur_layers)
{
	std::lock_guard<std::mutex> lock(m_pipelineMutex);
	PipelineInfo_t key = {type, layerCount, ycbcrMask, radius, blur_layers};
	auto search = m_pipelineMap.find(key);
	if (search == m_pipelineMap.end())
	{
		VkPipeline result = compilePipeline(layerCount, ycbcrMask, radius, type, blur_layers);
		m_pipelineMap[key] = result;
		return result;
	}
	else
	{
		return search->second;
	}
}


int32_t CVulkanDevice::findMemoryType( VkMemoryPropertyFlags properties, uint32_t requiredTypeBits )
{
	for ( uint32_t i = 0; i < m_memoryProperties.memoryTypeCount; i++ )
	{
		if ( ( ( 1 << i ) & requiredTypeBits ) == 0 )
			continue;
		
		if ( ( properties & m_memoryProperties.memoryTypes[ i ].propertyFlags ) != properties )
			continue;
		
		return i;
	}
	
	return -1;
}

uint32_t CVulkanDevice::commandBuffer( VkCommandBuffer &cmdBuf )
{
	uint64_t currentSeqNo;
	VkResult res = vk.GetSemaphoreCounterValue(device(), m_scratchTimelineSemaphore, &currentSeqNo);
	assert( res == VK_SUCCESS );

	for ( uint32_t i = 0; i < k_nScratchCmdBufferCount; i++ )
	{
		if ( m_scratchCommandBuffers[ i ].seqNo <= currentSeqNo )
		{
			cmdBuf = m_scratchCommandBuffers[ i ].cmdBuf;
			
			VkCommandBufferBeginInfo commandBufferBeginInfo = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
			};
			
			VkResult res = vk.BeginCommandBuffer( cmdBuf, &commandBufferBeginInfo);
			
			if ( res != VK_SUCCESS )
			{
				break;
			}
			
			m_scratchCommandBuffers[ i ].refs.clear();
			
			return i;
		}
	}
	
	assert( 0 );
	return 0;
}

void CVulkanDevice::submitCommandBuffer( uint32_t handle, std::vector<std::shared_ptr<CVulkanTexture>> &vecRefs )
{
	auto &buf = m_scratchCommandBuffers[ handle ];

	VkResult res = vk.EndCommandBuffer( buf.cmdBuf );
	
	if ( res != VK_SUCCESS )
	{
		assert( 0 );
	}
	
	// The seq no of the last submission.
	const uint64_t lastSubmissionSeqNo = m_submissionSeqNo++;

	// This is the seq no of the command buffer we are going to submit.
	const uint64_t nextSeqNo = lastSubmissionSeqNo + 1;

	VkTimelineSemaphoreSubmitInfo timelineInfo = {
		.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
		.pNext = NULL,
		// Ensure order of scratch cmd buffer submission
		.waitSemaphoreValueCount = 1,
		.pWaitSemaphoreValues = &lastSubmissionSeqNo,
		.signalSemaphoreValueCount = 1,
		.pSignalSemaphoreValues = &nextSeqNo,
	};

	const VkPipelineStageFlags wait_mask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

	VkSubmitInfo submitInfo = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.pNext = &timelineInfo,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &m_scratchTimelineSemaphore,
		.pWaitDstStageMask = &wait_mask,
		.commandBufferCount = 1,
		.pCommandBuffers = &buf.cmdBuf,
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = &m_scratchTimelineSemaphore,
	};
	
	res = vk.QueueSubmit( queue(), 1, &submitInfo, VK_NULL_HANDLE );
	
	if ( res != VK_SUCCESS )
	{
		assert( 0 );
	}
	
	buf.seqNo = nextSeqNo;
	
	for( uint32_t i = 0; i < vecRefs.size(); i++ )
		m_scratchCommandBuffers[ handle ].refs.push_back( std::move(vecRefs[ i ]) );
}

void CVulkanDevice::garbageCollect( void )
{
	uint64_t currentSeqNo;
	VkResult res = vk.GetSemaphoreCounterValue(device(), m_scratchTimelineSemaphore, &currentSeqNo);
	assert( res == VK_SUCCESS );

	for ( uint32_t i = 0; i < k_nScratchCmdBufferCount; i++ )
	{
		// We reset seqNo to 0 when we know it's not busy.
		const uint64_t buffer_seq_no = m_scratchCommandBuffers[ i ].seqNo;
		if ( buffer_seq_no && buffer_seq_no <= currentSeqNo )
		{
			vk.ResetCommandBuffer( m_scratchCommandBuffers[ i ].cmdBuf, 0 );
			
			m_scratchCommandBuffers[ i ].refs.clear();
			m_scratchCommandBuffers[ i ].seqNo = 0;
		}
	}
}

static CVulkanDevice g_device;

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
	VkImageFormatListCreateInfo formatList = {};
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

	const VkImageFormatListCreateInfo *readonlyList = pNextFind<VkImageFormatListCreateInfo>(imageInfo, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO);
	if ( readonlyList != nullptr )
	{
		formatList = *readonlyList;
		formatList.pNext = std::exchange(imageFormatInfo.pNext, &formatList);
	}

	imageProps.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
	imageProps.pNext = externalFormatProps;

	return vkGetPhysicalDeviceImageFormatProperties2(g_device.physDev(), &imageFormatInfo, &imageProps);
}

bool CVulkanTexture::BInit( uint32_t width, uint32_t height, uint32_t drmFormat, createFlags flags, wlr_dmabuf_attributes *pDMA /* = nullptr */,  uint32_t contentWidth /* = 0 */, uint32_t contentHeight /* =  0 */)
{
	VkResult res = VK_ERROR_INITIALIZATION_FAILED;

	VkImageTiling tiling = (flags.bMappable || flags.bLinear) ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
	VkImageUsageFlags usage = 0;
	VkMemoryPropertyFlags properties;

	if ( flags.bSampled == true )
	{
		usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	}

	if ( flags.bStorage == true )
	{
		usage |= VK_IMAGE_USAGE_STORAGE_BIT;
	}

	if ( flags.bFlippable == true )
	{
		flags.bExportable = true;
	}

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
	imageInfo.format = DRMFormatToVulkan(drmFormat, false);
	imageInfo.tiling = tiling;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = usage;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	assert( imageInfo.format != VK_FORMAT_UNDEFINED );

	std::array<VkFormat, 2> formats = {
		DRMFormatToVulkan(drmFormat, false),
		DRMFormatToVulkan(drmFormat, true),
	};
	VkImageFormatListCreateInfo formatList = {};
	formatList.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
	formatList.viewFormatCount = (uint32_t)formats.size();
	formatList.pViewFormats = formats.data();
	if ( formats[0] != formats[1] )
	{
		formatList.pNext = std::exchange(imageInfo.pNext, &formatList);
		imageInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
	}

	if ( pDMA != nullptr )
	{
		assert( drmFormat == pDMA->format );
	}

	if ( g_device.supportsModifiers() && pDMA && pDMA->modifier != DRM_FORMAT_MOD_INVALID )
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
			modifierInfo.pNext = std::exchange(imageInfo.pNext, &modifierInfo);
			modifierInfo.drmFormatModifier = pDMA->modifier;
			modifierInfo.drmFormatModifierPlaneCount = pDMA->n_planes;
			modifierInfo.pPlaneLayouts = modifierPlaneLayouts;

			for ( int i = 0; i < pDMA->n_planes; ++i )
			{
				modifierPlaneLayouts[i].offset = pDMA->offset[i];
				modifierPlaneLayouts[i].rowPitch = pDMA->stride[i];
			}

			imageInfo.tiling = tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
		}
	}

	std::vector<uint64_t> modifiers = {};
	if ( flags.bFlippable == true && g_device.supportsModifiers() && !pDMA )
	{
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
		modifierListInfo.pNext = std::exchange(imageInfo.pNext, &modifierListInfo);
		modifierListInfo.pDrmFormatModifiers = modifiers.data();
		modifierListInfo.drmFormatModifierCount = modifiers.size();

		imageInfo.tiling = tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	}

	if ( flags.bFlippable == true && tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT )
	{
		// We want to scan-out the image
		wsiImageCreateInfo.sType = VK_STRUCTURE_TYPE_WSI_IMAGE_CREATE_INFO_MESA;
		wsiImageCreateInfo.scanout = VK_TRUE;
		wsiImageCreateInfo.pNext = std::exchange(imageInfo.pNext, &wsiImageCreateInfo);
	}
	
	if ( pDMA != nullptr )
	{
		externalImageCreateInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
		externalImageCreateInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		externalImageCreateInfo.pNext = std::exchange(imageInfo.pNext, &externalImageCreateInfo);
	}

	m_width = width;
	m_height = height;

	if (contentWidth && contentHeight)
	{
		m_contentWidth = contentWidth;
		m_contentHeight = contentHeight;
	}
	else
	{
		m_contentWidth = width;
		m_contentHeight = height;
	}

	m_format = imageInfo.format;

	res = vkCreateImage(g_device.device(), &imageInfo, nullptr, &m_vkImage);
	if (res != VK_SUCCESS) {
		vk_errorf( res, "vkCreateImage failed" );
		return false;
	}
	
	VkMemoryRequirements memRequirements;
	vkGetImageMemoryRequirements(g_device.device(), m_vkImage, &memRequirements);
	
	// Possible pNexts
	wsi_memory_allocate_info wsiAllocInfo = {};
	VkImportMemoryFdInfoKHR importMemoryInfo = {};
	VkExportMemoryAllocateInfo memory_export_info = {};
	VkMemoryDedicatedAllocateInfo memory_dedicated_info = {};
	
	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = g_device.findMemoryType(properties, memRequirements.memoryTypeBits );
	
	if ( flags.bExportable == true || pDMA != nullptr )
	{
		memory_dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
		memory_dedicated_info.image = m_vkImage;
		memory_dedicated_info.buffer = VK_NULL_HANDLE;
		memory_dedicated_info.pNext = allocInfo.pNext;
		
		allocInfo.pNext = &memory_dedicated_info;
	}
	
	if ( flags.bExportable == true && pDMA == nullptr )
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
	
	res = vkAllocateMemory( g_device.device(), &allocInfo, nullptr, &m_vkImageMemory );
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkAllocateMemory failed" );
		return false;
	}
	
	res = vkBindImageMemory( g_device.device(), m_vkImage, m_vkImageMemory, 0 );
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
		vkGetImageSubresourceLayout(g_device.device(), m_vkImage, &image_subresource, &image_layout);

		m_unRowPitch = image_layout.rowPitch;
	}
	
	if ( flags.bExportable == true )
	{
		// We assume we own the memory when doing this right now.
		// We could support the import scenario as well if needed (but we
		// already have a DMA-BUF in that case).
// 		assert( bTextureable == false );
		assert( pDMA == nullptr );

		struct wlr_dmabuf_attributes dmabuf = {};
		dmabuf.width = width;
		dmabuf.height = height;
		dmabuf.format = drmFormat;
		assert( dmabuf.format != DRM_FORMAT_INVALID );

		// TODO: disjoint planes support
		const VkMemoryGetFdInfoKHR memory_get_fd_info = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
			.pNext = NULL,
			.memory = m_vkImageMemory,
			.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		};
		res = g_device.vk.GetMemoryFdKHR(g_device.device(), &memory_get_fd_info, &dmabuf.fd[0]);
		if ( res != VK_SUCCESS ) {
			vk_errorf( res, "vkGetMemoryFdKHR failed" );
			return false;
		}

		if ( tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT )
		{
			assert( g_device.vk.GetImageDrmFormatModifierPropertiesEXT != nullptr );
			VkImageDrmFormatModifierPropertiesEXT imgModifierProps = {};
			imgModifierProps.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
			res = g_device.vk.GetImageDrmFormatModifierPropertiesEXT( g_device.device(), m_vkImage, &imgModifierProps );
			if ( res != VK_SUCCESS ) {
				vk_errorf( res, "vkGetImageDrmFormatModifierPropertiesEXT failed" );
				return false;
			}
			dmabuf.modifier = imgModifierProps.drmFormatModifier;

			assert( DRMModifierProps.count( m_format ) > 0);
			assert( DRMModifierProps[ m_format ].count( dmabuf.modifier ) > 0);

			dmabuf.n_planes = DRMModifierProps[ m_format ][ dmabuf.modifier ].drmFormatModifierPlaneCount;

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
				vkGetImageSubresourceLayout( g_device.device(), m_vkImage, &subresource, &subresourceLayout );
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
			vkGetImageSubresourceLayout( g_device.device(), m_vkImage, &subresource, &subresourceLayout );

			dmabuf.n_planes = 1;
			dmabuf.modifier = DRM_FORMAT_MOD_INVALID;
			dmabuf.offset[0] = 0;
			dmabuf.stride[0] = subresourceLayout.rowPitch;
		}

		m_dmabuf = dmabuf;
	}

	if ( flags.bFlippable == true )
	{
		m_FBID = drm_fbid_from_dmabuf( &g_DRM, nullptr, &m_dmabuf );
		if ( m_FBID == 0 ) {
			vk_log.errorf( "drm_fbid_from_dmabuf failed" );
			return false;
		}
	}

	bool bHasAlpha = pDMA ? DRMFormatHasAlpha( pDMA->format ) : true;

	if (!bHasAlpha )
	{
		// not compatible with with swizzles
		assert ( flags.bStorage == false );
	}

	if ( flags.bStorage || flags.bSampled )
	{
		VkImageViewCreateInfo createInfo = {};
		createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		createInfo.image = m_vkImage;
		createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		createInfo.format = DRMFormatToVulkan(drmFormat, false);;
		createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
		createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
		createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
		createInfo.components.a = bHasAlpha ? VK_COMPONENT_SWIZZLE_IDENTITY : VK_COMPONENT_SWIZZLE_ONE;
		createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		createInfo.subresourceRange.baseMipLevel = 0;
		createInfo.subresourceRange.levelCount = 1;
		createInfo.subresourceRange.baseArrayLayer = 0;
		createInfo.subresourceRange.layerCount = 1;

		res = vkCreateImageView(g_device.device(), &createInfo, nullptr, &m_srgbView);
		if ( res != VK_SUCCESS ) {
			vk_errorf( res, "vkCreateImageView failed" );
			return false;
		}

		if ( flags.bSampled )
		{
			VkImageViewUsageCreateInfo viewUsageInfo = {};
			viewUsageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
			viewUsageInfo.usage = usage & ~VK_IMAGE_USAGE_STORAGE_BIT;
			createInfo.pNext = &viewUsageInfo;
			createInfo.format = DRMFormatToVulkan(drmFormat, true);
			res = vkCreateImageView(g_device.device(), &createInfo, nullptr, &m_linearView);
			if ( res != VK_SUCCESS ) {
				vk_errorf( res, "vkCreateImageView failed" );
				return false;
			}
		}
	}

	if ( flags.bMappable )
	{
		res = vkMapMemory( g_device.device(), m_vkImageMemory, 0, VK_WHOLE_SIZE, 0, &m_pMappedData );
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
	wlr_dmabuf_attributes_finish( &m_dmabuf );

	if ( m_pMappedData != nullptr )
	{
		vkUnmapMemory( g_device.device(), m_vkImageMemory );
		m_pMappedData = nullptr;
	}

	if ( m_srgbView != VK_NULL_HANDLE )
	{
		vkDestroyImageView( g_device.device(), m_srgbView, nullptr );
		m_srgbView = VK_NULL_HANDLE;
	}

	if ( m_linearView != VK_NULL_HANDLE )
	{
		vkDestroyImageView( g_device.device(), m_linearView, nullptr );
		m_linearView = VK_NULL_HANDLE;
	}

	if ( m_FBID != 0 )
	{
		drm_drop_fbid( &g_DRM, m_FBID );
		m_FBID = 0;
	}


	if ( m_vkImage != VK_NULL_HANDLE )
	{
		vkDestroyImage( g_device.device(), m_vkImage, nullptr );
		m_vkImage = VK_NULL_HANDLE;
	}


	if ( m_vkImageMemory != VK_NULL_HANDLE )
	{
		vkFreeMemory( g_device.device(), m_vkImageMemory, nullptr );
		m_vkImageMemory = VK_NULL_HANDLE;
	}

	m_bInitialized = false;
}

bool vulkan_init_format(VkFormat format, uint32_t drmFormat)
{
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
	VkResult res = vkGetPhysicalDeviceImageFormatProperties2( g_device.physDev(), &imageFormatInfo, &imageFormatProps );
	if ( res == VK_ERROR_FORMAT_NOT_SUPPORTED )
	{
		return false;
	}
	else if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkGetPhysicalDeviceImageFormatProperties2 failed for DRM format 0x%" PRIX32, drmFormat );
		return false;
	}

	if ( std::find( sampledShmFormats.begin(), sampledShmFormats.end(), drmFormat ) == sampledShmFormats.end() ) 
		sampledShmFormats.push_back( drmFormat );

	if ( !g_device.supportsModifiers() )
	{
		if ( BIsNested() == false && !wlr_drm_format_set_has( &g_DRM.formats, drmFormat, DRM_FORMAT_MOD_INVALID ) )
		{
			return false;
		}
		wlr_drm_format_set_add( &sampledDRMFormats, drmFormat, DRM_FORMAT_MOD_INVALID );
		return false;
	}

	// Then, collect the list of modifiers supported for sampled usage
	VkDrmFormatModifierPropertiesListEXT modifierPropList = {};
	modifierPropList.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
	VkFormatProperties2 formatProps = {};
	formatProps.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
	formatProps.pNext = &modifierPropList;
	vkGetPhysicalDeviceFormatProperties2( g_device.physDev(), format, &formatProps );

	if ( modifierPropList.drmFormatModifierCount == 0 )
	{
		vk_errorf( res, "vkGetPhysicalDeviceFormatProperties2 returned zero modifiers for DRM format 0x%" PRIX32, drmFormat );
		return false;
	}

	std::vector<VkDrmFormatModifierPropertiesEXT> modifierProps(modifierPropList.drmFormatModifierCount);
	modifierPropList.pDrmFormatModifierProperties = modifierProps.data();
	vkGetPhysicalDeviceFormatProperties2( g_device.physDev(), format, &formatProps );

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
	return true;
}

bool vulkan_init_formats()
{
	for ( size_t i = 0; s_DRMVKFormatTable[i].DRMFormat != DRM_FORMAT_INVALID; i++ )
	{
		VkFormat format = s_DRMVKFormatTable[i].vkFormat;
		VkFormat srgbFormat = s_DRMVKFormatTable[i].vkFormatSrgb;
		uint32_t drmFormat = s_DRMVKFormatTable[i].DRMFormat;

		vulkan_init_format(format, drmFormat);
		if (format != srgbFormat)
			vulkan_init_format(srgbFormat, drmFormat);
	}

	vk_log.infof( "supported DRM formats for sampling usage:" );
	for ( size_t i = 0; i < sampledDRMFormats.len; i++ )
	{
		uint32_t fmt = sampledDRMFormats.formats[ i ]->format;
		vk_log.infof( "  0x%" PRIX32, fmt );
	}

	return true;
}

bool acquire_next_image( void )
{
	VkResult res = vkAcquireNextImageKHR( g_device.device(), g_output.swapChain, UINT64_MAX, VK_NULL_HANDLE, g_output.acquireFence, &g_output.nSwapChainImageIndex );
	if ( res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR )
		return false;
	if ( vkWaitForFences( g_device.device(), 1, &g_output.acquireFence, false, UINT64_MAX ) != VK_SUCCESS )
		return false;
	return vkResetFences( g_device.device(), 1, &g_output.acquireFence ) == VK_SUCCESS;
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
	
	if ( vkQueuePresentKHR( g_device.queue(), &presentInfo ) != VK_SUCCESS )
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
	
	if (vkCreateSwapchainKHR( g_device.device(), &createInfo, nullptr, &pOutput->swapChain) != VK_SUCCESS ) {
		return 0;
	}
	
	vkGetSwapchainImagesKHR( g_device.device(), pOutput->swapChain, &imageCount, nullptr );
	pOutput->swapChainImages.resize( imageCount );
	pOutput->swapChainImageViews.resize( imageCount );
	vkGetSwapchainImagesKHR( g_device.device(), pOutput->swapChain, &imageCount, pOutput->swapChainImages.data() );
	
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
		
		result = vkCreateImageView(g_device.device(), &createInfo, nullptr, &pOutput->swapChainImageViews[ i ]);
		
		if ( result != VK_SUCCESS )
			return false;
	}
	
	VkFenceCreateInfo fenceInfo = {};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

	vkCreateFence( g_device.device(), &fenceInfo, nullptr, &pOutput->acquireFence );

	return true;
}

bool vulkan_remake_swapchain( void )
{
	VulkanOutput_t *pOutput = &g_output;
	vkQueueWaitIdle( g_device.queue() );

	for ( uint32_t i = 0; i < pOutput->swapChainImages.size(); i++ )
	{
		vkDestroyImageView( g_device.device(), pOutput->swapChainImageViews[ i ], nullptr );
		
		pOutput->swapChainImageViews[ i ] = VK_NULL_HANDLE;
		pOutput->swapChainImages[ i ] = VK_NULL_HANDLE;
	}
	
	vkDestroySwapchainKHR( g_device.device(), pOutput->swapChain, nullptr );
	
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
	outputImageflags.bStorage = true;
	outputImageflags.bTransferSrc = true; // for screenshots

	pOutput->outputImage[0] = nullptr;
	pOutput->outputImage[1] = nullptr;

	pOutput->outputImage[0] = std::make_shared<CVulkanTexture>();
	bool bSuccess = pOutput->outputImage[0]->BInit( g_nOutputWidth, g_nOutputHeight, VulkanFormatToDRM(pOutput->outputFormat), outputImageflags );
	if ( bSuccess != true )
	{
		vk_log.errorf( "failed to allocate buffer for KMS" );
		return false;
	}

	pOutput->outputImage[1] = std::make_shared<CVulkanTexture>();
	bSuccess = pOutput->outputImage[1]->BInit( g_nOutputWidth, g_nOutputHeight, VulkanFormatToDRM(pOutput->outputFormat), outputImageflags );
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
	vkQueueWaitIdle( g_device.queue() );

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
		if ( !SDL_Vulkan_CreateSurface( g_SDLWindow, g_device.instance(), &pOutput->surface ) )
		{
			vk_log.errorf( "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError() );
			return false;
		}

		// TODO: check this when selecting the physical device and queue family
		VkBool32 canPresent = false;
		vkGetPhysicalDeviceSurfaceSupportKHR( g_device.physDev(), g_device.queueFamily(), pOutput->surface, &canPresent );
		if ( !canPresent )
		{
			vk_log.errorf( "physical device queue doesn't support presenting on our surface" );
			return false;
		}

		result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR( g_device.physDev(), pOutput->surface, &pOutput->surfaceCaps );
		if ( result != VK_SUCCESS )
		{
			vk_errorf( result, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed" );
			return false;
		}
		
		uint32_t formatCount = 0;
		result = vkGetPhysicalDeviceSurfaceFormatsKHR( g_device.physDev(), pOutput->surface, &formatCount, nullptr );
		if ( result != VK_SUCCESS )
		{
			vk_errorf( result, "vkGetPhysicalDeviceSurfaceFormatsKHR failed" );
			return false;
		}
		
		if ( formatCount != 0 ) {
			pOutput->surfaceFormats.resize( formatCount );
			vkGetPhysicalDeviceSurfaceFormatsKHR( g_device.physDev(), pOutput->surface, &formatCount, pOutput->surfaceFormats.data() );
			if ( result != VK_SUCCESS )
			{
				vk_errorf( result, "vkGetPhysicalDeviceSurfaceFormatsKHR failed" );
				return false;
			}
		}
		
		uint32_t presentModeCount = false;
		result = vkGetPhysicalDeviceSurfacePresentModesKHR(g_device.physDev(), pOutput->surface, &presentModeCount, nullptr );
		if ( result != VK_SUCCESS )
		{
			vk_errorf( result, "vkGetPhysicalDeviceSurfacePresentModesKHR failed" );
			return false;
		}
		
		if ( presentModeCount != 0 ) {
			pOutput->presentModes.resize(presentModeCount);
			result = vkGetPhysicalDeviceSurfacePresentModesKHR( g_device.physDev(), pOutput->surface, &presentModeCount, pOutput->presentModes.data() );
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
		.commandPool = g_device.commandPool(),
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 2
	};
	
	result = vkAllocateCommandBuffers(g_device.device(), &commandBufferAllocateInfo, pOutput->commandBuffers);
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

	if (!g_device.BInit())
		return false;

	if (!init_nis_data())
		return false;

	return true;
}

static void update_tmp_images( uint32_t width, uint32_t height )
{
	if ( g_output.tmpOutput != nullptr
			&& width == g_output.tmpOutput->width()
			&& height == g_output.tmpOutput->height() )
	{
		return;
	}

	CVulkanTexture::createFlags createFlags;
	createFlags.bSampled = true;
	createFlags.bStorage = true;

	g_output.tmpOutput = std::make_shared<CVulkanTexture>();
	bool bSuccess = g_output.tmpOutput->BInit( width, height, DRM_FORMAT_ARGB8888, createFlags, nullptr );

	if ( !bSuccess )
	{
		vk_log.errorf( "failed to create fsr output" );
		return;
	}
}


static bool allocate_nis_memory()
{
	VkMemoryRequirements memRequirements = {};

	vkGetImageMemoryRequirements(g_device.device(), g_output.nisScalerImage, &memRequirements);
	uint32_t supportedMemoryTypes = memRequirements.memoryTypeBits;
	VkDeviceSize memorySize = memRequirements.size;

	vkGetImageMemoryRequirements(g_device.device(), g_output.nisUsmImage, &memRequirements);
	supportedMemoryTypes &= memRequirements.memoryTypeBits;

	VkDeviceSize usmOffset = memorySize + (memRequirements.alignment - (memorySize % memRequirements.alignment));
	memorySize += memRequirements.size + usmOffset;

	int32_t memTypeIndex = g_device.findMemoryType(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, supportedMemoryTypes);
	if ( memTypeIndex == -1 )
	{
		vk_log.errorf( "findMemoryType failed" );
		return false;
	}

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memorySize;
	allocInfo.memoryTypeIndex = memTypeIndex;

	vkAllocateMemory(g_device.device(), &allocInfo, nullptr, &g_output.nisMemory );

	vkBindImageMemory(g_device.device(), g_output.nisScalerImage, g_output.nisMemory, 0);
	vkBindImageMemory(g_device.device(), g_output.nisUsmImage, g_output.nisMemory, usmOffset);

	return true;
}

static bool init_nis_data()
{
	// Create NIS Sampler

	VulkanSamplerCacheKey_t samplerKey;
	samplerKey.bNearest = false;
	samplerKey.bUnnormalized = false;

	g_output.nisSampler = g_device.sampler(samplerKey);
	assert ( g_output.nisSampler != VK_NULL_HANDLE );

	// Create the NIS images

	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = g_device.supportsFp16() ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R32G32B32A32_SFLOAT;
	imageInfo.extent.width = kFilterSize / 4;
	imageInfo.extent.height = kPhaseCount;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VkResult result = vkCreateImage(g_device.device(), &imageInfo, NULL, &g_output.nisScalerImage);
	if(result != VK_SUCCESS)
	{
		vk_log.errorf( "vkCreateImage for scaler coef image failed" );
		return false;
	}

	result = vkCreateImage(g_device.device(), &imageInfo, NULL, &g_output.nisUsmImage);
	if(result != VK_SUCCESS)
	{
		vk_log.errorf( "vkCreateImage for usm coef image failed" );
		return false;
	}

	if (!allocate_nis_memory())
		return false;

	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = g_output.nisScalerImage;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = g_device.supportsFp16() ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R32G32B32A32_SFLOAT;;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.baseMipLevel = 0;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.baseArrayLayer = 0;
	viewInfo.subresourceRange.layerCount = 1;

	result = vkCreateImageView(g_device.device(), &viewInfo, NULL, &g_output.nisScalerView);
	if(result != VK_SUCCESS)
	{
		vk_log.errorf( "vkCreateImageView for scaler coef view failed" );
		return false;
	}

	viewInfo.image = g_output.nisUsmImage;

	result = vkCreateImageView(g_device.device(), &viewInfo, NULL, &g_output.nisUsmView);
	if(result != VK_SUCCESS)
	{
		vk_log.errorf( "vkCreateImageView for scaler coef view failed" );
		return false;
	}

	// Select between the FP16 or FP32 coefficients

	const void* coefScaleData = g_device.supportsFp16() ?
		static_cast<const void*>(coef_scale_fp16) : static_cast<const void*>(coef_scale);
	uint32_t coefScaleSize = g_device.supportsFp16() ? sizeof(coef_scale_fp16) : sizeof(coef_scale);

	const void* coefUsmData = g_device.supportsFp16() ?
		static_cast<const void*>(coef_usm_fp16) : static_cast<const void*>(coef_usm);
	uint32_t coefUsmSize = g_device.supportsFp16() ? sizeof(coef_usm_fp16) : sizeof(coef_usm);

	// Create a staging buffer for uploading the data to the GPU

	VkBufferCreateInfo bufferInfo = {};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = coefScaleSize + coefUsmSize;
	bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VkBuffer stagingBuffer = VK_NULL_HANDLE;
	result = vkCreateBuffer( g_device.device(), &bufferInfo, NULL, &stagingBuffer );
	if(result != VK_SUCCESS)
	{
		vk_log.errorf( "Failed to create nis staging buffer" );
		return false;
	}

	VkMemoryRequirements memRequirements = {};
	vkGetBufferMemoryRequirements(g_device.device(), stagingBuffer, &memRequirements);

	int32_t memTypeIndex = g_device.findMemoryType(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, memRequirements.memoryTypeBits);
	if ( memTypeIndex == -1 )
	{
		vk_log.errorf( "findMemoryType failed for nis staging buffer" );
		return false;
	}

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = memTypeIndex;
	
	VkDeviceMemory stagingMem = VK_NULL_HANDLE;
	vkAllocateMemory( g_device.device(), &allocInfo, nullptr, &stagingMem );
	vkBindBufferMemory( g_device.device(), stagingBuffer, stagingMem, 0 );

	void* stagingData = nullptr;
	vkMapMemory(g_device.device(), stagingMem, 0, VK_WHOLE_SIZE, 0, &stagingData);

	// Copy the data into the staging buffer and upload it to GPU memory

	memcpy(stagingData, coefScaleData, coefScaleSize);
	memcpy(static_cast<char*>(stagingData) + coefScaleSize, coefUsmData, coefUsmSize);

	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	uint32_t handle = g_device.commandBuffer( commandBuffer );

	std::array<VkImageMemoryBarrier, 2> barriers = {};

	barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].image = g_output.nisScalerImage;
	barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers[0].subresourceRange.baseMipLevel = 0;
	barriers[0].subresourceRange.levelCount = 1;
	barriers[0].subresourceRange.baseArrayLayer = 0;
	barriers[0].subresourceRange.layerCount = 1;
	barriers[0].srcAccessMask = 0;
	barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

	barriers[1] = barriers[0];
	barriers[1].image = g_output.nisUsmImage;

	vkCmdPipelineBarrier(
		commandBuffer,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0,
		0, NULL,
		0, NULL,
		barriers.size(), barriers.data());

	VkBufferImageCopy imageCopy = {};

	imageCopy.bufferOffset = 0;
	imageCopy.imageExtent.width = kFilterSize / 4;
	imageCopy.imageExtent.height = kPhaseCount;
	imageCopy.imageExtent.depth = 1;
	imageCopy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	imageCopy.imageSubresource.mipLevel = 0;
	imageCopy.imageSubresource.baseArrayLayer = 0;
	imageCopy.imageSubresource.layerCount = 1;

	vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, g_output.nisScalerImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &imageCopy);

	imageCopy.bufferOffset = coefScaleSize;

	vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, g_output.nisUsmImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &imageCopy);

	barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barriers[0].dstAccessMask = 0;

	barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barriers[1].dstAccessMask = 0;

	vkCmdPipelineBarrier(
		commandBuffer,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		0,
		0, NULL,
		0, NULL,
		barriers.size(), barriers.data());

	std::vector<std::shared_ptr<CVulkanTexture>> refs;
	g_device.submitCommandBuffer( handle, refs );

	// Write all the static NIS descriptor set bindings

	std::array< VkDescriptorImageInfo, 2 > nisImageDescriptors = {{
		{
			.sampler = g_output.nisSampler,
			.imageView = g_output.nisScalerView,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		},
		{
			.sampler = g_output.nisSampler,
			.imageView = g_output.nisUsmView,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		}
	}};

	std::array< VkWriteDescriptorSet, 2 > nisWriteDescriptorSets = {{
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.pNext = nullptr,
			.dstSet = VK_NULL_HANDLE,
			.dstBinding = 4,
			.dstArrayElement = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &nisImageDescriptors[0],
			.pBufferInfo = nullptr,
			.pTexelBufferView = nullptr
		},
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.pNext = nullptr,
			.dstSet = VK_NULL_HANDLE,
			.dstBinding = 5,
			.dstArrayElement = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &nisImageDescriptors[1],
			.pBufferInfo = nullptr,
			.pTexelBufferView = nullptr
		}
	}};

	VkDescriptorSet firstSet = g_device.descriptorSet();
	VkDescriptorSet currentSet = firstSet;
	do
	{
		for ( VkWriteDescriptorSet& descriptorSetWrite : nisWriteDescriptorSets )
		{
			descriptorSetWrite.dstSet = currentSet;
		}

		vkUpdateDescriptorSets(g_device.device(), nisWriteDescriptorSets.size(), nisWriteDescriptorSets.data(), 0, nullptr);
		currentSet = g_device.descriptorSet();
	} while (currentSet != firstSet);

	// Wait for the upload to finish and then cleanup

	vkQueueWaitIdle(g_device.queue());

	vkDestroyBuffer(g_device.device(), stagingBuffer, nullptr);
	vkFreeMemory(g_device.device(), stagingMem, nullptr);

	return true;
}

std::shared_ptr<CVulkanTexture> vulkan_create_texture_from_dmabuf( struct wlr_dmabuf_attributes *pDMA )
{
	std::shared_ptr<CVulkanTexture> pTex = std::make_shared<CVulkanTexture>();

	CVulkanTexture::createFlags texCreateFlags;
	texCreateFlags.bSampled = true;
	
	if ( pTex->BInit( pDMA->width, pDMA->height, pDMA->format, texCreateFlags, pDMA ) == false )
		return nullptr;
	
	return pTex;
}

std::shared_ptr<CVulkanTexture> vulkan_create_texture_from_bits( uint32_t width, uint32_t height, uint32_t contentWidth, uint32_t contentHeight, uint32_t drmFormat, CVulkanTexture::createFlags texCreateFlags, void *bits )
{
	std::shared_ptr<CVulkanTexture> pTex = std::make_shared<CVulkanTexture>();

	texCreateFlags.bSampled = true;
	texCreateFlags.bTransferDst = true;

	if ( pTex->BInit( width, height, drmFormat, texCreateFlags, nullptr,  contentWidth, contentHeight) == false )
		return nullptr;
	
	memcpy( g_device.uploadBufferData(), bits, width * height * 4 );
	
	VkCommandBuffer commandBuffer;
	uint32_t handle = g_device.commandBuffer( commandBuffer );
	
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
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = pTex->vkImage(),
		.subresourceRange = subResRange
	};

	vkCmdPipelineBarrier( commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
				  	    0, 0, nullptr, 0, nullptr, 1, &memoryBarrier );

	vkCmdCopyBufferToImage( commandBuffer, g_device.uploadBuffer(), pTex->vkImage(), VK_IMAGE_LAYOUT_GENERAL, 1, &region );

	memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	memoryBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	memoryBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;

	vkCmdPipelineBarrier( commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				  	    0, 0, nullptr, 0, nullptr, 1, &memoryBarrier );
	
	std::vector<std::shared_ptr<CVulkanTexture>> refs;
	refs.push_back( pTex );
	
	g_device.submitCommandBuffer( handle, refs );
	
	return pTex;
}

bool float_is_integer(float x)
{
	return fabsf(ceilf(x) - x) <= 0.0001f;
}

static uint32_t s_frameId = 0;

VkDescriptorSet vulkan_update_descriptor( const struct FrameInfo_t *frameInfo, bool firstNrm, bool firstSrgb, VkImageView targetImageView, VkImageView extraImageView = VK_NULL_HANDLE)
{
	VkDescriptorSet descriptorSet = g_device.descriptorSet();

	{
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
		
		vkUpdateDescriptorSets(g_device.device(), 1, &writeDescriptorSet, 0, nullptr);
	}
	
	std::array< VkDescriptorImageInfo, k_nMaxLayers > imageDescriptors = {};
	for ( uint32_t i = 0; i < k_nMaxLayers; i++ )
	{
		bool compositeLayer = i > 0;

		VkImageView imageView = frameInfo->layers[i].tex
			? frameInfo->layers[i].tex->view(compositeLayer || !firstSrgb)
			: VK_NULL_HANDLE;

		VulkanSamplerCacheKey_t samplerKey;
		samplerKey.bNearest = !frameInfo->layers[i].linearFilter;
		samplerKey.bUnnormalized = compositeLayer || !firstNrm;

		VkSampler sampler = g_device.sampler(samplerKey);
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
		if ( frameInfo->ycbcrMask() & ( 1u << i ) )
		{
			ycbcrImageDescriptors[i] = imageDescriptors[i];
			// We use immutable samplers.
			ycbcrImageDescriptors[i].sampler = VK_NULL_HANDLE;
			// The ycbcr image might not be usable as a rgb image
			// and the shader doesn't use it anyway.
			imageDescriptors[i].imageView =  VK_NULL_HANDLE;
		}
	}

	std::array< VkWriteDescriptorSet, 3 > writeDescriptorSets;

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

	VkDescriptorImageInfo extraInfo = {
		.sampler = imageDescriptors[0].sampler,
		.imageView = extraImageView,
		.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	};

	writeDescriptorSets[2] = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.pNext = nullptr,
		.dstSet = descriptorSet,
		.dstBinding = 3,
		.dstArrayElement = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &extraInfo,
		.pBufferInfo = nullptr,
		.pTexelBufferView = nullptr,
	};

	vkUpdateDescriptorSets(g_device.device(), writeDescriptorSets.size(), writeDescriptorSets.data(), 0, nullptr);

    return descriptorSet;
}

void vulkan_garbage_collect( void )
{
	g_device.garbageCollect();
}

std::shared_ptr<CVulkanTexture> vulkan_acquire_screenshot_texture(bool exportable)
{
	for (auto& pScreenshotImage : g_output.pScreenshotImages)
	{
		if (pScreenshotImage == nullptr)
		{
			pScreenshotImage = std::make_shared<CVulkanTexture>();

			CVulkanTexture::createFlags screenshotImageFlags;
			screenshotImageFlags.bMappable = true;
			screenshotImageFlags.bTransferDst = true;
			if (exportable) {
				screenshotImageFlags.bExportable = true;
				screenshotImageFlags.bLinear = true; // TODO: support multi-planar DMA-BUF export via PipeWire
			}

			bool bSuccess = pScreenshotImage->BInit( currentOutputWidth, currentOutputHeight, VulkanFormatToDRM(g_output.outputFormat), screenshotImageFlags );

			assert( bSuccess );
		}

		if (pScreenshotImage.use_count() > 1)
			continue;

		return pScreenshotImage;
	}

	return nullptr;
}

struct CompositeData_t
{
	vec2_t scale[k_nMaxLayers];
	vec2_t offset[k_nMaxLayers];
	float opacity[k_nMaxLayers];
	uint32_t borderMask;
	uint32_t frameId;
};

static CompositeData_t pack_composite_data( const struct FrameInfo_t *frameInfo )
{
	CompositeData_t result = {};
	for (int i = 0; i < frameInfo->layerCount; i++) {
		const FrameInfo_t::Layer_t *layer = &frameInfo->layers[i];
		result.scale[i] = layer->scale;
		result.offset[i] = layer->offset;
		result.opacity[i] = layer->opacity;
	}
	result.borderMask = frameInfo->borderMask();
	result.frameId = s_frameId++;
	return result;
}

bool vulkan_composite( struct FrameInfo_t *frameInfo, std::shared_ptr<CVulkanTexture> pScreenshotTexture )
{
	VkImage compositeImage;
	VkImageView targetImageView;

	if ( BIsNested() == true )
	{
		compositeImage = g_output.swapChainImages[ g_output.nSwapChainImageIndex ];
		targetImageView = g_output.swapChainImageViews[ g_output.nSwapChainImageIndex ];
	}
	else
	{
		compositeImage = g_output.outputImage[ g_output.nOutImage ]->vkImage();
		targetImageView = g_output.outputImage[ g_output.nOutImage ]->srgbView();
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

	std::vector<VkImageMemoryBarrier> textureBarriers(frameInfo->layerCount);
	for (int32_t i = 0; i < frameInfo->layerCount; i++)
	{
		textureBarriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		textureBarriers[i].pNext = nullptr;
		textureBarriers[i].srcAccessMask = 0;
		textureBarriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		textureBarriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		textureBarriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
		textureBarriers[i].srcQueueFamilyIndex = g_device.supportsModifiers()
													? VK_QUEUE_FAMILY_FOREIGN_EXT
													: VK_QUEUE_FAMILY_EXTERNAL_KHR;
		textureBarriers[i].dstQueueFamilyIndex = g_device.queueFamily();
		textureBarriers[i].image = frameInfo->layers[i].tex->vkImage();
		textureBarriers[i].subresourceRange = subResRange;
	}

	vkCmdPipelineBarrier( curCommandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				  		0, 0, nullptr, 0, nullptr, textureBarriers.size(), textureBarriers.data() );

	bool isUpscaling = frameInfo->useFSRLayer0 || frameInfo->useNISLayer0;
	for ( int i = isUpscaling ? 1 : 0; i < frameInfo->layerCount; i++ )
	{
		FrameInfo_t::Layer_t *layer = &frameInfo->layers[i];

		bool bForceNearest = layer->scale.x == 1.0f &&
							 layer->scale.y == 1.0f &&
							 float_is_integer(layer->offset.x) &&
							 float_is_integer(layer->offset.y);

		layer->linearFilter &= !bForceNearest;

		layer->offset.x += 0.5f / layer->scale.x;
		layer->offset.y += 0.5f / layer->scale.y;
	}


	if ( frameInfo->useFSRLayer0 )
	{
		struct uvec4_t
		{
			uint32_t  x;
			uint32_t  y;
			uint32_t  z;
			uint32_t  w;
		};
		struct uvec2_t
		{
			uint32_t x;
			uint32_t y;
		};

		struct
		{
			uvec4_t Const0;
			uvec4_t Const1;
			uvec4_t Const2;
			uvec4_t Const3;
		} easuConstants;

		struct
		{
			uvec2_t u_layer0Offset;
			vec2_t u_scale[k_nMaxLayers - 1];
			vec2_t u_offset[k_nMaxLayers - 1];
			float u_opacity[k_nMaxLayers];
			uint32_t u_borderMask;
			uint32_t u_frameId;
			uint32_t u_c1;
		} rcasConstants;

		struct FrameInfo_t fsrFrameInfo = *frameInfo;
		fsrFrameInfo.layers[0].linearFilter = true;

		uint32_t inputX = fsrFrameInfo.layers[0].tex->width();
		uint32_t inputY = fsrFrameInfo.layers[0].tex->height();

		uint32_t tempX = fsrFrameInfo.layers[0].integerWidth();
		uint32_t tempY = fsrFrameInfo.layers[0].integerHeight();

		update_tmp_images(tempX, tempY);

		memoryBarrier.image = g_output.tmpOutput->vkImage();
		vkCmdPipelineBarrier( curCommandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				  		0, 0, nullptr, 0, nullptr, 1, &memoryBarrier );

		vkCmdBindPipeline(curCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, g_device.pipeline(SHADER_TYPE_EASU));

		VkDescriptorSet descriptorSet = vulkan_update_descriptor( &fsrFrameInfo, true, true, g_output.tmpOutput->srgbView() );

		vkCmdBindDescriptorSets(curCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, g_device.pipelineLayout(), 0, 1, &descriptorSet, 0, 0);

		FsrEasuCon(&easuConstants.Const0.x, &easuConstants.Const1.x, &easuConstants.Const2.x, &easuConstants.Const3.x,
			inputX, inputY, inputX, inputY, tempX, tempY);

		vkCmdPushConstants(curCommandBuffer, g_device.pipelineLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(easuConstants), &easuConstants);

		int threadGroupWorkRegionDim = 16;
		int dispatchX = (tempX + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
		int dispatchY = (tempY + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;

		vkCmdDispatch( curCommandBuffer, dispatchX, dispatchY, 1 );

		memoryBarrier =
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = g_output.tmpOutput->vkImage(),
			.subresourceRange = subResRange
		};

		vkCmdPipelineBarrier( curCommandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				  		0, 0, nullptr, 0, nullptr, 1, &memoryBarrier );


		vkCmdBindPipeline(curCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, g_device.pipeline(SHADER_TYPE_RCAS, frameInfo->layerCount, frameInfo->ycbcrMask()));
		fsrFrameInfo.layers[0].tex = g_output.tmpOutput;
		descriptorSet = vulkan_update_descriptor( &fsrFrameInfo, true, true, targetImageView );

		vkCmdBindDescriptorSets(curCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, g_device.pipelineLayout(), 0, 1, &descriptorSet, 0, 0);

		uvec4_t tmp;
		FsrRcasCon(&tmp.x, g_upscalerSharpness / 10.0f);
		rcasConstants.u_layer0Offset.x = uint32_t(int32_t(fsrFrameInfo.layers[0].offset.x));
		rcasConstants.u_layer0Offset.y = uint32_t(int32_t(fsrFrameInfo.layers[0].offset.y));
		rcasConstants.u_opacity[0] = fsrFrameInfo.layers[0].opacity;
		rcasConstants.u_borderMask = fsrFrameInfo.borderMask() >> 1u;
		rcasConstants.u_frameId = s_frameId++;
		rcasConstants.u_c1 = tmp.x;
		for (uint32_t i = 1; i < k_nMaxLayers; i++)
		{
			rcasConstants.u_scale[i - 1] = fsrFrameInfo.layers[i].scale;
			rcasConstants.u_offset[i - 1] = fsrFrameInfo.layers[i].offset;
			rcasConstants.u_opacity[i] = fsrFrameInfo.layers[i].opacity;
		}


		vkCmdPushConstants(curCommandBuffer, g_device.pipelineLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rcasConstants), &rcasConstants);

		dispatchX = (currentOutputWidth + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
		dispatchY = (currentOutputHeight + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;

		vkCmdDispatch( curCommandBuffer, dispatchX, dispatchY, 1 );
	}
	else if ( frameInfo->useNISLayer0 )
	{
		struct FrameInfo_t nisFrameInfo = *frameInfo;

		uint32_t inputX = nisFrameInfo.layers[0].tex->width();
		uint32_t inputY = nisFrameInfo.layers[0].tex->height();

		uint32_t tempX = nisFrameInfo.layers[0].integerWidth();
		uint32_t tempY = nisFrameInfo.layers[0].integerHeight();

		update_tmp_images(tempX, tempY);

		memoryBarrier.image = g_output.tmpOutput->vkImage();
		vkCmdPipelineBarrier( curCommandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				  		0, 0, nullptr, 0, nullptr, 1, &memoryBarrier );

		vkCmdBindPipeline(curCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, g_device.pipeline(SHADER_TYPE_NIS));

		VkDescriptorSet descriptorSet = vulkan_update_descriptor( &nisFrameInfo, true, true, g_output.tmpOutput->srgbView() );

		vkCmdBindDescriptorSets(curCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, g_device.pipelineLayout(), 0, 1, &descriptorSet, 0, 0);

		NISConfig nisConfig = {};
		float nisSharpness = (20 - g_upscalerSharpness) / 20.0f;
		NVScalerUpdateConfig(
			nisConfig, nisSharpness,
			0, 0,
			inputX, inputY,
			inputX, inputY,
			0, 0,
			tempX, tempY,
			tempX, tempY);

		vkCmdPushConstants(curCommandBuffer, g_device.pipelineLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(nisConfig), &nisConfig);

		vkCmdDispatch(curCommandBuffer, uint32_t(std::ceil(tempX / 32.0f)),
			uint32_t(std::ceil(tempY / 24.0f)), 1);

		memoryBarrier =
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = g_output.tmpOutput->vkImage(),
			.subresourceRange = subResRange
		};

		vkCmdPipelineBarrier( curCommandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				  		0, 0, nullptr, 0, nullptr, 1, &memoryBarrier );

		vkCmdBindPipeline(curCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, g_device.pipeline(SHADER_TYPE_BLIT, nisFrameInfo.layerCount, nisFrameInfo.ycbcrMask()));

		nisFrameInfo.layers[0].tex = g_output.tmpOutput;
		descriptorSet = vulkan_update_descriptor( &nisFrameInfo, false, false, targetImageView );

		vkCmdBindDescriptorSets(curCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, g_device.pipelineLayout(), 0, 1, &descriptorSet, 0, 0);

		nisFrameInfo.layers[0].scale.x = 1.0f;
		nisFrameInfo.layers[0].scale.y = 1.0f;

		CompositeData_t data = pack_composite_data(&nisFrameInfo);

		vkCmdPushConstants(curCommandBuffer, g_device.pipelineLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(data), &data);

		uint32_t nGroupCountX = currentOutputWidth % 8 ? currentOutputWidth / 8 + 1: currentOutputWidth / 8;
		uint32_t nGroupCountY = currentOutputHeight % 8 ? currentOutputHeight / 8 + 1: currentOutputHeight / 8;

		vkCmdDispatch( curCommandBuffer, nGroupCountX, nGroupCountY, 1 );
	}
	else if ( frameInfo->blurLayer0 )
	{
		struct FrameInfo_t blurFrameInfo = *frameInfo;
		blurFrameInfo.layers[0].linearFilter = true;

		update_tmp_images(currentOutputWidth, currentOutputHeight);

		memoryBarrier.image = g_output.tmpOutput->vkImage();
		vkCmdPipelineBarrier( curCommandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				  		0, 0, nullptr, 0, nullptr, 1, &memoryBarrier );

		ShaderType type = SHADER_TYPE_BLUR_FIRST_PASS;

		uint32_t blur_layer_count = 0;
		// Also blur the override on top if we have one.
		if (blurFrameInfo.layerCount >= 2 && blurFrameInfo.layers[1].zpos == g_zposOverride)
			blur_layer_count++;

		vkCmdBindPipeline(curCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, g_device.pipeline(type, blur_layer_count + 1, blurFrameInfo.ycbcrMask() & 0x1u, blurFrameInfo.blurRadius));

		VkDescriptorSet descriptorSet = vulkan_update_descriptor( &blurFrameInfo, false, false, g_output.tmpOutput->srgbView() );

		vkCmdBindDescriptorSets(curCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
								g_device.pipelineLayout(), 0, 1, &descriptorSet, 0, 0);

		CompositeData_t data = pack_composite_data(frameInfo);

		vkCmdPushConstants(curCommandBuffer, g_device.pipelineLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(data), &data);

		uint32_t nGroupCountX = currentOutputWidth % 8 ? currentOutputWidth / 8 + 1: currentOutputWidth / 8;
		uint32_t nGroupCountY = currentOutputHeight % 8 ? currentOutputHeight / 8 + 1: currentOutputHeight / 8;

		vkCmdDispatch( curCommandBuffer, nGroupCountX, nGroupCountY, 1 );

		memoryBarrier =
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = g_output.tmpOutput->vkImage(),
			.subresourceRange = subResRange
		};

		vkCmdPipelineBarrier( curCommandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				  		0, 0, nullptr, 0, nullptr, 1, &memoryBarrier );

		descriptorSet = vulkan_update_descriptor( &blurFrameInfo, false, false, targetImageView, g_output.tmpOutput->linearView() );

		vkCmdBindDescriptorSets(curCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, g_device.pipelineLayout(), 0, 1, &descriptorSet, 0, 0);

		type = blurFrameInfo.blurLayer0 == BLUR_MODE_COND ? SHADER_TYPE_BLUR_COND : SHADER_TYPE_BLUR;

		vkCmdBindPipeline(curCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, g_device.pipeline(type, blurFrameInfo.layerCount, blurFrameInfo.ycbcrMask(), blurFrameInfo.blurRadius, blur_layer_count));

		vkCmdPushConstants(curCommandBuffer, g_device.pipelineLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(data), &data);

		nGroupCountX = currentOutputWidth % 8 ? currentOutputWidth / 8 + 1: currentOutputWidth / 8;
		nGroupCountY = currentOutputHeight % 8 ? currentOutputHeight / 8 + 1: currentOutputHeight / 8;

		vkCmdDispatch( curCommandBuffer, nGroupCountX, nGroupCountY, 1 );
	}
	else
	{

		vkCmdBindPipeline(curCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, g_device.pipeline(SHADER_TYPE_BLIT, frameInfo->layerCount, frameInfo->ycbcrMask()));

		VkDescriptorSet descriptorSet = vulkan_update_descriptor( frameInfo, false, false, targetImageView );

		vkCmdBindDescriptorSets(curCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
								g_device.pipelineLayout(), 0, 1, &descriptorSet, 0, 0);

		CompositeData_t data = pack_composite_data(frameInfo);

		vkCmdPushConstants(curCommandBuffer, g_device.pipelineLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(data), &data);

		uint32_t nGroupCountX = currentOutputWidth % 8 ? currentOutputWidth / 8 + 1: currentOutputWidth / 8;
		uint32_t nGroupCountY = currentOutputHeight % 8 ? currentOutputHeight / 8 + 1: currentOutputHeight / 8;

		vkCmdDispatch( curCommandBuffer, nGroupCountX, nGroupCountY, 1 );
	}

	if ( pScreenshotTexture != nullptr )
	{
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
			.image = pScreenshotTexture->vkImage(),
			.subresourceRange = subResRange
		};

		vkCmdPipelineBarrier( curCommandBuffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier );


		memoryBarrier =
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

		vkCmdCopyImage( curCommandBuffer, compositeImage, VK_IMAGE_LAYOUT_GENERAL, pScreenshotTexture->vkImage(), VK_IMAGE_LAYOUT_GENERAL, 1, &region );
	}

	bool useForeignQueue = !BIsNested() && g_device.supportsModifiers();

	memoryBarrier =
	{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = pScreenshotTexture ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = useForeignQueue ? (VkAccessFlagBits)0 : VK_ACCESS_MEMORY_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = BIsNested() ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = g_device.queueFamily(),
		.dstQueueFamilyIndex = useForeignQueue
								? VK_QUEUE_FAMILY_FOREIGN_EXT
						    	: g_device.queueFamily(),
		.image = compositeImage,
		.subresourceRange = subResRange
	};
	
	vkCmdPipelineBarrier( curCommandBuffer, pScreenshotTexture ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				  useForeignQueue ? 0 : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				  0, 0, nullptr, 0, nullptr, 1, &memoryBarrier );

	for (int32_t i = 0; i < frameInfo->layerCount; i++)
	{
		textureBarriers[i].dstAccessMask = 0;
		textureBarriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		textureBarriers[i].srcQueueFamilyIndex = g_device.queueFamily();
		textureBarriers[i].dstQueueFamilyIndex = g_device.supportsModifiers()
													? VK_QUEUE_FAMILY_FOREIGN_EXT
													: VK_QUEUE_FAMILY_EXTERNAL_KHR;
	}

	vkCmdPipelineBarrier( curCommandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				  		0, 0, nullptr, 0, nullptr, textureBarriers.size(), textureBarriers.data() );

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
	
	res = vkQueueSubmit( g_device.queue(), 1, &submitInfo, 0 );
	
	if ( res != VK_SUCCESS )
	{
		return false;
	}
	
	vkQueueWaitIdle( g_device.queue() );

	if ( BIsNested() == false )
	{
		g_output.nOutImage = !g_output.nOutImage;
	}
	
	g_output.nCurCmdBuffer = !g_output.nCurCmdBuffer;
	
	return true;
}

std::shared_ptr<CVulkanTexture> vulkan_get_last_output_image( void )
{
	return g_output.outputImage[ !g_output.nOutImage ];
}

int CVulkanTexture::memoryFence()
{
	const VkMemoryGetFdInfoKHR memory_get_fd_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
		.pNext = NULL,
		.memory = m_vkImageMemory,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	int fence = -1;
	VkResult res = g_device.vk.GetMemoryFdKHR(g_device.device(), &memory_get_fd_info, &fence);
	if ( res != VK_SUCCESS ) {
		fprintf( stderr, "vkGetMemoryFdKHR failed\n" );
	}

	return fence;
}

bool vulkan_primary_dev_id(dev_t *id)
{
	*id = g_device.primaryDevId();
	return g_device.hasDrmPrimaryDevId();
}

bool vulkan_supports_modifiers(void)
{
	return g_device.supportsModifiers();
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
	return g_device.drmRenderFd();
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

	uint32_t width = buf->width;
	uint32_t height = buf->height;

	VkBufferCreateInfo bufferCreateInfo = {};
	bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferCreateInfo.size = stride * height;
	bufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VkBuffer buffer;
	result = vkCreateBuffer( g_device.device(), &bufferCreateInfo, nullptr, &buffer );
	if ( result != VK_SUCCESS )
	{
		wlr_buffer_end_data_ptr_access( buf );
		return 0;
	}

	VkMemoryRequirements memRequirements;
	vkGetBufferMemoryRequirements(g_device.device(), buffer, &memRequirements);

	int memTypeIndex =  g_device.findMemoryType(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, memRequirements.memoryTypeBits );
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
	result = vkAllocateMemory( g_device.device(), &allocInfo, nullptr, &bufferMemory);
	if ( result != VK_SUCCESS )
	{
		wlr_buffer_end_data_ptr_access( buf );
		return 0;
	}

	result = vkBindBufferMemory( g_device.device(), buffer, bufferMemory, 0 );
	if ( result != VK_SUCCESS )
	{
		wlr_buffer_end_data_ptr_access( buf );
		return 0;
	}

	void *dst;
	result = vkMapMemory( g_device.device(), bufferMemory, 0, VK_WHOLE_SIZE, 0, &dst );
	if ( result != VK_SUCCESS )
	{
		wlr_buffer_end_data_ptr_access( buf );
		return 0;
	}

	memcpy( dst, src, stride * height );

	vkUnmapMemory( g_device.device(), bufferMemory );

	wlr_buffer_end_data_ptr_access( buf );

	std::shared_ptr<CVulkanTexture> pTex = std::make_shared<CVulkanTexture>();
	CVulkanTexture::createFlags texCreateFlags = {};
	texCreateFlags.bSampled = true;
	texCreateFlags.bTransferDst = true;
	if ( pTex->BInit( width, height, drmFormat, texCreateFlags ) == false )
		return nullptr;

	VkCommandBuffer commandBuffer;
	uint32_t handle = g_device.commandBuffer( commandBuffer );

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
	vkCmdCopyBufferToImage( commandBuffer, buffer, pTex->vkImage(), VK_IMAGE_LAYOUT_GENERAL, 1, &region );

	std::vector<std::shared_ptr<CVulkanTexture>> refs;
	refs.push_back( pTex );

	g_device.submitCommandBuffer( handle, refs );

	return pTex;
}
