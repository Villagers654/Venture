#pragma once
#include "common/paths.h"
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace venture::android {
// The identity an APK declares in its binary AndroidManifest.xml.
struct Manifest {
  std::string package, version_name;
  std::int64_t version_code = 0;  // versionCodeMajor in the upper 32 bits
  std::string label;              // a literal android:label; read_apk also resolves a label resource
  std::uint32_t label_id = 0;     // android:label as a resource reference (resources.arsc)
  std::uint32_t banner_id = 0, icon_id = 0;  // <application> android:banner and android:icon
  std::vector<std::string> required_features;  // <uses-feature> names whose android:required is true
};
// AOSP ResourceTypes.h binary XML; throws for malformed input or a manifest
// without a package.
Manifest parse_manifest(std::span<const std::uint8_t> xml);
Manifest read_apk(const fs::path &apk);
Manifest read_apk(std::span<const std::uint8_t> apk);  // an APK in memory
// resources.arsc lookups as Android resolves them for an en-US device. Both
// follow references and return an empty string for missing or odd entries.
std::string resource_string(std::span<const std::uint8_t> arsc, std::uint32_t id);
std::string resource_file(std::span<const std::uint8_t> arsc, std::uint32_t id);  // highest-density PNG/JPEG/WebP
// How the home screen presents an app: its label, banner and icon image bytes,
// each empty when the APK declares none.
struct Presentation {
  std::string label;
  std::vector<std::uint8_t> banner, icon;
};
Presentation read_presentation(const fs::path &apk);
}  // namespace venture::android
