#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
#include "texture_memory.h"
#include "internal.h"
#include "core/alloc_failure.h"
#include "gfx/gpu_device.h"
#include "gfx/vulkan_memory.h"
#include <algorithm>
#include <atomic>
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
std::atomic<bool> refused{false};
std::atomic<int> failing{0};
// Each VMA block is one vkAllocateMemory against maxMemoryAllocationCount.
void VKAPI_CALL block_allocated(VmaAllocator, std::uint32_t, VkDeviceMemory, VkDeviceSize, void*) {
  note_device_memory(1);
}
void VKAPI_CALL block_freed(VmaAllocator, std::uint32_t, VkDeviceMemory, VkDeviceSize, void*) {
  note_device_memory(-1);
}
// One pool allocation, retried once after Venture's spare memory is given back.
// A final refusal is reported with the memory type the request preferred.
template <class F>
VkResult allocate_with_relief(VkDevice device, std::string_view what, const VkMemoryRequirements& requirements,
                              std::uint32_t memory_type, F allocate) {
  const auto attempt = [&] {
    if (failing.load(std::memory_order_relaxed) > 0 && failing.fetch_sub(1) > 0) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    return allocate();
  };
  auto result = attempt();
  if (!out_of_memory_result(result)) return result;
  alloc_failure::relieve();
  result = attempt();
  if (!out_of_memory_result(result)) return result;
  refused.store(true, std::memory_order_relaxed);
  if (memory_type == ~0u) {
    VkPhysicalDeviceMemoryProperties memory{};
    const auto physical = device_physical(device);
    reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        raw_physical_proc(physical, "vkGetPhysicalDeviceMemoryProperties"))(physical, &memory);
    memory_type = gfx::gpu_only_memory_type(memory, requirements.memoryTypeBits);
  }
  report_gpu_memory_failure(device, what, requirements.size, memory_type);
  return result;
}

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
  VmaDeviceMemoryCallbacks callbacks{block_allocated, block_freed, nullptr};
  info.pDeviceMemoryCallbacks = &callbacks;
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
  VkMemoryRequirements requirements{};
  reinterpret_cast<PFN_vkGetImageMemoryRequirements>(raw_device_proc(device, "vkGetImageMemoryRequirements"))(
      device, image, &requirements);
  // VMA itself tries every compatible type, system memory included, in turn.
  result = allocate_with_relief(device, "decoded texture image", requirements, ~0u, [&] {
    return vmaAllocateMemoryForImage(allocation->owner->handle, image, &info, &allocation->handle, nullptr);
  });
  if (result != VK_SUCCESS) return result;
  result = vmaBindImageMemory(allocation->owner->handle, allocation->handle, image);
  if (result == VK_SUCCESS) out = std::move(allocation);
  return result;
}
namespace {
struct BufferAllocation {
  std::shared_ptr<Allocator> owner;
  VmaAllocation handle{};
  // Private buffers are released by their owner once no recorded command can use them.
  ~BufferAllocation() { if (handle) vmaFreeMemory(owner->handle, handle); }
};
}

VkResult bind_private_buffer(VkDevice device, VkBuffer buffer, std::uint32_t memory_type, bool mapped,
                             PrivateBufferMemory& out) {
  auto allocation = std::make_shared<BufferAllocation>();
  auto result = allocator_for(device, allocation->owner);
  if (result != VK_SUCCESS) return result;
  VmaAllocationCreateInfo info{};
  // The caller chose the type (device-local, spill or a mappable cached type).
  info.memoryTypeBits = 1u << memory_type;
  if (mapped) info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VmaAllocationInfo placed{};
  VkMemoryRequirements requirements{};
  reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(raw_device_proc(device, "vkGetBufferMemoryRequirements"))(
      device, buffer, &requirements);
  result = allocate_with_relief(device, mapped ? "texture staging buffer" : "texture scratch buffer", requirements,
                                memory_type, [&] {
    return vmaAllocateMemoryForBuffer(allocation->owner->handle, buffer, &info, &allocation->handle, &placed);
  });
  if (result != VK_SUCCESS) return result;
  if (placed.memoryType != memory_type) return VK_ERROR_OUT_OF_DEVICE_MEMORY;  // the allocation is freed with `allocation`
  result = vmaBindBufferMemory(allocation->owner->handle, allocation->handle, buffer);
  if (result != VK_SUCCESS) return result;
  if (mapped && !placed.pMappedData) return VK_ERROR_MEMORY_MAP_FAILED;
  out.memory = placed.deviceMemory;
  out.offset = placed.offset;
  out.mapped = placed.pMappedData;
  out.allocation = std::move(allocation);
  return VK_SUCCESS;
}
VkResult map_private_buffer(const std::shared_ptr<void>& allocation, void** out) {
  auto& a = *static_cast<BufferAllocation*>(allocation.get());
  return vmaMapMemory(a.owner->handle, a.handle, out);
}
void unmap_private_buffer(const std::shared_ptr<void>& allocation) {
  auto& a = *static_cast<BufferAllocation*>(allocation.get());
  vmaUnmapMemory(a.owner->handle, a.handle);
}
VkResult flush_private_buffer(const std::shared_ptr<void>& allocation) {
  auto& a = *static_cast<BufferAllocation*>(allocation.get());
  return vmaFlushAllocation(a.owner->handle, a.handle, 0, VK_WHOLE_SIZE);
}
bool texture_memory_refused() { return refused.load(std::memory_order_relaxed); }
void fail_texture_allocations(int count) {
  failing.store(count);
  if (count < 0) refused.store(false);
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
