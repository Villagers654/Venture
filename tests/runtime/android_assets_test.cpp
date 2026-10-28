#include "android/errors.h"
#include <doctest/doctest.h>
#include "android/package_assets.h"
#include "android_test_support.h"
#include "core/guest.h"
#include "common/paths.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace venture;
using namespace venture::android;
namespace {
const std::filesystem::path fixture_dir = VENTURE_ANDROID_FIXTURES;
std::string path(const char* name) { return (fixture_dir / name).string(); }
std::vector<unsigned char> unhex(const std::string& text) {
  std::vector<unsigned char> bytes;
  for (std::size_t i = 0; i < text.size(); i += 2)
    bytes.push_back(static_cast<unsigned char>(std::stoi(text.substr(i, 2), nullptr, 16)));
  return bytes;
}
void check_oracle(const nlohmann::json& archive) {
  auto& assets = PackageAssets::get();
  auto filename = archive.at("path").get<std::string>();
  CAPTURE(filename);
  REQUIRE(assets.open_apk(filename));
  for (const auto& e : archive.at("entries")) {
    auto name = e.at("name").get<std::string>();
    CAPTURE(name);
    auto found = assets.find(name);
    REQUIRE(found.has_value());
    CHECK(found->size == e.at("size").get<std::uint64_t>());
    CHECK(found->compressed == e.at("compressed").get<bool>());
  }
  std::map<std::string, std::unique_ptr<AssetStream>> streams;
  for (const auto& sample : archive.at("samples")) {
    auto name = sample.at("name").get<std::string>();
    CAPTURE(name);
    auto& stream = streams[name];
    if (!stream) stream = assets.open(name);
    REQUIRE(stream);
    auto offset = sample.at("offset").get<std::int64_t>();
    REQUIRE(stream->seek(offset, 0) == offset);
    auto expected = unhex(sample.at("hex"));
    std::vector<unsigned char> actual(127);
    auto read = stream->read(actual.data(), actual.size());
    REQUIRE(read == static_cast<std::int64_t>(expected.size()));
    actual.resize(static_cast<std::size_t>(read));
    CHECK(actual == expected);
  }
}
}  // namespace
TEST_CASE("android extraction: interrupted writes never publish a partial asset") {
  struct InterruptedStream final : AssetStream {
    std::uint64_t position = 0;
    bool interrupted = true;
    std::int64_t read(void* out, std::uint64_t count) override {
      if (interrupted && position == 65536) return -err::io;
      count = std::min(count, length() - position);
      std::memset(out, 'x', count);
      position += count;
      return count;
    }
    std::int64_t seek(std::int64_t offset, int) override { return position = offset; }
    std::uint64_t length() const override { return 131072; }
    bool file_range(std::string*, std::uint64_t*) const override { return false; }
  } stream;
  const auto target = fixture_dir / "interrupted-copy.dat";
  paths::atomic_write(target, "previous complete contents");
  CHECK_FALSE(extract_asset_atomic(stream, target));
  CHECK(paths::read(target) == "previous complete contents");
  CHECK(std::filesystem::file_size(target.string() + ".venture-part") == 65536);
  stream.position = 0;
  stream.interrupted = false;
  REQUIRE(extract_asset_atomic(stream, target));
  CHECK(paths::read(target) == std::string(131072, 'x'));
  CHECK_FALSE(std::filesystem::exists(target.string() + ".venture-part"));
}
TEST_CASE("android extraction: startup repairs metadata despite Unity completion marker") {
  auto& assets = PackageAssets::get();
  REQUIRE(assets.open_apk(path("unity-extraction.apk")));
  const auto files = fixture_dir / "extraction-recovery";
  const auto root = files / "il2cpp";
  const auto metadata = root / "Metadata" / "global-metadata.dat";
  const auto marker = root / "unity.ver";
  const auto resource = root / "Resources" / "test.dll-resources.dat";
  paths::atomic_write(marker, "Unity version");
  paths::atomic_write(metadata, "partial");
  REQUIRE(assets.prepare_unity_extraction(files));
  CHECK_FALSE(std::filesystem::exists(marker));
  CHECK(std::filesystem::file_size(metadata) == assets.find("assets/bin/Data/Managed/Metadata/global-metadata.dat")->size);
  CHECK(paths::read(resource) == "synthetic resources");
  CHECK(std::filesystem::exists(root / ".venture-complete"));
  const auto resource_time = std::filesystem::last_write_time(resource);
  paths::atomic_write(marker, "Unity version");
  REQUIRE(assets.prepare_unity_extraction(files));
  CHECK(paths::read(marker) == "Unity version");
  // A process can die while re-extracting even after our own previous marker.
  // Validate sizes on every launch, rather than trusting either marker alone.
  paths::atomic_write(metadata, "partial again");
  REQUIRE(assets.prepare_unity_extraction(files));
  CHECK_FALSE(std::filesystem::exists(marker));
  CHECK(std::filesystem::last_write_time(resource) == resource_time);
  CHECK(std::filesystem::file_size(metadata) == assets.find("assets/bin/Data/Managed/Metadata/global-metadata.dat")->size);
  std::filesystem::remove(metadata);
  paths::atomic_write(marker, "Unity version");
  REQUIRE(assets.prepare_unity_extraction(files));
  CHECK_FALSE(std::filesystem::exists(marker));
  CHECK(std::filesystem::exists(metadata));
  // Older, complete caches have no Venture marker. Creating it must not
  // replace valid engine files (including any handles already open on them).
  std::filesystem::remove(root / ".venture-complete");
  const auto metadata_time = std::filesystem::last_write_time(metadata);
  paths::atomic_write(marker, "Unity version");
  REQUIRE(assets.prepare_unity_extraction(files));
  CHECK(std::filesystem::last_write_time(metadata) == metadata_time);
  CHECK(std::filesystem::last_write_time(resource) == resource_time);
  CHECK(paths::read(marker) == "Unity version");
  // A changed package manifest must replace same-size stale contents too.
  paths::atomic_write(root / ".venture-complete", "older package inventory");
  paths::atomic_write(resource, std::string(std::string("synthetic resources").size(), 'x'));
  REQUIRE(assets.prepare_unity_extraction(files));
  CHECK_FALSE(std::filesystem::exists(marker));
  CHECK(paths::read(resource) == "synthetic resources");
}
TEST_CASE("android ZIP: Python zipfile oracle, stored/deflated/UTF8/ZIP64") {
  std::ifstream file(path("synthetic.json"));
  REQUIRE(file);
  auto oracles = nlohmann::json::parse(file);
  for (auto& oracle : oracles) check_oracle(oracle);
  REQUIRE(PackageAssets::get().open_apk(path("zip64.apk")));
  auto entry = PackageAssets::get().find("assets/large-offset.bin");
  REQUIRE(entry);
  CHECK(entry->data_offset > (1ull << 32));
}
TEST_CASE("android ZIP: seek boundaries, CRC failure and atomic index replacement") {
  auto& a = PackageAssets::get();
  REQUIRE(a.open_apk(path("test.apk")));
  auto plain = a.open("assets/plain.bin");
  REQUIRE(plain);
  CHECK(plain->seek(-1, 0) == -err::invalid);
  CHECK(plain->seek(INT64_MAX, 1) == -err::invalid);
  CHECK(plain->seek(0, 88) == -err::invalid);
  CHECK(plain->seek(0, 2) == 262144);
  char bytes[16];
  CHECK(plain->read(bytes, 16) == 0);
  CHECK(plain->seek(-8, 1) == 262136);
  CHECK(plain->read(bytes, 16) == 8);
  CHECK(plain->seek(1, 2) == -err::invalid);
  CHECK(plain->read(nullptr, 1) == -err::fault);
  std::string host;
  std::uint64_t offset;
  CHECK(plain->file_range(&host, &offset));
  CHECK(host == path("test.apk"));
  CHECK(a.list("assets/sub/") == std::vector<std::string>{"nested.txt"});
  auto children = a.list("assets");
  CHECK(std::find(children.begin(), children.end(), "empty-directory") != children.end());
  CHECK(a.find("main.test.obb/assets/obb.txt").has_value());
  CHECK(a.find("assets/plain.bin")->size == 262144);
  CHECK_FALSE(a.open_apk(path("truncated.apk")));
  CHECK(a.find("assets/plain.bin")->size == 262144);
  REQUIRE(a.open_apk(path("bad-crc.apk")));
  auto bad = a.open("assets/deflated.bin");
  REQUIRE(bad);
  std::vector<char> data(static_cast<std::size_t>(bad->length()));
  CHECK(bad->read(data.data(), data.size()) == -err::io);
  // The original stream still refers to the old APK even after replacement.
  CHECK(plain->seek(0, 0) == 0);
  CHECK(plain->read(bytes, 16) == 16);
  CHECK(bytes[15] == 15);
}
TEST_CASE("android assets: HLE read seek buffer descriptor and directory semantics") {
  using test::android_call;
  REQUIRE(PackageAssets::get().open_apk(path("test.apk")));
  auto call = [](const char* name, std::initializer_list<std::uint64_t> args = {}) {
    return test::android_call("libandroid.so", name, args).x0;
  };
  CHECK(call("AAssetManager_fromJava", {0, 0}) == 0);
  auto manager = call("AAssetManager_fromJava", {0, 1});
  REQUIRE(manager);
  auto asset = call("AAssetManager_open", {manager, h2g("plain.bin"), 2});
  REQUIRE(asset);
  CHECK(call("AAsset_getLength64", {asset}) == 262144);
  CHECK(call("AAsset_seek", {asset, 513, 0}) == 513);
  unsigned char bytes[16]{};
  CHECK(call("AAsset_read", {asset, h2g(bytes), 16}) == 16);
  CHECK(bytes[0] == 1);
  auto buffer = call("AAsset_getBuffer", {asset});
  REQUIRE(buffer);
  CHECK(g2h<unsigned char>(buffer)[255] == 255);
  CHECK(call("AAsset_getRemainingLength", {asset}) == 262144 - 529);
  CHECK(call("AAsset_isAllocated", {asset}) == 1);
  std::int64_t start = -1, len = -1;
  auto fd = static_cast<int>(call("AAsset_openFileDescriptor64", {asset, h2g(&start), h2g(&len)}));
  REQUIRE(fd >= 0);
  CHECK(start > 0);
  CHECK(len == 262144);
  CHECK(test::android_fd_path(fd) == path("test.apk"));
  test::android_close_fd(fd);
  call("AAsset_close", {asset});
  CHECK(static_cast<std::int64_t>(call("AAsset_read", {asset, h2g(bytes), 1})) == -1);
  auto compressed = call("AAssetManager_open", {manager, h2g("deflated.bin"), 1});
  REQUIRE(compressed);
  CHECK(static_cast<std::int64_t>(call("AAsset_openFileDescriptor", {compressed, h2g(&start), h2g(&len)})) ==
        -1);
  CHECK(call("AAsset_getBuffer", {compressed}) != 0);
  call("AAsset_close", {compressed});
  auto dir = call("AAssetManager_openDir", {manager, h2g("")});
  REQUIRE(dir);
  std::vector<std::string> names;
  while (auto name = call("AAssetDir_getNextFileName", {dir})) names.emplace_back(g2h<char>(name));
  CHECK(std::find(names.begin(), names.end(), "plain.bin") != names.end());
  CHECK(std::find(names.begin(), names.end(), "sub") == names.end());
  call("AAssetDir_rewind", {dir});
  CHECK(call("AAssetDir_getNextFileName", {dir}) != 0);
  call("AAssetDir_close", {dir});
  // AOSP returns an empty directory, never NULL, for names with no entries.
  for (const char* missing : {"/sdcard/Android/obb/com.example", "absent", "../plain.bin"}) {
    auto empty = call("AAssetManager_openDir", {manager, h2g(missing)});
    REQUIRE(empty);
    CHECK(call("AAssetDir_getNextFileName", {empty}) == 0);
    call("AAssetDir_close", {empty});
  }
}
