#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <type_traits>
#include <vector>

#include "gfx/gpu_device.h"
#include "gfx/pipeline_trace.h"
#include "gfx/vulkan_memory.h"
#include "vulkan_test_support.h"

namespace {
std::uintptr_t next_handle = 0x10000;
template <class H> H handle() {
  if constexpr (std::is_pointer_v<H>) return reinterpret_cast<H>(++next_handle);
  else return static_cast<H>(++next_handle);
}
std::map<VkBuffer, VkDeviceSize> buffers;
std::map<VkImage, VkDeviceSize> images;
std::vector<VkBuffer> copies;
unsigned created_buffers = 0, transfer_compute_dependencies = 0;
bool lazy_mock = false;
unsigned dispatches = 0, sparse_binds = 0;
std::map<VkDeviceMemory, std::vector<std::byte>> memory;
std::vector<VkDeviceSize> allocation_sizes;

template <class Info, class H>
VkResult VKAPI_CALL create(VkDevice, const Info*, const VkAllocationCallbacks*, H* out) {
  *out = handle<H>();
  return VK_SUCCESS;
}
template <class H> void VKAPI_CALL destroy(VkDevice, H, const VkAllocationCallbacks*) {}
VkResult VKAPI_CALL create_buffer(VkDevice, const VkBufferCreateInfo* info,
                                  const VkAllocationCallbacks*, VkBuffer* out) {
  *out = handle<VkBuffer>(); buffers[*out] = info->size; ++created_buffers;
  return VK_SUCCESS;
}
void VKAPI_CALL destroy_buffer(VkDevice, VkBuffer b, const VkAllocationCallbacks*) { buffers.erase(b); }
VkResult VKAPI_CALL create_image(VkDevice, const VkImageCreateInfo* info,
                                 const VkAllocationCallbacks*, VkImage* out) {
  *out = handle<VkImage>(); images[*out] = VkDeviceSize(info->extent.width) * info->extent.height * 8;
  return VK_SUCCESS;
}
void VKAPI_CALL destroy_image(VkDevice, VkImage i, const VkAllocationCallbacks*) { images.erase(i); }
void VKAPI_CALL buffer_requirements(VkDevice, VkBuffer b, VkMemoryRequirements* out) {
  *out = {buffers.at(b), 256, 3};
}
void VKAPI_CALL image_requirements(VkDevice, VkImage i, VkMemoryRequirements* out) {
  *out = {images.at(i), 256, 3};
}
VkResult VKAPI_CALL allocate_memory(VkDevice, const VkMemoryAllocateInfo* info,
                                    const VkAllocationCallbacks*, VkDeviceMemory* out) {
  if (!lazy_mock) CHECK(info->memoryTypeIndex == 0); // Scratch remains unmappable DEVICE_LOCAL memory.
  *out = handle<VkDeviceMemory>();
  memory[*out].resize(info->allocationSize);
  allocation_sizes.push_back(info->allocationSize);
  return VK_SUCCESS;
}
void VKAPI_CALL free_memory(VkDevice, VkDeviceMemory m, const VkAllocationCallbacks*) { memory.erase(m); }
VkResult VKAPI_CALL map_memory(VkDevice, VkDeviceMemory m, VkDeviceSize offset, VkDeviceSize, VkMemoryMapFlags, void** out) {
  *out = memory.at(m).data() + offset; return VK_SUCCESS;
}
void VKAPI_CALL unmap_memory(VkDevice, VkDeviceMemory) {}
void VKAPI_CALL sparse_requirements(VkDevice, VkImage, unsigned* count, VkSparseImageMemoryRequirements* out) {
  *count = 1;
  if (out) {
    *out = {}; out->formatProperties.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    out->formatProperties.imageGranularity = {4, 4, 1}; out->imageMipTailFirstLod = 1;
  }
}
void VKAPI_CALL sparse_formats(VkPhysicalDevice, VkFormat, VkImageType, VkSampleCountFlagBits,
    VkImageUsageFlags, VkImageTiling, unsigned* count, VkSparseImageFormatProperties*) { *count = 1; }
VkResult VKAPI_CALL bind_sparse(VkQueue, unsigned count, const VkBindSparseInfo* infos, VkFence) {
  ++sparse_binds;
  for (unsigned i = 0; i < count; ++i) for (unsigned j = 0; j < infos[i].imageBindCount; ++j)
    for (unsigned k = 0; k < infos[i].pImageBinds[j].bindCount; ++k)
      CHECK(memory.contains(infos[i].pImageBinds[j].pBinds[k].memory));
  return VK_SUCCESS;
}
VkResult VKAPI_CALL submit(VkQueue, unsigned, const VkSubmitInfo*, VkFence) { return VK_SUCCESS; }
VkResult VKAPI_CALL wait_fences(VkDevice, unsigned, const VkFence*, VkBool32, std::uint64_t) { return VK_SUCCESS; }
VkResult VKAPI_CALL fence_status(VkDevice, VkFence) { return VK_SUCCESS; }
VkResult VKAPI_CALL reset_fences(VkDevice, unsigned, const VkFence*) { return VK_SUCCESS; }
VkResult VKAPI_CALL idle(VkQueue) { return VK_SUCCESS; }
VkResult VKAPI_CALL end(VkCommandBuffer) { return VK_SUCCESS; }
void VKAPI_CALL get_queue(VkDevice, unsigned, unsigned index, VkQueue* queue) {
  *queue = reinterpret_cast<VkQueue>(0x3000 + index);
}
VkResult VKAPI_CALL bind_buffer(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; }
VkResult VKAPI_CALL bind_image(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; }
VkResult VKAPI_CALL allocate_commands(VkDevice, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* out) {
  for (unsigned i = 0; i < info->commandBufferCount; ++i) out[i] = handle<VkCommandBuffer>();
  return VK_SUCCESS;
}
VkResult VKAPI_CALL begin(VkCommandBuffer, const VkCommandBufferBeginInfo*) { return VK_SUCCESS; }
VkResult VKAPI_CALL reset(VkCommandBuffer, VkCommandBufferResetFlags) { return VK_SUCCESS; }
VkResult VKAPI_CALL pipelines(VkDevice, VkPipelineCache, unsigned count,
                              const VkComputePipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline* out) {
  for (unsigned i = 0; i < count; ++i) out[i] = handle<VkPipeline>();
  return VK_SUCCESS;
}
VkResult VKAPI_CALL sets(VkDevice, const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* out) {
  for (unsigned i = 0; i < info->descriptorSetCount; ++i) out[i] = handle<VkDescriptorSet>();
  return VK_SUCCESS;
}
void VKAPI_CALL update_sets(VkDevice, unsigned, const VkWriteDescriptorSet*, unsigned, const VkCopyDescriptorSet*) {}
void VKAPI_CALL bind_pipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}
void VKAPI_CALL bind_sets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, unsigned, unsigned,
                          const VkDescriptorSet*, unsigned, const unsigned*) {}
