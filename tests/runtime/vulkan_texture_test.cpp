#include <astcenc.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <random>
#include <set>
#include <stdexcept>
#include <string_view>

#include "gfx/gpu_device.h"
#include "gfx/pipeline_trace.h"
#include "hle/vulkan/texture.h"
#include "hle/vulkan/full_rate_image.h"
#include "hle/vulkan/internal.h"
#include "hle/vulkan/chain.h"
#include "vulkan_test_support.h"

using namespace venture;

TEST_CASE("Identity density maps used through KHR shading rate render full rate") {
  // Captured bad state: a stereo MSAA pass supplies the XR R8G8 density image
  // as a 16x16 KHR shading-rate attachment. Inspect translation without ever
  // creating this invalid native render pass or submitting it to a GPU.
  for (const auto format : {VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8_UINT}) {
    CAPTURE(format);
    VkAttachmentDescription2 attachments[4]{};
    for (auto& a : attachments) {
      a.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
      a.format = VK_FORMAT_R8G8B8A8_SRGB;
      a.samples = VK_SAMPLE_COUNT_4_BIT;
      a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[2].format = VK_FORMAT_D24_UNORM_S8_UINT;
    attachments[2].initialLayout = attachments[2].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[3].format = format;
    attachments[3].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[3].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[3].initialLayout = attachments[3].finalLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
    VkAttachmentReference2 color{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference2 resolve{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference2 depth{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference2 rate{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 3, VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR};
    VkSubpassDescriptionDepthStencilResolve tail{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
    VkFragmentShadingRateAttachmentInfoKHR shading{VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR, &tail, &rate, {16, 16}};
    VkSubpassDescription2 subpasses[2]{};
    for (auto& s : subpasses) {
      s.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
      s.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      s.viewMask = 3;
      s.colorAttachmentCount = 1;
      s.pColorAttachments = &color;
      s.pDepthStencilAttachment = &depth;
    }
    subpasses[0].pNext = &shading;
    subpasses[1].pResolveAttachments = &resolve;
    subpasses[1].inputAttachmentCount = 1;
    subpasses[1].pInputAttachments = &depth;
    VkRenderPassCreateInfo2 pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
    pass.attachmentCount = 4; pass.pAttachments = attachments;
    pass.subpassCount = 2; pass.pSubpasses = subpasses;
    CHECK_FALSE(vulkan::borrowable_chain(&pass));
    vulkan::Chain chain;
    const auto* host = static_cast<const VkRenderPassCreateInfo2*>(chain.translate(&pass));
    REQUIRE(host);
    REQUIRE(chain.result() == VK_SUCCESS);
    CHECK(host->attachmentCount == 4);
    CHECK(host->pSubpasses[1].pResolveAttachments->attachment == 1);
    CHECK(host->pSubpasses[1].pInputAttachments->attachment == 2);
    CHECK(host->pSubpasses[0].viewMask == 3);
    const auto* node = static_cast<const VkBaseInStructure*>(host->pSubpasses[0].pNext);
    REQUIRE(node);
    if (format == VK_FORMAT_R8G8_UNORM) {
      CHECK(node->sType == VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE);
      CHECK(host->pAttachments[3].initialLayout == VK_IMAGE_LAYOUT_GENERAL);
      CHECK(host->pAttachments[3].finalLayout == VK_IMAGE_LAYOUT_GENERAL);
    } else {
      CHECK(node->sType == VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR);
      CHECK(host->pAttachments[3].finalLayout == VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR);
      const auto* kept = reinterpret_cast<const VkFragmentShadingRateAttachmentInfoKHR*>(node);
      CHECK(kept->pFragmentShadingRateAttachment->attachment == 3);
      REQUIRE(node->pNext);
      CHECK(node->pNext->sType == VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE);
    }
    CHECK(attachments[3].finalLayout == VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR);
    CHECK(subpasses[0].pNext == &shading);
    CHECK(shading.pNext == &tail);
    CHECK(shading.pFragmentShadingRateAttachment == &rate);
  }
}

TEST_CASE("Guest D24 depth reaches hosts without D24 attachments as 32-bit float depth") {
  // HITMAN 3 creates D24_UNORM_S8_UINT depth; RADV attaches no D24 format.
  test::VulkanDevice device;
  auto attaches = [&](VkFormat format) {
    // The host's answer: a guest query is itself translated.
    VkFormatProperties properties{};
    reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(vulkan::raw_physical_proc(
        device.physical, "vkGetPhysicalDeviceFormatProperties"))(device.physical, format, &properties);
    return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
  };
  auto expected = [&](VkFormat guest, VkFormat wide) { return attaches(guest) || !attaches(wide) ? guest : wide; };
  const auto d24s8 = expected(VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT);
  const auto x8d24 = expected(VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT);
  CHECK(vulkan::host_depth_format(device.physical, VK_FORMAT_D24_UNORM_S8_UINT) == d24s8);
  CHECK(vulkan::host_depth_format(device.physical, VK_FORMAT_X8_D24_UNORM_PACK32) == x8d24);
  CHECK(vulkan::host_depth_format(device.physical, VK_FORMAT_D16_UNORM) == VK_FORMAT_D16_UNORM);
  CHECK(vulkan::host_depth_format(VK_NULL_HANDLE, VK_FORMAT_D24_UNORM_S8_UINT) == VK_FORMAT_D24_UNORM_S8_UINT);

  const VkFormat views[] = {VK_FORMAT_D24_UNORM_S8_UINT};
  VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, nullptr, 1, views};
  VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &list};
  image.imageType = VK_IMAGE_TYPE_2D;
  image.format = VK_FORMAT_D24_UNORM_S8_UINT;
  image.extent = {64, 64, 1};
  image.mipLevels = image.arrayLayers = 1;
  image.samples = VK_SAMPLE_COUNT_1_BIT;
  image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  CHECK_FALSE(vulkan::borrowable_chain(&image));
  vulkan::Chain chain;
  chain.set_device(device.device);
  const auto* host = static_cast<const VkImageCreateInfo*>(chain.translate(&image));
  REQUIRE(host);
  CHECK(chain.result() == VK_SUCCESS);
  CHECK(host->format == d24s8);
  const auto* host_list = static_cast<const VkImageFormatListCreateInfo*>(host->pNext);
  REQUIRE(host_list);
  CHECK(host_list->pViewFormats[0] == d24s8);
  CHECK(image.format == VK_FORMAT_D24_UNORM_S8_UINT);
  CHECK(views[0] == VK_FORMAT_D24_UNORM_S8_UINT);

  VkAttachmentDescription attachment{};
  attachment.format = VK_FORMAT_X8_D24_UNORM_PACK32;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  pass.attachmentCount = 1;
  pass.pAttachments = &attachment;
  vulkan::Chain passes;
  passes.set_device(device.device);
  const auto* host_pass = static_cast<const VkRenderPassCreateInfo*>(passes.translate(&pass));
  REQUIRE(host_pass);
  CHECK(host_pass->pAttachments[0].format == x8d24);

  VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.depthAttachmentFormat = rendering.stencilAttachmentFormat = VK_FORMAT_D24_UNORM_S8_UINT;
  vulkan::Chain pipelines;
  pipelines.set_physical(device.physical);
  const auto* host_rendering = static_cast<const VkPipelineRenderingCreateInfo*>(pipelines.translate(&rendering));
  REQUIRE(host_rendering);
  CHECK(host_rendering->depthAttachmentFormat == d24s8);
  CHECK(host_rendering->stencilAttachmentFormat == d24s8);

  // Format queries answer for the substituted format under the guest's name.
  VkFormat queried = VK_FORMAT_D24_UNORM_S8_UINT;
  pipelines.translate_format(queried);
  CHECK(queried == d24s8);
  // Other formats keep borrowing the guest's structures untouched.
  image.pNext = nullptr;
  image.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
  CHECK(vulkan::borrowable_chain(&image));
}

TEST_SUITE_BEGIN("gpu");

static bool native_astc(VkPhysicalDevice physical) {
  VkFormatProperties properties{};
  reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
      vulkan::raw_physical_proc(physical, "vkGetPhysicalDeviceFormatProperties"))(
      physical, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, &properties);
  return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
}

TEST_CASE("Vulkan GPU ASTC decode is bit-exact to astcenc for all LDR footprints and sRGB") {
  test::VulkanDevice gpu;
  constexpr std::uint32_t width = 31, height = 23;
  std::vector<std::uint8_t> rgba(width * height * 4);
  std::mt19937 random(812);
  for (std::uint32_t y = 0; y < height; ++y)
    for (std::uint32_t x = 0; x < width; ++x) {
      auto i = (y * width + x) * 4;
      rgba[i] = (x * 7 + y * 3) & 255;
      rgba[i + 1] = (y * 11) & 255;
      rgba[i + 2] = random() & 255;
      rgba[i + 3] = (x * 3 + y * 13) & 255;
    }
  astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
  for (int value = VK_FORMAT_ASTC_4x4_UNORM_BLOCK; value <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK; ++value) for (bool float16 : {false, true}) {
    auto format = vulkan::texture_format(static_cast<VkFormat>(value));
    bool srgb = format.decoded == VK_FORMAT_R8G8B8A8_SRGB;
    if (float16 && srgb) continue;
    if (float16) { format.decoded = VK_FORMAT_R16G16B16A16_SFLOAT; format.texel_bytes = 8; }
    CAPTURE(float16);
    CAPTURE(value);
    CAPTURE(format.width);
    CAPTURE(format.height);
    astcenc_config config{};
    REQUIRE(astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, format.width, format.height, 1,
                                ASTCENC_PRE_MEDIUM, 0, &config) == ASTCENC_SUCCESS);
    astcenc_context* raw{};
    REQUIRE(astcenc_context_alloc(&config, 1, &raw) == ASTCENC_SUCCESS);
    std::unique_ptr<astcenc_context, decltype(&astcenc_context_free)> context(raw, astcenc_context_free);
    void* input = rgba.data();
    astcenc_image in{width, height, 1, ASTCENC_TYPE_U8, &input};
    std::size_t compressed_size =
        ((width + format.width - 1) / format.width) * ((height + format.height - 1) / format.height) * 16;
    test::VulkanBuffer source(gpu, compressed_size), dest(gpu, width * height * format.texel_bytes);
    REQUIRE(astcenc_compress_image(raw, &in, &swizzle, static_cast<std::uint8_t*>(source.mapped),
                                   compressed_size, 0) == ASTCENC_SUCCESS);
    std::vector<std::uint8_t> expected(dest.size);
    void* output = expected.data();
    astcenc_image out{width, height, 1, float16 ? ASTCENC_TYPE_F16 : ASTCENC_TYPE_U8, &output};
    REQUIRE(astcenc_decompress_image(raw, static_cast<const std::uint8_t*>(source.mapped), compressed_size,
                                     &out, &swizzle, 0) == ASTCENC_SUCCESS);
    gpu.begin();
    auto resources =
        vulkan::record_decode(gpu.device, gpu.command, source.buffer, source.size, dest.buffer, dest.size,
                              {format, width, height, (width + format.width - 1) / format.width, 0});
    gpu.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                VK_ACCESS_HOST_READ_BIT);
    gpu.submit();
    auto* actual = static_cast<std::uint8_t*>(dest.mapped);
    std::size_t mismatches = 0, first = 0;
    for (std::size_t i = 0; i < expected.size(); ++i)
      if (expected[i] != actual[i]) {
        if (!mismatches) first = i;
        ++mismatches;
      }
    CAPTURE(first);
    CAPTURE(expected[first]);
    CAPTURE(actual[first]);
    CHECK(mismatches == 0);
    REQUIRE(test::vk_call("vkResetCommandBuffer", gpu.command, 0u) == VK_SUCCESS);
  }
}

