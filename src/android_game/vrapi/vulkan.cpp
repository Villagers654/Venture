// SPDX-License-Identifier: GPL-3.0-or-later
// The game's Vulkan device inside libvrapi.so. Vulkan is reached through the
// system loader, opened at run time, so the library still loads where a
// headset has no Vulkan.
//
// The runtime must be told the game's queue as a family and an index, but
// VrApi hands over only the VkQueue (ovrModeParmsVulkan's
// SynchronizationQueue). Vulkan offers no query from a queue to its family, so
// the queue is matched against vkGetDeviceQueue over the device's families,
// graphics families first and index 0 first: the queue games create, which
// matches at the first probe.
#include "android_game/vrapi/vulkan.h"

#include <dlfcn.h>

#include <array>
#include <mutex>
#include <vector>

#include "common/log.h"
#include "xr/host_xr.h"
#include "xr/session.h"

namespace venture::vrapi_native::vulkan {
namespace {
struct Functions {
  PFN_vkGetInstanceProcAddr instance_proc = nullptr;
  PFN_vkGetDeviceProcAddr device_proc = nullptr;
  PFN_vkGetPhysicalDeviceQueueFamilyProperties families = nullptr;
  PFN_vkGetDeviceQueue device_queue = nullptr;
  PFN_vkCreateCommandPool create_pool = nullptr;
  PFN_vkDestroyCommandPool destroy_pool = nullptr;
  PFN_vkAllocateCommandBuffers allocate = nullptr;
  PFN_vkCreateFence create_fence = nullptr;
  PFN_vkDestroyFence destroy_fence = nullptr;
  PFN_vkWaitForFences wait = nullptr;
  PFN_vkResetFences reset_fences = nullptr;
  PFN_vkBeginCommandBuffer begin = nullptr;
  PFN_vkEndCommandBuffer end = nullptr;
  PFN_vkCmdPipelineBarrier pipeline_barrier = nullptr;
  PFN_vkCmdCopyImage copy_image = nullptr;
  PFN_vkCmdBlitImage blit_image = nullptr;
  PFN_vkQueueSubmit queue_submit = nullptr;
};
constexpr std::size_t kSlots = 4;
struct Device {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  Functions f;
  // The session's queue, and the command buffers submitted on it.
  VkQueue queue = VK_NULL_HANDLE;
  std::uint32_t family = 0;
  VkCommandPool pool = VK_NULL_HANDLE;
  std::array<VkCommandBuffer, kSlots> commands{};
  std::array<VkFence, kSlots> fences{};
  std::size_t next = 0;
};
std::mutex mutex;
Device g;  // mutex

template <class T> T instance_function(const Device &d, const char *name) {
  return reinterpret_cast<T>(d.f.instance_proc(d.instance, name));
}
template <class T> T device_function(const Device &d, const char *name) {
  return reinterpret_cast<T>(d.f.device_proc(d.device, name));
}

void release_commands(Device &d) {
  if (!d.pool) return;
  d.f.wait(d.device, std::uint32_t(kSlots), d.fences.data(), VK_TRUE, UINT64_MAX);
  for (auto &fence : d.fences) d.f.destroy_fence(d.device, fence, nullptr), fence = VK_NULL_HANDLE;
  d.f.destroy_pool(d.device, d.pool, nullptr);
  d.pool = VK_NULL_HANDLE;
  d.commands = {};
}

// The family and index of `queue` on the game's device, by matching.
bool find_queue(const Device &d, VkQueue queue, std::uint32_t &family, std::uint32_t &index) {
  std::uint32_t count = 0;
  d.f.families(d.physical, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  d.f.families(d.physical, &count, families.data());
  for (const bool graphics : {true, false})
    for (std::uint32_t f = 0; f < count; ++f) {
      if (bool(families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) != graphics) continue;
      for (std::uint32_t i = 0; i < families[f].queueCount; ++i) {
        VkQueue candidate = VK_NULL_HANDLE;
        d.f.device_queue(d.device, f, i, &candidate);
        if (candidate == queue) {
          family = f;
          index = i;
          return true;
        }
      }
    }
  return false;
}
bool first_graphics_queue(const Device &d, std::uint32_t &family) {
  std::uint32_t count = 0;
  d.f.families(d.physical, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  d.f.families(d.physical, &count, families.data());
  for (std::uint32_t f = 0; f < count; ++f)
    if (families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      family = f;
      return true;
    }
  return false;
}

XrResult make_commands(Device &d) {
  VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                             VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, d.family};
  if (d.f.create_pool(d.device, &pi, nullptr, &d.pool) != VK_SUCCESS) return XR_ERROR_RUNTIME_FAILURE;
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, d.pool,
                                 VK_COMMAND_BUFFER_LEVEL_PRIMARY, std::uint32_t(kSlots)};
  if (d.f.allocate(d.device, &ai, d.commands.data()) != VK_SUCCESS) return XR_ERROR_RUNTIME_FAILURE;
  for (auto &fence : d.fences) {
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
    if (d.f.create_fence(d.device, &fi, nullptr, &fence) != VK_SUCCESS) return XR_ERROR_RUNTIME_FAILURE;
  }
  return XR_SUCCESS;
}
}  // namespace

std::int32_t bind_system(const hle::vrapi::abi::SystemVulkan &info) {
  std::lock_guard lock(mutex);
  if (g.device) return hle::vrapi::abi::invalid_operation;
  auto &h = xr::host_xr();
  if (!h.available || !h.enabled_extensions.contains("XR_KHR_vulkan_enable")) return hle::vrapi::abi::unsupported;
  static void *loader = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  if (!loader) return hle::vrapi::abi::unsupported;
  Device d;
  d.instance = reinterpret_cast<VkInstance>(info.instance);
  d.physical = reinterpret_cast<VkPhysicalDevice>(info.physical);
  d.device = reinterpret_cast<VkDevice>(info.device);
  d.f.instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr"));
  if (!d.f.instance_proc) return hle::vrapi::abi::unsupported;
  d.f.device_proc = instance_function<PFN_vkGetDeviceProcAddr>(d, "vkGetDeviceProcAddr");
  d.f.families = instance_function<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
      d, "vkGetPhysicalDeviceQueueFamilyProperties");
  if (!d.f.device_proc || !d.f.families) return hle::vrapi::abi::unsupported;
  d.f.device_queue = device_function<PFN_vkGetDeviceQueue>(d, "vkGetDeviceQueue");
  d.f.create_pool = device_function<PFN_vkCreateCommandPool>(d, "vkCreateCommandPool");
  d.f.destroy_pool = device_function<PFN_vkDestroyCommandPool>(d, "vkDestroyCommandPool");
  d.f.allocate = device_function<PFN_vkAllocateCommandBuffers>(d, "vkAllocateCommandBuffers");
  d.f.create_fence = device_function<PFN_vkCreateFence>(d, "vkCreateFence");
  d.f.destroy_fence = device_function<PFN_vkDestroyFence>(d, "vkDestroyFence");
  d.f.wait = device_function<PFN_vkWaitForFences>(d, "vkWaitForFences");
  d.f.reset_fences = device_function<PFN_vkResetFences>(d, "vkResetFences");
  d.f.begin = device_function<PFN_vkBeginCommandBuffer>(d, "vkBeginCommandBuffer");
  d.f.end = device_function<PFN_vkEndCommandBuffer>(d, "vkEndCommandBuffer");
  d.f.pipeline_barrier = device_function<PFN_vkCmdPipelineBarrier>(d, "vkCmdPipelineBarrier");
  d.f.copy_image = device_function<PFN_vkCmdCopyImage>(d, "vkCmdCopyImage");
  d.f.blit_image = device_function<PFN_vkCmdBlitImage>(d, "vkCmdBlitImage");
  d.f.queue_submit = device_function<PFN_vkQueueSubmit>(d, "vkQueueSubmit");
  if (!d.f.device_queue || !d.f.create_pool || !d.f.destroy_pool || !d.f.allocate || !d.f.create_fence ||
      !d.f.destroy_fence || !d.f.wait || !d.f.reset_fences || !d.f.begin || !d.f.end || !d.f.pipeline_barrier ||
      !d.f.copy_image || !d.f.blit_image || !d.f.queue_submit)
    return hle::vrapi::abi::unsupported;
  // OpenXR requires both queries before a Vulkan session; the runtime names
  // the physical device it composites on.
  XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
  auto r = xr::host_call<PFN_xrGetVulkanGraphicsRequirementsKHR, "xrGetVulkanGraphicsRequirementsKHR">(
      h.instance, h.system, &requirements);
  VkPhysicalDevice wanted = VK_NULL_HANDLE;
  if (XR_SUCCEEDED(r))
    r = xr::host_call<PFN_xrGetVulkanGraphicsDeviceKHR, "xrGetVulkanGraphicsDeviceKHR">(h.instance, h.system,
                                                                                        d.instance, &wanted);
  if (XR_FAILED(r) || wanted != d.physical) {
    log::warn("vrapi", "The runtime cannot composite from the game's Vulkan device (result={})", int(r));
    return hle::vrapi::abi::invalid;
  }
  g = d;
  log::info("vrapi", "Bound the game's Vulkan device");
  return hle::vrapi::abi::success;
}
void unbind_system() {
  std::lock_guard lock(mutex);
  release_commands(g);
  g = {};
}
bool bound() {
  std::lock_guard lock(mutex);
  return g.device != VK_NULL_HANDLE;
}

