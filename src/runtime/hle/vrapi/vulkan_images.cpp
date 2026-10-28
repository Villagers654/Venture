#include "common/environment.h"
#include "hle/vrapi/vulkan_images.h"
#include "common/log.h"
#include "core/guest.h"
#include "core/renderer_capabilities.h"
#include "gfx/vulkan_memory.h"
#include "hle/vrapi/state.h"
#include "hle/vulkan/internal.h"
#include "hle/vulkan/full_rate_image.h"
#include "hle/vulkan/timing.h"
#include "core/wait.h"
#include "platform/profile.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>

namespace venture::hle::vrapi {
namespace {
class VulkanImages final : public XrImages {
  struct Image {
    VkImage image{};
    VkDeviceMemory memory{};
    ImageDesc desc{};
    std::unique_ptr<vulkan::FullRateImage> full_rate;
  };
  abi::SystemVulkan system_;
  XrImages *previous_;
  std::map<std::uint32_t, Image> images_;
  std::map<VkImage, bool> destinations_;
  std::uint32_t next_ = 1;
  VkQueue queue_{};
  VkQueue guest_queue_{};
  std::optional<vulkan::QueueBinding> binding_;
  VkCommandPool pool_{};
  // Copies are queue-ordered after the guest's rendering and before the XR
  // runtime's reads of the destination; the CPU only waits to reuse a slot.
  struct Slot {
    VkCommandBuffer command{};
    VkFence fence{};
    bool pending = false;
    vulkan::XrCopySample sample{};  // VENTURE_VULKAN_GPU_TIMING, reported on reuse
  };
  static constexpr std::uint32_t slot_count = 8;
  std::array<Slot, slot_count> slots_{};
  std::uint32_t next_slot_ = 0;
  VkCommandBuffer command_{};  // the slot being recorded
  VkQueryPool queries_{};  // VENTURE_VULKAN_GPU_TIMING: two timestamps per slot
  double timestamp_period_ = 1;
  bool failed_ = false;
  VkDevice device() const { return g2h<VkDevice_T>(system_.device); }
  VkPhysicalDevice physical() const { return g2h<VkPhysicalDevice_T>(system_.physical); }
  template<class F> F vk(const char *name) const {
    return reinterpret_cast<F>(vulkan::raw_device_proc(device(), name));
  }
  template<class F> F physical_vk(const char *name) const {
    return reinterpret_cast<F>(vulkan::raw_physical_proc(physical(), name));
  }
  XrResult error(const char *operation, VkResult r) {
    log::error("vrapi-vulkan", "{} failed: {}", operation, std::int32_t(r));
    return r == VK_ERROR_OUT_OF_HOST_MEMORY || r == VK_ERROR_OUT_OF_DEVICE_MEMORY
        ? XR_ERROR_OUT_OF_MEMORY : XR_ERROR_RUNTIME_FAILURE;
  }
  void release(Image &image) {
    if (image.image) {
      vulkan::forget_image(image.image);
      vk<PFN_vkDestroyImage>("vkDestroyImage")(device(), image.image, nullptr);
    }
    if (image.memory) vk<PFN_vkFreeMemory>("vkFreeMemory")(device(), image.memory, nullptr);
    image = {};
  }
  void barrier(VkImage image, VkImageLayout before, VkImageLayout after,
               const ImageDesc &desc) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = before; b.newLayout = after;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.srcAccessMask = before == VK_IMAGE_LAYOUT_UNDEFINED ? 0
        : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, desc.mip_count, 0,
                          desc.layers * desc.face_count};
    vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
        command_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b);
  }
