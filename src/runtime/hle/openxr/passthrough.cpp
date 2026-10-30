#include "hle/openxr/passthrough.h"
#include "hle/openxr/state.h"
#include "xr/steam_link.h"
#include "common/log.h"
#include <algorithm>
#include <cmath>

namespace venture::hle::openxr {
namespace {
struct Feature { XrSession session; bool running; };
struct Layer { XrSession session; XrPassthroughFB feature; bool running; float opacity = 1; };
struct Geometry { XrSession session; XrPassthroughLayerFB layer; };
// Storage also supplies stable, typed local handles. Native handles retain the
// host's identity and are never substituted in a forwarded structure.
std::map<XrPassthroughFB, std::unique_ptr<Feature>> features;
std::map<XrPassthroughLayerFB, std::unique_ptr<Layer>> layers;
std::map<XrGeometryInstanceFB, Geometry> geometries;
std::unique_ptr<SteamLinkSource> source;
bool native() { return host_xr().enabled_extensions.contains("XR_FB_passthrough"); }
void withdraw() { if (source) source->request(false); }
template<class H> bool belongs(H object, XrSession target) {
  const auto it = state().objects.find({handle_type<H>(), key(object)});
  return it != state().objects.end() && it->second.parent == key(target) && it->second.parent_type == "XrSession";
}
bool valid_layer(XrPassthroughLayerFB layer) {
  const auto found = layers.find(layer);
  return found != layers.end() && valid_handle(layer, "XrPassthroughLayerFB") &&
         features.contains(found->second->feature);
}
} // namespace
bool local_passthrough() { return !native() && steam_link_passthrough_configured(); }
void suspend_passthrough_source() { withdraw(); }
bool passthrough_properties(XrBaseOutStructure* property) {
  if (!local_passthrough() || !enabled("XR_FB_passthrough")) return false;
  if (property->type == XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB) {
    reinterpret_cast<XrSystemPassthroughPropertiesFB*>(property)->supportsPassthrough = XR_TRUE;
    return true;
  }
  if (property->type == XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB) {
    reinterpret_cast<XrSystemPassthroughProperties2FB*>(property)->capabilities =
        XR_PASSTHROUGH_CAPABILITY_BIT_FB | XR_PASSTHROUGH_CAPABILITY_COLOR_BIT_FB;
    return true;
  }
  return false;
}
XrResult validate_passthrough_layer(XrSession target, const XrCompositionLayerBaseHeader* header) {
  if (!header || header->type != XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB) return XR_ERROR_LAYER_INVALID;
  if (!enabled("XR_FB_passthrough")) return XR_ERROR_LAYER_INVALID;
  const auto& layer = *reinterpret_cast<const XrCompositionLayerPassthroughFB*>(header);
  // XR_FB_passthrough requires null space: reconstruction is runtime-owned.
  if (layer.space != XR_NULL_HANDLE) return XR_ERROR_LAYER_INVALID;
  if (!valid_layer(layer.layerHandle) || !belongs(layer.layerHandle, target)) return XR_ERROR_HANDLE_INVALID;
  auto root = layer; root.next = nullptr;
  const auto result = validate_chain(&root, !local_passthrough());
  return XR_FAILED(result) ? result : validate_chain(layer.next, !local_passthrough(), layer.type, true);
}
bool passthrough_permission_current(std::uint64_t permission) {
  return permission && source && source->permission() == permission;
}
XrResult make_passthrough_frame_opaque(XrSession target, PassthroughFrame& frame) {
  frame.permission = 0;
  const auto result = frame.guest_projection ? prepare_passthrough_images(target, *frame.guest_projection, frame, 0) : XR_SUCCESS;
  frame.projection.layerFlags = 0;
  return result;
}
XrResult prepare_passthrough_frame(XrSession target, const XrFrameEndInfo& info,
                                   PassthroughFrame& out, bool& handled) {
  handled = false;
  if (!local_passthrough()) return XR_SUCCESS;
  auto fail = [&](XrResult error) {
    withdraw();
    std::string types;
    for (std::uint32_t i = 0; i < std::min(info.layerCount, 8u); ++i)
      types += (types.empty() ? "" : ",") + std::to_string(info.layers[i] ? int(info.layers[i]->type) : 0);
    log::warn("xr-passthrough", "Steam Link frame refused: result={} layers={} types={}", int(error), info.layerCount, types);
    return error;
  };
  const XrCompositionLayerPassthroughFB* room = nullptr;
  const XrCompositionLayerProjection* scene = nullptr;
  for (std::uint32_t i = 0; i < info.layerCount; ++i) {
    const auto* header = info.layers[i];
    if (!header) return fail(XR_ERROR_LAYER_INVALID);
    if (header->type == XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB) {
      const auto result = validate_passthrough_layer(target, header);
      if (XR_FAILED(result)) return fail(result);
      if (room || scene) return fail(XR_ERROR_FEATURE_UNSUPPORTED); // reconstruction underlay only
      room = reinterpret_cast<const XrCompositionLayerPassthroughFB*>(header);
      for (auto* node = static_cast<const XrBaseInStructure*>(room->next); node; node = node->next)
        for (const auto& metadata : structure_metadata())
          if (metadata.type == node->type && (!*metadata.extension || enabled(metadata.extension)))
            return fail(XR_ERROR_FEATURE_UNSUPPORTED);
      if (room->flags & ~(XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
          XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT | XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT))
        return fail(XR_ERROR_FEATURE_UNSUPPORTED);
    } else if (header->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
      if (scene) { if (room) return fail(XR_ERROR_FEATURE_UNSUPPORTED); }
      scene = reinterpret_cast<const XrCompositionLayerProjection*>(header);
    }
  }
  if (!room) {
    withdraw();
    // On MR -> VR transitions, keep the current ordinary projection opaque
    // while the peer observes pipe EOF. No old alpha can extend camera intent.
    if (source && scene && info.layerCount == 1) {
      handled = true;
      out.info = info; out.guest_projection = scene;
      const auto result = make_passthrough_frame_opaque(target, out);
      if (XR_FAILED(result)) return result;
      out.layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&out.projection);
      out.info.layers = &out.layer;
    }
    return XR_SUCCESS;
  }
  if (!session().running) return fail(XR_ERROR_SESSION_NOT_RUNNING);
  if (session().phase != Session::Phase::begun) return fail(XR_ERROR_CALL_ORDER_INVALID);
  if (info.layerCount != (scene ? 2u : 1u)) return fail(XR_ERROR_FEATURE_UNSUPPORTED);
  if (info.environmentBlendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE) return fail(XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED);
  const auto& layer = *layers.at(room->layerHandle);
  const auto& feature = *features.at(layer.feature);
  // Paused passthrough retains ordinary opaque VR; no source declaration.
  const bool active = feature.running && layer.running;
  handled = true;
  out.info = info;
  out.info.layerCount = 0; out.info.layers = nullptr;
  out.guest_projection = scene;
  out.permission = active && source ? source->permission() : 0;
  if (!scene && layer.opacity != 1) return fail(XR_ERROR_FEATURE_UNSUPPORTED);
  // Without a current lease the pixels themselves have A=1. This also covers
  // the bounded interval before the worker closes a withdrawn pipe: an older
  // server grant must never keep the camera visible after a guest pause.
  auto result = scene ? prepare_passthrough_images(target, *scene, out, out.permission ? layer.opacity : 0) : XR_SUCCESS;
  if (XR_FAILED(result)) return fail(result);
  if (active && !source) source = std::make_unique<SteamLinkSource>();
  if (source) source->request(active);
  if (out.permission && !passthrough_permission_current(out.permission)) {
    result = make_passthrough_frame_opaque(target, out);
    if (XR_FAILED(result)) return fail(result);
  }
  if (!scene) return XR_SUCCESS;
  // Without a live format grant retain ordinary VR submission. A running
  // companion separately proves encoder/decoder/image ownership for MR.
  out.projection.layerFlags = 0;
  out.layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&out.projection);
  out.info.layerCount = 1; out.info.layers = &out.layer;
  return result;
}
XrResult destroy_passthrough_session(XrSession target) {
  withdraw();
  source.reset();
  session().finish_host_end();
  const auto images_result = destroy_passthrough_images();
  if (XR_FAILED(images_result)) return images_result;
  std::vector<XrGeometryInstanceFB> geometry_handles;
  for (const auto& [handle, record] : geometries) if (record.session == target) geometry_handles.push_back(handle);
  for (auto handle : geometry_handles) {
    auto result = guest::xrDestroyGeometryInstanceFB(handle); if (XR_FAILED(result)) return result;
  }
  std::vector<XrPassthroughLayerFB> layer_handles;
  for (const auto& [handle, record] : layers) if (record->session == target) layer_handles.push_back(handle);
  for (auto handle : layer_handles) {
    auto result = guest::xrDestroyPassthroughLayerFB(handle); if (XR_FAILED(result)) return result;
  }
  std::vector<XrPassthroughFB> feature_handles;
  for (const auto& [handle, record] : features) if (record->session == target) feature_handles.push_back(handle);
  for (auto handle : feature_handles) {
    auto result = guest::xrDestroyPassthroughFB(handle); if (XR_FAILED(result)) return result;
  }
  return XR_SUCCESS;
}
namespace guest {
XrResult xrCreatePassthroughFB(XrSession target, const XrPassthroughCreateInfoFB* info, XrPassthroughFB* out) {
  std::lock_guard lock(state().mutex);
  if (!enabled("XR_FB_passthrough")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!valid_handle(target, "XrSession")) return XR_ERROR_HANDLE_INVALID;
  if (!out) return XR_ERROR_VALIDATION_FAILURE;
  *out = XR_NULL_HANDLE;
  if (XR_FAILED(validate_struct(info, XR_TYPE_PASSTHROUGH_CREATE_INFO_FB))) return XR_ERROR_VALIDATION_FAILURE;
  for (const auto& [handle, feature] : features)
    if (feature->session == target) return XR_ERROR_FEATURE_ALREADY_CREATED_PASSTHROUGH_FB;
  if (!native() && (!local_passthrough() || info->next ||
                   (info->flags & ~XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB))) return XR_ERROR_FEATURE_UNSUPPORTED;
  auto record = std::make_unique<Feature>(target, bool(info->flags & XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB));
  XrResult result = XR_SUCCESS;
  if (native()) result = host_call<PFN_xrCreatePassthroughFB, "xrCreatePassthroughFB">(target, info, out);
  else *out = reinterpret_cast<XrPassthroughFB>(record.get());
  if (XR_SUCCEEDED(result)) { features[*out] = std::move(record); track_handle(*out, "XrPassthroughFB", target); }
  log::info("xr-passthrough", "Create passthrough: backend={} flags={:#x} result={}",
            native() ? "native OpenXR" : "Steam Link", info->flags, int(result));
  return result;
}
XrResult xrDestroyPassthroughFB(XrPassthroughFB handle) {
  std::lock_guard lock(state().mutex);
  if (!enabled("XR_FB_passthrough")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!valid_handle(handle, "XrPassthroughFB") || !features.contains(handle)) return XR_ERROR_HANDLE_INVALID;
  std::vector<XrPassthroughLayerFB> children;
  for (const auto& [id, layer] : layers) if (layer->feature == handle) children.push_back(id);
  for (auto child : children) {
    const auto result = guest::xrDestroyPassthroughLayerFB(child);
    if (XR_FAILED(result)) return result;
  }
  session().finish_host_end();
  const auto result = native() ? host_call<PFN_xrDestroyPassthroughFB, "xrDestroyPassthroughFB">(handle) : XR_SUCCESS;
  if (XR_SUCCEEDED(result)) { withdraw(); forget_handle(handle); features.erase(handle); }
  return result;
}
XrResult xrPassthroughStartFB(XrPassthroughFB handle) {
  std::lock_guard lock(state().mutex);
  if (!enabled("XR_FB_passthrough")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!valid_handle(handle, "XrPassthroughFB") || !features.contains(handle)) return XR_ERROR_HANDLE_INVALID;
  const auto result = native() ? host_call<PFN_xrPassthroughStartFB, "xrPassthroughStartFB">(handle) : XR_SUCCESS;
  if (XR_SUCCEEDED(result)) features.at(handle)->running = true;
  return result;
}
XrResult xrPassthroughPauseFB(XrPassthroughFB handle) {
  std::lock_guard lock(state().mutex);
  if (!enabled("XR_FB_passthrough")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!valid_handle(handle, "XrPassthroughFB") || !features.contains(handle)) return XR_ERROR_HANDLE_INVALID;
  const auto result = native() ? host_call<PFN_xrPassthroughPauseFB, "xrPassthroughPauseFB">(handle) : XR_SUCCESS;
  if (XR_SUCCEEDED(result)) { features.at(handle)->running = false; withdraw(); }
  return result;
}
XrResult xrCreatePassthroughLayerFB(XrSession target, const XrPassthroughLayerCreateInfoFB* info, XrPassthroughLayerFB* out) {
  std::lock_guard lock(state().mutex);
  if (!enabled("XR_FB_passthrough")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!valid_handle(target, "XrSession")) return XR_ERROR_HANDLE_INVALID;
  if (!out) return XR_ERROR_VALIDATION_FAILURE;
  *out = XR_NULL_HANDLE;
  if (XR_FAILED(validate_struct(info, XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB))) return XR_ERROR_VALIDATION_FAILURE;
  if (std::none_of(features.begin(), features.end(), [&](const auto& entry) { return entry.second->session == target; }))
    return XR_ERROR_FEATURE_REQUIRED_PASSTHROUGH_FB;
  if (!features.contains(info->passthrough) || !belongs(info->passthrough, target)) return XR_ERROR_HANDLE_INVALID;
  if (!native() && (info->next || (info->flags & ~XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB) ||
      info->purpose != XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB)) return XR_ERROR_FEATURE_UNSUPPORTED;
  if (layers.size() >= 64) return XR_ERROR_LIMIT_REACHED;
  auto record = std::make_unique<Layer>(target, info->passthrough,
      bool(info->flags & XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB));
  const auto result = native() ? host_call<PFN_xrCreatePassthroughLayerFB, "xrCreatePassthroughLayerFB">(target, info, out) : XR_SUCCESS;
  if (!native()) *out = reinterpret_cast<XrPassthroughLayerFB>(record.get());
  if (XR_SUCCEEDED(result)) { layers[*out] = std::move(record); track_handle(*out, "XrPassthroughLayerFB", target); }
  return result;
}
XrResult xrDestroyPassthroughLayerFB(XrPassthroughLayerFB handle) {
  std::lock_guard lock(state().mutex);
  if (!enabled("XR_FB_passthrough")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!valid_layer(handle)) return XR_ERROR_HANDLE_INVALID;
  std::vector<XrGeometryInstanceFB> children;
  for (const auto& [id, record] : geometries) if (record.layer == handle) children.push_back(id);
  for (auto child : children) {
    const auto result = guest::xrDestroyGeometryInstanceFB(child);
    if (XR_FAILED(result)) return result;
  }
  session().finish_host_end();
  const auto result = native() ? host_call<PFN_xrDestroyPassthroughLayerFB, "xrDestroyPassthroughLayerFB">(handle) : XR_SUCCESS;
  if (XR_SUCCEEDED(result)) { withdraw(); forget_handle(handle); layers.erase(handle); }
  return result;
}
XrResult xrPassthroughLayerPauseFB(XrPassthroughLayerFB handle) {
  std::lock_guard lock(state().mutex);
  if (!enabled("XR_FB_passthrough")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!valid_layer(handle)) return XR_ERROR_HANDLE_INVALID;
  const auto result = native() ? host_call<PFN_xrPassthroughLayerPauseFB, "xrPassthroughLayerPauseFB">(handle) : XR_SUCCESS;
  if (XR_SUCCEEDED(result)) { layers.at(handle)->running = false; withdraw(); }
  return result;
}
XrResult xrPassthroughLayerResumeFB(XrPassthroughLayerFB handle) {
  std::lock_guard lock(state().mutex);
  if (!enabled("XR_FB_passthrough")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!valid_layer(handle)) return XR_ERROR_HANDLE_INVALID;
  const auto result = native() ? host_call<PFN_xrPassthroughLayerResumeFB, "xrPassthroughLayerResumeFB">(handle) : XR_SUCCESS;
  if (XR_SUCCEEDED(result)) layers.at(handle)->running = true;
  return result;
}
XrResult xrPassthroughLayerSetStyleFB(XrPassthroughLayerFB handle, const XrPassthroughStyleFB* style) {
  std::lock_guard lock(state().mutex);
  if (!enabled("XR_FB_passthrough")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!valid_layer(handle)) return XR_ERROR_HANDLE_INVALID;
  if (XR_FAILED(validate_struct(style, XR_TYPE_PASSTHROUGH_STYLE_FB))) return XR_ERROR_VALIDATION_FAILURE;
  if (!std::isfinite(style->textureOpacityFactor) || style->textureOpacityFactor < 0 || style->textureOpacityFactor > 1 ||
      !std::isfinite(style->edgeColor.r) || !std::isfinite(style->edgeColor.g) ||
      !std::isfinite(style->edgeColor.b) || !std::isfinite(style->edgeColor.a)) return XR_ERROR_VALIDATION_FAILURE;
  if (native()) return host_call<PFN_xrPassthroughLayerSetStyleFB, "xrPassthroughLayerSetStyleFB">(handle, style);
  // The receiver owns camera composition. Do not claim camera styling that
  // cannot be represented by its ordinary-RGBA profile.
  if (style->next || style->edgeColor.a != 0) return XR_ERROR_FEATURE_UNSUPPORTED;
  layers.at(handle)->opacity = style->textureOpacityFactor;
  return XR_SUCCESS;
}
XrResult xrCreateGeometryInstanceFB(XrSession target, const XrGeometryInstanceCreateInfoFB* info, XrGeometryInstanceFB* out) {
  std::lock_guard lock(state().mutex);
  if (!enabled("XR_FB_passthrough")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!valid_handle(target, "XrSession")) return XR_ERROR_HANDLE_INVALID;
  if (!out) return XR_ERROR_VALIDATION_FAILURE;
  *out = XR_NULL_HANDLE;
  if (!native()) return XR_ERROR_FEATURE_UNSUPPORTED;
  if (XR_FAILED(validate_struct(info, XR_TYPE_GEOMETRY_INSTANCE_CREATE_INFO_FB))) return XR_ERROR_VALIDATION_FAILURE;
  if (!valid_layer(info->layer) || !belongs(info->layer, target) || !belongs(info->mesh, target) ||
      !belongs(info->baseSpace, target)) return XR_ERROR_HANDLE_INVALID;
  const auto result = host_call<PFN_xrCreateGeometryInstanceFB, "xrCreateGeometryInstanceFB">(target, info, out);
  if (XR_SUCCEEDED(result)) { geometries[*out] = {target, info->layer}; track_handle(*out, "XrGeometryInstanceFB", target); }
  return result;
}
XrResult xrDestroyGeometryInstanceFB(XrGeometryInstanceFB handle) {
  std::lock_guard lock(state().mutex);
  if (!enabled("XR_FB_passthrough")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!valid_handle(handle, "XrGeometryInstanceFB") || !geometries.contains(handle)) return XR_ERROR_HANDLE_INVALID;
  session().finish_host_end();
  const auto result = host_call<PFN_xrDestroyGeometryInstanceFB, "xrDestroyGeometryInstanceFB">(handle);
  if (XR_SUCCEEDED(result)) { forget_handle(handle); geometries.erase(handle); }
  return result;
}
XrResult xrGeometryInstanceSetTransformFB(XrGeometryInstanceFB handle, const XrGeometryInstanceTransformFB* info) {
  std::lock_guard lock(state().mutex);
  if (!enabled("XR_FB_passthrough")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!valid_handle(handle, "XrGeometryInstanceFB") || !geometries.contains(handle)) return XR_ERROR_HANDLE_INVALID;
  if (XR_FAILED(validate_struct(info, XR_TYPE_GEOMETRY_INSTANCE_TRANSFORM_FB))) return XR_ERROR_VALIDATION_FAILURE;
  if (!belongs(info->baseSpace, geometries.at(handle).session)) return XR_ERROR_HANDLE_INVALID;
  return host_call<PFN_xrGeometryInstanceSetTransformFB, "xrGeometryInstanceSetTransformFB">(handle, info);
}
} // namespace guest
} // namespace venture::hle::openxr