void VKAPI_CALL push(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, unsigned, unsigned, const void*) {}
void VKAPI_CALL dispatch(VkCommandBuffer, unsigned, unsigned, unsigned) {
  ++dispatches;
  if (!lazy_mock) CHECK(transfer_compute_dependencies == copies.size() / 2 + 1);
}
void VKAPI_CALL copy_buffer(VkCommandBuffer, VkBuffer source, VkBuffer dest, unsigned count,
                            const VkBufferCopy* regions) {
  REQUIRE(buffers.contains(source));
  REQUIRE(buffers.contains(dest));
  for (unsigned i = 0; i < count; ++i) {
    CHECK(regions[i].srcOffset + regions[i].size <= buffers.at(source));
    CHECK(regions[i].dstOffset + regions[i].size <= buffers.at(dest));
  }
}
void VKAPI_CALL copy_image(VkCommandBuffer, VkBuffer b, VkImage, VkImageLayout, unsigned, const VkBufferImageCopy*) {
  copies.push_back(b);
}
void VKAPI_CALL barrier(VkCommandBuffer, VkPipelineStageFlags source, VkPipelineStageFlags dest,
                        VkDependencyFlags, unsigned, const VkMemoryBarrier*, unsigned,
                        const VkBufferMemoryBarrier*, unsigned, const VkImageMemoryBarrier*) {
  if ((source & VK_PIPELINE_STAGE_TRANSFER_BIT) && (dest & VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT))
    ++transfer_compute_dependencies;
}
void VKAPI_CALL properties(VkPhysicalDevice, VkPhysicalDeviceProperties* out) {
  *out = {}; out->limits.maxStorageBufferRange = 1 << 20;
  out->limits.maxMemoryAllocationCount = 4096;
  out->limits.bufferImageGranularity = out->limits.nonCoherentAtomSize = 1;
}
void VKAPI_CALL memory_properties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* out) {
  *out = {}; out->memoryTypeCount = 2;
  out->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  out->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  out->memoryTypes[1].heapIndex = 1;
  // Every driver reports its heaps; the VMA-pooled decoded images budget by heap.
  out->memoryHeapCount = 2;
  out->memoryHeaps[0] = {VkDeviceSize(1) << 32, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
  out->memoryHeaps[1] = {VkDeviceSize(1) << 32, 0};
}
void VKAPI_CALL formats(VkPhysicalDevice, VkFormat, VkFormatProperties* out) { *out = {}; }
void VKAPI_CALL families(VkPhysicalDevice, unsigned* count, VkQueueFamilyProperties* out) {
  *count = 1;
  if (out) {
    *out = {}; out->queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    if (lazy_mock) out->queueFlags |= VK_QUEUE_SPARSE_BINDING_BIT;
    out->queueCount = lazy_mock ? 2 : 1;
  }
}
PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice, const char* name) {
#define FN(n, f) if (!std::strcmp(name, n)) return reinterpret_cast<PFN_vkVoidFunction>(f)
  FN("vkCreateBuffer", create_buffer); FN("vkDestroyBuffer", destroy_buffer);
  FN("vkGetBufferMemoryRequirements", buffer_requirements); FN("vkBindBufferMemory", bind_buffer);
  FN("vkAllocateMemory", allocate_memory); FN("vkFreeMemory", free_memory);
  FN("vkMapMemory", map_memory); FN("vkUnmapMemory", unmap_memory);
  FN("vkCreateImage", create_image); FN("vkDestroyImage", destroy_image);
  FN("vkGetImageMemoryRequirements", image_requirements); FN("vkBindImageMemory", bind_image);
  FN("vkGetImageSparseMemoryRequirements", sparse_requirements);
  FN("vkCreateImageView", (create<VkImageViewCreateInfo, VkImageView>));
  FN("vkDestroyImageView", destroy<VkImageView>);
  FN("vkGetDeviceQueue", get_queue); FN("vkQueueSubmit", submit); FN("vkQueueBindSparse", bind_sparse);
  FN("vkQueueWaitIdle", idle); FN("vkGetFenceStatus", fence_status); FN("vkWaitForFences", wait_fences);
  FN("vkResetFences", reset_fences);
  FN("vkCreateFence", (create<VkFenceCreateInfo, VkFence>)); FN("vkDestroyFence", destroy<VkFence>);
  FN("vkCreateSemaphore", (create<VkSemaphoreCreateInfo, VkSemaphore>)); FN("vkDestroySemaphore", destroy<VkSemaphore>);
  FN("vkCreateCommandPool", (create<VkCommandPoolCreateInfo, VkCommandPool>));
  FN("vkAllocateCommandBuffers", allocate_commands); FN("vkBeginCommandBuffer", begin);
  FN("vkResetCommandBuffer", reset);
  FN("vkEndCommandBuffer", end); FN("vkDestroyCommandPool", destroy<VkCommandPool>);
  FN("vkCreateDescriptorSetLayout", (create<VkDescriptorSetLayoutCreateInfo, VkDescriptorSetLayout>));
  FN("vkDestroyDescriptorSetLayout", destroy<VkDescriptorSetLayout>);
  FN("vkCreatePipelineLayout", (create<VkPipelineLayoutCreateInfo, VkPipelineLayout>));
  FN("vkDestroyPipelineLayout", destroy<VkPipelineLayout>);
  FN("vkCreateShaderModule", (create<VkShaderModuleCreateInfo, VkShaderModule>));
  FN("vkDestroyShaderModule", destroy<VkShaderModule>);
  FN("vkCreateComputePipelines", pipelines); FN("vkDestroyPipeline", destroy<VkPipeline>);
  FN("vkCreateDescriptorPool", (create<VkDescriptorPoolCreateInfo, VkDescriptorPool>));
  FN("vkDestroyDescriptorPool", destroy<VkDescriptorPool>); FN("vkAllocateDescriptorSets", sets);
  FN("vkUpdateDescriptorSets", update_sets); FN("vkCmdBindPipeline", bind_pipeline);
  FN("vkCmdBindDescriptorSets", bind_sets); FN("vkCmdPushConstants", push); FN("vkCmdDispatch", dispatch);
  FN("vkCmdCopyBuffer", copy_buffer); FN("vkCmdCopyBufferToImage", copy_image); FN("vkCmdPipelineBarrier", barrier);
  return nullptr;
#undef FN
}
PFN_vkVoidFunction VKAPI_CALL proc(VkInstance, const char* name) {
#define FN(n, f) if (!std::strcmp(name, n)) return reinterpret_cast<PFN_vkVoidFunction>(f)
  FN("vkGetDeviceProcAddr", device_proc); FN("vkGetPhysicalDeviceProperties", properties);
  FN("vkGetPhysicalDeviceMemoryProperties", memory_properties); FN("vkGetPhysicalDeviceFormatProperties", formats);
  FN("vkGetPhysicalDeviceQueueFamilyProperties", families);
  FN("vkGetPhysicalDeviceSparseImageFormatProperties", sparse_formats);
  return device_proc(VK_NULL_HANDLE, name);
#undef FN
}
} // namespace
namespace venture::gfx {
GpuDevice& gpu_device() {
  static GpuDevice gpu;
  gpu.get_instance_proc_addr = proc;
  return gpu;
}
}

