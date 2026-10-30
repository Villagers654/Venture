#include "core/xr_call_trace.h"
#include <atomic>
#include "xr/haptic_sequence.h"
#include "xr/haptic_output.h"
#include "xr/menu_chord.h"
#include "xr/clock.h"
#include "common/first_frame_progress.h"
#include "common/log.h"
#include "core/engine_policy.h"
#include "core/process.h"
#include "core/thread.h"
#include "core/shutdown_trace.h"
#include "gfx/gpu_device.h"
#include "hle/openxr/state.h"
#include "hle/vulkan/vulkan.h"
#include "hle/vulkan/timing.h"
#include "platform/thread_priority.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <numbers>

namespace venture::hle::openxr {
namespace {
// XR_KHR_opengl_es_enable: a GLES subimage rect has its origin at the image's
// bottom-left corner. The GLES image bridge flips every image vertically
// (gfx/xr_images.cpp), so the host must sample the mirrored rows. Host copies
// of layer structures get y' = height - y - h for each XrSwapchainSubImage of a
// bridged swapchain, including projection views and their next chains.
// XR_FB_composition_layer_image_layout is applied here, not by the host: a
// VERTICAL_FLIP layer's chains are copied with reversed rows, which mirrors
// their rects again, and the node never reaches the host.
// Guest handles are replaced by lazy host presentation handles in these
// copies; application structures and private image handles stay unchanged.
class PresentationLayers {
 public:
  explicit PresentationLayers(bool host = false) : host_(host) {}
  std::set<XrSwapchain> submitted, flipped;
  // One application layer; collects its swapchains into `flipped` on request.
  const void* layer(const void* node) {
    flip_ = false;
    return chain(node);
  }
  const void* chain(const void* node, bool explicit_depth = false) {
    if (!node) return nullptr;
    auto base = static_cast<const XrBaseInStructure*>(node);
    if (base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW && full_rate_composition()) {
      explicit_depth = false;
      if (enabled("XR_KHR_composition_layer_depth"))
        for (auto* p = base->next; p; p = p->next)
          if (p->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) explicit_depth = true;
    }
    return structure(base, next(base->next, explicit_depth), explicit_depth);
  }

 private:
  bool host_, flip_ = false;
  static bool guest_only_depth(const XrBaseInStructure* p) {
    XrSwapchain chain{};
    if (p->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR)
      chain = reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(p)->subImage.swapchain;
    else if (p->type == XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB)
      chain = reinterpret_cast<const XrCompositionLayerSpaceWarpInfoFB*>(p)->depthSubImage.swapchain;
    else
      return false;
    const auto found = state().swapchains.find(chain);
    return found != state().swapchains.end() && found->second.guest_only;
  }
  // OpenXR "Valid Usage for Structure Pointer Chains": runtimes ignore
  // next-chain nodes of extensions the application did not enable. Host
  // copies omit them so the host never reads them either; OVRPlugin attaches
  // XrCompositionLayerSpaceWarpInfoFB without enabling XR_FB_space_warp.
  const void* next(const void* node, bool explicit_depth) {
    // The collection pass already filtered guest nodes and may have synthesized
    // host-native depth. Preserve that normalized chain during handle mapping.
    if (host_) return chain(node, explicit_depth);
    for (auto p = static_cast<const XrBaseInStructure*>(node); p; p = p->next) {
      if (p->type == XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB) {
        if (enabled("XR_FB_composition_layer_image_layout") &&
            (reinterpret_cast<const XrCompositionLayerImageLayoutFB*>(p)->flags &
             XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB))
          flip_ = true;
        continue;
      }
      // Depth in a chain the host has no format for stays the guest's.
      if (guest_only_depth(p)) continue;
      const StructureInfo* metadata = nullptr;
      for (const auto& m : structure_metadata())
        if (m.type == p->type) { metadata = &m; break; }
      if (!metadata && full_rate_composition()) continue;
      if (!metadata || !*metadata->extension || enabled(metadata->extension)) return chain(p, explicit_depth);
    }
    return nullptr;
  }
  // Per structure type: its size and the offsets of XrSwapchainSubImage members
  // and of projection-view arrays with their uint32 count, from registry metadata.
  struct Fields {
    std::size_t size = 0;
    std::vector<std::size_t> subimages;
    std::vector<std::size_t> swapchains;
    std::vector<std::pair<std::size_t, std::size_t>> views;  // pointer, count
  };
  static const Fields* fields(XrStructureType type) {
    static const auto table = [] {
      std::map<XrStructureType, Fields> out;
      for (const auto& s : structure_metadata()) {
        Fields f{s.size};
        for (const auto& m : member_metadata()) {
          if (std::string_view(m.structure) != s.name) continue;
          if (!m.pointer && std::string_view(m.type) == "XrSwapchainSubImage")
            f.subimages.push_back(m.offset);
          else if (!m.pointer && std::string_view(m.type) == "XrSwapchain")
            f.swapchains.push_back(m.offset);
          else if (m.pointer && m.length && std::string_view(m.type) == "XrCompositionLayerProjectionView")
            for (const auto& count : member_metadata())
              if (std::string_view(count.structure) == s.name && std::string_view(count.name) == m.length &&
                  count.size == sizeof(std::uint32_t))
                f.views.emplace_back(m.offset, count.offset);
        }
        out.emplace(s.type, std::move(f));
      }
      return out;
    }();
    auto it = table.find(type);
    return it == table.end() ? nullptr : &it->second;
  }
  const void* structure(const XrBaseInStructure* source, const void* next, bool explicit_depth = false) {
    if (source->type == XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB &&
        full_rate_composition() && !host_xr().enabled_extensions.contains("XR_FB_space_warp")) {
      // Every application frame is presented at the host's native cadence.
      // Preserve usable depth for native reprojection; never forward a mobile
      // motion-vector structure to a host that did not enable its extension.
      // An explicit KHR depth node takes precedence over the lower-resolution
      // depth carried for motion-vector extrapolation.
      if (explicit_depth) return next;
      for (auto* p = static_cast<const XrBaseInStructure*>(next); p; p = p->next)
        if (p->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) return next;
      if (!host_xr().enabled_extensions.contains("XR_KHR_composition_layer_depth")) return next;
      const auto& warp = *reinterpret_cast<const XrCompositionLayerSpaceWarpInfoFB*>(source);
      const auto found = state().swapchains.find(warp.depthSubImage.swapchain);
      const auto& rect = warp.depthSubImage.imageRect;
      if (found == state().swapchains.end() ||
          warp.depthSubImage.imageArrayIndex >= found->second.info.arraySize ||
          rect.offset.x < 0 || rect.offset.y < 0 || rect.extent.width <= 0 || rect.extent.height <= 0 ||
          std::int64_t(rect.offset.x) + rect.extent.width > found->second.info.width ||
          std::int64_t(rect.offset.y) + rect.extent.height > found->second.info.height ||
          !std::isfinite(warp.minDepth) || !std::isfinite(warp.maxDepth) ||
          warp.minDepth < 0 || warp.maxDepth > 1 || warp.minDepth > warp.maxDepth ||
          !(warp.nearZ > 0) || !(warp.farZ > 0) || warp.nearZ == warp.farZ) {
        static std::atomic_flag reported = ATOMIC_FLAG_INIT;
        if (!reported.test_and_set(std::memory_order_relaxed))
          log::warn("xr", "Ignoring unusable optional motion depth on a full-rate host");
        return next;
      }
      XrCompositionLayerDepthInfoKHR depth{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR, next,
          warp.depthSubImage, warp.minDepth, warp.maxDepth, warp.nearZ, warp.farZ};
      auto& node = storage_.emplace_back(sizeof(depth));
      std::memcpy(node.data(), &depth, sizeof(depth));
      return structure(reinterpret_cast<const XrBaseInStructure*>(node.data()), next);
    }
    const auto* f = fields(source->type);
    if (!f) return source;
    const auto* bytes = reinterpret_cast<const std::byte*>(source);
    std::vector<std::byte>* copy = nullptr;
    auto writable = [&] {
      if (!copy) {
        copy = &storage_.emplace_back(f->size);
        std::memcpy(copy->data(), source, f->size);
      }
      return copy->data();
    };
    if (next != source->next)
      reinterpret_cast<XrBaseInStructure*>(writable())->next = static_cast<const XrBaseInStructure*>(next);
    for (auto offset : f->subimages) {
      XrSwapchainSubImage sub;
      std::memcpy(&sub, bytes + offset, sizeof(sub));
      if (map_subimage(sub)) std::memcpy(writable() + offset, &sub, sizeof(sub));
    }
    for (auto offset : f->swapchains) {
      XrSwapchain handle;
      std::memcpy(&handle, bytes + offset, sizeof(handle));
      if (state().swapchains.contains(handle)) {
        submitted.insert(handle);
        if (flip_) flipped.insert(handle);
        if (host_) {
          handle = state().swapchains.at(handle).host_handle;
          std::memcpy(writable() + offset, &handle, sizeof(handle));
        }
      }
    }
    for (auto [pointer, length] : f->views) {
      const XrCompositionLayerProjectionView* views;
      std::uint32_t count;
      std::memcpy(&views, bytes + pointer, sizeof(views));
      std::memcpy(&count, bytes + length, sizeof(count));
      if (!views || !count) continue;
      std::vector<const void*> mapped(count);
      bool changed = false;
      for (std::uint32_t i = 0; i < count; ++i)
        changed |= (mapped[i] = chain(&views[i])) != &views[i];
      if (!changed) continue;
      auto& array = storage_.emplace_back(count * sizeof(*views));
      for (std::uint32_t i = 0; i < count; ++i)
        std::memcpy(array.data() + i * sizeof(*views), mapped[i], sizeof(*views));
      const void* copied = array.data();
      std::memcpy(writable() + pointer, &copied, sizeof(copied));
    }
    return copy ? static_cast<const void*>(copy->data()) : source;
  }
  bool map_subimage(XrSwapchainSubImage& sub) {
    auto it = state().swapchains.find(sub.swapchain);
    if (it == state().swapchains.end()) return false;
    submitted.insert(sub.swapchain);
    if (flip_) flipped.insert(sub.swapchain);
    if (!host_) return false;
    sub.swapchain = it->second.host_handle;
    // Unreal's Meta OpenXR plugin can round a view's crop a pixel or two past
    // the swapchain's right or bottom edge. Quest's compositor accepts that;
    // desktop runtimes reject the whole frame (XR_ERROR_SWAPCHAIN_RECT_INVALID),
    // so the overshoot is clipped. A rect that starts outside the image stays as
    // submitted for the host to reject.
    if (auto& r = sub.imageRect; r.offset.x >= 0 && r.offset.y >= 0 &&
        r.offset.x < std::int64_t(it->second.info.width) && r.offset.y < std::int64_t(it->second.info.height)) {
      const auto width = std::min<std::int64_t>(r.extent.width, it->second.info.width - std::int64_t(r.offset.x));
      const auto height = std::min<std::int64_t>(r.extent.height, it->second.info.height - std::int64_t(r.offset.y));
      if (width != r.extent.width || height != r.extent.height) {
        if (!std::exchange(it->second.rect_clipped, true))
          log::warn("xr", "Clipping a layer rect {}x{}+{}+{} to its {}x{} swapchain", r.extent.width,
                    r.extent.height, r.offset.x, r.offset.y, it->second.info.width, it->second.info.height);
        r.extent = {std::int32_t(width), std::int32_t(height)};
      }
    }
    if (it->second.textures.empty() == it->second.host_flip) {
      const auto y = std::int64_t(it->second.info.height) - sub.imageRect.offset.y -
                     sub.imageRect.extent.height;
      // An out-of-bounds rect stays as submitted for the host to reject.
      if (y < 0 || y > INT32_MAX) return false;
      sub.imageRect.offset.y = static_cast<std::int32_t>(y);
    }
    // A guest chain over the host maximum is presented through a smaller host
    // chain (prepare_swapchains): map the rect's edges into its pixels.
    const auto& guest = it->second.info;
    const auto& host = it->second.host_info;
    if (host.width && host.height && (host.width != guest.width || host.height != guest.height)) {
      const auto edge = [](std::int64_t value, std::uint32_t from, std::uint32_t to) {
        return std::int32_t((value * to + from / 2) / from);
      };
      auto& rect = sub.imageRect;
      const auto x0 = edge(rect.offset.x, guest.width, host.width);
      const auto y0 = edge(rect.offset.y, guest.height, host.height);
      const auto x1 = edge(std::int64_t(rect.offset.x) + rect.extent.width, guest.width, host.width);
      const auto y1 = edge(std::int64_t(rect.offset.y) + rect.extent.height, guest.height, host.height);
      rect.offset = {x0, y0};
      rect.extent = {std::max(1, x1 - x0), std::max(1, y1 - y0)};
    }
    return true;
  }
  std::deque<std::vector<std::byte>> storage_;
};

// An owned deep copy of a host frame submission, for a host xrEndFrame made
// after the guest call returns (xr::Session asynchronous submission). Nodes are
// copied by their registry size. A structure with a non-null pointer member
// other than next and its projection views is not copied: that frame submits
// synchronously.
class OwnedFrameEnd {
 public:
  bool copy(const XrFrameEndInfo& source) {
    info = source;
    layers_.resize(source.layerCount);
    for (std::uint32_t i = 0; i < source.layerCount; ++i)
      layers_[i] = static_cast<const XrCompositionLayerBaseHeader*>(node(source.layers[i]));
    info.next = node(source.next);
    info.layers = layers_.data();
    return ok_;
  }
  XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};

