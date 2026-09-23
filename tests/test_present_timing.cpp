#include <catch2/catch_test_macros.hpp>

#include "../layer/present_timing.hpp"

#include <array>
#include <cstdint>
#include <limits>

using GamescopeWSILayer::PresentTimingQueue;

namespace {

constexpr VkPresentStageFlagsEXT AllReportedStages =
  VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT |
  VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT |
  VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;

struct TimingOutput {
  std::array<VkPresentStageTimeEXT, 4> stages = {};
  VkPastPresentationTimingEXT timing = {};

  TimingOutput() {
    timing.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT;
    timing.presentStageCount = stages.size();
    timing.pPresentStages = stages.data();
  }
};

VkPastPresentationTimingInfoEXT QueryInfo(VkPastPresentationTimingFlagsEXT flags = 0) {
  VkPastPresentationTimingInfoEXT info = {};
  info.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT;
  info.flags = flags;
  return info;
}

VkPastPresentationTimingPropertiesEXT QueryProperties(VkPastPresentationTimingEXT *outputs, uint32_t count) {
  VkPastPresentationTimingPropertiesEXT properties = {};
  properties.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT;
  properties.presentationTimingCount = count;
  properties.pPresentationTimings = outputs;
  return properties;
}

}

TEST_CASE("Present timing queue starts with no result slots", "[present_timing]") {
  PresentTimingQueue queue;

  REQUIRE_FALSE(queue.hasSlot());
  REQUIRE_FALSE(queue.reserve(7, 11, VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT));
  REQUIRE(queue.setSize(1) == VK_SUCCESS);
  REQUIRE(queue.hasSlot());
  REQUIRE(queue.reserve(7, 11, VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT) == 1);
  REQUIRE_FALSE(queue.hasSlot());
  REQUIRE_FALSE(queue.reserve(8, 12, VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT));
}

TEST_CASE("Present timing queue cannot shrink below its outstanding results", "[present_timing]") {
  PresentTimingQueue queue;
  REQUIRE(queue.setSize(2) == VK_SUCCESS);
  REQUIRE(queue.reserve(1, 0, AllReportedStages) == 1);
  REQUIRE(queue.reserve(2, 0, AllReportedStages) == 2);

  REQUIRE(queue.setSize(1) == VK_NOT_READY);
  REQUIRE_FALSE(queue.reserve(3, 0, AllReportedStages));

  queue.complete(1, 10, 20, 30);
  TimingOutput output;
  auto info = QueryInfo();
  auto properties = QueryProperties(&output.timing, 1);
  REQUIRE(queue.getPast(&info, &properties) == VK_SUCCESS);
  REQUIRE(queue.setSize(1) == VK_SUCCESS);
  REQUIRE_FALSE(queue.reserve(3, 0, AllReportedStages));
}

TEST_CASE("Present timing serials do not depend on application present ids", "[present_timing]") {
  PresentTimingQueue queue;
  REQUIRE(queue.setSize(3) == VK_SUCCESS);

  REQUIRE(queue.nextSerial() == 1);
  REQUIRE(queue.reserve(0, 100, AllReportedStages) == 2);
  REQUIRE(queue.reserve(0, 200, AllReportedStages) == 3);
  REQUIRE(queue.nextSerial() == 4);
}

TEST_CASE("Present timing results wait for earlier submissions by default", "[present_timing]") {
  PresentTimingQueue queue;
  REQUIRE(queue.setSize(3) == VK_SUCCESS);
  const uint64_t first = *queue.reserve(10, 100, AllReportedStages);
  const uint64_t second = *queue.reserve(20, 200, AllReportedStages);
  const uint64_t third = *queue.reserve(30, 300, AllReportedStages);

  queue.complete(second, 21, 22, 23);
  auto info = QueryInfo();
  auto countOnly = QueryProperties(nullptr, 0);
  REQUIRE(queue.getPast(&info, &countOnly) == VK_SUCCESS);
  REQUIRE(countOnly.presentationTimingCount == 0);

  auto outOfOrderInfo = QueryInfo(VK_PAST_PRESENTATION_TIMING_ALLOW_OUT_OF_ORDER_RESULTS_BIT_EXT);
  countOnly = QueryProperties(nullptr, 0);
  REQUIRE(queue.getPast(&outOfOrderInfo, &countOnly) == VK_SUCCESS);
  REQUIRE(countOnly.presentationTimingCount == 1);

  TimingOutput output;
  auto properties = QueryProperties(&output.timing, 1);
  REQUIRE(queue.getPast(&outOfOrderInfo, &properties) == VK_SUCCESS);
  REQUIRE(output.timing.presentId == 20);
  REQUIRE(output.timing.targetTime == 200);

  queue.complete(third, 31, 32, 33);
  countOnly = QueryProperties(nullptr, 0);
  REQUIRE(queue.getPast(&info, &countOnly) == VK_SUCCESS);
  REQUIRE(countOnly.presentationTimingCount == 0);

  queue.complete(first, 11, 12, 13);
  std::array<TimingOutput, 2> outputs;
  std::array<VkPastPresentationTimingEXT, 2> timings = { outputs[0].timing, outputs[1].timing };
  properties = QueryProperties(timings.data(), timings.size());
  REQUIRE(queue.getPast(&info, &properties) == VK_SUCCESS);
  REQUIRE(properties.presentationTimingCount == 2);
  REQUIRE(timings[0].presentId == 10);
  REQUIRE(timings[1].presentId == 30);
}

