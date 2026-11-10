// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string_view>

namespace venture::hle::vrapi {
// Breathedge's original OVRPlugin 1.59 path needs discrete horizontal
// directions for turning, in addition to the analog joystick coordinates.
// The original APK's left controller and unrelated games retain their inputs.
inline std::uint32_t breathedge_horizontal_button_mask(std::string_view package, std::uint32_t device) {
  return package == "com.HypetrainDigitalLtd.Breathedge" && device == 2 ? 0x000c0000u : 0u;
}
inline std::uint32_t breathedge_horizontal_buttons(std::string_view package, std::uint32_t device, float x) {
  if (!breathedge_horizontal_button_mask(package, device)) return 0;
  return x < -0.5f ? 0x00040000u : x > 0.5f ? 0x00080000u : 0u;
}
}
