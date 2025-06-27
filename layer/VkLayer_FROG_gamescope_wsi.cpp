#define VK_USE_PLATFORM_WAYLAND_KHR
#define VK_USE_PLATFORM_XCB_KHR
#define VK_USE_PLATFORM_XLIB_KHR
#include "vkroots.h"
#include "xcb_helpers.hpp"
#include "vulkan_operators.hpp"
#include "gamescope-swapchain-client-protocol.h"
#include "../src/color_helpers.h"
#include "../src/layer_defines.h"

#include <charconv>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <optional>

#include <poll.h>
// For limiter file.
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

#include "../src/messagey.h"

using namespace std::literals;

namespace GamescopeWSILayer {

  static const size_t MaxPastPresentationTimes = 16;

  static uint64_t timespecToNanos(struct timespec& spec) {
    return spec.tv_sec * 1'000'000'000ul + spec.tv_nsec;
  }

  [[maybe_unused]] static uint64_t getTimeMonotonic() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return timespecToNanos(ts);
  }

  static bool contains(const std::vector<const char *> vec, std::string_view lookupValue) {
    return std::ranges::any_of(vec, std::bind_front(std::equal_to{}, lookupValue));
  }

  static int waylandPumpEvents(wl_display *display) {
    int wlFd = wl_display_get_fd(display);

    while (true) {
      int ret = 0;

      if ((ret = wl_display_dispatch_pending(display)) < 0)
        return ret;

      if ((ret = wl_display_prepare_read(display)) < 0) {
        if (errno == EAGAIN)
          continue;

        return -1;
      }

      pollfd pollfd = {
        .fd = wlFd,
        .events = POLLIN,
      };
      timespec zeroTimeout = {};
      ret = ppoll(&pollfd, 1, &zeroTimeout, NULL);

      if (ret <= 0) {
        wl_display_cancel_read(display);
        if (ret == 0)
          wl_display_flush(display);
        return ret;
      }

      ret = wl_display_read_events(display);
      if (ret < 0)
        return ret;

      ret = wl_display_flush(display);
      return ret;
    }
  }

  uint32_t clientAppId() {
    const char *appid = getenv("SteamAppId");
    if (!appid || !*appid)
      return 0;

    return atoi(appid);
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

      // Gamescope always sets or unsets WAYLAND_SOCKET.
      // So if that is set to something else, we know we cannot be running
      // under Gamescope and must be in a nested Wayland session inside of gamescope.
      const char *waylandSocketName = std::getenv("WAYLAND_DISPLAY");
      if (waylandSocketName && *waylandSocketName && strcmp(gamescopeSocketName, waylandSocketName) != 0)
        return false;

      return true;
    }();

    return s_isRunningUnderGamescope;
  }

  template <typename T>
  std::optional<T> parseEnv(const char *envName) {
    const char *str = std::getenv(envName);
    if (!str || !*str)
      return std::nullopt;

    T value;
    auto result = std::from_chars(str, str + strlen(str), value);
    if (result.ec != std::errc{})
      return std::nullopt;

    return value;
  }

  template <>
  std::optional<bool> parseEnv(const char *envName) {
    const char *str = std::getenv(envName);
    if (!str || !*str)
      return std::nullopt;

    if (str == "true"sv || str == "1"sv)
      return true;

    return false;
  }

  static uint32_t getMinImageCount() {
    static uint32_t s_minImageCount = []() -> uint32_t {
      if (auto minCount = parseEnv<uint32_t>("GAMESCOPE_WSI_MIN_IMAGE_COUNT")) {
        fprintf(stderr, "[Gamescope WSI] minImageCount overridden by GAMESCOPE_WSI_MIN_IMAGE_COUNT: %u\n", *minCount);
        return *minCount;
      }

      if (auto minCount = parseEnv<uint32_t>("vk_wsi_override_min_image_count")) {
        fprintf(stderr, "[Gamescope WSI] minImageCount overridden by vk_wsi_override_min_image_count: %u\n", *minCount);
        return *minCount;
      }

      if (auto minCount = parseEnv<uint32_t>("vk_x11_override_min_image_count")) {
        fprintf(stderr, "[Gamescope WSI] minImageCount overridden by vk_x11_override_min_image_count: %u\n", *minCount);
        return *minCount;
      }

      return 3u;
    }();

    return s_minImageCount;
  }

  static bool getEnsureMinImageCount() {
    static bool s_ensureMinImageCount = []() -> bool {
      if (auto ensure = parseEnv<bool>("GAMESCOPE_WSI_ENSURE_MIN_IMAGE_COUNT")) {
        return *ensure;
      }
      if (auto ensure = parseEnv<bool>("vk_x11_ensure_min_image_count")) {
        return *ensure;
      }
      return false;
    }();
    return s_ensureMinImageCount;
  }

  // Taken from Mesa, licensed under MIT.
  //
  // No real reason to rewrite this code,
  // it works :)
  static char *
  __getProgramName()
  {
    char * arg = strrchr(program_invocation_name, '/');
    if (arg) {
        char *program_name = NULL;
        /* If the / character was found this is likely a linux path or
        * an invocation path for a 64-bit wine program.
        *
        * However, some programs pass command line arguments into argv[0].
        * Strip these arguments out by using the realpath only if it was
        * a prefix of the invocation name.
        */
        char *path = realpath("/proc/self/exe", NULL);

        if (path && strncmp(path, program_invocation_name, strlen(path)) == 0) {
          /* This shouldn't be null because path is a a prefix,
            * but check it anyway since path is static. */
          char * name = strrchr(path, '/');
          if (name)
              program_name = strdup(name + 1);
        }
        if (path) {
          free(path);
        }
        if (!program_name) {
          program_name = strdup(arg+1);
        }
        return program_name;
    }

    /* If there was no '/' at all we likely have a windows like path from
      * a wine application.
      */
    arg = strrchr(program_invocation_name, '\\');
    if (arg)
        return strdup(arg+1);

    return strdup(program_invocation_name);
  }

  std::string_view getExecutableName() {
    static std::string s_execName = []() -> std::string
    {
      const char *mesaExecutableEnv = getenv("MESA_DRICONF_EXECUTABLE_OVERRIDE");
      if (mesaExecutableEnv && *mesaExecutableEnv) {
        fprintf(stderr, "[Gamescope WSI] Executable name overriden by MESA_DRICONF_EXECUTABLE_OVERRIDE: %s\n", mesaExecutableEnv);
        return mesaExecutableEnv;
      }

      const char *mesaProcessName = getenv("MESA_PROCESS_NAME");
      if (mesaProcessName && *mesaProcessName) {
        fprintf(stderr, "[Gamescope WSI] Executable name overriden by MESA_PROCESS_NAME: %s\n", mesaExecutableEnv);
        return mesaProcessName;
      }

      std::string name;
      {
        char *programNameCStr = __getProgramName();
        name = programNameCStr;
        free(programNameCStr);
      }

      fprintf(stderr, "[Gamescope WSI] Executable name: %s\n", name.c_str());
      return name;
    }();

    return s_execName;
  }

  static GamescopeLayerClient::Flags defaultLayerClientFlags(const VkApplicationInfo *pApplicationInfo, uint32_t appid) {
    GamescopeLayerClient::Flags flags = 0;

    const char *bypassEnv = getenv("GAMESCOPE_WSI_FORCE_BYPASS");
    if (bypassEnv && *bypassEnv && atoi(bypassEnv) != 0)
      flags |= GamescopeLayerClient::Flag::ForceBypass;

    // My Little Pony: A Maretime Bay Adventure picks a HDR colorspace if available,
    // but does not render as HDR at all.
    if (appid == 1600780)
      flags |= GamescopeLayerClient::Flag::DisableHDR;

    const char *frameLimiterAwareEnv = getenv("GAMESCOPE_WSI_FRAME_LIMITER_AWARE");
    if (frameLimiterAwareEnv && *frameLimiterAwareEnv) {
      if (atoi(frameLimiterAwareEnv) != 0)
        flags |= GamescopeLayerClient::Flag::FrameLimiterAware;
    } else if (pApplicationInfo && pApplicationInfo->pEngineName) {
      // This matches regular vkd3d, not just vkd3d-proton as well...
      // Oh well... /shrug.
      if ((pApplicationInfo->pEngineName == "vkd3d"sv && pApplicationInfo->engineVersion >= VK_MAKE_VERSION(2, 12, 0)) ||
          (pApplicationInfo->pEngineName == "DXVK"sv  && pApplicationInfo->engineVersion >= VK_MAKE_VERSION(2, 3,  0))) {
        flags |= GamescopeLayerClient::Flag::FrameLimiterAware;
      }
    }

    std::string_view executable = getExecutableName();

    // Work around various Croteam games not handling
    // suboptimal and swapchain extent correctly.
    if (executable == "Talos"sv ||
        executable == "Talos_Unrestricted"sv ||
        executable == "Talos_VR"sv ||
        executable == "Talos_Unrestricted_VR"sv ||
        executable == "Sam2017"sv ||
        executable == "Sam2017_Unrestricted"sv) {
      flags |= GamescopeLayerClient::Flag::ForceSwapchainExtent;
      flags |= GamescopeLayerClient::Flag::NoSuboptimal;
    }

    {
      const char *forceSwapchainExtentEnvVar = getenv("vk_wsi_force_swapchain_to_current_extent");
      if (forceSwapchainExtentEnvVar && *forceSwapchainExtentEnvVar) {
        if (forceSwapchainExtentEnvVar == "true"sv)
          flags |= GamescopeLayerClient::Flag::ForceSwapchainExtent;
        else
          flags &= ~GamescopeLayerClient::Flag::ForceSwapchainExtent;
      }
    }

    {
      const char *ignoreSuboptimalEnvVar = getenv("vk_x11_ignore_suboptimal");
      if (ignoreSuboptimalEnvVar && *ignoreSuboptimalEnvVar) {
        if (ignoreSuboptimalEnvVar == "true"sv)
          flags |= GamescopeLayerClient::Flag::NoSuboptimal;
        else
          flags &= ~GamescopeLayerClient::Flag::NoSuboptimal;
      }
    }

    return flags;
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

  static bool gamescopeIsForcingFifo() {
    return gamescopeFrameLimiterOverride() == 1;
  }

  struct GamescopeWaylandObjects {
    wl_compositor* compositor;
    gamescope_swapchain_factory_v2* gamescopeSwapchainFactory;

    static GamescopeWaylandObjects get(wl_display *display) {
      wl_registry *registry = wl_display_get_registry(display);
      if (!registry)
        return {};
      GamescopeWaylandObjects waylandObjects{};
      wl_registry_add_listener(registry, &s_registryListener, reinterpret_cast<void *>(&waylandObjects));
      // Dispatch then roundtrip to get registry info.
      wl_display_dispatch(display);
      wl_display_roundtrip(display);
      wl_registry_destroy(registry);

      return waylandObjects;
    }

    bool valid() const { return compositor && gamescopeSwapchainFactory; }

    static const wl_registry_listener s_registryListener;
  };

  const wl_registry_listener GamescopeWaylandObjects::s_registryListener = {
    .global = [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
      auto objects = reinterpret_cast<GamescopeWaylandObjects *>(data);

      if (interface == "wl_compositor"sv) {
        objects->compositor = reinterpret_cast<wl_compositor *>(
          wl_registry_bind(registry, name, &wl_compositor_interface, version));
      } else if (interface == "gamescope_swapchain_factory_v2"sv) {
        objects->gamescopeSwapchainFactory = reinterpret_cast<gamescope_swapchain_factory_v2 *>(
          wl_registry_bind(registry, name, &gamescope_swapchain_factory_v2_interface, version));
      }
    },
    .global_remove = [](void* data, wl_registry* registry, uint32_t name) {
    },
  };

  struct GamescopeInstanceData {
    wl_display* display;
    uint32_t appId = 0;
    std::string engineName;
    GamescopeLayerClient::Flags flags = 0;
  };
  VKROOTS_DEFINE_SYNCHRONIZED_MAP_TYPE(GamescopeInstance, VkInstance);

  struct GamescopeSurfaceData {
    VkInstance instance;
    wl_display *display;
    GamescopeWaylandObjects waylandObjects;
    VkSurfaceKHR fallbackSurface;
    wl_surface* surface;

    xcb_connection_t* connection;
    xcb_window_t window;
    GamescopeLayerClient::Flags flags;
    bool hdrOutput;

    // Cached for comparison.
    std::optional<VkRect2D> cachedWindowRect;

    bool isWayland() const {
      // Is native Wayland?
      return connection == nullptr;
    }

    bool frameLimiterAware() const {
      return !!(flags & GamescopeLayerClient::Flag::FrameLimiterAware);
    }

    bool shouldExposeHDR() const {
      const bool hdrAllowed = !(flags & GamescopeLayerClient::Flag::DisableHDR);
      return hdrOutput && hdrAllowed;
    }

    bool canBypassXWayland() {
      if (isWayland())
        return true;

      auto rect = xcb::getWindowRect(connection, window);
      auto largestObscuringWindowSize = xcb::getLargestObscuringChildWindowSize(connection, window);
      auto toplevelWindow = xcb::getToplevelWindow(connection, window);
      if (!rect || !largestObscuringWindowSize || !toplevelWindow) {
        fprintf(stderr, "[Gamescope WSI] canBypassXWayland: failed to get window info for window 0x%x.\n", window);
        return false;
      }

      cachedWindowRect = *rect;

      auto toplevelRect = xcb::getWindowRect(connection, *toplevelWindow);
      if (!toplevelRect) {
        fprintf(stderr, "[Gamescope WSI] canBypassXWayland: failed to get window info for window 0x%x.\n", window);
        return false;
      }

      // Some games do things like have a 1280x800 top-level window and
      // a 1280x720 child window for "fullscreen".
      // To avoid Glamor work on the XWayland side of things, have a
      // flag to force bypassing this.
      if (!!(flags & GamescopeLayerClient::Flag::ForceBypass))
        return true;

      // If we have any child windows obscuring us bigger than 1x1,
      // then we cannot flip.
      // (There can be dummy composite redirect windows and whatever.)
      if (largestObscuringWindowSize->width > 1 || largestObscuringWindowSize->height > 1) {
#if GAMESCOPE_WSI_BYPASS_DEBUG
        fprintf(stderr, "[Gamescope WSI] Largest obscuring window size: %u %u\n", largestObscuringWindowSize->width, largestObscuringWindowSize->height);
#endif
        return false;
      }

      // If this window is not within 2px margin of error for the size of
      // it's top level window, then it cannot be flipped.
      //
      // Some games like Halo Infinite, make a child window that is 1280x802px
      // I have no idea how that happens, or whether its an app or Wine bug or not.
      if (*toplevelWindow != window) {
        if (iabs(rect->offset.x) > 1 ||
            iabs(rect->offset.y) > 1 ||
            iabs(int32_t(toplevelRect->extent.width)  - int32_t(rect->extent.width)) > 2 ||
            iabs(int32_t(toplevelRect->extent.height) - int32_t(rect->extent.height)) > 2) {
  #if GAMESCOPE_WSI_BYPASS_DEBUG
          fprintf(stderr, "[Gamescope WSI] Not within 1px margin of error. Offset: %d %d Extent: %u %u vs %u %u\n",
            rect->offset.x, rect->offset.y,
            toplevelRect->extent.width, toplevelRect->extent.height,
            rect->extent.width, rect->extent.height);
  #endif
          return false;
        }
      }

      // I want to add more checks wrt. composite redirects and such here,
      // but it seems what is exposed in xcb_composite is quite limited.
      // So let's see how it goes for now. :-)
      // Come back to this eventually.
      return true;
    }
  };
  VKROOTS_DEFINE_SYNCHRONIZED_MAP_TYPE(GamescopeSurface, VkSurfaceKHR);

  struct GamescopeSwapchainData {
    gamescope_swapchain *object;
    wl_display* display;
    VkSurfaceKHR surface; // Always the Gamescope Surface surface -- so the Wayland one.
    bool isWayland;
    bool isBypassingXWayland;
    bool forceFifo;
    VkPresentModeKHR presentMode;
    VkExtent2D extent;
    uint32_t serverId = 0;
    bool retired = false;

    std::unique_ptr<std::mutex> presentTimingMutex = std::make_unique<std::mutex>();
    std::vector<VkPastPresentationTimingGOOGLE> pastPresentTimings;
    uint64_t refreshCycle = 16'666'666;
  };
  VKROOTS_DEFINE_SYNCHRONIZED_MAP_TYPE(GamescopeSwapchain, VkSwapchainKHR);
  static constexpr gamescope_swapchain_listener s_swapchainListener = {
    .past_present_timing = [](
            void *data,
            gamescope_swapchain *object,
            uint32_t present_id,
            uint32_t desired_present_time_hi,
            uint32_t desired_present_time_lo,
            uint32_t actual_present_time_hi,
            uint32_t actual_present_time_lo,
            uint32_t earliest_present_time_hi,
            uint32_t earliest_present_time_lo,
            uint32_t present_margin_hi,
            uint32_t present_margin_lo) {
      GamescopeSwapchainData *swapchain = reinterpret_cast<GamescopeSwapchainData*>(data);
      std::unique_lock lock(*swapchain->presentTimingMutex);
      swapchain->pastPresentTimings.emplace_back(VkPastPresentationTimingGOOGLE {
        .presentID           = present_id,
        .desiredPresentTime  = (uint64_t(desired_present_time_hi) << 32) | desired_present_time_lo,
        .actualPresentTime   = (uint64_t(actual_present_time_hi) << 32) | actual_present_time_lo,
        .earliestPresentTime = (uint64_t(earliest_present_time_hi) << 32) | earliest_present_time_lo,
        .presentMargin       = (uint64_t(present_margin_hi) << 32) | present_margin_lo
      });
      // Remove the first element if we are already at the max size.
      if (swapchain->pastPresentTimings.size() >= MaxPastPresentationTimes)
        swapchain->pastPresentTimings.erase(swapchain->pastPresentTimings.begin());
    },

    .refresh_cycle = [](
            void *data,
            gamescope_swapchain *object,
            uint32_t refresh_cycle_hi,
            uint32_t refresh_cycle_lo) {
      GamescopeSwapchainData *swapchain = reinterpret_cast<GamescopeSwapchainData*>(data);
      {
        std::unique_lock lock(*swapchain->presentTimingMutex);
        swapchain->refreshCycle = (uint64_t(refresh_cycle_hi) << 32) | refresh_cycle_lo;
      }
      fprintf(stderr, "[Gamescope WSI] Swapchain received new refresh cycle: %.2fms\n", swapchain->refreshCycle / 1'000'000.0);
    },

    .retired = [](
            void *data,
            gamescope_swapchain *object) {
      GamescopeSwapchainData *swapchain = reinterpret_cast<GamescopeSwapchainData*>(data);
      {
        swapchain->retired = true;
      }
      fprintf(stderr, "[Gamescope WSI] Swapchain retired\n");
    },
  };

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

      setenv("vk_xwayland_wait_ready", "false", 0);
      setenv("vk_khr_present_wait", "true", 0);

      VkResult result = pfnCreateInstanceProc(&createInfo, pAllocator, pInstance);
      if (result != VK_SUCCESS)
        return result;

      wl_display *display = wl_display_connect(gamescopeWaylandSocket());
      if (!display) {
        fprintf(stderr, "[Gamescope WSI] Failed to connect to gamescope socket: %s. Bypass layer will be unavailable.\n", gamescopeWaylandSocket());
        return result;
      }

      {
        if (pCreateInfo->pApplicationInfo) {
          fprintf(stderr, "[Gamescope WSI] Application info:\n");
          fprintf(stderr, "  pApplicationName: %s\n", pCreateInfo->pApplicationInfo->pApplicationName);
          fprintf(stderr, "  applicationVersion: %u\n", pCreateInfo->pApplicationInfo->applicationVersion);
          fprintf(stderr, "  pEngineName: %s\n", pCreateInfo->pApplicationInfo->pEngineName);
          fprintf(stderr, "  engineVersion: %u\n", pCreateInfo->pApplicationInfo->engineVersion);
          fprintf(stderr, "  apiVersion: %u\n", pCreateInfo->pApplicationInfo->apiVersion);
        } else {
          fprintf(stderr, "[Gamescope WSI] No application info given.\n");
        }
      }
      
      {
        uint32_t appId = clientAppId();

        std::string engineName;
        if (pCreateInfo->pApplicationInfo && pCreateInfo->pApplicationInfo->pEngineName)
          engineName = pCreateInfo->pApplicationInfo->pEngineName;

        auto state = GamescopeInstance::create(*pInstance, GamescopeInstanceData {
          .display = display,
          .appId   = appId,
          .engineName = engineName,
          .flags   = defaultLayerClientFlags(pCreateInfo->pApplicationInfo, appId),
        });

        // If we know at instance creation time we should disable HDR, force off
        // DXVK_HDR now.
        if (state->flags & GamescopeLayerClient::Flag::DisableHDR)
          setenv("DXVK_HDR", "0", 1);
      }

      // Work around the Mesa implementation of this being broken.
      // ( https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/31134 )
      setenv("vk_wsi_force_swapchain_to_current_extent", "false", 0);

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

    static VkResult CreateDevice(
      const vkroots::VkInstanceDispatch* pDispatch,
            VkPhysicalDevice             physicalDevice,
      const VkDeviceCreateInfo*          pCreateInfo,
      const VkAllocationCallbacks*       pAllocator,
            VkDevice*                    pDevice) {
      VkDeviceCreateInfo deviceCreateInfo = *pCreateInfo;

      std::vector<const char *> extensions(pCreateInfo->ppEnabledExtensionNames, pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);
      if (!contains(extensions, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME))
        extensions.push_back(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
      deviceCreateInfo.ppEnabledExtensionNames = extensions.data();
      deviceCreateInfo.enabledExtensionCount   = uint32_t(extensions.size());

      vkroots::ChainPatcher<VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT>
        maintenance1Patcher(&deviceCreateInfo, [&](VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT *pMaintenance1)
      {
        fprintf(stderr, "[Gamescope WSI] Forcing on VK_EXT_swapchain_maintenance1.\n");
        pMaintenance1->swapchainMaintenance1 = VK_TRUE;
        return true;
      });

      return pDispatch->CreateDevice(physicalDevice, &deviceCreateInfo, pAllocator, pDevice);
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

    static VkResult CreateWaylandSurfaceKHR(
      const vkroots::VkInstanceDispatch*   pDispatch,
            VkInstance                     instance,
      const VkWaylandSurfaceCreateInfoKHR* pCreateInfo,
      const VkAllocationCallbacks*         pAllocator,
            VkSurfaceKHR*                  pSurface) {
      auto gamescopeInstance = GamescopeInstance::get(instance);
      if (!gamescopeInstance)
        return pDispatch->CreateWaylandSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);

      GamescopeWaylandObjects waylandObjects = GamescopeWaylandObjects::get(pCreateInfo->display);
      if (!waylandObjects.valid()) {
        fprintf(stderr, "[Gamescope WSI] Failed to get Wayland objects\n");
        return VK_ERROR_SURFACE_LOST_KHR;
      }

      VkResult res = pDispatch->CreateWaylandSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
      if (res != VK_SUCCESS)
        return res;

      auto gamescopeSurface = GamescopeSurface::create(*pSurface, GamescopeSurfaceData {
        .instance        = instance,
        .display         = pCreateInfo->display,
        .waylandObjects  = waylandObjects,
        .surface         = pCreateInfo->surface,
        .flags           = gamescopeInstance->flags,
        .hdrOutput       = false, // XXXX FIXME FIXME FIXME //hdrOutput,
      });

      DumpGamescopeSurfaceState(gamescopeInstance, gamescopeSurface);

      return res;
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

      if (!gamescopeSurface->isWayland()) {
        auto rect = xcb::getWindowRect(gamescopeSurface->connection, gamescopeSurface->window);
        if (!rect)
          return VK_ERROR_SURFACE_LOST_KHR;

        pSurfaceCapabilities->currentExtent = rect->extent;
      }
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

      // Incomplete writes here, do not return VK_INCOMPLETE.
      if (gamescopeIsForcingFifo() && gamescopeSurface->frameLimiterAware()) {
        const auto *pPresentMode = vkroots::FindInChain<VkSurfacePresentModeEXT>(pSurfaceInfo);
        const std::array<VkPresentModeKHR, 1> s_SingleMode = {{
          pPresentMode ? pPresentMode->presentMode : VK_PRESENT_MODE_FIFO_KHR,
        }};
        auto [pPresentModeCompat, pPresentModeCompatParent] = vkroots::RemoveFromChain<VkSurfacePresentModeCompatibilityEXT>(pSurfaceCapabilities);
        if (pPresentModeCompat)
          vkroots::helpers::array(s_SingleMode, &pPresentModeCompat->presentModeCount, pPresentModeCompat->pPresentModes);

        VkResult res = VK_SUCCESS;
        if ((res = pDispatch->GetPhysicalDeviceSurfaceCapabilities2KHR(physicalDevice, pSurfaceInfo, pSurfaceCapabilities)) != VK_SUCCESS)
          return res;

        if (pPresentModeCompat)
          vkroots::AddToChain(pPresentModeCompatParent, pPresentModeCompat);
      } else {
        VkResult res = VK_SUCCESS;
        if ((res = pDispatch->GetPhysicalDeviceSurfaceCapabilities2KHR(physicalDevice, pSurfaceInfo, pSurfaceCapabilities)) != VK_SUCCESS)
          return res;
      }

      if (!gamescopeSurface->isWayland()) {
        auto rect = xcb::getWindowRect(gamescopeSurface->connection, gamescopeSurface->window);
        if (!rect)
          return VK_ERROR_SURFACE_LOST_KHR;

        pSurfaceCapabilities->surfaceCapabilities.currentExtent = rect->extent;
      }
      pSurfaceCapabilities->surfaceCapabilities.minImageCount = getMinImageCount();

      return VK_SUCCESS;
    }

    static void GetPhysicalDeviceFeatures2(
      const vkroots::VkInstanceDispatch* pDispatch,
            VkPhysicalDevice             physicalDevice,
            VkPhysicalDeviceFeatures2*   pFeatures) {
      pDispatch->GetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
    }

    static void GetPhysicalDeviceFeatures2KHR(
      const vkroots::VkInstanceDispatch* pDispatch,
            VkPhysicalDevice             physicalDevice,
            VkPhysicalDeviceFeatures2*   pFeatures) {
      GetPhysicalDeviceFeatures2(pDispatch, physicalDevice, pFeatures);
    }

    static VkResult GetPhysicalDeviceSurfacePresentModesKHR(
      const vkroots::VkInstanceDispatch* pDispatch,
        VkPhysicalDevice                 physicalDevice,
        VkSurfaceKHR                     surface,
        uint32_t*                        pPresentModeCount,
        VkPresentModeKHR*                pPresentModes) {
      static constexpr std::array<VkPresentModeKHR, 1> s_FifoPresentModes = {{
        VK_PRESENT_MODE_FIFO_KHR,
      }};

      if (auto state = GamescopeSurface::get(surface)) {
        if (gamescopeIsForcingFifo() && state->frameLimiterAware())
          return vkroots::helpers::array(s_FifoPresentModes, pPresentModeCount, pPresentModes);
      }

      return pDispatch->GetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, pPresentModeCount, pPresentModes);
    }

    static void DestroySurfaceKHR(
      const vkroots::VkInstanceDispatch* pDispatch,
            VkInstance                   instance,
            VkSurfaceKHR                 surface,
      const VkAllocationCallbacks*       pAllocator) {
      if (auto state = GamescopeSurface::get(surface)) {
        pDispatch->DestroySurfaceKHR(instance, state->fallbackSurface, pAllocator);
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
      static constexpr std::array<VkExtensionProperties, 2> s_LayerExposedExts = {{
        { VK_EXT_HDR_METADATA_EXTENSION_NAME,
          VK_EXT_HDR_METADATA_SPEC_VERSION },
        { VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME,
          VK_GOOGLE_DISPLAY_TIMING_SPEC_VERSION },
      }};

      if (pLayerName) {
        if (pLayerName == "VK_LAYER_FROG_gamescope_wsi"sv) {
          return vkroots::helpers::array(s_LayerExposedExts, pPropertyCount, pProperties);
        } else {
          return pDispatch->EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
        }
      }

      VkResult result = vkroots::helpers::append(
        pDispatch->EnumerateDeviceExtensionProperties,
        s_LayerExposedExts,
        pPropertyCount,
        pProperties,
        physicalDevice,
        pLayerName);

      return result;
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

      GamescopeWaylandObjects waylandObjects = GamescopeWaylandObjects::get(gamescopeInstance->display);
      if (!waylandObjects.valid()) {
        fprintf(stderr, "[Gamescope WSI] Failed to get Wayland objects\n");
        return VK_ERROR_SURFACE_LOST_KHR;
      }

      wl_surface* waylandSurface = wl_compositor_create_surface(waylandObjects.compositor);
      if (!waylandSurface) {
        fprintf(stderr, "[Gamescope WSI] Failed to create wayland surface - xid: 0x%x\n", window);
        return VK_ERROR_SURFACE_LOST_KHR;
      }

      GamescopeLayerClient::Flags flags = gamescopeInstance->flags;
      if (auto prop = xcb::getPropertyValue<GamescopeLayerClient::Flags>(connection, "GAMESCOPE_LAYER_CLIENT_FLAGS"sv))
        flags = *prop;

      bool hdrOutput = false;
      if (auto prop = xcb::getPropertyValue<uint32_t>(connection, "GAMESCOPE_HDR_OUTPUT_FEEDBACK"sv))
        hdrOutput = !!*prop;

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
        .display         = gamescopeInstance->display,
        .waylandObjects  = waylandObjects,
        .fallbackSurface = fallbackSurface,
        .surface         = waylandSurface,
        .connection      = connection,
        .window          = window,
        .flags           = flags,
        .hdrOutput       = hdrOutput,
      });

      DumpGamescopeSurfaceState(gamescopeInstance, gamescopeSurface);

      return result;
    }

    static void DumpGamescopeSurfaceState(GamescopeInstance& instance, GamescopeSurface& surface) {
      fprintf(stderr, "[Gamescope WSI] Surface state:\n");
      fprintf(stderr, "  steam app id:                  %u\n", instance->appId);
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

  };

  class VkDeviceOverrides {
  public:
    static void DestroySwapchainKHR(
      const vkroots::VkDeviceDispatch* pDispatch,
            VkDevice                   device,
            VkSwapchainKHR             swapchain,
      const VkAllocationCallbacks*     pAllocator) {
      if (auto state = GamescopeSwapchain::get(swapchain)) {
        gamescope_swapchain_destroy(state->object);
      }
      GamescopeSwapchain::remove(swapchain);
      fprintf(stderr, "[Gamescope WSI] Destroying swapchain: %p\n", reinterpret_cast<void*>(swapchain));
      pDispatch->DestroySwapchainKHR(device, swapchain, pAllocator);
      fprintf(stderr, "[Gamescope WSI] Destroyed swapchain: %p\n", reinterpret_cast<void*>(swapchain));
    }

    static VkResult CreateSwapchainKHR(
      const vkroots::VkDeviceDispatch* pDispatch,
            VkDevice                   device,
      const VkSwapchainCreateInfoKHR*  pCreateInfo,
      const VkAllocationCallbacks*     pAllocator,
            VkSwapchainKHR*            pSwapchain) {
      auto gamescopeSurface = GamescopeSurface::get(pCreateInfo->surface);

      if (!gamescopeSurface) {
        static bool s_warned = false;
        if (!s_warned) {
          int messageId = -1;
          messagey::ShowSimple(
            "CreateSwapchainKHR: Creating swapchain for non-Gamescope swapchain.\nHooking has failed somewhere!\nYou may have a bad Vulkan layer interfering.\nPress OK to try to power through this error, or Cancel to stop.",
            "Gamescope WSI Layer Error",
            messagey::MessageBoxFlag::Warning | messagey::MessageBoxFlag::Simple_Cancel | messagey::MessageBoxFlag::Simple_OK,
            &messageId);
          if (messageId == 0) // Cancel
            abort();
          s_warned = true;
        }
        return pDispatch->CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
      }

      const bool canBypass = gamescopeSurface->canBypassXWayland();

      VkSwapchainCreateInfoKHR swapchainInfo = *pCreateInfo;

      if (pCreateInfo->oldSwapchain) {
        if (auto gamescopeSwapchain = GamescopeSwapchain::get(pCreateInfo->oldSwapchain)) {
          gamescopeSwapchain->retired = true;
          // If we are going to/from being able to bypass XWayland, make sure
          // we NULL out oldSwapchain, as they'll be for different surfaces and swapchain types.
          if (gamescopeSwapchain->isBypassingXWayland != canBypass)
            swapchainInfo.oldSwapchain = VK_NULL_HANDLE;
        }
      }

      if (gamescopeSurface->flags & GamescopeLayerClient::Flag::ForceSwapchainExtent) {
        if (!gamescopeSurface->isWayland()) {
          auto rect = xcb::getWindowRect(gamescopeSurface->connection, gamescopeSurface->window);
          if (!rect)
            return VK_ERROR_SURFACE_LOST_KHR;

          swapchainInfo.imageExtent = rect->extent;
        }
      }

      // If we can't flip, fallback to the regular XCB surface on the XCB window.
      if (!canBypass)
        swapchainInfo.surface = gamescopeSurface->fallbackSurface;

      // We yolo to 3 min images always in Gamescope WSI, regardless of the underlying implementation.
      // Anyway, deal with present modes passed in...
      vkroots::ChainPatcher<VkSwapchainPresentModesCreateInfoEXT>
        presentModePatcher(&swapchainInfo, [&](VkSwapchainPresentModesCreateInfoEXT *pPresentModesCreateInfo)
      {
        // Always send MAILBOX as the mode to the driver, as we implement FIFO ourselves -- using the
        // Gamescope swapchain protocol.
        static constexpr std::array<VkPresentModeKHR, 1> s_MailboxMode = {{
          VK_PRESENT_MODE_MAILBOX_KHR,
        }};
        pPresentModesCreateInfo->presentModeCount = uint32_t(s_MailboxMode.size());
        pPresentModesCreateInfo->pPresentModes    = s_MailboxMode.data();
        return true;
      });

      // Force the colorspace to sRGB before sending to the driver.
      swapchainInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      // We always send MAILBOX to the driver.
      swapchainInfo.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;

      uint32_t minImageCount = swapchainInfo.minImageCount;
      if (getEnsureMinImageCount())
        minImageCount = std::max(getMinImageCount(), minImageCount);
      swapchainInfo.minImageCount = minImageCount;

      fprintf(stderr, "[Gamescope WSI] Creating swapchain for xid: 0x%0x - oldSwapchain: %p - provided minImageCount: %u - minImageCount: %u - format: %s - colorspace: %s - flip: %s\n",
        gamescopeSurface->window,
        reinterpret_cast<void*>(pCreateInfo->oldSwapchain),
        pCreateInfo->minImageCount,
        minImageCount,
        vkroots::helpers::enumString(pCreateInfo->imageFormat),
        vkroots::helpers::enumString(pCreateInfo->imageColorSpace),
        canBypass ? "true" : "false");

      // Check for VkFormat support and return VK_ERROR_INITIALIZATION_FAILED
      // if that VkFormat is unsupported for the underlying surface.
      {
        std::vector<VkSurfaceFormatKHR> supportedSurfaceFormats;
        vkroots::helpers::enumerate(
          pDispatch->pPhysicalDeviceDispatch->pInstanceDispatch->GetPhysicalDeviceSurfaceFormatsKHR,
          supportedSurfaceFormats,
          pDispatch->PhysicalDevice,
          swapchainInfo.surface);

        bool supportedSwapchainFormat = std::ranges::any_of(
          supportedSurfaceFormats,
          std::bind_front(std::equal_to{}, swapchainInfo.imageFormat),
          &VkSurfaceFormatKHR::format)  ;

        if (!supportedSwapchainFormat) {
          fprintf(stderr, "[Gamescope WSI] Refusing to make swapchain (unsupported VkFormat) for xid: 0x%0x - format: %s - colorspace: %s - flip: %s\n",
            gamescopeSurface->window,
            vkroots::helpers::enumString(pCreateInfo->imageFormat),
            vkroots::helpers::enumString(pCreateInfo->imageColorSpace),
            canBypass ? "true" : "false");

          return VK_ERROR_INITIALIZATION_FAILED;
        }
      }

      uint32_t serverId = ~0u;
      if (!gamescopeSurface->isWayland()) {
        auto oServerId = xcb::getPropertyValue<uint32_t>(gamescopeSurface->connection, "GAMESCOPE_XWAYLAND_SERVER_ID"sv);
        if (!oServerId) {
          fprintf(stderr, "[Gamescope WSI] Failed to get Xwayland server id. Failing swapchain creation.\n");
          return VK_ERROR_SURFACE_LOST_KHR;
        }
        serverId = *oServerId;
      }

      auto gamescopeInstance = GamescopeInstance::get(gamescopeSurface->instance);
      if (!gamescopeInstance) {
        fprintf(stderr, "[Gamescope WSI] CreateSwapchainKHR: Instance for swapchain was already destroyed. (App use after free).\n");
        return VK_ERROR_SURFACE_LOST_KHR;
      }

      VkResult result = pDispatch->CreateSwapchainKHR(device, &swapchainInfo, pAllocator, pSwapchain);
      if (result != VK_SUCCESS) {
        fprintf(stderr, "[Gamescope WSI] Failed to create swapchain - vr: %s xid: 0x%x\n", vkroots::helpers::enumString(result), gamescopeSurface->window);
        return result;
      }

      gamescope_swapchain *gamescopeSwapchainObject = gamescope_swapchain_factory_v2_create_swapchain(
        gamescopeSurface->waylandObjects.gamescopeSwapchainFactory,
        gamescopeSurface->surface);

      {
        auto gamescopeSwapchain = GamescopeSwapchain::create(*pSwapchain, GamescopeSwapchainData{
          .object              = gamescopeSwapchainObject,
          .display             = gamescopeSurface->display,
          .surface             = pCreateInfo->surface, // Always the Wayland side surface.
          .isWayland           = gamescopeSurface->isWayland(),
          .isBypassingXWayland = canBypass,
          .forceFifo           = gamescopeIsForcingFifo(), // Were we forcing fifo when this swapchain was made?
          .presentMode         = pCreateInfo->presentMode, // The new present mode.
          .extent              = pCreateInfo->imageExtent,
          .serverId            = serverId,
        });
        gamescopeSwapchain->pastPresentTimings.reserve(MaxPastPresentationTimes);

        gamescope_swapchain_add_listener(gamescopeSwapchainObject, &s_swapchainListener, reinterpret_cast<void*>(gamescopeSwapchain.get()));
      }

      uint32_t imageCount = 0;
      pDispatch->GetSwapchainImagesKHR(device, *pSwapchain, &imageCount, nullptr);

      fprintf(stderr, "[Gamescope WSI] Created swapchain for xid: 0x%0x swapchain: %p - imageCount: %u\n",
        gamescopeSurface->window,
        reinterpret_cast<void*>(*pSwapchain),
        imageCount);

      gamescope_swapchain_swapchain_feedback(
        gamescopeSwapchainObject,
        imageCount,
        uint32_t(pCreateInfo->imageFormat),
        uint32_t(pCreateInfo->imageColorSpace),
        uint32_t(pCreateInfo->compositeAlpha),
        uint32_t(pCreateInfo->preTransform),
        uint32_t(pCreateInfo->clipped),
        gamescopeInstance->engineName.c_str());

      return VK_SUCCESS;
    }

    static VkResult AcquireNextImageKHR(
      const vkroots::VkDeviceDispatch* pDispatch,
            VkDevice                   device,
            VkSwapchainKHR             swapchain,
            uint64_t                   timeout,
            VkSemaphore                semaphore,
            VkFence                    fence,
            uint32_t*                  pImageIndex) {
      VkAcquireNextImageInfoKHR acquireInfo = {
        .sType      = VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR,
        .swapchain  = swapchain,
        .timeout    = timeout,
        .semaphore  = semaphore,
        .fence      = fence,
        .deviceMask = 0x1,
      };

      return AcquireNextImage2KHR(pDispatch, device, &acquireInfo, pImageIndex);
    }

    static VkResult AcquireNextImage2KHR(
      const vkroots::VkDeviceDispatch* pDispatch,
            VkDevice                   device,
      const VkAcquireNextImageInfoKHR* pAcquireInfo,
            uint32_t*                  pImageIndex) {
      if (auto gamescopeSwapchain = GamescopeSwapchain::get(pAcquireInfo->swapchain)) {
        if (gamescopeSwapchain->retired)
          return VK_ERROR_OUT_OF_DATE_KHR;
      }

      return pDispatch->AcquireNextImage2KHR(device, pAcquireInfo, pImageIndex);
    }

    static VkResult QueuePresentKHR(
      const vkroots::VkDeviceDispatch* pDispatch,
            VkQueue                    queue,
      const VkPresentInfoKHR*          pPresentInfo) {
      VkPresentInfoKHR presentInfo = *pPresentInfo;

      bool forceFifo = gamescopeIsForcingFifo();

      auto pPresentTimes = vkroots::FindInChain<const VkPresentTimesInfoGOOGLE>(&presentInfo);

      wl_display *display = nullptr;
      for (uint32_t i = 0; i < presentInfo.swapchainCount; i++) {
        if (auto gamescopeSwapchain = GamescopeSwapchain::get(presentInfo.pSwapchains[i])) {
          if (gamescopeSwapchain->retired) {
            return VK_ERROR_OUT_OF_DATE_KHR;
          }

          if (pPresentTimes && pPresentTimes->pTimes) {
            assert(pPresentTimes->swapchainCount == presentInfo.swapchainCount);

#if GAMESCOPE_WSI_DISPLAY_TIMING_DEBUG
            fprintf(stderr, "[Gamescope WSI] QueuePresentKHR: presentID: %u - desiredPresentTime: %lu - now: %lu\n", pPresentTimes->pTimes[i].presentID, pPresentTimes->pTimes[i].desiredPresentTime, getTimeMonotonic());
#endif
            gamescope_swapchain_set_present_time(
              gamescopeSwapchain->object,
              pPresentTimes->pTimes[i].presentID,
              pPresentTimes->pTimes[i].desiredPresentTime >> 32,
              pPresentTimes->pTimes[i].desiredPresentTime & 0xffffffff);
          }

          assert(display == nullptr || display == gamescopeSwapchain->display);
          display = gamescopeSwapchain->display;
        }
      }

      // All VkSurfaceKHR's come from the same VkInstance, so we only need to check one surface.
      bool frameLimiterAware = [&]() {
        for (uint32_t i = 0; i < presentInfo.swapchainCount; i++) {
          if (auto gamescopeSwapchain = GamescopeSwapchain::get(presentInfo.pSwapchains[i])) {
              auto gamescopeSurface = GamescopeSurface::get(gamescopeSwapchain->surface);
              if (gamescopeSurface)
                return gamescopeSurface->frameLimiterAware();
          }
        }
        return false;
      }();

      // Grab the actual intended present modes.
      std::optional<VkSwapchainPresentModeInfoEXT> oOriginalPresentModeInfo;
      const auto *pPresentModeInfo = vkroots::FindInChain<VkSwapchainPresentModeInfoEXT>(&presentInfo);
      if (pPresentModeInfo)
        oOriginalPresentModeInfo = *pPresentModeInfo;

      // Force all present modes to MAILBOX to the underlying driver
      // We implement fifo ourselves.
      vkroots::ChainPatcher<VkSwapchainPresentModeInfoEXT, std::vector<VkPresentModeKHR>>
        presentModePatcher(&presentInfo, [&](std::vector<VkPresentModeKHR>& mailboxModes, VkSwapchainPresentModeInfoEXT *pMaintenance1)
      {
        for (uint32_t i = 0; i < presentInfo.swapchainCount; i++) {
          if (auto gamescopeSwapchain = GamescopeSwapchain::get(presentInfo.pSwapchains[i])) {
            mailboxModes.emplace_back(VK_PRESENT_MODE_MAILBOX_KHR);
          }
        }

        pMaintenance1->pPresentModes = mailboxModes.data();
        return true;
      });


      if (display) {
        waylandPumpEvents(display);
      } else {
        static bool s_warned = false;
        if (!s_warned) {
          int messageId = -1;
          messagey::ShowSimple(
            "QueuePresentKHR: Attempting to present to a non-hooked swapchain.\nHooking has failed somewhere!\nYou may have a bad Vulkan layer interfering.\nPress OK to try to power through this error, or Cancel to stop.",
            "Gamescope WSI Layer Error",
            messagey::MessageBoxFlag::Warning | messagey::MessageBoxFlag::Simple_Cancel | messagey::MessageBoxFlag::Simple_OK,
            &messageId);
          if (messageId == 0) // Cancel
            abort();
          s_warned = true;
        }
      }

      for (uint32_t i = 0; i < presentInfo.swapchainCount; i++) {
        if (auto gamescopeSwapchain = GamescopeSwapchain::get(presentInfo.pSwapchains[i])) {
          auto gamescopeSurface = GamescopeSurface::get(gamescopeSwapchain->surface);
          if (gamescopeSwapchain->isWayland || gamescopeSwapchain->isBypassingXWayland) {
            if (!gamescopeSwapchain->isWayland) {
              gamescope_swapchain_override_window_content(gamescopeSwapchain->object, gamescopeSwapchain->serverId, gamescopeSurface->window);
            }
            VkPresentModeKHR presentMode = oOriginalPresentModeInfo ? oOriginalPresentModeInfo->pPresentModes[i] : gamescopeSwapchain->presentMode;
            if (forceFifo && !frameLimiterAware)
              presentMode = VK_PRESENT_MODE_FIFO_KHR;
            gamescope_swapchain_set_present_mode(gamescopeSwapchain->object, uint32_t(presentMode));
          }
        }
      }

      VkResult result = pDispatch->QueuePresentKHR(queue, &presentInfo);

      for (uint32_t i = 0; i < presentInfo.swapchainCount; i++) {
        VkSwapchainKHR swapchain = presentInfo.pSwapchains[i];

        auto UpdateSwapchainResult = [&](VkResult newResult) {
          if (presentInfo.pResults && presentInfo.pResults[i] >= VK_SUCCESS)
            presentInfo.pResults[i] = newResult;
          if (result >= VK_SUCCESS)
            result = newResult;
        };

        if (auto gamescopeSwapchain = GamescopeSwapchain::get(swapchain)) {
          // If we are a frame limiter aware application like DXVK or VKD3D-Proton, we don't
          // transparently change their vkQueuePresent to just FIFO modes, we change what is
          // exposed as supported in order for them to handle presentation latency like they
          // would as in FIFO mode.
          if (frameLimiterAware && gamescopeSwapchain->forceFifo != forceFifo) {
              fprintf(stderr, "[Gamescope WSI] Forcing swapchain recreation as frame limiter changed, and we want the app to know the exposed modes changed.\n");
              UpdateSwapchainResult(VK_ERROR_OUT_OF_DATE_KHR);
          }

          auto gamescopeSurface = GamescopeSurface::get(gamescopeSwapchain->surface);
          if (!gamescopeSurface) {
            fprintf(stderr, "[Gamescope WSI] QueuePresentKHR: Surface for swapchain %u was already destroyed. (App use after free).\n", i);
            abort();
            continue;
          }

          const bool canBypass = gamescopeSurface->canBypassXWayland();
          if (canBypass != gamescopeSwapchain->isBypassingXWayland) {
            if (canBypass) {
              if (!(gamescopeSurface->flags & GamescopeLayerClient::Flag::NoSuboptimal))
                UpdateSwapchainResult(VK_SUBOPTIMAL_KHR);
            } else {
              UpdateSwapchainResult(VK_ERROR_OUT_OF_DATE_KHR);  
            }
          }

          // Emulate behaviour when currentExtent changes in X11 swapchain.
          if (!gamescopeSurface->isWayland() && !(gamescopeSurface->flags & GamescopeLayerClient::Flag::ForceSwapchainExtent)) {
            // gamescopeSurface->cachedWindowSize is set by canBypassXWayland.
            // TODO: Rename that to be some update cached vars thing, then read back canBypassXWayland.            
            if (gamescopeSurface->cachedWindowRect) {
              const bool windowSizeChanged = gamescopeSurface->cachedWindowRect->extent != gamescopeSwapchain->extent;
              if (windowSizeChanged)
                UpdateSwapchainResult(VK_ERROR_OUT_OF_DATE_KHR);
            } else {
              fprintf(stderr, "[Gamescope WSI] QueuePresentKHR: Failed to get cached window size for swapchain %u\n", i);
            }
          }
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

        const VkHdrMetadataEXT& metadata = pMetadata[i];
        gamescope_swapchain_set_hdr_metadata(
          gamescopeSwapchain->object,
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

          fprintf(stderr, "[Gamescope WSI] VkHdrMetadataEXT: display primaries:\n");
          fprintf(stderr, "                                      r: %.4g %.4g\n", metadata.displayPrimaryRed.x, metadata.displayPrimaryRed.y);
          fprintf(stderr, "                                      g: %.4g %.4g\n", metadata.displayPrimaryGreen.x, metadata.displayPrimaryGreen.y);
          fprintf(stderr, "                                      b: %.4g %.4g\n", metadata.displayPrimaryBlue.x, metadata.displayPrimaryBlue.y);
          fprintf(stderr, "                                      w: %.4g %.4g\n", metadata.whitePoint.x, metadata.whitePoint.y);
          fprintf(stderr, "                                  mastering luminance: min %g nits, max %g nits\n", metadata.minLuminance, metadata.maxLuminance);
          fprintf(stderr, "                                  maxContentLightLevel: %g nits\n", metadata.maxContentLightLevel);
          fprintf(stderr, "                                  maxFrameAverageLightLevel: %g nits\n", metadata.maxFrameAverageLightLevel);
      }
    }

    static VkResult GetPastPresentationTimingGOOGLE(
      const vkroots::VkDeviceDispatch*      pDispatch,
            VkDevice                        device,
            VkSwapchainKHR                  swapchain,
            uint32_t*                       pPresentationTimingCount,
            VkPastPresentationTimingGOOGLE* pPresentationTimings) {
      auto gamescopeSwapchain = GamescopeSwapchain::get(swapchain);
      if (!gamescopeSwapchain) {
        fprintf(stderr, "[Gamescope WSI] GetPastPresentationTimingGOOGLE: Not a gamescope swapchain.\n");
        return VK_ERROR_SURFACE_LOST_KHR;
      }

      // Dispatch to get the latest timings.
      if (waylandPumpEvents(gamescopeSwapchain->display) < 0)
        return VK_ERROR_SURFACE_LOST_KHR;

      uint32_t originalCount = *pPresentationTimingCount;

      std::unique_lock lock(*gamescopeSwapchain->presentTimingMutex);
      auto& timings = gamescopeSwapchain->pastPresentTimings;

      VkResult result = vkroots::helpers::array(timings, pPresentationTimingCount, pPresentationTimings);
      // Erase those that we returned so we don't return them again.
      timings.erase(timings.begin(), timings.begin() + originalCount);

      return result;
    }

    static VkResult GetRefreshCycleDurationGOOGLE(
      const vkroots::VkDeviceDispatch*      pDispatch,
            VkDevice                        device,
            VkSwapchainKHR                  swapchain,
            VkRefreshCycleDurationGOOGLE*   pDisplayTimingProperties) {
      auto gamescopeSwapchain = GamescopeSwapchain::get(swapchain);
      if (!gamescopeSwapchain) {
        fprintf(stderr, "[Gamescope WSI] GetRefreshCycleDurationGOOGLE: Not a gamescope swapchain.\n");
        return VK_ERROR_SURFACE_LOST_KHR;
      }

      // Dispatch to get the latest cycle.
      if (waylandPumpEvents(gamescopeSwapchain->display) < 0)
        return VK_ERROR_SURFACE_LOST_KHR;

      std::unique_lock lock(*gamescopeSwapchain->presentTimingMutex);
      pDisplayTimingProperties->refreshDuration = gamescopeSwapchain->refreshCycle;

      return VK_SUCCESS;
    }

  };

}

VKROOTS_DEFINE_LAYER_INTERFACES(GamescopeWSILayer::VkInstanceOverrides,
                                vkroots::NoOverrides,
                                GamescopeWSILayer::VkDeviceOverrides);

VKROOTS_IMPLEMENT_SYNCHRONIZED_MAP_TYPE(GamescopeWSILayer::GamescopeInstance);
VKROOTS_IMPLEMENT_SYNCHRONIZED_MAP_TYPE(GamescopeWSILayer::GamescopeSurface);
VKROOTS_IMPLEMENT_SYNCHRONIZED_MAP_TYPE(GamescopeWSILayer::GamescopeSwapchain);