XrResult create_session(VkQueue queue) {
  std::lock_guard lock(mutex);
  if (!g.device) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  std::uint32_t family = 0, index = 0;
  if (queue) {
    if (!find_queue(g, queue, family, index)) {
      log::warn("vrapi", "The game's synchronization queue is not one of its device's queues");
      return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
  } else if (!first_graphics_queue(g, family)) {
    return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  }
  XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR, nullptr, g.instance, g.physical, g.device,
                                     family, index};
  XrResult r;
  {
    std::lock_guard sl(xr::session().mutex);
    r = xr::session().create_native(xr::SessionOwner::vrapi, &binding);
  }
  if (XR_FAILED(r)) return r;
  release_commands(g);
  g.family = family;
  if (!queue) g.f.device_queue(g.device, family, index, &queue);
  g.queue = queue;
  r = make_commands(g);
  if (XR_FAILED(r)) log::warn("vrapi", "Command buffers on the game's queue failed");
  log::info("vrapi", "Vulkan session on queue family {} index {}", family, index);
  return r;
}
bool session_queue(VkQueue queue) {
  std::lock_guard lock(mutex);
  return g.queue == queue;
}

XrResult submit(const std::function<void(VkCommandBuffer)> &record) {
  std::lock_guard lock(mutex);
  if (!g.pool) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  const auto slot = g.next++ % kSlots;
  auto fence = g.fences[slot];
  auto command = g.commands[slot];
  g.f.wait(g.device, 1, &fence, VK_TRUE, UINT64_MAX);
  g.f.reset_fences(g.device, 1, &fence);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                              VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
  g.f.begin(command, &bi);  // the pool resets command buffers on begin
  record(command);
  g.f.end(command);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &command;
  return g.f.queue_submit(g.queue, 1, &si, fence) == VK_SUCCESS ? XR_SUCCESS : XR_ERROR_RUNTIME_FAILURE;
}

void wait_idle() {
  std::lock_guard lock(mutex);
  if (g.pool) g.f.wait(g.device, std::uint32_t(kSlots), g.fences.data(), VK_TRUE, UINT64_MAX);
}

void barrier(VkCommandBuffer command, VkImage image, std::uint32_t layers, VkImageLayout from, VkImageLayout to) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                    VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, layers};
  g.f.pipeline_barrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &b);
}
void copy(VkCommandBuffer command, VkImage from, VkImage to, std::uint32_t width, std::uint32_t height,
          std::uint32_t layers, bool flip) {
  const VkImageSubresourceLayers all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
  if (!flip) {
    VkImageCopy region{all, {}, all, {}, {width, height, 1}};
    g.f.copy_image(command, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &region);
    return;
  }
  VkImageBlit region{};
  region.srcSubresource = region.dstSubresource = all;
  region.srcOffsets[1] = {std::int32_t(width), std::int32_t(height), 1};
  region.dstOffsets[0] = {0, std::int32_t(height), 0};
  region.dstOffsets[1] = {std::int32_t(width), 0, 1};
  g.f.blit_image(command, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                 &region, VK_FILTER_NEAREST);
}
}  // namespace venture::vrapi_native::vulkan
