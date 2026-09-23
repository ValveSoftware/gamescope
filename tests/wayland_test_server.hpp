#pragma once

#include <wayland-server-core.h>
#include <wayland-client.h>
#include <algorithm>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>
#include "../src/WaylandServer/SwapchainTiming.h"
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <future>
#include <mutex>
#include <thread>

// A real protocol peer; all server-side work runs on its event-loop thread.
struct WaylandTestServer {
  wl_display *server = wl_display_create();
  wl_display *display = nullptr;
  wl_event_queue *syncQueue = nullptr;
  int wakeFd = eventfd(0, EFD_CLOEXEC);
  wl_event_source *wakeSource = nullptr;
  std::thread thread;
  std::mutex mutex;
  std::vector<std::function<void()>> tasks;
  std::vector<wl_resource *> limiters;
  std::vector<wl_resource *> swapchains;
  std::unordered_map<wl_resource *, std::shared_ptr<gamescope::PresentTimingRoute>> routes;
  unsigned factories = 0;
  unsigned surfaces = 0;

  explicit WaylandTestServer(bool compositor = true, uint32_t factoryVersion = 2) {
    REQUIRE(server);
    REQUIRE(wakeFd >= 0);
    int sockets[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    REQUIRE(wl_client_create(server, sockets[1]));
    display = wl_display_connect_to_fd(sockets[0]);
    REQUIRE(display);
    syncQueue = wl_display_create_queue(display);
    REQUIRE(syncQueue);
    if (compositor)
      REQUIRE(wl_global_create(server, &wl_compositor_interface, 1, this,
        [](wl_client *client, void *data, uint32_t version, uint32_t id) {
          auto *resource = wl_resource_create(client, &wl_compositor_interface, version, id);
          static const struct {
            void (*createSurface)(wl_client *, wl_resource *, uint32_t);
            void (*createRegion)(wl_client *, wl_resource *, uint32_t);
          } implementation = {
            [](wl_client *client, wl_resource *compositor, uint32_t id) {
              auto *self = static_cast<WaylandTestServer *>(wl_resource_get_user_data(compositor));
              auto *surface = wl_resource_create(client, &wl_surface_interface, 1, id);
              static const struct { void (*destroy)(wl_client *, wl_resource *); } surfaceImplementation = {
                [](wl_client *, wl_resource *surface) { wl_resource_destroy(surface); },
              };
              ++self->surfaces;
              wl_resource_set_implementation(surface, &surfaceImplementation, self, [](wl_resource *surface) {
                --static_cast<WaylandTestServer *>(wl_resource_get_user_data(surface))->surfaces;
              });
            }, nullptr,
          };
          wl_resource_set_implementation(resource, &implementation, data, nullptr);
        }));
    REQUIRE(wl_global_create(server, &gamescope_swapchain_factory_v2_interface, factoryVersion, this,
      [](wl_client *client, void *data, uint32_t version, uint32_t id) {
        auto *self = static_cast<WaylandTestServer *>(data);
        auto *resource = wl_resource_create(client, &gamescope_swapchain_factory_v2_interface, version, id);
        static const struct gamescope_swapchain_factory_v2_interface implementation = {
          .destroy = [](wl_client *, wl_resource *resource) { wl_resource_destroy(resource); },
          .create_swapchain = [](wl_client *client, wl_resource *factory, wl_resource *, uint32_t id) {
            CreateSwapchain(client, factory, id, false);
          },
          .create_swapchain_with_timing = [](wl_client *client, wl_resource *factory, wl_resource *, uint32_t id) {
            CreateSwapchain(client, factory, id, true);
          },
        };
        ++self->factories;
        wl_resource_set_implementation(resource, &implementation, self, [](wl_resource *resource) {
          --static_cast<WaylandTestServer *>(wl_resource_get_user_data(resource))->factories;
        });
      }));
    REQUIRE(wl_global_create(server, &gamescope_limiter_interface, 1, this,
      [](wl_client *client, void *data, uint32_t version, uint32_t id) {
        auto *self = static_cast<WaylandTestServer *>(data);
        auto *resource = wl_resource_create(client, &gamescope_limiter_interface, version, id);
        static const struct { void (*destroy)(wl_client *, wl_resource *); } implementation = {
          [](wl_client *, wl_resource *resource) { wl_resource_destroy(resource); },
        };
        self->limiters.push_back(resource);
        wl_resource_set_implementation(resource, &implementation, self, [](wl_resource *resource) {
          auto *self = static_cast<WaylandTestServer *>(wl_resource_get_user_data(resource));
          std::erase(self->limiters, resource);
        });
        wl_resource_post_event(resource, 0, 0u);
      }));
    wakeSource = wl_event_loop_add_fd(wl_display_get_event_loop(server), wakeFd, WL_EVENT_READABLE,
      [](int fd, uint32_t, void *data) {
        uint64_t value;
        if (read(fd, &value, sizeof(value)) != sizeof(value))
          return 0;
        auto *self = static_cast<WaylandTestServer *>(data);
        std::vector<std::function<void()>> jobs;
        {
          std::lock_guard lock(self->mutex);
          jobs.swap(self->tasks);
        }
        for (auto &job : jobs)
          job();
        return 0;
      }, this);
    REQUIRE(wakeSource);
    thread = std::thread([&] { wl_display_run(server); });
  }

  static void CreateSwapchain(wl_client *client, wl_resource *factory, uint32_t id, bool timing) {
    auto *self = static_cast<WaylandTestServer *>(wl_resource_get_user_data(factory));
    auto *resource = wl_resource_create(client, &gamescope_swapchain_interface, wl_resource_get_version(factory), id);
    static const struct { void (*destroy)(wl_client *, wl_resource *); } implementation = {
      [](wl_client *, wl_resource *resource) { wl_resource_destroy(resource); },
    };
    self->swapchains.push_back(resource);
    self->routes.emplace(resource, std::make_shared<gamescope::PresentTimingRoute>(gamescope::PresentTimingRoute{
      .resource = resource, .timing_events = timing}));
    wl_resource_set_implementation(resource, &implementation, self, [](wl_resource *resource) {
      auto *self = static_cast<WaylandTestServer *>(wl_resource_get_user_data(resource));
      self->routes.at(resource)->resource = nullptr;
      self->routes.erase(resource);
      std::erase(self->swapchains, resource);
    });
  }

  void Run(std::function<void()> fn) {
    std::promise<void> done;
    auto future = done.get_future();
    {
      std::lock_guard lock(mutex);
      tasks.push_back([&] { fn(); done.set_value(); });
    }
    uint64_t value = 1;
    REQUIRE(write(wakeFd, &value, sizeof(value)) == sizeof(value));
    future.get();
  }

  void SendState(uint32_t value) {
    Run([&] {
      for (auto *limiter : limiters)
        wl_resource_post_event(limiter, 0, value);
      wl_display_flush_clients(server);
    });
  }

  void SendRefreshCycle(uint32_t cycle) {
    Run([&] {
      for (auto *swapchain : swapchains)
        wl_resource_post_event(swapchain, 1, 0u, cycle);
      wl_display_flush_clients(server);
    });
  }

  void SendTimingEvents() {
    Run([&] {
      for (auto &[resource, route] : routes) {
        route->SendTimingProperties(8'333'333, UINT64_MAX);
        route->SendPresentTiming(0x100000002, 0x200000003, 0x300000004, 0x400000005);
        route->SendPresentTiming(0x100000006, 0, 0, 0);
      }
      wl_display_flush_clients(server);
    });
  }

  void SendLegacyEvents() {
    Run([&] {
      for (auto *resource : swapchains) {
        gamescope_swapchain_send_refresh_cycle(resource, 0, 4'166'667);
        gamescope_swapchain_send_past_present_timing(resource, 7, 0, 0, 0, 10, 0, 9, 0, 1);
        gamescope_swapchain_send_retired(resource);
      }
      wl_display_flush_clients(server);
    });
  }

  void Sync() { REQUIRE(wl_display_roundtrip_queue(display, syncQueue) >= 0); }

  wl_surface *CreateSurface() {
    auto *registry = wl_display_get_registry(display);
    // The compositor is the first global exported by this fixture.
    auto *compositor = static_cast<wl_compositor *>(wl_registry_bind(registry, 1, &wl_compositor_interface, 1));
    auto *surface = wl_compositor_create_surface(compositor);
    wl_compositor_destroy(compositor);
    wl_registry_destroy(registry);
    Sync();
    return surface;
  }

  unsigned SurfaceCount() {
    unsigned result = 0;
    Run([&] { result = surfaces; });
    return result;
  }

  unsigned LimiterCount() {
    unsigned result = 0;
    Run([&] { result = limiters.size(); });
    return result;
  }

  unsigned FactoryCount() {
    unsigned result = 0;
    Run([&] { result = factories; });
    return result;
  }

  ~WaylandTestServer() {
    Run([&] { wl_display_terminate(server); });
    thread.join();
    wl_event_queue_destroy(syncQueue);
    wl_display_disconnect(display);
    wl_display_destroy_clients(server);
    wl_event_source_remove(wakeSource);
    close(wakeFd);
    wl_display_destroy(server);
  }
};
