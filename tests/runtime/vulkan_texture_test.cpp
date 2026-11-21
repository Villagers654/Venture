#include <astcenc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
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
#include "gfx/texture_decode.h"
#include "core/alloc_failure.h"
#include "hle/vulkan/texture.h"
#include "hle/vulkan/texture_memory.h"
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

// Decodes `blocks` with astcenc (profile) and the GPU decoder (format, FP16 or
// UNORM8 per the format's TextureFormat), returning mismatched bytes. Any two
// FP16 NaNs match: astcenc round-trips void-extent payloads through FP32. Its
// NaN error color is magenta for LDR profiles, as the specification requires.
static std::size_t astc_mismatches(test::VulkanDevice& gpu, vulkan::TextureFormat format, astcenc_profile profile,
                                   const std::vector<std::uint8_t>& blocks, std::uint32_t width,
                                   std::uint32_t height) {
  astcenc_config config{};
  REQUIRE(astcenc_config_init(profile, format.width, format.height, 1, ASTCENC_PRE_FASTEST,
                              ASTCENC_FLG_DECOMPRESS_ONLY, &config) == ASTCENC_SUCCESS);
  astcenc_context* raw{};
  REQUIRE(astcenc_context_alloc(&config, 1, &raw) == ASTCENC_SUCCESS);
  std::unique_ptr<astcenc_context, decltype(&astcenc_context_free)> context(raw, astcenc_context_free);
  const bool half = format.texel_bytes == 8;
  test::VulkanBuffer source(gpu, blocks.size()), dest(gpu, width * height * format.texel_bytes);
  std::memcpy(source.mapped, blocks.data(), blocks.size());
  std::vector<std::uint8_t> expected(dest.size);
  void* output = expected.data();
  astcenc_image out{width, height, 1, half ? ASTCENC_TYPE_F16 : ASTCENC_TYPE_U8, &output};
  astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
  REQUIRE(astcenc_decompress_image(raw, blocks.data(), blocks.size(), &out, &swizzle, 0) == ASTCENC_SUCCESS);
  gpu.begin();
  auto resources = vulkan::record_decode(gpu.device, gpu.command, source.buffer, source.size, dest.buffer, dest.size,
                                         {format, width, height, (width + format.width - 1) / format.width, 0});
  gpu.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
              VK_ACCESS_HOST_READ_BIT);
  gpu.submit();
  REQUIRE(test::vk_call("vkResetCommandBuffer", gpu.command, 0u) == VK_SUCCESS);
  const auto* actual = static_cast<const std::uint8_t*>(dest.mapped);
  auto nan = [](const std::uint8_t* p) {
    const auto v = std::uint16_t(p[0] | (p[1] << 8));
    return (v & 0x7c00) == 0x7c00 && (v & 0x3ff);
  };
  std::size_t mismatches = 0, first = 0;
  for (std::size_t i = 0; i < expected.size(); i += half ? 2 : 1) {
    const auto magenta = (i / 2) % 4 == 1 ? 0x0000 : 0x3c00;
    if (half ? std::memcmp(&expected[i], &actual[i], 2) == 0 || (nan(&expected[i]) && nan(&actual[i])) ||
                   (profile != ASTCENC_PRF_HDR && nan(&expected[i]) && (actual[i] | (actual[i + 1] << 8)) == magenta)
             : expected[i] == actual[i])
      continue;
    if (!mismatches) first = i;
    ++mismatches;
  }
  if (mismatches) {
    const auto texel = first / (format.texel_bytes);
    const auto x = texel % width, y = texel / width;
    const auto block = (y / format.height) * ((width + format.width - 1) / format.width) + x / format.width;
    std::string bytes;
    for (int b = 0; b < 16; ++b) {
      char hex[3];
      std::snprintf(hex, sizeof(hex), "%02x", blocks[block * 16 + b]);
      bytes += hex;
    }
    INFO("first mismatch at texel " << x << "," << y << " channel byte " << first % format.texel_bytes
         << " block " << bytes << " expected " << int(expected[first]) << " actual " << int(actual[first]));
    CHECK(mismatches == 0);
  }
  return mismatches;
}

TEST_CASE("Vulkan GPU ASTC HDR decode is bit-exact to astcenc for all footprints") {
  test::VulkanDevice gpu;
  constexpr std::uint32_t width = 31, height = 23;
  // HDR gradients from 2^-12 to 2^14, an LDR band, LDR and HDR alpha, and a
  // flat HDR corner for FP16 void extents.
  std::vector<float> rgba(width * height * 4);
  std::mt19937 random(1031);
  for (std::uint32_t y = 0; y < height; ++y)
    for (std::uint32_t x = 0; x < width; ++x) {
      auto* p = &rgba[(y * width + x) * 4];
      if (x < 12 && y < 12) {
        p[0] = 3.5f, p[1] = 0.25f, p[2] = 900.0f, p[3] = 1.0f;
        continue;
      }
      const bool ldr = y >= 16;
      p[0] = ldr ? x / 31.0f : std::exp2(x * 0.8f - 12.0f);
      p[1] = ldr ? (y * 11 % 23) / 23.0f : std::exp2(y * 1.1f - 10.0f);
      p[2] = ldr ? (random() & 255) / 255.0f : std::exp2((random() % 2600) / 100.0f - 12.0f);
      p[3] = x % 3 == 0 ? 1.0f : x % 3 == 1 ? (y * 13 % 23) / 23.0f : std::exp2((x + y) * 0.3f - 4.0f);
    }
  astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
  for (int value = VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK; value <= VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK; ++value)
    for (auto profile : {ASTCENC_PRF_HDR, ASTCENC_PRF_HDR_RGB_LDR_A}) {
      const auto format = vulkan::texture_format(static_cast<VkFormat>(value));
      REQUIRE(format.decoded == VK_FORMAT_R16G16B16A16_SFLOAT);
      CAPTURE(value);
      CAPTURE(int(profile));
      astcenc_config config{};
      REQUIRE(astcenc_config_init(profile, format.width, format.height, 1, ASTCENC_PRE_FAST, 0, &config) ==
              ASTCENC_SUCCESS);
      astcenc_context* raw{};
      REQUIRE(astcenc_context_alloc(&config, 1, &raw) == ASTCENC_SUCCESS);
      std::unique_ptr<astcenc_context, decltype(&astcenc_context_free)> context(raw, astcenc_context_free);
      void* input = rgba.data();
      astcenc_image in{width, height, 1, ASTCENC_TYPE_F32, &input};
      std::vector<std::uint8_t> blocks(((width + format.width - 1) / format.width) *
                                       ((height + format.height - 1) / format.height) * 16);
      REQUIRE(astcenc_compress_image(raw, &in, &swizzle, blocks.data(), blocks.size(), 0) == ASTCENC_SUCCESS);
      // SFLOAT formats always decode with the full HDR profile.
      astc_mismatches(gpu, format, ASTCENC_PRF_HDR, blocks, width, height);
    }
}

