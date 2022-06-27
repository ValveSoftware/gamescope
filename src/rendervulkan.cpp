// Initialize Vulkan and composite stuff with a compute queue

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <algorithm>
#include <array>
#include <bitset>
#include <thread>
#include <vulkan/vulkan_core.h>

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
#include "shaders/descriptor_set_constants.h"

extern "C"
{
VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
	const VkInstanceCreateInfo*                 pCreateInfo,
	const VkAllocationCallbacks*                pAllocator,
	VkInstance*                                 pInstance);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
	VkInstance                                  instance,
	const char*                                 pName);
}


bool g_bIsCompositeDebug = false;

struct VulkanOutput_t
{
	VkSurfaceKHR surface;
	VkSurfaceCapabilitiesKHR surfaceCaps;
	std::vector< VkSurfaceFormatKHR > surfaceFormats;
	std::vector< VkPresentModeKHR > presentModes;


	VkSwapchainKHR swapChain;
	VkFence acquireFence;

	uint32_t nOutImage; // swapchain index in nested mode, or ping/pong between two RTs
	std::vector<std::shared_ptr<CVulkanTexture>> outputImages;

	VkFormat outputFormat;

	std::array<std::shared_ptr<CVulkanTexture>, 8> pScreenshotImages;

	// NIS and FSR
	std::shared_ptr<CVulkanTexture> tmpOutput;

	// NIS
	std::shared_ptr<CVulkanTexture> nisScalerImage;
	std::shared_ptr<CVulkanTexture> nisUsmImage;
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

VulkanOutput_t g_output;

struct SamplerState
{
	bool bNearest : 1;
	bool bUnnormalized : 1;

	SamplerState( void )
	{
		bNearest = false;
		bUnnormalized = false;
	}

	bool operator==( const SamplerState& other ) const
	{
		return this->bNearest == other.bNearest
			&& this->bUnnormalized == other.bUnnormalized;
	}
};

namespace std
{
	template <>
	struct hash<SamplerState>
	{
		size_t operator()( const SamplerState& k ) const
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

static uint32_t div_roundup(uint32_t x, uint32_t y)
{
	return (x + (y - 1)) / y;
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

// DRM doesn't have 32bit floating point formats, so add our own
#define DRM_FORMAT_ABGR32323232F fourcc_code('A', 'B', '8', 'F')

struct {
	uint32_t DRMFormat;
	VkFormat vkFormat;
	VkFormat vkFormatSrgb;
	uint32_t bpp;
	bool bHasAlpha;
	bool internal;
} s_DRMVKFormatTable[] = {
	{ DRM_FORMAT_ARGB8888, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, 4, true, false },
	{ DRM_FORMAT_XRGB8888, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, 4, false, false },
	{ DRM_FORMAT_NV12, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, 0, false, false },
	{ DRM_FORMAT_ABGR16161616F, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, 8, true, true },
	{ DRM_FORMAT_ABGR32323232F, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, 16,true, true },
	{ DRM_FORMAT_INVALID, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, false, true },
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

static inline uint32_t DRMFormatGetBPP( uint32_t nDRMFormat )
{
	for ( int i = 0; s_DRMVKFormatTable[i].vkFormat != VK_FORMAT_UNDEFINED; i++ )
	{
		if ( s_DRMVKFormatTable[i].DRMFormat == nDRMFormat )
		{
			return s_DRMVKFormatTable[i].bpp;
		}
	}

	return false;
}

class CVulkanDevice;

struct TextureState
{
	bool discared : 1;
	bool dirty : 1;
	bool needsPresentLayout : 1;
	bool needsExport : 1;
	bool needsImport : 1;

	TextureState()
	{
		discared = false;
		dirty = false;
		needsPresentLayout = false;
		needsExport = false;
		needsImport = false;
	}
};

class CVulkanCmdBuffer
{
public:
	CVulkanCmdBuffer(CVulkanDevice *parent, VkCommandBuffer cmdBuffer);
	~CVulkanCmdBuffer();
	CVulkanCmdBuffer(const CVulkanCmdBuffer& other) = delete;
	CVulkanCmdBuffer(CVulkanCmdBuffer&& other) = delete;
	CVulkanCmdBuffer& operator=(const CVulkanCmdBuffer& other) = delete;
	CVulkanCmdBuffer& operator=(CVulkanCmdBuffer&& other) = delete;

	inline VkCommandBuffer rawBuffer() {return m_cmdBuffer;}
	void reset();
	void begin();
	void end();
	void bindTexture(uint32_t slot, std::shared_ptr<CVulkanTexture> texture);
	void setTextureSrgb(uint32_t slot, bool srgb);
	void setSamplerNearest(uint32_t slot, bool nearest);
	void setSamplerUnnormalized(uint32_t slot, bool unnormalized);
	void bindTarget(std::shared_ptr<CVulkanTexture> target);
	void clearState();
	template<class PushData, class... Args>
	void pushConstants(Args&&... args);
	void bindPipeline(VkPipeline pipeline);
	void dispatch(uint32_t x, uint32_t y = 1, uint32_t z = 1);
	void copyImage(std::shared_ptr<CVulkanTexture> src, std::shared_ptr<CVulkanTexture> dst);
	void copyBufferToImage(VkBuffer buffer, VkDeviceSize offset, uint32_t stride, std::shared_ptr<CVulkanTexture> dst);


private:
	void prepareSrcImage(CVulkanTexture *image);
	void prepareDestImage(CVulkanTexture *image);
	void markDirty(CVulkanTexture *image);
	void insertBarrier(bool flush = false);

	VkCommandBuffer m_cmdBuffer;
	CVulkanDevice *m_device;

	// Per Use State
	std::unordered_map<CVulkanTexture *, std::shared_ptr<CVulkanTexture>> m_textureRefs;
	std::unordered_map<CVulkanTexture *, TextureState> m_textureState;

	// Draw State
	std::array<CVulkanTexture *, VKR_SAMPLER_SLOTS> m_boundTextures;
	std::bitset<VKR_SAMPLER_SLOTS> m_useSrgb;
	std::array<SamplerState, VKR_SAMPLER_SLOTS> m_samplerState;
	CVulkanTexture *m_target;
};

#define VULKAN_INSTANCE_FUNCTIONS \
	VK_FUNC(CreateDevice) \
	VK_FUNC(EnumerateDeviceExtensionProperties) \
	VK_FUNC(EnumeratePhysicalDevices) \
	VK_FUNC(GetDeviceProcAddr) \
	VK_FUNC(GetPhysicalDeviceFeatures2) \
	VK_FUNC(GetPhysicalDeviceFormatProperties) \
	VK_FUNC(GetPhysicalDeviceFormatProperties2) \
	VK_FUNC(GetPhysicalDeviceImageFormatProperties2) \
	VK_FUNC(GetPhysicalDeviceMemoryProperties) \
	VK_FUNC(GetPhysicalDeviceQueueFamilyProperties) \
	VK_FUNC(GetPhysicalDeviceProperties) \
	VK_FUNC(GetPhysicalDeviceProperties2) \
	VK_FUNC(GetPhysicalDeviceSurfaceCapabilitiesKHR) \
	VK_FUNC(GetPhysicalDeviceSurfaceFormatsKHR) \
	VK_FUNC(GetPhysicalDeviceSurfacePresentModesKHR) \
	VK_FUNC(GetPhysicalDeviceSurfaceSupportKHR)

#define VULKAN_DEVICE_FUNCTIONS \
	VK_FUNC(AllocateCommandBuffers) \
	VK_FUNC(AllocateDescriptorSets) \
	VK_FUNC(AllocateMemory) \
	VK_FUNC(AcquireNextImageKHR) \
	VK_FUNC(BeginCommandBuffer) \
	VK_FUNC(BindBufferMemory) \
	VK_FUNC(BindImageMemory) \
	VK_FUNC(CreateBuffer) \
	VK_FUNC(CreateCommandPool) \
	VK_FUNC(CreateComputePipelines) \
	VK_FUNC(CreateDescriptorPool) \
	VK_FUNC(CreateDescriptorSetLayout) \
	VK_FUNC(CreateFence) \
	VK_FUNC(CreateImage) \
	VK_FUNC(CreateImageView) \
	VK_FUNC(CreatePipelineLayout) \
	VK_FUNC(CreateSampler) \
	VK_FUNC(CreateSamplerYcbcrConversion) \
	VK_FUNC(CreateSemaphore) \
	VK_FUNC(CreateShaderModule) \
	VK_FUNC(CreateSwapchainKHR) \
	VK_FUNC(CmdBindDescriptorSets) \
	VK_FUNC(CmdBindPipeline) \
	VK_FUNC(CmdCopyBufferToImage) \
	VK_FUNC(CmdCopyImage) \
	VK_FUNC(CmdDispatch) \
	VK_FUNC(CmdPipelineBarrier) \
	VK_FUNC(CmdPushConstants) \
	VK_FUNC(DestroyBuffer) \
	VK_FUNC(DestroyImage) \
	VK_FUNC(DestroyImageView) \
	VK_FUNC(DestroyPipeline) \
	VK_FUNC(DestroySwapchainKHR) \
	VK_FUNC(EndCommandBuffer) \
	VK_FUNC(FreeCommandBuffers) \
	VK_FUNC(FreeMemory) \
	VK_FUNC(GetBufferMemoryRequirements) \
	VK_FUNC(GetDeviceQueue) \
	VK_FUNC(GetImageDrmFormatModifierPropertiesEXT) \
	VK_FUNC(GetImageMemoryRequirements) \
	VK_FUNC(GetImageSubresourceLayout) \
	VK_FUNC(GetMemoryFdKHR) \
	VK_FUNC(GetSemaphoreCounterValue) \
	VK_FUNC(GetSwapchainImagesKHR) \
	VK_FUNC(MapMemory) \
	VK_FUNC(QueuePresentKHR) \
	VK_FUNC(QueueSubmit) \
	VK_FUNC(ResetCommandBuffer) \
	VK_FUNC(ResetFences) \
	VK_FUNC(UnmapMemory) \
	VK_FUNC(UpdateDescriptorSets) \
	VK_FUNC(WaitForFences) \
	VK_FUNC(WaitSemaphores)

class CVulkanDevice
{
public:
	bool BInit();

	VkSampler sampler(SamplerState key);
	VkPipeline pipeline(ShaderType type, uint32_t layerCount = 1, uint32_t ycbcrMask = 0, uint32_t radius = 0, uint32_t blur_layers = 0);
	int32_t findMemoryType( VkMemoryPropertyFlags properties, uint32_t requiredTypeBits );
	std::unique_ptr<CVulkanCmdBuffer> commandBuffer();
	uint64_t submit( std::unique_ptr<CVulkanCmdBuffer> cmdBuf);
	void wait(uint64_t sequence);
	void waitIdle();
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
	void resetCmdBuffers(uint64_t sequence);

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

	std::unordered_map< SamplerState, VkSampler > m_samplerCache;
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
	std::vector<std::unique_ptr<CVulkanCmdBuffer>> m_unusedCmdBufs;
	std::map<uint64_t, std::unique_ptr<CVulkanCmdBuffer>> m_pendingCmdBufs;
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
			// Select the device if it's the first one or the preferred one
			if (!m_physDev ||
			    (g_preferVendorID == deviceProperties.vendorID && g_preferDeviceID == deviceProperties.deviceID))
			{
				m_queueFamily = computeOnlyIndex == ~0u ? generalIndex : computeOnlyIndex;
				m_physDev = cphysDev;
			}
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
	vk.EnumerateDeviceExtensionProperties( physDev(), NULL, &supportedExtensionCount, NULL );

	std::vector<VkExtensionProperties> supportedExts(supportedExtensionCount);
	vk.EnumerateDeviceExtensionProperties( physDev(), NULL, &supportedExtensionCount, supportedExts.data() );

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

	VkPhysicalDeviceFeatures2 features2 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		.features = {
			.shaderInt16 = m_bSupportsFp16,
		},
	};

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

	VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrFeatures = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
		.pNext = std::exchange(features2.pNext, &ycbcrFeatures),
		.samplerYcbcrConversion = VK_TRUE,
	};

	VkPhysicalDeviceRobustness2FeaturesEXT robustness2Features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
		.pNext = std::exchange(features2.pNext, &robustness2Features),
		.nullDescriptor = VK_TRUE,
	};

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

static VkSamplerYcbcrModelConversion colorspaceToYCBCRModel( EStreamColorspace colorspace )
{
	switch (colorspace)
	{
		default:
		case k_EStreamColorspace_Unknown:
			return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;

		case k_EStreamColorspace_BT601:
		case k_EStreamColorspace_BT601_Full:
			return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;

		case k_EStreamColorspace_BT709:
		case k_EStreamColorspace_BT709_Full:
			return VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
	}
}

static VkSamplerYcbcrRange colorspaceToYCBCRRange( EStreamColorspace colorspace )
{
	switch (colorspace)
	{
		default:
		case k_EStreamColorspace_Unknown:
			return VK_SAMPLER_YCBCR_RANGE_ITU_FULL;

		case k_EStreamColorspace_BT709:
		case k_EStreamColorspace_BT601:
			return VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;

		case k_EStreamColorspace_BT601_Full:
		case k_EStreamColorspace_BT709_Full:
			return VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
	}
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
		.ycbcrModel = colorspaceToYCBCRModel( g_ForcedNV12ColorSpace ),
		.ycbcrRange = colorspaceToYCBCRRange( g_ForcedNV12ColorSpace ),
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
	std::array<VkSampler, VKR_SAMPLER_SLOTS> ycbcrSamplers;
	for (auto& sampler : ycbcrSamplers)
		sampler = m_ycbcrSampler;

	std::array<VkDescriptorSetLayoutBinding, 3> layoutBindings = {
		VkDescriptorSetLayoutBinding {
			.binding = 0,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		},
		VkDescriptorSetLayoutBinding {
			.binding = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.descriptorCount = VKR_SAMPLER_SLOTS,
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		},
		VkDescriptorSetLayoutBinding {
			.binding = 2,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.descriptorCount = VKR_SAMPLER_SLOTS,
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			.pImmutableSamplers = ycbcrSamplers.data(),
		},
	};

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
			uint32_t(m_descriptorSets.size()) * 2 * VKR_SAMPLER_SLOTS,
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
	
	VkBufferCreateInfo bufferCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = 512 * 512 * 4,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	};

