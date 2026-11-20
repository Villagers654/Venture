#pragma once
#include <array>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
namespace venture::environment {
// Immutable host settings captured on first use. Guest libc environment is separate.
// Returned pointers remain valid for the process lifetime, including after refresh.
const char* getenv(const char* name);
// For controlled initialization/test changes. Serialize host mutation with refresh;
// readers may continue concurrently. Previous snapshots are retained.
void refresh();
// Linux: gamescope (SteamOS's game mode, the Steam Frame's shell) offers a
// Wayland display with xdg-shell to every client, but names it only in
// GAMESCOPE_WAYLAND_DISPLAY and unsets WAYLAND_DISPLAY for what it starts
// unless it runs with --expose-wayland. When WAYLAND_DISPLAY is unset or
// empty and gamescope's socket exists, sets WAYLAND_DISPLAY to it, so SDL, GTK
// and the processes Venture starts use it. Call before other threads start
// (setenv). True when it set it.
bool use_gamescope_wayland();
// The display use_gamescope_wayland() would set from these variables (the
// socket must exist), or empty.
std::string gamescope_wayland_display(const char* wayland, const char* gamescope, const char* runtime_dir);
// Whether this process's windows go to gamescope (WAYLAND_DISPLAY is
// gamescope's). gamescope shows only the windows of the app Steam has in
// focus, found from each window's process ancestry; a window another
// process opens for Venture (the portal's file chooser, a browser the portal
// starts) belongs to no Steam app and stays hidden.
bool inside_gamescope();
// Whether the launcher's windows (its own and its sign-in window's) go on
// gamescope's Xwayland: inside gamescope, with its DISPLAY set. gamescope
// closes an app's windows (SteamVR's close on the app's panel, as on the Steam
// Frame) only by sending WM_DELETE_WINDOW to its X11 windows, and logs that it
// cannot close Wayland ones. False elsewhere (Linux uses Wayland).
bool gamescope_xwayland();
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
// Inside the Venture Flatpak (FLATPAK_ID), SteamVR as the OpenXR runtime
// (XR_RUNTIME_JSON, else ~/.config/openxr/1/active_runtime.json): whether its
// manifest and client library are visible in the sandbox, and when not, the
// host folder to grant (SteamVR in a Steam library on another drive, say).
struct FlatpakSteamVr {
  bool steamvr = false;       // in the Flatpak with SteamVR the runtime
  bool visible = true;        // its manifest and client library are in the sandbox
  std::string hidden_folder;  // when not visible: the folder holding what is missing
};
FlatpakSteamVr flatpak_steamvr_state();
// flatpak_steamvr_state() names SteamVR and it is not visible, so a launch cannot work.
bool flatpak_steamvr();
// The documented way to grant one more folder to this Flatpak:
// flatpak override --user --filesystem='<folder>':ro <FLATPAK_ID>.
std::string flatpak_override_command(std::string_view folder);
// The client library an OpenXR runtime manifest names (`contents`), resolved
// beside the manifest's real file when relative; empty when it names none.
std::string steamvr_library(std::string_view manifest, std::string_view contents);
} // namespace venture::environment