TEST_CASE("Vulkan ETC2 EAC GPU decoder covers alpha signed and sRGB formats") {
  test::VulkanDevice gpu;
  for (int value = VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK; value <= VK_FORMAT_EAC_R11G11_SNORM_BLOCK; ++value) {
    CAPTURE(value);
    auto format = vulkan::texture_format(static_cast<VkFormat>(value));
    test::VulkanBuffer source(gpu, format.block_bytes), output(gpu, 16 * format.texel_bytes);
    std::memset(source.mapped, 0, source.size);
    gpu.begin();
    auto resources = vulkan::record_decode(gpu.device, gpu.command, source.buffer, source.size, output.buffer,
                                           output.size, {format, 4, 4, 1, 0});
    gpu.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                VK_ACCESS_HOST_READ_BIT);
    gpu.submit();
    if (value >= VK_FORMAT_EAC_R11_UNORM_BLOCK) {
      bool signed_format = (value & 1) == 0;
      float expected = signed_format ? -3.f / 1023.f : 1.f / 2047.f;
      auto* actual = static_cast<float*>(output.mapped);
      for (std::size_t i = 0; i < output.size / sizeof(float); ++i)
        CHECK(actual[i] == doctest::Approx(expected).epsilon(0.000001));
    } else {
      auto* actual = static_cast<std::uint8_t*>(output.mapped);
      auto rgb =
          value == VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK || value == VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK ? 0 : 2;
      auto alpha = value >= VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK ? 0 : 255;
      for (int i = 0; i < 16; ++i) {
        CHECK(actual[i * 4] == rgb);
        CHECK(actual[i * 4 + 1] == rgb);
        CHECK(actual[i * 4 + 2] == rgb);
        CHECK(actual[i * 4 + 3] == alpha);
      }
    }
    REQUIRE(test::vk_call("vkResetCommandBuffer", gpu.command, 0u) == VK_SUCCESS);
  }
}

