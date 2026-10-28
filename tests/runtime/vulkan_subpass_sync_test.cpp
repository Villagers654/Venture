#include <doctest/doctest.h>

#include "hle/vulkan/internal.h"

using namespace venture;
namespace {
VkAttachmentDescription attachment(VkFormat format, VkImageLayout initial, VkImageLayout final_layout) {
  VkAttachmentDescription a{};
  a.format = format;
  a.samples = VK_SAMPLE_COUNT_1_BIT;
  a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  a.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  a.initialLayout = initial;
  a.finalLayout = final_layout;
  return a;
}
// The RE4 pattern: subpass 0 draws color 0 and depth 2; subpass 1 reads depth
// as an input attachment and resolves color 0 into 1. The guest's dependency
// covers only the input attachment read of a color write.
struct MobilePass {
  VkAttachmentDescription attachments[3]{
      attachment(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
      attachment(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      attachment(VK_FORMAT_D24_UNORM_S8_UINT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)};
  VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference resolve{1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depth{2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depth_input{2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
  VkSubpassDescription subpasses[2]{};
  VkSubpassDependency dependency{0,
                                 1,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                 VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
                                 VK_DEPENDENCY_BY_REGION_BIT};
  VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  MobilePass() {
    for (auto& s : subpasses) s.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpasses[0].colorAttachmentCount = 1;
    subpasses[0].pColorAttachments = &color;
    subpasses[0].pDepthStencilAttachment = &depth;
    subpasses[1].inputAttachmentCount = 1;
    subpasses[1].pInputAttachments = &depth_input;
    subpasses[1].colorAttachmentCount = 1;
    subpasses[1].pColorAttachments = &color;
    subpasses[1].pResolveAttachments = &resolve;
    info.attachmentCount = 3;
    info.pAttachments = attachments;
    info.subpassCount = 2;
    info.pSubpasses = subpasses;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
  }
};
}  // namespace

TEST_CASE("Vulkan render passes gain the subpass dependencies their attachment accesses need") {
  MobilePass pass;
  auto added = vulkan::missing_subpass_dependencies(pass.info);
  REQUIRE(added.size() == 1);
  const auto& d = added[0];
  CHECK(d.srcSubpass == 0);
  CHECK(d.dstSubpass == 1);
  CHECK(d.dependencyFlags == VK_DEPENDENCY_BY_REGION_BIT);
  // Late depth writes and color writes before the layout transition, the
  // input attachment read and the color resolve after it.
  CHECK((d.srcStageMask & VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT));
  CHECK((d.srcStageMask & VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT));
  CHECK(d.srcAccessMask ==
        (VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT));
  CHECK((d.dstStageMask & VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT));
  CHECK((d.dstStageMask & VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT));
  CHECK((d.dstAccessMask & VK_ACCESS_INPUT_ATTACHMENT_READ_BIT));
  CHECK((d.dstAccessMask & VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT));
  // Subpass 1 is the depth attachment's last use: its DONT_CARE store writes.
  CHECK((d.dstAccessMask & VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT));

  // With the completed set nothing more is missing.
  std::vector<VkSubpassDependency> all{pass.dependency, d};
  pass.info.dependencyCount = 2;
  pass.info.pDependencies = all.data();
  CHECK(vulkan::missing_subpass_dependencies(pass.info).empty());

  // A guest dependency over all commands and memory already covers them.
  VkSubpassDependency full{0, 1, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                           VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0};
  pass.info.dependencyCount = 1;
  pass.info.pDependencies = &full;
  CHECK(vulkan::missing_subpass_dependencies(pass.info).empty());

  // Dependencies from or to VK_SUBPASS_EXTERNAL do not order subpasses.
  VkSubpassDependency external{VK_SUBPASS_EXTERNAL, 1, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                               VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0};
  pass.info.pDependencies = &external;
  CHECK(vulkan::missing_subpass_dependencies(pass.info).size() == 1);
}
