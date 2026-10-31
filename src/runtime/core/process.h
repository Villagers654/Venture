// Process-wide configuration of the hosted game: identity and guest paths.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include "core/device_identity.h"
#include "core/guest.h"

namespace venture {
// The process id guest code observes (getpid, /proc, AT_* and thread groups).
inline constexpr int kGuestPid = 1000;

struct ProcessConfig {
  std::string package;          // com.example.game
  std::string version_name;
  std::int64_t version_code = 0;
  std::string source = "meta";  // launch-plan source, never inferred from guest code
  std::string abi = "arm64-v8a";   // "armeabi-v7a" selects an A32 process
  GuestIsa isa() const { return abi == "armeabi-v7a" ? GuestIsa::a32 : GuestIsa::a64; }
  int sdk_level = 32;           // reported ro.build.version.sdk (Quest 3: Android 12L / API 32)

  // Host locations.
  std::filesystem::path apk;           // downloaded base APK
  std::filesystem::path obb_dir;       // directory containing main.*.obb / patch.*.obb
  std::filesystem::path data_dir;      // per-game persistent storage root
  std::filesystem::path cache_dir;     // per-game cache root (extracted libs, translated code)
  std::filesystem::path native_lib_dir;  // extracted lib/<abi>/ directory

  // Guest-visible paths (Android conventions).
  std::string guest_apk_path() const { return "/data/app/" + package + "/base.apk"; }
  std::string guest_data_dir() const { return "/data/user/0/" + package; }
  std::string guest_native_lib_dir() const { return "/data/app/" + package + native_lib_suffix; }
  std::string guest_system_lib_dir() const { return system_lib_path; }
  std::string guest_obb_dir() const { return "/storage/emulated/0/Android/obb/" + package; }
  std::string guest_external_files_dir() const { return "/storage/emulated/0/Android/data/" + package + "/files"; }

  // System properties (__system_property_get) presented to the guest.
  std::map<std::string, std::string> properties;
  // Selected before guest execution and immutable for the lifetime of this launch.
  const DeviceIdentity* device_identity = &native_device_identity();
  // Chosen once by select_process_isa before HLE registration, so path users
  // do not introduce per-call ISA branches on A64.
  std::string native_lib_suffix = "/lib/arm64", system_lib_path = "/system/lib64";
  // Additive fields: guest writes never reach the shared extraction/conversion
  // cache.
  std::string account_key = "local";
  std::filesystem::path guest_cache_dir;
  bool platform_leaderboard_writes = true; // per-launch opt-out, owner-authorized by default
  bool embedded_java_scripts = false; // tooling agent needed only for prepared Java loaders
  bool has_code = true; // classes.dex present; a native-only APK (hasCode="false") has none
  // Saved launcher preference, loaded once before guest/XR initialization.
  bool steam_frame_passthrough = false;
};

ProcessConfig& process_config();
std::uint64_t guest_physical_memory(); // bytes, selected Quest device profile
std::uint64_t guest_usable_memory(); // profile RAM bounded by admitted guest backing

}  // namespace venture
