#pragma once
#include "xr/api.h"
#include <array>

namespace venture::hle::openxr {
// All functions are serialized by the guest OpenXR state mutex.
bool local_passthrough();
bool passthrough_properties(XrBaseOutStructure*);
XrResult validate_passthrough_layer(XrSession, const XrCompositionLayerBaseHeader*);
XrResult destroy_passthrough_session(XrSession);
void suspend_passthrough_source();
struct PassthroughFrame {
  XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
  XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  std::array<XrCompositionLayerProjectionView, 2> views;
  const XrCompositionLayerBaseHeader* layer{};
  std::uint64_t permission = 0;
  const XrCompositionLayerProjection* guest_projection = nullptr;
};
// The local backend emits one complete independent RGBA texture per eye.
// Unsupported layer arrangements are rejected before declaring source intent.
XrResult prepare_passthrough_frame(XrSession, const XrFrameEndInfo&, PassthroughFrame&, bool& handled);
bool passthrough_permission_current(std::uint64_t);
XrResult make_passthrough_frame_opaque(XrSession, PassthroughFrame&);
XrResult prepare_passthrough_images(XrSession, const XrCompositionLayerProjection&, PassthroughFrame&, float camera_opacity = 1);
XrResult destroy_passthrough_images();
} // namespace venture::hle::openxr