	res = vk.CreateBuffer( device(), &bufferCreateInfo, nullptr, &m_uploadBuffer );
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkCreateBuffer failed" );
		return false;
	}
	
	VkMemoryRequirements memRequirements;
	vk.GetBufferMemoryRequirements(device(), m_uploadBuffer, &memRequirements);
	
	uint32_t memTypeIndex =  findMemoryType(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, memRequirements.memoryTypeBits );
	if ( memTypeIndex == ~0u )
	{
		vk_log.errorf( "findMemoryType failed" );
		return false;
	}
	
	VkMemoryAllocateInfo allocInfo = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = memRequirements.size,
		.memoryTypeIndex = memTypeIndex,
	};
	
	vk.AllocateMemory( device(), &allocInfo, nullptr, &m_uploadBufferMemory);
	
	vk.BindBufferMemory( device(), m_uploadBuffer, m_uploadBufferMemory, 0 );

	res = vk.MapMemory( device(), m_uploadBufferMemory, 0, VK_WHOLE_SIZE, 0, (void**)&m_uploadBufferData );
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkMapMemory failed" );
		return false;
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

VkSampler CVulkanDevice::sampler( SamplerState key )
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
					for (uint32_t blur_layers = 1; blur_layers <= info.blurLayerCount; blur_layers++) {
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

std::unique_ptr<CVulkanCmdBuffer> CVulkanDevice::commandBuffer()
{
	std::unique_ptr<CVulkanCmdBuffer> cmdBuffer;
	if (m_unusedCmdBufs.empty())
	{
		VkCommandBuffer rawCmdBuffer;
		VkCommandBufferAllocateInfo commandBufferAllocateInfo = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = m_commandPool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1
		};

		VkResult res = vk.AllocateCommandBuffers( device(), &commandBufferAllocateInfo, &rawCmdBuffer );
		if ( res != VK_SUCCESS )
		{
			vk_errorf( res, "vkAllocateCommandBuffers failed" );
			return nullptr;
		}

		cmdBuffer = std::make_unique<CVulkanCmdBuffer>(this, rawCmdBuffer);
	}
	else
	{
		cmdBuffer = std::move(m_unusedCmdBufs.back());
		m_unusedCmdBufs.pop_back();
	}

	cmdBuffer->begin();
	return cmdBuffer;
}

uint64_t CVulkanDevice::submit( std::unique_ptr<CVulkanCmdBuffer> cmdBuffer)
{
	cmdBuffer->end();

	// The seq no of the last submission.
	const uint64_t lastSubmissionSeqNo = m_submissionSeqNo++;

	// This is the seq no of the command buffer we are going to submit.
	const uint64_t nextSeqNo = lastSubmissionSeqNo + 1;

	VkTimelineSemaphoreSubmitInfo timelineInfo = {
		.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
		// no need to ensure order of cmd buffer submission, we only have one queue
		.waitSemaphoreValueCount = 0,
		.pWaitSemaphoreValues = nullptr,
		.signalSemaphoreValueCount = 1,
		.pSignalSemaphoreValues = &nextSeqNo,
	};

	VkCommandBuffer rawCmdBuffer = cmdBuffer->rawBuffer();

	VkSubmitInfo submitInfo = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.pNext = &timelineInfo,
		.commandBufferCount = 1,
		.pCommandBuffers = &rawCmdBuffer,
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = &m_scratchTimelineSemaphore,
	};

