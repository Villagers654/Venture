#include <doctest/doctest.h>
#include "core/engine_defaults.h"
#include "core/process.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
struct Fixture {
  const fs::path temporary = fs::weakly_canonical(fs::temp_directory_path());
  const fs::path root = temporary / ("venture-engine-defaults-" + std::to_string(std::random_device{}()));
  venture::ProcessConfig config;
  std::vector<fs::path> libraries{"libCrySystem.so", "libvrapi.so"};
  Fixture() {
    if (!fs::create_directory(root)) throw std::runtime_error("Test directory collision");
    config.data_dir = root / "app data";
  }
  ~Fixture() {
    std::error_code ignored;
    if (root.parent_path() == temporary && root.filename().string().starts_with("venture-engine-defaults-"))
      fs::remove_all(root, ignored);
  }
  fs::path file() const { return config.data_dir / "external-data/files/user.cfg"; }
  void put(std::string_view bytes) {
    fs::create_directories(file().parent_path());
    std::ofstream out(file(), std::ios::binary);
    out.write(bytes.data(), bytes.size());
    REQUIRE(out.good());
  }
  std::string read() const {
    std::ifstream in(file(), std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
  }
  void apply(bool supported = false) {
    venture::engine_policy::prepare_renderer_defaults(config, libraries, {supported});
  }
};
}

TEST_CASE("engine renderer defaults respect explicit user settings") {
  for (const std::string original : {
      "sys_MaxFPS = 72\nr_variable_rate_shading = 1\n",
      "sys_MaxFPS = 72\r\nr_variable_rate_shading = 1\r\n",
      "sys_MaxFPS = 72\nr_variable_rate_shading = 1",
      "  R_VARIABLE_RATE_SHADING\t= 1\n"}) {
    Fixture f;
    f.put(original);
    f.apply();
    CHECK(f.read() == original);
    f.apply(true);
    CHECK(f.read() == original);
  }
}

TEST_CASE("engine renderer defaults keep later user commands separate on restoration") {
  Fixture f;
  const std::string original = "sys_MaxFPS = 72";
  f.put(original);
  f.apply();
  f.put(f.read() + "r_DisplayInfo = 1\n");
  f.apply(true);
  CHECK(f.read() == "sys_MaxFPS = 72\nr_DisplayInfo = 1\n");
}

TEST_CASE("engine renderer defaults reject unreadable text without overwriting configuration") {
  Fixture f;
  const std::string original("setting\0value", 13);
  f.put(original);
  CHECK_NOTHROW(f.apply());
  CHECK(f.read() == original);
  f.put("\n-- Venture renderer defaults begin\nr_variable_rate_shading = 0\n");
  const auto incomplete = f.read();
  CHECK_NOTHROW(f.apply());
  CHECK(f.read() == incomplete);
}

TEST_CASE("engine renderer defaults never write through a redirected config file") {
  Fixture f;
  const auto outside = f.root / "other.cfg";
  { std::ofstream out(outside); out << "preserve\n"; }
  fs::create_directories(f.file().parent_path());
  std::error_code error;
  fs::create_symlink(outside, f.file(), error);
  if (error) { MESSAGE("Host does not allow unprivileged symbolic-link creation"); return; }
  CHECK_NOTHROW(f.apply());
  std::ifstream in(outside);
  CHECK(std::string(std::istreambuf_iterator<char>(in), {}) == "preserve\n");
}

TEST_CASE("engine renderer defaults keep generated configuration inside app storage") {
  Fixture f;
  const auto elsewhere = f.root / "other app";
  fs::create_directories(elsewhere);
  fs::create_directories(f.config.data_dir);
  std::error_code error;
  fs::create_directory_symlink(elsewhere, f.config.data_dir / "external-data", error);
  if (error) { MESSAGE("Host does not allow unprivileged symbolic-link creation"); return; }
  CHECK_NOTHROW(f.apply());
  CHECK_FALSE(fs::exists(elsewhere / "files/user.cfg"));
}
