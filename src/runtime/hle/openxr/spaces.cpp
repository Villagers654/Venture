#include "hle/openxr/state.h"
#include "core/av_telemetry.h"
#include "common/log.h"
#include "xr/host_xr.h"
#include <atomic>
#include <chrono>
#include <cmath>

namespace venture::hle::openxr {
namespace {
// Action spaces of a hand, so harness runs can see the controller poses the
// guest itself locates (a game need not draw its controllers in a probe scene).
struct HandSpace {
  std::string hand;
  std::chrono::steady_clock::time_point reported{};
};
std::mutex hand_mutex;
std::map<XrSpace, HandSpace> hand_spaces;
void note_hand_space(XrSpace space, const XrActionSpaceCreateInfo& info) {
  if (!av::enabled()) return;
  char text[XR_MAX_PATH_LENGTH]{};
  std::uint32_t count = 0;
  std::string hand = "action";
  if (info.subactionPath &&
      XR_SUCCEEDED(::xrPathToString(host_xr().instance, info.subactionPath, sizeof(text), &count, text)))
    hand = text;
  std::lock_guard lock(hand_mutex);
  hand_spaces[space] = {std::move(hand)};
}
void report_hand_pose(XrSpace space, const XrSpaceLocation& location) {
  if (!av::enabled()) return;
  const auto now = std::chrono::steady_clock::now();
  std::string hand;
  {
    std::lock_guard lock(hand_mutex);
    auto it = hand_spaces.find(space);
    if (it == hand_spaces.end() || now - it->second.reported < std::chrono::milliseconds(250)) return;
    it->second.reported = now;
    hand = it->second.hand;
  }
  const auto& p = location.pose.position;
  const auto& q = location.pose.orientation;
  av::emit({{"kind", "controller_pose"}, {"api", "openxr"}, {"hand", hand},
            {"space", reinterpret_cast<std::uintptr_t>(space)}, {"flags", location.locationFlags},
            {"orientation_valid", bool(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)},
            {"position", {p.x, p.y, p.z}}, {"orientation", {q.x, q.y, q.z, q.w}}});
}
}  // namespace
}  // namespace venture::hle::openxr