	VkResult res = vk.QueueSubmit( queue(), 1, &submitInfo, VK_NULL_HANDLE );

	if ( res != VK_SUCCESS )
	{
		assert( 0 );
	}

	m_pendingCmdBufs.emplace(nextSeqNo, std::move(cmdBuffer));

	return nextSeqNo;
}

void CVulkanDevice::garbageCollect( void )
{
	uint64_t currentSeqNo;
	VkResult res = vk.GetSemaphoreCounterValue(device(), m_scratchTimelineSemaphore, &currentSeqNo);
	assert( res == VK_SUCCESS );

	resetCmdBuffers(currentSeqNo);
}

void CVulkanDevice::wait(uint64_t sequence)
{
	VkSemaphoreWaitInfo waitInfo = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
		.semaphoreCount = 1,
		.pSemaphores = &m_scratchTimelineSemaphore,
		.pValues = &sequence,
	} ;

	VkResult res = vk.WaitSemaphores(device(), &waitInfo, ~0ull);
	if (res != VK_SUCCESS)
		assert( 0 );
	resetCmdBuffers(sequence);
}

void CVulkanDevice::waitIdle()
{
	wait(m_submissionSeqNo);
}

void CVulkanDevice::resetCmdBuffers(uint64_t sequence)
{
	auto last = m_pendingCmdBufs.find(sequence);
	if (last == m_pendingCmdBufs.end())
		return;

	for (auto it = m_pendingCmdBufs.begin(); ; it++)
	{
		it->second->reset();
		m_unusedCmdBufs.push_back(std::move(it->second));
		if (it == last)
			break;
	}

	m_pendingCmdBufs.erase(m_pendingCmdBufs.begin(), ++last);
}

CVulkanCmdBuffer::CVulkanCmdBuffer(CVulkanDevice *parent, VkCommandBuffer cmdBuffer)
	: m_cmdBuffer(cmdBuffer), m_device(parent)
{
}

CVulkanCmdBuffer::~CVulkanCmdBuffer()
{
	m_device->vk.FreeCommandBuffers(m_device->device(), m_device->commandPool(), 1, &m_cmdBuffer);
}

void CVulkanCmdBuffer::reset()
{
	VkResult res = m_device->vk.ResetCommandBuffer(m_cmdBuffer, 0);
	assert(res == VK_SUCCESS);
	m_textureRefs.clear();
	m_textureState.clear();
}

void CVulkanCmdBuffer::begin()
{
	VkCommandBufferBeginInfo commandBufferBeginInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
	};

	VkResult res = m_device->vk.BeginCommandBuffer(m_cmdBuffer, &commandBufferBeginInfo);
	assert(res == VK_SUCCESS);

	clearState();
}

void CVulkanCmdBuffer::end()
{
	insertBarrier(true);
	VkResult res = m_device->vk.EndCommandBuffer(m_cmdBuffer);
	assert(res == VK_SUCCESS);
}

void CVulkanCmdBuffer::bindTexture(uint32_t slot, std::shared_ptr<CVulkanTexture> texture)
{
	m_boundTextures[slot] = texture.get();
	if (texture)
		m_textureRefs.emplace(texture.get(), texture);
}

void CVulkanCmdBuffer::setTextureSrgb(uint32_t slot, bool srgb)
{
	m_useSrgb[slot] = srgb;
}

void CVulkanCmdBuffer::setSamplerNearest(uint32_t slot, bool nearest)
{
	m_samplerState[slot].bNearest = nearest;
}

void CVulkanCmdBuffer::setSamplerUnnormalized(uint32_t slot, bool unnormalized)
{
	m_samplerState[slot].bUnnormalized = unnormalized;
}

void CVulkanCmdBuffer::bindTarget(std::shared_ptr<CVulkanTexture> target)
{
	m_target = target.get();
	if (target)
		m_textureRefs.emplace(target.get(), target);
}

void CVulkanCmdBuffer::clearState()
{
	for (auto& texture : m_boundTextures)
		texture = nullptr;

	for (auto& sampler : m_samplerState)
		sampler = {};

	m_target = nullptr;
	m_useSrgb.reset();
}

template<class PushData, class... Args>
void CVulkanCmdBuffer::pushConstants(Args&&... args)
{
	static_assert(sizeof(PushData) <= 128, "Only 128 bytes push constants.");
	PushData data(std::forward<Args>(args)...);
	m_device->vk.CmdPushConstants(m_cmdBuffer, m_device->pipelineLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(data), &data);
}

void CVulkanCmdBuffer::bindPipeline(VkPipeline pipeline)
{
	m_device->vk.CmdBindPipeline(m_cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
}

void CVulkanCmdBuffer::dispatch(uint32_t x, uint32_t y, uint32_t z)
{
	for (auto src : m_boundTextures)
	{
		if (src)
			prepareSrcImage(src);
	}
	assert(m_target != nullptr);
	prepareDestImage(m_target);
	insertBarrier();

	VkDescriptorSet descriptorSet = m_device->descriptorSet();

	std::array<VkWriteDescriptorSet, 3> writeDescriptorSets;
	std::array<VkDescriptorImageInfo, VKR_SAMPLER_SLOTS> imageDescriptors = {};
	std::array<VkDescriptorImageInfo, VKR_SAMPLER_SLOTS> ycbcrImageDescriptors = {};
	VkDescriptorImageInfo targetDescriptor = {
		.imageView = m_target->srgbView(),
		.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	};

	writeDescriptorSets[0] = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = descriptorSet,
		.dstBinding = 0,
		.dstArrayElement = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		.pImageInfo = &targetDescriptor,
	};

	writeDescriptorSets[1] = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = descriptorSet,
		.dstBinding = 1,
		.dstArrayElement = 0,
		.descriptorCount = imageDescriptors.size(),
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = imageDescriptors.data(),
	};

	writeDescriptorSets[2] = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = descriptorSet,
		.dstBinding = 2,
		.dstArrayElement = 0,
		.descriptorCount = ycbcrImageDescriptors.size(),
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = ycbcrImageDescriptors.data(),
	};

	for (uint32_t i = 0; i < VKR_SAMPLER_SLOTS; i++)
	{
		imageDescriptors[i].sampler = m_device->sampler(m_samplerState[i]);
		imageDescriptors[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		ycbcrImageDescriptors[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		if (m_boundTextures[i] == nullptr)
			continue;

		VkImageView view = m_useSrgb[i] ? m_boundTextures[i]->srgbView() : m_boundTextures[i]->linearView();

		if (m_boundTextures[i]->format() == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
			ycbcrImageDescriptors[i].imageView = view;
		else
			imageDescriptors[i].imageView = view;
	}

	m_device->vk.UpdateDescriptorSets(m_device->device(), writeDescriptorSets.size(), writeDescriptorSets.data(), 0, nullptr);

	m_device->vk.CmdBindDescriptorSets(m_cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_device->pipelineLayout(), 0, 1, &descriptorSet, 0, nullptr);

	m_device->vk.CmdDispatch(m_cmdBuffer, x, y, z);

	markDirty(m_target);
}

void CVulkanCmdBuffer::copyImage(std::shared_ptr<CVulkanTexture> src, std::shared_ptr<CVulkanTexture> dst)
{
	assert(src->width() == dst->width());
	assert(src->height() == dst->height());
	m_textureRefs.emplace(src.get(), src);
	m_textureRefs.emplace(dst.get(), dst);
	prepareSrcImage(src.get());
	prepareDestImage(dst.get());
	insertBarrier();

	VkImageCopy region = {
		.srcSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.layerCount = 1
		},
		.dstSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.layerCount = 1
		},
		.extent = {
			.width = src->width(),
			.height = src->height(),
			.depth = 1
		},
	};

	m_device->vk.CmdCopyImage(m_cmdBuffer, src->vkImage(), VK_IMAGE_LAYOUT_GENERAL, dst->vkImage(), VK_IMAGE_LAYOUT_GENERAL, 1, &region);

	markDirty(dst.get());
}

