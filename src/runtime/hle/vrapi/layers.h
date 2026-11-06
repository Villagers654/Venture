#pragma once
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
};
// (ring, slot, texture rect, eye, bottom_up, out): bottom_up places guest
// memory row 0 at the bottom of the displayed image (see XrImages).
using LayerImage =
    std::function<XrResult(TextureSwapChain *, int, Rect, int, bool, XrSwapchainSubImage &)>;
// A projection layer asks for `first_eye`'s image first: when both eyes share
// one texture, that request copies it (the desktop mirror's eye goes first).
XrResult translate_layer(const LayerHeader &, const XrView *eyes, LayerImage, LayerStorage &,
                         std::vector<const XrCompositionLayerBaseHeader *> &, int first_eye = 0);
} // namespace venture::hle::vrapi