TEST_CASE("Vulkan GPU ASTC decode matches astcenc on random blocks in every profile") {
  // Random bits reach every block mode, endpoint mode, and illegal encoding.
  test::VulkanDevice gpu;
  constexpr std::uint32_t width = 120, height = 120;
  std::mt19937 random(31);
  for (auto [first, last] : {std::pair{VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK, VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK},
                             std::pair{VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_12x12_SRGB_BLOCK}})
    for (int value = first; value <= last; ++value) {
      auto format = vulkan::texture_format(static_cast<VkFormat>(value));
      const bool hdr = value >= VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK;
      const bool srgb = format.decoded == VK_FORMAT_R8G8B8A8_SRGB;
      CAPTURE(value);
      std::vector<std::uint8_t> blocks(((width + format.width - 1) / format.width) *
                                       ((height + format.height - 1) / format.height) * 16);
      for (std::size_t i = 0; i < blocks.size(); i += 16) {
        for (int b = 0; b < 16; ++b) blocks[i + b] = std::uint8_t(random());
        // A quarter are void extents, whose bits random data rarely forms.
        if (random() % 4 == 0) {
          blocks[i] = 0xfc;
          blocks[i + 1] = (blocks[i + 1] & 0x02) | 0xfd;
          if (random() % 2) std::memset(&blocks[i + 2], 0xff, 6);
        }
      }
      astc_mismatches(gpu, format, hdr ? ASTCENC_PRF_HDR : srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, blocks,
                      width, height);
      if (!hdr && !srgb) {
        format.decoded = VK_FORMAT_R16G16B16A16_SFLOAT;
        format.texel_bytes = 8;
        astc_mismatches(gpu, format, ASTCENC_PRF_LDR, blocks, width, height);
      }
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

TEST_CASE("Vulkan BC7 encode keeps transparent and opaque texels exact") {
  // An icon: an opaque disc with a soft rim over a transparent background whose
  // colors are arbitrary (Batman: Arkham Shadow's combat icons showed their box).
  test::VulkanDevice gpu;
  constexpr std::uint32_t size = 64;
  std::vector<std::uint8_t> rgba(size * size * 4);
  std::mt19937 random(31);
  for (std::uint32_t y = 0; y < size; ++y)
    for (std::uint32_t x = 0; x < size; ++x) {
      auto i = (y * size + x) * 4;
      const double r = std::hypot(x - 31.5, y - 31.5);
      rgba[i] = random() & 255; rgba[i + 1] = random() & 255; rgba[i + 2] = random() & 255;
      rgba[i + 3] = r < 20 ? 255 : r > 23 ? 0 : std::uint8_t((23 - r) / 3 * 255);
      if (r < 20) rgba[i] = 230, rgba[i + 1] = (x * 7) & 255, rgba[i + 2] = 40;
    }
  test::VulkanBuffer source(gpu, rgba.size()), encoded(gpu, size * size);
  std::memcpy(source.mapped, rgba.data(), rgba.size());
  gpu.begin();
  auto resources = vulkan::record_encode_bc7(gpu.device, gpu.command, source.buffer, source.size, encoded.buffer,
                                             encoded.size, size, size);
  gpu.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
              VK_ACCESS_HOST_READ_BIT);
  gpu.submit();
  const auto* bytes = static_cast<const std::uint8_t*>(encoded.mapped);
  int transparent = 0, opaque = 0, worst = 0;
  for (std::uint32_t by = 0; by < size / 4; ++by)
    for (std::uint32_t bx = 0; bx < size / 4; ++bx) {
      const auto texels = decode_bc7_mode6(bytes + (by * (size / 4) + bx) * 16);
      for (std::uint32_t i = 0; i < 16; ++i) {
        const auto alpha = rgba[((by * 4 + i / 4) * size + bx * 4 + i % 4) * 4 + 3], decoded = texels[i * 4 + 3];
        if (alpha == 0 && decoded != 0) ++transparent;
        if (alpha == 255 && decoded != 255) ++opaque;
        worst = std::max(worst, std::abs(int(alpha) - int(decoded)));
      }
    }
  CHECK(transparent == 0);
  CHECK(opaque == 0);
  CHECK(worst <= 24);
}

// Encodes each <name>_<w>x<h>.rgba in VENTURE_BC7_IMAGES and writes the decoded
// blocks beside it as <name>_<w>x<h>.bc7.rgba, for checks on real textures.
TEST_CASE("Vulkan BC7 encode of RGBA images (VENTURE_BC7_IMAGES)") {
  const char* dir = std::getenv("VENTURE_BC7_IMAGES");
  if (!dir) return;
  test::VulkanDevice gpu;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    const auto name = entry.path().filename().string();
    if (!name.ends_with(".rgba") || name.ends_with(".bc7.rgba")) continue;
    const auto size = name.substr(name.rfind('_') + 1);
    const std::uint32_t width = std::stoul(size), height = std::stoul(size.substr(size.find('x') + 1));
    std::ifstream in(entry.path(), std::ios::binary);
    std::vector<char> rgba((std::istreambuf_iterator<char>(in)), {});
    REQUIRE(rgba.size() == std::size_t(width) * height * 4);
    const std::uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    test::VulkanBuffer source(gpu, rgba.size()), encoded(gpu, bw * bh * 16);
    std::memcpy(source.mapped, rgba.data(), rgba.size());
    gpu.begin();
    auto resources = vulkan::record_encode_bc7(gpu.device, gpu.command, source.buffer, source.size, encoded.buffer,
                                               encoded.size, width, height);
    gpu.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                VK_ACCESS_HOST_READ_BIT);
    gpu.submit();
    std::vector<char> decoded(rgba.size());
    for (std::uint32_t by = 0; by < bh; ++by)
      for (std::uint32_t bx = 0; bx < bw; ++bx) {
        const auto texels = decode_bc7_mode6(static_cast<const std::uint8_t*>(encoded.mapped) + (by * bw + bx) * 16);
        for (std::uint32_t i = 0; i < 16; ++i) {
          const auto x = bx * 4 + i % 4, y = by * 4 + i / 4;
          if (x < width && y < height) std::memcpy(&decoded[(y * width + x) * 4], &texels[i * 4], 4);
        }
      }
    auto out = entry.path();
    out.replace_extension(".bc7.rgba");
    std::ofstream(out, std::ios::binary).write(decoded.data(), std::streamsize(decoded.size()));
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
  static inline int pools_until_failure = -1;
  static inline std::vector<VkDescriptorPool> created, destroyed;
  static inline PFN_vkGetPhysicalDeviceMemoryProperties memory_properties{};
  static inline PFN_vkGetBufferMemoryRequirements buffer_requirements{};
  static inline PFN_vkAllocateMemory allocate_memory{};
  // A discrete GPU's memory types: host memory, the BAR window, AMD device-
  // coherent memory, then two plain device-local types. Allocations fail.
  static inline bool discrete_memory = false;
  static inline bool restore_memory = false, fail_cached = false, fail_mapped = false,
      fail_coherent = false, noncoherent = false, lost_idle = false;
  static inline VkResult allocation_failure = VK_SUCCESS;
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
    return flush_memory(d, count, ranges);
  }
  static VkResult VKAPI_CALL on_idle(VkQueue queue) {
    if (lost_idle) { ++lost_waits; return VK_ERROR_DEVICE_LOST; }
    return wait_idle(queue);
  }
  static void VKAPI_CALL on_destroy_image(VkDevice d, VkImage image, const VkAllocationCallbacks* a) {
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

TEST_CASE("Vulkan emulated compressed images accept input-attachment usage") {
  struct ForceEmulation {
    ForceEmulation() { vulkan::force_texture_emulation(true); }
    ~ForceEmulation() { vulkan::force_texture_emulation(false); }
  } force;
  test::VulkanDevice gpu;
  // The Climb (CryEngine) creates its ETC2 textures with input-attachment
  // usage and mutable format, and crashes when creation fails.
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK;
  ci.extent = {32, 32, 1};
  ci.mipLevels = 4;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
             VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
  VkImage image{};
  REQUIRE(test::vk_call("vkCreateImage", gpu.device, &ci, nullptr, &image) == VK_SUCCESS);
  test::vk_call("vkDestroyImage", gpu.device, image, nullptr);
}

TEST_CASE("Vulkan views of emulated images created before the guest binds memory") {
  for (int compression : {0, 1}) {
    CAPTURE(compression);
    struct Force {
      explicit Force(int mode) { vulkan::force_texture_emulation(true); vulkan::force_texture_compression(mode); }
      ~Force() { vulkan::force_texture_compression(-1); vulkan::force_texture_emulation(false); }
    } force(compression);
    test::VulkanDevice gpu;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK;
    ci.extent = {32, 32, 1};
    ci.mipLevels = 4;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    VkImage image{};
    REQUIRE(test::vk_call("vkCreateImage", gpu.device, &ci, nullptr, &image) == VK_SUCCESS);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ci.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 4, 0, 1};
    VkImageView view{};
    REQUIRE(test::vk_call("vkCreateImageView", gpu.device, &vi, nullptr, &view) == VK_SUCCESS);
    VkMemoryRequirements req{};
    test::vk_call("vkGetImageMemoryRequirements", gpu.device, image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = gpu.memory_type(req.memoryTypeBits, 0);
    VkDeviceMemory memory{};
    REQUIRE(test::vk_call("vkAllocateMemory", gpu.device, &ai, nullptr, &memory) == VK_SUCCESS);
    CHECK(test::vk_call("vkBindImageMemory", gpu.device, image, memory, 0ull) == VK_SUCCESS);
    test::vk_call("vkDestroyImageView", gpu.device, view, nullptr);
    test::vk_call("vkDestroyImage", gpu.device, image, nullptr);
    test::vk_call("vkFreeMemory", gpu.device, memory, nullptr);
  }
}

TEST_CASE("Vulkan views of emulated input-attachment images, decoded and as BC7") {
  for (int compression : {0, 1}) {
    CAPTURE(compression);
    struct Force {
      explicit Force(int mode) { vulkan::force_texture_emulation(true); vulkan::force_texture_compression(mode); }
      ~Force() { vulkan::force_texture_compression(-1); vulkan::force_texture_emulation(false); }
    } force(compression);
    test::VulkanDevice gpu;
    // The Climb then views each texture as its own and its sRGB/UNORM sibling
    // format, and the driver crashed in vkCreateImageView.
    for (auto format : {VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,
                        VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK, VK_FORMAT_ASTC_4x4_SRGB_BLOCK}) {
      CAPTURE(int(format));
      test::VulkanImage texture(gpu, format, 32, 32, 4, 1, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, nullptr, 1,
                                VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
      const auto info = vulkan::texture_format(format);
      const VkFormat sibling = info.decoded == VK_FORMAT_R8G8B8A8_SRGB ? VkFormat(format - 1) : VkFormat(format + 1);
      for (auto view_format : {format, sibling})
        for (VkImageUsageFlags usage : {VkImageUsageFlags(0), VkImageUsageFlags(VK_IMAGE_USAGE_SAMPLED_BIT),
                                        VkImageUsageFlags(VK_IMAGE_USAGE_SAMPLED_BIT |
                                                          VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)}) {
          CAPTURE(int(view_format));
          CAPTURE(usage);
          VkImageViewUsageCreateInfo view_usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, nullptr, usage};
          VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
          vi.pNext = usage ? &view_usage : nullptr;
          vi.image = texture.image;
          vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
          vi.format = view_format;
          vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 4, 0, 1};
          VkImageView view{};
          REQUIRE(test::vk_call("vkCreateImageView", gpu.device, &vi, nullptr, &view) == VK_SUCCESS);
          test::vk_call("vkDestroyImageView", gpu.device, view, nullptr);
        }
    }
  }
}

