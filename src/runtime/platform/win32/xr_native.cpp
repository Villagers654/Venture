#include "common/environment.h"
#include "xr/native.h"
#include <cstdlib>
#include <windows.h>
namespace venture::xr {
PFN_vkGetInstanceProcAddr native_vulkan_loader() {
  static auto loader = [] {
    auto dll = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return dll ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                     GetProcAddress(dll, "vkGetInstanceProcAddr"))
               : nullptr;
  }();
  return loader;
}
std::int64_t native_counter_to_ns(std::int64_t counter) {
  static const auto frequency = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
  }();
  return (counter / frequency) * 1'000'000'000 + (counter % frequency) * 1'000'000'000 / frequency;
}
std::int64_t native_now_ns() {
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return native_counter_to_ns(c.QuadPart);
}
bool set_runtime_manifest_for_process(const std::string &path) {
  // The loader uses the Win32 environment, while libraries using CRT getenv
  // may keep a separate copy. Set both within this process only.
  const bool success = SetEnvironmentVariableA("XR_RUNTIME_JSON", path.c_str()) &&
         _putenv_s("XR_RUNTIME_JSON", path.c_str()) == 0;
  if (success) environment::refresh();
  return success;
}
} // namespace venture::xr