void CVulkanCmdBuffer::copyBufferToImage(VkBuffer buffer, VkDeviceSize offset, uint32_t stride, std::shared_ptr<CVulkanTexture> dst)
{
	m_textureRefs.emplace(dst.get(), dst);
	prepareDestImage(dst.get());
	insertBarrier();
	VkBufferImageCopy region = {
		.bufferOffset = offset,
		.bufferRowLength = stride,
		.imageSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.layerCount = 1,
		},
		.imageExtent = {
			.width = dst->width(),
			.height = dst->height(),
			.depth = 1,
		},
	};

	m_device->vk.CmdCopyBufferToImage(m_cmdBuffer, buffer, dst->vkImage(), VK_IMAGE_LAYOUT_GENERAL, 1, &region);

	markDirty(dst.get());
}

void CVulkanCmdBuffer::prepareSrcImage(CVulkanTexture *image)
{
	auto result = m_textureState.emplace(image, TextureState());
	// no need to reimport if the image didn't change
	if (!result.second)
		return;
	// using the swapchain image as a source without writing to it doesn't make any sense
	assert(image->swapchainImage() == false);
	result.first->second.needsImport = image->externalImage();
	result.first->second.needsExport = image->externalImage();
}

void CVulkanCmdBuffer::prepareDestImage(CVulkanTexture *image)
{
	auto result = m_textureState.emplace(image, TextureState());
	// no need to discard if the image is already image/in the correct layout
	if (!result.second)
		return;
	result.first->second.discared = true;
	result.first->second.needsExport = image->externalImage();
	result.first->second.needsPresentLayout = image->swapchainImage();
}

void CVulkanCmdBuffer::markDirty(CVulkanTexture *image)
{
	auto result = m_textureState.find(image);
	// image should have been prepared already
	assert(result !=  m_textureState.end());
	result->second.dirty = true;
}