 private:
  struct Pointer { std::size_t offset, count = 0; bool next = false, views = false; };
  struct Layout { std::size_t size = 0; std::vector<Pointer> pointers; };
  static const Layout* layout(XrStructureType type) {
    static const auto table = [] {
      std::map<XrStructureType, Layout> out;
      for (const auto& s : structure_metadata()) {
        Layout l{s.size};
        for (const auto& m : member_metadata()) {
          if (!m.pointer || std::string_view(m.structure) != s.name) continue;
          Pointer p{m.offset};
          p.next = std::string_view(m.name) == "next";
          if (!p.next && std::string_view(m.type) == "XrCompositionLayerProjectionView" && *m.length)
            for (const auto& count : member_metadata())
              if (std::string_view(count.structure) == s.name && std::string_view(count.name) == m.length &&
                  count.size == sizeof(std::uint32_t)) {
                p.views = true;
                p.count = count.offset;
              }
          l.pointers.push_back(p);
        }
        out.emplace(s.type, std::move(l));
      }
      return out;
    }();
    auto it = table.find(type);
    return it == table.end() ? nullptr : &it->second;
  }
  const void* node(const void* source) {
    if (!source || !ok_) return nullptr;
    const auto* l = layout(static_cast<const XrBaseInStructure*>(source)->type);
    if (!l) { ok_ = false; return nullptr; }
    auto& bytes = storage_.emplace_back(l->size);
    std::memcpy(bytes.data(), source, l->size);
    for (const auto& p : l->pointers) {
      const void* pointer;
      std::memcpy(&pointer, bytes.data() + p.offset, sizeof(pointer));
      if (!pointer) continue;
      if (p.next) {
        pointer = node(pointer);
      } else if (p.views) {
        std::uint32_t count;
        std::memcpy(&count, bytes.data() + p.count, sizeof(count));
        auto& array = storage_.emplace_back(count * sizeof(XrCompositionLayerProjectionView));
        std::memcpy(array.data(), pointer, array.size());
        auto* views = reinterpret_cast<XrCompositionLayerProjectionView*>(array.data());
        for (std::uint32_t i = 0; i < count; ++i)
          views[i].next = node(views[i].next);
        pointer = array.data();
      } else {
        ok_ = false;
      }
      std::memcpy(bytes.data() + p.offset, &pointer, sizeof(pointer));
    }
    return bytes.data();
  }
  std::vector<const XrCompositionLayerBaseHeader*> layers_;
  std::deque<std::vector<std::byte>> storage_;
  bool ok_ = true;
};

}  // namespace
}  // namespace venture::hle::openxr

