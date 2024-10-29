// Initialize Vulkan and composite stuff with a compute queue

#pragma once

#include <atomic>
#include <stdint.h>
#include <memory>
#include <map>
#include <unordered_map>
#include <array>
#include <bitset>
#include <mutex>
#include <optional>

#include "main.hpp"

#include "gamescope_shared.h"
#include "backend.h"

#include "shaders/descriptor_set_constants.h"

class CVulkanCmdBuffer;

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
#define k_nMaxYcbcrMask_ToPreCompile 3

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

#include <memory>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <wayland-server-core.h>

#include "wlr_begin.hpp"
#include <wlr/render/dmabuf.h>
#include <wlr/render/interface.h>
#include "wlr_end.hpp"

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

inline VkFormat ToSrgbVulkanFormat( VkFormat format )
{
	switch ( format )
	{
		case VK_FORMAT_B8G8R8A8_UNORM:	return VK_FORMAT_B8G8R8A8_SRGB;
		case VK_FORMAT_R8G8B8A8_UNORM:	return VK_FORMAT_R8G8B8A8_SRGB;
		default:						return format;
	}
}

inline VkFormat ToLinearVulkanFormat( VkFormat format )
{
	switch ( format )
	{
		case VK_FORMAT_B8G8R8A8_SRGB:	return VK_FORMAT_B8G8R8A8_UNORM;
		case VK_FORMAT_R8G8B8A8_SRGB:	return VK_FORMAT_R8G8B8A8_UNORM;
		default:						return format;
	}
}

inline GamescopeAppTextureColorspace VkColorSpaceToGamescopeAppTextureColorSpace(VkFormat format, VkColorSpaceKHR colorspace)
{
	switch (colorspace)
	{
		default:
		case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
			// We will use image view conversions for these 8888 formats.
			if (ToSrgbVulkanFormat(format) != ToLinearVulkanFormat(format))
				return GAMESCOPE_APP_TEXTURE_COLORSPACE_LINEAR;
			return GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB;

		case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
			return GAMESCOPE_APP_TEXTURE_COLORSPACE_SCRGB;

		case VK_COLOR_SPACE_HDR10_ST2084_EXT:
			return GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ;
	}
}

class CVulkanTexture : public gamescope::RcObject
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
			bOutputImage = false;
			bColorAttachment = false;
			imageType = VK_IMAGE_TYPE_2D;
		}

		bool bFlippable : 1;
		bool bMappable : 1;
		bool bSampled : 1;
		bool bStorage : 1;
		bool bTransferSrc : 1;
		bool bTransferDst : 1;
		bool bLinear : 1;
		bool bExportable : 1;
		bool bOutputImage : 1;
		bool bColorAttachment : 1;
		VkImageType imageType;
	};

	bool BInit( uint32_t width, uint32_t height, uint32_t depth, uint32_t drmFormat, createFlags flags, wlr_dmabuf_attributes *pDMA = nullptr, uint32_t contentWidth = 0, uint32_t contentHeight = 0, CVulkanTexture *pExistingImageToReuseMemory = nullptr, gamescope::OwningRc<gamescope::IBackendFb> pBackendFb = nullptr );
	bool BInitFromSwapchain( VkImage image, uint32_t width, uint32_t height, VkFormat format );

	uint32_t IncRef();
	uint32_t DecRef();

	bool IsInUse();

	inline VkImageView view( bool linear ) { return linear ? m_linearView : m_srgbView; }
	inline VkImageView linearView() { return m_linearView; }
	inline VkImageView srgbView() { return m_srgbView; }
	inline VkImageView lumaView() { return m_lumaView; }
	inline VkImageView chromaView() { return m_chromaView; }
	inline uint32_t width() { return m_width; }
	inline uint32_t height() { return m_height; }
	inline uint32_t depth() { return m_depth; }
	inline uint32_t contentWidth() {return m_contentWidth; }
	inline uint32_t contentHeight() {return m_contentHeight; }
	inline uint32_t rowPitch() { return m_unRowPitch; }
	inline gamescope::IBackendFb* GetBackendFb() { return m_pBackendFb.get(); }
	inline uint8_t *mappedData() { return m_pMappedData; }
	inline VkFormat format() const { return m_format; }
	inline const struct wlr_dmabuf_attributes& dmabuf() { return m_dmabuf; }
	inline VkImage vkImage() { return m_vkImage; }
	inline bool outputImage() { return m_bOutputImage; }
	inline bool externalImage() { return m_bExternal; }
	inline VkDeviceSize totalSize() const { return m_size; }
	inline uint32_t drmFormat() const { return m_drmFormat; }

	inline uint32_t lumaOffset() const { return m_lumaOffset; }
	inline uint32_t lumaRowPitch() const { return m_lumaPitch; }
	inline uint32_t chromaOffset() const { return m_chromaOffset; }
	inline uint32_t chromaRowPitch() const { return m_chromaPitch; }

	inline EStreamColorspace streamColorspace() const { return m_streamColorspace; }
	inline void setStreamColorspace(EStreamColorspace colorspace) { m_streamColorspace = colorspace; }

	inline bool isYcbcr() const
	{
		return format() == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	}

	int memoryFence();

	CVulkanTexture( void );
	~CVulkanTexture( void );

	uint32_t queueFamily = VK_QUEUE_FAMILY_IGNORED;

