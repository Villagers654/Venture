#include "common/download/manager.h"
#include "common/file_ops.h"
#include "common/library/library.h"
#include "common/steam_library.h"
#include "launcher/options.h"
#include <chrono>
#include <cstdlib>
#include <doctest/doctest.h>
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
  CHECK(sync(f.root, f.root / "venture.exe", {}, {}, [&] {
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
  CHECK(library::scan(games, &reliable).empty());
  CHECK_FALSE(reliable);
  paths::atomic_write(games, "not a directory");
  CHECK(library::scan(games, &reliable).empty());
  CHECK_FALSE(reliable);
  fs::remove(games);
  fs::create_directories(games);
  CHECK(library::scan(games, &reliable).empty());
  CHECK(reliable); // A genuinely empty accessible library may remove shortcuts.
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
