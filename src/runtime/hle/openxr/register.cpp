#include "common/log.h"
#include "core/hle.h"
#include "core/linker.h"
#include "core/process.h"
#include "hle/openxr/state.h"
#include "xr/marshal.h"

namespace venture::hle::openxr {
namespace {
bool try_action_state(hle::CallContext& ctx) {
  // Lifecycle owners can await guest invalidation while holding this lock.
  // These selected reads have no guest callbacks, ownership changes or waits.
  std::unique_lock lock(state().mutex, std::try_to_lock);
  if (!lock.owns_lock()) return false;
  ctx.info().handler(ctx); // Retain the existing validation and host forwarding.
  return true;
}
const std::map<std::string, GuestAddr> &thunks() {
  static const auto functions = [] {
    std::map<std::string, GuestAddr> result;
#define VENTURE_XR_COMMAND(name, ext, manual)                                                           \
  {                                                                                              \
    const bool action_read = process_config().isa() == GuestIsa::a64 &&                            \
        (std::string_view(#name) == "xrGetActionStateBoolean" ||                                    \
         std::string_view(#name) == "xrGetActionStateFloat");                                      \
    result[#name] = hle::make_thunk("libopenxr_loader.so", #name, wrap_xr<&guest::name>(), nullptr,    \
        action_read ? hle::Execution::InJitFp : hle::Execution::RunLoop,                            \
        action_read ? try_action_state : nullptr);                                                \
  }
#include "hle/openxr/generated/commands.inc"
    VENTURE_XR_COMMAND(xrSetAndroidApplicationThreadKHR, "", true)
    VENTURE_XR_COMMAND(xrGetOpenGLESGraphicsRequirementsKHR, "", true)
    VENTURE_XR_COMMAND(xrConvertTimespecTimeToTimeKHR, "", true)
    VENTURE_XR_COMMAND(xrConvertTimeToTimespecTimeKHR, "", true)
    if (process_config().isa() == GuestIsa::a64) {
      VENTURE_XR_COMMAND(xrCreateSwapchainAndroidSurfaceKHR, "XR_KHR_android_surface_swapchain", true)
    }
#undef VENTURE_XR_COMMAND
    result["_Z21xrInitializeLoaderKHRPK29XrLoaderInitInfoBaseHeaderKHR"] =
        result.at("xrInitializeLoaderKHR");
    return result;
  }();
  return functions;
}
} // namespace
GuestAddr command_thunk(std::string_view name) {
  auto it = thunks().find(std::string(name));
  return it == thunks().end() ? 0 : it->second;
}
void register_library() {
  install_angle_xr_images();
  static std::once_flag once;
  std::call_once(once, [] {
    auto root = process_config().guest_native_lib_dir();
    auto lib = std::make_shared<HleLibrary>(
        "libopenxr_loader.so",
        std::vector<std::string>{
            "/system/lib64/libopenxr_loader.so", "/system/lib/libopenxr_loader.so",
            "libopenxr_forwardloader.so", "/system/lib64/libopenxr_forwardloader.so",
            root + "/libopenxr_loader.so",
            process_config().guest_apk_path() + "!/lib/arm64-v8a/libopenxr_loader.so",
            "/system/priv-app/VrDriver/VrDriver.apk!/lib/arm64-v8a/libopenxr_forwardloader.so"});
    for (auto &[name, thunk] : thunks())
      lib->add_function(name, thunk);
    Linker::get().register_hle_library(std::move(lib));
  });
}
namespace guest {
XrResult xrGetInstanceProcAddr(XrInstance instance, const char *name, PFN_xrVoidFunction *out) {
  std::lock_guard lock(state().mutex);
  if (!out || !name)
    return XR_ERROR_VALIDATION_FAILURE;
  *out = nullptr;
  const std::string_view n(name);
  const bool global = n == "xrGetInstanceProcAddr" || n == "xrEnumerateApiLayerProperties" ||
                      n == "xrEnumerateInstanceExtensionProperties" || n == "xrCreateInstance" ||
                      n == "xrInitializeLoaderKHR";
  if (instance && !valid_handle(instance, "XrInstance"))
    return XR_ERROR_HANDLE_INVALID;
  if (!instance && !global)
    return XR_ERROR_HANDLE_INVALID;
  if (n == "xrLocateSpaces" && state().api_version < XR_MAKE_VERSION(1, 1, 0))
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  std::string_view extension;
  for (const auto &c : command_metadata())
    if (n == c.name)
      extension = c.extension;
  if (n == "xrSetAndroidApplicationThreadKHR")
    extension = "XR_KHR_android_thread_settings";
  if (n == "xrGetOpenGLESGraphicsRequirementsKHR")
    extension = "XR_KHR_opengl_es_enable";
  if (n == "xrConvertTimespecTimeToTimeKHR" || n == "xrConvertTimeToTimespecTimeKHR")
    extension = "XR_KHR_convert_timespec_time";
  if (!global && !extension.empty() && !enabled(extension))
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  auto thunk = command_thunk(n);
  log::debug("xr", "Guest GIPA {}: {}", n, thunk ? "thunk" : "unsupported");
  if (!thunk)
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  *out = reinterpret_cast<PFN_xrVoidFunction>(static_cast<std::uintptr_t>(thunk));
  return XR_SUCCESS;
}
} // namespace guest
} // namespace venture::hle::openxr