namespace {
// BC7 mode 6 reference decode (the only mode the encoder writes).
std::array<std::uint8_t, 64> decode_bc7_mode6(const std::uint8_t* block) {
  unsigned position = 0;
  auto bits = [&](unsigned count) {
    unsigned value = 0;
    for (unsigned i = 0; i < count; ++i, ++position) value |= ((block[position / 8] >> (position % 8)) & 1u) << i;
    return value;
  };
  REQUIRE(bits(7) == 64);
  unsigned endpoints[2][4];
  for (auto& channel : {0, 1, 2, 3})
    for (auto& end : endpoints) end[channel] = bits(7) << 1;
  for (auto& end : endpoints) {
    const auto p = bits(1);
    for (auto& c : end) c |= p;
  }
  constexpr unsigned weights[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
  std::array<std::uint8_t, 64> out{};
  for (unsigned i = 0; i < 16; ++i) {
    const auto w = weights[bits(i ? 4 : 3)];
    for (unsigned c = 0; c < 4; ++c)
      out[i * 4 + c] = std::uint8_t(((64 - w) * endpoints[0][c] + w * endpoints[1][c] + 32) >> 6);
  }
  return out;
}
}  // namespace

TEST_CASE("Vulkan texture storage for memory pressure maps 8-bit planes to BC7") {
  const auto astc = vulkan::texture_storage(VK_FORMAT_ASTC_6x6_SRGB_BLOCK, true, {}, true);
  REQUIRE(astc.planes.size() == 2);
  CHECK(astc.planes[0].format == VK_FORMAT_BC7_SRGB_BLOCK);
  CHECK(astc.planes[1].format == VK_FORMAT_BC7_UNORM_BLOCK);
  CHECK(vulkan::texture_view(astc, VK_FORMAT_ASTC_6x6_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM)->plane == 1);
  const auto etc2 = vulkan::texture_storage(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, true, {}, true);
  REQUIRE(etc2.planes.size() == 1);
  CHECK(etc2.planes[0].format == VK_FORMAT_BC7_UNORM_BLOCK);
  CHECK(vulkan::texture_view(etc2, VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK)->format == VK_FORMAT_BC7_SRGB_BLOCK);
  const auto eac = vulkan::texture_storage(VK_FORMAT_EAC_R11G11_UNORM_BLOCK, false, {}, true);
  CHECK(eac.planes[0].format == VK_FORMAT_R32G32_SFLOAT);
  CHECK(vulkan::plane_bytes(astc.planes[0], 61, 37) == 16 * 10 * 16);
  CHECK(vulkan::plane_bytes(eac.planes[0], 61, 37) == 61 * 37 * 8);
}

TEST_CASE("Vulkan emulated textures under memory pressure are stored as BC7 close to the RGBA8 decode") {
  struct Force {
    Force() { vulkan::force_texture_emulation(true); vulkan::force_texture_compression(1); }
    ~Force() { vulkan::force_texture_compression(-1); vulkan::force_texture_emulation(false); }
  } force;
  test::VulkanDevice gpu;
  VkPhysicalDeviceFeatures features{};
  test::vk_call("vkGetPhysicalDeviceFeatures", gpu.physical, &features);
  if (!features.textureCompressionBC) {
    MESSAGE("host has no BC textures");
    return;
  }
  constexpr std::uint32_t width = 61, height = 37;
  std::vector<std::uint8_t> rgba(width * height * 4);
  std::mt19937 random(4417);
  for (std::uint32_t y = 0; y < height; ++y)
    for (std::uint32_t x = 0; x < width; ++x) {
      auto i = (y * width + x) * 4;
      rgba[i] = (x * 4 + y) & 255;
      rgba[i + 1] = (y * 6 + (random() & 15)) & 255;
      rgba[i + 2] = 128 + int((random() & 31)) - 16;
      rgba[i + 3] = x < width / 2 ? 255 : (x * 3 + y * 5) & 255;
    }
  astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
  auto blit = reinterpret_cast<PFN_vkCmdBlitImage>(vulkan::raw_device_proc(gpu.device, "vkCmdBlitImage"));
  auto read = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(
      vulkan::raw_device_proc(gpu.device, "vkCmdCopyImageToBuffer"));
  for (auto format : {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_6x6_SRGB_BLOCK, VK_FORMAT_ASTC_8x8_UNORM_BLOCK}) {
    CAPTURE(int(format));
    const auto info = vulkan::texture_format(format);
    const bool srgb = info.decoded == VK_FORMAT_R8G8B8A8_SRGB;
    astcenc_config config{};
    REQUIRE(astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, info.width, info.height, 1,
                                ASTCENC_PRE_FAST, 0, &config) == ASTCENC_SUCCESS);
    astcenc_context* raw{};
    REQUIRE(astcenc_context_alloc(&config, 1, &raw) == ASTCENC_SUCCESS);
    std::unique_ptr<astcenc_context, decltype(&astcenc_context_free)> context(raw, astcenc_context_free);
    void* input = rgba.data();
    astcenc_image in{width, height, 1, ASTCENC_TYPE_U8, &input};
    const auto row_blocks = (width + info.width - 1) / info.width;
    const std::size_t compressed_size = row_blocks * ((height + info.height - 1) / info.height) * 16;
    test::VulkanBuffer upload(gpu, compressed_size), reference(gpu, width * height * 4);
    REQUIRE(astcenc_compress_image(raw, &in, &swizzle, static_cast<std::uint8_t*>(upload.mapped),
                                   compressed_size, 0) == ASTCENC_SUCCESS);
    constexpr std::uint32_t blocks = 16 * 10;
    test::VulkanBuffer encoded(gpu, blocks * 16), hardware(gpu, width * height * 4);
    test::VulkanImage texture(gpu, format, width, height);
    // An sRGB destination keeps the blit from converting to linear; it may round by one.
    test::VulkanImage decompressed(gpu, srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, width, height);
    const auto plane = vulkan::emulated_texture_planes(texture.image).at(0);
    VkFormatProperties bc7{};
    test::vk_call("vkGetPhysicalDeviceFormatProperties", gpu.physical, VK_FORMAT_BC7_UNORM_BLOCK, &bc7);
    const bool blit_supported = bc7.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT;
    gpu.begin();
    texture.transition();
    decompressed.transition();
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    test::vk_call("vkCmdCopyBufferToImage", gpu.command, upload.buffer, texture.image, VK_IMAGE_LAYOUT_GENERAL,
                  1u, &copy);
    auto resources = vulkan::record_decode(gpu.device, gpu.command, upload.buffer, upload.size, reference.buffer,
                                           reference.size, {info, width, height, row_blocks, 0});
    gpu.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
    read(gpu.command, plane, VK_IMAGE_LAYOUT_GENERAL, encoded.buffer, 1, &copy);
    if (blit_supported) {
      VkImageBlit region{};
      region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.srcOffsets[1] = region.dstOffsets[1] = {std::int32_t(width), std::int32_t(height), 1};
      blit(gpu.command, plane, VK_IMAGE_LAYOUT_GENERAL, decompressed.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region,
           VK_FILTER_NEAREST);
      gpu.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_TRANSFER_READ_BIT);
      read(gpu.command, decompressed.image, VK_IMAGE_LAYOUT_GENERAL, hardware.buffer, 1, &copy);
    }
    gpu.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                VK_ACCESS_HOST_READ_BIT);
    gpu.submit();
    const auto* expected = static_cast<const std::uint8_t*>(reference.mapped);
    const auto* bytes = static_cast<const std::uint8_t*>(encoded.mapped);
    const auto* sampled = static_cast<const std::uint8_t*>(hardware.mapped);
    double squared = 0;
    int worst = 0;
    std::size_t hardware_mismatches = 0;
    for (std::uint32_t by = 0; by < 10; ++by)
      for (std::uint32_t bx = 0; bx < 16; ++bx) {
        const auto texels = decode_bc7_mode6(bytes + (by * 16 + bx) * 16);
        for (std::uint32_t i = 0; i < 16; ++i) {
          const auto x = bx * 4 + i % 4, y = by * 4 + i / 4;
          if (x >= width || y >= height) continue;
          for (std::uint32_t c = 0; c < 4; ++c) {
            const auto at = (y * width + x) * 4 + c;
            const int error = int(texels[i * 4 + c]) - int(expected[at]);
            squared += double(error) * error;
            worst = std::max(worst, std::abs(error));
            if (blit_supported && std::abs(int(sampled[at]) - int(texels[i * 4 + c])) > (srgb ? 1 : 0))
              ++hardware_mismatches;
          }
        }
      }
    const double psnr = 10 * std::log10(255.0 * 255.0 / (squared / (width * height * 4)));
    MESSAGE("BC7 vs RGBA8 decode: format ", int(format), " PSNR ", psnr, " dB, worst ", worst);
    // Noisy, weakly correlated channels: the hard case for single-subset BC7.
    CHECK(psnr > 35);
    CHECK(worst <= 32);
    CHECK(hardware_mismatches == 0);
    REQUIRE(test::vk_call("vkResetCommandBuffer", gpu.command, 0u) == VK_SUCCESS);
  }
}

TEST_CASE("Vulkan BC7 encode GPU time against the RGBA8 decode (VENTURE_BC7_BENCH)") {
  if (!std::getenv("VENTURE_BC7_BENCH")) return;
  test::VulkanDevice gpu;
  constexpr std::uint32_t size = 2048;
  const auto info = vulkan::texture_format(VK_FORMAT_ASTC_6x6_UNORM_BLOCK);
  std::vector<std::uint8_t> rgba(size * size * 4);
  std::mt19937 random(9);
  for (std::uint32_t y = 0; y < size; ++y)
    for (std::uint32_t x = 0; x < size; ++x) {
      auto i = (y * size + x) * 4;
      rgba[i] = (x + (random() & 7)) & 255; rgba[i + 1] = (y / 3) & 255;
      rgba[i + 2] = ((x ^ y) >> 2) & 255; rgba[i + 3] = 255;
    }
  astcenc_config config{};
  REQUIRE(astcenc_config_init(ASTCENC_PRF_LDR, 6, 6, 1, ASTCENC_PRE_FASTEST, 0, &config) == ASTCENC_SUCCESS);
  astcenc_context* raw{};
  REQUIRE(astcenc_context_alloc(&config, 1, &raw) == ASTCENC_SUCCESS);
  std::unique_ptr<astcenc_context, decltype(&astcenc_context_free)> context(raw, astcenc_context_free);
  astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
  void* input = rgba.data();
  astcenc_image in{size, size, 1, ASTCENC_TYPE_U8, &input};
  const auto row_blocks = (size + 5) / 6;
  const std::size_t compressed_size = row_blocks * row_blocks * 16;
  test::VulkanBuffer source(gpu, compressed_size), rgba8(gpu, size * size * 4), bc7(gpu, size * size);
  REQUIRE(astcenc_compress_image(raw, &in, &swizzle, static_cast<std::uint8_t*>(source.mapped), compressed_size,
                                 0) == ASTCENC_SUCCESS);
  VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
  qi.queryCount = 4;
  VkQueryPool pool{};
  REQUIRE(test::vk_call("vkCreateQueryPool", gpu.device, &qi, nullptr, &pool) == VK_SUCCESS);
  VkPhysicalDeviceProperties props{};
  test::vk_call("vkGetPhysicalDeviceProperties", gpu.physical, &props);
  double best[2] = {1e9, 1e9};
  for (int run = 0; run < 8; ++run) {
    gpu.begin();
    test::vk_call("vkCmdResetQueryPool", gpu.command, pool, 0u, 4u);
    auto decoded = info;
    test::vk_call("vkCmdWriteTimestamp", gpu.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, 0u);
    auto a = vulkan::record_decode(gpu.device, gpu.command, source.buffer, source.size, rgba8.buffer, rgba8.size,
                                   {decoded, size, size, row_blocks, 0});
    test::vk_call("vkCmdWriteTimestamp", gpu.command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, 1u);
    gpu.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT);
    test::vk_call("vkCmdWriteTimestamp", gpu.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, 2u);
    auto b = vulkan::record_encode_bc7(gpu.device, gpu.command, rgba8.buffer, rgba8.size, bc7.buffer, bc7.size,
                                       size, size);
    test::vk_call("vkCmdWriteTimestamp", gpu.command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, 3u);
    gpu.submit();
    std::uint64_t t[4]{};
    REQUIRE(test::vk_call("vkGetQueryPoolResults", gpu.device, pool, 0u, 4u, sizeof(t), t, sizeof(std::uint64_t),
                          VkQueryResultFlags(VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT)) == VK_SUCCESS);
    best[0] = std::min(best[0], double(t[1] - t[0]) * props.limits.timestampPeriod / 1e6);
    best[1] = std::min(best[1], double(t[3] - t[2]) * props.limits.timestampPeriod / 1e6);
    REQUIRE(test::vk_call("vkResetCommandBuffer", gpu.command, 0u) == VK_SUCCESS);
  }
  MESSAGE(props.deviceName, ": 2048x2048 ASTC 6x6 -> RGBA8 ", best[0], " ms, RGBA8 -> BC7 ", best[1], " ms");
  test::vk_call("vkDestroyQueryPool", gpu.device, pool, nullptr);
}