TEST_CASE("Vulkan texture decode refused memory skips that texture, logs the limit and recovers") {
  struct Force {
    Force() { vulkan::force_texture_emulation(true); vulkan::force_texture_compression(0); }
    ~Force() {
      vulkan::fail_texture_allocations(-1);
      vulkan::force_texture_compression(-1);
      vulkan::force_texture_emulation(false);
    }
  } force;
  test::VulkanDevice gpu;
  constexpr auto format = VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
  // An ASTC UNORM16 void-extent block: opaque red.
  const std::array<std::uint8_t, 16> red{0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                         0xff, 0xff, 0,    0,    0,    0,    0xff, 0xff};
  test::VulkanImage texture(gpu, format, 8, 4);
  test::VulkanBuffer upload(gpu, 32), decoded(gpu, 8 * 4 * 8);
  std::memcpy(upload.mapped, red.data(), 16);
  std::memcpy(static_cast<std::uint8_t*>(upload.mapped) + 16, red.data(), 16);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {8, 4, 1};
  // RE4 on an 8 GB card: every pool allocation is refused, in VRAM and in
  // system memory, before and after the spare memory is given back. The
  // upload is left out with a warning; recording and submission succeed.
  alloc_failure::reset_counts();
  vulkan::fail_texture_allocations(1 << 20);
  gpu.begin();
  texture.transition();
  test::vk_call("vkCmdCopyBufferToImage", gpu.command, upload.buffer, texture.image, VK_IMAGE_LAYOUT_GENERAL, 1u,
                &copy);
  gpu.submit();
  CHECK(alloc_failure::counts().reports > 0);
  CHECK(vulkan::texture_memory_refused());
  REQUIRE(test::vk_call("vkResetCommandBuffer", gpu.command, 0u) == VK_SUCCESS);
  // Memory back: the next upload of the same texture decodes as before.
  vulkan::fail_texture_allocations(-1);
  alloc_failure::reset_counts();
  gpu.begin();
  test::vk_call("vkCmdCopyBufferToImage", gpu.command, upload.buffer, texture.image, VK_IMAGE_LAYOUT_GENERAL, 1u,
                &copy);
  gpu.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_ACCESS_TRANSFER_READ_BIT);
  reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(vulkan::raw_device_proc(gpu.device, "vkCmdCopyImageToBuffer"))(
      gpu.command, texture.image, VK_IMAGE_LAYOUT_GENERAL, decoded.buffer, 1, &copy);
  gpu.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
              VK_ACCESS_HOST_READ_BIT);
  gpu.submit();
  CHECK(alloc_failure::counts().reports == 0);
  const auto storage = vulkan::texture_storage(format, false, {});
  REQUIRE(storage.planes.size() == 1);
  if (storage.planes[0].texel_bytes == 4) {
    const auto* texels = static_cast<const std::uint8_t*>(decoded.mapped);
    for (int texel = 0; texel < 32; ++texel) {
      CAPTURE(texel);
      CHECK(texels[texel * 4] == 255);
      CHECK(texels[texel * 4 + 1] == 0);
      CHECK(texels[texel * 4 + 3] == 255);
    }
  }
  REQUIRE(test::vk_call("vkResetCommandBuffer", gpu.command, 0u) == VK_SUCCESS);
}

