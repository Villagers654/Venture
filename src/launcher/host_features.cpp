// SPDX-License-Identifier: GPL-3.0-or-later
#include "launcher/host_features.h"

namespace venture::launcher {
HostFeatures device_app_features() {
  // The device runs each game as its own app (no Venture runtime, so no
  // Steam shortcut to start one either), Venture keeps everything in its app
  // storage, which no file manager shows, it starts no other programs (adb),
  // and the system's document picker opens files and folders through
  // Venture's own picker (common/library/import.h, documents).
  return {.runtime = false,
          .steam = false,
          .adb = false,
          .library_folders = false,
          .open_folders = false,
          .folder_dialog = true};
}

// Built where Venture installs games as the device's own apps
// (platform/android/headset_play.cpp defines it there).
#if !VENTURE_GAMES_AS_DEVICE_APPS
HostFeatures host_features() { return {}; }
#endif

bool shown(Feature feature, const HostFeatures& host) {
  switch (feature) {
    case Feature::games_folder:
    case Feature::install_location:
      return host.library_folders;
    case Feature::adb_location:
    case Feature::import_device:
      return host.adb;
    case Feature::steam:
      return host.steam && host.runtime;
    case Feature::install_ask:
      // It asks for the drive and whether the game goes to Steam.
      return host.library_folders || (host.steam && host.runtime);
    case Feature::display:
    case Feature::vr_runtime:
    case Feature::passthrough:
    case Feature::runtime_console:
    case Feature::game_language:
    case Feature::texture_compression:
    case Feature::translated_code:
    case Feature::controls:
    case Feature::game_mods:
    case Feature::game_settings:
    case Feature::save_transfer:
      return host.runtime;
    case Feature::game_data_folders:
      // The saves Venture's runtime keeps for the game, in a file manager.
      return host.runtime && host.open_folders;
    case Feature::logs_folder:
      return host.open_folders;
    case Feature::import_folder:
      return host.folder_dialog;
  }
  return true;
}
}  // namespace venture::launcher