private:
	bool m_bInitialized = false;
	bool m_bExternal = false;
	bool m_bOutputImage = false;

	uint32_t m_drmFormat = DRM_FORMAT_INVALID;

	VkImage m_vkImage = VK_NULL_HANDLE;
	VkDeviceMemory m_vkImageMemory = VK_NULL_HANDLE;
	
	VkImageView m_srgbView = VK_NULL_HANDLE;
	VkImageView m_linearView = VK_NULL_HANDLE;

	VkImageView m_lumaView = VK_NULL_HANDLE;
	VkImageView m_chromaView = VK_NULL_HANDLE;

	uint32_t m_width = 0;
	uint32_t m_height = 0;
	uint32_t m_depth = 0;

	uint32_t m_contentWidth = 0;
	uint32_t m_contentHeight = 0;

	uint32_t m_unRowPitch = 0;
	VkDeviceSize m_size = 0;

	uint32_t m_lumaOffset = 0;
	uint32_t m_lumaPitch = 0;
	uint32_t m_chromaOffset = 0;
	uint32_t m_chromaPitch = 0;
	
	// If this texture owns the backend Fb (ie. it's an internal texture)
	gamescope::OwningRc<gamescope::IBackendFb> m_pBackendFb;

	uint8_t *m_pMappedData = nullptr;

	VkFormat m_format = VK_FORMAT_UNDEFINED;

	EStreamColorspace m_streamColorspace = k_EStreamColorspace_Unknown;

	struct wlr_dmabuf_attributes m_dmabuf = {};
};

struct vec2_t
{
	float x, y;
};

static inline bool float_is_integer(float x)
{
	return fabsf(ceilf(x) - x) <= 0.001f;
}

inline bool close_enough(float a, float b, float epsilon = 0.001f)
{
	return fabsf(a - b) <= epsilon;
}

bool DRMFormatHasAlpha( uint32_t nDRMFormat );

struct FrameInfo_t
{
	bool useFSRLayer0;
	bool useNISLayer0;
	bool bFadingOut;
	BlurMode blurLayer0;
	int blurRadius;

	gamescope::Rc<CVulkanTexture> shaperLut[EOTF_Count];
	gamescope::Rc<CVulkanTexture> lut3D[EOTF_Count];

	bool allowVRR;
	bool applyOutputColorMgmt; // drm only
	EOTF outputEncodingEOTF;

	int layerCount;
	struct Layer_t
	{
		gamescope::Rc<CVulkanTexture> tex;
		int zpos;

		vec2_t offset;
		vec2_t scale;

		float opacity;

		GamescopeUpscaleFilter filter = GamescopeUpscaleFilter::LINEAR;

