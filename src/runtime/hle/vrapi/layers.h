#pragma once
#include "hle/vrapi/drawn_layers.h"
#include "hle/vrapi/state.h"
#include <functional>

namespace venture::hle::vrapi {
struct LayerStorage {
  XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                            {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  XrCompositionLayerCylinderKHR cylinder[2]{{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR},
                                            {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR}};
  XrCompositionLayerCubeKHR cube[2]{{XR_TYPE_COMPOSITION_LAYER_CUBE_KHR},
                                    {XR_TYPE_COMPOSITION_LAYER_CUBE_KHR}};
  XrCompositionLayerEquirectKHR equirect[2]{{XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR},
                                            {XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR}};
  XrCompositionLayerQuad quad[2]{{XR_TYPE_COMPOSITION_LAYER_QUAD},
                                 {XR_TYPE_COMPOSITION_LAYER_QUAD}};
  XrCompositionLayerQuad icon{XR_TYPE_COMPOSITION_LAYER_QUAD};
  XrCompositionLayerColorScaleBiasKHR color{XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR};
  // Why no host layer shows it exactly, when translate_layer could not.
  const char *reason = nullptr;
};
// (ring, slot, texture rect, eye, bottom_up, out): bottom_up places guest
// memory row 0 at the bottom of the displayed image (see XrImages).
using LayerImage =
    std::function<XrResult(TextureSwapChain *, int, Rect, int, bool, XrSwapchainSubImage &)>;
// A projection layer asks for `first_eye`'s image first: when both eyes share
// one texture, that request copies it (the desktop mirror's eye goes first).
// A layer no host layer shows exactly goes to `drawn` when given (its
// placeholder joins `layers`); without it, the error says why (LayerStorage::reason).
XrResult translate_layer(const LayerHeader &, const XrView *eyes, LayerImage, LayerStorage &,
                         std::vector<const XrCompositionLayerBaseHeader *> &, int first_eye = 0,
                         DrawnLayers *drawn = nullptr);
// Logs a layer's matrices, rects and rings (debug reports).
void log_layer_shape(const LayerHeader &);
} // namespace venture::hle::vrapi
