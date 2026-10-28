#include "xr/dev_dump.h"
#include "android/framework_bridge.h"
#include "android/native_window.h"
#include "common/log.h"
#include "core/process.h"
#include "gfx/gpu_device.h"
#include "hle/openxr/state.h"
#include "java/jvm.h"
#include "xr/queue.h"
#include <jni.h>
#include <cstring>

namespace venture::hle::openxr {
// The window owns completed producer buffers; the XR consumer publishes those bytes
// into real runtime-owned images on the session's Vulkan device. No guest
// acquire/release operations are permitted on an Android Surface swapchain.
// https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrCreateSwapchainAndroidSurfaceKHR.html
struct AndroidSurface {
  android::NativeWindow* window = nullptr;
  jobject surface = nullptr;
  bool global_surface = false;
  XrGraphicsBindingVulkanKHR binding{};
  VkQueue queue{};
  VkCommandPool pool{};
  VkCommandBuffer command{};
  VkFence fence{};
  VkBuffer staging{};
  VkDeviceMemory memory{};
  void* mapped = nullptr;
  bool coherent = false, initialized = false;
  std::uint64_t published = 0;
  std::vector<std::size_t> source_columns;
  std::uint32_t sampled_width = 0;
  XrColorSpaceFB published_color = XR_COLOR_SPACE_REC709_FB;

