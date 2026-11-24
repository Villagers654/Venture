#include "xr/color_space.h"
#include <cmath>
#include <doctest/doctest.h>
#include "xr_zero_copy_setting.h"
#include <stdexcept>
#include "unorm_test_support.h"

using namespace venture::xr;
#include "gfx/gpu_device.h"
#include "vulkan_test_support.h"
#include "xr/capture.h"
#include "xr/color_images.h"
#include "xr/guest_images.h"
#include <condition_variable>
#include <future>

namespace {
struct ColorAllocationProbe {
  inline static PFN_vkGetInstanceProcAddr fallback{};
  inline static PFN_vkAllocateMemory allocate{};
  inline static PFN_vkDestroyImage destroy{};
  inline static PFN_vkWaitForFences wait{};
  inline static unsigned allocations = 0, destructions = 0, waits = 0;
  inline static std::mutex gate_mutex;
  inline static std::condition_variable gate_cv;
  inline static bool hold = false, entered = false;
  static VkResult VKAPI_CALL wait_for_fences(VkDevice device, std::uint32_t count,
      const VkFence* fences, VkBool32 all, std::uint64_t timeout) {
    {
      std::unique_lock lock(gate_mutex);
      ++waits;
      if (hold) {
        entered = true; gate_cv.notify_all();
        gate_cv.wait(lock, [] { return !hold; });
      }
    }
    return wait(device, count, fences, all, timeout);
  }
  static VkResult VKAPI_CALL allocate_memory(VkDevice device,
      const VkMemoryAllocateInfo* info, const VkAllocationCallbacks* callbacks,
      VkDeviceMemory* memory) {
    ++allocations;
    return allocate(device, info, callbacks, memory);
  }
  static void VKAPI_CALL destroy_image(VkDevice device, VkImage image,
      const VkAllocationCallbacks* callbacks) {
    ++destructions;
    destroy(device, image, callbacks);
  }
  static PFN_vkVoidFunction VKAPI_CALL get(VkInstance instance, const char* name) {
    if (!std::strcmp(name, "vkAllocateMemory")) {
      allocate = reinterpret_cast<PFN_vkAllocateMemory>(fallback(instance, name));
      return reinterpret_cast<PFN_vkVoidFunction>(allocate_memory);
    }
    if (!std::strcmp(name, "vkDestroyImage")) {
      destroy = reinterpret_cast<PFN_vkDestroyImage>(fallback(instance, name));
      return reinterpret_cast<PFN_vkVoidFunction>(destroy_image);
    }
    if (!std::strcmp(name, "vkWaitForFences")) {
      wait = reinterpret_cast<PFN_vkWaitForFences>(fallback(instance, name));
      return reinterpret_cast<PFN_vkVoidFunction>(wait_for_fences);
    }
    return fallback(instance, name);
  }
  explicit ColorAllocationProbe(PFN_vkGetInstanceProcAddr original) {
    fallback = original;
    allocations = destructions = waits = 0;
  }
};
} // namespace

TEST_CASE("XR color handoff transforms linear and sRGB arrays without changing "
          "alpha or guest pixels") {
  using namespace venture::test;
  VulkanDevice gpu;
  auto get = venture::gfx::gpu_device().get_instance_proc_addr;
  XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR,
                                     nullptr,
                                     gpu.instance,
                                     gpu.physical,
                                     gpu.device,
                                     gpu.family,
                                     0};
  for (auto format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB}) {
    CAPTURE(format);
    VulkanImage target(gpu, format, 8, 8, 2, 2, 0, nullptr, 1,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    std::vector<VkBufferImageCopy> copies;
    std::vector<unsigned char> original;
    for (unsigned layer = 0; layer < 2; ++layer)
      for (unsigned mip = 0; mip < 2; ++mip) {
        const auto extent = 8u >> mip;
        VkBufferImageCopy copy{};
        copy.bufferOffset = original.size();
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1};
        copy.imageExtent = {extent, extent, 1};
        copies.push_back(copy);
        for (unsigned pixel = 0; pixel < extent * extent; ++pixel)
          for (auto channel :
               layer ? std::array<unsigned char, 4>{64, 192, 128, 204}
                     : std::array<unsigned char, 4>{128, 64, 192, 102})
            original.push_back(channel);
      }
    VulkanBuffer upload(gpu, original.size());
    std::memcpy(upload.mapped, original.data(), original.size());
    auto barrier = [&](VkImageLayout before, VkImageLayout after, VkImage handle = VK_NULL_HANDLE) {
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      b.oldLayout = before;
      b.newLayout = after;
      b.image = handle ? handle : target.image;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 2, 0, 2};
      b.srcAccessMask =
          before == VK_IMAGE_LAYOUT_UNDEFINED
              ? 0
              : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      vk_call("vkCmdPipelineBarrier", gpu.command,
              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 0u, nullptr, 0u, nullptr,
              1u, &b);
    };
    gpu.begin();
    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vk_call("vkCmdCopyBufferToImage", gpu.command, upload.buffer, target.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, std::uint32_t(copies.size()),
            copies.data());
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    gpu.submit();
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.format = format;
    info.width = info.height = 8;
    info.arraySize = 2;
    info.faceCount = 1;
    info.mipCount = 2;
    info.sampleCount = 1;
    XrSwapchainImageVulkanKHR image{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR, nullptr,
                                    target.image};
    ColorImages filter(binding, get, info, {&image, 1});
    auto matrix =
        color_matrix(XR_COLOR_SPACE_RIFT_CV1_FB, XR_COLOR_SPACE_REC709_FB);
    auto read = [&](VkImage handle = VK_NULL_HANDLE) {
      VulkanBuffer download(gpu, original.size());
      REQUIRE(vk_call("vkResetCommandBuffer", gpu.command, 0u) == VK_SUCCESS);
      gpu.begin();
      barrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, handle);
      vk_call("vkCmdCopyImageToBuffer", gpu.command, handle ? handle : target.image,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, download.buffer,
              std::uint32_t(copies.size()), copies.data());
      barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, handle);
      gpu.submit();
      auto bytes = static_cast<unsigned char *>(download.mapped);
      return std::vector<unsigned char>(bytes, bytes + original.size());
    };
    REQUIRE(filter.restore(0) == XR_SUCCESS);
    REQUIRE(filter.convert(0, matrix) == XR_SUCCESS);
    auto first = read();
    auto decode = [](double x) {
      return x <= .04045 ? x / 12.92 : std::pow((x + .055) / 1.055, 2.4);
    };
    auto encode = [](double x) {
      return x <= .0031308 ? 12.92 * x : 1.055 * std::pow(x, 1 / 2.4) - .055;
    };
    for (std::size_t pixel = 0; pixel < original.size(); pixel += 4) {
      std::array<double, 3> rgb{};
      for (unsigned c = 0; c < 3; ++c) {
        rgb[c] = original[pixel + c] / 255.;
        if (format == VK_FORMAT_R8G8B8A8_SRGB)
          rgb[c] = decode(rgb[c]);
      }
      auto expected = transform_color(matrix, rgb);
      for (unsigned c = 0; c < 3; ++c) {
        auto value = std::clamp(expected[c], 0., 1.);
        if (format == VK_FORMAT_R8G8B8A8_SRGB)
          value = encode(value);
        CHECK(std::abs(int(first[pixel + c]) - int(std::lround(value * 255))) <=
              1);
      }
      CHECK(first[pixel + 3] == original[pixel + 3]);
    }
    REQUIRE(filter.restore(0) == XR_SUCCESS);
    CHECK(read() == original);
    REQUIRE(filter.convert(0, matrix) == XR_SUCCESS);
    CHECK(read() == first);
    REQUIRE(filter.restore(0) == XR_SUCCESS);
    CHECK(read() == original);
    VulkanImage presentation(gpu, format, 8, 8, 2, 2, 0, nullptr, 1,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    XrSwapchainImageVulkanKHR output{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR, nullptr,
                                    presentation.image};
    for (bool sampleable : {false, true}) for (bool ordered : {false, true}) {
      CAPTURE(sampleable);
      CAPTURE(ordered);
      auto separate_info = info;
      if (sampleable)
        separate_info.usageFlags |= XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
      ColorAllocationProbe probe(get);
      {
        ColorImages separate(binding, probe.get, separate_info, {&output, 1}, ordered);
        REQUIRE(separate.convert_from(target.image, 0, matrix) == XR_SUCCESS);
        CHECK(probe.waits == (ordered ? 0 : 1));
        if (ordered) {
          // Reuse the guest source immediately on the binding queue, without
          // waiting on ColorImages' fence. The presentation must retain the
          // original pixels even though the source is overwritten afterwards.
          gpu.begin();
          barrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
          VkClearColorValue black{{0, 0, 0, 1}};
          VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 2, 0, 2};
          vk_call("vkCmdClearColorImage", gpu.command, target.image,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1u, &all);
          barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
          gpu.submit();
          CHECK(read(presentation.image) == first);
          CHECK(read() != original);
          gpu.begin();
          barrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
          vk_call("vkCmdCopyBufferToImage", gpu.command, upload.buffer, target.image,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, std::uint32_t(copies.size()),
                  copies.data());
          barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
          gpu.submit();
        }
        CHECK(read() == original);
        CHECK(read(presentation.image) == first);
        CHECK(probe.allocations == (sampleable ? 0 : 1));
        REQUIRE(separate.convert_from(target.image, 0, matrix) == XR_SUCCESS);
        CHECK(read() == original);
        CHECK(read(presentation.image) == first);
        // Rebind a different source to the same presentation slot after its
        // previous submission. An identity pass makes stale descriptors visible.
        REQUIRE(separate.convert_from(presentation.image, 0,
            color_matrix(XR_COLOR_SPACE_REC709_FB, XR_COLOR_SPACE_REC709_FB)) == XR_SUCCESS);
        CHECK(read(presentation.image) == first);
        REQUIRE(separate.restore(0) == XR_SUCCESS);
        REQUIRE(separate.convert_from(target.image, 0, matrix) == XR_SUCCESS);
        CHECK(read() == original);
        CHECK(read(presentation.image) == first);
        // Borrowed sources allocate nothing; an in-place conversion still needs
        // one exact backup. Returning to a borrowed source releases that backup.
        CHECK(probe.allocations == 1);
      }
      CHECK(probe.destructions == 1);
      CHECK(read() == original);
    }
  }
}

