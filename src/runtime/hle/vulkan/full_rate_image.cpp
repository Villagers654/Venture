#include "full_rate_image.h"
#include "internal.h"
#include "common/log.h"
#include "gfx/queue_sync.h"
#include "gfx/vulkan_memory.h"
#include <stdexcept>
#include <algorithm>
#include <map>
#include <mutex>

namespace venture::vulkan {
namespace {
std::mutex images_mutex;
std::map<VkDevice, std::map<VkImage, VkImageLayout>> image_layouts;
void check(VkResult result, const char* operation) {
  if (result != VK_SUCCESS) throw std::runtime_error(operation);
}
}
std::optional<VkImageLayout> full_rate_image_layout(VkDevice device, VkImage image) {
  std::lock_guard lock(images_mutex);
  const auto d = image_layouts.find(device);
  if (d == image_layouts.end()) return std::nullopt;
  const auto i = d->second.find(image);
  return i == d->second.end() ? std::nullopt : std::optional(i->second);
}
struct FullRateImage::Impl {
  VkDevice device{};
  VkImage image{};
  VkDeviceMemory memory{};
  VkCommandPool pool{};
  VkFence fence{};
  std::uint32_t width = 1, height = 1;
  template<class F> F vk(const char* name) const {
    return reinterpret_cast<F>(raw_device_proc(device, name));
  }
  ~Impl() {
    if (fence) vk<PFN_vkDestroyFence>("vkDestroyFence")(device, fence, nullptr);
    if (pool) vk<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device, pool, nullptr);
    if (image) vk<PFN_vkDestroyImage>("vkDestroyImage")(device, image, nullptr);
    if (memory) vk<PFN_vkFreeMemory>("vkFreeMemory")(device, memory, nullptr);
  }
  void initialize(VkPhysicalDevice physical, std::uint32_t family,
                  std::uint32_t queue_index, std::uint32_t layers,
                  std::uint32_t eye_width, std::uint32_t eye_height) {
    const bool native_density = device_density_maps_enabled(device);
    if (native_density) {
      auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
          raw_physical_proc(physical, "vkGetPhysicalDeviceProperties2"));
      if (!properties) properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
          raw_physical_proc(physical, "vkGetPhysicalDeviceProperties2KHR"));
      if (!properties) throw std::runtime_error("Density-map properties unavailable");
      VkPhysicalDeviceFragmentDensityMapPropertiesEXT density{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_PROPERTIES_EXT};
      VkPhysicalDeviceProperties2 host{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &density};
      properties(physical, &host);
      const auto x = std::max(1u, density.maxFragmentDensityTexelSize.width);
      const auto y = std::max(1u, density.maxFragmentDensityTexelSize.height);
      width = eye_width / x + (eye_width % x != 0);
      height = eye_height / y + (eye_height % y != 0);
    } else if (emulated_density_maps(physical)) {
      // Sized for the texel size the guest was told; it is never read.
      const auto x = emulated_density_texel_max.width, y = emulated_density_texel_max.height;
      width = eye_width / x + (eye_width % x != 0);
      height = eye_height / y + (eye_height % y != 0);
    }
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8_UNORM;
    ci.extent = {width, height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (native_density) ci.usage |= VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT;
    check(vk<PFN_vkCreateImage>("vkCreateImage")(device, &ci, nullptr, &image),
          "Create full-rate compatibility image failed");
    VkMemoryRequirements requirements{};
    vk<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(
        device, image, &requirements);
    VkPhysicalDeviceMemoryProperties properties{};
    reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        raw_physical_proc(physical, "vkGetPhysicalDeviceMemoryProperties"))(physical, &properties);
    const auto type = gfx::gpu_only_memory_type(properties, requirements.memoryTypeBits);
    if (type == properties.memoryTypeCount)
      throw std::runtime_error("No compatible full-rate image memory");
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
                            requirements.size, type};
    check(vk<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &ai, nullptr, &memory),
          "Allocate full-rate image memory failed");
    check(vk<PFN_vkBindImageMemory>("vkBindImageMemory")(device, image, memory, 0),
          "Bind full-rate image memory failed");
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pi.queueFamilyIndex = family;
    check(vk<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device, &pi, nullptr, &pool),
          "Create full-rate image command pool failed");
    VkCommandBuffer command{};
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    check(vk<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device, &ca, &command),
          "Allocate full-rate image command failed");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vk<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command, &begin),
          "Begin full-rate image command failed");
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    auto pipeline_barrier = vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    pipeline_barrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkClearColorValue color{{1.f, 1.f, 1.f, 1.f}};
    vk<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(
        command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &barrier.subresourceRange);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = native_density ? VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = native_density ? VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT : VK_ACCESS_MEMORY_READ_BIT;
    pipeline_barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      native_density ? VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                      0, 0, nullptr, 0, nullptr, 1, &barrier);
    check(vk<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command),
          "End full-rate image command failed");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vk<PFN_vkCreateFence>("vkCreateFence")(device, &fi, nullptr, &fence),
          "Create full-rate image fence failed");
    VkQueue queue{};
    vk<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(device, family, queue_index, &queue);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    {
      auto mutex = gfx::queue_mutex(queue);
      std::lock_guard lock(*mutex);
      check(vk<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &submit, fence),
            "Submit full-rate image initialization failed");
    }
    check(vk<PFN_vkWaitForFences>("vkWaitForFences")(device, 1, &fence, VK_TRUE, UINT64_MAX),
          "Wait for full-rate image initialization failed");
    vk<PFN_vkDestroyFence>("vkDestroyFence")(device, fence, nullptr); fence = {};
    vk<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device, pool, nullptr); pool = {};
  }
};
FullRateImage::FullRateImage(VkPhysicalDevice physical, VkDevice device, std::uint32_t family,
                             std::uint32_t queue_index, std::uint32_t layers,
                             std::uint32_t eye_width, std::uint32_t eye_height)
    : impl_(std::make_unique<Impl>()) {
  if (!physical || !device || !layers || !eye_width || !eye_height) throw std::invalid_argument("Invalid full-rate image binding");
  impl_->device = device;
  impl_->initialize(physical, family, queue_index, layers, eye_width, eye_height);
  const auto layout = device_density_maps_enabled(device)
      ? VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_GENERAL;
  {
    std::lock_guard lock(images_mutex);
    image_layouts[device][impl_->image] = layout;
  }
  log::info("vulkan", "Full-resolution compatibility image initialized ({} layers)", layers);
}
FullRateImage::~FullRateImage() {
  std::lock_guard lock(images_mutex);
  if (auto d = image_layouts.find(impl_->device); d != image_layouts.end()) {
    d->second.erase(impl_->image);
    if (d->second.empty()) image_layouts.erase(d);
  }
}
VkImage FullRateImage::image() const { return impl_->image; }
std::uint32_t FullRateImage::width() const { return impl_->width; }
std::uint32_t FullRateImage::height() const { return impl_->height; }
}
