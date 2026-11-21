#include "common/download/manager.h"
#include "common/file_ops.h"
#include "common/library/library.h"
#include "common/environment.h"
#include "common/process.h"
#include "common/steam_library.h"
#include "launcher/options.h"
#include <chrono>
#include <cstdlib>
#include <map>
#include <doctest/doctest.h>
#include <thread>
#ifndef _WIN32
#include <csignal>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <unistd.h>
extern char **environ;
#endif
using namespace venture;
using namespace venture::steam_library;
namespace {
struct Env {
  std::string key, previous;
  bool had;
  Env(std::string k, const fs::path &value)
      : key(k), had(std::getenv(k.c_str()) != nullptr) {
    if (had)
      previous = std::getenv(k.c_str());
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
    if (had)
      setenv(key.c_str(), previous.c_str(), 1);
    else
      unsetenv(key.c_str());
#endif
  }
};
struct Fixture {
  fs::path root =
      paths::executable().parent_path() / "test-artifacts" /
      ("steam-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  Env data{"VENTURE_DATA_DIR", root / "data"},
      steam{"VENTURE_STEAM_ROOT", root};
  fs::path config = root / "userdata/42/config",
           target = config / "shortcuts.vdf";
  Fixture() { fs::create_directories(config); }
};
Vdf vstring(std::string key, std::string value) {
  Vdf n;
  n.type = 1;
  n.key = key;
  n.value = value + '\0';
  return n;
}
Vdf object(std::string key, std::vector<Vdf> children) {
  Vdf n;
  n.key = key;
  n.children = children;
  return n;
}
Vdf foreign(std::string key) {
  auto entry = object(key, {vstring("appname", "RiftLift title"),
                            vstring("exe", "\"riftlift\""),
                            vstring("LaunchOptions", "launch example"),
                            object("tags", {vstring("0", "RiftLift")})});
  Vdf unknown;
  unknown.type = 3;
  unknown.key = "unknown-float";
  unknown.value = std::string("\x01\x02\x80\x7f", 4);
  entry.children.push_back(unknown);
  unknown.type = 5;
  unknown.key = "unknown-wide";
  unknown.value = std::string("A\0B\0\0\0", 6);
  entry.children.push_back(unknown);
  unknown.type = 7;
  unknown.key = "unknown-u64";
  unknown.value = std::string("\x01\x23\x45\x67\x89\xab\xcd\xef", 8);
  entry.children.push_back(unknown);
  return entry;
}
const Vdf &member(const Vdf &v, std::string_view key) {
  for (const auto &n : v.children)
    if (n.key == key)
      return n;
  throw std::runtime_error("fixture member missing");
}
} // namespace
TEST_CASE(
    "Steam binary VDF preserves RiftLift and unknown scalar fields exactly") {
  auto fixture = paths::read(paths::executable().parent_path() /
                             "test-data/steam-shortcuts.vdf");
  auto parsed = read_vdf(fixture);
  CHECK(write_vdf(parsed) == fixture);
  auto &title = member(member(parsed, "shortcuts"), "0");
  CHECK(member(title, "appid").value == std::string("\x67\x45\x23\x81", 4));
  CHECK(member(title, "future_float").type == 3);
  CHECK(member(title, "future_wide").type == 5);
  CHECK(member(title, "future_u64").type == 7);
  Vdf doc = object("", {object("shortcuts", {foreign("0"), foreign("7")}),
                        vstring("unknown-root", "opaque")});
  for (auto type : {2, 4, 6, 9, 10}) {
    Vdf n;
    n.type = type;
    n.key = "wire-" + std::to_string(type);
    n.value = std::string(type == 9 ? 1 : type == 10 ? 8 : 4, '\xff');
    doc.children.push_back(n);
  }
  doc.children[0].children[1].end = 11;
  auto bytes = write_vdf(doc);
  CHECK(read_vdf(bytes) == doc);
  CHECK(write_vdf(read_vdf(bytes)) == bytes);
  CHECK_THROWS(read_vdf(bytes.substr(0, bytes.size() - 1)));
  CHECK_THROWS(read_vdf(bytes + "trailing"));
  CHECK_THROWS(read_vdf(std::string(16 * 1024 * 1024 + 1, 'x')));
  CHECK_THROWS(read_vdf(std::string("\x0c"
                                    "bad\0",
                                    5)));
  CHECK_THROWS(read_vdf(std::string(66 * 2, '\0')));
}
TEST_CASE("Steam sync preserves foreign entries and removes only owned "
          "uninstalled games") {
  Fixture f;
  auto doc = object("", {object("shortcuts", {foreign("0"), foreign("7")}),
                         vstring("unknown-root", "unchanged")});
  auto original = write_vdf(doc);
  paths::atomic_write(f.target, original);
  const auto exe = f.root / "venture.exe";
  CHECK(discover_root() == f.root);
  auto result = sync(discover_root(), exe, f.root / "art",
                     {{"com.example.one", "One", "111111"},
                      {"com.example.two", "Two", "222222"}},
                     [] { return false; });
  CHECK_FALSE(result.pending);
  CHECK(result.games == 2);
  CHECK(paths::read(f.config / "shortcuts.vdf.venture.bak") == original);
  auto first = read_vdf(paths::read(f.target));
  auto entries = member(first, "shortcuts").children;
  REQUIRE(entries.size() == 4);
  CHECK(entries[0] == doc.children[0].children[0]);
  CHECK(entries[1] == doc.children[0].children[1]);
  auto id = member(entries[2], "appid").value;
  CHECK(member(entries[2], "LaunchOptions").value ==
        std::string("--launch com.example.one\0", 25));
  sync(f.root, f.root / "moved/venture.exe", f.root / "art",
       {{"com.example.one", "Renamed", "111111"}}, [] { return false; });
  auto second = read_vdf(paths::read(f.target));
  auto remaining = member(second, "shortcuts").children;
  REQUIRE(remaining.size() == 3);
  CHECK(remaining[0] == entries[0]);
  CHECK(remaining[1] == entries[1]);
  CHECK(member(remaining[2], "appid").value == id);
  CHECK(member(second, "unknown-root") == member(doc, "unknown-root"));
  auto stable = paths::read(f.target),
       backup = paths::read(f.config / "shortcuts.vdf.venture.bak");
  sync(f.root, f.root / "moved/venture.exe", f.root / "art",
       {{"com.example.one", "Renamed", "111111"}}, [] { return false; });
  CHECK(paths::read(f.target) == stable);
  CHECK(paths::read(f.config / "shortcuts.vdf.venture.bak") == backup);
}
TEST_CASE("Steam sync adds artwork once and keeps images set in Steam") {
  Fixture f;
  // A 1x1 PNG: every kind of artwork falls back to it.
  const std::string png("\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00\x1f\x15\xc4\x89\x00\x00\x00\x0d\x49\x44\x41\x54\x78\x9c\x63\xf8\xcf\xc0\xf0\x1f\x00\x05\x00\x01\xff\x89\x99\x3d\x1d\x00\x00\x00\x00\x49\x45\x4e\x44\xae\x42\x60\x82", 70);
  fs::create_directories(f.root / "art");
  paths::atomic_write(f.root / "art" / "111111.png", png);
  paths::atomic_write(f.root / "art" / "222222.png", png);
  const auto exe = f.root / "venture.exe";
  sync(f.root, exe, f.root / "art", {{"com.example.one", "One", "111111"}}, [] { return false; });
  auto entries = member(read_vdf(paths::read(f.target)), "shortcuts").children;
  REQUIRE(entries.size() == 1);
  const auto value = member(entries[0], "appid").value;
  std::uint32_t id = 0;
  for (int i = 3; i >= 0; --i) id = id << 8 | std::uint8_t(value[std::size_t(i)]);
  const auto grid = f.config / "grid";
  const auto portrait = grid / (std::to_string(id) + "p.png"), hero = grid / (std::to_string(id) + "_hero.png");
  REQUIRE(fs::exists(portrait));
  REQUIRE(fs::exists(hero));
  // The user sets a JPEG cover and edits the hero in Steam, then adds a game.
  fs::remove(portrait);
  paths::atomic_write(grid / (std::to_string(id) + "p.jpg"), "custom cover");
  paths::atomic_write(hero, "custom hero");
  sync(f.root, exe, f.root / "art",
       {{"com.example.one", "One", "111111"}, {"com.example.two", "Two", "222222"}}, [] { return false; });
  CHECK_FALSE(fs::exists(portrait));
  CHECK(paths::read(grid / (std::to_string(id) + "p.jpg")) == "custom cover");
  CHECK(paths::read(hero) == "custom hero");
  CHECK(member(read_vdf(paths::read(f.target)), "shortcuts").children.size() == 2);
}
TEST_CASE(
    "Steam sync defers while running and never overwrites corrupt shortcuts") {
  Fixture f;
  paths::atomic_write(f.target, "corrupt");
  auto result = sync(f.root, f.root / "venture.exe", {},
                     {{"com.example.one", "One", ""}}, [] { return true; });
  CHECK(result.pending);
  CHECK(result.games == 1);
  CHECK(paths::read(f.target) == "corrupt");
  CHECK_THROWS(
      sync(f.root, f.root / "venture.exe", {}, {}, [] { return false; }));
  CHECK(paths::read(f.target) == "corrupt");
  CHECK_FALSE(fs::exists(f.config / "shortcuts.vdf.venture.bak"));
  fs::remove(f.target);
  int calls = 0;
  CHECK(sync(f.root, f.root / "venture.exe", {}, {{"com.example.one", "One", ""}}, [&] {
          return ++calls >= 2;
        }).pending);
  CHECK_FALSE(fs::exists(f.target));
  auto original = write_vdf(object("", {object("shortcuts", {foreign("0")})}));
  auto concurrent =
      write_vdf(object("", {object("shortcuts", {foreign("7")})}));
  paths::atomic_write(f.target, original);
  calls = 0;
  CHECK_THROWS(sync(f.root, f.root / "venture.exe", {},
                    {{"com.example.one", "One", ""}}, [&] {
                      if (++calls == 3)
                        paths::atomic_write(f.target, concurrent);
                      return false;
                    }));
  CHECK(paths::read(f.target) == concurrent);
}
TEST_CASE("Steam picks the most recent profile, else the last sign-in") {
  Fixture f;
  fs::create_directories(f.root / "userdata/0/config");
  fs::create_directories(f.root / "userdata/7/config");
  fs::create_directories(f.root / "config");
  auto users = [&](std::string body) {
    paths::atomic_write(f.root / "config/loginusers.vdf", "\"users\"\n{\n" + body + "}\n");
  };
  auto user = [](uint64_t account, std::string fields) {
    return "\"" + std::to_string(76561197960265728ull + account) + "\"\n{\n" + fields + "}\n";
  };
  users(user(42, "\"mostrecent\" \"1\"\n") + user(7, "\"MostRecent\" \"0\"\n"));
  CHECK(account_config(f.root) == f.config);
  users(user(42, "\"Timestamp\" \"200\"\n") + user(7, "\"Timestamp\" \"100\"\n"));
  CHECK(account_config(f.root) == f.config);
  users(user(42, "\"Timestamp\" \"100\"\n") + user(7, "\"Timestamp\" \"100\"\n"));
  CHECK_THROWS(account_config(f.root));
  // Today's client: tab-separated, AutoLogin, a lowercase "timestamp" and no MostRecent.
  users(user(7, "\t\t\"AccountName\"\t\t\"seven\"\n\t\t\"AutoLogin\"\t\t\"0\"\n\t\t\"timestamp\"\t\t\"1793137000\"\n") +
        user(42, "\t\t\"AccountName\"\t\t\"main\"\n\t\t\"AutoLogin\"\t\t\"1\"\n\t\t\"timestamp\"\t\t\"1793137530\"\n"));
  CHECK(account_config(f.root) == f.config);
  // One signed-in account among several profile folders (the others left by old accounts).
  users(user(42, "\"AccountName\" \"main\"\n"));
  CHECK(account_config(f.root) == f.config);
  fs::remove(f.root / "config/loginusers.vdf");
  CHECK_THROWS(account_config(f.root));
  fs::remove_all(f.root / "userdata/7");
  CHECK(account_config(f.root) == f.config);  // userdata/0 is not an account
}
TEST_CASE("Steam's userdata may live elsewhere through a link; links inside it may not") {
  Fixture f;
  const auto persist = f.root.parent_path() / (f.root.filename().string() + "-persist");
  fs::create_directories(persist / "42/config");
  fs::remove_all(f.root / "userdata");
  std::error_code linked;
  fs::create_directory_symlink(persist, f.root / "userdata", linked);
  if (linked) return;  // Windows without symlink rights; Scoop itself uses a junction
  CHECK(account_config(f.root) == f.config);
  fs::remove_all(persist / "42/config");
  fs::create_directories(persist / "elsewhere");
  fs::create_directory_symlink(persist / "elsewhere", persist / "42/config", linked);
  if (!linked) CHECK_THROWS(account_config(f.root));
  fs::remove(f.root / "userdata");
  fs::remove_all(persist);
}
TEST_CASE(
    "Steam discovery refuses a redirected profile without an explicit root") {
  Fixture f;
  CHECK(profile_sync_allowed());
  CHECK(discover_root() == f.root);
  f.steam.set("");
#ifndef _WIN32
  unsetenv("VENTURE_STEAM_ROOT");
#endif
  CHECK_FALSE(profile_sync_allowed());
  CHECK(discover_root().empty()); // No registry/home discovery on this path.
}
TEST_CASE("Steam installation scans cannot prune shortcuts when unavailable or "
          "failed") {
  Fixture f;
  auto original = write_vdf(object(
      "",
      {object("shortcuts",
              {object("0", {vstring("LaunchOptions",
                                    "--launch com.example.installed"),
                            object("tags", {vstring("0", "Venture")})})})}));
  paths::atomic_write(f.target, original);
  auto games = f.root / "games";
  bool reliable = true;
  // A folder on a drive that is not connected: its parent is gone as well.
  CHECK(library::scan(f.root / "unplugged/games", &reliable).empty());
  CHECK_FALSE(reliable);
  // A report: a library folder that was never created (or was deleted) on a
  // present drive kept every later scan from syncing Steam.
  reliable = false;
  CHECK(library::scan(games, &reliable).empty());
  CHECK(reliable);
  paths::atomic_write(games, "not a directory");
  CHECK(library::scan(games, &reliable).empty());
  CHECK_FALSE(reliable);
  fs::remove(games);
  fs::create_directories(games);
  CHECK(library::scan(games, &reliable).empty());
  CHECK(reliable); // A genuinely empty accessible library may remove shortcuts.
  // A report: version folders left without a manifest (an interrupted
  // download, an uninstall that could not remove a file) are not installs
  // and kept the scan from being authoritative across launcher restarts.
  paths::atomic_write(games / "com.example.leftover/974/base.apk", "partial");
  fs::create_directories(games / "com.example.leftover/975");
  CHECK(library::scan(games, &reliable).empty());
  CHECK(reliable);
  fs::remove_all(games / "com.example.leftover");
  paths::atomic_write(games / "com.example.installed/1/plan.json",
                      "malformed manifest");
  CHECK(library::scan(games, &reliable).empty());
  CHECK_FALSE(reliable);
  // An unreadable manifest (a directory at the expected file path) also fails.
  fs::remove(games / "com.example.installed/1/plan.json");
  fs::create_directory(games / "com.example.installed/1/plan.json");
  CHECK(library::scan(games, &reliable).empty());
  CHECK_FALSE(reliable);
  CHECK(paths::read(f.target) == original);
  CHECK_FALSE(fs::exists(f.config / "shortcuts.vdf.venture.bak"));
}
#ifdef VENTURE_TEST_STEAM_ART
#endif
TEST_CASE("Steam sync while Steam runs asks for a restart only when its shortcuts differ") {
  Fixture f;
  const auto exe = f.root / "venture.exe";
  std::vector<Game> games{{"com.example.one", "One", ""}, {"com.example.two", "Two", ""}};
  CHECK(sync(f.root, exe, {}, games, [] { return true; }).pending);
  CHECK_FALSE(sync(f.root, exe, {}, games, [] { return false; }).pending);
  // A report: the banner came back at every start although Steam listed both.
  auto running = sync(f.root, exe, {}, games, [] { return true; });
  CHECK_FALSE(running.pending);
  CHECK(running.games == 2);
  CHECK(running.reason.empty());
  // Each restart prompt says what Steam's shortcuts lack, for the log.
  auto removed = sync(f.root, exe, {}, {games[0]}, [] { return true; });
  CHECK(removed.pending);
  CHECK(removed.reason == "com.example.two is no longer installed but still has a shortcut");
  auto renamed = sync(f.root, exe, {}, {games[0], {"com.example.two", "Renamed", ""}}, [] { return true; });
  CHECK(renamed.pending);
  CHECK(renamed.reason == "Renamed: its shortcut is named Two");
  auto moved = sync(f.root, f.root / "moved/venture.exe", {}, games, [] { return true; });
  CHECK(moved.pending);
  CHECK(moved.reason.starts_with("One: its shortcut starts \""));
  CHECK(moved.reason.find("moved") != std::string::npos);
}
#ifndef _WIN32
TEST_CASE("Steam shortcuts of an AppImage start the image file, not its mount") {
  // A report: every start of the AppImage found its shortcuts stale ("its
  // shortcut starts /tmp/.mount_VenturmOdDkE/...") since each run mounts the
  // image in a new folder, gone once it exits.
  Fixture f;
  Env flatpak{"FLATPAK_ID", ""};
  unsetenv("FLATPAK_ID");
  Env image{"APPIMAGE", f.root / "Applications/Venture-aarch64.AppImage"};
  CHECK(paths::launch_program() == f.root / "Applications/Venture-aarch64.AppImage");
  std::vector<Game> games{{"com.example.one", "One", ""}};
  CHECK_FALSE(sync(f.root, paths::launch_program(), {}, games, [] { return false; }).pending);
  const auto doc = read_vdf(paths::read(f.target));
  CHECK(member(member(member(doc, "shortcuts"), "0"), "exe").value ==
        "\"" + paths::utf8(f.root / "Applications/Venture-aarch64.AppImage") + "\"" + '\0');
  // The next run, from another mount, finds them current while Steam runs.
  CHECK_FALSE(sync(f.root, paths::launch_program(), {}, games, [] { return true; }).pending);
  // A relative value is not the AppImage runtime's; the Flatpak is started by its own command.
  image.set("Venture.AppImage");
  CHECK(paths::launch_program() == paths::executable());
  image.set(paths::utf8(f.root / "Venture.AppImage"));
  flatpak.set("org.venture_emu.Venture");
  CHECK(paths::launch_program() == paths::executable());
}
#endif
TEST_CASE("A Steam restart handed to a helper syncs once Steam closes, then brings Steam back") {
  Fixture f;
  const auto exe = f.root / "Venture.AppImage";
  Handoff handoff{f.root, exe, f.root / "art", {{"com.example.one", "One", "111"}, {"com.example.two", "Two", ""}},
                  {{"WAYLAND_DISPLAY", "gamescope-0"}, {"LD_PRELOAD", "/overlay.so"}}};
  auto copy = read_handoff(write_handoff(handoff));
  CHECK(copy.root == handoff.root);
  CHECK(copy.executable == exe);
  CHECK(copy.art == handoff.art);
  REQUIRE(copy.games.size() == 2);
  CHECK(copy.games[0].package == "com.example.one");
  CHECK(copy.games[0].art_id == "111");
  CHECK(copy.games[1].name == "Two");
  // Only session variables; never Steam's overlay preload.
  REQUIRE(copy.environment.size() == 1);
  CHECK(copy.environment[0] == std::pair<std::string, std::string>{"WAYLAND_DISPLAY", "gamescope-0"});
  CHECK_THROWS(read_handoff("{}"));
  CHECK_THROWS(read_handoff("not json"));
  using namespace std::chrono_literals;
  // Steam closes after a few polls; it does not come back by itself: the helper starts it.
  int polls = 0, starts = 0;
  auto result = finish_handoff(copy, [&] { return ++polls < 4; }, [] { return false; }, [&] { ++starts; }, 5s, 20ms,
                               1ms);
  CHECK(result.synced);
  CHECK(result.started);
  CHECK(starts == 1);
  CHECK_FALSE(sync(f.root, exe, {}, copy.games, [] { return true; }).pending);
  // Steam's session starts it again by itself (SteamOS's game mode): no second start.
  polls = starts = 0;
  result = finish_handoff(copy, [&] { return ++polls == 1; }, [&] { return ++polls > 10; }, [&] { ++starts; }, 5s, 5s,
                          1ms);
  CHECK(result.synced);
  CHECK_FALSE(result.started);
  CHECK(starts == 0);
  // A service manager starts a new Steam as soon as the old one closes
  // (Restart=always): the shortcuts are written once the Steam asked to quit
  // has gone, and nothing starts a second one.
  starts = 0;
  bool asked_open = true;
  int checks = 0;
  result = finish_handoff(copy, [&] { return asked_open && (asked_open = ++checks < 3); }, [] { return true; },
                          [&] { ++starts; }, 5s, 5s, 1ms);
  CHECK(result.synced);
  CHECK_FALSE(result.started);
  // Steam that never closes leaves the shortcuts and Steam alone.
  starts = 0;
  result = finish_handoff(copy, [] { return true; }, [] { return true; }, [&] { ++starts; }, 10ms, 10ms, 1ms);
  CHECK_FALSE(result.synced);
  CHECK_FALSE(result.started);
  CHECK(starts == 0);
  CHECK(result.reason == "Steam did not close");
}
namespace {
// Steam's steam://addnonsteamgame handler, as far as Venture sees it: it lists
// the desktop entry the URL names under an id of its own and saves its
// shortcuts, when the sentinel file exists, and takes the sentinel.
struct FakeSteam {
  fs::path target, sentinel;
  uint32_t next_id = 0x9000'0001;
  std::vector<std::string> urls;
  bool accept = true;
  bool open(const std::string &url) {
    urls.push_back(url);
    if (!accept || !fs::exists(sentinel)) return true;
    fs::remove(sentinel);
    std::string path;
    const std::string prefix = "steam://addnonsteamgame/";
    REQUIRE(url.starts_with(prefix));
    for (size_t i = prefix.size(); i < url.size(); ++i)
      if (url[i] == '%') {
        path += char(std::stoi(url.substr(i + 1, 2), nullptr, 16));
        i += 2;
      } else {
        path += url[i];
      }
    auto doc = fs::exists(target) ? read_vdf(paths::read(target)) : object("", {object("shortcuts", {})});
    auto &list = doc.children[0];
    Vdf id;
    id.type = 2;
    id.key = "appid";
    for (int i = 0; i < 4; ++i) id.value += char(next_id >> (8 * i));
    ++next_id;
    // Steam names it from the entry and keeps the entry's path.
    list.children.push_back(object(std::to_string(list.children.size()),
                                   {id, vstring("AppName", "From entry"), vstring("Exe", "\"/usr/bin/env\""),
                                    vstring("StartDir", "\"/usr/bin\""), vstring("ShortcutPath", path),
                                    vstring("LaunchOptions", ""), object("tags", {})}));
    paths::atomic_write(target, write_vdf(doc));
    return true;
  }
};
uint32_t id_of(const Vdf &entry) {
  const auto &v = member(entry, "appid").value;
  uint32_t n = 0;
  for (int i = 0; i < 4; ++i) n |= uint32_t(uint8_t(v[i])) << (8 * i);
  return n;
}
} // namespace
TEST_CASE("Steam adds games while it runs through its own handler, and a later sync adopts them") {
  Fixture f;
  const auto exe = f.root / "Venture.AppImage";
  FakeSteam steam{f.target, f.root / "addnonsteamgamefile"};
  LiveAdd live;
  live.sentinel = steam.sentinel;
  live.open = [&](const std::string &url) { return steam.open(url); };
  live.steam_running = [] { return true; };
  live.wait = std::chrono::milliseconds(500);
  live.poll = std::chrono::milliseconds(1);
  std::vector<Game> games{{"com.example.one", "One", ""}, {"com.example.two", "Two", ""}};
  // Steam running: both go in at once, and no restart is asked for.
  auto result = sync(f.root, exe, {}, games, [] { return true; }, &live);
  CHECK_FALSE(result.pending);
  CHECK(steam.urls.size() == 2);
  CHECK_FALSE(fs::exists(steam.sentinel));
  const auto entry = f.root / "data" / "steam-shortcuts" / "com.example.one.desktop";
  REQUIRE(fs::exists(entry));
  CHECK(steam.urls[0] == add_game_url(entry));
  const auto text = paths::read(entry);
  CHECK(text.find("Name=One\n") != std::string::npos);
  CHECK(text.find(" --launch \"com.example.one\"\n") != std::string::npos);
  // Each game is handed to Steam once per run.
  CHECK_FALSE(sync(f.root, exe, {}, games, [] { return true; }, &live).pending);
  CHECK(steam.urls.size() == 2);
  // Steam closed: they become ordinary Venture shortcuts under Steam's ids.
  CHECK_FALSE(sync(f.root, exe, {}, games, [] { return false; }, &live).pending);
  auto doc = read_vdf(paths::read(f.target));
  const auto &list = member(doc, "shortcuts");
  REQUIRE(list.children.size() == 2);
  const auto &one = list.children[0];
  CHECK(id_of(one) == 0x9000'0001);
  CHECK(member(one, "appname").value == std::string("One") + '\0');
  CHECK(member(one, "exe").value == "\"" + paths::utf8(fs::absolute(exe)) + "\"" + '\0');
  CHECK(member(one, "LaunchOptions").value == std::string("--launch com.example.one") + '\0');
  CHECK(member(one, "ShortcutPath").value == std::string(1, '\0'));
  CHECK(member(member(one, "tags"), "0").value == std::string("Venture") + '\0');
  for (const auto &n : one.children) CHECK((n.key != "AppName" && n.key != "Exe"));
  CHECK_FALSE(fs::exists(entry));
  CHECK_FALSE(sync(f.root, exe, {}, games, [] { return true; }).pending);
}
TEST_CASE("Steam's live add replaces a shortcut whose program is gone and never starts Steam") {
  Fixture f;
  // An AppImage's mount from an earlier run, gone now.
  CHECK_FALSE(sync(f.root, f.root / "tmp/.mount_old/usr/bin/venture", {}, {{"com.example.one", "One", ""}},
                   [] { return false; })
                  .pending);
  const auto exe = f.root / "Venture.AppImage";
  FakeSteam steam{f.target, f.root / "addnonsteamgamefile"};
  LiveAdd live;
  live.sentinel = steam.sentinel;
  live.open = [&](const std::string &url) { return steam.open(url); };
  bool steam_runs = false;
  live.steam_running = [&] { return steam_runs; };
  live.wait = std::chrono::milliseconds(500);
  live.poll = std::chrono::milliseconds(1);
  std::vector<Game> games{{"com.example.one", "One", ""}};
  // The probe also reports newer work; with Steam closed nothing is handed to it.
  sync(f.root, exe, {}, games, [] { return true; }, &live);
  CHECK(steam.urls.empty());
  steam_runs = true;
  CHECK_FALSE(sync(f.root, exe, {}, games, [] { return true; }, &live).pending);
  REQUIRE(steam.urls.size() == 1);
  CHECK_FALSE(sync(f.root, exe, {}, games, [] { return false; }).pending);
  const auto doc = read_vdf(paths::read(f.target));
  const auto &list = member(doc, "shortcuts");
  REQUIRE(list.children.size() == 1);
  CHECK(id_of(list.children[0]) == 0x9000'0001);
  // A Steam that does not take the file: the game waits for Steam to close.
  Fixture g;
  FakeSteam refusing{g.target, g.root / "addnonsteamgamefile"};
  refusing.accept = false;
  live.tried.clear();
  live.sentinel = refusing.sentinel;
  live.open = [&](const std::string &url) { return refusing.open(url); };
  live.wait = std::chrono::milliseconds(20);
  auto result = sync(g.root, exe, {}, games, [] { return true; }, &live);
  CHECK(result.pending);
  CHECK_FALSE(fs::exists(refusing.sentinel));
}
#ifndef _WIN32
TEST_CASE("Desktop entries for Steam quote their Exec line and URL as the specifications ask") {
  const auto entry = desktop_entry(fs::path("/home/deck/My Games/Venture $1.AppImage"), {"com.example.one", "A\nB", ""});
  CHECK(entry.find("Name=A\\nB\n") != std::string::npos);
  CHECK(entry.find(R"(Exec="/home/deck/My Games/Venture \\$1.AppImage" --launch "com.example.one")") !=
        std::string::npos);
  CHECK(entry.find("Path=/home/deck/My Games\n") != std::string::npos);
  CHECK(add_game_url(fs::path("/home/deck/a b%.desktop")) == "steam://addnonsteamgame/%2Fhome%2Fdeck%2Fa%20b%25.desktop");
}
TEST_CASE("Linux sees Steam running, and a Steam restart waits while Steam runs a game other than SteamVR") {
  CHECK(restart_blocker().empty());
  // Steam's reaper: "reaper SteamLaunch AppId=<id> -- <game>".
  auto start = [](const char *id) {
    pid_t pid = -1;
    std::string app = std::string("AppId=") + id;
    char *argv[] = {const_cast<char *>("/opt/steam/ubuntu12_32/reaper"), const_cast<char *>("-c"),
                    const_cast<char *>("sleep 5; :"), const_cast<char *>("SteamLaunch"), app.data(),
                    const_cast<char *>("--"), nullptr};
    REQUIRE(posix_spawn(&pid, "/bin/sh", nullptr, nullptr, argv, environ) == 0);
    return pid;
  };
  auto stop = [](pid_t pid) {
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
  };
  auto wait_listed = [](pid_t pid) {
    for (int i = 0; i < 200; ++i) {
      if (paths::read(fs::path("/proc") / std::to_string(pid) / "cmdline").find("SteamLaunch") != std::string::npos)
        return;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  };
  // Kernel files report no size: a report found Steam never seen running on
  // Linux, so shortcuts written while it ran were lost when it saved its own.
  CHECK(paths::read("/proc/self/comm").ends_with("\n"));
  {
    const auto dir = paths::executable().parent_path() / "test-artifacts" / "fake-steam";
    fs::create_directories(dir);
    fs::copy_file("/bin/sleep", dir / "steam", fs::copy_options::overwrite_existing);
    pid_t steam = -1;
    std::string program = paths::utf8(dir / "steam");
    char *argv[] = {program.data(), const_cast<char *>("5"), nullptr};
    REQUIRE(posix_spawn(&steam, program.c_str(), nullptr, nullptr, argv, environ) == 0);
    bool seen = false;
    for (int i = 0; i < 200 && !seen; ++i) {
      seen = running();
      if (!seen) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(seen);
    kill(steam, SIGKILL);
    waitpid(steam, nullptr, 0);
  }
  const auto shell = start("250820");  // SteamVR, the headset's shell
  wait_listed(shell);
  CHECK(restart_blocker().empty());
  const auto game = start("620980");
  wait_listed(game);
  CHECK(restart_blocker().find("620980") != std::string::npos);
  stop(game);
  stop(shell);
  // A child the shell forked just before the kill shows the reaper's command line until it execs.
  bool cleared = false;
  for (int i = 0; i < 200 && !cleared; ++i) {
    cleared = restart_blocker().empty();
    if (!cleared) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(cleared);
}
#endif
TEST_CASE("gamescope's Wayland display stands in for an unset WAYLAND_DISPLAY when its socket exists") {
  CHECK(environment::gamescope_wayland_display("wayland-0", "gamescope-0", "/run/user/1000").empty());
  CHECK(environment::gamescope_wayland_display(nullptr, nullptr, "/run/user/1000").empty());
  CHECK(environment::gamescope_wayland_display(nullptr, "", "/run/user/1000").empty());
  CHECK(environment::gamescope_wayland_display(nullptr, "gamescope-0", nullptr).empty());
  Fixture f;
  // No socket there: nothing to connect to.
  CHECK(environment::gamescope_wayland_display("", "gamescope-0", paths::utf8(f.root).c_str()).empty());
#ifndef _WIN32
  // A short folder: sun_path holds 108 bytes.
  const auto dir = fs::temp_directory_path() / ("vgs-" + std::to_string(getpid()));
  fs::create_directories(dir);
  const auto socket_path = dir / "gamescope-0";
  fs::remove(socket_path);
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path.c_str());
  REQUIRE(bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
  CHECK(environment::gamescope_wayland_display(nullptr, "gamescope-0", dir.c_str()) == "gamescope-0");
  CHECK(environment::gamescope_wayland_display("", "gamescope-0", dir.c_str()) == "gamescope-0");
  CHECK(environment::gamescope_wayland_display(nullptr, socket_path.c_str(), nullptr) == socket_path.string());
  // A plain file of that name is not a display.
  close(fd);
  fs::remove(socket_path);
  paths::atomic_write(socket_path, "");
  CHECK(environment::gamescope_wayland_display(nullptr, "gamescope-0", dir.c_str()).empty());
  fs::remove_all(dir);
#endif
}
TEST_CASE("a game started from a Venture that Steam started is not taken for Venture by Steam and SteamVR") {
  std::map<std::string, std::string> vars{
      {"SteamGameId", "13609473281357643776"}, {"SteamAppId", "0"}, {"SteamOverlayGameId", "13609473281357643776"},
      {"STEAM_COMPAT_APP_ID", "0"}, {"SteamClientLaunch", "1"},
      {"LD_PRELOAD", "/home/u/.steam/ubuntu12_32/gameoverlayrenderer.so:/home/u/.steam/ubuntu12_64/gameoverlayrenderer.so"}};
  auto get = [&](const char *name) -> const char * {
    auto it = vars.find(name);
    return it == vars.end() ? nullptr : it->second.c_str();
  };
  auto identity = launch_identity(get);
  CHECK(identity.removed ==
        std::vector<std::string>{"SteamGameId", "SteamAppId", "SteamOverlayGameId", "STEAM_COMPAT_APP_ID", "LD_PRELOAD"});
  CHECK(identity.replaced.empty());
  // A preload of the player's own stays, in its order.
  vars["LD_PRELOAD"] = "libmine.so /x/ubuntu12_64/gameoverlayrenderer.so:/opt/libother.so";
  identity = launch_identity(get);
  REQUIRE(identity.replaced.size() == 1);
  CHECK(identity.replaced[0] == std::pair<std::string, std::string>{"LD_PRELOAD", "libmine.so:/opt/libother.so"});
  // Started outside Steam: nothing to change.
  vars = {{"LD_PRELOAD", "libmine.so"}, {"HOME", "/home/u"}};
  identity = launch_identity(get);
  CHECK(identity.removed.empty());
  CHECK(identity.replaced.empty());
}
TEST_CASE("steam.sh starts with the session's environment, not the one Steam gave Venture") {
  const std::vector<std::string> started_by_steam{
      "HOME=/home/deck", "DISPLAY=:0", "PATH=/steam/runtime/bin:/usr/bin", "SYSTEM_PATH=/usr/bin:/bin",
      "LD_LIBRARY_PATH=/steam/runtime/lib", "SYSTEM_LD_LIBRARY_PATH=", "LD_PRELOAD=/x/gameoverlayrenderer.so",
      "STEAM_RUNTIME=/steam/runtime", "STEAM_RUNTIME_LIBRARY_PATH=/steam/runtime/lib", "SteamGameId=123",
      "SteamAppId=0", "PRESSURE_VESSEL_RUNTIME=scout", "STEAM_COMPAT_CLIENT_INSTALL_PATH=/steam", "XDG_RUNTIME_DIR=/run/user/1000"};
  CHECK(client_environment(started_by_steam) ==
        std::vector<std::string>{"HOME=/home/deck", "DISPLAY=:0", "XDG_RUNTIME_DIR=/run/user/1000", "PATH=/usr/bin:/bin"});
  // Started outside Steam: its own PATH and library path stay.
  CHECK(client_environment({"HOME=/h", "PATH=/usr/bin", "LD_LIBRARY_PATH=/opt/lib"}) ==
        std::vector<std::string>{"HOME=/h", "PATH=/usr/bin"});
}
TEST_CASE("a child does not inherit the variables its spawn removes") {
  Env game{"SteamGameId", fs::path("13609473281357643776")}, kept{"VENTURE_TEST_KEPT", fs::path("kept")};
  const auto log = fs::temp_directory_path() / ("venture-env-child-" + std::to_string(current_process_id()) + ".log");
  Process child;
  child.spawn(VENTURE_ENVIRONMENT_CHILD, {"SteamGameId", "VENTURE_TEST_KEPT", "VENTURE_TEST_SET"}, log, false, nullptr,
              {{"VENTURE_TEST_SET", "set"}}, {"SteamGameId"});
  REQUIRE(child.wait(30000).has_value());
  const auto output = paths::read(log, 4096);
  CHECK(output.find("SteamGameId unset") != std::string::npos);
  CHECK(output.find("VENTURE_TEST_KEPT=kept") != std::string::npos);
  CHECK(output.find("VENTURE_TEST_SET=set") != std::string::npos);
  std::error_code ec;
  fs::remove(log, ec);
}
