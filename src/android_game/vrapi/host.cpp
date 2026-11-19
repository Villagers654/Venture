// SPDX-License-Identifier: GPL-3.0-or-later
// The VrApi core's host (hle/vrapi/host.h) inside a game on an Android
// headset: the game renders with its own GLES context into the runtime's
// swapchain images, so the session is made on that context and kept from the
// first swapchain until vrapi_Shutdown.
#include "android_game/vrapi/platform_xr.h"

#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>

#include "android_game/vrapi/native.h"
#include "android_game/vrapi/vulkan.h"
#include "common/log.h"
#include "core/device_identity.h"
#include "hle/vrapi/host.h"
#include "hle/vrapi/state.h"

namespace venture::vrapi_native {
int futex_wait(const std::uint32_t *word, std::uint32_t expected, std::int64_t deadline_ns) {
  timespec deadline{static_cast<time_t>(deadline_ns / 1'000'000'000), static_cast<long>(deadline_ns % 1'000'000'000)};
  const auto r = syscall(SYS_futex, word, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG, expected,
                         deadline_ns < 0 ? nullptr : &deadline, nullptr, FUTEX_BITSET_MATCH_ANY);
  return r < 0 ? -errno : 0;
}

namespace {
// A session on `context` (the game's), whose EGL config the binding names.
XrResult create_gles_session(EGLDisplay display, EGLContext context) {
  if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT) {
    log::warn("vrapi", "No EGL context is current: the game's GLES context makes the session");
    return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  }
  EGLint id = 0, found = 0;
  EGLConfig config{};
  if (!eglQueryContext(display, context, EGL_CONFIG_ID, &id)) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  const EGLint query[]{EGL_CONFIG_ID, id, EGL_NONE};
  if (!eglChooseConfig(display, query, &config, 1, &found) || found != 1) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  auto &h = xr::host_xr();
  if (!h.available) return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
  if (!h.enabled_extensions.contains("XR_KHR_opengl_es_enable")) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  // OpenXR requires the requirements query before the session.
  XrGraphicsRequirementsOpenGLESKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
  auto r = xr::host_call<PFN_xrGetOpenGLESGraphicsRequirementsKHR, "xrGetOpenGLESGraphicsRequirementsKHR">(
      h.instance, h.system, &requirements);
  if (XR_FAILED(r)) return r;
  XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR, nullptr, display,
                                              config, context};
  std::lock_guard sl(xr::session().mutex);
  return xr::session().create_native(xr::SessionOwner::vrapi, &binding);
}
}  // namespace

XrResult ensure_session() {
  if (xr::session().handle) return XR_SUCCESS;
  // A Vulkan game's queue is known at EnterVrMode; a swapchain made before it
  // gets the session on the queue games create (vulkan.h).
  if (hle::vrapi::state().init.graphics_api == hle::vrapi::vulkan_api) return vulkan::create_session(VK_NULL_HANDLE);
  return create_gles_session(eglGetCurrentDisplay(), eglGetCurrentContext());
}
}  // namespace venture::vrapi_native

namespace venture::hle::vrapi::host {
bool vulkan_available() {
  auto &h = xr::host_xr();
  return h.available && h.enabled_extensions.contains("XR_KHR_vulkan_enable");
}
bool images_available() { return true; }
std::int32_t device_type() { return native_device_identity().vrapi_type; }
bool hand_tracking_required() { return false; }
bool vulkan_extension_usable(std::string_view) { return true; }
XrResult create_session(const abi::Mode &mode) {
  if (state().init.graphics_api == vulkan_api) {
    // EnterVrMode checked the mode type: this is ovrModeParmsVulkan.
    const auto queue = reinterpret_cast<VkQueue>(reinterpret_cast<const abi::ModeVulkan &>(mode).queue);
    if (!xr::session().handle) return vrapi_native::vulkan::create_session(queue);
    if (!vrapi_native::vulkan::session_queue(queue))
      log::warn("vrapi", "EnterVrMode names another queue than the one the session was made with");
    return XR_SUCCESS;
  }
  const auto display = reinterpret_cast<EGLDisplay>(mode.display);
  const auto context = reinterpret_cast<EGLContext>(mode.context);
  if (xr::session().handle) return XR_SUCCESS;  // made for the game's first swapchain
  return vrapi_native::create_gles_session(display, context);
}
bool keeps_session() { return true; }
void vr_mode_entered() {}
bool wait_frame_change(std::uint32_t epoch, std::int64_t deadline) {
  vrapi_native::futex_wait(&xr::session().frame_epoch, epoch, deadline);
  return true;
}
}  // namespace venture::hle::vrapi::host
