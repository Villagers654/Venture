#pragma once
// Foveated rendering on the host GPU (gfx/foveation.h has the pattern).
//
// Desktop GPUs foveate with shading-rate attachments (VK_KHR_fragment_shading_rate):
// a small R8_UINT image, one texel per 8x8 or 16x16 pixels, gives each tile's
// fragment size. Venture adds one to every render pass with colour output, on
// both GPU paths (guest Vulkan's layer chain and ANGLE's loader for GLES
// games), so pipelines and render passes stay compatible: passes into a game's
// eye images (its XR swapchains') get that swapchain's foveation map, every
// other pass a shared full-rate map, which changes nothing. Pipelines get the
// state that lets the attachment set the rate (combiner REPLACE); where a
// pipeline cannot shade coarsely (depth or sample-mask writes the GPU cannot
// combine with it, sample shading) the GPU itself keeps it at full rate.
//
// Maps are filled from the eye views the game submits each frame (xrEndFrame's
// projection layers: rectangle, array layer and field of view) and the gaze,
// and uploaded before the next submission on the queue that renders the eyes.
// Nothing changes when foveation is off: no extension, attachment or pipeline
// state is added.
#include <vulkan/vulkan.h>

#include "gfx/foveation.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace venture::gfx::foveation {
// Whether the host gives the gaze (xr/eye_gaze.h decides unless this is
// called first: tests).
void set_host_eye_tracking(bool available);
// Whether foveation is on for this process (VENTURE_FOVEATION, eye tracking).
bool wanted();
// Whether it follows the gaze.
bool eye_tracked();

// Device creation: when wanted() and the GPU has shading-rate attachments,
// adds VK_KHR_fragment_shading_rate and attachmentFragmentShadingRate to a
// copy of `in` (out.info), else out.info is `in`. `out` keeps what it added.
struct DeviceAdditions {
  VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  std::vector<const char*> extensions;
  VkPhysicalDeviceFragmentShadingRateFeaturesKHR features{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
  bool added = false;
};
void add_to_device(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkPhysicalDevice physical,
                   const VkDeviceCreateInfo& in, DeviceAdditions& out);
// After creating a device while wanted(): `active` when it was created with
// additions (out.added), so its passes are foveated from now. `raw` resolves
// the driver's own functions, which the intercepted commands call.
void register_device(VkDevice device, VkPhysicalDevice physical, PFN_vkGetInstanceProcAddr gipa,
                     VkInstance instance, PFN_vkGetDeviceProcAddr raw, bool active);
void release_device(VkDevice device);
bool device_active(VkDevice device);
// The device commands foveation intercepts (render passes, framebuffers,
// image views, beginning passes, queue submission), or nullptr. They call the
// driver's functions from register_device.
PFN_vkVoidFunction proc(std::string_view name);

// Graphics pipelines on an active device: lets the shading-rate attachment set
// the rate. `storage` keeps the added structures alive for the call.
struct PipelineStorage {
  std::vector<VkPipelineFragmentShadingRateStateCreateInfoKHR> states;
};
void patch_pipelines(VkDevice device, std::span<VkGraphicsPipelineCreateInfo> infos, PipelineStorage& storage);

// A game's eye swapchain: its images (a GLES game's are ANGLE's images of its
// textures, one per array layer: first_layer gives each image's layer).
// bottom_up: rows run from the bottom (GLES), so the pattern flips.
struct EyeImage {
  VkImage image{};
  std::uint32_t first_layer = 0;
};
void register_eye_swapchain(VkDevice device, const void* swapchain, std::span<const EyeImage> images,
                            std::uint32_t width, std::uint32_t height, std::uint32_t layers, bool bottom_up);
void forget_eye_swapchain(VkDevice device, const void* swapchain);
// One eye view the game submitted in a projection layer.
struct EyeView {
  std::uint32_t layer = 0;
  std::int32_t x = 0, y = 0;
  std::uint32_t width = 0, height = 0;
  Fov fov;
  Centre centre;
};
// The views a swapchain shows this frame, with the game's foveation request:
// refills its map when they changed (cheap otherwise).
// `gaze`: the centres follow the gaze this frame (eye-tracked regions); false
// keeps the fixed regions.
void update_eye_views(const void* swapchain, std::span<const EyeView> views, Level requested,
                      float vertical_offset_degrees, bool gaze);
// The left and right views of the frame's first projection layer: they shape
// the game's own stereo targets (scene passes into images that are not eye
// images), which cover each eye's whole view.
void update_scene_views(const EyeView& left, const EyeView& right, Level requested, float vertical_offset_degrees,
                        bool gaze);
}  // namespace venture::gfx::foveation
