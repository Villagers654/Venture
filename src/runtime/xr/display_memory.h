// The host's display period, remembered across launches per host runtime and
// headset. Hosts without XR_FB_display_refresh_rate (SteamVR, for one) report
// their rate only through the first frame's predicted period; until then the
// guest sees the remembered rate instead of Android's 90 Hz default.
#pragma once
#include <cstdint>
#include <string_view>

namespace venture::xr {
// Seeds display_timing_state() from the remembered period of this host.
void recall_display_period(std::string_view runtime, std::string_view system);
// Records a period the host reported; a no-op until recall_display_period ran.
void remember_display_period(std::int64_t period);
} // namespace venture::xr