TEST_CASE("Vulkan pageable compressed sources retain replay and restore partial updates exactly") {
  struct Force {
    Force() { vulkan::force_texture_emulation(true); vulkan::force_pageable_texture_sources(true); }
    ~Force() { vulkan::force_pageable_texture_sources(false); vulkan::force_texture_emulation(false); }
  } force;
  test::VulkanDevice gpu;
  vulkan::ensure_texture_source_test_queue(gpu.device, gpu.queue, gpu.family);
  test::VulkanBuffer upload(gpu, 64), raw(gpu, 64), pixels(gpu, 8 * 8 * 4);
  test::VulkanImage image(gpu, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, 8, 8);
  const std::array<std::uint8_t, 16> red{0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                      0xff, 0xff, 0, 0, 0, 0, 0xff, 0xff};
  auto green = red;
  green[8] = green[9] = 0; green[10] = green[11] = 255;
  for (int i = 0; i < 4; ++i) std::memcpy(static_cast<char*>(upload.mapped) + i * 16, red.data(), 16);
  VkBufferImageCopy full{};
  full.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  full.imageExtent = {8, 8, 1};
  auto read = [&] {
    test::vk_call("vkCmdCopyImageToBuffer", gpu.command, image.image, VK_IMAGE_LAYOUT_GENERAL,
                  raw.buffer, 1u, &full);
    auto native = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(
        vulkan::raw_device_proc(gpu.device, "vkCmdCopyImageToBuffer"));
    gpu.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    native(gpu.command, image.image, VK_IMAGE_LAYOUT_GENERAL, pixels.buffer, 1, &full);
    gpu.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
  };
  auto trim = [&] {
    vulkan::trim_texture_sources(gpu.device);
    vulkan::wait_texture_sources(gpu.device);
  };
  gpu.begin(); image.transition();
  test::vk_call("vkCmdCopyBufferToImage", gpu.command, upload.buffer, image.image,
                VK_IMAGE_LAYOUT_GENERAL, 1u, &full);
  read();
  // Even an unsubmitted command owns its source: retirement cannot invalidate it.
  trim(); CHECK(vulkan::texture_source_gpu_resident(image.image));
  gpu.submit();
  trim(); CHECK(vulkan::texture_source_gpu_resident(image.image));
  // Replay consumes changed GPU input, rather than a stale CPU snapshot.
  std::memcpy(upload.mapped, green.data(), 16);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &gpu.command;
  REQUIRE(test::vk_call("vkQueueSubmit", gpu.queue, 1u, &si, VK_NULL_HANDLE) == VK_SUCCESS);
  REQUIRE(test::vk_call("vkQueueWaitIdle", gpu.queue) == VK_SUCCESS);
  CHECK(std::memcmp(raw.mapped, upload.mapped, 64) == 0);
  REQUIRE(test::vk_call("vkResetCommandBuffer", gpu.command, 0u) == VK_SUCCESS);
  trim(); CHECK_FALSE(vulkan::texture_source_gpu_resident(image.image));
  // A later partial upload restores all untouched blocks from pageable storage.
  gpu.begin();
  VkBufferImageCopy partial = full; partial.imageOffset = {4, 4, 0}; partial.imageExtent = {4, 4, 1};
  test::vk_call("vkCmdCopyBufferToImage", gpu.command, upload.buffer, image.image,
                VK_IMAGE_LAYOUT_GENERAL, 1u, &partial);
  read(); gpu.submit();
  std::array<std::uint8_t, 64> expected{};
  for (int i = 0; i < 4; ++i) std::memcpy(expected.data() + i * 16, i == 0 || i == 3 ? green.data() : red.data(), 16);
  CHECK(std::memcmp(raw.mapped, expected.data(), expected.size()) == 0);
  auto* rgba = static_cast<std::uint8_t*>(pixels.mapped);
  for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
    const int channel = (x < 4) == (y < 4) ? 1 : 0;
    for (int c = 0; c < 4; ++c) CHECK(rgba[(y * 8 + x) * 4 + c] == (c == channel || c == 3 ? 255 : 0));
  }
  REQUIRE(test::vk_call("vkResetCommandBuffer", gpu.command, 0u) == VK_SUCCESS);
  trim(); CHECK_FALSE(vulkan::texture_source_gpu_resident(image.image));
  // Pure readback also restores exact blocks without a new upload.
  gpu.begin(); read(); gpu.submit();
  CHECK(std::memcmp(raw.mapped, expected.data(), expected.size()) == 0);
  REQUIRE(test::vk_call("vkResetCommandBuffer", gpu.command, 0u) == VK_SUCCESS);
  trim(); CHECK_FALSE(vulkan::texture_source_gpu_resident(image.image));
  // A copy into another emulated image consumes the retained source exactly.
  test::VulkanImage copied(gpu, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, 8, 8);
  gpu.begin(); copied.transition();
  VkImageCopy copy{}; copy.srcSubresource = copy.dstSubresource = full.imageSubresource;
  copy.extent = full.imageExtent;
  test::vk_call("vkCmdCopyImage", gpu.command, image.image, VK_IMAGE_LAYOUT_GENERAL,
                copied.image, VK_IMAGE_LAYOUT_GENERAL, 1u, &copy);
  test::vk_call("vkCmdCopyImageToBuffer", gpu.command, copied.image, VK_IMAGE_LAYOUT_GENERAL,
                raw.buffer, 1u, &full);
  gpu.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
  gpu.submit();
  CHECK(std::memcmp(raw.mapped, expected.data(), expected.size()) == 0);
  REQUIRE(test::vk_call("vkResetCommandBuffer", gpu.command, 0u) == VK_SUCCESS);
  { test::VulkanImage never_uploaded(gpu, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, 8, 8); }
  vulkan::trim_texture_sources(gpu.device);
  vulkan::wait_texture_sources(gpu.device);
}

