#pragma once
#include "xr/host_xr.h"
#include <openxr/openxr.h>
#include <cstring>
#include <sstream>
#include <utility>
#include <vector>

namespace venture::xr {
// Legacy Vulkan enable returns native-driver requirements. These are not an
// Android capability list: WSI and external-handle extensions belong to the
// host even when the Vulkan instance/device is owned by a guest application.
inline XrResult vulkan_extension_requirements(XrInstance instance, XrSystemId system,
                                            bool device, std::vector<std::string>& names) {
  names.clear();
  auto resolve = reinterpret_cast<PFN_xrGetInstanceProcAddr>(host_xr().get_instance_proc_addr);
  if (!resolve) return XR_ERROR_RUNTIME_UNAVAILABLE;
  PFN_xrVoidFunction address{};
  auto result = resolve(instance, device ? "xrGetVulkanDeviceExtensionsKHR"
                                         : "xrGetVulkanInstanceExtensionsKHR", &address);
  if (XR_FAILED(result)) return result;
  if (!address) return XR_ERROR_FUNCTION_UNSUPPORTED;
  using Query = XrResult (XRAPI_PTR *)(XrInstance, XrSystemId, std::uint32_t, std::uint32_t*, char*);
  auto query = reinterpret_cast<Query>(address);
  std::uint32_t count = 0;
  result = query(instance, system, 0, &count, nullptr);
  if (XR_FAILED(result) || !count) return result;
  std::string text(count, '\0');
  result = query(instance, system, count, &count, text.data());
  if (XR_FAILED(result)) return result;
  if (!count || count > text.size() || !std::memchr(text.data(), '\0', count))
    return XR_ERROR_RUNTIME_FAILURE;
  std::istringstream stream(text.c_str());
  for (std::string name; stream >> name;) names.push_back(std::move(name));
  return result;
}

template<class Translatable>
XrResult guest_vulkan_extensions(XrInstance instance, XrSystemId system, bool device,
                                std::uint32_t capacity, std::uint32_t* count, char* out,
                                Translatable translatable) {
  if (!count || (capacity && !out)) return XR_ERROR_VALIDATION_FAILURE;
  std::vector<std::string> names;
  auto result = vulkan_extension_requirements(instance, system, device, names);
  if (XR_FAILED(result)) return result;
  std::string guest;
  for (const auto& name : names) {
    if (!translatable(name)) continue;
    if (!guest.empty()) guest += ' ';
    guest += name;
  }
  *count = static_cast<std::uint32_t>(guest.size() + 1);
  if (!capacity) return XR_SUCCESS;
  if (capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT;
  std::memcpy(out, guest.c_str(), *count);
  return XR_SUCCESS;
}
} // namespace venture::xr
