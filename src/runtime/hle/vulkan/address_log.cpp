// VENTURE_VULKAN_ADDRESS_LOG=<file>: an opt-in diagnostic that records every
// GPU virtual-address bind and unbind the driver reports for guest devices
// (VK_EXT_device_address_binding_report), so the address of a GPU page fault
// (VK_EXT_device_fault, the kernel log) can be traced to its object and to
// when that object lost its memory. Off by default; no guest-visible effect.
#include "hle/vulkan/internal.h"
#include "hle/vulkan/vulkan.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>

#include "common/environment.h"
#include "common/log.h"
#include "gfx/gpu_device.h"

namespace venture::vulkan {
namespace {
std::mutex mutex;
std::FILE* file = nullptr;
std::map<VkInstance, VkDebugUtilsMessengerEXT> messengers;

VkBool32 VKAPI_CALL report(VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT types,
                           const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
  if (!(types & VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT) || !data) return VK_FALSE;
  for (auto* next = static_cast<const VkBaseInStructure*>(data->pNext); next; next = next->pNext) {
    if (next->sType != VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT) continue;
    const auto& binding = *reinterpret_cast<const VkDeviceAddressBindingCallbackDataEXT*>(next);
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    std::lock_guard lock(mutex);
    if (!file) return VK_FALSE;
    std::fprintf(file, "%lld %s type=%d handle=0x%llx base=0x%llx size=0x%llx flags=%u\n",
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(now).count()),
                 binding.bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT ? "bind" : "unbind",
                 data->objectCount ? int(data->pObjects[0].objectType) : 0,
                 static_cast<unsigned long long>(data->objectCount ? data->pObjects[0].objectHandle : 0),
                 static_cast<unsigned long long>(binding.baseAddress), static_cast<unsigned long long>(binding.size),
                 unsigned(binding.flags));
    // Kept current: the process may not survive the fault being investigated.
    std::fflush(file);
  }
  return VK_FALSE;
}
bool supported(const std::vector<VkExtensionProperties>& host, const char* name) {
  for (const auto& e : host)
    if (std::string_view(e.extensionName) == name) return true;
  return false;
}
}  // namespace

bool address_log_enabled() {
  static const bool enabled = [] {
    const char* path = venture::environment::getenv("VENTURE_VULKAN_ADDRESS_LOG");
    if (!path || !*path) return false;
    file = std::fopen(path, "a");
    if (file) log::info("vulkan", "GPU address bindings are logged to {}", path);
    return file != nullptr;
  }();
  return enabled;
}
void address_log_instance_extensions(std::vector<const char*>& names, const std::vector<VkExtensionProperties>& host) {
  if (!address_log_enabled() || !supported(host, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) return;
  for (auto* n : names)
    if (std::string_view(n) == VK_EXT_DEBUG_UTILS_EXTENSION_NAME) return;
  names.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
}
void address_log_attach(VkInstance instance) {
  if (!address_log_enabled()) return;
  auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
      gfx::gpu_device().get_instance_proc_addr(instance, "vkCreateDebugUtilsMessengerEXT"));
  if (!create) return;
  VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
  info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT;
  info.pfnUserCallback = report;
  VkDebugUtilsMessengerEXT messenger{};
  if (create(instance, &info, nullptr, &messenger) != VK_SUCCESS) return;
  std::lock_guard lock(mutex);
  messengers[instance] = messenger;
}
void address_log_detach(VkInstance instance) {
  VkDebugUtilsMessengerEXT messenger{};
  {
    std::lock_guard lock(mutex);
    auto it = messengers.find(instance);
    if (it == messengers.end()) return;
    messenger = it->second;
    messengers.erase(it);
  }
  if (auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
          gfx::gpu_device().get_instance_proc_addr(instance, "vkDestroyDebugUtilsMessengerEXT")))
    destroy(instance, messenger, nullptr);
}
bool address_log_device(const std::vector<VkExtensionProperties>& host) {
  return address_log_enabled() && supported(host, VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME);
}
}  // namespace venture::vulkan