void CVulkanCmdBuffer::insertBarrier(bool flush)
{
	std::vector<VkImageMemoryBarrier> barriers;

	uint32_t externalQueue = m_device->supportsModifiers() ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL_KHR;

	VkImageSubresourceRange subResRange =
	{
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.levelCount = 1,
		.layerCount = 1
	};

	for (auto& pair : m_textureState)
	{
		CVulkanTexture *image = pair.first;
		TextureState& state = pair.second;
		assert(!flush || !state.needsImport);

		bool isExport = flush && state.needsExport;
		bool isPresent = flush && state.needsPresentLayout;

		if (!state.discared && !state.dirty && !state.needsImport && !isExport && !isPresent)
			continue;

		const VkAccessFlags write_bits = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
		const VkAccessFlags read_bits = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

		VkImageMemoryBarrier memoryBarrier =
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = state.dirty ? write_bits : 0u,
			.dstAccessMask = flush ? 0u : read_bits | write_bits,
			.oldLayout = (state.discared || state.needsImport) ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
			.newLayout = isPresent ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_GENERAL,
			.srcQueueFamilyIndex = isExport ? m_device->queueFamily() : state.needsImport ? externalQueue : VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = isExport ? externalQueue : state.needsImport ? m_device->queueFamily() : VK_QUEUE_FAMILY_IGNORED,
			.image = image->vkImage(),
			.subresourceRange = subResRange
		};

		barriers.push_back(memoryBarrier);

		state.discared = false;
		state.dirty = false;
		state.needsImport = false;
	}

	// TODO replace VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
	m_device->vk.CmdPipelineBarrier(m_cmdBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
									0, 0, nullptr, 0, nullptr, barriers.size(), barriers.data());
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
	VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifierFormatInfo = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
		.drmFormatModifier = modifier,
		.sharingMode = imageInfo->sharingMode,
	};

	VkPhysicalDeviceExternalImageFormatInfo externalImageFormatInfo = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
		.pNext = &modifierFormatInfo,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};

	VkPhysicalDeviceImageFormatInfo2 imageFormatInfo = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
		.pNext = &externalImageFormatInfo,
		.format = imageInfo->format,
		.type = imageInfo->imageType,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.usage = imageInfo->usage,
		.flags = imageInfo->flags,
	};

	const VkImageFormatListCreateInfo *readonlyList = pNextFind<VkImageFormatListCreateInfo>(imageInfo, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO);
	VkImageFormatListCreateInfo formatList = {};
	if ( readonlyList != nullptr )
	{
		formatList = *readonlyList;
		formatList.pNext = std::exchange(imageFormatInfo.pNext, &formatList);
	}

	VkImageFormatProperties2 imageProps = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
		.pNext = externalFormatProps,
	};

	return g_device.vk.GetPhysicalDeviceImageFormatProperties2(g_device.physDev(), &imageFormatInfo, &imageProps);
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

	m_bExternal = pDMA || flags.bExportable == true;

	// Possible extensions for below
	wsi_image_create_info wsiImageCreateInfo = {};
	VkExternalMemoryImageCreateInfo externalImageCreateInfo = {};
	VkImageDrmFormatModifierExplicitCreateInfoEXT modifierInfo = {};
	VkSubresourceLayout modifierPlaneLayouts[4] = {};
	VkImageDrmFormatModifierListCreateInfoEXT modifierListInfo = {};
	
	VkImageCreateInfo imageInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = DRMFormatToVulkan(drmFormat, false),
		.extent = {
			.width = width,
			.height = height,
			.depth = 1,
		},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = tiling,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};

	assert( imageInfo.format != VK_FORMAT_UNDEFINED );

	std::array<VkFormat, 2> formats = {
		DRMFormatToVulkan(drmFormat, false),
		DRMFormatToVulkan(drmFormat, true),
	};

	VkImageFormatListCreateInfo formatList = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
		.viewFormatCount = (uint32_t)formats.size(),
		.pViewFormats = formats.data(),
	};

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
		VkExternalImageFormatProperties externalImageProperties = {
			.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
		};

		res = getModifierProps( &imageInfo, pDMA->modifier, &externalImageProperties );
		if ( res != VK_SUCCESS && res != VK_ERROR_FORMAT_NOT_SUPPORTED ) {
			vk_errorf( res, "getModifierProps failed" );
			return false;
		}

		if ( res == VK_SUCCESS &&
		     ( externalImageProperties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT ) )
		{
			modifierInfo = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
				.pNext = std::exchange(imageInfo.pNext, &modifierInfo),
				.drmFormatModifier = pDMA->modifier,
				.drmFormatModifierPlaneCount = uint32_t(pDMA->n_planes),
				.pPlaneLayouts = modifierPlaneLayouts,
			};

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

			VkExternalImageFormatProperties externalFormatProps = {
				.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
			};
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

		modifierListInfo = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
			.pNext = std::exchange(imageInfo.pNext, &modifierListInfo),
			.drmFormatModifierCount = uint32_t(modifiers.size()),
			.pDrmFormatModifiers = modifiers.data(),
		};

		imageInfo.tiling = tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	}

	if ( flags.bFlippable == true && tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT )
	{
		// We want to scan-out the image
		wsiImageCreateInfo = {
			.sType = VK_STRUCTURE_TYPE_WSI_IMAGE_CREATE_INFO_MESA,
			.pNext = std::exchange(imageInfo.pNext, &wsiImageCreateInfo),
			.scanout = VK_TRUE,
		};
	}
	
	if ( pDMA != nullptr )
	{
		externalImageCreateInfo = {
			.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
			.pNext = std::exchange(imageInfo.pNext, &externalImageCreateInfo),
			.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		};
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

	res = g_device.vk.CreateImage(g_device.device(), &imageInfo, nullptr, &m_vkImage);
	if (res != VK_SUCCESS) {
		vk_errorf( res, "vkCreateImage failed" );
		return false;
	}
	
	VkMemoryRequirements memRequirements;
	g_device.vk.GetImageMemoryRequirements(g_device.device(), m_vkImage, &memRequirements);
	
	// Possible pNexts
	wsi_memory_allocate_info wsiAllocInfo = {};
	VkImportMemoryFdInfoKHR importMemoryInfo = {};
	VkExportMemoryAllocateInfo memory_export_info = {};
	VkMemoryDedicatedAllocateInfo memory_dedicated_info = {};

	VkMemoryAllocateInfo allocInfo = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = memRequirements.size,
		.memoryTypeIndex = uint32_t(g_device.findMemoryType(properties, memRequirements.memoryTypeBits)),
	};

	if ( flags.bExportable == true || pDMA != nullptr )
	{
		memory_dedicated_info = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
			.pNext = std::exchange(allocInfo.pNext, &memory_dedicated_info),
			.image = m_vkImage,
		};
	}
	
	if ( flags.bExportable == true && pDMA == nullptr )
	{
		// We'll export it to DRM
		memory_export_info = {
			.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
			.pNext = std::exchange(allocInfo.pNext, &memory_export_info),
			.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		};
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
		wsiAllocInfo = {
				.sType = VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA,
				.pNext = std::exchange(allocInfo.pNext, &wsiAllocInfo),
				.implicit_sync = true,
		};

		// Memory already provided by pDMA
		importMemoryInfo = {
				.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
				.pNext = std::exchange(allocInfo.pNext, &importMemoryInfo),
				.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
				.fd = fd,
		};
	}
	
	res = g_device.vk.AllocateMemory( g_device.device(), &allocInfo, nullptr, &m_vkImageMemory );
	if ( res != VK_SUCCESS )
	{
		vk_errorf( res, "vkAllocateMemory failed" );
		return false;
	}
	
	res = g_device.vk.BindImageMemory( g_device.device(), m_vkImage, m_vkImageMemory, 0 );
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
		};
		VkSubresourceLayout image_layout;
		g_device.vk.GetImageSubresourceLayout(g_device.device(), m_vkImage, &image_subresource, &image_layout);

		m_unRowPitch = image_layout.rowPitch;
	}
	
	if ( flags.bExportable == true )
	{
		// We assume we own the memory when doing this right now.
		// We could support the import scenario as well if needed (but we
		// already have a DMA-BUF in that case).
		assert( pDMA == nullptr );

		struct wlr_dmabuf_attributes dmabuf = {
			.width = int(width),
			.height = int(height),
			.format = drmFormat,
		};
		assert( dmabuf.format != DRM_FORMAT_INVALID );

		// TODO: disjoint planes support
		const VkMemoryGetFdInfoKHR memory_get_fd_info = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
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

			VkImageDrmFormatModifierPropertiesEXT imgModifierProps = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
			};

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
				};
				VkSubresourceLayout subresourceLayout = {};
				g_device.vk.GetImageSubresourceLayout( g_device.device(), m_vkImage, &subresource, &subresourceLayout );
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
			};
			VkSubresourceLayout subresourceLayout = {};
			g_device.vk.GetImageSubresourceLayout( g_device.device(), m_vkImage, &subresource, &subresourceLayout );

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
		VkImageViewCreateInfo createInfo = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = m_vkImage,
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = DRMFormatToVulkan(drmFormat, false),
			.components = {
				.r = VK_COMPONENT_SWIZZLE_IDENTITY,
				.g = VK_COMPONENT_SWIZZLE_IDENTITY,
				.b = VK_COMPONENT_SWIZZLE_IDENTITY,
				.a = bHasAlpha ? VK_COMPONENT_SWIZZLE_IDENTITY : VK_COMPONENT_SWIZZLE_ONE,
			},
			.subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1,
				.layerCount = 1,
			},
		};

		res = g_device.vk.CreateImageView(g_device.device(), &createInfo, nullptr, &m_srgbView);
		if ( res != VK_SUCCESS ) {
			vk_errorf( res, "vkCreateImageView failed" );
			return false;
		}

		if ( flags.bSampled )
		{
			VkImageViewUsageCreateInfo viewUsageInfo = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
				.usage = usage & ~VK_IMAGE_USAGE_STORAGE_BIT,
			};
			createInfo.pNext = &viewUsageInfo;
			createInfo.format = DRMFormatToVulkan(drmFormat, true);
			res = g_device.vk.CreateImageView(g_device.device(), &createInfo, nullptr, &m_linearView);
			if ( res != VK_SUCCESS ) {
				vk_errorf( res, "vkCreateImageView failed" );
				return false;
			}
		}
	}

	if ( flags.bMappable )
	{
		res = g_device.vk.MapMemory( g_device.device(), m_vkImageMemory, 0, VK_WHOLE_SIZE, 0, &m_pMappedData );
		if ( res != VK_SUCCESS )
		{
			vk_errorf( res, "vkMapMemory failed" );
			return false;
		}
	}
	
	m_bInitialized = true;
	
	return true;
}

bool CVulkanTexture::BInitFromSwapchain( VkImage image, uint32_t width, uint32_t height, VkFormat format )
{
	m_vkImage = image;
	m_vkImageMemory = VK_NULL_HANDLE;
	m_width = width;
	m_height = height;
	m_format = format;
	m_contentWidth = width;
	m_contentHeight = height;

	VkImageViewCreateInfo createInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = format,
		.components = {
			.r = VK_COMPONENT_SWIZZLE_IDENTITY,
			.g = VK_COMPONENT_SWIZZLE_IDENTITY,
			.b = VK_COMPONENT_SWIZZLE_IDENTITY,
			.a = VK_COMPONENT_SWIZZLE_IDENTITY,
		},
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1,
			.layerCount = 1,
		},
	};

	VkResult res = g_device.vk.CreateImageView(g_device.device(), &createInfo, nullptr, &m_srgbView);
	if ( res != VK_SUCCESS ) {
		vk_errorf( res, "vkCreateImageView failed" );
		return false;
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
		g_device.vk.UnmapMemory( g_device.device(), m_vkImageMemory );
		m_pMappedData = nullptr;
	}

	if ( m_srgbView != VK_NULL_HANDLE )
	{
		g_device.vk.DestroyImageView( g_device.device(), m_srgbView, nullptr );
		m_srgbView = VK_NULL_HANDLE;
	}

	if ( m_linearView != VK_NULL_HANDLE )
	{
		g_device.vk.DestroyImageView( g_device.device(), m_linearView, nullptr );
		m_linearView = VK_NULL_HANDLE;
	}

	if ( m_FBID != 0 )
	{
		drm_drop_fbid( &g_DRM, m_FBID );
		m_FBID = 0;
	}


	if ( m_vkImageMemory != VK_NULL_HANDLE )
	{
		if ( m_vkImage != VK_NULL_HANDLE )
		{
			g_device.vk.DestroyImage( g_device.device(), m_vkImage, nullptr );
			m_vkImage = VK_NULL_HANDLE;
		}

		g_device.vk.FreeMemory( g_device.device(), m_vkImageMemory, nullptr );
		m_vkImageMemory = VK_NULL_HANDLE;
	}

	m_bInitialized = false;
}

