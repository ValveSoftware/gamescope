#!/usr/bin/env python3
"""Compile the production Wayland loop and compositor cleanup against test peers.

python3 src/tests/test_wlserver_shutdown.py [--compare-ref REV] [--sanitize KIND]
The optional ref must predate the Wayland shutdown handshake. Its deletion of
an active input callback must fail; the working tree must finish safely.
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
parser.add_argument('--sanitize', choices=('thread', 'address,undefined'))
args = parser.parse_args()
if args.compare_ref and args.sanitize:
    parser.error('Run the intentional baseline failure without sanitizers')


def read(name, ref):
    if ref:
        return subprocess.check_output(['git', 'show', f'{ref}:src/{name}'], cwd=src, text=True)
    return (src / name).read_text()


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def build(directory, ref=None):
    wl = read('wlserver.cpp', ref)
    wm = read('steamcompmgr.cpp', ref)
    fixed = 'bWaylandServerStopped.wait( false )' in wm
    assert fixed == (ref is None), 'Comparison ref must predate the handshake'
    code = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
using namespace std::chrono_literals;
#define HAVE_LIBEIS 1
#define HAVE_SESSION 0
std::atomic<bool> inputActive{false}, eisActive{false}, releaseInput{false}, releaseEis{false};
std::atomic<bool> eisStopping{false}, eisJoined{false}, deleted{false}, teardownStarted{false};
std::atomic<bool> g_bRun{true}, g_bShutdownWLServer{false};
bool failPoll = false;
std::atomic<int> errors{0};
std::mutex waylock;
std::mutex g_SteamCompMgrXWaylandServerMutex;
void wlserver_lock() { waylock.lock(); }
void wlserver_unlock(bool = true) { waylock.unlock(); }
bool wlserver_is_lock_held() { return true; }
void nudge_steamcompmgr() {}
struct Logger {
    void errorf_errno(const char*) { ++errors; }
    void errorf(const char*, const char*) {}
} wl_log;
namespace gamescope {
    constexpr int GAMESCOPE_SCREEN_TYPE_COUNT = 2;
    enum class InputType { Mouse };
    struct IBackend {
        virtual ~IBackend() {
            assert(!inputActive && !eisActive); // No callback may overlap deletion.
            deleted = true;
        }
        virtual void NotifyPhysicalInput(InputType) {
            inputActive = true;
            while (!releaseInput) std::this_thread::yield();
            assert(!deleted);
            inputActive = false;
        }
        static void Set(IBackend *backend);
    };
}
gamescope::IBackend *backend = new gamescope::IBackend;
gamescope::IBackend *GetBackend() { return backend; }
void gamescope::IBackend::Set(IBackend *p) { delete backend; backend = p; }
struct Waiter {
    std::thread thread;
    explicit Waiter(const char*) {}
    ~Waiter() { Shutdown(); }
    void Shutdown() {
        eisStopping = true;
        if (thread.joinable()) thread.join();
        eisJoined = true;
    }
};
struct InputServer { ~InputServer() { assert(eisJoined && !eisActive); } };
namespace gamescope {
    using GamescopeInputServer = ::InputServer;
    using CAsyncWaiter = ::Waiter;
}
'''
    declarations = min(wl.index('static gamescope::CAsyncWaiter g_LibEisWaiter'),
                       wl.index('static std::unique_ptr<gamescope::GamescopeInputServer> g_InputServer'))
    code += wl[declarations:wl.index('#endif', declarations)]
    code += r'''
struct Entry { int present_timing = 0; };
struct Server {
    std::atomic<bool> bWaylandServerStarted{false}, bWaylandServerStopped{false};
    std::atomic<bool> bWaylandServerRunning{false}; // For the comparison revision.
    void *display = this, *event_loop = this;
    bool physical_cursor_move = false;
    struct { std::vector<int> xwayland_servers; } wlr;
    struct Listener { int link = 0; } new_pointer_constraint, new_xdg_surface,
        new_xdg_toplevel, new_layer_shell_surface;
    std::mutex xdg_commit_lock;
    std::vector<Entry> xdg_commit_queue;
    std::vector<int> xdg_wins;
} wlserver;
std::mutex g_wlserver_xdg_shell_windows_lock;
int g_wlserverNudgePipe[2]{-1, -1};
int wl_event_loop_get_fd(void*) { return -1; }
void wl_display_flush_clients(void*) {}
int wl_event_loop_dispatch(void*, int) { wlserver.physical_cursor_move = true; return 0; }
int fakePoll(pollfd *fds, unsigned long, int) {
    if (failPoll) { errno = EIO; return -1; }
    fds[0].revents = POLLIN;
    return 1;
}
#define poll fakePoll
void wlserver_present_timing_discard(int&) {}
void wl_list_remove(int*) {}
void wl_display_destroy_clients(void*) { assert(deleted && eisJoined); }
void wl_display_destroy(void*) { assert(deleted && eisJoined); }
struct { int link = 0; } new_surface_listener, new_input_listener;
'''
    code += r'''
void wlserver_shutdown();
struct { void Shutdown() {} } g_ImageWaiter;
struct steamcompmgr_win_t {
    std::vector<int> commit_queue;
    struct { steamcompmgr_win_t *next = nullptr; } state;
    auto &xwayland() { return state; }
};
struct gamescope_xwayland_server_t {
    struct Context { steamcompmgr_win_t *list = nullptr; } *ctx;
};
gamescope_xwayland_server_t *wlserver_get_xwayland_server(unsigned) { return nullptr; }
std::vector<int> g_steamcompmgr_xdg_wins, g_VirtualConnectorFocuses;
struct Lut { void shutdown() {} };
Lut g_ColorMgmtLuts[1], g_ColorMgmtLutsOverride[1], g_ScreenshotColorMgmtLuts[1], g_ScreenshotColorMgmtLutsHDR[1];
bool statsThreadRun = false;
struct { void signal() {} } statsThreadSem;
struct { struct { void *appHDRMetadata = nullptr; } pending, current; } g_ColorMgmt;
void *s_scRGB709To2020Matrix = nullptr;
void *s_MuraCorrectionImage[2]{}, *s_MuraCTMBlob[2]{};
'''
    code += function(wl, 'void wlserver_run(void)')
    code += function(wl, 'void wlserver_shutdown()')
    code += function(wm, 'static void\nsteamcompmgr_exit(void)')
    code += r'''
template<typename F> void until(F predicate) {
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
int main(int argc, char **argv) {
    int mode = argc > 1 ? argv[1][0] - '0' : 0;
    failPoll = mode == 2;
    g_InputServer = std::make_unique<InputServer>();
    g_LibEisWaiter.thread = std::thread([] {
        eisActive = true;
        while (!releaseEis) std::this_thread::yield();
        assert(!deleted);
        eisActive = false;
    });
    until([] { return eisActive.load(); });
    if (mode == 3) {
        releaseEis = true;
        return 0; // Static cleanup must join the waiter before destroying its server.
    }
    // Match the compositor's ownership of the Xwayland cleanup barrier.
    std::thread compositor([&] {
        std::unique_lock guard(g_SteamCompMgrXWaylandServerMutex);
        if (mode == 0) until([] { return inputActive.load(); });
        if (mode == 2) until([] { return !g_bRun.load(); });
        teardownStarted = true;
        steamcompmgr_exit();
    });
    if (mode == 1) until([] { return teardownStarted.load(); });
    std::thread server(wlserver_run);
'''
    if fixed:
        code += r'''
    until([] { return g_bShutdownWLServer.load(); });
    assert(!deleted);
    releaseInput = true;
    until([] { return eisStopping.load(); });
    assert(!deleted && g_InputServer);
    releaseEis = true;
    server.join();
    compositor.join();
    assert(deleted && !GetBackend() && eisJoined);
    assert(wlserver.bWaylandServerStopped && !g_InputServer);
    assert(errors == (failPoll ? 1 : 0));
    // A delayed SDL startup waiter must observe even a very short dispatch loop.
    wlserver.bWaylandServerStarted.wait(false);
    close(g_wlserverNudgePipe[0]);
    close(g_wlserverNudgePipe[1]);
    std::puts("PASS: dispatch and libei callbacks finished before backend deletion");
'''
    else:
        code += r'''
    // The old cleanup deletes the backend while its input callback is active.
    compositor.join();
    std::abort();
'''
    code += '}\n'
    cpp = directory / ('baseline.cpp' if ref else 'fixed.cpp')
    cpp.write_text(code)
    flags = ['-fsanitize=' + args.sanitize, '-fno-omit-frame-pointer'] if args.sanitize else []
    binary = cpp.with_suffix('')
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-pthread', '-g',
                    '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
                    *flags, str(cpp), '-o', str(binary)], check=True)
    return binary


resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
with tempfile.TemporaryDirectory(prefix='gamescope-wlserver-shutdown-') as directory:
    directory = Path(directory)
    if args.compare_ref:
        baseline = build(directory, args.compare_ref)
        result = subprocess.run([str(baseline)], timeout=10)
        assert result.returncode == -signal.SIGABRT, result.returncode
        print('Original cleanup reproduced deletion during an input callback.', flush=True)
        result = subprocess.run([str(baseline), '3'], timeout=10)
        assert result.returncode == -signal.SIGABRT, result.returncode
        print('Original static cleanup also destroyed the input server before its waiter.', flush=True)
    fixed = build(directory)
    for mode in ('0', '1', '2', '3'):
        subprocess.run([str(fixed), mode], check=True, timeout=10)
