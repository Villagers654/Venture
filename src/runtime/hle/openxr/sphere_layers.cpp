#include "hle/openxr/sphere_layers.h"
#include "hle/openxr/state.h"
#include "common/log.h"
#include "gfx/gpu_device.h"
#include "xr/desktop_mirror.h"
#include "xr/dev_dump.h"
#include "xr/display_override.h"
#include "xr/queue.h"
#include "xr/session.h"
#include "xr/sphere_images.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <memory>
#include <numbers>
#include <set>
#include <tuple>

namespace venture::hle::openxr {
namespace {
// One host chain per run of the frame (two array layers, one per eye).
struct Chain {
  XrSwapchain handle{};
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  std::unique_ptr<venture::xr::SphereImages> images;
  std::vector<XrSwapchainImageVulkanKHR> host_images;
  std::uint32_t idle = 0;  // frames without a run
  // What its last released image shows: the run's sources (key, release),
  // draws and views. A run that would draw the same again reuses that image.
  std::vector<std::uint64_t> shown;
  std::vector<std::uint64_t> drawing;  // this frame's, until its image is released
  XrResult destroy() {
    shown.clear();
    images.reset();  // waits for its commands
    if (!handle) return XR_SUCCESS;
    venture::xr::HostQueueLock queue(session().uses_shared_gpu);
    if (!queue) return XR_ERROR_RUNTIME_FAILURE;
    const auto result = venture::xr::dev_destroy_swapchain(handle);
    if (XR_SUCCEEDED(result)) handle = XR_NULL_HANDLE, host_images.clear();
    return result;
  }
};
struct Source {
  std::uintptr_t key;
  std::uint64_t serial = 0;
};
// A run's chain image, acquired until the frame's drawing was submitted.
struct Acquired {
  std::size_t chain;
  std::uint32_t target;
  std::size_t first, last;  // its draws
};
struct Presenter {
  // Records every run's drawing for one submission (created on first use);
  // declared first so that it outlives the chains recording into it.
  std::unique_ptr<venture::xr::LayerFrame> frame;
  std::vector<Chain> chains;
  // Reused from frame to frame.
  std::vector<venture::xr::SphereImages::Draw> draws;
  std::vector<Acquired> acquired;
  // Private copies per run, guest chain and array layer, with the release they hold.
  using At = std::tuple<std::size_t, XrSwapchain, std::uint32_t>;
  std::map<At, Source> sources;
  std::uintptr_t next_key = 1;
  XrExtent2Di size{};
};
Presenter& presenter() {
  static Presenter p;
  return p;
}
XrQuaternionf multiply(const XrQuaternionf& a, const XrQuaternionf& b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
XrVector3f rotate(const XrQuaternionf& q, const XrVector3f& v) {
  const XrVector3f t{2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x)};
  return {v.x + q.w * t.x + q.y * t.z - q.z * t.y, v.y + q.w * t.y + q.z * t.x - q.x * t.z,
          v.z + q.w * t.z + q.x * t.y - q.y * t.x};
}
XrPosef compose(const XrPosef& a, const XrPosef& b) {
  const auto p = rotate(a.orientation, b.position);
  return {multiply(a.orientation, b.orientation),
          {a.position.x + p.x, a.position.y + p.y, a.position.z + p.z}};
}
// Where one drawn layer shows (xr/sphere_images.h), its frame being `layer`.
venture::xr::LayerPlacement placement(const SphereLayers::Sphere& s, const XrPosef& layer, const Swapchain& chain) {
  constexpr float pi = std::numbers::pi_v<float>;
  venture::xr::LayerPlacement p{};
  p.frame = layer;
  p.scale = s.color_scale;
  p.bias = s.color_bias;
  for (int i = 0; i < 4; ++i) p.local[i][i] = 1;
  // Opaque layers replace what is below; the others blend as their flags say.
  const bool blend = s.flags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  p.source_factor = blend && (s.flags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) ? 2 : 1;
  p.destination_factor = blend ? 5 : 0;
  if (s.type == XR_TYPE_COMPOSITION_LAYER_CUBE_KHR) {
    p.shape = venture::xr::LayerShape::cube;
    p.frame.position = {0, 0, 0};  // at infinity
    p.source_layer = s.sub.imageArrayIndex * 6;
    return p;
  }
  const float width = float(chain.info.width), height = float(chain.info.height);
  auto rect = s.sub.imageRect;
  // GLES rects count rows from the bottom; the private copy is top-down.
  if (!chain.textures.empty()) rect.offset.y = std::int32_t(chain.info.height) - rect.offset.y - rect.extent.height;
  const float x = float(rect.offset.x) / width, y = float(rect.offset.y) / height;
  const float w = float(rect.extent.width) / width, h = float(rect.extent.height) / height;
  p.clip[0] = x, p.clip[1] = y, p.clip[2] = x + w, p.clip[3] = y + h;
  // Zero or infinite radius: a sphere at infinity.
  const bool finite = s.radius > 0 && std::isfinite(s.radius);
  if (finite && s.type != XR_TYPE_COMPOSITION_LAYER_QUAD && s.type != XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW)
    for (int i = 0; i < 3; ++i) p.local[i][i] = 1 / s.radius;
  // (u, v) in the subimage, from the shape's (a, b), then into the source.
  float su = 0, u0 = 0, vb = 0, v0 = 0;
  if (s.type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
    // The quad's (x, y) in its frame over its size: (-0.5, 0.5) is the top left.
    p.shape = venture::xr::LayerShape::quad;
    p.local[0][0] = 1 / s.size.width, p.local[1][1] = 1 / s.size.height;
    su = 1, u0 = 0.5f;
    vb = -1, v0 = 0.5f;
  } else if (s.type == XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW) {
    // The view's image as a tan-angle plane in the view's frame: a ray
    // (tx, ty, -1) shows (S, T) = ((tx - l) / (r - l), (ty - u) / (d - u)).
    p.shape = venture::xr::LayerShape::plane;
    const float l = std::tan(s.fov.angleLeft), r = std::tan(s.fov.angleRight);
    const float u = std::tan(s.fov.angleUp), d = std::tan(s.fov.angleDown);
    for (auto& row : p.local) std::fill(std::begin(row), std::end(row), 0.f);
    p.local[0][0] = 1 / (r - l), p.local[0][3] = -l / (r - l);
    p.local[1][1] = 1 / (d - u), p.local[1][3] = -u / (d - u);
    p.local[2][3] = 1;
    su = 1, u0 = 0;
    vb = 1, v0 = 0;
  } else if (s.type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) {
    // The arc's middle is at -Z; it is radius * central / aspect high.
    p.shape = venture::xr::LayerShape::cylinder;
    su = 1 / s.central, u0 = 0.5f;
    vb = -s.upper / s.central, v0 = 0.5f;
  } else if (s.type == XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR) {
    p.shape = finite ? venture::xr::LayerShape::sphere : venture::xr::LayerShape::far_sphere;
    su = s.scale.x / (2 * pi), u0 = 0.5f * s.scale.x + s.bias.x;
    vb = -s.scale.y / pi, v0 = 0.5f * s.scale.y + s.bias.y;
  } else {
    p.shape = finite ? venture::xr::LayerShape::sphere : venture::xr::LayerShape::far_sphere;
    const float central = std::min(s.central, 2 * pi);
    su = 1 / central, u0 = 0.5f;
    vb = -1 / (s.upper - s.lower), v0 = s.upper / (s.upper - s.lower);
  }
  p.map[0][0] = w * su, p.map[0][1] = 0, p.map[0][2] = x + w * u0;
  p.map[1][0] = 0, p.map[1][1] = h * vb, p.map[1][2] = y + h * v0;
  // VERTICAL_FLIP: the image is upside down, the rect names the same rows
  // (the native path's mirrored copy and mirrored rect), so rows within the
  // rect reverse.
  if (s.flip) p.map[1][1] = -p.map[1][1], p.map[1][2] = 2 * y + h - p.map[1][2];
  return p;
}
bool color_format(VkFormat format) {
  switch (format) {
  case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
  case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
  case VK_FORMAT_S8_UINT: return false;
  default: return true;
  }
}
}  // namespace
bool sphere_layer_type(XrStructureType type) {
  return type == XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR || type == XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR ||
         type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR || type == XR_TYPE_COMPOSITION_LAYER_CUBE_KHR;
}
XrResult SphereLayers::add(const XrCompositionLayerBaseHeader* layer,
                           std::vector<const XrCompositionLayerBaseHeader*>& layers) {
  Sphere s{};
  s.type = layer->type;
  if (layer->type == XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR) {
    const auto& e = *reinterpret_cast<const XrCompositionLayerEquirectKHR*>(layer);
    s.flags = e.layerFlags, s.space = e.space, s.eye = e.eyeVisibility, s.sub = e.subImage, s.pose = e.pose;
    s.radius = e.radius, s.scale = e.scale, s.bias = e.bias;
  } else if (layer->type == XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR) {
    const auto& e = *reinterpret_cast<const XrCompositionLayerEquirect2KHR*>(layer);
    s.flags = e.layerFlags, s.space = e.space, s.eye = e.eyeVisibility, s.sub = e.subImage, s.pose = e.pose;
    s.radius = e.radius;
    s.central = e.centralHorizontalAngle, s.upper = e.upperVerticalAngle, s.lower = e.lowerVerticalAngle;
    if (!(e.centralHorizontalAngle > 0) || !(e.upperVerticalAngle > e.lowerVerticalAngle))
      return XR_ERROR_VALIDATION_FAILURE;
  } else if (layer->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) {
    const auto& e = *reinterpret_cast<const XrCompositionLayerCylinderKHR*>(layer);
    s.flags = e.layerFlags, s.space = e.space, s.eye = e.eyeVisibility, s.sub = e.subImage, s.pose = e.pose;
    s.radius = e.radius, s.central = e.centralAngle, s.upper = e.aspectRatio;
    if (!(e.radius >= 0) || !(e.centralAngle >= 0) || !(e.centralAngle <= 2 * std::numbers::pi_v<float>) ||
        !(e.aspectRatio > 0) || !std::isfinite(e.aspectRatio))
      return XR_ERROR_VALIDATION_FAILURE;
    // No arc, or none at a distance: nothing to show.
    if (!(e.radius > 0) || !std::isfinite(e.radius) || !(e.centralAngle > 0)) return XR_SUCCESS;
  } else if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
    const auto& e = *reinterpret_cast<const XrCompositionLayerQuad*>(layer);
    s.flags = e.layerFlags, s.space = e.space, s.eye = e.eyeVisibility, s.sub = e.subImage, s.pose = e.pose;
    s.size = e.size;
    if (!std::isfinite(e.size.width) || !std::isfinite(e.size.height)) return XR_ERROR_VALIDATION_FAILURE;
    if (!(e.size.width > 0) || !(e.size.height > 0)) return XR_SUCCESS;  // nothing to show
  } else {
    const auto& e = *reinterpret_cast<const XrCompositionLayerCubeKHR*>(layer);
    s.flags = e.layerFlags, s.space = e.space, s.eye = e.eyeVisibility, s.pose = {e.orientation, {0, 0, 0}};
    s.sub.swapchain = e.swapchain, s.sub.imageArrayIndex = e.imageArrayIndex;
  }
  // Color scale and bias apply to the texels before blending.
  for (auto* next = static_cast<const XrBaseInStructure*>(layer->next); next; next = next->next)
    if (next->type == XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR &&
        enabled("XR_KHR_composition_layer_color_scale_bias")) {
      const auto& color = *reinterpret_cast<const XrCompositionLayerColorScaleBiasKHR*>(next);
      s.color_scale = color.colorScale, s.color_bias = color.colorBias;
    } else if (next->type == XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB &&
               enabled("XR_FB_composition_layer_image_layout")) {
      s.flip = reinterpret_cast<const XrCompositionLayerImageLayoutFB*>(next)->flags &
               XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;
    }
  if (!valid_handle(s.sub.swapchain, "XrSwapchain")) return XR_ERROR_VALIDATION_FAILURE;
  const auto& chain = state().swapchains.at(s.sub.swapchain);
  if (chain.released_index < 0) return XR_ERROR_LAYER_INVALID;
  if (s.type == XR_TYPE_COMPOSITION_LAYER_CUBE_KHR ? chain.info.faceCount != 6 || s.sub.imageArrayIndex >= chain.info.arraySize
                                                   : s.sub.imageArrayIndex >= chain.info.arraySize * chain.info.faceCount)
    return XR_ERROR_VALIDATION_FAILURE;
  if (chain.guest_only || chain.info.sampleCount != 1 || !color_format(VkFormat(chain.info.format))) {
    static bool reported = false;
    if (!std::exchange(reported, true))
      log::warn("xr", "A drawn layer's swapchain cannot be sampled (samples={} format={}); leaving it out",
                chain.info.sampleCount, chain.info.format);
    return XR_SUCCESS;
  }
  static std::set<XrStructureType> reported;
  if (layer->type != XR_TYPE_COMPOSITION_LAYER_QUAD && reported.insert(layer->type).second)
    log::info("xr", "Host lacks {}; drawing layer type {} into a projection layer",
              layer->type == XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR    ? "XR_KHR_composition_layer_equirect"
              : layer->type == XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR ? "XR_KHR_composition_layer_equirect2"
              : layer->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR  ? "XR_KHR_composition_layer_cylinder"
                                                                       : "XR_KHR_composition_layer_cube",
              int(layer->type));
  if (runs_.empty() || layers.empty() ||
      layers.back() != reinterpret_cast<const XrCompositionLayerBaseHeader*>(&runs_.back().projection)) {
    auto& run = runs_.emplace_back();
    layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&run.projection));
  }
  runs_.back().spheres.push_back(s);
  return XR_SUCCESS;
}
bool SphereLayers::drawable(const XrCompositionLayerBaseHeader* layer) {
  auto sampled = [](XrSwapchain handle) {
    if (!valid_handle(handle, "XrSwapchain")) return false;
    const auto& chain = state().swapchains.at(handle);
    return !chain.guest_only && chain.info.sampleCount == 1 && color_format(VkFormat(chain.info.format)) &&
           chain.released_index >= 0;
  };
  if (!layer) return false;
  if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
    const auto& p = *reinterpret_cast<const XrCompositionLayerProjection*>(layer);
    if (p.viewCount != 2 || !p.views) return false;
    for (std::uint32_t i = 0; i < 2; ++i)
      if (p.views[i].type != XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW || !sampled(p.views[i].subImage.swapchain))
        return false;
    return true;
  }
  if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD)
    return sampled(reinterpret_cast<const XrCompositionLayerQuad*>(layer)->subImage.swapchain);
  if (layer->type == XR_TYPE_COMPOSITION_LAYER_CUBE_KHR)
    return sampled(reinterpret_cast<const XrCompositionLayerCubeKHR*>(layer)->swapchain);
  if (layer->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR)
    return sampled(reinterpret_cast<const XrCompositionLayerCylinderKHR*>(layer)->subImage.swapchain);
  if (layer->type == XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR)
    return sampled(reinterpret_cast<const XrCompositionLayerEquirectKHR*>(layer)->subImage.swapchain);
  if (layer->type == XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR)
    return sampled(reinterpret_cast<const XrCompositionLayerEquirect2KHR*>(layer)->subImage.swapchain);
  return false;
}
XrResult SphereLayers::add_projection(const XrCompositionLayerProjection& projection,
                                      std::vector<const XrCompositionLayerBaseHeader*>& layers) {
  if (!drawable(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)))
    return XR_ERROR_VALIDATION_FAILURE;
  bool vflip = false;
  if (enabled("XR_FB_composition_layer_image_layout"))
    for (auto* next = static_cast<const XrBaseInStructure*>(projection.next); next; next = next->next)
      if (next->type == XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB)
        vflip = reinterpret_cast<const XrCompositionLayerImageLayoutFB*>(next)->flags &
                XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;
  for (std::uint32_t i = 0; i < 2; ++i) {
    const auto& view = projection.views[i];
    const auto& chain = state().swapchains.at(view.subImage.swapchain);
    if (view.subImage.imageArrayIndex >= chain.info.arraySize * chain.info.faceCount)
      return XR_ERROR_VALIDATION_FAILURE;
  }
  if (runs_.empty() || layers.empty() ||
      layers.back() != reinterpret_cast<const XrCompositionLayerBaseHeader*>(&runs_.back().projection)) {
    auto& run = runs_.emplace_back();
    layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&run.projection));
  }
  auto& run = runs_.back();
  run.topped = true;
  run.top_space = projection.space;
  for (std::uint32_t i = 0; i < 2; ++i) {
    const auto& view = projection.views[i];
    Sphere s{};
    s.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
    s.flags = projection.layerFlags;
    s.space = projection.space;
    s.eye = i ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT;
    s.sub = view.subImage;
    s.pose = view.pose;
    s.fov = view.fov;
    s.flip = vflip;
    run.spheres.push_back(s);
    run.top_views[i] = {XR_TYPE_VIEW, nullptr, view.pose, view.fov};
  }
  static bool reported = false;
  if (!std::exchange(reported, true))
    log::info("xr", "Layers under a projection layer that blends by source alpha (underlays): drawing them and "
              "the projection into one projection layer");
  return XR_SUCCESS;
}
XrResult SphereLayers::present(XrSession handle, XrTime time,
                               std::vector<const XrCompositionLayerBaseHeader*>& layers,
                               std::optional<std::uint32_t> mirror_eye) {
  auto& p = presenter();
  std::vector<std::uintptr_t> used;
  std::set<Presenter::At> live;
  auto drop = [&](Run& run) {
    std::erase(layers, reinterpret_cast<const XrCompositionLayerBaseHeader*>(&run.projection));
  };
  if (!runs_.empty() && !p.size.width) {
    std::uint32_t count = 0;
    std::array<XrViewConfigurationView, 2> views{};
    for (auto& v : views) v = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
    if (XR_SUCCEEDED(::xrEnumerateViewConfigurationViews(host_xr().instance, host_xr().system,
                                                         session().view_configuration, 2, &count, views.data())) &&
        count == 2)
      p.size = {std::int32_t(std::max(views[0].recommendedImageRectWidth, views[1].recommendedImageRectWidth)),
                std::int32_t(std::max(views[0].recommendedImageRectHeight, views[1].recommendedImageRectHeight))};
    if (!p.size.width || !p.size.height) return XR_ERROR_RUNTIME_FAILURE;
  }
  XrResult qualified = XR_SUCCESS;
  p.draws.clear();
  p.acquired.clear();
  // Every acquired chain image goes back, whatever happened.
  auto release = [&](XrResult result) {
    for (const auto& held : p.acquired) {
      auto& chain = p.chains[held.chain];
      venture::xr::HostQueueLock queue(session().uses_shared_gpu);
      const auto released = !queue ? XR_ERROR_RUNTIME_FAILURE
                                   : venture::xr::dev_release_swapchain(chain.handle, nullptr,
                                                                        chain.host_images.at(held.target).image);
      if (XR_SUCCEEDED(result) && XR_FAILED(released)) result = released;
      if (released == XR_SESSION_LOSS_PENDING && result == XR_SUCCESS) result = released;
    }
    p.acquired.clear();
    return result;
  };
  for (std::size_t index = 0; index < runs_.size(); ++index) {
    auto& run = runs_[index];
    if (run.spheres.empty()) {
      drop(run);
      continue;
    }
    // A run under a projection is drawn in the projection's space; where the
    // host's views are not located, at the projection's own views.
    const XrSpace base = run.topped ? run.top_space : run.spheres.front().space;
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO, nullptr, session().view_configuration, time, base};
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    std::array<XrView, 2> views{};
    for (auto& v : views) v = {XR_TYPE_VIEW};
    std::uint32_t count = 0;
    constexpr auto located = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if (XR_FAILED(::xrLocateViews(handle, &locate, &view_state, 2, &count, views.data())) || count != 2 ||
        (view_state.viewStateFlags & located) != located) {
      if (!run.topped) {
        drop(run);
        continue;
      }
      views = run.top_views;
    }
    if (p.chains.size() <= index) p.chains.resize(index + 1);
    auto& chain = p.chains[index];
    chain.idle = 0;
    const auto format = state().swapchains.at(run.spheres.front().sub.swapchain).info.format;
    if (chain.handle && (chain.info.format != format || chain.info.width != std::uint32_t(p.size.width) ||
                         chain.info.height != std::uint32_t(p.size.height))) {
      if (auto result = chain.destroy(); XR_FAILED(result)) return release(result);
    }
    if (!chain.handle) {
      chain.info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
      chain.info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
      chain.info.format = format;
      chain.info.sampleCount = 1;
      chain.info.width = std::uint32_t(p.size.width);
      chain.info.height = std::uint32_t(p.size.height);
      chain.info.faceCount = 1;
      chain.info.arraySize = 2;
      chain.info.mipCount = 1;
      XrResult result;
      {
        venture::xr::HostQueueLock queue(session().uses_shared_gpu);
        if (!queue) return release(XR_ERROR_RUNTIME_FAILURE);
        result = state().gles ? venture::xr::create_host_swapchain(handle, chain.info, &chain.handle)
                              : venture::xr::dev_create_swapchain(handle, &chain.info, &chain.handle);
      }
      if (XR_FAILED(result)) {
        log::warn("xr", "Drawn layers: no host swapchain (result={}); leaving them out", int(result));
        chain.handle = XR_NULL_HANDLE;
        drop(run);
        continue;
      }
      std::uint32_t images = 0;
      result = ::xrEnumerateSwapchainImages(chain.handle, 0, &images, nullptr);
      if (XR_SUCCEEDED(result) && images) {
        chain.host_images.assign(images, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        result = ::xrEnumerateSwapchainImages(chain.handle, images, &images,
                                              reinterpret_cast<XrSwapchainImageBaseHeader*>(chain.host_images.data()));
      } else if (XR_SUCCEEDED(result)) {
        result = XR_ERROR_RUNTIME_FAILURE;
      }
      if (XR_FAILED(result)) {
        chain.destroy();
        return release(result);
      }
      if (!p.frame)
        p.frame = std::make_unique<venture::xr::LayerFrame>(state().graphics, gfx::gpu_device().get_instance_proc_addr);
      chain.images = std::make_unique<venture::xr::SphereImages>(state().graphics,
          gfx::gpu_device().get_instance_proc_addr, chain.info, chain.host_images, *p.frame);
    }
    const auto first = p.draws.size();
    chain.drawing.clear();
    XrCompositionLayerFlags aberration = 0;
    for (const auto& s : run.spheres) {
      XrPosef pose = s.pose;
      if (s.space != base) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        constexpr auto valid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if (XR_FAILED(::xrLocateSpace(s.space, base, time, &location)) ||
            (location.locationFlags & valid) != valid)
          continue;
        pose = compose(location.pose, s.pose);
      }
      auto& guest = state().swapchains.at(s.sub.swapchain);
      // A cube samples its six faces (all of the chain's layers); the others one layer.
      const bool cube = s.type == XR_TYPE_COMPOSITION_LAYER_CUBE_KHR;
      const auto at = Presenter::At{index, s.sub.swapchain, cube ? UINT32_MAX : s.sub.imageArrayIndex};
      auto [entry, inserted] = p.sources.try_emplace(at, Source{p.next_key});
      if (inserted) ++p.next_key;
      VkImage image{};
      bool fresh = false;
      const auto layers = cube ? guest.info.arraySize * guest.info.faceCount : 1u;
      auto result = chain.images->source(entry->second.key, VkFormat(guest.info.format),
                                         {guest.info.width, guest.info.height}, image, fresh, layers);
      if (XR_FAILED(result)) return release(result);
      if (fresh || entry->second.serial != guest.release_serial) {
        result = guest.backing->copy(std::uint32_t(guest.released_index), image, VkFormat(guest.info.format), {},
                                     false, cube ? -1 : std::int32_t(s.sub.imageArrayIndex));
        if (XR_FAILED(result)) return release(result);
        chain.images->filled(entry->second.key);
        entry->second.serial = guest.release_serial;
      }
      live.insert(at);
      used.push_back(entry->second.key);
      chain.drawing.push_back(entry->second.key);
      chain.drawing.push_back(entry->second.serial);
      aberration |= s.flags & XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT;
      for (std::uint32_t eye = 0; eye < 2; ++eye) {
        if (s.eye != XR_EYE_VISIBILITY_BOTH && s.eye != (eye ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT))
          continue;
        const auto where = placement(s, pose, guest);
        p.draws.push_back({entry->second.key, eye, venture::xr::layer_constants(where, views[eye], p.size),
                         venture::xr::layer_bounds(where, views[eye], p.size)});
      }
    }
    // Composited premultiplied (layer.frag) over the layers below.
    run.projection.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | aberration;
    run.projection.space = base;
    run.projection.viewCount = 2;
    run.projection.views = run.views.data();
    for (std::uint32_t eye = 0; eye < 2; ++eye)
      run.views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, nullptr, views[eye].pose, views[eye].fov,
                        {chain.handle, {{0, 0}, p.size}, eye}};
    // The draws and views as numbers: the same as last time, the host's image stands.
    for (std::size_t d = first; d < p.draws.size(); ++d) {
      const auto& draw = p.draws[d];
      chain.drawing.push_back(draw.source);
      chain.drawing.push_back(draw.eye);
      for (float c : draw.constants) chain.drawing.push_back(std::bit_cast<std::uint32_t>(c));
    }
    for (const auto& view : run.views) {
      const float numbers[]{view.pose.orientation.x, view.pose.orientation.y, view.pose.orientation.z,
                            view.pose.orientation.w, view.pose.position.x, view.pose.position.y,
                            view.pose.position.z, view.fov.angleLeft, view.fov.angleRight, view.fov.angleUp,
                            view.fov.angleDown};
      for (float n : numbers) chain.drawing.push_back(std::bit_cast<std::uint32_t>(n));
    }
    chain.drawing.push_back(run.projection.layerFlags);
    if (!chain.shown.empty() && chain.shown == chain.drawing) {
      p.draws.resize(first);
      continue;
    }
    chain.shown.clear();
    std::uint32_t target{};
    {
      venture::xr::HostQueueLock queue(session().uses_shared_gpu);
      if (!queue) return release(XR_ERROR_RUNTIME_FAILURE);
      const auto result = ::xrAcquireSwapchainImage(chain.handle, nullptr, &target);
      if (XR_FAILED(result)) return release(result);
      if (result == XR_SESSION_LOSS_PENDING) qualified = result;
    }
    p.acquired.push_back({index, target, first, p.draws.size()});
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    const auto waited = ::xrWaitSwapchainImage(chain.handle, &wait);
    if (waited != XR_SUCCESS && waited != XR_SESSION_LOSS_PENDING) return release(waited);
    if (waited == XR_SESSION_LOSS_PENDING) qualified = waited;
  }
  if (!p.acquired.empty()) {
    // Every run in one command buffer and one submission, after the sources'
    // copies on the same queue and before the chain images go back.
    const auto command = p.frame->begin();
    XrResult result = command ? XR_SUCCESS : XR_ERROR_RUNTIME_FAILURE;
    for (const auto& held : p.acquired) {
      if (XR_FAILED(result)) break;
      result = p.chains[held.chain].images->record(
          command, held.target, std::span(p.draws).subspan(held.first, held.last - held.first));
    }
    if (XR_SUCCEEDED(result)) result = p.frame->submit();
    else p.frame->abandon();
    // The desktop mirror shows the frame's first projection layer: when that
    // is a drawn run, its eye as drawn, before the image goes back.
    if (XR_SUCCEEDED(result) && mirror_eye) {
      const auto projection = std::ranges::find_if(layers, [](const XrCompositionLayerBaseHeader* layer) {
        return layer && layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION;
      });
      for (const auto& held : p.acquired) {
        if (projection == layers.end() ||
            *projection != reinterpret_cast<const XrCompositionLayerBaseHeader*>(&runs_[held.chain].projection))
          continue;
        const auto& chain = p.chains[held.chain];
        const auto eye = venture::xr::mirror_view(2, *mirror_eye);
        venture::xr::MirrorSource source{chain.host_images.at(held.target).image, VkFormat(chain.info.format), eye,
                                         {{0, 0}, p.size}};
        venture::xr::mirror_axis(runs_[held.chain].views[eye].fov, source.axis_x, source.axis_y);
        venture::xr::mirror_capture(session().binding, session().uses_shared_gpu, source);
      }
    }
    std::vector<std::size_t> drawn;
    for (const auto& held : p.acquired) drawn.push_back(held.chain);
    result = release(result);
    if (XR_FAILED(result)) return result;
    if (result == XR_SESSION_LOSS_PENDING) qualified = result;
    for (auto index : drawn) p.chains[index].shown = p.chains[index].drawing;
  }
  // Copies no longer drawn go. A chain unused for a second (a menu backdrop
  // gone in gameplay) returns its memory; a backdrop that comes and goes keeps it.
  // An idle chain keeps its copies (and the guest chains they follow) until it goes.
  for (std::size_t index = 0; index < p.chains.size(); ++index) {
    auto& chain = p.chains[index];
    const bool drawn = index < runs_.size();
    if (drawn) {
      if (chain.images) chain.images->retain(used);
    } else if (chain.handle && ++chain.idle > 90) {
      if (auto result = chain.destroy(); XR_FAILED(result)) return result;
    }
    for (auto it = p.sources.begin(); it != p.sources.end();) {
      const bool gone = std::get<0>(it->first) == index && (drawn ? !live.contains(it->first) : !chain.handle);
      it = gone ? p.sources.erase(it) : std::next(it);
    }
  }
  while (!p.chains.empty() && !p.chains.back().handle) p.chains.pop_back();
  return qualified;
}
XrResult forget_sphere_source(XrSwapchain handle) {
  auto& p = presenter();
  bool found = false;
  for (auto it = p.sources.begin(); it != p.sources.end();) {
    if (std::get<1>(it->first) != handle) { ++it; continue; }
    found = true;
    it = p.sources.erase(it);
  }
  if (!found) return XR_SUCCESS;
  for (std::size_t index = 0; index < p.chains.size(); ++index) {
    std::vector<std::uintptr_t> keys;
    for (const auto& [at, source] : p.sources)
      if (std::get<0>(at) == index) keys.push_back(source.key);
    auto& chain = p.chains[index];
    if (keys.empty()) {
      if (auto result = chain.destroy(); XR_FAILED(result)) return result;
    } else if (chain.images) {
      chain.images->retain(keys);
    }
  }
  while (!p.chains.empty() && !p.chains.back().handle) p.chains.pop_back();
  return XR_SUCCESS;
}
XrResult destroy_sphere_layers() {
  auto& p = presenter();
  XrResult result = XR_SUCCESS;
  for (auto& chain : p.chains)
    if (auto destroyed = chain.destroy(); XR_FAILED(destroyed)) result = destroyed;
  if (XR_SUCCEEDED(result)) {
    p.chains.clear();
    p.sources.clear();
    p.frame.reset();
    p.size = {};
  }
  return result;
}
} // namespace venture::hle::openxr
