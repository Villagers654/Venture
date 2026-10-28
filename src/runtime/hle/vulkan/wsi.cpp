// Guest window-system integration without a host display. An Android window
// is never shown on the PC (frames reach the headset through OpenXR), so its
// VkSurfaceKHR and swapchains are emulated: swapchain images are plain device
// images, acquire signals the guest's semaphore and fence after the image's
// previous present has executed, and present executes the guest's waits. A
// window with an image consumer receives each presented image's pixels, as
// with EGL window surfaces (pbuffers) and CPU posts.
//
// No host surface or swapchain extension is used, so this works the same with
// every driver and display server, or none. VK_IMAGE_LAYOUT_PRESENT_SRC_KHR is
// only valid for presentable images, which the host never has; the guest's
// uses of it are rewritten to GENERAL, which every image supports.
#include "hle/vulkan/internal.h"
#include "hle/vulkan/vulkan.h"
#include "hle/vulkan/window_readback.h"
#include "hle/vulkan/chain.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "android/native_window.h"
#include "common/log.h"
#include "core/guest.h"

namespace venture::vulkan {
namespace {
constexpr auto presented_layout = VK_IMAGE_LAYOUT_GENERAL;
// Android's own swapchain limits: one layer, identity transform only here.
constexpr std::uint32_t min_images = 2, max_images = 8;
constexpr VkFormat surface_formats_offered[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
constexpr VkPresentModeKHR present_modes_offered[] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR};
constexpr VkImageUsageFlags surface_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
    VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;

template <class F> F device_fn(VkDevice d, const char* name) { return reinterpret_cast<F>(raw_device_proc(d, name)); }
template <class F> F physical_fn(VkPhysicalDevice p, const char* name) {
  return reinterpret_cast<F>(raw_physical_proc(p, name));
}

struct Surface {
  android::NativeWindow* window{};  // holds a reference
  ~Surface() { if (window) android::release(window); }
};
struct Image {
  VkImage image{};
  VkDeviceMemory memory{};
  VkSemaphore presented{};      // signaled by the present that released the image
  bool presented_pending = false;  // that signal still awaits the next acquire's wait
  bool acquired = false;
};
struct Swapchain {
  VkDevice device{};
  android::NativeWindow* window{};  // holds a reference
  VkFormat format{};
  VkExtent2D extent{};
  std::vector<Image> images;
  std::deque<std::uint32_t> available;  // presentation order, oldest first
  bool retired = false;
  VkResult deferred_result = VK_SUCCESS;
  // Submissions touching the images' semaphores; destruction waits for them.
  std::deque<VkFence> in_flight;
  std::vector<VkFence> spare;
  ~Swapchain() { if (window) android::release(window); }
};

std::mutex mutex;
std::uint64_t next_handle = 0;
std::map<VkSurfaceKHR, std::shared_ptr<Surface>> surfaces;
std::map<VkSwapchainKHR, std::shared_ptr<Swapchain>> swapchains;
std::atomic<bool> any_swapchain{false};

template <class H> H new_handle() { return reinterpret_cast<H>(static_cast<std::uintptr_t>(++next_handle)); }
std::shared_ptr<Surface> find_surface(VkSurfaceKHR surface) {
  std::lock_guard lock(mutex);
  auto it = surfaces.find(surface);
  return it == surfaces.end() ? nullptr : it->second;
}
std::shared_ptr<Swapchain> find_swapchain(VkSwapchainKHR chain) {
  std::lock_guard lock(mutex);
  auto it = swapchains.find(chain);
  return it == swapchains.end() ? nullptr : it->second;
}

bool format_supported(VkPhysicalDevice physical, VkFormat format) {
  VkFormatProperties properties{};
  physical_fn<PFN_vkGetPhysicalDeviceFormatProperties>(physical, "vkGetPhysicalDeviceFormatProperties")(
      physical, format, &properties);
  constexpr VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
  return (properties.optimalTilingFeatures & needed) == needed;
}
std::vector<VkFormat> offered_formats(VkPhysicalDevice physical) {
  std::vector<VkFormat> out;
  for (auto format : surface_formats_offered)
    if (format_supported(physical, format)) out.push_back(format);
  return out;
}
VkExtent2D window_extent(android::NativeWindow* window) {
  std::lock_guard lock(window->lock);
  if (window->width <= 0 || window->height <= 0) return {UINT32_MAX, UINT32_MAX};
  return {std::uint32_t(window->width), std::uint32_t(window->height)};
}
VkSurfaceCapabilitiesKHR capabilities(VkPhysicalDevice physical, const Surface& surface) {
  VkPhysicalDeviceProperties properties{};
  physical_fn<PFN_vkGetPhysicalDeviceProperties>(physical, "vkGetPhysicalDeviceProperties")(physical, &properties);
  VkSurfaceCapabilitiesKHR out{};
  out.minImageCount = min_images;
  out.maxImageCount = max_images;
  out.currentExtent = window_extent(surface.window);
  out.minImageExtent = {1, 1};
  out.maxImageExtent = {properties.limits.maxImageDimension2D, properties.limits.maxImageDimension2D};
  out.maxImageArrayLayers = 1;
  out.supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  out.currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  out.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
  out.supportedUsageFlags = surface_usage;
  return out;
}

VkResult VKAPI_CALL create_android_surface(VkInstance, const VkAndroidSurfaceCreateInfoKHR* info,
                                           const VkAllocationCallbacks*, VkSurfaceKHR* out) {
  if (!out) return VK_ERROR_INITIALIZATION_FAILED;
  *out = VK_NULL_HANDLE;
  if (!info || info->flags) return VK_ERROR_INITIALIZATION_FAILED;
  auto window = android::pin_native_window(h2g(info->window));
  if (!window) return VK_ERROR_SURFACE_LOST_KHR;
  auto surface = std::make_shared<Surface>();
  android::acquire(window.get());
  surface->window = window.get();
  std::lock_guard lock(mutex);
  *out = new_handle<VkSurfaceKHR>();
  surfaces.emplace(*out, std::move(surface));
  return VK_SUCCESS;
}
void VKAPI_CALL destroy_surface(VkInstance, VkSurfaceKHR surface, const VkAllocationCallbacks*) {
  std::shared_ptr<Surface> record;  // released outside the lock
  std::lock_guard lock(mutex);
  if (auto it = surfaces.find(surface); it != surfaces.end()) {
    record = std::move(it->second);
    surfaces.erase(it);
  }
}
VkResult VKAPI_CALL surface_support(VkPhysicalDevice, std::uint32_t, VkSurfaceKHR surface, VkBool32* out) {
  if (!out) return VK_ERROR_INITIALIZATION_FAILED;
  auto record = find_surface(surface);
  if (!record) return VK_ERROR_SURFACE_LOST_KHR;
  // Presentation only executes semaphore waits, which every queue can.
  *out = VK_TRUE;
  return VK_SUCCESS;
}
VkResult VKAPI_CALL surface_capabilities(VkPhysicalDevice physical, VkSurfaceKHR surface,
                                         VkSurfaceCapabilitiesKHR* out) {
  auto record = find_surface(surface);
  if (!record) return VK_ERROR_SURFACE_LOST_KHR;
  if (!out) return VK_ERROR_INITIALIZATION_FAILED;
  *out = capabilities(physical, *record);
  return VK_SUCCESS;
}
VkResult VKAPI_CALL surface_capabilities2(VkPhysicalDevice physical, const VkPhysicalDeviceSurfaceInfo2KHR* info,
                                          VkSurfaceCapabilities2KHR* out) {
  if (!info || !out) return VK_ERROR_INITIALIZATION_FAILED;
  return surface_capabilities(physical, info->surface, &out->surfaceCapabilities);
}
template <class T, class Fill>
VkResult enumerate_into(std::size_t size, std::uint32_t* count, T* out, Fill fill) {
  if (!count) return VK_ERROR_INITIALIZATION_FAILED;
  if (!out) { *count = std::uint32_t(size); return VK_SUCCESS; }
  const auto n = std::min<std::size_t>(*count, size);
  for (std::size_t i = 0; i < n; ++i) fill(out[i], i);
  *count = std::uint32_t(n);
  return n < size ? VK_INCOMPLETE : VK_SUCCESS;
}
VkResult VKAPI_CALL surface_formats(VkPhysicalDevice physical, VkSurfaceKHR surface, std::uint32_t* count,
                                    VkSurfaceFormatKHR* out) {
  if (!find_surface(surface)) return VK_ERROR_SURFACE_LOST_KHR;
  auto formats = offered_formats(physical);
  return enumerate_into(formats.size(), count, out, [&](auto& o, std::size_t i) {
    o = {formats[i], VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
  });
}
VkResult VKAPI_CALL surface_formats2(VkPhysicalDevice physical, const VkPhysicalDeviceSurfaceInfo2KHR* info,
                                     std::uint32_t* count, VkSurfaceFormat2KHR* out) {
  if (!info) return VK_ERROR_INITIALIZATION_FAILED;
  if (!find_surface(info->surface)) return VK_ERROR_SURFACE_LOST_KHR;
  auto formats = offered_formats(physical);
  return enumerate_into(formats.size(), count, out, [&](auto& o, std::size_t i) {
    o.surfaceFormat = {formats[i], VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
  });
}
VkResult VKAPI_CALL present_modes(VkPhysicalDevice, VkSurfaceKHR surface, std::uint32_t* count,
                                  VkPresentModeKHR* out) {
  if (!find_surface(surface)) return VK_ERROR_SURFACE_LOST_KHR;
  return enumerate_into(std::size(present_modes_offered), count, out,
                        [](auto& o, std::size_t i) { o = present_modes_offered[i]; });
}
VkResult VKAPI_CALL present_rectangles(VkPhysicalDevice physical, VkSurfaceKHR surface, std::uint32_t* count,
                                       VkRect2D* out) {
  auto record = find_surface(surface);
  if (!record) return VK_ERROR_SURFACE_LOST_KHR;
  auto extent = window_extent(record->window);
  if (extent.width == UINT32_MAX) extent = capabilities(physical, *record).maxImageExtent;
  return enumerate_into(1, count, out, [&](auto& o, std::size_t) { o = {{0, 0}, extent}; });
}
VkResult VKAPI_CALL group_present_capabilities(VkDevice, VkDeviceGroupPresentCapabilitiesKHR* out) {
  if (!out) return VK_ERROR_INITIALIZATION_FAILED;
  std::fill(std::begin(out->presentMask), std::end(out->presentMask), 0u);
  out->presentMask[0] = 1;
  out->modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
  return VK_SUCCESS;
}
VkResult VKAPI_CALL group_surface_present_modes(VkDevice, VkSurfaceKHR surface,
                                                VkDeviceGroupPresentModeFlagsKHR* out) {
  if (!find_surface(surface)) return VK_ERROR_SURFACE_LOST_KHR;
  if (!out) return VK_ERROR_INITIALIZATION_FAILED;
  *out = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
  return VK_SUCCESS;
}

// Tracks a submission that uses the swapchain's semaphores; caller holds `mutex`.
VkFence tracking_fence(Swapchain& chain) {
  auto status = device_fn<PFN_vkGetFenceStatus>(chain.device, "vkGetFenceStatus");
  while (!chain.in_flight.empty() && status(chain.device, chain.in_flight.front()) == VK_SUCCESS) {
    chain.spare.push_back(chain.in_flight.front());
    chain.in_flight.pop_front();
  }
  VkFence fence{};
  if (!chain.spare.empty()) {
    fence = chain.spare.back();
    chain.spare.pop_back();
    if (device_fn<PFN_vkResetFences>(chain.device, "vkResetFences")(chain.device, 1, &fence) != VK_SUCCESS) {
      device_fn<PFN_vkDestroyFence>(chain.device, "vkDestroyFence")(chain.device, fence, nullptr);
      return VK_NULL_HANDLE;
    }
    return fence;
  }
  VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (device_fn<PFN_vkCreateFence>(chain.device, "vkCreateFence")(chain.device, &info, nullptr, &fence) != VK_SUCCESS)
    return VK_NULL_HANDLE;
  return fence;
}
// Destroys everything once the swapchain's submissions have completed.
void destroy_resources(Swapchain& chain) {
  auto d = chain.device;
  if (!chain.in_flight.empty()) {
    std::vector<VkFence> fences(chain.in_flight.begin(), chain.in_flight.end());
    device_fn<PFN_vkWaitForFences>(d, "vkWaitForFences")(d, std::uint32_t(fences.size()), fences.data(), VK_TRUE,
                                                          UINT64_MAX);
  }
  auto destroy_fence = device_fn<PFN_vkDestroyFence>(d, "vkDestroyFence");
  for (auto fence : chain.in_flight) destroy_fence(d, fence, nullptr);
  for (auto fence : chain.spare) destroy_fence(d, fence, nullptr);
  chain.in_flight.clear();
  chain.spare.clear();
  for (auto& image : chain.images) {
    if (image.presented) device_fn<PFN_vkDestroySemaphore>(d, "vkDestroySemaphore")(d, image.presented, nullptr);
    if (image.image) device_fn<PFN_vkDestroyImage>(d, "vkDestroyImage")(d, image.image, nullptr);
    if (image.memory) device_fn<PFN_vkFreeMemory>(d, "vkFreeMemory")(d, image.memory, nullptr);
  }
  chain.images.clear();
}
VkResult create_image(VkDevice device, const VkSwapchainCreateInfoKHR& info, Image& out) {
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  VkImageFormatListCreateInfo list{};
  for (auto* next = static_cast<const VkBaseInStructure*>(info.pNext); next; next = next->pNext)
    if (next->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
      // Only this structure of the swapchain's chain applies to its images.
      list = *reinterpret_cast<const VkImageFormatListCreateInfo*>(next);
      list.pNext = nullptr;
      ci.pNext = &list;
    }
  if (info.flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR)
    ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = info.imageFormat;
  ci.extent = {info.imageExtent.width, info.imageExtent.height, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = info.imageArrayLayers;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = info.imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // window consumers read it back
  ci.sharingMode = info.imageSharingMode;
  ci.queueFamilyIndexCount = info.imageSharingMode == VK_SHARING_MODE_CONCURRENT ? info.queueFamilyIndexCount : 0;
  ci.pQueueFamilyIndices = ci.queueFamilyIndexCount ? info.pQueueFamilyIndices : nullptr;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  auto r = device_fn<PFN_vkCreateImage>(device, "vkCreateImage")(device, &ci, nullptr, &out.image);
  if (r != VK_SUCCESS) return r;
  VkMemoryRequirements requirements{};
  device_fn<PFN_vkGetImageMemoryRequirements>(device, "vkGetImageMemoryRequirements")(device, out.image, &requirements);
  auto physical = device_physical(device);
  VkPhysicalDeviceMemoryProperties memory{};
  physical_fn<PFN_vkGetPhysicalDeviceMemoryProperties>(physical, "vkGetPhysicalDeviceMemoryProperties")(physical, &memory);
  std::uint32_t type = memory.memoryTypeCount;
  for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
    if (!(requirements.memoryTypeBits & (1u << i))) continue;
    if (type == memory.memoryTypeCount) type = i;
    if (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) { type = i; break; }
  }
  if (type == memory.memoryTypeCount) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, type};
  r = device_fn<PFN_vkAllocateMemory>(device, "vkAllocateMemory")(device, &allocate, nullptr, &out.memory);
  if (r != VK_SUCCESS) return r;
  r = device_fn<PFN_vkBindImageMemory>(device, "vkBindImageMemory")(device, out.image, out.memory, 0);
  if (r != VK_SUCCESS) return r;
  VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  return device_fn<PFN_vkCreateSemaphore>(device, "vkCreateSemaphore")(device, &semaphore, nullptr, &out.presented);
}
VkResult VKAPI_CALL create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR* info,
                                     const VkAllocationCallbacks*, VkSwapchainKHR* out) {
  if (!info || !out) return VK_ERROR_INITIALIZATION_FAILED;
  *out = VK_NULL_HANDLE;
  if (info->oldSwapchain)
    if (auto old = find_swapchain(info->oldSwapchain)) {
      std::lock_guard lock(mutex);
      old->retired = true;
    }
  auto surface = find_surface(info->surface);
  if (!surface) return VK_ERROR_SURFACE_LOST_KHR;
  auto physical = device_physical(device);
  auto formats = offered_formats(physical);
  const auto caps = capabilities(physical, *surface);
  if (std::ranges::find(formats, info->imageFormat) == formats.end() ||
      info->imageColorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR ||
      std::ranges::find(present_modes_offered, info->presentMode) == std::end(present_modes_offered))
    return VK_ERROR_FORMAT_NOT_SUPPORTED;
  if (info->flags & ~VkSwapchainCreateFlagsKHR(VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) ||
      info->imageArrayLayers != 1 || (info->imageUsage & ~caps.supportedUsageFlags) ||
      !info->imageExtent.width || !info->imageExtent.height ||
      info->imageExtent.width > caps.maxImageExtent.width || info->imageExtent.height > caps.maxImageExtent.height ||
      info->minImageCount > max_images)
    return VK_ERROR_INITIALIZATION_FAILED;
  auto chain = std::make_shared<Swapchain>();
  chain->device = device;
  chain->format = info->imageFormat;
  chain->extent = info->imageExtent;
  chain->images.resize(std::max(info->minImageCount, min_images));
  VkResult r = VK_SUCCESS;
  for (auto& image : chain->images)
    if (r == VK_SUCCESS) r = create_image(device, *info, image);
  if (r != VK_SUCCESS) {
    destroy_resources(*chain);
    // Image creation reports format/usage combinations the device lacks.
    return r == VK_ERROR_FORMAT_NOT_SUPPORTED ? VK_ERROR_INITIALIZATION_FAILED : r;
  }
  for (std::uint32_t i = 0; i < chain->images.size(); ++i) chain->available.push_back(i);
  android::acquire(surface->window);
  chain->window = surface->window;
  log::info("vulkan", "Guest swapchain {}x{} format {} with {} images (offscreen)", info->imageExtent.width,
            info->imageExtent.height, int(info->imageFormat), chain->images.size());
  std::lock_guard lock(mutex);
  *out = new_handle<VkSwapchainKHR>();
  swapchains.emplace(*out, std::move(chain));
  any_swapchain.store(true, std::memory_order_relaxed);
  return VK_SUCCESS;
}
void VKAPI_CALL destroy_swapchain(VkDevice, VkSwapchainKHR handle, const VkAllocationCallbacks*) {
  std::shared_ptr<Swapchain> chain;
  {
    std::lock_guard lock(mutex);
    auto it = swapchains.find(handle);
    if (it == swapchains.end()) return;
    chain = std::move(it->second);
    swapchains.erase(it);
  }
  destroy_resources(*chain);
}
VkResult VKAPI_CALL swapchain_images(VkDevice, VkSwapchainKHR handle, std::uint32_t* count, VkImage* out) {
  auto chain = find_swapchain(handle);
  if (!chain) return VK_ERROR_SURFACE_LOST_KHR;
  return enumerate_into(chain->images.size(), count, out, [&](auto& o, std::size_t i) { o = chain->images[i].image; });
}
VkResult acquire(VkDevice device, VkSwapchainKHR handle, std::uint64_t timeout, VkSemaphore semaphore,
                 VkFence fence, std::uint32_t* index) {
  if (!index) return VK_ERROR_INITIALIZATION_FAILED;
  auto chain = find_swapchain(handle);
  if (!chain) return VK_ERROR_SURFACE_LOST_KHR;
  auto queue = internal_queue(device);
  if (!queue) return VK_ERROR_DEVICE_LOST;
  VkSemaphore wait{};
  VkFence tracking{};
  {
    std::lock_guard lock(mutex);
    if (chain->retired) return VK_ERROR_OUT_OF_DATE_KHR;
    if (chain->deferred_result != VK_SUCCESS) {
      const auto result = chain->deferred_result;
      chain->deferred_result = VK_SUCCESS;
      return result;
    }
    // Presented images return at once; an application holding all the others
    // waits forever on Android too, so a timeout is reported instead.
    if (chain->available.empty()) return timeout ? VK_TIMEOUT : VK_NOT_READY;
    tracking = tracking_fence(*chain);
    if (!tracking) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *index = chain->available.front();
    chain->available.pop_front();
    auto& image = chain->images[*index];
    image.acquired = true;
    if (image.presented_pending) wait = image.presented;
    image.presented_pending = false;
  }
  VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkSubmitInfo submits[2]{{VK_STRUCTURE_TYPE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SUBMIT_INFO}};
  submits[0].waitSemaphoreCount = wait ? 1 : 0;
  submits[0].pWaitSemaphores = &wait;
  submits[0].pWaitDstStageMask = &stage;
  submits[0].signalSemaphoreCount = semaphore ? 1 : 0;
  submits[0].pSignalSemaphores = &semaphore;
  VkResult r;
  {
    std::lock_guard queue_lock(*queue->mutex);
    auto submit = device_fn<PFN_vkQueueSubmit>(device, "vkQueueSubmit");
    r = submit(queue->queue, 1, &submits[0], tracking);
    // A later empty batch signals the guest's fence after the one above.
    if (r == VK_SUCCESS && fence) r = submit(queue->queue, 0, nullptr, fence);
  }
  {
    std::lock_guard lock(mutex);
    (r == VK_SUCCESS ? chain->in_flight.push_back(tracking) : chain->spare.push_back(tracking));
  }
  return r == VK_SUCCESS ? external_semaphore_signal(device, semaphore) : r;
}
VkResult VKAPI_CALL acquire_next_image(VkDevice device, VkSwapchainKHR chain, std::uint64_t timeout,
                                       VkSemaphore semaphore, VkFence fence, std::uint32_t* index) {
  return acquire(device, chain, timeout, semaphore, fence, index);
}
VkResult VKAPI_CALL acquire_next_image2(VkDevice device, const VkAcquireNextImageInfoKHR* info, std::uint32_t* index) {
  if (!info) return VK_ERROR_INITIALIZATION_FAILED;
  return acquire(device, info->swapchain, info->timeout, info->semaphore, info->fence, index);
}
VkResult queue_present_impl(VkQueue queue, const VkPresentInfoKHR* info, bool* waits_issued) {
  if (!info || (info->swapchainCount && (!info->pSwapchains || !info->pImageIndices)))
    return VK_ERROR_INITIALIZATION_FAILED;
  auto binding = queue_binding(queue);
  if (!binding) return VK_ERROR_DEVICE_LOST;
  std::vector<std::pair<std::shared_ptr<Swapchain>, std::uint32_t>> presents;
  {
    std::lock_guard lock(mutex);
    for (std::uint32_t i = 0; i < info->swapchainCount; ++i) {
      auto it = swapchains.find(info->pSwapchains[i]);
      if (it == swapchains.end() || info->pImageIndices[i] >= it->second->images.size() ||
          !it->second->images[info->pImageIndices[i]].acquired)
        return VK_ERROR_SURFACE_LOST_KHR;
      presents.emplace_back(it->second, info->pImageIndices[i]);
    }
  }
  if (info->waitSemaphoreCount) texture_queue_uncertain(queue);
  std::span<const VkSemaphore> waits{info->pWaitSemaphores, info->waitSemaphoreCount};
  // A window consumer gets the completed pixels; the readback executes the waits.
  for (auto& [chain, index] : presents) {
    if (!android::has_window_image_consumer(chain->window)) continue;
    auto r = publish_window_image(binding->device, queue, binding->family, chain->images[index].image,
                                  chain->format, chain->extent, chain->window, waits);
    if (r != VK_SUCCESS) return r;
    if (!waits.empty()) *waits_issued = true;
    waits = {};
  }
  std::vector<VkSemaphore> signals;
  std::vector<VkFence> tracking;
  {
    std::lock_guard lock(mutex);
    for (auto& [chain, index] : presents) {
      signals.push_back(chain->images[index].presented);
      tracking.push_back(tracking_fence(*chain));
      if (!tracking.back()) {
        tracking.pop_back();
        for (std::size_t i = 0; i < tracking.size(); ++i) presents[i].first->spare.push_back(tracking[i]);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
    }
  }
  std::vector<VkPipelineStageFlags> stages(waits.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.waitSemaphoreCount = std::uint32_t(waits.size());
  submit.pWaitSemaphores = waits.data();
  submit.pWaitDstStageMask = stages.data();
  submit.signalSemaphoreCount = std::uint32_t(signals.size());
  submit.pSignalSemaphores = signals.data();
  // The caller holds the queue's submission mutex (a queue command).
  auto host_submit = device_fn<PFN_vkQueueSubmit>(binding->device, "vkQueueSubmit");
  VkResult r = host_submit(queue, 1, &submit, tracking.empty() ? VK_NULL_HANDLE : tracking.front());
  if (r == VK_SUCCESS) *waits_issued = true;
  for (std::size_t i = 1; r == VK_SUCCESS && i < tracking.size(); ++i) r = host_submit(queue, 0, nullptr, tracking[i]);
  if (r == VK_SUCCESS) sync_fd_submitted(queue, 1, &submit);
  std::lock_guard lock(mutex);
  VkResult overall = r;
  for (std::size_t i = 0; i < presents.size(); ++i) {
    auto& [chain, index] = presents[i];
    auto& image = chain->images[index];
    if (r == VK_SUCCESS) {
      chain->in_flight.push_back(tracking[i]);
      image.presented_pending = true;
    } else chain->spare.push_back(tracking[i]);
    image.acquired = false;
    chain->available.push_back(index);
    VkResult result = r;
    if (r == VK_SUCCESS) {
      const auto extent = window_extent(chain->window);
      if (chain->retired) result = VK_ERROR_OUT_OF_DATE_KHR;
      else if (extent.width != UINT32_MAX && (extent.width != chain->extent.width || extent.height != chain->extent.height))
        result = VK_SUBOPTIMAL_KHR;  // the window was resized
    }
    if (info->pResults) info->pResults[i] = result;
    // Errors take precedence over SUBOPTIMAL, which takes precedence over SUCCESS.
    if (result < 0 && overall >= 0) overall = result;
    else if (result == VK_SUBOPTIMAL_KHR && overall == VK_SUCCESS) overall = result;
  }
  return overall;
}

VkResult VKAPI_CALL queue_present(VkQueue queue, const VkPresentInfoKHR* info) {
  if (!info) return VK_ERROR_INITIALIZATION_FAILED;
  if ((info->swapchainCount && (!info->pSwapchains || !info->pImageIndices)) ||
      (info->waitSemaphoreCount && !info->pWaitSemaphores)) return VK_ERROR_INITIALIZATION_FAILED;
  struct Present {
    Chain storage;
    VkPresentInfoKHR info;
    std::vector<VkResult> results;
    std::vector<VkPipelineStageFlags> stages;
    std::vector<std::shared_ptr<Swapchain>> chains;
    std::mutex result_mutex;
    VkResult result = VK_SUCCESS;
    bool returned = false, executed = false, waits_issued = false;
  };
  try {
    auto present = std::make_shared<Present>();
    {
      std::lock_guard lock(mutex);
      for (std::uint32_t i = 0; i < info->swapchainCount; ++i) {
        const auto found = swapchains.find(info->pSwapchains[i]);
        if (found == swapchains.end()) return VK_ERROR_SURFACE_LOST_KHR;
        if (found->second->deferred_result != VK_SUCCESS) {
          const auto result = found->second->deferred_result;
          found->second->deferred_result = VK_SUCCESS;
          return result;
        }
        present->chains.push_back(found->second);
      }
    }
    present->storage.retain_input_arrays();
    const auto* copied = static_cast<const VkPresentInfoKHR*>(present->storage.translate(info));
    if (!copied || present->storage.result() != VK_SUCCESS) return VK_ERROR_INITIALIZATION_FAILED;
    present->info = *copied;
    present->results.resize(info->swapchainCount, VK_SUCCESS);
    present->stages.resize(info->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    present->info.pResults = present->results.data();
    auto result = native_queue_work(queue,
        {present->info.pWaitSemaphores, present->info.waitSemaphoreCount}, {},
        [present](VkQueue native) {
          std::lock_guard result_lock(present->result_mutex);
          auto result = queue_present_impl(native, &present->info, &present->waits_issued);
          // An asynchronously rejected present still owns its admitted waits.
          // Consume them so a later re-signal cannot inherit a live payload.
          if (result != VK_SUCCESS && !present->waits_issued) {
            const auto binding = queue_binding(native);
            if (!binding) return VK_ERROR_DEVICE_LOST;
            VkSubmitInfo consume{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            consume.waitSemaphoreCount = present->info.waitSemaphoreCount;
            consume.pWaitSemaphores = present->info.pWaitSemaphores;
            consume.pWaitDstStageMask = present->stages.data();
            const auto consumed = device_fn<PFN_vkQueueSubmit>(binding->device, "vkQueueSubmit")(
                native, 1, &consume, VK_NULL_HANDLE);
            if (consumed != VK_SUCCESS) result = consumed;
            else present->waits_issued = true;
          }
          present->result = result;
          present->executed = true;
          if (present->returned) {
            std::lock_guard lock(mutex);
            for (std::size_t i = 0; i < present->chains.size(); ++i) {
              const auto status = present->results[i] == VK_SUCCESS ? result : present->results[i];
              if (status != VK_SUCCESS) present->chains[i]->deferred_result = status;
            }
          }
          return result;
        }, &present->waits_issued);
    std::lock_guard result_lock(present->result_mutex);
    if (result == VK_SUCCESS && present->executed) result = present->result;
    if (info->pResults)
      std::copy(present->results.begin(), present->results.end(), info->pResults);
    present->returned = true;
    return result;
  } catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}

// PRESENT_SRC_KHR to GENERAL in the guest's commands and render passes.
VkImageLayout layout(VkImageLayout l) { return l == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ? presented_layout : l; }
template <class Barrier> bool presents(std::span<const Barrier> barriers) {
  return std::ranges::any_of(barriers, [](const Barrier& b) {
    return b.oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR || b.newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  });
}
template <class Barrier> std::vector<Barrier> rewritten(std::span<const Barrier> barriers) {
  std::vector<Barrier> out(barriers.begin(), barriers.end());
  for (auto& b : out) { b.oldLayout = layout(b.oldLayout); b.newLayout = layout(b.newLayout); }
  return out;
}
template <class F, ProcedureName Name> F next(VkCommandBuffer command) {
  if (auto f = cached_layer_proc<Layer::wsi, Name>()) return reinterpret_cast<F>(f);
  return reinterpret_cast<F>(raw_device_proc(command_device(command), Name.value));
}
template <class F, ProcedureName Name> F next_device(VkDevice device) {
  if (auto f = cached_layer_proc<Layer::wsi, Name>()) return reinterpret_cast<F>(f);
  return reinterpret_cast<F>(raw_device_proc(device, Name.value));
}
void VKAPI_CALL pipeline_barrier(VkCommandBuffer command, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                                 VkDependencyFlags flags, std::uint32_t memory_count, const VkMemoryBarrier* memory,
                                 std::uint32_t buffer_count, const VkBufferMemoryBarrier* buffers,
                                 std::uint32_t image_count, const VkImageMemoryBarrier* images) {
  auto f = next<PFN_vkCmdPipelineBarrier, "vkCmdPipelineBarrier">(command);
  std::span<const VkImageMemoryBarrier> list{images, image_count};
  if (!any_swapchain.load(std::memory_order_relaxed) || !presents(list))
    return f(command, src, dst, flags, memory_count, memory, buffer_count, buffers, image_count, images);
  auto copy = rewritten(list);
  f(command, src, dst, flags, memory_count, memory, buffer_count, buffers, image_count, copy.data());
}
void VKAPI_CALL wait_events(VkCommandBuffer command, std::uint32_t event_count, const VkEvent* events,
                            VkPipelineStageFlags src, VkPipelineStageFlags dst, std::uint32_t memory_count,
                            const VkMemoryBarrier* memory, std::uint32_t buffer_count,
                            const VkBufferMemoryBarrier* buffers, std::uint32_t image_count,
                            const VkImageMemoryBarrier* images) {
  auto f = next<PFN_vkCmdWaitEvents, "vkCmdWaitEvents">(command);
  std::span<const VkImageMemoryBarrier> list{images, image_count};
  if (!any_swapchain.load(std::memory_order_relaxed) || !presents(list))
    return f(command, event_count, events, src, dst, memory_count, memory, buffer_count, buffers, image_count, images);
  auto copy = rewritten(list);
  f(command, event_count, events, src, dst, memory_count, memory, buffer_count, buffers, image_count, copy.data());
}
// Dependency infos with rewritten image barriers; `storage` keeps them alive.
bool rewrite_dependencies(std::span<const VkDependencyInfo> infos, std::vector<VkDependencyInfo>& out,
                          std::vector<std::vector<VkImageMemoryBarrier2>>& storage) {
  if (!any_swapchain.load(std::memory_order_relaxed)) return false;
  if (std::ranges::none_of(infos, [](const VkDependencyInfo& d) {
        return presents(std::span{d.pImageMemoryBarriers, d.imageMemoryBarrierCount});
      }))
    return false;
  out.assign(infos.begin(), infos.end());
  storage.resize(out.size());
  for (std::size_t i = 0; i < out.size(); ++i) {
    storage[i] = rewritten(std::span{out[i].pImageMemoryBarriers, out[i].imageMemoryBarrierCount});
    out[i].pImageMemoryBarriers = storage[i].data();
  }
  return true;
}
template <ProcedureName Name>
void VKAPI_CALL pipeline_barrier2(VkCommandBuffer command, const VkDependencyInfo* info) {
  auto f = next<PFN_vkCmdPipelineBarrier2, Name>(command);
  std::vector<VkDependencyInfo> copy;
  std::vector<std::vector<VkImageMemoryBarrier2>> storage;
  if (!info || !rewrite_dependencies({info, 1}, copy, storage)) return f(command, info);
  f(command, copy.data());
}
template <ProcedureName Name>
void VKAPI_CALL wait_events2(VkCommandBuffer command, std::uint32_t count, const VkEvent* events,
                             const VkDependencyInfo* infos) {
  auto f = next<PFN_vkCmdWaitEvents2, Name>(command);
  std::vector<VkDependencyInfo> copy;
  std::vector<std::vector<VkImageMemoryBarrier2>> storage;
  if (!infos || !rewrite_dependencies({infos, count}, copy, storage)) return f(command, count, events, infos);
  f(command, count, events, copy.data());
}
template <ProcedureName Name>
void VKAPI_CALL set_event2(VkCommandBuffer command, VkEvent event, const VkDependencyInfo* info) {
  auto f = next<PFN_vkCmdSetEvent2, Name>(command);
  std::vector<VkDependencyInfo> copy;
  std::vector<std::vector<VkImageMemoryBarrier2>> storage;
  if (!info || !rewrite_dependencies({info, 1}, copy, storage)) return f(command, event, info);
  f(command, event, copy.data());
}
template <class Attachment> bool presents_attachments(std::span<const Attachment> attachments) {
  return std::ranges::any_of(attachments, [](const Attachment& a) {
    return a.initialLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR || a.finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  });
}
template <class Attachment> std::vector<Attachment> rewritten_attachments(std::span<const Attachment> attachments) {
  std::vector<Attachment> out(attachments.begin(), attachments.end());
  for (auto& a : out) { a.initialLayout = layout(a.initialLayout); a.finalLayout = layout(a.finalLayout); }
  return out;
}
// Render passes may precede the swapchain, so they are always rewritten.
VkResult VKAPI_CALL create_render_pass(VkDevice device, const VkRenderPassCreateInfo* info,
                                       const VkAllocationCallbacks* alloc, VkRenderPass* out) {
  auto f = next_device<PFN_vkCreateRenderPass, "vkCreateRenderPass">(device);
  std::span<const VkAttachmentDescription> list{info ? info->pAttachments : nullptr, info ? info->attachmentCount : 0};
  if (!info || !presents_attachments(list)) return f(device, info, alloc, out);
  auto attachments = rewritten_attachments(list);
  auto copy = *info;
  copy.pAttachments = attachments.data();
  return f(device, &copy, alloc, out);
}
template <ProcedureName Name>
VkResult VKAPI_CALL create_render_pass2(VkDevice device, const VkRenderPassCreateInfo2* info,
                                        const VkAllocationCallbacks* alloc, VkRenderPass* out) {
  auto f = next_device<PFN_vkCreateRenderPass2, Name>(device);
  std::span<const VkAttachmentDescription2> list{info ? info->pAttachments : nullptr, info ? info->attachmentCount : 0};
  if (!info || !presents_attachments(list)) return f(device, info, alloc, out);
  auto attachments = rewritten_attachments(list);
  auto copy = *info;
  copy.pAttachments = attachments.data();
  return f(device, &copy, alloc, out);
}
}  // namespace

bool emulated_wsi_extension(std::string_view name) {
  return name == "VK_KHR_surface" || name == "VK_KHR_android_surface" || name == "VK_KHR_get_surface_capabilities2" ||
         name == "VK_KHR_swapchain" || name == "VK_KHR_swapchain_mutable_format";
}
bool host_wsi_extension(std::string_view name) {
  if (emulated_wsi_extension(name)) return false;
  // Everything that needs a host surface, display or swapchain.
  return name.ends_with("_surface") || name.find("surface_") != name.npos || name.find("display") != name.npos ||
         name.find("swapchain") != name.npos || name.find("present") != name.npos ||
         name == "VK_EXT_hdr_metadata" || name == "VK_EXT_full_screen_exclusive" || name == "VK_NV_low_latency2";
}
void wsi_instance_extensions(std::vector<VkExtensionProperties>& values) {
  std::erase_if(values, [](auto& p) { return host_wsi_extension(p.extensionName) || emulated_wsi_extension(p.extensionName); });
  values.push_back({"VK_KHR_surface", VK_KHR_SURFACE_SPEC_VERSION});
  values.push_back({"VK_KHR_android_surface", VK_KHR_ANDROID_SURFACE_SPEC_VERSION});
  values.push_back({"VK_KHR_get_surface_capabilities2", VK_KHR_GET_SURFACE_CAPABILITIES_2_SPEC_VERSION});
}
void wsi_device_extensions(std::vector<VkExtensionProperties>& values) {
  std::erase_if(values, [](auto& p) { return host_wsi_extension(p.extensionName) || emulated_wsi_extension(p.extensionName); });
  values.push_back({"VK_KHR_swapchain", VK_KHR_SWAPCHAIN_SPEC_VERSION});
  values.push_back({"VK_KHR_swapchain_mutable_format", VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_SPEC_VERSION});
}
PFN_vkVoidFunction wsi_override(std::string_view name) {
#define FN(command, function) \
  if (name == #command) return reinterpret_cast<PFN_vkVoidFunction>(function)
  FN(vkCreateAndroidSurfaceKHR, create_android_surface);
  FN(vkDestroySurfaceKHR, destroy_surface);
  FN(vkGetPhysicalDeviceSurfaceSupportKHR, surface_support);
  FN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, surface_capabilities);
  FN(vkGetPhysicalDeviceSurfaceCapabilities2KHR, surface_capabilities2);
  FN(vkGetPhysicalDeviceSurfaceFormatsKHR, surface_formats);
  FN(vkGetPhysicalDeviceSurfaceFormats2KHR, surface_formats2);
  FN(vkGetPhysicalDeviceSurfacePresentModesKHR, present_modes);
  FN(vkGetPhysicalDevicePresentRectanglesKHR, present_rectangles);
  FN(vkGetDeviceGroupPresentCapabilitiesKHR, group_present_capabilities);
  FN(vkGetDeviceGroupSurfacePresentModesKHR, group_surface_present_modes);
  FN(vkCreateSwapchainKHR, create_swapchain);
  FN(vkDestroySwapchainKHR, destroy_swapchain);
  FN(vkGetSwapchainImagesKHR, swapchain_images);
  FN(vkAcquireNextImageKHR, acquire_next_image);
  FN(vkAcquireNextImage2KHR, acquire_next_image2);
  FN(vkQueuePresentKHR, queue_present);
#undef FN
  return nullptr;
}
PFN_vkVoidFunction wsi_proc(std::string_view name) {
#define FN(command, function) \
  if (name == #command) return reinterpret_cast<PFN_vkVoidFunction>(function)
  FN(vkCmdPipelineBarrier, pipeline_barrier);
  FN(vkCmdPipelineBarrier2, pipeline_barrier2<"vkCmdPipelineBarrier2">);
  FN(vkCmdPipelineBarrier2KHR, pipeline_barrier2<"vkCmdPipelineBarrier2KHR">);
  FN(vkCmdWaitEvents, wait_events);
  FN(vkCmdWaitEvents2, wait_events2<"vkCmdWaitEvents2">);
  FN(vkCmdWaitEvents2KHR, wait_events2<"vkCmdWaitEvents2KHR">);
  FN(vkCmdSetEvent2, set_event2<"vkCmdSetEvent2">);
  FN(vkCmdSetEvent2KHR, set_event2<"vkCmdSetEvent2KHR">);
  FN(vkCreateRenderPass, create_render_pass);
  FN(vkCreateRenderPass2, create_render_pass2<"vkCreateRenderPass2">);
  FN(vkCreateRenderPass2KHR, create_render_pass2<"vkCreateRenderPass2KHR">);
#undef FN
  return nullptr;
}
void release_wsi_device(VkDevice device) {
  std::vector<std::shared_ptr<Swapchain>> leaked;
  {
    std::lock_guard lock(mutex);
    std::erase_if(swapchains, [&](auto& entry) {
      if (entry.second->device != device) return false;
      leaked.push_back(std::move(entry.second));
      return true;
    });
  }
  for (auto& chain : leaked) destroy_resources(*chain);
}
}  // namespace venture::vulkan
