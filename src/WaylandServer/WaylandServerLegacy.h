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


namespace gamescope
{
    class BackendBlob;
}

struct wlserver_x11_surface_info;
struct wlserver_xdg_surface_info;

struct wlserver_vk_swapchain_feedback
{
	uint32_t image_count;
	VkFormat vk_format;
	VkColorSpaceKHR vk_colorspace;
	VkCompositeAlphaFlagBitsKHR vk_composite_alpha;
	VkSurfaceTransformFlagBitsKHR vk_pre_transform;
	VkBool32 vk_clipped;

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

	uint64_t sequence = 0;
	std::vector<struct wl_resource*> pending_presentation_feedbacks;

	std::vector<struct wl_resource *> gamescope_swapchains;
	std::optional<uint32_t> present_id = std::nullopt;
	uint64_t desired_present_time = 0;

	uint64_t last_refresh_cycle = 0;
};

wlserver_wl_surface_info *get_wl_surface_info(struct wlr_surface *wlr_surf);
