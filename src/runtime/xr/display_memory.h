// The host's display period, remembered across launches per host runtime and
// headset. Hosts without XR_FB_display_refresh_rate (SteamVR, for one) report
// their rate only through the first frame's predicted period; until then the
// guest sees the remembered rate instead of Android's 90 Hz default.
#pragma once
#include <openxr/openxr.h>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace venture::xr {
// Seeds display_timing_state() from the remembered period of this host.
void recall_display_period(std::string_view runtime, std::string_view system);
// Records a period the host reported; a no-op until recall_display_period ran.
void remember_display_period(std::int64_t period);
// The host's eye fields of view, remembered the same way (recall_display_period
// also recalls them). VrApi games read SUGGESTED_EYE_FOV before EnterVrMode, when
// no session can locate the views yet.
std::optional<std::array<XrFovf, 2>> remembered_eye_fov();
// Records the angles a session's views reported; a no-op until recall_display_period ran.
void remember_eye_fov(const std::array<XrFovf, 2> &fov);
} // namespace venture::xr