// SwiftShader (the suites' GPU) samples ASTC LDR, as the Steam Frame's Turnip,
// Intel's iGPUs and Android headsets do; desktop NVIDIA and AMD do not.
TEST_CASE("Vulkan native ASTC passes through; an image the host refuses is decoded and copies keep the blocks") {
  test::VulkanDevice gpu;
  if (!native_astc(gpu.physical)) {
    MESSAGE("the host GPU has no native ASTC; the decoded path is covered elsewhere");
    return;
  }
  constexpr auto format = VK_FORMAT_ASTC_8x8_UNORM_BLOCK;
  // The guest sees the host's own format properties and feature.
  VkFormatProperties host{}, guest{};
  reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
      vulkan::raw_physical_proc(gpu.physical, "vkGetPhysicalDeviceFormatProperties"))(gpu.physical, format, &host);
  test::vk_call("vkGetPhysicalDeviceFormatProperties", gpu.physical, format, &guest);
  CHECK(guest.optimalTilingFeatures == host.optimalTilingFeatures);
  CHECK(guest.linearTilingFeatures == host.linearTilingFeatures);
  CHECK(guest.bufferFeatures == host.bufferFeatures);
  VkPhysicalDeviceFeatures features{};
  test::vk_call("vkGetPhysicalDeviceFeatures", gpu.physical, &features);
  CHECK(features.textureCompressionASTC_LDR == VK_TRUE);

  // An optimal sampled image is the host's: no decoded planes.
  test::VulkanImage native(gpu, format, 64, 64);
  CHECK(vulkan::emulated_texture_planes(native.image).empty());

  // A shape the host refuses: LINEAR (Creed: Rise to Glory's transfer-only
  // texture), else a 3D image of 2D blocks.
  auto raw_query = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties>(
      vulkan::raw_physical_proc(gpu.physical, "vkGetPhysicalDeviceImageFormatProperties"));
  constexpr VkImageUsageFlags transfer = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  constexpr VkImageUsageFlags sampled = transfer | VK_IMAGE_USAGE_SAMPLED_BIT;
  VkImageFormatProperties limits{};
  const bool linear_refused = raw_query(gpu.physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_LINEAR, transfer, 0,
                                        &limits) != VK_SUCCESS;
  const bool volume_refused = raw_query(gpu.physical, format, VK_IMAGE_TYPE_3D, VK_IMAGE_TILING_OPTIMAL, sampled, 0,
                                        &limits) != VK_SUCCESS;
  MESSAGE("host refuses LINEAR ASTC: " << linear_refused << ", 3D ASTC: " << volume_refused);
  if (volume_refused) {
    // Answered as the decoder serves it, so the guest may create it.
    CHECK(test::vk_call("vkGetPhysicalDeviceImageFormatProperties", gpu.physical, format, VK_IMAGE_TYPE_3D,
                        VK_IMAGE_TILING_OPTIMAL, sampled, 0u, &limits) == VK_SUCCESS);
    CHECK(limits.sampleCounts == VK_SAMPLE_COUNT_1_BIT);
  }
  // A host that accepts both (SwiftShader) is taken to refuse LINEAR, as most GPUs do.
  struct RefuseLinear {
    bool on;
    explicit RefuseLinear(bool on) : on(on) { if (on) vulkan::force_linear_compressed_refusal(true); }
    ~RefuseLinear() { if (on) vulkan::force_linear_compressed_refusal(false); }
  } refuse_linear(!linear_refused && !volume_refused);
  const bool linear = linear_refused || refuse_linear.on;
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = linear ? VK_IMAGE_TYPE_2D : VK_IMAGE_TYPE_3D;
  ci.format = format;
  ci.extent = {64, 64, linear ? 1u : 2u};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = linear ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
  ci.usage = linear ? transfer : sampled;
  VkImage refused{};
  REQUIRE(test::vk_call("vkCreateImage", gpu.device, &ci, nullptr, &refused) == VK_SUCCESS);
  CHECK(!vulkan::emulated_texture_planes(refused).empty());
  VkMemoryRequirements requirements{};
  test::vk_call("vkGetImageMemoryRequirements", gpu.device, refused, &requirements);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = requirements.size;
  ai.memoryTypeIndex = gpu.memory_type(requirements.memoryTypeBits, 0);
  VkDeviceMemory memory{};
  REQUIRE(test::vk_call("vkAllocateMemory", gpu.device, &ai, nullptr, &memory) == VK_SUCCESS);
  REQUIRE(test::vk_call("vkBindImageMemory", gpu.device, refused, memory, 0ull) == VK_SUCCESS);

  constexpr VkDeviceSize slice = 8 * 8 * 16;  // 64x64 texels of 8x8 blocks
  test::VulkanBuffer upload(gpu, 2 * slice), readback(gpu, 2 * slice);
  std::mt19937 random(171);
  auto* bytes = static_cast<std::uint8_t*>(upload.mapped);
  for (VkDeviceSize i = 0; i < 2 * slice; ++i) bytes[i] = static_cast<std::uint8_t>(random());
  std::memset(readback.mapped, 0, 2 * slice);

  gpu.begin();
  native.transition();
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = refused;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  test::vk_call("vkCmdPipelineBarrier", gpu.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &b);
  const auto after_transfer = [&] {
    gpu.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
  };
  // Slice 0 of the upload goes into the decoded image, then into the native one.
  VkBufferImageCopy to_refused{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {64, 64, 1}};
  test::vk_call("vkCmdCopyBufferToImage", gpu.command, upload.buffer, refused, VK_IMAGE_LAYOUT_GENERAL, 1u,
                &to_refused);
  after_transfer();
  VkImageCopy across{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     {0, 0, 0}, {64, 64, 1}};
  test::vk_call("vkCmdCopyImage", gpu.command, refused, VK_IMAGE_LAYOUT_GENERAL, native.image,
                VK_IMAGE_LAYOUT_GENERAL, 1u, &across);
  after_transfer();
  VkBufferImageCopy from_native{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {64, 64, 1}};
  test::vk_call("vkCmdCopyImageToBuffer", gpu.command, native.image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1u,
                &from_native);
  after_transfer();
  // Slice 1 goes into the native image, then into the decoded one (its second
  // depth slice when 3D), and back out of the decoded image's blocks.
  VkBufferImageCopy to_native{slice, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {64, 64, 1}};
  test::vk_call("vkCmdCopyBufferToImage", gpu.command, upload.buffer, native.image, VK_IMAGE_LAYOUT_GENERAL, 1u,
                &to_native);
  after_transfer();
  across.dstOffset.z = linear ? 0 : 1;
  test::vk_call("vkCmdCopyImage", gpu.command, native.image, VK_IMAGE_LAYOUT_GENERAL, refused,
                VK_IMAGE_LAYOUT_GENERAL, 1u, &across);
  after_transfer();
  VkBufferImageCopy from_refused{slice, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, linear ? 0 : 1},
                                 {64, 64, 1}};
  test::vk_call("vkCmdCopyImageToBuffer", gpu.command, refused, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1u,
                &from_refused);
  gpu.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
              VK_ACCESS_HOST_READ_BIT);
  gpu.submit();
  CHECK(std::memcmp(readback.mapped, upload.mapped, slice) == 0);
  CHECK(std::memcmp(static_cast<std::uint8_t*>(readback.mapped) + slice, bytes + slice, slice) == 0);
  REQUIRE(test::vk_call("vkResetCommandBuffer", gpu.command, 0u) == VK_SUCCESS);
  test::vk_call("vkDestroyImage", gpu.device, refused, nullptr);
  test::vk_call("vkFreeMemory", gpu.device, memory, nullptr);
}