TEST_CASE("Decoded scratch follows per-plane demand and preserves simultaneous-use outputs") {
  using venture::test::vk_call;
  auto physical = reinterpret_cast<VkPhysicalDevice>(0x1000);
  auto device = reinterpret_cast<VkDevice>(0x2000);
  VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  venture::vulkan::adopt_device(physical, device, device_info);
  venture::gfx::register_pipeline_device(device, device_proc, false, venture::profile::PipelineSource::guest);
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  VkCommandPool pool{};
  REQUIRE(vk_call("vkCreateCommandPool", device, &pool_info, nullptr, &pool) == VK_SUCCESS);
  VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocate.commandPool = pool; allocate.commandBufferCount = 1;
  VkCommandBuffer command{};
  REQUIRE(vk_call("vkAllocateCommandBuffers", device, &allocate, &command) == VK_SUCCESS);
  VkBufferCreateInfo upload_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  upload_info.size = 1024; upload_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  VkBuffer upload{};
  REQUIRE(vk_call("vkCreateBuffer", device, &upload_info, nullptr, &upload) == VK_SUCCESS);
  VkMemoryRequirements upload_requirements{};
  vk_call("vkGetBufferMemoryRequirements", device, upload, &upload_requirements);
  VkMemoryAllocateInfo upload_allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  upload_allocation.allocationSize = upload_requirements.size;
  VkDeviceMemory upload_memory{};
  REQUIRE(vk_call("vkAllocateMemory", device, &upload_allocation, nullptr, &upload_memory) == VK_SUCCESS);
  REQUIRE(vk_call("vkBindBufferMemory", device, upload, upload_memory, 0ull) == VK_SUCCESS);
  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D; image_info.extent = {8, 8, 1};
  image_info.format = VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
  image_info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  image_info.mipLevels = image_info.arrayLayers = 1; image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VkImage image{};
  REQUIRE(vk_call("vkCreateImage", device, &image_info, nullptr, &image) == VK_SUCCESS);
  for (auto flags : {VkCommandBufferUsageFlags(0), VkCommandBufferUsageFlags(VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT)}) {
    CAPTURE(flags);
    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin_info.flags = flags;
    REQUIRE(vk_call("vkBeginCommandBuffer", command, &begin_info) == VK_SUCCESS);
    const auto before = created_buffers;
    copies.clear(); transfer_compute_dependencies = 0;
    for (unsigned upload_index = 0; upload_index < 32; ++upload_index) {
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; region.imageExtent = {8, 8, 1};
      vk_call("vkCmdCopyBufferToImage", command, upload, image,
              VK_IMAGE_LAYOUT_GENERAL, 1u, &region);
    }
    REQUIRE(copies.size() == 64);
    CHECK(transfer_compute_dependencies == 32);
    CHECK(created_buffers - before == (flags ? 62 : 2)); // The reset pool supplies two existing buffers.
    for (unsigned upload = 0; upload < 32; ++upload) {
      CHECK(copies[2 * upload] != copies[2 * upload + 1]);
      if (!flags) { CHECK(copies[2 * upload] == copies[0]); CHECK(copies[2 * upload + 1] == copies[1]); }
      else if (upload) { CHECK(copies[2 * upload] != copies[2 * upload - 2]); }
    }
    REQUIRE(vk_call("vkResetCommandBuffer", command, 0u) == VK_SUCCESS);
  }
  venture::vulkan::trim_texture_pool(device, pool);
  std::vector<VkImage> growing;
  for (unsigned width : {12u, 16u, 18u}) {
    image_info.extent = {width, width, 1};
    VkImage next{};
    REQUIRE(vk_call("vkCreateImage", device, &image_info, nullptr, &next) == VK_SUCCESS);
    growing.push_back(next);
  }
  VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  REQUIRE(vk_call("vkBeginCommandBuffer", command, &begin_info) == VK_SUCCESS);
  const auto before = created_buffers;
  copies.clear(); transfer_compute_dependencies = 0;
  for (unsigned i = 0; i < 36; ++i) {
    const unsigned width = i == 0 ? 8 : i == 1 ? 12 : i == 2 ? 16 : 18;
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; region.imageExtent = {width, width, 1};
    vk_call("vkCmdCopyBufferToImage", command, upload,
            i == 0 ? image : growing[std::min(i - 1, 2u)], VK_IMAGE_LAYOUT_GENERAL, 1u, &region);
  }
  CHECK(created_buffers - before == 8);
  REQUIRE(copies.size() == 72);
  VkDeviceSize retained = 0;
  for (unsigned i = 0; i < 8; ++i) retained += buffers.at(copies[i]);
  CHECK(retained == VkDeviceSize(8 * 8 + 12 * 12 + 16 * 16 + 18 * 18) * 8);
  for (unsigned i = 3; i < 36; ++i) {
    CHECK(copies[2 * i] == copies[6]); CHECK(copies[2 * i + 1] == copies[7]);
    CHECK(buffers.at(copies[2 * i]) == 18 * 18 * 4);
    CHECK(buffers.at(copies[2 * i + 1]) == 18 * 18 * 4);
  }
  venture::vulkan::release_texture_commands(command);
  venture::vulkan::trim_texture_pool(device, pool);
  vk_call("vkDestroyImage", device, image, nullptr);
  for (auto next : growing) vk_call("vkDestroyImage", device, next, nullptr);
  vk_call("vkDestroyBuffer", device, upload, nullptr);
  vk_call("vkFreeMemory", device, upload_memory, nullptr);
  venture::vulkan::release_texture_device(device);
  venture::gfx::release_pipeline_device(device);
  CHECK(buffers.empty());
  CHECK(images.empty());
}
