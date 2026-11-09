#pragma once
#include <cstdint>
#include <vulkan/vulkan.h>

namespace venture::gfx {
// GPU-only buffers and images prefer local memory the CPU cannot map, then
// any local memory (including UMA), then any compatible ordinary memory.
// Private resources do not enable the features required by these special types.
inline std::uint32_t gpu_only_memory_type(const VkPhysicalDeviceMemoryProperties& memory,
                                         std::uint32_t compatible) {
  constexpr VkMemoryPropertyFlags unusable = VK_MEMORY_PROPERTY_PROTECTED_BIT |
                                             VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT |
                                             VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD;
  auto lowest = [&](VkMemoryPropertyFlags required, VkMemoryPropertyFlags excluded) {
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
      const auto flags = memory.memoryTypes[i].propertyFlags;
      if ((compatible & (1u << i)) && (flags & required) == required && !(flags & (excluded | unusable)))
        return i;
    }
    return memory.memoryTypeCount;
  };
  auto type = lowest(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
  if (type == memory.memoryTypeCount) type = lowest(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
  if (type == memory.memoryTypeCount) type = lowest(0, 0);
  return type;
}
// Where an allocation goes when VRAM is full: the lowest allowed type in system memory (a heap
// without DEVICE_LOCAL). Slower to sample, but the driver can place the resource there.
inline std::uint32_t spill_memory_type(const VkPhysicalDeviceMemoryProperties& memory,
                                       std::uint32_t compatible) {
  constexpr VkMemoryPropertyFlags unusable = VK_MEMORY_PROPERTY_PROTECTED_BIT |
                                             VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT |
                                             VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD;
  for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
    const auto& type = memory.memoryTypes[i];
    if ((compatible & (1u << i)) && !(type.propertyFlags & unusable) &&
        !(memory.memoryHeaps[type.heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT))
      return i;
  }
  return memory.memoryTypeCount;
}
// Where a refused allocation in CPU-visible VRAM (a discrete GPU's BAR window, 256 MiB
// without Resizable BAR and shared with every VR app) can go instead: an allowed system-memory
// type with the same host properties (visible, and coherent or cached as the original was).
// The CPU maps it the same way; the GPU reads it over PCIe. memoryTypeCount when none, or when
// the type is not CPU-visible VRAM.
inline std::uint32_t bar_fallback_memory_type(const VkPhysicalDeviceMemoryProperties& memory, std::uint32_t type,
                                              std::uint32_t compatible) {
  if (type >= memory.memoryTypeCount) return memory.memoryTypeCount;
  constexpr VkMemoryPropertyFlags bar = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
  constexpr VkMemoryPropertyFlags unusable = VK_MEMORY_PROPERTY_PROTECTED_BIT |
                                             VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT |
                                             VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD;
  const auto flags = memory.memoryTypes[type].propertyFlags;
  if ((flags & bar) != bar || (memory.memoryHeaps[memory.memoryTypes[type].heapIndex].flags &
                               VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0)
    return memory.memoryTypeCount;
  const VkMemoryPropertyFlags host = flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                              VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
  // The exact host properties first, then cached on top of them.
  for (int pass = 0; pass < 2; ++pass)
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
      const auto f = memory.memoryTypes[i].propertyFlags;
      if (!(compatible & (1u << i)) || (f & unusable) || (f & host) != host ||
          (memory.memoryHeaps[memory.memoryTypes[i].heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT))
        continue;
      if (pass == 0 && (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != (host & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) continue;
      return i;
    }
  return memory.memoryTypeCount;
}
} // namespace venture::gfx
