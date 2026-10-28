#pragma once
#include <span>
#include <string>
#include <vector>

#include "gfx/gpu_device.h"
namespace venture::gfx {
// Before either graphics API initializes. Unsupported required extensions cause
// Vulkan creation failure, never silent removal. Late additions are rejected.
bool request_gpu_extensions(std::span<const std::string> instance_extensions,
                            std::span<const std::string> device_extensions);
const std::vector<std::string>& enabled_gpu_instance_extensions();
const std::vector<std::string>& enabled_gpu_device_extensions();
bool gpu_unified_image_layouts_enabled();
// Internal initialization state; does not recurse into ANGLE.
GpuDevice& gpu_device_state();
// Flat stock ANGLE path: borrow the exact device and queue. Never used with XR.
bool borrow_angle_device(VkInstance, VkPhysicalDevice, VkDevice, VkQueue,
                         std::uint32_t family, PFN_vkGetInstanceProcAddr,
                         std::uint32_t api_version,
                         const char* const* instance_extensions,
                         const char* const* device_extensions);
void discard_angle_device();
}  // namespace venture::gfx
