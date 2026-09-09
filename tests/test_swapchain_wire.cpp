#include <catch2/catch_test_macros.hpp>
#include "gamescope-swapchain-client-protocol.h"
#include "gamescope-limiter-client-protocol.h"
#include "wayland_test_server.hpp"

namespace
{
struct Events {
  unsigned past = 0;
  unsigned refresh = 0;
  unsigned retired = 0;
  uint64_t duration = 0;
  uint64_t interval = 0;
  std::vector<uint64_t> serials;
  std::vector<uint64_t> pixels;
  std::vector<uint64_t> queued;
  std::vector<uint64_t> dequeued;
};
const gamescope_swapchain_listener listener = {
  .past_present_timing = [](void *data, gamescope_swapchain *, uint32_t, uint32_t, uint32_t,
                           uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
    ++static_cast<Events *>(data)->past;
  },
  .refresh_cycle = [](void *data, gamescope_swapchain *, uint32_t, uint32_t) {
    ++static_cast<Events *>(data)->refresh;
  },
  .retired = [](void *data, gamescope_swapchain *) { ++static_cast<Events *>(data)->retired; },
  .present_timing = [](void *data, gamescope_swapchain *, uint32_t hi, uint32_t lo,
                      uint32_t queueHi, uint32_t queueLo, uint32_t dequeueHi, uint32_t dequeueLo, uint32_t pixelHi, uint32_t pixelLo) {
    auto &events = *static_cast<Events *>(data);
    events.serials.push_back((uint64_t(hi) << 32) | lo);
    events.pixels.push_back((uint64_t(pixelHi) << 32) | pixelLo);
    events.queued.push_back((uint64_t(queueHi) << 32) | queueLo);
    events.dequeued.push_back((uint64_t(dequeueHi) << 32) | dequeueLo);
  },
  .timing_properties = [](void *data, gamescope_swapchain *, uint32_t durationHi, uint32_t durationLo,
                         uint32_t intervalHi, uint32_t intervalLo) {
    auto &events = *static_cast<Events *>(data);
    events.duration = (uint64_t(durationHi) << 32) | durationLo;
    events.interval = (uint64_t(intervalHi) << 32) | intervalLo;
  },
};

struct WireClient {
  WaylandTestServer peer;
  wl_registry *registry;
  gamescope_swapchain_factory_v2 *factory;
  wl_surface *surface;
  gamescope_swapchain *swapchain;
  Events events;
  wl_interface interface = gamescope_swapchain_interface;

  WireClient(uint32_t version, bool legacy, bool timing) : peer(true, version) {
    registry = wl_display_get_registry(peer.display);
    // Compositor is global 1 and the factory is global 2 in this peer.
    factory = static_cast<gamescope_swapchain_factory_v2 *>(wl_registry_bind(registry, 2,
      &gamescope_swapchain_factory_v2_interface, version));
    wl_registry_destroy(registry);
    registry = nullptr;
    surface = peer.CreateSurface();
    if (legacy) {
      interface.version = 1;
      interface.method_count = 6;
      interface.event_count = 3;
    }
    // Mirror an old generated constructor while retaining its three-event descriptor.
    swapchain = reinterpret_cast<gamescope_swapchain *>(wl_proxy_marshal_flags(
      reinterpret_cast<wl_proxy *>(factory), timing ? GAMESCOPE_SWAPCHAIN_FACTORY_V2_CREATE_SWAPCHAIN_WITH_TIMING : GAMESCOPE_SWAPCHAIN_FACTORY_V2_CREATE_SWAPCHAIN,
      &interface, version, 0, surface, nullptr));
    REQUIRE(swapchain);
    gamescope_swapchain_add_listener(swapchain, &listener, &events);
    peer.Sync();
  }

  void Dispatch() {
    REQUIRE(wl_display_roundtrip(peer.display) >= 0);
    REQUIRE(wl_display_get_error(peer.display) == 0);
  }

  ~WireClient() {
    if (swapchain) gamescope_swapchain_destroy(swapchain);
    wl_surface_destroy(surface);
    gamescope_swapchain_factory_v2_destroy(factory);
    if (registry) wl_registry_destroy(registry);
  }
};
}

TEST_CASE("A shipped three-event layer can bind version two without opting in", "[swapchain_wire]") {
  WireClient client(2, true, false);
  client.peer.SendTimingEvents();
  client.peer.SendLegacyEvents();
  client.Dispatch();
  REQUIRE(client.events.past == 1);
  REQUIRE(client.events.refresh == 1);
  REQUIRE(client.events.retired == 1);
  REQUIRE(client.events.serials.empty());
}

TEST_CASE("The legacy constructor does not opt in even with current event descriptors", "[swapchain_wire]") {
  WireClient client(2, false, false);
  client.peer.SendTimingEvents();
  client.Dispatch();
  REQUIRE(client.events.duration == 0);
  REQUIRE(client.events.serials.empty());
}

TEST_CASE("The timing constructor receives properties and full-width reports", "[swapchain_wire]") {
  WireClient client(2, false, true);
  client.peer.SendTimingEvents();
  client.Dispatch();
  REQUIRE(client.events.duration == 8'333'333);
  REQUIRE(client.events.interval == UINT64_MAX);
  REQUIRE(client.events.serials == std::vector<uint64_t>{0x100000002, 0x100000006});
  REQUIRE(client.events.pixels == std::vector<uint64_t>{0x400000005, 0});
  REQUIRE(client.events.queued == std::vector<uint64_t>{0x200000003, 0});
  REQUIRE(client.events.dequeued == std::vector<uint64_t>{0x300000004, 0});
}

TEST_CASE("Version one clients retain legacy events", "[swapchain_wire]") {
  WireClient client(1, true, false);
  client.peer.SendTimingEvents();
  client.peer.SendLegacyEvents();
  client.Dispatch();
  REQUIRE(client.events.past == 1);
  REQUIRE(client.events.refresh == 1);
  REQUIRE(client.events.retired == 1);
}

TEST_CASE("Retained timing routes stop sending after resource destruction", "[swapchain_wire]") {
  WireClient client(2, false, true);
  std::shared_ptr<gamescope::PresentTimingRoute> route;
  client.peer.Run([&] { route = client.peer.routes.begin()->second; });
  gamescope_swapchain_destroy(client.swapchain);
  client.swapchain = nullptr;
  client.peer.Sync();
  bool sent = true;
  client.peer.Run([&] {
    route->SendTimingProperties(1, 1);
    sent = route->SendPresentTiming(1, 1, 1, 1);
  });
  REQUIRE_FALSE(sent);
  client.Dispatch();
  REQUIRE(client.events.serials.empty());
}
