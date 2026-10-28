#include <doctest/doctest.h>

#include <cstring>

#include "hle/vulkan/internal.h"
#include "vulkan_test_support.h"

using namespace venture;
TEST_CASE("Vulkan uninitialized loop counters execute a bounded compute loop") {
  // GLSL: int counter, accumulator; for (;;) { if(counter>=8)break;
  // accumulator+=1; ++counter; } result.value=accumulator;
  const std::uint32_t code[]{
      0x07230203, 0x00010000, 0x0008000b, 0x00000025, 0x00000000, 0x00020011,
      0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
      0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0005000f, 0x00000005,
      0x00000004, 0x6e69616d, 0x00000000, 0x00060010, 0x00000004, 0x00000011,
      0x00000001, 0x00000001, 0x00000001, 0x00030003, 0x00000002, 0x000001c2,
      0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00040005, 0x0000000c,
      0x6e756f63, 0x00726574, 0x00050005, 0x00000014, 0x75636361, 0x616c756d,
      0x00726f74, 0x00040005, 0x0000001a, 0x7074754f, 0x00007475, 0x00050006,
      0x0000001a, 0x00000000, 0x756c6176, 0x00000065, 0x00040005, 0x0000001c,
      0x75736572, 0x0000746c, 0x00030047, 0x0000001a, 0x00000003, 0x00050048,
      0x0000001a, 0x00000000, 0x00000023, 0x00000000, 0x00040047, 0x0000001c,
      0x00000021, 0x00000000, 0x00040047, 0x0000001c, 0x00000022, 0x00000000,
      0x00040047, 0x00000024, 0x0000000b, 0x00000019, 0x00020013, 0x00000002,
      0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x0000000a, 0x00000020,
      0x00000001, 0x00040020, 0x0000000b, 0x00000007, 0x0000000a, 0x0004002b,
      0x0000000a, 0x0000000e, 0x00000008, 0x00020014, 0x0000000f, 0x0004002b,
      0x0000000a, 0x00000015, 0x00000001, 0x0003001e, 0x0000001a, 0x0000000a,
      0x00040020, 0x0000001b, 0x00000002, 0x0000001a, 0x0004003b, 0x0000001b,
      0x0000001c, 0x00000002, 0x0004002b, 0x0000000a, 0x0000001d, 0x00000000,
      0x00040020, 0x0000001f, 0x00000002, 0x0000000a, 0x00040015, 0x00000021,
      0x00000020, 0x00000000, 0x00040017, 0x00000022, 0x00000021, 0x00000003,
      0x0004002b, 0x00000021, 0x00000023, 0x00000001, 0x0006002c, 0x00000022,
      0x00000024, 0x00000023, 0x00000023, 0x00000023, 0x00050036, 0x00000002,
      0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003b,
      0x0000000b, 0x0000000c, 0x00000007, 0x0004003b, 0x0000000b, 0x00000014,
      0x00000007, 0x000200f9, 0x00000006, 0x000200f8, 0x00000006, 0x000400f6,
      0x00000008, 0x00000009, 0x00000000, 0x000200f9, 0x00000007, 0x000200f8,
      0x00000007, 0x0004003d, 0x0000000a, 0x0000000d, 0x0000000c, 0x000500af,
      0x0000000f, 0x00000010, 0x0000000d, 0x0000000e, 0x000300f7, 0x00000012,
      0x00000000, 0x000400fa, 0x00000010, 0x00000011, 0x00000012, 0x000200f8,
      0x00000011, 0x000200f9, 0x00000008, 0x000200f8, 0x00000012, 0x0004003d,
      0x0000000a, 0x00000016, 0x00000014, 0x00050080, 0x0000000a, 0x00000017,
      0x00000016, 0x00000015, 0x0003003e, 0x00000014, 0x00000017, 0x0004003d,
      0x0000000a, 0x00000018, 0x0000000c, 0x00050080, 0x0000000a, 0x00000019,
      0x00000018, 0x00000015, 0x0003003e, 0x0000000c, 0x00000019, 0x000200f9,
      0x00000009, 0x000200f8, 0x00000009, 0x000200f9, 0x00000006, 0x000200f8,
      0x00000008, 0x0004003d, 0x0000000a, 0x0000001e, 0x00000014, 0x00050041,
      0x0000001f, 0x00000020, 0x0000001c, 0x0000001d, 0x0003003e, 0x00000020,
      0x0000001e, 0x000100fd, 0x00010038,
  };
  test::VulkanDevice gpu;
  test::VulkanBuffer output(gpu, 16);
  std::memset(output.mapped, 0xff, 16);
  VkShaderModuleCreateInfo shader_info{
      VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  shader_info.codeSize = sizeof(code);
  shader_info.pCode = code;
  VkShaderModule shader{};
  REQUIRE(test::vk_call("vkCreateShaderModule", gpu.device, &shader_info,
                        nullptr, &shader) == VK_SUCCESS);
  CHECK(shader_info.pCode == code);
  CHECK(shader_info.codeSize == sizeof(code));
  VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo set_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 1;
  set_info.pBindings = &binding;
  VkDescriptorSetLayout set_layout{};
  REQUIRE(test::vk_call("vkCreateDescriptorSetLayout", gpu.device, &set_info,
                        nullptr, &set_layout) == VK_SUCCESS);
  VkPipelineLayoutCreateInfo layout_info{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &set_layout;
  VkPipelineLayout layout{};
  REQUIRE(test::vk_call("vkCreatePipelineLayout", gpu.device, &layout_info,
                        nullptr, &layout) == VK_SUCCESS);
  VkComputePipelineCreateInfo pipeline_info{
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipeline_info.stage.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipeline_info.stage.module = shader;
  pipeline_info.stage.pName = "main";
  pipeline_info.layout = layout;
  VkPipeline pipeline{};
  REQUIRE(test::vk_call("vkCreateComputePipelines", gpu.device, VK_NULL_HANDLE,
                        1u, &pipeline_info, nullptr, &pipeline) == VK_SUCCESS);
  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
  VkDescriptorPoolCreateInfo pool_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &size;
  VkDescriptorPool pool{};
  REQUIRE(test::vk_call("vkCreateDescriptorPool", gpu.device, &pool_info,
                        nullptr, &pool) == VK_SUCCESS);
  VkDescriptorSetAllocateInfo allocation{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocation.descriptorPool = pool;
  allocation.descriptorSetCount = 1;
  allocation.pSetLayouts = &set_layout;
  VkDescriptorSet set{};
  REQUIRE(test::vk_call("vkAllocateDescriptorSets", gpu.device, &allocation,
                        &set) == VK_SUCCESS);
  VkDescriptorBufferInfo buffer{output.buffer, 0, 16};
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = set;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write.pBufferInfo = &buffer;
  test::vk_call("vkUpdateDescriptorSets", gpu.device, 1u, &write, 0u, nullptr);
  gpu.begin();
  test::vk_call("vkCmdBindPipeline", gpu.command,
                VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  test::vk_call("vkCmdBindDescriptorSets", gpu.command,
                VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0u, 1u, &set, 0u,
                nullptr);
  test::vk_call("vkCmdDispatch", gpu.command, 1u, 1u, 1u);
  gpu.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
              VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
  gpu.submit();
  CHECK(*static_cast<std::int32_t*>(output.mapped) == 8);
  test::vk_call("vkDestroyDescriptorPool", gpu.device, pool, nullptr);
  test::vk_call("vkDestroyPipeline", gpu.device, pipeline, nullptr);
  test::vk_call("vkDestroyPipelineLayout", gpu.device, layout, nullptr);
  test::vk_call("vkDestroyDescriptorSetLayout", gpu.device, set_layout,
                nullptr);
  test::vk_call("vkDestroyShaderModule", gpu.device, shader, nullptr);
}

TEST_CASE("Vulkan storage images follow the view format without invalid duplicate types") {
  const std::vector<std::uint32_t> code{
      0x07230203, 0x00010000, 0, 8, 0,
      0x00020011, 1,                                  // OpCapability Shader
      0x00030016, 1, 32,                              // %1 float
      0x00090019, 2, 1, 2, 0, 0, 0, 2, 1,             // %2 3D storage Rgba32f
      0x00090019, 3, 1, 1, 0, 0, 0, 2, 1,             // %3 2D storage Rgba32f
      0x00090019, 4, 1, 1, 0, 0, 0, 2, 2,             // %4 2D storage Rgba16f
      0x00090019, 5, 1, 3, 0, 0, 0, 2, 3,             // %5 Cube storage R32f (atomics)
      0x00090019, 6, 1, 0, 0, 0, 0, 1, 0};            // %6 1D sampled
  auto out = vulkan::formatless_storage_images(code);
  REQUIRE(out.size() == code.size() + 4);
  CHECK(std::vector<std::uint32_t>(out.begin() + 5, out.begin() + 11) ==
        std::vector<std::uint32_t>{0x00020011, 1, 0x00020011, 55, 0x00020011, 56});
  auto format = [&](std::uint32_t id) {
    for (std::size_t i = 5; i < out.size(); i += out[i] >> 16)
      if ((out[i] & 0xffff) == 25 && out[i + 1] == id) return out[i + 8];
    return ~0u;
  };
  CHECK(format(2) == 0);  // now follows the bound view
  CHECK(format(3) == 1);  // %3 and %4 would become one duplicate type: unchanged
  CHECK(format(4) == 2);
  CHECK(format(5) == 3);  // atomic-capable format stays declared
  CHECK(format(6) == 0);
  CHECK(vulkan::formatless_storage_images(out).empty());
  CHECK(vulkan::formatless_storage_images({}).empty());
  // A declared format the host cannot access without a format stays declared.
  const std::uint32_t keep[] = {1};
  CHECK(vulkan::formatless_storage_images(code, keep).empty());
  CHECK(vulkan::storage_image_format(1) == VK_FORMAT_R32G32B32A32_SFLOAT);
  CHECK(vulkan::storage_image_format(8) == VK_FORMAT_B10G11R11_UFLOAT_PACK32);
  CHECK(vulkan::storage_image_format(41) == VK_FORMAT_R64_SINT);
  CHECK(vulkan::storage_image_format(42) == VK_FORMAT_UNDEFINED);
}
