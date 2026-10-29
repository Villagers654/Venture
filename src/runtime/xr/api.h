#pragma once

#include <openxr/openxr.h>
#include <vulkan/vulkan.h>
#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#include <openxr/openxr_platform.h>
#include "xr/host_xr.h"
#include <mutex>

namespace venture::xr {
// The name identifies a compile-time slot, never a per-call string lookup.
template <std::size_t N> struct HostProcedureName {
  char value[N];
  constexpr HostProcedureName(const char (&name)[N]) {
    for (std::size_t i = 0; i < N; ++i) value[i] = name[i];
  }
};
template <class F, HostProcedureName Name> F host_proc() {
  auto& h = host_xr();
  struct Entry {
    std::uint64_t generation = 0;
    XrInstance instance = nullptr;
    void* loader = nullptr;
    F function = nullptr;
  };
  static std::mutex mutex;
  static Entry shared;
  thread_local Entry local;
  const auto generation = host_procedure_generation();
  const auto matches = [&](const Entry& entry) {
    return entry.generation == generation && entry.instance == h.instance &&
           entry.loader == h.get_instance_proc_addr;
  };
  if (matches(local)) return local.function;
  std::lock_guard lock(mutex);
  if (!matches(shared)) {
    PFN_xrVoidFunction p = nullptr;
    auto gipa = reinterpret_cast<PFN_xrGetInstanceProcAddr>(h.get_instance_proc_addr);
    const auto result = gipa ? gipa(h.instance, Name.value, &p) : XR_ERROR_FUNCTION_UNSUPPORTED;
    // A missing entry point is stable for this instance. A transient lookup
    // failure must retain the old forwarding behavior: try again next call.
    if (XR_FAILED(result) && result != XR_ERROR_FUNCTION_UNSUPPORTED)
      return reinterpret_cast<F>(p);
    shared = {generation, h.instance, h.get_instance_proc_addr, reinterpret_cast<F>(p)};
  }
  local = shared;
  return local.function;
}
template <class F, HostProcedureName Name, class... A> XrResult host_call(A... args) {
  auto f = host_proc<F, Name>();
  return f ? f(args...) : XR_ERROR_FUNCTION_UNSUPPORTED;
}
// The host's GPU for a VkInstance the application created itself. That is
// XR_KHR_vulkan_enable's contract; enable2 accepts only instances made by
// xrCreateVulkanInstanceKHR, which the Oculus runtime enforces.
inline XrResult host_graphics_device(XrSystemId system, VkInstance instance, VkPhysicalDevice* out) {
  auto& h = host_xr();
  if (h.enabled_extensions.contains("XR_KHR_vulkan_enable")) {
    // XR_KHR_vulkan_enable requires its own requirements call first (the guest's
    // may have reached the host as the enable2 one); the Oculus runtime enforces it.
    XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    host_call<PFN_xrGetVulkanGraphicsRequirementsKHR, "xrGetVulkanGraphicsRequirementsKHR">(
        h.instance, system, &requirements);
    if (XR_SUCCEEDED((host_call<PFN_xrGetVulkanGraphicsDeviceKHR, "xrGetVulkanGraphicsDeviceKHR">(
            h.instance, system, instance, out))))
      return XR_SUCCESS;
  }
  XrVulkanGraphicsDeviceGetInfoKHR info{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR, nullptr, system, instance};
  return host_call<PFN_xrGetVulkanGraphicsDevice2KHR, "xrGetVulkanGraphicsDevice2KHR">(h.instance, &info, out);
}
} // namespace venture::xr
