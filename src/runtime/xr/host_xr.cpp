#include "xr/application_identity.h"
#include "xr/host_xr.h"
#include "xr/host_extensions.h"
#include "xr/display_memory.h"
#include "xr/openxr_layers.h"
#include "common/environment.h"
#include "common/openxr_layers.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string_view>
#include <filesystem>
#include <format>
#include <openxr/openxr.h>
#include <thread>
#include <vector>
#include <mutex>
#include "xr/native.h"

#include "common/log.h"

namespace venture::xr {
namespace {
std::mutex configuration_mutex;
// Host audio queries the instance from its own thread; shutdown waits for them.
std::mutex audio_endpoint_mutex;
bool initialized = false;
std::atomic<std::uint64_t> procedure_generation{1};
XrApplicationInfo configured_identity = application_identity({}, {}, 0);
std::chrono::milliseconds system_wait{0};
// Lists the host extensions and creates the instance on `h`; on failure
// `operation` names the step for host_initialization_error, or is null when
// the runtime lacks Vulkan (h.error says so).
XrResult create_instance(HostXr &h, const char *&operation) {
    h.available_extensions.clear();
    h.enabled_extensions.clear();
    operation = "list its extensions";
    std::uint32_t count = 0;
    XrResult result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    if (XR_FAILED(result)) {
      log::info("xr", "Host extension enumeration result={}", int(result));
      return result;
    }
    std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
    result = xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data());
    if (XR_FAILED(result)) {
      log::info("xr", "Host extension enumeration result={}", int(result));
      return result;
    }
    std::set<std::string> usable(std::begin(kHostExtensions), std::end(kHostExtensions));
#ifdef _WIN32
    // The headset's audio endpoints, for host audio (headset_audio_endpoint).
    usable.insert("XR_OCULUS_audio_device_guid");
#endif
    // The gaze for eye-tracked foveated rendering (xr/eye_gaze.h): Dynamic
    // (VENTURE_FOVEATION unset or dynamic) follows it; fixed and off never do.
    if (const char* fov = environment::getenv("VENTURE_FOVEATION");
        !fov || (std::string_view(fov) != "fixed" && std::string_view(fov) != "off"))
      usable.insert("XR_EXT_eye_gaze_interaction");
    // Passthrough backends are chosen by what the host offers (hle/openxr/passthrough.cpp);
    // XR_HTC_passthrough is only a host-side target and never offered to guests.
    if (environment::passthrough_enabled()) {
      usable.insert("XR_FB_passthrough");
      usable.insert("XR_META_passthrough_color_lut");
      usable.insert("XR_FB_triangle_mesh");
      usable.insert("XR_HTC_passthrough");
    }
    for (const auto &e : extensions) {
      h.available_extensions.emplace(e.extensionName, e.extensionVersion);
      if (usable.contains(e.extensionName))
        h.enabled_extensions.insert(e.extensionName);
      log::debug("xr", "Host extension {} v{}", e.extensionName, e.extensionVersion);
    }
    if (!h.enabled_extensions.contains("XR_KHR_vulkan_enable2")) {
      log::warn("xr", "Host runtime lacks required XR_KHR_vulkan_enable2; flat mode");
      h.error = "The active OpenXR runtime does not support Vulkan (XR_KHR_vulkan_enable2)";
      operation = nullptr;
      return XR_ERROR_EXTENSION_NOT_PRESENT;
    }
    std::vector<const char *> names;
    for (const auto &e : h.enabled_extensions)
      names.push_back(e.c_str());
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    info.applicationInfo = configured_identity;
    // SteamVR's supported API is queried through success, not assumed to be 1.1.
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    info.enabledExtensionCount = static_cast<std::uint32_t>(names.size());
    info.enabledExtensionNames = names.data();
    operation = "create an instance";
    result = xrCreateInstance(&info, &h.instance);
    log::info("xr", "Host xrCreateInstance result={}", int(result));
    return result;
}
bool creating_instance(const char *operation) {
    return operation && std::string_view(operation) == "create an instance";
}
// The host instance failed at xrCreateInstance with `failed`: leaves out the
// enabled implicit API layers that are broken and creates it again; every
// working layer stays (xr/openxr_layers.h). A layer whose library does not load
// or negotiate is found directly. When none is, and the instance starts with
// no implicit layer at all, each layer is added back in the loader's order
// and the ones the instance fails with stay out. `unloadable` gets the
// layers that do not load, for the error text.
XrResult create_without_broken_layers(HostXr &h, const char *&operation, XrResult failed, std::string &unloadable) {
    auto layers = enabled_openxr_layers();
    std::vector<ApiLayer> broken, working;
    for (auto &layer : layers) (layer.reason.empty() ? working : broken).push_back(std::move(layer));
    auto report = [&](const ApiLayer &layer) {
      log::warn("xr", "OpenXR API layer broken: {} ({}): {}", layer.name, layer.manifest, layer.reason);
      h.skipped_api_layers.push_back(layer.name);
    };
    auto create_without = [&](const std::vector<ApiLayer> &skipped) {
      if (!skip_openxr_layers(skipped)) {
        log::warn("xr", "The OpenXR loader refused its layer properties; no layer can be left out");
        return XR_ERROR_INITIALIZATION_FAILED;
      }
      return create_instance(h, operation);
    };
    auto names = [](const std::vector<ApiLayer> &list) {
      std::string out;
      for (const auto &layer : list) out += (out.empty() ? "" : ", ") + layer.name;
      return out.empty() ? std::string("none") : out;
    };
    XrResult result = failed;
    if (!broken.empty()) {
      for (const auto &layer : broken) {
        report(layer);
        unloadable += std::format("{}the OpenXR API layer {} ({}) could not be loaded: {}", unloadable.empty() ? "" : "; ",
                                  layer.name, layer.manifest, layer.reason);
      }
      log::warn("xr", "Creating the host instance again without the broken OpenXR API layers: {}; the others stay: {}",
                names(broken), names(working));
      result = create_without(broken);
    }
    if (XR_SUCCEEDED(result) || !creating_instance(operation) || result == XR_ERROR_RUNTIME_UNAVAILABLE ||
        working.empty())
      return result;
    // Whether an implicit layer fails it at all.
    auto none = broken;
    none.insert(none.end(), working.begin(), working.end());
    const XrResult with_layers = result;
    result = create_without(none);
    if (XR_FAILED(result)) {
      log::info("xr", "The host instance fails without implicit OpenXR API layers too (XrResult {}); none is left "
                "out for that", int(result));
      skip_openxr_layers(broken);
      return result;
    }
    log::warn("xr", "The host instance starts without the implicit OpenXR API layers ({}) and failed with them "
              "(XrResult {}); adding them back one at a time", names(working), int(with_layers));
    for (std::size_t i = 0; i < working.size(); ++i) {
      if (h.instance) xrDestroyInstance(h.instance);
      h.instance = XR_NULL_HANDLE;
      auto skipped = broken;
      skipped.insert(skipped.end(), working.begin() + std::ptrdiff_t(i) + 1, working.end());
      result = create_without(skipped);
      if (XR_FAILED(result) && creating_instance(operation)) {
        working[i].reason = std::format("xrCreateInstance fails with XrResult {} while it is loaded", int(result));
        report(working[i]);
        broken.push_back(working[i]);
      }
    }
    // The last layer added back failed it: the instance without it.
    if (XR_FAILED(result)) result = create_without(broken);
    if (XR_SUCCEEDED(result))
      log::warn("xr", "The host instance runs without the broken OpenXR API layers: {}", names(broken));
    return result;
}
HostXr initialize_host() {
    HostXr h;
    // The Khronos loader honors XR_RUNTIME_JSON in this process. Never write
    // ActiveRuntime or an active_runtime.json as part of discovery.
    h.get_instance_proc_addr = reinterpret_cast<void *>(&xrGetInstanceProcAddr);
    // The loader loads the runtime and every implicit API layer here; a damaged
    // DLL among them must fail its load, not stop the start behind a dialog.
    QuietLibraryErrors quiet_library_errors;
    // A host's implicit API layers sit between Venture and the runtime and can
    // reach into the shared Vulkan device; name each with the variable that
    // turns it off ([facts]: debug reports keep these lines whole).
    {
      std::string listed;
      for (const auto &path : openxr_layers::implicit_manifests()) {
        const auto layer = openxr_layers::read(path);
        if (layer.name.empty()) continue;
        const bool off = !openxr_layers::enabled(layer) ||
                         std::ranges::any_of(user_left_out_layers(), [&](const ApiLayer &l) { return l.name == layer.name; });
        listed += std::format("{}{} ({}{})", listed.empty() ? "" : ", ", layer.name, off ? "off; " : "",
                              layer.disable_environment.empty() ? std::string("no disable variable")
                                                                : "off with " + layer.disable_environment + "=1");
      }
      log::info("facts", "Host OpenXR implicit API layers: {}", listed.empty() ? "none" : listed);
    }
    if (!user_left_out_layers().empty()) {
      std::string names;
      for (const auto &layer : user_left_out_layers()) {
        names += (names.empty() ? "" : ", ") + layer.name;
        h.skipped_api_layers.push_back(layer.name);
      }
      if (skip_openxr_layers({}))
        log::warn("xr", "OpenXR API layers left out by VENTURE_OPENXR_LAYERS_OFF: {}", names);
      else
        log::warn("xr", "The OpenXR loader refused its layer properties; VENTURE_OPENXR_LAYERS_OFF has no effect");
    }
    const char *operation = nullptr;
    XrResult result = create_instance(h, operation);
    std::string unloadable;
    if (XR_FAILED(result) && creating_instance(operation) && result != XR_ERROR_RUNTIME_UNAVAILABLE)
      result = create_without_broken_layers(h, operation, result, unloadable);
    if (XR_FAILED(result)) {
      if (operation) {
        h.error = host_initialization_error(result, operation);
        if (!unloadable.empty())
          h.error += ". Also, " + unloadable + ". Repair or uninstall the program that installed it";
      }
      return h;
    }
    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(h.instance, &properties))) {
      h.runtime_name = properties.runtimeName;
      h.runtime_version = properties.runtimeVersion;
    }
    XrSystemGetInfo system{XR_TYPE_SYSTEM_GET_INFO};
    system.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    result = xrGetSystem(h.instance, &system, &h.system);
    if (result == XR_ERROR_FORM_FACTOR_UNAVAILABLE && system_wait.count() > 0) {
      // OpenXR 1.0 xrGetSystem: the form factor is supported but not
      // currently available (headset off, runtime still starting); retry.
      log::info("xr", "Host {}: no headset yet; waiting up to {} s", h.runtime_name,
                std::chrono::duration_cast<std::chrono::seconds>(system_wait).count());
      result = static_cast<XrResult>(wait_for_system(
          [&] { return static_cast<std::int32_t>(xrGetSystem(h.instance, &system, &h.system)); },
          system_wait, std::chrono::milliseconds(500)));
    }
    h.available = XR_SUCCEEDED(result);
    h.system_result = result;
    log::info("xr", "Host {} {}.{}.{}: HMD result {}{}", h.runtime_name,
              XR_VERSION_MAJOR(h.runtime_version), XR_VERSION_MINOR(h.runtime_version),
              XR_VERSION_PATCH(h.runtime_version), int(result), h.available ? "" : " (flat mode)");
    if (h.available) {
      XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
      if (XR_SUCCEEDED(xrGetSystemProperties(h.instance, h.system, &system_properties))) {
        recall_display_period(h.runtime_name, system_properties.systemName);
        // [facts] lines are collected whole into debug reports.
        XrViewConfigurationView views[2]{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
        std::uint32_t count = 0;
        if (XR_FAILED(xrEnumerateViewConfigurationViews(h.instance, h.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                        2, &count, views)))
          count = 0;
        log::info("facts", "Host runtime: {} {}.{}.{}; headset: {} (vendor {:#x}); recommended eye {}x{}, max {}x{}",
                  h.runtime_name, XR_VERSION_MAJOR(h.runtime_version), XR_VERSION_MINOR(h.runtime_version),
                  XR_VERSION_PATCH(h.runtime_version), system_properties.systemName, system_properties.vendorId,
                  count ? views[0].recommendedImageRectWidth : 0, count ? views[0].recommendedImageRectHeight : 0,
                  count ? views[0].maxImageRectWidth : 0, count ? views[0].maxImageRectHeight : 0);
        if (count)
          remember_host_views(h.runtime_name, system_properties.systemName, views[0].recommendedImageRectWidth,
                              views[0].recommendedImageRectHeight, views[0].maxImageRectWidth,
                              views[0].maxImageRectHeight);
      }
    }
    if (result == XR_ERROR_RUNTIME_UNAVAILABLE)
      h.error = host_initialization_error(result, "get the headset system");
    else if (result == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
      h.error = h.runtime_name + " reports no headset. Connect the headset, start its runtime and try again";
      // Another installed runtime may be the one the headset is on (a Steam Frame on SteamVR
      // while Virtual Desktop is the active runtime, #161).
      std::string others;
      for (const auto &r : venture::openxr_layers::installed_runtimes())
        if (r.name != h.runtime_name) others += (others.empty() ? "" : ", ") + r.name;
      if (!others.empty())
        h.error += ", or choose the runtime your headset uses (" + others + ") in Venture's Settings > VR runtime";
    }
    else if (!h.available)
      h.error = h.runtime_name + " has no head-mounted display system (XrResult " +
                std::to_string(int(result)) + ")";
    // Process lifetime: the GPU may outlive other static objects and refers to
    // this instance. Guest destruction never tears down the shared host instance.
    return h;
}
}
std::string host_initialization_error(std::int32_t result, const char *operation) {
  // In the Flatpak SteamVR's folder may not be in the sandbox (a Steam library
  // on another drive), whatever the error says: name the folder and the grant.
  const auto steamvr = venture::environment::flatpak_steamvr_state();
  if (steamvr.steamvr && !steamvr.visible) {
    if (steamvr.hidden_folder.empty())
      return "SteamVR's folder is not visible in the Venture Flatpak. Use the Venture AppImage, tarball or your "
             "distribution's package with SteamVR.";
    return "SteamVR's folder is not visible in the Venture Flatpak: " + steamvr.hidden_folder +
           ". Give Venture read access in a terminal with: " +
           venture::environment::flatpak_override_command(steamvr.hidden_folder) +
           " and press Play again, or use the Venture AppImage.";
  }
  // SteamVR runs its OpenXR client inside Flatpak and other sandboxes from 2.19.1 on.
  if (steamvr.steamvr && (result == XR_ERROR_RUNTIME_UNAVAILABLE || result == XR_ERROR_RUNTIME_FAILURE))
    return std::string("SteamVR could not ") + operation + " (XrResult " + std::to_string(result) +
           "). In the Venture Flatpak, SteamVR 2.19.1 or newer is needed: older SteamVR does not run in a sandbox. "
           "Make sure SteamVR is up to date and running, then press Play again, or use the Venture AppImage.";
  if (result == XR_ERROR_RUNTIME_UNAVAILABLE)
    return "Your configured OpenXR runtime is unavailable. Make sure your headset and its runtime service are running, then press Play again.";
  // SteamVR answers xrCreateInstance this way while its server is stuck or
  // still shutting down from a previous session; a restart clears it.
  if (result == XR_ERROR_RUNTIME_FAILURE)
    return std::string("The OpenXR runtime failed to ") + operation +
           " (XrResult -2). Close your VR runtime (SteamVR: quit it from its window) and start it again, then press Play again.";
  return std::string("The OpenXR runtime could not ") + operation +
         " (XrResult " + std::to_string(result) + ")";
}
std::int32_t wait_for_system(const std::function<std::int32_t()> &get_system,
                             std::chrono::milliseconds limit, std::chrono::milliseconds interval) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  for (;;) {
    const auto result = get_system();
    const auto now = std::chrono::steady_clock::now();
    if (result != XR_ERROR_FORM_FACTOR_UNAVAILABLE || now >= deadline) return result;
    std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(interval, deadline - now));
  }
}
bool set_application_identity(std::string_view label, std::string_view package, std::int64_t version) {
  std::lock_guard lock(configuration_mutex);
  if (initialized) return false;
  configured_identity = application_identity(label, package, version);
  return true;
}
std::string application_name() {
  std::lock_guard lock(configuration_mutex);
  return configured_identity.applicationName;
}
bool set_system_wait(std::chrono::milliseconds limit) {
  std::lock_guard lock(configuration_mutex);
  if (initialized) return false;
  system_wait = limit;
  return true;
}
HostXr &host_xr() {
  // The loader's default (system active) runtime; there is no fallback runtime.
  static HostXr state = [] {
    std::lock_guard lock(configuration_mutex);
    initialized = true;
    // The launch boundary logs the failure once and returns a nonzero status.
    return initialize_host();
  }();
  return state;
}
std::uint64_t host_procedure_generation() {
  return procedure_generation.load(std::memory_order_acquire);
}
std::string headset_audio_endpoint(bool input) {
#ifdef _WIN32
  std::lock_guard lock(audio_endpoint_mutex);
  auto& host = host_xr();
  if (!host.instance || !host.enabled_extensions.contains("XR_OCULUS_audio_device_guid")) return {};
  auto get = reinterpret_cast<PFN_xrGetInstanceProcAddr>(host.get_instance_proc_addr);
  PFN_xrVoidFunction raw{};
  if (!get || XR_FAILED(get(host.instance, input ? "xrGetAudioInputDeviceGuidOculus" : "xrGetAudioOutputDeviceGuidOculus",
                            &raw)) || !raw)
    return {};
  // XR_MAX_AUDIO_DEVICE_STR_SIZE_OCULUS wide characters.
  wchar_t buffer[128]{};
  if (XR_FAILED(reinterpret_cast<XrResult(XRAPI_PTR *)(XrInstance, wchar_t *)>(raw)(host.instance, buffer))) return {};
  buffer[std::size(buffer) - 1] = 0;
  // An MMDevice endpoint id ("{0.0.0.00000000}.{guid}") is ASCII; never throw.
  std::string text;
  for (const wchar_t* c = buffer; *c; ++c) text += *c < 0x80 ? static_cast<char>(*c) : '?';
  return text;
#else
  (void)input;
  return {};
#endif
}
bool shutdown_host_xr() {
  std::lock_guard audio_lock(audio_endpoint_mutex);
  auto& host = host_xr();
  if (!host.instance) return true;
  auto get = reinterpret_cast<PFN_xrGetInstanceProcAddr>(host.get_instance_proc_addr);
  PFN_xrVoidFunction raw{};
  if (!get || XR_FAILED(get(host.instance, "xrDestroyInstance", &raw)) || !raw) return false;
  if (XR_FAILED(reinterpret_cast<PFN_xrDestroyInstance>(raw)(host.instance))) return false;
  host.instance = nullptr; host.system = 0; host.available = false;
  invalidate_host_procedures();
  return true;
}
void finish_runtime_diagnostics() noexcept {
  try {
    {
      std::lock_guard lock(configuration_mutex);
      if (!initialized) return;
    }
    auto& host = host_xr();
    if (!host.instance) return;
    auto get = reinterpret_cast<PFN_xrGetInstanceProcAddr>(host.get_instance_proc_addr);
    PFN_xrVoidFunction finish{};
    if (get && XR_SUCCEEDED(get(host.instance, "xrVentureFinishDiagnostics", &finish)) && finish)
      reinterpret_cast<void (XRAPI_PTR*)()>(finish)();
  } catch (...) {} // Diagnostic output is best-effort; the supervisor bounds it.
}
void invalidate_host_procedures() {
  procedure_generation.fetch_add(1, std::memory_order_acq_rel);
}
} // namespace venture::xr
