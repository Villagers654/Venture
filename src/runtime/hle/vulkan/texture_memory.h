#pragma once
#include <memory>
#include <vulkan/vulkan.h>

namespace venture::vulkan {
// Owns a private decoded-image suballocation, retired after preceding GPU work.
VkResult bind_decoded_image(VkDevice device, VkImage image, std::shared_ptr<void>& allocation);
void release_texture_allocator(VkDevice device);
}