TEST_CASE("Present timing retrieval reports short record arrays", "[present_timing]") {
  PresentTimingQueue queue;
  REQUIRE(queue.setSize(2) == VK_SUCCESS);
  const uint64_t first = *queue.reserve(1, 0, VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT);
  const uint64_t second = *queue.reserve(2, 0, VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT);
  queue.complete(first, 0, 0, 10);
  queue.complete(second, 0, 0, 20);

  TimingOutput output;
  auto info = QueryInfo();
  auto properties = QueryProperties(&output.timing, 1);
  REQUIRE(queue.getPast(&info, &properties) == VK_INCOMPLETE);
  REQUIRE(properties.presentationTimingCount == 1);
  REQUIRE(output.timing.presentId == 1);
  REQUIRE(output.timing.presentStageCount == 1);
  REQUIRE(output.stages[0].stage == VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT);
  REQUIRE(output.stages[0].time == 10);
  REQUIRE(queue.reserve(3, 0, VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT).has_value());
}

TEST_CASE("Present timing does not expose an atomically pending report as partial", "[present_timing]") {
  PresentTimingQueue queue;
  REQUIRE(queue.setSize(1) == VK_SUCCESS);
  const uint64_t serial = *queue.reserve(44, 55, AllReportedStages);

  TimingOutput output;
  auto info = QueryInfo(VK_PAST_PRESENTATION_TIMING_ALLOW_PARTIAL_RESULTS_BIT_EXT);
  auto properties = QueryProperties(&output.timing, 1);
  REQUIRE(queue.getPast(&info, &properties) == VK_SUCCESS);
  REQUIRE(properties.presentationTimingCount == 0);
  REQUIRE_FALSE(queue.reserve(45, 0, AllReportedStages));

  queue.complete(serial, 10, 20, 30);
  output.timing.presentStageCount = output.stages.size();
  properties = QueryProperties(&output.timing, 1);
  REQUIRE(queue.getPast(&info, &properties) == VK_SUCCESS);
  REQUIRE(output.timing.reportComplete == VK_TRUE);
  REQUIRE(output.timing.presentStageCount == 3);
  REQUIRE(output.stages[0].stage == VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT);
  REQUIRE(output.stages[0].time == 10);
  REQUIRE(output.stages[1].stage == VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT);
  REQUIRE(output.stages[1].time == 20);
  REQUIRE(output.stages[2].stage == VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT);
  REQUIRE(output.stages[2].time == 30);
  REQUIRE(queue.reserve(45, 0, AllReportedStages).has_value());
}

TEST_CASE("Present timing stage output stays within the caller capacity", "[present_timing]") {
  PresentTimingQueue queue;
  REQUIRE(queue.setSize(1) == VK_SUCCESS);
  const uint64_t serial = *queue.reserve(6, 0, AllReportedStages);
  queue.complete(serial, 10, 20, 30);

  TimingOutput output;
  output.timing.presentStageCount = 2;
  auto info = QueryInfo();
  auto properties = QueryProperties(&output.timing, 1);
  REQUIRE(queue.getPast(&info, &properties) == VK_SUCCESS);
  REQUIRE(properties.presentationTimingCount == 1);
  REQUIRE(output.timing.presentStageCount == 2);
  REQUIRE(output.stages[2].stage == 0);
  REQUIRE(queue.reserve(7, 0, AllReportedStages).has_value());
}

TEST_CASE("Present timing reports dropped stages and never steps a stage backwards", "[present_timing]") {
  PresentTimingQueue queue;
  REQUIRE(queue.setSize(3) == VK_SUCCESS);
  const uint64_t first = *queue.reserve(1, 10, AllReportedStages);
  const uint64_t dropped = *queue.reserve(2, 20, AllReportedStages);
  const uint64_t third = *queue.reserve(3, 30, AllReportedStages);
  queue.complete(first, 100, 200, 300);
  queue.complete(dropped, 0, 0, 0);
  queue.complete(third, 99, 200, 250);

  std::array<TimingOutput, 3> outputs;
  std::array<VkPastPresentationTimingEXT, 3> timings = { outputs[0].timing, outputs[1].timing, outputs[2].timing };
  auto info = QueryInfo();
  auto properties = QueryProperties(timings.data(), timings.size());
  REQUIRE(queue.getPast(&info, &properties) == VK_SUCCESS);
  REQUIRE(properties.presentationTimingCount == 3);
  REQUIRE(timings[0].timeDomain == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT);
  REQUIRE(timings[0].timeDomainId == 1);
  REQUIRE(timings[0].reportComplete == VK_TRUE);
  REQUIRE(outputs[1].stages[0].time == 0);
  REQUIRE(outputs[1].stages[1].time == 0);
  REQUIRE(outputs[1].stages[2].time == 0);
  REQUIRE(outputs[2].stages[0].time == 100);
  REQUIRE(outputs[2].stages[1].time == 200);
  REQUIRE(outputs[2].stages[2].time == 300);
}