TEST_CASE("Vulkan GPU-produced strided partial ASTC uploads copy mips layers and replay") {
  test::VulkanDevice gpu;
  test::VulkanBuffer upload(gpu, 2048), produced(gpu, 2048), readback(gpu, 160), decoded(gpu, 16 * 8 * 8);
  test::VulkanImage a(gpu, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, 16, 8, 2, 2),
      b(gpu, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, 16, 8, 2, 1);
  const std::array<std::uint8_t, 16> red{0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                         0xff, 0xff, 0,    0,    0,    0,    0xff, 0xff};
  auto green = red;
  green[8] = green[9] = 0;
  green[10] = green[11] = 255;
  auto blue = red;
  blue[8] = blue[9] = 0;
  blue[12] = blue[13] = 255;
  auto* bytes = static_cast<std::uint8_t*>(upload.mapped);
  std::memset(bytes, 0, upload.size);
  for (int i = 0; i < 16; ++i) std::memcpy(bytes + i * 16, blue.data(), 16);
  std::memcpy(bytes + 512, red.data(), 16);
  std::memcpy(bytes + 528, red.data(), 16);
  std::memcpy(bytes + 592, green.data(), 16);
  std::memcpy(bytes + 608, green.data(), 16);
  std::memcpy(bytes + 1024, red.data(), 16);
  std::memcpy(bytes + 1040, red.data(), 16);
  gpu.begin();
  a.transition();
  b.transition();
  VkBufferCopy buffer_copy{0, 0, 2048};
  test::vk_call("vkCmdCopyBuffer", gpu.command, upload.buffer, produced.buffer, 1u, &buffer_copy);
  gpu.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_ACCESS_TRANSFER_READ_BIT);
  VkBufferImageCopy regions[3]{};
  regions[0].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 2};
  regions[0].imageExtent = {16, 8, 1};
  regions[1].bufferOffset = 512;
  regions[1].bufferRowLength = 20;
  regions[1].bufferImageHeight = 12;
  regions[1].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1};
  regions[1].imageOffset = {4, 0, 0};
  regions[1].imageExtent = {8, 8, 1};
  regions[2].bufferOffset = 1024;
  regions[2].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 1};
  regions[2].imageExtent = {8, 4, 1};
  test::vk_call("vkCmdCopyBufferToImage", gpu.command, produced.buffer, a.image, VK_IMAGE_LAYOUT_GENERAL, 3u,
                regions);
  VkImageCopy image_copies[2]{};
  image_copies[0].srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1};
  image_copies[0].dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  image_copies[0].extent = {16, 8, 1};
  image_copies[1].srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 1};
  image_copies[1].dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1};
  image_copies[1].extent = {8, 4, 1};
  test::vk_call("vkCmdCopyImage", gpu.command, a.image, VK_IMAGE_LAYOUT_GENERAL, b.image,
                VK_IMAGE_LAYOUT_GENERAL, 2u, image_copies);
  VkBufferImageCopy reads[2]{};
  reads[0].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  reads[0].imageExtent = {16, 8, 1};
  reads[1].bufferOffset = 128;
  reads[1].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1};
  reads[1].imageExtent = {8, 4, 1};
  test::vk_call("vkCmdCopyImageToBuffer", gpu.command, b.image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 2u,
                reads);
  gpu.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_ACCESS_TRANSFER_READ_BIT);
  reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(vulkan::raw_device_proc(gpu.device, "vkCmdCopyImageToBuffer"))(
      gpu.command, b.image, VK_IMAGE_LAYOUT_GENERAL, decoded.buffer, 1, reads);
  gpu.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
              VK_ACCESS_HOST_READ_BIT);
  gpu.submit();
  auto verify = [&](bool replay) {
    auto* raw = static_cast<std::uint8_t*>(readback.mapped);
    for (int i = 0; i < 10; ++i) {
      auto& expected = i == 1 || i == 2   ? (replay ? green : red)
                       : i == 5 || i == 6 ? green
                       : i >= 8           ? red
                                          : blue;
      CHECK(std::memcmp(raw + i * 16, expected.data(), 16) == 0);
    }
    auto* pixels = static_cast<std::uint8_t*>(decoded.mapped);
    if (native_astc(gpu.physical)) {
      CHECK(std::memcmp(raw, decoded.mapped, 128) == 0);
      return;
    }
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 16; ++x) {
        int channel = x < 4 || x >= 12 ? 2 : y < 4 && !replay ? 0 : 1;
        for (int c = 0; c < 4; ++c)
          CHECK(pixels[(y * 16 + x) * 4 + c] == ((c == channel || c == 3) ? 255 : 0));
      }
  };
  verify(false);
  // No re-recording: the original command buffer must consume new producer
  // bytes at each submission, rather than retaining a CPU-decoded snapshot.
  std::memcpy(bytes + 512, green.data(), 16);
  std::memcpy(bytes + 528, green.data(), 16);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &gpu.command;
  REQUIRE(test::vk_call("vkQueueSubmit", gpu.queue, 1u, &si, VK_NULL_HANDLE) == VK_SUCCESS);
  REQUIRE(test::vk_call("vkQueueWaitIdle", gpu.queue) == VK_SUCCESS);
  verify(true);
  REQUIRE(test::vk_call("vkResetCommandBuffer", gpu.command, 0u) == VK_SUCCESS);
}

