// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// What the machine the launcher runs on can do, and which settings and
// features the interface shows because of it. The interface asks shown()
// about a feature, never which platform it runs on: a headset that runs
// games as its own apps hides the PC's VR runtime choice, Steam, display and
// mirror settings because it has none of them, and a later host gets the
// right interface from its features alone.

namespace venture::launcher {
struct HostFeatures {
  // Games run in Venture's runtime on this machine, through an OpenXR runtime
  // Venture picks (a PC, the Steam Frame). Off where Venture installs each
  // game as the device's own app (an Android headset, headset_play.h): the
  // game then uses the device's runtime, display, audio, passthrough and
  // controllers itself, and keeps its saves in its own app data.
  bool runtime = true;
  // A Steam client whose library Venture adds games to (Steam shortcuts start
  // Venture's runtime here).
  bool steam = true;
  // Venture can run adb to reach a headset (importing its games).
  bool adb = true;
  // Games can go in folders and on drives the user picks. Off where Venture
  // keeps them in its own app storage.
  bool library_folders = true;
  // The system's file manager can show one of Venture's folders.
  bool open_folders = true;
  // The system's file dialog can pick a folder (not only files).
  bool folder_dialog = true;
};

// This machine's features, settled at start.
HostFeatures host_features();
// A host where Venture installs games as the device's own apps.
HostFeatures device_app_features();

// The settings and features that depend on the host.
enum class Feature {
  games_folder,        // Settings > Library > Games folder
  adb_location,        // Settings > Library > adb
  steam,               // Add games to Steam, Restart Steam, a game's Add to Steam
  install_ask,         // Ask when installing (drive and Steam)
  display,             // Settings > Display, a game's display rows
  vr_runtime,          // Settings > VR runtime
  passthrough,         // Passthrough and key color
  runtime_console,     // Show runtime console
  game_language,       // the language games see as the system's
  texture_compression, // Compress textures and its threshold
  translated_code,     // Keep translated code
  logs_folder,         // Troubleshooting > Open logs
  controls,            // the Controls page and a game's Controls tab
  game_mods,           // a game's Mods tab
  game_settings,       // a game's own Settings tab
  game_data_folders,   // a game's Show save data / Show game data
  save_transfer,       // a game's Import save data
  import_device,       // Import > from a headset over adb
  import_folder,       // Import > Choose folder
  install_location,    // the drive or folder a new game goes to
};
bool shown(Feature feature, const HostFeatures& host);
}  // namespace venture::launcher