		bool blackBorder;
		bool applyColorMgmt; // drm only

		std::shared_ptr<gamescope::BackendBlob> ctm;

		GamescopeAppTextureColorspace colorspace;

		bool isYcbcr() const
		{
			if ( !tex )
				return false;

			return tex->isYcbcr();
		}

		bool hasAlpha() const
		{
			if ( !tex )
				return false;

			return DRMFormatHasAlpha( tex->drmFormat() );
		}

		bool isScreenSize() const {
			return close_enough(scale.x, 1.0f) &&
			       close_enough(scale.y, 1.0f) &&
				float_is_integer(offset.x) &&
				float_is_integer(offset.y);
		}

		bool viewConvertsToLinearAutomatically() const {
			return colorspace == GAMESCOPE_APP_TEXTURE_COLORSPACE_LINEAR ||
				colorspace == GAMESCOPE_APP_TEXTURE_COLORSPACE_SCRGB ||
				colorspace == GAMESCOPE_APP_TEXTURE_COLORSPACE_PASSTHRU;
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
	uint32_t colorspaceMask() const {
		uint32_t result = 0;
		for (int i = 0; i < layerCount; i++)
		{
			result |= layers[ i ].colorspace << (i * GamescopeAppTextureColorspace_Bits);
		}
		return result;
	}
};

extern uint32_t g_uCompositeDebug;
extern gamescope::ConVar<uint32_t> cv_composite_debug;

namespace CompositeDebugFlag
{
	static constexpr uint32_t Markers = 1u << 0;
	static constexpr uint32_t PlaneBorders = 1u << 1;
	static constexpr uint32_t Heatmap = 1u << 2;
	static constexpr uint32_t Heatmap_MSWCG = 1u << 3;
	static constexpr uint32_t Heatmap_Hard = 1u << 4;
	static constexpr uint32_t Markers_Partial = 1u << 5;
	static constexpr uint32_t Tonemap_Reinhard = 1u << 7;
};

VkInstance vulkan_get_instance(void);
bool vulkan_init(VkInstance instance, VkSurfaceKHR surface);
bool vulkan_init_formats(void);
bool vulkan_make_output();

gamescope::OwningRc<CVulkanTexture> vulkan_create_texture_from_dmabuf( struct wlr_dmabuf_attributes *pDMA, gamescope::OwningRc<gamescope::IBackendFb> pBackendFb );
gamescope::OwningRc<CVulkanTexture> vulkan_create_texture_from_bits( uint32_t width, uint32_t height, uint32_t contentWidth, uint32_t contentHeight, uint32_t drmFormat, CVulkanTexture::createFlags texCreateFlags, void *bits );
gamescope::OwningRc<CVulkanTexture> vulkan_create_texture_from_wlr_buffer( struct wlr_buffer *buf, gamescope::OwningRc<gamescope::IBackendFb> pBackendFb );

std::optional<uint64_t> vulkan_composite( struct FrameInfo_t *frameInfo, gamescope::Rc<CVulkanTexture> pScreenshotTexture, bool partial, gamescope::Rc<CVulkanTexture> pOutputOverride = nullptr, bool increment = true, std::unique_ptr<CVulkanCmdBuffer> pInCommandBuffer = nullptr );
void vulkan_wait( uint64_t ulSeqNo, bool bReset );
gamescope::Rc<CVulkanTexture> vulkan_get_last_output_image( bool partial, bool defer );
gamescope::Rc<CVulkanTexture> vulkan_acquire_screenshot_texture(uint32_t width, uint32_t height, bool exportable, uint32_t drmFormat, EStreamColorspace colorspace = k_EStreamColorspace_Unknown);

void vulkan_present_to_window( void );

void vulkan_garbage_collect( void );
bool vulkan_remake_swapchain( void );
bool vulkan_remake_output_images( void );
bool acquire_next_image( void );

bool vulkan_primary_dev_id(dev_t *id);
bool vulkan_supports_modifiers(void);

gamescope::Rc<CVulkanTexture> vulkan_create_1d_lut(uint32_t size);
gamescope::Rc<CVulkanTexture> vulkan_create_3d_lut(uint32_t width, uint32_t height, uint32_t depth);
void vulkan_update_luts(const gamescope::Rc<CVulkanTexture>& lut1d, const gamescope::Rc<CVulkanTexture>& lut3d, void* lut1d_data, void* lut3d_data);

gamescope::Rc<CVulkanTexture> vulkan_get_hacky_blank_texture();

std::optional<uint64_t> vulkan_screenshot( const struct FrameInfo_t *frameInfo, gamescope::Rc<CVulkanTexture> pScreenshotTexture, gamescope::Rc<CVulkanTexture> pYUVOutTexture );

struct wlr_renderer *vulkan_renderer_create( void );

using mat3x4 = std::array<std::array<float, 4>, 3>;

#include "color_helpers_impl.h"

struct gamescope_color_mgmt_t
{
	bool enabled;
	uint32_t externalDirtyCtr;
	nightmode_t nightmode;
	float sdrGamutWideness = -1; // user property to widen gamut
	float flInternalDisplayBrightness = 500.f;
	float flSDROnHDRBrightness = 203.f;
	float flHDRInputGain = 1.f;
	float flSDRInputGain = 1.f;

