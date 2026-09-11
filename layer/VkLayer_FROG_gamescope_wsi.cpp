#define VK_USE_PLATFORM_WAYLAND_KHR
#define VK_USE_PLATFORM_XCB_KHR
#define VK_USE_PLATFORM_XLIB_KHR
#include "vkroots.h"
#include "xcb_helpers.hpp"
#include "vulkan_operators.hpp"
#include "present_timing.hpp"
#include "gamescope-swapchain-client-protocol.h"
#include "gamescope-limiter-client-protocol.h"
#include "../src/color_helpers.h"
#include "../src/layer_defines.h"

#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <tuple>
#include <vector>
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <optional>

#include <poll.h>
#include <sys/stat.h>
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

  // The default queue carries the limiter on the layer's display, but on a
  // native Wayland client's display it carries the client's own handlers.
  static int waylandPumpEvents(wl_display *display, wl_event_queue *queue, bool ownDisplay) {
    const int wlFd = wl_display_get_fd(display);
    for (;;) {
      if (ownDisplay && wl_display_dispatch_pending(display) < 0)
        return -1;
      if (wl_display_dispatch_queue_pending(display, queue) < 0)
        return -1;
      if (wl_display_prepare_read_queue(display, queue) < 0) {
        if (errno == EAGAIN || errno == EINTR)
          continue;
        return -1;
      }
      pollfd fd = { .fd = wlFd, .events = POLLIN };
      timespec zeroTimeout = {};
      int ret = ppoll(&fd, 1, &zeroTimeout, nullptr);
      if (ret <= 0) {
        const int error = errno;
        wl_display_cancel_read(display);
        if (ret < 0 && error == EINTR)
          continue;
        if (ret == 0) {
          ret = wl_display_flush(display);
          if (ret < 0 && errno == EAGAIN)
            return 0;
        }
        return ret;
      }
      if (wl_display_read_events(display) < 0)
        return -1;
      // Dispatch everything just read before deciding there is nothing left.
    }
  }

  // Unlink a layer-owned structure and restore the application's chain.
  template<typename T>
  class ChainRemoval {
  public:
    template<typename U>
    explicit ChainRemoval(U *root, bool remove = true) {
      if (remove)
        std::tie(object, parent) = vkroots::RemoveFromChain<T>(root);
    }
    ~ChainRemoval() {
      if (object)
        parent->pNext = reinterpret_cast<VkBaseOutStructure *>(object);
    }
  private:
    T *object = nullptr;
    VkBaseOutStructure *parent = nullptr;
  };

  // Copy the chain so routing one swapchain never rewrites the caller's arrays.
  class PresentInfoSlice {
  public:
    PresentInfoSlice(const VkPresentInfoKHR &source, uint32_t index, VkResult *result)
      : info(source) {
      info.swapchainCount = 1;
      info.pSwapchains += index;
      info.pImageIndices += index;
      info.pResults = result;
      info.waitSemaphoreCount = 0;
      info.pWaitSemaphores = nullptr;
      info.pNext = nullptr;
      tail = &info.pNext;
      for (auto *entry = static_cast<const VkBaseInStructure *>(source.pNext); entry; entry = entry->pNext) {
        switch (entry->sType) {
          case VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR:
            slice<VkPresentRegionsKHR>(entry, index, &VkPresentRegionsKHR::pRegions); break;
          case VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR:
            slice<VkDeviceGroupPresentInfoKHR>(entry, index, &VkDeviceGroupPresentInfoKHR::pDeviceMasks); break;
          case VK_STRUCTURE_TYPE_PRESENT_ID_KHR:
            slice<VkPresentIdKHR>(entry, index, &VkPresentIdKHR::pPresentIds); break;
          case VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR:
            slice<VkPresentId2KHR>(entry, index, &VkPresentId2KHR::pPresentIds); break;
          case VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT:
            slice<VkPresentTimingsInfoEXT>(entry, index, &VkPresentTimingsInfoEXT::pTimingInfos); break;
          case VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE:
            slice<VkPresentTimesInfoGOOGLE>(entry, index, &VkPresentTimesInfoGOOGLE::pTimes); break;
          case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT:
            slice<VkSwapchainPresentFenceInfoEXT>(entry, index, &VkSwapchainPresentFenceInfoEXT::pFences); break;
          case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT:
            slice<VkSwapchainPresentModeInfoEXT>(entry, index, &VkSwapchainPresentModeInfoEXT::pPresentModes); break;
          case VK_STRUCTURE_TYPE_DISPLAY_PRESENT_INFO_KHR:
            append<VkDisplayPresentInfoKHR>(entry); break;
          case VK_STRUCTURE_TYPE_FRAME_BOUNDARY_EXT: {
            auto &boundary = append<VkFrameBoundaryEXT>(entry);
            if (index + 1 != source.swapchainCount)
              boundary.flags &= ~VK_FRAME_BOUNDARY_FRAME_END_BIT_EXT;
            break;
          }
          default: complete = false; break;
        }
      }
    }

    PresentInfoSlice(const PresentInfoSlice &) = delete;
    PresentInfoSlice &operator=(const PresentInfoSlice &) = delete;

    VkPresentInfoKHR info;
    bool complete = true;

  private:
    template<typename T>
    T &append(const VkBaseInStructure *entry) {
      auto &copy = std::get<T>(chain);
      copy = *reinterpret_cast<const T *>(entry);
      copy.pNext = nullptr;
      *tail = &copy;
      tail = &copy.pNext;
      return copy;
    }

    template<typename T, typename Array>
    void slice(const VkBaseInStructure *entry, uint32_t index, Array T::*array) {
      auto &copy = append<T>(entry);
      if (copy.swapchainCount) {
        copy.swapchainCount = 1;
        if (copy.*array)
          copy.*array += index;
      }
    }

    const void **tail;
    std::tuple<VkPresentRegionsKHR, VkDeviceGroupPresentInfoKHR, VkPresentIdKHR, VkPresentId2KHR,
      VkPresentTimingsInfoEXT, VkPresentTimesInfoGOOGLE, VkSwapchainPresentFenceInfoEXT,
      VkSwapchainPresentModeInfoEXT, VkDisplayPresentInfoKHR, VkFrameBoundaryEXT> chain;
  };

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

      const char *waylandSocketName = std::getenv("WAYLAND_DISPLAY");
      if (!waylandSocketName || !*waylandSocketName || strcmp(gamescopeSocketName, waylandSocketName) == 0)
        return true;

      // An inherited connection overrides the socket name. Do not broaden
      // the existing check when we cannot identify it by its path.
      if (std::getenv("WAYLAND_SOCKET"))
        return false;

      std::array<std::string, 2> paths = { gamescopeSocketName, waylandSocketName };
      const char *runtimeDir = std::getenv("XDG_RUNTIME_DIR");
      for (auto &path : paths) {
        if (path.front() != '/') {
          if (!runtimeDir || !*runtimeDir)
            return false;
          path = std::string(runtimeDir) + "/" + path;
        }
      }

      struct stat gamescopeStat, waylandStat;
      if (stat(paths[0].c_str(), &gamescopeStat) != 0 || !S_ISSOCK(gamescopeStat.st_mode))
        return false;

      // Pressure-vessel can rewrite an empty display to a missing wayland-0,
      // or expose one socket under two different bind-mount paths.
      if (stat(paths[1].c_str(), &waylandStat) != 0)
        return errno == ENOENT || errno == ENOTDIR;

      return S_ISSOCK(waylandStat.st_mode) &&
        gamescopeStat.st_dev == waylandStat.st_dev && gamescopeStat.st_ino == waylandStat.st_ino;
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

  // Frame limiter state received over the gamescope_limiter protocol.
  // Owned by the surfaces holding copies of GamescopeWaylandObjects.
  struct GamescopeLimiterState {
    ~GamescopeLimiterState() {
      if (proxy)
        gamescope_limiter_destroy(proxy);
    }

    gamescope_limiter *proxy = nullptr;
    std::atomic<uint32_t> state = { 0 };
  };

  static constexpr gamescope_limiter_listener s_limiterListener = {
    .state = [](void *data, gamescope_limiter *limiter, uint32_t frameLimitState) {
      reinterpret_cast<GamescopeLimiterState *>(data)->state = frameLimitState;
    },
  };

  // Legacy fallback for compositors without gamescope_limiter. The Mesa DRI3
  // path on SteamOS uses the same file. It may not be visible inside app
  // containers.
  static std::mutex gamescopeSwapchainLimiterFDMutex;
  static uint32_t gamescopeFrameLimiterFileOverride() {
    const char *path = getenv("GAMESCOPE_LIMITER_FILE");
    if (!path)
        return 0;

    int fd = -1;
    {
      std::unique_lock lock(gamescopeSwapchainLimiterFDMutex);

      static int s_limiterFD = -1;
      static bool s_warnedOpenFailure = false;

      if (s_limiterFD < 0) {
        s_limiterFD = open(path, O_RDONLY);
        if (s_limiterFD < 0 && !std::exchange(s_warnedOpenFailure, true))
          fprintf(stderr, "[Gamescope WSI] Could not open GAMESCOPE_LIMITER_FILE (%s): %s\n", path, strerror(errno));
      }

      fd = s_limiterFD;
    }

    if (fd < 0)
        return 0;

    uint32_t overrideValue = 0;
    pread(fd, &overrideValue, sizeof(overrideValue), 0);
    return overrideValue;
  }

  struct GamescopeWaylandObjects {
    wl_compositor* compositor;
    gamescope_swapchain_factory_v2* gamescopeSwapchainFactory;
    std::shared_ptr<GamescopeLimiterState> limiterState;

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
          wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, uint32_t(wl_compositor_interface.version))));
      } else if (interface == "gamescope_swapchain_factory_v2"sv) {
        objects->gamescopeSwapchainFactory = reinterpret_cast<gamescope_swapchain_factory_v2 *>(
          wl_registry_bind(registry, name, &gamescope_swapchain_factory_v2_interface, std::min(version, uint32_t(gamescope_swapchain_factory_v2_interface.version))));
      } else if (interface == "gamescope_limiter"sv) {
        objects->limiterState = std::make_shared<GamescopeLimiterState>();
        // Cap at our version, binding higher is a fatal protocol error.
        objects->limiterState->proxy = reinterpret_cast<gamescope_limiter *>(
          wl_registry_bind(registry, name, &gamescope_limiter_interface, std::min(version, uint32_t(gamescope_limiter_interface.version))));
        gamescope_limiter_add_listener(objects->limiterState->proxy, &s_limiterListener, objects->limiterState.get());
      }
    },
    .global_remove = [](void* data, wl_registry* registry, uint32_t name) {
    },
  };

  static bool gamescopeIsForcingFifo(const GamescopeWaylandObjects& waylandObjects) {
    if (waylandObjects.limiterState)
      return waylandObjects.limiterState->state == 1;

    return gamescopeFrameLimiterFileOverride() == 1;
  }

  struct GamescopeInstanceData {
    wl_display* display;
    GamescopeWaylandObjects waylandObjects;
    uint32_t appId = 0;
    std::string engineName;
    GamescopeLayerClient::Flags flags = 0;
  };
  static vkroots::ObjectMap<VkInstance, GamescopeInstanceData> gamescopeInstances;

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

    bool canBypassXWayland(bool hdrColorspace = false) {
      if (isWayland())
        return true;

      auto rect = xcb::getWindowRect(connection, window);
      if (!rect) {
        fprintf(stderr, "[Gamescope WSI] canBypassXWayland: failed to get window info for window 0x%x.\n", window);
        return false;
      }

      cachedWindowRect = *rect;

      // Never bypass windows Wine presents offscreen to GDI-blit onto the
      // real toplevel: marked with _WINE_ALLOW_FLIP=0 or parented under
      // Wine's unnamed 1x1 dummy window. The blit can only carry SDR, so
      // an HDR colorspace always bypasses. Wine also detaches while a
      // toplevel is transiently unmapped and refusing there wedges games
      // polling for HDR formats.
      if (!hdrColorspace) {
        auto allowFlip = xcb::getPropertyValue<uint32_t>(connection, window, "_WINE_ALLOW_FLIP");
        if (allowFlip) {
          if (*allowFlip == 0) {
#if GAMESCOPE_WSI_BYPASS_DEBUG
            fprintf(stderr, "[Gamescope WSI] Not bypassing: _WINE_ALLOW_FLIP is 0 for window 0x%x.\n", window);
#endif
            return false;
          }
        } else if (auto parent = xcb::getParentWindow(connection, window)) {
          auto parentRect = xcb::getWindowRect(connection, *parent);
          if (parentRect && parentRect->extent.width == 1 && parentRect->extent.height == 1 &&
              xcb::isOverrideRedirect(connection, *parent) &&
              !xcb::hasProperty(connection, *parent, XCB_ATOM_WM_CLASS)) {
#if GAMESCOPE_WSI_BYPASS_DEBUG
            fprintf(stderr, "[Gamescope WSI] Not bypassing: window 0x%x is parked under Wine dummy parent 0x%x.\n", window, *parent);
#endif
            return false;
          }
        }
      }

      auto largestObscuringWindowSize = xcb::getLargestObscuringChildWindowSize(connection, window);
      auto toplevelWindow = xcb::getToplevelWindow(connection, window);
      if (!largestObscuringWindowSize || !toplevelWindow) {
        fprintf(stderr, "[Gamescope WSI] canBypassXWayland: failed to get window info for window 0x%x.\n", window);
        return false;
      }

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
      //
      // Ignore a 1x1 toplevel: winex11 represents an empty window rect as
      // a 1x1 X window, so it's not a real size to validate against.
      if (*toplevelWindow != window &&
          (toplevelRect->extent.width > 1 || toplevelRect->extent.height > 1)) {
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
  static vkroots::ObjectMap<VkSurfaceKHR, GamescopeSurfaceData> gamescopeSurfaces;

  struct GamescopeSwapchainData {
    gamescope_swapchain *object;
    wl_display* display;
    wl_event_queue *eventQueue;
    VkSurfaceKHR surface; // Always the Gamescope Surface surface -- so the Wayland one.
    bool isWayland;
    bool isBypassingXWayland;
    bool forceFifo;
    VkPresentModeKHR presentMode;
    VkExtent2D extent;
    uint32_t serverId = 0;
    bool isHdrColorspace = false;
    std::unique_ptr<std::atomic<bool>> retired = std::make_unique<std::atomic<bool>>(false);
    bool presentTimingEnabled = false;
    PresentTimingQueue timingQueue;

    std::unique_ptr<std::mutex> presentTimingMutex = std::make_unique<std::mutex>();
    std::vector<VkPastPresentationTimingGOOGLE> pastPresentTimings;
    uint64_t refreshCycle = 16'666'666;
  };
  static vkroots::ObjectMap<VkSwapchainKHR, GamescopeSwapchainData> gamescopeSwapchains;
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
        *swapchain->retired = true;
      }
      fprintf(stderr, "[Gamescope WSI] Swapchain retired\n");
    },
    .present_timing = [](void *data, gamescope_swapchain *, uint32_t serialHi, uint32_t serialLo,
                         uint32_t queueEndHi, uint32_t queueEndLo, uint32_t dequeuedHi, uint32_t dequeuedLo,
                         uint32_t pixelOutHi, uint32_t pixelOutLo) {
      auto *swapchain = static_cast<GamescopeSwapchainData *>(data);
      std::lock_guard lock(*swapchain->presentTimingMutex);
      swapchain->timingQueue.complete((uint64_t(serialHi) << 32) | serialLo,
        (uint64_t(queueEndHi) << 32) | queueEndLo, (uint64_t(dequeuedHi) << 32) | dequeuedLo,
        (uint64_t(pixelOutHi) << 32) | pixelOutLo);
    },
    .timing_properties = [](void *data, gamescope_swapchain *, uint32_t durationHi, uint32_t durationLo,
                            uint32_t intervalHi, uint32_t intervalLo) {
      auto *swapchain = static_cast<GamescopeSwapchainData *>(data);
      std::lock_guard lock(*swapchain->presentTimingMutex);
      if (swapchain->isBypassingXWayland && !swapchain->isWayland)
        swapchain->timingQueue.setTimingProperties((uint64_t(durationHi) << 32) | durationLo,
                                                  (uint64_t(intervalHi) << 32) | intervalLo);
    },

  };

  template<typename Function, typename T, typename... Args>
  static VkResult enumerateValues(Function function, std::vector<T> &values, Args... args) {
    for (;;) {
      uint32_t count = 0;
      VkResult result = function(args..., &count, nullptr);
      if (result != VK_SUCCESS)
        return result;
      values.resize(count);
      if (!count)
        return VK_SUCCESS;
      result = function(args..., &count, values.data());
      if (result == VK_INCOMPLETE)
        continue;
      values.resize(count);
      return result;
    }
  }

  static_assert(uint32_t(GAMESCOPE_SWAPCHAIN_PRESENT_TIMING_FLAGS_RELATIVE) == uint32_t(VK_PRESENT_TIMING_INFO_PRESENT_AT_RELATIVE_TIME_BIT_EXT));
  static_assert(uint32_t(GAMESCOPE_SWAPCHAIN_PRESENT_TIMING_FLAGS_NEAREST_REFRESH_CYCLE) == uint32_t(VK_PRESENT_TIMING_INFO_PRESENT_AT_NEAREST_REFRESH_CYCLE_BIT_EXT));

  static bool hasExtension(const std::vector<VkExtensionProperties> &extensions, std::string_view name) {
    return std::ranges::any_of(extensions, [&](const auto &extension) { return name == extension.extensionName; });
  }

  static bool presentTimingSupported(const vkroots::VkPhysicalDeviceDispatch &dispatch,
                                      VkPhysicalDevice physicalDevice,
                                      const std::vector<VkExtensionProperties> *knownExtensions = nullptr) {
    auto instance = gamescopeInstances.find(dispatch.pInstanceDispatch->Instance);
    if (!instance || !instance->waylandObjects.gamescopeSwapchainFactory ||
        gamescope_swapchain_factory_v2_get_version(instance->waylandObjects.gamescopeSwapchainFactory) < 2)
      return false;
    std::vector<VkExtensionProperties> extensions;
    if (!knownExtensions) {
      if (enumerateValues(std::bind_front(&vkroots::VkPhysicalDeviceDispatch::EnumerateDeviceExtensionProperties, &dispatch),
                                     extensions, physicalDevice, nullptr) != VK_SUCCESS)
        return false;
      knownExtensions = &extensions;
    }
    for (const char *dependency : { VK_KHR_PRESENT_ID_2_EXTENSION_NAME, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME }) {
      if (!hasExtension(*knownExtensions, dependency))
        return false;
    }
    return true;
  }

  // Every layer stage is reported in CLOCK_MONOTONIC, so a local domain query
  // needs no driver call. Mixed queries forward only the driver's domains.
  template<typename Next>
  static VkResult calibratePresentTimestamps(Next next, VkDevice device, uint32_t count,
      const VkCalibratedTimestampInfoKHR *infos, uint64_t *timestamps, uint64_t *maxDeviation) {
    std::vector<VkCalibratedTimestampInfoKHR> driverInfos;
    std::vector<uint32_t> driverIndices;
    std::vector<uint32_t> localIndices;
    for (uint32_t i = 0; i < count; i++) {
      bool local = false;
      if (infos[i].timeDomain == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT ||
          infos[i].timeDomain == VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT) {
        if (auto *swapchainInfo = vkroots::FindInChain<const VkSwapchainCalibratedTimestampInfoEXT>(&infos[i]))
          local = gamescopeSwapchains.find(swapchainInfo->swapchain) != nullptr;
      }
      if (local) {
        localIndices.push_back(i);
      } else {
        driverIndices.push_back(i);
        driverInfos.push_back(infos[i]);
      }
    }
    if (localIndices.empty())
      return next(device, count, infos, timestamps, maxDeviation);

    std::vector<uint64_t> driverTimes(driverInfos.size());
    uint64_t deviation = 0;
    const uint64_t before = getTimeMonotonic();
    if (!driverInfos.empty()) {
      VkResult result = next(device, uint32_t(driverInfos.size()), driverInfos.data(), driverTimes.data(), &deviation);
      if (result != VK_SUCCESS)
        return result;
    }
    const uint64_t after = getTimeMonotonic();
    for (uint32_t i : localIndices)
      timestamps[i] = before + (after - before) / 2;
    for (size_t i = 0; i < driverIndices.size(); i++)
      timestamps[driverIndices[i]] = driverTimes[i];
    const uint64_t span = after - before;
    *maxDeviation = deviation > UINT64_MAX - span ? UINT64_MAX : deviation + span;
    return VK_SUCCESS;
  }

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
      
      GamescopeWaylandObjects waylandObjects = GamescopeWaylandObjects::get(display);
      if (!waylandObjects.valid()) {
        waylandObjects.limiterState.reset();
        if (waylandObjects.gamescopeSwapchainFactory)
          gamescope_swapchain_factory_v2_destroy(waylandObjects.gamescopeSwapchainFactory);
        if (waylandObjects.compositor)
          wl_compositor_destroy(waylandObjects.compositor);
        wl_display_disconnect(display);
        return result;
      }
      {
        uint32_t appId = clientAppId();

        std::string engineName;
        if (pCreateInfo->pApplicationInfo && pCreateInfo->pApplicationInfo->pEngineName)
          engineName = pCreateInfo->pApplicationInfo->pEngineName;

        auto state = gamescopeInstances.create(*pInstance, GamescopeInstanceData {
          .display = display,
          .waylandObjects = waylandObjects,
          .appId   = appId,
          .engineName = engineName,
          .flags   = defaultLayerClientFlags(pCreateInfo->pApplicationInfo, appId),
        });

        // If we know at instance creation time we should disable HDR, force off
        // DXVK_HDR now.
        if (state && (state->flags & GamescopeLayerClient::Flag::DisableHDR))
          setenv("DXVK_HDR", "0", 1);
      }

      // Work around the Mesa implementation of this being broken.
      // ( https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/31134 )
      setenv("vk_wsi_force_swapchain_to_current_extent", "false", 0);

      return result;
    }

    static void DestroyInstance(
      const vkroots::VkInstanceDispatch & pDispatch,
            VkInstance                   instance,
      const VkAllocationCallbacks*       pAllocator) {
      if (auto state = gamescopeInstances.find(instance)) {
        state->waylandObjects.limiterState.reset();
        if (state->waylandObjects.gamescopeSwapchainFactory)
          gamescope_swapchain_factory_v2_destroy(state->waylandObjects.gamescopeSwapchainFactory);
        if (state->waylandObjects.compositor)
          wl_compositor_destroy(state->waylandObjects.compositor);
        wl_display_disconnect(state->display);
      }
      gamescopeInstances.erase(instance);
      // vkroots' DestroyInstance method erases its own dispatch before using
      // the saved entry point. Keep that entry point alive across table cleanup.
      auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(pDispatch.GetInstanceProcAddr(instance, "vkDestroyInstance"));
      vkroots::tables::DestroyDispatchTable(instance);
      destroy(instance, pAllocator);
    }

    static VkResult CreateDevice(
      const vkroots::VkPhysicalDeviceDispatch & pDispatch,
            VkPhysicalDevice             physicalDevice,
      const VkDeviceCreateInfo*          pCreateInfo,
      const VkAllocationCallbacks*       pAllocator,
            VkDevice*                    pDevice) {
      if (!gamescopeInstances.find(pDispatch.pInstanceDispatch->Instance))
        return pDispatch.CreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
      VkDeviceCreateInfo deviceCreateInfo = *pCreateInfo;
      std::vector<VkExtensionProperties> driverExtensions;
      VkResult extensionResult = enumerateValues(std::bind_front(&vkroots::VkPhysicalDeviceDispatch::EnumerateDeviceExtensionProperties, &pDispatch),
        driverExtensions, physicalDevice, nullptr);
      if (extensionResult != VK_SUCCESS)
        return extensionResult;
      const bool driverPresentTiming = hasExtension(driverExtensions, VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
      ChainRemoval<VkPhysicalDevicePresentTimingFeaturesEXT> timingFeatures(&deviceCreateInfo, !driverPresentTiming);

      std::vector<const char *> extensions(pCreateInfo->ppEnabledExtensionNames, pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);
      if (!driverPresentTiming)
        std::erase_if(extensions, [](const char *extension) { return std::string_view(extension) == VK_EXT_PRESENT_TIMING_EXTENSION_NAME; });
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

      return pDispatch.CreateDevice(physicalDevice, &deviceCreateInfo, pAllocator, pDevice);
    }

    static VkBool32 GetPhysicalDeviceXcbPresentationSupportKHR(
      const vkroots::VkPhysicalDeviceDispatch & pDispatch,
            VkPhysicalDevice             physicalDevice,
            uint32_t                     queueFamilyIndex,
            xcb_connection_t*            connection,
            xcb_visualid_t               visual_id) {
      auto gamescopeInstance = gamescopeInstances.find(pDispatch.pInstanceDispatch->Instance);
      if (!gamescopeInstance)
        return pDispatch.GetPhysicalDeviceXcbPresentationSupportKHR(physicalDevice, queueFamilyIndex, connection, visual_id);

      return GetPhysicalDeviceGamescopePresentationSupport(pDispatch, gamescopeInstance, physicalDevice, queueFamilyIndex);
    }

    static VkBool32 GetPhysicalDeviceXlibPresentationSupportKHR(
      const vkroots::VkPhysicalDeviceDispatch & pDispatch,
            VkPhysicalDevice             physicalDevice,
            uint32_t                     queueFamilyIndex,
            Display*                     dpy,
            VisualID                     visualID) {
      auto gamescopeInstance = gamescopeInstances.find(pDispatch.pInstanceDispatch->Instance);
      if (!gamescopeInstance)
        return pDispatch.GetPhysicalDeviceXlibPresentationSupportKHR(physicalDevice, queueFamilyIndex, dpy, visualID);

      return GetPhysicalDeviceGamescopePresentationSupport(pDispatch, gamescopeInstance, physicalDevice, queueFamilyIndex);
    }

    static VkResult CreateXcbSurfaceKHR(
      const vkroots::VkInstanceDispatch & pDispatch,
            VkInstance                   instance,
      const VkXcbSurfaceCreateInfoKHR*   pCreateInfo,
      const VkAllocationCallbacks*       pAllocator,
            VkSurfaceKHR*                pSurface) {
      auto gamescopeInstance = gamescopeInstances.find(instance);
      if (!gamescopeInstance)
        return pDispatch.CreateXcbSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);

      return CreateGamescopeSurface(pDispatch, gamescopeInstance, instance, pCreateInfo->connection, pCreateInfo->window, pAllocator, pSurface);
    }

    static VkResult CreateXlibSurfaceKHR(
      const vkroots::VkInstanceDispatch & pDispatch,
            VkInstance                   instance,
      const VkXlibSurfaceCreateInfoKHR*  pCreateInfo,
      const VkAllocationCallbacks*       pAllocator,
            VkSurfaceKHR*                pSurface) {
      auto gamescopeInstance = gamescopeInstances.find(instance);
      if (!gamescopeInstance)
        return pDispatch.CreateXlibSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);

      return CreateGamescopeSurface(pDispatch, gamescopeInstance, instance, XGetXCBConnection(pCreateInfo->dpy), xcb_window_t(pCreateInfo->window), pAllocator, pSurface);
    }

    static VkResult CreateWaylandSurfaceKHR(
      const vkroots::VkInstanceDispatch &   pDispatch,
            VkInstance                     instance,
      const VkWaylandSurfaceCreateInfoKHR* pCreateInfo,
      const VkAllocationCallbacks*         pAllocator,
            VkSurfaceKHR*                  pSurface) {
      auto gamescopeInstance = gamescopeInstances.find(instance);
      if (!gamescopeInstance)
        return pDispatch.CreateWaylandSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);

      GamescopeWaylandObjects waylandObjects = GamescopeWaylandObjects::get(pCreateInfo->display);
      if (!waylandObjects.valid()) {
        fprintf(stderr, "[Gamescope WSI] Failed to get Wayland objects\n");
        return VK_ERROR_SURFACE_LOST_KHR;
      }

      VkResult res = pDispatch.CreateWaylandSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
      if (res != VK_SUCCESS)
        return res;

      auto gamescopeSurface = gamescopeSurfaces.create(*pSurface, GamescopeSurfaceData {
        .instance        = instance,
        .display         = pCreateInfo->display,
        .waylandObjects  = waylandObjects,
        .surface         = pCreateInfo->surface,
        .flags           = gamescopeInstance->flags,
        .hdrOutput       = false, // XXXX FIXME FIXME FIXME //hdrOutput,
      });
      if (!gamescopeSurface)
        return VK_ERROR_SURFACE_LOST_KHR;

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
      const vkroots::VkPhysicalDeviceDispatch & pDispatch,
            VkPhysicalDevice             physicalDevice,
            VkSurfaceKHR                 surface,
            uint32_t*                    pSurfaceFormatCount,
            VkSurfaceFormatKHR*          pSurfaceFormats) {
      auto gamescopeSurface = gamescopeSurfaces.find(surface);
      if (!gamescopeSurface)
        return pDispatch.GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, pSurfaceFormatCount, pSurfaceFormats);

      const bool canBypass = gamescopeSurface->canBypassXWayland();
      VkSurfaceKHR selectedSurface = canBypass ? surface : gamescopeSurface->fallbackSurface;

      // HDR skips the Wine offscreen gate, so expose HDR formats whenever
      // an HDR colorspace could bypass, even if SDR currently can't.
      if (!gamescopeSurface->shouldExposeHDR() ||
          !(canBypass || gamescopeSurface->canBypassXWayland(true)))
        return pDispatch.GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, selectedSurface, pSurfaceFormatCount, pSurfaceFormats);

      return vkroots::append(
        std::bind_front(&vkroots::VkPhysicalDeviceDispatch::GetPhysicalDeviceSurfaceFormatsKHR, &pDispatch),
        s_ExtraHDRSurfaceFormats,
        pSurfaceFormatCount,
        pSurfaceFormats,
        physicalDevice,
        selectedSurface);
    }

    static VkResult GetPhysicalDeviceSurfaceFormats2KHR(
      const vkroots::VkPhysicalDeviceDispatch &     pDispatch,
            VkPhysicalDevice                 physicalDevice,
      const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
            uint32_t*                        pSurfaceFormatCount,
            VkSurfaceFormat2KHR*             pSurfaceFormats) {
      auto gamescopeSurface = gamescopeSurfaces.find(pSurfaceInfo->surface);
      if (!gamescopeSurface)
        return pDispatch.GetPhysicalDeviceSurfaceFormats2KHR(physicalDevice, pSurfaceInfo, pSurfaceFormatCount, pSurfaceFormats);

      VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo = *pSurfaceInfo;
      const bool canBypass = gamescopeSurface->canBypassXWayland();
      surfaceInfo.surface = canBypass ? surfaceInfo.surface : gamescopeSurface->fallbackSurface;

      // HDR skips the Wine offscreen gate, so expose HDR formats whenever
      // an HDR colorspace could bypass, even if SDR currently can't.
      if (!gamescopeSurface->shouldExposeHDR() ||
          !(canBypass || gamescopeSurface->canBypassXWayland(true)))
        return pDispatch.GetPhysicalDeviceSurfaceFormats2KHR(physicalDevice, &surfaceInfo, pSurfaceFormatCount, pSurfaceFormats);

      return vkroots::append(
        std::bind_front(&vkroots::VkPhysicalDeviceDispatch::GetPhysicalDeviceSurfaceFormats2KHR, &pDispatch),
        s_ExtraHDRSurfaceFormat2s,
        pSurfaceFormatCount,
        pSurfaceFormats,
        physicalDevice,
        &surfaceInfo);
    }

    static VkResult GetPhysicalDeviceSurfaceCapabilitiesKHR(
      const vkroots::VkPhysicalDeviceDispatch &     pDispatch,
            VkPhysicalDevice                 physicalDevice,
            VkSurfaceKHR                     surface,
            VkSurfaceCapabilitiesKHR*        pSurfaceCapabilities) {
      auto gamescopeSurface = gamescopeSurfaces.find(surface);
      if (!gamescopeSurface)
        return pDispatch.GetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, pSurfaceCapabilities);

      VkResult res = VK_SUCCESS;
      if ((res = pDispatch.GetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, pSurfaceCapabilities)) != VK_SUCCESS)
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
      const vkroots::VkPhysicalDeviceDispatch &     pDispatch,
            VkPhysicalDevice                 physicalDevice,
      const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
            VkSurfaceCapabilities2KHR*       pSurfaceCapabilities) {
      auto gamescopeSurface = gamescopeSurfaces.find(pSurfaceInfo->surface);
      if (!gamescopeSurface)
        return pDispatch.GetPhysicalDeviceSurfaceCapabilities2KHR(physicalDevice, pSurfaceInfo, pSurfaceCapabilities);

      // Incomplete writes here, do not return VK_INCOMPLETE.
      if (gamescopeIsForcingFifo(gamescopeSurface->waylandObjects) && gamescopeSurface->frameLimiterAware()) {
        const auto *pPresentMode = vkroots::FindInChain<VkSurfacePresentModeEXT>(pSurfaceInfo);
        const std::array<VkPresentModeKHR, 1> s_SingleMode = {{
          pPresentMode ? pPresentMode->presentMode : VK_PRESENT_MODE_FIFO_KHR,
        }};
        auto [pPresentModeCompat, pPresentModeCompatParent] = vkroots::RemoveFromChain<VkSurfacePresentModeCompatibilityEXT>(pSurfaceCapabilities);
        if (pPresentModeCompat)
          vkroots::array(s_SingleMode, &pPresentModeCompat->presentModeCount, pPresentModeCompat->pPresentModes);

        VkResult res = VK_SUCCESS;
        if ((res = pDispatch.GetPhysicalDeviceSurfaceCapabilities2KHR(physicalDevice, pSurfaceInfo, pSurfaceCapabilities)) != VK_SUCCESS)
          return res;

        if (pPresentModeCompat)
          vkroots::AddToChain(pPresentModeCompatParent, pPresentModeCompat);
      } else {
        VkResult res = VK_SUCCESS;
        if ((res = pDispatch.GetPhysicalDeviceSurfaceCapabilities2KHR(physicalDevice, pSurfaceInfo, pSurfaceCapabilities)) != VK_SUCCESS)
          return res;
      }

      if (!gamescopeSurface->isWayland()) {
        auto rect = xcb::getWindowRect(gamescopeSurface->connection, gamescopeSurface->window);
        if (!rect)
          return VK_ERROR_SURFACE_LOST_KHR;

        pSurfaceCapabilities->surfaceCapabilities.currentExtent = rect->extent;
      }
      pSurfaceCapabilities->surfaceCapabilities.minImageCount = getMinImageCount();
      if (auto *timing = vkroots::FindInChainMutable<VkPresentTimingSurfaceCapabilitiesEXT>(pSurfaceCapabilities)) {
        // Only bypassing swapchains reach the compositor's timing path.
        const bool supported = !gamescopeSurface->isWayland() && gamescopeSurface->canBypassXWayland() &&
          presentTimingSupported(pDispatch, physicalDevice);
        timing->presentTimingSupported = supported;
        timing->presentAtAbsoluteTimeSupported = supported;
        timing->presentAtRelativeTimeSupported = supported;
        timing->presentStageQueries = supported ?
          VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT | VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT |
          VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT : 0;
      }

      return VK_SUCCESS;
    }

    static void GetPhysicalDeviceFeatures2(
      const vkroots::VkPhysicalDeviceDispatch & pDispatch,
            VkPhysicalDevice             physicalDevice,
            VkPhysicalDeviceFeatures2*   pFeatures) {
      pDispatch.GetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
      if (auto *timing = vkroots::FindInChainMutable<VkPhysicalDevicePresentTimingFeaturesEXT>(pFeatures)) {
        const VkBool32 supported = presentTimingSupported(pDispatch, physicalDevice);
        timing->presentTiming = supported;
        timing->presentAtAbsoluteTime = supported;
        timing->presentAtRelativeTime = supported;
      }
    }

    static void GetPhysicalDeviceFeatures2KHR(
      const vkroots::VkPhysicalDeviceDispatch & pDispatch,
            VkPhysicalDevice             physicalDevice,
            VkPhysicalDeviceFeatures2*   pFeatures) {
      GetPhysicalDeviceFeatures2(pDispatch, physicalDevice, pFeatures);
    }

    static VkResult GetPhysicalDeviceSurfacePresentModesKHR(
      const vkroots::VkPhysicalDeviceDispatch & pDispatch,
        VkPhysicalDevice                 physicalDevice,
        VkSurfaceKHR                     surface,
        uint32_t*                        pPresentModeCount,
        VkPresentModeKHR*                pPresentModes) {
      static constexpr std::array<VkPresentModeKHR, 1> s_FifoPresentModes = {{
        VK_PRESENT_MODE_FIFO_KHR,
      }};

      if (auto state = gamescopeSurfaces.find(surface)) {
        if (gamescopeIsForcingFifo(state->waylandObjects) && state->frameLimiterAware())
          return vkroots::array(s_FifoPresentModes, pPresentModeCount, pPresentModes);
      }

      return pDispatch.GetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, pPresentModeCount, pPresentModes);
    }

    static void DestroySurfaceKHR(
      const vkroots::VkInstanceDispatch & pDispatch,
            VkInstance                   instance,
            VkSurfaceKHR                 surface,
      const VkAllocationCallbacks*       pAllocator) {
      if (auto state = gamescopeSurfaces.find(surface)) {
        pDispatch.DestroySurfaceKHR(instance, state->fallbackSurface, pAllocator);
        wl_surface_destroy(state->surface);
      }
      gamescopeSurfaces.erase(surface);
      pDispatch.DestroySurfaceKHR(instance, surface, pAllocator);
    }

    static VkResult EnumerateDeviceExtensionProperties(
      const vkroots::VkPhysicalDeviceDispatch & pDispatch,
            VkPhysicalDevice             physicalDevice,
            const char*                  pLayerName,
            uint32_t*                    pPropertyCount,
            VkExtensionProperties*       pProperties) {
      if (!gamescopeInstances.find(pDispatch.pInstanceDispatch->Instance) ||
          (pLayerName && pLayerName != "VK_LAYER_FROG_gamescope_wsi"sv))
        return pDispatch.EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
      std::vector<VkExtensionProperties> driverExtensions;
      VkResult result = enumerateValues(std::bind_front(&vkroots::VkPhysicalDeviceDispatch::EnumerateDeviceExtensionProperties, &pDispatch),
        driverExtensions, physicalDevice, nullptr);
      if (result != VK_SUCCESS)
        return result;
      auto extensions = pLayerName ? std::vector<VkExtensionProperties>{} : driverExtensions;
      auto append = [&](const char *name, uint32_t version) {
        if (!hasExtension(extensions, name)) {
          VkExtensionProperties property{};
          std::strncpy(property.extensionName, name, sizeof(property.extensionName) - 1);
          property.specVersion = version;
          extensions.push_back(property);
        }
      };
      append(VK_EXT_HDR_METADATA_EXTENSION_NAME, VK_EXT_HDR_METADATA_SPEC_VERSION);
      append(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME, VK_GOOGLE_DISPLAY_TIMING_SPEC_VERSION);
      if (presentTimingSupported(pDispatch, physicalDevice, &driverExtensions))
        append(VK_EXT_PRESENT_TIMING_EXTENSION_NAME, VK_EXT_PRESENT_TIMING_SPEC_VERSION);
      return vkroots::array(extensions, pPropertyCount, pProperties);
    }


    static VkResult GetPhysicalDeviceCalibrateableTimeDomainsKHR(
      const vkroots::VkPhysicalDeviceDispatch &dispatch, VkPhysicalDevice physicalDevice,
      uint32_t *count, VkTimeDomainKHR *domains) {
      return CalibrateableTimeDomains(dispatch, physicalDevice, count, domains,
        std::bind_front(&vkroots::VkPhysicalDeviceDispatch::GetPhysicalDeviceCalibrateableTimeDomainsKHR, &dispatch));
    }

    static VkResult GetPhysicalDeviceCalibrateableTimeDomainsEXT(
      const vkroots::VkPhysicalDeviceDispatch &dispatch, VkPhysicalDevice physicalDevice,
      uint32_t *count, VkTimeDomainKHR *domains) {
      return CalibrateableTimeDomains(dispatch, physicalDevice, count, domains,
        std::bind_front(&vkroots::VkPhysicalDeviceDispatch::GetPhysicalDeviceCalibrateableTimeDomainsEXT, &dispatch));
    }

    template<typename Next>
    static VkResult CalibrateableTimeDomains(
      const vkroots::VkPhysicalDeviceDispatch &dispatch, VkPhysicalDevice physicalDevice,
      uint32_t *count, VkTimeDomainKHR *domains, Next next) {
      if (!presentTimingSupported(dispatch, physicalDevice))
        return next(physicalDevice, count, domains);
      std::vector<VkTimeDomainKHR> values;
      VkResult result = enumerateValues(next, values, physicalDevice);
      if (result != VK_SUCCESS)
        return result;
      if (std::ranges::find(values, VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT) == values.end())
        values.push_back(VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT);
      return vkroots::array(values, count, domains);
    }

  private:
    static VkResult CreateGamescopeSurface(
      const vkroots::VkInstanceDispatch & pDispatch,
            GamescopeInstanceData *           gamescopeInstance,
            VkInstance                   instance,
            xcb_connection_t*            connection,
            xcb_window_t                 window,
      const VkAllocationCallbacks*       pAllocator,
            VkSurfaceKHR*                pSurface) {
      fprintf(stderr, "[Gamescope WSI] Creating Gamescope surface: xid: 0x%x\n", window);

      GamescopeWaylandObjects waylandObjects = gamescopeInstance->waylandObjects;
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

      VkResult result = pDispatch.CreateWaylandSurfaceKHR(instance, &waylandCreateInfo, pAllocator, pSurface);
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
      result = pDispatch.CreateXcbSurfaceKHR(instance, &xcbCreateInfo, pAllocator, &fallbackSurface);
      if (result != VK_SUCCESS) {
        fprintf(stderr, "[Gamescope WSI] Failed to create Vulkan xcb (fallback) surface - vr: %s xid: 0x%x\n", vkroots::helpers::enumString(result), window);
        return result;
      }

      fprintf(stderr, "[Gamescope WSI] Made gamescope surface for xid: 0x%x\n", window);
      auto gamescopeSurface = gamescopeSurfaces.create(*pSurface, GamescopeSurfaceData {
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
      if (!gamescopeSurface)
        return VK_ERROR_SURFACE_LOST_KHR;

      DumpGamescopeSurfaceState(gamescopeInstance, gamescopeSurface);

      return result;
    }

    static void DumpGamescopeSurfaceState(GamescopeInstanceData * instance, GamescopeSurfaceData * surface) {
      fprintf(stderr, "[Gamescope WSI] Surface state:\n");
      fprintf(stderr, "  steam app id:                  %u\n", instance->appId);
      fprintf(stderr, "  window xid:                    0x%x\n", surface->window);
      fprintf(stderr, "  wayland surface res id:        %u\n", wl_proxy_get_id(reinterpret_cast<struct wl_proxy *>(surface->surface)));
      fprintf(stderr, "  layer client flags:            0x%x\n", surface->flags);
      fprintf(stderr, "  server hdr output enabled:     %s\n", surface->hdrOutput ? "true" : "false");
      fprintf(stderr, "  hdr formats exposed to client: %s\n", surface->shouldExposeHDR() ? "true" : "false");
    }

    static VkBool32 GetPhysicalDeviceGamescopePresentationSupport(
      const vkroots::VkPhysicalDeviceDispatch & pDispatch,
            GamescopeInstanceData *           gamescopeInstance,
            VkPhysicalDevice             physicalDevice,
            uint32_t                     queueFamilyIndex) {
      return pDispatch.GetPhysicalDeviceWaylandPresentationSupportKHR(physicalDevice, queueFamilyIndex, gamescopeInstance->display);
    }

  };

  class VkDeviceOverrides {
  public:
    static void DestroyDevice(
      const vkroots::VkDeviceDispatch &dispatch, VkDevice device, const VkAllocationCallbacks *allocator) {
      // As with instance teardown, table cleanup invalidates dispatch itself.
      auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(dispatch.GetDeviceProcAddr(device, "vkDestroyDevice"));
      vkroots::tables::DestroyDispatchTable(device);
      destroy(device, allocator);
    }

    static void DestroySwapchainKHR(
      const vkroots::VkDeviceDispatch & pDispatch,
            VkDevice                   device,
            VkSwapchainKHR             swapchain,
      const VkAllocationCallbacks*     pAllocator) {
      if (auto state = gamescopeSwapchains.find(swapchain)) {
        gamescope_swapchain_destroy(state->object);
        wl_event_queue_destroy(state->eventQueue);
      }
      gamescopeSwapchains.erase(swapchain);
      fprintf(stderr, "[Gamescope WSI] Destroying swapchain: %p\n", reinterpret_cast<void*>(swapchain));
      pDispatch.DestroySwapchainKHR(device, swapchain, pAllocator);
      fprintf(stderr, "[Gamescope WSI] Destroyed swapchain: %p\n", reinterpret_cast<void*>(swapchain));
    }

    static VkResult CreateSwapchainKHR(
      const vkroots::VkDeviceDispatch & pDispatch,
            VkDevice                   device,
      const VkSwapchainCreateInfoKHR*  pCreateInfo,
      const VkAllocationCallbacks*     pAllocator,
            VkSwapchainKHR*            pSwapchain) {
      auto gamescopeSurface = gamescopeSurfaces.find(pCreateInfo->surface);

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
        return pDispatch.CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
      }

      // Only the colorspaces we expose in s_ExtraHDRSurfaceFormat2s count as
      // HDR. Other non-sRGB colorspaces are still SDR content Wine can blit.
      const bool hdrColorspace = pCreateInfo->imageColorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT ||
                                  pCreateInfo->imageColorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
      const bool canBypass = gamescopeSurface->canBypassXWayland(hdrColorspace);

      VkSwapchainCreateInfoKHR swapchainInfo = *pCreateInfo;
      swapchainInfo.flags &= ~VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;

      if (pCreateInfo->oldSwapchain) {
        if (auto gamescopeSwapchain = gamescopeSwapchains.find(pCreateInfo->oldSwapchain)) {
          *gamescopeSwapchain->retired = true;
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
        vkroots::enumerate(
          std::bind_front(&vkroots::VkPhysicalDeviceDispatch::GetPhysicalDeviceSurfaceFormatsKHR, pDispatch.pPhysicalDeviceDispatch),
          supportedSurfaceFormats,
          pDispatch.PhysicalDevice,
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

      auto gamescopeInstance = gamescopeInstances.find(gamescopeSurface->instance);
      if (!gamescopeInstance) {
        fprintf(stderr, "[Gamescope WSI] CreateSwapchainKHR: Instance for swapchain was already destroyed. (App use after free).\n");
        return VK_ERROR_SURFACE_LOST_KHR;
      }

      VkResult result = pDispatch.CreateSwapchainKHR(device, &swapchainInfo, pAllocator, pSwapchain);
      if (result != VK_SUCCESS) {
        fprintf(stderr, "[Gamescope WSI] Failed to create swapchain - vr: %s xid: 0x%x\n", vkroots::helpers::enumString(result), gamescopeSurface->window);
        return result;
      }

      wl_event_queue *eventQueue = wl_display_create_queue(gamescopeSurface->display);
      auto *factory = static_cast<gamescope_swapchain_factory_v2 *>(
        wl_proxy_create_wrapper(gamescopeSurface->waylandObjects.gamescopeSwapchainFactory));
      if (!eventQueue || !factory) {
        if (factory)
          wl_proxy_wrapper_destroy(factory);
        if (eventQueue)
          wl_event_queue_destroy(eventQueue);
        pDispatch.DestroySwapchainKHR(device, *pSwapchain, pAllocator);
        *pSwapchain = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
      wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(factory), eventQueue);
      gamescope_swapchain *gamescopeSwapchainObject = gamescope_swapchain_factory_v2_create_swapchain(
        factory, gamescopeSurface->surface);
      wl_proxy_wrapper_destroy(factory);

      {
        auto gamescopeSwapchain = gamescopeSwapchains.create(*pSwapchain, GamescopeSwapchainData{
          .object              = gamescopeSwapchainObject,
          .display             = gamescopeSurface->display,
          .eventQueue          = eventQueue,
          .surface             = pCreateInfo->surface, // Always the Wayland side surface.
          .isWayland           = gamescopeSurface->isWayland(),
          .isBypassingXWayland = canBypass,
          .forceFifo           = gamescopeIsForcingFifo(gamescopeSurface->waylandObjects), // Were we forcing fifo when this swapchain was made?
          .presentMode         = pCreateInfo->presentMode, // The new present mode.
          .extent              = pCreateInfo->imageExtent,
          .serverId            = serverId,
          .isHdrColorspace     = hdrColorspace,
          .presentTimingEnabled = !!(pCreateInfo->flags & VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT) &&
            gamescope_swapchain_factory_v2_get_version(gamescopeSurface->waylandObjects.gamescopeSwapchainFactory) >= 2,
        });
        if (!gamescopeSwapchain) {
          gamescope_swapchain_destroy(gamescopeSwapchainObject);
          wl_event_queue_destroy(eventQueue);
          pDispatch.DestroySwapchainKHR(device, *pSwapchain, pAllocator);
          *pSwapchain = VK_NULL_HANDLE;
          return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        gamescopeSwapchain->pastPresentTimings.reserve(MaxPastPresentationTimes);

        gamescope_swapchain_add_listener(gamescopeSwapchainObject, &s_swapchainListener, reinterpret_cast<void*>(gamescopeSwapchain));
      }

      uint32_t imageCount = 0;
      pDispatch.GetSwapchainImagesKHR(device, *pSwapchain, &imageCount, nullptr);

      fprintf(stderr, "[Gamescope WSI] Created swapchain for xid: 0x%0x swapchain: %p - imageCount: %u - presentTiming: %s\n",
        gamescopeSurface->window,
        reinterpret_cast<void*>(*pSwapchain),
        imageCount,
        !(pCreateInfo->flags & VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT) ? "off" : canBypass ? "on" : "no bypass");

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
      const vkroots::VkDeviceDispatch & pDispatch,
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
      const vkroots::VkDeviceDispatch & pDispatch,
            VkDevice                   device,
      const VkAcquireNextImageInfoKHR* pAcquireInfo,
            uint32_t*                  pImageIndex) {
      if (auto gamescopeSwapchain = gamescopeSwapchains.find(pAcquireInfo->swapchain)) {
        if (*gamescopeSwapchain->retired)
          return VK_ERROR_OUT_OF_DATE_KHR;
      }

      return pDispatch.AcquireNextImage2KHR(device, pAcquireInfo, pImageIndex);
    }

    // Once the waits are submitted an OOM return can no longer promise the
    // untouched synchronization state vkQueuePresentKHR requires of it.
    static VkResult AfterSubmission(VkResult result) {
      return result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY ? VK_ERROR_DEVICE_LOST : result;
    }

    // A refused present must still run its waits and signal its present fences.
    static VkResult PresentWithoutImage(
      const vkroots::VkQueueDispatch & pDispatch,
            VkQueue                    queue,
      const VkPresentInfoKHR*          pPresentInfo,
            VkResult                   presentResult,
            bool                       releaseImages) {
      std::vector<VkPipelineStageFlags> waitStages(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

      std::vector<VkFence> presentFences;
      if (auto pFenceInfo = vkroots::FindInChain<const VkSwapchainPresentFenceInfoEXT>(pPresentInfo)) {
        for (uint32_t i = 0; i < pFenceInfo->swapchainCount; i++) {
          if (pFenceInfo->pFences[i] != VK_NULL_HANDLE)
            presentFences.push_back(pFenceInfo->pFences[i]);
        }
      }

      std::vector<VkResult> swapchainResults(pPresentInfo->swapchainCount, presentResult);
      // A refused last slice still owes tools the frame end its siblings dropped.
      std::optional<VkFrameBoundaryEXT> boundary;
      if (auto *entry = vkroots::FindInChain<VkFrameBoundaryEXT>(pPresentInfo)) {
        boundary = *entry;
        boundary->pNext = nullptr;
      }
      if (pPresentInfo->waitSemaphoreCount || !presentFences.empty() || boundary) {
        VkSubmitInfo submitInfo = {
          .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
          .pNext              = boundary ? &*boundary : nullptr,
          .waitSemaphoreCount = pPresentInfo->waitSemaphoreCount,
          .pWaitSemaphores    = pPresentInfo->pWaitSemaphores,
          .pWaitDstStageMask  = waitStages.data(),
        };

        VkResult result = pDispatch.QueueSubmit(queue, 1, &submitInfo, presentFences.empty() ? VK_NULL_HANDLE : presentFences[0]);
        if (result < VK_SUCCESS)
          return result;

        // Fence signals are ordered after everything earlier in submission
        // order, so any extra fences can ride empty submits.
        for (size_t i = 1; i < presentFences.size(); i++) {
          VkSubmitInfo emptySubmitInfo = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
          if (VkResult extra = pDispatch.QueueSubmit(queue, 1, &emptySubmitInfo, presentFences[i]); extra < VK_SUCCESS)
            return AfterSubmission(extra);
        }
      }

      VkResult result = presentResult;
      if (releaseImages) {
        if (VkResult idle = pDispatch.QueueWaitIdle(queue); idle < VK_SUCCESS)
          return AfterSubmission(idle);
        // One swapchain's release failing must not leave the others acquired.
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
          VkReleaseSwapchainImagesInfoKHR releaseInfo = {
            .sType = VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_KHR,
            .swapchain = pPresentInfo->pSwapchains[i],
            .imageIndexCount = 1,
            .pImageIndices = &pPresentInfo->pImageIndices[i],
          };
          VkResult released = pDispatch.ReleaseSwapchainImagesEXT(pDispatch.pDeviceDispatch->Device, &releaseInfo);
          if (released < VK_SUCCESS) {
            swapchainResults[i] = released;
            result = MergePresentResults(result, released);
          }
        }
      }

      if (pPresentInfo->pResults)
        std::copy(swapchainResults.begin(), swapchainResults.end(), pPresentInfo->pResults);

      return result;
    }

    static VkResult MergePresentResults(VkResult previous, VkResult current) {
      // The precedence vkQueuePresentKHR's return value rules give these codes.
      auto priority = [](VkResult result) {
        switch (result) {
          case VK_ERROR_DEVICE_LOST: return 7;
          case VK_ERROR_SURFACE_LOST_KHR: return 5;
          case VK_ERROR_OUT_OF_DATE_KHR: return 4;
          case VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT: return 3;
          case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT: return 2;
          case VK_SUBOPTIMAL_KHR: return 1;
          case VK_SUCCESS: return 0;
          default: return result < VK_SUCCESS ? 6 : 0;
        }
      };
      return priority(current) > priority(previous) ? current : previous;
    }

    // An unsupported chain entry cannot be sliced, so such a batch keeps the whole-batch behavior.
    static bool CanSplit(const VkPresentInfoKHR &info) {
      return info.swapchainCount > 1 && PresentInfoSlice(info, 0, nullptr).complete;
    }

    static VkResult PresentSwapchains(const vkroots::VkQueueDispatch &dispatch, VkQueue queue,
                                     const VkPresentInfoKHR *info) {
      std::vector<VkPipelineStageFlags> stages(info->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
      std::vector<VkResult> results(info->swapchainCount, VK_SUCCESS);
      bool submitted = false;
      if (info->waitSemaphoreCount) {
        VkSubmitInfo submit = {
          .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
          .waitSemaphoreCount = info->waitSemaphoreCount,
          .pWaitSemaphores = info->pWaitSemaphores,
          .pWaitDstStageMask = stages.data(),
        };
        VkResult result = dispatch.QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
        if (result < VK_SUCCESS)
          return result;
        submitted = true;
        // A queue-ordered wait alone does not synchronize a later presentation engine read.
        if (VkResult idle = dispatch.QueueWaitIdle(queue); idle < VK_SUCCESS)
          return AfterSubmission(idle);
      }

      VkResult result = VK_SUCCESS;
      for (uint32_t i = 0; i < info->swapchainCount; i++) {
        PresentInfoSlice single(*info, i, &results[i]);
        VkResult current = gamescopeSwapchains.find(info->pSwapchains[i])
          ? QueuePresentKHR(dispatch, queue, &single.info)
          : dispatch.QueuePresentKHR(queue, &single.info);
        if (submitted)
          current = AfterSubmission(current);
        results[i] = MergePresentResults(results[i], current);
        result = MergePresentResults(result, results[i]);
        if (current == VK_ERROR_OUT_OF_HOST_MEMORY || current == VK_ERROR_OUT_OF_DEVICE_MEMORY ||
            current == VK_ERROR_DEVICE_LOST) {
          std::fill(results.begin() + i + 1, results.end(), current);
          break;
        }
        submitted = true;
      }
      if (info->pResults)
        std::copy(results.begin(), results.end(), info->pResults);
      return result;
    }

    static VkResult QueuePresentKHR(
      const vkroots::VkQueueDispatch & pDispatch,
            VkQueue                    queue,
      const VkPresentInfoKHR*          pPresentInfo) {
      VkPresentInfoKHR presentInfo = *pPresentInfo;

      auto pPresentTimes = vkroots::FindInChain<const VkPresentTimesInfoGOOGLE>(&presentInfo);
      auto pPresentTimings = vkroots::FindInChain<const VkPresentTimingsInfoEXT>(&presentInfo);
      auto pPresentIds = vkroots::FindInChain<const VkPresentIdKHR>(&presentInfo);
      auto pPresentIds2 = vkroots::FindInChain<const VkPresentId2KHR>(&presentInfo);
      if (pPresentTimings && pPresentTimings->pTimingInfos && CanSplit(presentInfo)) {
        bool layer = false, native = false;
        for (uint32_t i = 0; i < presentInfo.swapchainCount; i++) {
          if (gamescopeSwapchains.find(presentInfo.pSwapchains[i]))
            layer = true;
          else
            native = true;
        }
        if (layer && native)
          return PresentSwapchains(pDispatch, queue, pPresentInfo);
      }
      bool anyLayer = false;
      // A report read now frees its slot for this present.
      for (uint32_t i = 0; i < presentInfo.swapchainCount; i++) {
        auto swapchain = gamescopeSwapchains.find(presentInfo.pSwapchains[i]);
        if (!swapchain)
          continue;
        anyLayer = true;
        if (waylandPumpEvents(swapchain->display, swapchain->eventQueue, !swapchain->isWayland) < 0 || *swapchain->retired) {
          if (CanSplit(presentInfo))
            return PresentSwapchains(pDispatch, queue, pPresentInfo);
          return PresentWithoutImage(pDispatch, queue, pPresentInfo, VK_ERROR_OUT_OF_DATE_KHR, false);
        }
      }
      if (!anyLayer) {
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
        return pDispatch.QueuePresentKHR(queue, pPresentInfo);
      }

      auto presentId = [&](uint32_t i) -> uint64_t {
        if (pPresentIds2 && pPresentIds2->pPresentIds)
          return pPresentIds2->pPresentIds[i];
        if (pPresentIds && pPresentIds->pPresentIds)
          return pPresentIds->pPresentIds[i];
        return 0;
      };

      auto timingRequest = [&](uint32_t i, const GamescopeSwapchainData *swapchain) -> const VkPresentTimingInfoEXT * {
        if (pPresentTimings && pPresentTimings->pTimingInfos && swapchain->presentTimingEnabled)
          return &pPresentTimings->pTimingInfos[i];
        return nullptr;
      };

      // Refuse before any slot is taken. The compositor must never see it.
      for (uint32_t i = 0; i < presentInfo.swapchainCount; i++) {
        auto swapchain = gamescopeSwapchains.find(presentInfo.pSwapchains[i]);
        if (!swapchain)
          continue;
        if (auto *timing = timingRequest(i, swapchain); timing && timing->presentStageQueries) {
          std::lock_guard lock(*swapchain->presentTimingMutex);
          if (swapchain->timingQueue.hasSlot())
            continue;
        } else {
          continue;
        }
        // Past the lock, as the split re-enters here for this swapchain.
        if (CanSplit(presentInfo))
          return PresentSwapchains(pDispatch, queue, pPresentInfo);
        return PresentWithoutImage(pDispatch, queue, pPresentInfo, VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT, true);
      }

      std::vector<VkResult> timingResults(presentInfo.swapchainCount, VK_SUCCESS);
      std::vector<uint64_t> timingSerials(presentInfo.swapchainCount, 0);
      std::vector<bool> timingArmed(presentInfo.swapchainCount, false);
      for (uint32_t i = 0; i < presentInfo.swapchainCount; i++) {
        auto swapchain = gamescopeSwapchains.find(presentInfo.pSwapchains[i]);
        if (!swapchain)
          continue;
        if (auto *pTiming = timingRequest(i, swapchain)) {
          const auto &timing = *pTiming;
          const bool target = timing.targetTime || (timing.flags & VK_PRESENT_TIMING_INFO_PRESENT_AT_RELATIVE_TIME_BIT_EXT);
          const bool bypass = swapchain->isBypassingXWayland && !swapchain->isWayland;
          std::lock_guard lock(*swapchain->presentTimingMutex);
          uint64_t serial = 0;
          // The one advertised domain is CLOCK_MONOTONIC, so the target stage and domain id are moot.
          if (timing.presentStageQueries)
            serial = timingSerials[i] = *swapchain->timingQueue.reserve(presentId(i), timing.targetTime, timing.presentStageQueries);
          // A target needs bypass. Forward so the driver releases the image.
          if (!bypass) {
            if (serial)
              swapchain->timingQueue.complete(serial, 0, 0, 0);
            if (target)
              timingResults[i] = VK_ERROR_OUT_OF_DATE_KHR;
          } else if (serial || target) {
            if (!serial)
              serial = swapchain->timingQueue.nextSerial();
            timingArmed[i] = true;
            gamescope_swapchain_set_present_timing(swapchain->object, serial >> 32, uint32_t(serial),
              timing.targetTime >> 32, uint32_t(timing.targetTime), timing.flags);
          }
        } else if (pPresentTimes && pPresentTimes->pTimes) {
          assert(pPresentTimes->swapchainCount == presentInfo.swapchainCount);

#if GAMESCOPE_WSI_DISPLAY_TIMING_DEBUG
          fprintf(stderr, "[Gamescope WSI] QueuePresentKHR: presentID: %u - desiredPresentTime: %lu - now: %lu\n", pPresentTimes->pTimes[i].presentID, pPresentTimes->pTimes[i].desiredPresentTime, getTimeMonotonic());
#endif
          gamescope_swapchain_set_present_time(swapchain->object, pPresentTimes->pTimes[i].presentID,
            pPresentTimes->pTimes[i].desiredPresentTime >> 32, uint32_t(pPresentTimes->pTimes[i].desiredPresentTime));
        }
      }

      // The driver's swapchains were created without the timing flag.
      ChainRemoval<VkPresentTimingsInfoEXT> removeTiming(&presentInfo);

      // All VkSurfaceKHR's come from the same VkInstance, so we only need to check one surface.
      bool frameLimiterAware = [&]() {
        for (uint32_t i = 0; i < presentInfo.swapchainCount; i++) {
          if (auto gamescopeSwapchain = gamescopeSwapchains.find(presentInfo.pSwapchains[i])) {
              auto gamescopeSurface = gamescopeSurfaces.find(gamescopeSwapchain->surface);
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

      ChainRemoval<VkSwapchainPresentModeInfoEXT> removeModes(&presentInfo);
      std::vector<VkPresentModeKHR> driverModes;
      bool allLayer = true;
      for (uint32_t i = 0; i < presentInfo.swapchainCount; i++) {
        auto swapchain = gamescopeSwapchains.find(presentInfo.pSwapchains[i]);
        allLayer &= swapchain != nullptr;
        driverModes.push_back(swapchain ? VK_PRESENT_MODE_MAILBOX_KHR :
          (oOriginalPresentModeInfo ? oOriginalPresentModeInfo->pPresentModes[i] : VK_PRESENT_MODE_MAX_ENUM_KHR));
      }
      VkSwapchainPresentModeInfoEXT driverModeInfo = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT,
        .pNext = presentInfo.pNext,
        .swapchainCount = presentInfo.swapchainCount,
        .pPresentModes = driverModes.data(),
      };
      // Without an explicit mode for a non-layer swapchain, leave its creation
      // mode in effect. Layer swapchains were already created as MAILBOX.
      if (allLayer || oOriginalPresentModeInfo)
        presentInfo.pNext = &driverModeInfo;

      // After the pump, so the state reflects events received this frame.
      bool forceFifo = [&]() {
        for (uint32_t i = 0; i < presentInfo.swapchainCount; i++) {
          if (auto gamescopeSwapchain = gamescopeSwapchains.find(presentInfo.pSwapchains[i])) {
            if (auto gamescopeSurface = gamescopeSurfaces.find(gamescopeSwapchain->surface))
              return gamescopeIsForcingFifo(gamescopeSurface->waylandObjects);
          }
        }
        return false;
      }();

      for (uint32_t i = 0; i < presentInfo.swapchainCount; i++) {
        if (auto gamescopeSwapchain = gamescopeSwapchains.find(presentInfo.pSwapchains[i])) {
          auto gamescopeSurface = gamescopeSurfaces.find(gamescopeSwapchain->surface);
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

      std::vector<VkResult> driverResults(presentInfo.swapchainCount, VK_SUCCESS);
      if (!presentInfo.pResults)
        presentInfo.pResults = driverResults.data();
      VkResult result = pDispatch.QueuePresentKHR(queue, &presentInfo);
      const bool driverFailed = result < VK_SUCCESS;

      for (uint32_t i = 0; i < presentInfo.swapchainCount; i++) {
        VkSwapchainKHR swapchain = presentInfo.pSwapchains[i];

        const bool swapchainDriverFailed = presentInfo.pResults[i] < VK_SUCCESS;
        auto UpdateSwapchainResult = [&](VkResult newResult) {
          if (!swapchainDriverFailed)
            presentInfo.pResults[i] = MergePresentResults(presentInfo.pResults[i], newResult);
          if (!driverFailed)
            result = MergePresentResults(result, newResult);
        };

        if (auto gamescopeSwapchain = gamescopeSwapchains.find(swapchain)) {
          if (timingArmed[i] && presentInfo.pResults[i] < VK_SUCCESS)
            gamescope_swapchain_set_present_timing(gamescopeSwapchain->object, 0, 0, 0, 0, 0);
          if (timingSerials[i] && presentInfo.pResults[i] < VK_SUCCESS) {
            std::lock_guard lock(*gamescopeSwapchain->presentTimingMutex);
            gamescopeSwapchain->timingQueue.complete(timingSerials[i], 0, 0, 0);
          }
          if (timingResults[i] != VK_SUCCESS)
            UpdateSwapchainResult(timingResults[i]);
          // If we are a frame limiter aware application like DXVK or VKD3D-Proton, we don't
          // transparently change their vkQueuePresent to just FIFO modes, we change what is
          // exposed as supported in order for them to handle presentation latency like they
          // would as in FIFO mode.
          if (frameLimiterAware && gamescopeSwapchain->forceFifo != forceFifo) {
              fprintf(stderr, "[Gamescope WSI] Forcing swapchain recreation as frame limiter changed, and we want the app to know the exposed modes changed.\n");
              UpdateSwapchainResult(VK_ERROR_OUT_OF_DATE_KHR);
          }

          auto gamescopeSurface = gamescopeSurfaces.find(gamescopeSwapchain->surface);
          if (!gamescopeSurface) {
            fprintf(stderr, "[Gamescope WSI] QueuePresentKHR: Surface for swapchain %u was already destroyed. (App use after free).\n", i);
            abort();
            continue;
          }

          const bool canBypass = gamescopeSurface->canBypassXWayland(gamescopeSwapchain->isHdrColorspace);
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
      const vkroots::VkDeviceDispatch & pDispatch,
            VkDevice                   device,
            uint32_t                   swapchainCount,
      const VkSwapchainKHR*            pSwapchains,
      const VkHdrMetadataEXT*          pMetadata) {
      for (uint32_t i = 0; i < swapchainCount; i++) {
        auto gamescopeSwapchain = gamescopeSwapchains.find(pSwapchains[i]);
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

    static VkResult SetSwapchainPresentTimingQueueSizeEXT(
      const vkroots::VkDeviceDispatch &dispatch, VkDevice device, VkSwapchainKHR swapchain, uint32_t size) {
      auto state = gamescopeSwapchains.find(swapchain);
      if (!state)
        return dispatch.SetSwapchainPresentTimingQueueSizeEXT(device, swapchain, size);
      // A report read now frees a slot, and the spec allows no error here.
      waylandPumpEvents(state->display, state->eventQueue, !state->isWayland);
      std::lock_guard lock(*state->presentTimingMutex);
      return state->timingQueue.setSize(size);
    }

    static VkResult GetSwapchainTimingPropertiesEXT(
      const vkroots::VkDeviceDispatch &dispatch, VkDevice device, VkSwapchainKHR swapchain,
      VkSwapchainTimingPropertiesEXT *properties, uint64_t *counter) {
      auto state = gamescopeSwapchains.find(swapchain);
      if (!state)
        return dispatch.GetSwapchainTimingPropertiesEXT(device, swapchain, properties, counter);
      if (waylandPumpEvents(state->display, state->eventQueue, !state->isWayland) < 0)
        return VK_ERROR_SURFACE_LOST_KHR;
      std::lock_guard lock(*state->presentTimingMutex);
      return state->timingQueue.getTimingProperties(properties, counter);
    }

    static VkResult GetSwapchainTimeDomainPropertiesEXT(
      const vkroots::VkDeviceDispatch &dispatch, VkDevice device, VkSwapchainKHR swapchain,
      VkSwapchainTimeDomainPropertiesEXT *properties, uint64_t *counter) {
      auto state = gamescopeSwapchains.find(swapchain);
      if (!state)
        return dispatch.GetSwapchainTimeDomainPropertiesEXT(device, swapchain, properties, counter);
      std::lock_guard lock(*state->presentTimingMutex);
      return state->timingQueue.getTimeDomainProperties(properties, counter);
    }

    static VkResult GetPastPresentationTimingEXT(
      const vkroots::VkDeviceDispatch &dispatch, VkDevice device,
      const VkPastPresentationTimingInfoEXT *info, VkPastPresentationTimingPropertiesEXT *properties) {
      auto state = gamescopeSwapchains.find(info->swapchain);
      if (!state)
        return dispatch.GetPastPresentationTimingEXT(device, info, properties);
      if (waylandPumpEvents(state->display, state->eventQueue, !state->isWayland) < 0)
        return VK_ERROR_SURFACE_LOST_KHR;
      std::lock_guard lock(*state->presentTimingMutex);
      return state->timingQueue.getPast(info, properties);
    }

    static VkResult GetCalibratedTimestampsKHR(
      const vkroots::VkDeviceDispatch &dispatch, VkDevice device, uint32_t count,
      const VkCalibratedTimestampInfoKHR *infos, uint64_t *timestamps, uint64_t *maxDeviation) {
      return calibratePresentTimestamps(std::bind_front(&vkroots::VkDeviceDispatch::GetCalibratedTimestampsKHR, &dispatch),
        device, count, infos, timestamps, maxDeviation);
    }

    static VkResult GetCalibratedTimestampsEXT(
      const vkroots::VkDeviceDispatch &dispatch, VkDevice device, uint32_t count,
      const VkCalibratedTimestampInfoKHR *infos, uint64_t *timestamps, uint64_t *maxDeviation) {
      return calibratePresentTimestamps(std::bind_front(&vkroots::VkDeviceDispatch::GetCalibratedTimestampsEXT, &dispatch),
        device, count, infos, timestamps, maxDeviation);
    }

    static VkResult GetPastPresentationTimingGOOGLE(
      const vkroots::VkDeviceDispatch &      pDispatch,
            VkDevice                        device,
            VkSwapchainKHR                  swapchain,
            uint32_t*                       pPresentationTimingCount,
            VkPastPresentationTimingGOOGLE* pPresentationTimings) {
      auto gamescopeSwapchain = gamescopeSwapchains.find(swapchain);
      if (!gamescopeSwapchain) {
        fprintf(stderr, "[Gamescope WSI] GetPastPresentationTimingGOOGLE: Not a gamescope swapchain.\n");
        return VK_ERROR_SURFACE_LOST_KHR;
      }

      // Dispatch to get the latest timings.
      if (waylandPumpEvents(gamescopeSwapchain->display, gamescopeSwapchain->eventQueue, !gamescopeSwapchain->isWayland) < 0)
        return VK_ERROR_SURFACE_LOST_KHR;

      std::unique_lock lock(*gamescopeSwapchain->presentTimingMutex);
      auto& timings = gamescopeSwapchain->pastPresentTimings;

      VkResult result = vkroots::array(timings, pPresentationTimingCount, pPresentationTimings);
      // Erase those that we returned so we don't return them again.
      if (pPresentationTimings)
        timings.erase(timings.begin(), timings.begin() + *pPresentationTimingCount);

      return result;
    }

    static VkResult GetRefreshCycleDurationGOOGLE(
      const vkroots::VkDeviceDispatch &      pDispatch,
            VkDevice                        device,
            VkSwapchainKHR                  swapchain,
            VkRefreshCycleDurationGOOGLE*   pDisplayTimingProperties) {
      auto gamescopeSwapchain = gamescopeSwapchains.find(swapchain);
      if (!gamescopeSwapchain) {
        fprintf(stderr, "[Gamescope WSI] GetRefreshCycleDurationGOOGLE: Not a gamescope swapchain.\n");
        return VK_ERROR_SURFACE_LOST_KHR;
      }

      // Dispatch to get the latest cycle.
      if (waylandPumpEvents(gamescopeSwapchain->display, gamescopeSwapchain->eventQueue, !gamescopeSwapchain->isWayland) < 0)
        return VK_ERROR_SURFACE_LOST_KHR;

      std::unique_lock lock(*gamescopeSwapchain->presentTimingMutex);
      pDisplayTimingProperties->refreshDuration = gamescopeSwapchain->refreshCycle;

      return VK_SUCCESS;
    }

  };

}

VKROOTS_DEFINE_LAYER_INTERFACES(GamescopeWSILayer::VkInstanceOverrides,
                                GamescopeWSILayer::VkDeviceOverrides);
