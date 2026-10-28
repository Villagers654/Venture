#include <doctest/doctest.h>
#include "core/process.h"
#include "gfx/gpu_device.h"
#include "gfx/queue_sync.h"
#include "hle/vulkan/vulkan.h"
#include "vulkan_test_support.h"
#include <cstdint>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>

namespace {
VkPhysicalDeviceProperties host_properties{};
void VKAPI_CALL properties(VkPhysicalDevice, VkPhysicalDeviceProperties* value) { *value = host_properties; }
void VKAPI_CALL properties2(VkPhysicalDevice, VkPhysicalDeviceProperties2* value) {
  value->properties = host_properties;
  for (auto* next = static_cast<VkBaseOutStructure*>(value->pNext); next; next = next->pNext)
    if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES)
      reinterpret_cast<VkPhysicalDeviceDriverProperties*>(next)->driverID = VK_DRIVER_ID_GOOGLE_SWIFTSHADER;
}
void VKAPI_CALL formats(VkPhysicalDevice, VkFormat, VkFormatProperties* value) {
  *value = {};
  value->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
}
// SwiftShader's color sample counts; a created image records its sample count.
int created_images = 0;
std::atomic<VkResult> quit_idle_result{VK_SUCCESS};
std::atomic<unsigned> quit_idle_calls{0}, quit_destroy_calls{0};
VkResult VKAPI_CALL quit_idle(VkDevice) {
  ++quit_idle_calls;
  return quit_idle_result.load();
}
void VKAPI_CALL quit_destroy(VkDevice, const VkAllocationCallbacks*) { ++quit_destroy_calls; }
VkResult VKAPI_CALL image_formats(VkPhysicalDevice, VkFormat, VkImageType, VkImageTiling, VkImageUsageFlags,
                                  VkImageCreateFlags, VkImageFormatProperties* value) {
  *value = {};
  value->maxExtent = {4096, 4096, 1};
  value->maxMipLevels = 13;
  value->maxArrayLayers = 2048;
  value->sampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
  value->maxResourceSize = VkDeviceSize{1} << 31;
  return VK_SUCCESS;
}
VkResult VKAPI_CALL create_image(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage* out) {
  ++created_images;
  *out = reinterpret_cast<VkImage>(std::uintptr_t{0x2000});
  return VK_SUCCESS;
}
PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice, const char* name) {
  if (std::strcmp(name, "vkDeviceWaitIdle") == 0) return reinterpret_cast<PFN_vkVoidFunction>(quit_idle);
  if (std::strcmp(name, "vkDestroyDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(quit_destroy);
  if (std::strcmp(name, "vkCreateImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(create_image);
  return nullptr;
}

TEST_CASE("OS quit idles a guest-owned Vulkan device without destroying its live children") {
  using namespace venture;
  auto physical = reinterpret_cast<VkPhysicalDevice>(std::uintptr_t{0x7100});
  auto device = reinterpret_cast<VkDevice>(std::uintptr_t{0x7200});
  auto queue = reinterpret_cast<VkQueue>(std::uintptr_t{0x7300});
  VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  vulkan::adopt_device(physical, device, info);
  gfx::register_queue(device, queue);
  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_R8G8B8A8_SRGB;
  image_info.extent = {4, 4, 1};
  image_info.mipLevels = image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  VkImage image{};
  REQUIRE(test::vk_call("vkCreateImage", device, &image_info, nullptr, &image) == VK_SUCCESS);
  REQUIRE(image);
  quit_idle_calls = quit_destroy_calls = 0;
  SUBCASE("success with a live image") {
    quit_idle_result = VK_SUCCESS;
    CHECK(vulkan::wait_devices_for_os_quit(std::chrono::seconds(20)));
    CHECK(quit_idle_calls > 0);
  }
  SUBCASE("device loss remains a recorded terminal device state") {
    quit_idle_result = VK_ERROR_DEVICE_LOST;
    CHECK(vulkan::wait_devices_for_os_quit(std::chrono::seconds(20)));
    CHECK(quit_idle_calls > 0);
  }
  SUBCASE("queue-lock acquisition is included in the bound") {
    quit_idle_result = VK_SUCCESS;
    auto mutex = gfx::queue_mutex(queue);
    std::unique_lock held(*mutex);
    CHECK_FALSE(vulkan::wait_devices_for_os_quit(std::chrono::milliseconds(30)));
    const auto calls = quit_idle_calls.load();
    held.unlock();
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (quit_idle_calls.load() == calls && std::chrono::steady_clock::now() < end)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(quit_idle_calls.load() > calls);
    // Join the queue's critical section before this fixture returns. The
    // production timeout path instead ends its worker with a nonzero status.
    std::lock_guard finished(*mutex);
  }
  CHECK(quit_destroy_calls == 0);
}
PFN_vkVoidFunction VKAPI_CALL proc(VkInstance, const char* name) {
  if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(device_proc);
  if (std::strcmp(name, "vkGetPhysicalDeviceImageFormatProperties") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(image_formats);
  if (std::strcmp(name, "vkGetPhysicalDeviceProperties") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(properties);
  if (std::strcmp(name, "vkGetPhysicalDeviceProperties2") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(properties2);
  if (std::strcmp(name, "vkGetPhysicalDeviceFormatProperties") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(formats);
  return nullptr;
}
}

// This executable replaces only the static-library host device accessor, so
// the device type and API version are deterministic on any machine.
namespace venture::gfx {
GpuDevice& gpu_device() {
  static GpuDevice gpu;
  gpu.get_instance_proc_addr = proc;
  gpu.api_version = VK_API_VERSION_1_3;
  return gpu;
}
}

TEST_CASE("Guest Vulkan sees a CPU host device as an integrated GPU and Quest's uniform buffer limits") {
  // SwiftShader's identity; the emulated Quest's Adreno is an integrated GPU.
  host_properties = {};
  host_properties.apiVersion = VK_API_VERSION_1_3;
  host_properties.vendorID = 0x1ae0;
  host_properties.deviceID = 0xc0de;
  std::strcpy(host_properties.deviceName, "SwiftShader Device (Subzero)");
  host_properties.limits.framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
  // RADV's uniform limits; guests see Adreno's 64 KiB at 64-byte offsets.
  host_properties.limits.maxUniformBufferRange = 0xffffffffu;
  host_properties.limits.minUniformBufferOffsetAlignment = 4;
  host_properties.limits.maxStorageBufferRange = 0xffffffffu;
  auto physical = reinterpret_cast<VkPhysicalDevice>(std::uintptr_t{0x1000});
  struct Case {
    VkPhysicalDeviceType host, guest;
  };
  for (auto [host, guest] : {Case{VK_PHYSICAL_DEVICE_TYPE_CPU, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU},
                             Case{VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU},
                             Case{VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU}}) {
    CAPTURE(host);
    host_properties.deviceType = host;
    auto expected = host_properties;
    expected.deviceType = guest;
    expected.limits.maxUniformBufferRange = 65536;
    expected.limits.minUniformBufferOffsetAlignment = 64;
    VkPhysicalDeviceProperties seen{};
    venture::test::vk_call("vkGetPhysicalDeviceProperties", physical, &seen);
    CHECK(seen.deviceType == guest);
    CHECK(std::memcmp(&seen, &expected, sizeof seen) == 0);
    for (const char* name : {"vkGetPhysicalDeviceProperties2", "vkGetPhysicalDeviceProperties2KHR"}) {
      CAPTURE(name);
      VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
      VkPhysicalDeviceProperties2 seen2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
      venture::test::vk_call(name, physical, &seen2);
      CHECK(seen2.properties.deviceType == guest);
      CHECK(std::memcmp(&seen2.properties, &expected, sizeof expected) == 0);
      CHECK(driver.driverID == VK_DRIVER_ID_GOOGLE_SWIFTSHADER);
    }
  }
  // The Android feature query reads the host type and is unaffected.
  host_properties.deviceType = VK_PHYSICAL_DEVICE_TYPE_CPU;
  CHECK(venture::vulkan::android_hardware_version() == VK_MAKE_API_VERSION(0, 1, 0, 3));
}

TEST_CASE("Guest images with a sample count the host lacks fail validation before reaching the host") {
  // RE4 (UE4) creates 2x MSAA targets; SwiftShader rasterizes 1x and 4x only
  // and divides by a zero sample count at draw time if given such an image.
  auto physical = reinterpret_cast<VkPhysicalDevice>(std::uintptr_t{0x1000});
  auto device = reinterpret_cast<VkDevice>(std::uintptr_t{0x3000});
  VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  venture::vulkan::adopt_device(physical, device, device_info);
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = VK_FORMAT_R8G8B8A8_SRGB;
  info.extent = {1024, 1024, 1};
  info.mipLevels = info.arrayLayers = 1;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
  VkImage image{};
  created_images = 0;
  info.samples = VK_SAMPLE_COUNT_2_BIT;
  CHECK(static_cast<VkResult>(venture::test::vk_call("vkCreateImage", device, &info, nullptr, &image)) ==
        VK_ERROR_VALIDATION_FAILED);
  CHECK(created_images == 0);
  for (auto samples : {VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_4_BIT}) {
    CAPTURE(samples);
    info.samples = samples;
    CHECK(static_cast<VkResult>(venture::test::vk_call("vkCreateImage", device, &info, nullptr, &image)) == VK_SUCCESS);
  }
  CHECK(created_images == 2);
}
