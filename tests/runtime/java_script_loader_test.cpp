#include "core/java_script_loader.h"
#include "common/file_ops.h"
#include "common/paths.h"
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <array>

using namespace venture;
namespace {
using Json = nlohmann::json;
struct Fixture {
  fs::path root = fs::temp_directory_path() / ("venture-script-loader-" + random_account_key());
  fs::path binary = root / "libarbitrary.so";
  fs::path config = root / "libarbitrary.config.so";
  fs::path script = root / "original.js";
  std::string source = "'use strict';\nvar originalText = '\\u2603';\nrpc.exports = {init() {}};\n";
  Json settings = {{"interaction", {{"type", "script"}, {"path", "original.js"},
      {"parameters", {{"fixture", true}}}}}, {"runtime", "jit"}, {"teardown", "full"}};
  Fixture() {
    fs::create_directories(root);
    paths::atomic_write(binary, std::string("\177ELF", 4));
    paths::atomic_write(script, source);
    save();
  }
  ~Fixture() {
    std::error_code ignored;
    fs::remove_all(root, ignored); // unique owned test directory
  }
  void save() { paths::atomic_write(config, settings.dump()); }
  std::size_t prepare() {
    const std::array libraries{config, binary, script};
    return prepare_java_script_loaders(libraries, [](std::string_view) { return fs::path{}; });
  }
  fs::path wrapper() { return root / paths::from_utf8(Json::parse(paths::read(config))["interaction"]["path"].get<std::string>()); }
};
}
TEST_CASE("restored APK configuration regenerates stable startup support and repairs cache damage") {
  Fixture f;
  REQUIRE(f.prepare() == 1);
  const auto wrapper_path = f.wrapper();
  const auto original_wrapper = paths::read(wrapper_path);
  f.save(); // normal extraction restores the original CRC-verified configuration
  REQUIRE(f.prepare() == 1);
  CHECK(f.wrapper() == wrapper_path);
  paths::atomic_write(wrapper_path, "damaged");
  f.save();
  REQUIRE(f.prepare() == 1);
  CHECK(paths::read(wrapper_path) == original_wrapper);
  CHECK(paths::read(f.script) == f.source);
}
TEST_CASE("absolute startup script paths use the guest resolver") {
  Fixture f;
  f.settings["interaction"]["path"] = "/data/example/startup.js";
  f.save();
  std::string resolved;
  const std::array libraries{f.config};
  REQUIRE(prepare_java_script_loaders(libraries, [&](std::string_view path) {
    resolved = path;
    return f.script;
  }) == 1);
  CHECK(resolved == "/data/example/startup.js");
  CHECK(paths::read(f.script) == f.source);
}
TEST_CASE("startup script support requires the paired ELF and keeps relative reads inside the cache") {
  Fixture f;
  paths::atomic_write(f.binary, "not an ELF");
  CHECK(f.prepare() == 0);
  paths::atomic_write(f.binary, std::string("\177ELF", 4));
  f.settings["interaction"]["path"] = "../outside.js";
  f.save();
  CHECK(f.prepare() == 0);
}