#include "hle/openxr/state.h"
#include "xr/presentation.h"
#include "xr/host_xr.h"
namespace {
struct ColorHost {
  void *previous = venture::xr::host_xr().get_instance_proc_addr;
  bool previous_enabled =
      venture::xr::host_xr().enabled_extensions.contains("XR_FB_color_space");
  inline static PFN_xrGetInstanceProcAddr fallback{};
  static XrResult XRAPI_CALL enumerate(XrSession, std::uint32_t capacity,
                                       std::uint32_t *count,
                                       XrColorSpaceFB *values) {
    *count = 2;
    if (capacity < 2)
      return capacity ? XR_ERROR_SIZE_INSUFFICIENT : XR_SUCCESS;
    values[0] = XR_COLOR_SPACE_REC709_FB;
    values[1] = XR_COLOR_SPACE_P3_FB;
    return XR_SUCCESS;
  }
  static XrResult XRAPI_CALL set(XrSession, XrColorSpaceFB space) {
    return space == XR_COLOR_SPACE_REC709_FB || space == XR_COLOR_SPACE_P3_FB
               ? XR_SUCCESS
               : XR_ERROR_COLOR_SPACE_UNSUPPORTED_FB;
  }
  static XrResult XRAPI_CALL get(XrInstance instance, const char *name,
                                 PFN_xrVoidFunction *out) {
    if (!std::strcmp(name, "xrEnumerateColorSpacesFB"))
      *out = reinterpret_cast<PFN_xrVoidFunction>(enumerate);
    else if (!std::strcmp(name, "xrSetColorSpaceFB"))
      *out = reinterpret_cast<PFN_xrVoidFunction>(set);
    else
      return fallback(instance, name, out);
    return XR_SUCCESS;
  }
  ColorHost() {
    fallback = reinterpret_cast<PFN_xrGetInstanceProcAddr>(previous);
    venture::xr::host_xr().enabled_extensions.insert("XR_FB_color_space");
    venture::xr::host_xr().get_instance_proc_addr =
        reinterpret_cast<void *>(get);
  }
  ~ColorHost() {
    venture::xr::host_xr().get_instance_proc_addr = previous;
    if (!previous_enabled)
      venture::xr::host_xr().enabled_extensions.erase("XR_FB_color_space");
  }
};
} // namespace
TEST_CASE("XR color handoff resolves multisample coverage and restores native "
          "samples") {
  using namespace venture::test;
  VulkanDevice gpu;
  VulkanImage target(gpu, VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 2, 0, nullptr, 1,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                     VK_SAMPLE_COUNT_4_BIT);
  VulkanImage resolved(gpu, VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 2);
  auto transition = [&](VkImage image, VkImageLayout before,
                        VkImageLayout after) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.image = image;
    b.oldLayout = before;
    b.newLayout = after;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
    b.srcAccessMask =
        before == VK_IMAGE_LAYOUT_UNDEFINED
            ? 0
            : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vk_call("vkCmdPipelineBarrier", gpu.command,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 0u, nullptr, 0u, nullptr,
            1u, &b);
  };
  gpu.begin();
  transition(target.image, VK_IMAGE_LAYOUT_UNDEFINED,
             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkClearColorValue color{{.2f, .4f, .6f, .8f}};
  VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
  vk_call("vkCmdClearColorImage", gpu.command, target.image,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1u, &range);
  transition(target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  transition(resolved.image, VK_IMAGE_LAYOUT_UNDEFINED,
             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  gpu.submit();
  auto get = venture::gfx::gpu_device().get_instance_proc_addr;
  XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR,
                                     nullptr,
                                     gpu.instance,
                                     gpu.physical,
                                     gpu.device,
                                     gpu.family,
                                     0};
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.format = VK_FORMAT_R8G8B8A8_UNORM;
  info.width = info.height = 4;
  info.arraySize = 2;
  info.faceCount = info.mipCount = 1;
  info.sampleCount = 4;
  XrSwapchainImageVulkanKHR image{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR, nullptr,
                                  target.image};
  ColorImages filter(binding, get, info, {&image, 1});
  auto read = [&] {
    VulkanBuffer download(gpu, 4 * 4 * 2 * 4);
    REQUIRE(vk_call("vkResetCommandBuffer", gpu.command, 0u) == VK_SUCCESS);
    gpu.begin();
    transition(target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(resolved.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageResolve resolve{};
    resolve.srcSubresource =
        resolve.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 2};
    resolve.extent = {4, 4, 1};
    vk_call("vkCmdResolveImage", gpu.command, target.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, resolved.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &resolve);
    transition(target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transition(resolved.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 2};
    copy.imageExtent = {4, 4, 1};
    vk_call("vkCmdCopyImageToBuffer", gpu.command, resolved.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, download.buffer, 1u, &copy);
    transition(resolved.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    gpu.submit();
    auto bytes = static_cast<unsigned char *>(download.mapped);
    return std::vector<unsigned char>(bytes, bytes + 4 * 4 * 2 * 4);
  };
  auto original = read();
  const auto matrix = color_matrix(XR_COLOR_SPACE_RIFT_CV1_FB,
                                   XR_COLOR_SPACE_REC709_FB);
  REQUIRE(filter.convert(0, matrix) == XR_SUCCESS);
  auto output = read();
  for (std::size_t i = 0; i < output.size(); i += 4) {
    const auto expected = transform_color(matrix,
        {original[i] / 255., original[i + 1] / 255., original[i + 2] / 255.});
    for (unsigned channel = 0; channel < 3; ++channel) {
      CAPTURE(i);
      CAPTURE(channel);
      CAPTURE(expected[channel]);
      CHECK(venture::test::unorm8_matches(output[i + channel], expected[channel]));
    }
    CHECK(output[i + 3] == original[i + 3]);
  }
  REQUIRE(filter.restore(0) == XR_SUCCESS);
  CHECK(read() == original);
}

namespace {
// Records every image a copy or blit reads.
struct CopySourceProbe {
  inline static PFN_vkGetInstanceProcAddr fallback{};
  inline static PFN_vkCmdCopyImage copy{};
  inline static PFN_vkCmdCopyImageToBuffer to_buffer{};
  inline static PFN_vkCmdBlitImage blit{};
  inline static std::vector<VkImage> sources;
  static void VKAPI_CALL copy_image(VkCommandBuffer command, VkImage source, VkImageLayout source_layout,
      VkImage target, VkImageLayout target_layout, std::uint32_t count, const VkImageCopy* regions) {
    sources.push_back(source);
    copy(command, source, source_layout, target, target_layout, count, regions);
  }
  static void VKAPI_CALL copy_to_buffer(VkCommandBuffer command, VkImage source, VkImageLayout layout,
      VkBuffer buffer, std::uint32_t count, const VkBufferImageCopy* regions) {
    sources.push_back(source);
    to_buffer(command, source, layout, buffer, count, regions);
  }
  static void VKAPI_CALL blit_image(VkCommandBuffer command, VkImage source, VkImageLayout source_layout,
      VkImage target, VkImageLayout target_layout, std::uint32_t count, const VkImageBlit* regions, VkFilter filter) {
    sources.push_back(source);
    blit(command, source, source_layout, target, target_layout, count, regions, filter);
  }
  static PFN_vkVoidFunction VKAPI_CALL get(VkInstance instance, const char* name) {
    if (!std::strcmp(name, "vkCmdCopyImage")) {
      copy = reinterpret_cast<PFN_vkCmdCopyImage>(fallback(instance, name));
      return reinterpret_cast<PFN_vkVoidFunction>(copy_image);
    }
    if (!std::strcmp(name, "vkCmdCopyImageToBuffer")) {
      to_buffer = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(fallback(instance, name));
      return reinterpret_cast<PFN_vkVoidFunction>(copy_to_buffer);
    }
    if (!std::strcmp(name, "vkCmdBlitImage")) {
      blit = reinterpret_cast<PFN_vkCmdBlitImage>(fallback(instance, name));
      return reinterpret_cast<PFN_vkVoidFunction>(blit_image);
    }
    return fallback(instance, name);
  }
  explicit CopySourceProbe(PFN_vkGetInstanceProcAddr original) {
    fallback = original;
    sources.clear();
  }
};
} // namespace
TEST_CASE("XR color pass draws from the game's image into a host image it only writes, matching copy then convert") {
  using namespace venture::test;
  VulkanDevice gpu;
  auto get = venture::gfx::gpu_device().get_instance_proc_addr;
  XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR,
      nullptr, gpu.instance, gpu.physical, gpu.device, gpu.family, 0};
  constexpr std::uint32_t size = 8, layers = 2, bytes = size * size * layers * 4;
  auto barrier = [&](VkImage image, VkImageLayout before, VkImageLayout after) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.image = image;
    b.oldLayout = before;
    b.newLayout = after;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    b.srcAccessMask = before == VK_IMAGE_LAYOUT_UNDEFINED
                          ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vk_call("vkCmdPipelineBarrier", gpu.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &b);
  };
  VkBufferImageCopy whole{};
  whole.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
  whole.imageExtent = {size, size, 1};
  auto read = [&](VkImage image) {
    VulkanBuffer download(gpu, bytes);
    REQUIRE(vk_call("vkResetCommandBuffer", gpu.command, 0u) == VK_SUCCESS);
    gpu.begin();
    barrier(image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vk_call("vkCmdCopyImageToBuffer", gpu.command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            download.buffer, 1u, &whole);
    barrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    gpu.submit();
    auto data = static_cast<unsigned char*>(download.mapped);
    return std::vector<unsigned char>(data, data + bytes);
  };
  // A host image starts as a solid yellow with alpha 0, the fill one report
  // showed: none of it may survive the pass.
  auto fill_host = [&](VkImage image) {
    REQUIRE(vk_call("vkResetCommandBuffer", gpu.command, 0u) == VK_SUCCESS);
    gpu.begin();
    barrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue yellow{{1, 1, 0, 0}};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    vk_call("vkCmdClearColorImage", gpu.command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &yellow, 1u, &range);
    barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    gpu.submit();
  };
  for (auto format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB}) {
    CAPTURE(format);
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.format = format;
    info.width = info.height = size;
    info.arraySize = layers;
    info.faceCount = info.mipCount = info.sampleCount = 1;
    // The game asks for no sampling; Venture's slot images allow it anyway.
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    GuestImages guest(binding, get, info);
    REQUIRE(guest.sampled());
    REQUIRE(guest.wait(0) == XR_SUCCESS);
    const auto game = guest.images()[0].image;
    std::vector<unsigned char> original(bytes);
    for (std::uint32_t i = 0; i < bytes; i += 4) {
      const auto pixel = i / 4, layer = pixel / (size * size);
      original[i] = std::uint8_t(pixel * 7 + 16 * layer);
      original[i + 1] = std::uint8_t(255 - pixel * 3);
      original[i + 2] = std::uint8_t(pixel * 11 + 90 * layer);
      original[i + 3] = std::uint8_t(pixel % 3 ? 255 : 40 + pixel);
    }
    VulkanBuffer upload(gpu, bytes);
    std::memcpy(upload.mapped, original.data(), bytes);
    REQUIRE(vk_call("vkResetCommandBuffer", gpu.command, 0u) == VK_SUCCESS);
    gpu.begin();
    barrier(game, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vk_call("vkCmdCopyBufferToImage", gpu.command, upload.buffer, game, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1u, &whole);
    barrier(game, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    gpu.submit();
    const auto matrix = color_matrix(XR_COLOR_SPACE_RIFT_CV1_FB, XR_COLOR_SPACE_REC709_FB);
    for (bool passthrough : {false, true}) {
      CAPTURE(passthrough);
      const auto alpha = passthrough ? ColorImages::Alpha::opaque : ColorImages::Alpha::preserve;
      VulkanImage direct_host(gpu, format, size, size, 1, layers, 0, nullptr, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
      VulkanImage copied_host(gpu, format, size, size, 1, layers, 0, nullptr, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
      fill_host(direct_host.image);
      fill_host(copied_host.image);
      XrSwapchainImageVulkanKHR direct_image{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR, nullptr, direct_host.image};
      XrSwapchainImageVulkanKHR copied_image{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR, nullptr, copied_host.image};
      // Reference: copy into the host image, then convert it in place.
      REQUIRE(guest.copy(0, copied_host.image, format) == XR_SUCCESS);
      REQUIRE(guest.wait(0) == XR_SUCCESS);
      std::vector<unsigned char> expected;
      {
        ColorImages in_place(binding, get, info, {&copied_image, 1}, false, passthrough);
        REQUIRE(in_place.convert(0, matrix, false, alpha) == XR_SUCCESS);
        expected = read(copied_host.image);
      }
      CopySourceProbe probe(get);
      ColorImages direct(binding, probe.get, info, {&direct_image, 1}, false, passthrough);
      for (unsigned repeat = 0; repeat < 2; ++repeat) {
        REQUIRE(guest.submit(0, [&](VkCommandBuffer command, VkImage image, VkImageLayout layout) {
          CHECK(image == game);
          CHECK(layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
          return direct.record_from(command, image, layout, 0, matrix, alpha);
        }) == XR_SUCCESS);
        REQUIRE(guest.wait(0) == XR_SUCCESS);
        CHECK(read(direct_host.image) == expected);
      }
      // The host image is never read, and the game's image stays as drawn.
      CHECK(probe.sources.empty());
      CHECK(read(game) == original);
      if (!passthrough) {
        // The pass converts: compare with the matrix applied on the CPU.
        auto decode = [](double x) { return x <= .04045 ? x / 12.92 : std::pow((x + .055) / 1.055, 2.4); };
        auto encode = [](double x) { return x <= .0031308 ? x * 12.92 : 1.055 * std::pow(x, 1 / 2.4) - .055; };
        const auto output = read(direct_host.image);
        for (std::uint32_t i = 0; i < bytes; i += 4) {
          std::array<double, 3> rgb{};
          for (unsigned c = 0; c < 3; ++c)
            rgb[c] = format == VK_FORMAT_R8G8B8A8_SRGB ? decode(original[i + c] / 255.) : original[i + c] / 255.;
          const auto converted = transform_color(matrix, rgb);
          for (unsigned c = 0; c < 3; ++c) {
            auto value = std::clamp(converted[c], 0., 1.);
            if (format == VK_FORMAT_R8G8B8A8_SRGB) value = encode(value);
            // Drivers approximate the sRGB encode on store: Mesa's lavapipe lands 2 steps off in the darks.
            const int tolerance = format == VK_FORMAT_R8G8B8A8_SRGB ? 2 : 1;
            CHECK(std::abs(int(output[i + c]) - int(std::lround(value * 255))) <= tolerance);
          }
          CHECK(output[i + 3] == original[i + 3]);
        }
      } else {
        const auto output = read(direct_host.image);
        for (std::uint32_t i = 3; i < bytes; i += 4) CHECK(output[i] == 255);
      }
      // A failing recorder submits nothing and leaves the slot usable.
      CHECK(guest.submit(0, [](VkCommandBuffer, VkImage, VkImageLayout) { return XR_ERROR_LIMIT_REACHED; }) ==
            XR_ERROR_LIMIT_REACHED);
      REQUIRE(guest.wait(0) == XR_SUCCESS);
    }
  }
}

#include "../mock_openxr_runtime/control.h"
#include "common/environment.h"
#include <cstdlib>
TEST_CASE("XR guest first wait establishes attachment layouts and handoff copies the base mip of every layer and aspect") {
  using namespace venture::test;
  VulkanDevice gpu;
  auto get = venture::gfx::gpu_device().get_instance_proc_addr;
  XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR,
      nullptr, gpu.instance, gpu.physical, gpu.device, gpu.family, 0};
  for (auto format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D16_UNORM,
                     VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT,
                     VK_FORMAT_D32_SFLOAT_S8_UINT}) {
    // Every device offers some depth/stencil formats, not all (RADV has no D24S8). GuestImages takes host
    // formats (create_swapchain substitutes 32-bit depth for a guest's D24 first), so ask the host driver: the
    // guest-facing query reports the emulated D24 formats as supported.
    VkFormatProperties support{};
    const auto& host = venture::gfx::gpu_device();
    reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(host.get_instance_proc_addr(host.instance,
        "vkGetPhysicalDeviceFormatProperties"))(host.physical_device, format, &support);
    if (!(support.optimalTilingFeatures & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                           VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))) continue;
    for (auto usage : {XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT, XR_SWAPCHAIN_USAGE_SAMPLED_BIT}) {
      CAPTURE(format);
      CAPTURE(usage);
      const bool color = format == VK_FORMAT_R8G8B8A8_UNORM;
      const bool stencil = format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
      const VkImageAspectFlags aspects = color ? VK_IMAGE_ASPECT_COLOR_BIT :
          VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
      const auto layout = color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
      info.format = format; info.width = info.height = 8;
      info.arraySize = 2; info.faceCount = info.sampleCount = 1; info.mipCount = 3;
      info.usageFlags = usage;
      GuestImages source(binding, get, info), target(binding, get, info);
      REQUIRE(source.wait(0) == XR_SUCCESS);
      REQUIRE(target.wait(0) == XR_SUCCESS);
      const auto image = source.images()[0].image;
      if (color) {
        // A first attachment render with LOAD_OP_CLEAR uses exactly OpenXR's
        // promised layout, without an application-side initialization barrier.
        VkImageViewCreateInfo iv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        iv.image = image; iv.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; iv.format = format;
        iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
        VkImageView view{};
        REQUIRE(vk_call("vkCreateImageView", gpu.device, &iv, nullptr, &view) == VK_SUCCESS);
        VkAttachmentDescription attachment{};
        attachment.format = format; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &reference;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1; rp.pAttachments = &attachment; rp.subpassCount = 1; rp.pSubpasses = &subpass;
        VkRenderPass pass{};
        REQUIRE(vk_call("vkCreateRenderPass", gpu.device, &rp, nullptr, &pass) == VK_SUCCESS);
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = pass; fb.attachmentCount = 1; fb.pAttachments = &view;
        fb.width = fb.height = 8; fb.layers = 2;
        VkFramebuffer framebuffer{};
        REQUIRE(vk_call("vkCreateFramebuffer", gpu.device, &fb, nullptr, &framebuffer) == VK_SUCCESS);
        REQUIRE(vk_call("vkResetCommandBuffer", gpu.command, 0u) == VK_SUCCESS);
        gpu.begin();
        VkClearValue clear{}; clear.color = {{.2f,.4f,.6f,.8f}};
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = pass; begin.framebuffer = framebuffer; begin.renderArea.extent = {8,8};
        begin.clearValueCount = 1; begin.pClearValues = &clear;
        vk_call("vkCmdBeginRenderPass", gpu.command, &begin, VK_SUBPASS_CONTENTS_INLINE);
        vk_call("vkCmdEndRenderPass", gpu.command);
        gpu.submit();
        vk_call("vkDestroyFramebuffer", gpu.device, framebuffer, nullptr);
        vk_call("vkDestroyRenderPass", gpu.device, pass, nullptr);
        vk_call("vkDestroyImageView", gpu.device, view, nullptr);
      }
      auto barrier = [&](VkImage handle, VkImageLayout old, VkImageLayout next) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.image = handle; b.oldLayout = old; b.newLayout = next;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.srcAccessMask = b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.subresourceRange = {aspects, 0, 3, 0, 2};
        vk_call("vkCmdPipelineBarrier", gpu.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &b);
      };
      REQUIRE(vk_call("vkResetCommandBuffer", gpu.command, 0u) == VK_SUCCESS);
      gpu.begin();
      // No UNDEFINED transition: the first guest use relies on successful wait.
      barrier(image, layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      for (unsigned mip = 0; mip < 3; ++mip) {
        VkImageSubresourceRange range{aspects, mip, 1, 0, 2};
        if (color) {
          VkClearColorValue value{{float(32 + 64 * mip) / 255, .4f, .6f, .8f}};
          vk_call("vkCmdClearColorImage", gpu.command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1u, &range);
        } else {
          VkClearDepthStencilValue value{.25f * (mip + 1), 73 + mip};
          vk_call("vkCmdClearDepthStencilImage", gpu.command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1u, &range);
        }
      }
      barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layout);
      gpu.submit();
      REQUIRE(source.copy(0, target.images()[0].image, format) == XR_SUCCESS);
      REQUIRE(source.wait(0) == XR_SUCCESS);
      // Host chains are single-level: compare the base mip of every layer,
      // including stencil separately.
      for (auto aspect : {VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_ASPECT_STENCIL_BIT}) {
        if (!(aspects & aspect)) continue;
        const auto bytes_per_pixel = aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? 1u : format == VK_FORMAT_D16_UNORM ? 2u : 4u;
        for (unsigned mip = 0; mip < 1; ++mip) {
          const auto extent = 8u >> mip;
          VulkanBuffer original(gpu, extent * extent * 2 * bytes_per_pixel);
          VulkanBuffer copied(gpu, original.size);
          REQUIRE(vk_call("vkResetCommandBuffer", gpu.command, 0u) == VK_SUCCESS);
          gpu.begin();
          for (auto pair : {std::pair{image, original.buffer}, std::pair{target.images()[0].image, copied.buffer}}) {
            barrier(pair.first, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy region{};
            region.imageSubresource = {VkImageAspectFlags(aspect), mip, 0, 2};
            region.imageExtent = {extent, extent, 1};
            vk_call("vkCmdCopyImageToBuffer", gpu.command, pair.first, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pair.second, 1u, &region);
            barrier(pair.first, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout);
          }
          gpu.submit();
          if (format == VK_FORMAT_D24_UNORM_S8_UINT && aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
            // Depth-only D24 transfers have an undefined padding byte. Compare
            // the authored depth bits, not driver-specific padding contents.
            const auto* a = static_cast<const unsigned char*>(original.mapped);
            const auto* b = static_cast<const unsigned char*>(copied.mapped);
            for (std::size_t offset = 0; offset < original.size; offset += 4)
              CHECK(std::memcmp(a + offset, b + offset, 3) == 0);
          } else {
            CHECK(std::memcmp(original.mapped, copied.mapped, original.size) == 0);
          }
          if (color) CHECK(static_cast<unsigned char*>(copied.mapped)[0] == 32 + 64 * mip);
          if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT) CHECK(static_cast<unsigned char*>(copied.mapped)[0] == 73 + mip);
        }
      }
    }
  }
}

TEST_CASE("XR lazy swapchains retain guest color and recycle only inactive host chains") {
  venture::test::ZeroCopySetting copies("0");  // the copy path
  namespace g = venture::hle::openxr::guest;
  MockXrScript script;
  script.swapchain_limit = 16;
  ventureMockXrSetScript(&script);
  const char *names[]{"XR_KHR_vulkan_enable2", "XR_FB_color_space"};
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  ci.enabledExtensionCount = 2;
  ci.enabledExtensionNames = names;
  XrInstance instance{};
  REQUIRE(g::xrCreateInstance(&ci, &instance) == XR_SUCCESS);
  auto &gpu = venture::gfx::gpu_device();
  XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR,
      nullptr, gpu.instance, gpu.physical_device, gpu.device,
      gpu.queue_family, gpu.queue_index};
  XrSessionCreateInfo sc{XR_TYPE_SESSION_CREATE_INFO, &binding, 0,
      venture::xr::host_xr().system};
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.format = VK_FORMAT_R8G8B8A8_SRGB;
  info.width = info.height = 8;
  info.sampleCount = info.faceCount = 1;
  info.mipCount = 3;
  info.arraySize = 2;
  info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  const auto rejections_before = ventureMockXrStats().swapchain_limit_rejections;
  VkCommandPool pool{};
  VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cp.queueFamilyIndex = gpu.queue_family;
  auto get = gpu.get_instance_proc_addr;
  auto fn = [&]<class T>(const char* name) { return reinterpret_cast<T>(get(gpu.instance, name)); };
  REQUIRE(fn.operator()<PFN_vkCreateCommandPool>("vkCreateCommandPool")(gpu.device, &cp, nullptr, &pool) == VK_SUCCESS);
  auto initialize = [&](VkImage image, float seed) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer command{};
    REQUIRE(fn.operator()<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(gpu.device, &ai, &command) == VK_SUCCESS);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    REQUIRE(fn.operator()<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command, &bi) == VK_SUCCESS);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.image = image;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 3, 0, 2};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    auto transition = fn.operator()<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    transition(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkClearColorValue color{{seed, .4f, .6f, .8f}};
    fn.operator()<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(command, image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &barrier.subresourceRange);
    barrier.oldLayout = barrier.newLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    transition(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    REQUIRE(fn.operator()<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command) == VK_SUCCESS);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
    REQUIRE(fn.operator()<PFN_vkQueueSubmit>("vkQueueSubmit")(gpu.queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
    REQUIRE(fn.operator()<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(gpu.queue) == VK_SUCCESS);
    fn.operator()<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(gpu.device, pool, 1, &command);
  };
  // Both explicit compositor teardown and implicit session-child teardown must
  // release the full budget. A second session catches lifetime accounting leaks.
  for (unsigned rebuild = 0; rebuild < 3; ++rebuild) {
    XrSession session{};
    REQUIRE(g::xrCreateSession(instance, &sc, &session) == XR_SUCCESS);
    if (rebuild == 0) {
      for (unsigned invalid = 0; invalid < 3; ++invalid) {
        auto bad = info;
        if (invalid == 0) bad.format = VK_FORMAT_R64_SFLOAT;
        if (invalid == 1) bad.sampleCount = 2; // mock advertises a maximum of one
        if (invalid == 2) bad.width = 1u << 20; // beyond any guest device image limit
        XrSwapchain rejected{};
        CHECK(XR_FAILED(g::xrCreateSwapchain(session, &bad, &rejected)));
        CHECK(rejected == XR_NULL_HANDLE);
        CHECK(ventureMockXrStats().live_swapchains == 0);
      }
    }
    REQUIRE(g::xrSetColorSpaceFB(session, XR_COLOR_SPACE_P3_FB) == XR_SUCCESS);
    XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
        nullptr, XR_REFERENCE_SPACE_TYPE_LOCAL, {{0,0,0,1},{0,0,0}}};
    XrSpace space{};
    REQUIRE(g::xrCreateReferenceSpace(session, &reference, &space) == XR_SUCCESS);
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (g::xrPollEvent(instance, &event) == XR_SUCCESS) event = {XR_TYPE_EVENT_DATA_BUFFER};
    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    REQUIRE(g::xrBeginSession(session, &begin) == XR_SUCCESS);
    std::map<VkImage, std::vector<unsigned char>> originals;
    std::vector<XrSwapchain> chains(24);
    for (auto &chain : chains) {
      auto create = info;
      if (&chain == &chains.back()) create.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
      REQUIRE(g::xrCreateSwapchain(session, &create, &chain) == XR_SUCCESS);
    }
    XrSwapchain excess{};
    CHECK(g::xrCreateSwapchain(session, &info, &excess) == XR_SUCCESS);
    CHECK(excess != XR_NULL_HANDLE);
    CHECK(ventureMockXrStats().live_swapchains == 0); // creation/enumeration never needs a host chain
    for (unsigned frame_number = 0; frame_number < 14; ++frame_number) {
      XrFrameState frame{XR_TYPE_FRAME_STATE};
      REQUIRE(g::xrWaitFrame(session, nullptr, &frame) == XR_SUCCESS);
      REQUIRE(g::xrBeginFrame(session, nullptr) == XR_SUCCESS);
      for (auto chain : chains) {
        if (chain == chains.back() && frame_number) continue; // static contents stay converted
        std::uint32_t index{};
        REQUIRE(g::xrAcquireSwapchainImage(chain, nullptr, &index) == XR_SUCCESS);
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
        REQUIRE(g::xrWaitSwapchainImage(chain, &wait) == XR_SUCCESS);
        const auto image = venture::hle::openxr::state().swapchains.at(chain).images[index].image;
        const bool fresh = !originals.contains(image);
        if (fresh) initialize(image, float(std::find(chains.begin(), chains.end(), chain) - chains.begin() + 1) / 32);
        XrSwapchainSubImage sub{chain, {{0,0},{8,8}}, 0};
        auto read = [&] {
          std::vector<unsigned char> pixels;
          std::string error;
          REQUIRE(read_eye(binding, get, image, VkFormat(info.format), sub, pixels, &error));
          return pixels;
        };
        if (fresh) originals[image] = read();
        else CHECK(read() == originals.at(image)); // exact restoration before guest access
        REQUIRE(g::xrReleaseSwapchainImage(chain, nullptr) == XR_SUCCESS);
        CHECK(read() == originals.at(image)); // release must not transform guest-only render targets

      }
      // Rotate two presented chains through 24 live guests. The first eight
      // pairs fit the real host limit, then LRU eviction/recreation is required.
      const auto eye_chain = chains[(2 * frame_number) % chains.size()];
      const auto quad_chain = chains[(2 * frame_number + 1) % chains.size()];
      XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                              {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
      for (unsigned i = 0; i < 2; ++i) {
        views[i].pose.orientation.w = 1;
        views[i].fov = {-.5f,.5f,.5f,-.5f};
        views[i].subImage = {eye_chain, {{1,2},{4,4}}, i};
      }
      XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION, nullptr, 0, space, 2, views};
      XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD, nullptr, 0, space,
          XR_EYE_VISIBILITY_BOTH, {quad_chain, {{0,0},{8,8}}, 0}, {{0,0,0,1},{0,0,-1}}, {1,1}};
      const XrCompositionLayerBaseHeader *layers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),
          reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
      XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, frame.predictedDisplayTime,
          XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 2, layers};
      REQUIRE(g::xrEndFrame(session, &end) == XR_SUCCESS);
      CHECK(venture::hle::openxr::state().swapchains.at(eye_chain).host_info.mipCount == 1);
      XrSwapchainSubImage host[3]{};
      REQUIRE(ventureMockXrLastSubImages(host, 3) == 3);
      CHECK(host[0].swapchain == venture::hle::openxr::state().swapchains.at(eye_chain).host_handle);
      CHECK(host[1].swapchain == host[0].swapchain);
      CHECK(host[2].swapchain == venture::hle::openxr::state().swapchains.at(quad_chain).host_handle);
      CHECK(host[0].swapchain != eye_chain);
      CHECK(views[0].subImage.swapchain == eye_chain); // caller memory is unchanged
      for (auto chain : {eye_chain, quad_chain}) {
        const auto& record = venture::hle::openxr::state().swapchains.at(chain);
        const auto guest_image = record.images.at(record.released_index).image;
        const auto& original = originals.at(guest_image);
        // Same size and format: the pass draws from the game's image directly.
        CHECK(record.backing->sampled());
        XrSwapchainSubImage sub{record.host_handle, {{0,0},{8,8}}, 0};
        std::vector<unsigned char> converted;
        std::string error;
        REQUIRE(read_eye(binding, get, record.host_images.at(record.host_index).image,
            VkFormat(info.format), sub, converted, &error));
        // The game's P3 goes to a host without XR_FB_color_space unconverted.
        for (unsigned pixel = 0; pixel < converted.size(); pixel += 4)
          for (unsigned c = 0; c < 4; ++c) CHECK(converted[pixel+c] == original[pixel+c]);
      }
      CHECK(host[1].imageArrayIndex == 1);
      CHECK(host[0].imageRect.offset.y == 2);
      CHECK(ventureMockXrStats().live_swapchains <= 16);
    }
    CHECK(venture::hle::openxr::state().host_swapchain_limit == 16);
    CHECK(venture::hle::openxr::state().host_evictions > 0);
    // A frame that genuinely needs 17 distinct host chains cannot fit. Never
    // steal a pinned member merely to make the next allocation succeed.
    std::set<XrSwapchain> oversized(chains.begin(), chains.begin() + 17);
    std::map<XrSwapchain, XrSwapchain> pinned;
    for (auto chain : oversized) {
      const auto& record = venture::hle::openxr::state().swapchains.at(chain);
      if (record.host_handle) pinned[chain] = record.host_handle;
    }
    CHECK(venture::hle::openxr::prepare_swapchains(oversized) == XR_ERROR_LIMIT_REACHED);
    CHECK(ventureMockXrStats().live_swapchains == 16);
    for (auto [guest, host] : pinned)
      CHECK(venture::hle::openxr::state().swapchains.at(guest).host_handle == host);
    CHECK(venture::hle::openxr::state().swapchains.size() == 25);
    if (rebuild == 0) {
      // Hold an actual guest wait outside the API mutex. At capacity every
      // other resident is frame-pinned, so eviction must leave this one alone.
      auto& state = venture::hle::openxr::state();
      XrSwapchain victim{}, missing{};
      std::set<XrSwapchain> frame_set;
      for (auto chain : chains) {
        if (state.swapchains.at(chain).host_handle) {
          if (!victim && chain != chains.back()) victim = chain;
          else frame_set.insert(chain);
        } else missing = chain;
      }
      REQUIRE(victim); REQUIRE(missing);
      frame_set.insert(missing);
      auto& record = state.swapchains.at(victim);
      const auto old_host = record.host_handle;
      ColorAllocationProbe probe(get);
      record.backing = std::make_unique<GuestImages>(binding, probe.get, info);
      record.images.assign(record.backing->images().begin(), record.backing->images().end());
      std::uint32_t index{};
      REQUIRE(g::xrAcquireSwapchainImage(victim, nullptr, &index) == XR_SUCCESS);
      {
        std::lock_guard lock(probe.gate_mutex);
        probe.hold = true; probe.entered = false;
      }
      auto waiter = std::async(std::launch::async, [&] {
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
        return g::xrWaitSwapchainImage(victim, &wait);
      });
      bool entered;
      {
        std::unique_lock lock(probe.gate_mutex);
        entered = probe.gate_cv.wait_for(lock, std::chrono::seconds(5), [&] { return probe.entered; });
      }
      CHECK(entered);
      std::future<XrResult> eviction;
      if (entered) {
        eviction = std::async(std::launch::async, [&] {
          std::lock_guard lock(state.mutex);
          return venture::hle::openxr::prepare_swapchains(frame_set);
        });
        // Check before releasing the wait gate, but always release it even
        // on regression so the test reports a failure instead of hanging.
        CHECK(eviction.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
      }
      {
        std::lock_guard lock(probe.gate_mutex); probe.hold = false;
      }
      probe.gate_cv.notify_all();
      if (eviction.valid()) CHECK(eviction.get() == XR_ERROR_LIMIT_REACHED);
      CHECK(waiter.get() == XR_SUCCESS);
      CHECK(record.host_handle == old_host);
      REQUIRE(g::xrReleaseSwapchainImage(victim, nullptr) == XR_SUCCESS);
      // Qualified success from host wait still makes the image available.
      // Preserve the chain, balance the acquisition, and propagate the result.
      script.swapchain_wait_result = XR_SESSION_LOSS_PENDING;
      ventureMockXrSetScript(&script);
      XrFrameState frame{XR_TYPE_FRAME_STATE};
      REQUIRE(g::xrWaitFrame(session, nullptr, &frame) == XR_SUCCESS);
      REQUIRE(g::xrBeginFrame(session, nullptr) == XR_SUCCESS);
      XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD, nullptr, 0, space,
          XR_EYE_VISIBILITY_BOTH, {victim, {{0,0},{8,8}}, 0}, {{0,0,0,1},{0,0,-1}}, {1,1}};
      const auto* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
      XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, frame.predictedDisplayTime,
          XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1, &layer};
      const auto ends = ventureMockXrStats().ends;
      CHECK(g::xrEndFrame(session, &end) == XR_SESSION_LOSS_PENDING);
      CHECK(record.host_handle == old_host);
      CHECK(ventureMockXrStats().ends == ends + 1);
      CHECK(quad.subImage.swapchain == victim);
      script.swapchain_wait_result = XR_SUCCESS;
      ventureMockXrSetScript(&script);
      REQUIRE(g::xrWaitFrame(session, nullptr, &frame) == XR_SUCCESS);
      REQUIRE(g::xrBeginFrame(session, nullptr) == XR_SUCCESS);
      end.displayTime = frame.predictedDisplayTime;
      end.layerCount = 0;
      REQUIRE(g::xrEndFrame(session, &end) == XR_SUCCESS);
    }
    if (rebuild == 0) {
      // Host chains of destroyed guest chains stay parked for reuse until the
      // session ends (dev_dump.cpp; the Oculus runtime faults in a running
      // session's xrDestroySwapchain). A new chain like them takes one back.
#ifdef _WIN32
      _putenv_s("VENTURE_XR_PARK_SWAPCHAINS", "1");
#else
      setenv("VENTURE_XR_PARK_SWAPCHAINS", "1", 1);
#endif
      venture::environment::refresh();
      const auto live = ventureMockXrStats().live_swapchains;
      for (auto chain : chains) REQUIRE(g::xrDestroySwapchain(chain) == XR_SUCCESS);
      CHECK(ventureMockXrStats().live_swapchains == live);
      XrSwapchain again{};
      REQUIRE(g::xrCreateSwapchain(session, &info, &again) == XR_SUCCESS);
      std::uint32_t index{};
      REQUIRE(g::xrAcquireSwapchainImage(again, nullptr, &index) == XR_SUCCESS);
      XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
      REQUIRE(g::xrWaitSwapchainImage(again, &wait) == XR_SUCCESS);
      REQUIRE(g::xrReleaseSwapchainImage(again, nullptr) == XR_SUCCESS);
      {
        std::lock_guard lock(venture::hle::openxr::state().mutex);
        CHECK(venture::hle::openxr::prepare_swapchains({again}) == XR_SUCCESS);
        CHECK(venture::hle::openxr::state().swapchains.at(again).host_handle != XR_NULL_HANDLE);
      }
      CHECK(ventureMockXrStats().live_swapchains == live);
      REQUIRE(g::xrDestroySwapchain(again) == XR_SUCCESS);
#ifdef _WIN32
      _putenv_s("VENTURE_XR_PARK_SWAPCHAINS", "");
#else
      unsetenv("VENTURE_XR_PARK_SWAPCHAINS");
#endif
      venture::environment::refresh();
    }
    REQUIRE(g::xrDestroySession(session) == XR_SUCCESS);
    CHECK(ventureMockXrStats().live_swapchains == 0);
    CHECK(ventureMockXrStats().live_images == 0);
  }
  CHECK(ventureMockXrStats().peak_swapchains >= 16);
  CHECK(ventureMockXrStats().swapchain_limit_rejections == rejections_before + 3);
  fn.operator()<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(gpu.device, pool, nullptr);
  REQUIRE(g::xrDestroyInstance(instance) == XR_SUCCESS);
  script.swapchain_limit = 0;
  ventureMockXrSetScript(&script);
}

