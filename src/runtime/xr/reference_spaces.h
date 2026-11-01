#pragma once
#include "xr/api.h"
namespace venture::xr {
// The host reference space to create for a LOCAL or LOCAL_FLOOR request. Both
// are gravity-aligned in OpenXR, as on every Quest; SteamVR without a room
// setup returns them upside down against STAGE (the head's up axis is -Y,
// Batman: Arkham Shadow's camera turned NaN). Such a host's LOCAL becomes STAGE
// at the head (yaw only) as measured on first use, LOCAL_FLOOR the floor below
// it. Other types and upright hosts are returned unchanged.
XrReferenceSpaceCreateInfo upright_reference_space(XrSession, XrReferenceSpaceCreateInfo);
void forget_reference_spaces(XrSession);
// The yaw-only, upright part of an orientation (its forward axis kept).
XrQuaternionf yaw_only(XrQuaternionf);
} // namespace venture::xr
