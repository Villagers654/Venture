// The process-wide host OpenXR instance used by both guest XR APIs.
//
// Venture creates ONE host XrInstance + XrSystemId at startup (before guest
// graphics initialization, because the shared Vulkan device must be created
// through XR_KHR_vulkan_enable2). Guest OpenXR (libopenxr_loader.so HLE) and
// guest VrApi (libvrapi.so HLE) are both implemented on top of it; a guest
// xrCreateInstance returns a guest handle bound to this host instance, and
// guest sessions map to one host XrSession.
//
// The host instance enables every host extension Venture can make use of
// (graphics binding, hand tracking, refresh rate, ...); which ones the guest
// sees is decided separately by the guest-facing capability policy.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>

typedef struct XrInstance_T *XrInstance;
typedef std::uint64_t XrSystemId;

namespace venture::xr {

struct HostXr {
  bool available = false; // false: no runtime / no HMD -> flat debug mode
  XrInstance instance = nullptr;
  XrSystemId system = 0;
  std::string runtime_name; // host runtime, e.g. "SteamVR/OpenXR"
  std::uint64_t runtime_version = 0;
  std::set<std::string> enabled_extensions; // host extensions enabled on `instance`
  void *get_instance_proc_addr = nullptr;   // PFN_xrGetInstanceProcAddr of the host loader
  std::map<std::string, std::uint32_t> available_extensions;
  std::int32_t system_result = -51; // XR_ERROR_RUNTIME_UNAVAILABLE until queried
  std::string error; // why `available` is false, phrased for the user
};

// Initializes on first call (loads the host OpenXR loader, creates the
// instance and system). Thread-safe.
HostXr &host_xr();
bool shutdown_host_xr(); // host-only, after sessions/devices have been retired
// Optional host-runtime diagnostic flush before terminal process exit. Does
// not initialize XR or end the session; unsupported runtimes are unchanged.
void finish_runtime_diagnostics() noexcept;
// Guest instance boundaries invalidate forwarding slots even when the shared
// process-lifetime host instance is reused.
std::uint64_t host_procedure_generation();
void invalidate_host_procedures();
// Describe a loader failure without consulting runtime registration.
std::string host_initialization_error(std::int32_t result, const char *operation);
// Configure before first use: keep polling xrGetSystem for up to `limit`
// while the runtime reports the headset as temporarily unavailable
// (XR_ERROR_FORM_FACTOR_UNAVAILABLE, e.g. while SteamVR starts). The default
// zero queries once. Returns false once host_xr() has initialized.
bool set_application_identity(std::string_view label, std::string_view package, std::int64_t version);
bool set_system_wait(std::chrono::milliseconds limit);
// Calls `get_system` until it returns something other than
// XR_ERROR_FORM_FACTOR_UNAVAILABLE or `limit` has elapsed; returns the last result.
std::int32_t wait_for_system(const std::function<std::int32_t()> &get_system,
                             std::chrono::milliseconds limit, std::chrono::milliseconds interval);

} // namespace venture::xr