TEST_CASE("Vulkan emulated mutable compressed images back every view format of their class") {
  struct ForceEmulation {
    ForceEmulation() { vulkan::force_texture_emulation(true); vulkan::force_pageable_texture_sources(true); }
    ~ForceEmulation() { vulkan::force_pageable_texture_sources(false); vulkan::force_texture_emulation(false); }
  } force;
  test::VulkanDevice gpu;
  vulkan::ensure_texture_source_test_queue(gpu.device, gpu.queue, gpu.family);
  VkImageFormatProperties limits{};
  CHECK(test::vk_call("vkGetPhysicalDeviceImageFormatProperties", gpu.physical, VK_FORMAT_ASTC_6x6_SRGB_BLOCK,
                      VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, &limits) == VK_SUCCESS);
  // Views of raw blocks would need the compressed bytes in the image itself.
  CHECK(static_cast<std::int32_t>(test::vk_call(
            "vkGetPhysicalDeviceImageFormatProperties", gpu.physical, VK_FORMAT_ASTC_6x6_SRGB_BLOCK,
            VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT, &limits)) ==
        VK_ERROR_FORMAT_NOT_SUPPORTED);

  constexpr std::uint32_t width = 12, height = 12;
  std::vector<std::uint8_t> rgba(width * height * 4);
  for (std::uint32_t i = 0; i < width * height; ++i) {
    rgba[i * 4] = (i * 37) & 255;
    rgba[i * 4 + 1] = (i * 11 + 64) & 255;
    rgba[i * 4 + 2] = 255 - ((i * 5) & 255);
    rgba[i * 4 + 3] = (i * 23 + 9) & 255;
  }
  auto read_image = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(
      vulkan::raw_device_proc(gpu.device, "vkCmdCopyImageToBuffer"));
  std::mt19937 random(4421);
  const VkFormat astc_views[] = {VK_FORMAT_ASTC_6x6_UNORM_BLOCK, VK_FORMAT_ASTC_6x6_SRGB_BLOCK};
  VkImageFormatListCreateInfo astc_list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
  astc_list.viewFormatCount = 2;
  astc_list.pViewFormats = astc_views;
  struct Case { VkFormat format; const VkImageFormatListCreateInfo* list; };
  for (auto [format, list] : {Case{VK_FORMAT_ASTC_6x6_SRGB_BLOCK, &astc_list},
                              Case{VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK, nullptr},
                              Case{VK_FORMAT_EAC_R11_SNORM_BLOCK, nullptr},
                              Case{VK_FORMAT_EAC_R11G11_UNORM_BLOCK, nullptr}}) {
    CAPTURE(format);
    const auto block = vulkan::texture_format(format);
    const auto storage = vulkan::texture_storage(
        format, true, list ? std::span<const VkFormat>(list->pViewFormats, list->viewFormatCount)
                           : std::span<const VkFormat>{});
    const std::uint32_t row_blocks = (width + block.width - 1) / block.width;
    const std::uint32_t blocks = row_blocks * ((height + block.height - 1) / block.height);
    test::VulkanBuffer upload(gpu, blocks * block.block_bytes), raw_back(gpu, upload.size);
    auto* bytes = static_cast<std::uint8_t*>(upload.mapped);
    // astcenc references for the ASTC planes: sRGB and UNORM to their respective top-eight-bit results.
    std::vector<std::uint8_t> reference, reference_f16;
    if (block.width == 6) {
      astcenc_config config{};
      REQUIRE(astcenc_config_init(ASTCENC_PRF_LDR_SRGB, 6, 6, 1, ASTCENC_PRE_FASTEST, 0, &config) ==
              ASTCENC_SUCCESS);
      astcenc_context* raw{};
      REQUIRE(astcenc_context_alloc(&config, 1, &raw) == ASTCENC_SUCCESS);
      std::unique_ptr<astcenc_context, decltype(&astcenc_context_free)> context(raw, astcenc_context_free);
      astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
      void* input = rgba.data();
      astcenc_image in{width, height, 1, ASTCENC_TYPE_U8, &input};
      REQUIRE(astcenc_compress_image(raw, &in, &swizzle, bytes, upload.size, 0) == ASTCENC_SUCCESS);
      reference.resize(width * height * 4);
      void* output = reference.data();
      astcenc_image out{width, height, 1, ASTCENC_TYPE_U8, &output};
      REQUIRE(astcenc_decompress_image(raw, bytes, upload.size, &out, &swizzle, 0) == ASTCENC_SUCCESS);
      astcenc_config ldr{};
      REQUIRE(astcenc_config_init(ASTCENC_PRF_LDR, 6, 6, 1, ASTCENC_PRE_FASTEST, 0, &ldr) == ASTCENC_SUCCESS);
      astcenc_context* raw_ldr{};
      REQUIRE(astcenc_context_alloc(&ldr, 1, &raw_ldr) == ASTCENC_SUCCESS);
      std::unique_ptr<astcenc_context, decltype(&astcenc_context_free)> ldr_context(raw_ldr, astcenc_context_free);
      reference_f16.resize(width * height * 4);
      void* f16 = reference_f16.data();
      astcenc_image f16_out{width, height, 1, ASTCENC_TYPE_U8, &f16};
      REQUIRE(astcenc_decompress_image(raw_ldr, bytes, upload.size, &f16_out, &swizzle, 0) == ASTCENC_SUCCESS);
    } else {
      for (std::size_t i = 0; i < upload.size; ++i) bytes[i] = random() & 255;
    }
    // Oracle: each plane's format decoded on its own.
    std::vector<std::unique_ptr<test::VulkanBuffer>> singles, backing;
    gpu.begin();
    std::vector<std::shared_ptr<vulkan::DecodeResources>> resources;
    for (const auto& plane : storage.planes) {
      const auto single = vulkan::texture_format(plane.decode);
      singles.push_back(std::make_unique<test::VulkanBuffer>(gpu, width * height * single.texel_bytes));
      backing.push_back(std::make_unique<test::VulkanBuffer>(gpu, width * height * plane.texel_bytes));
      resources.push_back(vulkan::record_decode(gpu.device, gpu.command, upload.buffer, upload.size,
                                                singles.back()->buffer, singles.back()->size,
                                                {single, width, height, row_blocks, 0}));
    }
    test::VulkanImage image(gpu, format, width, height, 1, 1, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, list);
    image.transition();
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    test::vk_call("vkCmdCopyBufferToImage", gpu.command, upload.buffer, image.image, VK_IMAGE_LAYOUT_GENERAL, 1u,
                  &copy);
    gpu.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
    test::vk_call("vkCmdCopyImageToBuffer", gpu.command, image.image, VK_IMAGE_LAYOUT_GENERAL, raw_back.buffer,
                  1u, &copy);
    gpu.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                VK_ACCESS_HOST_READ_BIT);
    gpu.submit();
    CHECK(std::memcmp(upload.mapped, raw_back.mapped, upload.size) == 0);
    REQUIRE(test::vk_call("vkResetCommandBuffer", gpu.command, 0u) == VK_SUCCESS);
    vulkan::trim_texture_sources(gpu.device);
    vulkan::wait_texture_sources(gpu.device);
    CHECK_FALSE(vulkan::texture_source_gpu_resident(image.image));
    // Creating a guest view activates a sparse companion and decodes the
    // submitted producer. Peeking at its raw VkImage before that would read
    // intentionally unbound memory on a sparse-capable host.
    VkImageView activated_view{};
    VkImageViewCreateInfo activate{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    activate.image = image.image;
    activate.viewType = VK_IMAGE_VIEW_TYPE_2D;
    activate.format = storage.planes.back().guest_views.front();
    activate.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    REQUIRE(test::vk_call("vkCreateImageView", gpu.device, &activate, nullptr, &activated_view) == VK_SUCCESS);
    gpu.begin();
    // The layer kept every plane in the guest image's layout (GENERAL).
    const auto planes = vulkan::emulated_texture_planes(image.image);
    REQUIRE(planes.size() == storage.planes.size());
    for (std::size_t i = 0; i < planes.size(); ++i)
      read_image(gpu.command, planes[i], VK_IMAGE_LAYOUT_GENERAL, backing[i]->buffer, 1, &copy);
    gpu.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                VK_ACCESS_HOST_READ_BIT);
    gpu.submit();
    test::vk_call("vkDestroyImageView", gpu.device, activated_view, nullptr);
    for (std::size_t i = 0; i < planes.size(); ++i) {
      CAPTURE(i);
      if (std::memcmp(singles[i]->mapped, backing[i]->mapped, backing[i]->size) != 0) {
        const auto* a = static_cast<const std::uint16_t*>(singles[i]->mapped);
        const auto* b = static_cast<const std::uint16_t*>(backing[i]->mapped);
        unsigned shown = 0;
        for (std::size_t n = 0; n < backing[i]->size / 2 && shown < 8; ++n) if (a[n] != b[n]) {
          MESSAGE("decode/backing mismatch half element=", n, " direct_bits=", a[n], " image_bits=", b[n]);
          ++shown;
        }
      }
      CHECK(std::memcmp(singles[i]->mapped, backing[i]->mapped, backing[i]->size) == 0);
      if (reference.empty()) continue;
      const auto& expected = storage.planes[i].decode == VK_FORMAT_ASTC_6x6_SRGB_BLOCK ? reference : reference_f16;
      REQUIRE(expected.size() == backing[i]->size);
      CHECK(std::memcmp(expected.data(), backing[i]->mapped, expected.size()) == 0);
    }
    // Every format of the class is a valid view; other classes are not.
    std::vector<VkFormat> guest_views;
    for (const auto& plane : storage.planes)
      guest_views.insert(guest_views.end(), plane.guest_views.begin(), plane.guest_views.end());
    for (auto view_format : guest_views) {
      CAPTURE(view_format);
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vi.image = image.image;
      vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = view_format;
      vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkImageView view{};
      REQUIRE(test::vk_call("vkCreateImageView", gpu.device, &vi, nullptr, &view) == VK_SUCCESS);
      test::vk_call("vkDestroyImageView", gpu.device, view, nullptr);
    }
    CHECK(guest_views.size() == 2);
    VkImageViewCreateInfo wrong{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    wrong.image = image.image;
    wrong.viewType = VK_IMAGE_VIEW_TYPE_2D;
    wrong.format = VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
    wrong.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view{};
    CHECK(static_cast<std::int32_t>(test::vk_call("vkCreateImageView", gpu.device, &wrong, nullptr, &view)) ==
          VK_ERROR_FORMAT_NOT_SUPPORTED);
    resources.clear();
    REQUIRE(test::vk_call("vkResetCommandBuffer", gpu.command, 0u) == VK_SUCCESS);
  }
}

