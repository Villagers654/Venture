#pragma once
#include "xr/api.h"
#include <array>
#include <deque>
#include <optional>
#include <vector>

namespace venture::hle::openxr {
// How guest XR_FB_passthrough reaches the real world. Backends are chosen by
// what the host offers and what actually starts (never by a runtime or
// headset name), in this order of preference:
//   native    host XR_FB_passthrough reporting support: calls and layers forwarded
//   htc       host XR_HTC_passthrough: each reconstruction layer becomes a planar HTC layer
//   blend     host ALPHA_BLEND (or ADDITIVE) environment blend mode: the host shows the
//             world behind the guest's layers; reconstruction layers become that underlay
//   camera    a headset camera through OpenVR's IVRTrackedCamera (Windows): the camera
//             image on world-locked planes at the reconstruction layer's place
//   external  TEMPORARY: the Steam Link passthrough companion answering over its
//             SBN1 protocol (xr/passthrough_source.h): it shows the world under
//             the guest's alpha
//   none      supportsPassthrough false, creation XR_ERROR_FEATURE_UNSUPPORTED
// Only with passthrough not opted out (VENTURE_PASSTHROUGH) and for A64 guests.
// One backend is never chosen by capability, only by the user (Settings, which
// sets VENTURE_PASSTHROUGH_KEY), and then replaces the list above:
//   color_key the guest's see-through regions are drawn in a flat key color into
//             the opaque frame, for a streaming tool or layer that replaces that
//             color with the camera image (it cannot be detected)
enum class PassthroughBackend { none, native, htc, blend, camera, external, color_key };
// What a backend shows of XR_FB_passthrough. The guest is offered exactly
// this: properties report it, and anything else is refused with
// XR_ERROR_FEATURE_UNSUPPORTED (native forwards everything to the host).
struct PassthroughCapabilities {
  bool forwarded = false;   // guest calls and layers go to the host unchanged
  bool color = false;       // colour image (XR_PASSTHROUGH_CAPABILITY_COLOR_BIT_FB)
  bool depth = false;       // XR_PASSTHROUGH_CAPABILITY_LAYER_DEPTH_BIT_FB
  bool opacity = false;     // textureOpacityFactor below 1 lets the layers below show
  bool layer_order = false; // shown at the layer's place in the list, not only as an underlay
  bool styles = false;      // edge colour, colour maps
  bool projected = false;   // projected purpose, triangle meshes, geometry instances
};
PassthroughBackend passthrough_backend();
const char* passthrough_backend_name(PassthroughBackend);
PassthroughCapabilities passthrough_capabilities(PassthroughBackend);
// The host's see-through blend mode the blend backend submits (ALPHA_BLEND or
// ADDITIVE), or OPAQUE when the host has none.
XrEnvironmentBlendMode passthrough_blend_mode();
// Host capability answers and failed backends are kept per host instance;
// tests change the host.
void reset_passthrough_probe();
// All functions are serialized by the guest OpenXR state mutex.
// True when guest XR_FB_passthrough structures are applied locally.
bool local_passthrough();
bool passthrough_properties(XrBaseOutStructure*);
XrResult validate_passthrough_layer(XrSession, const XrCompositionLayerBaseHeader*);
XrResult destroy_passthrough_session(XrSession);
void suspend_passthrough_source();
void withdraw_passthrough_source();
struct PassthroughFrame {
  XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
  XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  std::array<XrCompositionLayerProjectionView, 2> views;
  const XrCompositionLayerBaseHeader* layer{};
  std::uint64_t permission = 0;
  bool force_opaque = false; // normal presentation while the external source withdraws
  const XrCompositionLayerProjection* guest_projection = nullptr;
  // Ordinary presentation (every backend but a handled external-source frame):
  // guest layers before first_layer are covered by a running reconstruction
  // layer and left out; blend_mode, when not OPAQUE, replaces the guest's
  // environment blend mode; underlay_opacity < 1 is the reconstruction opacity
  // carried by the alpha of the lowest remaining layer (ALPHA_BLEND only).
  std::uint32_t first_layer = 0;
  XrEnvironmentBlendMode blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  float underlay_opacity = 1;
  // color_key: the key (linear-light RGB) the lowest remaining layer's host
  // copy shows where its coverage, after underlay_opacity, is below one half.
  std::optional<std::array<float, 3>> key;
};
// External source: the local backend emits one complete independent RGBA
// texture per eye; unsupported layer arrangements are rejected before
// declaring source intent. Blend: plans the ordinary submission (handled stays false).
XrResult prepare_passthrough_frame(XrSession, const XrFrameEndInfo&, PassthroughFrame&, bool& handled);
// Storage for the layers a guest passthrough layer becomes in one frame.
struct PassthroughLayerStorage {
  std::deque<XrCompositionLayerPassthroughHTC> htc;
  std::deque<XrCompositionLayerQuad> quads;
};
// The form a validated guest passthrough layer takes on the ordinary path, at
// its place in the list: `forward` is the guest layer itself (native); `host`
// a host-form layer (htc); `guest` guest-form layers for ordinary presentation
// (camera planes over internal swapchains and spaces). All empty when the layer
// is left out (paused, or shown through the blend mode or an external source).
struct PassthroughLayerForm {
  const XrCompositionLayerBaseHeader* forward = nullptr;
  const XrCompositionLayerBaseHeader* host = nullptr;
  std::vector<const XrCompositionLayerBaseHeader*> guest;
};
XrResult passthrough_layer_form(XrSession, const XrCompositionLayerBaseHeader*, PassthroughLayerStorage&, PassthroughLayerForm&);
bool passthrough_permission_current(std::uint64_t);
XrResult make_passthrough_frame_opaque(XrSession, PassthroughFrame&);
XrResult prepare_passthrough_images(XrSession, const XrCompositionLayerProjection&, PassthroughFrame&, float camera_opacity = 1);
XrResult destroy_passthrough_images();
} // namespace venture::hle::openxr
