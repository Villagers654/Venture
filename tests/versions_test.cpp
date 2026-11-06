// Versions installed beside a game's own install (pinned): Meta's version
// history, the library identity, folders, saves and caches of a pinned build.
#include "common/download/manager.h"
#include "common/library/library.h"
#include "common/saves_migration.h"
#include "common/steam_library.h"
#include "launcher/options.h"
#include <chrono>
#include <cstdlib>
#include <doctest/doctest.h>

#include "async_wait.h"

using namespace venture;
namespace {
Json history() {
  return parse_json(R"({"data":{"node":{"primary_binaries":{"nodes":[
    {"__typename":"AndroidBinary","id":"300","version":"1.2","version_code":12,
     "binary_release_channels":{"nodes":[{"channel_name":"LIVE"}]}},
    {"__typename":"AndroidBinary","id":"400","version":"1.3-beta","version_code":13,
     "binary_release_channels":{"nodes":[{"channel_name":"BETA"}]}},
    {"__typename":"AndroidBinary","id":"250","version":"1.1","version_code":11,
     "binary_release_channels":{"nodes":[]}},
    {"__typename":"AndroidBinary","id":"100","version":"1.0","version_code":10,
     "binary_release_channels":{"nodes":[{"channel_name":"LIVE"},{"channel_name":"RC"}]}},
    {"__typename":"AndroidBinary","id":"90","version":"1.0","version_code":9,
     "binary_release_channels":{"nodes":[{"channel_name":"LIVE"}]}},
    {"__typename":"RiftBinary","id":"555","version":"pc","version_code":50,
     "binary_release_channels":{"nodes":[{"channel_name":"LIVE"}]}},
    {"__typename":"AndroidBinary","id":"666","platform":"PC","version":"pc","version_code":60,
     "binary_release_channels":{"nodes":[{"channel_name":"LIVE"}]}}
  ]}}}})");
}
struct Env {
  std::string key, previous;
  bool had;
  Env(std::string k, const fs::path &value) : key(std::move(k)), had(std::getenv(key.c_str()) != nullptr) {
    if (had) previous = std::getenv(key.c_str());
    set(paths::utf8(value));
  }
  void set(const std::string &value) {
#ifdef _WIN32
    _putenv_s(key.c_str(), value.c_str());
#else
    setenv(key.c_str(), value.c_str(), 1);
#endif
  }
  ~Env() {
#ifdef _WIN32
    _putenv_s(key.c_str(), had ? previous.c_str() : "");
#else
    if (had) setenv(key.c_str(), previous.c_str(), 1);
    else unsetenv(key.c_str());
#endif
  }
};
struct Directory {
  fs::path root = fs::temp_directory_path() /
                  ("venture-versions-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Directory() { fs::create_directories(root); }
  ~Directory() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};
// Two builds of one game: 456789 (1.2, the live one) and 456700 (1.1).
Json binary(std::string_view id) {
  const bool old = id == "456700";
  return Json{{"id", std::string(id)},
              {"binary_application", {{"id", "123456"}}},
              {"package_name", "com.example.versions"},
              {"version", old ? "1.1" : "1.2"},
              {"version_code", old ? 11 : 12},
              {"size", 4},
              {"asset_files", {{"nodes", Json::array({Json{{"id", old ? "111100" : "111111"},
                                                           {"file_name", "required.dat"},
                                                           {"size", 2},
                                                           {"is_required", true}}})},
                               {"count", 1}}}};
}
meta::Store::Transport queries() {
  return [](std::string_view url, const http::Fields &fields) {
    auto u = http::parse_url(url);
    if (u.path == "/authenticate_application")
      return Json{{"access_token", "fixture-app-token"}};
    std::map<std::string, std::string> f(fields.begin(), fields.end());
    if (f["doc_id"] == "4850747515044496")
      return parse_json(
          R"({"data":{"viewer":{"user":{"id":"777777","active_entitlements":{"nodes":[{"item":{"id":"123456","platform":"ANDROID_6DOF"}}],"page_info":{"has_next_page":false}}}}}})");
    if (f["doc_id"] == "6549406941839522")
      return Json{{"data",
                   {{"item", {{"id", "123456"}, {"platform", "ANDROID_6DOF"}, {"latest_supported_binary", binary("456789")}}}}}};
    auto vars = parse_json(f["variables"]);
    return Json{{"data", {{"node", binary(str(vars, "binaryID"))}}}};
  };
}
} // namespace

