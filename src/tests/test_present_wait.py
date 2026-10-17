#!/usr/bin/env python3
"""Run with python3 src/tests/test_present_wait.py [extra compiler flags].

Compile the production worker/lifecycle functions against deterministic Vulkan
and backend doubles. No GPU, display server, Meson, or Catch2 is required.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

src = Path(__file__).resolve().parents[1]
vulkan = (src / 'rendervulkan.cpp').read_text()
vblank = (src / 'vblankmanager.cpp').read_text()


def function(source, signature):
    start = source.index(signature)
    end = source.index('\n}', start) + 2
    return source[start:end]


# Keep the actual atomic protocol, Vulkan result handling, and join logic.
worker = vulkan[vulkan.index('static std::atomic<uint64_t> g_currentPresentWaitId'):vulkan.index('void vulkan_update_swapchain_hdr_metadata')]
init = function(vulkan, 'bool vulkan_init( VkInstance')
remake = function(vulkan, 'bool vulkan_remake_swapchain( void )')
# Member functions have a namespace indentation level.
def member(signature):
    start = vblank.index(signature)
    end = vblank.index('\n\t}', start) + 3
    return vblank[start:end]

program = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
using namespace std::chrono_literals;
using VkInstance = int;
using VkSurfaceKHR = int;
using VkResult = int;
constexpr int VK_SUCCESS = 0, VK_TIMEOUT = 2, VK_ERROR_OUT_OF_DATE_KHR = -1;
std::atomic<int> calls{0}, feedback{0}, mango{0}, result{VK_TIMEOUT};
std::atomic<bool> inWait{false}, holdWait{false};
std::atomic<int> liveSwapchain{1};
struct VulkanOutput_t {
    int swapChain = 1, surface = 0;
    std::vector<int> outputImages;
    std::vector<std::shared_ptr<int>> pScreenshotTextures, pCaptureTextures;
} g_output;
struct Device {
    int device() { return 0; }
    int queue() { return 0; }
    bool BInit(int, int) { return true; }
    void waitIdle() {}
    struct Dispatch {
        int WaitForPresentKHR(int, int chain, uint64_t id, uint64_t timeout) {
            assert(id && id != UINT64_MAX);
            assert(timeout && timeout <= 50'000'000);
            assert(chain == liveSwapchain);
            inWait = true;
            ++calls;
            while (holdWait) std::this_thread::yield();
            std::this_thread::sleep_for(1ms);
            assert(chain == liveSwapchain);
            inWait = false;
            return result;
        }
        void QueueWaitIdle(int) {}
        void DestroySwapchainKHR(int, int chain, void*) {
            assert(!inWait && chain == liveSwapchain);
            liveSwapchain = 0;
        }
    } vk;
} g_device;
bool init_nis_data() { return true; }
bool vulkan_make_swapchain(VulkanOutput_t *output) {
    output->swapChain = 2;
    liveSwapchain = 2;
    return true;
}
struct VBlankScheduleTime { uint64_t ulTargetVBlank, ulScheduledWakeupPoint; };
uint64_t get_time_in_nanos() { return 1; }
uint64_t mHzToRefreshCycle(int) { return 1; }
void sleep_for_nanos(uint64_t) {}
int g_nNestedRefresh = 60000, g_nOutputRefresh = 60000;
struct Logger { void errorf_errno(const char*) {} } g_VBlankLog;
void gpuvis_trace_printf(const char*) {}
struct ITimerWaitable { void DisarmTimer() {} };
struct CVBlankTimer : ITimerWaitable {
    std::mutex m_ScheduleMutex;
    std::atomic<bool> m_bArmed{false}, m_bRunning{true};
    std::thread m_NudgeThread;
    int m_nNudgePipe[2]{-1, -1};
    void MarkVBlank(uint64_t, bool) { ++feedback; }
    void WaitToBeArmed();
    void Shutdown();
    void NudgeThread();
};
CVBlankTimer timer;
CVBlankTimer &GetVBlankTimer() { return timer; }
struct Connector {
    std::function<VBlankScheduleTime()> sync;
    VBlankScheduleTime FrameSync() { return sync(); }
} connector;
struct Backend {
    bool UsesVulkanSwapchain() { return true; }
    Connector *GetCurrentConnector() { return &connector; }
} backend;
Backend *GetBackend() { return &backend; }
struct VBlankTime { VBlankScheduleTime schedule; uint64_t ulWakeupTime; };
void mangoapp_output_update(uint64_t) { ++mango; }
'''
program += worker + init + remake
program += '\n'.join(member(s) for s in (
    'void CVBlankTimer::Shutdown()',
    'void CVBlankTimer::WaitToBeArmed()',
    'void CVBlankTimer::NudgeThread()'))
program += r'''
template<typename Predicate> void until(Predicate predicate) {
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
void start() {
    calls = feedback = mango = 0;
    result = VK_TIMEOUT;
    assert(vulkan_init(0, 0));
}
int main() {
    // No-op stop and repeated start/stop, including stop before callback setup.
    vulkan_stop_present_wait_thread();
    for (int i = 0; i < 200; ++i) {
        start();
        if (i % 2) std::this_thread::sleep_for(100us);
        vulkan_stop_present_wait_thread();
        vulkan_stop_present_wait_thread();
        assert(!g_presentWaitThread.joinable());
    }
    // Timeouts retry the same ID, without generating fake feedback.
    start();
    publish_present_wait_id(1);
    until([] { return calls >= 3; });
    assert(feedback == 0 && mango == 0);
    result = VK_SUCCESS;
    until([] { return feedback == 1; });
    // Errors consume the ID, without generating feedback or spinning.
    result = VK_ERROR_OUT_OF_DATE_KHR;
    int beforeError = calls;
    publish_present_wait_id(2);
    until([&] { return calls > beforeError; });
    std::this_thread::sleep_for(5ms);
    assert(calls == beforeError + 1);
    vulkan_stop_present_wait_thread();
    assert(feedback == 1 && mango == 1);
    // Atomic reset/publication racing shutdown cannot overwrite its sentinel.
    for (int i = 0; i < 50; ++i) {
        start();
        std::atomic<bool> publishing{true};
        std::thread producer([&] {
            while (publishing) {
                publish_present_wait_id(3);
                publish_present_wait_id(0);
            }
        });
        vulkan_stop_present_wait_thread();
        publishing = false;
        producer.join();
        assert(g_currentPresentWaitId == k_PresentWaitStopped);
    }
    // Recreation cannot destroy an in-use swapchain; timeout retries unlock it.
    start();
    publish_present_wait_id(4);
    until([] { return calls >= 2; });
    assert(vulkan_remake_swapchain());
    publish_present_wait_id(5);
    until([] { return calls >= 3; });
    vulkan_stop_present_wait_thread();
    assert(feedback == 0);
    // A successful Vulkan return racing stop must not call the scheduler.
    start();
    holdWait = true;
    result = VK_SUCCESS;
    publish_present_wait_id(6);
    until([] { return inWait.load(); });
    std::thread stopper(vulkan_stop_present_wait_thread);
    until([] { return g_currentPresentWaitId == k_PresentWaitStopped; });
    holdWait = false;
    stopper.join();
    assert(feedback == 0 && mango == 0);
    // Stop also works while the worker is waiting for the swapchain lock.
    start();
    {
        std::unique_lock lock(present_wait_lock);
        publish_present_wait_id(7);
        std::thread blockedStopper(vulkan_stop_present_wait_thread);
        until([] { return g_currentPresentWaitId == k_PresentWaitStopped; });
        lock.unlock();
        blockedStopper.join();
    }
    assert(calls == 0 && feedback == 0);
    // Stop an idle nudge thread, and one inside FrameSync taking its mutex.
    for (bool active : {false, true}) {
        CVBlankTimer nudge;
        assert(pipe2(nudge.m_nNudgePipe, O_NONBLOCK | O_CLOEXEC) == 0);
        std::atomic<bool> entered{false}, release{false};
        connector.sync = [&] {
            entered = true;
            until([&] { return release.load(); });
            std::unique_lock lock(nudge.m_ScheduleMutex);
            return VBlankScheduleTime{1, 1};
        };
        nudge.m_NudgeThread = std::thread([&] { nudge.NudgeThread(); });
        if (active) {
            nudge.m_bArmed = true;
            nudge.m_bArmed.notify_all();
            until([&] { return entered.load(); });
        }
        std::thread stopNudge([&] { nudge.Shutdown(); });
        until([&] { return !nudge.m_bRunning.load(); });
        release = true;
        stopNudge.join();
        assert(nudge.m_bArmed && !nudge.m_NudgeThread.joinable());
        VBlankTime time;
        assert(read(nudge.m_nNudgePipe[0], &time, sizeof(time)) == -1);
        nudge.Shutdown();
        close(nudge.m_nNudgePipe[0]);
        close(nudge.m_nNudgePipe[1]);
    }
}
'''
with tempfile.TemporaryDirectory(prefix='gamescope-present-wait-') as directory:
    test = Path(directory) / 'test.cpp'
    test.write_text(program)
    binary = Path(directory) / 'test'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-pthread',
                    '-Wall', '-Wextra', '-Werror', *sys.argv[1:], str(test),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
print('Present worker and vblank shutdown regression checks passed.')
