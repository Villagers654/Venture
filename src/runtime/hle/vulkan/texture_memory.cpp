#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
#include "texture_memory.h"
#include "internal.h"
#include "gfx/gpu_device.h"
#include <algorithm>
#include <map>
#include <mutex>

namespace venture::vulkan {
namespace {
struct Allocator {
  VkDevice device{};
  VmaAllocator handle{};
  ~Allocator() { if (handle) vmaDestroyAllocator(handle); }
};
std::mutex allocator_mutex;
std::map<VkDevice, std::shared_ptr<Allocator>> allocators;

VkResult allocator_for(VkDevice device, std::shared_ptr<Allocator>& out) {
  std::lock_guard lock(allocator_mutex);
  if (auto it = allocators.find(device); it != allocators.end()) {
    out = it->second;
    return VK_SUCCESS;
  }
  auto owner = std::make_shared<Allocator>();
  owner->device = device;
  VmaVulkanFunctions functions{};
  functions.vkGetInstanceProcAddr = gfx::gpu_device().get_instance_proc_addr;
  functions.vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
      functions.vkGetInstanceProcAddr(device_instance(device), "vkGetDeviceProcAddr"));
  VmaAllocatorCreateInfo info{};
  info.instance = device_instance(device);
  info.physicalDevice = device_physical(device);
  info.device = device;
  info.preferredLargeHeapBlockSize = 32 * 1024 * 1024;
  info.vulkanApiVersion = std::min(device_api_version(device), VK_API_VERSION_1_1);
  info.pVulkanFunctions = &functions;
  auto result = vmaCreateAllocator(&info, &owner->handle);
  if (result != VK_SUCCESS) return result;
  allocators.emplace(device, owner);
  out = std::move(owner);
  return VK_SUCCESS;
}
struct Allocation {
  std::shared_ptr<Allocator> owner;
  VmaAllocation handle{};
  ~Allocation() {
    if (handle) defer_gpu_release(owner->device, [keep = owner, memory = handle] {
      vmaFreeMemory(keep->handle, memory);
    });
  }
};
}

VkResult bind_decoded_image(VkDevice device, VkImage image, std::shared_ptr<void>& out) {
  auto allocation = std::make_shared<Allocation>();
  auto result = allocator_for(device, allocation->owner);
  if (result != VK_SUCCESS) return result;
  VmaAllocationCreateInfo info{};
  // AllocateMemoryForImage has no image-create usage metadata for AUTO; this
  // storage is exclusively GPU-read/written. VMA pools compatible memory and
  // honors the driver's alignment and dedicated-allocation requirements.
  info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
  result = vmaAllocateMemoryForImage(allocation->owner->handle, image, &info,
                                    &allocation->handle, nullptr);
  if (result != VK_SUCCESS) return result;
  result = vmaBindImageMemory(allocation->owner->handle, allocation->handle, image);
  if (result == VK_SUCCESS) out = std::move(allocation);
  return result;
}
void release_texture_allocator(VkDevice device) {
  std::shared_ptr<Allocator> removed;
  {
    std::lock_guard lock(allocator_mutex);
    auto node = allocators.extract(device);
    if (!node.empty()) removed = std::move(node.mapped());
  }
  // Pending GPU retirement closures retain their allocator until their final
  // range is freed. No Vulkan allocation/free runs under texture_lock.
}
}
