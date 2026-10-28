#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

#include "gfx/gpu_device.h"
#include "hle/vulkan/texture.h"
#include "hle/vulkan/texture_memory.h"
#include "vulkan_test_support.h"

using namespace venture;

namespace {
struct AllocationHook {
  static inline PFN_vkGetInstanceProcAddr gipa{};
  static inline PFN_vkGetDeviceProcAddr gdpa{};
  static inline PFN_vkAllocateMemory allocate{};
  static inline PFN_vkCreateImage create_image{};
  static inline PFN_vkCreateImageView create_view{};
  static inline PFN_vkCreateDescriptorPool create_pool{};
  static inline bool fail_pool = false;
  static inline VkImageUsageFlags view_usage{};
  static inline PFN_vkUpdateDescriptorSets update{};
  static inline PFN_vkUpdateDescriptorSetWithTemplate update_template{};
  static inline VkImageView watched{};
  static inline VkImageLayout descriptor_layout{}, template_layout{};
  static inline unsigned allocations{};
  static inline unsigned sparse_images{};
  static VkResult VKAPI_CALL on_pool(VkDevice d, const VkDescriptorPoolCreateInfo* info,
      const VkAllocationCallbacks* a, VkDescriptorPool* pool) {
    if (fail_pool) return VK_ERROR_OUT_OF_HOST_MEMORY;
    return create_pool(d, info, a, pool);
  }
  static VkResult VKAPI_CALL on_view(VkDevice d, const VkImageViewCreateInfo* info,
      const VkAllocationCallbacks* a, VkImageView* view) {
    view_usage = 0;
    for (auto* node = static_cast<const VkBaseInStructure*>(info->pNext); node; node = node->pNext)
      if (node->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO)
        view_usage = reinterpret_cast<const VkImageViewUsageCreateInfo*>(node)->usage;
    return create_view(d, info, a, view);
  }
  static void VKAPI_CALL on_update(VkDevice d, std::uint32_t count, const VkWriteDescriptorSet* writes,
      std::uint32_t copy_count, const VkCopyDescriptorSet* copies) {
    for (std::uint32_t i = 0; i < count; ++i)
      if (writes[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
        for (std::uint32_t j = 0; j < writes[i].descriptorCount; ++j)
          if (writes[i].pImageInfo[j].imageView == watched)
            descriptor_layout = writes[i].pImageInfo[j].imageLayout;
    update(d, count, writes, copy_count, copies);
  }
  static void VKAPI_CALL on_template(VkDevice d, VkDescriptorSet set, VkDescriptorUpdateTemplate t,
                                    const void* data) {
    if (watched) {
      VkDescriptorImageInfo info;
      std::memcpy(&info, static_cast<const std::byte*>(data) + 8, sizeof(info));
      if (info.imageView == watched) template_layout = info.imageLayout;
    }
    update_template(d, set, t, data);
  }
  static VkResult VKAPI_CALL on_image(VkDevice d, const VkImageCreateInfo* info,
      const VkAllocationCallbacks* a, VkImage* image) {
    auto r = create_image(d, info, a, image);
    if (r == VK_SUCCESS && (info->flags & VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT)) ++sparse_images;
    return r;
  }
  static VkResult VKAPI_CALL on_allocate(VkDevice d, const VkMemoryAllocateInfo* info,
      const VkAllocationCallbacks* a, VkDeviceMemory* memory) {
    ++allocations;
    return allocate(d, info, a, memory);
  }
  static PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice d, const char* name) {
    auto proc = gdpa(d, name);
    if (std::string_view(name) == "vkAllocateMemory") {
      allocate = reinterpret_cast<PFN_vkAllocateMemory>(proc);
      return reinterpret_cast<PFN_vkVoidFunction>(on_allocate);
    }
    if (std::string_view(name) == "vkCreateImage") {
      create_image = reinterpret_cast<PFN_vkCreateImage>(proc);
      return reinterpret_cast<PFN_vkVoidFunction>(on_image);
    }
    if (std::string_view(name) == "vkCreateImageView") {
      create_view = reinterpret_cast<PFN_vkCreateImageView>(proc);
      return reinterpret_cast<PFN_vkVoidFunction>(on_view);
    }
    if (std::string_view(name) == "vkCreateDescriptorPool") {
      create_pool = reinterpret_cast<PFN_vkCreateDescriptorPool>(proc);
      return reinterpret_cast<PFN_vkVoidFunction>(on_pool);
    }
    if (std::string_view(name) == "vkUpdateDescriptorSets") {
      update = reinterpret_cast<PFN_vkUpdateDescriptorSets>(proc);
      return reinterpret_cast<PFN_vkVoidFunction>(on_update);
    }
    if (std::string_view(name) == "vkUpdateDescriptorSetWithTemplate") {
      update_template = reinterpret_cast<PFN_vkUpdateDescriptorSetWithTemplate>(proc);
      return reinterpret_cast<PFN_vkVoidFunction>(on_template);
    }
    return proc;
  }
  static PFN_vkVoidFunction VKAPI_CALL instance_proc(VkInstance i, const char* name) {
    auto proc = gipa(i, name);
    if (std::string_view(name) == "vkGetDeviceProcAddr") {
      gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(proc);
      return reinterpret_cast<PFN_vkVoidFunction>(device_proc);
    }
    return proc;
  }
  AllocationHook() {
    gipa = gfx::gpu_device().get_instance_proc_addr;
    gfx::gpu_device().get_instance_proc_addr = instance_proc;
  }
  ~AllocationHook() { watched = VK_NULL_HANDLE; gfx::gpu_device().get_instance_proc_addr = gipa; }
};
struct ForceDecode {
  ForceDecode() { vulkan::force_texture_emulation(true); }
  ~ForceDecode() { vulkan::force_texture_emulation(false); }
};
std::array<std::uint8_t, 16> color_block(bool red) {
  return {0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
          std::uint8_t(red ? 255 : 0), std::uint8_t(red ? 255 : 0),
          std::uint8_t(red ? 0 : 255), std::uint8_t(red ? 0 : 255), 0, 0, 255, 255};
}

}

TEST_CASE("Mutable ASTC companions initialize a late view from the latest uploads") {
  for (bool srgb_companion : {false, true}) {
    CAPTURE(srgb_companion);
    AllocationHook hook;
    ForceDecode force;
    test::VulkanDevice gpu;
    test::VulkanBuffer upload(gpu, 16), readback(gpu, 4 * 4 * 8);
    const auto sparse_before = AllocationHook::sparse_images;
    test::VulkanImage image(gpu, srgb_companion ? VK_FORMAT_ASTC_4x4_UNORM_BLOCK : VK_FORMAT_ASTC_4x4_SRGB_BLOCK, 4, 4, 1, 1,
                            VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
    auto planes = vulkan::emulated_texture_planes(image.image);
    REQUIRE(planes.size() == 2);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {4, 4, 1};
    for (bool red : {true, true, false}) {
      auto block = color_block(red);
      std::memcpy(upload.mapped, block.data(), block.size());
      gpu.begin();
      if (red) image.transition();
      test::vk_call("vkCmdCopyBufferToImage", gpu.command, upload.buffer, image.image,
                    VK_IMAGE_LAYOUT_GENERAL, 1u, &copy);
      if (red) gpu.submit();
      else {
        REQUIRE(test::vk_call("vkEndCommandBuffer", gpu.command) == VK_SUCCESS);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1; submit.pCommandBuffers = &gpu.command;
        REQUIRE(test::vk_call("vkQueueSubmit", gpu.queue, 1u, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
      }
    }
    const auto before_view = AllocationHook::allocations;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image.image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = srgb_companion ? VK_FORMAT_ASTC_4x4_SRGB_BLOCK : VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view{};
    if (AllocationHook::sparse_images > sparse_before) {
      AllocationHook::fail_pool = true;
      CHECK(static_cast<VkResult>(test::vk_call("vkCreateImageView", gpu.device, &vi, nullptr, &view)) ==
            VK_ERROR_OUT_OF_HOST_MEMORY);
      AllocationHook::fail_pool = false;
    }
    REQUIRE(test::vk_call("vkCreateImageView", gpu.device, &vi, nullptr, &view) == VK_SUCCESS);
    REQUIRE(test::vk_call("vkQueueWaitIdle", gpu.queue) == VK_SUCCESS);
    // A sparse host allocates at first use; hosts without sparse support keep
    // the eager fallback, and must produce identical decoded contents.
    CHECK(AllocationHook::allocations - before_view ==
          (AllocationHook::sparse_images > sparse_before ? 1u : 0u));
    if (AllocationHook::sparse_images > sparse_before) {
      CHECK((AllocationHook::view_usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0);
      CHECK((AllocationHook::view_usage & VK_IMAGE_USAGE_STORAGE_BIT) == 0);
    }
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.bindingCount = 1; layout_info.pBindings = &binding;
    VkDescriptorSetLayout layout{};
    REQUIRE(test::vk_call("vkCreateDescriptorSetLayout", gpu.device, &layout_info, nullptr, &layout) == VK_SUCCESS);
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 1; pool_info.poolSizeCount = 1; pool_info.pPoolSizes = &pool_size;
    VkDescriptorPool pool{};
    REQUIRE(test::vk_call("vkCreateDescriptorPool", gpu.device, &pool_info, nullptr, &pool) == VK_SUCCESS);
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = pool; allocate.descriptorSetCount = 1; allocate.pSetLayouts = &layout;
    VkDescriptorSet set{};
    REQUIRE(test::vk_call("vkAllocateDescriptorSets", gpu.device, &allocate, &set) == VK_SUCCESS);
    AllocationHook::watched = view;
    VkDescriptorImageInfo descriptor{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set; write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.descriptorCount = 1; write.pImageInfo = &descriptor;
    test::vk_call("vkUpdateDescriptorSets", gpu.device, 1u, &write, 0u, nullptr);
    const auto expected_layout = AllocationHook::sparse_images > sparse_before ? VK_IMAGE_LAYOUT_GENERAL :
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    CHECK(AllocationHook::descriptor_layout == expected_layout);
    CHECK(descriptor.imageLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkDescriptorUpdateTemplateEntry entry{0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8, 32};
    VkDescriptorUpdateTemplateCreateInfo ti{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
    ti.descriptorUpdateEntryCount = 1; ti.pDescriptorUpdateEntries = &entry;
    ti.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET; ti.descriptorSetLayout = layout;
    VkDescriptorUpdateTemplate t{};
    REQUIRE(test::vk_call("vkCreateDescriptorUpdateTemplate", gpu.device, &ti, nullptr, &t) == VK_SUCCESS);
    std::array<std::byte, 8 + sizeof(descriptor)> payload{};
    std::memcpy(payload.data() + 8, &descriptor, sizeof(descriptor));
    test::vk_call("vkUpdateDescriptorSetWithTemplate", gpu.device, set, t, payload.data());
    CHECK(AllocationHook::template_layout == expected_layout);
    VkDescriptorImageInfo unchanged;
    std::memcpy(&unchanged, payload.data() + 8, sizeof(unchanged));
    CHECK(unchanged.imageLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    test::vk_call("vkDestroyDescriptorUpdateTemplate", gpu.device, t, nullptr);
    test::vk_call("vkDestroyDescriptorPool", gpu.device, pool, nullptr);
    test::vk_call("vkDestroyDescriptorSetLayout", gpu.device, layout, nullptr);
    AllocationHook::watched = VK_NULL_HANDLE;
    for (bool red : {false, true}) {
      gpu.begin();
      if (red) {
        auto block = color_block(true);
        std::memcpy(upload.mapped, block.data(), block.size());
        test::vk_call("vkCmdCopyBufferToImage", gpu.command, upload.buffer, image.image,
                      VK_IMAGE_LAYOUT_GENERAL, 1u, &copy);
      }
      gpu.barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
      reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(vulkan::raw_device_proc(gpu.device, "vkCmdCopyImageToBuffer"))(
          gpu.command, planes[1], VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &copy);
      gpu.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
      gpu.submit();
      auto* values = static_cast<const std::uint8_t*>(readback.mapped);
      for (unsigned i = 0; i < 16; ++i) {
        if (srgb_companion) {
          auto* bytes = static_cast<const std::uint8_t*>(readback.mapped);
          CHECK(bytes[i * 4] == (red ? 255 : 0));
          CHECK(bytes[i * 4 + 1] == (red ? 0 : 255));
          CHECK(bytes[i * 4 + 2] == 0);
          CHECK(bytes[i * 4 + 3] == 255);
        } else {
        CHECK(values[i * 4] == (red ? 255 : 0));
        CHECK(values[i * 4 + 1] == (red ? 0 : 255));
        CHECK(values[i * 4 + 2] == 0);
        CHECK(values[i * 4 + 3] == 255);
        }
      }
    }
    test::vk_call("vkDestroyImageView", gpu.device, view, nullptr);
  }
}
