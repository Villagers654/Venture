#include "common/library/library.h"
#include "common/paths.h"
#include "common/environment.h"
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
TEST_CASE("Mixed reality is on unless the saved settings turn it off") {
  Profile profile;
  CHECK(library::Settings::load(profile.root).passthrough);
  paths::atomic_write(profile.root / "settings.json", Json{{"mixedReality", false}}.dump());
  CHECK_FALSE(library::Settings::load(profile.root).passthrough);
  for (const auto& value : {Json(true), Json(nullptr), Json(0), Json("false"), Json::array()}) {
    paths::atomic_write(profile.root / "settings.json", Json{{"mixedReality", value}}.dump());
    CHECK(library::Settings::load(profile.root).passthrough);
  }
  // The earlier opt-in keys recorded a default-off choice; they do not turn it off now.
  for (const auto* key : {"passthrough", "steamFramePassthrough"}) {
    paths::atomic_write(profile.root / "settings.json", Json{{key, false}}.dump());
    CHECK(library::Settings::load(profile.root).passthrough);
  }
}
TEST_CASE("Mixed reality on and off survive a fresh settings load without changing other choices") {
  Profile profile;
  auto settings = library::Settings::defaults(profile.root);
  CHECK(settings.passthrough);
  settings.console = true;
  settings.controls = controls::migrate_menu_button(2);
  settings.texture_compression = 1;
  settings.game_language = "de-DE";
  for (const bool enabled : {false, true, false}) {
    settings.passthrough = enabled;
    settings.save(profile.root);
    const auto loaded = library::Settings::load(profile.root);
    const auto saved = paths::read_json(profile.root / "settings.json");
    CHECK(loaded.passthrough == enabled);
    CHECK(saved["mixedReality"] == enabled);
    CHECK_FALSE(saved.contains("passthrough"));
    CHECK_FALSE(saved.contains("steamFramePassthrough"));
    CHECK(loaded.games == settings.games);
    CHECK(loaded.console);
    CHECK(loaded.controls == settings.controls);
    CHECK(loaded.texture_compression == 1);
    CHECK(loaded.game_language == "de-DE");
  }
}
TEST_CASE("Kept Vulkan layers survive a settings round trip; separators are refused") {
  Profile profile;
  paths::atomic_write(profile.root / "settings.json",
                      Json{{"vulkanLayers", {"VK_LAYER_reshade*", "", "a,b", 3, "*foveabridge*"}}}.dump());
  auto settings = library::Settings::load(profile.root);
  CHECK(settings.vulkan_layers == std::vector<std::string>{"VK_LAYER_reshade*", "*foveabridge*"});
  settings.save(profile.root);
  CHECK(library::Settings::load(profile.root).vulkan_layers == settings.vulkan_layers);
  settings.vulkan_layers.clear();
  settings.save(profile.root);
  CHECK_FALSE(paths::read_json(profile.root / "settings.json").contains("vulkanLayers"));
}
TEST_CASE("Controls are saved globally and per game, and the old menu setting migrates once") {
  Profile profile;
  // Settings > Runtime "Menu from both stick clicks: Always" becomes the two-button menu.
  paths::atomic_write(profile.root / "settings.json", Json{{"menuButton", 1}}.dump());
  auto settings = library::Settings::load(profile.root);
  CHECK(settings.controls == controls::migrate_menu_button(1));
  CHECK_FALSE(settings.controls.empty());
  // Saved without menuButton; a later load keeps the player's edits.
  settings.controls["/interaction_profiles/valve/index_controller"].menu.mode = controls::MenuMode::off;
  settings.game_controls["1234"]["/interaction_profiles/htc/vive_controller"].rows[0][0] = controls::Inputs{"/input/menu/click"};
  settings.save(profile.root);
  const auto json = paths::read_json(profile.root / "settings.json");
  CHECK_FALSE(json.contains("menuButton"));
  const auto loaded = library::Settings::load(profile.root);
  CHECK(loaded.controls == settings.controls);
  CHECK(loaded.game_controls == settings.game_controls);
  CHECK(&loaded.controls_for("1234") == &loaded.game_controls.at("1234"));
  CHECK(&loaded.controls_for("999") == &loaded.controls);
  // Off migrates too; automatic leaves the defaults.
  paths::atomic_write(profile.root / "settings.json", Json{{"menuButton", 2}}.dump());
  CHECK(library::Settings::load(profile.root).controls == controls::migrate_menu_button(2));
  paths::atomic_write(profile.root / "settings.json", Json{{"menuButton", 0}}.dump());
  CHECK(library::Settings::load(profile.root).controls.empty());
  // A settings file that has controls ignores a stale menuButton.
  paths::atomic_write(profile.root / "settings.json", Json{{"menuButton", 1}, {"controls", Json::object()}}.dump());
  CHECK(library::Settings::load(profile.root).controls.empty());
}
TEST_CASE("Color key passthrough is off unless chosen and keeps a valid key color") {
  Profile profile;
  auto loaded = library::Settings::load(profile.root);
  CHECK_FALSE(loaded.color_key);
  CHECK(loaded.key_color == "FF00FF");
  paths::atomic_write(profile.root / "settings.json",
                      Json{{"passthroughColorKey", true}, {"passthroughKeyColor", "#00ff00"}}.dump());
  loaded = library::Settings::load(profile.root);
  CHECK(loaded.color_key);
  CHECK(loaded.key_color == "00ff00");
  for (const auto& bad : {Json("00ff0"), Json("GG0000"), Json(12), Json(nullptr)}) {
    paths::atomic_write(profile.root / "settings.json", Json{{"passthroughKeyColor", bad}}.dump());
    CHECK(library::Settings::load(profile.root).key_color == "FF00FF");
  }
  auto settings = library::Settings::defaults(profile.root);
  settings.color_key = true;
  settings.key_color = "0000FF";
  settings.save(profile.root);
  loaded = library::Settings::load(profile.root);
  CHECK(loaded.color_key);
  CHECK(loaded.key_color == "0000FF");
  CHECK(loaded.passthrough);
}
TEST_CASE("Color key values parse as six hex digits only") {
  CHECK(environment::parse_color_key("FF00FF") == std::array<std::uint8_t, 3>{255, 0, 255});
  CHECK(environment::parse_color_key("#99007f") == std::array<std::uint8_t, 3>{153, 0, 127});
  for (const auto* bad : {"", "#", "FF00F", "FF00FF0", "FF00FG", " FF00FF"})
    CHECK_FALSE(environment::parse_color_key(bad));
}
TEST_CASE("Hidden library entries and Show hidden survive a settings round trip") {
  Profile profile;
  auto settings = library::Settings::load(profile.root);
  CHECK(settings.hidden.empty());
  CHECK_FALSE(settings.show_hidden);
  CHECK(settings.listed("1234567890"));
  settings.hidden = {"1234567890", "com.example.import", "2345678901@abc"};
  settings.steam_skip = {"1234567890"};
  settings.save(profile.root);
  auto loaded = library::Settings::load(profile.root);
  CHECK(loaded.hidden == settings.hidden);
  CHECK(loaded.steam_skip == settings.steam_skip);  // hiding and Steam are separate choices
  CHECK_FALSE(loaded.show_hidden);
  CHECK_FALSE(loaded.listed("1234567890"));
  CHECK(loaded.listed("3456789012"));
  loaded.show_hidden = true;
  loaded.save(profile.root);
  loaded = library::Settings::load(profile.root);
  CHECK(loaded.show_hidden);
  CHECK(loaded.listed("1234567890"));
  // Malformed entries are dropped, not fatal.
  paths::atomic_write(profile.root / "settings.json",
                      Json{{"hiddenGames", Json::array({"1234567890", 5, "", std::string(65, '1'), nullptr})},
                           {"showHiddenGames", "yes"}}
                          .dump());
  loaded = library::Settings::load(profile.root);
  CHECK(loaded.hidden == std::set<std::string>{"1234567890"});
  CHECK_FALSE(loaded.show_hidden);
}
TEST_CASE("SteamVR runtime manifests are recognised by path, link or contents") {
  CHECK(environment::steamvr_manifest("/home/u/.local/share/Steam/steamapps/common/SteamVR/steamxr_linux64.json", ""));
  CHECK(environment::steamvr_manifest("/home/u/.config/openxr/1/active_runtime.json /x/SteamVR/steamxr_linux64.json", ""));
  CHECK(environment::steamvr_manifest("/tmp/active_runtime.json",
                                      R"({"runtime":{"library_path":"../bin/linux64/vrclient.so","name":"SteamVR"}})"));
  CHECK_FALSE(environment::steamvr_manifest("/usr/share/openxr/1/openxr_monado.json",
                                            R"({"runtime":{"library_path":"../../../lib/libopenxr_monado.so"}})"));
  CHECK_FALSE(environment::steamvr_manifest("/home/u/.config/openxr/1/active_runtime.json",
                                            R"({"runtime":{"name":"WiVRn","library_path":"libopenxr_wivrn.so"}})"));
}