TEST_CASE("XR guest chains over the host maximum present through a fitted host chain") {
  // A Quest app may size its eye chain past a desktop runtime's maximum (the
  // mock advertises 4096). Venture keeps the guest size and scales the copy.
  namespace g = venture::hle::openxr::guest;
  const char *names[]{"XR_KHR_vulkan_enable2"};
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  ci.enabledExtensionCount = 1;
  ci.enabledExtensionNames = names;
  XrInstance instance{};
  REQUIRE(g::xrCreateInstance(&ci, &instance) == XR_SUCCESS);
  auto &gpu = venture::gfx::gpu_device();
  auto get = gpu.get_instance_proc_addr;
  XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR,
      nullptr, gpu.instance, gpu.physical_device, gpu.device,
      gpu.queue_family, gpu.queue_index};
  XrSessionCreateInfo sc{XR_TYPE_SESSION_CREATE_INFO, &binding, 0,
      venture::xr::host_xr().system};
  XrSession session{};
  REQUIRE(g::xrCreateSession(instance, &sc, &session) == XR_SUCCESS);
  XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
      nullptr, XR_REFERENCE_SPACE_TYPE_LOCAL, {{0,0,0,1},{0,0,0}}};
  XrSpace space{};
  REQUIRE(g::xrCreateReferenceSpace(session, &reference, &space) == XR_SUCCESS);
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  while (g::xrPollEvent(instance, &event) == XR_SUCCESS) event = {XR_TYPE_EVENT_DATA_BUFFER};
  XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
  REQUIRE(g::xrBeginSession(session, &begin) == XR_SUCCESS);
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.format = VK_FORMAT_R8G8B8A8_UNORM;
  info.width = 5000;
  info.height = 10;
  info.sampleCount = info.faceCount = info.mipCount = info.arraySize = 1;
  info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
  XrSwapchain chain{};
  REQUIRE(g::xrCreateSwapchain(session, &info, &chain) == XR_SUCCESS);
  XrFrameState frame{XR_TYPE_FRAME_STATE};
  REQUIRE(g::xrWaitFrame(session, nullptr, &frame) == XR_SUCCESS);
  REQUIRE(g::xrBeginFrame(session, nullptr) == XR_SUCCESS);
  std::uint32_t index{};
  REQUIRE(g::xrAcquireSwapchainImage(chain, nullptr, &index) == XR_SUCCESS);
  XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
  REQUIRE(g::xrWaitSwapchainImage(chain, &wait) == XR_SUCCESS);
  // xrWaitSwapchainImage promises the attachment layout, not initialized
  // contents. Define a nonzero uniform image before testing a scaled copy.
  const auto& acquired = venture::hle::openxr::state().swapchains.at(chain);
  const auto image = acquired.images.at(index).image;
  auto fn = [&]<class T>(const char* name) {
    return reinterpret_cast<T>(get(gpu.instance, name));
  };
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.queueFamilyIndex = gpu.queue_family;
  VkCommandPool pool{};
  REQUIRE(fn.operator()<PFN_vkCreateCommandPool>("vkCreateCommandPool")(
      gpu.device, &pool_info, nullptr, &pool) == VK_SUCCESS);
  VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
  VkCommandBuffer command{};
  REQUIRE(fn.operator()<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(
      gpu.device, &allocate, &command) == VK_SUCCESS);
  VkCommandBufferBeginInfo command_begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  REQUIRE(fn.operator()<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(
      command, &command_begin) == VK_SUCCESS);
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.image = image;
  barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  auto transition = fn.operator()<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
  transition(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
      0, 0, nullptr, 0, nullptr, 1, &barrier);
  VkClearColorValue color{{1.f, 0.f, 1.f, 1.f}};
  fn.operator()<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(command,
      image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &barrier.subresourceRange);
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  transition(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
      0, 0, nullptr, 0, nullptr, 1, &barrier);
  REQUIRE(fn.operator()<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command) == VK_SUCCESS);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command;
  REQUIRE(fn.operator()<PFN_vkQueueSubmit>("vkQueueSubmit")(
      gpu.queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
  REQUIRE(fn.operator()<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(gpu.queue) == VK_SUCCESS);
  fn.operator()<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(gpu.device, pool, nullptr);
  REQUIRE(g::xrReleaseSwapchainImage(chain, nullptr) == XR_SUCCESS);
  // Right half of the guest image: guest pixels 2500..5000 of 5000.
  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD, nullptr, 0, space,
      XR_EYE_VISIBILITY_BOTH, {chain, {{2500,0},{2500,10}}, 0}, {{0,0,0,1},{0,0,-1}}, {1,1}};
  const XrCompositionLayerBaseHeader *layers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
  XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, frame.predictedDisplayTime,
      XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1, layers};
  REQUIRE(g::xrEndFrame(session, &end) == XR_SUCCESS);
  const auto& record = venture::hle::openxr::state().swapchains.at(chain);
  REQUIRE(record.info.format == VK_FORMAT_R8G8B8A8_UNORM);
  REQUIRE(record.host_info.format == VK_FORMAT_R8G8B8A8_UNORM);
  CHECK(record.info.width == 5000);
  CHECK(record.host_info.width == 4096);
  CHECK(record.host_info.height == 8); // 10 * 4096 / 5000, aspect kept
  XrSwapchainSubImage host{};
  REQUIRE(ventureMockXrLastSubImages(&host, 1) == 1);
  CHECK(host.swapchain == record.host_handle);
  CHECK(host.imageRect.offset.x == 2048);
  CHECK(host.imageRect.extent.width == 2048);
  CHECK(host.imageRect.offset.y == 0);
  CHECK(host.imageRect.extent.height == 8);
  // The explicit guest clear must survive the scaled copy, not merely match
  // another uninitialized allocation or an all-zero image.
  std::vector<unsigned char> guest_pixels, host_pixels;
  std::string error;
  XrSwapchainSubImage guest_sub{chain, {{0,0},{8,8}}, 0};
  REQUIRE(read_eye(binding, get, record.images.at(record.released_index).image,
      VK_FORMAT_R8G8B8A8_UNORM, guest_sub, guest_pixels, &error));
  XrSwapchainSubImage host_sub{record.host_handle, {{0,0},{8,8}}, 0};
  REQUIRE(read_eye(binding, get, record.host_images.at(record.host_index).image,
      VK_FORMAT_R8G8B8A8_UNORM, host_sub, host_pixels, &error));
  const std::array<unsigned char, 4> expected{255, 0, 255, 255};
  REQUIRE(guest_pixels.size() == 8 * 8 * 4);
  REQUIRE(host_pixels.size() == guest_pixels.size());
  for (std::size_t i = 0; i < guest_pixels.size(); ++i) {
    CAPTURE(i);
    CHECK(guest_pixels[i] == expected[i % expected.size()]);
    CHECK(host_pixels[i] == expected[i % expected.size()]);
  }
  REQUIRE(g::xrDestroySwapchain(chain) == XR_SUCCESS);
  REQUIRE(g::xrDestroySession(session) == XR_SUCCESS);
  REQUIRE(g::xrDestroyInstance(instance) == XR_SUCCESS);
}

