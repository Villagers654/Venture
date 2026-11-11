// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace venture::hle::vrapi {
// VrApi_Input.h ovrButton_Left/Right, advertised and set from the thumbstick of
// both Touch controllers: some OVRPlugin titles turn from these bits rather than
// from the analog joystick (Breathedge's original OVRPlugin 1.59 build, tested
// in the headset). Up/Down are not reported: with them also advertised and set,
// Breathedge no longer turns with the right stick (tested in the headset).
inline constexpr std::uint32_t stick_left = 0x00040000u, stick_right = 0x00080000u;
inline constexpr std::uint32_t stick_direction_mask = stick_left | stick_right;
// OVRInput's axis-as-button threshold.
inline constexpr float stick_direction_threshold = 0.5f;
inline std::uint32_t stick_direction_buttons(float x) {
  return (x < -stick_direction_threshold ? stick_left : 0u) | (x > stick_direction_threshold ? stick_right : 0u);
}
}  // namespace venture::hle::vrapi
