#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "gfx/blob_cache.h"

using venture::gfx::BlobStore;
namespace fs = std::filesystem;
namespace {
std::string value_for(const std::string& key, std::size_t size) {
  std::string value(size, '\0');
  for (std::size_t i = 0; i < size; ++i)
    value[i] = static_cast<char>(key[i % key.size()] + i);
  return value;
}
std::string read(const BlobStore& store, const std::string& key) {
  const auto size = store.get(key.data(), key.size(), nullptr, 0);
  std::string value(size, '\0');
  if (size) CHECK(store.get(key.data(), key.size(), value.data(), size) == size);
  return value;
}
void put(BlobStore& store, const std::string& key, const std::string& value) {
  store.set(key.data(), key.size(), value.data(), value.size());
}
struct TempDir {
  fs::path path = fs::temp_directory_path() /
                  ("venture-blob-cache-" + std::to_string(std::random_device{}()));
  TempDir() { fs::create_directories(path); }
  ~TempDir() {
    std::error_code ignored;
    fs::remove_all(path, ignored);  // plain files only
  }
};
std::string file_bytes(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}
void write_bytes(const fs::path& file, const std::string& bytes) {
  std::ofstream(file, std::ios::binary | std::ios::trunc) << bytes;
}
constexpr BlobStore::Limits kLimits{64, 4096, 1 << 20};
}  // namespace

TEST_CASE("blob cache file round trip keeps every entry") {
  TempDir dir;
  const auto file = dir.path / "com.android.opengl.shaders_cache";
  BlobStore store(kLimits);
  for (int i = 0; i < 200; ++i) {
    const auto key = "program-" + std::to_string(i);
    put(store, key, value_for(key, 1 + (i * 37) % 4000));
  }
  REQUIRE(store.save(file, "angle|vendor=0x10de"));
  CHECK_FALSE(fs::exists(fs::path(file) += ".tmp"));
  BlobStore loaded(kLimits);
  REQUIRE(loaded.load(file, "angle|vendor=0x10de"));
  CHECK(loaded.entries() == 200);
  CHECK(loaded.total_bytes() == store.total_bytes());
  for (int i = 0; i < 200; ++i) {
    const auto key = "program-" + std::to_string(i);
    CHECK(read(loaded, key) == value_for(key, 1 + (i * 37) % 4000));
  }
  // Entries already in memory are newer than the file's.
  BlobStore merged(kLimits);
  put(merged, "program-0", "newer");
  REQUIRE(merged.load(file, "angle|vendor=0x10de"));
  CHECK(read(merged, "program-0") == "newer");
  CHECK(merged.entries() == 200);
}

TEST_CASE("blob cache ignores corrupt, truncated or extended files") {
  TempDir dir;
  const auto file = dir.path / "cache";
  BlobStore store(kLimits);
  for (int i = 0; i < 20; ++i) put(store, "k" + std::to_string(i), value_for("v" + std::to_string(i), 300));
  REQUIRE(store.save(file, "id"));
  const auto good = file_bytes(file);
  auto check_rejected = [&](const std::string& bytes) {
    write_bytes(file, bytes);
    BlobStore loaded(kLimits);
    CHECK_FALSE(loaded.load(file, "id"));
    CHECK(loaded.entries() == 0);
  };
  auto flipped = good;
  flipped[good.size() / 2] ^= 0x40;
  check_rejected(flipped);
  check_rejected(good.substr(0, good.size() - 7));
  check_rejected(good.substr(0, 10));
  check_rejected(good + "x");
  check_rejected("");
  BlobStore missing(kLimits);
  CHECK_FALSE(missing.load(dir.path / "absent", "id"));
  // A crash during the next save leaves only a partial temporary file.
  write_bytes(file, good);
  write_bytes(fs::path(file) += ".tmp", good.substr(0, 100));
  BlobStore loaded(kLimits);
  CHECK(loaded.load(file, "id"));
  CHECK(loaded.entries() == 20);
  REQUIRE(loaded.save(file, "id"));
  CHECK(file_bytes(file) == good);
}
