#include "hle/vrapi/drawn_layers.h"
#include "common/log.h"
#include "gfx/gpu_device.h"
#include "xr/dev_dump.h"
#include "xr/queue.h"
#include "xr/session.h"
#include <algorithm>
#include <memory>

namespace venture::hle::vrapi {
namespace {
// One host chain per run of the frame (two array layers, one per eye).
struct Chain {
  XrSwapchain handle{};
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  std::unique_ptr<venture::xr::SphereImages> images;
  std::vector<XrSwapchainImageVulkanKHR> host_images;
  std::uint32_t idle = 0; // frames without a run
  XrResult destroy() {
    images.reset(); // waits for its commands
    if (!handle) return XR_SUCCESS;
    venture::xr::HostQueueLock queue(session().uses_shared_gpu);
    if (!queue) return XR_ERROR_RUNTIME_FAILURE;
    const auto result = venture::xr::dev_destroy_swapchain(handle);
    if (XR_SUCCEEDED(result)) handle = XR_NULL_HANDLE, host_images.clear();
    return result;
  }
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
  XrExtent2Di size{};
  // Reused from frame to frame.
  std::vector<venture::xr::SphereImages::Draw> draws;
  std::vector<Acquired> acquired;
};
Presenter &presenter() {
  static Presenter p;
  return p;
}
std::uintptr_t key(VkImage image) { return (std::uintptr_t)image; }
} // namespace
void DrawnLayers::add(const DrawnLayer &layer, std::vector<const XrCompositionLayerBaseHeader *> &layers) {
  if (runs_.empty() || layers.empty() || runs_.back().layers.front().space != layer.space ||
      layers.back() != reinterpret_cast<const XrCompositionLayerBaseHeader *>(&runs_.back().projection)) {
    auto &run = runs_.emplace_back();
    layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&run.projection));
  }
  runs_.back().layers.push_back(layer);
}
XrResult DrawnLayers::gather(const Sources &sources, std::vector<const XrCompositionLayerBaseHeader *> &layers) {
  for (auto run = runs_.begin(); run != runs_.end();) {
    run->sources.clear();
    for (auto layer = run->layers.begin(); layer != run->layers.end();) {
      std::array<Source, 2> found{};
      XrResult result = XR_SUCCESS;
      for (int eye = 0; eye < 2 && XR_SUCCEEDED(result); ++eye)
        result = sources(layer->eyes[eye].chain, layer->eyes[eye].slot, found[eye]);
      if (result == XR_ERROR_FEATURE_UNSUPPORTED) {
        layer = run->layers.erase(layer);
        continue;
      }
      if (XR_FAILED(result)) return result;
      run->sources.push_back(found);
      ++layer;
    }
    if (run->layers.empty()) {
      std::erase(layers, reinterpret_cast<const XrCompositionLayerBaseHeader *>(&run->projection));
      run = runs_.erase(run);
    } else {
      ++run;
    }
  }
  return XR_SUCCESS;
}
XrResult DrawnLayers::present(std::vector<const XrCompositionLayerBaseHeader *> &layers) {
  auto &p = presenter();
  if (runs_.empty() && p.chains.empty()) return XR_SUCCESS;
  auto drop = [&](Run &run) {
    std::erase(layers, reinterpret_cast<const XrCompositionLayerBaseHeader *>(&run.projection));
  };
  if (!runs_.empty() && !p.size.width) {
    std::uint32_t count = 0;
    std::array<XrViewConfigurationView, 2> views{};
    for (auto &v : views) v = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
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
    for (const auto &held : p.acquired) {
      auto &chain = p.chains[held.chain];
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
    auto &run = runs_[index];
    const XrSpace base = run.layers.front().space;
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO, nullptr, session().view_configuration,
                            session().frame.predictedDisplayTime, base};
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    std::array<XrView, 2> views{};
    for (auto &v : views) v = {XR_TYPE_VIEW};
    std::uint32_t count = 0;
    constexpr auto located = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if (XR_FAILED(::xrLocateViews(session().handle, &locate, &view_state, 2, &count, views.data())) || count != 2 ||
        (view_state.viewStateFlags & located) != located) {
      drop(run);
      continue;
    }
    if (p.chains.size() <= index) p.chains.resize(index + 1);
    auto &chain = p.chains[index];
    chain.idle = 0;
    const auto format = run.sources.front()[0].format;
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
        result = venture::xr::create_host_swapchain(session().handle, chain.info, &chain.handle);
      }
      if (XR_FAILED(result)) {
        log::warn("vrapi", "Drawn layers: no host swapchain (result={}); leaving them out", int(result));
        chain.handle = XR_NULL_HANDLE;
        drop(run);
        continue;
      }
      std::uint32_t images = 0;
      result = ::xrEnumerateSwapchainImages(chain.handle, 0, &images, nullptr);
      if (XR_SUCCEEDED(result) && images) {
        chain.host_images.assign(images, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        result = ::xrEnumerateSwapchainImages(chain.handle, images, &images,
                                              reinterpret_cast<XrSwapchainImageBaseHeader *>(chain.host_images.data()));
      } else if (XR_SUCCEEDED(result)) {
        result = XR_ERROR_RUNTIME_FAILURE;
      }
      if (XR_FAILED(result)) {
        chain.destroy();
        return release(result);
      }
      if (!p.frame)
        p.frame = std::make_unique<venture::xr::LayerFrame>(session().binding, gfx::gpu_device().get_instance_proc_addr);
      chain.images = std::make_unique<venture::xr::SphereImages>(
          session().binding, gfx::gpu_device().get_instance_proc_addr, chain.info, chain.host_images, *p.frame);
    }
    const auto first = p.draws.size();
    for (std::size_t i = 0; i < run.layers.size(); ++i)
      for (std::uint32_t eye = 0; eye < 2; ++eye) {
        const auto &source = run.sources[i][eye];
        auto result = chain.images->external(key(source.image), source.image, source.format, source.layers);
        if (XR_FAILED(result)) return release(result);
        const auto &where = run.layers[i].eyes[eye].placement;
        p.draws.push_back({key(source.image), eye, venture::xr::layer_constants(where, views[eye], p.size),
                           venture::xr::layer_bounds(where, views[eye], p.size)});
      }
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
    // Composited premultiplied (layer.frag) over the layers below.
    run.projection.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    run.projection.space = base;
    run.projection.viewCount = 2;
    run.projection.views = run.views.data();
    for (std::uint32_t eye = 0; eye < 2; ++eye)
      run.views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, nullptr, views[eye].pose, views[eye].fov,
                        {chain.handle, {{0, 0}, p.size}, eye}};
  }
  if (!p.acquired.empty()) {
    // Every run in one command buffer and one submission, after the sources'
    // copies on the same queue and before the chain images go back.
    const auto command = p.frame->begin();
    XrResult result = command ? XR_SUCCESS : XR_ERROR_RUNTIME_FAILURE;
    for (const auto &held : p.acquired) {
      if (XR_FAILED(result)) break;
      result = p.chains[held.chain].images->record(
          command, held.target, std::span(p.draws).subspan(held.first, held.last - held.first));
    }
    if (XR_SUCCEEDED(result)) result = p.frame->submit();
    else p.frame->abandon();
    result = release(result);
    if (XR_FAILED(result)) return result;
    if (result == XR_SESSION_LOSS_PENDING) qualified = result;
  }
  // A chain unused for 90 frames (a menu gone in gameplay) returns its memory.
  for (std::size_t index = runs_.size(); index < p.chains.size(); ++index) {
    auto &chain = p.chains[index];
    if (chain.handle && ++chain.idle > 90)
      if (auto result = chain.destroy(); XR_FAILED(result)) return result;
  }
  while (!p.chains.empty() && !p.chains.back().handle) p.chains.pop_back();
  return qualified;
}
void forget_drawn_sources(std::span<const VkImage> images) {
  for (auto &chain : presenter().chains)
    if (chain.images) chain.images->forget(images);
}
std::uint64_t drawn_layers_gpu_ns() {
  auto &p = presenter();
  return p.frame ? p.frame->last_gpu_ns() : 0;
}
XrResult destroy_drawn_layers() {
  auto &p = presenter();
  XrResult result = XR_SUCCESS;
  for (auto &chain : p.chains)
    if (auto destroyed = chain.destroy(); XR_FAILED(destroyed)) result = destroyed;
  if (XR_SUCCEEDED(result)) {
    p.chains.clear();
    p.frame.reset();
    p.size = {};
  }
  return result;
}
} // namespace venture::hle::vrapi