TEST_CASE("XR eye presentation fits the reported eye size and keeps small overshoots 1:1") {
  using venture::xr::eye_presentation_extent;
  // H3VR2 at 1.6x of a 2684x2860 recommendation.
  const auto h3 = eye_presentation_extent({4296, 4576}, {4296, 4576}, {2684, 2860});
  CHECK(h3.width == 2684);
  CHECK(h3.height == 2859);
  // A side-by-side chain scales by its eye rect, not its width.
  const auto pair = eye_presentation_extent({3200, 1600}, {1600, 1600}, {1024, 1024});
  CHECK(pair.width == 2048);
  CHECK(pair.height == 1024);
  // Within 5% (rounding, padding) and at or below the size: unchanged.
  CHECK(eye_presentation_extent({1050, 1050}, {1050, 1050}, {1024, 1024}).width == 1050);
  CHECK(eye_presentation_extent({900, 900}, {900, 900}, {1024, 1024}).width == 900);
  CHECK(eye_presentation_extent({2000, 2000}, {2000, 2000}, {0, 0}).width == 2000);
}

TEST_CASE("XR eye chains past the reported eye size present through a host chain of that size") {
  venture::test::ZeroCopySetting copies("0");  // the copy path
  // The mock recommends 1024x1024 per eye. A game multiplying that renders a
  // larger eye; the host chain holds the reported size, a quad keeps its own.
  namespace g = venture::hle::openxr::guest;
  const char *names[]{"XR_KHR_vulkan_enable2"};
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  ci.enabledExtensionCount = 1;
  ci.enabledExtensionNames = names;
  XrInstance instance{};
  REQUIRE(g::xrCreateInstance(&ci, &instance) == XR_SUCCESS);
  auto &gpu = venture::gfx::gpu_device();
  XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR,
      nullptr, gpu.instance, gpu.physical_device, gpu.device,
      gpu.queue_family, gpu.queue_index};
  XrSessionCreateInfo sc{XR_TYPE_SESSION_CREATE_INFO, &binding, 0, venture::xr::host_xr().system};
  XrSession session{};
  REQUIRE(g::xrCreateSession(instance, &sc, &session) == XR_SUCCESS);
  XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
      nullptr, XR_REFERENCE_SPACE_TYPE_LOCAL, {{0,0,0,1},{0,0,0}}};
  XrSpace space{};
  REQUIRE(g::xrCreateReferenceSpace(session, &reference, &space) == XR_SUCCESS);
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  while (g::xrPollEvent(instance, &event) == XR_SUCCESS) event = {XR_TYPE_EVENT_DATA_BUFFER};
  XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
  REQUIRE(g::xrBeginSession(session, &begin) == XR_SUCCESS);
  auto create = [&](std::uint32_t width, std::uint32_t height) {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.width = width;
    info.height = height;
    info.sampleCount = info.faceCount = info.mipCount = info.arraySize = 1;
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    XrSwapchain chain{};
    REQUIRE(g::xrCreateSwapchain(session, &info, &chain) == XR_SUCCESS);
    std::uint32_t index{};
    REQUIRE(g::xrAcquireSwapchainImage(chain, nullptr, &index) == XR_SUCCESS);
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    REQUIRE(g::xrWaitSwapchainImage(chain, &wait) == XR_SUCCESS);
    REQUIRE(g::xrReleaseSwapchainImage(chain, nullptr) == XR_SUCCESS);
    return chain;
  };
  XrFrameState frame{XR_TYPE_FRAME_STATE};
  REQUIRE(g::xrWaitFrame(session, nullptr, &frame) == XR_SUCCESS);
  REQUIRE(g::xrBeginFrame(session, nullptr) == XR_SUCCESS);
  // Both eyes side by side at 1.5625x, and a 2000x2000 quad panel.
  const auto eyes = create(3200, 1600), panel = create(2000, 2000);
  XrCompositionLayerProjectionView views[2]{};
  for (int i = 0; i < 2; ++i) {
    views[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    views[i].pose = {{0,0,0,1},{0,0,0}};
    views[i].fov = {-0.8f, 0.8f, 0.8f, -0.8f};
    views[i].subImage = {eyes, {{i * 1600, 0}, {1600, 1600}}, 0};
  }
  XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION, nullptr, 0, space, 2, views};
  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD, nullptr, 0, space,
      XR_EYE_VISIBILITY_BOTH, {panel, {{0,0},{2000,2000}}, 0}, {{0,0,0,1},{0,0,-1}}, {1,1}};
  const XrCompositionLayerBaseHeader *layers[]{
      reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),
      reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
  XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, frame.predictedDisplayTime,
      XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 2, layers};
  REQUIRE(g::xrEndFrame(session, &end) == XR_SUCCESS);
  const auto& eye_record = venture::hle::openxr::state().swapchains.at(eyes);
  CHECK(eye_record.host_info.width == 2048);
  CHECK(eye_record.host_info.height == 1024);
  const auto& panel_record = venture::hle::openxr::state().swapchains.at(panel);
  CHECK(panel_record.host_info.width == 2000);
  CHECK(panel_record.host_info.height == 2000);
  XrSwapchainSubImage host[3]{};
  REQUIRE(ventureMockXrLastSubImages(host, 3) == 3);
  for (int i = 0; i < 2; ++i) {
    CAPTURE(i);
    CHECK(host[i].swapchain == eye_record.host_handle);
    CHECK(host[i].imageRect.offset.x == i * 1024);
    CHECK(host[i].imageRect.extent.width == 1024);
    CHECK(host[i].imageRect.extent.height == 1024);
  }
  CHECK(host[2].imageRect.extent.width == 2000);
  REQUIRE(g::xrDestroySwapchain(eyes) == XR_SUCCESS);
  REQUIRE(g::xrDestroySwapchain(panel) == XR_SUCCESS);
  REQUIRE(g::xrDestroySession(session) == XR_SUCCESS);
  REQUIRE(g::xrDestroyInstance(instance) == XR_SUCCESS);
}