  template<class F> F vk(const char* name) const {
    return reinterpret_cast<F>(gfx::gpu_device().get_instance_proc_addr(binding.instance, name));
  }
  XrResult checked(VkResult result, const char* operation) const {
    if (result == VK_SUCCESS) return XR_SUCCESS;
    log::error("xr-surface", "{} failed: VkResult={}", operation, int(result));
    return result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY
        ? XR_ERROR_OUT_OF_MEMORY : XR_ERROR_RUNTIME_FAILURE;
  }
  ~AndroidSurface() {
    if (binding.device) {
      if (mapped) vk<PFN_vkUnmapMemory>("vkUnmapMemory")(binding.device, memory);
      if (staging) vk<PFN_vkDestroyBuffer>("vkDestroyBuffer")(binding.device, staging, nullptr);
      if (memory) vk<PFN_vkFreeMemory>("vkFreeMemory")(binding.device, memory, nullptr);
      if (fence) vk<PFN_vkDestroyFence>("vkDestroyFence")(binding.device, fence, nullptr);
      if (pool) vk<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(binding.device, pool, nullptr);
    }
    if (surface && java::host_vm()) {
      auto* env = java::host_env();
      // Cleanup may run after a failed JNI allocation. Preserve that failure
      // while releasing the Surface's separately acquired native reference.
      auto pending = env->ExceptionOccurred();
      if (pending) env->ExceptionClear();
      auto cls = env->GetObjectClass(surface);
      auto release = cls ? env->GetMethodID(cls, "release", "()V") : nullptr;
      if (release) {
        env->CallVoidMethod(surface, release);
        env->ExceptionCheck();
      }
      if (cls) env->DeleteLocalRef(cls);
      if (global_surface) env->DeleteGlobalRef(surface);
      else env->DeleteLocalRef(surface);
      if (pending) {
        if (env->ExceptionCheck()) {
          log::warn("xr-surface", "Surface cleanup raised a JNI exception");
          env->ExceptionClear();
        }
        env->Throw(pending);
        env->DeleteLocalRef(pending);
      }
    }
    if (window) {
      { std::lock_guard lock(window->lock); window->image_consumer = false; window->frame.reset(); }
      android::release(window);
    }
  }
  XrResult initialize(std::uint32_t width, std::uint32_t height) {
    binding = state().graphics;
    vk<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(
        binding.device, binding.queueFamilyIndex, binding.queueIndex, &queue);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = binding.queueFamilyIndex;
    auto result = checked(vk<PFN_vkCreateCommandPool>("vkCreateCommandPool")(
        binding.device, &pci, nullptr, &pool), "vkCreateCommandPool");
    if (XR_FAILED(result)) return result;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
        pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    result = checked(vk<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(
        binding.device, &cai, &command), "vkAllocateCommandBuffers");
    if (XR_FAILED(result)) return result;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    result = checked(vk<PFN_vkCreateFence>("vkCreateFence")(
        binding.device, &fci, nullptr, &fence), "vkCreateFence");
    if (XR_FAILED(result)) return result;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = std::uint64_t(width) * height * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    result = checked(vk<PFN_vkCreateBuffer>("vkCreateBuffer")(
        binding.device, &bci, nullptr, &staging), "vkCreateBuffer");
    if (XR_FAILED(result)) return result;
    VkMemoryRequirements requirements{};
    vk<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(
        binding.device, staging, &requirements);
    VkPhysicalDeviceMemoryProperties properties{};
    vk<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(
        binding.physicalDevice, &properties);
    std::uint32_t type = properties.memoryTypeCount;
    for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
      auto flags = properties.memoryTypes[i].propertyFlags;
      if ((requirements.memoryTypeBits & (1u << i)) && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        type = i;
        if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) break;
      }
    }
    if (type == properties.memoryTypeCount) return XR_ERROR_OUT_OF_MEMORY;
    coherent = properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, type};
    result = checked(vk<PFN_vkAllocateMemory>("vkAllocateMemory")(
        binding.device, &mai, nullptr, &memory), "vkAllocateMemory");
    if (XR_FAILED(result)) return result;
    result = checked(vk<PFN_vkBindBufferMemory>("vkBindBufferMemory")(
        binding.device, staging, memory, 0), "vkBindBufferMemory");
    if (XR_FAILED(result)) return result;
    return checked(vk<PFN_vkMapMemory>("vkMapMemory")(
        binding.device, memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
  }
  XrResult present(Swapchain& chain) {
    std::shared_ptr<const android::SurfaceFrame> frame;
    {
      std::lock_guard lock(window->lock);
      frame = window->frame;
    }
    auto serial = frame ? frame->serial : 0;
    if (initialized && serial == published && published_color == state().color_space) return XR_SUCCESS;
    auto* destination = static_cast<std::uint8_t*>(mapped);
    auto width = chain.info.width, height = chain.info.height;
    if (!frame) {
      std::memset(destination, 0, std::size_t(width) * height * 4);
    } else {
      // BufferQueue producers may resize. Snapshot one completed producer
      // buffer, with the same consumer for CPU, EGL and Vulkan publication.
      // Cache horizontal sampling offsets: division for every destination
      // pixel needlessly runs on the game's submitting thread for movies.
      if(sampled_width!=std::uint32_t(frame->width) || source_columns.size()!=width) {
        sampled_width=frame->width;source_columns.resize(width);
        for(std::uint32_t x=0;x<width;++x)source_columns[x]=(std::uint64_t(x)*frame->width/width)*4;
      }
      for (std::uint32_t y = 0; y < height; ++y) {
        auto sy = std::uint64_t(y) * frame->height / height;
        auto* row=frame->rgba.data()+sy*frame->width*4;
        for (std::uint32_t x = 0; x < width; ++x) {
          std::memcpy(destination + (std::uint64_t(y) * width + x) * 4,
              row+source_columns[x], 4);
        }
      }
    }
    HostQueueLock lock(session().uses_shared_gpu);
    if (!lock) return XR_ERROR_RUNTIME_FAILURE;
    if (!coherent) {
      VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, memory, 0, VK_WHOLE_SIZE};
      auto result = checked(vk<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges")(
          binding.device, 1, &range), "vkFlushMappedMemoryRanges");
      if (XR_FAILED(result)) return result;
    }
    const std::uint32_t index = 0;
    auto result = chain.backing->wait(index);
    if (result != XR_SUCCESS) return result;
    auto publish = [&]() -> XrResult {
      auto r = checked(vk<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(command, 0), "vkResetCommandBuffer");
      if (XR_FAILED(r)) return r;
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      r = checked(vk<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command, &begin), "vkBeginCommandBuffer");
      if (XR_FAILED(r)) return r;
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = chain.images.at(index).image;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
      VkBufferImageCopy copy{};
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.imageExtent = {chain.info.width, chain.info.height, 1};
      vk<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(command, staging, barrier.image,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
      barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
      r = checked(vk<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command), "vkEndCommandBuffer");
      if (XR_FAILED(r)) return r;
      r = checked(vk<PFN_vkResetFences>("vkResetFences")(binding.device, 1, &fence), "vkResetFences");
      if (XR_FAILED(r)) return r;
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &command;
      r = checked(vk<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &submit, fence), "vkQueueSubmit");
      if (XR_FAILED(r)) return r;
      return checked(vk<PFN_vkWaitForFences>("vkWaitForFences")(
          binding.device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    }();
    if (XR_FAILED(publish)) return publish;
    chain.released_index = index;
    ++chain.release_serial;
    initialized = true;
    published = serial;
    published_color = state().color_space;
    return XR_SUCCESS;
  }
};

