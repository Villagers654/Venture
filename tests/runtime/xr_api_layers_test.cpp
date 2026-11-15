#include "common/json.h"
#include "common/openxr_layers.h"
#include "common/paths.h"
#include "xr/host_xr.h"
#include <doctest/doctest.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace venture;

namespace {
#ifdef _WIN32
constexpr const char *kLibraryExtension = ".dll";
int process_id() { return int(GetCurrentProcessId()); }
#else
constexpr const char *kLibraryExtension = ".so";
int process_id() { return int(getpid()); }
#endif
void write_file(const fs::path &path, const std::string &text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
}
// An implicit layer manifest as installers write them.
void write_manifest(const fs::path &path, const std::string &name, const std::string &library,
                    const Json &functions = Json()) {
  Json layer{{"name", name},
             {"library_path", library},
             {"api_version", "1.0"},
             {"implementation_version", "1"},
             {"description", "Venture test layer"},
             {"disable_environment", "DISABLE_" + name}};
  if (!functions.is_null()) layer["functions"] = functions;
  write_file(path, Json{{"file_format_version", "1.0.0"}, {"api_layer", layer}}.dump(2));
}

#ifdef _WIN32
// The loader reads HKLM, then HKCU. Both are remapped, for this process only,
// to keys under HKCU\Software\Venture\Tests: HKCU to one that lists the
// test's layers, HKLM to an empty one so the PC's own layers stay out (the
// mock runtime comes from XR_RUNTIME_JSON). The user's registry stays as it
// is and no other OpenXR program sees these layers. Values keep their order:
// the first ones load first.
class LayerRegistry {
 public:
  LayerRegistry(const std::vector<fs::path> &first, const std::vector<fs::path> &later)
      : root_(L"Software\\Venture\\Tests\\openxr-layers-" + std::to_wstring(process_id())) {
    HKEY list{};
    REQUIRE(RegCreateKeyExW(HKEY_CURRENT_USER, (root_ + L"\\SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit").c_str(),
                            0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &list, nullptr) == ERROR_SUCCESS);
    for (const auto *manifests : {&first, &later})
      for (const auto &manifest : *manifests) {
        const DWORD enabled = 0;
        RegSetValueExW(list, manifest.wstring().c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE *>(&enabled),
                       sizeof(enabled));
      }
    RegCloseKey(list);
    REQUIRE(RegCreateKeyExW(HKEY_CURRENT_USER, (root_ + L"\\machine").c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr,
                            &machine_, nullptr) == ERROR_SUCCESS);
    REQUIRE(RegOpenKeyExW(HKEY_CURRENT_USER, root_.c_str(), 0, KEY_ALL_ACCESS, &user_) == ERROR_SUCCESS);
    REQUIRE(RegOverridePredefKey(HKEY_LOCAL_MACHINE, machine_) == ERROR_SUCCESS);
    REQUIRE(RegOverridePredefKey(HKEY_CURRENT_USER, user_) == ERROR_SUCCESS);
  }
  ~LayerRegistry() {
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
    RegCloseKey(user_);
    RegCloseKey(machine_);
    RegDeleteTreeW(HKEY_CURRENT_USER, root_.c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Venture\\Tests");  // when no other run's key is left
  }

 private:
  std::wstring root_;
  HKEY user_{}, machine_{};
};
int layer_instances(const fs::path &library) {
  HMODULE module = GetModuleHandleW(library.wstring().c_str());
  auto count = module ? reinterpret_cast<int (*)()>(GetProcAddress(module, "ventureTestLayerInstances")) : nullptr;
  return count ? count() : -1;
}
#else
// The loader searches XDG_CONFIG_DIRS before XDG_DATA_HOME; XDG_DATA_DIRS
// points at an empty dir so the system's own layers there stay out.
class LayerRegistry {
 public:
  LayerRegistry(const std::vector<fs::path> &first, const std::vector<fs::path> &later) {
    const auto root = fs::temp_directory_path() / ("venture-xr-layer-dirs-" + std::to_string(process_id()));
    place(root / "config", first);
    place(root / "data", later);
    setenv("XDG_CONFIG_DIRS", (root / "config").c_str(), 1);
    setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
    fs::create_directories(root / "none");
    setenv("XDG_DATA_DIRS", (root / "none").c_str(), 1);
  }

