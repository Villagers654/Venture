#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace venture {
struct DeviceIdentity {
  std::string_view manifest_name, legacy_name, model, device, system_name;
  std::int32_t vrapi_type;
  // Guest renderer identity; API versions, extensions and limits remain capability queries.
  std::string_view gpu_renderer;
  // XR_META_headset_id XrUuidEXT bytes; OVRPlugin classifies the headset by them
  // and falls back to a systemName table that predates Quest 3.
  std::array<std::uint8_t, 16> headset_id;
};
const DeviceIdentity& native_device_identity();
const DeviceIdentity& select_device_identity(std::string_view supported_devices);
// Original APK binary XML, not Store build targeting or translated metadata.
std::string manifest_supported_devices(std::span<const std::byte> xml);
} // namespace venture
