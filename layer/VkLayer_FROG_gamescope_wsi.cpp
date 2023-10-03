#define VK_USE_PLATFORM_WAYLAND_KHR
#define VK_USE_PLATFORM_XCB_KHR
#define VK_USE_PLATFORM_XLIB_KHR
#include "vkroots.h"
#include "xcb_helpers.hpp"
#include "gamescope-xwayland-client-protocol.h"
#include "../src/color_helpers.h"
#include "../src/layer_defines.h"

#include <cstdio>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <optional>

// For limiter file.
#include <fcntl.h>
#include <unistd.h>

using namespace std::literals;

namespace GamescopeWSILayer {

  static bool contains(const std::vector<const char *> vec, std::string_view lookupValue) {
    return std::any_of(vec.begin(), vec.end(),
      [=](const char* value) { return value == lookupValue; });
  }

  // TODO: Maybe move to Wayland event or something.
  // This just utilizes the same code as the Mesa path used
  // for without the layer or GL though. Need to keep it around anyway.
  static std::mutex gamescopeSwapchainLimiterFDMutex;
  static uint32_t gamescopeFrameLimiterOverride() {
    const char *path = getenv("GAMESCOPE_LIMITER_FILE");
    if (!path)
        return 0;

    int fd = -1;
    {
      std::unique_lock lock(gamescopeSwapchainLimiterFDMutex);

      static int s_limiterFD = -1;

      if (s_limiterFD < 0)
        s_limiterFD = open(path, O_RDONLY);

      fd = s_limiterFD;
    }

    if (fd < 0)
        return 0;

    uint32_t overrideValue = 0;
    pread(fd, &overrideValue, sizeof(overrideValue), 0);
    return overrideValue;
  }

  struct GamescopeInstanceData {
    wl_display* display;
    wl_compositor* compositor;
    gamescope_xwayland* gamescope;
  };
  VKROOTS_DEFINE_SYNCHRONIZED_MAP_TYPE(GamescopeInstance, VkInstance);

  struct GamescopeSurfaceData {
    VkInstance instance;
    VkSurfaceKHR fallbackSurface;
    wl_surface* surface;

    xcb_connection_t* connection;
    xcb_window_t window;
    GamescopeLayerClient::Flags flags;
    bool hdrOutput;

    bool shouldExposeHDR() const {
      const bool hdrAllowed = !(flags & GamescopeLayerClient::Flag::DisableHDR);
      return hdrOutput && hdrAllowed;
    }

    bool canBypassXWayland() const {
      auto rect = xcb::getWindowRect(connection, window);
      auto largestObscuringWindowSize = xcb::getLargestObscuringChildWindowSize(connection, window);
      auto toplevelWindow = xcb::getToplevelWindow(connection, window);
      if (!rect || !largestObscuringWindowSize || !toplevelWindow) {
        fprintf(stderr, "[Gamescope WSI] canBypassXWayland: failed to get window info for window 0x%x.\n", window);
        return false;
      }

      auto toplevelRect = xcb::getWindowRect(connection, *toplevelWindow);
      if (!toplevelRect) {
        fprintf(stderr, "[Gamescope WSI] canBypassXWayland: failed to get window info for window 0x%x.\n", window);
        return false;
      }

      // If we have any child windows obscuring us bigger than 1x1,
      // then we cannot flip.
      // (There can be dummy composite redirect windows and whatever.)
      if (largestObscuringWindowSize->width > 1 || largestObscuringWindowSize->height > 1)
        return false;

      // If this window is not within 1px margin of error for the size of
      // it's top level window, then it cannot be flipped.
      if (iabs(rect->offset.x) > 1 ||
          iabs(rect->offset.y) > 1 ||
          iabs(int32_t(toplevelRect->extent.width)  - int32_t(rect->extent.width)) > 1 ||
          iabs(int32_t(toplevelRect->extent.height) - int32_t(rect->extent.height)) > 1)
        return false;

      // I want to add more checks wrt. composite redirects and such here,
      // but it seems what is exposed in xcb_composite is quite limited.
      // So let's see how it goes for now. :-)
      // Come back to this eventually.
      return true;
    }
  };
  VKROOTS_DEFINE_SYNCHRONIZED_MAP_TYPE(GamescopeSurface, VkSurfaceKHR);

