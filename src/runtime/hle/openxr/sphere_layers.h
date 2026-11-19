#pragma once
#include "xr/api.h"
#include <array>
#include <deque>
#include <optional>
#include <vector>

namespace venture::hle::openxr {
// Equirect, equirect2, cylinder and cube layers on a host without their
// extension: each run of consecutive ones is drawn into one projection layer at
// its place in the frame, at its true geometry (xr/sphere_images.h). A Quest
// composes them all (360 backdrops, loading videos, curved UI panels), so the
// extensions are offered everywhere. Serialized by the guest OpenXR state mutex.
//
// Underlays: layers under a projection layer that blends by source alpha are
// meant to show through its alpha (a game's menu panels under the eye image,
// which leaves holes where they show). Hosts differ there: SteamVR's OpenVR
// compositor makes OpenXR quads overlays (openxr_quadlayer_*), drawn over its
// scene, and the first projection that scene, so nothing can be under it.
// Such layers and the projection over them are drawn as one run, the
// projection last with its blending, so every host gets the composed image.
bool sphere_layer_type(XrStructureType);
class SphereLayers {
 public:
  // Takes one layer of a sphere type, or a quad under a projection; a run's
  // first layer appends its projection placeholder to `layers`.
  XrResult add(const XrCompositionLayerBaseHeader*, std::vector<const XrCompositionLayerBaseHeader*>& layers);
  // Whether `add` and `add_projection` can draw this layer: a quad or sphere
  // type, or a projection, whose swapchains can be sampled.
  static bool drawable(const XrCompositionLayerBaseHeader*);
  // Takes the projection layer over the layers just added, as the run's top.
  XrResult add_projection(const XrCompositionLayerProjection&,
                          std::vector<const XrCompositionLayerBaseHeader*>& layers);
  // After the guest chains are prepared: draws every run into its host chain
  // and completes its placeholder. A run that cannot be drawn this frame
  // (views not located) is removed from `layers`. A run whose sources,
  // placements and views are all as last drawn is not drawn again: its
  // placeholder names the image the host already has. `mirror_eye`: the
  // desktop mirror takes this eye from a drawn image when a run is the frame's
  // first projection layer (the mirror shows the first one).
  XrResult present(XrSession, XrTime, std::vector<const XrCompositionLayerBaseHeader*>& layers,
                   std::optional<std::uint32_t> mirror_eye = std::nullopt);
  bool empty() const { return runs_.empty(); }

  struct Sphere {
    XrStructureType type;
    XrCompositionLayerFlags flags;
    XrSpace space;
    XrEyeVisibility eye;
    XrSwapchainSubImage sub;               // a cube: its swapchain and array index
    XrPosef pose;                          // a projection view: the view's pose
    XrFovf fov;                            // a projection view
    XrExtent2Df size;                      // a quad
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
    // A run under a projection (add_projection): its space and views, used
    // when the host's views cannot be located.
    bool topped = false;
    XrSpace top_space{};
    std::array<XrView, 2> top_views{};
  };
  std::deque<Run> runs_;
};
// A guest chain is destroyed: its copies go, and a host chain left drawing nothing.
XrResult forget_sphere_source(XrSwapchain);
// At session destruction: the host chains and private source copies.
XrResult destroy_sphere_layers();
} // namespace venture::hle::openxr
