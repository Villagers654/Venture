#include "launcher/obs_mirror.h"
#include "common/json.h"
#include <doctest/doctest.h>
#include <chrono>
#include <fstream>

using namespace venture;
using namespace venture::launcher;
namespace {
struct Folder {
  fs::path root = fs::temp_directory_path() / ("venture-obs-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  Folder() { fs::create_directories(root); }
  ~Folder() { std::error_code error; fs::remove_all(root, error); }
};
fs::path layer(const fs::path& folder, std::string_view file, std::string_view name, std::string_view library,
               std::string_view version = "1") {
  fs::create_directories(folder);
  const auto manifest = folder / file;
  paths::atomic_write(manifest, Json{{"file_format_version", "1.0.0"},
                                     {"api_layer", {{"name", name},
                                                    {"library_path", library},
                                                    {"api_version", "1.0"},
                                                    {"implementation_version", version},
                                                    {"disable_environment", "DISABLE_X"}}}}
                                    .dump());
  return manifest;
}
void touch(const fs::path& path) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << "x";
}
}  // namespace

TEST_CASE("OBS Mirror is found among the implicit OpenXR layers") {
  Folder folder;
  const auto obs = folder.root / "OpenXR-OBSMirror";
  touch(obs / "XR_APILAYER_NOVENDOR_OBSMirror.dll");
  const auto manifest = layer(obs, "XR_APILAYER_NOVENDOR_OBSMirror.json", "XR_APILAYER_NOVENDOR_OBSMirror",
                              "XR_APILAYER_NOVENDOR_OBSMirror.dll", "0.3.0");
  const auto other = layer(folder.root / "other", "other.json", "XR_APILAYER_TEST_other", "missing.dll");

  CHECK(find_obs_mirror({}).state == ObsMirror::State::missing);
  CHECK(find_obs_mirror({{other, false}}).state == ObsMirror::State::missing);

  auto found = find_obs_mirror({{other, false}, {manifest, false}});
  CHECK(found.state == ObsMirror::State::ready);  // relative library beside its manifest
  CHECK(found.manifest == manifest);
  CHECK(found.version == "0.3.0");

  CHECK(find_obs_mirror({{manifest, true}}).state == ObsMirror::State::disabled);
  // A machine copy turned off and a user copy on: recording works.
  CHECK(find_obs_mirror({{manifest, true}, {manifest, false}}).state == ObsMirror::State::ready);

  // Registered but its library is gone (uninstalled): not ready.
  fs::remove(obs / "XR_APILAYER_NOVENDOR_OBSMirror.dll");
  CHECK(find_obs_mirror({{manifest, false}}).state == ObsMirror::State::missing);
  // A manifest with a UTF-8 BOM (PowerShell installers) still reads.
  touch(obs / "XR_APILAYER_NOVENDOR_OBSMirror.dll");
  const auto text = paths::read(manifest);
  paths::atomic_write(manifest, "\xEF\xBB\xBF" + text);
  CHECK(find_obs_mirror({{manifest, false}}).state == ObsMirror::State::ready);
  // An unreadable manifest is skipped.
  CHECK(find_obs_mirror({{folder.root / "absent.json", false}}).state == ObsMirror::State::missing);
}