TEST_CASE("Vulkan ASTC decode override: forced decoding hides the host's ASTC") {
  test::VulkanDevice gpu;
  if (!native_astc(gpu.physical)) {
    MESSAGE("the host GPU has no native ASTC");
    return;
  }
  struct ForceDecode {
    ForceDecode() { vulkan::force_texture_emulation(true); }
    ~ForceDecode() { vulkan::force_texture_emulation(false); }
  } force;
  constexpr auto format = VK_FORMAT_ASTC_8x8_UNORM_BLOCK;
  VkFormatProperties guest{};
  test::vk_call("vkGetPhysicalDeviceFormatProperties", gpu.physical, format, &guest);
  CHECK(guest.linearTilingFeatures == 0);
  CHECK(guest.bufferFeatures == 0);
  test::VulkanImage decoded(gpu, format, 64, 64);
  CHECK(!vulkan::emulated_texture_planes(decoded.image).empty());
}

TEST_SUITE_END();

TEST_CASE("VENTURE_NATIVE_ASTC: off values force decoding, anything else keeps the host's ASTC") {
  CHECK(gfx::native_astc_setting(nullptr));
  CHECK(gfx::native_astc_setting(""));
  CHECK(gfx::native_astc_setting("1"));
  CHECK(gfx::native_astc_setting("on"));
  for (const char* off : {"0", "off", "false", "never"}) CHECK_FALSE(gfx::native_astc_setting(off));
}