TEST_CASE("version history keeps published Quest builds, newest first, and marks the live one") {
  auto list = meta::release_builds(history(), Json{{"id", "300"}});
  REQUIRE(list.size() == 4);
  CHECK(list[0].binary_id == "400");  // a newer beta stays listed, not live
  CHECK_FALSE(list[0].live);
  CHECK(list[0].channels == std::vector<std::string>{"BETA"});
  CHECK(list[1].binary_id == "300");
  CHECK(list[1].live);
  CHECK(list[1].version_code == "12");
  CHECK(list[2].channels == std::vector<std::string>{"LIVE", "RC"});
  CHECK(list[3].label() == "1.0 (9)");
  // Builds on no channel (internal uploads) and PC builds are not offered.
  for (auto id : {"250", "555", "666"})
    CHECK(std::ranges::find(list, std::string(id), &meta::Build::binary_id) == list.end());
  // The live build is listed even when the history lacks it.
  auto added = meta::release_builds(history(), Json{{"id", "700"}, {"version", "2.0"}, {"version_code", 20}});
  REQUIRE(added.size() == 5);
  CHECK(added[0].binary_id == "700");
  CHECK(added[0].live);
  CHECK(added[0].channels.empty());
  CHECK(std::ranges::count_if(meta::release_builds(history()), &meta::Build::live) == 0);
  CHECK_THROWS(meta::release_builds(parse_json(R"({"data":{"node":null}})")));
}

TEST_CASE("a pinned version has its own library id, folder, saves and launch target") {
  CHECK(meta::version_id("123456", "456700") == "123456@456700");
  CHECK(meta::base_app("123456@456700") == "123456");
  CHECK(meta::base_app("123456") == "123456");
  CHECK(meta::version_binary("123456@456700") == "456700");
  CHECK(meta::version_binary("123456").empty());

  auto plan = meta::build_plan("123456", binary("456700"));
  CHECK(plan.folder() == "11");
  CHECK_FALSE(plan.json().contains("pinned"));
  CHECK((library::Install{plan, {}, true, 0}.id() == "123456"));
  plan.pinned = true;
  CHECK(plan.folder() == "11-456700");
  auto parsed = meta::parse_plan(plan.json());
  CHECK(parsed.pinned);
  CHECK((library::Install{parsed, {}, true, 0}.id() == "123456@456700"));

  CHECK(paths::version_folder("11"));
  CHECK(paths::version_folder("11-456700"));
  for (auto bad : {"", "11-", "-456700", "a-1", "1-2-3", "11-45x"})
    CHECK_FALSE(paths::version_folder(bad));
  const auto root = fs::temp_directory_path() / "venture-versions-root";
  CHECK(paths::game_dir(root, "com.example.versions", "11-456700") == root / "com.example.versions" / "11-456700");
  CHECK_THROWS(paths::game_dir(root, "com.example.versions", "11-../x"));

  CHECK(paths::safe_launch_target("com.example.versions"));
  CHECK(paths::safe_launch_target("com.example.versions@11-456700"));
  CHECK_FALSE(paths::safe_launch_target("com.example.versions@x"));
  CHECK_FALSE(paths::safe_launch_target("@11"));
  auto options = launcher::parse_options({"venture", "--launch", "com.example.versions@11-456700"});
  CHECK_FALSE(options.invalid_launch);
  CHECK(options.launch_package == "com.example.versions@11-456700");

  // The game's own saves stay where they are; a pinned version has its own.
  const auto data = fs::temp_directory_path() / "venture-versions-data";
  CHECK(account_data_dir(data, "local", "com.example.versions") == data / "data/users/local/com.example.versions");
  CHECK(account_data_dir(data, "local", "com.example.versions", "11-456700") ==
        data / "data/users/local/com.example.versions@11-456700");
  CHECK_THROWS(account_data_dir(data, "local", "com.example.versions", "../x"));
}

