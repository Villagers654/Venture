#pragma once
#include <cstdint>
#include <memory>
#include <vulkan/vulkan.h>

namespace venture::vulkan {
// Owns a private decoded-image suballocation, retired after preceding GPU work.
VkResult bind_decoded_image(VkDevice device, VkImage image, std::shared_ptr<void>& allocation);
void release_texture_allocator(VkDevice device);
// Whether any texture allocation was refused after relief (create_image then
// stores new textures compressed where it can).
bool texture_memory_refused();
// Tests: refuse the next `count` texture pool allocations as a full heap would.
void fail_texture_allocations(int count);
// A private buffer's memory, suballocated from pooled blocks of one memory type:
// a texture's source copy or scratch no longer costs the driver an allocation.
struct PrivateBufferMemory {
  std::shared_ptr<void> allocation;  // frees the range when released
  VkDeviceMemory memory{};
  VkDeviceSize offset{};
  void* mapped{};  // persistent mapping of the range, when requested
};
VkResult bind_private_buffer(VkDevice device, VkBuffer buffer, std::uint32_t memory_type, bool mapped,
                             PrivateBufferMemory& out);
VkResult map_private_buffer(const std::shared_ptr<void>& allocation, void** out);
void unmap_private_buffer(const std::shared_ptr<void>& allocation);
VkResult flush_private_buffer(const std::shared_ptr<void>& allocation);
}