namespace venture::hle::openxr::guest {
XrResult xrEnumerateApiLayerProperties(std::uint32_t capacity, std::uint32_t *count,
                                       XrApiLayerProperties *properties) {
  if (!count || (capacity && !properties))
    return XR_ERROR_VALIDATION_FAILURE;
  *count = 0;
  return XR_SUCCESS;
}
XrResult xrEnumerateInstanceExtensionProperties(const char *layer, std::uint32_t capacity,
                                                std::uint32_t *count, XrExtensionProperties *out) {
  std::lock_guard lock(state().mutex);
  if (layer && *layer)
    return XR_ERROR_API_LAYER_NOT_PRESENT;
  if (!count || (capacity && !out))
    return XR_ERROR_VALIDATION_FAILURE;
  auto caps = capabilities();
  *count = static_cast<std::uint32_t>(caps.size());
  if (!capacity)
    return XR_SUCCESS;
  if (capacity < caps.size())
    return XR_ERROR_SIZE_INSUFFICIENT;
  std::uint32_t i = 0;
  for (const auto &[name, version] : caps) {
    if (out[i].type != XR_TYPE_EXTENSION_PROPERTIES || out[i].next)
      return XR_ERROR_VALIDATION_FAILURE;
    std::strcpy(out[i].extensionName, name.c_str());
    out[i++].extensionVersion = version;
  }
  return XR_SUCCESS;
}
XrResult xrInitializeLoaderKHR(const XrLoaderInitInfoBaseHeaderKHR *header) {
  std::lock_guard lock(state().mutex);
  if (!header || header->type != XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR || header->next)
    return XR_ERROR_VALIDATION_FAILURE;
  const auto &info = *reinterpret_cast<const AndroidInit *>(header);
  if (!info.vm || !info.context)
    return XR_ERROR_VALIDATION_FAILURE;
  state().vm = info.vm;
  state().context = info.context;
  log::debug("xr", "Stored guest Android loader context");
  return XR_SUCCESS;
}
XrResult xrCreateInstance(const XrInstanceCreateInfo *info, XrInstance *out) {
  std::lock_guard lock(state().mutex);
  if (!out)
    return XR_ERROR_VALIDATION_FAILURE;
  *out = XR_NULL_HANDLE;
  if (!info || info->type != XR_TYPE_INSTANCE_CREATE_INFO || info->createFlags)
    return XR_ERROR_VALIDATION_FAILURE;
  const auto minor = XR_VERSION_MINOR(info->applicationInfo.apiVersion);
  // xrLocateSpaces has a 1.0-host fallback. LOCAL_FLOOR is the additional
  // mandatory reference space in 1.1, so require its real host implementation.
  if (XR_VERSION_MAJOR(info->applicationInfo.apiVersion) != 1 || minor > 1 ||
      (minor == 1 && !host_xr().enabled_extensions.contains("XR_EXT_local_floor"))) {
    log::error("xr", "Unsupported guest OpenXR version {}.{}.{}",
               XR_VERSION_MAJOR(info->applicationInfo.apiVersion), minor,
               XR_VERSION_PATCH(info->applicationInfo.apiVersion));
    return XR_ERROR_API_VERSION_UNSUPPORTED;
  }
  if (state().instance_live)
    return XR_ERROR_LIMIT_REACHED;
  if (info->enabledApiLayerCount)
    return XR_ERROR_API_LAYER_NOT_PRESENT;
  if (info->enabledExtensionCount && !info->enabledExtensionNames)
    return XR_ERROR_VALIDATION_FAILURE;
  const auto caps = capabilities();
  std::set<std::string> requested;
  for (std::uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
    const char *name = info->enabledExtensionNames[i];
    if (!name)
      return XR_ERROR_VALIDATION_FAILURE;
    log::info("xr", "Guest requests {}", name);
    if (!caps.contains(name))
      return XR_ERROR_EXTENSION_NOT_PRESENT;
    requested.insert(name);
  }
  // OVRPlugin requests both sampler extensions for a Vulkan-only instance.
  // Accept the unused API's declaration: Get/Update still require the actual
  // session graphics binding, and both sampler extensions need the base API.
  for (const char* sampler : {"XR_FB_swapchain_update_state_vulkan",
                             "XR_FB_swapchain_update_state_opengl_es"}) {
    if (requested.contains(sampler) && !requested.contains("XR_FB_swapchain_update_state"))
      return XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED;
  }
  if (requested.contains("XR_FB_foveation") && !requested.contains("XR_FB_swapchain_update_state"))
    return XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED;
  if (requested.contains("XR_FB_foveation_configuration") && !requested.contains("XR_FB_foveation"))
    return XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED;
  if (info->next) {
    auto android = static_cast<const AndroidInstance *>(info->next);
    if (android->type != XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR || android->next ||
        !requested.contains("XR_KHR_android_create_instance") || !android->vm || !android->activity)
      return XR_ERROR_VALIDATION_FAILURE;
    state().vm = android->vm;
    state().context = android->activity;
  }
  auto &h = host_xr();
  if (!h.instance)
    return XR_ERROR_RUNTIME_UNAVAILABLE;
  state().enabled = std::move(requested);
  state().api_version = info->applicationInfo.apiVersion;
  state().instance_live = true;
  *out = h.instance;
  track_handle(*out, "XrInstance", static_cast<XrInstance>(nullptr));
  state().timespec_instance.store(enabled("XR_KHR_convert_timespec_time") ? *out : XrInstance{},
                                  std::memory_order_release);
  invalidate_host_procedures();
  // Runtime-gate query logging is deliberately separate from host identity.
  log::info("xr", "Guest instance created for {} on {}", process_config().package, h.runtime_name);
  return XR_SUCCESS;
}
XrResult xrDestroyInstance(XrInstance instance) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(instance, "XrInstance"))
    return XR_ERROR_HANDLE_INVALID;
  if (session().owner == SessionOwner::openxr && session().handle) {
    auto result = guest::xrDestroySession(session().handle);
    if (XR_FAILED(result))
      return result;
  }
  std::vector<XrActionSet> sets;
  for (const auto &[h, o] : state().objects)
    if (o.type == "XrActionSet")
      sets.push_back(reinterpret_cast<XrActionSet>(h.second));
  for (auto set : sets) {
    auto r = ::xrDestroyActionSet(set);
    if (XR_FAILED(r))
      return r;
  }
  state().timespec_instance.store(XrInstance{}, std::memory_order_release);
  forget_handle(instance);
  state().enabled.clear();
  state().instance_live = false;
  state().guest_profiles.clear();
  state().guest_sources.clear();
  state().events.clear();
  invalidate_host_procedures();
  if (shutdown_trace::enabled) log::info("shutdown", "xrDestroyInstance returned success");
  return XR_SUCCESS;
}
XrResult xrGetInstanceProperties(XrInstance instance, XrInstanceProperties *out) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(instance, "XrInstance"))
    return XR_ERROR_HANDLE_INVALID;
  if (XR_FAILED(validate_struct(out, XR_TYPE_INSTANCE_PROPERTIES)))
    return XR_ERROR_VALIDATION_FAILURE;
  auto result = ::xrGetInstanceProperties(instance, out);
  if (XR_SUCCEEDED(result)) {
    // Verified in owned V-Speedway OVRPlugin SHA256
    // eba967f29a2bfec7a3e2c5b8cee6671406373d0689b9d9dc9c821ffe4e8ac32a:
    // @0x1b5794 sets XR_TYPE_INSTANCE_PROPERTIES (32), @0x1b57b0 queries it,
    // @0x1b595c passes runtimeName (+24) to @0x1b7450. The helper lowercases
    // up to 128 bytes and strstr-tests "oculus" (@0x8303d) or "meta" (@0x90d02).
    // Failed test branches @0x1b5964 to the rejection at @0x1b689c. r4-xr's
    // candidate reference is thus a runtimeName gate, not an extension test.
    std::snprintf(out->runtimeName, sizeof(out->runtimeName), "%s", "Oculus");
    log::debug("xr", "Guest runtime identity queried: {}", out->runtimeName);
  }
  return result;
}
XrResult xrGetSystem(XrInstance instance, const XrSystemGetInfo *info, XrSystemId *out) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(instance, "XrInstance"))
    return XR_ERROR_HANDLE_INVALID;
  if (!out || XR_FAILED(validate_struct(info, XR_TYPE_SYSTEM_GET_INFO)))
    return XR_ERROR_VALIDATION_FAILURE;
  if (info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY)
    return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
  *out = host_xr().system;
  return static_cast<XrResult>(host_xr().system_result);
}
XrResult xrGetSystemProperties(XrInstance instance, XrSystemId system, XrSystemProperties *out) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(instance, "XrInstance"))
    return XR_ERROR_HANDLE_INVALID;
  if (!out || out->type != XR_TYPE_SYSTEM_PROPERTIES)
    return XR_ERROR_VALIDATION_FAILURE;
  // The fallback reports its assumed Rec709 display; native color-space
  // properties remain in the host chain when the host supports the extension.
  // Clone understood nodes, then copy back payload only, preserving next pointers.
  struct Node {
    XrBaseOutStructure *guest;
    std::vector<std::uint64_t> storage;
    std::size_t size;
  };
  std::vector<Node> nodes;
  std::set<void *> seen;
  std::set<XrStructureType> types;
  for (auto p = static_cast<XrBaseOutStructure *>(out->next); p; p = p->next) {
    if (!seen.insert(p).second || seen.size() > 64)
      return XR_ERROR_VALIDATION_FAILURE;
    // OpenXR fundamentals, "Valid Usage for Structure Pointer Chains": skip
    // unrecognized/disabled nodes and process only the first occurrence of a
    // type. Loader/API layers may prepend a replacement for an existing node.
    if (!types.insert(p->type).second) continue;
    // The guest identity's headset id, never the host headset's.
    if (p->type == XR_TYPE_SYSTEM_HEADSET_ID_PROPERTIES_META && enabled("XR_META_headset_id")) {
      const auto &id = process_config().device_identity->headset_id;
      std::memcpy(reinterpret_cast<XrSystemHeadsetIdPropertiesMETA *>(p)->id.data, id.data(), id.size());
      continue;
    }
    if (p->type == XR_TYPE_SYSTEM_COLOR_SPACE_PROPERTIES_FB && enabled("XR_FB_color_space") &&
        !host_xr().enabled_extensions.contains("XR_FB_color_space")) {
      reinterpret_cast<XrSystemColorSpacePropertiesFB *>(p)->colorSpace = XR_COLOR_SPACE_REC709_FB;
      continue;
    }
    if (p->type == XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB && enabled("XR_FB_space_warp") &&
        full_rate_composition() && !host_xr().enabled_extensions.contains("XR_FB_space_warp")) {
      XrViewConfigurationView views[2]{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
      std::uint32_t count{};
      const auto result = ::xrEnumerateViewConfigurationViews(instance, system,
          XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, views);
      if (XR_FAILED(result)) return result;
      if (count != 2) return XR_ERROR_FEATURE_UNSUPPORTED;
      auto& properties = *reinterpret_cast<XrSystemSpaceWarpPropertiesFB*>(p);
      // Recommendations do not constrain application swapchain sizes. Native
      // full-rate composition does not extrapolate motion, so avoid inducing
      // a full-eye auxiliary pass that the host never consumes.
      const auto motion_size = [](std::uint32_t eye) {
        return ((std::max(16u, eye / 8) + 7u) / 8u) * 8u;
      };
      properties.recommendedMotionVectorImageRectWidth = motion_size(std::max(
          views[0].recommendedImageRectWidth, views[1].recommendedImageRectWidth));
      properties.recommendedMotionVectorImageRectHeight = motion_size(std::max(
          views[0].recommendedImageRectHeight, views[1].recommendedImageRectHeight));
      continue;
    }
    const StructureInfo *m = nullptr;
    for (auto &s : structure_metadata())
      if (s.type == p->type)
        m = &s;
    // OpenXR 1.0 section 2.9.7: unknown and unenabled next-chain nodes
    // must be ignored, including output chains. Leave their bytes untouched.
    if (!m || (*m->extension &&
               (!enabled(m->extension) || !host_xr().enabled_extensions.contains(m->extension)))) {
      log::debug("xr", "Skipping unavailable system property structure {}", int(p->type));
      continue;
    }
    if (!extends_structure(*m, XR_TYPE_SYSTEM_PROPERTIES))
      return XR_ERROR_VALIDATION_FAILURE;
    nodes.push_back({p, std::vector<std::uint64_t>((m->size + 7) / 8), m->size});
    std::memcpy(nodes.back().storage.data(), p, m->size);
  }
  void *chain = nullptr;
  for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
    auto *node = reinterpret_cast<XrBaseOutStructure *>(it->storage.data());
    node->next = static_cast<XrBaseOutStructure *>(chain);
    chain = node;
  }
  auto copy = *out;
  copy.next = chain;
  auto result = ::xrGetSystemProperties(instance, system, &copy);
  if (XR_SUCCEEDED(result)) {
    for (auto &n : nodes) {
      auto next = n.guest->next;
      std::memcpy(n.guest, n.storage.data(), n.size);
      n.guest->next = next;
    }
    copy.next = out->next;
    *out = copy;
    out->vendorId = 0x2833; // Oculus vendor identity, independent of host GPU/runtime.
    // Some hosts report zero for limits and tracking they don't enforce or
    // describe (SteamVR 2.18 with a Quest over Link); a guest gets what a Quest has.
    auto& graphics = out->graphicsProperties;
    if (!graphics.maxLayerCount) graphics.maxLayerCount = XR_MIN_COMPOSITION_LAYERS_SUPPORTED;
    if (!graphics.maxSwapchainImageWidth || !graphics.maxSwapchainImageHeight) {
      std::uint32_t count = 0;
      XrViewConfigurationView views[2]{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
      if (XR_SUCCEEDED(::xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                           2, &count, views)) && count) {
        graphics.maxSwapchainImageWidth = views[0].maxImageRectWidth;
        graphics.maxSwapchainImageHeight = views[0].maxImageRectHeight;
      }
    }
    if (!out->trackingProperties.orientationTracking && !out->trackingProperties.positionTracking)
      out->trackingProperties.orientationTracking = out->trackingProperties.positionTracking = XR_TRUE;
    const auto name = process_config().device_identity->system_name;
    std::snprintf(out->systemName, sizeof(out->systemName), "%.*s",
                  static_cast<int>(name.size()), name.data());
    log::debug("xr", "Guest system identity queried: {}", out->systemName);
  }
  return result;
}
XrResult xrCreateSession(XrInstance instance, const XrSessionCreateInfo *info, XrSession *out) {
  // Engine SDK policy runs guest code, so it reads the negotiated state first
  // and applies it without the OpenXR locks.
  bool space_warp;
  {
    std::lock_guard lock(state().mutex);
    space_warp = enabled("XR_FB_space_warp") &&
        host_xr().enabled_extensions.contains("XR_FB_space_warp");
  }
  if (auto *thread = GuestThread::current()) engine_policy::on_xr_session(*thread, space_warp);
  std::lock_guard lock(state().mutex);
  std::lock_guard session_lock(session().mutex);
  if (!valid_handle(instance, "XrInstance"))
    return XR_ERROR_HANDLE_INVALID;
  if (!out || !info || info->type != XR_TYPE_SESSION_CREATE_INFO || info->createFlags ||
      !info->next)
    return XR_ERROR_VALIDATION_FAILURE;
  *out = XR_NULL_HANDLE;
  if (info->systemId != host_xr().system)
    return XR_ERROR_SYSTEM_INVALID;
  auto b = static_cast<const XrBaseInStructure *>(info->next);
  if (b->next)
    return XR_ERROR_VALIDATION_FAILURE;
  bool gles = b->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR;
  if (gles) {
    auto binding = reinterpret_cast<const GlesBinding *>(b);
    if (!enabled("XR_KHR_opengl_es_enable") || !xr_images())
      return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    if (!binding->context || !binding->display)
      return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  } else if (b->type != XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR ||
             (!enabled("XR_KHR_vulkan_enable") && !enabled("XR_KHR_vulkan_enable2"))) {
    return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  }
  // The host runtime binds the host queue behind the guest's queue index.
  XrGraphicsBindingVulkanKHR vulkan_binding{};
  if (!gles) {
    vulkan_binding = *reinterpret_cast<const XrGraphicsBindingVulkanKHR *>(b);
    vulkan_binding.queueIndex = vulkan::host_queue_index(vulkan_binding.device, vulkan_binding.queueFamilyIndex,
                                                         vulkan_binding.queueIndex);
  }
  auto result = session().create(SessionOwner::openxr, gles ? nullptr : &vulkan_binding);
  if (XR_SUCCEEDED(result)) {
    *out = session().handle;
    state().gles = gles;
    state().color_space = XR_COLOR_SPACE_REC709_FB;
    state().color_spaces_logged = state().color_space_set_logged = false;
    if (gles) {
      auto& gpu = gfx::gpu_device();
      state().graphics = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR, nullptr, gpu.instance,
          gpu.physical_device, gpu.device, gpu.queue_family, gpu.queue_index};
    } else {
      state().graphics = vulkan_binding;
      state().guest_graphics_queue_index = reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(b)->queueIndex;
    }
    track_handle(*out, "XrSession", instance);
  }
  return result;
}
XrResult xrDestroySession(XrSession handle) {
  std::lock_guard lock(state().mutex);
  std::lock_guard session_lock(session().mutex);
  if (!valid_handle(handle, "XrSession"))
    return XR_ERROR_HANDLE_INVALID;
  for (const auto& [chain, image] : state().swapchains)
    if (image.operation_in_progress) return XR_ERROR_CALL_ORDER_INVALID;
  while (!state().swapchains.empty()) {
    auto result = guest::xrDestroySwapchain(state().swapchains.begin()->first);
    if (XR_FAILED(result))
      return result;
  }
  venture::xr::cancel_sequence(handle);
  venture::xr::clear_haptic_bindings(handle);
  auto result = session().destroy(SessionOwner::openxr);
  if (XR_SUCCEEDED(result)) {
    forget_handle(handle);
    state().attached_sets.erase(handle);
    state().graphics = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    state().guest_graphics_queue_index = 0;
    state().presentation_epoch = 0;
    state().host_swapchain_limit = 0;
    state().host_evictions = 0;
    state().foveation.clear();
    state().hands.clear();
  }
  if (shutdown_trace::enabled) log::info("shutdown", "xrDestroySession returned result={}", int(result));
  return result;
}
namespace {
// A host without XR_EXT_local_floor (Virtual Desktop's runtime) gets the floor
// space that extension specifies: LOCAL lowered to the STAGE floor, measured
// now. Until the floor height is known, STAGE itself is the floor.
// https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_EXT_local_floor.html
XrReferenceSpaceCreateInfo emulated_local_floor(XrSession handle, XrReferenceSpaceCreateInfo native) {
  native.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
  XrReferenceSpaceCreateInfo probe{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, XR_REFERENCE_SPACE_TYPE_LOCAL,
                                   {{0, 0, 0, 1}, {0, 0, 0}}};
  XrSpace local{}, stage{};
  bool ok = XR_SUCCEEDED(::xrCreateReferenceSpace(handle, &probe, &local));
  probe.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
  ok = ok && XR_SUCCEEDED(::xrCreateReferenceSpace(handle, &probe, &stage));
  XrTime now{};
  std::int64_t monotonic{};
  XrSpaceLocation where{XR_TYPE_SPACE_LOCATION};
  ok = ok && XR_SUCCEEDED(venture::xr::clock_sample(&now, &monotonic)) &&
       XR_SUCCEEDED(::xrLocateSpace(local, stage, now, &where)) &&
       (where.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
  if (ok) {
    native.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    native.poseInReferenceSpace.position.y -= where.pose.position.y;
  }
  if (local) ::xrDestroySpace(local);
  if (stage) ::xrDestroySpace(stage);
  static std::atomic_flag reported = ATOMIC_FLAG_INIT;
  if (!reported.test_and_set(std::memory_order_relaxed))
    log::info("xr", "Host has no XR_EXT_local_floor; floor space is {}", ok
        ? std::format("LOCAL lowered {:.3f} m to the STAGE floor", where.pose.position.y) : std::string("STAGE"));
  return native;
}
}  // namespace
XrResult xrCreateReferenceSpace(XrSession handle, const XrReferenceSpaceCreateInfo* info,
                                XrSpace* out) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(handle, "XrSession"))
    return XR_ERROR_HANDLE_INVALID;
  if (!out || XR_FAILED(validate_struct(info, XR_TYPE_REFERENCE_SPACE_CREATE_INFO)))
    return XR_ERROR_VALIDATION_FAILURE;
  auto native = *info;
  // Legacy XR_OCULUS_common_reference_spaces ABI, verified in Job Simulator
  // OVRPlugin 1.88 (SHA256 8310b817e740547afc746ebfa66812a4126369788fb2cfafb5686f17b63b05e7):
  // 0x18faa8/0x18fb4c create raw/floor spaces with next=null unconditionally,
  // even without enabling that private extension. 0x193840 selects floor for
  // FloorLevel; 0x1a465c locates m_xrRawSpace relative to m_xrAppSpace.
  // Use the host's fixed STAGE coordinates for raw tracking and LOCAL_FLOOR
  // for the calibrated floor origin (OpenXR EXT_local_floor). Keep this narrow
  // legacy compatibility independent of extension advertising; unknown types
  // still reach the host's XR_ERROR_REFERENCE_SPACE_UNSUPPORTED path.
  if (int(info->referenceSpaceType) == 1000043000) {
    native.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
  } else if (int(info->referenceSpaceType) == 1000043001) {
    // Our native instance is OpenXR 1.0 and must enable this host extension.
    if (host_xr().enabled_extensions.contains("XR_EXT_local_floor"))
      native.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR;
    else
      native = emulated_local_floor(handle, native);
  }
  auto result = ::xrCreateReferenceSpace(handle, &native, out);
  if (XR_SUCCEEDED(result)) track_handle(*out, "XrSpace", handle);
  const auto& p = info->poseInReferenceSpace;
  log::info("xr", "Reference space guest_type={} host_type={} result={} space={} offset=[{:.3f},{:.3f},{:.3f},{:.3f}]/[{:.3f},{:.3f},{:.3f}]",
            int(info->referenceSpaceType), int(native.referenceSpaceType), int(result),
            XR_SUCCEEDED(result) ? reinterpret_cast<std::uintptr_t>(*out) : 0, p.orientation.x, p.orientation.y,
            p.orientation.z, p.orientation.w, p.position.x, p.position.y, p.position.z);
  return result;
}
XrResult xrBeginSession(XrSession handle, const XrSessionBeginInfo *info) {
  std::lock_guard lock(state().mutex);
  std::lock_guard sl(session().mutex);
  if (!valid_handle(handle, "XrSession"))
    return XR_ERROR_HANDLE_INVALID;
  if (XR_FAILED(validate_struct(info, XR_TYPE_SESSION_BEGIN_INFO)))
    return XR_ERROR_VALIDATION_FAILURE;
  return session().begin_session(info);
}
XrResult xrEndSession(XrSession handle) {
  std::lock_guard lock(state().mutex);
  std::lock_guard sl(session().mutex);
  if (!valid_handle(handle, "XrSession"))
    return XR_ERROR_HANDLE_INVALID;
  venture::xr::cancel_sequence(handle);
  return session().end_session();
}
XrResult xrRequestExitSession(XrSession handle) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(handle, "XrSession")) return XR_ERROR_HANDLE_INVALID;
  return session().request_exit();
}
namespace { void schedule_space_probe(); }
XrResult xrPollEvent(XrInstance instance, XrEventDataBuffer *out) {
  xr_call_trace::Call trace("guest.xrPollEvent");
  return trace.finish([&]() -> XrResult {
  std::lock_guard lock(state().mutex);
  std::lock_guard sl(session().mutex);
  if (!valid_handle(instance, "XrInstance"))
    return XR_ERROR_HANDLE_INVALID;
  if (!out || out->type != XR_TYPE_EVENT_DATA_BUFFER || out->next)
    return XR_ERROR_VALIDATION_FAILURE;
  if (!state().events.empty()) {
    *out = state().events.front();
    state().events.pop_front();
    if (out->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
      trace.state(reinterpret_cast<XrEventDataSessionStateChanged*>(out)->state);
    return XR_SUCCESS;
  }
  XrResult result;
  for (;;) {
    *out = {XR_TYPE_EVENT_DATA_BUFFER};
    result = ::xrPollEvent(instance, out);
    if (result != XR_SUCCESS)
      return result;
    if (!session().deliver_host_event(*out)) continue;
    session().observe_host_event(*out);
    const StructureInfo *metadata = nullptr;
    for (const auto &m : structure_metadata())
      if (m.type == out->type) {
        metadata = &m;
        break;
      }
    // The single host instance enables a superset of each guest's extensions.
    // Never deliver an extension event that the guest did not opt into.
    if (metadata && (!*metadata->extension || enabled(metadata->extension)))
      break;
    log::debug("xr", "Filtered host event type {}", int(out->type));
  }
  if (result == XR_SUCCESS && out->type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
    // Rare (recenter, remount, boundary change) and decisive for orientation bugs.
    const auto* e = reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(out);
    const auto& p = e->poseInPreviousSpace;
    log::info("xr", "Host reference space change type={} change_time={} pose_valid={} pose=[{:.3f},{:.3f},{:.3f},{:.3f}]/[{:.3f},{:.3f},{:.3f}]",
              int(e->referenceSpaceType), e->changeTime, e->poseValid, p.orientation.x, p.orientation.y,
              p.orientation.z, p.orientation.w, p.position.x, p.position.y, p.position.z);
    schedule_space_probe();  // locate the head in each host space over the next ~5 s
  }
  if (result == XR_SUCCESS && out->type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
    venture::xr::clear_haptic_bindings(session().handle);
    venture::xr::note_menu_profiles_changed(session().handle);
  }
  if (result == XR_SUCCESS && out->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
    auto e = reinterpret_cast<XrEventDataSessionStateChanged *>(out);
    trace.state(e->state);
    if (e->session == session().handle) {
      session().state = e->state;
      if (e->state != XR_SESSION_STATE_FOCUSED) venture::xr::cancel_sequence(e->session);
      log::info("xr", "Host session state={}", int(e->state));
      startup_diagnostics::note_host_session_state(int(e->state));
      // Frame functions of a session pending loss return SESSION_LOST; do
      // not leave a pipelined xrWaitFrame blocked on a BeginFrame that the
      // render thread can no longer complete.
      if (e->state == XR_SESSION_STATE_LOSS_PENDING) session().lose(XR_ERROR_SESSION_LOST);
    }
  }
  return result;

  }());
}
XrResult xrWaitFrame(XrSession handle, const XrFrameWaitInfo *info, XrFrameState *out) {
  xr_call_trace::Call trace("guest.xrWaitFrame");
  return trace.finish([&]() -> XrResult {
  float previous_rate = 0;
  {
    std::lock_guard lock(state().mutex);
    if (!valid_handle(handle, "XrSession"))
      return XR_ERROR_HANDLE_INVALID;
    if ((info && XR_FAILED(validate_struct(info, XR_TYPE_FRAME_WAIT_INFO))) ||
        XR_FAILED(validate_struct(out, XR_TYPE_FRAME_STATE)))
      return XR_ERROR_VALIDATION_FAILURE;
    std::lock_guard sl(session().mutex);
    previous_rate = session().display_timing.source == xr::DisplayTiming::Source::unknown
        ? 0 : session().display_timing.rate();
  }
  auto result = session().wait(info, out);
  if (XR_SUCCEEDED(result)) trace.frame(out->predictedDisplayTime, out->predictedDisplayPeriod, out->shouldRender);
  if (XR_SUCCEEDED(result) && (venture::environment::getenv("VENTURE_TRACE_XR") ||
       venture::environment::getenv("VENTURE_TRACE_XR_FRAMES"))) {
    static std::atomic<unsigned> traced_waits{};
    if (traced_waits.fetch_add(1, std::memory_order_relaxed) < 64)
      log::info("xr-frame", "wait_frame should_render={} display_time={} period={}",
          bool(out->shouldRender), out->predictedDisplayTime, out->predictedDisplayPeriod);
  }
  if (XR_SUCCEEDED(result) && previous_rate > 0 && out->predictedDisplayPeriod > 0) {
    std::lock_guard lock(state().mutex);
    std::lock_guard sl(session().mutex);
    float rate = session().display_timing.rate();
    if (enabled("XR_FB_display_refresh_rate") &&
        !host_xr().enabled_extensions.contains("XR_FB_display_refresh_rate") &&
        std::abs(rate - previous_rate) > .01f) {
      XrEventDataDisplayRefreshRateChangedFB changed{
          XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB, nullptr, previous_rate, rate};
      XrEventDataBuffer buffer{};
      std::memcpy(&buffer, &changed, sizeof(changed));
      state().events.push_back(buffer);
    }
  }
  return result;

  }());
}
XrResult xrBeginFrame(XrSession handle, const XrFrameBeginInfo *info) {
  xr_call_trace::Call trace("guest.xrBeginFrame");
  return trace.finish([&]() -> XrResult {
  std::lock_guard lock(state().mutex);
  std::lock_guard sl(session().mutex);
  if (!valid_handle(handle, "XrSession"))
    return XR_ERROR_HANDLE_INVALID;
  if (info && XR_FAILED(validate_struct(info, XR_TYPE_FRAME_BEGIN_INFO)))
    return XR_ERROR_VALIDATION_FAILURE;
  return session().begin(info);

  }());
}
namespace {
// Diagnostic: the head in each host reference space, located by Venture's own spaces (never the
// guest's), shortly after a host recenter and every ~50 s. Tells a host space that turns over
// from a guest that misapplies XrEventDataReferenceSpaceChangePending.
struct SpaceProbe {
  XrSession session{};
  XrSpace view{};
  std::vector<std::pair<XrReferenceSpaceType, XrSpace>> bases;
  std::uint64_t frames = 0;
};
SpaceProbe& space_probe() { static SpaceProbe probe; return probe; }
// Set by the event thread, counted down by the render thread.
std::atomic<int> recenter_frames{-1};
void schedule_space_probe() { recenter_frames.store(360, std::memory_order_relaxed); }
void probe_spaces(XrSession session, XrTime time, const char* why) {
  auto& probe = space_probe();
  if (probe.session != session) {
    probe = {session};
    const XrReferenceSpaceType types[]{XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL,
                                       XR_REFERENCE_SPACE_TYPE_STAGE, XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR};
    for (auto type : types) {
      XrReferenceSpaceCreateInfo ci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, type, {{0, 0, 0, 1}, {0, 0, 0}}};
      XrSpace space{};
      if (XR_FAILED(::xrCreateReferenceSpace(session, &ci, &space))) continue;
      if (type == XR_REFERENCE_SPACE_TYPE_VIEW) probe.view = space;
      else probe.bases.emplace_back(type, space);
    }
  }
  if (!probe.view) return;
  for (const auto& [type, base] : probe.bases) {
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    const auto result = ::xrLocateSpace(probe.view, base, time, &location);
    const auto& q = location.pose.orientation;
    // The head's up axis (+Y rotated by q) in the base space.
    const float up_x = 2 * (q.x * q.y - q.w * q.z), up_y = 1 - 2 * (q.x * q.x + q.z * q.z),
                up_z = 2 * (q.y * q.z + q.w * q.x);
    log::info("xr-space", "{} host_space={} result={} flags={:#x} head_up=[{:+.2f},{:+.2f},{:+.2f}] head_y={:+.2f}",
              why, int(type), int(result), location.locationFlags, up_x, up_y, up_z, location.pose.position.y);
  }
}
}  // namespace
namespace {
// The image area a layer shows, to choose what a host over its layer limit
// leaves out; projection, cube and other layers are never left out.
std::uint64_t layer_pixels(const XrCompositionLayerBaseHeader* layer) {
  auto area = [](const XrSwapchainSubImage& image) {
    return std::uint64_t(std::max(0, image.imageRect.extent.width)) *
           std::uint64_t(std::max(0, image.imageRect.extent.height));
  };
  switch (layer->type) {
    case XR_TYPE_COMPOSITION_LAYER_QUAD:
      return area(reinterpret_cast<const XrCompositionLayerQuad*>(layer)->subImage);
    case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:
      return area(reinterpret_cast<const XrCompositionLayerCylinderKHR*>(layer)->subImage);
    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:
      return area(reinterpret_cast<const XrCompositionLayerEquirectKHR*>(layer)->subImage);
    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:
      return area(reinterpret_cast<const XrCompositionLayerEquirect2KHR*>(layer)->subImage);
    default:
      return UINT64_MAX;
  }
}
// The host's maxLayerCount, read once per session (0 when unknown).
std::uint32_t host_layer_limit(XrSession session) {
  static std::mutex mutex;
  static XrSession cached{};
  static std::uint32_t limit = 0;
  std::lock_guard lock(mutex);
  if (cached != session) {
    cached = session;
    limit = 0;
    auto& host = venture::xr::host_xr();
    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
    if (host.available && XR_SUCCEEDED(::xrGetSystemProperties(host.instance, host.system, &properties)))
      limit = properties.graphicsProperties.maxLayerCount;
  }
  return limit;
}
// A frame over the host's layer limit fails whole (XR_ERROR_LAYER_LIMIT_EXCEEDED,
// every frame after, so the game freezes) where a Quest takes it. The smallest
// layers are left out instead; the projection always stays.
void fit_layer_limit(XrSession session, std::vector<const XrCompositionLayerBaseHeader*>& layers) {
  const auto limit = host_layer_limit(session);
  if (!limit || layers.size() <= limit) return;
  std::vector<std::size_t> order(layers.size());
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::ranges::stable_sort(order, {}, [&](std::size_t i) { return layer_pixels(layers[i]); });
  std::vector<bool> drop(layers.size());
  std::size_t excess = layers.size() - limit, dropped = 0;
  for (auto i : order)
    if (dropped < excess && layer_pixels(layers[i]) != UINT64_MAX) drop[i] = true, ++dropped;
  static std::atomic_uint reports{};
  if (reports.fetch_add(1, std::memory_order_relaxed) < 4)
    log::warn("xr", "The game submits {} layers and the host takes {}; the {} smallest are left out",
              layers.size(), limit, dropped);
  std::size_t out = 0;
  for (std::size_t i = 0; i < layers.size(); ++i)
    if (!drop[i]) layers[out++] = layers[i];
  layers.resize(out);
}
}  // namespace