namespace {
// Wraps the host loader to observe or fail individual host commands.
struct HostHooks {
  static inline PFN_vkGetInstanceProcAddr gipa{};
  static inline PFN_vkGetDeviceProcAddr gdpa{};
  static inline PFN_vkDestroyImage destroy_image{};
  static inline PFN_vkCreateDescriptorPool create_pool{};
  static inline PFN_vkDestroyDescriptorPool destroy_pool{};
  static inline PFN_vkCmdPushConstants push_constants{};
  static inline VkImage watched{};
  static inline int watched_registered = -1;
  static inline int pools_until_failure = -1;
  static inline std::vector<VkDescriptorPool> created, destroyed;
  static inline VkPipelineLayout last_push_layout{};
  static inline PFN_vkGetPhysicalDeviceMemoryProperties memory_properties{};
  static inline PFN_vkGetBufferMemoryRequirements buffer_requirements{};
  static inline PFN_vkAllocateMemory allocate_memory{};
  // A discrete GPU's memory types: host memory, the BAR window, AMD device-
  // coherent memory, then two plain device-local types. Allocations fail.
  static inline bool discrete_memory = false;
  static inline bool restore_memory = false, fail_cached = false, fail_mapped = false,
      fail_coherent = false, noncoherent = false, lost_idle = false;
  static inline VkResult allocation_failure = VK_SUCCESS;
  static inline int flushes = 0;
  static inline std::atomic<int> lost_waits{0};
  static inline std::set<VkDeviceMemory> map_failures;
  static inline PFN_vkMapMemory map_memory{};
  static inline PFN_vkFlushMappedMemoryRanges flush_memory{};
  static inline PFN_vkQueueWaitIdle wait_idle{};
  static inline std::uint32_t buffer_type_bits{};
  static inline std::vector<std::uint32_t> allocated_types;
  static void VKAPI_CALL on_memory_properties(VkPhysicalDevice physical, VkPhysicalDeviceMemoryProperties* out) {
    if (restore_memory) {
      *out = {};
      out->memoryTypeCount = 3;
      out->memoryTypes[0] = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0};
      out->memoryTypes[1] = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          (noncoherent ? 0u : VK_MEMORY_PROPERTY_HOST_COHERENT_BIT), 0};
      out->memoryTypes[2] = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1};
      out->memoryHeapCount = 2;
      out->memoryHeaps[0].size = out->memoryHeaps[1].size = VkDeviceSize(8) << 30;
      out->memoryHeaps[1].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
      return;
    }
    if (!discrete_memory) return memory_properties(physical, out);
    *out = {};
    constexpr VkMemoryPropertyFlags local = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                    host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkMemoryPropertyFlags types[] = {
        host, local | host, local | VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD | VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD,
        local, local};
    out->memoryTypeCount = std::uint32_t(std::size(types));
    for (std::uint32_t i = 0; i < out->memoryTypeCount; ++i)
      out->memoryTypes[i] = {types[i], (types[i] & local) ? 0u : 1u};
    out->memoryHeapCount = 2;
    out->memoryHeaps[0] = {VkDeviceSize(8) << 30, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    out->memoryHeaps[1] = {VkDeviceSize(16) << 30, 0};
  }
  static void VKAPI_CALL on_buffer_requirements(VkDevice d, VkBuffer buffer, VkMemoryRequirements* out) {
    buffer_requirements(d, buffer, out);
    if (buffer_type_bits) out->memoryTypeBits = buffer_type_bits;
  }
  static VkResult VKAPI_CALL on_allocate(VkDevice d, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks* a,
                                         VkDeviceMemory* out) {
    if (restore_memory) {
      const auto type = info->memoryTypeIndex;
      allocated_types.push_back(type);
      if (allocation_failure != VK_SUCCESS) return allocation_failure;
      if ((type == 0 && fail_cached) || (type == 1 && fail_coherent)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      VkPhysicalDeviceMemoryProperties properties{};
      memory_properties(vulkan::device_physical(d), &properties);
      auto copy = *info;
      // Fake type indices all bind real compatible coherent test-host memory.
      copy.memoryTypeIndex = 0;
      while (copy.memoryTypeIndex < properties.memoryTypeCount &&
          (properties.memoryTypes[copy.memoryTypeIndex].propertyFlags &
          (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) !=
          (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ++copy.memoryTypeIndex;
      auto r = allocate_memory(d, &copy, a, out);
      if (r == VK_SUCCESS && type == 0 && fail_mapped) map_failures.insert(*out);
      return r;
    }
    if (!discrete_memory) return allocate_memory(d, info, a, out);
    allocated_types.push_back(info->memoryTypeIndex);
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  static VkResult VKAPI_CALL on_map(VkDevice d, VkDeviceMemory memory, VkDeviceSize offset,
      VkDeviceSize size, VkMemoryMapFlags flags, void** out) {
    if (map_failures.erase(memory)) return VK_ERROR_MEMORY_MAP_FAILED;
    return map_memory(d, memory, offset, size, flags, out);
  }
  static VkResult VKAPI_CALL on_flush(VkDevice d, std::uint32_t count, const VkMappedMemoryRange* ranges) {
    ++flushes;
    return flush_memory(d, count, ranges);
  }
  static VkResult VKAPI_CALL on_idle(VkQueue queue) {
    if (lost_idle) { ++lost_waits; return VK_ERROR_DEVICE_LOST; }
    return wait_idle(queue);
  }
  static void VKAPI_CALL on_destroy_image(VkDevice d, VkImage image, const VkAllocationCallbacks* a) {
    if (image == watched) watched_registered = !vulkan::emulated_texture_planes(image).empty();
    destroy_image(d, image, a);
  }
  static VkResult VKAPI_CALL on_create_pool(VkDevice d, const VkDescriptorPoolCreateInfo* info,
                                            const VkAllocationCallbacks* a, VkDescriptorPool* out) {
    if (pools_until_failure == 0) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (pools_until_failure > 0) --pools_until_failure;
    auto r = create_pool(d, info, a, out);
    if (r == VK_SUCCESS) created.push_back(*out);
    return r;
  }
  static void VKAPI_CALL on_destroy_pool(VkDevice d, VkDescriptorPool pool, const VkAllocationCallbacks* a) {
    destroyed.push_back(pool);
    destroy_pool(d, pool, a);
  }
  static void VKAPI_CALL on_push_constants(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stages,
                                           std::uint32_t offset, std::uint32_t size, const void* values) {
    last_push_layout = layout;
    push_constants(cmd, layout, stages, offset, size, values);
  }
  template <class F, class H>
  static PFN_vkVoidFunction hook(PFN_vkVoidFunction real, F& saved, H handler) {
    saved = reinterpret_cast<F>(real);
    return reinterpret_cast<PFN_vkVoidFunction>(handler);
  }
  static PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice d, const char* name) {
    auto real = gdpa(d, name);
    std::string_view n = name;
    if (!real) return real;
    if (n == "vkDestroyImage") return hook(real, destroy_image, on_destroy_image);
    if (n == "vkCreateDescriptorPool") return hook(real, create_pool, on_create_pool);
    if (n == "vkDestroyDescriptorPool") return hook(real, destroy_pool, on_destroy_pool);
    if (n == "vkCmdPushConstants") return hook(real, push_constants, on_push_constants);
    if (n == "vkGetBufferMemoryRequirements") return hook(real, buffer_requirements, on_buffer_requirements);
    if (n == "vkAllocateMemory") return hook(real, allocate_memory, on_allocate);
    if (n == "vkMapMemory") return hook(real, map_memory, on_map);
    if (n == "vkFlushMappedMemoryRanges") return hook(real, flush_memory, on_flush);
    if (n == "vkQueueWaitIdle") return hook(real, wait_idle, on_idle);
    return real;
  }
  static PFN_vkVoidFunction VKAPI_CALL instance_proc(VkInstance i, const char* name) {
    auto real = gipa(i, name);
    if (real && std::string_view(name) == "vkGetDeviceProcAddr") return hook(real, gdpa, device_proc);
    if (real && std::string_view(name) == "vkGetPhysicalDeviceMemoryProperties")
      return hook(real, memory_properties, on_memory_properties);
    return real;
  }
  HostHooks() {
    auto& device = gfx::gpu_device();
    gipa = device.get_instance_proc_addr;
    device.get_instance_proc_addr = instance_proc;
  }
  ~HostHooks() { gfx::gpu_device().get_instance_proc_addr = gipa; }
};
}  // namespace

namespace {
struct SourceTestMode {
  SourceTestMode() { vulkan::force_texture_emulation(true); vulkan::force_pageable_texture_sources(true); }
  ~SourceTestMode() {
    vulkan::set_texture_source_worker_hook({});
    vulkan::set_texture_source_copy_hook({});
    HostHooks::allocation_failure = VK_SUCCESS;
    vulkan::force_texture_emulation(false); vulkan::force_pageable_texture_sources(false);
    HostHooks::restore_memory = HostHooks::fail_cached = HostHooks::fail_mapped =
        HostHooks::fail_coherent = HostHooks::noncoherent = HostHooks::lost_idle = false;
    HostHooks::map_failures.clear(); HostHooks::buffer_type_bits = 0;
  }
};
struct SourceWorkerGate {
  std::promise<void> entered, proceed;
  std::shared_future<void> ready = proceed.get_future().share();
  std::atomic<bool> opened{false};
  void open() { if (!opened.exchange(true)) proceed.set_value(); }
};
}
TEST_CASE("Vulkan emulated compressed 3D images upload, copy and read back depth slices") {
  struct ForceEmulation {
    ForceEmulation() { vulkan::force_texture_emulation(true); }
    ~ForceEmulation() { vulkan::force_texture_emulation(false); }
  } force;
  test::VulkanDevice gpu;
  // Puzzling Places (UE 5.4) creates 32x32x32 ASTC 6x6 volume textures with
  // VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT: stacks of 2D blocks, one per slice.
  constexpr auto format = VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
  constexpr auto flags = VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
  VkImageFormatProperties limits{};
  REQUIRE(test::vk_call("vkGetPhysicalDeviceImageFormatProperties", gpu.physical, format, VK_IMAGE_TYPE_3D,
                        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, flags, &limits) == VK_SUCCESS);
  CHECK(limits.maxExtent.depth > 1);
  // ASTC UNORM16 void-extent blocks: opaque red, green and blue.
  const std::array<std::uint8_t, 16> red{0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                         0xff, 0xff, 0,    0,    0,    0,    0xff, 0xff};
  auto green = red, blue = red;
  green[8] = green[9] = 0;
  green[10] = green[11] = 255;
  blue[8] = blue[9] = 0;
  blue[12] = blue[13] = 255;
  const std::array<std::uint8_t, 16>* colors[] = {&red, &green, &blue};
  test::VulkanImage a(gpu, format, 8, 4, 2, 1, flags, nullptr, 3), b(gpu, format, 8, 4, 1, 1, flags, nullptr, 3);
  test::VulkanBuffer upload(gpu, 112), readback(gpu, 112);
  const auto storage = vulkan::texture_storage(format, false, {});
  REQUIRE(storage.planes.size() == 1);
  const auto texel_bytes = storage.planes[0].texel_bytes;
  test::VulkanBuffer decoded(gpu, 8 * 4 * 3 * texel_bytes);
  auto* bytes = static_cast<std::uint8_t*>(upload.mapped);
  for (int z = 0; z < 3; ++z)
    for (int block = 0; block < 2; ++block) std::memcpy(bytes + (z * 2 + block) * 16, colors[z]->data(), 16);
  std::memcpy(bytes + 96, green.data(), 16);  // mip 1: 4x2x1, one block
  gpu.begin();
  a.transition();
  b.transition();
  VkBufferImageCopy uploads[2]{};
  uploads[0].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  uploads[0].imageExtent = {8, 4, 3};
  uploads[1].bufferOffset = 96;
  uploads[1].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1};
  uploads[1].imageExtent = {4, 2, 1};
  test::vk_call("vkCmdCopyBufferToImage", gpu.command, upload.buffer, a.image, VK_IMAGE_LAYOUT_GENERAL, 2u,
                uploads);
  gpu.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_ACCESS_TRANSFER_READ_BIT);
  // Rotate the slices: b = (blue, red, green).
  VkImageCopy copies[2]{};
  copies[0].srcSubresource = copies[0].dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copies[0].srcOffset = {0, 0, 2};
  copies[0].extent = {8, 4, 1};
  copies[1].srcSubresource = copies[1].dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copies[1].dstOffset = {0, 0, 1};
  copies[1].extent = {8, 4, 2};
  test::vk_call("vkCmdCopyImage", gpu.command, a.image, VK_IMAGE_LAYOUT_GENERAL, b.image, VK_IMAGE_LAYOUT_GENERAL,
                2u, copies);
  gpu.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_ACCESS_TRANSFER_READ_BIT);
  VkBufferImageCopy reads[2]{};
  reads[0].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  reads[0].imageExtent = {8, 4, 3};
  reads[1].bufferOffset = 96;
  reads[1].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1};
  reads[1].imageExtent = {4, 2, 1};
  test::vk_call("vkCmdCopyImageToBuffer", gpu.command, b.image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1u,
                &reads[0]);
  test::vk_call("vkCmdCopyImageToBuffer", gpu.command, a.image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1u,
                &reads[1]);
  reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(vulkan::raw_device_proc(gpu.device, "vkCmdCopyImageToBuffer"))(
      gpu.command, b.image, VK_IMAGE_LAYOUT_GENERAL, decoded.buffer, 1, &reads[0]);
  gpu.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
              VK_ACCESS_HOST_READ_BIT);
  gpu.submit();
  const int order[] = {2, 0, 1};
  auto* raw = static_cast<const std::uint8_t*>(readback.mapped);
  for (int z = 0; z < 3; ++z)
    for (int block = 0; block < 2; ++block) {
      CAPTURE(z);
      CHECK(std::memcmp(raw + (z * 2 + block) * 16, colors[order[z]]->data(), 16) == 0);
    }
  CHECK(std::memcmp(raw + 96, green.data(), 16) == 0);
  // Each host depth slice holds the decode of its own blocks.
  REQUIRE((texel_bytes == 8 || texel_bytes == 4));
  std::size_t mismatches = 0;
  for (int z = 0; z < 3; ++z)
    for (int texel = 0; texel < 32; ++texel)
      for (int c = 0; c < 4; ++c) {
        const bool lit = c == order[z] || c == 3;
        const auto index = (z * 32 + texel) * 4 + c;
        mismatches += texel_bytes == 8
                          ? static_cast<const std::uint8_t*>(decoded.mapped)[index] != (lit ? 255 : 0)
                          : static_cast<const std::uint8_t*>(decoded.mapped)[index] != (lit ? 255 : 0);
      }
  CHECK(mismatches == 0);
  // VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT: a slice viewed as a 2D image.
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = b.image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = format;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, 1};
  VkImageView view{};
  REQUIRE(test::vk_call("vkCreateImageView", gpu.device, &vi, nullptr, &view) == VK_SUCCESS);
  test::vk_call("vkDestroyImageView", gpu.device, view, nullptr);
  REQUIRE(test::vk_call("vkResetCommandBuffer", gpu.command, 0u) == VK_SUCCESS);
}

