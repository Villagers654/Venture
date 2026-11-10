#include "xr/eye_gaze.h"
#include "common/environment.h"
#include "common/log.h"
#include "xr/host_xr.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#ifdef _WIN32
#include <windows.h>
#endif
#include <cstring>
#include <mutex>
namespace venture::xr::eye_gaze {
namespace {
constexpr const char* profile_name = "/interaction_profiles/ext/eye_gaze_interaction";
constexpr const char* binding_name = "/user/eyes_ext/input/gaze_ext/pose";
std::mutex mutex;
bool checked = false, supported = false;
bool game_suggested = false;  // the game's own bindings on the eye gaze profile carry ours
XrActionSet set{};
XrAction gaze{};
XrSession session{};
XrSpace gaze_space{}, view_space{};

// With `mutex` held.
bool supported_locked(XrInstance instance) {
  if (checked) return supported;
  checked = true;
  const auto& host = host_xr();
  if (!requested() || !host.enabled_extensions.contains(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME)) return false;
  XrSystemEyeGazeInteractionPropertiesEXT eyes{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
  XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES, &eyes};
  if (XR_FAILED(::xrGetSystemProperties(instance, host.system, &properties)) || !eyes.supportsEyeGazeInteraction) {
    log::info("xr", "Eye gaze: the runtime reports no eye tracking; Virtual Desktop's tracking data is used when it streams");
    return false;
  }
  XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strcpy(si.actionSetName, "venture_eye_gaze");
  std::strcpy(si.localizedActionSetName, "Venture eye gaze");
  if (XR_FAILED(::xrCreateActionSet(instance, &si, &set))) return false;
  XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
  std::strcpy(ai.actionName, "gaze");
  std::strcpy(ai.localizedActionName, "Eye gaze");
  ai.actionType = XR_ACTION_TYPE_POSE_INPUT;
  if (XR_FAILED(::xrCreateAction(set, &ai, &gaze))) {
    ::xrDestroyActionSet(set);
    set = {};
    return false;
  }
  supported = true;
  log::info("xr", "Eye gaze: available ({}); foveated VRS can follow it", host.runtime_name);
  return true;
}
}  // namespace

bool requested() {
  static const bool on = [] {
    const char* text = venture::environment::getenv("VENTURE_VRS_GAZE");
    return text && *text == '1';
  }();
  return on;
}

XrActionSet action_set(XrInstance instance) {
  std::lock_guard lock(mutex);
  return supported_locked(instance) ? set : XrActionSet{};
}

void add_binding(XrInstance instance, std::string_view profile, std::vector<XrActionSuggestedBinding>& bindings) {
  if (profile != profile_name) return;
  std::lock_guard lock(mutex);
  if (!supported_locked(instance)) return;
  XrPath path{};
  if (XR_FAILED(::xrStringToPath(instance, binding_name, &path))) return;
  bindings.push_back({gaze, path});
  game_suggested = true;
}

void before_attach(XrInstance instance) {
  std::lock_guard lock(mutex);
  if (!supported_locked(instance) || game_suggested) return;
  XrPath profile{}, path{};
  if (XR_FAILED(::xrStringToPath(instance, profile_name, &profile)) ||
      XR_FAILED(::xrStringToPath(instance, binding_name, &path)))
    return;
  const XrActionSuggestedBinding binding{gaze, path};
  XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
  info.interactionProfile = profile;
  info.countSuggestedBindings = 1;
  info.suggestedBindings = &binding;
  const auto result = ::xrSuggestInteractionProfileBindings(instance, &info);
  if (XR_FAILED(result)) log::warn("xr", "Eye gaze: binding refused ({})", int(result));
}

void attached(XrSession attached_session) {
  std::lock_guard lock(mutex);
  if (!supported) return;
  if (gaze_space) ::xrDestroySpace(gaze_space);
  if (view_space) ::xrDestroySpace(view_space);
  gaze_space = view_space = {};
  session = attached_session;
  XrActionSpaceCreateInfo ai{XR_TYPE_ACTION_SPACE_CREATE_INFO};
  ai.action = gaze;
  ai.poseInActionSpace.orientation.w = 1.f;
  XrReferenceSpaceCreateInfo vi{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  vi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  vi.poseInReferenceSpace.orientation.w = 1.f;
  if (XR_FAILED(::xrCreateActionSpace(session, &ai, &gaze_space)) ||
      XR_FAILED(::xrCreateReferenceSpace(session, &vi, &view_space))) {
    log::warn("xr", "Eye gaze: its spaces could not be created");
    if (gaze_space) ::xrDestroySpace(gaze_space);
    gaze_space = view_space = {};
  }
}

void forget_session(XrSession forgotten) {
  std::lock_guard lock(mutex);
  if (forgotten != session) return;
  // The runtime destroys a session's spaces with it.
  gaze_space = view_space = {};
  session = {};
}

namespace {
#ifdef _WIN32
// Virtual Desktop's tracking data (VirtualDesktop.BodyState, as VDXR reads it): each eye's pose relative to the
// head, published while Virtual Desktop streams, whatever the OpenXR runtime (SteamVR through Virtual Desktop).
// The layout up to the eyes of VDXR's BodyTracking::BodyStateV2.
struct VdPose {
  float qx, qy, qz, qw;
  float px, py, pz;
};
struct VdBodyStateEyes {
  std::uint8_t face_valid, eye_following_valid;
  float expressions[70];
  float confidences[2];
  std::uint8_t left_valid, right_valid;
  VdPose left, right;
  float left_confidence, right_confidence;
};
std::optional<XrVector3f> virtual_desktop_direction() {
  static HANDLE mapping{};
  static const VdBodyStateEyes* view{};
  static std::chrono::steady_clock::time_point next_open{};
  if (!view) {
    const auto now = std::chrono::steady_clock::now();
    if (now < next_open) return std::nullopt;
    next_open = now + std::chrono::seconds(2);
    mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"VirtualDesktop.BodyState");
    if (!mapping) return std::nullopt;
    view = static_cast<const VdBodyStateEyes*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(VdBodyStateEyes)));
    if (!view) {
      CloseHandle(mapping);
      mapping = {};
      return std::nullopt;
    }
    log::info("xr", "Eye gaze: from Virtual Desktop's tracking data");
  }
  VdBodyStateEyes eyes{};
  std::memcpy(&eyes, view, sizeof(eyes));
  if (!eyes.left_valid || !eyes.right_valid || eyes.left_confidence <= 0.5f || eyes.right_confidence <= 0.5f)
    return std::nullopt;
  // Both eyes' forward (-z), averaged: the head's gaze.
  XrVector3f sum{};
  for (const auto& q : {eyes.left, eyes.right}) {
    sum.x += -2.f * (q.qx * q.qz + q.qw * q.qy);
    sum.y += -2.f * (q.qy * q.qz - q.qw * q.qx);
    sum.z += -(1.f - 2.f * (q.qx * q.qx + q.qy * q.qy));
  }
  if (!std::isfinite(sum.x) || !std::isfinite(sum.y) || !std::isfinite(sum.z) || sum.z > -0.2f) return std::nullopt;
  return XrVector3f{sum.x * 0.5f, sum.y * 0.5f, sum.z * 0.5f};
}
#else
std::optional<XrVector3f> virtual_desktop_direction() { return std::nullopt; }
#endif
}  // namespace

std::optional<XrVector3f> direction(XrTime time) {
  if (!requested()) return std::nullopt;
  std::lock_guard lock(mutex);
  // The runtime's eye gaze, else Virtual Desktop's (SteamVR through Virtual Desktop reports none).
  if (!gaze_space || !view_space || !time) return virtual_desktop_direction();
  XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
  if (XR_FAILED(::xrLocateSpace(gaze_space, view_space, time, &location))) return std::nullopt;
  constexpr XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                                          XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
  if ((location.locationFlags & needed) != needed) return std::nullopt;
  // The gaze looks along the pose's -z.
  const auto& q = location.pose.orientation;
  const XrVector3f d{-2.f * (q.x * q.z + q.w * q.y), -2.f * (q.y * q.z - q.w * q.x),
                     -(1.f - 2.f * (q.x * q.x + q.y * q.y))};
  if (!std::isfinite(d.x) || !std::isfinite(d.y) || !std::isfinite(d.z) || d.z > -0.1f) return std::nullopt;
  return d;
}
}  // namespace venture::xr::eye_gaze
