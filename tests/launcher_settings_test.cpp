#include "common/library/library.h"
#include "common/paths.h"
#include <doctest/doctest.h>
#include <chrono>

using namespace venture;
namespace {
struct Profile {
  fs::path root = fs::temp_directory_path() / ("venture-settings-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  Profile() { fs::create_directories(root); }
  ~Profile() { std::error_code error; fs::remove_all(root, error); }
};
}
TEST_CASE("Mixed reality passthrough is an explicit typed saved preference") {
  Profile profile;
  CHECK_FALSE(library::Settings::load(profile.root).passthrough);
  for (const auto* key : {"passthrough", "steamFramePassthrough"}) {
    for (const auto& value : {Json(false), Json(nullptr), Json(1), Json("1"), Json("true"), Json::array()}) {
      paths::atomic_write(profile.root / "settings.json", Json{{key, value}}.dump());
      CHECK_FALSE(library::Settings::load(profile.root).passthrough);
    }
    paths::atomic_write(profile.root / "settings.json", Json{{key, true}}.dump());
    CHECK(library::Settings::load(profile.root).passthrough);
  }
  for (const auto& value : {Json(false), Json(nullptr), Json("true")}) {
    paths::atomic_write(profile.root / "settings.json", Json{{"passthrough", value}, {"steamFramePassthrough", true}}.dump());
    CHECK_FALSE(library::Settings::load(profile.root).passthrough);
  }
}
TEST_CASE("Mixed reality on and off survive a fresh settings load without changing other choices") {
  Profile profile;
  auto settings = library::Settings::defaults(profile.root);
  settings.console = true;
  settings.menu_button = 2;
  settings.texture_compression = 1;
  settings.game_language = "de-DE";
  for (const bool enabled : {true, false, true}) {
    settings.passthrough = enabled;
    settings.save(profile.root);
    const auto loaded = library::Settings::load(profile.root);
    CHECK(loaded.passthrough == enabled);
    CHECK(paths::read_json(profile.root / "settings.json")["passthrough"] == enabled);
    CHECK_FALSE(paths::read_json(profile.root / "settings.json").contains("steamFramePassthrough"));
    CHECK(loaded.games == settings.games);
    CHECK(loaded.console);
    CHECK(loaded.menu_button == 2);
    CHECK(loaded.texture_compression == 1);
    CHECK(loaded.game_language == "de-DE");
  }
}
