#include "common/android/headset_games.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "common/json.h"
#include "common/log.h"
#include "common/paths.h"

namespace venture::android {
std::filesystem::path headset_games_file(const std::filesystem::path& data) {
  return data / "headset" / "games.json";
}

std::vector<HeadsetGame> load_headset_games(const std::filesystem::path& data) {
  std::vector<HeadsetGame> games;
  std::ifstream in(headset_games_file(data), std::ios::binary);
  if (!in) return games;
  std::stringstream text;
  text << in.rdbuf();
  try {
    const auto root = parse_json(text.str());
    for (const auto& entry : field(root, "games")) {
      HeadsetGame game{str(entry, "package"), str(entry, "appId"), str(entry, "binaryId"),
                       static_cast<std::int64_t>(number(entry, "versionCode")), str(entry, "versionName"),
                       str(entry, "account")};
      if (!game.package.empty()) games.push_back(std::move(game));
    }
  } catch (const std::exception&) {
    log::warn("headset", "{} is unreadable; no installed games are known", headset_games_file(data).string());
    games.clear();
  }
  return games;
}

void save_headset_games(const std::filesystem::path& data, const std::vector<HeadsetGame>& games) {
  Json list = Json::array();
  for (const auto& game : games)
    list.push_back({{"package", game.package}, {"appId", game.app_id}, {"binaryId", game.binary_id},
                    {"versionCode", game.version_code}, {"versionName", game.version_name},
                    {"account", game.account}});
  const auto file = headset_games_file(data);
  std::filesystem::create_directories(file.parent_path());
  paths::atomic_write(file, Json{{"version", 1}, {"games", list}}.dump(2) + '\n');
}

std::optional<HeadsetGame> find_headset_game(const std::filesystem::path& data, std::string_view package) {
  for (auto& game : load_headset_games(data))
    if (game.package == package) return std::move(game);
  return std::nullopt;
}

void record_headset_game(const std::filesystem::path& data, const HeadsetGame& game) {
  auto games = load_headset_games(data);
  std::erase_if(games, [&](const HeadsetGame& g) { return g.package == game.package; });
  games.push_back(game);
  std::ranges::sort(games, {}, &HeadsetGame::package);
  save_headset_games(data, games);
}
}  // namespace venture::android
