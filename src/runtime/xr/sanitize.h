// Undefined or non-finite runtime poses, velocities and views become neutral
// values: a Quest never hands an app NaN. Shared by the guest OpenXR and VrApi.
#pragma once
#include <openxr/openxr.h>

#include <array>
#include <cstdint>
#include <optional>

namespace venture::xr {
void sanitize(XrSpaceLocationFlags& flags, XrPosef& pose);
void sanitize(XrSpaceVelocityFlags& flags, XrVector3f& linear, XrVector3f& angular);
void sanitize_views(XrViewState& state, std::uint32_t count, XrView* views);
// The field of view of each of the two views the host last located for the game
// (sanitize_views records a stereo locate), or nothing before the first one.
std::optional<std::array<XrFovf, 2>> located_stereo_fov();
// A new or destroyed session starts without one.
void forget_located_stereo_fov();
}  // namespace venture::xr
