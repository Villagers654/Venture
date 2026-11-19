#include "hle/openxr/layer_layout.h"
#include "hle/openxr/openxr.h"
#include <algorithm>
#include <format>

namespace venture::hle::openxr {
namespace {
const char* type_name(XrStructureType type) {
  switch (type) {
  case XR_TYPE_COMPOSITION_LAYER_PROJECTION: return "projection";
  case XR_TYPE_COMPOSITION_LAYER_QUAD: return "quad";
  case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR: return "cylinder";
  case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR: return "equirect";
  case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR: return "equirect2";
  case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR: return "cube";
  case XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB: return "passthrough";
  case XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_HTC: return "HTC passthrough";
  default: return nullptr;
  }
}
std::string type_text(XrStructureType type) {
  const char* name = type_name(type);
  return name ? std::string(name) : std::format("type {}", int(type));
}
const char* eye_name(XrEyeVisibility eye) {
  return eye == XR_EYE_VISIBILITY_LEFT ? "left" : eye == XR_EYE_VISIBILITY_RIGHT ? "right" : "both";
}
// The subimage, eye and geometry of the layer types that have them.
struct Shape {
  const XrSwapchainSubImage* sub = nullptr;
  XrEyeVisibility eye = XR_EYE_VISIBILITY_BOTH;
  bool has_eye = false;
  std::string geometry;
};
// Where a layer is and which way it faces: a panel turned edge-on or a
// collapsed orientation shows as a thin strip, so the rotation is named too.
std::string orientation(const XrQuaternionf& q) {
  return std::format("rot ({:.2f},{:.2f},{:.2f},{:.2f})", q.x, q.y, q.z, q.w);
}
std::string position(const XrPosef& pose) {
  return std::format("at ({:.2f},{:.2f},{:.2f}) {}", pose.position.x, pose.position.y, pose.position.z,
                     orientation(pose.orientation));
}
Shape shape(const XrCompositionLayerBaseHeader& layer, bool geometry = true) {
  Shape s;
  switch (layer.type) {
  case XR_TYPE_COMPOSITION_LAYER_QUAD: {
    const auto& q = reinterpret_cast<const XrCompositionLayerQuad&>(layer);
    s = {&q.subImage, q.eyeVisibility, true, !geometry ? std::string() : std::format("{:.2f}x{:.2f} m {}", q.size.width, q.size.height, position(q.pose))};
    break;
  }
  case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR: {
    const auto& c = reinterpret_cast<const XrCompositionLayerCylinderKHR&>(layer);
    s = {&c.subImage, c.eyeVisibility, true, !geometry ? std::string() :
         std::format("radius {:.2f} m angle {:.2f} aspect {:.2f} {}", c.radius, c.centralAngle, c.aspectRatio, position(c.pose))};
    break;
  }
  case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR: {
    const auto& e = reinterpret_cast<const XrCompositionLayerEquirectKHR&>(layer);
    s = {&e.subImage, e.eyeVisibility, true, !geometry ? std::string() : std::format("radius {:.2f} m scale {:.2f}x{:.2f} bias {:.2f},{:.2f} {}", e.radius,
                                                          e.scale.x, e.scale.y, e.bias.x, e.bias.y, position(e.pose))};
    break;
  }
  case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR: {
    const auto& e = reinterpret_cast<const XrCompositionLayerEquirect2KHR&>(layer);
    s = {&e.subImage, e.eyeVisibility, true, !geometry ? std::string() : std::format("radius {:.2f} m angles {:.2f} {:.2f}/{:.2f} {}", e.radius,
                                                          e.centralHorizontalAngle, e.upperVerticalAngle, e.lowerVerticalAngle, position(e.pose))};
    break;
  }
  case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR: {
    const auto& c = reinterpret_cast<const XrCompositionLayerCubeKHR&>(layer);
    s.eye = c.eyeVisibility, s.has_eye = true;
    if (geometry) s.geometry = orientation(c.orientation);
    break;
  }
  default: break;
  }
  return s;
}
// Chains are read before validation, which rejects cycles: a walk stops after
// kMaxChain nodes and returns whether more followed.
constexpr unsigned kMaxChain = 16;
// Layers past this many (a host takes 16 at most) are not described.
constexpr std::uint32_t kMaxLayers = 64;
template <class F> bool each_chained(const void* next, F&& f) {
  auto* node = static_cast<const XrBaseInStructure*>(next);
  for (unsigned depth = 0; node && depth < kMaxChain; ++depth, node = node->next) f(node->type);
  return node != nullptr;
}
// A chained structure: its name, and whether the guest enabled its extension.
std::string chained(XrStructureType type, const LayerLayoutQueries& queries) {
  for (const auto& info : structure_metadata())
    if (info.type == type) {
      std::string text = info.name;
      if (*info.extension && queries.enabled && !queries.enabled(info.extension)) text += " (not enabled)";
      return text;
    }
  return std::format("type {} (unknown)", int(type));
}
std::string chain_text(XrSwapchain chain, const LayerLayoutQueries& queries) {
  const auto size = queries.chain_size ? queries.chain_size(chain) : std::nullopt;
  return size ? std::format("chain {}x{}", size->width, size->height) : std::string("chain unknown");
}
std::string describe(const XrCompositionLayerBaseHeader& layer, const LayerLayoutQueries& queries) {
  std::string text = std::format("{} flags={:#x} space={}", type_text(layer.type), layer.layerFlags,
                                 reinterpret_cast<std::uintptr_t>(layer.space));
  if (layer.type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
    const auto& p = reinterpret_cast<const XrCompositionLayerProjection&>(layer);
    for (std::uint32_t i = 0; p.views && i < p.viewCount && i < 2; ++i) {
      const auto& sub = p.views[i].subImage;
      text += std::format(" view {} {} slice {} rect {}x{}", i, chain_text(sub.swapchain, queries), sub.imageArrayIndex,
                          sub.imageRect.extent.width, sub.imageRect.extent.height);
      if (sub.imageRect.offset.x || sub.imageRect.offset.y)
        text += std::format("+{}+{}", sub.imageRect.offset.x, sub.imageRect.offset.y);
      // Degrees left/right/up/down: a field that differs from the host's eye FOV shows here.
      constexpr double degrees = 57.29577951308232;
      const auto& f = p.views[i].fov;
      text += std::format(" fov {:.1f}/{:.1f}/{:.1f}/{:.1f}", f.angleLeft * degrees, f.angleRight * degrees,
                          f.angleUp * degrees, f.angleDown * degrees);
      if (each_chained(p.views[i].next, [&](XrStructureType type) { text += " +" + chained(type, queries); }))
        text += " +...";
    }
  } else {
    const auto s = shape(layer);
    if (s.has_eye) text += std::format(" eye {}", eye_name(s.eye));
    if (s.sub)
      text += std::format(" {} slice {} rect {}x{}", chain_text(s.sub->swapchain, queries), s.sub->imageArrayIndex,
                          s.sub->imageRect.extent.width, s.sub->imageRect.extent.height);
    if (layer.type == XR_TYPE_COMPOSITION_LAYER_CUBE_KHR)
      text += " " + chain_text(reinterpret_cast<const XrCompositionLayerCubeKHR&>(layer).swapchain, queries);
    if (!s.geometry.empty()) text += " " + s.geometry;
  }
  if (each_chained(layer.next, [&](XrStructureType type) { text += " +" + chained(type, queries); }))
    text += " +...";
  return text;
}
// What identifies an arrangement: rects and poses change from frame to frame.
void key_of(const XrFrameEndInfo& info, std::vector<std::uint64_t>& key) {
  key.clear();
  key.push_back(std::uint64_t(info.environmentBlendMode));
  for (std::uint32_t i = 0; info.layers && i < std::min(info.layerCount, kMaxLayers); ++i) {
    const auto* layer = info.layers[i];
    if (!layer) { key.push_back(0); continue; }
    key.push_back(std::uint64_t(layer->type));
    key.push_back(layer->layerFlags);
    key.push_back(reinterpret_cast<std::uintptr_t>(layer->space));
    if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
      const auto& p = *reinterpret_cast<const XrCompositionLayerProjection*>(layer);
      key.push_back(p.viewCount);
      for (std::uint32_t v = 0; p.views && v < p.viewCount && v < 2; ++v) {
        key.push_back(reinterpret_cast<std::uintptr_t>(p.views[v].subImage.swapchain));
        key.push_back(p.views[v].subImage.imageArrayIndex);
        each_chained(p.views[v].next, [&](XrStructureType type) { key.push_back(std::uint64_t(type)); });
      }
    } else {
      const auto s = shape(*layer, false);
      key.push_back(s.eye);
      if (s.sub) key.push_back(reinterpret_cast<std::uintptr_t>(s.sub->swapchain)), key.push_back(s.sub->imageArrayIndex);
      if (layer->type == XR_TYPE_COMPOSITION_LAYER_CUBE_KHR)
        key.push_back(reinterpret_cast<std::uintptr_t>(reinterpret_cast<const XrCompositionLayerCubeKHR*>(layer)->swapchain));
    }
    if (each_chained(layer->next, [&](XrStructureType type) { key.push_back(std::uint64_t(type)); }))
      key.push_back(UINT64_MAX - 1);  // longer chain
    key.push_back(UINT64_MAX);  // layer separator
  }
}
}  // namespace

