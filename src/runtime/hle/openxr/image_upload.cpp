#include "hle/openxr/image_upload.h"
#include "common/log.h"
#include "gfx/gpu_device.h"
#include "xr/queue.h"

namespace venture::hle::openxr {
template<class F> F ImageUpload::vk(const char* name) const {
  return reinterpret_cast<F>(gfx::gpu_device().get_instance_proc_addr(binding_.instance, name));
}
XrResult ImageUpload::checked(VkResult result, const char* operation) const {
  if (result == VK_SUCCESS) return XR_SUCCESS;
  log::error("xr-upload", "{} failed: VkResult={}", operation, int(result));
  return result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY
      ? XR_ERROR_OUT_OF_MEMORY : XR_ERROR_RUNTIME_FAILURE;
}
ImageUpload::~ImageUpload() {
  if (!binding_.device) return;
  if (mapped_) vk<PFN_vkUnmapMemory>("vkUnmapMemory")(binding_.device, memory_);
  if (staging_) vk<PFN_vkDestroyBuffer>("vkDestroyBuffer")(binding_.device, staging_, nullptr);
  if (memory_) vk<PFN_vkFreeMemory>("vkFreeMemory")(binding_.device, memory_, nullptr);
  if (fence_) vk<PFN_vkDestroyFence>("vkDestroyFence")(binding_.device, fence_, nullptr);
  if (pool_) vk<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(binding_.device, pool_, nullptr);
}
XrResult ImageUpload::initialize(std::uint32_t width, std::uint32_t height) {
  binding_ = state().graphics;
  vk<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(binding_.device, binding_.queueFamilyIndex, binding_.queueIndex, &queue_);
  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = binding_.queueFamilyIndex;
  auto result = checked(vk<PFN_vkCreateCommandPool>("vkCreateCommandPool")(binding_.device, &pci, nullptr, &pool_),
                        "vkCreateCommandPool");
  if (XR_FAILED(result)) return result;
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool_,
                                  VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
  result = checked(vk<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(binding_.device, &cai, &command_),
                   "vkAllocateCommandBuffers");
  if (XR_FAILED(result)) return result;
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  result = checked(vk<PFN_vkCreateFence>("vkCreateFence")(binding_.device, &fci, nullptr, &fence_), "vkCreateFence");
  if (XR_FAILED(result)) return result;
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = std::uint64_t(width) * height * 4;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  result = checked(vk<PFN_vkCreateBuffer>("vkCreateBuffer")(binding_.device, &bci, nullptr, &staging_), "vkCreateBuffer");
  if (XR_FAILED(result)) return result;
  VkMemoryRequirements requirements{};
  vk<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(binding_.device, staging_, &requirements);
  VkPhysicalDeviceMemoryProperties properties{};
  vk<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(binding_.physicalDevice, &properties);
  std::uint32_t type = properties.memoryTypeCount;
  for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
    auto flags = properties.memoryTypes[i].propertyFlags;
    if ((requirements.memoryTypeBits & (1u << i)) && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
      type = i;
      if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) break;
    }
  }
  if (type == properties.memoryTypeCount) return XR_ERROR_OUT_OF_MEMORY;
  coherent_ = properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, type};
  result = checked(vk<PFN_vkAllocateMemory>("vkAllocateMemory")(binding_.device, &mai, nullptr, &memory_), "vkAllocateMemory");
  if (XR_FAILED(result)) return result;
  result = checked(vk<PFN_vkBindBufferMemory>("vkBindBufferMemory")(binding_.device, staging_, memory_, 0),
                   "vkBindBufferMemory");
  if (XR_FAILED(result)) return result;
  return checked(vk<PFN_vkMapMemory>("vkMapMemory")(binding_.device, memory_, 0, VK_WHOLE_SIZE, 0, &mapped_), "vkMapMemory");
}
XrResult ImageUpload::publish(Swapchain& chain) {
  HostQueueLock lock(session().uses_shared_gpu);
  if (!lock) return XR_ERROR_RUNTIME_FAILURE;
  if (!coherent_) {
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, memory_, 0, VK_WHOLE_SIZE};
    auto result = checked(vk<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges")(binding_.device, 1, &range),
                          "vkFlushMappedMemoryRanges");
    if (XR_FAILED(result)) return result;
  }
  const std::uint32_t index = 0;
  auto result = chain.backing->wait(index);
  if (result != XR_SUCCESS) return result;
  result = checked(vk<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(command_, 0), "vkResetCommandBuffer");
  if (XR_FAILED(result)) return result;
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  result = checked(vk<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command_, &begin), "vkBeginCommandBuffer");
  if (XR_FAILED(result)) return result;
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = chain.images.at(index).image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {chain.info.width, chain.info.height, 1};
  vk<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(command_, staging_, barrier.image,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  result = checked(vk<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command_), "vkEndCommandBuffer");
  if (XR_FAILED(result)) return result;
  result = checked(vk<PFN_vkResetFences>("vkResetFences")(binding_.device, 1, &fence_), "vkResetFences");
  if (XR_FAILED(result)) return result;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command_;
  result = checked(vk<PFN_vkQueueSubmit>("vkQueueSubmit")(queue_, 1, &submit, fence_), "vkQueueSubmit");
  if (XR_FAILED(result)) return result;
  result = checked(vk<PFN_vkWaitForFences>("vkWaitForFences")(binding_.device, 1, &fence_, VK_TRUE, UINT64_MAX),
                   "vkWaitForFences");
  if (XR_FAILED(result)) return result;
  chain.released_index = index;
  ++chain.release_serial;
  return XR_SUCCESS;
}
} // namespace venture::hle::openxr