public:
  VulkanImages(abi::SystemVulkan system, XrImages *previous) : system_(system), previous_(previous) {}
  // Waits for the slot's last copy; reports its timing sample if enabled.
  VkResult retire(std::uint32_t index) {
    auto &slot = slots_[index];
    if (!slot.pending) return VK_SUCCESS;
    const auto start = queries_ ? monotonic_ns() : 0;
    auto r = vk<PFN_vkWaitForFences>("vkWaitForFences")(device(), 1, &slot.fence, VK_TRUE, 2'000'000'000ull);
    if (r == VK_TIMEOUT) {
      log::warn("vrapi-vulkan", "Eye copy slot {} not complete after 2 s (fence status {}); still waiting", index,
                int(vk<PFN_vkGetFenceStatus>("vkGetFenceStatus")(device(), slot.fence)));
      r = vk<PFN_vkWaitForFences>("vkWaitForFences")(
          device(), 1, &slot.fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max());
    }
    if (r != VK_SUCCESS) return r;
    slot.pending = false;
    if (queries_) {
      std::uint64_t ticks[2]{};
      slot.sample.wait_ns = monotonic_ns() - start;
      if (vk<PFN_vkGetQueryPoolResults>("vkGetQueryPoolResults")(device(), queries_, 2 * index, 2, sizeof(ticks),
              ticks, sizeof(ticks[0]), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        slot.sample.vk_begin_ns = std::uint64_t(double(ticks[0]) * timestamp_period_);
        slot.sample.vk_end_ns = std::uint64_t(double(ticks[1]) * timestamp_period_);
      }
      vulkan::timing_xr_copy(slot.sample);
    }
    return VK_SUCCESS;
  }
  bool uses(VkDevice device) const { return this->device() == device; }
  ~VulkanImages() override {
    {
      auto queue_lock = lock_queue();
      for (std::uint32_t i = 0; i < slot_count; ++i) retire(i);
    }
    for (auto &[id, image] : images_) release(image);
    for (auto &slot : slots_)
      if (slot.fence) vk<PFN_vkDestroyFence>("vkDestroyFence")(device(), slot.fence, nullptr);
    if (queries_) vk<PFN_vkDestroyQueryPool>("vkDestroyQueryPool")(device(), queries_, nullptr);
    if (pool_) vk<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device(), pool_, nullptr);
    install_xr_images(previous_);
  }
  bool empty() const { return images_.empty(); }
  XrResult prepare_submission() {
    return vulkan::flush_queue_submissions(guest_queue_) == VK_SUCCESS ? XR_SUCCESS : XR_ERROR_RUNTIME_FAILURE;
  }
  std::unique_lock<std::recursive_mutex> lock_queue() {
    return binding_ ? std::unique_lock(*binding_->submission_mutex)
                    : std::unique_lock<std::recursive_mutex>{};
  }
  XrResult bind(const abi::ModeVulkan &mode, XrGraphicsBindingVulkanKHR &out) {
    auto queue = g2h<VkQueue_T>(mode.queue);
    auto binding = vulkan::queue_binding(queue);
    if (!binding || h2g(binding->instance) != system_.instance ||
        h2g(binding->physical) != system_.physical || h2g(binding->device) != system_.device)
      return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    if (queue_ && queue_ != vulkan::native_queue(queue)) return XR_ERROR_FEATURE_UNSUPPORTED;
    std::uint32_t count{};
    auto query = physical_vk<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
    query(physical(), &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    query(physical(), &count, families.data());
    if (binding->family >= count || !(families[binding->family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
      return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    if (!pool_) {
      VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      pi.queueFamilyIndex = binding->family;
      auto r = vk<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device(), &pi, nullptr, &pool_);
      if (r != VK_SUCCESS) return error("CreateCommandPool", r);
      std::array<VkCommandBuffer, slot_count> commands{};
      VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
          pool_, VK_COMMAND_BUFFER_LEVEL_PRIMARY, slot_count};
      r = vk<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device(), &ai, commands.data());
      for (std::uint32_t i = 0; r == VK_SUCCESS && i < slot_count; ++i) {
        slots_[i].command = commands[i];
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        r = vk<PFN_vkCreateFence>("vkCreateFence")(device(), &fi, nullptr, &slots_[i].fence);
      }
      if (r != VK_SUCCESS) {
        for (auto &slot : slots_)
          if (slot.fence) vk<PFN_vkDestroyFence>("vkDestroyFence")(device(), slot.fence, nullptr);
        slots_ = {};
        vk<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device(), pool_, nullptr);
        pool_ = {}; return error("Create eye copy slots", r);
      }
      if (vulkan::timing_enabled() && families[binding->family].timestampValidBits == 64) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2 * slot_count;
        VkPhysicalDeviceProperties properties{};
        physical_vk<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(physical(), &properties);
        timestamp_period_ = properties.limits.timestampPeriod;
        if (vk<PFN_vkCreateQueryPool>("vkCreateQueryPool")(device(), &qi, nullptr, &queries_) != VK_SUCCESS)
          queries_ = {};
      }
    }
    guest_queue_ = queue;
    queue_ = vulkan::native_queue(queue); binding_ = binding;
    out = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR, nullptr, binding->instance,
           binding->physical, binding->device, binding->family, binding->index};
    return XR_SUCCESS;
  }
  VkImage image(std::uint32_t id) const {
    auto it = images_.find(id);
    return it == images_.end() ? VK_NULL_HANDLE : it->second.image;
  }
  XrResult full_rate_image(std::uint32_t id, VkImage& out, std::uint32_t& width, std::uint32_t& height) {
    auto it = images_.find(id);
    if (it == images_.end() || !binding_) return XR_ERROR_CALL_ORDER_INVALID;
    try {
      auto& source = it->second;
      if (!source.full_rate)
        source.full_rate = std::make_unique<vulkan::FullRateImage>(
            physical(), device(), binding_->family, binding_->index,
            source.desc.layers * source.desc.face_count, source.desc.width, source.desc.height);
      out = source.full_rate->image();
      width = source.full_rate->width();
      height = source.full_rate->height();
      return XR_SUCCESS;
    } catch (const std::exception& e) {
      log::error("vrapi-vulkan", "Full-resolution compatibility image failed: {}", e.what());
      return XR_ERROR_RUNTIME_FAILURE;
    }
  }
  XrResult create_ring(const ImageDesc &desc, std::span<std::uint32_t> names) override {
    if (desc.samples != 1 || desc.mip_count > 32) return XR_ERROR_FEATURE_UNSUPPORTED;
    VkFormatProperties format{};
    physical_vk<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(
        physical(), desc.vk_format, &format);
    if (!(format.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
      return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    VkPhysicalDeviceMemoryProperties memory{};
    physical_vk<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(
        physical(), &memory);
    std::fill(names.begin(), names.end(), 0);
    XrResult result = XR_SUCCESS;
    for (auto &name : names) {
      Image image{}; image.desc = desc;
      VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      ci.flags = desc.face_count == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
      ci.imageType = VK_IMAGE_TYPE_2D; ci.format = desc.vk_format;
      ci.extent = {desc.width, desc.height, 1}; ci.mipLevels = desc.mip_count;
      ci.arrayLayers = desc.layers * desc.face_count; ci.samples = VK_SAMPLE_COUNT_1_BIT;
      ci.tiling = VK_IMAGE_TILING_OPTIMAL;
      ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      auto r = vk<PFN_vkCreateImage>("vkCreateImage")(device(), &ci, nullptr, &image.image);
      if (r == VK_SUCCESS) {
        VkMemoryRequirements requirements{};
        vk<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(
            device(), image.image, &requirements);
        const auto type = gfx::gpu_only_memory_type(memory, requirements.memoryTypeBits);
        if (type == memory.memoryTypeCount) r = VK_ERROR_FEATURE_NOT_PRESENT;
        else {
          VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
                                 requirements.size, type};
          r = vk<PFN_vkAllocateMemory>("vkAllocateMemory")(device(), &ai, nullptr, &image.memory);
          if (r == VK_SUCCESS) r = vk<PFN_vkBindImageMemory>("vkBindImageMemory")(
              device(), image.image, image.memory, 0);
        }
      }
      if (r != VK_SUCCESS) {
        release(image); result = error("Create eye image", r); break;
      }
      // The guest's framebuffers are clamped to the eye image's extent.
      vulkan::note_image(image.image, ci);
      if (venture::environment::getenv("VENTURE_TRACE_VULKAN"))
        log::info("vrapi-vulkan", "Eye source image {:#x} format={} flags={:#x} {}x{} layers={} mips={}",
                  reinterpret_cast<std::uintptr_t>(image.image), int(ci.format), ci.flags,
                  ci.extent.width, ci.extent.height, ci.arrayLayers, ci.mipLevels);
      name = next_++;
      images_.emplace(name, std::move(image));
    }
    if (XR_FAILED(result)) { destroy_ring(names); std::fill(names.begin(), names.end(), 0); }
    return result;
  }
  void destroy_ring(std::span<const std::uint32_t> names) override {
    // Retire every copy before releasing any of its persistent source images.
    auto queue_lock = lock_queue();
    for (std::uint32_t i = 0; i < slot_count; ++i)
      if (auto r = retire(i); r != VK_SUCCESS) error("Retire eye images", r);
    for (auto name : names) if (auto it = images_.find(name); it != images_.end()) {
      release(it->second); images_.erase(it);
    }
  }
  bool owns_texture(std::uint32_t name, const ImageDesc &desc) const override {
    auto it = images_.find(name);
    return it != images_.end() && it->second.desc.vk_format == desc.vk_format &&
        it->second.desc.width == desc.width && it->second.desc.height == desc.height &&
        it->second.desc.layers == desc.layers && it->second.desc.face_count == desc.face_count;
  }
  XrResult release_images(std::span<const VkImage> images) override {
    // The runtime may destroy these images next: finish copies into them.
    auto queue_lock = lock_queue();
    for (std::uint32_t i = 0; i < slot_count; ++i)
      if (auto r = retire(i); r != VK_SUCCESS) return error("Retire eye copy", r);
    for (auto image : images) destinations_.erase(image);
    return XR_SUCCESS;
  }
  XrResult copy_to_image(std::uint32_t name, VkImage destination, const ImageDesc &desc,
                         std::uint64_t completion_fence, bool bottom_up) override {
    if (!queue_ || failed_) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    // Vulkan uses ovrModeParmsVulkan.SynchronizationQueue, not an EGLSyncKHR.
    // Do not interpret an unknown nonzero completion-fence packet as a host pointer.
    if (completion_fence) {
      static std::once_flag reported;
      std::call_once(reported, [] { log::warn("vrapi-vulkan", "Nonzero Vulkan completion fence packet unsupported"); });
      return XR_ERROR_FEATURE_UNSUPPORTED;
    }
    auto source = image(name);
    if (!source) return XR_ERROR_HANDLE_INVALID;
    if (desc.host_mip_count > desc.mip_count) return XR_ERROR_VALIDATION_FAILURE;
    auto host_desc = desc;
    if (desc.host_mip_count) host_desc.mip_count = desc.host_mip_count;
    if (bottom_up) {
      VkFormatProperties format{};
      physical_vk<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(
          physical(), desc.vk_format, &format);
      constexpr auto blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
      if ((format.optimalTilingFeatures & blit) != blit) {
        static std::once_flag reported;
        std::call_once(reported, [&] {
          log::warn("vrapi-vulkan", "Row-reversed eye copy unsupported for format {}",
                    int(desc.vk_format));
        });
        return XR_ERROR_FEATURE_UNSUPPORTED;
      }
    }
    const auto start = queries_ ? monotonic_ns() : 0;
    const auto* native = queries_ ? [] {
      thread_local const auto value = platform::profile_current_thread();
      return &value;
    }() : nullptr;
    std::uint64_t cpu_begin = 0;
    const bool cpu_timed = native && platform::profile_cpu_time(*native, cpu_begin);
    auto queue_lock = lock_queue();
    const auto index = next_slot_;
    auto r = retire(index);
    if (r != VK_SUCCESS) { failed_ = true; return error("Retire eye copy", r); }
    auto &slot = slots_[index];
    command_ = slot.command;
    r = vk<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(command_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (r == VK_SUCCESS) r = vk<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command_, &bi);
    if (r != VK_SUCCESS) return error("Begin eye copy", r);
    if (queries_) {
      vk<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool")(command_, queries_, 2 * index, 2);
      vk<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(command_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_,
                                                         2 * index);
    }
    // VrApi's compositor samples guest color images; restore their readable
    // layout after copying. XR requires color-attachment layout on release.
    barrier(source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, desc);
    barrier(destination, destinations_[destination] ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, host_desc);
    // A substituted host format (e.g. SRGB for a host without 8-bit UNORM) needs a converting blit, not a copy.
    if (bottom_up || (desc.host_format && desc.host_format != desc.vk_format)) {
      // Same-size nearest blits with reversed source rows are exact copies.
      std::vector<VkImageBlit> blits(host_desc.mip_count);
      for (std::uint32_t level = 0; level < host_desc.mip_count; ++level) {
        auto &blit = blits[level];
        const auto w = std::int32_t(std::max(1u, desc.width >> level));
        const auto h = std::int32_t(std::max(1u, desc.height >> level));
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0,
                                                     desc.layers * desc.face_count};
        blit.srcOffsets[0] = {0, bottom_up ? h : 0, 0};
        blit.srcOffsets[1] = {w, bottom_up ? 0 : h, 1};
        blit.dstOffsets[1] = {w, h, 1};
      }
      vk<PFN_vkCmdBlitImage>("vkCmdBlitImage")(command_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, std::uint32_t(blits.size()),
          blits.data(), VK_FILTER_NEAREST);
    } else {
      std::vector<VkImageCopy> copies(host_desc.mip_count);
      for (std::uint32_t level = 0; level < host_desc.mip_count; ++level) {
        auto &copy = copies[level];
        copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0,
                                                   desc.layers * desc.face_count};
        copy.extent = {std::max(1u, desc.width >> level), std::max(1u, desc.height >> level), 1};
      }
      vk<PFN_vkCmdCopyImage>("vkCmdCopyImage")(command_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copies.size(), copies.data());
    }
    barrier(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, desc);
    barrier(destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, host_desc);
    if (queries_)
      vk<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(command_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_,
                                                         2 * index + 1);
    r = vk<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command_);
    if (r == VK_SUCCESS) r = vk<PFN_vkResetFences>("vkResetFences")(device(), 1, &slot.fence);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1; submit.pCommandBuffers = &command_;
    if (r == VK_SUCCESS) r = vk<PFN_vkQueueSubmit>("vkQueueSubmit")(queue_, 1, &submit, slot.fence);
    if (r != VK_SUCCESS) { failed_ = true; return error("Submit eye copy", r); }
    slot.pending = true;
    next_slot_ = (index + 1) % slot_count;
    destinations_[destination] = true;
    if (queries_) {
      slot.sample = {"vrapi-vk", vulkan::timing_frame(), monotonic_ns() - start, 0, 0, 0, 0, 0, 0};
      std::uint64_t cpu_end = 0;
      if (cpu_timed && platform::profile_cpu_time(*native, cpu_end) && cpu_end >= cpu_begin)
        slot.sample.thread_cpu_ns = std::int64_t(cpu_end - cpu_begin);
    }
    return XR_SUCCESS;
  }
};
std::unique_ptr<VulkanImages> &system_images() {
  // Construct after the Vulkan/GPU registries. A game can exit without calling
  // DestroySystemVulkan; its images must retire before those registries' locks.
  static std::unique_ptr<VulkanImages> images;
  return images;
}
// A game can also destroy its VkDevice without DestroySystemVulkan: UE4 shuts
// its RHI down before ForceQuit (RE4 without entitlement). Retire the images
// while the device is valid; at exit they would be released on a freed device.
void release_device_images(VkDevice device) {
  std::lock_guard lock(state().mutex);
  auto &images = system_images();
  if (images && images->uses(device)) {
    log::info("vrapi-vulkan", "Guest destroyed its eye-image device; releasing the eye images");
    images.reset();
  }
}
}
XrResult bind_vulkan_queue(const abi::ModeVulkan &mode, XrGraphicsBindingVulkanKHR &binding) {
  auto &images = system_images();
  return images ? images->bind(mode, binding) : XR_ERROR_GRAPHICS_DEVICE_INVALID;
}
std::unique_lock<std::recursive_mutex> vulkan_submission_lock() {
  auto &images = system_images();
  return images ? images->lock_queue() : std::unique_lock<std::recursive_mutex>{};
}
XrResult prepare_vulkan_submission() {
  auto &images = system_images();
  return images ? images->prepare_submission() : XR_SUCCESS;
}
namespace guest {
std::int32_t vrapi_CreateSystemVulkan(const SystemVulkan *info) {
  std::lock_guard lock(state().mutex);
  auto &images = system_images();
  if (!state().initialized || state().init.graphics_api != vulkan_api) return not_initialized;
  if (!info || !info->instance || !info->physical || !info->device) return invalid;
  if (images || !state().rings.empty() || state().mobile) return invalid_operation;
  try {
    if (vulkan::device_physical(g2h<VkDevice_T>(info->device)) != g2h<VkPhysicalDevice_T>(info->physical) ||
        vulkan::device_instance(g2h<VkDevice_T>(info->device)) != g2h<VkInstance_T>(info->instance))
      return invalid;
    images = std::make_unique<VulkanImages>(*info, xr_images());
    install_xr_images(images.get());
    vulkan::add_device_release_hook(release_device_images);
    log::info("vrapi-vulkan", "Bound guest-owned Vulkan eye-image device");
    return success;
  } catch (const std::exception &) {
    failure("CreateSystemVulkan device", invalid);
    return invalid;
  }
}
void vrapi_DestroySystemVulkan() {
  std::lock_guard lock(state().mutex);
  auto &images = system_images();
  if (!images) return;
  if (state().mobile || !state().rings.empty()) { failure("DestroySystemVulkan", invalid_operation); return; }
  images.reset();
}
VkImage vrapi_GetTextureSwapChainBufferVulkan(TextureSwapChain *token, std::int32_t slot) {
  std::lock_guard lock(state().mutex);
  auto &images = system_images();
  auto it = state().rings.find(token);
  if (!images || it == state().rings.end() || slot < 0 || std::size_t(slot) >= it->second->textures.size()) {
    failure("GetTextureSwapChainBufferVulkan", invalid); return VK_NULL_HANDLE;
  }
  return images->image(it->second->textures[slot]);
}
std::int32_t vrapi_GetTextureSwapChainBufferFoveationVulkan(TextureSwapChain *token, std::int32_t slot,
                                                         VkImage *image, std::uint32_t *width,
                                                         std::uint32_t *height) {
  std::lock_guard lock(state().mutex);
  auto &images = system_images();
  // VrApi_Vulkan.h explicitly requires a null image and zero dimensions on
  // failure. OVRPlugin 1.51 reads these outputs even when the result is negative.
  if (image) *image = VK_NULL_HANDLE;
  if (width) *width = 0;
  if (height) *height = 0;
  auto it = state().rings.find(token);
  if (!image || !width || !height || !images || it == state().rings.end() || slot < 0 ||
      std::size_t(slot) >= it->second->textures.size()) return invalid;
  // The query is valid, but full-rate composition has no density resource.
  // Distinguish that empty result from an unsupported API: some clients treat
  // an error as fatal, while others consume the initialized outputs directly.
  return success;
}
}
}
