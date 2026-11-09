#pragma once
// Experimental DLSS Super Resolution for scaled projection chains
// (VENTURE_EXPERIMENTAL_RENDER_SCALE + VENTURE_EXPERIMENTAL_DLSS=1). NGX runs
// through venture_ngx.dll (tools/ngx-bridge) beside the runtime. Windows only;
// elsewhere requested() is false and nothing loads.
#include "xr/api.h"
#include <memory>
#include <span>
#include <vector>

namespace venture::xr::dlss {
// VENTURE_EXPERIMENTAL_RENDER_SCALE in [0.25, 1); 1 when unset or invalid.
double render_scale();
// The opt-in is set and a render scale below 1 is active.
bool requested();
// Extensions NGX needs, for create_instance_impl / create_device_impl. Empty
// when DLSS is not requested or the bridge does not load.
std::vector<const char*> required_extensions(bool device);

// One projection view as the guest rendered it: pose and unjittered field of
// view in the layer's space, and the jitter in render pixels.
struct ViewSample {
  XrPosef pose{};
  XrFovf fov{};
  float jitter_x = 0, jitter_y = 0;
  bool valid = false;
  // The view's subimage rect in the guest image (OpenXR rows, top first);
  // empty for the whole image.
  XrRect2Di rect{};
};
// The render size offered to the guest (scaled recommendation).
void note_render_size(std::uint32_t width, std::uint32_t height);
// xrLocateViews: shifts each stereo view's field of view by this frame's
// subpixel jitter (Halton 2,3), the same for every call with display_time.
// VENTURE_DLSS_JITTER=0 disables it.
void jitter_views(XrTime display_time, std::uint32_t count, XrView* views);
// A field of view jitter_views returned: restores it and gives the jitter.
// False (unchanged) for any other field of view.
bool unjitter(XrFovf& fov, float& jitter_x, float& jitter_y);

// DLSS for one guest chain: each array layer (eye) has its own feature and
// history. Owned by GuestImages; destroyed only once its queue is idle.
class Upscaler {
 public:
  static std::unique_ptr<Upscaler> create(const XrGraphicsBindingVulkanKHR& binding, PFN_vkGetInstanceProcAddr get,
                                          const XrSwapchainCreateInfo& guest, std::span<const VkImage> slots);
  ~Upscaler();
  // Records the upscale of slot `index` into `target` (extent, one level, all
  // layers). Source and target end in `layout`; the target starts undefined.
  // False, with nothing recorded, when DLSS cannot run (the caller blits);
  // after a failure every later call is false.
  // `views` holds this frame's sample per layer (may be empty or invalid:
  // motion vectors are then zero).
  // side_by_side: `target` has one layer and layer i goes to x = i * extent.width
  // (`extent` is one layer's size).
  bool record(VkCommandBuffer command, std::uint32_t index, VkImage target, VkFormat target_format,
              VkExtent2D extent, bool flip_rows, VkImageLayout layout, std::span<const ViewSample> views,
              bool side_by_side = false);

 private:
  struct Impl;
  explicit Upscaler(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
};
}  // namespace venture::xr::dlss
