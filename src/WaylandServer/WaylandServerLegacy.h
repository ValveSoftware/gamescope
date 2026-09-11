#pragma once

#include <wayland-server-core.h>
#include "WaylandDecls.h"
#include <memory>
#include <optional>
#include <vector>
#include "vulkan_include.h"


#include "wlr_begin.hpp"
#include <wlr/types/wlr_compositor.h>
#include "wlr_end.hpp"

// A queued commit can outlive its client, whose disconnect nulls the resource.
struct wlserver_presentation_feedback
{
	struct wl_resource *resource = nullptr;
};
using wlserver_presentation_feedback_ref = std::shared_ptr<wlserver_presentation_feedback>;


struct wlserver_x11_surface_info;
struct wlserver_xdg_surface_info;

namespace gamescope
{
	class BackendBlob;

	struct PresentTimingRoute
	{
		// Accessed only under wlserver_lock, including resource destruction.
		wl_resource *resource = nullptr;
		// Predicted scanout of this swapchain's last latched frame, the anchor
		// for relative targets. steamcompmgr thread only.
		uint64_t last_expected_present_time = 0;
	};

	struct PresentTiming
	{
		std::optional<uint64_t> serial;
		uint64_t target = 0;
		uint32_t flags = 0;
		bool legacy = false;
		std::shared_ptr<PresentTimingRoute> route;
	};
}

struct wlserver_vk_swapchain_feedback
{
	uint32_t image_count;
	VkFormat vk_format;
	VkColorSpaceKHR vk_colorspace;
	VkCompositeAlphaFlagBitsKHR vk_composite_alpha;
	VkSurfaceTransformFlagBitsKHR vk_pre_transform;
	VkBool32 vk_clipped;
	std::shared_ptr<std::string> vk_engine_name;

	std::shared_ptr<gamescope::BackendBlob> hdr_metadata_blob;
};


struct wlserver_wl_surface_info
{
	wlserver_x11_surface_info *x11_surface = nullptr;
	wlserver_xdg_surface_info *xdg_surface = nullptr;

	gamescope::WaylandServer::CLinuxDrmSyncobjSurface *pSyncobjSurface = nullptr;

	struct wlr_surface *wlr = nullptr;
	struct wl_listener commit;
	struct wl_listener destroy;

	std::shared_ptr<wlserver_vk_swapchain_feedback> swapchain_feedback = {};
	std::optional<VkPresentModeKHR> oCurrentPresentMode;

	std::vector<wlserver_presentation_feedback_ref> pending_presentation_feedbacks;

	std::vector<struct wl_resource *> gamescope_swapchains;
	gamescope::PresentTiming present_timing;

	uint64_t last_refresh_cycle = 0;
	uint64_t last_refresh_interval = 0;
};

wlserver_wl_surface_info *get_wl_surface_info(struct wlr_surface *wlr_surf);
