#include "core/wait.h"
#include "core/process.h"
#include "core/renderer_capabilities.h"
#include "hle/openxr/state.h"
#include "hle/vrapi/state.h"
#include "common/log.h"
#include "hle/vulkan/vulkan.h"
#include "xr/vulkan_extensions.h"
#include "platform/thread_priority.h"
#include <algorithm>
#include <utility>
#include <cmath>
#include <cstring>
#include <set>

namespace venture::hle::vrapi::guest {
namespace {
XrViewConfigurationView view() {
  XrViewConfigurationView out[2]{{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                 {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
  std::uint32_t count = 0;
  if (XR_FAILED(::xrEnumerateViewConfigurationViews(host_xr().instance, host_xr().system,
                                                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                                                    &count, out)))
    return {XR_TYPE_VIEW_CONFIGURATION_VIEW};
  return out[0];
}
std::vector<float> rates() {
  float rate = 0;
  if (XR_SUCCEEDED(hle::openxr::refresh_rate(session().handle, &rate)))
    return {rate};
  return {};
}
XrResult bounds(XrExtent2Df &extent, XrPosef &stage) {
  if (!state().stage)
    return XR_SPACE_BOUNDS_UNAVAILABLE;
  auto r =
      ::xrGetReferenceSpaceBoundsRect(session().handle, XR_REFERENCE_SPACE_TYPE_STAGE, &extent);
  if (r != XR_SUCCESS)
    return r;
  XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
  r = ::xrLocateSpace(state().stage, state().app, time_from_seconds(0), &loc);
  if (XR_SUCCEEDED(r)) {
    hle::openxr::sanitize(loc.locationFlags, loc.pose);
    stage = loc.pose;
  }
  return r;
}
} // namespace
std::int32_t vrapi_SetRemoteEmulation(Mobile *mobile, bool /*enabled*/) {
  std::lock_guard lock(state().mutex);
  if (!valid(mobile)) return invalid;
  // VrApi_Input.h limits this switch to GearVR controller emulation of the
  // headset buttons/touchpad. We expose Quest Touch controllers, never a
  // ModelGearVR remote, so neither setting has an Android event producer to
  // change. A valid session can toggle it without rejecting Touch input.
  return success;
}
std::int32_t vrapi_GetSystemPropertyInt(const Java *, std::int32_t property) {
  std::lock_guard lock(state().mutex);
  switch (property) {
  case 0:
    if (process_config().device_identity->vrapi_type < 0)
      failure("GetSystemPropertyInt device type absent from public VrApi enum", unsupported);
    return process_config().device_identity->vrapi_type;
  case 1:
    return int(view().maxSwapchainSampleCount);
  case 2:
    return int(view().recommendedImageRectWidth * 2);
  case 3:
    return int(view().recommendedImageRectHeight);
  case 5:
    return int(view().recommendedImageRectWidth);
  case 6:
    return int(view().recommendedImageRectHeight);
  case 10:
    return 0; // REGION_UNSPECIFIED
  case 12:
    // Mobile SDK 1.30 VrApi_Types.h: legacy HEADSET_TYPE (Quest family),
    // distinct from DEVICE_TYPE. Older OVRPlugin uses it for HMD presence.
    return host_xr().available ? 256 : -1;
  case 15:
    return 0; // No host dominant-hand preference has been supplied.
  case 16:
  case 17: {
    XrSystemProperties p{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_FAILED(::xrGetSystemProperties(host_xr().instance, host_xr().system, &p)))
      return 0;
    // A host reporting neither (SteamVR 2.18 with a Quest over Link) still tracks.
    const auto& t = p.trackingProperties;
    if (!t.orientationTracking && !t.positionTracking) return 1;
    return property == 16 ? t.orientationTracking : t.positionTracking;
  }
  case 64:
    return int(rates().size());
  case 128:
    // VrApi reports whether its compositor accepts texture-array layers;
    // applications separately check GL_OVR_multiview on their GL context.
    // Meta's Mobile SDK 1.33 release notes retire this query because it is
    // always supported on standalone devices. Our image bridge likewise
    // supports both layers of a stereo array, independently of the producer.
    return host_xr().available && xr_images();
  case 129:
    // Mobile SDK 1.30 SRGB_LAYER_SOURCE_AVAILABLE: the image bridge preserves
    // GL_SRGB8_ALPHA8 as VK_FORMAT_R8G8B8A8_SRGB through compositor submission.
    return host_xr().available && xr_images();
  case 130:
    return renderer_capabilities::vrapi_variable_rate_shading;
  default:
    failure("GetSystemPropertyInt unknown property", property);
    return 0;
  }
}
float vrapi_GetSystemPropertyFloat(const Java *, std::int32_t property) {
  std::lock_guard lock(state().mutex);
  if (property == 4) {
    float rate = 0;
    return XR_SUCCEEDED(hle::openxr::refresh_rate(session().handle, &rate)) ? rate : 0;
  }
  if ((property == 7 || property == 8) && state().initialized) {
    if (state().mobile) {
      auto tracking = vrapi_GetPredictedTracking2(state().mobile, 0);
      auto &p = tracking.eye[0].projection;
      if (p.m[0][0] > 0 && p.m[1][1] > 0)
        return (property == 7 ? 2 * std::atan(1 / p.m[0][0]) : 2 * std::atan(1 / p.m[1][1])) *
               57.2957795f;
    }
    // SUGGESTED_EYE_FOV_DEGREES is queried to construct cameras before
    // EnterVrMode. Recommend a nominal 90-degree camera until a live OpenXR
    // session provides projection data; zero creates a degenerate camera.
    return 90.f;
  }
  failure("GetSystemPropertyFloat unavailable property", property);
  return 0;
}
std::int32_t vrapi_GetSystemPropertyFloatArray(const Java *, std::int32_t property, float *out,
                                               std::int32_t capacity) {
  std::lock_guard lock(state().mutex);
  if (property != 65 || capacity < 0 || (capacity && !out)) {
    failure("GetSystemPropertyFloatArray", invalid);
    return 0;
  }
  auto values = rates();
  auto n = std::min(std::size_t(capacity), values.size());
  std::copy_n(values.begin(), n, out);
  return int(n);
}
std::int32_t vrapi_GetSystemStatusInt(const Java *, std::int32_t property) {
  std::lock_guard lock(state().mutex);
  std::lock_guard sl(session().mutex);
  if (state().mobile)
    pump_events();
  switch (property) {
  case 0:
    // Mobile SDK 1.30 DOCKED means attached, independently of MOUNTED.
    // A standalone headset stays attached when the wearer removes it.
    return host_xr().available && session().state != XR_SESSION_STATE_LOSS_PENDING &&
           session().state != XR_SESSION_STATE_EXITING;
  case 1:
    return session().state == XR_SESSION_STATE_VISIBLE ||
           session().state == XR_SESSION_STATE_FOCUSED;
  case 13:
    return int(state().recenter_count);
  case 15:
    return 0; // USER_RECENTER_COUNT; application recenter is separate.
  case 130:
    return 1; // color-managed sRGB compositor baseline
  case 131:
    return 0; // no capture in this process
  default:
    // OVRPlugin polls status queries every frame. An unavailable status is a
    // persistent host capability, not a new failure on each poll. The VrApi
    // mutex also protects this per-process first-occurrence table.
    static std::set<std::int32_t> reported;
    if (reported.insert(property).second)
      failure("GetSystemStatusInt unknown property", property);
    return 0;
  }
}
float vrapi_GetSystemStatusFloat(const Java *, std::int32_t property) {
  std::lock_guard lock(state().mutex);
  if (property == 8) {
    // Actual application submissions over a trailing one-second window, not
    // the HMD refresh rate (VrApi_Types.h APP_FRAMES_PER_SECOND).
    auto now = monotonic_ns();
    return float(std::count_if(state().submissions.begin(), state().submissions.end(),
                               [&](auto t) { return t >= now - 1'000'000'000; }));
  }
  // No invented thermal/latency/FPS samples. The public getter has no error code.
  // Report each unavailable counter once per launch, before formatting or
  // entering the shared logger, rather than on every OVRPlugin frame poll.
  static std::set<std::int32_t> reported;
  if (reported.insert(property).second)
    failure("GetSystemStatusFloat unavailable counter", property);
  return 0;
}
bool vrapi_GetPropertyInt(const Java *, std::int32_t property, std::int32_t *out) {
  std::lock_guard lock(state().mutex);
  if (!out)
    return false;
  if (property == 24) {
    *out = -1;
    if (state().mobile) {
      InputHeader h{};
      if (vrapi_EnumerateInputDevices(state().mobile, 0, &h) == 0)
        *out = int(h.id);
    }
    return true;
  }
  if (property == 29) {
    *out = 0;
    return true;
  }
  if (property == 15 || property == 20 || property == 30) {
    *out = state().properties[property];
    return true;
  }
  failure("GetPropertyInt unknown property", property);
  return false;
}
void vrapi_SetPropertyInt(const Java *, std::int32_t property, std::int32_t value) {
  std::lock_guard lock(state().mutex);
  if ((property == 15 && value >= 0 && value <= 3) ||
      ((property == 20 || property == 30) && (value == 0 || value == 1))) {
    state().properties[property] = value;
    return;
  }
  failure("SetPropertyInt unsupported property/value", property);
}
void vrapi_SetPropertyFloat(const Java *, std::int32_t property, float) {
  // VrApi.h declares a void setter. No supported float property has a host
  // implementation yet; report the rejection instead of caching a fake value.
  failure("SetPropertyFloat unsupported property", property);
}
std::int32_t vrapi_SetClockLevels(Mobile *mobile, std::int32_t cpu, std::int32_t gpu) {
  std::lock_guard lock(state().mutex);
  if (!valid(mobile))
    return invalid;
  state().cpu_level = std::clamp(cpu, 0, 3);
  state().gpu_level = std::clamp(gpu, 0, 3);
  return success;
}
std::int32_t vrapi_SetPerfThread(Mobile *mobile, std::int32_t type, std::uint32_t tid) {
  std::lock_guard lock(state().mutex);
  if (!valid(mobile) || type < 0 || type > 1 || !tid)
    return invalid;
  // Main (0) and renderer (1) threads: same host priority as the OpenXR
  // android thread hint. Advisory; an unknown thread is not an error.
  if (!platform::thread_hints_enabled()) return success;
  // UE4 repeats the call every frame; hint each thread once.
  if (std::exchange(state().perf_threads[type], tid) == tid) return success;
  const int result = platform::set_thread_priority(static_cast<std::int32_t>(tid), -10);
  log::info("vrapi", "perf thread type={} tid={} priority result={}", type, tid, result);
  return success;
}
std::int32_t vrapi_SetExtraLatencyMode(Mobile *mobile, std::int32_t mode) {
  std::lock_guard lock(state().mutex);
  if (!valid(mobile) || mode < 0 || mode > 2)
    return invalid;
  if (mode != 0)
    return unsupported;
  state().latency_mode = mode;
  return success;
}
std::int32_t vrapi_SetDisplayRefreshRate(Mobile *mobile, float rate) {
  std::lock_guard lock(state().mutex);
  if (!valid(mobile) || !std::isfinite(rate) || rate < 0)
    return invalid;
  if (rate == 0)
    return success;
  auto available = rates();
  for (auto supported : available)
    if (std::abs(supported - rate) < 0.01f)
      return success;
  return unsupported;
}
ColorDesc vrapi_GetHmdColorDesc(Mobile *mobile) {
  std::lock_guard lock(state().mutex);
  if (!valid(mobile)) {
    failure("GetHmdColorDesc", invalid);
    return {};
  }
  return {2}; // VrApi ovrColorSpace_Rec_709 is 2; inspect SDK enum on updates.
}
std::int32_t vrapi_SetClientColorDesc(Mobile *mobile, const ColorDesc *color) {
  std::lock_guard lock(state().mutex);
  if (!valid(mobile) || !color)
    return invalid;
  if (color->color_space != 0 && color->color_space != 2)
    return unsupported;
  return success;
}
std::int32_t vrapi_GetInstanceExtensionsVulkan(char *names, std::uint32_t *length) {
  std::lock_guard lock(state().mutex);
  if (!length)
    return invalid;
  return result(xr::guest_vulkan_extensions(host_xr().instance, host_xr().system, false,
                    names ? *length : 0, length, names, vulkan::extension_translatable),
                "GetInstanceExtensionsVulkan");
}
std::int32_t vrapi_GetDeviceExtensionsVulkan(char *names, std::uint32_t *length) {
  std::lock_guard lock(state().mutex);
  if (!length)
    return invalid;
  return result(xr::guest_vulkan_extensions(host_xr().instance, host_xr().system, true,
                    names ? *length : 0, length, names, vulkan::extension_translatable),
                "GetDeviceExtensionsVulkan");
}
std::int32_t vrapi_GetBoundaryGeometry(Mobile *mobile, std::uint32_t capacity, std::uint32_t *count,
                                       Vec3 *out) {
  std::lock_guard lock(state().mutex);
  if (!valid(mobile) || !count)
    return invalid;
  XrExtent2Df e{};
  XrPosef stage{};
  auto r = bounds(e, stage);
  if (r != XR_SUCCESS) {
    *count = 0;
    return 1001;
  }
  *count = out && capacity ? std::min(capacity, 4u) : 4;
  if (!out || !capacity)
    return success;
  XrVector3f p[]{{-e.width / 2, 0, -e.height / 2},
                 {e.width / 2, 0, -e.height / 2},
                 {e.width / 2, 0, e.height / 2},
                 {-e.width / 2, 0, e.height / 2}};
  for (std::uint32_t i = 0; i < *count; ++i) {
    auto q = compose(stage, {{0, 0, 0, 1}, p[i]}).position;
    out[i] = {q.x, q.y, q.z};
  }
  return success;
}
std::int32_t vrapi_GetBoundaryOrientedBoundingBox(Mobile *mobile, Pose *out, Vec3 *scale) {
  std::lock_guard lock(state().mutex);
  if (!valid(mobile) || !out || !scale)
    return invalid;
  XrExtent2Df e{};
  XrPosef stage{};
  if (bounds(e, stage) != XR_SUCCESS) {
    *out = {};
    *scale = {};
    return 1001;
  }
  *out = pose(stage);
  *scale = {e.width / 2, 0, e.height / 2};
  return success;
}
std::int32_t test_point(Mobile *mobile, Vec3 point, bool *inside, Boundary *out) {
  std::lock_guard lock(state().mutex);
  if (!valid(mobile) || !inside)
    return invalid;
  XrExtent2Df e{};
  XrPosef stage{};
  if (bounds(e, stage) != XR_SUCCESS) {
    *inside = false;
    if (out)
      *out = {};
    return 1001;
  }
  auto p = compose(inverse(stage), {{0, 0, 0, 1}, {point.x, point.y, point.z}}).position;
  float x = std::clamp(p.x, -e.width / 2, e.width / 2),
        z = std::clamp(p.z, -e.height / 2, e.height / 2);
  *inside = std::abs(p.x) <= e.width / 2 && std::abs(p.z) <= e.height / 2;
  if (*inside) {
    if (e.width / 2 - std::abs(p.x) < e.height / 2 - std::abs(p.z))
      x = std::copysign(e.width / 2, p.x);
    else
      z = std::copysign(e.height / 2, p.z);
  }
  if (out) {
    auto q = compose(stage, {{0, 0, 0, 1}, {x, p.y, z}}).position;
    float dx = point.x - q.x, dz = point.z - q.z, dist = std::hypot(dx, dz);
    *out = {{q.x, q.y, q.z},
            {dist ? dx / dist : 0, 0, dist ? dz / dist : 0},
            dist,
            !*inside || dist < 0.1f};
  }
  return success;
}
std::int32_t vrapi_GetBoundaryTriggerState(Mobile *mobile, std::int32_t device, Boundary *out) {
  std::lock_guard lock(state().mutex);
  if (!valid(mobile) || !out || device < 0 || device > 2)
    return invalid;
  auto tracked = tracking_at(time_from_seconds(0),
                             device == 0 ? state().view : state().input.grip[device - 1]);
  if (!(tracked.status & 8))
    return unavailable;
  bool inside = false;
  return test_point(mobile, tracked.head.pose.position, &inside, out);
}
} // namespace venture::hle::vrapi::guest
