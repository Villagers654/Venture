#include <functional>
#pragma once
#include "gfx/gpu_device.h"
#include "xr/xr_images.h"
#include <array>
#include <cstring>
#include <doctest/doctest.h>
#include <functional>
#include <map>
#include <utility>
#include <vector>

namespace venture::test {
// A test-only GLES stand-in with real Vulkan images. It does not claim to
// exercise ANGLE: it verifies the XR-side copy contract and ownership order.
class XrImageFixture final : public xr::XrImages {
  gfx::GpuDevice& gpu_;
  struct Image {
    VkImage image;
    VkDeviceMemory memory;
    xr::ImageDesc desc;
  };
  std::map<std::uint32_t, Image> images;
  std::uint32_t next = 10;
  VkCommandPool pool{};
  template <class F> F vk(const char *name) {
    auto &gpu = gpu_;
    return reinterpret_cast<F>(gpu.get_instance_proc_addr(gpu.instance, name));
  }
  void commands(const std::function<void(VkCommandBuffer)> &record) {
    auto &gpu = gpu_;
    if (!pool) {
      VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      ci.queueFamilyIndex = gpu.queue_family;
      REQUIRE(vk<PFN_vkCreateCommandPool>("vkCreateCommandPool")(gpu.device, &ci, nullptr, &pool) ==
              VK_SUCCESS);
    }
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
                                   VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer buffer{};
    REQUIRE(vk<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(gpu.device, &ai,
                                                                         &buffer) == VK_SUCCESS);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    REQUIRE(vk<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(buffer, &bi) == VK_SUCCESS);
    record(buffer);
    REQUIRE(vk<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(buffer) == VK_SUCCESS);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &buffer;
    REQUIRE(vk<PFN_vkQueueSubmit>("vkQueueSubmit")(gpu.queue, 1, &submit, VK_NULL_HANDLE) ==
            VK_SUCCESS);
    REQUIRE(vk<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(gpu.queue) == VK_SUCCESS);
    vk<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(gpu.device, pool, 1, &buffer);
  }
  void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout before, VkImageLayout after,
               std::uint32_t layers) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = before;
    b.newLayout = after;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.srcAccessMask = before == VK_IMAGE_LAYOUT_UNDEFINED
                          ? 0
                          : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                                         nullptr, 0, nullptr, 1, &b);
  }

