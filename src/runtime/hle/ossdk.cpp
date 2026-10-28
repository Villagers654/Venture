#include "core/hle.h"
#include "core/linker.h"
#include "common/log.h"

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
  Linker::get().register_hle_library(std::move(library));
}
} // namespace venture::hle::ossdk