TEST_CASE("Vulkan emulated compressed LINEAR images are created and lay out their raw blocks") {
  struct ForceEmulation {
    ForceEmulation() { vulkan::force_texture_emulation(true); }
    ~ForceEmulation() { vulkan::force_texture_emulation(false); }
  } force;
  test::VulkanDevice gpu;
  // Creed: Rise to Glory (UE4) creates a 128x128 ASTC 8x8 transfer-only LINEAR
  // texture with 8 mips although the query reports linear unsupported, and
  // quits when creation fails.
  constexpr auto format = VK_FORMAT_ASTC_8x8_UNORM_BLOCK;
  constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VkImageFormatProperties limits{};
  CHECK(static_cast<std::int32_t>(test::vk_call("vkGetPhysicalDeviceImageFormatProperties", gpu.physical, format,
                                                VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_LINEAR, usage, 0u, &limits)) ==
        VK_ERROR_FORMAT_NOT_SUPPORTED);
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = format;
  ci.extent = {128, 128, 1};
  ci.mipLevels = 8;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_LINEAR;
  ci.usage = usage;
  VkImage image{};
  REQUIRE(test::vk_call("vkCreateImage", gpu.device, &ci, nullptr, &image) == VK_SUCCESS);
  VkMemoryRequirements requirements{};
  test::vk_call("vkGetImageMemoryRequirements", gpu.device, image, &requirements);
  // Mip 1 (64x64) follows mip 0's 16x16 blocks; 8 blocks of 16 bytes per row.
  VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT, 1, 0};
  VkSubresourceLayout layout{};
  test::vk_call("vkGetImageSubresourceLayout", gpu.device, image, &sub, &layout);
  CHECK(layout.offset == 16 * 16 * 16);
  CHECK(layout.rowPitch == 8 * 16);
  CHECK(layout.size == 8 * 8 * 16);
  sub.mipLevel = 7;  // 1x1: one block, the last bytes the image needs
  test::vk_call("vkGetImageSubresourceLayout", gpu.device, image, &sub, &layout);
  CHECK(layout.size == 16);
  CHECK(layout.offset + layout.size <= requirements.size);
  test::vk_call("vkDestroyImage", gpu.device, image, nullptr);
  // An OPTIMAL image's guest memory is never read or written (the blocks live
  // in the private source and decoded image): it reserves one alignment unit,
  // not a second copy of the texture in VRAM.
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
  REQUIRE(test::vk_call("vkCreateImage", gpu.device, &ci, nullptr, &image) == VK_SUCCESS);
  test::vk_call("vkGetImageMemoryRequirements", gpu.device, image, &requirements);
  CHECK(requirements.size <= std::max<VkDeviceSize>(requirements.alignment, 1));
  CHECK(requirements.size < 16 * 16 * 16);
  test::vk_call("vkDestroyImage", gpu.device, image, nullptr);
}

TEST_SUITE_END();

namespace {
std::shared_future<void> decode_gate;
std::promise<void>* decode_entered = nullptr;
PFN_vkGetDeviceProcAddr decode_raw_gdpa = nullptr;
PFN_vkCreateComputePipelines decode_raw_create = nullptr;
std::atomic<unsigned> decode_creates{0};
VkResult VKAPI_CALL gated_decode_create(VkDevice d, VkPipelineCache cache, std::uint32_t count,
    const VkComputePipelineCreateInfo* info, const VkAllocationCallbacks* allocator, VkPipeline* out) {
  ++decode_creates;
  decode_entered->set_value();
  decode_gate.wait();
  return decode_raw_create(d, cache, count, info, allocator, out);
}

}