  struct GamescopeSwapchainData {
    VkSurfaceKHR surface; // Always the Gamescope Surface surface -- so the Wayland one.
    bool isBypassingXWayland;
    VkPresentModeKHR presentMode;
    VkPresentModeKHR originalPresentMode;
  };
  VKROOTS_DEFINE_SYNCHRONIZED_MAP_TYPE(GamescopeSwapchain, VkSwapchainKHR);

  class VkInstanceOverrides {
  public:
    static VkResult CreateInstance(
            PFN_vkCreateInstance   pfnCreateInstanceProc,
      const VkInstanceCreateInfo*  pCreateInfo,
      const VkAllocationCallbacks* pAllocator,
            VkInstance*            pInstance) {
      // If we are an app running under gamescope and we aren't gamescope itself,
      // then setup our state for xwayland bypass.
      if (!isRunningUnderGamescope() || isAppInfoGamescope(pCreateInfo->pApplicationInfo))
        return pfnCreateInstanceProc(pCreateInfo, pAllocator, pInstance);

      auto enabledExts = std::vector<const char*>(
        pCreateInfo->ppEnabledExtensionNames,
        pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);

      if (!contains(enabledExts, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME))
        enabledExts.push_back(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);

      if (!contains(enabledExts, VK_KHR_XCB_SURFACE_EXTENSION_NAME))
        enabledExts.push_back(VK_KHR_XCB_SURFACE_EXTENSION_NAME);

      VkInstanceCreateInfo createInfo = *pCreateInfo;
      createInfo.enabledExtensionCount   = uint32_t(enabledExts.size());
      createInfo.ppEnabledExtensionNames = enabledExts.data();

      setenv("vk_khr_present_wait", "true", 0);

      VkResult result = pfnCreateInstanceProc(&createInfo, pAllocator, pInstance);
      if (result != VK_SUCCESS)
        return result;

      wl_display *display = wl_display_connect(gamescopeWaylandSocket());
      if (!display) {
        fprintf(stderr, "[Gamescope WSI] Failed to connect to gamescope socket: %s. Bypass layer will be unavailable.\n", gamescopeWaylandSocket());
        return result;
      }
      wl_registry *registry = wl_display_get_registry(display);

      {
        auto state = GamescopeInstance::create(*pInstance, GamescopeInstanceData {
          .display = display,
        });
        wl_registry_add_listener(registry, &s_registryListener, reinterpret_cast<void *>(state.get()));
      }
      wl_display_dispatch(display);
      wl_display_roundtrip(display);
      wl_registry_destroy(registry);

      return result;
    }

    static void DestroyInstance(
      const vkroots::VkInstanceDispatch* pDispatch,
            VkInstance                   instance,
      const VkAllocationCallbacks*       pAllocator) {
      if (auto state = GamescopeInstance::get(instance)) {
        wl_display_disconnect(state->display);
      }
      GamescopeInstance::remove(instance);
      pDispatch->DestroyInstance(instance, pAllocator);
    }

    static VkBool32 GetPhysicalDeviceXcbPresentationSupportKHR(
      const vkroots::VkInstanceDispatch* pDispatch,
            VkPhysicalDevice             physicalDevice,
            uint32_t                     queueFamilyIndex,
            xcb_connection_t*            connection,
            xcb_visualid_t               visual_id) {
      auto gamescopeInstance = GamescopeInstance::get(pDispatch->Instance);
      if (!gamescopeInstance)
        return pDispatch->GetPhysicalDeviceXcbPresentationSupportKHR(physicalDevice, queueFamilyIndex, connection, visual_id);

      return GetPhysicalDeviceGamescopePresentationSupport(pDispatch, gamescopeInstance, physicalDevice, queueFamilyIndex);
    }

    static VkBool32 GetPhysicalDeviceXlibPresentationSupportKHR(
      const vkroots::VkInstanceDispatch* pDispatch,
            VkPhysicalDevice             physicalDevice,
            uint32_t                     queueFamilyIndex,
            Display*                     dpy,
            VisualID                     visualID) {
      auto gamescopeInstance = GamescopeInstance::get(pDispatch->Instance);
      if (!gamescopeInstance)
        return pDispatch->GetPhysicalDeviceXlibPresentationSupportKHR(physicalDevice, queueFamilyIndex, dpy, visualID);

      return GetPhysicalDeviceGamescopePresentationSupport(pDispatch, gamescopeInstance, physicalDevice, queueFamilyIndex);
    }

