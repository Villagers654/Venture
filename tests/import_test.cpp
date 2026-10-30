// Game import: APK manifests, adb output, source discovery, the import plan
// against Meta's catalog, and a whole import through the download queue.
#include "common/android/adb.h"
#include "common/android/archive.h"
#include "common/android/manifest.h"
#include "common/digest.h"
#include "common/download/manager.h"
#include "common/download/unity_bundle.h"
#include "common/library/import.h"
#include "common/library/library.h"
#include "common/process.h"
#include "common/saves_migration.h"

#include <doctest/doctest.h>
#include <miniz.h>

#include <atomic>
#include <fstream>

#include "async_wait.h"

using namespace venture;
namespace {
struct Directory {
  fs::path root = fs::temp_directory_path() /
                  ("venture-import-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Directory() { fs::create_directories(root); }
  ~Directory() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

// A compiled AndroidManifest.xml: <manifest package versionCode versionName>
// <application label/></manifest>. With `named` false the attribute names
// are empty and only the resource map identifies them, as in shrunk APKs.
// features: <uses-feature> names with android:required as 1 (true), 0 (false) or -1 (absent).
std::vector<std::uint8_t> axml(std::string package, std::uint32_t code, std::string version, std::string label,
                               bool named = true, std::vector<std::pair<std::string, int>> features = {}) {
  std::vector<std::string> strings{named ? "label" : "", named ? "versionCode" : "", named ? "versionName" : "",
                                   "package", "manifest", "application", package, version, label,
                                   "uses-feature", "name", "required"};
  for (auto &[feature, required] : features) strings.push_back(feature);
  const std::vector<std::uint32_t> ids{0x01010001, 0x0101021b, 0x0101021c};
  std::vector<std::uint8_t> out;
  auto u16 = [](std::vector<std::uint8_t> &v, std::uint32_t x) {
    v.push_back(std::uint8_t(x));
    v.push_back(std::uint8_t(x >> 8));
  };
  auto u32 = [&](std::vector<std::uint8_t> &v, std::uint32_t x) {
    u16(v, x & 0xffff);
    u16(v, x >> 16);
  };
  // String pool, UTF-16.
  std::vector<std::uint8_t> data;
  std::vector<std::uint32_t> offsets;
  for (auto &s : strings) {
    offsets.push_back(std::uint32_t(data.size()));
    u16(data, std::uint32_t(s.size()));
    for (char c : s) u16(data, std::uint8_t(c));
    u16(data, 0);
  }
  while (data.size() % 4) data.push_back(0);
  std::vector<std::uint8_t> pool;
  const std::uint32_t pool_header = 28, start = pool_header + std::uint32_t(offsets.size()) * 4;
  u16(pool, 1);
  u16(pool, pool_header);
  u32(pool, start + std::uint32_t(data.size()));
  u32(pool, std::uint32_t(strings.size()));
  u32(pool, 0);
  u32(pool, 0);  // UTF-16
  u32(pool, start);
  u32(pool, 0);
  for (auto o : offsets) u32(pool, o);
  pool.insert(pool.end(), data.begin(), data.end());
  std::vector<std::uint8_t> map;
  u16(map, 0x180);
  u16(map, 8);
  u32(map, 8 + std::uint32_t(ids.size()) * 4);
  for (auto id : ids) u32(map, id);
  struct Attr {
    std::uint32_t name, raw;
    std::uint8_t type;
    std::uint32_t data;
  };
  auto element = [&](std::uint32_t name, std::vector<Attr> attrs) {
    std::vector<std::uint8_t> e;
    u16(e, 0x102);
    u16(e, 16);
    u32(e, 16 + 20 + std::uint32_t(attrs.size()) * 20);
    u32(e, 1);
    u32(e, 0xffffffff);
    u32(e, 0xffffffff);
    u32(e, name);
    u16(e, 20);
    u16(e, 20);
    u16(e, std::uint32_t(attrs.size()));
    u16(e, 0);
    u16(e, 0);
    u16(e, 0);
    for (auto &a : attrs) {
      u32(e, 0xffffffff);
      u32(e, a.name);
      u32(e, a.raw);
      u16(e, 8);
      e.push_back(0);
      e.push_back(a.type);
      u32(e, a.data);
    }
    return e;
  };
  auto end = [&](std::uint32_t name) {
    std::vector<std::uint8_t> e;
    u16(e, 0x103);
    u16(e, 16);
    u32(e, 24);
    u32(e, 1);
    u32(e, 0xffffffff);
    u32(e, 0xffffffff);
    u32(e, name);
    return e;
  };
  auto manifest = element(4, {{3, 6, 3, 6}, {1, 0xffffffff, 0x10, code}, {2, 7, 3, 7}});
  auto application = element(5, {{0, 8, 3, 8}});
  std::vector<std::uint8_t> body;
  for (auto *part : {&pool, &map, &manifest, &application}) body.insert(body.end(), part->begin(), part->end());
  auto close_app = end(5), close_manifest = end(4);
  body.insert(body.end(), close_app.begin(), close_app.end());
  for (std::uint32_t i = 0; i < features.size(); ++i) {
    std::vector<Attr> attrs{{10, 12 + i, 3, 12 + i}};
    if (features[i].second >= 0) attrs.push_back({11, 0xffffffff, 0x12, features[i].second ? 0xffffffffu : 0u});
    auto open = element(9, attrs), close = end(9);
    body.insert(body.end(), open.begin(), open.end());
    body.insert(body.end(), close.begin(), close.end());
  }
  body.insert(body.end(), close_manifest.begin(), close_manifest.end());
  u16(out, 3);
  u16(out, 8);
  u32(out, 8 + std::uint32_t(body.size()));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}
// A ZIP with the given members.
void zip(const fs::path &file, const std::vector<std::pair<std::string, std::string>> &members) {
  fs::create_directories(file.parent_path());
  mz_zip_archive z{};
  REQUIRE(mz_zip_writer_init_file(&z, paths::utf8(file).c_str(), 0));
  for (auto &[name, content] : members)
    REQUIRE(mz_zip_writer_add_mem(&z, name.c_str(), content.data(), content.size(), MZ_DEFAULT_COMPRESSION));
  REQUIRE(mz_zip_writer_finalize_archive(&z));
  REQUIRE(mz_zip_writer_end(&z));
}
std::string as_string(const std::vector<std::uint8_t> &v) { return {v.begin(), v.end()}; }
// An APK of `package`, padded so its size is distinctive.
void apk(const fs::path &file, const std::string &package, std::uint32_t code, std::size_t padding = 0) {
  zip(file, {{"AndroidManifest.xml", as_string(axml(package, code, "1.0", "Game"))},
             {"classes.dex", std::string(padding, 'd')}});
}
void write(const fs::path &file, const std::string &content) {
  fs::create_directories(file.parent_path());
  std::ofstream(file, std::ios::binary) << content;
}
}  // namespace

TEST_CASE("APK manifests give package, version and label by name or by resource ID") {
  for (bool named : {true, false}) {
    auto m = android::parse_manifest(axml("com.example.game", 4321, "2.1.0", "Example Game", named));
    CHECK(m.package == "com.example.game");
    CHECK(m.version_code == 4321);
    CHECK(m.version_name == "2.1.0");
    CHECK(m.label == "Example Game");
  }
  auto truncated = axml("com.example.game", 1, "1", "x");
  truncated.resize(truncated.size() / 2);
  CHECK_THROWS(android::parse_manifest(truncated));
  CHECK_THROWS(android::parse_manifest(axml("../escape", 1, "1", "x")));
  std::vector<std::uint8_t> garbage(64, 0x41);
  CHECK_THROWS(android::parse_manifest(garbage));
}

TEST_CASE("APK manifests list the features they require") {
  auto m = android::parse_manifest(axml("com.example.game", 1, "1", "x", true,
      {{"oculus.software.handtracking", 1}, {"android.hardware.vr.headtracking", -1},
       {"com.oculus.feature.PASSTHROUGH", 0}}));
  // android:required defaults to true; required="false" features are optional.
  CHECK(m.required_features ==
        std::vector<std::string>{"oculus.software.handtracking", "android.hardware.vr.headtracking"});
  CHECK(android::parse_manifest(axml("com.example.game", 1, "1", "x")).required_features.empty());
}

TEST_CASE("resource tables resolve labels for an en-US device and the densest launcher image") {
  std::vector<std::uint8_t> out;
  auto u16 = [](std::vector<std::uint8_t> &v, std::uint32_t x) {
    v.push_back(std::uint8_t(x));
    v.push_back(std::uint8_t(x >> 8));
  };
  auto u32 = [&](std::vector<std::uint8_t> &v, std::uint32_t x) {
    u16(v, x & 0xffff);
    u16(v, x >> 16);
  };
  // Global value pool, UTF-8.
  const std::vector<std::string> strings{"Default", "English", "French", "res/a.png", "res/b.png", "res/c.xml"};
  std::vector<std::uint8_t> data, pool;
  std::vector<std::uint32_t> offsets;
  for (auto &s : strings) {
    offsets.push_back(std::uint32_t(data.size()));
    data.push_back(std::uint8_t(s.size()));
    data.push_back(std::uint8_t(s.size()));
    data.insert(data.end(), s.begin(), s.end());
    data.push_back(0);
  }
  while (data.size() % 4) data.push_back(0);
  const std::uint32_t start = 28 + std::uint32_t(offsets.size()) * 4;
  u16(pool, 1);
  u16(pool, 28);
  u32(pool, start + std::uint32_t(data.size()));
  u32(pool, std::uint32_t(strings.size()));
  u32(pool, 0);
  u32(pool, 0x100);  // UTF-8
  u32(pool, start);
  u32(pool, 0);
  for (auto o : offsets) u32(pool, o);
  pool.insert(pool.end(), data.begin(), data.end());
  // A ResTable_type chunk: entry index -> (value type, data), for one configuration.
  auto type = [&](std::uint8_t id, const char *language, std::uint16_t density,
                  std::vector<std::pair<std::uint8_t, std::uint32_t>> values) {
    std::vector<std::uint8_t> t;
    const std::uint32_t header = 20 + 64, entries = header + std::uint32_t(values.size()) * 4;
    u16(t, 0x201);
    u16(t, header);
    u32(t, entries + std::uint32_t(values.size()) * 16);
    t.push_back(id);
    t.push_back(0);
    u16(t, 0);
    u32(t, std::uint32_t(values.size()));
    u32(t, entries);
    std::vector<std::uint8_t> config(64, 0);
    config[0] = 64;
    if (language) config[8] = std::uint8_t(language[0]), config[9] = std::uint8_t(language[1]);
    config[14] = std::uint8_t(density), config[15] = std::uint8_t(density >> 8);
    t.insert(t.end(), config.begin(), config.end());
    for (std::size_t i = 0; i < values.size(); ++i) u32(t, std::uint32_t(i * 16));
    for (auto [kind, value] : values) {
      u16(t, 8);
      u16(t, 0);
      u32(t, 0);
      u16(t, 8);
      t.push_back(0);
      t.push_back(kind);
      u32(t, value);
    }
    return t;
  };
  constexpr std::uint8_t ref = 0x01, str = 0x03;
  std::vector<std::uint8_t> package;
  for (auto &chunk : {type(1, nullptr, 0, {{str, 0}, {ref, 0x7f010000}}), type(1, "fr", 0, {{str, 2}}),
                      type(1, "en", 0, {{str, 1}}), type(2, nullptr, 160, {{str, 3}}),
                      type(2, nullptr, 0xfffe, {{str, 5}}), type(2, nullptr, 640, {{str, 4}})})
    package.insert(package.end(), chunk.begin(), chunk.end());
  std::vector<std::uint8_t> head;
  u16(head, 0x200);
  u16(head, 288);
  u32(head, 288 + std::uint32_t(package.size()));
  u32(head, 0x7f);
  head.resize(288, 0);
  u16(out, 2);
  u16(out, 12);
  u32(out, 12 + std::uint32_t(pool.size() + head.size() + package.size()));
  u32(out, 1);
  out.insert(out.end(), pool.begin(), pool.end());
  out.insert(out.end(), head.begin(), head.end());
  out.insert(out.end(), package.begin(), package.end());

  CHECK(android::resource_string(out, 0x7f010000) == "English");
  CHECK(android::resource_string(out, 0x7f010001) == "English");  // through a reference
  CHECK(android::resource_file(out, 0x7f020000) == "res/b.png");   // xxxhdpi over mdpi; never the anydpi XML
  CHECK(android::resource_string(out, 0x7f010005).empty());
  CHECK(android::resource_string(out, 0x01010001).empty());
  CHECK(android::resource_file(out, 0x7f010000).empty());  // a string, not an image
  auto truncated = out;
  truncated.resize(out.size() - 20);
  CHECK_THROWS(android::resource_string(truncated, 0x7f010000));
}

TEST_CASE("APKs and archives are read through miniz from files and memory") {
  Directory d;
  apk(d.root / "game.apk", "com.example.game", 7, 100);
  auto m = android::read_apk(d.root / "game.apk");
  CHECK(m.package == "com.example.game");
  CHECK(m.version_code == 7);
  CHECK(android::is_archive(d.root / "game.apk"));
  write(d.root / "not.zip", "plain text");
  CHECK_FALSE(android::is_archive(d.root / "not.zip"));
  CHECK_THROWS(android::read_apk(d.root / "not.zip"));
  android::Archive archive(d.root / "game.apk");
  const auto *dex = archive.find("classes.dex");
  REQUIRE(dex);
  std::string streamed;
  archive.extract(*dex, [&](auto chunk) { streamed.append(chunk.begin(), chunk.end()); });
  CHECK(streamed == std::string(100, 'd'));
  std::stop_source stop;
  stop.request_stop();
  CHECK_THROWS(archive.extract(*dex, [](auto) {}, stop.get_token()));
}

namespace {
Json catalog_binary(std::uint64_t apk_size) {
  return Json{{"id", "456789"},
              {"binary_application", {{"id", "123456"}}},
              {"package_name", "com.example.game"},
              {"version", "1.2"},
              {"version_code", 12},
              {"size", apk_size},
              {"obb_binary", {{"id", "111110"}, {"file_name", "main.12.com.example.game.obb"}, {"size", 7}}},
              {"asset_files",
               {{"nodes",
                 Json::array({Json{{"id", "111111"}, {"file_name", "required.dat"}, {"size", 2}, {"is_required", true}},
                              Json{{"id", "222222"}, {"file_name", "voice_fr.pak"}, {"size", 3}, {"is_required", false}},
                              Json{{"id", "333333"}, {"file_name", "optional.dat"}, {"size", 4}, {"is_required", false}}})},
                {"count", 3}}}};
}
// A source as a headset presents it: saves, and a checksum computed on its
// side (here, optionally wrong).
class Headset final : public import::Source {
public:
  Headset(std::unique_ptr<import::Source> inner, std::vector<import::SourceFile> with_saves, bool damaged)
      : inner_(std::move(inner)), damaged_(damaged) {
    manifest = inner_->manifest;
    apk = inner_->apk;
    data = inner_->data;
    saves = std::move(with_saves);
  }
  std::string spec() const override { return inner_->spec(); }
  std::string describe() const override { return "headset"; }
  std::optional<std::string> sha256(const import::SourceFile &file, std::stop_token) const override {
    if (damaged_) return std::string(64, '0');
    return digest::sha256(std::span(reinterpret_cast<const std::uint8_t *>(paths::read(file.location).data()),
                                    file.size));
  }

protected:
  void read_from(const import::SourceFile &file, std::uint64_t offset,
                 const std::function<void(std::span<const std::uint8_t>)> &sink,
                 std::stop_token stop) const override {
    inner_->read(file, sink, stop, offset);
  }

private:
  std::unique_ptr<import::Source> inner_;
  bool damaged_;
};
}  // namespace

TEST_CASE("an import job installs the source's files, completes them from an owned build, and resumes") {
  Directory d;
  const auto games = d.root / "games", from = d.root / "from";
  apk(from / "game.apk", "com.example.game", 12, 50);
  const auto apk_size = fs::file_size(from / "game.apk");
  write(from / "com.example.game" / "main.12.com.example.game.obb", "obbdata");
  write(from / "com.example.game" / "save_cache.bin", "local");
  std::atomic<int> downloads{0};
  bool owned = true, headset = false, damaged = false;
  std::int64_t version = 12;
  write(from / "Android" / "files" / "save.dat", "save");
  auto queries = [&](std::string_view url, const http::Fields &fields) -> Json {
    std::map<std::string, std::string> f(fields.begin(), fields.end());
    if (http::parse_url(url).path == "/authenticate_application") return Json{{"access_token", "fixture-app-token"}};
    if (f["doc_id"] == "4850747515044496")
      return Json{{"data",
                   {{"viewer",
                     {{"user",
                       {{"id", "777777"},
                        {"active_entitlements",
                         {{"nodes", owned ? Json::array({Json{{"item", {{"id", "123456"}, {"platform", "ANDROID_6DOF"}}}}})
                                          : Json::array()},
                          {"page_info", {{"has_next_page", false}}}}}}}}}}}};
    if (f["doc_id"] == "6549406941839522")
      return Json{{"data",
                   {{"item",
                     {{"id", "123456"}, {"platform", "ANDROID_6DOF"}, {"latest_supported_binary", {{"id", "456789"}}}}}}}};
    if (f["doc_id"] == "2885322071572384")
      return Json{{"data",
                   {{"node",
                     {{"primary_binaries",
                       {{"nodes", Json::array({Json{{"id", "456789"}, {"version_code", version}, {"platform", "ANDROID_6DOF"}}})}}}}}}}};
    auto binary = catalog_binary(apk_size);
    binary["version_code"] = version;  // the build Meta lists now
    return Json{{"data", {{"node", binary}}}};
  };
  download::Manager::Services services{
      queries,
      {},
      [&](std::string, const fs::path &path, uint64_t size, std::stop_token, const http::Progress &progress) {
        ++downloads;
        paths::atomic_write(path, std::string(size, 'M'));
        progress(size, size);
      },
      [&](std::string_view spec, std::string_view package, std::stop_token stop) -> std::unique_ptr<import::Source> {
        auto source = import::reopen(spec, package, {}, stop);
        if (!headset) return source;
        const auto save = from / "Android" / "files" / "save.dat";
        return std::make_unique<Headset>(
            std::move(source),
            std::vector<import::SourceFile>{{"files/save.dat", fs::file_size(save), paths::utf8(save)},
                                            // A cache the headset dropped while it was listed.
                                            {"files/cache.store", 4, paths::utf8(save) + ".gone"}},
            damaged);
      }};
  download::Manager manager(d.root / "state", [] {}, services);
  manager.set_account("0123456789ab");
  auto spec = import::open_local(from / "game.apk").front()->spec();
  manager.enqueue_import("123456", "Game", games, "fixture", "0123456789ab", spec, "com.example.game");
  REQUIRE(test::wait_until([&] { return !manager.busy(); }));
  auto job = manager.snapshot().front();
  INFO("error: ", job.error);
  REQUIRE(job.state == download::State::complete);
  CHECK(downloads == 1);  // only required.dat, which the source lacks
  auto installs = library::scan(games);
  REQUIRE(installs.size() == 1);
  CHECK(installs[0].complete);
  CHECK(installs[0].plan.launcher_format);
  const auto dir = installs[0].directory;
  CHECK(fs::file_size(dir / "com.example.game.apk") == apk_size);
  CHECK(paths::read(dir / "main.12.com.example.game.obb") == "obbdata");
  CHECK(paths::read(dir / "save_cache.bin") == "local");
  for (auto &f : installs[0].plan.files) {
    if (f.name == "main.12.com.example.game.obb")
      CHECK(f.sha256 == digest::sha256(std::span(reinterpret_cast<const std::uint8_t *>("obbdata"), 7)));
    if (f.name == "required.dat") CHECK(f.sha256.empty());
  }
  CHECK(std::ranges::any_of(installs[0].plan.files, [](auto &f) { return f.name == "save_cache.bin" && f.id.empty(); }));
  // A version Meta does not list installs the source's files alone.
  version = 13;
  downloads = 0;
  manager.enqueue_import("123456", "Game", d.root / "games2", "fixture", "0123456789ab", spec, "com.example.game");
  REQUIRE(test::wait_until([&] { return !manager.busy(); }));
  job = manager.snapshot().front();
  INFO("error: ", job.error);
  CHECK(job.state == download::State::complete);
  CHECK(downloads == 0);
  auto other = library::scan(d.root / "games2");
  REQUIRE(other.size() == 1);
  CHECK(other[0].complete);
  CHECK(other[0].plan.local);
  CHECK(other[0].plan.app_id == "123456");
  CHECK(other[0].plan.binary_id.empty());
  CHECK(paths::read(other[0].directory / "main.12.com.example.game.obb") == "obbdata");
  CHECK(fs::file_size(other[0].directory / "com.example.game.apk") == apk_size);
  CHECK(std::ranges::all_of(other[0].plan.files, [](auto &f) { return f.sha256.size() == 64; }));
  // An interrupted copy continues from its last whole block: the kept block
  // (here deliberately unlike the source) stays, the rest is read from there.
  version = 12;
  const auto block = android::adb::kPullBlock;
  write(from / "com.example.game" / "save_cache.bin", std::string(block + 5, 'b'));
  const auto resumed = paths::game_dir(d.root / "games4", "com.example.game", "12");
  write(resumed / "save_cache.bin.part", std::string(block, 'a') + "XYZ");
  manager.enqueue_import("123456", "Game", d.root / "games4", "fixture", "0123456789ab", spec, "com.example.game");
  REQUIRE(test::wait_until([&] { return !manager.busy(); }));
  CHECK(manager.snapshot().front().state == download::State::complete);
  CHECK(paths::read(resumed / "save_cache.bin") == std::string(block, 'a') + "bbbbb");
  // From a headset: copies are checked against its checksums, and saves land
  // in the owning account's storage for the game unless it already has some.
  headset = true;
  const auto saves = account_data_dir(d.root / "state", "0123456789ab", "com.example.game") / "external-data";
  manager.enqueue_import("123456", "Game", d.root / "games5", "fixture", "0123456789ab", spec, "com.example.game");
  REQUIRE(test::wait_until([&] { return !manager.busy(); }));
  job = manager.snapshot().front();
  INFO("error: ", job.error);
  CHECK(job.state == download::State::complete);
  CHECK(paths::read(saves / "files" / "save.dat") == "save");
  CHECK_FALSE(fs::exists(saves / "files" / "cache.store"));
  CHECK_FALSE(job.saves_kept);
  CHECK_FALSE(fs::exists(paths::utf8(saves) + ".import"));
  write(from / "Android" / "files" / "save.dat", "newer");
  manager.enqueue_import("123456", "Game", d.root / "games6", "fixture", "0123456789ab", spec, "com.example.game");
  REQUIRE(test::wait_until([&] { return !manager.busy(); }));
  CHECK(manager.snapshot().front().state == download::State::complete);
  CHECK(manager.snapshot().front().saves_kept);
  CHECK(paths::read(saves / "files" / "save.dat") == "save");
  damaged = true;
  manager.enqueue_import("123456", "Game", d.root / "games7", "fixture", "0123456789ab", spec, "com.example.game");
  REQUIRE(test::wait_until([&] { return !manager.busy(); }));
  job = manager.snapshot().front();
  CHECK(job.state == download::State::failed);
  CHECK(job.error.find("arrived damaged") != std::string::npos);
  CHECK_FALSE(fs::exists(paths::game_dir(d.root / "games7", "com.example.game", "12") / "com.example.game.apk"));
  headset = damaged = false;
  // Without ownership the source's files install as they are.
  version = 12;
  owned = false;
  downloads = 0;
  manager.enqueue_import("123456", "Game", d.root / "games3", "fixture", "0123456789ab", spec, "com.example.game");
  REQUIRE(test::wait_until([&] { return !manager.busy(); }));
  job = manager.snapshot().front();
  INFO("error: ", job.error);
  CHECK(job.state == download::State::complete);
  CHECK(downloads == 0);
  REQUIRE(library::scan(d.root / "games3").size() == 1);
  CHECK(library::scan(d.root / "games3")[0].plan.local);
  // Nor is an account needed: a local import is keyed by its package, shows
  // whichever account is active, and keeps headset saves in local storage.
  headset = true;
  manager.set_account("");
  manager.enqueue_import("com.example.game", "com.example.game", d.root / "games8", "", "local", spec,
                         "com.example.game");
  REQUIRE(test::wait_until([&] { return !manager.busy(); }));
  const auto jobs = manager.snapshot();
  auto local = std::ranges::find(jobs, std::string("local"), &download::Job::account);
  REQUIRE(local != jobs.end());
  INFO("error: ", local->error);
  CHECK(local->state == download::State::complete);
  auto installed = library::scan(d.root / "games8");
  REQUIRE(installed.size() == 1);
  CHECK(installed[0].complete);
  CHECK(installed[0].plan.app_id == "com.example.game");
  CHECK(paths::read(account_data_dir(d.root / "state", "local", "com.example.game") / "external-data" / "files" /
                    "save.dat") == "newer");
  // Store downloads still need an account and a sign-in.
  CHECK_THROWS(manager.enqueue("123456", "Game", d.root / "games9", "", "local"));
  CHECK_THROWS(manager.enqueue("123456", "Game", d.root / "games9", "", "0123456789ab"));
  CHECK_THROWS(manager.enqueue_import("com.other", "Game", d.root / "games9", "", "local", spec, "com.example.game"));
  // A Unity bundle shorter than its header says fails the import by name; a
  // whole one imports.
  headset = false;
  auto bundle = [](std::uint64_t whole, std::size_t length) {
    std::string bytes("UnityFS\0\0\0\0\x08" "5.x.x\0" "2022.3.45f1\0", 30);
    for (int shift = 56; shift >= 0; shift -= 8) bytes += char(whole >> shift & 0xff);
    bytes.resize(length, 'B');
    return bytes;
  };
  write(from / "com.example.game" / "scenes.bundle", bundle(4096, 1000));
  CHECK(download::unity_bundle_size(from / "com.example.game" / "scenes.bundle") == 4096);
  CHECK_FALSE(download::unity_bundle_size(from / "com.example.game" / "save_cache.bin"));
  manager.enqueue_import("com.example.game", "com.example.game", d.root / "games10", "", "local", spec,
                         "com.example.game");
  REQUIRE(test::wait_until([&] { return !manager.busy(); }));
  job = manager.snapshot().front();
  CHECK(job.state == download::State::failed);
  CHECK(job.error.find("scenes.bundle is damaged") != std::string::npos);
  write(from / "com.example.game" / "scenes.bundle", bundle(4096, 4096));
  manager.enqueue_import("com.example.game", "com.example.game", d.root / "games11", "", "local", spec,
                         "com.example.game");
  REQUIRE(test::wait_until([&] { return !manager.busy(); }));
  job = manager.snapshot().front();
  INFO("error: ", job.error);
  CHECK(job.state == download::State::complete);
}

TEST_CASE("queued jobs start in list order, move, and cancel; a running job stops before it leaves") {
  Directory d;
  const auto games = d.root / "games";
  std::map<std::string, std::string> specs;
  for (std::string name : {"a", "b", "c"}) {
    const auto package = "com.example." + name;
    apk(d.root / name / "game.apk", package, 1);
    specs[name] = import::open_local(d.root / name / "game.apk").front()->spec();
  }
  std::atomic<bool> hold{true}, started{false};
  download::Manager::Services services{
      {}, {}, {},
      [&](std::string_view spec, std::string_view package, std::stop_token stop) -> std::unique_ptr<import::Source> {
        if (package == "com.example.a") {
          started = true;
          while (hold && !stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
          if (stop.stop_requested()) throw std::runtime_error("Transfer paused.");
        }
        return import::reopen(spec, package, {}, stop);
      }};
  download::Manager manager(d.root / "state", [] {}, services);
  auto add = [&](std::string name) {
    manager.enqueue_import("com.example." + name, name, games, "", "local", specs[name], "com.example." + name);
  };
  auto order = [&] {
    std::string out;
    for (auto &j : manager.snapshot()) out += j.name;
    return out;
  };
  auto job = [&](std::string name) {
    for (auto &j : manager.snapshot())
      if (j.name == name) return j;
    FAIL("no job " << name);
    return download::Job{};
  };
  add("a");
  REQUIRE(test::wait_until([&] { return started.load(); }));
  add("b");
  add("c");
  CHECK(order() == "abc");
  manager.move(job("c"), -1);
  CHECK(order() == "acb");
  manager.move(job("a"), -1);  // already first
  manager.move(job("b"), 1);   // already last
  CHECK(order() == "acb");
  manager.remove(job("b"), true);  // queued: leaves at once
  CHECK(order() == "ac");
  manager.remove(job("a"), true);  // running: hidden now, released once stopped
  CHECK(order() == "c");
  REQUIRE(test::wait_until([&] { return !manager.busy(); }));
  CHECK(job("c").state == download::State::complete);
  CHECK(manager.discarded() == 2);
  // Cancelling a finished job clears it and keeps the installed game.
  manager.remove(job("c"), true);
  CHECK(order().empty());
  auto installs = library::scan(games);
  REQUIRE(installs.size() == 1);
  CHECK(installs[0].complete);
  // The saved queue drops removed jobs; a stopped job can be queued again.
  hold = false;
  add("a");
  REQUIRE(test::wait_until([&] { return !manager.busy(); }));
  CHECK(job("a").state == download::State::complete);
}

TEST_CASE("parallel jobs run different games side by side and one game's jobs in turn") {
  Directory d;
  const auto games = d.root / "games";
  std::map<std::string, std::string> specs;
  for (std::string name : {"a", "b"}) {
    apk(d.root / name / "game.apk", "com.example." + name, 1);
    specs[name] = import::open_local(d.root / name / "game.apk").front()->spec();
  }
  std::atomic<bool> hold{true};
  std::atomic<int> running{0}, peak{0};
  download::Manager::Services services{
      {}, {}, {},
      [&](std::string_view spec, std::string_view package, std::stop_token stop) -> std::unique_ptr<import::Source> {
        const int now = ++running;
        for (int seen = peak; now > seen && !peak.compare_exchange_weak(seen, now);) {}
        while (hold && !stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        --running;
        return import::reopen(spec, package, {}, stop);
      }};
  download::Manager manager(d.root / "state", [] {}, services);
  manager.set_parallel(3);  // room for all three; the second "a" still waits
  auto add = [&](std::string name, std::string app) {
    manager.enqueue_import(app, name, games, "", "local", specs[name], "com.example." + name);
  };
  add("a", "com.example.a");
  add("a", "com.example.a");  // the same game waits for its first job
  add("b", "com.example.b");
  REQUIRE(test::wait_until([&] { return running == 2; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(running == 2);
  hold = false;
  REQUIRE(test::wait_until([&] { return !manager.busy(); }));
  CHECK(peak == 2);
}

#ifndef _WIN32
TEST_CASE("an adb client lists a headset's apps and pulls files through exec-out") {
  Directory d;
  // A stand-in adb answering the commands the client sends.
  const auto adb = d.root / "adb";
  write(adb,
        "#!/bin/sh\n"
        "[ \"$1\" = -s ] && shift 2\n"
        "case \"$1\" in\n"
        "devices) printf 'List of devices attached\\nSERIAL1 device product:eureka model:Quest_3\\n' ;;\n"
        "shell) case \"$2\" in\n"
        "  pm\\ list*) printf 'package:com.example.game versionCode:12\\n' ;;\n"
        "  *) printf '@com.example.game\\n9 /data/app/x/base.apk\\n4 "
        "/sdcard/Android/obb/com.example.game/main.12.com.example.game.obb\\n' ;;\n"
        "  esac ;;\n"
        "exec-out) case \"$2\" in sha256sum*) printf '%064d  x\\n' 7 ;; dd*skip=1\\ *) printf 'end' ;;\n"
        "  *base.apk*) printf 'apk-bytes' ;; *) printf 'obb!' ;; esac ;;\n"
        "esac\n");
  fs::permissions(adb, fs::perms::owner_all);
  CHECK(android::adb::locate(adb) == adb);
  CHECK(android::adb::locate(d.root) == adb);
  CHECK_FALSE(android::adb::locate(d.root / "missing"));
  android::adb::Client client(adb, "SERIAL1");
  auto devices = client.devices();
  REQUIRE(devices.size() == 1);
  CHECK(devices[0].model == "Quest 3");
  auto apps = client.apps();
  REQUIRE(apps.size() == 1);
  auto source = import::open_device(client, apps[0]);
  CHECK(source->spec() == "adb:SERIAL1");
  CHECK(source->apk.name == "base.apk");
  REQUIRE(source->data.size() == 1);
  std::string pulled;
  source->read(source->apk, [&](auto chunk) { pulled.append(chunk.begin(), chunk.end()); }, {});
  CHECK(pulled == "apk-bytes");
  // A short transfer is an error, never a silently truncated file.
  CHECK_THROWS(client.pull({"/data/app/x/base.apk", 100}, [](auto) {}));
  // Resumed pulls read from a whole block; the device hashes its own copy.
  std::string rest;
  client.pull({"/data/app/x/base.apk", android::adb::kPullBlock + 3}, [&](auto chunk) { rest.append(chunk.begin(), chunk.end()); },
              {}, android::adb::kPullBlock);
  CHECK(rest == "end");
  CHECK_THROWS(client.pull({"/data/app/x/base.apk", 9}, [](auto) {}, {}, 5));
  CHECK(client.sha256({"/data/app/x/base.apk", 9}) == std::string(63, '0') + "7");
}
#endif
