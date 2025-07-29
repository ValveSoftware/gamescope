#include "steamcompmgr_shared.hpp"
#include "Utils/NonCopyable.h"

#include <optional>
#include "main.hpp"
#include "rendervulkan.hpp"

class CVulkanTexture;

struct UpscaledTexture_t
{
	GamescopeUpscaleFilter eFilter{};
	GamescopeUpscaleScaler eScaler{};
	uint32_t uOutputWidth = 0;
	uint32_t uOutputHeight = 0;
	gamescope::Rc<CVulkanTexture> pTexture{};
	VkColorSpaceKHR colorspace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
};

struct commit_t final : public gamescope::RcObject, public gamescope::IWaitable, public gamescope::NonCopyable
{
	commit_t();
    ~commit_t();

	GamescopeAppTextureColorspace colorspace() const;

	// For waitable:
	int GetFD() final;
	void OnPollIn() final;
	void Signal();
	void OnPollHangUp() final;

	bool IsPerfOverlayFIFO();

	// Returns true if we had a fence that was closed.
	bool CloseFenceInternal();
	void SetFence( int nFence, bool bMangoNudge, CommitDoneList_t *pDoneCommits );

	bool ShouldPreemptivelyUpscale();

	struct wlr_buffer *buf = nullptr;
	gamescope::Rc<CVulkanTexture> vulkanTex;
	std::optional<UpscaledTexture_t> upscaledTexture;

	gamescope::Rc<CVulkanTexture> GetTexture( GamescopeUpscaleFilter eFilter, GamescopeUpscaleScaler eScaler, GamescopeAppTextureColorspace &colorspace )
	{
		if ( upscaledTexture &&
			 upscaledTexture->eFilter == eFilter &&
			 upscaledTexture->eScaler == eScaler &&
			 upscaledTexture->uOutputWidth == g_nOutputWidth &&
			 upscaledTexture->uOutputHeight == g_nOutputHeight )
		{
			colorspace = VkColorSpaceToGamescopeAppTextureColorSpace( upscaledTexture->pTexture->format(), upscaledTexture->colorspace );
			return upscaledTexture->pTexture;
		}

		colorspace = this->colorspace();
		return vulkanTex;
	}

	uint64_t commitID = 0;
	bool done = false;
	bool async = false;
	bool fifo = false;
	bool is_steam = false;
	std::optional<wlserver_vk_swapchain_feedback> feedback = std::nullopt;

	uint64_t win_seq = 0;
	struct wlr_surface *surf = nullptr;
	std::vector<struct wl_resource*> presentation_feedbacks;

	std::optional<uint32_t> present_id = std::nullopt;
	uint64_t desired_present_time = 0;
	uint64_t earliest_present_time = 0;
	uint64_t present_margin = 0;
	uint64_t present_time = 0;

	std::mutex m_WaitableCommitStateMutex;
	int m_nCommitFence = -1;
	bool m_bMangoNudge = false;
	CommitDoneList_t *m_pDoneCommits = nullptr; // I hate this
};