	// HDR Display Metadata Override & Tonemapping
	ETonemapOperator hdrTonemapOperator = ETonemapOperator_None;
	tonemap_info_t hdrTonemapDisplayMetadata = { 0 };
	tonemap_info_t hdrTonemapSourceMetadata = { 0 };

	// the native colorimetry capabilities of the display
	displaycolorimetry_t displayColorimetry;
	EOTF displayEOTF;

	// the output encoding colorimetry
	// ie. for HDR displays we send an explicit 2020 colorimetry packet.
	// on SDR displays this is the same as displayColorimetry.
	displaycolorimetry_t outputEncodingColorimetry;
	EOTF outputEncodingEOTF;

	// If non-zero, use this as the emulated "virtual" white point for the output
	glm::vec2 outputVirtualWhite = { 0.f, 0.f };
	EChromaticAdaptationMethod chromaticAdaptationMode = k_EChromaticAdapatationMethod_Bradford;

	std::shared_ptr<gamescope::BackendBlob> appHDRMetadata;

	bool operator == (const gamescope_color_mgmt_t&) const = default;
	bool operator != (const gamescope_color_mgmt_t&) const = default;
};

//namespace members from "color_helpers_impl.h":
using rendervulkan::s_nLutEdgeSize3d;
using rendervulkan::s_nLutSize1d;

struct gamescope_color_mgmt_luts
{
	bool bHasLut3D = false;
	bool bHasLut1D = false;
	uint16_t lut3d[s_nLutEdgeSize3d*s_nLutEdgeSize3d*s_nLutEdgeSize3d*4];
	uint16_t lut1d[s_nLutSize1d*4];

	gamescope::Rc<CVulkanTexture> vk_lut3d;
	gamescope::Rc<CVulkanTexture> vk_lut1d;

	bool HasLuts() const
	{
		return bHasLut3D && bHasLut1D;
	}

	void reset()
	{
		bHasLut1D = false;
		bHasLut3D = false;
	}
};

struct gamescope_color_mgmt_tracker_t
{
	gamescope_color_mgmt_t pending{};
	gamescope_color_mgmt_t current{};
	uint32_t serial{};
};

extern gamescope_color_mgmt_tracker_t g_ColorMgmt;
extern gamescope_color_mgmt_luts g_ColorMgmtLuts[ EOTF_Count ];

struct VulkanOutput_t
{
	VkSurfaceKHR surface;
	VkSurfaceCapabilitiesKHR surfaceCaps;
	std::vector< VkSurfaceFormatKHR > surfaceFormats;
	std::vector< VkPresentModeKHR > presentModes;


	std::shared_ptr<gamescope::BackendBlob> swapchainHDRMetadata;
	VkSwapchainKHR swapChain;
	VkFence acquireFence;

