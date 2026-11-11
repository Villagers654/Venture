// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace venture::hle::vrapi {
// VrApi_Input.h ovrButton_Up/Down/Left/Right. The Quest runtime sets them from
// the thumbstick on both Touch controllers, and some OVRPlugin titles turn or
// navigate from these bits rather than from the analog joystick.
inline constexpr std::uint32_t stick_up = 0x00010000u, stick_down = 0x00020000u,
                               stick_left = 0x00040000u, stick_right = 0x00080000u;
inline constexpr std::uint32_t stick_direction_mask = stick_up | stick_down | stick_left | stick_right;
// OVRInput's axis-as-button threshold.
inline constexpr float stick_direction_threshold = 0.5f;
inline std::uint32_t stick_direction_buttons(float x, float y) {
  return (y > stick_direction_threshold ? stick_up : 0u) |
         (y < -stick_direction_threshold ? stick_down : 0u) |
         (x < -stick_direction_threshold ? stick_left : 0u) |
         (x > stick_direction_threshold ? stick_right : 0u);
}
}  // namespace venture::hle::vrapi
