#include "common/android/manifest.h"
#include "core/device_identity.h"
#include <doctest/doctest.h>
#include <string>
#include <utility>
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

// An element of a binary manifest with string attributes.
struct Element {
  std::string tag;
  std::vector<std::pair<std::string, std::string>> attributes;
  std::vector<Element> children;
};
std::vector<std::byte> binary_xml(const Element& root) {
  std::vector<std::string> strings;
  auto index = [&](const std::string& text) {
    for (std::size_t i = 0; i < strings.size(); ++i) if (strings[i] == text) return std::uint32_t(i);
    strings.push_back(text);
    return std::uint32_t(strings.size() - 1);
  };
  std::vector<std::byte> body;
  auto word = [](std::vector<std::byte>& out, std::uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(std::byte((n >> (8*i)) & 255));
  };
  auto emit = [&](auto& self, const Element& element) -> void {
    const auto count = std::uint32_t(element.attributes.size());
    word(body, 0x00100102); word(body, 36 + 20*count); word(body, 1); word(body, 0xffffffff);
    word(body, 0xffffffff); word(body, index(element.tag)); word(body, 0x00140014); word(body, count); word(body, 0);
    for (const auto& [name, value] : element.attributes) {
      word(body, 0xffffffff); word(body, index(name)); word(body, index(value)); word(body, 0x03000008); word(body, index(value));
    }
    for (const auto& child : element.children) self(self, child);
    word(body, 0x00100103); word(body, 24); word(body, 1); word(body, 0xffffffff);
    word(body, 0xffffffff); word(body, index(element.tag));
  };
  emit(emit, root);
  std::vector<std::byte> pool;
  std::vector<std::byte> text;
  std::vector<std::uint32_t> offsets;
  for (const auto& s : strings) {
    offsets.push_back(std::uint32_t(text.size()));
    text.push_back(std::byte(s.size())); text.push_back(std::byte(s.size()));
    for (char c : s) text.push_back(std::byte(c));
    text.push_back(std::byte{});
  }
  while (text.size() % 4) text.push_back(std::byte{});
  const auto count = std::uint32_t(strings.size());
  word(pool, 0x001c0001); word(pool, 28 + 4*count + std::uint32_t(text.size())); word(pool, count); word(pool, 0);
  word(pool, 0x100); word(pool, 28 + 4*count); word(pool, 0);
  for (auto offset : offsets) word(pool, offset);
  pool.insert(pool.end(), text.begin(), text.end());
  std::vector<std::byte> out;
  word(out, 0x00080003); word(out, std::uint32_t(8 + pool.size() + body.size()));
  out.insert(out.end(), pool.begin(), pool.end());
  out.insert(out.end(), body.begin(), body.end());
  return out;
}
Element activity(std::string name, std::vector<std::string> categories, std::string action = "android.intent.action.MAIN") {
  Element filter{"intent-filter", {}, {{"action", {{"name", action}}, {}}}};
  for (auto& category : categories) filter.children.push_back({"category", {{"name", category}}, {}});
  return {"activity", {{"name", std::move(name)}}, {filter}};
}
}
TEST_CASE("A manifest's launch activity, or a named one, chooses between the 2D and the VR framework") {
  // CitraVR's shape: a 2D launcher menu and a VR activity started in a new task.
  auto xml = binary_xml({"manifest", {{"package", "org.example.emu"}}, {{"application", {}, {
      activity("org.example.emu.ui.MainActivity", {"android.intent.category.LAUNCHER", "com.oculus.intent.category.2D"}),
      activity(".vr.VrActivity", {"com.oculus.intent.category.VR", "android.intent.category.LAUNCHER"},
               "android.intent.action.VIEW")}}}});
  CHECK(venture::manifest_launches_2d(xml));
  CHECK(venture::manifest_launches_2d(xml, "org.example.emu.ui.MainActivity"));
  CHECK_FALSE(venture::manifest_launches_2d(xml, "org.example.emu.vr.VrActivity"));
  CHECK(venture::manifest_launches_2d(xml, "org.example.emu.Missing"));
  auto vr = binary_xml({"manifest", {{"package", "org.example.game"}}, {{"application", {}, {
      activity("Game", {"com.oculus.intent.category.VR", "android.intent.category.LAUNCHER"})}}}});
  CHECK_FALSE(venture::manifest_launches_2d(vr));
  CHECK_FALSE(venture::manifest_launches_2d(vr, "org.example.game.Game"));
  // The launcher reads the same rule from the APK before a launch, to offer the
  // Android 2D component's download first (common/android/manifest.h).
  auto parsed = [](const std::vector<std::byte>& bytes) {
    return venture::android::parse_manifest({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
  };
  CHECK(venture::android::launches_2d(parsed(xml)));
  CHECK_FALSE(venture::android::launches_2d(parsed(vr)));
  auto native = binary_xml({"manifest", {{"package", "org.example.native"}}, {{"application", {}, {
      activity("android.app.NativeActivity", {"android.intent.category.LAUNCHER"})}}}});
  CHECK_FALSE(venture::manifest_launches_2d(native));
  CHECK_FALSE(venture::android::launches_2d(parsed(native)));
  // The Climb 2: CryEngine's NativeActivity subclass with only LAUNCHER, in a
  // vr_only application. Either one makes it immersive.
  auto engine = [](bool vr_only, bool lib_name) {
    Element launch = activity("com.crytek.cryengine.EngineActivity", {"android.intent.category.LAUNCHER"});
    if (lib_name) launch.children.push_back({"meta-data", {{"name", "android.app.lib_name"}, {"value", "AndroidLauncher"}}, {}});
    Element application{"application", {}, {}};
    if (vr_only)
      application.children.push_back(
          {"meta-data", {{"name", "com.samsung.android.vr.application.mode"}, {"value", "vr_only"}}, {}});
    application.children.push_back(std::move(launch));
    return binary_xml({"manifest", {{"package", "com.crytek.climb2"}}, {std::move(application)}});
  };
  for (auto [vr_only, lib_name] : {std::pair{true, true}, std::pair{true, false}, std::pair{false, true}}) {
    CHECK_FALSE(venture::manifest_launches_2d(engine(vr_only, lib_name)));
    CHECK_FALSE(venture::manifest_launches_2d(engine(vr_only, lib_name), "com.crytek.cryengine.EngineActivity"));
    CHECK_FALSE(venture::android::launches_2d(parsed(engine(vr_only, lib_name))));
  }
  CHECK(venture::manifest_launches_2d(engine(false, false)));
  CHECK(venture::android::launches_2d(parsed(engine(false, false))));
  // Another meta-data value is not Horizon's VR mode.
  auto dual = binary_xml({"manifest", {{"package", "org.example.app"}}, {{"application", {}, {
      {"meta-data", {{"name", "com.samsung.android.vr.application.mode"}, {"value", "dual"}}, {}},
      activity("org.example.app.Main", {"android.intent.category.LAUNCHER"})}}}});
  CHECK(venture::manifest_launches_2d(dual));
  CHECK(venture::android::launches_2d(parsed(dual)));
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
  // The Climb 2 lists "quest|delmar": delmar is the Quest 2.
  CHECK(select_device_identity("quest|delmar").model == "Quest 2");
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