int CVulkanTexture::memoryFence()
{
	const VkMemoryGetFdInfoKHR memory_get_fd_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
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


bool vulkan_init_format(VkFormat format, uint32_t drmFormat)
{
	// First, check whether the Vulkan format is supported
	VkPhysicalDeviceImageFormatInfo2 imageFormatInfo = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
		.format = format,
		.type = VK_IMAGE_TYPE_2D,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT,
	};

	VkImageFormatProperties2 imageFormatProps = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
	};

	VkResult res = g_device.vk.GetPhysicalDeviceImageFormatProperties2( g_device.physDev(), &imageFormatInfo, &imageFormatProps );
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
	VkDrmFormatModifierPropertiesListEXT modifierPropList = {
		.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
	};
	VkFormatProperties2 formatProps = {
		.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
		.pNext = &modifierPropList,
	};

	g_device.vk.GetPhysicalDeviceFormatProperties2( g_device.physDev(), format, &formatProps );

	if ( modifierPropList.drmFormatModifierCount == 0 )
	{
		vk_errorf( res, "vkGetPhysicalDeviceFormatProperties2 returned zero modifiers for DRM format 0x%" PRIX32, drmFormat );
		return false;
	}

	std::vector<VkDrmFormatModifierPropertiesEXT> modifierProps(modifierPropList.drmFormatModifierCount);
	modifierPropList.pDrmFormatModifierProperties = modifierProps.data();
	g_device.vk.GetPhysicalDeviceFormatProperties2( g_device.physDev(), format, &formatProps );

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
		if (s_DRMVKFormatTable[i].internal)
			continue;

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
	VkResult res = g_device.vk.AcquireNextImageKHR( g_device.device(), g_output.swapChain, UINT64_MAX, VK_NULL_HANDLE, g_output.acquireFence, &g_output.nOutImage );
	if ( res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR )
		return false;
	if ( g_device.vk.WaitForFences( g_device.device(), 1, &g_output.acquireFence, false, UINT64_MAX ) != VK_SUCCESS )
		return false;
	return g_device.vk.ResetFences( g_device.device(), 1, &g_output.acquireFence ) == VK_SUCCESS;
}

void vulkan_present_to_window( void )
{
	VkPresentInfoKHR presentInfo = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.swapchainCount = 1,
		.pSwapchains = &g_output.swapChain,
		.pImageIndices = &g_output.nOutImage,
	};

	if ( g_device.vk.QueuePresentKHR( g_device.queue(), &presentInfo ) != VK_SUCCESS )
		vulkan_remake_swapchain();
	
	while ( !acquire_next_image() )
		vulkan_remake_swapchain();
}

bool vulkan_make_swapchain( VulkanOutput_t *pOutput )
{
	uint32_t imageCount = pOutput->surfaceCaps.minImageCount + 1;
	uint32_t surfaceFormat = 0;
	uint32_t formatCount = pOutput->surfaceFormats.size();

	for ( surfaceFormat = 0; surfaceFormat < formatCount; surfaceFormat++ )
	{
		if ( pOutput->surfaceFormats[ surfaceFormat ].format == VK_FORMAT_B8G8R8A8_UNORM )
			break;
	}
	
	if ( surfaceFormat == formatCount )
		return false;

	pOutput->outputFormat = pOutput->surfaceFormats[ surfaceFormat ].format;
	
	VkSwapchainCreateInfoKHR createInfo = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = pOutput->surface,
		.minImageCount = imageCount,
		.imageFormat = pOutput->outputFormat,
		.imageColorSpace = pOutput->surfaceFormats[surfaceFormat].colorSpace,
		.imageExtent = {
			.width = g_nOutputWidth,
			.height = g_nOutputHeight,
		},
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.preTransform = pOutput->surfaceCaps.currentTransform,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		.presentMode = VK_PRESENT_MODE_FIFO_KHR,
		.clipped = VK_TRUE,
	};

	if (g_device.vk.CreateSwapchainKHR( g_device.device(), &createInfo, nullptr, &pOutput->swapChain) != VK_SUCCESS ) {
		return false;
	}

	g_device.vk.GetSwapchainImagesKHR( g_device.device(), pOutput->swapChain, &imageCount, nullptr );
	std::vector<VkImage> swapchainImages( imageCount );
	g_device.vk.GetSwapchainImagesKHR( g_device.device(), pOutput->swapChain, &imageCount, swapchainImages.data() );

	pOutput->outputImages.resize(imageCount);

	for ( uint32_t i = 0; i < pOutput->outputImages.size(); i++ )
	{
		pOutput->outputImages[i] = std::make_shared<CVulkanTexture>();

		if ( !pOutput->outputImages[i]->BInitFromSwapchain(swapchainImages[i], g_nOutputWidth, g_nOutputHeight, pOutput->outputFormat))
			return false;
	}

	VkFenceCreateInfo fenceInfo = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
	};

	g_device.vk.CreateFence( g_device.device(), &fenceInfo, nullptr, &pOutput->acquireFence );

	return true;
}

