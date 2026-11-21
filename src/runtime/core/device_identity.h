#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace venture {
struct DeviceIdentity {
  std::string_view manifest_name, legacy_name, model, device, system_name;
  std::int32_t vrapi_type;
  // Guest renderer identity; API versions, extensions and limits remain capability queries.
  std::string_view gpu_renderer;
  // XR_META_headset_id XrUuidEXT bytes; OVRPlugin classifies the headset by them
  // and falls back to a systemName table that predates Quest 3.
  std::array<std::uint8_t, 16> headset_id;
  // A supportedDevices value Meta's early documentation used for the model.
  std::string_view early_name{};
};
const DeviceIdentity& native_device_identity();
const DeviceIdentity& select_device_identity(std::string_view supported_devices);
// Original APK binary XML, not Store build targeting or translated metadata.
std::string manifest_supported_devices(std::span<const std::byte> xml);
// Each element of an APK's binary manifest, with its ancestors' tags and its
// string attributes by name (nullopt when absent or not a string); returning
// false stops the walk.
using ManifestAttribute = std::function<std::optional<std::string>(std::string_view)>;
using ManifestVisitor = std::function<bool(std::string_view tag, const std::vector<std::string>& parents,
                                           const ManifestAttribute& attribute)>;
void visit_manifest(std::span<const std::byte> xml, const ManifestVisitor& visit);
// Whether the activity an APK launches with is a 2D one: none of its intent
// filters names Quest's VR or OpenXR's immersive category (docs/design/layoutlib.md).
// With an activity name (as the manifest or an intent's component names it),
// whether that activity is 2D instead; an activity the manifest lacks answers
// as the launch activity does.
bool manifest_launches_2d(std::span<const std::byte> xml, std::string_view activity = {});
} // namespace venture