public:
  explicit XrImageFixture(gfx::GpuDevice& gpu = gfx::gpu_device()) : gpu_(gpu) {}
  unsigned sampler_updates = 0;
  XrResult sampler_result = XR_SUCCESS;
  xr::SamplerOpenGLES last_sampler{};
  std::vector<std::uint32_t> sampler_textures;
  XrResult update_sampler(std::span<const std::uint32_t> names, const xr::ImageDesc&,
                          const xr::SamplerOpenGLES& sampler) override {
    CHECK_FALSE(host_queue_locked);
    ++sampler_updates;
    last_sampler = sampler;
    sampler_textures.assign(names.begin(), names.end());
    return sampler_result;
  }
  bool host_queue_locked = false;
  bool external_queue_active() const override { return true; }
  bool lock_external_queue() override {
    CHECK_FALSE(host_queue_locked);
    host_queue_locked = true;
    return true;
  }
  void unlock_external_queue() override {
    CHECK(host_queue_locked);
    host_queue_locked = false;
  }
  bool batch_enabled = false;
  unsigned batch_submits = 0;
  std::function<void()> before_batch_submit;
  bool begin_handoff_batch() override { return batch_enabled; }
  XrResult end_handoff_batch() override {
    if (before_batch_submit) before_batch_submit();
    ++batch_submits;
    return XR_SUCCESS;
  }
  std::function<void()> before_copy;
  std::function<void()> after_copy;
  std::uint32_t copies = 0, last_texture = 0;
  std::uint32_t last_source_mips = 0, last_destination_mips = 0;
  VkImage last_destination{};
  std::uint64_t last_fence = 0;
  std::vector<std::uint64_t> producer_syncs;
  bool last_bottom_up = false;
  std::vector<std::pair<VkImage, bool>> destinations;
  // A host-visible buffer of `size` bytes for transfers in either direction.
  std::pair<VkBuffer, VkDeviceMemory> host_buffer(VkDeviceSize size) {
    auto &gpu = gpu_;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer buffer{};
    REQUIRE(vk<PFN_vkCreateBuffer>("vkCreateBuffer")(gpu.device, &ci, nullptr, &buffer) ==
            VK_SUCCESS);
    VkMemoryRequirements requirements;
    vk<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(gpu.device, buffer,
                                                                           &requirements);
    VkPhysicalDeviceMemoryProperties properties;
    vk<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(
        gpu.physical_device, &properties);
    std::uint32_t type = 0;
    constexpr auto flags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while (type < properties.memoryTypeCount &&
           (!(requirements.memoryTypeBits & (1u << type)) ||
            (properties.memoryTypes[type].propertyFlags & flags) != flags))
      ++type;
    REQUIRE(type < properties.memoryTypeCount);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size,
                            type};
    VkDeviceMemory memory{};
    REQUIRE(vk<PFN_vkAllocateMemory>("vkAllocateMemory")(gpu.device, &ai, nullptr, &memory) ==
            VK_SUCCESS);
    REQUIRE(vk<PFN_vkBindBufferMemory>("vkBindBufferMemory")(gpu.device, buffer, memory, 0) ==
            VK_SUCCESS);
    return {buffer, memory};
  }
  void free_buffer(std::pair<VkBuffer, VkDeviceMemory> buffer) {
    vk<PFN_vkDestroyBuffer>("vkDestroyBuffer")(gpu_.device, buffer.first, nullptr);
    vk<PFN_vkFreeMemory>("vkFreeMemory")(gpu_.device, buffer.second, nullptr);
  }
  // Reads one texel of `image` (by default the last copy destination), which
  // is in the color attachment layout, as after a host copy.
  std::array<std::uint8_t, 4> read_pixel(std::int32_t x = 0, std::int32_t y = 0,
                                         std::uint32_t layer = 0, VkImage image = VK_NULL_HANDLE) {
    auto &gpu = gpu_;
    if (!image) image = last_destination;
    std::lock_guard queue(gpu.queue_lock);
    auto [buffer, memory] = host_buffer(4);
    commands([&](VkCommandBuffer cmd) {
      barrier(cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layer + 1);
      VkBufferImageCopy copy{};
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
      copy.imageOffset = {x,y,0};
      copy.imageExtent = {1, 1, 1};
      vk<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
          cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
      barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, layer + 1);
    });
    void *mapped{};
    REQUIRE(vk<PFN_vkMapMemory>("vkMapMemory")(gpu.device, memory, 0, 4, 0, &mapped) == VK_SUCCESS);
    std::array<std::uint8_t, 4> pixel{};
    std::memcpy(pixel.data(), mapped, 4);
    vk<PFN_vkUnmapMemory>("vkUnmapMemory")(gpu.device, memory);
    free_buffer({buffer, memory});
    return pixel;
  }
  // Writes every texel of a ring texture: texel(layer, x, y), y in GLES rows
  // (row 0 is the bottom and is stored first, as ANGLE stores it).
  void paint(std::uint32_t name,
             const std::function<std::array<std::uint8_t, 4>(std::uint32_t, std::uint32_t, std::uint32_t)> &texel) {
    auto &gpu = gpu_;
    auto &ring = images.at(name);
    const auto &d = ring.desc;
    const auto layers = d.layers * d.face_count;  // a cube's faces are layers too
    const VkDeviceSize size = VkDeviceSize(d.width) * d.height * layers * 4;
    std::lock_guard queue(gpu.queue_lock);
    auto [buffer, memory] = host_buffer(size);
    void *mapped{};
    REQUIRE(vk<PFN_vkMapMemory>("vkMapMemory")(gpu.device, memory, 0, size, 0, &mapped) == VK_SUCCESS);
    auto *out = static_cast<std::uint8_t *>(mapped);
    for (std::uint32_t layer = 0; layer < layers; ++layer)
      for (std::uint32_t y = 0; y < d.height; ++y)
        for (std::uint32_t x = 0; x < d.width; ++x) {
          const auto value = texel(layer, x, y);
          std::memcpy(out + ((std::size_t(layer) * d.height + y) * d.width + x) * 4, value.data(), 4);
        }
    vk<PFN_vkUnmapMemory>("vkUnmapMemory")(gpu.device, memory);
    commands([&](VkCommandBuffer cmd) {
      barrier(cmd, ring.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              layers);
      VkBufferImageCopy copy{};
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
      copy.imageExtent = {d.width, d.height, 1};
      vk<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(
          cmd, buffer, ring.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
      barrier(cmd, ring.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              layers);
    });
    free_buffer({buffer, memory});
  }
  std::size_t ring_textures() const { return images.size(); }
  // Optional provider staging (xr::XrImages::create_staging): one 2D image per
  // layer, filled by stage() from the ring texture's layers.
  bool staging = false;
  XrResult staging_result = XR_SUCCESS;
  unsigned stages = 0;
  std::map<const xr::StagedImage *, std::vector<Image>> staged_images;
  XrResult create_staging(const xr::ImageDesc &desc, std::span<const std::uint32_t> names,
                          std::span<xr::StagedImage> out) override {
    CHECK_FALSE(host_queue_locked);
    if (!staging) return XR_ERROR_FEATURE_UNSUPPORTED;
    if (XR_FAILED(staging_result)) return staging_result;
    REQUIRE(names.size() == out.size());
    for (auto &slot : out) {
      auto &list = staged_images[&slot];
      for (std::uint32_t layer = 0; layer < desc.layers; ++layer) {
        auto single = desc;
        single.layers = single.mip_count = single.face_count = 1;
        list.push_back(make_image(single, false));
        slot.layers.push_back(list.back().image);
      }
      slot.layouts.assign(desc.layers, VK_IMAGE_LAYOUT_UNDEFINED);
      slot.idle_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      slot.staged = false;
    }
    return XR_SUCCESS;
  }
  XrResult stage(std::uint32_t name, xr::StagedImage &slot, const xr::ImageDesc &desc) override {
    CHECK_FALSE(host_queue_locked);
    auto it = images.find(name);
    if (it == images.end() || !staged_images.contains(&slot) || slot.layers.size() != desc.layers)
      return XR_ERROR_HANDLE_INVALID;
    std::lock_guard queue(gpu_.queue_lock);
    commands([&](VkCommandBuffer cmd) {
      for (std::uint32_t layer = 0; layer < desc.layers; ++layer) {
        barrier(cmd, slot.layers[layer], slot.layouts[layer], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1);
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {desc.width, desc.height, 1};
        vk<PFN_vkCmdCopyImage>("vkCmdCopyImage")(cmd, it->second.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                 slot.layers[layer], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                                 &copy);
        barrier(cmd, slot.layers[layer], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1);
        slot.layouts[layer] = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      }
    });
    slot.staged = true;
    ++stages;
    return XR_SUCCESS;
  }
  void destroy_staging(std::span<xr::StagedImage> slots) override {
    CHECK_FALSE(host_queue_locked);
    for (auto &slot : slots) {
      auto it = staged_images.find(&slot);
      if (it == staged_images.end()) continue;
      for (auto &image : it->second) free_image(image);
      staged_images.erase(it);
      slot = {};
    }
  }
  ~XrImageFixture() override {
    for (auto &[slot, list] : staged_images)
      for (auto &image : list) free_image(image);
    std::vector<std::uint32_t> remaining;
    for (auto &[id, image] : images)
      remaining.push_back(id);
    destroy_ring(remaining);
    if (pool)
      vk<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(gpu_.device, pool, nullptr);
  }
  Image make_image(const xr::ImageDesc &desc, bool clear) {
    auto &gpu = gpu_;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = desc.vk_format;
    ci.extent = {desc.width, desc.height, 1};
    ci.mipLevels = desc.mip_count;
    ci.arrayLayers = desc.layers * desc.face_count;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (desc.face_count == 6)
      ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    Image image{{}, {}, desc};
    REQUIRE(vk<PFN_vkCreateImage>("vkCreateImage")(gpu.device, &ci, nullptr, &image.image) ==
            VK_SUCCESS);
    VkMemoryRequirements requirements;
    vk<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(gpu.device, image.image,
                                                                         &requirements);
    VkPhysicalDeviceMemoryProperties properties;
    vk<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(
        gpu.physical_device, &properties);
    std::uint32_t type = 0;
    while (!(requirements.memoryTypeBits & (1u << type)))
      ++type;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size,
                            type};
    REQUIRE(vk<PFN_vkAllocateMemory>("vkAllocateMemory")(gpu.device, &ai, nullptr,
                                                         &image.memory) == VK_SUCCESS);
    REQUIRE(vk<PFN_vkBindImageMemory>("vkBindImageMemory")(gpu.device, image.image, image.memory,
                                                           0) == VK_SUCCESS);
    if (clear) commands([&](VkCommandBuffer cmd) {
      barrier(cmd, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              ci.arrayLayers);
      VkClearColorValue color{{0.25f, 0.5f, 0.75f, 1}};
      VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, ci.arrayLayers};
      vk<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(
          cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
      barrier(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ci.arrayLayers);
    });
    return image;
  }
  void free_image(const Image &image) {
    vk<PFN_vkDestroyImage>("vkDestroyImage")(gpu_.device, image.image, nullptr);
    vk<PFN_vkFreeMemory>("vkFreeMemory")(gpu_.device, image.memory, nullptr);
  }
  XrResult create_ring(const xr::ImageDesc &desc, std::span<std::uint32_t> names) override {
    CHECK_FALSE(host_queue_locked);
    for (auto &name : names) {
      auto image = make_image(desc, true);
      name = next++;
      images[name] = image;
    }
    return XR_SUCCESS;
  }
  void destroy_ring(std::span<const std::uint32_t> names) override {
    CHECK_FALSE(host_queue_locked);
    for (auto name : names) {
      auto it = images.find(name);
      if (it == images.end())
        continue;
      free_image(it->second);
      images.erase(it);
    }
  }
  bool owns_texture(std::uint32_t name, const xr::ImageDesc &) const override {
    return images.contains(name);
  }
  XrResult wait_producer_sync(std::uint64_t fence) override {
    if (fence) producer_syncs.push_back(fence);
    return XR_SUCCESS;
  }
  XrResult copy_to_image(std::uint32_t name, VkImage destination, const xr::ImageDesc &desc,
                         std::uint64_t fence, bool bottom_up) override {
    CHECK_FALSE(host_queue_locked);
    auto it = images.find(name);
    if (it == images.end())
      return XR_ERROR_HANDLE_INVALID;
    commands([&](VkCommandBuffer cmd) {
      barrier(cmd, destination, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              desc.layers * desc.face_count);
      VkImageCopy copy{};
      copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, desc.layers * desc.face_count};
      copy.dstSubresource = copy.srcSubresource;
      copy.extent = {desc.width, desc.height, 1};
      vk<PFN_vkCmdCopyImage>("vkCmdCopyImage")(cmd, it->second.image,
                                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
      barrier(cmd, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, desc.layers * desc.face_count);
    });
    if (before_copy) before_copy();
    ++copies;
    last_texture = name;
    last_source_mips = desc.mip_count;
    last_destination_mips = desc.host_mip_count ? desc.host_mip_count : desc.mip_count;
    last_destination = destination;
    last_fence = fence;
    last_bottom_up = bottom_up;
    destinations.emplace_back(destination, bottom_up);
    if (after_copy) after_copy();
    return XR_SUCCESS;
  }
};
} // namespace venture::test
