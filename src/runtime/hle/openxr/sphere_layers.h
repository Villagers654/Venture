#pragma once
#include "xr/api.h"
#include <array>
#include <deque>
#include <vector>

namespace venture::hle::openxr {
// Equirect, equirect2, cylinder and cube layers on a host without their
// extension: each run of consecutive ones is drawn into one projection layer at
// its place in the frame, at its true geometry (xr/sphere_images.h). A Quest
// composes them all (360 backdrops, loading videos, curved UI panels), so the
// extensions are offered everywhere. Serialized by the guest OpenXR state mutex.
bool sphere_layer_type(XrStructureType);
class SphereLayers {
 public:
  // Takes one layer of a sphere type; a run's first layer appends its
  // projection placeholder to `layers`.
  XrResult add(const XrCompositionLayerBaseHeader*, std::vector<const XrCompositionLayerBaseHeader*>& layers);
  // After the guest chains are prepared: draws every run into its host chain
  // and completes its placeholder. A run that cannot be drawn this frame
  // (views not located) is removed from `layers`.
  XrResult present(XrSession, XrTime, std::vector<const XrCompositionLayerBaseHeader*>& layers);
  bool empty() const { return runs_.empty(); }

  struct Sphere {
    XrStructureType type;
    XrCompositionLayerFlags flags;
    XrSpace space;
    XrEyeVisibility eye;
    XrSwapchainSubImage sub;               // a cube: its swapchain and array index
    XrPosef pose;
    float radius;                          // not a cube
    XrVector2f scale, bias;                // equirect
    float central, upper, lower;           // equirect2; a cylinder: central angle, aspect ratio
    XrColor4f color_scale{1, 1, 1, 1}, color_bias{0, 0, 0, 0};  // XR_KHR_composition_layer_color_scale_bias
    bool flip = false;  // XR_FB_composition_layer_image_layout VERTICAL_FLIP
  };
 private:
  struct Run {
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::array<XrCompositionLayerProjectionView, 2> views{};
    std::vector<Sphere> spheres;
  };
  std::deque<Run> runs_;
};
// A guest chain is destroyed: its copies go, and a host chain left drawing nothing.
XrResult forget_sphere_source(XrSwapchain);
// At session destruction: the host chains and private source copies.
XrResult destroy_sphere_layers();
} // namespace venture::hle::openxr
