// The process-wide host GPU device shared by every graphics path.
//
// Venture uses Vulkan as its only host graphics API (Windows and Linux, any
// vendor). One VkInstance/VkPhysicalDevice/VkDevice and one graphics queue are
// shared by:
//   * guest GLES/EGL, implemented by ANGLE's Vulkan backend running on this
//     device (ANGLE is patched so its instance/device creation goes through
//     Venture callbacks),
//   * guest Vulkan (forwarded to this device's instance/physical device; the
//     guest's own VkDevice is created on the same physical device),
//   * the host OpenXR session (XR_KHR_vulkan_enable2), which is why the device
//     is created through xrCreateVulkanInstanceKHR / xrCreateVulkanDeviceKHR
//     using the physical device chosen by xrGetVulkanGraphicsDevice2KHR.
// When no XR runtime is available (flat/debug mode) the device is created
// directly with the Vulkan loader.
#pragma once

#include <cstdint>
#include <mutex>

// Vulkan handles are declared opaquely here so this header does not force
// vulkan.h on every includer.
typedef struct VkInstance_T* VkInstance;
typedef struct VkPhysicalDevice_T* VkPhysicalDevice;
typedef struct VkDevice_T* VkDevice;
typedef struct VkQueue_T* VkQueue;
typedef void (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(VkInstance, const char*);

namespace venture::gfx {

struct GpuDevice {
  VkInstance instance = nullptr;
  VkPhysicalDevice physical_device = nullptr;
  VkDevice device = nullptr;
  VkQueue queue = nullptr;
  std::uint32_t queue_family = 0;
  std::uint32_t queue_index = 0;
  std::uint32_t api_version = 0;
  PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;  // host loader entry

  // External synchronization for this queue shared with ANGLE and XR. Guest
  // Vulkan holds it only when submitting to this exact queue. Other queues use
  // queue_sync.h's per-queue mutex; never serialize independent native submits
  // with this mutex, since one may wait for another queue to progress.
  std::recursive_mutex queue_lock;
};

// Creates the device on first use (after host_xr() is initialized when XR is
// available). Thread-safe; returns the same object afterwards.
GpuDevice& gpu_device();
bool shutdown_for_os_quit();

}  // namespace venture::gfx