bool vulkan_remake_swapchain( void )
{
	VulkanOutput_t *pOutput = &g_output;
	g_device.waitIdle();

	pOutput->outputImages.clear();

	g_device.vk.DestroySwapchainKHR( g_device.device(), pOutput->swapChain, nullptr );

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

	pOutput->outputImages.resize(2);

	pOutput->outputImages[0] = nullptr;
	pOutput->outputImages[1] = nullptr;

	pOutput->outputImages[0] = std::make_shared<CVulkanTexture>();
	bool bSuccess = pOutput->outputImages[0]->BInit( g_nOutputWidth, g_nOutputHeight, VulkanFormatToDRM(pOutput->outputFormat), outputImageflags );
	if ( bSuccess != true )
	{
		vk_log.errorf( "failed to allocate buffer for KMS" );
		return false;
	}

	pOutput->outputImages[1] = std::make_shared<CVulkanTexture>();
	bSuccess = pOutput->outputImages[1]->BInit( g_nOutputWidth, g_nOutputHeight, VulkanFormatToDRM(pOutput->outputFormat), outputImageflags );
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
	g_device.waitIdle();

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
		g_device.vk.GetPhysicalDeviceSurfaceSupportKHR( g_device.physDev(), g_device.queueFamily(), pOutput->surface, &canPresent );
		if ( !canPresent )
		{
			vk_log.errorf( "physical device queue doesn't support presenting on our surface" );
			return false;
		}

		result = g_device.vk.GetPhysicalDeviceSurfaceCapabilitiesKHR( g_device.physDev(), pOutput->surface, &pOutput->surfaceCaps );
		if ( result != VK_SUCCESS )
		{
			vk_errorf( result, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed" );
			return false;
		}
		
		uint32_t formatCount = 0;
		result = g_device.vk.GetPhysicalDeviceSurfaceFormatsKHR( g_device.physDev(), pOutput->surface, &formatCount, nullptr );
		if ( result != VK_SUCCESS )
		{
			vk_errorf( result, "vkGetPhysicalDeviceSurfaceFormatsKHR failed" );
			return false;
		}
		
		if ( formatCount != 0 ) {
			pOutput->surfaceFormats.resize( formatCount );
			g_device.vk.GetPhysicalDeviceSurfaceFormatsKHR( g_device.physDev(), pOutput->surface, &formatCount, pOutput->surfaceFormats.data() );
			if ( result != VK_SUCCESS )
			{
				vk_errorf( result, "vkGetPhysicalDeviceSurfaceFormatsKHR failed" );
				return false;
			}
		}
		
		uint32_t presentModeCount = false;
		result = g_device.vk.GetPhysicalDeviceSurfacePresentModesKHR(g_device.physDev(), pOutput->surface, &presentModeCount, nullptr );
		if ( result != VK_SUCCESS )
		{
			vk_errorf( result, "vkGetPhysicalDeviceSurfacePresentModesKHR failed" );
			return false;
		}
		
		if ( presentModeCount != 0 ) {
			pOutput->presentModes.resize(presentModeCount);
			result = g_device.vk.GetPhysicalDeviceSurfacePresentModesKHR( g_device.physDev(), pOutput->surface, &presentModeCount, pOutput->presentModes.data() );
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


static bool init_nis_data()
{
	// Create the NIS images
	// Select between the FP16 or FP32 coefficients

	void* coefScaleData = g_device.supportsFp16() ? (void*) coef_scale_fp16 : (void*) coef_scale;

	void* coefUsmData = g_device.supportsFp16() ? (void*) coef_usm_fp16 : (void*) coef_usm;

	uint32_t nisFormat = g_device.supportsFp16() ? DRM_FORMAT_ABGR16161616F : DRM_FORMAT_ABGR32323232F;

	uint32_t width = kFilterSize / 4;
	uint32_t height = kPhaseCount;

	g_output.nisScalerImage = vulkan_create_texture_from_bits( width, height, width, height, nisFormat, {}, coefScaleData );
	g_device.waitIdle();
	g_output.nisUsmImage = vulkan_create_texture_from_bits( width, height, width, height, nisFormat, {}, coefUsmData );
	g_device.waitIdle();

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

	memcpy( g_device.uploadBufferData(), bits, width * height * DRMFormatGetBPP(drmFormat) );

	auto cmdBuffer = g_device.commandBuffer();

	cmdBuffer->copyBufferToImage(g_device.uploadBuffer(), 0, 0, pTex);

	g_device.submit(std::move(cmdBuffer));

	return pTex;
}

bool float_is_integer(float x)
{
	return fabsf(ceilf(x) - x) <= 0.0001f;
}

static uint32_t s_frameId = 0;

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

struct BlitPushData_t
{
	vec2_t scale[k_nMaxLayers];
	vec2_t offset[k_nMaxLayers];
	float opacity[k_nMaxLayers];
	uint32_t borderMask;
	uint32_t frameId;

	explicit BlitPushData_t(const struct FrameInfo_t *frameInfo)
	{
		for (int i = 0; i < frameInfo->layerCount; i++) {
			const FrameInfo_t::Layer_t *layer = &frameInfo->layers[i];
			scale[i] = layer->scale;
			offset[i] = layer->offsetPixelCenter();
			opacity[i] = layer->opacity;
		}
		borderMask = frameInfo->borderMask();
		frameId = s_frameId++;
	}
};

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

struct EasuPushData_t
{
	uvec4_t Const0;
	uvec4_t Const1;
	uvec4_t Const2;
	uvec4_t Const3;

	EasuPushData_t(uint32_t inputX, uint32_t inputY, uint32_t tempX, uint32_t tempY)
	{
		FsrEasuCon(&Const0.x, &Const1.x, &Const2.x, &Const3.x, inputX, inputY, inputX, inputY, tempX, tempY);
	}
};

struct RcasPushData_t
{
	uvec2_t u_layer0Offset;
	vec2_t u_scale[k_nMaxLayers - 1];
	vec2_t u_offset[k_nMaxLayers - 1];
	float u_opacity[k_nMaxLayers];
	uint32_t u_borderMask;
	uint32_t u_frameId;
	uint32_t u_c1;

	RcasPushData_t(const struct FrameInfo_t *frameInfo, float sharpness)
	{
		uvec4_t tmp;
		FsrRcasCon(&tmp.x, sharpness);
		u_layer0Offset.x = uint32_t(int32_t(frameInfo->layers[0].offset.x));
		u_layer0Offset.y = uint32_t(int32_t(frameInfo->layers[0].offset.y));
		u_opacity[0] = frameInfo->layers[0].opacity;
		u_borderMask = frameInfo->borderMask() >> 1u;
		u_frameId = s_frameId++;
		u_c1 = tmp.x;
		for (uint32_t i = 1; i < k_nMaxLayers; i++)
		{
			u_scale[i - 1] = frameInfo->layers[i].scale;
			u_offset[i - 1] = frameInfo->layers[i].offsetPixelCenter();
			u_opacity[i] = frameInfo->layers[i].opacity;
		}
	}
};

struct NisPushData_t
{
	NISConfig nisConfig;

	NisPushData_t(uint32_t inputX, uint32_t inputY, uint32_t tempX, uint32_t tempY, float sharpness)
	{
		NVScalerUpdateConfig(
			nisConfig, sharpness,
			0, 0,
			inputX, inputY,
			inputX, inputY,
			0, 0,
			tempX, tempY,
			tempX, tempY);
	}
};

void bind_all_layers(CVulkanCmdBuffer* cmdBuffer, const struct FrameInfo_t *frameInfo)
{
	for ( int i = 0; i < frameInfo->layerCount; i++ )
	{
		const FrameInfo_t::Layer_t *layer = &frameInfo->layers[i];

		bool bForceNearest = layer->scale.x == 1.0f &&
							 layer->scale.y == 1.0f &&
							 float_is_integer(layer->offset.x) &&
							 float_is_integer(layer->offset.y);

		bool nearest = bForceNearest | !layer->linearFilter;
		cmdBuffer->bindTexture(i, layer->tex);
		cmdBuffer->setTextureSrgb(i, false);
		cmdBuffer->setSamplerNearest(i, nearest);
		cmdBuffer->setSamplerUnnormalized(i, true);
	}
	for (uint32_t i = frameInfo->layerCount; i < VKR_SAMPLER_SLOTS; i++)
	{
		cmdBuffer->bindTexture(i, nullptr);
	}
}

bool vulkan_composite( const struct FrameInfo_t *frameInfo, std::shared_ptr<CVulkanTexture> pScreenshotTexture )
{
	auto compositeImage = g_output.outputImages[ g_output.nOutImage ];

	auto cmdBuffer = g_device.commandBuffer();

	if ( frameInfo->useFSRLayer0 )
	{
		uint32_t inputX = frameInfo->layers[0].tex->width();
		uint32_t inputY = frameInfo->layers[0].tex->height();

		uint32_t tempX = frameInfo->layers[0].integerWidth();
		uint32_t tempY = frameInfo->layers[0].integerHeight();

		update_tmp_images(tempX, tempY);

		cmdBuffer->bindPipeline(g_device.pipeline(SHADER_TYPE_EASU));
		cmdBuffer->bindTarget(g_output.tmpOutput);
		cmdBuffer->bindTexture(0, frameInfo->layers[0].tex);
		cmdBuffer->setTextureSrgb(0, true);
		cmdBuffer->setSamplerUnnormalized(0, false);
		cmdBuffer->setSamplerNearest(0, false);
		cmdBuffer->pushConstants<EasuPushData_t>(inputX, inputY, tempX, tempY);

		int pixelsPerGroup = 16;

		cmdBuffer->dispatch(div_roundup(tempX, pixelsPerGroup), div_roundup(tempY, pixelsPerGroup));

		cmdBuffer->bindPipeline(g_device.pipeline(SHADER_TYPE_RCAS, frameInfo->layerCount, frameInfo->ycbcrMask() & ~1));
		bind_all_layers(cmdBuffer.get(), frameInfo);
		cmdBuffer->bindTexture(0, g_output.tmpOutput);
		cmdBuffer->setTextureSrgb(0, true);
		cmdBuffer->setSamplerUnnormalized(0, false);
		cmdBuffer->setSamplerNearest(0, false);
		cmdBuffer->bindTarget(compositeImage);
		cmdBuffer->pushConstants<RcasPushData_t>(frameInfo, g_upscalerSharpness / 10.0f);

		cmdBuffer->dispatch(div_roundup(currentOutputWidth, pixelsPerGroup), div_roundup(currentOutputHeight, pixelsPerGroup));
	}
	else if ( frameInfo->useNISLayer0 )
	{
		uint32_t inputX = frameInfo->layers[0].tex->width();
		uint32_t inputY = frameInfo->layers[0].tex->height();

		uint32_t tempX = frameInfo->layers[0].integerWidth();
		uint32_t tempY = frameInfo->layers[0].integerHeight();

		update_tmp_images(tempX, tempY);

		float nisSharpness = (20 - g_upscalerSharpness) / 20.0f;

		cmdBuffer->bindPipeline(g_device.pipeline(SHADER_TYPE_NIS));
		cmdBuffer->bindTarget(g_output.tmpOutput);
		cmdBuffer->bindTexture(0, frameInfo->layers[0].tex);
		cmdBuffer->setTextureSrgb(0, true);
		cmdBuffer->setSamplerUnnormalized(0, false);
		cmdBuffer->setSamplerNearest(0, false);
		cmdBuffer->bindTexture(VKR_NIS_COEF_SCALER_SLOT, g_output.nisScalerImage);
		cmdBuffer->setSamplerUnnormalized(VKR_NIS_COEF_SCALER_SLOT, false);
		cmdBuffer->setSamplerNearest(VKR_NIS_COEF_SCALER_SLOT, false);
		cmdBuffer->bindTexture(VKR_NIS_COEF_USM_SLOT, g_output.nisUsmImage);
		cmdBuffer->setSamplerUnnormalized(VKR_NIS_COEF_USM_SLOT, false);
		cmdBuffer->setSamplerNearest(VKR_NIS_COEF_USM_SLOT, false);
		cmdBuffer->pushConstants<NisPushData_t>(inputX, inputY, tempX, tempY, nisSharpness);

		int pixelsPerGroupX = 32;
		int pixelsPerGroupY = 24;

		cmdBuffer->dispatch(div_roundup(tempX, pixelsPerGroupX), div_roundup(tempY, pixelsPerGroupY));

		struct FrameInfo_t nisFrameInfo = *frameInfo;
		nisFrameInfo.layers[0].tex = g_output.tmpOutput;
		nisFrameInfo.layers[0].scale.x = 1.0f;
		nisFrameInfo.layers[0].scale.y = 1.0f;

		cmdBuffer->bindPipeline( g_device.pipeline(SHADER_TYPE_BLIT, nisFrameInfo.layerCount, nisFrameInfo.ycbcrMask()));
		bind_all_layers(cmdBuffer.get(), &nisFrameInfo);
		cmdBuffer->bindTarget(compositeImage);
		cmdBuffer->pushConstants<BlitPushData_t>(&nisFrameInfo);

		int pixelsPerGroup = 8;

		cmdBuffer->dispatch(div_roundup(currentOutputWidth, pixelsPerGroup), div_roundup(currentOutputHeight, pixelsPerGroup));
	}
	else if ( frameInfo->blurLayer0 )
	{
		update_tmp_images(currentOutputWidth, currentOutputHeight);

		ShaderType type = SHADER_TYPE_BLUR_FIRST_PASS;

		uint32_t blur_layer_count = 1;
		// Also blur the override on top if we have one.
		if (frameInfo->layerCount >= 2 && frameInfo->layers[1].zpos == g_zposOverride)
			blur_layer_count++;

		cmdBuffer->bindPipeline(g_device.pipeline(type, blur_layer_count, frameInfo->ycbcrMask() & 0x3u, frameInfo->blurRadius));
		cmdBuffer->bindTarget(g_output.tmpOutput);
		for (uint32_t i = 0; i < blur_layer_count; i++)
		{
			cmdBuffer->bindTexture(i, frameInfo->layers[i].tex);
			cmdBuffer->setTextureSrgb(i, false);
			cmdBuffer->setSamplerUnnormalized(i, true);
			cmdBuffer->setSamplerNearest(i, false);
		}
		cmdBuffer->pushConstants<BlitPushData_t>(frameInfo);

		int pixelsPerGroup = 8;

		cmdBuffer->dispatch(div_roundup(currentOutputWidth, pixelsPerGroup), div_roundup(currentOutputHeight, pixelsPerGroup));

		type = frameInfo->blurLayer0 == BLUR_MODE_COND ? SHADER_TYPE_BLUR_COND : SHADER_TYPE_BLUR;
		cmdBuffer->bindPipeline(g_device.pipeline(type, frameInfo->layerCount, frameInfo->ycbcrMask(), frameInfo->blurRadius, blur_layer_count));
		bind_all_layers(cmdBuffer.get(), frameInfo);
		cmdBuffer->bindTarget(compositeImage);
		cmdBuffer->bindTexture(VKR_BLUR_EXTRA_SLOT, g_output.tmpOutput);
		cmdBuffer->setTextureSrgb(VKR_BLUR_EXTRA_SLOT, false);
		cmdBuffer->setSamplerUnnormalized(VKR_BLUR_EXTRA_SLOT, true);
		cmdBuffer->setSamplerNearest(VKR_BLUR_EXTRA_SLOT, false);

		cmdBuffer->dispatch(div_roundup(currentOutputWidth, pixelsPerGroup), div_roundup(currentOutputHeight, pixelsPerGroup));
	}
	else
	{
		cmdBuffer->bindPipeline( g_device.pipeline(SHADER_TYPE_BLIT, frameInfo->layerCount, frameInfo->ycbcrMask()));
		bind_all_layers(cmdBuffer.get(), frameInfo);
		cmdBuffer->bindTarget(compositeImage);
		cmdBuffer->pushConstants<BlitPushData_t>(frameInfo);

		int pixelsPerGroup = 8;

		cmdBuffer->dispatch(div_roundup(currentOutputWidth, pixelsPerGroup), div_roundup(currentOutputHeight, pixelsPerGroup));
	}

	if ( pScreenshotTexture != nullptr )
	{
		cmdBuffer->copyImage(compositeImage, pScreenshotTexture);
	}

	uint64_t sequence = g_device.submit(std::move(cmdBuffer));
	g_device.wait(sequence);

	if ( BIsNested() == false )
	{
		g_output.nOutImage = !g_output.nOutImage;
	}

	return true;
}

std::shared_ptr<CVulkanTexture> vulkan_get_last_output_image( void )
{
	return g_output.outputImages[ !g_output.nOutImage ];
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

	VkBufferCreateInfo bufferCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = stride * height,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	};
	VkBuffer buffer;
	result = g_device.vk.CreateBuffer( g_device.device(), &bufferCreateInfo, nullptr, &buffer );
	if ( result != VK_SUCCESS )
	{
		wlr_buffer_end_data_ptr_access( buf );
		return 0;
	}

	VkMemoryRequirements memRequirements;
	g_device.vk.GetBufferMemoryRequirements(g_device.device(), buffer, &memRequirements);

	uint32_t memTypeIndex =  g_device.findMemoryType(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, memRequirements.memoryTypeBits );
	if ( memTypeIndex == ~0u )
	{
		wlr_buffer_end_data_ptr_access( buf );
		return 0;
	}

	VkMemoryAllocateInfo allocInfo = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = memRequirements.size,
		.memoryTypeIndex = memTypeIndex,
	};

	VkDeviceMemory bufferMemory;
	result = g_device.vk.AllocateMemory( g_device.device(), &allocInfo, nullptr, &bufferMemory);
	if ( result != VK_SUCCESS )
	{
		wlr_buffer_end_data_ptr_access( buf );
		return 0;
	}

	result = g_device.vk.BindBufferMemory( g_device.device(), buffer, bufferMemory, 0 );
	if ( result != VK_SUCCESS )
	{
		wlr_buffer_end_data_ptr_access( buf );
		return 0;
	}

	void *dst;
	result = g_device.vk.MapMemory( g_device.device(), bufferMemory, 0, VK_WHOLE_SIZE, 0, &dst );
	if ( result != VK_SUCCESS )
	{
		wlr_buffer_end_data_ptr_access( buf );
		return 0;
	}

	memcpy( dst, src, stride * height );

	g_device.vk.UnmapMemory( g_device.device(), bufferMemory );

	wlr_buffer_end_data_ptr_access( buf );

	std::shared_ptr<CVulkanTexture> pTex = std::make_shared<CVulkanTexture>();
	CVulkanTexture::createFlags texCreateFlags;
	texCreateFlags.bSampled = true;
	texCreateFlags.bTransferDst = true;
	if ( pTex->BInit( width, height, drmFormat, texCreateFlags ) == false )
		return nullptr;

	auto cmdBuffer = g_device.commandBuffer();

	cmdBuffer->copyBufferToImage( buffer, 0, stride, pTex);

	uint64_t sequence = g_device.submit(std::move(cmdBuffer));

	g_device.wait(sequence);

	g_device.vk.DestroyBuffer(g_device.device(), buffer, nullptr);
	g_device.vk.FreeMemory(g_device.device(), bufferMemory, nullptr);

	return pTex;
}
