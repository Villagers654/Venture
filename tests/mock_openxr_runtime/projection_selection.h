#pragma once
#include <cstdint>
#include <openxr/openxr.h>

namespace mock {
// Composition order gives routes a portable choice between the world view and
// a topmost menu view. A bridge that precomposes them has one projection, so
// both choices select that projection.
inline const XrCompositionLayerProjection* select_projection(const XrFrameEndInfo& info, bool last) {
  const XrCompositionLayerProjection* selected = nullptr;
  for (std::uint32_t i = 0; i < info.layerCount; ++i) {
    if (!info.layers[i] || info.layers[i]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
    const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(info.layers[i]);
    if (!projection->viewCount) continue;
    selected = projection;
    if (!last) break;
  }
  return selected;
}
}
