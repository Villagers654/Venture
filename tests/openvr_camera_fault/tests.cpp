// The OpenVR camera survives a fault inside the OpenVR client: the camera is
// given up, the game goes on, and a later run skips the camera until the
// faulting file changes. Two processes share one data directory: the first
// faults, the second reads the record (ctest runs them in order).
#include "common/json.h"
#include "common/paths.h"
#include "xr/tracked_camera.h"
#include <doctest/doctest.h>
#include <windows.h>
#include <filesystem>

using namespace venture;
namespace fs = std::filesystem;

namespace {
const fs::path data = fs::path(VENTURE_OPENVR_FAULT_DATA);
const fs::path client = data / "client" / "vrclient_x64.dll";
// Loads a copy of the stand-in client, so the second process can change it.
void load_client(bool fresh) {
  _wputenv_s(L"VENTURE_DATA_DIR", data.c_str());
  if (fresh) {
    fs::remove_all(data);
    fs::create_directories(client.parent_path());
    fs::copy_file(fs::path(VENTURE_FAKE_VRCLIENT), client);
  }
  REQUIRE(LoadLibraryW(client.c_str()));
}
} // namespace

TEST_CASE("OpenVR camera: a fault in the client gives up the camera and is recorded") {
  load_client(true);
  REQUIRE(xr::tracked_camera_present());
  CHECK(xr::open_tracked_camera(100) == nullptr);
  CHECK_FALSE(xr::tracked_camera_present());
  const auto record = parse_json(paths::read(data / "openvr-camera-fault.json"));
  CHECK(record.value("call", std::string{}) == "AcquireVideoStreamingService");
  CHECK(fs::equivalent(paths::from_utf8(record.value("module", std::string{})), client));
  CHECK_FALSE(record.value("module_identity", std::string{}).empty());
}

TEST_CASE("OpenVR camera: a later run skips the camera until the faulting file changes") {
  load_client(false);
  REQUIRE(fs::exists(data / "openvr-camera-fault.json"));
  CHECK_FALSE(xr::tracked_camera_present());
  // Stand in for a driver update before the next run: the recorded identity no
  // longer matches the installed file.
  auto record = parse_json(paths::read(data / "openvr-camera-fault.json"));
  record["module_identity"] = "0:0";
  paths::atomic_write(data / "openvr-camera-fault.json", record.dump());
}

TEST_CASE("OpenVR camera: a changed faulting file is tried again") {
  load_client(false);
  CHECK(xr::tracked_camera_present());
}
