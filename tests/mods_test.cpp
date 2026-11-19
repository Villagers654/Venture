#include "common/library/mods.h"

#include "common/file_ops.h"

#include <doctest/doctest.h>
#include <miniz.h>

#include <chrono>
#include <fstream>

using namespace venture;
namespace {
struct Directory {
  fs::path root = fs::temp_directory_path() /
                  ("venture-mods-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Directory() { fs::create_directories(root); }
  ~Directory() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};
void zip(const fs::path &file, const std::vector<std::pair<std::string, std::string>> &members) {
  fs::create_directories(file.parent_path());
  mz_zip_archive z{};
  REQUIRE(mz_zip_writer_init_file(&z, paths::utf8(file).c_str(), 0));
  for (auto &[name, content] : members)
    REQUIRE(mz_zip_writer_add_mem(&z, name.c_str(), content.data(), content.size(), MZ_DEFAULT_COMPRESSION));
  REQUIRE(mz_zip_writer_finalize_archive(&z));
  REQUIRE(mz_zip_writer_end(&z));
}
std::string read(const fs::path &file) {
  std::ifstream in(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}
void write(const fs::path &file, const std::string &content) {
  fs::create_directories(file.parent_path());
  std::ofstream(file, std::ios::binary) << content;
}
constexpr const char *kGame = "com.example.game";
// A .qmod holding `manifest` as mod.json and the given files.
fs::path qmod(const fs::path &dir, const std::string &name, Json manifest,
              std::vector<std::pair<std::string, std::string>> files = {}) {
  files.emplace_back("mod.json", manifest.dump());
  const auto file = dir / (name + ".qmod");
  zip(file, files);
  return file;
}
Json sl2_mod(const std::string &id, const std::string &version, Json dependencies = Json::array(), bool library = false) {
  return Json{{"_QPVersion", "1.2.0"}, {"id", id},           {"name", id + " mod"},
              {"author", "someone"},   {"version", version}, {"packageId", kGame},
              {"modloader", "Scotland2"}, {"isLibrary", library},
              {"modFiles", Json::array()}, {"lateModFiles", Json::array({"lib" + id + ".so"})},
              {"libraryFiles", Json::array()}, {"dependencies", dependencies}};
}
}  // namespace

TEST_CASE("a qmod manifest gives its loader, files, copies, file types and dependencies") {
  const auto m = mods::parse_manifest(R"({
    "_QPVersion": "1.1.0", "id": "qosmetics-sabers", "name": "Qosmetics Sabers", "author": "RedBrumbler",
    "porter": "someone", "version": "1.4.2", "packageId": "com.beatgames.beatsaber", "packageVersion": "1.37.0_9064817954",
    "description": "Custom sabers", "coverImage": "cover.png", "modloader": "Scotland2", "isLibrary": false,
    "modFiles": ["libearly.so"], "lateModFiles": ["libsabers.so"], "libraryFiles": ["libs/libhelper.so"],
    "fileCopies": [{"name": "default.qsaber", "destination": "/sdcard/ModData/com.beatgames.beatsaber/Mods/Qosmetics/Sabers/default.qsaber"}],
    "copyExtensions": [{"extension": ".QSABER", "destination": "/sdcard/ModData/com.beatgames.beatsaber/Mods/Qosmetics/Sabers/"}],
    "dependencies": [{"id": "beatsaber-hook", "version": "^5.1.0", "downloadIfMissing": "https://example.com/bshook.qmod"},
                     {"id": "optional-thing", "version": "*", "required": false}]
  })");
  CHECK(m.id == "qosmetics-sabers");
  CHECK(m.name == "Qosmetics Sabers");
  CHECK(m.version == "1.4.2");
  CHECK(m.loader == mods::Loader::scotland2);
  CHECK(m.package == "com.beatgames.beatsaber");
  CHECK(m.package_version == "1.37.0_9064817954");
  CHECK(m.cover == "cover.png");
  CHECK(m.mod_files == std::vector<std::string>{"libearly.so"});
  CHECK(m.late_mod_files == std::vector<std::string>{"libsabers.so"});
  CHECK(m.library_files == std::vector<std::string>{"libs/libhelper.so"});
  REQUIRE(m.copies.size() == 1);
  CHECK(m.copies[0].name == "default.qsaber");
  REQUIRE(m.extensions.size() == 1);
  CHECK(m.extensions[0].extension == "qsaber");
  REQUIRE(m.dependencies.size() == 2);
  CHECK(m.dependencies[0].id == "beatsaber-hook");
  CHECK(m.dependencies[0].range == "^5.1.0");
  CHECK(m.dependencies[0].download == "https://example.com/bshook.qmod");
  CHECK(m.dependencies[0].required);
  CHECK_FALSE(m.dependencies[1].required);
  // QuestLoader is the format's default loader; a name is optional.
  const auto old = mods::parse_manifest(R"({"id": "old", "version": "0.1.0", "modFiles": ["libold.so"]})");
  CHECK(old.loader == mods::Loader::questloader);
  CHECK(old.name == "old");
  CHECK(old.package.empty());
  CHECK_THROWS(mods::parse_manifest("not json"));
  CHECK_THROWS(mods::parse_manifest(R"({"version": "1.0.0"})"));
  CHECK_THROWS(mods::parse_manifest(R"({"id": "../up", "version": "1.0.0"})"));
  CHECK_THROWS(mods::parse_manifest(R"({"id": "a", "version": "one"})"));
  CHECK_THROWS(mods::parse_manifest(R"({"id": "a", "version": "1.0.0", "modloader": "Lemon"})"));
  CHECK_THROWS(mods::parse_manifest(R"({"id": "a", "version": "1.0.0", "modFiles": "lib.so"})"));
}

TEST_CASE("dependency version ranges follow node-semver") {
  struct Case {
    const char *version, *range;
    bool ok;
  };
  for (auto [version, range, ok] : std::initializer_list<Case>{
           {"1.2.3", "*", true},           {"1.2.3", "", true},             {"1.2.3", "1.2.3", true},
           {"1.2.4", "1.2.3", false},      {"1.2.3", "=1.2.3", true},       {"1.9.0", "^1.2.3", true},
           {"2.0.0", "^1.2.3", false},     {"1.2.2", "^1.2.3", false},      {"0.2.9", "^0.2.3", true},
           {"0.3.0", "^0.2.3", false},     {"0.0.3", "^0.0.3", true},       {"0.0.4", "^0.0.3", false},
           {"1.2.9", "~1.2.3", true},      {"1.3.0", "~1.2.3", false},      {"1.9.0", "~1", true},
           {"1.0.0", ">=1.0.0 <2.0.0", true}, {"2.0.0", ">=1.0.0 <2.0.0", false}, {"1.5.0", ">= 1.0.0", true},
           {"1.4.0", "1.0.0 - 1.4.0", true}, {"1.4.1", "1.0.0 - 1.4.0", false}, {"1.4.9", "1.0 - 1.4", true},
           {"1.5.0", "1.x", true},         {"2.0.0", "1.x", false},         {"1.2.7", "1.2", true},
           {"3.0.0", "^1.0.0 || ^3.0.0", true}, {"2.0.0", "^1.0.0 || ^3.0.0", false},
           {"1.3.0", ">1.2", true},        {"1.2.9", ">1.2", false},        {"1.2.9", "<=1.2", true},
           {"1.3.0", "<=1.2", false},      {"1.1.9", "<1.2", true},          {"2.0.0-beta", "^1.0.0", false},
           {"1.0.0-beta", "<1.0.0", true}, {"1.0.0-beta.2", ">1.0.0-beta.1", true}, {"1.0.0", "garbage", false},
           {"v1.2.3", "^1.0.0", true},     {"1.2", "*", false}}) {
    CAPTURE(version);
    CAPTURE(range);
    CHECK(mods::satisfies(version, range) == ok);
  }
}

TEST_CASE("guest storage paths map into a game's app data") {
  CHECK(mods::storage_path(kGame, "/sdcard/ModData/com.example.game/Mods/a.txt") ==
        fs::path("external") / "ModData" / kGame / "Mods" / "a.txt");
  CHECK(mods::storage_path(kGame, "/storage/emulated/0/Playlists//x.bplist") ==
        fs::path("external") / "Playlists" / "x.bplist");
  CHECK(mods::storage_path(kGame, "/sdcard/Android/data/com.example.game/files/mods/libx.so") ==
        fs::path("external-data") / "files" / "mods" / "libx.so");
  CHECK(mods::storage_path(kGame, "/sdcard/Android/data/com.other/files/x") ==
        fs::path("external") / "Android" / "data" / "com.other" / "files" / "x");
  CHECK(mods::storage_path(kGame, "/sdcard/Android/obb/com.example.game/main.obb").empty());
  CHECK(mods::storage_path(kGame, "/sdcard/../data/x").empty());
  CHECK(mods::storage_path(kGame, "/data/data/com.example.game/files/x").empty());
  CHECK(mods::storage_path(kGame, "sdcard/x").empty());
  CHECK(mods::storage_path(kGame, "/sdcard").empty());
}

TEST_CASE("a qmod installs into the mods folder, replacing another version, and checks what it lists") {
  Directory d;
  const auto mods = d.root / "mods";
  auto manifest = sl2_mod("songloader", "1.0.0");
  manifest["fileCopies"] = Json::array({{{"name", "assets/cover.png"}, {"destination", "/sdcard/ModData/com.example.game/Mods/SongLoader/"}}});
  const auto first = mods::install(mods, kGame, qmod(d.root, "a", manifest, {{"libsongloader.so", "v1"}, {"assets/cover.png", "png"}}));
  CHECK(first.manifest.id == "songloader");
  CHECK(first.enabled);
  CHECK_FALSE(first.dependency);
  CHECK(read(first.directory / "libsongloader.so") == "v1");
  CHECK(read(first.directory / "assets" / "cover.png") == "png");
  manifest["version"] = "1.1.0";
  mods::install(mods, kGame, qmod(d.root, "b", manifest, {{"libsongloader.so", "v2"}, {"assets/cover.png", "png"}}));
  auto all = mods::list(mods);
  REQUIRE(all.size() == 1);
  CHECK(all[0].manifest.version == "1.1.0");
  CHECK(read(all[0].directory / "libsongloader.so") == "v2");
  // Never another game's mod, a file the package lacks, or an unsafe member.
  auto other = sl2_mod("other", "1.0.0");
  other["packageId"] = "com.other.game";
  CHECK_THROWS(mods::install(mods, kGame, qmod(d.root, "c", other, {{"libother.so", "x"}})));
  CHECK_THROWS(mods::install(mods, kGame, qmod(d.root, "d", sl2_mod("lacking", "1.0.0"))));
  CHECK_THROWS(mods::install(mods, kGame, qmod(d.root, "e", sl2_mod("unsafe", "1.0.0"), {{"libunsafe.so", "x"}, {"../escape", "x"}})));
  auto outside = sl2_mod("outside", "1.0.0");
  outside["fileCopies"] = Json::array({{{"name", "liboutside.so"}, {"destination", "/data/local/tmp/x.so"}}});
  CHECK_THROWS(mods::install(mods, kGame, qmod(d.root, "f", outside, {{"liboutside.so", "x"}})));
  write(d.root / "plain.zip", "not a zip");
  CHECK_THROWS(mods::install(mods, kGame, d.root / "plain.zip"));
  CHECK(mods::list(mods).size() == 1);
  // A mod for any game (no packageId) installs.
  auto any = sl2_mod("any", "1.0.0");
  any.erase("packageId");
  CHECK_NOTHROW(mods::install(mods, kGame, qmod(d.root, "g", any, {{"libany.so", "x"}})));
}

TEST_CASE("mods turn on with what they need, off with what needs them, and libraries go with their last user") {
  Directory d;
  const auto mods = d.root / "mods";
  mods::install(mods, kGame, qmod(d.root, "hook", sl2_mod("hook", "5.1.0", Json::array(), true), {{"libhook.so", "h"}}), true);
  mods::install(mods, kGame, qmod(d.root, "ui", sl2_mod("ui", "1.0.0", Json::array({{{"id", "hook"}, {"version", "^5.0.0"}}}), true), {{"libui.so", "u"}}), true);
  auto deps = Json::array({{{"id", "hook"}, {"version", "^5.0.0"}}, {{"id", "ui"}, {"version", "^1.0.0"}}});
  mods::install(mods, kGame, qmod(d.root, "sabers", sl2_mod("sabers", "1.0.0", deps), {{"libsabers.so", "s"}}));
  auto enabled = [&](std::string id) {
    for (auto &m : mods::list(mods))
      if (m.manifest.id == id) return m.enabled;
    FAIL("not installed");
    return false;
  };
  mods::set_enabled(mods, "hook", false);
  CHECK_FALSE(enabled("hook"));
  CHECK_FALSE(enabled("ui"));
  CHECK_FALSE(enabled("sabers"));
  mods::set_enabled(mods, "sabers", true);
  CHECK(enabled("hook"));
  CHECK(enabled("ui"));
  CHECK(enabled("sabers"));
  CHECK(mods::missing_dependencies(mods::list(mods), mods::list(mods)[0].manifest).empty());
  auto wants_newer = sl2_mod("x", "1.0.0", Json::array({{{"id", "hook"}, {"version", "^6.0.0"}},
                                                         {{"id", "absent"}, {"version", "*"}},
                                                         {{"id", "optional"}, {"version", "*"}, {"required", false}}}));
  auto missing = mods::missing_dependencies(mods::list(mods), mods::parse_manifest(wants_newer.dump()));
  REQUIRE(missing.size() == 2);
  CHECK(missing[0].id == "hook");
  CHECK(missing[1].id == "absent");
  mods::remove(mods, "sabers");
  CHECK(mods::list(mods).empty());
  CHECK_THROWS(mods::remove(mods, "sabers"));
  // A library the player installed stays.
  mods::install(mods, kGame, qmod(d.root, "hook", sl2_mod("hook", "5.1.0", Json::array(), true), {{"libhook.so", "h"}}));
  mods::install(mods, kGame, qmod(d.root, "sabers", sl2_mod("sabers", "1.0.0", Json::array({{{"id", "hook"}}})), {{"libsabers.so", "s"}}));
  mods::remove(mods, "sabers");
  CHECK(mods::list(mods).size() == 1);
}

TEST_CASE("installing a mod fetches the dependencies its manifest gives a download for") {
  Directory d;
  const auto mods = d.root / "mods", served = d.root / "served";
  const auto hook = qmod(served, "hook", sl2_mod("hook", "5.2.0", Json::array({{{"id", "core"}, {"version", "^1.0.0"}, {"downloadIfMissing", "https://example.com/core.qmod"}}}), true), {{"libhook.so", "h"}});
  const auto core = qmod(served, "core", sl2_mod("core", "1.0.1", Json::array(), true), {{"libcore.so", "c"}});
  const auto wrong = qmod(served, "wrong", sl2_mod("impostor", "1.0.0"), {{"libimpostor.so", "i"}});
  std::vector<std::string> fetched;
  mods::Fetch fetch = [&](const std::string &url, const fs::path &file, std::stop_token) {
    fetched.push_back(url);
    if (url.ends_with("/hook.qmod")) fs::copy_file(hook, file);
    else if (url.ends_with("/core.qmod")) fs::copy_file(core, file);
    else if (url.ends_with("/wrong.qmod")) fs::copy_file(wrong, file);
    else throw std::runtime_error("404");
  };
  auto deps = Json::array({{{"id", "hook"}, {"version", "^5.0.0"}, {"downloadIfMissing", "https://example.com/hook.qmod"}},
                           {{"id", "gone"}, {"version", "^1.0.0"}, {"downloadIfMissing", "https://example.com/gone.qmod"}},
                           {{"id", "named"}, {"version", "*"}, {"downloadIfMissing", "https://example.com/wrong.qmod"}},
                           {{"id", "nowhere"}, {"version", "^2.0.0"}}});
  auto report = mods::install_with_dependencies(mods, kGame, qmod(d.root, "sabers", sl2_mod("sabers", "1.0.0", deps), {{"libsabers.so", "s"}}), fetch);
  REQUIRE(report.installed.size() == 3);
  CHECK(report.installed[0].manifest.id == "sabers");
  CHECK_FALSE(report.installed[0].dependency);
  CHECK(report.installed[1].dependency);
  std::vector<std::string> ids;
  for (auto &m : mods::list(mods)) ids.push_back(m.manifest.id);
  CHECK(ids == std::vector<std::string>{"core", "hook", "sabers"});
  CHECK(report.missing == std::vector<std::string>{"gone ^1.0.0", "named *", "nowhere ^2.0.0"});
  CHECK(fetched.size() == 4);
  // Without fetching, an installed dependency that fits is turned on.
  mods::set_enabled(mods, "hook", false);
  auto again = mods::install_with_dependencies(mods, kGame, qmod(d.root, "sabers2", sl2_mod("sabers", "1.0.1", Json::array({{{"id", "hook"}, {"version", "^5.0.0"}}})), {{"libsabers.so", "s2"}}), nullptr);
  CHECK(again.missing.empty());
  for (auto &m : mods::list(mods)) CHECK(m.enabled);
}

TEST_CASE("a dependency two mods link at different versions gets one that satisfies both") {
  Directory d;
  const auto mods = d.root / "mods", served = d.root / "served";
  auto link = [](const std::string &v) { return "https://example.com/v" + v + "/hook.qmod"; };
  std::map<std::string, fs::path> files;
  for (const auto *v : {"5.0.0", "5.2.0", "6.0.0"})
    files[link(v)] = qmod(served / v, "hook", sl2_mod("hook", v, Json::array(), true), {{"libhook.so", v}});
  // Like Beat Saber's core mods: most link an older hook than one of them requires.
  files["https://example.com/a.qmod"] = qmod(served, "a", sl2_mod("a", "1.0.0", Json::array({{{"id", "hook"}, {"version", "^5.0.0"}, {"downloadIfMissing", link("5.0.0")}}}), true), {{"liba.so", "a"}});
  files["https://example.com/b.qmod"] = qmod(served, "b", sl2_mod("b", "1.0.0", Json::array({{{"id", "hook"}, {"version", "^5.2.0"}, {"downloadIfMissing", link("5.2.0")}}}), true), {{"libb.so", "b"}});
  std::vector<std::string> fetched;
  mods::Fetch fetch = [&](const std::string &url, const fs::path &file, std::stop_token) {
    fetched.push_back(url);
    if (!files.contains(url)) throw std::runtime_error("404");
    fs::copy_file(files[url], file);
  };
  auto version_of = [&](const std::string &id) {
    for (auto &m : mods::list(mods))
      if (m.manifest.id == id) return m.manifest.version;
    return std::string();
  };
  auto deps = Json::array({{{"id", "a"}, {"downloadIfMissing", "https://example.com/a.qmod"}},
                           {{"id", "b"}, {"downloadIfMissing", "https://example.com/b.qmod"}}});
  auto report = mods::install_with_dependencies(mods, kGame, qmod(d.root, "top", sl2_mod("top", "1.0.0", deps), {{"libtop.so", "t"}}), fetch);
  CHECK(report.missing.empty());
  CHECK(version_of("hook") == "5.2.0");
  CHECK(std::ranges::count(fetched, link("5.2.0")) == 1);
  // An installed older version is replaced when a new mod requires more, and kept as a dependency.
  Directory e;
  const auto other = e.root / "mods";
  mods::install(other, kGame, files[link("5.0.0")], true);
  auto wants = Json::array({{{"id", "hook"}, {"version", "^5.2.0"}, {"downloadIfMissing", link("5.2.0")}}});
  CHECK(mods::install_with_dependencies(other, kGame, qmod(e.root, "new", sl2_mod("new", "1.0.0", wants), {{"libnew.so", "n"}}), fetch).missing.empty());
  for (auto &m : mods::list(other))
    if (m.manifest.id == "hook") CHECK((m.manifest.version == "5.2.0" && m.dependency));
  // No link satisfying every range: reported with all of them, nothing replaced.
  auto conflict = Json::array({{{"id", "hook"}, {"version", "^6.0.0"}, {"downloadIfMissing", link("6.0.0")}}});
  auto clash = mods::install_with_dependencies(other, kGame, qmod(e.root, "clash", sl2_mod("clash", "1.0.0", conflict), {{"libclash.so", "c"}}), fetch);
  CHECK(clash.missing == std::vector<std::string>{"hook ^6.0.0 ^5.2.0"});
  for (auto &m : mods::list(other))
    if (m.manifest.id == "hook") CHECK(m.manifest.version == "5.2.0");
}

TEST_CASE("a launch publishes enabled mods where their loader reads them and takes back the rest") {
  Directory d;
  const auto mods = d.root / "mods", data = d.root / "users" / "local" / kGame;
  auto sabers = sl2_mod("sabers", "1.0.0");
  sabers["modFiles"] = Json::array({"libearly.so"});
  sabers["libraryFiles"] = Json::array({"libs/libhelper.so"});
  sabers["fileCopies"] = Json::array({{{"name", "default.qsaber"}, {"destination", "/sdcard/ModData/com.example.game/Mods/Sabers/default.qsaber"}},
                                      {{"name", "config.json"}, {"destination", "/sdcard/ModData/com.example.game/Configs/"}}});
  sabers["copyExtensions"] = Json::array({{{"extension", "qsaber"}, {"destination", "/sdcard/ModData/com.example.game/Mods/Sabers/"}}});
  mods::install(mods, kGame, qmod(d.root, "sabers", sabers, {{"libearly.so", "early"}, {"libsabers.so", "late"}, {"libs/libhelper.so", "lib"},
                                                             {"default.qsaber", "saber"}, {"config.json", "{}"}}));
  Json old{{"id", "old"}, {"version", "1.0.0"}, {"modFiles", Json::array({"libold.so"})}, {"libraryFiles", Json::array({"libbase.so"})}};
  mods::install(mods, kGame, qmod(d.root, "old", old, {{"libold.so", "o"}, {"libbase.so", "b"}}));
  write(d.root / "Red.qsaber", "red");
  CHECK(mods::takes_item(mods, d.root / "Red.qsaber"));
  CHECK_FALSE(mods::takes_item(mods, d.root / "x.bplist"));
  mods::add_item(mods, d.root / "Red.qsaber");
  CHECK_THROWS(mods::add_item(mods, d.root / "x.bplist"));
  write(d.root / "libsl2.so", "\177ELF loader");
  mods::set_loader_file(mods, d.root / "libsl2.so");
  write(d.root / "notelf.so", "text");
  CHECK_THROWS(mods::set_loader_file(mods, d.root / "notelf.so"));

  auto result = mods::sync(mods, kGame, data);
  const auto modloader = data / "external" / "ModData" / kGame / "Modloader";
  CHECK(read(modloader / "early_mods" / "libearly.so") == "early");
  CHECK(read(modloader / "mods" / "libsabers.so") == "late");
  CHECK(read(modloader / "libs" / "libhelper.so") == "lib");
  CHECK(read(modloader / "libsl2.so") == "\177ELF loader");
  const auto sabers_dir = data / "external" / "ModData" / kGame / "Mods" / "Sabers";
  CHECK(read(sabers_dir / "default.qsaber") == "saber");
  CHECK(read(sabers_dir / "Red.qsaber") == "red");
  CHECK(read(data / "external" / "ModData" / kGame / "Configs" / "config.json") == "{}");
  CHECK(read(data / "external-data" / "files" / "mods" / "libold.so") == "o");
  CHECK(read(data / "external-data" / "files" / "libs" / "libbase.so") == "b");
  CHECK(result.published == 9);
  // Nothing changed: nothing is published again.
  CHECK(mods::sync(mods, kGame, data).published == 0);
  // The game rewrote its config: it stays the game's, also once the mod is off.
  write(data / "external" / "ModData" / kGame / "Configs" / "config.json", R"({"saber":"Red"})");
  mods::set_enabled(mods, "sabers", false);
  result = mods::sync(mods, kGame, data);
  CHECK_FALSE(fs::exists(modloader / "mods" / "libsabers.so"));
  CHECK_FALSE(fs::exists(modloader / "early_mods" / "libearly.so"));
  CHECK_FALSE(fs::exists(sabers_dir / "Red.qsaber"));
  CHECK(read(data / "external" / "ModData" / kGame / "Configs" / "config.json") == R"({"saber":"Red"})");
  CHECK(fs::exists(modloader / "libsl2.so"));
  CHECK(fs::exists(data / "external-data" / "files" / "mods" / "libold.so"));
  CHECK(result.kept == 1);
  // Back on, an updated mod replaces its libraries but not the game's config.
  mods::set_enabled(mods, "sabers", true);
  sabers["version"] = "1.1.0";
  mods::install(mods, kGame, qmod(d.root, "sabers2", sabers, {{"libearly.so", "early2"}, {"libsabers.so", "late2"}, {"libs/libhelper.so", "lib"},
                                                              {"default.qsaber", "saber"}, {"config.json", "{}"}}));
  mods::sync(mods, kGame, data);
  CHECK(read(modloader / "mods" / "libsabers.so") == "late2");
  CHECK(read(modloader / "early_mods" / "libearly.so") == "early2");
  CHECK(read(sabers_dir / "Red.qsaber") == "red");
  CHECK(read(data / "external" / "ModData" / kGame / "Configs" / "config.json") == R"({"saber":"Red"})");
  // Removing everything takes back what was published.
  mods::remove_item(mods, "qsaber", "Red.qsaber");
  mods::remove(mods, "sabers");
  mods::remove(mods, "old");
  mods::sync(mods, kGame, data);
  CHECK_FALSE(fs::exists(sabers_dir / "Red.qsaber"));
  CHECK_FALSE(fs::exists(data / "external-data" / "files" / "mods" / "libold.so"));
  CHECK_FALSE(fs::exists(modloader / "libs" / "libhelper.so"));
  // A game with no mods folder at all syncs to nothing.
  CHECK(mods::sync(d.root / "none", kGame, d.root / "users" / "local" / "com.other").published == 0);
}

TEST_CASE("a patched APK names its mod loader in its patcher's tag") {
  Directory d;
  zip(d.root / "sl2.apk", {{"AndroidManifest.xml", "x"}, {"modded.json", R"({"patcherName":"QuestPatcher","modloaderName":"Scotland2"})"}});
  zip(d.root / "ql.apk", {{"AndroidManifest.xml", "x"}, {"modded.json", R"({"modloaderName":"QuestLoader"})"}});
  zip(d.root / "bmbf.apk", {{"AndroidManifest.xml", "x"}, {"BMBF.modded", ""}});
  zip(d.root / "store.apk", {{"AndroidManifest.xml", "x"}});
  CHECK(mods::apk_loader(d.root / "sl2.apk") == mods::Loader::scotland2);
  CHECK(mods::apk_loader(d.root / "ql.apk") == mods::Loader::questloader);
  CHECK(mods::apk_loader(d.root / "bmbf.apk") == mods::Loader::questloader);
  CHECK_FALSE(mods::apk_loader(d.root / "store.apk").has_value());
}