TEST_CASE("XR fields of view that contain the host's crop to it exactly") {
  using venture::xr::crop_view_rect;
  using venture::xr::fov_crop;
  const auto rad = [](float degrees) { return degrees * 3.14159265358979f / 180; };
  // Underdogs (Unity Symmetric Projection) on the Meta Horizon PC runtime: the
  // host's left eye is 52/45 degrees out/in, 48 up and 50 down; the game renders 52/52/50/50.
  const XrFovf host{rad(-52), rad(45), rad(48), rad(-50)};
  const XrFovf symmetric{rad(-52), rad(52), rad(50), rad(-50)};
  const auto crop = fov_crop(symmetric, host);
  REQUIRE(crop);
  CHECK(crop->left == doctest::Approx(0));
  CHECK(crop->right == doctest::Approx((std::tan(rad(52)) - 1) / (2 * std::tan(rad(52)))).epsilon(1e-4));
  CHECK(crop->up == doctest::Approx((std::tan(rad(50)) - std::tan(rad(48))) / (2 * std::tan(rad(50)))).epsilon(1e-4));
  CHECK(crop->down == doctest::Approx(0));
  // A Vulkan image has its up edge at row 0; a GLES image at its last row.
  const auto right_pixels = std::int32_t(std::lround(2784 * crop->right));
  const auto up_pixels = std::int32_t(std::lround(2536 * crop->up));
  CHECK(right_pixels == 304);
  CHECK(up_pixels == 86);
  const auto vulkan = crop_view_rect({{0, 0}, {2784, 2536}}, *crop, true);
  CHECK(vulkan.offset.x == 0);
  CHECK(vulkan.extent.width == 2784 - right_pixels);
  CHECK(vulkan.offset.y == up_pixels);
  CHECK(vulkan.extent.height == 2536 - up_pixels);
  const auto gles = crop_view_rect({{10, 20}, {2784, 2536}}, *crop, false);
  CHECK(gles.offset.x == 10);
  CHECK(gles.offset.y == 20);
  CHECK(gles.extent.height == 2536 - up_pixels);
  // The right eye mirrors it: the crop comes off its left side.
  const auto right = fov_crop(symmetric, {rad(-45), rad(52), rad(48), rad(-50)});
  REQUIRE(right);
  CHECK(crop_view_rect({{0, 0}, {2784, 2536}}, *right, true).offset.x == right_pixels);
  // Matching fields, and fields that do not contain the host's, are left alone.
  CHECK_FALSE(fov_crop(host, host));
  CHECK_FALSE(fov_crop(host, symmetric));
  CHECK_FALSE(fov_crop({rad(-40), rad(40), rad(40), rad(-40)}, host));
  CHECK_FALSE(fov_crop({0, 0, 0, 0}, host));
}

