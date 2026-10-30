#include "core/xr_call_trace.h"
#include "xr/menu_chord.h"
#include "hle/openxr/state.h"
#include "hle/openxr/passthrough.h"
#include "xr/haptic_sequence.h"
#include "xr/haptic_output.h"
#include "common/log.h"
#include "common/environment.h"
#include "android/framework_bridge.h"
#include "core/process.h"
#include "common/android/manifest.h"
#include "java/jvm.h"
#include <algorithm>
#include <cstddef>
#include <cstring>

namespace venture::hle::openxr {
State &state() {
  static State s;
  return s;
}
bool enabled(std::string_view extension) {
  return state().enabled.contains(std::string(extension));
}
bool full_rate_composition() {
  // A64 has the mobile composition structure adapters and real full-rate
  // Vulkan resources. Desktop hosts can consume native depth and full-rate
  // color without mobile frame extrapolation. A32 needs its layout adapters
  // before it can offer the same contract.
  return process_config().isa() == GuestIsa::a64;
}
bool hand_tracking_required() {
  static const bool required = [] {
    const auto& apk = process_config().apk;
    if (apk.empty()) return false;
    try {
      const auto features = android::read_apk(apk).required_features;
      return std::ranges::find(features, "oculus.software.handtracking") != features.end();
    } catch (const std::exception& e) {
      log::warn("xr", "APK features unavailable: {}", e.what());
      return false;
    }
  }();
  return required;
}
void cancel_object_haptics(const ObjectKey& value) {
  if (!state().objects.contains(value)) return;
  if (value.first == "XrAction") {
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO, nullptr,
        reinterpret_cast<XrAction>(value.second), XR_NULL_PATH};
    venture::xr::cancel_sequence(session().handle, &info);
    venture::xr::clear_haptic_bindings(session().handle);
  }
  for (const auto &[child, record] : state().objects)
    if (record.parent == value.second && record.parent_type == value.first) cancel_object_haptics(child);
}
std::size_t State::ActionReadHash::operator()(const ActionRead &r) const noexcept {
  std::uint64_t h = key(r.session) * 0x9e3779b97f4a7c15ull;
  for (const std::uint64_t part : {std::uint64_t(key(r.action)), std::uint64_t(r.path), std::uint64_t(r.type)})
    h ^= part + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  return h;
}
void forget_action_states(bool all) {
  if (all) state().action_states.clear();
  ++state().action_sync;
}
void forget_object(const ObjectKey& value) {
  forget_action_states(true);
  venture::xr::forget_menu_object(value.second);
  std::vector<ObjectKey> children;
  for (const auto &[h, o] : state().objects)
    if (o.parent == value.second && o.parent_type == value.first)
      children.push_back(h);
  for (const auto& child : children)
    forget_object(child);
  if (value.first == "XrAction" && state().objects.contains(value)) {
    auto action = reinterpret_cast<XrAction>(value.second);
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, action, XR_NULL_PATH};
    venture::xr::cancel_sequence(session().handle, &info);
    venture::xr::clear_haptic_bindings(session().handle);
    state().actions.erase(action);
  }
  state().objects.erase(value);
}
std::span<const CommandInfo> command_metadata() {
  static constexpr CommandInfo commands[]{
#define VENTURE_XR_COMMAND(name, ext, manual) {#name, ext, manual},
#include "hle/openxr/generated/commands.inc"
#undef VENTURE_XR_COMMAND
    {"xrCreateSwapchainAndroidSurfaceKHR", "XR_KHR_android_surface_swapchain", true},
  };
  return commands;
}
std::span<const StructureInfo> structure_metadata() {
  static constexpr StructureInfo structures[]{
#define VENTURE_XR_STRUCT(name, tag, ext, parents)                                                      \
  {#name, tag, sizeof(name), alignof(name), ext, parents},
#define VENTURE_XR_MEMBER(...)
#include "hle/openxr/generated/structures.inc"
#undef VENTURE_XR_MEMBER
#undef VENTURE_XR_STRUCT
  };
  return structures;
}
std::span<const MemberInfo> member_metadata() {
  static constexpr MemberInfo members[]{
#define VENTURE_XR_STRUCT(...)
#define VENTURE_XR_MEMBER(s, m, t, p, n) {#s, #m, offsetof(s, m), sizeof(s::m), t, p, n},
#include "hle/openxr/generated/structures.inc"
#undef VENTURE_XR_MEMBER
#undef VENTURE_XR_STRUCT
  };
  return members;
}
namespace {
// Structures whose registry parentstruct is XrCompositionLayerBaseHeader begin
// with its members (type, next, layerFlags, space).
bool composition_layer(std::string_view structure) {
  static constexpr std::string_view header[]{"type", "next", "layerFlags", "space"};
  std::size_t i = 0;
  for (const auto &m : member_metadata())
    if (m.structure == structure) {
      // XR_FB_passthrough has a documented registry erratum: flags is the
      // common header's layerFlags member, at the same offset.
      const std::string_view member(m.name);
      if (member != header[i] &&
          !(i == 2 && structure == "XrCompositionLayerPassthroughFB" && member == "flags")) return false;
      if (++i == std::size(header)) return true;
    }
  return false;
}
}  // namespace
bool extends_structure(const StructureInfo &child, XrStructureType parent) {
  if (parent == XR_STRUCTURE_TYPE_MAX_ENUM)
    return true;
  for (const auto &m : structure_metadata())
    if (m.type == parent) {
      std::string_view list(child.extends), name(m.name);
      while (!list.empty()) {
        auto end = list.find(',');
        // structextends="XrCompositionLayerBaseHeader" (color scale/bias, image
        // layout, ...) extends every composition layer structure.
        if (list.substr(0, end) == name ||
            (list.substr(0, end) == "XrCompositionLayerBaseHeader" && composition_layer(name)))
          return true;
        if (end == list.npos)
          break;
        list.remove_prefix(end + 1);
      }
    }
  return false;
}
// Extensions whose structures Venture applies itself and never forwards.
static bool applied_locally(std::string_view extension) {
  return extension == "XR_FB_composition_layer_image_layout" ||
         (extension == "XR_FB_passthrough" && local_passthrough());
}
XrResult validate_chain(const void *next, bool host_only, XrStructureType parent,
                        bool ignore_unenabled) {
  std::set<const void *> seen;
  std::set<XrStructureType> types;
  for (auto p = static_cast<const XrBaseInStructure *>(next); p; p = p->next) {
    if (!seen.insert(p).second || seen.size() > 64)
      return XR_ERROR_VALIDATION_FAILURE;
    const StructureInfo *metadata = nullptr;
    for (const auto &m : structure_metadata())
      if (m.type == p->type) {
        metadata = &m;
        break;
      }
    if (!metadata) {
      // OpenXR "Valid Usage for Structure Pointer Chains": runtimes ignore
      // unrecognized next structures. Read only their common header and let
      // the native runtime do the same. A standalone composition-layer
      // header (no parent) still requires a supported layout below.
      if (parent != XR_STRUCTURE_TYPE_MAX_ENUM) continue;
      log::warn("xr", "Untranslated structure type {}", int(p->type));
      return XR_ERROR_VALIDATION_FAILURE;
    }
    if (ignore_unenabled && *metadata->extension && !enabled(metadata->extension))
      continue;
    if (!types.insert(p->type).second || !extends_structure(*metadata, parent)) {
      static unsigned reports = 0;
      if (reports++ < 8) log::warn("xr", "Invalid structure chain: {} under {}", metadata->name, int(parent));
      return XR_ERROR_VALIDATION_FAILURE;
    }
    if (*metadata->extension &&
        (!enabled(metadata->extension) ||
         (host_only && !host_xr().enabled_extensions.contains(metadata->extension) &&
          !applied_locally(metadata->extension) &&
          !(full_rate_composition() && std::string_view(metadata->extension) == "XR_FB_space_warp")))) {
      static unsigned reports = 0;
      if (reports++ < 8) log::warn("xr", "Structure {} requires {}: guest={} host={}", metadata->name,
          metadata->extension, enabled(metadata->extension), host_xr().enabled_extensions.contains(metadata->extension));
      return XR_ERROR_VALIDATION_FAILURE;
    }
  }
  return XR_SUCCESS;
}
XrResult validate_struct(const void *pointer, XrStructureType type) {
  auto p = static_cast<const XrBaseInStructure *>(pointer);
  if (!p || p->type != type)
    return XR_ERROR_VALIDATION_FAILURE;
  return validate_chain(p->next, true, type);
}
XrResult InputChain::copy(const void* root, bool validate_parent) {
  std::lock_guard lock(state().mutex);
  storage_.clear();
  if (!root) return XR_ERROR_VALIDATION_FAILURE;
  const auto parent = static_cast<const XrBaseInStructure*>(root)->type;
  std::set<const void*> seen;
  std::set<XrStructureType> types;
  XrBaseInStructure* previous = nullptr;
  for (auto p = static_cast<const XrBaseInStructure*>(root); p; p = p->next) {
    if (!seen.insert(p).second || seen.size() > 64) return XR_ERROR_VALIDATION_FAILURE;
    const StructureInfo* metadata = nullptr;
    for (const auto& m : structure_metadata())
      if (m.type == p->type) { metadata = &m; break; }
    // OpenXR 1.0 §2.9.7, Valid Usage for Structure Pointer Chains:
    // runtimes must ignore unknown and unenabled extension nodes in next.
    // https://registry.khronos.org/OpenXR/specs/1.0-khr/html/xrspec.html#valid-usage-for-structure-pointer-chains
    if (p != root && (!metadata || (*metadata->extension && !enabled(metadata->extension)))) {
      static std::set<XrStructureType> reported;
      if (reported.insert(p->type).second)
        log::info("xr", "Ignoring unknown or unenabled next-chain type {}", int(p->type));
      continue;
    }
    if (!metadata || !types.insert(p->type).second ||
        (p != root && validate_parent && !extends_structure(*metadata, parent)))
      return XR_ERROR_VALIDATION_FAILURE;
    if (p != root && *metadata->extension &&
        !host_xr().enabled_extensions.contains(metadata->extension) && !applied_locally(metadata->extension)) {
      log::warn("xr", "Host cannot forward enabled next-chain extension {}", metadata->extension);
      return XR_ERROR_FEATURE_UNSUPPORTED;
    }
    auto bytes = std::make_unique<std::byte[]>(metadata->size);
    std::memcpy(bytes.get(), p, metadata->size);
    auto* node = reinterpret_cast<XrBaseInStructure*>(bytes.get());
    node->next = nullptr;
    if (previous) previous->next = node;
    previous = node;
    storage_.push_back(std::move(bytes));
  }
  return XR_SUCCESS;
}
std::map<std::string, std::uint32_t> capabilities() {
  std::map<std::string, std::uint32_t> result{
      {"XR_KHR_loader_init", 2},
      {"XR_KHR_loader_init_android", 1},
      {"XR_KHR_android_create_instance", 3},
      {"XR_KHR_android_thread_settings", 6},
      {"XR_FB_display_refresh_rate", 1},
      {"XR_FB_color_space", 3},
      {"XR_FB_foveation", 1},
      {"XR_FB_foveation_configuration", 1},
      {"XR_FB_swapchain_update_state", 3},
      {"XR_FB_touch_controller_pro", 1},
      {"XR_META_touch_controller_plus", 1},
      {"XR_KHR_locate_spaces", 1},
      {"XR_FB_haptic_pcm", 1},
      {"XR_FB_haptic_amplitude_envelope", 1},
      // Guest Vulkan images are copied to the host chain, which applies the flip.
      {"XR_FB_composition_layer_image_layout", 1},
  };
  if (xr_images())
    result["XR_KHR_opengl_es_enable"] = 10;
  if (process_config().isa() == GuestIsa::a64) {
    result["XR_META_recommended_layer_resolution"] = 1;
    result["XR_META_headset_id"] = 2;
    result["XR_META_vulkan_swapchain_create_info"] = 1;  // translated to core usage flags (graphics.cpp)
  }
  if (process_config().isa() == GuestIsa::a64 && java::host_vm() && android::surface_bridge().to_surface)
    result["XR_KHR_android_surface_swapchain"] = 4;
  const auto &host = host_xr();
  for (const auto &[name, version] : host.available_extensions) {
    if (!host.enabled_extensions.contains(name))
      continue;
    if (name == "XR_KHR_win32_convert_performance_counter_time" ||
        name == "XR_KHR_convert_timespec_time") {
      result["XR_KHR_convert_timespec_time"] = 1;
      continue;
    }
    result[name] = version;
  }
  // Extensions with pointer layouts still requiring A32 adapters stay hidden.
  if (process_config().isa() == GuestIsa::a32) {
    result.erase("XR_META_recommended_layer_resolution");
    result.erase("XR_META_headset_id");
    result.erase("XR_META_vulkan_swapchain_create_info");
    // Record-pointer commands require A32 layout adapters. VrApi's buffered
    // path has those adapters; the OpenXR extension structs do not yet.
    result.erase("XR_FB_haptic_pcm");
    result.erase("XR_FB_haptic_amplitude_envelope");
    result.erase("XR_FB_space_warp");
  }
  if (local_passthrough()) result["XR_FB_passthrough"] = XR_FB_passthrough_SPEC_VERSION;
  if (process_config().isa() == GuestIsa::a32) {
    result.erase("XR_FB_passthrough");
    result.erase("XR_FB_triangle_mesh");
  }
  if (result.contains("XR_FB_swapchain_update_state")) {
    result["XR_FB_swapchain_update_state_vulkan"] = 1;
    result["XR_FB_swapchain_update_state_opengl_es"] = 1;
  } else {
    result.erase("XR_FB_swapchain_update_state_vulkan");
    result.erase("XR_FB_swapchain_update_state_opengl_es");
  }
  // The A64 adapter accepts mobile motion/depth inputs and presents the real
  // application frames at the native host cadence. Required mobile interfaces
  // must remain negotiable even when the host uses full-rate composition.
  // Vulkan density-map output is not implemented by this bridge: no placeholder
  // image may stand in for a host-owned fragment-density-map attachment.
  result.erase("XR_FB_foveation_vulkan");
  if (full_rate_composition()) {
    result["XR_FB_space_warp"] = 2;
    result["XR_FB_foveation_vulkan"] = 1;
  }
  if (result.contains("XR_EXT_hand_tracking")) {
    result["XR_FB_hand_tracking_aim"] = 2;
    result["XR_FB_hand_tracking_capsules"] = 3;
    result["XR_FB_hand_tracking_mesh"] = 3;
  }
  return result;
}
// Games read every action every frame (North Star: about 150 reads per frame),
// and each read was a host runtime call. Chained structures always reach the
// host; failures are not stored.
template <class F, HostProcedureName Name, class S>
XrResult stored_action_state(XrSession session, const XrActionStateGetInfo *info, S *out) {
  constexpr auto begin = offsetof(S, next) + sizeof(void *);
  static_assert(sizeof(S) - begin <= sizeof(State::StoredActionState::value));
  if (info->next || out->next) return host_call<F, Name>(session, info, out);
  auto &s = state();
  auto &stored = s.action_states[{session, info->action, info->subactionPath, out->type}];
  auto *bytes = reinterpret_cast<std::byte *>(out) + begin;
  if (stored.sync == s.action_sync) {
    std::memcpy(bytes, stored.value.data(), sizeof(S) - begin);
    return stored.result;
  }
  const auto result = host_call<F, Name>(session, info, out);
  if (XR_FAILED(result)) return result;
  stored.sync = s.action_sync;
  stored.result = result;
  std::memcpy(stored.value.data(), bytes, sizeof(S) - begin);
  return result;
}
namespace guest {
#include "hle/openxr/generated/forward.inc"
} // namespace guest
} // namespace venture::hle::openxr
