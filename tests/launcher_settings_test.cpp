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
TEST_CASE("Steam Frame passthrough is an explicit typed saved preference") {
  Profile profile;
  CHECK_FALSE(library::Settings::load(profile.root).steam_frame_passthrough);
  for (const auto& value : {Json(false), Json(nullptr), Json(1), Json("1"), Json("true"), Json::array()}) {
    paths::atomic_write(profile.root / "settings.json", Json{{"steamFramePassthrough", value}}.dump());
    CHECK_FALSE(library::Settings::load(profile.root).steam_frame_passthrough);
  }
  paths::atomic_write(profile.root / "settings.json", Json{{"steamFramePassthrough", true}}.dump());
  CHECK(library::Settings::load(profile.root).steam_frame_passthrough);
}
TEST_CASE("Steam Frame on and off survive a fresh settings load without changing other choices") {
  Profile profile;
  auto settings = library::Settings::defaults(profile.root);
  settings.console = true;
  settings.menu_button = 2;
  settings.texture_compression = 1;
  for (const bool enabled : {true, false, true}) {
    settings.steam_frame_passthrough = enabled;
    settings.save(profile.root);
    const auto loaded = library::Settings::load(profile.root);
    CHECK(loaded.steam_frame_passthrough == enabled);
    CHECK(paths::read_json(profile.root / "settings.json")["steamFramePassthrough"] == enabled);
    CHECK(loaded.games == settings.games);
    CHECK(loaded.console);
    CHECK(loaded.menu_button == 2);
    CHECK(loaded.texture_compression == 1);
  }
}

#ifdef _WIN32
#include "core/process.h"
#include "xr/host_xr.h"
#include "xr/steam_link.h"
#include "common/environment.h"
#include <windows.h>
#include <optional>
#include <cstdlib>

// The host identity and process configuration are explicit ports; the feature
// decision below is the actual production xr/steam_link.cpp implementation.
namespace venture {
ProcessConfig& process_config() { static ProcessConfig config; return config; }
namespace xr { HostXr& host_xr() { static HostXr host; return host; } }
}
TEST_CASE("Steam Frame runtime selection uses saved preferences instead of inherited environment") {
  Profile profile;
  constexpr auto key = "VENTURE_STEAM_LINK_PASSTHROUGH";
  const auto* current = std::getenv(key);
  struct Restore {
    const char* key;
    std::optional<std::string> previous;
    ~Restore() {
      _putenv_s(key, previous ? previous->c_str() : "");
      environment::refresh();
      process_config() = {};
      xr::host_xr().runtime_name.clear();
    }
  } restore{key, current ? std::optional<std::string>(current) : std::nullopt};
  xr::host_xr().runtime_name = "SteamVR/OpenXR";
  process_config() = {};
  _putenv_s(key, "1"); environment::refresh();
  process_config().steam_frame_passthrough = library::Settings::load(profile.root).steam_frame_passthrough;
  CHECK_FALSE(xr::steam_link_passthrough_configured());
  auto settings = library::Settings::defaults(profile.root);
  settings.steam_frame_passthrough = true;
  settings.save(profile.root);
  _putenv_s(key, "0"); environment::refresh();
  process_config().steam_frame_passthrough = library::Settings::load(profile.root).steam_frame_passthrough;
  CHECK(xr::steam_link_passthrough_configured());
  _putenv_s(key, ""); environment::refresh();
  CHECK(xr::steam_link_passthrough_configured());
  process_config().abi = "armeabi-v7a";
  CHECK_FALSE(xr::steam_link_passthrough_configured());
  process_config().abi = "arm64-v8a";
  xr::host_xr().runtime_name = "Other OpenXR runtime";
  CHECK_FALSE(xr::steam_link_passthrough_configured());
}
#endif