TEST_CASE("Vulkan barriers on a null image are dropped; the rest of the barrier still records") {
  // The Climb transitions the foveation image VrApi answered with no image, in
  // an all-commands barrier: the NVIDIA driver faulted on the null handle.
  test::VulkanDevice gpu(true);
  VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  memory.srcAccessMask = memory.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  VkImageMemoryBarrier null_image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  null_image.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  null_image.newLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
  null_image.srcQueueFamilyIndex = null_image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  null_image.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
  test::vk_call("vkCmdPipelineBarrier", gpu.command, VkPipelineStageFlags(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT),
                VkPipelineStageFlags(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT), VkDependencyFlags(0), 1u, &memory, 0u,
                static_cast<const VkBufferMemoryBarrier*>(nullptr), 1u, &null_image);
  VkImageMemoryBarrier2 null_image2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  null_image2.srcStageMask = null_image2.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  null_image2.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  null_image2.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  null_image2.srcQueueFamilyIndex = null_image2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  null_image2.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &null_image2;
  test::vk_call("vkCmdPipelineBarrier2", gpu.command, &dependency);
  gpu.submit();
}
TEST_CASE("Vulkan density views of a null image get a full-density image; other null views are refused") {
  test::VulkanDevice gpu;
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = VK_NULL_HANDLE;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  vi.format = VK_FORMAT_R8G8_UNORM;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
  VkImageView view{};
  REQUIRE(test::vk_call("vkCreateImageView", gpu.device, &vi, nullptr, &view) == VK_SUCCESS);
  CHECK(view != VK_NULL_HANDLE);
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, 1};
  VkImageView second{};
  REQUIRE(test::vk_call("vkCreateImageView", gpu.device, &vi, nullptr, &second) == VK_SUCCESS);
  test::vk_call("vkDestroyImageView", gpu.device, second, nullptr);
  test::vk_call("vkDestroyImageView", gpu.device, view, nullptr);
  vi.format = VK_FORMAT_R8G8B8A8_UNORM;
  VkImageView refused{};
  CHECK(test::vk_call("vkCreateImageView", gpu.device, &vi, nullptr, &refused) == VK_ERROR_INITIALIZATION_FAILED);
  CHECK(refused == VK_NULL_HANDLE);
}
namespace guest_compute {
#include "vulkan_texture_guest_compute.comp.inc"
}
namespace {
// Texture decodes and BC7 encodes recorded so far, and the passes in submissions the driver accepted,
// from the device-fault census.
struct PassCensus {
  unsigned long long decodes = 0, encodes = 0, submitted = 0;
};
PassCensus recorded_texture_passes() {
  const auto census = vulkan::texture_pass_census();
  PassCensus counts;
  const auto at = census.find("recorded ");
  REQUIRE(at != std::string::npos);
  REQUIRE(std::sscanf(census.c_str() + at, "recorded %llu texture decodes, %llu BC7 encodes", &counts.decodes,
                      &counts.encodes) == 2);
  const auto submitted = census.find("; ", at);
  REQUIRE(submitted != std::string::npos);
  REQUIRE(std::sscanf(census.c_str() + submitted, "; %llu in submissions the driver accepted", &counts.submitted) == 1);
  return counts;
}
}  // namespace
// RUMBLE on an RX 9060 XT lost the device in its first texture uploads, stored as BC7, before it had any
// descriptor set layout of its own. Venture's decode and BC7 passes in a guest command buffer bind every
// descriptor their shaders use and give the guest its compute state back: the guest's pipeline, both its
// sets and its push constants reach its dispatches between more than 1500 passes of mixed formats and sizes.
// Run under VK_LAYER_KHRONOS_validation (GPU-assisted) this also checks every pass's descriptors.
TEST_CASE("Vulkan texture decodes and BC7 encodes inside a guest command buffer bind their own descriptors and keep the guest's") {
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
  // The guest's compute pass: set 0 the totals, set 1 the input, push constants the slot and addend.
  VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  shader_info.codeSize = sizeof(guest_compute::decode_shader);
  shader_info.pCode = guest_compute::decode_shader;
  VkShaderModule shader{};
  REQUIRE(test::vk_call("vkCreateShaderModule", gpu.device, &shader_info, nullptr, &shader) == VK_SUCCESS);
  const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                                             nullptr};
  VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 1;
  set_info.pBindings = &binding;
  VkDescriptorSetLayout set_layouts[2]{};
  for (auto& l : set_layouts)
    REQUIRE(test::vk_call("vkCreateDescriptorSetLayout", gpu.device, &set_info, nullptr, &l) == VK_SUCCESS);
  const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 2;
  layout_info.pSetLayouts = set_layouts;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &range;
  VkPipelineLayout layout{};
  REQUIRE(test::vk_call("vkCreatePipelineLayout", gpu.device, &layout_info, nullptr, &layout) == VK_SUCCESS);
  VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipeline_info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                         shader, "main", nullptr};
  pipeline_info.layout = layout;
  VkPipeline pipeline{};
  REQUIRE(test::vk_call("vkCreateComputePipelines", gpu.device, VK_NULL_HANDLE, 1u, &pipeline_info, nullptr,
                        &pipeline) == VK_SUCCESS);
  const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 2;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  VkDescriptorPool pool{};
  REQUIRE(test::vk_call("vkCreateDescriptorPool", gpu.device, &pool_info, nullptr, &pool) == VK_SUCCESS);
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = pool;
  alloc.descriptorSetCount = 2;
  alloc.pSetLayouts = set_layouts;
  VkDescriptorSet sets[2]{};
  REQUIRE(test::vk_call("vkAllocateDescriptorSets", gpu.device, &alloc, sets) == VK_SUCCESS);
  test::VulkanBuffer totals(gpu, 16), input(gpu, 4);
  std::memset(totals.mapped, 0, 16);
  *static_cast<std::uint32_t*>(input.mapped) = 40;
  const VkDescriptorBufferInfo buffers[] = {{totals.buffer, 0, 16}, {input.buffer, 0, 4}};
  VkWriteDescriptorSet writes[2]{};
  for (std::uint32_t i = 0; i < 2; ++i) {
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[i].dstSet = sets[i];
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &buffers[i];
  }
  test::vk_call("vkUpdateDescriptorSets", gpu.device, 2u, writes, 0u, static_cast<const VkCopyDescriptorSet*>(nullptr));

  // Emulated textures of mixed formats and sizes, 512x512 among them, with every mip.
  struct Shape {
    VkFormat format;
    std::uint32_t width, height, levels;
  };
  std::vector<Shape> shapes;
  const VkFormat formats[] = {VK_FORMAT_ASTC_4x4_SRGB_BLOCK, VK_FORMAT_ASTC_6x6_UNORM_BLOCK,
                              VK_FORMAT_ASTC_8x8_SRGB_BLOCK, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,
                              VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK};
  const VkExtent2D sizes[] = {{512, 512}, {256, 256}, {61, 37}, {128, 64}, {64, 64}, {32, 128}, {16, 16}, {4, 4}};
  for (std::size_t i = 0; i < std::size(formats) * std::size(sizes); ++i) {
    const auto size = sizes[i % std::size(sizes)];
    const auto levels = 1 + std::uint32_t(std::log2(std::max(size.width, size.height)));
    shapes.push_back({formats[i % std::size(formats)], size.width, size.height, levels});
  }
  std::vector<std::unique_ptr<test::VulkanImage>> images;
  std::vector<std::vector<VkBufferImageCopy>> copies;
  VkDeviceSize upload_bytes = 0;
  for (const auto& s : shapes) {
    images.push_back(std::make_unique<test::VulkanImage>(gpu, s.format, s.width, s.height, s.levels));
    const auto info = vulkan::texture_format(s.format);
    auto& regions = copies.emplace_back();
    for (std::uint32_t mip = 0; mip < s.levels; ++mip) {
      const auto w = std::max(1u, s.width >> mip), h = std::max(1u, s.height >> mip);
      VkBufferImageCopy copy{};
      copy.bufferOffset = upload_bytes;
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
      copy.imageExtent = {w, h, 1};
      regions.push_back(copy);
      upload_bytes += VkDeviceSize((w + info.width - 1) / info.width) * ((h + info.height - 1) / info.height) *
                      info.block_bytes;
    }
  }
  // Random blocks: valid, error and void-extent encodings alike decode to some color.
  test::VulkanBuffer upload(gpu, upload_bytes);
  std::mt19937 random(9060);
  for (VkDeviceSize i = 0; i < upload_bytes / 4; ++i) static_cast<std::uint32_t*>(upload.mapped)[i] = random();

  const auto before = recorded_texture_passes();
  std::uint64_t uploaded = 0;
  gpu.begin();
  for (auto& image : images) image->transition();
  test::vk_call("vkCmdBindPipeline", gpu.command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  test::vk_call("vkCmdBindDescriptorSets", gpu.command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0u, 2u, sets, 0u,
                static_cast<const std::uint32_t*>(nullptr));
  std::uint32_t push[2] = {0, 1};
  test::vk_call("vkCmdPushConstants", gpu.command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0u, 8u,
                static_cast<const void*>(push));
  std::uint32_t expected[4]{};
  auto dispatch = [&] {
    gpu.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    test::vk_call("vkCmdDispatch", gpu.command, 1u, 1u, 1u);
    expected[push[0]] += 40 + push[1];
  };
  dispatch();
  for (std::uint32_t round = 0; uploaded < 1536; ++round) {
    for (std::size_t i = 0; i < images.size(); ++i) {
      test::vk_call("vkCmdCopyBufferToImage", gpu.command, upload.buffer, images[i]->image, VK_IMAGE_LAYOUT_GENERAL,
                    std::uint32_t(copies[i].size()), copies[i].data());
      uploaded += copies[i].size();
      // Between uploads the guest dispatches with the state it bound before them.
      if (i % 16 == 15) dispatch();
    }
    if (round == 0) {
      push[0] = 1;
      push[1] = 2;
      test::vk_call("vkCmdPushConstants", gpu.command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0u, 8u,
                    static_cast<const void*>(push));
    }
  }
  dispatch();
  gpu.barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
              VK_ACCESS_HOST_READ_BIT);
  REQUIRE(test::vk_call("vkEndCommandBuffer", gpu.command) == VK_SUCCESS);
  const auto recorded = recorded_texture_passes();
  CHECK(uploaded >= 1536);
  CHECK(recorded.decodes - before.decodes == uploaded);
  CHECK(recorded.encodes - before.encodes == uploaded);  // every texture is stored as BC7
  // The census tells passes that reached the driver from passes only recorded.
  CHECK(recorded.submitted == before.submitted);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &gpu.command;
  REQUIRE(test::vk_call("vkQueueSubmit", gpu.queue, 1u, &submit, VkFence{}) == VK_SUCCESS);
  REQUIRE(test::vk_call("vkQueueWaitIdle", gpu.queue) == VK_SUCCESS);
  CHECK(recorded_texture_passes().submitted - before.submitted == 2 * uploaded);
  const auto* got = static_cast<const std::uint32_t*>(totals.mapped);
  for (int i = 0; i < 4; ++i) {
    CAPTURE(i);
    CHECK(got[i] == expected[i]);
  }

  images.clear();
  test::vk_call("vkDestroyDescriptorPool", gpu.device, pool, nullptr);
  test::vk_call("vkDestroyPipeline", gpu.device, pipeline, nullptr);
  test::vk_call("vkDestroyPipelineLayout", gpu.device, layout, nullptr);
  for (auto l : set_layouts) test::vk_call("vkDestroyDescriptorSetLayout", gpu.device, l, nullptr);
  test::vk_call("vkDestroyShaderModule", gpu.device, shader, nullptr);
}
// Quest's driver keeps what a title frees until the GPU retires the work using it, so a title may reset or
// free a command buffer, or reset or destroy its pool, while that work still runs. Venture's decode and BC7
// passes in a secondary command buffer, run from two primaries, keep their descriptor pools, scratch and
// sources until that work completes when the command buffers are released right after the submit, with the
// work held behind a timeline value the host signals only afterwards. Under VK_LAYER_KHRONOS_validation no descriptor
// pool, buffer or image of Venture's is destroyed in use (VUID-vkDestroyDescriptorPool-descriptorPool-00303).
TEST_CASE("Vulkan texture passes keep their descriptor pools, scratch and sources while a released command buffer still runs") {
  struct Force {
    Force() { vulkan::force_texture_emulation(true); vulkan::force_texture_compression(1); }
    ~Force() { vulkan::force_texture_compression(-1); vulkan::force_texture_emulation(false); }
  } force;
  test::VulkanDevice gpu(false, false, true);  // timeline semaphores
  VkPhysicalDeviceFeatures features{};
  test::vk_call("vkGetPhysicalDeviceFeatures", gpu.physical, &features);
  if (!features.textureCompressionBC) {
    MESSAGE("host has no BC textures");
    return;
  }
  const VkFormat formats[] = {VK_FORMAT_ASTC_4x4_SRGB_BLOCK, VK_FORMAT_ASTC_6x6_UNORM_BLOCK,
                              VK_FORMAT_ASTC_8x8_SRGB_BLOCK, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,
                              VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK};
  const VkExtent2D sizes[] = {{512, 512}, {256, 256}, {61, 37}, {128, 64}, {64, 64}, {32, 128}, {16, 16}, {4, 4}};
  std::vector<std::unique_ptr<test::VulkanImage>> images;
  std::vector<std::vector<VkBufferImageCopy>> copies;
  VkDeviceSize upload_bytes = 0;
  std::uint64_t uploaded = 0;
  for (std::size_t i = 0; i < std::size(formats) * std::size(sizes); ++i) {
    const auto format = formats[i % std::size(formats)];
    const auto size = sizes[i % std::size(sizes)];
    const auto levels = 1 + std::uint32_t(std::log2(std::max(size.width, size.height)));
    images.push_back(std::make_unique<test::VulkanImage>(gpu, format, size.width, size.height, levels));
    const auto info = vulkan::texture_format(format);
    auto& regions = copies.emplace_back();
    for (std::uint32_t mip = 0; mip < levels; ++mip) {
      const auto w = std::max(1u, size.width >> mip), h = std::max(1u, size.height >> mip);
      VkBufferImageCopy copy{};
      copy.bufferOffset = upload_bytes;
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
      copy.imageExtent = {w, h, 1};
      regions.push_back(copy);
      upload_bytes += VkDeviceSize((w + info.width - 1) / info.width) * ((h + info.height - 1) / info.height) *
                      info.block_bytes;
      ++uploaded;
    }
  }
  auto upload = std::make_unique<test::VulkanBuffer>(gpu, upload_bytes);
  std::mt19937 random(9061);
  for (VkDeviceSize i = 0; i < upload_bytes / 4; ++i) static_cast<std::uint32_t*>(upload->mapped)[i] = random();
  gpu.begin();
  for (auto& image : images) image->transition();
  gpu.submit();

  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = gpu.family;
  VkCommandPool pool{};
  REQUIRE(test::vk_call("vkCreateCommandPool", gpu.device, &pool_info, nullptr, &pool) == VK_SUCCESS);
  VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 2;
  VkCommandBuffer primaries[2]{};
  REQUIRE(test::vk_call("vkAllocateCommandBuffers", gpu.device, &alloc, primaries) == VK_SUCCESS);
  alloc.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
  alloc.commandBufferCount = 1;
  VkCommandBuffer secondary{};
  REQUIRE(test::vk_call("vkAllocateCommandBuffers", gpu.device, &alloc, &secondary) == VK_SUCCESS);

  const auto before = recorded_texture_passes();
  VkCommandBufferInheritanceInfo inheritance{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;  // pending in both primaries at once
  begin.pInheritanceInfo = &inheritance;
  REQUIRE(test::vk_call("vkBeginCommandBuffer", secondary, &begin) == VK_SUCCESS);
  for (std::size_t i = 0; i < images.size(); ++i)
    test::vk_call("vkCmdCopyBufferToImage", secondary, upload->buffer, images[i]->image, VK_IMAGE_LAYOUT_GENERAL,
                  std::uint32_t(copies[i].size()), copies[i].data());
  REQUIRE(test::vk_call("vkEndCommandBuffer", secondary) == VK_SUCCESS);
  CHECK(recorded_texture_passes().decodes - before.decodes == uploaded);

  // The work waits on a timeline value the host signals only after the release.
  VkSemaphoreTypeCreateInfo timeline{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
  timeline.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timeline};
  VkSemaphore gate{};
  REQUIRE(test::vk_call("vkCreateSemaphore", gpu.device, &semaphore_info, nullptr, &gate) == VK_SUCCESS);
  VkCommandBufferBeginInfo primary_begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  primary_begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  for (auto primary : primaries) {
    REQUIRE(test::vk_call("vkBeginCommandBuffer", primary, &primary_begin) == VK_SUCCESS);
    test::vk_call("vkCmdExecuteCommands", primary, 1u, &secondary);
    REQUIRE(test::vk_call("vkEndCommandBuffer", primary) == VK_SUCCESS);
  }
  const std::uint64_t open = 1;
  VkTimelineSemaphoreSubmitInfo wait_value{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
  wait_value.waitSemaphoreValueCount = 1;
  wait_value.pWaitSemaphoreValues = &open;
  const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, &wait_value};
  submit.waitSemaphoreCount = 1;
  submit.pWaitSemaphores = &gate;
  submit.pWaitDstStageMask = &wait_stage;
  submit.commandBufferCount = 2;
  submit.pCommandBuffers = primaries;
  REQUIRE(test::vk_call("vkQueueSubmit", gpu.queue, 1u, &submit, VkFence{}) == VK_SUCCESS);
  // Each primary carries the secondary's passes.
  CHECK(recorded_texture_passes().submitted - before.submitted == 4 * uploaded);

  // While the work waits on the semaphore, Venture releases the command buffers as a guest reset, free or
  // pool reset or destroy releases them (vkResetCommandBuffer, vkBeginCommandBuffer, vkFreeCommandBuffers,
  // vkResetCommandPool and vkDestroyCommandPool all end in release_texture_commands). The host command
  // buffers themselves stay alive here: a host driver that runs them from their own memory (SwiftShader)
  // cannot have them freed under it.
  const auto held = vulkan::deferred_free_pending(gpu.device);
  for (auto command : {secondary, primaries[0], primaries[1]}) vulkan::release_texture_commands(gpu.device, command);
  // The secondary holds the passes; the primaries only count them.
  CHECK(vulkan::texture_commands_retiring(gpu.device) == 1);
  CHECK(vulkan::deferred_free_pending(gpu.device) == held + 1);
  VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
  signal.semaphore = gate;
  signal.value = open;
  REQUIRE(test::vk_call("vkSignalSemaphore", gpu.device, &signal) == VK_SUCCESS);
  REQUIRE(test::vk_call("vkQueueWaitIdle", gpu.queue) == VK_SUCCESS);
  CHECK(vulkan::deferred_free_pending(gpu.device) == 0);
  CHECK(vulkan::texture_commands_retiring(gpu.device) == 0);
  // The guest's own frees, now that the work is done.
  REQUIRE(test::vk_call("vkResetCommandBuffer", primaries[1], 0u) == VK_SUCCESS);
  test::vk_call("vkFreeCommandBuffers", gpu.device, pool, 1u, &secondary);
  REQUIRE(test::vk_call("vkResetCommandPool", gpu.device, pool, 0u) == VK_SUCCESS);
  test::vk_call("vkDestroyCommandPool", gpu.device, pool, nullptr);
  test::vk_call("vkDestroySemaphore", gpu.device, gate, nullptr);
  // The usual case: the guest re-records a command buffer whose work has completed. Its passes' objects
  // return at once.
  gpu.begin();
  test::vk_call("vkCmdCopyBufferToImage", gpu.command, upload->buffer, images[0]->image, VK_IMAGE_LAYOUT_GENERAL,
                std::uint32_t(copies[0].size()), copies[0].data());
  gpu.submit();  // waits for the queue
  gpu.begin();
  CHECK(vulkan::texture_commands_retiring(gpu.device) == 0);
  CHECK(vulkan::deferred_free_pending(gpu.device) == 0);
  REQUIRE(test::vk_call("vkEndCommandBuffer", gpu.command) == VK_SUCCESS);
  upload.reset();
  images.clear();
}