	uint32_t nOutImage; // swapchain index in nested mode, or ping/pong between two RTs
	std::vector<gamescope::OwningRc<CVulkanTexture>> outputImages;
	std::vector<gamescope::OwningRc<CVulkanTexture>> outputImagesPartialOverlay;
	gamescope::OwningRc<CVulkanTexture> temporaryHackyBlankImage;

	uint32_t uOutputFormat = DRM_FORMAT_INVALID;
	uint32_t uOutputFormatOverlay = DRM_FORMAT_INVALID;

	std::array<gamescope::OwningRc<CVulkanTexture>, 2> pScreenshotImages;

	// NIS and FSR
	gamescope::OwningRc<CVulkanTexture> tmpOutput;

	// NIS
	gamescope::OwningRc<CVulkanTexture> nisScalerImage;
	gamescope::OwningRc<CVulkanTexture> nisUsmImage;
};


enum ShaderType {
	SHADER_TYPE_BLIT = 0,
	SHADER_TYPE_BLUR,
	SHADER_TYPE_BLUR_COND,
	SHADER_TYPE_BLUR_FIRST_PASS,
	SHADER_TYPE_EASU,
	SHADER_TYPE_RCAS,
	SHADER_TYPE_NIS,
	SHADER_TYPE_RGB_TO_NV12,

	SHADER_TYPE_COUNT
};

extern VulkanOutput_t g_output;

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
	uint32_t blurLayerCount;

	uint32_t compositeDebug;

	uint32_t colorspaceMask;
	uint32_t outputEOTF;
	bool itmEnable;

	bool operator==(const PipelineInfo_t& o) const {
		return
		shaderType == o.shaderType &&
		layerCount == o.layerCount &&
		ycbcrMask == o.ycbcrMask &&
		blurLayerCount == o.blurLayerCount &&
		compositeDebug == o.compositeDebug &&
		colorspaceMask == o.colorspaceMask &&
		outputEOTF == o.outputEOTF &&
		itmEnable == o.itmEnable;
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
			hash = hash_combine(hash, k.blurLayerCount);
			hash = hash_combine(hash, k.compositeDebug);
			hash = hash_combine(hash, k.colorspaceMask);
			hash = hash_combine(hash, k.outputEOTF);
			hash = hash_combine(hash, k.itmEnable);
			return hash;
		}
	};
}

static inline uint32_t div_roundup(uint32_t x, uint32_t y)
{
	return (x + (y - 1)) / y;
}

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
	VK_FUNC(AcquireNextImageKHR) \
	VK_FUNC(AllocateCommandBuffers) \
	VK_FUNC(AllocateDescriptorSets) \
	VK_FUNC(AllocateMemory) \
	VK_FUNC(BeginCommandBuffer) \
	VK_FUNC(BindBufferMemory) \
	VK_FUNC(BindImageMemory) \
	VK_FUNC(CmdBeginRendering) \
	VK_FUNC(CmdBindDescriptorSets) \
	VK_FUNC(CmdBindPipeline) \
	VK_FUNC(CmdClearColorImage) \
	VK_FUNC(CmdCopyBufferToImage) \
	VK_FUNC(CmdCopyImage) \
	VK_FUNC(CmdDispatch) \
	VK_FUNC(CmdDraw) \
	VK_FUNC(CmdEndRendering) \
	VK_FUNC(CmdPipelineBarrier) \
	VK_FUNC(CmdPushConstants) \
	VK_FUNC(CreateBuffer) \
	VK_FUNC(CreateCommandPool) \
	VK_FUNC(CreateComputePipelines) \
	VK_FUNC(CreateDescriptorPool) \
	VK_FUNC(CreateDescriptorSetLayout) \
	VK_FUNC(CreateFence) \
	VK_FUNC(CreateGraphicsPipelines) \
	VK_FUNC(CreateImage) \
	VK_FUNC(CreateImageView) \
	VK_FUNC(CreatePipelineLayout) \
	VK_FUNC(CreateSampler) \
	VK_FUNC(CreateSamplerYcbcrConversion) \
	VK_FUNC(CreateSemaphore) \
	VK_FUNC(GetSemaphoreFdKHR) \
	VK_FUNC(ImportSemaphoreFdKHR) \
	VK_FUNC(CreateShaderModule) \
	VK_FUNC(CreateSwapchainKHR) \
	VK_FUNC(DestroyBuffer) \
	VK_FUNC(DestroyDescriptorPool) \
	VK_FUNC(DestroyDescriptorSetLayout) \
	VK_FUNC(DestroyImage) \
	VK_FUNC(DestroyImageView) \
	VK_FUNC(DestroyPipeline) \
	VK_FUNC(DestroySemaphore) \
	VK_FUNC(DestroyPipelineLayout) \
	VK_FUNC(DestroySampler) \
	VK_FUNC(DestroySwapchainKHR) \
	VK_FUNC(EndCommandBuffer) \
	VK_FUNC(FreeCommandBuffers) \
	VK_FUNC(FreeDescriptorSets) \
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
	VK_FUNC(QueueWaitIdle) \
	VK_FUNC(ResetCommandBuffer) \
	VK_FUNC(ResetFences) \
	VK_FUNC(UnmapMemory) \
	VK_FUNC(UpdateDescriptorSets) \
	VK_FUNC(WaitForFences) \
	VK_FUNC(WaitForPresentKHR) \
	VK_FUNC(WaitSemaphores) \
	VK_FUNC(SetHdrMetadataEXT)