std::optional<std::string> LayerLayouts::change(const XrFrameEndInfo& info, const LayerLayoutQueries& queries) {
  if (reports_ >= kMaxReports) return std::nullopt;
  key_of(info, key_);
  if (current_ < seen_.size() && seen_[current_] == key_) return std::nullopt;
  // Past the limit, only a return to a described arrangement is reported.
  if (unknown_ && std::ranges::find(seen_, key_) == seen_.end()) return std::nullopt;
  ++reports_;
  if (reports_ == kMaxReports) {
    current_ = SIZE_MAX;
    return std::string("layer changes are no longer reported (limit reached)");
  }
  if (const auto found = std::ranges::find(seen_, key_); found != seen_.end()) {
    current_ = std::size_t(found - seen_.begin());
    unknown_ = false;
    return std::format("layers back to arrangement #{}", current_ + 1);
  }
  if (seen_.size() >= kMaxDescribed) {
    current_ = SIZE_MAX;
    unknown_ = true;
    return std::format("layers changed to an arrangement not described ({} described already)", kMaxDescribed);
  }
  seen_.push_back(key_);
  current_ = seen_.size() - 1;
  unknown_ = false;
  std::string text = std::format("layers arrangement #{}: {} layer{}, blend mode {}", current_ + 1, info.layerCount,
                                 info.layerCount == 1 ? "" : "s", int(info.environmentBlendMode));
  for (std::uint32_t i = 0; info.layers && i < std::min(info.layerCount, kMaxLayers); ++i)
    text += std::format("; [{}] {}", i, info.layers[i] ? describe(*info.layers[i], queries) : std::string("null"));
  return text;
}
std::string layer_type_list(std::span<const XrCompositionLayerBaseHeader* const> layers) {
  std::string text;
  for (const auto* layer : layers) {
    if (!text.empty()) text += ", ";
    text += layer ? type_text(layer->type) : std::string("null");
  }
  return text.empty() ? std::string("none") : text;
}
} // namespace venture::hle::openxr
