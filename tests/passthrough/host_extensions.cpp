// Exercise the production host initializer against an external OpenXR host.
// Each invocation is a fresh process, as host_xr() owns a process-lifetime instance.
#include "common/log.h"
#include "xr/display_memory.h"
#include "xr/host_xr.h"
#include <openxr/openxr.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
std::vector<const char*> advertised{"XR_KHR_vulkan_enable", "XR_KHR_vulkan_enable2", "XR_EXT_hand_tracking",
                                    "XR_VENDOR_not_implemented"};
std::set<std::string> requested;
unsigned creates = 0;
}
namespace venture::log {
Level level() { return Level::error; }
void write(Level, std::string_view, std::string_view message) {
  std::fprintf(stderr, "%.*s\n", int(message.size()), message.data());
}
}
namespace venture::xr {
void recall_display_period(std::string_view, std::string_view) {}
}

extern "C" {
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateInstanceExtensionProperties(
    const char*, std::uint32_t capacity, std::uint32_t* count, XrExtensionProperties* properties) {
  *count = static_cast<std::uint32_t>(advertised.size());
  if (!capacity) return XR_SUCCESS;
  if (capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT;
  for (std::size_t i = 0; i < advertised.size(); ++i) {
    std::strcpy(properties[i].extensionName, advertised[i]);
    properties[i].extensionVersion = 1;
  }
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateInstance(const XrInstanceCreateInfo* info, XrInstance* instance) {
  ++creates;
  for (std::uint32_t i = 0; i < info->enabledExtensionCount; ++i) requested.insert(info->enabledExtensionNames[i]);
  *instance = reinterpret_cast<XrInstance>(1);
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProperties(XrInstance, XrInstanceProperties* properties) {
  std::strcpy(properties->runtimeName, "Passthrough test host");
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetSystem(XrInstance, const XrSystemGetInfo*, XrSystemId*) {
  return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetSystemProperties(XrInstance, XrSystemId, XrSystemProperties*) {
  return XR_ERROR_FUNCTION_UNSUPPORTED;
}
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateViewConfigurationViews(
    XrInstance, XrSystemId, XrViewConfigurationType, std::uint32_t, std::uint32_t*, XrViewConfigurationView*) {
  return XR_ERROR_FUNCTION_UNSUPPORTED;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance, const char*, PFN_xrVoidFunction* function) {
  *function = nullptr;
  return XR_ERROR_FUNCTION_UNSUPPORTED;
}
}

int main(int argc, char** argv) {
  if (argc != 3) throw std::runtime_error("Expected opt-in and host-extension profile");
  const bool opt_in = std::string_view(argv[1]) == "1";
  const std::string_view profile = argv[2];
  if (profile == "both" || profile == "passthrough") advertised.push_back("XR_FB_passthrough");
  if (profile == "both" || profile == "mesh") advertised.push_back("XR_FB_triangle_mesh");
  const auto& host = venture::xr::host_xr();
  std::set<std::string> expected{"XR_KHR_vulkan_enable", "XR_KHR_vulkan_enable2", "XR_EXT_hand_tracking"};
  for (const auto* name : {"XR_FB_passthrough", "XR_FB_triangle_mesh"}) {
    const bool available = std::any_of(advertised.begin(), advertised.end(),
                                      [&](const char* entry) { return std::string_view(entry) == name; });
    if (host.available_extensions.contains(name) != available)
      throw std::runtime_error("Host extension availability was changed");
    if (opt_in && available) expected.insert(name);
  }
  if (creates != 1 || requested != expected || host.enabled_extensions != expected)
    throw std::runtime_error("Unexpected extensions passed to host xrCreateInstance");
  std::puts("PASS production host instance requests exactly the permitted advertised extensions");
}
