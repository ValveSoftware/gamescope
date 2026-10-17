#!/usr/bin/env python3
"""Exercise backend teardown against an in-flight present and real vblank code.

python3 src/tests/test_backend_teardown.py [--compare-ref HEAD] [--output /tmp/repro]
The optional ref must contain the original detached worker. It must SIGSEGV;
the working tree must stop and join without touching the deleted backend.
Only Vulkan/display resources are doubled; lifecycle and scheduler bodies and
the CVBlankTimer declaration come from the selected production source.
"""
import argparse
import os
from pathlib import Path
import resource
import signal
import subprocess
import tempfile

src = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compare-ref')
parser.add_argument('--output', type=Path)
parser.add_argument('--sanitize', choices=('address,undefined', 'thread'))
args = parser.parse_args()
if args.compare_ref and args.sanitize:
    parser.error('Run the intentional baseline crash without sanitizers')


def read(name, ref):
    if ref:
        return subprocess.check_output(['git', 'show', f'{ref}:src/{name}'], cwd=src, text=True)
    return (src / name).read_text()


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def build(directory, ref=None):
    vulkan = read('rendervulkan.cpp', ref)
    vblank = read('vblankmanager.cpp', ref)
    backend = read('backend.cpp', ref)
    fixed = 'std::jthread g_presentWaitThread' in vulkan
    assert fixed == (ref is None), 'Comparison ref must have the original detached worker'
    header = read('vblankmanager.hpp', ref).replace('#pragma once', '').replace('#include "waitable.h"', '')
    prelude = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
using namespace std::chrono_literals;
std::atomic<int> timerArms{0};
struct ITimerWaitable {
    virtual ~ITimerWaitable() = default;
    virtual int GetFD() { return -1; }
    virtual void OnPollIn() {}
    void DisarmTimer() {}
    void ArmTimer(uint64_t) { ++timerArms; }
};
struct Logger {
    void infof(const char*) {}
    void errorf_errno(const char*) {}
} g_VBlankLog;
uint64_t get_time_in_nanos() { return 1'000'000'000; }
uint64_t mHzToRefreshCycle(int) { return 16'666'667; }
uint64_t GetDeepestIdleResidency() { return 0; }
int g_nNestedRefresh = 60000, g_nOutputRefresh = 60000;
bool cv_adaptive_sync_uncapped = false;
void vulkan_stop_present_wait_thread();
std::atomic<bool> waiting{false}, releaseWait{false}, deleted{false};
std::atomic<int> callbacks{0};
using VkResult = int;
constexpr int VK_SUCCESS = 0, VK_TIMEOUT = 2;
struct Device {
    int device() { return 0; }
    struct Dispatch {
        int WaitForPresentKHR(int, int, uint64_t, uint64_t) {
            waiting = true;
            while (!releaseWait) std::this_thread::yield();
            return VK_SUCCESS;
        }
    } vk;
} g_device;
struct { int swapChain = 1; } g_output;
void mangoapp_output_update(uint64_t) { ++callbacks; }
namespace gamescope {
    enum GamescopeScreenType { GAMESCOPE_SCREEN_TYPE_INTERNAL };
    struct Connector { bool IsVRRActive() { return false; } };
    struct IBackend {
        virtual ~IBackend() { deleted = true; }
        virtual bool Init() { return true; }
        virtual bool NeedsFrameSync() { return false; }
        virtual GamescopeScreenType GetScreenType() { return GAMESCOPE_SCREEN_TYPE_INTERNAL; }
        virtual Connector *GetCurrentConnector() { return nullptr; }
        virtual bool SupportsIndependentRefresh() { return false; }
        static IBackend *Get();
        static bool Set(IBackend*);
    };
    struct StartingBackend : IBackend {
        bool Init() override;
        ~StartingBackend() override;
    };
    struct CDeferredBackend : IBackend {
        IBackend *child;
        explicit CDeferredBackend(IBackend *p) : child(p) {}
        bool Init() override;
        ~CDeferredBackend() override;
    };
    static IBackend *s_pBackend = nullptr;
    bool vblank_debug = false;
}
bool g_bAllowDeferredBackend = false;
bool failDeferred = false;
gamescope::IBackend *GetBackend() { return gamescope::IBackend::Get(); }
'''
    code = prelude + header
    if fixed:
        code += '\nstatic std::atomic<gamescope::CVBlankTimer *> s_pVBlankTimer = nullptr;\n'
    code += 'namespace gamescope {\n'
    for signature in ('IBackend *IBackend::Get()', 'bool IBackend::Set( IBackend *pBackend )'):
        code += function(backend, signature) + '\n'
    for signature in ('CVBlankTimer::CVBlankTimer()', 'CVBlankTimer::~CVBlankTimer()',
                      'uint64_t CVBlankTimer::GetLastVBlank()', 'uint64_t CVBlankTimer::VRRWakeupOffset(',
                      'VBlankScheduleTime CVBlankTimer::CalcNextWakeupTimeLocked(',
                      'void CVBlankTimer::MarkVBlank(', 'void CVBlankTimer::ArmNextVBlank(',
                      'bool CVBlankTimer::UsingTimerFD()'):
        code += function(vblank, signature) + '\n'
    if fixed:
        code += function(vblank, 'void CVBlankTimer::Shutdown()')
    code += '''
void CVBlankTimer::NudgeThread() {}
void CVBlankTimer::OnPollIn() {}
int CVBlankTimer::GetFD() { return -1; }
void CVBlankTimer::VBlankDebugSpew(uint64_t, uint64_t, uint64_t) {}
}
'''
    code += function(vblank, 'gamescope::CVBlankTimer &GetVBlankTimer()')
    if fixed:
        code += function(vblank, 'void StopVBlankTimer()')
    code += vulkan[vulkan.index('static std::atomic<uint64_t> g_currentPresentWaitId'):vulkan.index('void vulkan_update_swapchain_hdr_metadata')]
    if fixed:
        code += r'''
