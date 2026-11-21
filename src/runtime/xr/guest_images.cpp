#include "xr/guest_images.h"
#include "common/log.h"
#include "core/wait.h"
#include "hle/vulkan/internal.h"
#include "hle/vulkan/timing.h"
#include "xr/queue.h"
#include <algorithm>
#include <cstring>
#include <format>
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
  bool staged_mode = false;
  bool sampled = false;
  // External (zero-copy) slots: the images are a host chain's; slot_owned
  // says which are acquired from the host now (always 1 for private slots).
  bool external = false;
  std::vector<std::uint8_t> slot_owned;
  std::vector<StagedImage> staged;
  std::uint64_t bytes = 0;
  template <class T> T fn(const char *name) const {
    return reinterpret_cast<T>(get(binding.instance, name));
  }
  void begin(const XrGraphicsBindingVulkanKHR &, PFN_vkGetInstanceProcAddr, const XrSwapchainCreateInfo &,
             bool staged, std::uint32_t count);
  void make_slot_sync(Slot &slot) {
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
                                   VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    checked(fn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(binding.device, &ca, &slot.command),
            "Guest copy commands");
    VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    checked(fn<PFN_vkCreateFence>("vkCreateFence")(binding.device, &fc, nullptr, &slot.fence), "Guest copy fence");
  }
  // A slot Venture may read or write now. Caller holds mutex. Logged once per object when refused.
  bool usable(std::uint32_t index) {
    if (index >= slots.size()) return false;
    if (slot_owned[index]) return true;
    if (!refused_logged) {
      refused_logged = true;
      log::warn("xr", "A zero-copy swapchain image was needed after it went back to the host runtime; "
                "it is not read");
    }
    return false;
  }
  bool refused_logged = false;
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
      if (slot.image && !external)
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
void GuestImages::Impl::begin(const XrGraphicsBindingVulkanKHR &binding_, PFN_vkGetInstanceProcAddr get_,
                              const XrSwapchainCreateInfo &info_, bool staged, std::uint32_t count) {
  auto &s = *this;
  const auto &binding = binding_;
  const auto &info = info_;
  s.binding = binding;
  s.get = get_;
  s.info = info;
  s.info.next = nullptr;
  s.staged_mode = staged;
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
  s.slots.resize(count);
  if (staged) s.staged.resize(count);
  s.slot_owned.assign(count, 1);
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
}
GuestImages::GuestImages(const XrGraphicsBindingVulkanKHR &binding,
                         PFN_vkGetInstanceProcAddr get,
                         const XrSwapchainCreateInfo &info, bool staged)
    : impl_(std::make_unique<Impl>()) {
  auto &s = *impl_;
  s.begin(binding, get, info, staged, info.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT ? 1u : 3u);
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
    const auto properties = s.fn<PFN_vkGetPhysicalDeviceImageFormatProperties>(
        "vkGetPhysicalDeviceImageFormatProperties");
    // A color slot is also sampled where the format allows it: a color space
    // conversion then draws from it straight into the host image
    // (ColorImages::record_from) instead of reading the host image back.
    if (s.aspects == VK_IMAGE_ASPECT_COLOR_BIT && !staged && !(ci.usage & VK_IMAGE_USAGE_SAMPLED_BIT) &&
        properties(binding.physicalDevice, ci.format, ci.imageType, ci.tiling,
                   ci.usage | VK_IMAGE_USAGE_SAMPLED_BIT, ci.flags, &supported) == VK_SUCCESS)
      ci.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    const auto capability = properties(
        binding.physicalDevice, ci.format, ci.imageType, ci.tiling, ci.usage,
        ci.flags, &supported);
    s.sampled = !staged && (ci.usage & VK_IMAGE_USAGE_SAMPLED_BIT);
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
    s.make_slot_sync(slot);
    if (staged) {
      // The provider's staging holds the released contents; nothing to
      // allocate or initialize here.
      s.exposed.push_back({XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR, nullptr, VK_NULL_HANDLE});
      continue;
    }
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
    s.bytes += requirements.size;
    checked(s.fn<PFN_vkBindImageMemory>("vkBindImageMemory")(
                binding.device, slot.image, slot.memory, 0),
            "Guest image binding");
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
GuestImages::GuestImages(const XrGraphicsBindingVulkanKHR &binding, PFN_vkGetInstanceProcAddr get,
                         const XrSwapchainCreateInfo &info, std::span<const VkImage> external, bool sampled)
    : impl_(std::make_unique<Impl>()) {
  auto &s = *impl_;
  s.external = true;
  s.begin(binding, get, info, false, std::uint32_t(external.size()));
  s.sampled = sampled && s.aspects == VK_IMAGE_ASPECT_COLOR_BIT;
  s.slot_owned.assign(external.size(), 0);
  for (std::size_t i = 0; i < external.size(); ++i) {
    s.make_slot_sync(s.slots[i]);
    s.slots[i].image = external[i];
    s.exposed.push_back({XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR, nullptr, external[i]});
  }
  vulkan::add_device_release_hook(Impl::release_device);
  std::lock_guard lock(Impl::registry_mutex);
  Impl::registry.insert(&s);
}
GuestImages::~GuestImages() = default;
bool GuestImages::external() const { return impl_->external; }
void GuestImages::set_owned(std::uint32_t index, bool owned) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->external && index < impl_->slot_owned.size()) impl_->slot_owned[index] = owned;
}
bool GuestImages::refused_unowned() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->refused_logged;
}
bool GuestImages::owned(std::uint32_t index) const {
  std::lock_guard lock(impl_->mutex);
  return index < impl_->slot_owned.size() && impl_->slot_owned[index];
}
XrResult GuestImages::initialize(std::uint32_t index) {
  auto &s = *impl_;
  return submit(index, [&](VkCommandBuffer command, VkImage image, VkImageLayout layout) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.image = image;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = layout;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.subresourceRange = {s.aspects, 0, s.info.mipCount, 0, s.info.arraySize * s.info.faceCount};
    s.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                                                           nullptr, 1, &barrier);
    return XR_SUCCESS;
  });
}
std::span<const XrSwapchainImageVulkanKHR> GuestImages::images() const {
  return impl_->exposed;
}
std::span<StagedImage> GuestImages::staged() { return impl_->staged; }
std::uint64_t GuestImages::private_bytes() const { return impl_->bytes; }
XrResult GuestImages::wait(std::uint32_t index, XrDuration timeout) {
  std::lock_guard lock(impl_->mutex);
  if (index >= impl_->slots.size())
    return XR_ERROR_RUNTIME_FAILURE;
  return impl_->wait(impl_->slots[index], timeout);
}
bool GuestImages::sampled() const { return impl_->sampled; }
XrResult GuestImages::submit(std::uint32_t index, const Commands &commands) {
  auto &s = *impl_;
  std::lock_guard slot_lock(s.mutex);
  if (index >= s.slots.size() || !s.slots[index].image)
    return XR_ERROR_RUNTIME_FAILURE;
  if (!s.usable(index)) return XR_ERROR_CALL_ORDER_INVALID;
  auto result = s.wait(s.slots[index], XR_INFINITE_DURATION);
  if (result != XR_SUCCESS)
    return result;
  auto &slot = s.slots[index];
  const auto start = s.queries ? monotonic_ns() : 0;
  try {
    checked(s.fn<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(slot.command, 0), "Guest pass reset");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    checked(s.fn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(slot.command, &bi), "Guest pass begin");
    if (s.queries) {
      s.fn<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool")(slot.command, s.queries, 2 * index, 2);
      s.fn<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(slot.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                                           s.queries, 2 * index);
    }
    result = commands(slot.command, slot.image, s.layout);
    if (s.queries)
      s.fn<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(slot.command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                                           s.queries, 2 * index + 1);
    // Ended either way, so the next reset starts from the executable state.
    checked(s.fn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(slot.command), "Guest pass end");
    if (result != XR_SUCCESS)
      return result;
    checked(s.fn<PFN_vkResetFences>("vkResetFences")(s.binding.device, 1, &slot.fence), "Guest pass fence reset");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &slot.command;
    HostQueueLock lock(session().uses_shared_gpu);
    if (!lock)
      return XR_ERROR_RUNTIME_FAILURE;
    checked(s.fn<PFN_vkQueueSubmit>("vkQueueSubmit")(s.queue, 1, &submit, slot.fence), "Guest pass submit");
    slot.pending = true;
    if (s.queries) {
      slot.timed = true;
      slot.sample = {"openxr-guest-handoff", vulkan::timing_frame(), monotonic_ns() - start, 0, 0, 0, 0, 0, 0};
    }
    return XR_SUCCESS;
  } catch (const std::exception &e) {
    log::error("xr", "Guest image pass failed: {}", e.what());
    return XR_ERROR_RUNTIME_FAILURE;
  }
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
  if (!s.usable(index)) return XR_ERROR_CALL_ORDER_INVALID;
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
    const auto blittable = [&] {
      VkFormatProperties source_properties{}, target_properties{};
      const auto properties = s.fn<PFN_vkGetPhysicalDeviceFormatProperties>(
          "vkGetPhysicalDeviceFormatProperties");
      properties(s.binding.physicalDevice, VkFormat(s.info.format), &source_properties);
      properties(s.binding.physicalDevice, format, &target_properties);
      return (source_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
             (target_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
    };
    if (s.staged_mode) {
      // Provider staging: one 2D image per layer, GLES rows (row 0 at the
      // bottom). One blit per layer turns them top-down, scales and converts.
      auto &staged = s.staged[index];
      if (!staged.staged || staged.layers.size() != layer_count ||
          staged.layouts.size() != layer_count || s.info.sampleCount != 1 ||
          s.aspects != VK_IMAGE_ASPECT_COLOR_BIT)
        return XR_ERROR_RUNTIME_FAILURE;
      if (!blittable())
        return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
      VkFormatProperties source_properties{};
      s.fn<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(
          s.binding.physicalDevice, VkFormat(s.info.format), &source_properties);
      const auto filter = scaled && (source_properties.optimalTilingFeatures &
                                     VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
                              ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
      barrier(target, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, host_mips, copied_layers);
      for (std::uint32_t layer = 0; layer < copied_layers; ++layer) {
        const auto source = staged.layers[first_layer + layer];
        barrier(source, staged.layouts[first_layer + layer],
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1, 1);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
        blit.srcOffsets[1] = {int(s.info.width), int(s.info.height), 1};
        blit.dstOffsets[1] = scaled ? VkOffset3D{int(extent.width), int(extent.height), 1}
                                    : blit.srcOffsets[1];
        if (!flip_rows)
          std::swap(blit.srcOffsets[0].y, blit.srcOffsets[1].y);
        s.fn<PFN_vkCmdBlitImage>("vkCmdBlitImage")(
            slot.command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);
        if (staged.idle_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
          barrier(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staged.idle_layout, 1, 1);
      }
      barrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, s.layout, host_mips, copied_layers);
    } else {
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
          // One region per slice: the same copy as one multi-layer region,
          // without relying on a driver's multi-layer path into a runtime's
          // (often externally allocated) image array.
          std::vector<VkImageCopy> regions(copied_layers);
          for (std::uint32_t layer = 0; layer < copied_layers; ++layer) {
            auto &copy = regions[layer];
            copy.srcSubresource = {VkImageAspectFlags(aspect), mip, first_layer + layer, 1};
            copy.dstSubresource = {VkImageAspectFlags(aspect), mip, layer, 1};
            copy.extent = {std::max(1u, s.info.width >> mip),
                           std::max(1u, s.info.height >> mip), 1};
          }
          s.fn<PFN_vkCmdCopyImage>("vkCmdCopyImage")(
              slot.command, slot.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              std::uint32_t(regions.size()), regions.data());
        }
      }
    } else {
      // GLES host format fallback: blit converts component order/encoding by
      // value, scales to a host chain smaller than the guest chain and flips.
      // Multisample images cannot be blitted; depth converts no format.
      if (s.info.sampleCount != 1 ||
          (s.aspects != VK_IMAGE_ASPECT_COLOR_BIT && format != VkFormat(s.info.format)))
        return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
      if (!blittable())
        return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
      VkFormatProperties source_properties{};
      s.fn<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(
          s.binding.physicalDevice, VkFormat(s.info.format), &source_properties);
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
    }
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
    if (s.staged_mode)
      for (std::uint32_t layer = 0; layer < copied_layers; ++layer)
        s.staged[index].layouts[first_layer + layer] = s.staged[index].idle_layout;
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
namespace {
// Bytes per texel for the formats eye chains use; 0 when the format is not sampled.
std::uint32_t sampled_texel_bytes(VkFormat format) {
  switch (format) {
  case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
  case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
  case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    return 4;
  case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
  case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
  default: return 0;
  }
}
// A texel whose color (alpha left out) is not black.
bool lit_texel(VkFormat format, const unsigned char *texel) {
  const auto any = [&](std::size_t n) {
    return std::any_of(texel, texel + n, [](unsigned char b) { return b != 0; });
  };
  switch (format) {
  case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
  case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    return any(3);
  case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
    std::uint32_t v;
    std::memcpy(&v, texel, sizeof(v));
    return (v & 0x3fffffffu) != 0;
  }
  case VK_FORMAT_R16G16B16A16_SFLOAT: return any(6);
  case VK_FORMAT_R32G32B32A32_SFLOAT: return any(12);
  default: return any(4);
  }
}
} // namespace
std::string GuestImages::sample_layers(std::uint32_t index, VkImage target, VkFormat target_format,
                                       VkExtent2D target_extent, std::uint32_t target_layers) {
  auto &s = *impl_;
  constexpr std::uint32_t grid = 8, samples = grid * grid;
  if (s.aspects != VK_IMAGE_ASPECT_COLOR_BIT || s.info.sampleCount != 1)
    return "not sampled (depth or multisample chain)";
  const auto guest_format = VkFormat(s.info.format);
  const auto guest_bytes = sampled_texel_bytes(guest_format), target_bytes = sampled_texel_bytes(target_format);
  if (!guest_bytes || !target_bytes || !target_layers || !target_extent.width || !target_extent.height)
    return std::format("not sampled (format {} -> {})", int(guest_format), int(target_format));
  std::lock_guard slot_lock(s.mutex);
  if (index >= s.slots.size() || !s.binding.device) return "not sampled (no slot)";
  if (!s.usable(index)) return "not sampled (the image is the host runtime's)";
  auto &slot = s.slots[index];
  const bool guest = !s.staged_mode && slot.image;
  const auto guest_layers = guest ? s.info.arraySize * s.info.faceCount : 0u;
  const VkDeviceSize guest_size = VkDeviceSize(guest_layers) * samples * guest_bytes;
  const VkDeviceSize size = guest_size + VkDeviceSize(target_layers) * samples * target_bytes;
  const auto device = s.binding.device;
  VkBuffer buffer{};
  VkDeviceMemory memory{};
  VkCommandBuffer command{};
  VkFence fence{};
  bool in_flight = false;
  std::string summary;
  try {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    checked(s.fn<PFN_vkCreateBuffer>("vkCreateBuffer")(device, &bi, nullptr, &buffer), "Layer check buffer");
    VkMemoryRequirements requirements{};
    s.fn<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device, buffer, &requirements);
    VkPhysicalDeviceMemoryProperties properties{};
    s.fn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(
        s.binding.physicalDevice, &properties);
    constexpr VkMemoryPropertyFlags wanted =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    std::uint32_t type = 0;
    while (type < properties.memoryTypeCount &&
           (!(requirements.memoryTypeBits & (1u << type)) ||
            (properties.memoryTypes[type].propertyFlags & wanted) != wanted))
      ++type;
    if (type == properties.memoryTypeCount) throw std::runtime_error("Layer check memory type");
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, type};
    checked(s.fn<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &ai, nullptr, &memory), "Layer check memory");
    checked(s.fn<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device, buffer, memory, 0), "Layer check binding");
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, s.pool,
                                   VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    checked(s.fn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device, &ca, &command),
            "Layer check commands");
    VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    checked(s.fn<PFN_vkCreateFence>("vkCreateFence")(device, &fc, nullptr, &fence), "Layer check fence");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checked(s.fn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command, &begin), "Layer check begin");
    auto barrier = [&](VkImage image, VkImageLayout before, VkImageLayout after, std::uint32_t levels,
                       std::uint32_t layers) {
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      b.image = image;
      b.oldLayout = before;
      b.newLayout = after;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.srcAccessMask = b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
      s.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
          command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
          nullptr, 1, &b);
    };
    // An 8x8 grid of single texels per slice, at the centres of an 8x8 tiling.
    auto read = [&](VkImage image, VkExtent2D extent, std::uint32_t layers, std::uint32_t bytes,
                    VkDeviceSize offset) {
      std::vector<VkBufferImageCopy> regions;
      regions.reserve(std::size_t(layers) * samples);
      for (std::uint32_t layer = 0; layer < layers; ++layer)
        for (std::uint32_t y = 0; y < grid; ++y)
          for (std::uint32_t x = 0; x < grid; ++x) {
            VkBufferImageCopy r{};
            r.bufferOffset = offset + VkDeviceSize(regions.size()) * bytes;
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
            r.imageOffset = {std::int32_t((2 * x + 1) * extent.width / (2 * grid)),
                             std::int32_t((2 * y + 1) * extent.height / (2 * grid)), 0};
            r.imageExtent = {1, 1, 1};
            regions.push_back(r);
          }
      s.fn<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
          command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, std::uint32_t(regions.size()),
          regions.data());
    };
    if (guest) {
      barrier(slot.image, s.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.info.mipCount, guest_layers);
      read(slot.image, {s.info.width, s.info.height}, guest_layers, guest_bytes, 0);
      barrier(slot.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.layout, s.info.mipCount, guest_layers);
    }
    // The copy (and any color conversion after it) leaves the host image in
    // the attachment layout, which is s.layout for color.
    barrier(target, s.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1, target_layers);
    read(target, target_extent, target_layers, target_bytes, guest_size);
    barrier(target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.layout, 1, target_layers);
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = buffer;
    host.size = VK_WHOLE_SIZE;
    s.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                           VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host,
                                                           0, nullptr);
    checked(s.fn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command), "Layer check end");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    {
      HostQueueLock lock(session().uses_shared_gpu);
      if (!lock) throw std::runtime_error("Layer check queue");
      checked(s.fn<PFN_vkQueueSubmit>("vkQueueSubmit")(s.queue, 1, &submit, fence), "Layer check submit");
    }
    in_flight = true;
    const auto waited =
        s.fn<PFN_vkWaitForFences>("vkWaitForFences")(device, 1, &fence, VK_TRUE, 2'000'000'000ull);
    if (waited != VK_SUCCESS) throw std::runtime_error("Layer check wait");
    in_flight = false;
    void *mapped = nullptr;
    checked(s.fn<PFN_vkMapMemory>("vkMapMemory")(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped), "Layer check map");
    const auto *bytes = static_cast<const unsigned char *>(mapped);
    auto describe = [&](const char *what, std::uint32_t layers, std::uint32_t texel, VkFormat format,
                        VkDeviceSize offset) {
      for (std::uint32_t layer = 0; layer < layers; ++layer) {
        std::uint32_t lit = 0;
        const auto *first = bytes + offset + VkDeviceSize(layer) * samples * texel;
        for (std::uint32_t i = 0; i < samples; ++i) lit += lit_texel(format, first + std::size_t(i) * texel);
        // The texel nearest the centre, its first four bytes as stored.
        const auto *centre = first + std::size_t(grid / 2 * grid + grid / 2) * texel;
        summary += std::format("{}{} slice {} {}/{} lit centre {:02x}{:02x}{:02x}{:02x}",
                               summary.empty() ? "" : "; ", what, layer, lit, samples, centre[0], centre[1],
                               centre[2], centre[3]);
      }
    };
    if (guest) describe("guest", guest_layers, guest_bytes, guest_format, 0);
    describe("host", target_layers, target_bytes, target_format, guest_size);
    s.fn<PFN_vkUnmapMemory>("vkUnmapMemory")(device, memory);
  } catch (const std::exception &e) {
    summary = std::format("check failed: {}", e.what());
  }
  // A submission that never finished keeps its objects: freeing them under the GPU is worse.
  if (!in_flight) {
    if (fence) s.fn<PFN_vkDestroyFence>("vkDestroyFence")(device, fence, nullptr);
    if (command) s.fn<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(device, s.pool, 1, &command);
    if (buffer) s.fn<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device, buffer, nullptr);
    if (memory) s.fn<PFN_vkFreeMemory>("vkFreeMemory")(device, memory, nullptr);
  }
  return summary;
}
} // namespace venture::xr