TEST_CASE("a pinned version installs beside the game's own install and uninstalls alone") {
  Directory directory;
  Env env("VENTURE_DATA_DIR", directory.root / "data");
  download::Manager manager(directory.root / "state", [] {},
                            {queries(),
                             {},
                             [](std::string, const fs::path &path, uint64_t size, std::stop_token,
                                const http::Progress &progress) {
                               paths::atomic_write(path, std::string(size, 'X'));
                               progress(size, size);
                             }});
  manager.set_account("0123456789ab");
  manager.enqueue("123456", "Game", directory.root, "fixture");
  REQUIRE(venture::test::wait_until([&] { return !manager.busy(); }));
  manager.enqueue_version("123456", "456700", "Game (1.1)", directory.root, "fixture", "0123456789ab");
  REQUIRE(venture::test::wait_until([&] { return !manager.busy(); }));
  auto jobs = manager.snapshot();
  REQUIRE(jobs.size() == 2);
  auto own = download::base_job(jobs, "123456"), pinned = download::base_job(jobs, "123456@456700");
  REQUIRE(own);
  REQUIRE(pinned);
  CHECK_FALSE(own->pinned);
  CHECK(pinned->pinned);
  CHECK(pinned->state == download::State::complete);
  CHECK_FALSE(download::base_job(jobs, "123456@999999"));

  auto installs = library::scan(directory.root);
  REQUIRE(installs.size() == 2);
  auto by_id = [&](std::string_view id) {
    return std::ranges::find_if(installs, [&](auto &i) { return i.id() == id; });
  };
  REQUIRE(by_id("123456") != installs.end());
  REQUIRE(by_id("123456@456700") != installs.end());
  const auto old = *by_id("123456@456700");
  CHECK(old.complete);
  CHECK(old.plan.pinned);
  CHECK(old.directory == directory.root / "com.example.versions" / "11-456700");
  CHECK(by_id("123456")->directory == directory.root / "com.example.versions" / "12");

  // Uninstalling the pinned version removes its files, OBB view and caches;
  // its saves and everything of the game's own install stay.
  const auto data = directory.root / "data";
  const auto own_view = data / "data/users/local/com.example.versions/external/Android/obb/com.example.versions";
  const auto pinned_data = data / "data/users/local/com.example.versions@11-456700";
  const auto pinned_view = pinned_data / "external/Android/obb/com.example.versions";
  for (auto file : {own_view / "main.12.com.example.versions.obb", pinned_view / "main.11.com.example.versions.obb",
                    pinned_data / "files/save.dat", data / "cache/com.example.versions/12/lib/x.so",
                    data / "cache/com.example.versions/11-456700/lib/x.so"})
    paths::atomic_write(file, "x");
  library::uninstall(directory.root, old);
  CHECK_FALSE(fs::exists(old.directory));
  CHECK_FALSE(fs::exists(pinned_view));
  CHECK_FALSE(fs::exists(data / "cache/com.example.versions/11-456700"));
  CHECK(fs::exists(pinned_data / "files/save.dat"));
  CHECK(fs::exists(own_view / "main.12.com.example.versions.obb"));
  CHECK(fs::exists(data / "cache/com.example.versions/12/lib/x.so"));
  CHECK(fs::exists(directory.root / "com.example.versions" / "12" / "plan.json"));
}

TEST_CASE("a paused pinned version stays pinned in the saved queue") {
  Directory directory;
  const auto state = directory.root / "state";
  const auto root = paths::utf8(directory.root);
  paths::atomic_write(state / "downloads.json",
                      Json{{"version", 2},
                           {"jobs", Json::array({Json{{"appId", "123456"}, {"name", "Game (1.1)"}, {"root", root},
                                                      {"account", "0123456789ab"}, {"binaryId", "456700"},
                                                      {"pinned", true}},
                                                 // A pinned job without its build is dropped.
                                                 Json{{"appId", "123456"}, {"name", "Broken"}, {"root", root},
                                                      {"account", "0123456789ab"}, {"pinned", true}},
                                                 Json{{"appId", "123456"}, {"name", "Game"}, {"root", root},
                                                      {"account", "0123456789ab"}}})}}
                          .dump());
  download::Manager manager(state, [] {});
  manager.set_account("0123456789ab");
  auto jobs = manager.snapshot();
  REQUIRE(jobs.size() == 2);
  auto pinned = download::base_job(jobs, "123456@456700");
  REQUIRE(pinned);
  CHECK(pinned->pinned);
  CHECK(pinned->state == download::State::paused);
  CHECK(download::base_job(jobs, "123456")->name == "Game");
  // Pausing the game's own download leaves the pinned version's alone, and
  // the saved queue keeps it pinned.
  manager.cancel("123456");
  auto saved = paths::read_json(state / "downloads.json");
  CHECK(std::ranges::count_if(saved["jobs"], [](const Json &j) { return field(j, "pinned") == true; }) == 1);
}

TEST_CASE("Steam launches a pinned version by its own target, beside the game's own shortcut") {
  Directory directory;
  Env data("VENTURE_DATA_DIR", directory.root / "data"), steam("VENTURE_STEAM_ROOT", directory.root);
  const auto config = directory.root / "userdata/42/config";
  fs::create_directories(config);
  auto result = steam_library::sync(steam_library::discover_root(), directory.root / "venture.exe",
                                    directory.root / "art",
                                    {{"com.example.versions", "Game", "123456"},
                                     {"com.example.versions@11-456700", "Game (1.1)", "123456"}},
                                    [] { return false; });
  CHECK(result.games == 2);
  auto doc = steam_library::read_vdf(paths::read(config / "shortcuts.vdf"));
  std::map<std::string, std::string> launches;  // name -> launch options
  for (const auto &list : doc.children)
    for (const auto &entry : list.children) {
      std::string name, options;
      for (const auto &n : entry.children) {
        if (n.key == "appname") name = n.value.substr(0, n.value.size() - 1);
        if (n.key == "LaunchOptions") options = n.value.substr(0, n.value.size() - 1);
      }
      launches[name] = options;
    }
  CHECK(launches["Game"] == "--launch com.example.versions");
  CHECK(launches["Game (1.1)"] == "--launch com.example.versions@11-456700");
}
