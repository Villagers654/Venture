// The games Venture installed on this headset (Android APK): adapted and
// signed with the user's game key, then installed through PackageInstaller
// (docs/design/native-headsets.md, 5.2). The Platform broker serves only
// these packages, for the account each was installed for.
#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace venture::android {
struct HeadsetGame {
  std::string package;
  std::string app_id;
  std::string binary_id;
  std::int64_t version_code = 0;
  std::string version_name;
  std::string account;  // the account key whose Meta session serves its Platform calls
  bool operator==(const HeadsetGame&) const = default;
};

// <data dir>/headset/games.json
std::filesystem::path headset_games_file(const std::filesystem::path& data);
// Missing or unreadable: no games. Entries without a package are skipped.
std::vector<HeadsetGame> load_headset_games(const std::filesystem::path& data);
void save_headset_games(const std::filesystem::path& data, const std::vector<HeadsetGame>& games);
std::optional<HeadsetGame> find_headset_game(const std::filesystem::path& data, std::string_view package);
// Adds or replaces the entry for game.package.
void record_headset_game(const std::filesystem::path& data, const HeadsetGame& game);
}  // namespace venture::android
