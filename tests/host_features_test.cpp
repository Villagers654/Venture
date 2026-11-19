// SPDX-License-Identifier: GPL-3.0-or-later
#include "launcher/debug_report.h"
#include "launcher/host_features.h"
#include "launcher/options.h"
#include "common/process.h"
#include <doctest/doctest.h>
#include <fstream>
#include <initializer_list>

using namespace venture;
using namespace venture::launcher;

namespace {
constexpr Feature kRuntimeOnly[] = {Feature::display,         Feature::vr_runtime,          Feature::passthrough,
                                    Feature::runtime_console, Feature::game_language,       Feature::texture_compression,
                                    Feature::translated_code, Feature::controls,            Feature::game_mods,
                                    Feature::game_settings,   Feature::save_transfer,       Feature::game_data_folders,
                                    Feature::steam};
}  // namespace

TEST_CASE("host features: a PC shows every setting") {
  const HostFeatures pc;
  for (int f = 0; f <= int(Feature::install_location); ++f) CHECK(shown(Feature(f), pc));
}

TEST_CASE("host features: where games are the device's own apps, the PC's settings are hidden") {
  const auto device = device_app_features();
  for (auto f : kRuntimeOnly) CHECK_FALSE(shown(f, device));
  // Folders and drives, adb and the file manager are not there either.
  for (auto f : {Feature::games_folder, Feature::install_location, Feature::install_ask, Feature::adb_location,
                 Feature::import_device, Feature::logs_folder})
    CHECK_FALSE(shown(f, device));
  // Picking a game's files (and folders) through the system's picker stays.
  CHECK(shown(Feature::import_folder, device));
}

TEST_CASE("host features: each setting follows the feature it needs, not the platform") {
  HostFeatures no_steam;
  no_steam.steam = false;
  CHECK_FALSE(shown(Feature::steam, no_steam));
  CHECK(shown(Feature::install_ask, no_steam));  // still asks for the drive
  CHECK(shown(Feature::vr_runtime, no_steam));

  HostFeatures one_folder;
  one_folder.library_folders = false;
  CHECK_FALSE(shown(Feature::games_folder, one_folder));
  CHECK_FALSE(shown(Feature::install_location, one_folder));
  CHECK(shown(Feature::install_ask, one_folder));  // still asks about Steam

  HostFeatures neither = one_folder;
  neither.steam = false;
  CHECK_FALSE(shown(Feature::install_ask, neither));

  HostFeatures no_file_manager;
  no_file_manager.open_folders = false;
  CHECK_FALSE(shown(Feature::logs_folder, no_file_manager));
  CHECK_FALSE(shown(Feature::game_data_folders, no_file_manager));
  CHECK(shown(Feature::save_transfer, no_file_manager));

  HostFeatures no_adb;
  no_adb.adb = false;
  CHECK_FALSE(shown(Feature::import_device, no_adb));
  CHECK(shown(Feature::import_folder, no_adb));

  HostFeatures no_folder_dialog;
  no_folder_dialog.folder_dialog = false;
  CHECK_FALSE(shown(Feature::import_folder, no_folder_dialog));
}

TEST_CASE("host features: --smoke-device-apps") {
  CHECK(parse_options({"venture", "--smoke-device-apps"}).smoke_device_apps);
  CHECK_FALSE(parse_options({"venture"}).smoke_device_apps);
}

TEST_CASE("debug report: where games are the device's own apps, nothing about a PC's VR runtime") {
  const auto data = fs::temp_directory_path() / ("venture-report-device-" + std::to_string(current_process_id()));
  fs::remove_all(data);
  fs::create_directories(data / "logs");
  auto read = [](const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in), {}};
  };
  const auto device = read(write_debug_report({.data = data, .version = "9.9.9", .headset = "Device: Vendor Headset\n",
                                               .runtime = false}));
  CHECK(device.find("=== Headset ===\nDevice: Vendor Headset") != std::string::npos);
  CHECK(device.find("Games run as their own apps on this headset") != std::string::npos);
  for (const char* pc : {"=== VR runtime ===", "=== Environment ===", "=== Settings for this game ===",
                         "VR processes", "SteamVR", "No game has been started"})
    CHECK_MESSAGE(device.find(pc) == std::string::npos, pc);
  const auto pc = read(write_debug_report({.data = data, .version = "9.9.9"}));
  CHECK(pc.find("=== VR runtime ===") != std::string::npos);
  CHECK(pc.find("No game has been started with this Venture yet.") != std::string::npos);
  fs::remove_all(data);
}
