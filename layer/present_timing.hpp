#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace GamescopeWSILayer {

  // The layer serializes access to this object with the swapchain results mutex.
  class PresentTimingQueue {
  public:
    VkResult setSize(uint32_t size) {
      if (size < m_entries.size())
        return VK_NOT_READY;

      m_entries.reserve(size);
      m_capacity = size;
      return VK_SUCCESS;
    }

    uint64_t nextSerial() {
      m_nextSerial++;
      if (m_nextSerial == 0)
        m_nextSerial++;
      return m_nextSerial;
    }

    bool hasSlot() const {
      return m_entries.size() < m_capacity;
    }

    std::optional<uint64_t> reserve(uint64_t presentId, uint64_t targetTime, VkPresentStageFlagsEXT stageQueries) {
      if (!hasSlot())
        return std::nullopt;

      const uint64_t serial = nextSerial();
      m_entries.push_back({
        .serial = serial,
        .presentId = presentId,
        .targetTime = targetTime,
        .stageQueries = stageQueries,
      });
      return serial;
    }

    void complete(uint64_t serial, uint64_t queueEnd, uint64_t requestDequeued, uint64_t pixelOut) {
      auto entry = std::ranges::find(m_entries, serial, &Entry::serial);
      if (entry == m_entries.end() || entry->complete)
        return;

      entry->times = { queueEnd, requestDequeued, pixelOut, 0 };
      entry->complete = true;
    }

    void setTimingProperties(uint64_t refreshDuration, uint64_t refreshInterval) {
      if (m_refreshDuration == refreshDuration && m_refreshInterval == refreshInterval)
        return;

      m_refreshDuration = refreshDuration;
      m_refreshInterval = refreshInterval;
      m_timingPropertiesCounter++;
    }

    VkResult getTimingProperties(VkSwapchainTimingPropertiesEXT *properties, uint64_t *counter) const {
      if (counter)
        *counter = m_timingPropertiesCounter;
      if (!m_timingPropertiesCounter)
        return VK_NOT_READY;
      properties->refreshDuration = m_refreshDuration;
      properties->refreshInterval = m_refreshInterval;
      return VK_SUCCESS;
    }

    VkResult getTimeDomainProperties(VkSwapchainTimeDomainPropertiesEXT *properties, uint64_t *counter) const {
      if (counter)
        *counter = TimeDomainsCounter;

      if (!properties->pTimeDomains && !properties->pTimeDomainIds) {
        properties->timeDomainCount = 1;
        return VK_SUCCESS;
      }

      if (properties->timeDomainCount == 0)
        return VK_INCOMPLETE;

      if (properties->pTimeDomains)
        properties->pTimeDomains[0] = VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT;
      if (properties->pTimeDomainIds)
        properties->pTimeDomainIds[0] = TimeDomainId;
      properties->timeDomainCount = 1;
      return VK_SUCCESS;
    }

    VkResult getPast(const VkPastPresentationTimingInfoEXT *info, VkPastPresentationTimingPropertiesEXT *properties) {
      properties->timingPropertiesCounter = m_timingPropertiesCounter;
      properties->timeDomainsCounter = TimeDomainsCounter;

      const bool outOfOrder = info->flags & VK_PAST_PRESENTATION_TIMING_ALLOW_OUT_OF_ORDER_RESULTS_BIT_EXT;
      const uint32_t available = availableCount(outOfOrder);

      if (!properties->pPresentationTimings) {
        properties->presentationTimingCount = available;
        return VK_SUCCESS;
      }

      const uint32_t outputCapacity = properties->presentationTimingCount;
      uint32_t outputCount = 0;
      auto returnedEnd = m_entries.begin();
      for (auto entryIt = m_entries.begin(); entryIt != m_entries.end(); entryIt++) {
        const Entry &entry = *entryIt;
        if (!entry.complete) {
          if (!outOfOrder)
            break;
          continue;
        }

        if (outputCount >= outputCapacity)
          break;

        VkPastPresentationTimingEXT &output = properties->pPresentationTimings[outputCount];
        const uint32_t stageCapacity = output.pPresentStages ? output.presentStageCount : 0;

        output.presentId = entry.presentId;
        output.targetTime = entry.targetTime;
        output.timeDomain = VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT;
        output.timeDomainId = TimeDomainId;
        // The compositor reports every stage at once, so there is no partial result.
        output.reportComplete = VK_TRUE;
        output.presentStageCount = writeStages(entry, output.pPresentStages, stageCapacity);
        outputCount++;
        returnedEnd = entryIt + 1;
      }

      properties->presentationTimingCount = outputCount;
      auto eraseBegin = std::remove_if(m_entries.begin(), returnedEnd, [](const Entry &entry) { return entry.complete; });
      m_entries.erase(eraseBegin, returnedEnd);

      return outputCount < available ? VK_INCOMPLETE : VK_SUCCESS;
    }

  private:
    static constexpr uint64_t TimeDomainId = 1;
    static constexpr uint64_t TimeDomainsCounter = 1;
    // Pixel visible is never advertised but a report must still list every
    // queried stage, with zero for the ones it cannot time.
    static constexpr std::array<VkPresentStageFlagBitsEXT, 4> Stages = {
      VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT,
      VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT,
      VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT,
      VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT,
    };

    struct Entry {
      uint64_t serial = 0;
      uint64_t presentId = 0;
      uint64_t targetTime = 0;
      VkPresentStageFlagsEXT stageQueries = 0;
      std::array<uint64_t, Stages.size()> times = {};
      bool complete = false;
    };

    uint32_t availableCount(bool outOfOrder) const {
      uint32_t count = 0;
      for (const Entry &entry : m_entries) {
        if (entry.complete)
          count++;
        else if (!outOfOrder)
          break;
      }
      return count;
    }

    // CTS requires nonzero times to not decrease between reports of a stage.
    uint32_t writeStages(const Entry &entry, VkPresentStageTimeEXT *outputs, uint32_t capacity) {
      uint32_t count = 0;
      for (size_t i = 0; i < Stages.size() && count < capacity; i++) {
        if (!(entry.stageQueries & Stages[i]))
          continue;
        uint64_t time = entry.times[i];
        if (time)
          time = m_lastStageTimes[i] = std::max(time, m_lastStageTimes[i]);
        outputs[count++] = { .stage = Stages[i], .time = time };
      }
      return count;
    }

    uint32_t m_capacity = 0;
    uint64_t m_nextSerial = 0;
    uint64_t m_refreshDuration = 0;
    uint64_t m_refreshInterval = 0;
    uint64_t m_timingPropertiesCounter = 0;
    std::array<uint64_t, Stages.size()> m_lastStageTimes = {};
    std::vector<Entry> m_entries;
  };

}
