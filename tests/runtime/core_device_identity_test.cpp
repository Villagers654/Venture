#include "core/device_identity.h"
#include <doctest/doctest.h>
#include <vector>

namespace {
std::vector<std::byte> manifest(bool utf8, std::string_view value, bool application = true) {
  std::vector<std::byte> out;
  auto word = [&](std::uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(std::byte((n >> (8*i)) & 255));
  };
  auto patch = [&](std::size_t at, std::uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) out[at+i] = std::byte((n >> (8*i)) & 255);
  };
  word(0x00080003); word(0);
  const auto pool = out.size();
  word(0x001c0001); word(0); word(7); word(0); word(utf8 ? 0x100 : 0); word(56); word(0);
  for (int i = 0; i < 7; ++i) word(0);
  const auto strings = out.size();
  unsigned index = 0;
  for (auto text : {std::string_view("manifest"), std::string_view("application"),
                   std::string_view("meta-data"), std::string_view("name"),
                   std::string_view("value"), std::string_view("com.oculus.supportedDevices"), value}) {
    patch(pool+28+index++*4, out.size()-strings);
    out.push_back(std::byte(text.size()));
    out.push_back(utf8 ? std::byte(text.size()) : std::byte{});
    for (char c : text) { out.push_back(std::byte(c)); if (!utf8) out.push_back(std::byte{}); }
    out.push_back(std::byte{}); if (!utf8) out.push_back(std::byte{});
  }
  while (out.size()%4) out.push_back(std::byte{});
  patch(pool+4, out.size()-pool);
  auto node = [&](unsigned name, bool metadata) {
    word(0x00100102); word(metadata ? 76 : 36); word(1); word(0xffffffff);
    word(0xffffffff); word(name); word(0x00140014); word(metadata ? 2 : 0); word(0);
    if (metadata) for (unsigned i = 0; i < 2; ++i) {
      word(0xffffffff); word(3+i); word(5+i); word(0x03000008); word(5+i);
    }
  };
  node(0, false);
  if (application) node(1, false);
  node(2, true);
  patch(4, out.size());
  return out;
}
}
TEST_CASE("Horizon compatibility selects the newest supported model no newer than native Quest 3") {
  using venture::select_device_identity;
  CHECK(&select_device_identity("quest2|eureka|cambria") == &venture::native_device_identity());
  CHECK(&select_device_identity("quest3") == &venture::native_device_identity());
  CHECK(select_device_identity("quest2|cambria").model == "Quest Pro");
  CHECK(select_device_identity("questpro|quest2").device == "seacliff");
  CHECK(select_device_identity("quest2|quest").system_name == "Oculus Quest2");
  CHECK(select_device_identity("").system_name == "Oculus Quest");
  CHECK(select_device_identity("quest3s|quest2").model == "Quest 2");
  CHECK(select_device_identity("future|quest30|notquestpro").model == "Quest");
  CHECK(select_device_identity("quest").gpu_renderer == "Adreno (TM) 540");
  CHECK(select_device_identity("quest2").gpu_renderer == "Adreno (TM) 650");
  CHECK(select_device_identity("cambria").gpu_renderer == "Adreno (TM) 650");
  CHECK(venture::native_device_identity().gpu_renderer == "Adreno (TM) 740");
}
TEST_CASE("APK identity metadata accepts both Android string pool encodings and respects application scope") {
  for (bool utf8 : {false, true}) {
    auto xml = manifest(utf8, "quest2|cambria");
    CHECK(venture::manifest_supported_devices(xml) == "quest2|cambria");
    CHECK(venture::manifest_supported_devices(manifest(utf8, "quest3", false)).empty());
    xml[12] = std::byte{0xff}; // String-pool chunk extends beyond the file.
    CHECK_THROWS(venture::manifest_supported_devices(xml));
  }
  CHECK_THROWS(venture::manifest_supported_devices({}));
}
