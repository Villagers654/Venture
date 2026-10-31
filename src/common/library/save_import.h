#pragma once
#include "common/android/adb.h"
#include "common/paths.h"
#include <optional>
#include <stop_token>

// Importing a game's save data on its own, from a Quest over adb or a folder
// on this computer, into an account's storage for the game. Each top-level
// folder the import brings (files/, shared_prefs/...) replaces the current
// one, which moves to a backup first.
namespace venture::saves {
// Where a game keeps data, as on Android: its private storage
// (/data/data/<package>: shared_prefs/, databases/, files/...) or its shared
// storage (/sdcard/Android/data/<package>, where most Quest saves live).
enum class Area { internal, external };
// Save data found in a picked folder.
struct Found {
  fs::path folder;   // its contents go into `area`
  Area area = Area::external;
  std::string entry;  // when set, `folder` itself is this top-level entry ("files")
};
// The game's save data in `picked`: a copy of Android/data/<package> (or of
// /sdcard, or a folder holding <package>/), its files/ folder, or a copy of
// its private storage. Nullopt when nothing there looks like it.
std::optional<Found> find_local(const fs::path &picked, std::string_view package);
struct Imported {
  std::size_t files = 0;
  std::size_t skipped = 0;  // headset files it would not send (each logged)
  std::uint64_t bytes = 0;
  fs::path backup;  // the replaced data; empty when nothing was replaced
};
// `data` is the launcher's data folder, `key` the account ("local" for none).
Imported import_local(const fs::path &data, std::string_view key, std::string_view package, const Found &found,
                      std::stop_token stop = {});
// The app's shared storage on the headset (its private storage cannot be read
// over adb); throws when the game is not on it or has nothing there.
Imported import_headset(const fs::path &data, std::string_view key, std::string_view package,
                        const android::adb::Client &client, std::stop_token stop = {});
// Backups of `package` for `key`, newest last.
fs::path backups_dir(const fs::path &data, std::string_view key, std::string_view package);
}  // namespace venture::saves