TEST_CASE("XR views wider than a fixed host field of view present through the crop that covers it") {
  // The mock reports fovMutable false, as the Meta Horizon PC runtime does; such
  // a host may compose with its own field of view whatever the game submits.
  namespace g = venture::hle::openxr::guest;
  const auto rad = [](float degrees) { return degrees * 3.14159265358979f / 180; };
  struct Restore {
    ~Restore() {
      MockXrScript defaults{};
      ventureMockXrSetScript(&defaults);
    }
  } restore;
  MockXrScript script{};
  script.view_fov = std::array{rad(-52), rad(45), rad(48), rad(-50)};
  ventureMockXrSetScript(&script);
  const char *names[]{"XR_KHR_vulkan_enable2"};
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  ci.enabledExtensionCount = 1;
  ci.enabledExtensionNames = names;
  XrInstance instance{};
  REQUIRE(g::xrCreateInstance(&ci, &instance) == XR_SUCCESS);
  auto &gpu = venture::gfx::gpu_device();
  XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR,
      nullptr, gpu.instance, gpu.physical_device, gpu.device,
      gpu.queue_family, gpu.queue_index};
  XrSessionCreateInfo sc{XR_TYPE_SESSION_CREATE_INFO, &binding, 0, venture::xr::host_xr().system};
  XrSession session{};
  REQUIRE(g::xrCreateSession(instance, &sc, &session) == XR_SUCCESS);
  XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
      nullptr, XR_REFERENCE_SPACE_TYPE_LOCAL, {{0,0,0,1},{0,0,0}}};
  XrSpace space{};
  REQUIRE(g::xrCreateReferenceSpace(session, &reference, &space) == XR_SUCCESS);
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  while (g::xrPollEvent(instance, &event) == XR_SUCCESS) event = {XR_TYPE_EVENT_DATA_BUFFER};
  XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
  REQUIRE(g::xrBeginSession(session, &begin) == XR_SUCCESS);
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.format = VK_FORMAT_R8G8B8A8_UNORM;
  info.width = 400;
  info.height = 300;
  info.sampleCount = info.faceCount = info.mipCount = 1;
  info.arraySize = 2;
  info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
  XrSwapchain eyes{};
  REQUIRE(g::xrCreateSwapchain(session, &info, &eyes) == XR_SUCCESS);
  struct Submitted {
    std::array<XrSwapchainSubImage, 2> subimages{};
    std::array<XrFovf, 2> fov{};
  };
  // Submits both eyes with the given fields; returns the host's rects and fields.
  auto submit = [&](const XrFovf& left, const XrFovf& right) {
    XrFrameState frame{XR_TYPE_FRAME_STATE};
    REQUIRE(g::xrWaitFrame(session, nullptr, &frame) == XR_SUCCESS);
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                            frame.predictedDisplayTime, space};
    XrViewState state{XR_TYPE_VIEW_STATE};
    std::array<XrView, 2> located{XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
    std::uint32_t count = 0;
    REQUIRE(g::xrLocateViews(session, &locate, &state, 2, &count, located.data()) == XR_SUCCESS);
    REQUIRE(g::xrBeginFrame(session, nullptr) == XR_SUCCESS);
    std::uint32_t index{};
    REQUIRE(g::xrAcquireSwapchainImage(eyes, nullptr, &index) == XR_SUCCESS);
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    REQUIRE(g::xrWaitSwapchainImage(eyes, &wait) == XR_SUCCESS);
    REQUIRE(g::xrReleaseSwapchainImage(eyes, nullptr) == XR_SUCCESS);
    XrCompositionLayerProjectionView views[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
      views[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
      views[i].pose = located[i].pose;
      views[i].fov = i ? right : left;
      views[i].subImage = {eyes, {{0, 0}, {400, 300}}, i};
    }
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION, nullptr, 0, space, 2, views};
    const auto* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, frame.predictedDisplayTime,
        XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1, &layer};
    REQUIRE(g::xrEndFrame(session, &end) == XR_SUCCESS);
    Submitted host;
    REQUIRE(ventureMockXrLastSubImages(host.subimages.data(), 2) == 2);
    REQUIRE(ventureMockXrLastProjectionFov(host.fov.data(), 2) == 2);
    // The game's own views are unchanged.
    CHECK(views[0].subImage.imageRect.extent.width == 400);
    CHECK(views[0].fov.angleRight == left.angleRight);
    return host;
  };
  const XrFovf host_fov{rad(-52), rad(45), rad(48), rad(-50)};
  const XrFovf symmetric{rad(-52), rad(52), rad(50), rad(-50)};
  const auto wide = submit(symmetric, symmetric);
  const auto crop = venture::xr::fov_crop(symmetric, host_fov);
  REQUIRE(crop);
  const auto right_pixels = std::int32_t(std::lround(400 * crop->right));
  const auto up_pixels = std::int32_t(std::lround(300 * crop->up));
  CHECK(right_pixels > 0);
  CHECK(up_pixels > 0);
  for (std::uint32_t i = 0; i < 2; ++i) {
    CAPTURE(i);
    CHECK(wide.fov[i].angleLeft == doctest::Approx(host_fov.angleLeft));
    CHECK(wide.fov[i].angleRight == doctest::Approx(host_fov.angleRight));
    CHECK(wide.fov[i].angleUp == doctest::Approx(host_fov.angleUp));
    CHECK(wide.fov[i].angleDown == doctest::Approx(host_fov.angleDown));
    CHECK(wide.subimages[i].imageArrayIndex == i);
    CHECK(wide.subimages[i].imageRect.offset.x == 0);
    CHECK(wide.subimages[i].imageRect.extent.width == 400 - right_pixels);
    CHECK(wide.subimages[i].imageRect.offset.y == up_pixels);
    CHECK(wide.subimages[i].imageRect.extent.height == 300 - up_pixels);
  }
  // Views that match the host's, or are narrower, reach it as submitted.
  const auto matching = submit(host_fov, host_fov);
  CHECK(matching.subimages[0].imageRect.extent.width == 400);
  CHECK(matching.subimages[0].imageRect.offset.y == 0);
  const XrFovf narrow{rad(-40), rad(40), rad(40), rad(-40)};
  const auto narrower = submit(narrow, narrow);
  CHECK(narrower.subimages[1].imageRect.extent.width == 400);
  CHECK(narrower.fov[1].angleLeft == doctest::Approx(narrow.angleLeft));
  // Teardown: the parked host chain goes with the host session, which destroys
  // its swapchains; no xrDestroySwapchain reaches the host for it (the Meta
  // Horizon PC runtime called through null there when Underdogs quit).
  const auto destroys = ventureMockXrStats().swapchain_destroys;
  REQUIRE(g::xrDestroySwapchain(eyes) == XR_SUCCESS);
  CHECK(ventureMockXrStats().live_swapchains == 1);
  REQUIRE(g::xrRequestExitSession(session) == XR_SUCCESS);
  while (g::xrPollEvent(instance, &event) == XR_SUCCESS) event = {XR_TYPE_EVENT_DATA_BUFFER};
  REQUIRE(g::xrEndSession(session) == XR_SUCCESS);
  REQUIRE(g::xrDestroySession(session) == XR_SUCCESS);
  CHECK(ventureMockXrStats().swapchain_destroys == destroys);
  CHECK(ventureMockXrStats().live_swapchains == 0);
  CHECK(ventureMockXrStats().live_images == 0);
  REQUIRE(g::xrDestroyInstance(instance) == XR_SUCCESS);
}