TEST_CASE("Present timing lists an unsupported queried stage with zero", "[present_timing]") {
  PresentTimingQueue queue;
  REQUIRE(queue.setSize(1) == VK_SUCCESS);
  const uint64_t serial = *queue.reserve(1, 0, VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT);
  queue.complete(serial, 10, 20, 30);

  TimingOutput output;
  auto info = QueryInfo();
  auto properties = QueryProperties(&output.timing, 1);
  REQUIRE(queue.getPast(&info, &properties) == VK_SUCCESS);
  REQUIRE(output.timing.reportComplete == VK_TRUE);
  REQUIRE(output.timing.presentStageCount == 1);
  REQUIRE(output.stages[0].stage == VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT);
  REQUIRE(output.stages[0].time == 0);
}

TEST_CASE("Present timing ignores reports with unknown serials", "[present_timing]") {
  PresentTimingQueue queue;
  REQUIRE(queue.setSize(1) == VK_SUCCESS);
  const uint64_t serial = *queue.reserve(9, 0, AllReportedStages);
  queue.complete(serial + 100, 10, 20, 30);

  auto info = QueryInfo(VK_PAST_PRESENTATION_TIMING_ALLOW_OUT_OF_ORDER_RESULTS_BIT_EXT);
  auto properties = QueryProperties(nullptr, 0);
  REQUIRE(queue.getPast(&info, &properties) == VK_SUCCESS);
  REQUIRE(properties.presentationTimingCount == 0);
}

TEST_CASE("Present timing properties change their counter only with their tuple", "[present_timing]") {
  PresentTimingQueue queue;
  VkSwapchainTimingPropertiesEXT properties = {};
  properties.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT;
  uint64_t counter = std::numeric_limits<uint64_t>::max();

  REQUIRE(queue.getTimingProperties(&properties, &counter) == VK_NOT_READY);
  REQUIRE(counter == 0);

  queue.setTimingProperties(33'333'334, 16'666'667);
  REQUIRE(queue.getTimingProperties(&properties, &counter) == VK_SUCCESS);
  REQUIRE(properties.refreshDuration == 33'333'334);
  REQUIRE(properties.refreshInterval == 16'666'667);
  REQUIRE(counter == 1);

  queue.setTimingProperties(33'333'334, 16'666'667);
  queue.getTimingProperties(&properties, &counter);
  REQUIRE(counter == 1);

  queue.setTimingProperties(6'944'444, std::numeric_limits<uint64_t>::max());
  queue.getTimingProperties(&properties, &counter);
  REQUIRE(counter == 2);
}

TEST_CASE("Present timing returns its constant local time domain", "[present_timing]") {
  PresentTimingQueue queue;
  VkSwapchainTimeDomainPropertiesEXT properties = {};
  properties.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT;
  uint64_t counter = 0;

  REQUIRE(queue.getTimeDomainProperties(&properties, &counter) == VK_SUCCESS);
  REQUIRE(properties.timeDomainCount == 1);
  REQUIRE(counter == 1);

  VkTimeDomainKHR domain = VK_TIME_DOMAIN_DEVICE_KHR;
  uint64_t id = 0;
  properties.timeDomainCount = 0;
  properties.pTimeDomains = &domain;
  properties.pTimeDomainIds = &id;
  REQUIRE(queue.getTimeDomainProperties(&properties, &counter) == VK_INCOMPLETE);
  REQUIRE(properties.timeDomainCount == 0);

  properties.timeDomainCount = 1;
  REQUIRE(queue.getTimeDomainProperties(&properties, &counter) == VK_SUCCESS);
  REQUIRE(properties.timeDomainCount == 1);
  REQUIRE(domain == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT);
  REQUIRE(id == 1);
}

TEST_CASE("Present timing feedback carries current property counters", "[present_timing]") {
  PresentTimingQueue queue;
  REQUIRE(queue.setSize(1) == VK_SUCCESS);
  queue.setTimingProperties(10, 10);
  const uint64_t serial = *queue.reserve(1, 0, VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT);
  queue.complete(serial, 0, 0, 100);

  auto info = QueryInfo();
  auto properties = QueryProperties(nullptr, 0);
  REQUIRE(queue.getPast(&info, &properties) == VK_SUCCESS);
  REQUIRE(properties.timingPropertiesCounter == 1);
  REQUIRE(properties.timeDomainsCounter == 1);
}