XrResult present_android_surfaces() {
  for (auto& [handle, chain] : state().swapchains)
    if (chain.android_surface) {
      auto result = chain.android_surface->present(chain);
      if (XR_FAILED(result)) return result;
    }
  return XR_SUCCESS;
}

namespace guest {
XrResult xrCreateSwapchainAndroidSurfaceKHR(XrSession handle, const XrSwapchainCreateInfo* info,
                                           XrSwapchain* out, void** surface) {
  std::lock_guard lock(state().mutex);
  if (out) *out = XR_NULL_HANDLE;
  if (surface) *surface = nullptr;
  if (!valid_handle(handle, "XrSession")) return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_KHR_android_surface_swapchain")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  // Android platform ABI: these five fields are required to be zero; the
  // runtime chooses the producer's image format and image count internally.
  if (!info || !out || !surface || info->type != XR_TYPE_SWAPCHAIN_CREATE_INFO ||
      info->format || info->sampleCount || info->faceCount || info->arraySize || info->mipCount ||
      !info->width || !info->height || std::uint64_t(info->width) * info->height > INT32_MAX / 4)
    return XR_ERROR_VALIDATION_FAILURE;
  constexpr auto known_flags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT | XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT;
  if (info->createFlags & ~known_flags) return XR_ERROR_VALIDATION_FAILURE;
  // Dynamic producers cannot satisfy the static-image contract, and a CPU
  // producer cannot provide a protected-content path.
  if (info->createFlags) return XR_ERROR_FEATURE_UNSUPPORTED;
  auto bridge = android::surface_bridge();
  if (!bridge.to_surface || !java::host_vm() || process_config().isa() != GuestIsa::a64)
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  auto producer = std::make_shared<AndroidSurface>();
  producer->window = android::create_native_window(info->width, info->height);
  if (!producer->window) return XR_ERROR_OUT_OF_MEMORY;
  producer->window->image_consumer = true;
  producer->window->fixed_consumer_extent = true;
  auto result = producer->initialize(info->width, info->height);
  if (XR_FAILED(result)) return result;
  auto* env = java::host_env();
  producer->surface = bridge.to_surface(nullptr, producer->window);
  if (!producer->surface) return XR_ERROR_RUNTIME_FAILURE;
  auto global = env->NewGlobalRef(producer->surface);
  if (!global) return XR_ERROR_OUT_OF_MEMORY;
  env->DeleteLocalRef(producer->surface);
  producer->surface = global;
  producer->global_surface = true;
  auto native = *info;
  // Android WINDOW_FORMAT_RGBA_8888/RGBX_8888/RGB_565 bytes use the
  // default sRGB dataspace (ANativeWindow / BufferQueue). Preserve the encoded
  // bytes and let the compositor's sRGB image view decode them when sampled.
  native.format = VK_FORMAT_R8G8B8A8_SRGB;
  native.sampleCount = native.faceCount = native.arraySize = native.mipCount = 1;
  native.usageFlags |= XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
  result = create_swapchain(handle, &native, out, true);
  if (XR_FAILED(result)) return result;
  auto& chain = state().swapchains.at(*out);
  chain.android_surface = std::move(producer);
  result = chain.android_surface->present(chain);
  if (XR_SUCCEEDED(result)) {
    *surface = env->NewLocalRef(chain.android_surface->surface);
    if (!*surface) result = XR_ERROR_OUT_OF_MEMORY;
  }
  if (XR_FAILED(result)) {
    guest::xrDestroySwapchain(*out);
    *out = XR_NULL_HANDLE;
    return result;
  }
  log::info("xr-surface", "Created Android Surface swapchain {}x{} backed by real Vulkan images",
            info->width, info->height);
  return XR_SUCCESS;
}
}
} // namespace venture::hle::openxr
