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
