#define VK_USE_PLATFORM_WAYLAND_KHR
#define VK_USE_PLATFORM_XCB_KHR
#define VK_USE_PLATFORM_XLIB_KHR
#include "vkroots.h"

// Exercise the actual layer and its loader interface with a fake driver.
#include "../layer/VkLayer_FROG_gamescope_wsi.cpp"

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <sys/socket.h>

namespace {

uint32_t destroyedInstances = 0;
uint32_t destroyedDevices = 0;

void VKAPI_CALL destroyInstance(VkInstance instance, const VkAllocationCallbacks *) {
  REQUIRE(vkroots::LookupDispatch(instance) == nullptr);
  destroyedInstances++;
}

void VKAPI_CALL destroyDevice(VkDevice device, const VkAllocationCallbacks *) {
  REQUIRE(vkroots::LookupDispatch(device) == nullptr);
  destroyedDevices++;
}

VkResult VKAPI_CALL enumeratePhysicalDevices(VkInstance, uint32_t *count, VkPhysicalDevice *) {
  *count = 0;
  return VK_SUCCESS;
}

PFN_vkVoidFunction VKAPI_CALL getInstanceProc(VkInstance, const char *name) {
  if (std::strcmp(name, "vkGetInstanceProcAddr") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(getInstanceProc);
  if (std::strcmp(name, "vkDestroyInstance") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(destroyInstance);
  if (std::strcmp(name, "vkEnumeratePhysicalDevices") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(enumeratePhysicalDevices);
  return nullptr;
}

struct RefusedPresent {
  uint32_t submits = 0;
  uint32_t idles = 0;
  std::vector<VkSwapchainKHR> released;
  VkResult firstReleaseResult = VK_SUCCESS;
  VkResult idleResult = VK_SUCCESS;
  uint32_t failSubmit = 0;
  std::vector<VkSemaphore> waits;
  std::vector<VkFence> fences;
  std::vector<VkFrameBoundaryFlagsEXT> boundaries;
} refused;

VkResult VKAPI_CALL submit(VkQueue, uint32_t count, const VkSubmitInfo *infos, VkFence fence) {
  refused.submits++;
  if (refused.submits == refused.failSubmit)
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  for (uint32_t i = 0; i < count; i++) {
    if (auto *boundary = vkroots::FindInChain<VkFrameBoundaryEXT>(&infos[i]))
      refused.boundaries.push_back(boundary->flags);
    for (uint32_t j = 0; j < infos[i].waitSemaphoreCount; j++)
      refused.waits.push_back(infos[i].pWaitSemaphores[j]);
  }
  if (fence)
    refused.fences.push_back(fence);
  return VK_SUCCESS;
}

VkResult VKAPI_CALL waitIdle(VkQueue) {
  refused.idles++;
  return refused.idleResult;
}

VkResult VKAPI_CALL releaseImages(VkDevice, const VkReleaseSwapchainImagesInfoKHR *info) {
  REQUIRE(info->imageIndexCount == 1);
  refused.released.push_back(info->swapchain);
  return refused.released.size() == 1 ? refused.firstReleaseResult : VK_SUCCESS;
}

struct PresentedSwapchain {
  VkSwapchainKHR swapchain;
  uint32_t image;
  uint64_t id;
  uint64_t target;
  VkPresentStageFlagsEXT stages;
  VkFence fence;
};
std::vector<PresentedSwapchain> presented;
uint32_t failPresent = 0;
uint32_t presentCalls = 0;

VkResult VKAPI_CALL present(VkQueue, const VkPresentInfoKHR *info) {
  presentCalls++;
  if (presentCalls == failPresent)
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  if (auto *boundary = vkroots::FindInChain<VkFrameBoundaryEXT>(info))
    refused.boundaries.push_back(boundary->flags);
  auto *ids = vkroots::FindInChain<VkPresentId2KHR>(info);
  auto *timings = vkroots::FindInChain<VkPresentTimingsInfoEXT>(info);
  auto *fences = vkroots::FindInChain<VkSwapchainPresentFenceInfoEXT>(info);
  for (uint32_t i = 0; i < info->waitSemaphoreCount; i++)
    refused.waits.push_back(info->pWaitSemaphores[i]);
  for (uint32_t i = 0; i < info->swapchainCount; i++) {
    const auto *timing = timings && timings->pTimingInfos ? &timings->pTimingInfos[i] : nullptr;
    presented.push_back({info->pSwapchains[i], info->pImageIndices[i], ids ? ids->pPresentIds[i] : 0,
      timing ? timing->targetTime : 0, timing ? timing->presentStageQueries : 0,
      fences ? fences->pFences[i] : VK_NULL_HANDLE});
    if (info->pResults)
      info->pResults[i] = VK_SUCCESS;
  }
  return VK_SUCCESS;
}

PFN_vkVoidFunction VKAPI_CALL getDeviceProc(VkDevice, const char *name) {
  if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(getDeviceProc);
  if (std::strcmp(name, "vkDestroyDevice") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(destroyDevice);
  if (std::strcmp(name, "vkQueuePresentKHR") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(present);
  if (std::strcmp(name, "vkQueueSubmit") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(submit);
  if (std::strcmp(name, "vkQueueWaitIdle") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(waitIdle);
  if (std::strcmp(name, "vkReleaseSwapchainImagesEXT") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(releaseImages);
  return nullptr;
}

struct DriverCalibration {
  bool ext = false;
  bool khr = false;
  uint32_t count = 0;
  uint32_t calls = 0;
  const VkCalibratedTimestampInfoKHR *original = nullptr;
  std::vector<VkTimeDomainKHR> domains;
  VkResult result = VK_SUCCESS;
  uint64_t deviation = 7;
} driver;

VkResult VKAPI_CALL calibrate(VkDevice, uint32_t count, const VkCalibratedTimestampInfoKHR *infos,
                              uint64_t *times, uint64_t *deviation) {
  driver.calls++;
  driver.count = count;
  driver.original = infos;
  for (uint32_t i = 0; i < count; i++) {
    driver.domains.push_back(infos[i].timeDomain);
    times[i] = 1000 + i;
  }
  *deviation = driver.deviation;
  return driver.result;
}

PFN_vkVoidFunction VKAPI_CALL getCalibrationProc(VkDevice device, const char *name) {
  if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(getCalibrationProc);
  if (std::strcmp(name, "vkGetCalibratedTimestampsEXT") == 0)
    return driver.ext ? reinterpret_cast<PFN_vkVoidFunction>(calibrate) : nullptr;
  if (std::strcmp(name, "vkGetCalibratedTimestampsKHR") == 0)
    return driver.khr ? reinterpret_cast<PFN_vkVoidFunction>(calibrate) : nullptr;
  return getDeviceProc(device, name);
}

VkResult VKAPI_CALL calibrationDomains(VkPhysicalDevice, uint32_t *count, VkTimeDomainKHR *domains) {
  const std::array values{ VK_TIME_DOMAIN_DEVICE_KHR, VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR };
  return vkroots::array(values, count, domains);
}

VkResult VKAPI_CALL calibrationExtensions(VkPhysicalDevice, const char *, uint32_t *count, VkExtensionProperties *properties) {
  std::vector<VkExtensionProperties> extensions{
    { VK_KHR_PRESENT_ID_2_EXTENSION_NAME, VK_KHR_PRESENT_ID_2_SPEC_VERSION },
  };
  if (driver.khr)
    extensions.push_back({ VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME, VK_KHR_CALIBRATED_TIMESTAMPS_SPEC_VERSION });
  if (driver.ext)
    extensions.push_back({ VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME, VK_EXT_CALIBRATED_TIMESTAMPS_SPEC_VERSION });
  return vkroots::array(extensions, count, properties);
}

PFN_vkVoidFunction VKAPI_CALL getCalibrationInstanceProc(VkInstance instance, const char *name) {
  if (std::strcmp(name, "vkGetInstanceProcAddr") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(getCalibrationInstanceProc);
  if (std::strcmp(name, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT") == 0)
    return driver.ext ? reinterpret_cast<PFN_vkVoidFunction>(calibrationDomains) : nullptr;
  if (std::strcmp(name, "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR") == 0)
    return driver.khr ? reinterpret_cast<PFN_vkVoidFunction>(calibrationDomains) : nullptr;
  if (std::strcmp(name, "vkEnumerateDeviceExtensionProperties") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(calibrationExtensions);
  return getInstanceProc(instance, name);
}

const VkSwapchainKHR layerSwapchain = reinterpret_cast<VkSwapchainKHR>(uintptr_t(1));
const VkSwapchainKHR driverSwapchain = reinterpret_cast<VkSwapchainKHR>(uintptr_t(2));

struct Calibration {
  VkSwapchainCalibratedTimestampInfoEXT local = {
    .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT,
    .swapchain = layerSwapchain,
    .presentStage = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT,
    .timeDomainId = 1,
  };
  VkSwapchainCalibratedTimestampInfoEXT foreign = {
    .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT,
    .swapchain = driverSwapchain,
    .presentStage = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT,
    .timeDomainId = 8,
  };
  std::array<VkCalibratedTimestampInfoKHR, 4> infos = {{
    { VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, &local, VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT },
    { VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, nullptr, VK_TIME_DOMAIN_DEVICE_KHR },
    { VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, &local, VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT },
    { VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, &foreign, VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT },
  }};
  std::array<uint64_t, 4> timestamps{};
  uint64_t deviation = 0;
  uint64_t before = 0;
  uint64_t after = 0;

  Calibration() {
    driver = {};
    GamescopeWSILayer::gamescopeSwapchains.erase(layerSwapchain);
    REQUIRE(GamescopeWSILayer::gamescopeSwapchains.create(layerSwapchain, GamescopeWSILayer::GamescopeSwapchainData{}) != nullptr);
  }

  ~Calibration() {
    GamescopeWSILayer::gamescopeSwapchains.erase(layerSwapchain);
  }

  VkResult run(uint32_t count) {
    before = GamescopeWSILayer::getTimeMonotonic();
    VkResult result = GamescopeWSILayer::calibratePresentTimestamps(calibrate, VK_NULL_HANDLE, count,
      infos.data(), timestamps.data(), &deviation);
    after = GamescopeWSILayer::getTimeMonotonic();
    return result;
  }

  bool sampledDuringCall(size_t i) const {
    return timestamps[i] >= before && timestamps[i] <= after;
  }
};

}

TEST_CASE("Local-only present calibration does not call the driver", "[present_timing_layer]") {
  Calibration calibration;
  REQUIRE(calibration.run(1) == VK_SUCCESS);
  REQUIRE(driver.calls == 0);
  REQUIRE(calibration.sampledDuringCall(0));
  REQUIRE(calibration.deviation <= calibration.after - calibration.before);
}

TEST_CASE("Timestamp lookup preserves enabled aliases for Zink fallback", "[present_timing_layer][calibration_lookup]") {
  Calibration calibration;
  SECTION("KHR only") { driver.khr = true; }
  SECTION("EXT only") { driver.ext = true; }
  SECTION("Both aliases") { driver.khr = driver.ext = true; }
  SECTION("Neither alias") {}

  VkInstance instance = reinterpret_cast<VkInstance>(uintptr_t(201));
  VkDevice device = reinterpret_cast<VkDevice>(uintptr_t(202));
  VkDeviceCreateInfo createInfo{ .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
  auto *instanceDispatch = vkroots::tables::InstanceDispatches.create(instance, getInstanceProc, instance, getInstanceProc);
  REQUIRE(instanceDispatch != nullptr);
  REQUIRE(vkroots::tables::DeviceDispatches.create(device, getCalibrationProc, device,
    VK_NULL_HANDLE, nullptr, &createInfo) != nullptr);
  struct Cleanup {
    VkDevice device;
    VkInstance instance;
    ~Cleanup() {
      vkroots::tables::DestroyDispatchTable(device);
      vkroots::tables::DestroyDispatchTable(instance);
    }
  } cleanup{ device, instance };

  VkNegotiateLayerInterface loader{ .sType = LAYER_NEGOTIATE_INTERFACE_STRUCT, .loaderLayerInterfaceVersion = 2 };
  REQUIRE(vkNegotiateLoaderLayerInterfaceVersion(&loader) == VK_SUCCESS);
  auto instanceProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(loader.pfnGetInstanceProcAddr(instance, "vkGetInstanceProcAddr"));
  REQUIRE(instanceProc != nullptr);
  const std::array lookups = {
    loader.pfnGetDeviceProcAddr,
    reinterpret_cast<PFN_vkGetDeviceProcAddr>(loader.pfnGetInstanceProcAddr(instance, "vkGetDeviceProcAddr")),
    reinterpret_cast<PFN_vkGetDeviceProcAddr>(loader.pfnGetDeviceProcAddr(device, "vkGetDeviceProcAddr")),
    reinterpret_cast<PFN_vkGetDeviceProcAddr>(instanceProc(instance, "vkGetDeviceProcAddr")),
  };
  for (auto lookup : lookups) {
    CHECK(lookup != nullptr);
    if (!lookup)
      continue;
    CHECK(lookup(device, "vkGetPastPresentationTimingEXT") != nullptr);
    for (const auto &[name, enabled] : std::array{
        std::pair{ "vkGetCalibratedTimestampsEXT", driver.ext },
        std::pair{ "vkGetCalibratedTimestampsKHR", driver.khr } }) {
      auto timestamp = reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(lookup(device, name));
      CHECK((timestamp != nullptr) == enabled);
      if (!enabled || !timestamp)
        continue;

      // Enabled aliases still pass through the layer for mixed local/device queries.
      calibration.before = GamescopeWSILayer::getTimeMonotonic();
      REQUIRE(timestamp(device, 2, calibration.infos.data(), calibration.timestamps.data(), &calibration.deviation) == VK_SUCCESS);
      calibration.after = GamescopeWSILayer::getTimeMonotonic();
      CHECK(calibration.sampledDuringCall(0));
      CHECK(calibration.timestamps[1] == 1000);
    }
  }
}

TEST_CASE("Time-domain lookup preserves available aliases and local domains", "[present_timing_layer][calibration_lookup][calibration_domains]") {
  driver = {};
  SECTION("KHR only") { driver.khr = true; }
  SECTION("EXT only") { driver.ext = true; }
  SECTION("Both aliases") { driver.khr = driver.ext = true; }
  SECTION("Neither alias") {}

  VkInstance instance = reinterpret_cast<VkInstance>(uintptr_t(203));
  VkPhysicalDevice physicalDevice = reinterpret_cast<VkPhysicalDevice>(uintptr_t(204));
  auto *dispatch = vkroots::tables::InstanceDispatches.create(instance, getCalibrationInstanceProc, instance, getCalibrationInstanceProc);
  REQUIRE(dispatch != nullptr);
  REQUIRE(vkroots::tables::AssignDispatchTable(physicalDevice, dispatch) != nullptr);
  REQUIRE(GamescopeWSILayer::gamescopeInstances.create(instance, GamescopeWSILayer::GamescopeInstanceData{
    .swapchainFactoryVersion = 2,
  }) != nullptr);
  struct Cleanup {
    VkPhysicalDevice physicalDevice;
    VkInstance instance;
    ~Cleanup() {
      GamescopeWSILayer::gamescopeInstances.erase(instance);
      vkroots::tables::UnassignDispatchTable(physicalDevice);
      vkroots::tables::DestroyDispatchTable(instance);
    }
  } cleanup{ physicalDevice, instance };

  VkNegotiateLayerInterface loader{ .sType = LAYER_NEGOTIATE_INTERFACE_STRUCT, .loaderLayerInterfaceVersion = 2 };
  REQUIRE(vkNegotiateLoaderLayerInterfaceVersion(&loader) == VK_SUCCESS);
  const std::array lookups = {
    loader.pfnGetInstanceProcAddr,
    reinterpret_cast<PFN_vkGetInstanceProcAddr>(loader.pfnGetInstanceProcAddr(instance, "vkGetInstanceProcAddr")),
    loader.pfnGetPhysicalDeviceProcAddr,
    reinterpret_cast<PFN_GetPhysicalDeviceProcAddr>(loader.pfnGetInstanceProcAddr(instance, "vk_layerGetPhysicalDeviceProcAddr")),
  };
  for (auto lookup : lookups) {
    REQUIRE(lookup != nullptr);
    for (const auto &[name, available] : std::array{
        std::pair{ "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT", driver.ext },
        std::pair{ "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR", driver.khr } }) {
      auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR>(lookup(instance, name));
      CHECK((query != nullptr) == available);
      if (!available || !query)
        continue;

      uint32_t count = 0;
      REQUIRE(query(physicalDevice, &count, nullptr) == VK_SUCCESS);
      CHECK(count == (driver.khr ? 3u : 2u));
      std::array<VkTimeDomainKHR, 3> domains{};
      count = domains.size();
      REQUIRE(query(physicalDevice, &count, domains.data()) == VK_SUCCESS);
      CHECK(domains[0] == VK_TIME_DOMAIN_DEVICE_KHR);
      CHECK(domains[1] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR);
      if (driver.khr)
        CHECK(domains[2] == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT);
    }
  }
}

TEST_CASE("Present calibration merges both local aliases with driver timestamps", "[present_timing_layer]") {
  Calibration calibration;
  REQUIRE(calibration.run(4) == VK_SUCCESS);
  REQUIRE(driver.calls == 1);
  REQUIRE(driver.count == 2);
  REQUIRE(driver.domains == std::vector<VkTimeDomainKHR>{VK_TIME_DOMAIN_DEVICE_KHR, VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT});
  REQUIRE(calibration.sampledDuringCall(0));
  REQUIRE(calibration.sampledDuringCall(2));
  REQUIRE(calibration.timestamps[0] == calibration.timestamps[2]);
  REQUIRE(calibration.timestamps[1] == 1000);
  REQUIRE(calibration.timestamps[3] == 1001);
  REQUIRE(calibration.deviation >= 7);
  REQUIRE(calibration.infos[0].pNext == &calibration.local);
  REQUIRE(calibration.infos[3].pNext == &calibration.foreign);
}

TEST_CASE("Non-layer calibration is forwarded untouched", "[present_timing_layer]") {
  Calibration calibration;
  calibration.infos[0].pNext = &calibration.foreign;
  REQUIRE(calibration.run(1) == VK_SUCCESS);
  REQUIRE(driver.original == calibration.infos.data());
  REQUIRE(calibration.timestamps[0] == 1000);
  REQUIRE(calibration.deviation == 7);
}

TEST_CASE("Present calibration preserves driver errors", "[present_timing_layer]") {
  Calibration calibration;
  driver.result = VK_ERROR_DEVICE_LOST;
  REQUIRE(calibration.run(2) == VK_ERROR_DEVICE_LOST);
  REQUIRE(calibration.timestamps == std::array<uint64_t, 4>{});
  REQUIRE(calibration.deviation == 0);
}

TEST_CASE("Present calibration deviation saturates", "[present_timing_layer]") {
  Calibration calibration;
  driver.deviation = std::numeric_limits<uint64_t>::max();
  REQUIRE(calibration.run(2) == VK_SUCCESS);
  REQUIRE(calibration.deviation == std::numeric_limits<uint64_t>::max());
}

TEST_CASE("A refused present releases every swapchain's image and reports each", "[present_timing_layer]") {
  refused = {};
  refused.firstReleaseResult = VK_ERROR_SURFACE_LOST_KHR;
  VkDeviceCreateInfo createInfo{ .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
  vkroots::VkDeviceDispatch dispatch(getDeviceProc, VK_NULL_HANDLE, VK_NULL_HANDLE, nullptr, &createInfo);
  vkroots::VkQueueDispatch queueDispatch(VK_NULL_HANDLE, &dispatch);
  std::array<VkSwapchainKHR, 2> swapchains{reinterpret_cast<VkSwapchainKHR>(uintptr_t(1)), reinterpret_cast<VkSwapchainKHR>(uintptr_t(2))};
  std::array<uint32_t, 2> images{0, 1};
  std::array<VkResult, 2> results{};
  VkSemaphore semaphore = reinterpret_cast<VkSemaphore>(uintptr_t(5));
  VkPresentInfoKHR info{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &semaphore, 2,
    swapchains.data(), images.data(), results.data() };

  REQUIRE(GamescopeWSILayer::VkDeviceOverrides::PresentWithoutImage(queueDispatch, VK_NULL_HANDLE, &info,
    VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT, true) == VK_ERROR_SURFACE_LOST_KHR);
  REQUIRE(refused.submits == 1);
  REQUIRE(refused.idles == 1);
  REQUIRE(refused.released == std::vector<VkSwapchainKHR>{swapchains[0], swapchains[1]});
  REQUIRE(results[0] == VK_ERROR_SURFACE_LOST_KHR);
  REQUIRE(results[1] == VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT);
}

TEST_CASE("A refused present cannot report OOM after its waits were submitted", "[present_timing_layer]") {
  refused = {};
  refused.idleResult = VK_ERROR_OUT_OF_HOST_MEMORY;
  VkDeviceCreateInfo createInfo{ .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
  vkroots::VkDeviceDispatch dispatch(getDeviceProc, VK_NULL_HANDLE, VK_NULL_HANDLE, nullptr, &createInfo);
  vkroots::VkQueueDispatch queueDispatch(VK_NULL_HANDLE, &dispatch);
  VkSwapchainKHR swapchain = reinterpret_cast<VkSwapchainKHR>(uintptr_t(1));
  uint32_t image = 0;
  VkSemaphore semaphore = reinterpret_cast<VkSemaphore>(uintptr_t(5));
  VkPresentInfoKHR info{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &semaphore, 1, &swapchain, &image, nullptr };

  REQUIRE(GamescopeWSILayer::VkDeviceOverrides::PresentWithoutImage(queueDispatch, VK_NULL_HANDLE, &info,
    VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT, true) == VK_ERROR_DEVICE_LOST);
  REQUIRE(refused.submits == 1);
  REQUIRE(refused.released.empty());
}

TEST_CASE("Layer instance teardown does not read the erased dispatch", "[present_timing_layer]") {
  destroyedInstances = 0;
  VkInstance instance = reinterpret_cast<VkInstance>(uintptr_t(101));
  auto *dispatch = vkroots::tables::InstanceDispatches.create(instance, getInstanceProc, instance, getInstanceProc);
  REQUIRE(dispatch != nullptr);
  GamescopeWSILayer::VkInstanceOverrides::DestroyInstance(*dispatch, instance, nullptr);
  REQUIRE(destroyedInstances == 1);
  REQUIRE(vkroots::LookupDispatch(instance) == nullptr);
}

TEST_CASE("Layer device teardown does not read the erased dispatch", "[present_timing_layer]") {
  destroyedDevices = 0;
  VkDevice device = reinterpret_cast<VkDevice>(uintptr_t(102));
  VkDeviceCreateInfo createInfo{ .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
  auto *dispatch = vkroots::tables::DeviceDispatches.create(device, getDeviceProc, device,
    VK_NULL_HANDLE, nullptr, &createInfo);
  REQUIRE(dispatch != nullptr);
  GamescopeWSILayer::VkDeviceOverrides::DestroyDevice(*dispatch, device, nullptr);
  REQUIRE(destroyedDevices == 1);
  REQUIRE(vkroots::LookupDispatch(device) == nullptr);
}

namespace {

struct PresentBatch {
  int sockets[2];
  wl_display *display;
  wl_registry *registry;
  wl_event_queue *globalsQueue;
  gamescope_swapchain_factory_v2 *factory;
  std::array<VkSwapchainKHR, 2> swapchains{reinterpret_cast<VkSwapchainKHR>(uintptr_t(501)), reinterpret_cast<VkSwapchainKHR>(uintptr_t(502))};
  std::array<VkSurfaceKHR, 2> surfaces{reinterpret_cast<VkSurfaceKHR>(uintptr_t(601)), reinterpret_cast<VkSurfaceKHR>(uintptr_t(602))};
  std::array<GamescopeWSILayer::GamescopeSwapchainData *, 2> states{};
  std::array<wl_surface *, 2> wlSurfaces{};
  std::array<uint32_t, 2> images{2, 1};
  std::array<uint64_t, 2> ids{71, 92};
  std::array<VkFence, 2> fences{reinterpret_cast<VkFence>(uintptr_t(701)), reinterpret_cast<VkFence>(uintptr_t(702))};
  std::array<VkPresentTimingInfoEXT, 2> timings{{
    {.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT, .presentStageQueries = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT},
    {.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT, .targetTime = 9000, .presentStageQueries = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT},
  }};
  std::array<VkResult, 2> results{};
  VkPresentTimingsInfoEXT timingInfo{VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT, nullptr, 2, timings.data()};
  VkPresentId2KHR idInfo{VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR, &timingInfo, 2, ids.data()};
  VkSwapchainPresentFenceInfoEXT fenceInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT, &idInfo, 2, fences.data()};
  VkSemaphore semaphore = reinterpret_cast<VkSemaphore>(uintptr_t(801));
  VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, &fenceInfo, 1, &semaphore, 2, swapchains.data(), images.data(), results.data()};
  VkDeviceCreateInfo createInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  vkroots::VkDeviceDispatch device{getDeviceProc, VK_NULL_HANDLE, VK_NULL_HANDLE, nullptr, &createInfo};
  vkroots::VkQueueDispatch queue{VK_NULL_HANDLE, &device};

  explicit PresentBatch(bool foreign = false) {
    refused = {};
    presented.clear();
    presentCalls = failPresent = 0;
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    display = wl_display_connect_to_fd(sockets[0]);
    REQUIRE(display);
    registry = wl_display_get_registry(display);
    globalsQueue = wl_display_create_queue(display);
    factory = static_cast<gamescope_swapchain_factory_v2 *>(wl_registry_bind(registry, 1, &gamescope_swapchain_factory_v2_interface, 2));
    for (size_t i = 0; i < (foreign ? 1u : 2u); i++) {
      auto *eventQueue = wl_display_create_queue(display);
      auto *wrapper = static_cast<gamescope_swapchain_factory_v2 *>(wl_proxy_create_wrapper(factory));
      wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapper), eventQueue);
      wlSurfaces[i] = reinterpret_cast<wl_surface *>(wl_proxy_create(reinterpret_cast<wl_proxy *>(display), &wl_surface_interface));
      auto *object = gamescope_swapchain_factory_v2_create_swapchain(wrapper, wlSurfaces[i]);
      wl_proxy_wrapper_destroy(wrapper);
      // No X server is needed: the surface's native path makes the size/bypass check deterministic.
      REQUIRE(GamescopeWSILayer::gamescopeSurfaces.create(surfaces[i], GamescopeWSILayer::GamescopeSurfaceData{}) != nullptr);
      states[i] = GamescopeWSILayer::gamescopeSwapchains.create(swapchains[i], GamescopeWSILayer::GamescopeSwapchainData{
        .object = object, .display = display, .globalsQueue = globalsQueue, .eventQueue = eventQueue, .surface = surfaces[i],
        .isBypassingXWayland = true, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .presentTimingEnabled = true});
      REQUIRE(states[i]);
      states[i]->timingQueue.setSize(4);
    }
  }

  ~PresentBatch() {
    for (size_t i = 0; i < states.size(); i++) {
      if (!states[i]) continue;
      gamescope_swapchain_destroy(states[i]->object);
      wl_event_queue_destroy(states[i]->eventQueue);
      GamescopeWSILayer::gamescopeSwapchains.erase(swapchains[i]);
      GamescopeWSILayer::gamescopeSurfaces.erase(surfaces[i]);
      wl_surface_destroy(wlSurfaces[i]);
    }
    gamescope_swapchain_factory_v2_destroy(factory);
    wl_registry_destroy(registry);
    wl_event_queue_destroy(globalsQueue);
    wl_display_disconnect(display);
    close(sockets[1]);
  }

  VkResult run() { return GamescopeWSILayer::VkDeviceOverrides::QueuePresentKHR(queue, VK_NULL_HANDLE, &info); }
};

}

TEST_CASE("Mixed presents preserve the driver's timing request", "[present_timing_layer][present_batch]") {
  PresentBatch batch(true);
  CHECK(batch.run() == VK_SUCCESS);
  CHECK(presented.size() == 2);
  CHECK(presented[0].target == 0);
  CHECK(presented[1].target == batch.timings[1].targetTime);
  CHECK(presented[1].stages == batch.timings[1].presentStageQueries);
  CHECK(presented[1].id == 92);
  CHECK(presented[1].image == 1);
  CHECK(presented[1].fence == batch.fences[1]);
  CHECK(refused.waits == std::vector<VkSemaphore>{batch.semaphore});
  CHECK(batch.info.pNext == &batch.fenceInfo);
  CHECK(batch.fenceInfo.pNext == &batch.idInfo);
  CHECK(batch.idInfo.pNext == &batch.timingInfo);
}

TEST_CASE("A full timing queue does not reject the other hooked swapchain", "[present_timing_layer][present_batch]") {
  PresentBatch batch;
  batch.states[0]->timingQueue.setSize(1);
  REQUIRE(batch.states[0]->timingQueue.reserve(1, 0, VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT));
  batch.states[1]->timingQueue.setSize(1);
  CHECK(batch.run() == VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT);
  // The refused swapchain still holds one entry, the presented one took its slot.
  CHECK(batch.states[0]->timingQueue.setSize(1) == VK_SUCCESS);
  CHECK(!batch.states[1]->timingQueue.hasSlot());
  CHECK(batch.results[0] == VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT);
  CHECK(batch.results[1] == VK_SUCCESS);
  CHECK(refused.released == std::vector<VkSwapchainKHR>{batch.swapchains[0]});
  CHECK(presented.size() == 1);
  if (!presented.empty()) {
    CHECK(presented[0].swapchain == batch.swapchains[1]);
    CHECK(presented[0].id == 92);
    CHECK(presented[0].fence == batch.fences[1]);
  }
  CHECK(refused.fences == std::vector<VkFence>{batch.fences[0]});
  CHECK(refused.waits == std::vector<VkSemaphore>{batch.semaphore});
}

TEST_CASE("An untimed swapchain survives another swapchain's full queue", "[present_timing_layer][present_batch]") {
  PresentBatch batch;
  batch.states[1]->timingQueue.setSize(0);
  batch.timings[0].presentStageQueries = 0;
  CHECK(batch.run() == VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT);
  CHECK(batch.results[0] == VK_SUCCESS);
  CHECK(batch.results[1] == VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT);
  CHECK(presented.size() == 1);
  if (!presented.empty())
    CHECK(presented[0].swapchain == batch.swapchains[0]);
  CHECK(refused.released == std::vector<VkSwapchainKHR>{batch.swapchains[1]});
}

TEST_CASE("Split presents preserve allocation failure before any submission", "[present_timing_layer][present_batch]") {
  PresentBatch batch(true);
  refused.failSubmit = 1;
  batch.results.fill(VK_NOT_READY);
  CHECK(batch.run() == VK_ERROR_OUT_OF_HOST_MEMORY);
  CHECK(refused.waits.empty());
  CHECK(refused.fences.empty());
  CHECK(refused.released.empty());
  CHECK(presented.empty());
  CHECK(batch.results == std::array<VkResult, 2>{VK_NOT_READY, VK_NOT_READY});
}

TEST_CASE("Split presents convert allocation failure after waits to device loss", "[present_timing_layer][present_batch]") {
  PresentBatch batch(true);
  refused.idleResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
  CHECK(batch.run() == VK_ERROR_DEVICE_LOST);
  CHECK(refused.waits == std::vector<VkSemaphore>{batch.semaphore});
  CHECK(refused.fences.empty());
  CHECK(refused.released.empty());
  CHECK(presented.empty());
}

TEST_CASE("A native present failing after a hooked present cannot return OOM", "[present_timing_layer][present_batch]") {
  PresentBatch batch(true);
  batch.info.waitSemaphoreCount = 0;
  batch.info.pWaitSemaphores = nullptr;
  failPresent = 2;
  CHECK(batch.run() == VK_ERROR_DEVICE_LOST);
  CHECK(presented.size() == 1);
  CHECK(batch.results[0] == VK_SUCCESS);
  CHECK(batch.results[1] == VK_ERROR_DEVICE_LOST);
}

TEST_CASE("A release error does not prevent the healthy swapchain from presenting", "[present_timing_layer][present_batch]") {
  PresentBatch batch;
  batch.states[0]->timingQueue.setSize(0);
  refused.firstReleaseResult = VK_ERROR_SURFACE_LOST_KHR;
  CHECK(batch.run() == VK_ERROR_SURFACE_LOST_KHR);
  CHECK(batch.results[0] == VK_ERROR_SURFACE_LOST_KHR);
  CHECK(batch.results[1] == VK_SUCCESS);
  CHECK(presented.size() == 1);
  if (!presented.empty())
    CHECK(presented[0].swapchain == batch.swapchains[1]);
}

TEST_CASE("Healthy hooked batches keep the single driver present", "[present_timing_layer][present_batch]") {
  PresentBatch batch;
  CHECK(batch.run() == VK_SUCCESS);
  CHECK(presentCalls == 1);
  CHECK(refused.submits == 0);
  CHECK(refused.idles == 0);
  CHECK(presented.size() == 2);
}

TEST_CASE("A failed extra fence submit cannot return OOM after the first succeeds", "[present_timing_layer][present_batch]") {
  PresentBatch batch;
  refused.failSubmit = 2;
  CHECK(GamescopeWSILayer::VkDeviceOverrides::PresentWithoutImage(batch.queue, VK_NULL_HANDLE, &batch.info,
    VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT, true) == VK_ERROR_DEVICE_LOST);
  CHECK(refused.submits == 2);
  CHECK(refused.waits == std::vector<VkSemaphore>{batch.semaphore});
  CHECK(refused.fences == std::vector<VkFence>{batch.fences[0]});
  CHECK(refused.released.empty());
}

TEST_CASE("Present slicing preserves all per-swapchain arrays and scalar metadata", "[present_timing_layer][present_batch]") {
  PresentBatch batch(true);
  std::array<VkPresentRegionKHR, 2> regions{{{0, nullptr}, {1, nullptr}}};
  VkRectLayerKHR rect{{1, 2}, {3, 4}, 0};
  regions[1].pRectangles = &rect;
  std::array<uint32_t, 2> masks{1, 2};
  std::array<VkPresentTimeGOOGLE, 2> times{{{11, 100}, {22, 200}}};
  std::array<VkPresentModeKHR, 2> modes{VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};
  VkDisplayPresentInfoKHR display{.sType = VK_STRUCTURE_TYPE_DISPLAY_PRESENT_INFO_KHR, .pNext = batch.info.pNext,
    .srcRect = {{1, 2}, {3, 4}}, .dstRect = {{5, 6}, {7, 8}}, .persistent = VK_TRUE};
  VkFrameBoundaryEXT boundary{.sType = VK_STRUCTURE_TYPE_FRAME_BOUNDARY_EXT, .pNext = &display,
    .flags = VK_FRAME_BOUNDARY_FRAME_END_BIT_EXT, .frameID = 99};
  VkPresentRegionsKHR regionInfo{VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR, &boundary, 2, regions.data()};
  VkDeviceGroupPresentInfoKHR group{VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR, &regionInfo, 2, masks.data(), VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR};
  VkPresentTimesInfoGOOGLE google{VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE, &group, 2, times.data()};
  VkPresentIdKHR ids{VK_STRUCTURE_TYPE_PRESENT_ID_KHR, &google, 2, batch.ids.data()};
  VkSwapchainPresentModeInfoEXT modeInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT, &ids, 2, modes.data()};
  batch.info.pNext = &modeInfo;
  for (uint32_t i = 0; i < 2; i++) {
    VkResult result;
    GamescopeWSILayer::PresentInfoSlice slice(batch.info, i, &result);
    CHECK(slice.info.swapchainCount == 1);
    CHECK(slice.info.pSwapchains[0] == batch.swapchains[i]);
    CHECK(slice.info.pImageIndices[0] == batch.images[i]);
    CHECK(slice.info.pResults == &result);
    CHECK(vkroots::FindInChain<VkPresentRegionsKHR>(&slice.info)->pRegions == &regions[i]);
    CHECK(vkroots::FindInChain<VkDeviceGroupPresentInfoKHR>(&slice.info)->pDeviceMasks == &masks[i]);
    CHECK(vkroots::FindInChain<VkPresentTimesInfoGOOGLE>(&slice.info)->pTimes == &times[i]);
    CHECK(vkroots::FindInChain<VkPresentIdKHR>(&slice.info)->pPresentIds == &batch.ids[i]);
    CHECK(vkroots::FindInChain<VkPresentId2KHR>(&slice.info)->pPresentIds == &batch.ids[i]);
    CHECK(vkroots::FindInChain<VkSwapchainPresentModeInfoEXT>(&slice.info)->pPresentModes == &modes[i]);
    CHECK(vkroots::FindInChain<VkSwapchainPresentFenceInfoEXT>(&slice.info)->pFences == &batch.fences[i]);
    CHECK(vkroots::FindInChain<VkPresentTimingsInfoEXT>(&slice.info)->pTimingInfos == &batch.timings[i]);
    CHECK(vkroots::FindInChain<VkDisplayPresentInfoKHR>(&slice.info)->dstRect.extent.width == 7);
    CHECK(vkroots::FindInChain<VkFrameBoundaryEXT>(&slice.info)->frameID == 99);
    CHECK(vkroots::FindInChain<VkFrameBoundaryEXT>(&slice.info)->flags == (i ? VK_FRAME_BOUNDARY_FRAME_END_BIT_EXT : 0));
  }
  CHECK(batch.info.pNext == &modeInfo);
  CHECK(modeInfo.pNext == &ids);
  CHECK(ids.pNext == &google);
  CHECK(google.pNext == &group);
  CHECK(group.pNext == &regionInfo);
  CHECK(boundary.flags == VK_FRAME_BOUNDARY_FRAME_END_BIT_EXT);
  CHECK(batch.fenceInfo.swapchainCount == 2);

  group.swapchainCount = 0;
  group.pDeviceMasks = nullptr;
  batch.timingInfo.pTimingInfos = nullptr;
  GamescopeWSILayer::PresentInfoSlice slice(batch.info, 1, nullptr);
  CHECK(vkroots::FindInChain<VkDeviceGroupPresentInfoKHR>(&slice.info)->swapchainCount == 0);
  CHECK(vkroots::FindInChain<VkDeviceGroupPresentInfoKHR>(&slice.info)->pDeviceMasks == nullptr);
  CHECK(vkroots::FindInChain<VkPresentTimingsInfoEXT>(&slice.info)->pTimingInfos == nullptr);
}

TEST_CASE("Mixed batches without timing entries need no split", "[present_timing_layer][present_batch]") {
  PresentBatch batch(true);
  batch.timingInfo.pTimingInfos = nullptr;
  CHECK(batch.run() == VK_SUCCESS);
  CHECK(presentCalls == 1);
  CHECK(refused.submits == 0);
  CHECK(refused.idles == 0);
  CHECK(presented.size() == 2);
}

TEST_CASE("A retired swapchain does not reject the other hooked swapchain", "[present_timing_layer][present_batch]") {
  PresentBatch batch;
  *batch.states[0]->retired = true;
  CHECK(batch.run() == VK_ERROR_OUT_OF_DATE_KHR);
  CHECK(batch.results[0] == VK_ERROR_OUT_OF_DATE_KHR);
  CHECK(batch.results[1] == VK_SUCCESS);
  CHECK(presented.size() == 1);
  if (!presented.empty())
    CHECK(presented[0].swapchain == batch.swapchains[1]);
  CHECK(refused.waits == std::vector<VkSemaphore>{batch.semaphore});
  CHECK(refused.fences == std::vector<VkFence>{batch.fences[0]});
}

TEST_CASE("An unknown chain entry keeps the whole-batch refusal", "[present_timing_layer][present_batch]") {
  PresentBatch batch;
  batch.states[0]->timingQueue.setSize(0);
  VkBaseInStructure unknown{static_cast<VkStructureType>(0x7fffffff), static_cast<const VkBaseInStructure *>(batch.info.pNext)};
  batch.info.pNext = &unknown;
  CHECK(batch.run() == VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT);
  CHECK(refused.released == std::vector<VkSwapchainKHR>{batch.swapchains[0], batch.swapchains[1]});
  CHECK(presented.empty());
}

TEST_CASE("A rejected last swapchain still ends the frame", "[present_timing_layer][present_batch]") {
  PresentBatch batch;
  batch.states[1]->timingQueue.setSize(0);
  VkFrameBoundaryEXT boundary{.sType = VK_STRUCTURE_TYPE_FRAME_BOUNDARY_EXT, .pNext = batch.info.pNext,
    .flags = VK_FRAME_BOUNDARY_FRAME_END_BIT_EXT, .frameID = 23};
  batch.info.pNext = &boundary;
  SECTION("with waits and fences") {}
  SECTION("without waits or fences") {
    batch.info.waitSemaphoreCount = 0;
    boundary.pNext = &batch.idInfo;
  }
  CHECK(batch.run() == VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT);
  CHECK(refused.boundaries == std::vector<VkFrameBoundaryFlagsEXT>{0, VK_FRAME_BOUNDARY_FRAME_END_BIT_EXT});
}

TEST_CASE("A failing middle present marks the remaining swapchains lost", "[present_timing_layer][present_batch]") {
  PresentBatch batch(true);
  std::array<VkSwapchainKHR, 3> swapchains{batch.swapchains[0], batch.swapchains[1], reinterpret_cast<VkSwapchainKHR>(uintptr_t(503))};
  std::array<uint32_t, 3> images{2, 1, 0};
  std::array<uint64_t, 3> ids{71, 92, 93};
  std::array<VkPresentTimingInfoEXT, 3> timings{{batch.timings[0], batch.timings[1], batch.timings[1]}};
  std::array<VkResult, 3> results{};
  VkPresentTimingsInfoEXT timingInfo{VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT, nullptr, 3, timings.data()};
  VkPresentId2KHR idInfo{VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR, &timingInfo, 3, ids.data()};
  VkFrameBoundaryEXT boundary{.sType = VK_STRUCTURE_TYPE_FRAME_BOUNDARY_EXT, .pNext = &idInfo,
    .flags = VK_FRAME_BOUNDARY_FRAME_END_BIT_EXT, .frameID = 7};
  VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, &boundary, 1, &batch.semaphore, 3, swapchains.data(), images.data(), results.data()};
  failPresent = 2;
  CHECK(GamescopeWSILayer::VkDeviceOverrides::QueuePresentKHR(batch.queue, VK_NULL_HANDLE, &info) == VK_ERROR_DEVICE_LOST);
  CHECK(presented.size() == 1);
  CHECK(results == std::array<VkResult, 3>{VK_SUCCESS, VK_ERROR_DEVICE_LOST, VK_ERROR_DEVICE_LOST});
  CHECK(refused.boundaries == std::vector<VkFrameBoundaryFlagsEXT>{0});
}

#include "wayland_test_server.hpp"

TEST_CASE("Surface globals do not dispatch another surface's limiter", "[present_timing_layer][surface_globals]") {
  WaylandTestServer peer;
  auto first = GamescopeWSILayer::GamescopeWaylandObjects::get(peer.display);
  std::optional second{GamescopeWSILayer::GamescopeWaylandObjects::get(peer.display)};
  REQUIRE(first.valid());
  REQUIRE(second->valid());
  auto *timingQueue = wl_display_create_queue(peer.display);
  peer.SendState(1);
  // Read all events without dispatching either surface's globals.
  auto *readQueue = wl_display_create_queue(peer.display);
  REQUIRE(wl_display_roundtrip_queue(peer.display, readQueue) >= 0);
  REQUIRE(GamescopeWSILayer::waylandPumpEvents(peer.display, timingQueue, first.queue.get()) >= 0);
  CHECK(first.limiterState->state == 1);
  CHECK(second->limiterState->state == 0);
  second.reset();
  peer.SendState(0);
  REQUIRE(wl_display_roundtrip_queue(peer.display, readQueue) >= 0);
  REQUIRE(GamescopeWSILayer::waylandPumpEvents(peer.display, timingQueue, first.queue.get()) >= 0);
  CHECK(first.limiterState->state == 0);
  wl_event_queue_destroy(timingQueue);
  wl_event_queue_destroy(readQueue);
}

namespace {
VkResult surfaceResult = VK_ERROR_OUT_OF_HOST_MEMORY;
uint64_t surfaceSerial = 10000;
std::vector<VkSurfaceKHR> destroyedSurfaces;
VkResult VKAPI_CALL createTestSurface(VkInstance, const VkWaylandSurfaceCreateInfoKHR *,
                                    const VkAllocationCallbacks *, VkSurfaceKHR *surface) {
  if (surfaceResult == VK_SUCCESS)
    *surface = reinterpret_cast<VkSurfaceKHR>(++surfaceSerial);
  return surfaceResult;
}
void VKAPI_CALL destroyTestSurface(VkInstance, VkSurfaceKHR surface, const VkAllocationCallbacks *) {
  if (surface)
    destroyedSurfaces.push_back(surface);
}
PFN_vkVoidFunction getSurfaceProc(VkInstance instance, const char *name) {
  if (std::strcmp(name, "vkCreateWaylandSurfaceKHR") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(createTestSurface);
  if (std::strcmp(name, "vkDestroySurfaceKHR") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(destroyTestSurface);
  return getInstanceProc(instance, name);
}
}

TEST_CASE("Failed native surface creation releases bound globals", "[present_timing_layer][surface_globals]") {
  WaylandTestServer peer;
  auto instance = reinterpret_cast<VkInstance>(uintptr_t(1001));
  REQUIRE(GamescopeWSILayer::gamescopeInstances.create(instance, GamescopeWSILayer::GamescopeInstanceData{
    .display = peer.display}) != nullptr);
  vkroots::VkInstanceDispatch dispatch(getSurfaceProc, instance, nullptr);
  auto *appSurface = peer.CreateSurface();
  VkWaylandSurfaceCreateInfoKHR info{.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
    .display = peer.display, .surface = appSurface};
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  surfaceResult = VK_ERROR_OUT_OF_HOST_MEMORY;
  destroyedSurfaces.clear();
  CHECK(GamescopeWSILayer::VkInstanceOverrides::CreateWaylandSurfaceKHR(dispatch, instance, &info, nullptr, &surface) == VK_ERROR_OUT_OF_HOST_MEMORY);
  peer.Sync();
  CHECK(peer.FactoryCount() == 0);
  CHECK(peer.LimiterCount() == 0);
  CHECK(peer.SurfaceCount() == 1);
  CHECK(destroyedSurfaces.empty());

  surfaceResult = VK_SUCCESS;
  REQUIRE(GamescopeWSILayer::VkInstanceOverrides::CreateWaylandSurfaceKHR(dispatch, instance, &info, nullptr, &surface) == VK_SUCCESS);
  REQUIRE(GamescopeWSILayer::gamescopeSurfaces.find(surface));
  GamescopeWSILayer::VkInstanceOverrides::DestroySurfaceKHR(dispatch, instance, surface, nullptr);
  peer.Sync();
  CHECK(peer.FactoryCount() == 0);
  CHECK(destroyedSurfaces == std::vector<VkSurfaceKHR>{surface});
  // The layer must leave the application's native surface alive.
  CHECK(peer.SurfaceCount() == 1);
  wl_surface_destroy(appSurface);
  peer.Sync();
  CHECK(peer.SurfaceCount() == 0);
  GamescopeWSILayer::gamescopeInstances.erase(instance);
}

TEST_CASE("Missing compositor globals unwind their other bindings", "[present_timing_layer][surface_globals]") {
  WaylandTestServer peer(false);
  {
    auto objects = GamescopeWSILayer::GamescopeWaylandObjects::get(peer.display);
    CHECK_FALSE(objects.valid());
  }
  peer.Sync();
  CHECK(peer.FactoryCount() == 0);
  CHECK(peer.LimiterCount() == 0);
}

namespace {
void VKAPI_CALL destroyTestSwapchain(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks *) {}
PFN_vkVoidFunction getSurfaceDeviceProc(VkDevice device, const char *name) {
  if (std::strcmp(name, "vkDestroySwapchainKHR") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(destroyTestSwapchain);
  return getDeviceProc(device, name);
}
}

TEST_CASE("Retiring an old swapchain preserves its surface globals and successor", "[present_timing_layer][surface_globals]") {
  using namespace GamescopeWSILayer;
  WaylandTestServer peer;
  auto owner = GamescopeWaylandObjects::get(peer.display);
  REQUIRE(owner.valid());
  auto *appSurface = peer.CreateSurface();
  VkDeviceCreateInfo createInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  vkroots::VkDeviceDispatch dispatch(getSurfaceDeviceProc, VK_NULL_HANDLE, VK_NULL_HANDLE, nullptr, &createInfo);
  const std::array<VkSwapchainKHR, 2> handles{
    reinterpret_cast<VkSwapchainKHR>(uintptr_t(2001)), reinterpret_cast<VkSwapchainKHR>(uintptr_t(2002))};
  std::array<GamescopeSwapchainData *, 2> states;
  for (size_t i = 0; i < states.size(); ++i) {
    auto *queue = wl_display_create_queue(peer.display);
    auto *wrapper = static_cast<gamescope_swapchain_factory_v2 *>(wl_proxy_create_wrapper(owner.gamescopeSwapchainFactory.get()));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapper), queue);
    auto *object = gamescope_swapchain_factory_v2_create_swapchain(wrapper, appSurface);
    wl_proxy_wrapper_destroy(wrapper);
    states[i] = gamescopeSwapchains.create(handles[i], GamescopeSwapchainData{
      .object = object, .display = peer.display, .globalsQueue = owner.queue.get(), .eventQueue = queue});
    REQUIRE(states[i]);
    gamescope_swapchain_add_listener(object, &s_swapchainListener, states[i]);
  }
  peer.Sync();
  peer.SendState(1);
  peer.SendRefreshCycle(8'333'333);
  peer.Sync();
  REQUIRE(waylandPumpEvents(peer.display, states[1]->eventQueue, states[1]->globalsQueue) >= 0);
  CHECK(owner.limiterState->state == 1);
  CHECK(states[1]->refreshCycle == 8'333'333);
  CHECK(states[0]->refreshCycle == 16'666'666);

  VkDeviceOverrides::DestroySwapchainKHR(dispatch, VK_NULL_HANDLE, handles[0], nullptr);
  peer.Sync();
  peer.SendState(0);
  peer.SendRefreshCycle(16'666'667);
  peer.Sync();
  REQUIRE(waylandPumpEvents(peer.display, states[1]->eventQueue, states[1]->globalsQueue) >= 0);
  CHECK(owner.limiterState->state == 0);
  CHECK(states[1]->refreshCycle == 16'666'667);
  VkDeviceOverrides::DestroySwapchainKHR(dispatch, VK_NULL_HANDLE, handles[1], nullptr);
  wl_surface_destroy(appSurface);
}
