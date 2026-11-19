#include "common/environment.h"
#include <doctest/doctest.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
namespace {
void set(const char* key, const char* value) {
#ifdef _WIN32
  _putenv_s(key, value ? value : "");
  // Unlike _putenv_s, the process environment can represent an explicitly empty value.
  SetEnvironmentVariableA(key, value);
#else
  if (value) setenv(key,value,1); else unsetenv(key);
#endif
}
struct Fixture {
  const char* key = "VENTURE_ENV_TEST_VALUE";
  Fixture() { set(key,nullptr); venture::environment::refresh(); }
  ~Fixture() { set(key,nullptr); venture::environment::refresh(); }
};
}
TEST_CASE("host environment concurrent readers survive snapshot refresh") {
  Fixture f; set(f.key,"first"); venture::environment::refresh();
  const char* first = venture::environment::getenv(f.key);
  std::atomic<bool> go{false}, bad{false};
  std::vector<std::thread> readers;
  for (int i=0;i<16;++i) readers.emplace_back([&]{
    while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
    for (int j=0;j<10000;++j) {
      auto* p=venture::environment::getenv(f.key);
      if (!p || (std::string_view(p)!="first" && std::string_view(p)!="second")) bad=true;
      if (venture::environment::getenv("VENTURE_ENV_MISSING_TEST")) bad=true;
    }
  });
  go.store(true,std::memory_order_release);
  for (int i=0;i<20;++i) { set(f.key,i%2 ? "first":"second"); venture::environment::refresh(); }
  for (auto& thread:readers) thread.join();
  CHECK_FALSE(bad.load());
  CHECK(std::string_view(first)=="first");
}
#ifdef _WIN32
TEST_CASE("host environment values are UTF-8 whatever the ANSI code page") {
  Fixture f;
  // A Turkish user folder: S-cedilla and u-umlaut are outside most ANSI code pages' ASCII.
  SetEnvironmentVariableW(L"VENTURE_ENV_TEST_VALUE", L"C:\\Users\\\u015e\u00fckr\u00fc\\bin");
  venture::environment::refresh();
  const char* value = venture::environment::getenv(f.key);
  REQUIRE(value);
  CHECK(std::string_view(value) == "C:\\Users\\\xC5\x9E\xC3\xBCkr\xC3\xBC\\bin");
}
#endif
namespace {
// FLATPAK_ID and XR_RUNTIME_JSON for one check, restored after.
struct FlatpakEnv {
  static constexpr const char* keys[2] = {"FLATPAK_ID", "XR_RUNTIME_JSON"};
  std::string saved[2];
  bool had[2]{};
  FlatpakEnv(const char* id, const std::string& manifest) {
    for (int i = 0; i < 2; ++i) {
      const char* v = std::getenv(keys[i]);
      had[i] = v != nullptr;
      saved[i] = v ? v : "";
    }
    set("FLATPAK_ID", id);
    set("XR_RUNTIME_JSON", manifest.empty() ? nullptr : manifest.c_str());
    venture::environment::refresh();
  }
  ~FlatpakEnv() {
    for (int i = 0; i < 2; ++i) set(keys[i], had[i] ? saved[i].c_str() : nullptr);
    venture::environment::refresh();
  }
};
struct TempDir {
  std::filesystem::path root = std::filesystem::temp_directory_path() /
      ("venture-flatpak-runtime-" +std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TempDir() { std::filesystem::create_directories(root); }
  ~TempDir() { std::error_code ec; std::filesystem::remove_all(root, ec); }
};
void write(const std::filesystem::path& file, const std::string& text) {
  std::filesystem::create_directories(file.parent_path());
  std::ofstream(file, std::ios::binary) << text;
}
}  // namespace
TEST_CASE("Flatpak SteamVR: a folder outside the sandbox is named for flatpak override") {
  TempDir dir;
  const auto steamvr = dir.root / "library/steamapps/common/SteamVR";
  const auto manifest = steamvr / "steamxr_linux64.json";
  const std::string steamvr_manifest = R"({"runtime":{"name":"SteamVR","library_path":"bin/linux64/vrclient.so"}})";
  SUBCASE("outside the Flatpak nothing applies") {
    FlatpakEnv env(nullptr, manifest.string());
    CHECK_FALSE(venture::environment::flatpak_steamvr_state().steamvr);
    CHECK_FALSE(venture::environment::flatpak_steamvr());
  }
  SUBCASE("a manifest the sandbox lacks: its folder") {
    FlatpakEnv env("org.venture_emu.Venture", manifest.string());
    const auto state = venture::environment::flatpak_steamvr_state();
    CHECK(state.steamvr);
    CHECK_FALSE(state.visible);
    CHECK(std::filesystem::path(state.hidden_folder) == std::filesystem::path(steamvr.generic_string()));
    CHECK(venture::environment::flatpak_steamvr());
  }
  SUBCASE("a visible manifest whose relative client library is missing: SteamVR's folder") {
    write(manifest, steamvr_manifest);
    FlatpakEnv env("org.venture_emu.Venture", manifest.string());
    const auto state = venture::environment::flatpak_steamvr_state();
    CHECK_FALSE(state.visible);
    std::error_code ec;
    CHECK(std::filesystem::equivalent(state.hidden_folder, steamvr, ec));
  }
  SUBCASE("manifest and library visible: no warning") {
    write(manifest, steamvr_manifest);
    write(steamvr / "bin/linux64/vrclient.so", "");
    FlatpakEnv env("org.venture_emu.Venture", manifest.string());
    const auto state = venture::environment::flatpak_steamvr_state();
    CHECK(state.steamvr);
    CHECK(state.visible);
    CHECK(state.hidden_folder.empty());
    CHECK_FALSE(venture::environment::flatpak_steamvr());
  }
  SUBCASE("another runtime: nothing applies") {
    const auto wivrn = dir.root / "openxr_wivrn.json";
    write(wivrn, R"({"runtime":{"name":"WiVRn","library_path":"/nonexistent/libopenxr_wivrn.so"}})");
    FlatpakEnv env("org.venture_emu.Venture", wivrn.string());
    CHECK_FALSE(venture::environment::flatpak_steamvr_state().steamvr);
  }
  SUBCASE("the override command quotes the folder for the shell") {
    FlatpakEnv env("org.venture_emu.Venture", "");
    CHECK(venture::environment::flatpak_override_command("/run/media/deck/SD card/it's/SteamVR") ==
          "flatpak override --user --filesystem='/run/media/deck/SD card/it'\\''s/SteamVR':ro org.venture_emu.Venture");
  }
}
