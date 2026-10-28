#pragma once
#include <doctest/doctest.h>

#include <array>
#include <vector>

#include "core/thread.h"
#include "fake_cpu.h"
#include "hle/vulkan/internal.h"
#include "hle/vulkan/vulkan.h"

namespace venture::test {
template <class T>
std::uint64_t vk_word(T value) {
  if constexpr (std::is_null_pointer_v<T>)
    return 0;
  else if constexpr (std::is_pointer_v<T>)
    return reinterpret_cast<std::uint64_t>(value);
  else
    return static_cast<std::uint64_t>(value);
}
template <class... A>
std::uint64_t vk_call(const char* name, A... args) {
  std::array<std::uint64_t, sizeof...(A)> words{vk_word(args)...};
  auto handler = vulkan::command_handler(name);
  REQUIRE(handler);
  auto thunk = hle::make_thunk("vulkan-test", name, handler);
  return GuestThread::adopt_current().call(thunk, {words, {}}).x0;
}
struct VulkanDevice {
  VkInstance instance{};
  VkPhysicalDevice physical{};
  VkDevice device{};
  VkQueue queue{};
  VkCommandPool pool{};
  VkCommandBuffer command{};
  std::uint32_t family = 0;
  explicit VulkanDevice(bool synchronization2 = false, bool window_surface = false, bool timeline_semaphore = false,
                        bool maintenance4 = false, std::uint32_t api_version = 0) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = api_version ? api_version :
        (synchronization2 || maintenance4 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2);
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    const char* instance_extensions[]{"VK_KHR_surface", "VK_KHR_android_surface"};
    if (window_surface) {
      ci.enabledExtensionCount = std::size(instance_extensions);
      ci.ppEnabledExtensionNames = instance_extensions;
    }
    REQUIRE(vk_call("vkCreateInstance", &ci, nullptr, &instance) == VK_SUCCESS);
    std::uint32_t count = 1;
    REQUIRE(vk_call("vkEnumeratePhysicalDevices", instance, &count, &physical) == VK_SUCCESS);
    REQUIRE(count == 1);
    vk_call("vkGetPhysicalDeviceQueueFamilyProperties", physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vk_call("vkGetPhysicalDeviceQueueFamilyProperties", physical, &count, families.data());
    while (family < count && !(families[family].queueFlags &
        (window_surface ? VK_QUEUE_GRAPHICS_BIT : VK_QUEUE_COMPUTE_BIT))) ++family;
    REQUIRE(family < count);
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkPhysicalDeviceSynchronization2Features sync{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
    sync.synchronization2 = VK_TRUE;
    if (synchronization2) di.pNext = &sync;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    if (timeline_semaphore) {
      timeline.timelineSemaphore = VK_TRUE;
      timeline.pNext = const_cast<void*>(di.pNext);
      di.pNext = &timeline;
    }
    VkPhysicalDeviceMaintenance4Features maintenance{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES};
    if (maintenance4) {
      maintenance.maintenance4 = VK_TRUE;
      maintenance.pNext = const_cast<void*>(di.pNext);
      di.pNext = &maintenance;
    }
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    std::vector<const char*> device_extensions;
    if (window_surface) device_extensions.push_back("VK_KHR_swapchain");
    if (timeline_semaphore && app.apiVersion < VK_API_VERSION_1_2)
      device_extensions.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    if (synchronization2 && app.apiVersion < VK_API_VERSION_1_3)
      device_extensions.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    if (maintenance4 && app.apiVersion < VK_API_VERSION_1_3)
      device_extensions.push_back(VK_KHR_MAINTENANCE_4_EXTENSION_NAME);
    di.enabledExtensionCount = static_cast<std::uint32_t>(device_extensions.size());
    di.ppEnabledExtensionNames = device_extensions.data();
    REQUIRE(vk_call("vkCreateDevice", physical, &di, nullptr, &device) == VK_SUCCESS);
    vk_call("vkGetDeviceQueue", device, family, 0u, &queue);
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = family;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    REQUIRE(vk_call("vkCreateCommandPool", device, &pi, nullptr, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    REQUIRE(vk_call("vkAllocateCommandBuffers", device, &ai, &command) == VK_SUCCESS);
  }
  ~VulkanDevice() {
    vk_call("vkDeviceWaitIdle", device);
    vk_call("vkDestroyCommandPool", device, pool, nullptr);
    vk_call("vkDestroyDevice", device, nullptr);
    vk_call("vkDestroyInstance", instance, nullptr);
  }
  std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties props{};
    vk_call("vkGetPhysicalDeviceMemoryProperties", physical, &props);
    for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
    throw std::runtime_error("No suitable memory type for this Vulkan test.");
  }
  void begin() {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    REQUIRE(vk_call("vkBeginCommandBuffer", command, &bi) == VK_SUCCESS);
  }
  void submit() {
    REQUIRE(vk_call("vkEndCommandBuffer", command) == VK_SUCCESS);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &command;
    REQUIRE(vk_call("vkQueueSubmit", queue, 1u, &si, VK_NULL_HANDLE) == VK_SUCCESS);
    REQUIRE(vk_call("vkQueueWaitIdle", queue) == VK_SUCCESS);
  }
  void barrier(VkPipelineStageFlags source_stage, VkAccessFlags source, VkPipelineStageFlags dest_stage,
               VkAccessFlags dest) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = source;
    b.dstAccessMask = dest;
    vk_call("vkCmdPipelineBarrier", command, source_stage, dest_stage, 0u, 1u, &b, 0u, nullptr, 0u, nullptr);
  }
};
struct VulkanBuffer {
  VulkanDevice& gpu;
  VkBuffer buffer{};
  VkDeviceMemory memory{};
  VkDeviceSize size{};
  void* mapped{};
  VulkanBuffer(VulkanDevice& gpu, VkDeviceSize size) : gpu(gpu), size(size) {
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    REQUIRE(vk_call("vkCreateBuffer", gpu.device, &ci, nullptr, &buffer) == VK_SUCCESS);
    VkMemoryRequirements req{};
    vk_call("vkGetBufferMemoryRequirements", gpu.device, buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = gpu.memory_type(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    REQUIRE(vk_call("vkAllocateMemory", gpu.device, &ai, nullptr, &memory) == VK_SUCCESS);
    REQUIRE(vk_call("vkBindBufferMemory", gpu.device, buffer, memory, 0ull) == VK_SUCCESS);
    REQUIRE(vk_call("vkMapMemory", gpu.device, memory, 0ull, size, 0u, &mapped) == VK_SUCCESS);
  }
  ~VulkanBuffer() {
    vk_call("vkUnmapMemory", gpu.device, memory);
    vk_call("vkDestroyBuffer", gpu.device, buffer, nullptr);
    vk_call("vkFreeMemory", gpu.device, memory, nullptr);
  }
};
struct VulkanImage {
  VulkanDevice& gpu;
  VkImage image{};
  VkDeviceMemory memory{};
  std::uint32_t levels{}, layers{};
  VulkanImage(VulkanDevice& gpu, VkFormat format, std::uint32_t width, std::uint32_t height,
              std::uint32_t levels = 1, std::uint32_t layers = 1, VkImageCreateFlags flags = 0,
              const void* next = nullptr, std::uint32_t depth = 1, VkImageUsageFlags extra_usage = 0,
              VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT)
      : gpu(gpu), levels(levels), layers(layers) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.pNext = next;
    ci.flags = flags;
    ci.imageType = depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {width, height, depth};
    ci.mipLevels = levels;
    ci.arrayLayers = layers;
    ci.samples = samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               extra_usage;
    REQUIRE(vk_call("vkCreateImage", gpu.device, &ci, nullptr, &image) == VK_SUCCESS);
    VkMemoryRequirements req{};
    vk_call("vkGetImageMemoryRequirements", gpu.device, image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = gpu.memory_type(req.memoryTypeBits, 0);
    REQUIRE(vk_call("vkAllocateMemory", gpu.device, &ai, nullptr, &memory) == VK_SUCCESS);
    REQUIRE(vk_call("vkBindImageMemory", gpu.device, image, memory, 0ull) == VK_SUCCESS);
  }
  ~VulkanImage() {
    vk_call("vkDestroyImage", gpu.device, image, nullptr);
    vk_call("vkFreeMemory", gpu.device, memory, nullptr);
  }
  void transition() {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
    vk_call("vkCmdPipelineBarrier", gpu.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &b);
  }
};
}  // namespace venture::test
