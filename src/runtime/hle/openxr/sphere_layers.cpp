#include "hle/openxr/sphere_layers.h"
#include "hle/openxr/state.h"
#include "common/log.h"
#include "gfx/gpu_device.h"
#include "xr/dev_dump.h"
#include "xr/queue.h"
#include "xr/session.h"
#include "xr/sphere_images.h"
#include <algorithm>
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
  XrResult destroy() {
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
struct Presenter {
  std::vector<Chain> chains;
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
XrQuaternionf conjugate(const XrQuaternionf& q) { return {-q.x, -q.y, -q.z, q.w}; }
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
XrQuaternionf normalized(XrQuaternionf q) {
  const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (!(n > 0) || !std::isfinite(n)) return {0, 0, 0, 1};
  return {q.x / n, q.y / n, q.z / n, q.w / n};
}
// sphere.frag push constants for one sphere in one eye.
venture::xr::SphereImages::Constants constants(const SphereLayers::Sphere& s, const XrPosef& layer, const XrView& view,
                                               const Swapchain& chain, XrExtent2Di size) {
  venture::xr::SphereImages::Constants c{};
  const auto inverse = conjugate(normalized(layer.orientation));
  const auto q = multiply(inverse, normalized(view.pose.orientation));
  const auto eye = rotate(inverse, {view.pose.position.x - layer.position.x, view.pose.position.y - layer.position.y,
                                    view.pose.position.z - layer.position.z});
  const float m[3][3] = {
      {1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y - q.z * q.w), 2 * (q.x * q.z + q.y * q.w)},
      {2 * (q.x * q.y + q.z * q.w), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z - q.x * q.w)},
      {2 * (q.x * q.z - q.y * q.w), 2 * (q.y * q.z + q.x * q.w), 1 - 2 * (q.x * q.x + q.y * q.y)}};
  const float position[3] = {eye.x, eye.y, eye.z};
  for (int r = 0; r < 3; ++r) {
    for (int k = 0; k < 3; ++k) c[r * 4 + k] = m[r][k];
    c[r * 4 + 3] = position[r];
  }
  c[12] = std::tan(view.fov.angleLeft);
  c[13] = std::tan(view.fov.angleRight);
  c[14] = std::tan(view.fov.angleUp);
  c[15] = std::tan(view.fov.angleDown);
  // Zero or infinite radius: a sphere at infinity.
  const float radius = s.radius > 0 && std::isfinite(s.radius) ? s.radius : 0;
  if (s.v1) {
    c[16] = s.scale.x, c[17] = s.scale.y, c[18] = s.bias.x, c[19] = s.bias.y;
  } else {
    c[16] = s.central, c[17] = s.upper, c[18] = s.lower, c[19] = radius;
  }
  const float width = float(chain.info.width), height = float(chain.info.height);
  auto rect = s.sub.imageRect;
  // GLES rects count rows from the bottom; the private copy is top-down.
  if (!chain.textures.empty()) rect.offset.y = std::int32_t(chain.info.height) - rect.offset.y - rect.extent.height;
  c[20] = float(rect.offset.x) / width;
  c[21] = float(rect.offset.y) / height;
  c[22] = float(rect.extent.width) / width;
  c[23] = float(rect.extent.height) / height;
  c[24] = s.v1 ? 1.0f : 0.0f;
  c[25] = radius;
  c[26] = !(s.flags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT)      ? 0.0f
          : (s.flags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) ? 2.0f
                                                                        : 1.0f;
  c[28] = float(size.width);
  c[29] = float(size.height);
  return c;
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
  return type == XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR || type == XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR;
}
XrResult SphereLayers::add(const XrCompositionLayerBaseHeader* layer,
                           std::vector<const XrCompositionLayerBaseHeader*>& layers) {
  Sphere s{};
  if (layer->type == XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR) {
    const auto& e = *reinterpret_cast<const XrCompositionLayerEquirectKHR*>(layer);
    s = {e.layerFlags, e.space, e.eyeVisibility, e.subImage, e.pose, e.radius, true, e.scale, e.bias, 0, 0, 0};
  } else {
    const auto& e = *reinterpret_cast<const XrCompositionLayerEquirect2KHR*>(layer);
    s = {e.layerFlags, e.space, e.eyeVisibility, e.subImage, e.pose, e.radius, false, {}, {},
         e.centralHorizontalAngle, e.upperVerticalAngle, e.lowerVerticalAngle};
    if (!(e.centralHorizontalAngle > 0) || !(e.upperVerticalAngle > e.lowerVerticalAngle))
      return XR_ERROR_VALIDATION_FAILURE;
  }
  if (!valid_handle(s.sub.swapchain, "XrSwapchain")) return XR_ERROR_VALIDATION_FAILURE;
  const auto& chain = state().swapchains.at(s.sub.swapchain);
  if (chain.released_index < 0) return XR_ERROR_LAYER_INVALID;
  if (s.sub.imageArrayIndex >= chain.info.arraySize * chain.info.faceCount) return XR_ERROR_VALIDATION_FAILURE;
  if (chain.guest_only || chain.info.sampleCount != 1 || !color_format(VkFormat(chain.info.format))) {
    static bool reported = false;
    if (!std::exchange(reported, true))
      log::warn("xr", "An equirect layer's swapchain cannot be sampled (samples={} format={}); leaving it out",
                chain.info.sampleCount, chain.info.format);
    return XR_SUCCESS;
  }
  static std::set<XrStructureType> reported;
  if (reported.insert(layer->type).second)
    log::info("xr", "Host lacks {}; drawing layer type {} into a projection layer",
              layer->type == XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR ? "XR_KHR_composition_layer_equirect"
                                                                     : "XR_KHR_composition_layer_equirect2",
              int(layer->type));
  if (runs_.empty() || layers.empty() ||
      layers.back() != reinterpret_cast<const XrCompositionLayerBaseHeader*>(&runs_.back().projection)) {
    auto& run = runs_.emplace_back();
    layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&run.projection));
  }
  runs_.back().spheres.push_back(s);
  return XR_SUCCESS;
}
XrResult SphereLayers::present(XrSession handle, XrTime time,
                               std::vector<const XrCompositionLayerBaseHeader*>& layers) {
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
  for (std::size_t index = 0; index < runs_.size(); ++index) {
    auto& run = runs_[index];
    const XrSpace base = run.spheres.front().space;
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO, nullptr, session().view_configuration, time, base};
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    std::array<XrView, 2> views{};
    for (auto& v : views) v = {XR_TYPE_VIEW};
    std::uint32_t count = 0;
    constexpr auto located = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if (XR_FAILED(::xrLocateViews(handle, &locate, &view_state, 2, &count, views.data())) || count != 2 ||
        (view_state.viewStateFlags & located) != located) {
      drop(run);
      continue;
    }
    if (p.chains.size() <= index) p.chains.resize(index + 1);
    auto& chain = p.chains[index];
    chain.idle = 0;
    const auto format = state().swapchains.at(run.spheres.front().sub.swapchain).info.format;
    if (chain.handle && (chain.info.format != format || chain.info.width != std::uint32_t(p.size.width) ||
                         chain.info.height != std::uint32_t(p.size.height))) {
      if (auto result = chain.destroy(); XR_FAILED(result)) return result;
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
        if (!queue) return XR_ERROR_RUNTIME_FAILURE;
        result = state().gles ? venture::xr::create_host_swapchain(handle, chain.info, &chain.handle)
                              : venture::xr::dev_create_swapchain(handle, &chain.info, &chain.handle);
      }
      if (XR_FAILED(result)) {
        log::warn("xr", "Equirect layers: no host swapchain (result={}); leaving them out", int(result));
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
        return result;
      }
      chain.images = std::make_unique<venture::xr::SphereImages>(state().graphics,
          gfx::gpu_device().get_instance_proc_addr, chain.info, chain.host_images);
    }
    std::vector<venture::xr::SphereImages::Draw> draws;
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
      const auto at = Presenter::At{index, s.sub.swapchain, s.sub.imageArrayIndex};
      auto [entry, inserted] = p.sources.try_emplace(at, Source{p.next_key});
      if (inserted) ++p.next_key;
      VkImage image{};
      bool fresh = false;
      auto result = chain.images->source(entry->second.key, VkFormat(guest.info.format),
                                         {guest.info.width, guest.info.height}, image, fresh);
      if (XR_FAILED(result)) return result;
      if (fresh || entry->second.serial != guest.release_serial) {
        result = guest.backing->copy(std::uint32_t(guest.released_index), image, VkFormat(guest.info.format), {},
                                     false, std::int32_t(s.sub.imageArrayIndex));
        if (XR_FAILED(result)) return result;
        chain.images->filled(entry->second.key);
        entry->second.serial = guest.release_serial;
      }
      live.insert(at);
      used.push_back(entry->second.key);
      aberration |= s.flags & XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT;
      for (std::uint32_t eye = 0; eye < 2; ++eye) {
        if (s.eye != XR_EYE_VISIBILITY_BOTH && s.eye != (eye ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT))
          continue;
        draws.push_back({entry->second.key, eye, constants(s, pose, views[eye], guest, p.size)});
      }
    }
    std::uint32_t target{};
    {
      venture::xr::HostQueueLock queue(session().uses_shared_gpu);
      if (!queue) return XR_ERROR_RUNTIME_FAILURE;
      const auto result = ::xrAcquireSwapchainImage(chain.handle, nullptr, &target);
      if (XR_FAILED(result)) return result;
      if (result == XR_SESSION_LOSS_PENDING) qualified = result;
    }
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    auto result = ::xrWaitSwapchainImage(chain.handle, &wait);
    if (result != XR_SUCCESS && result != XR_SESSION_LOSS_PENDING) {
      chain.destroy();
      return result;
    }
    if (result == XR_SESSION_LOSS_PENDING) qualified = result;
    result = chain.images->render(target, draws);
    {
      // Always balance the acquisition.
      venture::xr::HostQueueLock queue(session().uses_shared_gpu);
      if (!queue) return XR_ERROR_RUNTIME_FAILURE;
      const auto released = venture::xr::dev_release_swapchain(chain.handle, nullptr, chain.host_images.at(target).image);
      if (XR_SUCCEEDED(result)) result = released;
    }
    if (XR_FAILED(result)) return result;
    if (result == XR_SESSION_LOSS_PENDING) qualified = result;
    // Composited premultiplied (sphere.frag) over the layers below.
    run.projection.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | aberration;
    run.projection.space = base;
    run.projection.viewCount = 2;
    run.projection.views = run.views.data();
    for (std::uint32_t eye = 0; eye < 2; ++eye)
      run.views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, nullptr, views[eye].pose, views[eye].fov,
                        {chain.handle, {{0, 0}, p.size}, eye}};
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
    p.size = {};
  }
  return result;
}
} // namespace venture::hle::openxr
