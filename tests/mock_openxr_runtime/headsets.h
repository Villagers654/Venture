#pragma once
// Headset capability profiles for the mock runtime (VENTURE_MOCK_HEADSET), from
// each runtime's documented OpenXR version and extensions
// (docs/design/native-headsets.md, sections 2 and 8). Where a list is not
// documented the profile leaves the extension out, which exercises the most
// of the Quest layer. A profile adds to the mock's base extension list; it
// changes nothing else, and without one the mock behaves as before.
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace mock {
struct HeadsetProfile {
  std::string_view name;
  std::uint32_t api_minor;  // newest OpenXR 1.x the runtime accepts
  std::span<const std::string_view> extensions;
  // XR_FB_display_refresh_rate when not empty: the listed rates.
  std::span<const float> refresh_rates;
  // Lists rates but refuses every request for a fixed one.
  bool refuses_refresh_requests = false;
};

namespace headset_lists {
inline constexpr std::array<std::string_view, 4> quest_extensions{
    "XR_FB_display_refresh_rate", "XR_FB_color_space", "XR_KHR_locate_spaces", "XR_KHR_android_thread_settings"};
inline constexpr std::array<float, 4> quest_rates{72.0f, 80.0f, 90.0f, 120.0f};
inline constexpr std::array<std::string_view, 1> refresh_only{"XR_FB_display_refresh_rate"};
inline constexpr std::array<float, 2> frame_rates{90.0f, 120.0f};
}  // namespace headset_lists

inline constexpr std::array<HeadsetProfile, 5> kHeadsetProfiles{{
    // Meta's runtime: everything the layer could answer is native, so the
    // layer must stay out of the way.
    {"quest", 1, headset_lists::quest_extensions, headset_lists::quest_rates},
    // Pico OS 5.12 (Pico 4 Ultra): OpenXR 1.1 conformant. No documented
    // Quest extensions beyond the controller profile in the base list.
    {"pico", 1, {}, {}},
    // Android XR (Galaxy XR): OpenXR 1.1 plus XR_ANDROID_* extensions.
    {"android-xr", 1, {}, {}},
    // SteamVR's runtime on the Steam Frame: OpenXR 1.0 only; it lists
    // refresh rates and refuses a fixed one.
    {"frame", 0, headset_lists::refresh_only, headset_lists::frame_rates, true},
    // A 1.0 runtime with no documented Quest extensions (Vive Wave, Lynx,
    // Play For Dream, Pimax standalone where their lists are unknown).
    {"openxr-1.0", 0, {}, {}},
}};

inline const HeadsetProfile* headset_profile(std::string_view name) {
  for (const auto& profile : kHeadsetProfiles)
    if (profile.name == name) return &profile;
  return nullptr;
}
}  // namespace mock