TEST_CASE("XR hosts reporting zero limits get Quest values and unclamped chains") {
  venture::test::ZeroCopySetting copies("0");  // the copy path
  // SteamVR 2.18 with a Quest over Link reports a 0x0 swapchain maximum, no layers and
  // no tracking; Batman's eye chain once shrank to 1x1 and the headset went black.
  namespace g = venture::hle::openxr::guest;
  struct ZeroLimits {
    ZeroLimits() { set("1"); }
    ~ZeroLimits() { set(""); }
    static void set(const char* value) {
#ifdef _WIN32
      _putenv_s("VENTURE_MOCK_ZERO_SYSTEM_LIMITS", value);
#else
      if (*value) setenv("VENTURE_MOCK_ZERO_SYSTEM_LIMITS", value, 1); else unsetenv("VENTURE_MOCK_ZERO_SYSTEM_LIMITS");
#endif
      venture::environment::refresh();
      ventureMockXrRefreshEnvironment();
    }
  } zero;
  const char *names[]{"XR_KHR_vulkan_enable2"};
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  ci.enabledExtensionCount = 1;
  ci.enabledExtensionNames = names;
  XrInstance instance{};
  REQUIRE(g::xrCreateInstance(&ci, &instance) == XR_SUCCESS);
  XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
  REQUIRE(g::xrGetSystemProperties(instance, venture::xr::host_xr().system, &properties) == XR_SUCCESS);
  CHECK(properties.graphicsProperties.maxLayerCount == XR_MIN_COMPOSITION_LAYERS_SUPPORTED);
  CHECK(properties.graphicsProperties.maxSwapchainImageWidth > 0);
  CHECK(properties.graphicsProperties.maxSwapchainImageHeight > 0);
  CHECK(properties.trackingProperties.orientationTracking == XR_TRUE);
  CHECK(properties.trackingProperties.positionTracking == XR_TRUE);
  auto &gpu = venture::gfx::gpu_device();
  XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR,
      nullptr, gpu.instance, gpu.physical_device, gpu.device,
      gpu.queue_family, gpu.queue_index};
  XrSessionCreateInfo sc{XR_TYPE_SESSION_CREATE_INFO, &binding, 0,
      venture::xr::host_xr().system};
  XrSession session{};
  REQUIRE(g::xrCreateSession(instance, &sc, &session) == XR_SUCCESS);
  XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
      nullptr, XR_REFERENCE_SPACE_TYPE_LOCAL, {{0,0,0,1},{0,0,0}}};
  XrSpace space{};
  REQUIRE(g::xrCreateReferenceSpace(session, &reference, &space) == XR_SUCCESS);
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  while (g::xrPollEvent(instance, &event) == XR_SUCCESS) event = {XR_TYPE_EVENT_DATA_BUFFER};
  XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
  REQUIRE(g::xrBeginSession(session, &begin) == XR_SUCCESS);
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.format = VK_FORMAT_R8G8B8A8_UNORM;
  info.width = 2240;
  info.height = 16;
  info.sampleCount = info.faceCount = info.mipCount = info.arraySize = 1;
  info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  XrSwapchain chain{};
  REQUIRE(g::xrCreateSwapchain(session, &info, &chain) == XR_SUCCESS);
  XrFrameState frame{XR_TYPE_FRAME_STATE};
  REQUIRE(g::xrWaitFrame(session, nullptr, &frame) == XR_SUCCESS);
  REQUIRE(g::xrBeginFrame(session, nullptr) == XR_SUCCESS);
  std::uint32_t index{};
  REQUIRE(g::xrAcquireSwapchainImage(chain, nullptr, &index) == XR_SUCCESS);
  XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
  REQUIRE(g::xrWaitSwapchainImage(chain, &wait) == XR_SUCCESS);
  REQUIRE(g::xrReleaseSwapchainImage(chain, nullptr) == XR_SUCCESS);
  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD, nullptr, 0, space,
      XR_EYE_VISIBILITY_BOTH, {chain, {{0,0},{2240,16}}, 0}, {{0,0,0,1},{0,0,-1}}, {1,1}};
  const XrCompositionLayerBaseHeader *layers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
  XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, frame.predictedDisplayTime,
      XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1, layers};
  REQUIRE(g::xrEndFrame(session, &end) == XR_SUCCESS);
  const auto& record = venture::hle::openxr::state().swapchains.at(chain);
  CHECK(record.host_info.width == 2240);
  CHECK(record.host_info.height == 16);
  XrSwapchainSubImage host{};
  REQUIRE(ventureMockXrLastSubImages(&host, 1) == 1);
  CHECK(host.imageRect.extent.width == 2240);
  REQUIRE(g::xrDestroySwapchain(chain) == XR_SUCCESS);
  REQUIRE(g::xrDestroySession(session) == XR_SUCCESS);
  REQUIRE(g::xrDestroyInstance(instance) == XR_SUCCESS);
}
