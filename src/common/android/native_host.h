#pragma once
// How a Quest game runs on another headset's own Android (a native host):
// the game's code runs natively under the device's ART and the device keeps
// its OpenXR runtime, input, keyboard, audio and passthrough. Venture supplies
// only what is Meta-specific and missing there. See docs/design/native-headsets.md.
//
// Everything here is decided from capabilities: what the APK contains and
// what the headset reports about itself. Nothing names a vendor, model or title.
#include "common/android/manifest.h"
#include "common/paths.h"

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace venture::android::native_host {
// What an APK brings that decides how it runs on a native host.
struct ApkProfile {
  std::string package;
  std::int32_t min_sdk = 0, target_sdk = 0;
  // ABI -> the native libraries (file names) of its lib/<abi>/ slice.
  std::map<std::string, std::vector<std::string>> slices;
  // Categories of the MAIN intent filters on real <activity> elements, and on
  // <activity-alias> elements (which some shells do not start), sorted.
  std::vector<std::string> activity_categories, alias_categories;
  std::vector<std::string> required_features, native_libraries;
  // ABI -> libraries of that slice that are Meta's platform loader under
  // another name (an SDK's renamed copy), found by what they export.
  std::map<std::string, std::vector<std::string>> platform_copies;
};
ApkProfile profile(const Manifest& manifest, std::span<const std::string> entry_names);
ApkProfile profile(const fs::path& apk);

// What the headset reports about itself, probed on the device by Venture's
// companion (Build.SUPPORTED_ABIS, SDK_INT, the runtime's negotiated version
// and xrEnumerateInstanceExtensionProperties, its installed services).
struct HostProfile {
  std::vector<std::string> abis;  // preferred first
  std::int32_t android_api = 0;
  std::uint32_t xr_api_major = 1, xr_api_minor = 0;  // highest version the runtime accepts
  std::map<std::string, std::uint32_t> xr_extensions;
  bool vrapi = false;             // the device has a VrApi implementation (Meta's runtime stack)
  bool platform_service = false;  // the device has the Oculus Platform service
  // Manifest markers the device's shell needs to start an app in VR: intent
  // categories on the launch activity, and <application> meta-data.
  std::vector<std::string> launch_categories;
  std::vector<std::pair<std::string, std::string>> launch_metadata;
};

enum class Execution {
  native,          // the game's own slice runs on the device's CPU
  translated_a32,  // an ARM32-only game on an AArch64-only CPU: Venture's A32 translator
  unsupported,
};
// A game library Venture's build stands in for on this host.
enum class Supply {
  vrapi,          // VrApi on the device's OpenXR runtime
  openxr_loader,  // the Khronos Android loader, which finds the device's runtime
  platform,       // the Oculus Platform SDK on Venture's Meta session
  // A renamed copy of the platform loader: an alias that loads Venture's
  // platform loader, so the game has one Platform instance however it names it.
  platform_alias,
};
struct Replacement {
  std::string library;  // file name in the installed slice
  Supply with;
};
struct Plan {
  Execution execution = Execution::unsupported;
  std::string abi;  // the slice installed; empty for a game without native code
  std::vector<Replacement> replace;
  bool quest_layer = false;  // Venture's implicit OpenXR API layer is added
  // Quest extensions the device's runtime lacks that the layer answers.
  std::vector<std::string> layer_extensions;
  bool lower_api_version = false;  // the runtime accepts only OpenXR 1.0: the layer serves 1.1 games on it
  bool meta_java = false;          // Meta's Java classes (horizonos/metavr Build, com.oculus.*) are added
  // The platform loader is Venture's: the manifest declares its start-up
  // provider, lets the game see and call the Venture app, and use the network.
  bool platform_broker = false;
  std::vector<std::string> add_categories;
  std::vector<std::pair<std::string, std::string>> set_metadata;
  std::vector<std::string> blockers;  // why it cannot run; empty when it can
};
Plan plan(const ApkProfile& apk, const HostProfile& host);

// Applies a plan's manifest edits to a compiled AndroidManifest.xml before the
// system installs the game: its categories go on the launch activity's MAIN
// filter (the first <activity> with one, else the target of the first launch
// alias), and its <application> meta-data is set. With platform_broker, the
// Platform start-up provider, a <queries> entry for the Venture app and the
// INTERNET permission are added. When Venture supplies the Khronos loader
// (VrApi or libopenxr_loader.so), the runtime brokers are queried and the
// OpenXR permissions requested. Throws when the manifest has no activity to
// launch.
std::vector<std::uint8_t> adapt_manifest(std::span<const std::uint8_t> xml, const Plan& plan);

// Names the adapted game and the Venture app share.
inline constexpr std::string_view kVenturePackage = "org.venture_emu.venture";
inline constexpr std::string_view kPlatformProvider = "org.venture_emu.game.PlatformInit";
inline constexpr std::string_view kPlatformLoader = "libovrplatformloader.so";
// The Khronos Android loader's runtime brokers and permissions (OpenXR
// loader specification, Android): a game whose loader Venture supplies
// declares them.
inline constexpr std::string_view kRuntimeBroker = "org.khronos.openxr.runtime_broker";
inline constexpr std::string_view kRuntimeBrokers =
    "org.khronos.openxr.runtime_broker;org.khronos.openxr.system_runtime_broker";
inline constexpr std::string_view kOpenXrPermission = "org.khronos.openxr.permission.OPENXR";
inline constexpr std::string_view kOpenXrSystemPermission = "org.khronos.openxr.permission.OPENXR_SYSTEM";
// Whether a library's exports make it Meta's platform loader.
bool is_platform_loader(std::span<const std::string> exports);
}  // namespace venture::android::native_host
