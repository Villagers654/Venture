#pragma once
#include <array>
#include <string>

namespace venture::xr {
// The render model the host runtime gives the controller in each hand (left,
// right; "" where it gives none), read from OpenVR's IVRSystem
// (Prop_RenderModelName_String of the device in the hand's controller role)
// through the OpenVR client the host OpenXR runtime already loaded into this
// process. SteamVR's OpenXR runtime is that client; no other runtime loads it,
// and both stay "" there. VR_Init never runs here: it must not in an OpenXR process.
std::array<std::string, 2> openvr_controller_render_models();
}  // namespace venture::xr
