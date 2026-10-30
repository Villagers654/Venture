#include "xr/guest_images.h"
#include "common/log.h"
#include "core/wait.h"
#include "hle/vulkan/internal.h"
#include "hle/vulkan/timing.h"
#include "xr/queue.h"
#include <algorithm>
#include <mutex>
#include <set>
#include <stdexcept>
#include <vector>

namespace venture::xr {
namespace {
void checked(VkResult result, const char *operation) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(operation);
}
} // namespace
struct GuestImages::Impl {
  inline static std::mutex registry_mutex;
  inline static std::set<Impl *> registry;
  static void release_device(VkDevice device) {
    std::lock_guard lock(registry_mutex);
    for (auto *impl : registry)
      if (impl->binding.device == device)
        impl->release();
  }
  std::mutex mutex;
  XrGraphicsBindingVulkanKHR binding;
  PFN_vkGetInstanceProcAddr get;
  XrSwapchainCreateInfo info;
  VkQueue queue{};
  VkCommandPool pool{};
  VkImageAspectFlags aspects{};
  VkImageLayout layout{};
  VkQueryPool queries{};
  double timestamp_period = 1;
  struct Slot {
    VkImage image{};
    VkDeviceMemory memory{};
    VkCommandBuffer command{};
    VkFence fence{};
    bool pending = false;
    bool timed = false;
    vulkan::XrCopySample sample{};
  };
  std::vector<Slot> slots;
  std::vector<XrSwapchainImageVulkanKHR> exposed;
  template <class T> T fn(const char *name) const {
    return reinterpret_cast<T>(get(binding.instance, name));
  }
  XrResult wait(Slot &slot, XrDuration timeout) {
    if (!binding.device)
      return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    if (!slot.pending)
      return XR_SUCCESS;
    const auto start = slot.timed ? monotonic_ns() : 0;
    const auto result = fn<PFN_vkWaitForFences>("vkWaitForFences")(
        binding.device, 1, &slot.fence, VK_TRUE, std::uint64_t(timeout));
    if (result == VK_TIMEOUT)
      return XR_TIMEOUT_EXPIRED;
    if (result != VK_SUCCESS)
      return XR_ERROR_RUNTIME_FAILURE;
    slot.pending = false;
    if (slot.timed) {
      std::uint64_t ticks[2]{};
      const auto index = std::uint32_t(&slot - slots.data());
      slot.sample.wait_ns = monotonic_ns() - start;
      if (fn<PFN_vkGetQueryPoolResults>("vkGetQueryPoolResults")(
              binding.device, queries, 2 * index, 2, sizeof(ticks), ticks,
              sizeof(ticks[0]), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        slot.sample.vk_begin_ns =
            std::uint64_t(double(ticks[0]) * timestamp_period);
        slot.sample.vk_end_ns =
            std::uint64_t(double(ticks[1]) * timestamp_period);
      }
      vulkan::timing_xr_copy(slot.sample);
      slot.timed = false;
    }
    return XR_SUCCESS;
  }
  ~Impl() {
    std::lock_guard lock(registry_mutex);
    registry.erase(this);
    release();
  }
  void release() {
    std::lock_guard lock(mutex);
    if (!binding.device)
      return;
    for (auto &slot : slots)
      wait(slot, XR_INFINITE_DURATION);
    // DestroySwapchain also retires outstanding guest work on its binding
    // queue, before freeing the private images that replace host ownership.
    if (queue) {
      HostQueueLock lock(session().uses_shared_gpu);
      if (lock)
        fn<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(queue);
    }
    for (const auto &slot : slots) {
      if (slot.fence)
        fn<PFN_vkDestroyFence>("vkDestroyFence")(binding.device, slot.fence,
                                                 nullptr);
      if (slot.image)
        fn<PFN_vkDestroyImage>("vkDestroyImage")(binding.device, slot.image,
                                                 nullptr);
      if (slot.memory)
        fn<PFN_vkFreeMemory>("vkFreeMemory")(binding.device, slot.memory,
                                             nullptr);
    }
    if (pool)
      fn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(binding.device, pool,
                                                           nullptr);
    if (queries)
      fn<PFN_vkDestroyQueryPool>("vkDestroyQueryPool")(binding.device, queries,
                                                       nullptr);
    binding.device = VK_NULL_HANDLE;
  }
};
GuestImages::GuestImages(const XrGraphicsBindingVulkanKHR &binding,
                         PFN_vkGetInstanceProcAddr get,
                         const XrSwapchainCreateInfo &info)
    : impl_(std::make_unique<Impl>()) {
  auto &s = *impl_;
  s.binding = binding;
  s.get = get;
  s.info = info;
  s.info.next = nullptr;
  switch (VkFormat(info.format)) {
  case VK_FORMAT_D16_UNORM:
  case VK_FORMAT_X8_D24_UNORM_PACK32:
  case VK_FORMAT_D32_SFLOAT:
    s.aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
    break;
  case VK_FORMAT_D16_UNORM_S8_UINT:
  case VK_FORMAT_D24_UNORM_S8_UINT:
  case VK_FORMAT_D32_SFLOAT_S8_UINT:
    s.aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    break;
  case VK_FORMAT_S8_UINT:
    s.aspects = VK_IMAGE_ASPECT_STENCIL_BIT;
    break;
  default:
    s.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    break;
  }
  s.layout = s.aspects != VK_IMAGE_ASPECT_COLOR_BIT
                 ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                 : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  s.fn<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(
      binding.device, binding.queueFamilyIndex, binding.queueIndex, &s.queue);
  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool.queueFamilyIndex = binding.queueFamilyIndex;
  checked(s.fn<PFN_vkCreateCommandPool>("vkCreateCommandPool")(
              binding.device, &pool, nullptr, &s.pool),
          "Guest image command pool");
  const auto count =
      info.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT ? 1u : 3u;
  s.slots.resize(count);
  if (vulkan::timing_enabled()) {
    std::uint32_t n{};
    auto families = s.fn<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        "vkGetPhysicalDeviceQueueFamilyProperties");
    families(binding.physicalDevice, &n, nullptr);
    std::vector<VkQueueFamilyProperties> properties(n);
    families(binding.physicalDevice, &n, properties.data());
    if (binding.queueFamilyIndex < n &&
        properties[binding.queueFamilyIndex].timestampValidBits == 64) {
      VkPhysicalDeviceProperties device{};
      s.fn<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(
          binding.physicalDevice, &device);
      s.timestamp_period = device.limits.timestampPeriod;
      VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
      qi.queryCount = 2 * count;
      if (s.fn<PFN_vkCreateQueryPool>("vkCreateQueryPool")(
              binding.device, &qi, nullptr, &s.queries) != VK_SUCCESS)
        s.queries = VK_NULL_HANDLE;
    }
  }
  VkPhysicalDeviceMemoryProperties memory{};
  s.fn<PFN_vkGetPhysicalDeviceMemoryProperties>(
      "vkGetPhysicalDeviceMemoryProperties")(binding.physicalDevice, &memory);
  for (auto &slot : s.slots) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VkFormat(info.format);
    ci.extent = {info.width, info.height, 1};
    ci.mipLevels = info.mipCount;
    ci.arrayLayers = info.arraySize * info.faceCount;
    ci.samples = VkSampleCountFlagBits(info.sampleCount);
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // OpenXR promises the attachment layout after every successful wait,
    // even when the guest requested only transfer or sampled access.
    ci.usage |= s.aspects == VK_IMAGE_ASPECT_COLOR_BIT
                    ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                    : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)
      ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
      ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (info.usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT)
      ci.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (info.usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT)
      ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (info.usageFlags & XR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_KHR)
      ci.usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    if (info.usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT)
      ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (info.faceCount == 6)
      ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VkImageFormatProperties supported{};
    const auto capability = s.fn<PFN_vkGetPhysicalDeviceImageFormatProperties>(
        "vkGetPhysicalDeviceImageFormatProperties")(
        binding.physicalDevice, ci.format, ci.imageType, ci.tiling, ci.usage,
        ci.flags, &supported);
    if (capability == VK_ERROR_FORMAT_NOT_SUPPORTED)
      throw XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    if (capability != VK_SUCCESS)
      throw XR_ERROR_RUNTIME_FAILURE;
    if (ci.extent.width > supported.maxExtent.width ||
        ci.extent.height > supported.maxExtent.height ||
        ci.mipLevels > supported.maxMipLevels ||
        ci.arrayLayers > supported.maxArrayLayers ||
        !(supported.sampleCounts & ci.samples))
      throw XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    checked(s.fn<PFN_vkCreateImage>("vkCreateImage")(binding.device, &ci,
                                                     nullptr, &slot.image),
            "Guest swapchain image");
    VkMemoryRequirements requirements{};
    s.fn<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(
        binding.device, slot.image, &requirements);
    std::uint32_t type = 0;
    while (type < memory.memoryTypeCount &&
           (!(requirements.memoryTypeBits & (1u << type)) ||
            !(memory.memoryTypes[type].propertyFlags &
              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
      ++type;
    if (type == memory.memoryTypeCount)
      throw std::runtime_error("Guest image memory type");
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
                            requirements.size, type};
    checked(s.fn<PFN_vkAllocateMemory>("vkAllocateMemory")(
                binding.device, &ai, nullptr, &slot.memory),
            "Guest image memory");
    checked(s.fn<PFN_vkBindImageMemory>("vkBindImageMemory")(
                binding.device, slot.image, slot.memory, 0),
            "Guest image binding");
    VkCommandBufferAllocateInfo ca{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, s.pool,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    checked(s.fn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(
                binding.device, &ca, &slot.command),
            "Guest copy commands");
    VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    checked(s.fn<PFN_vkCreateFence>("vkCreateFence")(binding.device, &fc,
                                                     nullptr, &slot.fence),
            "Guest copy fence");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    checked(s.fn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(slot.command,
                                                                   &begin),
            "Guest initialization begin");
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.image = slot.image;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = s.layout;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    barrier.dstAccessMask =
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.subresourceRange = {s.aspects, 0, info.mipCount, 0, ci.arrayLayers};
    s.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
        slot.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
        &barrier);
    checked(s.fn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(slot.command),
            "Guest initialization end");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &slot.command;
    HostQueueLock queue(session().uses_shared_gpu);
    if (!queue)
      throw XR_ERROR_RUNTIME_FAILURE;
    checked(s.fn<PFN_vkQueueSubmit>("vkQueueSubmit")(s.queue, 1, &submit,
                                                     slot.fence),
            "Guest initialization submit");
    slot.pending = true;
    s.exposed.push_back(
        {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR, nullptr, slot.image});
  }
  vulkan::add_device_release_hook(Impl::release_device);
  std::lock_guard lock(Impl::registry_mutex);
  Impl::registry.insert(&s);
}
GuestImages::~GuestImages() = default;
std::span<const XrSwapchainImageVulkanKHR> GuestImages::images() const {
  return impl_->exposed;
}
XrResult GuestImages::wait(std::uint32_t index, XrDuration timeout) {
  std::lock_guard lock(impl_->mutex);
  if (index >= impl_->slots.size())
    return XR_ERROR_RUNTIME_FAILURE;
  return impl_->wait(impl_->slots[index], timeout);
}
XrResult GuestImages::drain() {
  std::lock_guard lock(impl_->mutex);
  for (auto &slot : impl_->slots) {
    auto result = impl_->wait(slot, XR_INFINITE_DURATION);
    if (result != XR_SUCCESS)
      return result;
  }
  return XR_SUCCESS;
}
XrResult GuestImages::copy(std::uint32_t index, VkImage target,
                           VkFormat format, VkExtent2D extent, bool flip_rows, std::int32_t source_layer) {
  auto &s = *impl_;
  const auto layer_count = s.info.arraySize * s.info.faceCount;
  if (source_layer < -1 || (source_layer >= 0 && std::uint32_t(source_layer) >= layer_count))
    return XR_ERROR_INDEX_OUT_OF_RANGE;
  const auto copied_layers = source_layer < 0 ? layer_count : 1u;
  const auto first_layer = source_layer < 0 ? 0u : std::uint32_t(source_layer);
  const bool scaled = extent.width && extent.height &&
                      (extent.width != s.info.width || extent.height != s.info.height);
  std::lock_guard slot_lock(s.mutex);
  if (index >= s.slots.size())
    return XR_ERROR_RUNTIME_FAILURE;
  auto result = s.wait(s.slots[index], XR_INFINITE_DURATION);
  if (result != XR_SUCCESS)
    return result;
  auto &slot = s.slots[index];
  const auto start = s.queries ? monotonic_ns() : 0;
  try {
    checked(
        s.fn<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(slot.command, 0),
        "Guest copy reset");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    checked(s.fn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(slot.command,
                                                                   &bi),
            "Guest copy begin");
    if (s.queries) {
      s.fn<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool")(
          slot.command, s.queries, 2 * index, 2);
      s.fn<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(
          slot.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s.queries,
          2 * index);
    }
    // The host chain has a single level (see prepare_swapchains); copy the base.
    constexpr std::uint32_t host_mips = 1;
    auto barrier = [&](VkImage image, VkImageLayout before,
                       VkImageLayout after, std::uint32_t levels, std::uint32_t count) {
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      b.image = image;
      b.oldLayout = before;
      b.newLayout = after;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.srcAccessMask =
          before == VK_IMAGE_LAYOUT_UNDEFINED
              ? 0
              : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      b.subresourceRange = {s.aspects, 0, levels, 0, count};
      s.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
          slot.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    barrier(slot.image, s.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.info.mipCount, layer_count);
    barrier(target, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, host_mips, copied_layers);
    if (format == VkFormat(s.info.format) && !scaled && !flip_rows) {
      for (const auto aspect :
           {VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
            VK_IMAGE_ASPECT_STENCIL_BIT}) {
        if (!(s.aspects & aspect))
          continue;
        for (std::uint32_t mip = 0; mip < host_mips; ++mip) {
          VkImageCopy copy{};
          copy.srcSubresource = {VkImageAspectFlags(aspect), mip, first_layer, copied_layers};
          copy.dstSubresource = {VkImageAspectFlags(aspect), mip, 0, copied_layers};
          copy.extent = {std::max(1u, s.info.width >> mip),
                         std::max(1u, s.info.height >> mip), 1};
          s.fn<PFN_vkCmdCopyImage>("vkCmdCopyImage")(
              slot.command, slot.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        }
      }
    } else {
      // GLES host format fallback: blit converts component order/encoding by
      // value, scales to a host chain smaller than the guest chain and flips.
      // Multisample images cannot be blitted; depth converts no format.
      if (s.info.sampleCount != 1 ||
          (s.aspects != VK_IMAGE_ASPECT_COLOR_BIT && format != VkFormat(s.info.format)))
        return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
      VkFormatProperties source_properties{}, target_properties{};
      const auto properties = s.fn<PFN_vkGetPhysicalDeviceFormatProperties>(
          "vkGetPhysicalDeviceFormatProperties");
      properties(s.binding.physicalDevice, VkFormat(s.info.format),
                 &source_properties);
      properties(s.binding.physicalDevice, format, &target_properties);
      if (!(source_properties.optimalTilingFeatures &
            VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
          !(target_properties.optimalTilingFeatures &
            VK_FORMAT_FEATURE_BLIT_DST_BIT))
        return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
      // Downscaling filters linearly where the format allows it; a 1:1
      // conversion stays exact.
      const auto filter =
          scaled && s.aspects == VK_IMAGE_ASPECT_COLOR_BIT &&
                  (source_properties.optimalTilingFeatures &
                   VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
              ? VK_FILTER_LINEAR
              : VK_FILTER_NEAREST;
      for (std::uint32_t mip = 0; mip < host_mips; ++mip) {
        VkImageBlit blit{};
        blit.srcSubresource = {s.aspects, mip, first_layer, copied_layers};
        blit.dstSubresource = {s.aspects, mip, 0, copied_layers};
        blit.srcOffsets[1] = {int(std::max(1u, s.info.width >> mip)),
                              int(std::max(1u, s.info.height >> mip)), 1};
        blit.dstOffsets[1] =
            scaled ? VkOffset3D{int(std::max(1u, extent.width >> mip)),
                                int(std::max(1u, extent.height >> mip)), 1}
                   : blit.srcOffsets[1];
        if (flip_rows)
          std::swap(blit.srcOffsets[0].y, blit.srcOffsets[1].y);
        s.fn<PFN_vkCmdBlitImage>("vkCmdBlitImage")(
            slot.command, slot.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);
      }
    }
    barrier(slot.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.layout, s.info.mipCount, layer_count);
    barrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, s.layout, host_mips, copied_layers);
    if (s.queries)
      s.fn<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(
          slot.command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.queries,
          2 * index + 1);
    checked(s.fn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(slot.command),
            "Guest copy end");
    checked(s.fn<PFN_vkResetFences>("vkResetFences")(s.binding.device, 1,
                                                     &slot.fence),
            "Guest copy fence reset");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &slot.command;
    HostQueueLock lock(session().uses_shared_gpu);
    if (!lock)
      return XR_ERROR_RUNTIME_FAILURE;
    checked(s.fn<PFN_vkQueueSubmit>("vkQueueSubmit")(s.queue, 1, &submit,
                                                     slot.fence),
            "Guest copy submit");
    slot.pending = true;
    if (s.queries) {
      slot.timed = true;
      slot.sample = {"openxr-guest-handoff",
                     vulkan::timing_frame(),
                     monotonic_ns() - start,
                     0,
                     0,
                     0,
                     0,
                     0,
                     0};
    }
    return XR_SUCCESS;
  } catch (const std::exception &e) {
    log::error("xr", "Private swapchain copy failed: {}", e.what());
    return XR_ERROR_RUNTIME_FAILURE;
  }
}
} // namespace venture::xr
