#include "common/log.h"
#include "hle/openxr/state.h"
#include "hle/openxr/passthrough.h"
#include "xr/clock.h"
#include "xr/queue.h"
#include "gfx/gpu_device.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace venture::hle::openxr {
XrResult refresh_rate(XrSession handle, float *out) {
  if (!out)
    return XR_ERROR_VALIDATION_FAILURE;
  std::lock_guard sl(session().mutex);
  if (handle && host_xr().enabled_extensions.contains("XR_FB_display_refresh_rate")) {
    float rate = 0;
    auto result = host_call<PFN_xrGetDisplayRefreshRateFB, "xrGetDisplayRefreshRateFB">(handle, &rate);
    // Before the host session runs it may not expose the mode yet. Keep the
    // same shared fallback as Android until the first period is available.
    if (XR_FAILED(result) && result != XR_ERROR_SESSION_NOT_RUNNING) return result;
    if (XR_SUCCEEDED(result)) {
      if (!std::isfinite(rate) || rate <= 0) return XR_ERROR_RUNTIME_FAILURE;
      session().observe_display_period(std::int64_t(std::llround(1e9 / rate)),
                                       xr::DisplayTiming::Source::host);
    }
  }
  *out = session().display_timing.rate();
  return XR_SUCCESS;
}
namespace guest {
namespace {
void rejected_swapchain_state(const char* operation, const XrSwapchainStateBaseHeaderFB* header) {
  // Callers hold the guest API mutex. Report each shape once, without turning
  // a repeated sampler probe into per-frame log traffic.
  static std::set<std::tuple<std::string_view, int, bool, bool, bool, bool>> reported;
  const auto type = header ? int(header->type) : 0;
  const bool next = header && header->next;
  const bool gles = enabled("XR_FB_swapchain_update_state_opengl_es");
  const bool vulkan = enabled("XR_FB_swapchain_update_state_vulkan");
  const bool foveation = enabled("XR_FB_foveation");
  if (reported.emplace(operation, type, next, gles, vulkan, foveation).second)
    log::warn("xr", "{} rejected state: type={} next={} sampler_gles_enabled={} sampler_vulkan_enabled={} foveation_enabled={}",
              operation, type, next, gles, vulkan, foveation);
}
}  // namespace
XrResult xrGetRecommendedLayerResolutionMETA(XrSession handle,
    const XrRecommendedLayerResolutionGetInfoMETA* info,
    XrRecommendedLayerResolutionMETA* out) {
  std::lock_guard lock(state().mutex);
  enum class HandleField : unsigned { session, space, swapchain };
  const auto invalid = [](HandleField field, bool null_handle, int layer_type) {
    // Identify the rejected field without logging opaque handles or repeating
    // a diagnostic on every frame. The guest API mutex protects these six bits.
    static unsigned reported = 0;
    constexpr const char* names[]{"session", "space", "swapchain"};
    const auto index = static_cast<unsigned>(field);
    const auto bit = 1u << (2 * index + unsigned(null_handle));
    if (!(reported & bit)) {
      reported |= bit;
      log::warn("xr", "xrGetRecommendedLayerResolutionMETA rejected handle: field={} null={} layer_type={}",
                names[index], null_handle, layer_type);
    }
    return XR_ERROR_HANDLE_INVALID;
  };
  if (!valid_handle(handle, "XrSession")) return invalid(HandleField::session, !handle, 0);
  if (!enabled("XR_META_recommended_layer_resolution")) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!info || info->type != XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_GET_INFO_META ||
      !out || out->type != XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_META || !info->layer)
    return XR_ERROR_VALIDATION_FAILURE;
  auto result = validate_chain(info->next, false, info->type);
  if (XR_FAILED(result)) return result;
  result = validate_chain(out->next, false, out->type);
  if (XR_FAILED(result)) return result;
  {
    std::lock_guard sl(session().mutex);
    const auto latest = std::max(session().frame.predictedDisplayTime,
        session().pending_wait ? session().pending_frame.predictedDisplayTime : XrTime{});
    if (info->predictedDisplayTime <= 0 || info->predictedDisplayTime < latest)
      return XR_ERROR_TIME_INVALID;
  }
  const auto* layer = info->layer;
  if (layer->type == XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB) {
    result = validate_passthrough_layer(handle, layer);
    if (XR_FAILED(result)) return result;
    out->isValid = XR_FALSE;
    out->recommendedImageDimensions = {0, 0};
    return XR_SUCCESS;
  }
  if (!valid_handle(layer->space, "XrSpace")) return invalid(HandleField::space, !layer->space, int(layer->type));
  // The layer is only inspected here. Its own type must be enabled, while its
  // next chain ignores unknown and unenabled nodes (OpenXR Â§2.9.7); OVRPlugin
  // chains XrCompositionLayerImageLayoutFB without enabling that extension.
  XrCompositionLayerBaseHeader header = *layer;
  header.next = nullptr;
  result = validate_chain(&header, false);
  if (XR_SUCCEEDED(result)) result = validate_chain(layer->next, false, layer->type, true);
  if (XR_FAILED(result)) return result;
  std::vector<XrSwapchain> images;
  float aspect = 0;
  switch (layer->type) {
    case XR_TYPE_COMPOSITION_LAYER_PROJECTION: {
      const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
      if (projection->viewCount != 2 || !projection->views) return XR_ERROR_VALIDATION_FAILURE;
      for (std::uint32_t i = 0; i < projection->viewCount; ++i) {
        if (projection->views[i].type != XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW)
          return XR_ERROR_VALIDATION_FAILURE;
        images.push_back(projection->views[i].subImage.swapchain);
      }
      break;
    }
    case XR_TYPE_COMPOSITION_LAYER_QUAD: {
      const auto* quad = reinterpret_cast<const XrCompositionLayerQuad*>(layer);
      if (!(quad->size.width > 0) || !(quad->size.height > 0) ||
          !std::isfinite(quad->size.width) || !std::isfinite(quad->size.height))
        return XR_ERROR_VALIDATION_FAILURE;
      aspect = quad->size.width / quad->size.height;
      if (!(aspect > 0) || !std::isfinite(aspect)) return XR_ERROR_VALIDATION_FAILURE;
      images.push_back(quad->subImage.swapchain);
      break;
    }
    case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR: {
      const auto* cylinder = reinterpret_cast<const XrCompositionLayerCylinderKHR*>(layer);
      aspect = cylinder->aspectRatio;
      if (!(aspect > 0) || !std::isfinite(aspect)) return XR_ERROR_VALIDATION_FAILURE;
      images.push_back(cylinder->subImage.swapchain);
      break;
    }
    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:
      images.push_back(reinterpret_cast<const XrCompositionLayerEquirectKHR*>(layer)->subImage.swapchain);
      aspect = 2;
      break;
    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR: {
      const auto* sphere = reinterpret_cast<const XrCompositionLayerEquirect2KHR*>(layer);
      aspect = sphere->centralHorizontalAngle / (sphere->upperVerticalAngle - sphere->lowerVerticalAngle);
      if (!(aspect > 0) || !std::isfinite(aspect)) return XR_ERROR_VALIDATION_FAILURE;
      images.push_back(sphere->subImage.swapchain);
      break;
    }
    case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR:
      images.push_back(reinterpret_cast<const XrCompositionLayerCubeKHR*>(layer)->swapchain);
      aspect = 1;
      break;
    default:
      out->isValid = XR_FALSE;
      out->recommendedImageDimensions = {0, 0};
      return XR_SUCCESS;
  }
  bool unbounded = false;
  std::uint32_t limit_width = UINT32_MAX, limit_height = UINT32_MAX;
  for (auto image : images) {
    if (!image) { unbounded = true; continue; }
    auto found = state().swapchains.find(image);
    if (found == state().swapchains.end() || state().objects.at({"XrSwapchain", key(image)}).parent != key(handle))
      return invalid(HandleField::swapchain, false, int(layer->type));
    limit_width = std::min(limit_width, found->second.info.width);
    limit_height = std::min(limit_height, found->second.info.height);
  }
  // XR_META_recommended_layer_resolution permits a fixed quality recommendation.
  // Use the real runtime's stereo render target as the pixel budget, preserving
  // non-projection aspect ratios. A null swapchain requests allocation sizing;
  // only an entirely allocated layer must fit every existing swapchain.
  // https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrGetRecommendedLayerResolutionMETA.html
  // Both answers are fixed for a host system (OpenXR 1.1 sections 5.4 and 8.2), and titles ask every
  // frame; a host round trip per call is IPC on Monado. The guest API mutex protects the cache.
  struct Budget { XrInstance instance{}; XrSystemId system{}; std::uint64_t generation{}; std::uint32_t width{}, height{},
                  max_width{}, max_height{}; };
  static Budget budget;
  const auto generation = host_procedure_generation();
  if (budget.instance != host_xr().instance || budget.system != host_xr().system || budget.generation != generation ||
      !budget.width) {
    std::uint32_t count{};
    result = host_call<PFN_xrEnumerateViewConfigurationViews, "xrEnumerateViewConfigurationViews">(host_xr().instance, host_xr().system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr);
    if (XR_FAILED(result)) return result;
    std::vector<XrViewConfigurationView> views(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    result = host_call<PFN_xrEnumerateViewConfigurationViews, "xrEnumerateViewConfigurationViews">(host_xr().instance, host_xr().system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        count, &count, views.data());
    if (XR_FAILED(result)) return result;
    Budget fresh{host_xr().instance, host_xr().system, generation};
    for (const auto& view : views) {
      fresh.width = std::max(fresh.width, view.recommendedImageRectWidth);
      fresh.height = std::max(fresh.height, view.recommendedImageRectHeight);
    }
    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
    result = host_call<PFN_xrGetSystemProperties, "xrGetSystemProperties">(host_xr().instance, host_xr().system, &properties);
    if (XR_FAILED(result)) return result;
    fresh.max_width = properties.graphicsProperties.maxSwapchainImageWidth;
    fresh.max_height = properties.graphicsProperties.maxSwapchainImageHeight;
    budget = fresh;
  }
  std::uint32_t width = budget.width, height = budget.height;
  // A zero maximum is no limit (the Oculus PC runtime reports 0x0).
  if (unbounded && budget.max_width && budget.max_height) {
    limit_width = budget.max_width;
    limit_height = budget.max_height;
  }
  if (aspect) {
    // Keep the runtime's pixel budget rather than inventing per-title sizes.
    const double pixels = double(width) * height;
    const auto desired_width = std::sqrt(pixels * aspect);
    const auto desired_height = std::sqrt(pixels / aspect);
    const auto scale = std::min({1.0, limit_width / desired_width, limit_height / desired_height});
    width = std::uint32_t(std::floor(desired_width * scale));
    height = std::uint32_t(std::floor(desired_height * scale));
  }
  width = std::min(width, limit_width);
  height = std::min(height, limit_height);
  out->isValid = width && height ? XR_TRUE : XR_FALSE;
  out->recommendedImageDimensions = out->isValid ? XrExtent2Di{std::int32_t(width), std::int32_t(height)}
                                                : XrExtent2Di{0, 0};
  return XR_SUCCESS;
}
XrResult xrGetDisplayRefreshRateFB(XrSession handle, float *out) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(handle, "XrSession"))
    return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_FB_display_refresh_rate"))
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  return refresh_rate(handle, out);
}
XrResult xrEnumerateDisplayRefreshRatesFB(XrSession handle, std::uint32_t capacity,
                                          std::uint32_t *count, float *out) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(handle, "XrSession"))
    return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_FB_display_refresh_rate"))
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!count || (capacity && !out))
    return XR_ERROR_VALIDATION_FAILURE;
  float rate = 0;
  auto result = refresh_rate(handle, &rate);
  if (XR_FAILED(result)) {
    *count = 0;
    return result;
  }
  *count = 1;
  if (capacity)
    out[0] = rate;
  return XR_SUCCESS;
}
XrResult xrRequestDisplayRefreshRateFB(XrSession handle, float rate) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(handle, "XrSession"))
    return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_FB_display_refresh_rate"))
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!std::isfinite(rate) || rate < 0)
    return XR_ERROR_VALIDATION_FAILURE;
  if (rate == 0)
    return XR_SUCCESS;
  float current = 0;
  auto result = refresh_rate(handle, &current);
  if (XR_FAILED(result))
    return result;
  return std::abs(current - rate) < 0.01f ? XR_SUCCESS
                                          : XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB;
}
XrResult xrEnumerateColorSpacesFB(XrSession handle, std::uint32_t capacity, std::uint32_t *count,
                                  XrColorSpaceFB *out) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(handle, "XrSession"))
    return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_FB_color_space"))
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!count || (capacity && !out))
    return XR_ERROR_VALIDATION_FAILURE;
  auto report = [&] {
    if (!state().color_spaces_logged) {
      log::info("xr", "xrEnumerateColorSpacesFB advertised count={}", *count);
      state().color_spaces_logged = true;
    }
  };
  if (host_xr().enabled_extensions.contains("XR_FB_color_space")) {
    auto result = host_call<PFN_xrEnumerateColorSpacesFB, "xrEnumerateColorSpacesFB">(
        handle, capacity, count, out);
    if (XR_SUCCEEDED(result) || result == XR_ERROR_SIZE_INSUFFICIENT)
      report();
    return result;
  }
  *count = std::uint32_t(color_spaces.size());
  report();
  if (!capacity) return XR_SUCCESS;
  if (capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT;
  std::copy(color_spaces.begin(), color_spaces.end(), out);
  return XR_SUCCESS;
}
XrResult xrSetColorSpaceFB(XrSession handle, XrColorSpaceFB color) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(handle, "XrSession"))
    return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_FB_color_space"))
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (host_xr().enabled_extensions.contains("XR_FB_color_space")) {
    auto result = host_call<PFN_xrSetColorSpaceFB, "xrSetColorSpaceFB">(handle, color);
    if (XR_SUCCEEDED(result)) {
      if (!state().color_space_set_logged || state().color_space != color)
        log::info("xr", "xrSetColorSpaceFB color={} source=host", int(color));
      state().color_space_set_logged = true;
      state().color_space = color;
    } else
      log::warn("xr", "xrSetColorSpaceFB returned {} for color={} source=host", int(result), int(color));
    return result;
  }
  if (!valid_color_space(color)) {
    log::warn("xr", "xrSetColorSpaceFB returned {} for color={} source=translation",
              int(XR_ERROR_COLOR_SPACE_UNSUPPORTED_FB), int(color));
    return XR_ERROR_COLOR_SPACE_UNSUPPORTED_FB;
  }
  if (!state().color_space_set_logged || state().color_space != color)
    log::info("xr", "Application color space={} host=Rec709", int(color));
  state().color_space_set_logged = true;
  state().color_space = color;
  return XR_SUCCESS;
}
XrResult xrCreateFoveationProfileFB(XrSession handle, const XrFoveationProfileCreateInfoFB *info,
                                    XrFoveationProfileFB *out) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(handle, "XrSession"))
    return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_FB_foveation"))
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!out || !info || info->type != XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB)
    return XR_ERROR_VALIDATION_FAILURE;
  *out = XR_NULL_HANDLE;
  auto profile = std::make_unique<Foveation>(
      Foveation{XR_FOVEATION_LEVEL_NONE_FB, 0, XR_FOVEATION_DYNAMIC_DISABLED_FB});
  std::set<const void*> seen;
  std::set<XrStructureType> types;
  for (auto p = static_cast<const XrBaseInStructure*>(info->next); p; p = p->next) {
    if (!seen.insert(p).second || seen.size() > 64) return XR_ERROR_VALIDATION_FAILURE;
    const StructureInfo* metadata = nullptr;
    for (const auto& m : structure_metadata()) if (m.type == p->type) { metadata = &m; break; }
    // OpenXR Valid Usage for Structure Pointer Chains: skip disabled and
    // unknown nodes without reading their payloads, and use the first type.
    if (!metadata || !types.insert(p->type).second) continue;
    if (*metadata->extension && !enabled(metadata->extension)) continue;
    if (!extends_structure(*metadata, XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB))
      return XR_ERROR_VALIDATION_FAILURE;
    if (p->type != XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB) {
      log::warn("xr", "Unsupported enabled foveation structure {}", metadata->name);
      return XR_ERROR_FEATURE_UNSUPPORTED;
    }
    const auto* level = reinterpret_cast<const XrFoveationLevelProfileCreateInfoFB*>(p);
    if (level->level < XR_FOVEATION_LEVEL_NONE_FB || level->level > XR_FOVEATION_LEVEL_HIGH_FB ||
        !std::isfinite(level->verticalOffset) ||
        level->dynamic < XR_FOVEATION_DYNAMIC_DISABLED_FB ||
        level->dynamic > XR_FOVEATION_DYNAMIC_LEVEL_ENABLED_FB)
      return XR_ERROR_VALIDATION_FAILURE;
    *profile = {level->level, level->verticalOffset, level->dynamic};
  }
  *out = reinterpret_cast<XrFoveationProfileFB>(profile.get());
  state().foveation.emplace(*out, std::move(profile));
  track_handle(*out, "XrFoveationProfileFB", handle);
  return XR_SUCCESS;
}
XrResult xrDestroyFoveationProfileFB(XrFoveationProfileFB profile) {
  std::lock_guard lock(state().mutex);
  if (!state().foveation.contains(profile))
    return XR_ERROR_HANDLE_INVALID;
  // Swapchains retain their selected quality state after profile destruction.
  for (auto &[h, chain] : state().swapchains)
    if (chain.profile == profile)
      chain.profile = XR_NULL_HANDLE;
  state().foveation.erase(profile);
  forget_handle(profile);
  return XR_SUCCESS;
}
XrResult xrUpdateSwapchainFB(XrSwapchain handle, const XrSwapchainStateBaseHeaderFB *header) {
  std::lock_guard lock(state().mutex);
  auto it = state().swapchains.find(handle);
  if (it == state().swapchains.end())
    return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_FB_swapchain_update_state"))
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (header && !header->next) {
    const bool vk = header->type == XR_TYPE_SWAPCHAIN_STATE_SAMPLER_VULKAN_FB;
    const bool gl = header->type == XR_TYPE_SWAPCHAIN_STATE_SAMPLER_OPENGL_ES_FB;
    // Reference-runtime applications use these structs with only the base
    // extension enabled. Accept the session's graphics API shape, retaining
    // strict field validation and rejecting the other graphics API's shape.
    if ((vk && !state().gles) || (gl && state().gles)) {
      XrSwapchainStateSamplerVulkanFB sampler{};
      if (vk) {
        sampler = *reinterpret_cast<const XrSwapchainStateSamplerVulkanFB*>(header);
        if (!valid_sampler(sampler)) return XR_ERROR_VALIDATION_FAILURE;
      } else if (!gles_sampler(*reinterpret_cast<const SamplerOpenGLES*>(header), sampler))
        return XR_ERROR_VALIDATION_FAILURE;
      if (gl && xr_images()) {
        auto result = xr_images()->update_sampler(it->second.textures, it->second.desc,
            *reinterpret_cast<const SamplerOpenGLES*>(header));
        if (XR_FAILED(result)) return result;
      }
      XrResult result;
      {
        std::unique_lock queue(gfx::gpu_device().queue_lock, std::defer_lock);
        if (session().uses_shared_gpu) queue.lock();
        HostQueueLock host_queue(session().uses_shared_gpu);
        result = host_queue ? (it->second.host_handle ? update_host_sampler(it->second.host_handle, sampler)
                                                    : XR_SUCCESS) : XR_ERROR_RUNTIME_FAILURE;
      }
      if (XR_FAILED(result)) {
        if (gl && xr_images())
          xr_images()->update_sampler(it->second.textures, it->second.desc, it->second.sampler_gles);
        return result;
      }
      it->second.sampler_host = sampler;
      it->second.sampler_updated = true;
      if (vk) it->second.sampler_vulkan = sampler;
      else it->second.sampler_gles = *reinterpret_cast<const SamplerOpenGLES*>(header);
      return XR_SUCCESS;
    }
  }
  if (!header || header->type != XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB || header->next ||
      !enabled("XR_FB_foveation")) {
    rejected_swapchain_state("xrUpdateSwapchainFB", header);
    return XR_ERROR_VALIDATION_FAILURE;
  }
  auto f = reinterpret_cast<const XrSwapchainStateFoveationFB *>(header);
  if (f->profile && !state().foveation.contains(f->profile))
    return XR_ERROR_HANDLE_INVALID;
  if (f->flags)
    return XR_ERROR_VALIDATION_FAILURE;
  it->second.profile = f->profile;
  it->second.foveation_flags = f->flags;
  return XR_SUCCESS;
}
XrResult xrGetSwapchainStateFB(XrSwapchain handle, XrSwapchainStateBaseHeaderFB *header) {
  std::lock_guard lock(state().mutex);
  auto it = state().swapchains.find(handle);
  if (it == state().swapchains.end())
    return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_FB_swapchain_update_state"))
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (header && !header->next) {
    if (header->type == XR_TYPE_SWAPCHAIN_STATE_SAMPLER_VULKAN_FB && !state().gles) {
      *reinterpret_cast<XrSwapchainStateSamplerVulkanFB*>(header) = it->second.sampler_vulkan;
      return XR_SUCCESS;
    }
    if (header->type == XR_TYPE_SWAPCHAIN_STATE_SAMPLER_OPENGL_ES_FB && state().gles) {
      *reinterpret_cast<SamplerOpenGLES*>(header) = it->second.sampler_gles;
      return XR_SUCCESS;
    }
  }
  if (!header || header->type != XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB || header->next ||
      !enabled("XR_FB_foveation")) {
    rejected_swapchain_state("xrGetSwapchainStateFB", header);
    return XR_ERROR_VALIDATION_FAILURE;
  }
  auto f = reinterpret_cast<XrSwapchainStateFoveationFB *>(header);
  f->profile = it->second.profile;
  f->flags = it->second.foveation_flags;
  return XR_SUCCESS;
}
// Lock-free when instance is the live instance with the extension enabled;
// otherwise the error code from the locked handle and extension checks.
static XrResult check_timespec_instance(XrInstance instance) {
  if (instance && instance == state().timespec_instance.load(std::memory_order_acquire))
    return XR_SUCCESS;
  std::lock_guard lock(state().mutex);
  if (!valid_handle(instance, "XrInstance"))
    return XR_ERROR_HANDLE_INVALID;
  return enabled("XR_KHR_convert_timespec_time") ? XR_SUCCESS : XR_ERROR_FUNCTION_UNSUPPORTED;
}
XrResult xrConvertTimespecTimeToTimeKHR(XrInstance instance, const Timespec *ts, XrTime *out) {
  if (auto result = check_timespec_instance(instance); XR_FAILED(result))
    return result;
  if (!ts || !out || ts->nsec < 0 || ts->nsec >= 1'000'000'000 || ts->sec < 0 ||
      ts->sec > (std::numeric_limits<std::int64_t>::max() - ts->nsec) / 1'000'000'000)
    return XR_ERROR_TIME_INVALID;
  return monotonic_to_time(ts->sec * 1'000'000'000 + ts->nsec, out);
}
XrResult xrConvertTimeToTimespecTimeKHR(XrInstance instance, XrTime time, Timespec *out) {
  if (auto result = check_timespec_instance(instance); XR_FAILED(result))
    return result;
  if (!out)
    return XR_ERROR_VALIDATION_FAILURE;
  std::int64_t ns = 0;
  auto result = time_to_monotonic(time, &ns);
  if (XR_SUCCEEDED(result)) {
    out->sec = ns / 1'000'000'000;
    out->nsec = ns % 1'000'000'000;
  }
  return result;
}
} // namespace guest
} // namespace venture::hle::openxr
