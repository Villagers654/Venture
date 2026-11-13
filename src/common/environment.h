#pragma once
#include <array>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string_view>
namespace venture::environment {
// Immutable host settings captured on first use. Guest libc environment is separate.
// Returned pointers remain valid for the process lifetime, including after refresh.
const char* getenv(const char* name);
// For controlled initialization/test changes. Serialize host mutation with refresh;
// readers may continue concurrently. Previous snapshots are retained.
void refresh();
// VENTURE_PASSTHROUGH: mixed reality is on by default; exactly "0" (the launcher's
// opt-out) turns it off. Unset or any other value leaves it on.
bool passthrough_enabled();
// VENTURE_PASSTHROUGH_KEY: the color key passthrough color as six hex digits
// (RRGGBB, sRGB, an optional leading '#'), chosen by the user in Settings; unset,
// empty or malformed means no color key. A color key is never chosen otherwise.
std::optional<std::array<std::uint8_t, 3>> passthrough_color_key();
// Six hex digits (an optional leading '#') as RGB; nothing when malformed.
std::optional<std::array<std::uint8_t, 3>> parse_color_key(std::string_view);
// Whether an OpenXR runtime manifest (its path, the path it links to, or its
// contents) names SteamVR.
bool steamvr_manifest(std::string_view path, std::string_view contents);
// Running inside the Venture Flatpak (FLATPAK_ID) with SteamVR as the OpenXR
// runtime (XR_RUNTIME_JSON, else ~/.config/openxr/1/active_runtime.json).
// Flatpak's sandbox cannot reach SteamVR, so a launch cannot work.
bool flatpak_steamvr();
} // namespace venture::environment
