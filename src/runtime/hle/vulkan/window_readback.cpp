#include "window_readback.h"
#include "internal.h"
#include "android/native_window.h"
#include <cstring>
#include <vector>

namespace venture::vulkan {
bool window_readback_format(VkFormat format) {
  return format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB ||
         format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
}
namespace {
struct Readback {
  VkDevice device;
  VkCommandPool pool{};
  VkBuffer buffer{};
  VkDeviceMemory memory{};
  VkFence fence{};
  void* mapped = nullptr;
  template<class F> F vk(const char* name) { return reinterpret_cast<F>(raw_device_proc(device, name)); }
  ~Readback() {
    if (mapped) vk<PFN_vkUnmapMemory>("vkUnmapMemory")(device, memory);
    if (fence) vk<PFN_vkDestroyFence>("vkDestroyFence")(device, fence, nullptr);
    if (pool) vk<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device, pool, nullptr);
    if (buffer) vk<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device, buffer, nullptr);
    if (memory) vk<PFN_vkFreeMemory>("vkFreeMemory")(device, memory, nullptr);
  }
};
}
VkResult publish_window_image(VkDevice device, VkQueue queue, std::uint32_t family,
    VkImage image, VkFormat format, VkExtent2D extent, android::NativeWindow* window,
    std::span<const VkSemaphore> waits) {
  if (!window_readback_format(format)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
  auto size = std::uint64_t(extent.width) * extent.height * 4;
  if (!size || size > INT32_MAX) return VK_ERROR_OUT_OF_HOST_MEMORY;
  // Allocate host bookkeeping before submitting; allocation failure must not
  // abandon an in-flight command using stack-owned Vulkan resources.
  std::vector<VkPipelineStageFlags> stages(waits.size(), VK_PIPELINE_STAGE_TRANSFER_BIT);
  std::vector<std::uint8_t> rgba(size);
  Readback readback{device};
  auto physical = device_physical(device);
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  auto result = readback.vk<PFN_vkCreateBuffer>("vkCreateBuffer")(device, &bi, nullptr, &readback.buffer);
  if (result != VK_SUCCESS) return result;
  VkMemoryRequirements requirements{};
  readback.vk<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device, readback.buffer, &requirements);
  VkPhysicalDeviceMemoryProperties properties{};
  reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(raw_physical_proc(physical, "vkGetPhysicalDeviceMemoryProperties"))(
      physical, &properties);
  std::uint32_t type = properties.memoryTypeCount;
  for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i)
    if ((requirements.memoryTypeBits & (1u << i)) &&
        (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
      type = i;
      if (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) break;
    }
  if (type == properties.memoryTypeCount) return VK_ERROR_OUT_OF_HOST_MEMORY;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, type};
  result = readback.vk<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &ai, nullptr, &readback.memory);
  if (result != VK_SUCCESS) return result;
  result = readback.vk<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device, readback.buffer, readback.memory, 0);
  if (result != VK_SUCCESS) return result;
  VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.queueFamilyIndex = family;
  result = readback.vk<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device, &pi, nullptr, &readback.pool);
  if (result != VK_SUCCESS) return result;
  VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, readback.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
  VkCommandBuffer command{};
  result = readback.vk<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device, &ci, &command);
  if (result != VK_SUCCESS) return result;
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  result = readback.vk<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command, &begin);
  if (result != VK_SUCCESS) return result;
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  auto barriers = readback.vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
  barriers(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {extent.width, extent.height, 1};
  readback.vk<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; barrier.dstAccessMask = 0;
  barriers(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; host.buffer = readback.buffer; host.size = VK_WHOLE_SIZE;
  barriers(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
  result = readback.vk<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command);
  if (result != VK_SUCCESS) return result;
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  result = readback.vk<PFN_vkCreateFence>("vkCreateFence")(device, &fi, nullptr, &readback.fence);
  if (result != VK_SUCCESS) return result;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.waitSemaphoreCount = static_cast<std::uint32_t>(waits.size()); submit.pWaitSemaphores = waits.data(); submit.pWaitDstStageMask = stages.data();
  submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
  result = readback.vk<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &submit, readback.fence);
  if (result != VK_SUCCESS) return result;
  sync_fd_submitted(queue, 1, &submit);  // the guest's present waits, executed here
  result = readback.vk<PFN_vkWaitForFences>("vkWaitForFences")(device, 1, &readback.fence, VK_TRUE, UINT64_MAX);
  if (result != VK_SUCCESS) return result;
  result = readback.vk<PFN_vkMapMemory>("vkMapMemory")(device, readback.memory, 0, VK_WHOLE_SIZE, 0, &readback.mapped);
  if (result != VK_SUCCESS) return result;
  if (!(properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, readback.memory, 0, VK_WHOLE_SIZE};
    result = readback.vk<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges")(device, 1, &range);
    if (result != VK_SUCCESS) return result;
  }
  std::memcpy(rgba.data(), readback.mapped, size);
  if (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB)
    for (std::size_t i = 0; i < size; i += 4) std::swap(rgba[i], rgba[i+2]);
  return android::publish_window_rgba(window, rgba, extent.width, extent.height) == 0
      ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
}
