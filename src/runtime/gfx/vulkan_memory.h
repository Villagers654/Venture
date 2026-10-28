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
} // namespace venture::gfx