 private:
  static void place(const fs::path &base, const std::vector<fs::path> &manifests) {
    const auto dir = base / "openxr/1/api_layers/implicit.d";
    fs::create_directories(dir);
    for (const auto &manifest : manifests) {
      std::error_code ec;
      if (fs::exists(manifest, ec)) fs::copy_file(manifest, dir / manifest.filename());
      else fs::create_symlink(manifest, dir / manifest.filename(), ec);  // a dangling registration
    }
  }
};
int layer_instances(const fs::path &library) {
  void *module = dlopen(library.c_str(), RTLD_LAZY | RTLD_NOLOAD);
  if (!module) return -1;
  auto count = reinterpret_cast<int (*)()>(dlsym(module, "ventureTestLayerInstances"));
  const int result = count ? count() : -1;
  dlclose(module);
  return result;
}
#endif
}  // namespace

TEST_CASE("OpenXR layer manifests are read as the loader reads them") {
  const auto dir = fs::temp_directory_path() / ("venture-xr-manifest-" + std::to_string(process_id()));
  fs::create_directories(dir);
  // jsoncpp, the loader's parser, takes comments; renamed entry points and a
  // library relative to the manifest.
  auto layer = openxr_layers::parse(dir / "a.json", R"({
    // installed by a test
    "file_format_version": "1.0.0",
    "api_layer": {"name": "XR_APILAYER_test", "library_path": "./bin/test.dll", "api_version": "1.0",
                  "implementation_version": "1", "disable_environment": "DISABLE_TEST",
                  "enable_environment": "ENABLE_TEST",
                  "functions": {"xrNegotiateLoaderApiLayerInterface": "TestNegotiate"}}})");
  CHECK(layer.problem.empty());
  CHECK(layer.name == "XR_APILAYER_test");
  CHECK(layer.library == (dir / "bin/test.dll").lexically_normal());
  CHECK(layer.disable_environment == "DISABLE_TEST");
  CHECK(layer.enable_environment == "ENABLE_TEST");
  CHECK(layer.functions.at("xrNegotiateLoaderApiLayerInterface") == "TestNegotiate");
  // A bare name is found through the library search path.
  CHECK(openxr_layers::parse(dir / "b.json", R"({"file_format_version":"1.0.0","api_layer":{"name":"x",
      "library_path":"x.dll","api_version":"1.0","implementation_version":"1","disable_environment":"D"}})")
            .library == fs::path("x.dll"));
  CHECK(openxr_layers::parse(dir / "c.json", "not json").problem == "the manifest is not valid JSON");
  CHECK(openxr_layers::parse(dir / "d.json", R"({"file_format_version":"1.0.0","api_layer":{"name":"x",
      "library_path":"x.dll","api_version":"1.0","implementation_version":"1"}})")
            .problem == "the manifest lacks \"disable_environment\"");
  CHECK(openxr_layers::read(dir / "missing.json").problem == "the manifest is missing");
  // Library files: missing, and not a library of this process.
  write_file(dir / "junk.bin", "not a library\n");
  CHECK(openxr_layers::library_problem(dir / "none.bin").ends_with(" is missing"));
  CHECK(openxr_layers::library_problem(dir / "junk.bin").find("is not a") != std::string::npos);
  write_manifest(dir / "junk.json", "XR_APILAYER_junk", "./junk.bin");
  CHECK(openxr_layers::read(dir / "junk.json").problem.find("is not a") != std::string::npos);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// Run on its own (ctest xr_api_layer_skip_*): the host instance is created