XrResult xrEndFrame(XrSession handle, const XrFrameEndInfo *info) {
  xr_call_trace::Call trace("guest.xrEndFrame");
  return trace.finish([&]() -> XrResult {
  // The guest's whole call; the host runtime's share is the xr_end_frame record inside it.
  vulkan::ScopedHostWait timing("guest_end_frame");
  std::unique_lock lock(state().mutex);
  std::lock_guard sl(session().mutex);
  if (!valid_handle(handle, "XrSession"))
    return XR_ERROR_HANDLE_INVALID;
  if (XR_FAILED(validate_struct(info, XR_TYPE_FRAME_END_INFO)) ||
      (info->layerCount && !info->layers))
    return XR_ERROR_VALIDATION_FAILURE;
  {
    auto& probe = space_probe();
    if (recenter_frames.load(std::memory_order_relaxed) >= 0) {
      const auto left = recenter_frames.fetch_sub(1, std::memory_order_relaxed);
      if (left == 359 || left == 288 || left == 144 || left == 0)
        probe_spaces(handle, info->displayTime, "after-recenter");
    } else if (probe.frames++ % 3600 == 0) {
      probe_spaces(handle, info->displayTime, "periodic");
    }
  }
  trace.display(info->displayTime);
  trace.layers(info->layerCount);
  // Native composition must follow accepted work on the guest queue. Pump
  // issuance before taking a physical queue lock so other producers can enter.
  if (!state().gles && [] {
        vulkan::ScopedHostWait timing("guest_end_flush");
        return vulkan::flush_queue_submissions(state().graphics.device, state().graphics.queueFamilyIndex,
                                               state().guest_graphics_queue_index);
      }() != VK_SUCCESS)
    return XR_ERROR_RUNTIME_FAILURE;
  if (venture::environment::getenv("VENTURE_TRACE_XR") ||
      venture::environment::getenv("VENTURE_TRACE_XR_FRAMES")) {
    static unsigned traced_ends{};
    if (traced_ends++ < 64) {
      log::info("xr-frame", "end_frame layers={} display_time={}", info->layerCount, info->displayTime);
      for (std::uint32_t i = 0; i < info->layerCount; ++i)
        log::info("xr-frame", "layer index={} type={} flags={:#x}", i, info->layers[i] ? int(info->layers[i]->type) : -1,
                  info->layers[i] ? info->layers[i]->layerFlags : 0);
    }
  }
  std::vector<const XrCompositionLayerBaseHeader*> layers;
  auto surface_result = [] { vulkan::ScopedHostWait timing("guest_end_surfaces"); return present_android_surfaces(); }();
  if (XR_FAILED(surface_result)) return surface_result;
  std::vector<XrCompositionLayerEquirectKHR> spheres;
  std::vector<XrCompositionLayerQuad> flats;
  std::vector<InputChain> chains;
  PresentationLayers rects;
  // A host without cylinders gets each as vertical strips of its arc, every
  // strip a quad on its chord; the strips share the host's spare layers.
  constexpr std::uint32_t max_strips = 32;
  std::uint32_t cylinders = 0, strips = 0;
  if (!host_xr().enabled_extensions.contains("XR_KHR_composition_layer_cylinder"))
    for (std::uint32_t i = 0; i < info->layerCount; ++i)
      if (info->layers[i] && info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) ++cylinders;
  if (cylinders) {
    const auto limit = host_layer_limit(handle), others = info->layerCount - cylinders;
    strips = !limit ? max_strips : std::clamp<std::uint32_t>(limit > others ? (limit - others) / cylinders : 1, 1, max_strips);
  }
  layers.reserve(info->layerCount + cylinders * strips);
  spheres.reserve(info->layerCount);
  flats.reserve(cylinders * strips);
  chains.reserve(info->layerCount + cylinders * strips);
  std::uint32_t i = 0;
  // Takes one host layer: copies its chain, validates it and maps its swapchains.
  auto append = [&](const XrCompositionLayerBaseHeader* layer, bool converted) -> XrResult {
    if (layer && layer->next) {
      chains.emplace_back();
      auto result = chains.back().copy(layer, false);
      if (XR_FAILED(result)) return result;
      layer = static_cast<const XrCompositionLayerBaseHeader*>(chains.back().head());
    }
    if (!layer || !valid_handle(layer->space, "XrSpace") ||
        XR_FAILED(converted ? validate_chain(layer->next) : validate_chain(layer))) {
      static unsigned reports = 0;
      if (reports++ < 8)
        log::warn("xr", "xrEndFrame rejected layer {}: type={} space_valid={}", i,
                  layer ? int(layer->type) : -1, layer && valid_handle(layer->space, "XrSpace"));
      return XR_ERROR_VALIDATION_FAILURE;
    }
    if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
      auto p = reinterpret_cast<const XrCompositionLayerProjection *>(layer);
      if (!p->views || p->viewCount != 2) {
        log::warn("xr", "xrEndFrame invalid projection view count {}", p->viewCount);
        return XR_ERROR_VALIDATION_FAILURE;
      }
      for (std::uint32_t j = 0; j < p->viewCount; ++j)
        if (p->views[j].type != XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW ||
            XR_FAILED(validate_chain(p->views[j].next, true, XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, true)) ||
            !valid_handle(p->views[j].subImage.swapchain, "XrSwapchain")) {
          static unsigned reports = 0;
          if (reports++ < 8)
            log::warn("xr", "xrEndFrame rejected projection view {}: type={} swapchain_valid={}", j,
                      int(p->views[j].type), valid_handle(p->views[j].subImage.swapchain, "XrSwapchain"));
          return XR_ERROR_VALIDATION_FAILURE;
        }
    }
    layers.push_back(static_cast<const XrCompositionLayerBaseHeader*>(rects.layer(layer)));
    return XR_SUCCESS;
  };
  for (; i < info->layerCount; ++i) {
    auto layer = info->layers[i];
    bool converted = false;
    if (layer && valid_handle(layer->space, "XrSpace")) {
      const StructureInfo* metadata = nullptr;
      for (const auto& candidate : structure_metadata())
        if (candidate.type == layer->type) { metadata = &candidate; break; }
      if (metadata && *metadata->extension &&
          !host_xr().enabled_extensions.contains(metadata->extension)) {
        if (layer->type == XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR &&
            host_xr().enabled_extensions.contains("XR_KHR_composition_layer_equirect")) {
          const auto& source = *reinterpret_cast<const XrCompositionLayerEquirect2KHR*>(layer);
          constexpr float pi = std::numbers::pi_v<float>;
          // Full-sphere Equirect2 has exactly the original extension's unit
          // scale/zero bias mapping. Cropped spheres need clipping/composition.
          if (std::abs(source.centralHorizontalAngle - 2*pi) < 1e-5f &&
              std::abs(source.upperVerticalAngle - pi/2) < 1e-5f &&
              std::abs(source.lowerVerticalAngle + pi/2) < 1e-5f) {
            spheres.push_back({XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR, source.next,
                source.layerFlags, source.space, source.eyeVisibility, source.subImage,
                source.pose, source.radius, {1, 1}, {0, 0}});
            layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&spheres.back());
            converted = true;
          }
        }
        if (layer->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) {
          const auto& source = *reinterpret_cast<const XrCompositionLayerCylinderKHR*>(layer);
          const float angle = source.centralAngle, radius = source.radius;
          const auto width = source.subImage.imageRect.extent.width;
          // A Quest always composes cylinders (UI panels, mostly), so the
          // extension is offered everywhere. Strips of at most 7.5 degrees keep
          // within 0.25% of the radius of the arc.
          if (angle > 0 && angle <= 2 * std::numbers::pi_v<float> && radius > 0 && std::isfinite(radius) &&
              source.aspectRatio > 0 && width > 0) {
            const auto wanted = std::uint32_t(std::ceil(angle / (std::numbers::pi_v<float> / 24)));
            const auto count = std::min({wanted, strips, std::uint32_t(width)});
            const auto& q = source.pose.orientation;
            for (std::uint32_t s = 0; s < count; ++s) {
              const auto x0 = std::int32_t(std::uint64_t(width) * s / count);
              const auto x1 = std::int32_t(std::uint64_t(width) * (s + 1) / count);
              // The image's middle is at -Z; its left edge turns toward -X.
              const float a0 = angle * (float(x0) / float(width) - 0.5f);
              const float a1 = angle * (float(x1) / float(width) - 0.5f);
              const float phi = (a0 + a1) / 2, half = (a1 - a0) / 2, depth = radius * std::cos(half);
              // The strip's centre on its chord, rotated by the pose.
              const XrVector3f v{depth * std::sin(phi), 0, -depth * std::cos(phi)};
              const XrVector3f t{2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x)};
              XrPosef pose = source.pose;
              pose.position = {pose.position.x + v.x + q.w * t.x + q.y * t.z - q.z * t.y,
                               pose.position.y + v.y + q.w * t.y + q.z * t.x - q.x * t.z,
                               pose.position.z + v.z + q.w * t.z + q.x * t.y - q.y * t.x};
              // The pose's orientation, then a turn of -phi about Y to face the axis.
              const float ry = -std::sin(phi / 2), rw = std::cos(phi / 2);
              pose.orientation = {q.x * rw - q.z * ry, q.w * ry + q.y * rw, q.z * rw + q.x * ry, q.w * rw - q.y * ry};
              auto sub = source.subImage;
              sub.imageRect.offset.x += x0;
              sub.imageRect.extent.width = x1 - x0;
              flats.push_back({XR_TYPE_COMPOSITION_LAYER_QUAD, source.next, source.layerFlags, source.space,
                               source.eyeVisibility, sub, pose,
                               {2 * radius * std::sin(half), radius * angle / source.aspectRatio}});
              if (auto result = append(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&flats.back()), true);
                  XR_FAILED(result))
                return result;
            }
            continue;
          }
        }
        if (!converted) {
          static std::set<XrStructureType> reported;
          if (reported.insert(layer->type).second)
            log::warn("xr", "Host lacks {}; omitting layer type {} while preserving supported layers",
                      metadata->extension, int(layer->type));
          continue;
        }
      }
    }
    if (auto result = append(layer, converted); XR_FAILED(result)) return result;
  }
  fit_layer_limit(handle, layers);
  for (auto chain : rects.submitted) {
    auto& r = state().swapchains.at(chain);
    r.ever_submitted = true;
    // Blits, which reverse rows, take single-sample images only.
    r.flip_rows = rects.flipped.contains(chain) && r.info.sampleCount == 1;
  }
  if (venture::environment::getenv("VENTURE_XR_SWAPCHAIN_STATS")) {
    const auto ever = std::count_if(state().swapchains.begin(), state().swapchains.end(),
        [](const auto& entry) { return entry.second.ever_submitted; });
    static std::array<std::size_t, 3> previous{};
    const std::array<std::size_t, 3> counts{state().swapchains.size(), rects.submitted.size(), std::size_t(ever)};
    if (counts != previous) {
      log::info("xr-budget", "guest_live={} submitted_this_frame={} live_ever_submitted={}", counts[0], counts[1], counts[2]);
      previous = counts;
    }
  }
  const auto prepared = [&] { vulkan::ScopedHostWait timing("guest_end_prepare"); return prepare_swapchains(rects.submitted); }();
  if (prepared != XR_SUCCESS && prepared != XR_SESSION_LOSS_PENDING) return prepared;
  PresentationLayers mapped(true);
  for (auto& layer : layers)
    layer = static_cast<const XrCompositionLayerBaseHeader*>(mapped.chain(layer));
  auto host_info = *info;
  host_info.layerCount = static_cast<std::uint32_t>(layers.size());
  host_info.layers = layers.data();
  // The host submission can block for the compositor. Session::end needs only
  // the session mutex and the host queue lock; holding the state lock across it
  // stalled other threads' OpenXR calls (Batman main thread: 5% of samples in
  // xrConvertTimespecTimeToTimeKHR waiting for it).
  std::shared_ptr<OwnedFrameEnd> owned;
  if (session().async_end_ready()) {
    owned = std::make_shared<OwnedFrameEnd>();
    if (!owned->copy(host_info)) owned.reset();
  }
  lock.unlock();
  const auto result = owned ? session().end(&owned->info, owned) : session().end(&host_info);
  return result == XR_SUCCESS ? prepared : result;

  }());
}
XrResult xrSetAndroidApplicationThreadKHR(XrSession handle, std::int32_t type, std::uint32_t tid) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(handle, "XrSession"))
    return XR_ERROR_HANDLE_INVALID;
  if (!enabled("XR_KHR_android_thread_settings"))
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (type < 1 || type > 4 || !tid)
    return XR_ERROR_VALIDATION_FAILURE;
  // Android runtimes raise the application and renderer main threads (types 1
  // and 3) above ordinary work. Apply the setpriority mapping for nice -10, the
  // highest ordinary host priority; worker hints keep their current priority.
  // The hint stays advisory: an unknown or exited thread is not an error.
  if ((type == 1 || type == 3) && platform::thread_hints_enabled()) {
    const int result = platform::set_thread_priority(static_cast<std::int32_t>(tid), -10);
    log::info("openxr", "android thread type={} tid={} priority result={}", type, tid, result);
  }
  return XR_SUCCESS;
}
} // namespace venture::hle::openxr::guest
