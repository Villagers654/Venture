// A decoder-side Vulkan device on the host GPU exporting one picture as a
// gfx::SharedImage, for tests of shared-image consumers. Requires doctest.
#pragma once
#include "gfx/gpu_device.h"
#include "gfx/shared_image.h"
#include <cstring>
#include <vector>
#include <doctest/doctest.h>
#include <vulkan/vulkan.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#else
#include <unistd.h>
#endif

namespace venture::test {
inline bool device_extension(venture::gfx::GpuDevice& gpu, const char* name) {
  auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
      gpu.get_instance_proc_addr(gpu.instance, "vkEnumerateDeviceExtensionProperties"));
  std::uint32_t count = 0;
  enumerate(gpu.physical_device, nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> extensions(count);
  enumerate(gpu.physical_device, nullptr, &count, extensions.data());
  for (auto& e : extensions) if (std::strcmp(e.extensionName, name) == 0) return true;
  return false;
}
// A decoder-side device on the same GPU: one exportable RGBA8 image cleared to
// a known color and released to VK_QUEUE_FAMILY_EXTERNAL, as media's Vulkan
// Video converter hands pictures over.
struct ExportedImage {
  venture::gfx::GpuDevice& gpu;
  VkDevice device{};
  VkImage image{};
  VkDeviceMemory memory{};
  venture::gfx::SharedImage shared;
  template <class F> F vk(const char* name) {
    return reinterpret_cast<F>(gpu.get_instance_proc_addr(gpu.instance, name));
  }
  ExportedImage(venture::gfx::GpuDevice& g, std::uint32_t width, std::uint32_t height, VkClearColorValue color) : gpu(g) {
    float priority = 1;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = gpu.queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    const char* extensions[]{venture::gfx::shared_image_extension};
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 1;
    device_info.ppEnabledExtensionNames = extensions;
    REQUIRE(vk<PFN_vkCreateDevice>("vkCreateDevice")(gpu.physical_device, &device_info, nullptr, &device) == VK_SUCCESS);
#ifdef _WIN32
    constexpr auto type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
    constexpr auto type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr,
                                             VkExternalMemoryHandleTypeFlags(type)};
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {width, height, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    REQUIRE(vk<PFN_vkCreateImage>("vkCreateImage")(device, &info, nullptr, &image) == VK_SUCCESS);
    VkMemoryRequirements requirements{};
    vk<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(device, image, &requirements);
    VkPhysicalDeviceMemoryProperties properties{};
    vk<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(gpu.physical_device, &properties);
    std::uint32_t memory_type = 0;
    while (!(requirements.memoryTypeBits & (1u << memory_type)) ||
           !(properties.memoryTypes[memory_type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
      ++memory_type;
    VkExportMemoryAllocateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, nullptr,
                                           VkExternalMemoryHandleTypeFlags(type)};
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &export_info, image};
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicated, requirements.size, memory_type};
    REQUIRE(vk<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &allocate, nullptr, &memory) == VK_SUCCESS);
    REQUIRE(vk<PFN_vkBindImageMemory>("vkBindImageMemory")(device, image, memory, 0) == VK_SUCCESS);
    VkQueue queue{};
    vk<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(device, gpu.queue_family, 0, &queue);
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = gpu.queue_family;
    VkCommandPool pool{};
    REQUIRE(vk<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device, &pool_info, nullptr, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
                                             VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer command{};
    REQUIRE(vk<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device, &command_info, &command) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vk<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command, &begin);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    auto pipeline_barrier = vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    pipeline_barrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                     nullptr, 1, &barrier);
    vk<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(command, image, VK_IMAGE_LAYOUT_GENERAL, &color, 1,
                                                         &barrier.subresourceRange);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = gpu.queue_family;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    pipeline_barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                     nullptr, 1, &barrier);
    vk<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    REQUIRE(vk<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
    REQUIRE(vk<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(queue) == VK_SUCCESS);
    vk<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device, pool, nullptr);
    shared.id = 1;
    shared.width = width;
    shared.height = height;
    shared.format = info.format;
    shared.usage = info.usage;
    shared.handle_type = type;
    shared.allocation_size = requirements.size;
    shared.memory_type = memory_type;
    shared.device_uuid = venture::gfx::host_gpu()->device_uuid;
#ifdef _WIN32
    VkMemoryGetWin32HandleInfoKHR get{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR, nullptr, memory, type};
    HANDLE handle{};
    REQUIRE(vk<PFN_vkGetMemoryWin32HandleKHR>("vkGetMemoryWin32HandleKHR")(device, &get, &handle) == VK_SUCCESS);
    shared.handle = reinterpret_cast<std::intptr_t>(handle);
#else
    VkMemoryGetFdInfoKHR get{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, nullptr, memory, type};
    int fd = -1;
    REQUIRE(vk<PFN_vkGetMemoryFdKHR>("vkGetMemoryFdKHR")(device, &get, &fd) == VK_SUCCESS);
    shared.handle = fd;
#endif
  }
  ~ExportedImage() {
#ifdef _WIN32
    if (shared.handle != -1) CloseHandle(reinterpret_cast<HANDLE>(shared.handle));
#else
    if (shared.handle >= 0) ::close(int(shared.handle));
#endif
    vk<PFN_vkDestroyImage>("vkDestroyImage")(device, image, nullptr);
    vk<PFN_vkFreeMemory>("vkFreeMemory")(device, memory, nullptr);
    vk<PFN_vkDestroyDevice>("vkDestroyDevice")(device, nullptr);
  }
};
}  // namespace venture::test
