// VENTURE_VULKAN_ADDRESS_LOG=<file>|1: an opt-in diagnostic that records every
// GPU virtual-address bind and unbind the driver reports for guest devices
// (VK_EXT_device_address_binding_report), so the address of a GPU page fault
// (VK_EXT_device_fault, the kernel log) can be traced to its object and to
// when that object lost its memory. A path also writes every event there;
// "1" keeps them in memory only. At a device fault each reported address is
// matched against the live ranges and the recently unbound ones, and the log
// says which object holds it, which held it last, or that none ever did (a
// pointer the GPU read from memory rather than one of the device's ranges).
// Off by default; no guest-visible effect.
#include "hle/vulkan/internal.h"
#include "hle/vulkan/vulkan.h"

#include <chrono>
#include <cstdio>
#include <deque>
#include <format>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "common/environment.h"
#include "common/log.h"
#include "gfx/gpu_device.h"

namespace venture::vulkan {
namespace {
struct Range {
  std::uint64_t base{}, size{};
  int type{};
  std::uint64_t handle{};
  unsigned flags{};
  std::int64_t bound_us{}, unbound_us{};
};
std::mutex mutex;
std::FILE* file = nullptr;
std::map<VkInstance, VkDebugUtilsMessengerEXT> messengers;
std::map<std::uint64_t, Range> live;  // by base address
// Recent unbinds, newest last: enough to name a range freed while in flight.
std::deque<Range> unbound;
constexpr std::size_t unbound_limit = 65536;

std::int64_t now_us() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}
// Requires mutex.
void record(bool bind, Range range) {
  if (bind) {
    live[range.base] = range;
    return;
  }
  auto it = live.find(range.base);
  if (it != live.end()) {
    range.bound_us = it->second.bound_us;
    live.erase(it);
  }
  unbound.push_back(range);
  if (unbound.size() > unbound_limit) unbound.pop_front();
}
VkBool32 VKAPI_CALL report(VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT types,
                           const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
  if (!(types & VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT) || !data) return VK_FALSE;
  for (auto* next = static_cast<const VkBaseInStructure*>(data->pNext); next; next = next->pNext) {
    if (next->sType != VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT) continue;
    const auto& binding = *reinterpret_cast<const VkDeviceAddressBindingCallbackDataEXT*>(next);
    const bool bind = binding.bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT;
    const auto now = now_us();
    Range range{binding.baseAddress, binding.size, data->objectCount ? int(data->pObjects[0].objectType) : 0,
                data->objectCount ? data->pObjects[0].objectHandle : 0, unsigned(binding.flags),
                bind ? now : 0, bind ? 0 : now};
    std::lock_guard lock(mutex);
    record(bind, range);
    if (!file) continue;
    std::fprintf(file, "%lld %s type=%d handle=0x%llx base=0x%llx size=0x%llx flags=%u\n",
                 static_cast<long long>(now), bind ? "bind" : "unbind", range.type,
                 static_cast<unsigned long long>(range.handle), static_cast<unsigned long long>(range.base),
                 static_cast<unsigned long long>(range.size), range.flags);
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
bool overlaps(const Range& r, std::uint64_t low, std::uint64_t high) {
  return r.base < high && r.size > 0 && r.base + (r.size - 1) >= low;
}
std::string describe(const Range& r, std::int64_t now) {
  auto text = std::format("type={} handle={:#x} base={:#x} size={:#x}{}", r.type, r.handle, r.base, r.size,
                          (r.flags & VK_DEVICE_ADDRESS_BINDING_INTERNAL_OBJECT_BIT_EXT) ? " internal" : "");
  if (r.bound_us) text += std::format(" bound {:.3f} s ago", double(now - r.bound_us) / 1e6);
  if (r.unbound_us) text += std::format(" unbound {:.3f} s ago", double(now - r.unbound_us) / 1e6);
  return text;
}
}  // namespace

bool address_log_enabled() {
  static const bool enabled = [] {
    const char* path = venture::environment::getenv("VENTURE_VULKAN_ADDRESS_LOG");
    if (!path || !*path) return false;
    if (std::string_view(path) == "1") {
      log::info("vulkan", "GPU address bindings are kept for device fault reports");
      return true;
    }
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
void address_log_note(bool bind, std::uint64_t base, std::uint64_t size, int type, std::uint64_t handle,
                      unsigned flags) {
  const auto now = now_us();
  std::lock_guard lock(mutex);
  record(bind, Range{base, size, type, handle, flags, bind ? now : 0, bind ? 0 : now});
}
void address_log_reset() {
  std::lock_guard lock(mutex);
  live.clear();
  unbound.clear();
}
std::vector<std::string> address_log_matches(std::uint64_t address, std::uint64_t precision) {
  const auto low = precision > 1 ? address & ~(precision - 1) : address;
  const auto high = low + std::max<std::uint64_t>(precision, 1);
  const auto now = now_us();
  std::vector<std::string> lines;
  std::lock_guard lock(mutex);
  if (live.empty() && unbound.empty()) {
    lines.push_back(std::format("address {:#x}: no GPU address bindings were reported (host driver lacks "
                                "VK_EXT_device_address_binding_report?)", address));
    return lines;
  }
  // Live ranges are disjoint: only the last one starting below `high` can reach it.
  auto it = live.lower_bound(high);
  if (it != live.begin() && overlaps(std::prev(it)->second, low, high))
    lines.push_back(std::format("address {:#x} is in live range {}", address, describe(std::prev(it)->second, now)));
  int listed = 0;
  for (auto r = unbound.rbegin(); r != unbound.rend() && listed < 4; ++r) {
    if (!overlaps(*r, low, high)) continue;
    lines.push_back(std::format("address {:#x} was in unbound range {}", address, describe(*r, now)));
    ++listed;
  }
  if (!lines.empty()) return lines;
  lines.push_back(std::format("address {:#x} lies in no range the driver reported bound or unbound since the "
                              "instance was created (a pointer or descriptor the GPU read from memory)", address));
  if (it != live.begin())
    lines.push_back(std::format("  nearest live range below: {}", describe(std::prev(it)->second, now)));
  if (it != live.end()) lines.push_back(std::format("  nearest live range above: {}", describe(it->second, now)));
  return lines;
}
void address_log_describe(std::uint64_t address, std::uint64_t precision) {
  if (!address_log_enabled()) return;
  for (const auto& line : address_log_matches(address, precision)) log::warn("vulkan-fault", "{}", line);
}
}  // namespace venture::vulkan