// once per process. VENTURE_TEST_XR_WORKING_LAYER picks the registrations:
//   0: only broken ones: a missing manifest, a file that is not a library, and
//      a bare library name that names no DLL anywhere (the loader cannot check
//      that one before it loads it). No layer loads, so the loader fails
//      xrCreateInstance with XR_ERROR_FILE_ACCESS_ERROR (the Haymaker report:
//      one implicit layer whose DLL did not load).
//   1: a working layer first, then the broken ones and a layer whose
//      negotiation entry point is missing; the loader fails on that one
//      (XR_ERROR_API_LAYER_NOT_PRESENT) because no layer loads after it.
//   2: a working layer first, then the broken ones and a layer that loads
//      and fails every xrCreateInstance itself.
TEST_CASE("The host OpenXR instance leaves out only the implicit layers that fail to load" * doctest::skip()) {
  const char *mode_text = std::getenv("VENTURE_TEST_XR_WORKING_LAYER");
  const int mode = mode_text ? std::atoi(mode_text) : 0;
  const bool working = mode != 0;
  const fs::path layer_library = paths::from_utf8(VENTURE_TEST_XR_LAYER);
  const auto dir = fs::temp_directory_path() / ("venture-xr-layers-" + std::to_string(process_id()));
  fs::create_directories(dir);
  write_file(dir / (std::string("unloadable_layer") + kLibraryExtension), "not a library\n");
  write_manifest(dir / "unloadable.json", "XR_APILAYER_VENTURE_test_unloadable",
                 paths::utf8(dir / (std::string("unloadable_layer") + kLibraryExtension)));
  write_manifest(dir / "missing_library.json", "XR_APILAYER_VENTURE_test_missing_library",
                 std::string("venture_no_such_layer_library") + kLibraryExtension);
  write_manifest(dir / "no_entry.json", "XR_APILAYER_VENTURE_test_no_entry", paths::utf8(layer_library),
                 Json{{"xrNegotiateLoaderApiLayerInterface", "ventureNoSuchEntryPoint"}});
  write_manifest(dir / "failing.json", "XR_APILAYER_VENTURE_test_failing", paths::utf8(layer_library),
                 Json{{"xrNegotiateLoaderApiLayerInterface", "ventureTestFailingNegotiate"}});
  write_manifest(dir / "working.json", "XR_APILAYER_VENTURE_test_working", paths::utf8(layer_library));
  std::vector<fs::path> first, later{dir / "missing.json", dir / "unloadable.json", dir / "missing_library.json"};
  if (working) first.push_back(dir / "working.json");
  if (mode == 1) later.push_back(dir / "no_entry.json");
  if (mode == 2) later.push_back(dir / "failing.json");
  {
    LayerRegistry registry(first, later);
    // The report's view of the same registrations.
#ifdef _WIN32
    const auto manifests = openxr_layers::implicit_manifests();
    CHECK(std::ranges::find(manifests, dir / "missing.json") != manifests.end());
#endif
    CHECK(openxr_layers::read(dir / "unloadable.json").problem.find("is not a") != std::string::npos);

    auto &host = xr::host_xr();
    CHECK(host.error == "");
    CHECK(host.available);
    CHECK(host.instance != nullptr);
    const auto &skipped = host.skipped_api_layers;
    auto skipped_layer = [&](const char *name) { return std::ranges::find(skipped, name) != skipped.end(); };
    CHECK(skipped_layer("XR_APILAYER_VENTURE_test_unloadable"));
    CHECK(skipped_layer("XR_APILAYER_VENTURE_test_missing_library"));
    CHECK_FALSE(skipped_layer("XR_APILAYER_VENTURE_test_working"));
    CHECK(skipped.size() == std::size_t(mode == 0 ? 2 : 3));
    if (mode == 1) {
      CHECK(skipped_layer("XR_APILAYER_VENTURE_test_no_entry"));
      // The working layer is in the instance, the only one created.
      CHECK(layer_instances(layer_library) == 1);
    }
    if (mode == 2) {
      CHECK(skipped_layer("XR_APILAYER_VENTURE_test_failing"));
      // The working layer is in the instance. The loader unloads the library
      // with the instance that found it works, so its count starts again.
      CHECK(layer_instances(layer_library) == 1);
    }
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}
