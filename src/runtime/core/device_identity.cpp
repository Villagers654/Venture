#include "core/device_identity.h"
#include <array>
#include <stdexcept>
#include <vector>

namespace venture {
namespace {
// OpenXR names: Meta's OVRPlugin Android SDK string classifier. Android device
// names: Meta firmware product names (monterey/hollywood/seacliff/eureka).
// VrApi_Types.h, Oculus Mobile SDK 1.50: Quest=256+3, Quest 2=320.
// Its public device enum cannot represent Quest Pro/3: UNKNOWN=-1, not Quest 2.
constexpr std::array devices{
    // Meta's GPU pipeline/render-stage documentation identifies 540/650/740
    // for Quest/XR2/XR2 Gen 2. Keep the Android GL_RENDERER spelling used by
    // UE4's BaseDeviceProfiles.ini; host ANGLE names select Android_Default.
    // https://developers.meta.com/vr/documentation/native/android/po-advanced-gpu-pipelines/
    // https://developers.meta.com/horizon/documentation/unreal/ts-renderdoc-renderstage/
    // Headset ids: the UUID constants OVRPlugin's headset classifier compares
    // against (Quest, Quest 2, Quest Pro, Quest 3), identical across its builds.
    DeviceIdentity{"quest", "monterey", "Quest", "monterey", "Oculus Quest", 259, "Adreno (TM) 540",
                   {0x4c, 0xa2, 0xf7, 0x94, 0xaf, 0x6a, 0x46, 0x9d, 0xbb, 0xe5, 0x1f, 0x7f, 0xce, 0xef, 0x7b, 0xad}},
    DeviceIdentity{"quest2", "hollywood", "Quest 2", "hollywood", "Oculus Quest2", 320, "Adreno (TM) 650",
                   {0x30, 0x64, 0xf2, 0x39, 0x15, 0xed, 0x47, 0x22, 0xa6, 0x59, 0x5e, 0xa1, 0xec, 0xd8, 0x99, 0xc2}},
    DeviceIdentity{"questpro", "cambria", "Quest Pro", "seacliff", "Meta Quest Pro", -1, "Adreno (TM) 650",
                   {0xb5, 0x84, 0x43, 0xc9, 0xdf, 0x86, 0x40, 0xcd, 0x89, 0x18, 0x2a, 0x27, 0x8e, 0x18, 0xab, 0x71}},
    DeviceIdentity{"quest3", "eureka", "Quest 3", "eureka", "Oculus Quest 3", -1, "Adreno (TM) 740",
                   {0x08, 0x27, 0x46, 0xba, 0xa0, 0x71, 0x4c, 0xf7, 0x8f, 0x87, 0xba, 0xde, 0x5c, 0xce, 0x43, 0xce}}};
}
const DeviceIdentity& native_device_identity() { return devices.back(); }
const DeviceIdentity& select_device_identity(std::string_view supported) {
  auto lists = [&](const DeviceIdentity& device) {
    auto rest = supported;
    while (!rest.empty()) {
      auto end = rest.find('|');
      auto name = rest.substr(0, end);
      if (name == device.manifest_name || name == device.legacy_name)
        return true;
      if (end == rest.npos) break;
      rest.remove_prefix(end + 1);
    }
    return false;
  };
  // Meta Horizon OS, "Compatibility mode":
  // "The runtime then chooses the most recent older Quest model that the app lists."
  // https://developers.meta.com/vr/documentation/unity/os-compatibility-mode/
  // A listed native model keeps its identity; a missing entry falls back to Quest.
  for (auto i = devices.rbegin(); i != devices.rend(); ++i)
    if (lists(*i)) return *i;
  return devices.front();
}

std::string manifest_supported_devices(std::span<const std::byte> xml) {
  // AOSP android-12.1 ResourceTypes.h: ResChunk_header, ResStringPool_header,
  // ResXMLTree_node/attrExt/attribute; all offsets are relative to their chunk.
  auto u16 = [&](std::size_t at) -> std::uint16_t {
    if (at > xml.size() || xml.size() - at < 2)
      throw std::runtime_error("truncated APK manifest");
    return std::to_integer<unsigned>(xml[at]) | (std::to_integer<unsigned>(xml[at+1]) << 8);
  };
  auto u32 = [&](std::size_t at) -> std::uint32_t {
    return u16(at) | (std::uint32_t(u16(at+2)) << 16);
  };
  if (u16(0) != 3 || u16(2) != 8 || u32(4) != xml.size())
    throw std::runtime_error("invalid APK binary manifest");
  std::size_t pool = 0, pool_end = 0, strings = 0;
  std::vector<std::uint32_t> offsets;
  bool utf8 = false;
  auto string = [&](std::uint32_t index) -> std::string {
    if (index == 0xffffffff) return {};
    if (index >= offsets.size() || offsets[index] >= pool_end - strings)
      throw std::runtime_error("invalid APK manifest string index");
    std::size_t at = strings + offsets[index];
    auto length = [&]() -> std::uint32_t {
      if (at > pool_end || pool_end - at < (utf8 ? 1u : 2u))
        throw std::runtime_error("truncated APK manifest string");
      std::uint32_t n = utf8 ? std::to_integer<unsigned>(xml[at++]) : u16(at);
      if (!utf8) at += 2;
      unsigned mask = utf8 ? 0x80 : 0x8000;
      if (n & mask) {
        if (pool_end - at < (utf8 ? 1u : 2u)) throw std::runtime_error("truncated string length");
        n = ((n & (mask-1)) << (utf8 ? 8 : 16)) |
            (utf8 ? std::to_integer<unsigned>(xml[at++]) : u16(at));
        if (!utf8) at += 2;
      }
      return n;
    };
    auto n = length();
    if (utf8) n = length(); // UTF-16 character count precedes UTF-8 byte count.
    std::size_t bytes = std::size_t(n) * (utf8 ? 1 : 2);
    if (at > pool_end || bytes + (utf8 ? 1 : 2) > pool_end - at)
      throw std::runtime_error("truncated APK manifest string data");
    if (utf8) {
      if (xml[at+bytes] != std::byte{}) throw std::runtime_error("unterminated manifest string");
      return {reinterpret_cast<const char*>(xml.data()+at), bytes};
    }
    // Only ASCII XML names and headset identifiers are needed here. Non-ASCII
    // strings cannot match either and need no transcoding or guessed identifier.
    std::string result;
    for (std::size_t i = 0; i < bytes; i += 2) {
      auto c = u16(at+i);
      if (c > 127) return {};
      result += char(c);
    }
    if (u16(at+bytes)) throw std::runtime_error("unterminated manifest string");
    return result;
  };
  std::vector<std::string> parents;
  for (std::size_t at = 8; at < xml.size();) {
    auto type = u16(at), header = u16(at+2);
    auto size = u32(at+4);
    if (header < 8 || size < header || size > xml.size()-at)
      throw std::runtime_error("invalid APK manifest chunk");
    if (type == 1) {
      if (pool || header < 28) throw std::runtime_error("invalid manifest string pool");
      pool = at; pool_end = at + size;
      auto count = u32(at+8), start = u32(at+20);
      if (count > (size-header)/4 || start < header+std::size_t(count)*4 || start >= size)
        throw std::runtime_error("invalid manifest string offsets");
      strings = at + start; utf8 = (u32(at+16) & 0x100) != 0;
      for (std::size_t i = 0; i < count; ++i) offsets.push_back(u32(at+header+i*4));
    } else if (type == 0x102) {
      if (!pool || header < 16 || size-header < 20)
        throw std::runtime_error("invalid manifest XML element");
      auto ext = at + header;
      auto tag = string(u32(ext+4));
      auto start = u16(ext+8), stride = u16(ext+10), count = u16(ext+12);
      if (start < 20 || stride < 20 || start > size-header || count > (size-header-start)/stride)
        throw std::runtime_error("invalid manifest XML attributes");
      if (tag == "meta-data" && parents == std::vector<std::string>{"manifest", "application"}) {
        std::string name, value;
        bool string_value = false;
        for (std::size_t i = 0; i < count; ++i) {
          auto attr = ext + start + i*stride;
          auto key = string(u32(attr+4));
          if (key != "name" && key != "value") continue;
          if (u16(attr+12) != 8) throw std::runtime_error("invalid manifest attribute value");
          auto raw = u32(attr+8);
          bool text = raw != 0xffffffff || xml[attr+15] == std::byte{3};
          auto v = text ? string(raw != 0xffffffff ? raw : u32(attr+16)) : std::string{};
          if (key == "name") name = v;
          else { value = v; string_value = text; }
        }
        if (name == "com.oculus.supportedDevices") {
          if (!string_value) throw std::runtime_error("supportedDevices must be a manifest string");
          return value;
        }
      }
      parents.push_back(tag);
    } else if (type == 0x103) {
      if (parents.empty()) throw std::runtime_error("unbalanced manifest XML");
      parents.pop_back();
    }
    at += size;
  }
  return {};
}
} // namespace venture