namespace venture::hle::openxr {
// A Quest never hands an app NaN. A PC runtime may leave values the flags mark invalid undefined
// (velocities it does not track, for one), and games use them anyway: make them zero/identity.
namespace {
bool finite(const XrVector3f& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
// Logged once: what the host runtime left undefined.
void note_sanitized(const char* what) {
  static std::atomic<bool> noted{false};
  if (!noted.exchange(true)) log::warn("xr", "Host runtime returned an undefined {}; the game gets a neutral value", what);
}
}  // namespace
void sanitize(XrSpaceLocationFlags& flags, XrPosef& pose) {
  auto& q = pose.orientation;
  const auto norm = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
  if (!std::isfinite(norm) || norm < 1e-6f) {
    if (flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) note_sanitized("orientation");
    q = {0, 0, 0, 1};
    flags &= ~XrSpaceLocationFlags(XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT);
  }
  if (!finite(pose.position)) {
    if (flags & XR_SPACE_LOCATION_POSITION_VALID_BIT) note_sanitized("position");
    pose.position = {};
    flags &= ~XrSpaceLocationFlags(XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT);
  }
}
void sanitize(XrSpaceVelocityFlags& flags, XrVector3f& linear, XrVector3f& angular) {
  if (!(flags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) || !finite(linear)) {
    linear = {};
    flags &= ~XrSpaceVelocityFlags(XR_SPACE_VELOCITY_LINEAR_VALID_BIT);
  }
  if (!(flags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) || !finite(angular)) {
    angular = {};
    flags &= ~XrSpaceVelocityFlags(XR_SPACE_VELOCITY_ANGULAR_VALID_BIT);
  }
}
void sanitize_views(XrViewState& state, std::uint32_t count, XrView* views) {
  for (std::uint32_t i = 0; i < count; ++i) {
    auto flags = XrSpaceLocationFlags(state.viewStateFlags);
    sanitize(flags, views[i].pose);
    state.viewStateFlags = XrViewStateFlags(flags);
    auto& f = views[i].fov;
    if (!std::isfinite(f.angleLeft) || !std::isfinite(f.angleRight) || !std::isfinite(f.angleUp) ||
        !std::isfinite(f.angleDown) || f.angleLeft >= f.angleRight || f.angleDown >= f.angleUp) {
      note_sanitized("view field of view");
      f = {-0.785398f, 0.785398f, 0.785398f, -0.785398f};
    }
  }
}
}  // namespace venture::hle::openxr

namespace venture::hle::openxr::guest {
XrResult xrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo* createInfo, XrSpace* space) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(session, "XrSession")) return XR_ERROR_HANDLE_INVALID;
  if (!createInfo || XR_FAILED(validate_struct(createInfo, XR_TYPE_ACTION_SPACE_CREATE_INFO))) return XR_ERROR_VALIDATION_FAILURE;
  if (!space) return XR_ERROR_VALIDATION_FAILURE;
  auto result = host_call<PFN_xrCreateActionSpace, "xrCreateActionSpace">(session, createInfo, space);
  if (XR_SUCCEEDED(result)) {
    track_handle(*space, "XrSpace", session);
    note_hand_space(*space, *createInfo);
  }
  return result;
}
XrResult xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation* location) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(space, "XrSpace")) return XR_ERROR_HANDLE_INVALID;
  if (!valid_handle(baseSpace, "XrSpace")) return XR_ERROR_HANDLE_INVALID;
  if (!location || XR_FAILED(validate_struct(location, XR_TYPE_SPACE_LOCATION))) return XR_ERROR_VALIDATION_FAILURE;
  auto result = host_call<PFN_xrLocateSpace, "xrLocateSpace">(space, baseSpace, time, location);
  if (XR_SUCCEEDED(result)) {
    sanitize(location->locationFlags, location->pose);
    for (auto* next = static_cast<XrBaseOutStructure*>(location->next); next; next = next->next)
      if (next->type == XR_TYPE_SPACE_VELOCITY) {
        auto* velocity = reinterpret_cast<XrSpaceVelocity*>(next);
        sanitize(velocity->velocityFlags, velocity->linearVelocity, velocity->angularVelocity);
      }
    report_hand_pose(space, *location);
  }
  return result;
}
XrResult xrLocateSpaces(XrSession handle, const XrSpacesLocateInfo *info, XrSpaceLocations *out) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(handle, "XrSession"))
    return XR_ERROR_HANDLE_INVALID;
  if (!info || info->type != XR_TYPE_SPACES_LOCATE_INFO || info->next || !info->spaceCount ||
      !info->spaces || !out || out->type != XR_TYPE_SPACE_LOCATIONS ||
      out->locationCount != info->spaceCount || !out->locations)
    return XR_ERROR_VALIDATION_FAILURE;
  if (!valid_handle(info->baseSpace, "XrSpace"))
    return XR_ERROR_HANDLE_INVALID;
  XrSpaceVelocities *velocities = static_cast<XrSpaceVelocities *>(out->next);
  if (velocities && (velocities->type != XR_TYPE_SPACE_VELOCITIES || velocities->next ||
                     velocities->velocityCount != info->spaceCount || !velocities->velocities))
    return XR_ERROR_VALIDATION_FAILURE;
  // 1.0 host fallback uses exactly the same time/base for every location.
  // Output is committed only after all handles and runtime calls succeed.
  std::vector<XrSpaceLocation> locations(info->spaceCount, {XR_TYPE_SPACE_LOCATION});
  std::vector<XrSpaceVelocity> speed(info->spaceCount, {XR_TYPE_SPACE_VELOCITY});
  for (std::uint32_t i = 0; i < info->spaceCount; ++i) {
    if (!valid_handle(info->spaces[i], "XrSpace"))
      return XR_ERROR_HANDLE_INVALID;
    if (velocities)
      locations[i].next = &speed[i];
    auto r = ::xrLocateSpace(info->spaces[i], info->baseSpace, info->time, &locations[i]);
    if (XR_FAILED(r))
      return r;
  }
  for (std::uint32_t i = 0; i < info->spaceCount; ++i) {
    sanitize(locations[i].locationFlags, locations[i].pose);
    out->locations[i] = {locations[i].locationFlags, locations[i].pose};
    report_hand_pose(info->spaces[i], locations[i]);
    if (velocities) {
      sanitize(speed[i].velocityFlags, speed[i].linearVelocity, speed[i].angularVelocity);
      velocities->velocities[i] = {speed[i].velocityFlags, speed[i].linearVelocity,
                                   speed[i].angularVelocity};
    }
  }
  return XR_SUCCESS;
}
XrResult xrLocateSpacesKHR(XrSession handle, const XrSpacesLocateInfo *info,
                           XrSpaceLocations *out) {
  return guest::xrLocateSpaces(handle, info, out);
}
} // namespace venture::hle::openxr::guest