void startWorker() {
    assert(!g_presentWaitThread.joinable());
    g_currentPresentWaitId = 0;
    g_presentWaitThread = std::jthread(present_wait_thread_func);
}
bool gamescope::StartingBackend::Init() {
    GetVBlankTimer().MarkVBlank(get_time_in_nanos(), true);
    startWorker();
    return false;
}
gamescope::StartingBackend::~StartingBackend() {
    assert(!g_presentWaitThread.joinable());
}
bool gamescope::CDeferredBackend::Init() {
    startWorker();
    return !failDeferred;
}
gamescope::CDeferredBackend::~CDeferredBackend() {
    assert(!g_presentWaitThread.joinable());
    delete child;
}
'''
    else:
        code += r'''
bool gamescope::StartingBackend::Init() { return false; }
gamescope::StartingBackend::~StartingBackend() = default;
bool gamescope::CDeferredBackend::Init() { return false; }
gamescope::CDeferredBackend::~CDeferredBackend() { delete child; }
'''
    code += r'''
template<typename F> void until(F predicate) {
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
'''
    if fixed:
        code += r'''
    if (argc > 1) {
        g_bAllowDeferredBackend = argv[1][0] == 'd';
        failDeferred = argv[1][1] == 'f';
        bool success = gamescope::IBackend::Set(new gamescope::StartingBackend);
        assert(success == (g_bAllowDeferredBackend && !failDeferred));
        if (success) {
            assert(!deleted && g_presentWaitThread.joinable());
            int before = timerArms;
            GetVBlankTimer().MarkVBlank(get_time_in_nanos(), true);
            assert(timerArms == before + 1); // A successful fallback keeps scheduling.
            assert(gamescope::IBackend::Set(nullptr));
        }
        assert(deleted && !GetBackend() && !g_presentWaitThread.joinable());
        std::puts("PASS: partial initialization and deferred fallback cleanup");
        return 0;
    }
'''
    code += r'''
    assert(gamescope::IBackend::Set(new gamescope::IBackend));
    // Establish the scheduler before teardown, as in a running compositor.
    GetVBlankTimer().MarkVBlank(get_time_in_nanos(), true);
'''
    if fixed:
        code += 'g_presentWaitThread = std::jthread(present_wait_thread_func);\npublish_present_wait_id(1);\n'
    else:
        code += 'std::thread(present_wait_thread_func).detach();\ng_currentPresentWaitId = 1;\ng_currentPresentWaitId.notify_all();\n'
    code += 'until([] { return waiting.load(); });\n'
    if fixed:
        code += r'''
    std::thread teardown([] { assert(gamescope::IBackend::Set(nullptr)); });
    until([] { return g_currentPresentWaitId == k_PresentWaitStopped; });
    assert(!deleted); // Backend must remain alive while the worker is in Vulkan.
    releaseWait = true;
    teardown.join();
    assert(deleted && !GetBackend() && !g_presentWaitThread.joinable());
    assert(callbacks == 0);
    // A late backend feedback callback must not rearm the stopped scheduler.
    GetVBlankTimer().MarkVBlank(get_time_in_nanos(), true);
    std::puts("PASS: worker joined before backend deletion; no late rearm");
'''
    else:
        code += r'''
    assert(gamescope::IBackend::Set(nullptr));
    assert(deleted && !GetBackend());
    std::puts("Backend deleted while present worker was still in Vulkan");
    std::fflush(stdout);
    releaseWait = true; // Original worker now re-arms against a null backend.
    std::this_thread::sleep_for(2s);
    std::abort(); // Expected to SIGSEGV in the scheduler before reaching here.
'''
    code += '}\n'
    cpp = directory / ('baseline.cpp' if ref else 'fixed.cpp')
    cpp.write_text(code)
    binary = cpp.with_suffix('')
    sanitizer = ['-fsanitize=' + args.sanitize, '-fno-omit-frame-pointer'] if args.sanitize else []
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-pthread', '-g', *sanitizer,
                    '-O0', '-Wall', '-Wextra', '-Werror', '-I', str(src), str(cpp),
                    '-o', str(binary)], check=True)
    return binary


def run(directory):
    # Keep deliberate baseline crashes from writing a core file into the checkout.
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    if args.compare_ref:
        baseline = build(directory, args.compare_ref)
        outcome = subprocess.run([str(baseline)], timeout=10)
        assert outcome.returncode == -signal.SIGSEGV, outcome.returncode
        print('Original production path reproduced SIGSEGV.', flush=True)
    fixed = build(directory)
    subprocess.run([str(fixed)], check=True, timeout=10)
    for scenario in ('initial-failure', 'deferred-success', 'df-deferred-failure'):
        subprocess.run([str(fixed), scenario], check=True, timeout=10)


if args.output:
    args.output.mkdir(parents=True, exist_ok=True)
    run(args.output)
else:
    with tempfile.TemporaryDirectory(prefix='gamescope-backend-teardown-') as directory:
        run(Path(directory))