template<typename T, typename U = T>
constexpr T align(T what, U to) {
return (what + to - 1) & ~(to - 1);
}

class CVulkanDevice;

struct VulkanTimelineSemaphore_t
{
	~VulkanTimelineSemaphore_t();

	CVulkanDevice *pDevice = nullptr;
	VkSemaphore pVkSemaphore = VK_NULL_HANDLE;

	int GetFd() const;
};

struct VulkanTimelinePoint_t
{
	std::shared_ptr<VulkanTimelineSemaphore_t> pTimelineSemaphore;
	uint64_t ulPoint;
};

class CVulkanDevice
{
public:
	bool BInit(VkInstance instance, VkSurfaceKHR surface);

	VkSampler sampler(SamplerState key);
	VkPipeline pipeline(ShaderType type, uint32_t layerCount = 1, uint32_t ycbcrMask = 0, uint32_t blur_layers = 0, uint32_t colorspace_mask = 0, uint32_t output_eotf = EOTF_Gamma22, bool itm_enable = false);
	int32_t findMemoryType( VkMemoryPropertyFlags properties, uint32_t requiredTypeBits );
	std::unique_ptr<CVulkanCmdBuffer> commandBuffer();
	uint64_t submit( std::unique_ptr<CVulkanCmdBuffer> cmdBuf);
	uint64_t submitInternal( CVulkanCmdBuffer* cmdBuf );
	void wait(uint64_t sequence, bool reset = true);
	void waitIdle(bool reset = true);
	void garbageCollect();
	inline VkDescriptorSet descriptorSet()
	{
		VkDescriptorSet ret = m_descriptorSets[m_currentDescriptorSet];
		m_currentDescriptorSet = (m_currentDescriptorSet + 1) % m_descriptorSets.size();
		return ret;
	}

	std::shared_ptr<VulkanTimelineSemaphore_t> CreateTimelineSemaphore( uint64_t ulStartingPoint, bool bShared = false );
	std::shared_ptr<VulkanTimelineSemaphore_t> ImportTimelineSemaphore( gamescope::CTimeline *pTimeline );

	static const uint32_t upload_buffer_size = 1920 * 1080 * 4;

