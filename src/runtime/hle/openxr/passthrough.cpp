#include "hle/openxr/passthrough.h"
#include "hle/openxr/passthrough_camera.h"
#include "hle/openxr/state.h"
#include "xr/passthrough_source.h"
#include "xr/tracked_camera.h"
#include "hle/openxr/passthrough_report.h"
#include "common/log.h"
#include "core/process.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <optional>
#include <thread>
#include <utility>

namespace venture::hle::openxr {
namespace {
struct Feature { XrSession session; bool running; };
struct Layer {
  XrSession session; XrPassthroughFB feature; bool running; float opacity = 1;
  // htc: the host passthrough this layer is shown through, and the space its
  // planar layer is submitted in (runtime-owned placement, as FB's null space).
  XrPassthroughHTC htc = XR_NULL_HANDLE;
  XrSpace host_space = XR_NULL_HANDLE;
};
struct Geometry { XrSession session; XrPassthroughLayerFB layer; };
// Storage also supplies stable, typed local handles. Native handles retain the
// host's identity and are never substituted in a forwarded structure.
std::map<XrPassthroughFB, std::unique_ptr<Feature>> features;
std::map<XrPassthroughLayerFB, std::unique_ptr<Layer>> layers;
std::map<XrGeometryInstanceFB, Geometry> geometries;
std::unique_ptr<PassthroughSource> source;
CameraPassthrough camera;
// The HTC passthrough that proved the backend starts, kept for the first layer.
struct SpareHtc { XrSession session{}; XrPassthroughHTC htc{}; XrSpace space{}; } spare_htc;
void drop_spare_htc() {
  if (spare_htc.space) ::xrDestroySpace(spare_htc.space);
  if (spare_htc.htc) host_call<PFN_xrDestroyPassthroughHTC, "xrDestroyPassthroughHTC">(spare_htc.htc);
  spare_htc = {};
}
constexpr std::array order{PassthroughBackend::native, PassthroughBackend::htc, PassthroughBackend::blend,
                           PassthroughBackend::camera, PassthroughBackend::external};
std::size_t slot(PassthroughBackend value) { return std::size_t(value); }
std::uint64_t now_ms() {
  return std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
// An external source is asked once synchronously (at most 500 ms, and only
// when its endpoint exists), then in the background at most every 2 s, so a
// frame never waits for it. A source that answered is kept for the process.
struct ExternalProbe {
  std::atomic<bool> found{false}, busy{false};
  bool asked = false, logged = false;
  std::uint64_t next = 0;
  std::thread thread;
  ~ExternalProbe() { reset(); }
  void reset() {
    if (thread.joinable()) thread.join();
    found = false; busy = false; asked = logged = false; next = 0;
  }
  bool available() {
    if (!found.load()) {
      const auto now = now_ms();
      if (!asked) {
        asked = true;
        next = now + 2000;
        if (passthrough_source_answers(500)) found = true;
      } else if (now >= next && !busy.exchange(true)) {
        next = now + 2000;
        if (thread.joinable()) thread.join();
        try {
          thread = std::thread([this] { if (passthrough_source_answers(500)) found = true; busy = false; });
        } catch (...) { busy = false; }
      }
    }
    if (found.load() && !std::exchange(logged, true))
      log::info("xr-passthrough", "An external passthrough source answered");
    return found.load();
  }
} external_probe;
// What the host can do, asked once per host instance and system, and which
// backends failed there.
struct Probe {
  bool valid = false;
  XrInstance instance = XR_NULL_HANDLE;
  XrSystemId system = XR_NULL_SYSTEM_ID;
  std::string system_name;
  bool fb_supported = false; // host XR_FB_passthrough system properties report support
  XrPassthroughCapabilityFlagsFB fb_capabilities = 0;
  XrEnvironmentBlendMode world = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  int camera = -1; // a headset camera through OpenVR; -1: not asked yet
  std::array<std::string, 7> failed; // why a backend stopped (empty: it did not)
  std::optional<PassthroughBackend> reported;
} probe;
// The guest's passthrough objects were made with the native backend (host
// handles) or a local one; a failed local backend is replaced by the next local one.
enum class Commit { none, native, local } commit = Commit::none;
const Probe& host_probe() {
  const auto& host = host_xr();
  if (probe.valid && probe.instance == host.instance && probe.system == host.system) return probe;
  probe = {};
  probe.valid = true; probe.instance = host.instance; probe.system = host.system;
  // Older runtimes fill only the first structure; unknown nodes are ignored.
  XrSystemPassthroughProperties2FB newer{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB};
  XrSystemPassthroughPropertiesFB older{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB, &newer, XR_FALSE};
  XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES, &older};
  if (XR_SUCCEEDED(::xrGetSystemProperties(host.instance, host.system, &properties))) {
    probe.system_name = properties.systemName;
    probe.fb_supported = older.supportsPassthrough || (newer.capabilities & XR_PASSTHROUGH_CAPABILITY_BIT_FB);
    probe.fb_capabilities = newer.capabilities;
  }
  std::uint32_t count = 0;
  if (XR_SUCCEEDED(::xrEnumerateEnvironmentBlendModes(host.instance, host.system,
          XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr)) && count && count <= 16) {
    std::array<XrEnvironmentBlendMode, 16> modes{};
    if (XR_SUCCEEDED(::xrEnumerateEnvironmentBlendModes(host.instance, host.system,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, modes.data()))) {
      const auto has = [&](XrEnvironmentBlendMode mode) {
        return std::find(modes.begin(), modes.begin() + std::min(count, 16u), mode) != modes.begin() + std::min(count, 16u);
      };
      // ALPHA_BLEND shows the world exactly where the guest's alpha leaves it
      // uncovered; ADDITIVE (optical see-through) always shows it.
      if (has(XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND)) probe.world = XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
      else if (has(XR_ENVIRONMENT_BLEND_MODE_ADDITIVE)) probe.world = XR_ENVIRONMENT_BLEND_MODE_ADDITIVE;
    }
  }
  log::info("xr-passthrough", "Host passthrough capabilities: XR_FB_passthrough={} (supported={}) "
            "XR_HTC_passthrough={} see-through blend mode={}",
            host.enabled_extensions.contains("XR_FB_passthrough"), probe.fb_supported,
            host.enabled_extensions.contains("XR_HTC_passthrough"), int(probe.world));
  return probe;
}
bool offered(PassthroughBackend value) {
  const auto& host = host_xr();
  switch (value) {
  case PassthroughBackend::native: return host.enabled_extensions.contains("XR_FB_passthrough") && probe.fb_supported;
  case PassthroughBackend::htc: return host.enabled_extensions.contains("XR_HTC_passthrough");
  case PassthroughBackend::blend: return probe.world != XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  case PassthroughBackend::camera:
    if (probe.camera < 0) probe.camera = xr::tracked_camera_present() ? 1 : 0;
    return probe.camera == 1;
  case PassthroughBackend::external: return external_probe.available();
  default: return false;
  }
}
// Why a backend is not used, for the log and the launcher's Info dialog.
std::string not_used(PassthroughBackend value) {
  if (!probe.failed[slot(value)].empty()) return probe.failed[slot(value)];
  const auto& host = host_xr();
  switch (value) {
  case PassthroughBackend::native:
    return host.enabled_extensions.contains("XR_FB_passthrough")
        ? "the runtime has XR_FB_passthrough but reports no passthrough for this headset"
        : "the runtime does not offer XR_FB_passthrough";
  case PassthroughBackend::htc: return "the runtime does not offer XR_HTC_passthrough";
  case PassthroughBackend::blend: return "the runtime offers only opaque blending for this headset";
#ifdef _WIN32
  case PassthroughBackend::camera: return "no headset camera through OpenVR (SteamVR)";
#else
  case PassthroughBackend::camera: return "OpenVR cameras are used on Windows only";
#endif
  case PassthroughBackend::external: return "no Steam Link passthrough companion answered";
  default: return {};
  }
}
// The chosen backend and why the others are not used: logged, and kept in
// passthrough.json in the data directory for the launcher's Info dialog.
void report(PassthroughBackend chosen) {
  if (probe.reported == chosen) return;
  probe.reported = chosen;
  const bool off = !process_config().passthrough, a32 = process_config().isa() != GuestIsa::a64;
  const bool keyed = process_config().passthrough_key.has_value();
  const auto rank = [](PassthroughBackend value) { return std::find(order.begin(), order.end(), value) - order.begin(); };
  Json backends = Json::array();
  for (auto value : order) {
    std::string status = value == chosen ? "used" : off ? "passthrough is turned off in Settings"
                       : a32 ? "32-bit games have no passthrough"
                       : keyed ? "Color key is chosen in Settings"
                       : chosen != PassthroughBackend::none && rank(value) > rank(chosen) &&
                                 probe.failed[slot(value)].empty() ? "not needed"
                       : not_used(value);
    backends.push_back({{"backend", passthrough_backend_name(value)}, {"status", status}});
  }
  backends.push_back({{"backend", passthrough_backend_name(PassthroughBackend::color_key)},
                      {"status", chosen == PassthroughBackend::color_key ? "used"
                                 : "only when chosen in Settings (Passthrough: Color key)"}});
  log::info("xr-passthrough", "Passthrough backend: {} (runtime {}, headset {})", passthrough_backend_name(chosen),
            host_xr().runtime_name, probe.system_name);
  save_passthrough_report({{"version", 1}, {"runtime", host_xr().runtime_name}, {"headset", probe.system_name},
                           {"backend", passthrough_backend_name(chosen)}, {"enabled", !off}, {"backends", backends}});
}
PassthroughBackend backend() {
  if (!process_config().passthrough || process_config().isa() != GuestIsa::a64) {
    report(PassthroughBackend::none);
    return PassthroughBackend::none;
  }
  // The user's color key replaces every detected backend: the tool keying the
  // color cannot be detected, and with it chosen the host's own is not wanted.
  if (process_config().passthrough_key) {
    report(PassthroughBackend::color_key);
    return PassthroughBackend::color_key;
  }
  host_probe();
  if (commit == Commit::native) return PassthroughBackend::native;
  for (auto value : order) {
    // Local guest objects cannot become host handles.
    if (commit == Commit::local && value == PassthroughBackend::native) continue;
    if (probe.failed[slot(value)].empty() && offered(value)) { report(value); return value; }
  }
  report(PassthroughBackend::none);
  return PassthroughBackend::none;
}
bool is_local(PassthroughBackend value) {
  return value != PassthroughBackend::none && value != PassthroughBackend::native;
}
bool native() { return backend() == PassthroughBackend::native; }
void withdraw() { if (source) source->request(false); }
// A backend that does not start, or stops working, is not used again on this
// host instance; the next one offered takes over.
void fail(PassthroughBackend value, std::string reason) {
  log::warn("xr-passthrough", "{} not usable: {}; trying the next backend", passthrough_backend_name(value), reason);
  probe.failed[slot(value)] = std::move(reason);
  if (value == PassthroughBackend::htc) {
    session().finish_host_end();
    drop_spare_htc();
    for (auto& [handle, record] : layers) {
      if (record->host_space) { ::xrDestroySpace(record->host_space); record->host_space = XR_NULL_HANDLE; }
      if (record->htc) {
        host_call<PFN_xrDestroyPassthroughHTC, "xrDestroyPassthroughHTC">(record->htc);
        record->htc = XR_NULL_HANDLE;
      }
    }
  } else if (value == PassthroughBackend::camera) {
    session().finish_host_end();
    camera.stop();
  }
}
bool running(const Layer& layer) {
  const auto feature = features.find(layer.feature);
  return layer.running && feature != features.end() && feature->second->running;
}
// A planar HTC passthrough and its VIEW space for one guest layer.
XrResult create_htc(XrSession target, XrPassthroughHTC& htc, XrSpace& space) {
  // A planar HTC passthrough is the whole-view camera image FB calls reconstruction.
  XrPassthroughCreateInfoHTC create{XR_TYPE_PASSTHROUGH_CREATE_INFO_HTC, nullptr, XR_PASSTHROUGH_FORM_PLANAR_HTC};
  auto result = host_call<PFN_xrCreatePassthroughHTC, "xrCreatePassthroughHTC">(target, &create, &htc);
  if (XR_FAILED(result)) { htc = XR_NULL_HANDLE; return result; }
  XrReferenceSpaceCreateInfo view{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, XR_REFERENCE_SPACE_TYPE_VIEW,
                                  {{0, 0, 0, 1}, {0, 0, 0}}};
  result = ::xrCreateReferenceSpace(target, &view, &space);
  if (XR_FAILED(result)) {
    host_call<PFN_xrDestroyPassthroughHTC, "xrDestroyPassthroughHTC">(htc);
    htc = XR_NULL_HANDLE;
    space = XR_NULL_HANDLE;
  }
  log::info("xr-passthrough", "Create host XR_HTC_passthrough planar passthrough: result={}", int(result));
  return result;
}
// Errors that belong to the guest's call rather than to the backend.
bool guest_error(XrResult result) {
  return result == XR_ERROR_VALIDATION_FAILURE || result == XR_ERROR_HANDLE_INVALID ||
         result == XR_ERROR_FEATURE_ALREADY_CREATED_PASSTHROUGH_FB || result == XR_ERROR_OUT_OF_MEMORY ||
         result == XR_ERROR_SESSION_LOST || result == XR_ERROR_INSTANCE_LOST || result == XR_ERROR_LIMIT_REACHED;
}
// The best backend that actually starts for this session, before the guest
// has a passthrough object; FEATURE_UNSUPPORTED when none does.
XrResult start_backend(XrSession target, const XrPassthroughCreateInfoFB* info, XrPassthroughFB* out,
                       PassthroughBackend& chosen) {
  for (;;) {
    chosen = backend();
    switch (chosen) {
    case PassthroughBackend::none: return XR_ERROR_FEATURE_UNSUPPORTED;
    case PassthroughBackend::native: {
      const auto result = host_call<PFN_xrCreatePassthroughFB, "xrCreatePassthroughFB">(target, info, out);
      if (XR_SUCCEEDED(result) || guest_error(result)) return result;
      fail(chosen, "xrCreatePassthroughFB failed (" + std::to_string(int(result)) + ")");
      continue;
    }
    case PassthroughBackend::htc: {
      XrPassthroughHTC htc{};
      XrSpace space{};
      const auto result = create_htc(target, htc, space);
      if (XR_FAILED(result)) { fail(chosen, "xrCreatePassthroughHTC failed (" + std::to_string(int(result)) + ")"); continue; }
      drop_spare_htc();
      spare_htc = {target, htc, space};
      return XR_SUCCESS;
    }
    case PassthroughBackend::camera:
      if (camera.start(1000)) return XR_SUCCESS;
      fail(chosen, "the camera did not stream");
      continue;
    default: return XR_SUCCESS;
    }
  }
}
XrResult validate_passthrough_info(const void* pointer, XrStructureType type) {
  const auto* root = static_cast<const XrBaseInStructure*>(pointer);
  if (!root || root->type != type) return XR_ERROR_VALIDATION_FAILURE;
  const auto result = validate_chain(root->next, native(), type, true);
  if (XR_FAILED(result) || native()) return result;
  // Colour maps and other chained styles: only a forwarding backend shows them.
  for (auto* node = root->next; node; node = node->next)
    for (const auto& metadata : structure_metadata())
      if (metadata.type == node->type && (!*metadata.extension || enabled(metadata.extension)))
        return XR_ERROR_FEATURE_UNSUPPORTED;
  return XR_SUCCESS;
}
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
PassthroughBackend passthrough_backend() { return backend(); }
const char* passthrough_backend_name(PassthroughBackend value) {
  switch (value) {
  case PassthroughBackend::native: return "host XR_FB_passthrough";
  case PassthroughBackend::htc: return "host XR_HTC_passthrough";
  case PassthroughBackend::blend: return "host environment blend mode";
  case PassthroughBackend::camera: return "OpenVR headset camera";
  case PassthroughBackend::external: return "external passthrough source";
  case PassthroughBackend::color_key: return "color key";
  default: return "none";
  }
}
PassthroughCapabilities passthrough_capabilities(PassthroughBackend value) {
  switch (value) {
  case PassthroughBackend::native: {
    // Whatever the host implements: its properties and calls are forwarded.
    const auto flags = host_probe().fb_capabilities;
    return {.forwarded = true, .color = bool(flags & XR_PASSTHROUGH_CAPABILITY_COLOR_BIT_FB),
            .depth = bool(flags & XR_PASSTHROUGH_CAPABILITY_LAYER_DEPTH_BIT_FB),
            .opacity = true, .layer_order = true, .styles = true, .projected = true};
  }
  case PassthroughBackend::htc: return {.color = true, .opacity = true, .layer_order = true};
  case PassthroughBackend::blend:
    // ADDITIVE (optical see-through) always shows the world: no opacity.
    return {.color = true, .opacity = host_probe().world == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND};
  case PassthroughBackend::camera: return {.color = true, .opacity = true, .layer_order = true};
  case PassthroughBackend::external: return {.color = true, .opacity = true};
  // Opacity moves the hard key threshold: the key shows where 1 - P(1 - A) < 1/2.
  case PassthroughBackend::color_key: return {.color = true, .opacity = true};
  default: return {};
  }
}
XrEnvironmentBlendMode passthrough_blend_mode() { return host_probe().world; }
void reset_passthrough_probe() {
  probe = {};
  commit = Commit::none;
  external_probe.reset();
  camera.stop();
}
bool local_passthrough() {
  return commit == Commit::local || (commit == Commit::none && is_local(backend()));
}
void withdraw_passthrough_source() { withdraw(); }
void suspend_passthrough_source() { session().passthrough_suspended = true; withdraw(); }
bool passthrough_properties(XrBaseOutStructure* property) {
  if (!enabled("XR_FB_passthrough") || native()) return false;
  const auto chosen = backend();
  const auto caps = passthrough_capabilities(chosen);
  const bool supported = chosen != PassthroughBackend::none;
  if (property->type == XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB) {
    reinterpret_cast<XrSystemPassthroughPropertiesFB*>(property)->supportsPassthrough = supported ? XR_TRUE : XR_FALSE;
    return true;
  }
  if (property->type == XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB) {
    reinterpret_cast<XrSystemPassthroughProperties2FB*>(property)->capabilities = !supported ? 0
        : XR_PASSTHROUGH_CAPABILITY_BIT_FB | (caps.color ? XR_PASSTHROUGH_CAPABILITY_COLOR_BIT_FB : 0) |
          (caps.depth ? XR_PASSTHROUGH_CAPABILITY_LAYER_DEPTH_BIT_FB : 0);
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
namespace {
// The host shows the world through its see-through blend mode. Quest composes
// reconstruction passthrough as a layer at its place in the list, so guest layers
// under the top running one are hidden (exact at opacity 1) and the rest are
// submitted unchanged over the world. Nothing is copied: the opacity, when below
// 1, is applied to the lowest remaining layer's alpha by its host copy.
XrResult plan_blend_frame(XrSession target, const XrFrameEndInfo& info, PassthroughFrame& out, bool keyed = false) {
  const Layer* top = nullptr;
  for (std::uint32_t i = 0; i < info.layerCount; ++i) {
    const auto* header = info.layers[i];
    if (!header || header->type != XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB) continue;
    const auto result = validate_passthrough_layer(target, header);
    if (XR_FAILED(result)) return result;
    const auto& record = *layers.at(reinterpret_cast<const XrCompositionLayerPassthroughFB*>(header)->layerHandle);
    if (!running(record)) continue;
    top = &record;
    out.first_layer = i + 1;
  }
  if (!top) return XR_SUCCESS;
  if (keyed) {
    // The host stays opaque; the lowest remaining layer's host copy carries the
    // key where the world shows (exact for any key, in the attachment's encoding).
    const auto& rgb = *process_config().passthrough_key;
    const auto linear = [](std::uint8_t value) {
      const double c = value / 255.0;
      return float(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
    };
    out.key = std::array<float, 3>{linear(rgb[0]), linear(rgb[1]), linear(rgb[2])};
    out.underlay_opacity = top->opacity;
    static bool keyed_reported = false;
    if (!std::exchange(keyed_reported, true))
      log::info("xr-passthrough", "Passthrough shown as the color key #{:02X}{:02X}{:02X}", rgb[0], rgb[1], rgb[2]);
    return XR_SUCCESS;
  }
  out.blend_mode = host_probe().world;
  if (out.blend_mode == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) out.underlay_opacity = top->opacity;
  static bool reported = false;
  if (!std::exchange(reported, true))
    log::info("xr-passthrough", "Passthrough shown through the host's environment blend mode {}", int(out.blend_mode));
  return XR_SUCCESS;
}
} // namespace
XrResult passthrough_layer_form(XrSession target, const XrCompositionLayerBaseHeader* header,
                                PassthroughLayerStorage& storage, PassthroughLayerForm& out) {
  out = {};
  const auto chosen = backend();
  if (chosen == PassthroughBackend::native) {
    out.forward = header;
    return XR_SUCCESS;
  }
  const auto& guest = *reinterpret_cast<const XrCompositionLayerPassthroughFB*>(header);
  auto& record = *layers.at(guest.layerHandle);
  if (chosen == PassthroughBackend::htc) {
    // A layer made under another backend gets its HTC passthrough here.
    if (!record.htc) {
      if (const auto result = create_htc(target, record.htc, record.host_space); XR_FAILED(result)) {
        fail(chosen, "xrCreatePassthroughHTC failed (" + std::to_string(int(result)) + ")");
        return passthrough_layer_form(target, header, storage, out);
      }
    }
    if (!running(record)) return XR_SUCCESS;
    // The FB layer's flags member is the common header's layerFlags (registry erratum).
    storage.htc.push_back({XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_HTC, nullptr, guest.flags, record.host_space,
                           record.htc, {XR_TYPE_PASSTHROUGH_COLOR_HTC, nullptr, record.opacity}});
    out.host = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&storage.htc.back());
    return XR_SUCCESS;
  }
  if (chosen == PassthroughBackend::camera) {
    if (!camera.started() && !camera.start(1000)) {
      fail(chosen, "the camera did not stream");
      return passthrough_layer_form(target, header, storage, out);
    }
    if (camera.stalled()) {
      fail(chosen, "the camera stopped delivering frames");
      return passthrough_layer_form(target, header, storage, out);
    }
    if (!running(record)) return XR_SUCCESS;
    const auto planes = out.guest.size();
    const auto result = camera.layers(target, record.opacity, storage.quads, out.guest);
    if (XR_SUCCEEDED(result) || result == XR_ERROR_SESSION_LOST || result == XR_ERROR_INSTANCE_LOST) return result;
    // The camera image cannot be shown on this host: the next backend takes over.
    out.guest.resize(planes);
    fail(chosen, "the camera image could not be shown (" + std::to_string(int(result)) + ")");
    return passthrough_layer_form(target, header, storage, out);
  }
  return XR_SUCCESS;
}
XrResult prepare_passthrough_frame(XrSession target, const XrFrameEndInfo& info,
                                   PassthroughFrame& out, bool& handled) {
  handled = false;
  const auto chosen = backend();
  if (chosen == PassthroughBackend::blend) return plan_blend_frame(target, info, out);
  if (chosen == PassthroughBackend::color_key) return plan_blend_frame(target, info, out, true);
  if (chosen != PassthroughBackend::external) return XR_SUCCESS;
  auto fail = [&](XrResult error) {
    withdraw();
    std::string types;
    for (std::uint32_t i = 0; i < std::min(info.layerCount, 8u); ++i)
      types += (types.empty() ? "" : ",") + std::to_string(info.layers[i] ? int(info.layers[i]->type) : 0);
    log::warn("xr-passthrough", "External source frame refused: result={} layers={} types={}", int(error), info.layerCount, types);
    return error;
  };
  const XrCompositionLayerPassthroughFB* room = nullptr;
  const XrCompositionLayerProjection* scene = nullptr;
  std::uint32_t paused_layers = 0;
  for (std::uint32_t i = 0; i < info.layerCount; ++i) {
    const auto* header = info.layers[i];
    if (!header) return fail(XR_ERROR_LAYER_INVALID);
    if (header->type == XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB) {
      const auto result = validate_passthrough_layer(target, header);
      if (XR_FAILED(result)) return fail(result);
      const auto* submitted = reinterpret_cast<const XrCompositionLayerPassthroughFB*>(header);
      const auto& record = *layers.at(submitted->layerHandle);
      if (!running(record)) {
        ++paused_layers;
        continue;
      }
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
    // Keep the ordinary path (depth, MSAA, formats and async ownership) intact.
    // Only force color alpha while the worker is still closing the old lease.
    out.force_opaque = source && !source->withdrawn();
    return XR_SUCCESS;
  }
  if (!session().running) return fail(XR_ERROR_SESSION_NOT_RUNNING);
  if (session().phase != Session::Phase::begun) return fail(XR_ERROR_CALL_ORDER_INVALID);
  if (info.layerCount - paused_layers != (scene ? 2u : 1u)) return fail(XR_ERROR_FEATURE_UNSUPPORTED);
  if (info.environmentBlendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE) return fail(XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED);
  const auto& layer = *layers.at(room->layerHandle);
  const bool active = !session().passthrough_suspended;
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
  if (active && !source) source = std::make_unique<PassthroughSource>();
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
  suspend_passthrough_source();
  source.reset();
  session().finish_host_end();
  const auto images_result = destroy_passthrough_images();
  if (XR_FAILED(images_result)) return images_result;
  const auto camera_result = camera.release_session();
  if (XR_FAILED(camera_result)) return camera_result;
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
  const auto validation = validate_passthrough_info(info, XR_TYPE_PASSTHROUGH_CREATE_INFO_FB);
  if (XR_FAILED(validation)) return validation;
  for (const auto& [handle, feature] : features)
    if (feature->session == target) return XR_ERROR_FEATURE_ALREADY_CREATED_PASSTHROUGH_FB;
  auto record = std::make_unique<Feature>(target, bool(info->flags & XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB));
  // Other creation flags (depth) are forwarded to a native host only.
  const bool plain = !(info->flags & ~XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB);
  PassthroughBackend chosen = backend();
  XrResult result = XR_SUCCESS;
  if (commit == Commit::none) {
    if (is_local(chosen) && !plain) return XR_ERROR_FEATURE_UNSUPPORTED;
    result = start_backend(target, info, out, chosen);
  } else if (chosen == PassthroughBackend::native) {
    result = host_call<PFN_xrCreatePassthroughFB, "xrCreatePassthroughFB">(target, info, out);
  } else if (chosen == PassthroughBackend::none) {
    result = XR_ERROR_FEATURE_UNSUPPORTED;
  }
  if (XR_SUCCEEDED(result) && is_local(chosen) && !plain) result = XR_ERROR_FEATURE_UNSUPPORTED;
  if (XR_SUCCEEDED(result)) {
    if (chosen != PassthroughBackend::native) *out = reinterpret_cast<XrPassthroughFB>(record.get());
    commit = chosen == PassthroughBackend::native ? Commit::native : Commit::local;
    features[*out] = std::move(record);
    track_handle(*out, "XrPassthroughFB", target);
  } else if (features.empty()) {
    drop_spare_htc();
    if (camera.started()) camera.stop();
  }
  log::info("xr-passthrough", "Create passthrough: backend={} flags={:#x} result={}",
            passthrough_backend_name(chosen), info->flags, int(result));
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
  if (XR_SUCCEEDED(result)) {
    withdraw(); forget_handle(handle); features.erase(handle);
    if (features.empty()) {
      commit = Commit::none;
      drop_spare_htc();
      camera.stop();
    }
  }
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
  const auto validation = validate_passthrough_info(info, XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB);
  if (XR_FAILED(validation)) return validation;
  if (backend() == PassthroughBackend::none) return XR_ERROR_FEATURE_UNSUPPORTED;
  if (std::none_of(features.begin(), features.end(), [&](const auto& entry) { return entry.second->session == target; }))
    return XR_ERROR_FEATURE_REQUIRED_PASSTHROUGH_FB;
  if (!features.contains(info->passthrough) || !belongs(info->passthrough, target)) return XR_ERROR_HANDLE_INVALID;
  if (!native() && ((info->flags & ~XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB) ||
      (info->purpose != XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB && !passthrough_capabilities(backend()).projected)))
    return XR_ERROR_FEATURE_UNSUPPORTED;
  if (layers.size() >= 64) return XR_ERROR_LIMIT_REACHED;
  auto record = std::make_unique<Layer>(target, info->passthrough,
      bool(info->flags & XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB));
  auto result = native() ? host_call<PFN_xrCreatePassthroughLayerFB, "xrCreatePassthroughLayerFB">(target, info, out) : XR_SUCCESS;
  if (backend() == PassthroughBackend::htc) {
    XrResult created = XR_SUCCESS;
    if (spare_htc.htc && spare_htc.session == target) {
      record->htc = std::exchange(spare_htc.htc, XrPassthroughHTC(XR_NULL_HANDLE));
      record->host_space = std::exchange(spare_htc.space, XrSpace(XR_NULL_HANDLE));
      spare_htc = {};
    } else {
      created = create_htc(target, record->htc, record->host_space);
    }
    // The next local backend shows this layer instead.
    if (XR_FAILED(created)) fail(PassthroughBackend::htc, "xrCreatePassthroughHTC failed (" + std::to_string(int(created)) + ")");
    if (backend() == PassthroughBackend::none) result = XR_ERROR_FEATURE_UNSUPPORTED;
  }
  if (!native()) *out = XR_SUCCEEDED(result) ? reinterpret_cast<XrPassthroughLayerFB>(record.get()) : XR_NULL_HANDLE;
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
  auto& record = *layers.at(handle);
  // An HTC layer may still be in a queued host submission: finished above.
  if (record.host_space) { ::xrDestroySpace(record.host_space); record.host_space = XR_NULL_HANDLE; }
  if (record.htc) {
    host_call<PFN_xrDestroyPassthroughHTC, "xrDestroyPassthroughHTC">(record.htc);
    record.htc = XR_NULL_HANDLE;
  }
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
  const auto validation = validate_passthrough_info(style, XR_TYPE_PASSTHROUGH_STYLE_FB);
  if (XR_FAILED(validation)) return validation;
  if (!std::isfinite(style->textureOpacityFactor) || style->textureOpacityFactor < 0 || style->textureOpacityFactor > 1 ||
      !std::isfinite(style->edgeColor.r) || !std::isfinite(style->edgeColor.g) ||
      !std::isfinite(style->edgeColor.b) || !std::isfinite(style->edgeColor.a)) return XR_ERROR_VALIDATION_FAILURE;
  if (native()) return host_call<PFN_xrPassthroughLayerSetStyleFB, "xrPassthroughLayerSetStyleFB">(handle, style);
  // Edge rendering is not something a local backend can show.
  if (style->edgeColor.a != 0 && !passthrough_capabilities(backend()).styles) return XR_ERROR_FEATURE_UNSUPPORTED;
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
