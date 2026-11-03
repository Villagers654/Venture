#include "common/environment.h"
#include "common/paths.h"
#include "core/elf.h"
#include "core/process_runtime.h"
#include <doctest/doctest.h>
#include <miniz.h>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>

using namespace venture;
namespace {
struct Environment {
  const char* name;
  std::optional<std::string> previous;
  explicit Environment(const char* name) : name(name) {
    if (const auto* value = environment::getenv(name)) previous = value;
  }
  void set(const char* value) const {
#ifdef _WIN32
    _wputenv_s(paths::from_utf8(name).c_str(), value ? paths::from_utf8(value).c_str() : L"");
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
    environment::refresh();
  }
  ~Environment() { set(previous ? previous->c_str() : nullptr); }
};
struct Fixture {
  ProcessConfig previous = process_config();
  fs::path root = fs::temp_directory_path() / ("venture-passthrough-config-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  Fixture() { fs::create_directories(root); }
  ~Fixture() {
    process_config() = previous;
    std::error_code error;
    fs::remove_all(root, error);
  }
};
}

TEST_CASE("Passthrough configuration snapshots an explicit launcher environment opt-in") {
  Fixture fixture;
  Environment data("VENTURE_DATA_DIR"), opt_in("VENTURE_PASSTHROUGH");
  const auto data_dir = fixture.root / "data";
  data.set(paths::utf8(data_dir).c_str());
  const auto install = fixture.root / "game";
  fs::create_directories(install);
  {
    mz_zip_archive zip{};
    const auto apk = paths::utf8(paths::io_path(install / "base.apk"));
    REQUIRE(mz_zip_writer_init_file(&zip, apk.c_str(), 0));
    struct Close { mz_zip_archive* zip; ~Close() { mz_zip_writer_end(zip); } } close{&zip};
    const std::array<std::uint32_t, 2> manifest{0x00080003, 8};
    REQUIRE(mz_zip_writer_add_mem(&zip, "AndroidManifest.xml", manifest.data(), sizeof(manifest), 0));
    elf::Header library{};
    std::memcpy(library.ident, "\177ELF\2\1\1", 7);
    library.machine = 183;
    REQUIRE(mz_zip_writer_add_mem(&zip, "lib/arm64-v8a/libfixture.so", &library, sizeof(library), 0));
    REQUIRE(mz_zip_writer_finalize_archive(&zip));
  }
  paths::atomic_write(install / "plan.json",
      R"({"package":"org.venture.passthroughconfig","versionCode":1,"files":[{"kind":"apk","name":"base.apk"}]})");

  for (const auto* value : {static_cast<const char*>(nullptr), "", "0", "true", " 1", "1 ", "01", "1"}) {
    INFO("VENTURE_PASSTHROUGH=", value ? value : "<unset>");
    const bool enabled = value && std::string_view(value) == "1";
    // Saved launcher settings must not override the child's explicit environment.
    paths::atomic_write(data_dir / "settings.json", enabled ? R"({"passthrough":false})" : R"({"passthrough":true})");
    opt_in.set(value);
    configure_game(install);
    CHECK(process_config().passthrough == enabled);
    opt_in.set(enabled ? "0" : "1");
    CHECK(process_config().passthrough == enabled);
  }
}