	inline VkDevice device() { return m_device; }
	inline VkPhysicalDevice physDev() {return m_physDev; }
	inline VkInstance instance() { return m_instance; }
	inline VkQueue queue() {return m_queue;}
	inline VkQueue generalQueue() {return m_generalQueue;}
	inline VkCommandPool commandPool() {return m_commandPool;}
	inline VkCommandPool generalCommandPool() {return m_generalCommandPool;}
	inline uint32_t queueFamily() {return m_queueFamily;}
	inline uint32_t generalQueueFamily() {return m_generalQueueFamily;}
	inline VkBuffer uploadBuffer() {return m_uploadBuffer;}
	inline VkPipelineLayout pipelineLayout() {return m_pipelineLayout;}
	inline int drmRenderFd() {return m_drmRendererFd;}
	inline bool supportsModifiers() {return m_bSupportsModifiers;}
	inline bool hasDrmPrimaryDevId() {return m_bHasDrmPrimaryDevId;}
	inline dev_t primaryDevId() {return m_drmPrimaryDevId;}
	inline bool supportsFp16() {return m_bSupportsFp16;}

	inline void *uploadBufferData(uint32_t size)
	{
		assert(size <= upload_buffer_size);

		m_uploadBufferOffset = align(m_uploadBufferOffset, 16);
		if (m_uploadBufferOffset + size > upload_buffer_size)
		{
			fprintf(stderr, "Exceeded uploadBufferData\n");
			waitIdle(false);
		}

		uint8_t *ptr = ((uint8_t*)m_uploadBufferData) + m_uploadBufferOffset;
		m_uploadBufferOffset += size;
		return ptr;
	}

	#define VK_FUNC(x) PFN_vk##x x = nullptr;
	struct
	{
		VULKAN_INSTANCE_FUNCTIONS
		VULKAN_DEVICE_FUNCTIONS
	} vk;
	#undef VK_FUNC

	void resetCmdBuffers(uint64_t sequence);

protected:
	friend class CVulkanCmdBuffer;

	bool selectPhysDev(VkSurfaceKHR surface);
	bool createDevice();
	bool createLayouts();
	bool createPools();
	bool createShaders();
	bool createScratchResources();
	VkPipeline compilePipeline(uint32_t layerCount, uint32_t ycbcrMask, ShaderType type, uint32_t blur_layer_count, uint32_t composite_debug, uint32_t colorspace_mask, uint32_t output_eotf, bool itm_enable);
	void compileAllPipelines();

	VkDevice m_device = nullptr;
	VkPhysicalDevice m_physDev = nullptr;
	VkInstance m_instance = nullptr;
	VkQueue m_queue = nullptr;
	VkQueue m_generalQueue = nullptr;
	VkSamplerYcbcrConversion m_ycbcrConversion = VK_NULL_HANDLE;
	VkSampler m_ycbcrSampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
	VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
	VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
	VkCommandPool m_commandPool = VK_NULL_HANDLE;
	VkCommandPool m_generalCommandPool = VK_NULL_HANDLE;

	uint32_t m_queueFamily = -1;
	uint32_t m_generalQueueFamily = -1;

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
	uint32_t m_uploadBufferOffset = 0;

	VkSemaphore m_scratchTimelineSemaphore;
	std::atomic<uint64_t> m_submissionSeqNo = { 0 };
	std::vector<std::unique_ptr<CVulkanCmdBuffer>> m_unusedCmdBufs;
	std::map<uint64_t, std::unique_ptr<CVulkanCmdBuffer>> m_pendingCmdBufs;
};

struct TextureState
{
	bool discarded : 1;
	bool dirty : 1;
	bool needsPresentLayout : 1;
	bool needsExport : 1;
	bool needsImport : 1;

	TextureState()
	{
		discarded = false;
		dirty = false;
		needsPresentLayout = false;
		needsExport = false;
		needsImport = false;
	}
};

class CVulkanCmdBuffer
{
public:
	CVulkanCmdBuffer(CVulkanDevice *parent, VkCommandBuffer cmdBuffer, VkQueue queue, uint32_t queueFamily);
	~CVulkanCmdBuffer();
	CVulkanCmdBuffer(const CVulkanCmdBuffer& other) = delete;
	CVulkanCmdBuffer(CVulkanCmdBuffer&& other) = delete;
	CVulkanCmdBuffer& operator=(const CVulkanCmdBuffer& other) = delete;
	CVulkanCmdBuffer& operator=(CVulkanCmdBuffer&& other) = delete;

