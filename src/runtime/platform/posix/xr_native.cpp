#include "common/environment.h"
#include "xr/native.h"
#include <cstdlib>
#include <dlfcn.h>
#include <time.h>
namespace venture::xr {
PFN_vkGetInstanceProcAddr native_vulkan_loader() {
  static auto loader = [] {
    auto dll = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    return dll ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(dll, "vkGetInstanceProcAddr"))
               : nullptr;
  }();
  return loader;
}
std::int64_t native_counter_to_ns(std::int64_t counter) { return counter; }
std::int64_t native_now_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1'000'000'000 + ts.tv_nsec;
}
bool set_runtime_manifest_for_process(const std::string &path) {
  const bool success = setenv("XR_RUNTIME_JSON", path.c_str(), 1) == 0;
  if (success) environment::refresh();
  return success;
}
} // namespace venture::xr
