#include <doctest/doctest.h>
#include "core/process.h"
#include "gfx/gpu_device.h"
#include "hle/vulkan/vulkan.h"
#include <cstring>

namespace {
unsigned host_queries;
void VKAPI_CALL properties(VkPhysicalDevice, VkPhysicalDeviceProperties* value) {
  *value = {};
  value->deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
  value->apiVersion = VK_API_VERSION_1_3;
}
void VKAPI_CALL formats(VkPhysicalDevice, VkFormat, VkFormatProperties* value) {
  *value = {};
  value->optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
}
PFN_vkVoidFunction VKAPI_CALL proc(VkInstance, const char* name) {
  if (std::strcmp(name, "vkGetPhysicalDeviceProperties") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(properties);
  if (std::strcmp(name, "vkGetPhysicalDeviceFormatProperties") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(formats);
  return nullptr;
}
}

// This executable replaces only the static-library host device accessor. A
// capable simulated GPU makes the regression deterministic on CPU machines,
// and querying Android features must not even initialize that host for A32.
namespace venture::gfx {
GpuDevice& gpu_device() {
  ++host_queries;
  static GpuDevice gpu;
  gpu.get_instance_proc_addr = proc;
  return gpu;
}
}

TEST_CASE("A32 Android Vulkan discovery does not advertise the A64-only marshaler") {
  REQUIRE(venture::process_config().isa() == venture::GuestIsa::a32);
  CHECK(venture::vulkan::android_hardware_version() == 0);
  CHECK(host_queries == 0);
}