	inline VkCommandBuffer rawBuffer() {return m_cmdBuffer;}
	void reset();
	void begin();
	void end();
	void bindTexture(uint32_t slot, gamescope::Rc<CVulkanTexture> texture);
	void bindColorMgmtLuts(uint32_t slot, gamescope::Rc<CVulkanTexture> lut1d, gamescope::Rc<CVulkanTexture> lut3d);
	void setTextureStorage(bool storage);
	void setTextureSrgb(uint32_t slot, bool srgb);
	void setSamplerNearest(uint32_t slot, bool nearest);
	void setSamplerUnnormalized(uint32_t slot, bool unnormalized);
	void bindTarget(gamescope::Rc<CVulkanTexture> target);
	void clearState();
	template<class PushData, class... Args>
	void uploadConstants(Args&&... args);
	void bindPipeline(VkPipeline pipeline);
	void dispatch(uint32_t x, uint32_t y = 1, uint32_t z = 1);
	void copyImage(gamescope::Rc<CVulkanTexture> src, gamescope::Rc<CVulkanTexture> dst);
	void copyBufferToImage(VkBuffer buffer, VkDeviceSize offset, uint32_t stride, gamescope::Rc<CVulkanTexture> dst);


	void prepareSrcImage(CVulkanTexture *image);
	void prepareDestImage(CVulkanTexture *image);
	void discardImage(CVulkanTexture *image);
	void markDirty(CVulkanTexture *image);
	void insertBarrier(bool flush = false);

	VkQueue queue() { return m_queue; }
	uint32_t queueFamily() { return m_queueFamily; }

	void AddDependency( std::shared_ptr<VulkanTimelineSemaphore_t> pTimelineSemaphore, uint64_t ulPoint );
	void AddSignal( std::shared_ptr<VulkanTimelineSemaphore_t> pTimelineSemaphore, uint64_t ulPoint );

	const std::vector<VulkanTimelinePoint_t> &GetExternalDependencies() const { return m_ExternalDependencies; }
	const std::vector<VulkanTimelinePoint_t> &GetExternalSignals() const { return m_ExternalSignals; }

private:
	VkCommandBuffer m_cmdBuffer;
	CVulkanDevice *m_device;

	VkQueue m_queue;
	uint32_t m_queueFamily;

	// Per Use State
	std::vector<gamescope::Rc<CVulkanTexture>> m_textureRefs;
	std::unordered_map<CVulkanTexture *, TextureState> m_textureState;

	// Draw State
	std::array<CVulkanTexture *, VKR_SAMPLER_SLOTS> m_boundTextures;
	std::bitset<VKR_SAMPLER_SLOTS> m_useSrgb;
	std::array<SamplerState, VKR_SAMPLER_SLOTS> m_samplerState;
	CVulkanTexture *m_target;

	std::array<CVulkanTexture *, VKR_LUT3D_COUNT> m_shaperLut;
	std::array<CVulkanTexture *, VKR_LUT3D_COUNT> m_lut3D;

	std::vector<VulkanTimelinePoint_t> m_ExternalDependencies;
	std::vector<VulkanTimelinePoint_t> m_ExternalSignals;

	uint32_t m_renderBufferOffset = 0;
};

uint32_t VulkanFormatToDRM( VkFormat vkFormat, std::optional<bool> obHasAlphaOverride = std::nullopt );
VkFormat DRMFormatToVulkan( uint32_t nDRMFormat, bool bSrgb );
bool DRMFormatHasAlpha( uint32_t nDRMFormat );
uint32_t DRMFormatGetBPP( uint32_t nDRMFormat );

gamescope::OwningRc<CVulkanTexture> vulkan_create_flat_texture( uint32_t width, uint32_t height, uint8_t r, uint8_t g, uint8_t b, uint8_t a );

bool vulkan_supports_hdr10();

void vulkan_wait_idle();

extern CVulkanDevice g_device;
