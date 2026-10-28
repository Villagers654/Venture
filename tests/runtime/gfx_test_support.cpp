// Explicit XR dispatch fixture; core, HLE and windows use production implementations.
#include "gfx_test_support.h"

#include <deque>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <map>
#include <mutex>
#include <set>

#include "android/native_window.h"
#include "core/linker.h"
#include "core/process.h"
#include "core/signals.h"
#include "core/thread.h"
#include "fake_cpu.h"
#include "xr/host_xr.h"

namespace {
std::recursive_mutex mutex;
thread_local venture::test::GraphicsCallback last_callback;
std::array<unsigned, 4> xr_calls{};
PFN_vkGetInstanceProcAddr xr_vk_loader = nullptr;
VkInstance xr_vk_instance = nullptr;
XrResult XRAPI_CALL xr_requirements(XrInstance, XrSystemId,
                                    XrGraphicsRequirementsVulkanKHR* out) {
  ++xr_calls[0];
  out->minApiVersionSupported = XR_MAKE_VERSION(1, 1, 0);
  out->maxApiVersionSupported = XR_MAKE_VERSION(1, 4, 0);
  return XR_SUCCESS;
}
XrResult XRAPI_CALL xr_instance(XrInstance,
                                const XrVulkanInstanceCreateInfoKHR* in,
                                VkInstance* out, VkResult* result) {
  ++xr_calls[1];
  xr_vk_loader = in->pfnGetInstanceProcAddr;
  auto ci = *in->vulkanCreateInfo;
  std::vector<const char*> names(
      ci.ppEnabledExtensionNames,
      ci.ppEnabledExtensionNames + ci.enabledExtensionCount);
  const char* extra = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
  bool found = false;
  for (auto name : names) found |= std::strcmp(name, extra) == 0;
  if (!found) names.push_back(extra);
  ci.ppEnabledExtensionNames = names.data();
  ci.enabledExtensionCount = static_cast<std::uint32_t>(names.size());
  *result = reinterpret_cast<PFN_vkCreateInstance>(
      xr_vk_loader(nullptr, "vkCreateInstance"))(&ci, in->vulkanAllocator, out);
  if (*result == VK_SUCCESS) xr_vk_instance = *out;
  return XR_SUCCESS;
}
XrResult XRAPI_CALL xr_physical(XrInstance,
                                const XrVulkanGraphicsDeviceGetInfoKHR* in,
                                VkPhysicalDevice* out) {
  ++xr_calls[2];
  auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
      xr_vk_loader(in->vulkanInstance, "vkEnumeratePhysicalDevices"));
  std::uint32_t count = 0;
  if (enumerate(in->vulkanInstance, &count, nullptr) != VK_SUCCESS || !count)
    return XR_ERROR_RUNTIME_FAILURE;
  std::vector<VkPhysicalDevice> devices(count);
  if (enumerate(in->vulkanInstance, &count, devices.data()) != VK_SUCCESS)
    return XR_ERROR_RUNTIME_FAILURE;
  *out = devices.front();
  return XR_SUCCESS;
}
XrResult XRAPI_CALL xr_device(XrInstance, const XrVulkanDeviceCreateInfoKHR* in,
                              VkDevice* out, VkResult* result) {
  ++xr_calls[3];
  auto ci = *in->vulkanCreateInfo;
  std::vector<const char*> names(
      ci.ppEnabledExtensionNames,
      ci.ppEnabledExtensionNames + ci.enabledExtensionCount);
  const char* extra = VK_KHR_MAINTENANCE_1_EXTENSION_NAME;
  bool found = false;
  for (auto name : names) found |= std::strcmp(name, extra) == 0;
  if (!found) names.push_back(extra);
  ci.ppEnabledExtensionNames = names.data();
  ci.enabledExtensionCount = static_cast<std::uint32_t>(names.size());
  *result = reinterpret_cast<PFN_vkCreateDevice>(
      in->pfnGetInstanceProcAddr(xr_vk_instance, "vkCreateDevice"))(
      in->vulkanPhysicalDevice, &ci, in->vulkanAllocator, out);
  return XR_SUCCESS;
}
XrResult XRAPI_CALL xr_get_proc(XrInstance, const char* name,
                                PFN_xrVoidFunction* out) {
  if (std::strcmp(name, "xrGetVulkanGraphicsRequirements2KHR") == 0)
    *out = reinterpret_cast<PFN_xrVoidFunction>(xr_requirements);
  else if (std::strcmp(name, "xrCreateVulkanInstanceKHR") == 0)
    *out = reinterpret_cast<PFN_xrVoidFunction>(xr_instance);
  else if (std::strcmp(name, "xrGetVulkanGraphicsDevice2KHR") == 0)
    *out = reinterpret_cast<PFN_xrVoidFunction>(xr_physical);
  else if (std::strcmp(name, "xrCreateVulkanDeviceKHR") == 0)
    *out = reinterpret_cast<PFN_xrVoidFunction>(xr_device);
  else {
    *out = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  }
  return XR_SUCCESS;
}
}  // namespace
namespace venture::test {
void configure_graphics_test_xr() {
  if (!std::getenv("VENTURE_GFX_FAKE_XR")) return;
  auto& host = xr::host_xr();
  host.available = true;
  host.instance = reinterpret_cast<XrInstance>(std::uintptr_t{1});
  host.system = 1;
  host.enabled_extensions.insert("XR_KHR_vulkan_enable2");
  host.get_instance_proc_addr = reinterpret_cast<void*>(xr_get_proc);
}
const hle::FunctionInfo* graphics_function(std::string_view name) {
  // Earlier linker tests may have requested a missing-symbol thunk of this
  // name before the graphics library was registered. Use the latest binding.
  const hle::FunctionInfo* found = nullptr;
  for (hle::FunctionId id = 1;; ++id) {
    try { const auto& f = hle::function(id); if (f.name == name) found = &f; }
    catch (const std::exception&) { return found; }
  }
}
GuestAddr graphics_callback() {
  static auto address = hle::make_thunk("gfx-test", "debug callback", [](hle::CallContext& ctx) {
    last_callback.function = ctx.info().thunk;
    last_callback.arguments.clear();
    for (int i = 0; i < 7; ++i) last_callback.arguments.push_back(ctx.x(i));
  });
  return address;
}
GraphicsCallback last_graphics_callback() { return last_callback; }
void dump_graphics_functions(const char* path) {
  std::ofstream out(path);
  for (hle::FunctionId id = 1;; ++id) {
    try { const auto& f = hle::function(id); if (f.library.starts_with("libGL") || f.library == "libEGL.so") out << f.name << '\n'; }
    catch (const std::exception&) { break; }
  }
}
std::array<unsigned, 4> graphics_xr_calls() { return xr_calls; }
}  // namespace venture::test
