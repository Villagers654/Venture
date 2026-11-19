#include "xr/sanitize.h"

#include <atomic>
#include <cmath>
#include <mutex>

#include "common/log.h"

namespace venture::xr {
// A Quest never hands an app NaN. A PC runtime may leave values the flags mark invalid undefined
// (velocities it does not track, for one), and games use them anyway: make them zero/identity.
namespace {
bool finite(const XrVector3f& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
// Logged once: what the host runtime left undefined.
void note_sanitized(const char* what) {
  static std::atomic<bool> noted{false};
  if (!noted.exchange(true)) log::warn("xr", "Host runtime returned an undefined {}; the game gets a neutral value", what);
}
std::mutex located_mutex;
std::optional<std::array<XrFovf, 2>> located;
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
  if (count == 2) {
    std::lock_guard lock(located_mutex);
    located = std::array<XrFovf, 2>{views[0].fov, views[1].fov};
  }
}
std::optional<std::array<XrFovf, 2>> located_stereo_fov() {
  std::lock_guard lock(located_mutex);
  return located;
}
void forget_located_stereo_fov() {
  std::lock_guard lock(located_mutex);
  located.reset();
}
}  // namespace venture::xr
