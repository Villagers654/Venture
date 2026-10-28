#pragma once
#include "xr/math.h"

namespace mock {
inline XrSpaceVelocityData pose_velocity(XrPosef before, XrPosef after, double seconds) {
  if (!(seconds > 0)) return {};
  const auto rate = float(1 / seconds);
  auto q = venture::xr::multiply(after.orientation, venture::xr::inverse(before.orientation));
  // q and -q describe the same orientation; take the shorter rotation.
  if (q.w < 0) q = {-q.x, -q.y, -q.z, -q.w};
  const auto axis_length = venture::xr::length({q.x, q.y, q.z});
  const auto angular_rate = axis_length > 1e-7f
      ? 2 * std::atan2(axis_length, q.w) * rate / axis_length : 2 * rate;
  return {XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT,
          {(after.position.x - before.position.x) * rate,
           (after.position.y - before.position.y) * rate,
           (after.position.z - before.position.z) * rate},
          {q.x * angular_rate, q.y * angular_rate, q.z * angular_rate}};
}
inline XrSpaceVelocityData relative_velocity(XrPosef space, XrSpaceVelocityData velocity,
                                            XrPosef base, XrSpaceVelocityData base_velocity) {
  const XrVector3f offset{space.position.x - base.position.x,
                         space.position.y - base.position.y,
                         space.position.z - base.position.z};
  const auto w = base_velocity.angularVelocity;
  const XrVector3f rotational{w.y * offset.z - w.z * offset.y,
                             w.z * offset.x - w.x * offset.z,
                             w.x * offset.y - w.y * offset.x};
  const auto inverse = venture::xr::inverse(base.orientation);
  auto flags = velocity.velocityFlags & base_velocity.velocityFlags;
  if (!(base_velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT))
    flags &= ~XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
  return {flags,
          venture::xr::rotate(inverse, {velocity.linearVelocity.x - base_velocity.linearVelocity.x - rotational.x,
                                  velocity.linearVelocity.y - base_velocity.linearVelocity.y - rotational.y,
                                  velocity.linearVelocity.z - base_velocity.linearVelocity.z - rotational.z}),
          venture::xr::rotate(inverse, {velocity.angularVelocity.x - w.x,
                                  velocity.angularVelocity.y - w.y,
                                  velocity.angularVelocity.z - w.z})};
}
}
