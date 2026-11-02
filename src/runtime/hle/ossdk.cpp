#include "core/address_space.h"
#include "core/hle.h"
#include "core/linker.h"
#include "core/process.h"
#include "common/log.h"

#include <new>

namespace venture::hle::ossdk {
namespace {
void* create_manager32(std::int64_t version) {
  log::info("ossdk", "Device-manager version {} unavailable", version);
  return nullptr;
}
void destroy_manager32(std::int64_t, void* manager) {
  if (manager)
    log::warn("ossdk", "Ignoring a device manager not created by the unavailable service");
}

// UnifiedTelemetry v3 (OSSDK::UnifiedTelemetry::v3::ITelemetryHandler). Meta
// Interaction SDK creates a handler with createTelemetryHandler(3) the first
// time it counts a value and calls through it without a null check: handler
// slot 0 (h, "interactionsdk"), slot 3 (h, name) for an event, slot 9
// (h, event, 0) to send it; event slots 4-7 and 9 set a field. It frees them
// with destroyTelemetryHandler(3, h) and destroyTelemetryEvent(3, e). No
// telemetry is collected: one inert handler and one inert event are shared,
// every method returns 0 and creating an event returns that event. The device's
// v3 vtables have 18 handler and 19 event slots.
constexpr std::int64_t kTelemetryVersion = 3;
constexpr std::size_t kTelemetrySlots = 32;
struct Telemetry {
  GuestAddr handler = 0, event = 0;
};
const Telemetry& telemetry() {
  static const Telemetry objects = [] {
    // One read-only page: handler vtable, event vtable, handler, event.
    auto page = AddressSpace::get().allocate(kGuestPageSize, kProtRead | kProtWrite, "[ossdk-telemetry]");
    if (!page) throw std::bad_alloc();
    const GuestAddr handler_vtable = page, event_vtable = page + 8 * kTelemetrySlots;
    const Telemetry t{page + 16 * kTelemetrySlots, page + 16 * kTelemetrySlots + 8};
    const auto inert = make_thunk("libossdk.so", "UnifiedTelemetry::inert",
                                  [](CallContext& ctx) { ctx.return_x(0); });
    const auto create_event = make_thunk(
        "libossdk.so", "UnifiedTelemetry::ITelemetryHandler::createEvent",
        [](CallContext& ctx) { ctx.return_x(reinterpret_cast<std::uintptr_t>(ctx.user())); },
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(t.event)));
    auto* words = g2h<std::uint64_t>(page);
    for (std::size_t i = 0; i < 2 * kTelemetrySlots; ++i) words[i] = inert;
    words[3] = create_event;
    *g2h<std::uint64_t>(t.handler) = handler_vtable;
    *g2h<std::uint64_t>(t.event) = event_vtable;
    AddressSpace::get().protect(page, kGuestPageSize, kProtRead);
    return t;
  }();
  return objects;
}
}
// OSSDK's versioned factory ABI takes a guest long, and its destructor takes
// that same version followed by the factory result. No device-manager service
// is implemented yet: return null, while retaining the matching destructor.
// Consumers may own a null result with a custom deleter and still call it.
void register_libraries() {
  a32::register_adapter("libossdk.so", "createVrDeviceManager", a32::wrap32<&create_manager32>());
  a32::register_adapter("libossdk.so", "destroyVrDeviceManager", a32::wrap32<&destroy_manager32>());
  auto library = std::make_shared<HleLibrary>(
      "libossdk.so", std::vector<std::string>{"libossdk.oculus.so",
      "/system/lib64/libossdk.so", "/system/lib/libossdk.so"});
  library->add_function("createVrDeviceManager", make_thunk(
      library->soname(), "createVrDeviceManager", [](CallContext& ctx) {
        log::info("ossdk", "Device-manager version {} unavailable", ctx.x(0));
        ctx.return_x(0);
      }));
  library->add_function("destroyVrDeviceManager", make_thunk(
      library->soname(), "destroyVrDeviceManager", [](CallContext& ctx) {
        if (ctx.x(1))
          log::warn("ossdk", "Ignoring a device manager not created by the unavailable service");
        ctx.return_x(0);
      }));
  // Interaction SDK ships for arm64 only; A32 processes keep these unresolved.
  if (process_config().isa() == GuestIsa::a64) {
    library->add_function("createTelemetryHandler", make_thunk(
        library->soname(), "createTelemetryHandler", [](CallContext& ctx) {
          const auto version = static_cast<std::int64_t>(ctx.x(0));
          if (version != kTelemetryVersion) {
            log::info("ossdk", "Telemetry handler version {} unavailable", version);
            ctx.return_x(0);
            return;
          }
          log::info("ossdk", "Telemetry handler version {}: inert, events are dropped", version);
          ctx.return_x(telemetry().handler);
        }));
    // The handler and event are shared and never freed.
    for (const char* name : {"destroyTelemetryHandler", "destroyTelemetryEvent"})
      library->add_function(name, make_thunk(library->soname(), name,
                                             [](CallContext& ctx) { ctx.return_x(0); }));
  }
  Linker::get().register_hle_library(std::move(library));
}
} // namespace venture::hle::ossdk
