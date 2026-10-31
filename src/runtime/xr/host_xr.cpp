#include "xr/application_identity.h"
#include "xr/host_xr.h"
#include "xr/display_memory.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <openxr/openxr.h>
#include <thread>
#include <vector>
#include <mutex>
#include "xr/native.h"

#include "common/log.h"

namespace venture::xr {
namespace {
std::mutex configuration_mutex;
bool initialized = false;
std::atomic<std::uint64_t> procedure_generation{1};
XrApplicationInfo configured_identity = application_identity({}, {}, 0);
std::chrono::milliseconds system_wait{0};
HostXr initialize_host() {
    HostXr h;
    // The Khronos loader honors XR_RUNTIME_JSON in this process. Never write
    // ActiveRuntime or an active_runtime.json as part of discovery.
    h.get_instance_proc_addr = reinterpret_cast<void *>(&xrGetInstanceProcAddr);
    std::uint32_t count = 0;
    XrResult result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    if (XR_FAILED(result)) {
      log::info("xr", "Host extension enumeration result={}", int(result));
      h.error = host_initialization_error(result, "list its extensions");
      return h;
    }
    std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
    result = xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data());
    if (XR_FAILED(result)) {
      log::info("xr", "Host extension enumeration result={}", int(result));
      h.error = host_initialization_error(result, "list its extensions");
      return h;
    }
    // r4-xr/report.md: SDK strings are not a guest capability list. This is
    // only the host side; guest enumeration is filtered independently.
    const std::set<std::string> usable{"XR_KHR_vulkan_enable",
                                       "XR_KHR_vulkan_enable2",
                                       "XR_KHR_win32_convert_performance_counter_time",
                                       "XR_KHR_convert_timespec_time",
                                       "XR_EXT_hand_tracking",
                                       "XR_EXT_hand_tracking_data_source",
                                       "XR_EXT_hand_joints_motion_range",
                                       "XR_FB_hand_tracking_aim",
                                       "XR_FB_hand_tracking_capsules",
                                       "XR_FB_hand_tracking_mesh",
                                       "XR_FB_display_refresh_rate",
                                       "XR_FB_color_space",
                                       "XR_FB_swapchain_update_state",
                                       "XR_FB_swapchain_update_state_vulkan",
                                       "XR_META_performance_metrics",
                                       "XR_KHR_generic_controller",
                                       "XR_FB_haptic_pcm",
                                       "XR_FB_haptic_amplitude_envelope",
                                       "XR_KHR_composition_layer_depth",
                                       "XR_KHR_swapchain_usage_input_attachment_bit",
                                       "XR_MND_swapchain_usage_input_attachment_bit",
                                       "XR_KHR_composition_layer_cylinder",
                                       "XR_KHR_composition_layer_cube",
                                       "XR_KHR_composition_layer_equirect",
                                       "XR_KHR_composition_layer_equirect2",
                                       "XR_KHR_composition_layer_color_scale_bias",
                                       "XR_FB_space_warp",
                                       "XR_FB_passthrough",
                                       "XR_FB_triangle_mesh",
                                       "XR_EXT_local_floor",
                                       "XR_EXT_performance_settings",
                                       "XR_EXT_view_configuration_depth_range"};
    for (const auto &e : extensions) {
      h.available_extensions.emplace(e.extensionName, e.extensionVersion);
      if (usable.contains(e.extensionName))
        h.enabled_extensions.insert(e.extensionName);
      log::debug("xr", "Host extension {} v{}", e.extensionName, e.extensionVersion);
    }
    if (!h.enabled_extensions.contains("XR_KHR_vulkan_enable2")) {
      log::warn("xr", "Host runtime lacks required XR_KHR_vulkan_enable2; flat mode");
      h.error = "The active OpenXR runtime does not support Vulkan (XR_KHR_vulkan_enable2)";
      return h;
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
    result = xrCreateInstance(&info, &h.instance);
    log::info("xr", "Host xrCreateInstance result={}", int(result));
    if (XR_FAILED(result)) {
      h.error = host_initialization_error(result, "create an instance");
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
      }
    }
    if (result == XR_ERROR_RUNTIME_UNAVAILABLE)
      h.error = host_initialization_error(result, "get the headset system");
    else if (result == XR_ERROR_FORM_FACTOR_UNAVAILABLE)
      h.error = h.runtime_name + " reports no headset. Connect the headset, start its runtime and try again";
    else if (!h.available)
      h.error = h.runtime_name + " has no head-mounted display system (XrResult " +
                std::to_string(int(result)) + ")";
    // Process lifetime: the GPU may outlive other static objects and refers to
    // this instance. Guest destruction never tears down the shared host instance.
    return h;
}
}
std::string host_initialization_error(std::int32_t result, const char *operation) {
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
bool shutdown_host_xr() {
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
