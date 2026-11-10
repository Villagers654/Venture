#pragma once
// Venture NR (VENTURE_NR=1): NVIDIA DLSS Neural Rendering on a foveated region of each eye, as the guest
// image is copied to the host chain, with any OpenXR runtime (SteamVR, Virtual Desktop...). The region
// follows the foveal center VRS uses (the eye gaze, or the stereo-aligned point; hle/vulkan/render_area.cpp),
// at native size or above (VENTURE_RENDER_SCALE). NGX runs through venture_ngx.dll (tools/ngx-bridge) beside
// the runtime and nvngx_dlssnr.dll in VENTURE_DLSS_PATH; Windows and NVIDIA RTX only.
//
// Its settings are read live from the per-user key SOFTWARE/Venture/NR: On, Width and Height (percent of each
// eye), Preset (0 NVIDIA's, 1 model A, 2 model B), Style (0 Neutral, 1 Natural, 2 Cinematic), Intensity, Tone,
// Structure and Skin (percent), Debug (green outline), WorkingScale (percent: NR at this size, its correction
// applied full size), Cache and Interval (NR every N frames, its last correction between), Transition (percent:
// the region's edge fades into the game's image).
#include "xr/api.h"
#include "xr/ngx_bridge.h"
#include <memory>
#include <vector>

namespace venture::xr::venture_nr {
// VENTURE_NR=1 (xr/ngx_bridge.h).
using ngx_bridge::requested;

class NeuralRenderer {
 public:
  // For a two-layer (stereo) single-sample colour chain; null otherwise or when NGX is unavailable.
  static std::unique_ptr<NeuralRenderer> create(const XrGraphicsBindingVulkanKHR& binding, PFN_vkGetInstanceProcAddr get,
                                                const XrSwapchainCreateInfo& guest);
  ~NeuralRenderer();
  // After the plain copy of `source` (both layers) into `target`, both in `layout`: replaces each eye's region in
  // `target` with NR's. False, with nothing recorded, when NR is off or cannot run.
  bool record(VkCommandBuffer command, VkImage source, VkImage target, VkFormat target_format, VkExtent2D extent,
              bool flip_rows, VkImageLayout layout);

 private:
  struct Impl;
  explicit NeuralRenderer(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
};
}  // namespace venture::xr::venture_nr
