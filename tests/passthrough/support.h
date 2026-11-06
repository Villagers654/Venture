#pragma once
#include "hle/openxr/state.h"
#include "common/json.h"
#include <array>
#include <functional>
#include <vector>

// Vulkan is loaded at run time (no import library), like the runtime itself.
#define VENTURE_PASSTHROUGH_VULKAN(X) X(vkCreateInstance) X(vkEnumeratePhysicalDevices) \
  X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice) \
  X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
  X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkCmdPipelineBarrier) \
  X(vkCmdClearColorImage) X(vkCmdCopyImageToBuffer) X(vkCreateBuffer) X(vkDestroyBuffer) \
  X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) \
  X(vkMapMemory) X(vkUnmapMemory) X(vkCmdCopyBufferToImage)
#define VENTURE_PASSTHROUGH_DECLARE(name) extern PFN_##name name;
extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
VENTURE_PASSTHROUGH_VULKAN(VENTURE_PASSTHROUGH_DECLARE)
#undef VENTURE_PASSTHROUGH_DECLARE

namespace venture::test::passthrough {
inline bool configured = true, grant = false, requested = false, withdrawal_complete = true;
// The scripted host: XR_FB_passthrough system support, environment blend modes
// beyond OPAQUE, XR_HTC_passthrough objects and host VIEW spaces.
inline bool host_fb_supported = true, view_spaces = false;
inline std::vector<XrEnvironmentBlendMode> blend_modes;
inline unsigned htc_creates = 0, htc_destroys = 0, spaces = 0;
// The scripted OpenVR camera (xr/tracked_camera.h).
inline bool camera_present = false, camera_streams = true, camera_stalled = false;
inline unsigned camera_opens = 0;
// The latest passthrough.json content (hle/openxr/passthrough_report.h).
inline venture::Json last_report;
inline std::uint32_t maximum_width = 0, maximum_height = 0;
inline unsigned requests = 0, native_creates = 0, native_destroys = 0, host_acquires = 0;
inline XrResult native_result = XR_SUCCESS;
inline XrSession target = reinterpret_cast<XrSession>(0x100);
inline XrSpace space = reinterpret_cast<XrSpace>(0x200);
struct Fixture {
  XrPassthroughFB feature{};
  XrPassthroughLayerFB layer{};
  Fixture();
  ~Fixture();
  void create(bool native = false);
  XrSwapchain chain(std::uint32_t layers = 2, VkFormat format = VK_FORMAT_R8G8B8A8_UNORM, std::uint32_t samples = 1);
};
void commands(const std::function<void(VkCommandBuffer)>&);
void fill(XrSwapchain, std::array<std::array<float, 4>, 2>);
// Each of a 4x4 RGBA8 image's four rows in one color (raw bytes), in every layer.
void fill_rows(XrSwapchain, std::array<std::array<unsigned char, 4>, 4>);
std::array<unsigned char, 4> pixel(XrSwapchain, std::uint32_t layer = 0, std::uint32_t row = 0);
// UNORM clears and blends of .5 round to 127 or 128 depending on the driver.
inline bool near(std::array<unsigned char, 4> value, std::array<unsigned char, 4> expected) {
  for (unsigned i = 0; i < 4; ++i)
    if (value[i] + 1 < expected[i] || value[i] > expected[i] + 1) return false;
  return true;
}
} // namespace venture::test::passthrough