    static VkResult CreateXcbSurfaceKHR(
      const vkroots::VkInstanceDispatch* pDispatch,
            VkInstance                   instance,
      const VkXcbSurfaceCreateInfoKHR*   pCreateInfo,
      const VkAllocationCallbacks*       pAllocator,
            VkSurfaceKHR*                pSurface) {
      auto gamescopeInstance = GamescopeInstance::get(instance);
      if (!gamescopeInstance)
        return pDispatch->CreateXcbSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);

      return CreateGamescopeSurface(pDispatch, gamescopeInstance, instance, pCreateInfo->connection, pCreateInfo->window, pAllocator, pSurface);
    }

    static VkResult CreateXlibSurfaceKHR(
      const vkroots::VkInstanceDispatch* pDispatch,
            VkInstance                   instance,
      const VkXlibSurfaceCreateInfoKHR*  pCreateInfo,
      const VkAllocationCallbacks*       pAllocator,
            VkSurfaceKHR*                pSurface) {
      auto gamescopeInstance = GamescopeInstance::get(instance);
      if (!gamescopeInstance)
        return pDispatch->CreateXlibSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);

      return CreateGamescopeSurface(pDispatch, gamescopeInstance, instance, XGetXCBConnection(pCreateInfo->dpy), xcb_window_t(pCreateInfo->window), pAllocator, pSurface);
    }

    static constexpr std::array<VkSurfaceFormat2KHR, 3> s_ExtraHDRSurfaceFormat2s = {{
      { .surfaceFormat = { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT, } },
      { .surfaceFormat = { VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT, } },
      { .surfaceFormat = { VK_FORMAT_R16G16B16A16_SFLOAT,      VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT, } },
    }};

    static constexpr auto s_ExtraHDRSurfaceFormats = []() {
      std::array<VkSurfaceFormatKHR, s_ExtraHDRSurfaceFormat2s.size()> array;
      for (size_t i = 0; i < s_ExtraHDRSurfaceFormat2s.size(); i++)
        array[i] = s_ExtraHDRSurfaceFormat2s[i].surfaceFormat;
      return array;
    }();

    static VkResult GetPhysicalDeviceSurfaceFormatsKHR(
      const vkroots::VkInstanceDispatch* pDispatch,
            VkPhysicalDevice             physicalDevice,
            VkSurfaceKHR                 surface,
            uint32_t*                    pSurfaceFormatCount,
            VkSurfaceFormatKHR*          pSurfaceFormats) {
      auto gamescopeSurface = GamescopeSurface::get(surface);
      if (!gamescopeSurface)
        return pDispatch->GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, pSurfaceFormatCount, pSurfaceFormats);

      const bool canBypass = gamescopeSurface->canBypassXWayland();
      VkSurfaceKHR selectedSurface = canBypass ? surface : gamescopeSurface->fallbackSurface;

      if (!canBypass || !gamescopeSurface->shouldExposeHDR())
        return pDispatch->GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, selectedSurface, pSurfaceFormatCount, pSurfaceFormats);

      return vkroots::helpers::append(
        pDispatch->GetPhysicalDeviceSurfaceFormatsKHR,
        s_ExtraHDRSurfaceFormats,
        pSurfaceFormatCount,
        pSurfaceFormats,
        physicalDevice,
        selectedSurface);
    }

    static VkResult GetPhysicalDeviceSurfaceFormats2KHR(
      const vkroots::VkInstanceDispatch*     pDispatch,
            VkPhysicalDevice                 physicalDevice,
      const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
            uint32_t*                        pSurfaceFormatCount,
            VkSurfaceFormat2KHR*             pSurfaceFormats) {
      auto gamescopeSurface = GamescopeSurface::get(pSurfaceInfo->surface);
      if (!gamescopeSurface)
        return pDispatch->GetPhysicalDeviceSurfaceFormats2KHR(physicalDevice, pSurfaceInfo, pSurfaceFormatCount, pSurfaceFormats);

      VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo = *pSurfaceInfo;
      const bool canBypass = gamescopeSurface->canBypassXWayland();
      surfaceInfo.surface = canBypass ? surfaceInfo.surface : gamescopeSurface->fallbackSurface;

      if (!canBypass || !gamescopeSurface->shouldExposeHDR())
        return pDispatch->GetPhysicalDeviceSurfaceFormats2KHR(physicalDevice, &surfaceInfo, pSurfaceFormatCount, pSurfaceFormats);

      return vkroots::helpers::append(
        pDispatch->GetPhysicalDeviceSurfaceFormats2KHR,
        s_ExtraHDRSurfaceFormat2s,
        pSurfaceFormatCount,
        pSurfaceFormats,
        physicalDevice,
        &surfaceInfo);
    }

    static VkResult GetPhysicalDeviceSurfaceCapabilitiesKHR(
      const vkroots::VkInstanceDispatch*     pDispatch,
            VkPhysicalDevice                 physicalDevice,
            VkSurfaceKHR                     surface,
            VkSurfaceCapabilitiesKHR*        pSurfaceCapabilities) {
      auto gamescopeSurface = GamescopeSurface::get(surface);
      if (!gamescopeSurface)
        return pDispatch->GetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, pSurfaceCapabilities);

      VkResult res = VK_SUCCESS;
      if ((res = pDispatch->GetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, pSurfaceCapabilities)) != VK_SUCCESS)
        return res;

      auto rect = xcb::getWindowRect(gamescopeSurface->connection, gamescopeSurface->window);
      if (!rect)
        return VK_ERROR_SURFACE_LOST_KHR;

      pSurfaceCapabilities->currentExtent = rect->extent;
      pSurfaceCapabilities->minImageCount = getMinImageCount();

      return VK_SUCCESS;
    }

    static VkResult GetPhysicalDeviceSurfaceCapabilities2KHR(
      const vkroots::VkInstanceDispatch*     pDispatch,
            VkPhysicalDevice                 physicalDevice,
      const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
            VkSurfaceCapabilities2KHR*       pSurfaceCapabilities) {
      auto gamescopeSurface = GamescopeSurface::get(pSurfaceInfo->surface);
      if (!gamescopeSurface)
        return pDispatch->GetPhysicalDeviceSurfaceCapabilities2KHR(physicalDevice, pSurfaceInfo, pSurfaceCapabilities);

      VkResult res = VK_SUCCESS;
      if ((res = pDispatch->GetPhysicalDeviceSurfaceCapabilities2KHR(physicalDevice, pSurfaceInfo, pSurfaceCapabilities)) != VK_SUCCESS)
        return res;

      auto rect = xcb::getWindowRect(gamescopeSurface->connection, gamescopeSurface->window);
      if (!rect)
        return VK_ERROR_SURFACE_LOST_KHR;

      pSurfaceCapabilities->surfaceCapabilities.currentExtent = rect->extent;
      pSurfaceCapabilities->surfaceCapabilities.minImageCount = getMinImageCount();

      return VK_SUCCESS;
    }

    static void DestroySurfaceKHR(
      const vkroots::VkInstanceDispatch* pDispatch,
            VkInstance                   instance,
            VkSurfaceKHR                 surface,
      const VkAllocationCallbacks*       pAllocator) {
      if (auto state = GamescopeSurface::get(surface)) {
        wl_surface_destroy(state->surface);
      }
      GamescopeSurface::remove(surface);
      pDispatch->DestroySurfaceKHR(instance, surface, pAllocator);
    }

    static VkResult EnumerateDeviceExtensionProperties(
      const vkroots::VkInstanceDispatch* pDispatch,
            VkPhysicalDevice             physicalDevice,
            const char*                  pLayerName,
            uint32_t*                    pPropertyCount,
            VkExtensionProperties*       pProperties) {
      static constexpr std::array<VkExtensionProperties, 1> s_LayerExposedExts = {{
        { VK_EXT_HDR_METADATA_EXTENSION_NAME,
          VK_EXT_HDR_METADATA_SPEC_VERSION },
      }};

      if (pLayerName) {
        if (pLayerName == "VK_LAYER_FROG_gamescope_wsi"sv) {
          return vkroots::helpers::array(s_LayerExposedExts, pPropertyCount, pProperties);
        } else {
          return pDispatch->EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
        }
      }

      return vkroots::helpers::append(
        pDispatch->EnumerateDeviceExtensionProperties,
        s_LayerExposedExts,
        pPropertyCount,
        pProperties,
        physicalDevice,
        pLayerName);
    }

  private:
    static VkResult CreateGamescopeSurface(
      const vkroots::VkInstanceDispatch* pDispatch,
            GamescopeInstance&           gamescopeInstance,
            VkInstance                   instance,
            xcb_connection_t*            connection,
            xcb_window_t                 window,
      const VkAllocationCallbacks*       pAllocator,
            VkSurfaceKHR*                pSurface) {
      fprintf(stderr, "[Gamescope WSI] Creating Gamescope surface: xid: 0x%x\n", window);

      wl_surface* waylandSurface = wl_compositor_create_surface(gamescopeInstance->compositor);
      if (!waylandSurface) {
        fprintf(stderr, "[Gamescope WSI] Failed to create wayland surface - xid: 0x%x\n", window);
        return VK_ERROR_SURFACE_LOST_KHR;
      }

      GamescopeLayerClient::Flags flags = 0u;
      if (auto prop = xcb::getPropertyValue<GamescopeLayerClient::Flags>(connection, "GAMESCOPE_LAYER_CLIENT_FLAGS"sv))
        flags = *prop;

      bool hdrOutput = false;
      if (auto prop = xcb::getPropertyValue<uint32_t>(connection, "GAMESCOPE_HDR_OUTPUT_FEEDBACK"sv))
        hdrOutput = !!*prop;

      auto serverId = xcb::getPropertyValue<uint32_t>(connection, "GAMESCOPE_XWAYLAND_SERVER_ID"sv);
      if (!serverId) {
        fprintf(stderr, "[Gamescope WSI] Failed to get Xwayland server id. Failing surface creation.\n");
        return VK_ERROR_SURFACE_LOST_KHR;
      }
      gamescope_xwayland_override_window_content2(gamescopeInstance->gamescope, waylandSurface, *serverId, window);

      wl_display_flush(gamescopeInstance->display);

      VkWaylandSurfaceCreateInfoKHR waylandCreateInfo = {
        .sType   = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
        .pNext   = nullptr,
        .flags   = 0,
        .display = gamescopeInstance->display,
        .surface = waylandSurface,
      };

      VkResult result = pDispatch->CreateWaylandSurfaceKHR(instance, &waylandCreateInfo, pAllocator, pSurface);
      if (result != VK_SUCCESS) {
        fprintf(stderr, "[Gamescope WSI] Failed to create Vulkan wayland surface - vr: %s xid: 0x%x\n", vkroots::helpers::enumString(result), window);
        return result;
      }

      VkXcbSurfaceCreateInfoKHR xcbCreateInfo = {
        .sType      = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
        .pNext      = nullptr,
        .flags      = 0,
        .connection = connection,
        .window     = window,
      };
      VkSurfaceKHR fallbackSurface = VK_NULL_HANDLE;
      result = pDispatch->CreateXcbSurfaceKHR(instance, &xcbCreateInfo, pAllocator, &fallbackSurface);
      if (result != VK_SUCCESS) {
        fprintf(stderr, "[Gamescope WSI] Failed to create Vulkan xcb (fallback) surface - vr: %s xid: 0x%x\n", vkroots::helpers::enumString(result), window);
        return result;
      }

      fprintf(stderr, "[Gamescope WSI] Made gamescope surface for xid: 0x%x\n", window);
      auto gamescopeSurface = GamescopeSurface::create(*pSurface, GamescopeSurfaceData {
        .instance        = instance,
        .fallbackSurface = fallbackSurface,
        .surface         = waylandSurface,
        .connection      = connection,
        .window          = window,
        .flags           = flags,
        .hdrOutput       = hdrOutput,
      });

      DumpGamescopeSurfaceState(gamescopeSurface);

      return result;
    }

    static void DumpGamescopeSurfaceState(GamescopeSurface& surface) {
      fprintf(stderr, "[Gamescope WSI] Surface state:\n");
      fprintf(stderr, "  window xid:                    0x%x\n", surface->window);
      fprintf(stderr, "  wayland surface res id:        %u\n", wl_proxy_get_id(reinterpret_cast<struct wl_proxy *>(surface->surface)));
      fprintf(stderr, "  layer client flags:            0x%x\n", surface->flags);
      fprintf(stderr, "  server hdr output enabled:     %s\n", surface->hdrOutput ? "true" : "false");
      fprintf(stderr, "  hdr formats exposed to client: %s\n", surface->shouldExposeHDR() ? "true" : "false");
    }

    static VkBool32 GetPhysicalDeviceGamescopePresentationSupport(
      const vkroots::VkInstanceDispatch* pDispatch,
            GamescopeInstance&           gamescopeInstance,
            VkPhysicalDevice             physicalDevice,
            uint32_t                     queueFamilyIndex) {
      return pDispatch->GetPhysicalDeviceWaylandPresentationSupportKHR(physicalDevice, queueFamilyIndex, gamescopeInstance->display);
    }

    static const char* gamescopeWaylandSocket() {
      return std::getenv("GAMESCOPE_WAYLAND_DISPLAY");
    }

    static bool isAppInfoGamescope(const VkApplicationInfo *appInfo) {
      if (!appInfo || !appInfo->pApplicationName)
        return false;

      return appInfo->pApplicationName == "gamescope"sv;
    }

    static bool isRunningUnderGamescope() {
      static bool s_isRunningUnderGamescope = []() -> bool {
        const char *gamescopeSocketName = gamescopeWaylandSocket();
        if (!gamescopeSocketName || !*gamescopeSocketName)
          return false;

        // Gamescope always unsets WAYLAND_SOCKET.
        // So if that is set, we know we cannot be running under Gamescope
        // and must be in a nested Wayland session inside of gamescope.
        const char *waylandSocketName = std::getenv("WAYLAND_DISPLAY");
        if (waylandSocketName && *waylandSocketName)
          return false;

        return true;
      }();

      return s_isRunningUnderGamescope;
    }

    static uint32_t getMinImageCount() {
      {
        const char *overrideStr = std::getenv("GAMESCOPE_WSI_MIN_IMAGE_COUNT");
        if (overrideStr && *overrideStr)
          return uint32_t(std::atoi(overrideStr));
      }

      {
        const char *overrideStr = std::getenv("vk_x11_override_min_image_count");
        if (overrideStr && *overrideStr)
          return uint32_t(std::atoi(overrideStr));
      }

      return 3;
    }

    static constexpr wl_registry_listener s_registryListener = {
      .global = [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
        auto instance = reinterpret_cast<GamescopeInstanceData *>(data);

        if (interface == "wl_compositor"sv) {
          instance->compositor = reinterpret_cast<wl_compositor *>(
            wl_registry_bind(registry, name, &wl_compositor_interface, version));
        } else if (interface == "gamescope_xwayland"sv) {
          instance->gamescope = reinterpret_cast<gamescope_xwayland *>(
            wl_registry_bind(registry, name, &gamescope_xwayland_interface, version));
        }
      },
      .global_remove = [](void* data, wl_registry* registry, uint32_t name) {
      },
    };

  };

  class VkDeviceOverrides {
  public:
    static void DestroySwapchainKHR(
      const vkroots::VkDeviceDispatch* pDispatch,
            VkDevice                   device,
            VkSwapchainKHR             swapchain,
      const VkAllocationCallbacks*     pAllocator) {
      GamescopeSwapchain::remove(swapchain);
      pDispatch->DestroySwapchainKHR(device, swapchain, pAllocator);
    }

    static VkResult CreateSwapchainKHR(
      const vkroots::VkDeviceDispatch* pDispatch,
            VkDevice                   device,
      const VkSwapchainCreateInfoKHR*  pCreateInfo,
      const VkAllocationCallbacks*     pAllocator,
            VkSwapchainKHR*            pSwapchain) {
      auto gamescopeSurface = GamescopeSurface::get(pCreateInfo->surface);
      if (!gamescopeSurface)
        return pDispatch->CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);

      VkSwapchainCreateInfoKHR swapchainInfo = *pCreateInfo;

      const VkPresentModeKHR originalPresentMode = swapchainInfo.presentMode;
      const uint32_t limiterOverride = gamescopeFrameLimiterOverride();
      if (limiterOverride == 1) {
        fprintf(stderr, "[Gamescope WSI] Overriding present mode to FIFO from frame limiter override.\n");
        swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
      }

      const bool canBypass = gamescopeSurface->canBypassXWayland();
      // If we can't flip, fallback to the regular XCB surface on the XCB window.
      if (!canBypass)
        swapchainInfo.surface = gamescopeSurface->fallbackSurface;

      if (gamescopeSurface) {
        // If this is a gamescope surface
        // Force the colorspace to sRGB before sending to the driver.
        swapchainInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

        fprintf(stderr, "[Gamescope WSI] Creating swapchain for xid: 0x%0x - minImageCount: %u - format: %s - colorspace: %s - flip: %s\n",
          gamescopeSurface->window,
          pCreateInfo->minImageCount,
          vkroots::helpers::enumString(pCreateInfo->imageFormat),
          vkroots::helpers::enumString(pCreateInfo->imageColorSpace),
          canBypass ? "true" : "false");
      }

      // Check for VkFormat support and return VK_ERROR_INITIALIZATION_FAILED
      // if that VkFormat is unsupported for the underlying surface.
      {
        std::vector<VkSurfaceFormatKHR> supportedSurfaceFormats;
        vkroots::helpers::enumerate(
          pDispatch->pPhysicalDeviceDispatch->pInstanceDispatch->GetPhysicalDeviceSurfaceFormatsKHR,
          supportedSurfaceFormats,
          pDispatch->PhysicalDevice,
          swapchainInfo.surface);

        bool supportedSwapchainFormat = std::any_of(
          supportedSurfaceFormats.begin(),
          supportedSurfaceFormats.end(),
          [=](VkSurfaceFormatKHR value) { return value.format == swapchainInfo.imageFormat; });

        if (!supportedSwapchainFormat) {
          fprintf(stderr, "[Gamescope WSI] Refusing to make swapchain (unsupported VkFormat) for xid: 0x%0x - format: %s - colorspace: %s - flip: %s\n",
            gamescopeSurface->window,
            vkroots::helpers::enumString(pCreateInfo->imageFormat),
            vkroots::helpers::enumString(pCreateInfo->imageColorSpace),
            canBypass ? "true" : "false");

          return VK_ERROR_INITIALIZATION_FAILED;
        }
      }

      VkResult result = pDispatch->CreateSwapchainKHR(device, &swapchainInfo, pAllocator, pSwapchain);
      if (gamescopeSurface) {
        if (result == VK_SUCCESS) {
          GamescopeSwapchain::create(*pSwapchain, GamescopeSwapchainData{
            .surface             = pCreateInfo->surface, // Always the Wayland side surface.
            .isBypassingXWayland = canBypass,
            .presentMode         = swapchainInfo.presentMode, // The new present mode.
            .originalPresentMode = originalPresentMode,
          });

          auto gamescopeInstance = GamescopeInstance::get(gamescopeSurface->instance);
          if (gamescopeInstance) {
            uint32_t imageCount = 0;
            pDispatch->GetSwapchainImagesKHR(device, *pSwapchain, &imageCount, nullptr);

            fprintf(stderr, "[Gamescope WSI] Created swapchain for xid: 0x%0x - imageCount: %u\n",
              gamescopeSurface->window,
              imageCount);

            gamescope_xwayland_swapchain_feedback(
              gamescopeInstance->gamescope,
              gamescopeSurface->surface,
              imageCount,
              uint32_t(pCreateInfo->imageFormat),
              uint32_t(pCreateInfo->imageColorSpace),
              uint32_t(pCreateInfo->compositeAlpha),
              uint32_t(pCreateInfo->preTransform),
              uint32_t(pCreateInfo->presentMode),
              uint32_t(pCreateInfo->clipped));
          }
        } else {
          fprintf(stderr, "[Gamescope WSI] Failed to create swapchain - vr: %s xid: 0x%x\n", vkroots::helpers::enumString(result), gamescopeSurface->window);
        }
      }
      return result;
    }

    static VkResult QueuePresentKHR(
      const vkroots::VkDeviceDispatch* pDispatch,
            VkQueue                    queue,
      const VkPresentInfoKHR*          pPresentInfo) {
      const uint32_t limiterOverride = gamescopeFrameLimiterOverride();

      VkResult result = pDispatch->QueuePresentKHR(queue, pPresentInfo);

      for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
        VkSwapchainKHR swapchain = pPresentInfo->pSwapchains[i];

        auto UpdateSwapchainResult = [&](VkResult newResult) {
          if (pPresentInfo->pResults && pPresentInfo->pResults[i] >= VK_SUCCESS)
            pPresentInfo->pResults[i] = newResult;
          if (result >= VK_SUCCESS)
            result = newResult;
        };

        if (auto gamescopeSwapchain = GamescopeSwapchain::get(swapchain)) {

          if ((limiterOverride == 1 && gamescopeSwapchain->presentMode != VK_PRESENT_MODE_FIFO_KHR) ||
              (limiterOverride != 1 && gamescopeSwapchain->presentMode != gamescopeSwapchain->originalPresentMode)) {
              fprintf(stderr, "[Gamescope WSI] Forcing swapchain recreation as frame limiter changed.\n");
              UpdateSwapchainResult(VK_ERROR_OUT_OF_DATE_KHR);
          }

          auto gamescopeSurface = GamescopeSurface::get(gamescopeSwapchain->surface);
          if (!gamescopeSurface) {
            fprintf(stderr, "[Gamescope WSI] QueuePresentKHR: Surface for swapchain %u was already destroyed. (App use after free).\n", i);
            abort();
            continue;
          }

          const bool canBypass = gamescopeSurface->canBypassXWayland();
          if (canBypass != gamescopeSwapchain->isBypassingXWayland)
            UpdateSwapchainResult(canBypass ? VK_SUBOPTIMAL_KHR : VK_ERROR_OUT_OF_DATE_KHR);
        }
      }

      return result;
    }

    static void SetHdrMetadataEXT(
      const vkroots::VkDeviceDispatch* pDispatch,
            VkDevice                   device,
            uint32_t                   swapchainCount,
      const VkSwapchainKHR*            pSwapchains,
      const VkHdrMetadataEXT*          pMetadata) {
      for (uint32_t i = 0; i < swapchainCount; i++) {
        auto gamescopeSwapchain = GamescopeSwapchain::get(pSwapchains[i]);
        if (!gamescopeSwapchain) {
          fprintf(stderr, "[Gamescope WSI] SetHdrMetadataEXT: Swapchain %u does not support HDR.\n", i);
          continue;
        }

        auto gamescopeSurface = GamescopeSurface::get(gamescopeSwapchain->surface);
        if (!gamescopeSurface) {
          fprintf(stderr, "[Gamescope WSI] SetHdrMetadataEXT: Surface for swapchain %u was already destroyed. (App use after free).\n", i);
          abort();
          continue;
        }

        auto gamescopeInstance = GamescopeInstance::get(gamescopeSurface->instance);
        if (!gamescopeInstance) {
          fprintf(stderr, "[Gamescope WSI] SetHdrMetadataEXT: Instance for swapchain %u was already destroyed. (App use after free).\n", i);
          abort();
          continue;
        }

        const VkHdrMetadataEXT& metadata = pMetadata[i];
        gamescope_xwayland_set_hdr_metadata(
          gamescopeInstance->gamescope,
          gamescopeSurface->surface,
          color_xy_to_u16(metadata.displayPrimaryRed.x),
          color_xy_to_u16(metadata.displayPrimaryRed.y),
          color_xy_to_u16(metadata.displayPrimaryGreen.x),
          color_xy_to_u16(metadata.displayPrimaryGreen.y),
          color_xy_to_u16(metadata.displayPrimaryBlue.x),
          color_xy_to_u16(metadata.displayPrimaryBlue.y),
          color_xy_to_u16(metadata.whitePoint.x),
          color_xy_to_u16(metadata.whitePoint.y),
          nits_to_u16(metadata.maxLuminance),
          nits_to_u16_dark(metadata.minLuminance),
          nits_to_u16(metadata.maxContentLightLevel),
          nits_to_u16(metadata.maxFrameAverageLightLevel));

          fprintf( stderr, "[Gamescope WSI] VkHdrMetadataEXT: mastering luminance min %f nits, max %f nits\n", metadata.minLuminance, metadata.maxLuminance );
          fprintf( stderr, "[Gamescope WSI] VkHdrMetadataEXT: maxContentLightLevel %f nits\n", metadata.maxContentLightLevel );
          fprintf( stderr, "[Gamescope WSI] VkHdrMetadataEXT: maxFrameAverageLightLevel %f nits\n", metadata.maxFrameAverageLightLevel );
      }
    }

  };

}

VKROOTS_DEFINE_LAYER_INTERFACES(GamescopeWSILayer::VkInstanceOverrides,
                                vkroots::NoOverrides,
                                GamescopeWSILayer::VkDeviceOverrides);

VKROOTS_IMPLEMENT_SYNCHRONIZED_MAP_TYPE(GamescopeWSILayer::GamescopeInstance);
VKROOTS_IMPLEMENT_SYNCHRONIZED_MAP_TYPE(GamescopeWSILayer::GamescopeSurface);
VKROOTS_IMPLEMENT_SYNCHRONIZED_MAP_TYPE(GamescopeWSILayer::GamescopeSwapchain);
