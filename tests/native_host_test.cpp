// Native hosts: what a Quest APK declares for a headset's own Android, and the
// plan that keeps the headset's native pieces and supplies only what is
// Meta-specific and missing (docs/design/native-headsets.md).
#include "common/android/apk_rewrite.h"
#include "common/android/archive.h"
#include "common/android/axml.h"
#include "common/android/dex.h"
#include "common/android/elf.h"
#include "common/android/game_adapter.h"
#include "common/android/manifest.h"
#include "common/android/native_host.h"
#include "common/quest_xr_surface.h"
#include "launcher/headset_check.h"

#include <doctest/doctest.h>
#include <miniz.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <stop_token>

using namespace venture;
namespace nh = venture::android::native_host;

namespace {
// A compiled AndroidManifest.xml from an element tree. Attributes with a
// resource ID go through the XML resource map; with `named` false their
// names are empty, as in shrunk APKs.
struct Attr {
  std::string name;
  std::uint32_t id = 0;
  enum Kind { text, integer, boolean } kind = text;
  std::string value;
  std::uint32_t number = 0;
};
struct Node {
  std::string tag;
  std::vector<Attr> attrs;
  std::vector<Node> children;
};
Attr text(std::string name, std::uint32_t id, std::string value) { return {std::move(name), id, Attr::text, std::move(value)}; }
Attr integer(std::string name, std::uint32_t id, std::uint32_t value) { return {std::move(name), id, Attr::integer, {}, value}; }
constexpr std::uint32_t kName = 0x01010003, kMinSdk = 0x0101020c, kTargetSdk = 0x01010270;

std::vector<std::uint8_t> compile(const Node& root, bool named = true) {
  std::vector<std::string> strings;
  std::vector<std::uint32_t> ids;
  std::map<std::pair<std::string, std::uint32_t>, std::uint32_t> attr_names;
  std::map<std::string, std::uint32_t> plain;
  // Attribute names with resource IDs come first: the map indexes the pool.
  std::function<void(const Node&)> collect_ids = [&](const Node& node) {
    for (const auto& a : node.attrs)
      if (a.id && !attr_names.contains({a.name, a.id})) {
        attr_names[{a.name, a.id}] = std::uint32_t(strings.size());
        strings.push_back(named ? a.name : "");
        ids.push_back(a.id);
      }
    for (const auto& child : node.children) collect_ids(child);
  };
  collect_ids(root);
  auto intern = [&](const std::string& s) {
    auto [it, added] = plain.try_emplace(s, std::uint32_t(strings.size()));
    if (added) strings.push_back(s);
    return it->second;
  };
  std::vector<std::uint8_t> out;
  auto u16 = [](std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(std::uint8_t(x));
    v.push_back(std::uint8_t(x >> 8));
  };
  auto u32 = [&](std::vector<std::uint8_t>& v, std::uint32_t x) {
    u16(v, x & 0xffff);
    u16(v, x >> 16);
  };
  std::vector<std::uint8_t> body;
  std::function<void(const Node&)> emit = [&](const Node& node) {
    const auto tag = intern(node.tag);
    std::vector<std::uint8_t> e;
    u16(e, 0x102);
    u16(e, 16);
    u32(e, 16 + 20 + std::uint32_t(node.attrs.size()) * 20);
    u32(e, 1);
    u32(e, 0xffffffff);
    u32(e, 0xffffffff);
    u32(e, tag);
    u16(e, 20);
    u16(e, 20);
    u16(e, std::uint32_t(node.attrs.size()));
    u16(e, 0);
    u16(e, 0);
    u16(e, 0);
    for (const auto& a : node.attrs) {
      const auto name = a.id ? attr_names.at({a.name, a.id}) : intern(a.name);
      std::uint32_t raw = 0xffffffff, data = a.number;
      std::uint8_t type = a.kind == Attr::integer ? 0x10 : 0x12;
      if (a.kind == Attr::text) {
        raw = data = intern(a.value);
        type = 0x03;
      }
      if (a.kind == Attr::boolean) data = a.number ? 0xffffffffu : 0u;
      u32(e, 0xffffffff);
      u32(e, name);
      u32(e, raw);
      u16(e, 8);
      e.push_back(0);
      e.push_back(type);
      u32(e, data);
    }
    body.insert(body.end(), e.begin(), e.end());
    for (const auto& child : node.children) emit(child);
    std::vector<std::uint8_t> end;
    u16(end, 0x103);
    u16(end, 16);
    u32(end, 24);
    u32(end, 1);
    u32(end, 0xffffffff);
    u32(end, 0xffffffff);
    u32(end, tag);
    body.insert(body.end(), end.begin(), end.end());
  };
  emit(root);
  // String pool, UTF-16.
  std::vector<std::uint8_t> data;
  std::vector<std::uint32_t> offsets;
  for (const auto& s : strings) {
    offsets.push_back(std::uint32_t(data.size()));
    u16(data, std::uint32_t(s.size()));
    for (char c : s) u16(data, std::uint8_t(c));
    u16(data, 0);
  }
  while (data.size() % 4) data.push_back(0);
  std::vector<std::uint8_t> head;
  const std::uint32_t pool_header = 28, start = pool_header + std::uint32_t(offsets.size()) * 4;
  u16(head, 1);
  u16(head, pool_header);
  u32(head, start + std::uint32_t(data.size()));
  u32(head, std::uint32_t(strings.size()));
  u32(head, 0);
  u32(head, 0);
  u32(head, start);
  u32(head, 0);
  for (auto o : offsets) u32(head, o);
  head.insert(head.end(), data.begin(), data.end());
  u16(head, 0x180);
  u16(head, 8);
  u32(head, 8 + std::uint32_t(ids.size()) * 4);
  for (auto id : ids) u32(head, id);
  head.insert(head.end(), body.begin(), body.end());
  u16(out, 3);
  u16(out, 8);
  u32(out, 8 + std::uint32_t(head.size()));
  out.insert(out.end(), head.begin(), head.end());
  return out;
}

Node filter(std::vector<std::string> actions, std::vector<std::string> categories) {
  Node f{"intent-filter"};
  for (auto& a : actions) f.children.push_back({"action", {text("name", kName, a)}});
  for (auto& c : categories) f.children.push_back({"category", {text("name", kName, c)}});
  return f;
}
constexpr const char* kMain = "android.intent.action.MAIN";
constexpr const char* kLauncher = "android.intent.category.LAUNCHER";
constexpr const char* kQuestVr = "com.oculus.intent.category.VR";
constexpr const char* kImmersive = "org.khronos.openxr.intent.category.IMMERSIVE_HMD";

// A Quest game's manifest: a VR activity, a launcher alias, a service.
Node quest_manifest(std::uint32_t min_sdk = 29) {
  return {"manifest",
          {text("package", 0, "com.example.questgame"), integer("versionCode", 0x0101021b, 7)},
          {{"uses-sdk", {integer("minSdkVersion", kMinSdk, min_sdk), integer("targetSdkVersion", kTargetSdk, 32)}},
           {"application",
            {},
            {{"meta-data", {text("name", kName, "com.oculus.supportedDevices"), text("value", 0, "quest3")}},
             {"uses-native-library", {text("name", kName, "libopenxr.google.so")}},
             {"activity",
              {text("name", kName, "com.unity3d.player.UnityPlayerActivity")},
              {filter({kMain}, {kQuestVr}), filter({"android.intent.action.VIEW"}, {kLauncher})}},
             {"activity-alias", {text("name", kName, ".Launcher")}, {filter({kMain}, {kLauncher})}},
             {"service", {text("name", kName, ".Sync")}, {filter({kMain}, {"x.y.SERVICE"})}}}}}};
}

std::vector<std::string> quest_entries() {
  return {"AndroidManifest.xml", "classes.dex", "lib/arm64-v8a/libil2cpp.so", "lib/arm64-v8a/libunity.so",
          "lib/arm64-v8a/libOVRPlugin.so", "lib/arm64-v8a/libopenxr_loader.so",
          "lib/arm64-v8a/libovrplatformloader.so", "lib/arm64-v8a/nested/libskip.so", "lib/arm64-v8a/readme.txt",
          "assets/bin/Data/data.unity3d"};
}

// A Meta headset: its own VrApi, platform service and every Quest extension.
nh::HostProfile meta_host() {
  nh::HostProfile host{.abis = {"arm64-v8a", "armeabi-v7a"}, .android_api = 34, .xr_api_minor = 1,
                       .vrapi = true, .platform_service = true, .launch_categories = {kQuestVr}};
  for (const auto& e : quest_xr::kExtensions) host.xr_extensions.emplace(e.name, e.version);
  host.xr_extensions["XR_KHR_opengl_es_enable"] = 10;
  return host;
}
// Another vendor's Android: OpenXR 1.0, a few Quest extensions, an AArch64-only CPU.
nh::HostProfile other_host() {
  return {.abis = {"arm64-v8a"}, .android_api = 33, .xr_api_minor = 0,
          .xr_extensions = {{"XR_KHR_opengl_es_enable", 10}, {"XR_KHR_vulkan_enable2", 2},
                            {"XR_FB_display_refresh_rate", 1}, {"XR_EXT_hand_tracking", 4}},
          .launch_categories = {kLauncher, kImmersive},
          .launch_metadata = {{"example.vr.mode", "immersive"}}};
}
bool has(const std::vector<std::string>& list, std::string_view value) {
  return std::ranges::find(list, value) != list.end();
}
}  // namespace

TEST_CASE("APK manifests give their SDK levels, launch filters and requested system libraries") {
  for (bool named : {true, false}) {
    CAPTURE(named);
    const auto m = android::parse_manifest(compile(quest_manifest(), named));
    CHECK(m.package == "com.example.questgame");
    CHECK(m.version_code == 7);
    CHECK(m.min_sdk == 29);
    CHECK(m.target_sdk == 32);
    REQUIRE(m.launch_filters.size() == 2);  // the VIEW filter and the service are not launch entries
    CHECK_FALSE(m.launch_filters[0].alias);
    CHECK(m.launch_filters[0].categories == std::vector<std::string>{kQuestVr});
    CHECK(m.launch_filters[1].alias);
    CHECK(m.launch_filters[1].categories == std::vector<std::string>{kLauncher});
    CHECK(m.native_libraries == std::vector<std::string>{"libopenxr.google.so"});
  }
  // Written as text, and a preview code name that is no level.
  Node sdk{"manifest", {text("package", 0, "com.example.game")},
           {{"uses-sdk", {text("minSdkVersion", kMinSdk, "26"), text("targetSdkVersion", kTargetSdk, "Baklava")}}}};
  const auto m = android::parse_manifest(compile(sdk));
  CHECK(m.min_sdk == 26);
  CHECK(m.target_sdk == 0);
  CHECK(m.launch_filters.empty());
}

TEST_CASE("an APK profile lists each ABI slice's libraries and the categories real activities launch with") {
  const auto profile = nh::profile(android::parse_manifest(compile(quest_manifest())), quest_entries());
  CHECK(profile.package == "com.example.questgame");
  REQUIRE(profile.slices.size() == 1);
  CHECK(profile.slices.at("arm64-v8a") == std::vector<std::string>{"libOVRPlugin.so", "libil2cpp.so",
                                                                   "libopenxr_loader.so", "libovrplatformloader.so",
                                                                   "libunity.so"});
  CHECK(profile.activity_categories == std::vector<std::string>{kQuestVr});
  CHECK(profile.alias_categories == std::vector<std::string>{kLauncher});
  CHECK(profile.native_libraries == std::vector<std::string>{"libopenxr.google.so"});

  // The same from an APK file.
  const auto apk = fs::temp_directory_path() /
                   ("venture-native-host-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".apk");
  {
    mz_zip_archive z{};
    REQUIRE(mz_zip_writer_init_file(&z, paths::utf8(apk).c_str(), 0));
    const auto xml = compile(quest_manifest());
    REQUIRE(mz_zip_writer_add_mem(&z, "AndroidManifest.xml", xml.data(), xml.size(), MZ_DEFAULT_COMPRESSION));
    for (const auto& name : quest_entries())
      if (name != "AndroidManifest.xml") REQUIRE(mz_zip_writer_add_mem(&z, name.c_str(), "x", 1, MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_finalize_archive(&z));
    REQUIRE(mz_zip_writer_end(&z));
  }
  const auto from_file = nh::profile(apk);
  std::error_code ec;
  fs::remove(apk, ec);
  CHECK(from_file.slices == profile.slices);
  CHECK(from_file.activity_categories == profile.activity_categories);
}

TEST_CASE("on a Meta headset a Quest game runs as it is") {
  const auto apk = nh::profile(android::parse_manifest(compile(quest_manifest())), quest_entries());
  const auto plan = nh::plan(apk, meta_host());
  CHECK(plan.execution == nh::Execution::native);
  CHECK(plan.abi == "arm64-v8a");
  CHECK(plan.replace.empty());
  CHECK_FALSE(plan.quest_layer);
  CHECK(plan.layer_extensions.empty());
  CHECK_FALSE(plan.lower_api_version);
  CHECK_FALSE(plan.meta_java);
  CHECK(plan.add_categories.empty());
  CHECK(plan.blockers.empty());
}

TEST_CASE("on another headset Venture supplies only the Meta pieces its Android and runtime lack") {
  const auto apk = nh::profile(android::parse_manifest(compile(quest_manifest())), quest_entries());
  const auto host = other_host();
  const auto plan = nh::plan(apk, host);
  CHECK(plan.blockers.empty());
  CHECK(plan.execution == nh::Execution::native);
  CHECK(plan.abi == "arm64-v8a");
  // The bundled loader gives way to one that finds the device's runtime, and the
  // platform loader to Venture's. OVRPlugin, the engine and the game stay native.
  REQUIRE(plan.replace.size() == 2);
  CHECK(plan.replace[0].library == "libopenxr_loader.so");
  CHECK(plan.replace[0].with == nh::Supply::openxr_loader);
  CHECK(plan.replace[1].library == "libovrplatformloader.so");
  CHECK(plan.replace[1].with == nh::Supply::platform);
  CHECK(plan.meta_java);
  CHECK(plan.quest_layer);
  CHECK(plan.lower_api_version);
  // The layer answers exactly the Quest extensions the runtime lacks.
  CHECK_FALSE(has(plan.layer_extensions, "XR_FB_display_refresh_rate"));
  CHECK(has(plan.layer_extensions, "XR_FB_foveation"));
  CHECK(has(plan.layer_extensions, "XR_META_headset_id"));
  CHECK_FALSE(has(plan.layer_extensions, "XR_KHR_loader_init"));  // the device's loader owns it
  for (const auto& name : plan.layer_extensions) {
    const auto* extension = quest_xr::find(name);
    REQUIRE(extension);
    CHECK(extension->kind == quest_xr::Kind::emulated);
    CHECK_FALSE(host.xr_extensions.contains(name));
  }
  // The alias's LAUNCHER does not count; both categories the shell wants are added.
  CHECK(plan.add_categories == std::vector<std::string>{kLauncher, kImmersive});
  CHECK(plan.set_metadata == host.launch_metadata);

  // A VrApi game: Venture's VrApi on the device's OpenXR runtime.
  auto vrapi = apk;
  vrapi.slices["arm64-v8a"] = {"libmain.so", "libvrapi.so"};
  const auto vrapi_plan = nh::plan(vrapi, host);
  REQUIRE(vrapi_plan.replace.size() == 1);
  CHECK(vrapi_plan.replace[0].library == "libvrapi.so");
  CHECK(vrapi_plan.replace[0].with == nh::Supply::vrapi);
  CHECK(vrapi_plan.quest_layer);

  // A runtime at OpenXR 1.1 with every Quest extension needs no layer.
  auto full = host;
  full.xr_api_minor = 1;
  for (const auto& e : quest_xr::kExtensions) full.xr_extensions.emplace(e.name, e.version);
  const auto full_plan = nh::plan(apk, full);
  CHECK_FALSE(full_plan.quest_layer);
  CHECK(full_plan.layer_extensions.empty());
  CHECK(full_plan.replace.size() == 2);  // still no Meta runtime or platform service
}

TEST_CASE("an ARM32-only game runs natively where the CPU has AArch32 and translated where it has not") {
  auto apk = nh::profile(android::parse_manifest(compile(quest_manifest())), quest_entries());
  apk.slices = {{"armeabi-v7a", {"libmain.so", "libvrapi.so"}}};
  auto with_a32 = other_host();
  with_a32.abis = {"arm64-v8a", "armeabi-v7a"};
  const auto native = nh::plan(apk, with_a32);
  CHECK(native.execution == nh::Execution::native);
  CHECK(native.abi == "armeabi-v7a");

  const auto translated = nh::plan(apk, other_host());
  CHECK(translated.execution == nh::Execution::translated_a32);
  CHECK(translated.abi == "armeabi-v7a");
  CHECK(translated.blockers.empty());
  // Extensions whose structures the A32 adapters do not translate stay out.
  for (const auto& name : translated.layer_extensions) CHECK(quest_xr::find(name)->isa == quest_xr::Isa::any);
  CHECK_FALSE(has(translated.layer_extensions, "XR_META_headset_id"));
  CHECK(has(translated.layer_extensions, "XR_FB_foveation"));
}

TEST_CASE("the headset check reports the host, its 32-bit support and each game's plan") {
  namespace check = venture::launcher::headset;
  auto host = other_host();
  host.abis = {"arm64-v8a", "armeabi-v7a"};
  check::HostFacts facts{.abis32 = {"armeabi-v7a", "armeabi"}, .device = "Vendor Headset", .runtime = "Runtime 1.2.3",
                         .page_size = 16384, .install_permission = true};
  facts.runtimes.push_back({"system", "armeabi-v7a", "com.vendor.xr", "/lib/arm/libxr.so", true});
  const auto text = check::describe_host(host, facts);
  CHECK(text.find("Device: Vendor Headset") != std::string::npos);
  CHECK(text.find("page size 16384") != std::string::npos);
  CHECK(text.find("32-bit ABIs (Build.SUPPORTED_32_BIT_ABIS): armeabi-v7a, armeabi") != std::string::npos);
  CHECK(text.find("ARM32 games: run natively") != std::string::npos);
  CHECK(text.find("OpenXR: Runtime 1.2.3, accepts version 1.0") != std::string::npos);
  CHECK(text.find("Runtime broker (system, armeabi-v7a): com.vendor.xr /lib/arm/libxr.so\n") != std::string::npos);
  CHECK(text.find("XR_FB_display_refresh_rate v1") != std::string::npos);
  const auto bare = check::describe_host(other_host(), {});
  CHECK(bare.find("ARM32 games: cannot run here") != std::string::npos);
  CHECK(bare.find("OpenXR: no runtime answered") != std::string::npos);
  CHECK(bare.find("Runtime brokers: none answered") != std::string::npos);

  auto apk = nh::profile(android::parse_manifest(compile(quest_manifest())), quest_entries());
  apk.slices = {{"armeabi-v7a", {"libmain.so", "libvrapi.so"}}};
  check::GameFacts game{.name = "Old Game", .package = "com.example.old", .version = "1.0", .apk = apk,
                        .installed_version = 7, .signed_by_venture = true, .adapted_current = false};
  auto described = check::describe_game(game, host);
  CHECK(described.find("--- Old Game (com.example.old) 1.0 ---") != std::string::npos);
  CHECK(described.find("version code 7, signed by Venture: yes, adapted by this Venture: no") != std::string::npos);
  CHECK(described.find("Runs: natively, armeabi-v7a") != std::string::npos);
  CHECK(described.find("Replaced: libvrapi.so with Venture's VrApi") != std::string::npos);
  described = check::describe_game(game, other_host());
  CHECK(described.find("needs a CPU with a 32-bit mode") != std::string::npos);

  check::GameFacts unreadable{.package = "com.example.gone", .apk_error = "no such file"};
  described = check::describe_game(unreadable, host);
  CHECK(described.find("--- com.example.gone") != std::string::npos);
  CHECK(described.find("On the headset: not installed") != std::string::npos);
  CHECK(described.find("APK: could not be read (no such file)") != std::string::npos);
}

TEST_CASE("the Linux headset check reports the sandbox and the runtime's own lines") {
  namespace check = venture::launcher::headset;
  const auto text = check::describe_linux({.sandbox = "Flatpak org.venture_emu.Venture (stable)",
                                           .session = "Wayland (wayland-0)",
                                           .runtime = "CPU: AArch64, page size 4096\nCPU engine for A64 games: native-a64 (x)"});
  CHECK(text.find("Sandbox: Flatpak org.venture_emu.Venture (stable)\n") != std::string::npos);
  CHECK(text.find("Launcher display: Wayland (wayland-0)\n") != std::string::npos);
  CHECK(text.find("CPU engine for A64 games: native-a64 (x)\n") != std::string::npos);
  CHECK(text.find("--host-check") == std::string::npos);
  const auto failed = check::describe_linux({.runtime_error = "exited with code 1"});
  CHECK(failed.find("Sandbox: none\n") != std::string::npos);
  CHECK(failed.find("venture-runtime --host-check: exited with code 1\n") != std::string::npos);
  CHECK(check::describe_linux({}).find("printed nothing") != std::string::npos);
}

TEST_CASE("a native host plan names what stops a game") {
  auto apk = nh::profile(android::parse_manifest(compile(quest_manifest(36))), quest_entries());
  auto host = other_host();
  auto plan = nh::plan(apk, host);
  REQUIRE(plan.blockers.size() == 1);
  CHECK(plan.blockers[0].find("Android API 36") != std::string::npos);

  apk.min_sdk = 29;
  apk.slices = {{"x86_64", {"libmain.so"}}};
  plan = nh::plan(apk, host);
  CHECK(plan.execution == nh::Execution::unsupported);
  REQUIRE(plan.blockers.size() == 1);
  CHECK(plan.blockers[0].find("x86_64") != std::string::npos);

  apk.slices = {{"arm64-v8a", {"libopenxr_loader.so"}}};
  host.xr_extensions = {{"XR_EXT_hand_tracking", 4}};
  plan = nh::plan(apk, host);
  REQUIRE(plan.blockers.size() == 1);
  CHECK(plan.blockers[0].find("OpenGL ES") != std::string::npos);

  // A game without native code runs on any of them.
  apk.slices.clear();
  plan = nh::plan(apk, other_host());
  CHECK(plan.execution == nh::Execution::native);
  CHECK(plan.abi.empty());
  CHECK_FALSE(plan.quest_layer);
}

TEST_CASE("the Quest extension surface names each extension once") {
  std::set<std::string_view> names;
  for (const auto& extension : quest_xr::kExtensions) {
    CHECK(names.insert(extension.name).second);
    CHECK(extension.version > 0);
    CHECK(quest_xr::find(extension.name) == &extension);
  }
  CHECK(quest_xr::find("XR_NOT_an_extension") == nullptr);
}

namespace {
const android::axml::Node* child(const android::axml::Node& node, std::string_view tag, std::string_view name = {}) {
  for (const auto& c : node.children) {
    if (c.text || c.name != tag) continue;
    const auto* a = c.attribute(0x01010003, "name");
    if (name.empty() || (a && android::axml::text(*a) == name)) return &c;
  }
  return nullptr;
}
}  // namespace

TEST_CASE("binary manifests survive a parse and rewrite with every field intact") {
  for (bool named : {true, false}) {
    CAPTURE(named);
    const auto original = compile(quest_manifest(), named);
    const auto rewritten = android::axml::serialize(android::axml::parse(original));
    const auto a = android::parse_manifest(original), b = android::parse_manifest(rewritten);
    CHECK(b.package == a.package);
    CHECK(b.version_code == a.version_code);
    CHECK(b.min_sdk == a.min_sdk);
    CHECK(b.target_sdk == a.target_sdk);
    CHECK(b.native_libraries == a.native_libraries);
    REQUIRE(b.launch_filters.size() == a.launch_filters.size());
    for (std::size_t i = 0; i < a.launch_filters.size(); ++i) {
      CHECK(b.launch_filters[i].alias == a.launch_filters[i].alias);
      CHECK(b.launch_filters[i].categories == a.launch_filters[i].categories);
    }
    // A second pass is byte-identical: the writer's layout is stable.
    CHECK(android::axml::serialize(android::axml::parse(rewritten)) == rewritten);
  }

  // Namespaces, text, non-ASCII strings and typed values.
  using namespace android::axml;
  Document doc;
  doc.root = android::axml::Node{.name = "manifest"};
  doc.root.namespaces.push_back({"android", std::string(kAndroidNamespace)});
  doc.root.attributes.push_back({.name = "package", .string = "com.example.cafe"});
  doc.root.attributes.push_back({.name = "label", .string = "Caf\xc3\xa9"});
  doc.root.attributes.push_back({.ns = std::string(kAndroidNamespace), .name = "versionCode", .resource_id = 0x0101021b,
                                 .raw = "12", .type = kIntDec, .data = 12});
  auto& app = doc.root.children.emplace_back(android::axml::Node{.name = "application"});
  app.attributes.push_back({.ns = std::string(kAndroidNamespace), .name = "debuggable", .resource_id = 0x0101000f,
                            .type = kBoolean, .data = 0xffffffff});
  app.attributes.push_back({.ns = std::string(kAndroidNamespace), .name = "icon", .resource_id = 0x01010002,
                            .type = kReference, .data = 0x7f080001});
  app.children.push_back(android::axml::Node{.text = true, .value = "\xf0\x9f\x8e\xae text"});
  const auto parsed = parse(serialize(doc));
  REQUIRE(parsed.root.namespaces.size() == 1);
  CHECK(parsed.root.namespaces[0].prefix == "android");
  CHECK(parsed.root.namespaces[0].uri == kAndroidNamespace);
  CHECK(android::axml::text(*parsed.root.attribute(0, "label")) == "Caf\xc3\xa9");
  CHECK(parsed.root.attribute(0x0101021b, "")->data == 12);
  const auto& papp = parsed.root.children.at(0);
  // Written in resource ID order whatever order they were added in.
  REQUIRE(papp.attributes.size() == 2);
  CHECK(papp.attributes[0].resource_id == 0x01010002);
  CHECK(papp.attributes[0].type == kReference);
  CHECK(papp.attributes[0].data == 0x7f080001);
  CHECK(papp.attributes[1].type == kBoolean);
  REQUIRE(papp.children.size() == 1);
  CHECK(papp.children[0].text);
  CHECK(papp.children[0].value == "\xf0\x9f\x8e\xae text");
  CHECK(android::parse_manifest(serialize(doc)).version_code == 12);
  std::vector<std::uint8_t> garbage(64, 0x41);
  CHECK_THROWS(parse(garbage));
}

TEST_CASE("a game's manifest is adapted for the headset's shell before it is installed") {
  const auto original = compile(quest_manifest());
  const auto host = other_host();
  const auto plan = nh::plan(nh::profile(android::parse_manifest(original), quest_entries()), host);
  const auto adapted = nh::adapt_manifest(original, plan);

  // The planner, run again, finds nothing left to add.
  const auto again = nh::plan(nh::profile(android::parse_manifest(adapted), quest_entries()), host);
  CHECK(again.add_categories.empty());
  const auto m = android::parse_manifest(adapted);
  REQUIRE_FALSE(m.launch_filters.empty());
  CHECK(m.launch_filters[0].categories == std::vector<std::string>{kQuestVr, kLauncher, kImmersive});
  CHECK(m.package == "com.example.questgame");
  CHECK(m.min_sdk == 29);

  const auto doc = android::axml::parse(adapted);
  const auto* app = child(doc.root, "application");
  REQUIRE(app);
  const auto* meta = child(*app, "meta-data", "example.vr.mode");
  REQUIRE(meta);
  CHECK(android::axml::text(*meta->attribute(0x01010024, "value")) == "immersive");
  REQUIRE(meta->attributes.size() == 2);
  CHECK(meta->attributes[0].resource_id < meta->attributes[1].resource_id);
  // Existing meta-data is kept.
  CHECK(child(*app, "meta-data", "com.oculus.supportedDevices"));

  // Adapting twice changes nothing more.
  CHECK(nh::adapt_manifest(adapted, plan) == android::axml::serialize(android::axml::parse(adapted)));

  // A value already present is replaced, not duplicated.
  auto replace = plan;
  replace.add_categories.clear();
  replace.set_metadata = {{"com.oculus.supportedDevices", "all"}};
  const auto replaced = android::axml::parse(nh::adapt_manifest(original, replace));
  const auto* rapp = child(replaced.root, "application");
  REQUIRE(rapp);
  int count = 0;
  for (const auto& c : rapp->children)
    if (c.name == "meta-data") ++count;
  CHECK(count == 1);
  CHECK(android::axml::text(*child(*rapp, "meta-data", "com.oculus.supportedDevices")->attribute(0x01010024, "value")) ==
        "all");
}

TEST_CASE("when only an alias launches, the activity it names gets the launch filter") {
  Node alias_only{"manifest", {text("package", 0, "com.example.alias")},
                  {{"application", {},
                    {{"activity", {text("name", kName, ".Main")}, {filter({"android.intent.action.VIEW"}, {})}},
                     {"activity-alias", {text("name", kName, ".Launch"), text("targetActivity", 0x01010202, ".Main")},
                      {filter({kMain}, {kLauncher})}}}}}};
  nh::Plan plan;
  plan.add_categories = {kLauncher, kImmersive};
  const auto m = android::parse_manifest(nh::adapt_manifest(compile(alias_only), plan));
  REQUIRE(m.launch_filters.size() == 2);
  CHECK(m.launch_filters[0].alias == false);
  CHECK(m.launch_filters[0].categories == std::vector<std::string>{kLauncher, kImmersive});

  Node none{"manifest", {text("package", 0, "com.example.none")}, {{"application"}}};
  CHECK_THROWS(nh::adapt_manifest(compile(none), plan));
  // Meta-data alone needs no activity.
  nh::Plan meta_only;
  meta_only.set_metadata = {{"k", "v"}};
  CHECK_NOTHROW(nh::adapt_manifest(compile(none), meta_only));
}

namespace {
// A minimal ELF64 shared object: its dynamic symbol table defines `defined`,
// imports `imported`, and DT_SONAME is `so`.
std::vector<std::uint8_t> elf64(const std::vector<std::string>& defined, const std::string& imported,
                                const std::string& so) {
  std::string strings(1, '\0');
  auto add = [&](const std::string& s) {
    const auto at = std::uint32_t(strings.size());
    strings += s;
    strings += '\0';
    return at;
  };
  std::vector<std::uint8_t> symbols(24, 0);  // the null symbol
  auto symbol = [&](const std::string& name, bool define) {
    std::uint8_t sym[24]{};
    const auto at = add(name);
    std::memcpy(sym, &at, 4);
    sym[4] = std::uint8_t((1 << 4) | 2);  // GLOBAL FUNC
    const std::uint16_t section = define ? 4 : 0;
    std::memcpy(sym + 6, &section, 2);
    symbols.insert(symbols.end(), sym, sym + 24);
  };
  for (const auto& name : defined) symbol(name, true);
  symbol(imported, false);
  const auto soname_at = add(so);
  std::vector<std::uint8_t> dynamic(32, 0);  // DT_SONAME, then DT_NULL
  const std::uint64_t tag = 14, value = soname_at;
  std::memcpy(dynamic.data(), &tag, 8);
  std::memcpy(dynamic.data() + 8, &value, 8);
  std::vector<std::uint8_t> out(64, 0);
  auto put = [&](std::size_t at, std::uint64_t v, int size) { std::memcpy(out.data() + at, &v, std::size_t(size)); };
  const auto dynsym_off = out.size();
  out.insert(out.end(), symbols.begin(), symbols.end());
  const auto dynstr_off = out.size();
  out.insert(out.end(), strings.begin(), strings.end());
  while (out.size() % 8) out.push_back(0);
  const auto dynamic_off = out.size();
  out.insert(out.end(), dynamic.begin(), dynamic.end());
  const auto sections_off = out.size();
  out.resize(out.size() + 5 * 64, 0);  // null, dynsym, dynstr, dynamic, text
  std::memcpy(out.data(), "\x7f" "ELF\x02\x01\x01", 7);
  put(0x10, 3, 2);  // ET_DYN
  put(0x12, 183, 2);
  put(0x28, sections_off, 8);
  put(0x3a, 64, 2);
  put(0x3c, 5, 2);
  auto section = [&](int index, std::uint32_t type, std::uint64_t offset, std::uint64_t size, std::uint32_t link,
                     std::uint64_t entry) {
    const auto at = sections_off + std::size_t(index) * 64;
    put(at + 4, type, 4);
    put(at + 0x18, offset, 8);
    put(at + 0x20, size, 8);
    put(at + 0x28, link, 4);
    put(at + 0x38, entry, 8);
  };
  section(1, 11, dynsym_off, symbols.size(), 2, 24);
  section(2, 3, dynstr_off, strings.size(), 0, 0);
  section(3, 6, dynamic_off, dynamic.size(), 2, 16);
  section(4, 1, 0, 0, 0, 0);
  return out;
}
}  // namespace

TEST_CASE("a library's exports and soname are read from its ELF") {
  const auto library = elf64({"ovr_PlatformInitializeAndroid", "ovr_Message_GetType"}, "malloc", "libRenamed.so");
  const auto names = android::elf::exports(library);
  CHECK(names == std::vector<std::string>{"ovr_PlatformInitializeAndroid", "ovr_Message_GetType"});
  CHECK(android::elf::soname(library) == "libRenamed.so");
  CHECK(nh::is_platform_loader(names));
  CHECK_FALSE(nh::is_platform_loader(android::elf::exports(elf64({"Java_com_example_Main"}, "ovr_Foo", "libx.so"))));
  std::vector<std::uint8_t> garbage(80, 0x7f);
  CHECK(android::elf::exports(garbage).empty());
  CHECK(android::elf::soname(garbage).empty());
}

TEST_CASE("a renamed copy of the platform loader becomes an alias of Venture's one instance") {
  auto apk = nh::profile(android::parse_manifest(compile(quest_manifest())), quest_entries());
  apk.slices["arm64-v8a"].push_back("libOVRPlatformRenamed.so");
  apk.platform_copies["arm64-v8a"] = {"libOVRPlatformRenamed.so"};
  const auto plan = nh::plan(apk, other_host());
  REQUIRE(plan.replace.size() == 3);
  CHECK(plan.replace[2].library == "libOVRPlatformRenamed.so");
  CHECK(plan.replace[2].with == nh::Supply::platform_alias);
  CHECK(plan.platform_broker);
  // A Meta headset keeps the game's own copies.
  const auto meta = nh::plan(apk, meta_host());
  CHECK(meta.replace.empty());
  CHECK_FALSE(meta.platform_broker);
}

TEST_CASE("a game with Venture's platform loader declares its provider and may reach the Venture app") {
  const auto original = compile(quest_manifest());
  nh::Plan plan;
  plan.platform_broker = true;
  const auto adapted = nh::adapt_manifest(original, plan);
  const auto doc = android::axml::parse(adapted);
  CHECK(child(doc.root, "uses-permission", "android.permission.INTERNET"));
  const auto* queries = child(doc.root, "queries");
  REQUIRE(queries);
  CHECK(child(*queries, "package", nh::kVenturePackage));
  const auto* app = child(doc.root, "application");
  REQUIRE(app);
  const auto* provider = child(*app, "provider", nh::kPlatformProvider);
  REQUIRE(provider);
  CHECK(android::axml::text(*provider->attribute(0x01010018, "authorities")) ==
        "com.example.questgame.venture_platform");
  const auto* exported = provider->attribute(0x01010010, "exported");
  REQUIRE(exported);
  CHECK(exported->type == android::axml::kBoolean);
  CHECK(exported->data == 0);
  // Attributes stay in resource-ID order, as Android's parser needs.
  for (std::size_t i = 1; i < provider->attributes.size(); ++i)
    CHECK(provider->attributes[i - 1].resource_id < provider->attributes[i].resource_id);
  // The new elements come before <application>.
  std::vector<std::string> order;
  for (const auto& c : doc.root.children) order.push_back(c.name);
  CHECK(std::ranges::find(order, "queries") < std::ranges::find(order, "application"));
  // The manifest still reads, and adapting again adds nothing.
  CHECK(android::parse_manifest(adapted).package == "com.example.questgame");
  CHECK(nh::adapt_manifest(adapted, plan) == android::axml::serialize(android::axml::parse(adapted)));
}

namespace {
// A dex file whose class_defs define `names` (type descriptors).
std::vector<std::uint8_t> dex_file(const std::vector<std::string>& names) {
  const auto n = std::uint32_t(names.size());
  const std::uint32_t string_ids = 0x70, type_ids = string_ids + 4 * n, class_defs = type_ids + 4 * n;
  std::vector<std::uint8_t> out(class_defs + 32 * n, 0);
  auto put = [&](std::size_t at, std::uint32_t v) { std::memcpy(out.data() + at, &v, 4); };
  std::memcpy(out.data(), "dex\n035", 8);
  put(0x38, n);
  put(0x3c, string_ids);
  put(0x40, n);
  put(0x44, type_ids);
  put(0x60, n);
  put(0x64, class_defs);
  for (std::uint32_t i = 0; i < n; ++i) {
    put(string_ids + 4 * i, std::uint32_t(out.size()));
    out.push_back(std::uint8_t(names[i].size()));
    out.insert(out.end(), names[i].begin(), names[i].end());
    out.push_back(0);
    put(type_ids + 4 * i, i);
    put(class_defs + 32 * i, i);
  }
  return out;
}

fs::path temp_file(const std::string& suffix) {
  return fs::temp_directory_path() /
         ("venture-native-host-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + suffix);
}

struct Member {
  std::string name;
  std::vector<std::uint8_t> data;
  bool stored = false;
};
void write_zip(const fs::path& file, const std::vector<Member>& members) {
  mz_zip_archive z{};
  REQUIRE(mz_zip_writer_init_file(&z, paths::utf8(file).c_str(), 0));
  for (const auto& m : members)
    REQUIRE(mz_zip_writer_add_mem(&z, m.name.c_str(), m.data.data(), m.data.size(),
                                  m.stored ? MZ_NO_COMPRESSION : MZ_DEFAULT_COMPRESSION));
  REQUIRE(mz_zip_writer_finalize_archive(&z));
  REQUIRE(mz_zip_writer_end(&z));
}
std::vector<std::uint8_t> bytes_of(std::string_view s) { return {s.begin(), s.end()}; }
}  // namespace

TEST_CASE("the classes a dex file defines are read from its class table") {
  const auto dex = dex_file({"Lcom/example/Main;", "Lcom/oculus/os/AnalyticsEvent;"});
  CHECK(android::dex::classes(dex) == std::vector<std::string>{"Lcom/example/Main;", "Lcom/oculus/os/AnalyticsEvent;"});
  CHECK(android::dex::classes(dex_file({})).empty());
  CHECK_THROWS(android::dex::classes(bytes_of("PK\3\4 not a dex file at all")));
  auto truncated = dex;
  truncated.resize(0x80);
  CHECK_THROWS(android::dex::classes(truncated));
}

TEST_CASE("an adapted APK keeps the game's entries, drops its signature and aligns stored entries") {
  CHECK(android::stored_alignment("lib/arm64-v8a/libmain.so") == 16384);
  CHECK(android::stored_alignment("resources.arsc") == 4);
  android::ApkEdit edit;
  edit.bytes["AndroidManifest.xml"] = bytes_of("adapted");
  edit.bytes["classes2.dex"] = dex_file({"Lorg/venture_emu/game/PlatformInit;"});
  const auto library = temp_file(".so");
  {
    std::ofstream out(library, std::ios::binary);
    out << std::string(5000, 'L');
  }
  edit.files["lib/arm64-v8a/libvrapi.so"] = library;
  CHECK_FALSE(android::kept_from_game("META-INF/MANIFEST.MF", edit));
  CHECK_FALSE(android::kept_from_game("META-INF/CERT.RSA", edit));
  CHECK_FALSE(android::kept_from_game("META-INF/CERT.SF", edit));
  CHECK_FALSE(android::kept_from_game("AndroidManifest.xml", edit));
  CHECK(android::kept_from_game("META-INF/services/x", edit));
  CHECK(android::kept_from_game("assets/data.bin", edit));

  const auto game = temp_file(".apk"), out = temp_file("-adapted.apk");
  write_zip(game, {{"AndroidManifest.xml", bytes_of("original")},
                   {"META-INF/MANIFEST.MF", bytes_of("Manifest-Version: 1.0")},
                   {"META-INF/CERT.RSA", bytes_of("signature")},
                   {"resources.arsc", bytes_of("abc"), true},
                   {"lib/arm64-v8a/libmain.so", std::vector<std::uint8_t>(3000, 'M'), true},
                   {"lib/arm64-v8a/libvrapi.so", bytes_of("meta vrapi"), true},
                   {"assets/big.bin", std::vector<std::uint8_t>(20000, 'B')}});
  android::rewrite_apk(game, edit, out);

  {
    android::Archive result(out);
    std::set<std::string> names;
    for (const auto& e : result.entries()) names.insert(e.name);
    CHECK(names == std::set<std::string>{"AndroidManifest.xml", "classes2.dex", "resources.arsc",
                                         "lib/arm64-v8a/libmain.so", "lib/arm64-v8a/libvrapi.so", "assets/big.bin"});
    CHECK(result.read(*result.find("AndroidManifest.xml")) == bytes_of("adapted"));
    CHECK(result.read(*result.find("lib/arm64-v8a/libvrapi.so")) == std::vector<std::uint8_t>(5000, 'L'));
    CHECK(result.read(*result.find("lib/arm64-v8a/libmain.so")) == std::vector<std::uint8_t>(3000, 'M'));
    CHECK(result.read(*result.find("assets/big.bin")) == std::vector<std::uint8_t>(20000, 'B'));
    CHECK(android::dex::classes(result.read(*result.find("classes2.dex"))) ==
          std::vector<std::string>{"Lorg/venture_emu/game/PlatformInit;"});
  }

  // Every stored entry's data starts at its alignment.
  {
    mz_zip_archive z{};
    REQUIRE(mz_zip_reader_init_file(&z, paths::utf8(out).c_str(), 0));
    std::ifstream raw(out, std::ios::binary);
    int stored = 0;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&z); ++i) {
      mz_zip_archive_file_stat st;
      REQUIRE(mz_zip_reader_file_stat(&z, i, &st));
      if (st.m_method != 0) continue;
      ++stored;
      std::uint8_t header[30];
      raw.seekg(std::streamoff(st.m_local_header_ofs));
      raw.read(reinterpret_cast<char*>(header), 30);
      const auto data = st.m_local_header_ofs + 30 + (header[26] | header[27] << 8) + (header[28] | header[29] << 8);
      CHECK_MESSAGE(data % android::stored_alignment(st.m_filename) == 0, st.m_filename);
    }
    CHECK(stored >= 3);
    mz_zip_reader_end(&z);
  }

  // Stopping throws.
  std::stop_source stop;
  stop.request_stop();
  CHECK_THROWS(android::rewrite_apk(game, edit, out, stop.get_token()));
  std::error_code ec;
  fs::remove(game, ec);
  fs::remove(out, ec);
  fs::remove(library, ec);
}

TEST_CASE("a game is adapted from the assets the Venture APK carries") {
  std::vector<std::string> taken{"classes.dex", "classes2.dex", "lib/x.so"};
  CHECK(android::next_dex_name(taken) == "classes3.dex");
  CHECK(android::next_dex_name(std::vector<std::string>{}) == "classes.dex");

  const std::string placeholder = "libventure_platform_alias_placeholder_soname_long_enough.so";
  const auto stub = elf64({}, "ovr_PlatformInitializeAndroid", placeholder);
  const auto alias = android::named_alias(stub, "libOVRPlatformRenamed.so");
  CHECK(android::elf::soname(alias) == "libOVRPlatformRenamed.so");
  CHECK(alias.size() == stub.size());
  CHECK_THROWS(android::named_alias(stub, placeholder + "x"));
  CHECK_THROWS(android::named_alias(stub, ""));

  const auto venture_apk = temp_file("-venture.apk"), game = temp_file("-game.apk");
  const auto work = temp_file("-work");
  write_zip(venture_apk, {{"assets/game/classes.dex", dex_file({"Lorg/venture_emu/game/PlatformInit;"})},
                          {"assets/game/lib/arm64-v8a/libvrapi.so", bytes_of("venture vrapi")},
                          {"assets/game/lib/arm64-v8a/libovrplatformloader.so", bytes_of("venture platform")},
                          {"assets/game/lib/arm64-v8a/libopenxr_loader.so", bytes_of("khronos loader")},
                          {"assets/game/platform_alias.so", stub},
                          {"assets/game/lib/armeabi-v7a/libvrapi.so", bytes_of("venture vrapi 32")},
                          {"assets/game/lib/armeabi-v7a/libovrplatformloader.so", bytes_of("venture platform 32")},
                          {"assets/game/lib/armeabi-v7a/libopenxr_loader.so", bytes_of("khronos loader 32")},
                          {"assets/game/lib/armeabi-v7a/platform_alias.so", stub},
                          {"assets/game/meta/com.oculus.os.dex", dex_file({"Lcom/oculus/os/AnalyticsEvent;"})},
                          {"assets/game/meta/horizonos.os.dex", dex_file({"Lhorizonos/os/Build;"})}});
  write_zip(game, {{"AndroidManifest.xml", compile(quest_manifest())},
                   {"classes.dex", dex_file({"Lcom/example/Main;", "Lcom/oculus/os/AnalyticsEvent;"})},
                   {"lib/arm64-v8a/libvrapi.so", bytes_of("meta vrapi")}});
  nh::Plan plan;
  plan.abi = "arm64-v8a";
  plan.replace = {{"libvrapi.so", nh::Supply::vrapi}, {"libOVRPlatformRenamed.so", nh::Supply::platform_alias}};
  plan.meta_java = true;
  plan.platform_broker = true;
  const auto edit = android::adapt_game(game, plan, venture_apk, work);
  REQUIRE(edit.files.contains("lib/arm64-v8a/libvrapi.so"));
  REQUIRE(edit.files.contains("lib/arm64-v8a/libovrplatformloader.so"));
  REQUIRE(edit.bytes.contains("lib/arm64-v8a/libOVRPlatformRenamed.so"));
  CHECK(android::elf::soname(edit.bytes.at("lib/arm64-v8a/libOVRPlatformRenamed.so")) == "libOVRPlatformRenamed.so");
  // Venture's classes, and only the Meta package the game does not define.
  REQUIRE(edit.bytes.contains("classes2.dex"));
  CHECK(android::dex::classes(edit.bytes.at("classes2.dex")) ==
        std::vector<std::string>{"Lorg/venture_emu/game/PlatformInit;"});
  REQUIRE(edit.bytes.contains("classes3.dex"));
  CHECK(android::dex::classes(edit.bytes.at("classes3.dex")) == std::vector<std::string>{"Lhorizonos/os/Build;"});
  CHECK_FALSE(edit.bytes.contains("classes4.dex"));
  const auto doc = android::axml::parse(edit.bytes.at("AndroidManifest.xml"));
  CHECK(child(doc.root, "queries"));

  // An ARM32 game gets the 32-bit libraries the APK carries, its own OpenXR
  // loader included; other ABIs have none.
  const auto contents = [](const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
  };
  auto arm32 = plan;
  arm32.abi = "armeabi-v7a";
  arm32.replace.push_back({"libopenxr_loader.so", nh::Supply::openxr_loader});
  const auto edit32 = android::adapt_game(game, arm32, venture_apk, work);
  REQUIRE(edit32.files.contains("lib/armeabi-v7a/libvrapi.so"));
  CHECK(contents(edit32.files.at("lib/armeabi-v7a/libvrapi.so")) == "venture vrapi 32");
  REQUIRE(edit32.files.contains("lib/armeabi-v7a/libovrplatformloader.so"));
  CHECK(contents(edit32.files.at("lib/armeabi-v7a/libovrplatformloader.so")) == "venture platform 32");
  REQUIRE(edit32.files.contains("lib/armeabi-v7a/libopenxr_loader.so"));
  CHECK(contents(edit32.files.at("lib/armeabi-v7a/libopenxr_loader.so")) == "khronos loader 32");
  REQUIRE(edit32.bytes.contains("lib/armeabi-v7a/libOVRPlatformRenamed.so"));
  CHECK_FALSE(edit32.files.contains("lib/arm64-v8a/libvrapi.so"));
  auto x86 = plan;
  x86.abi = "x86_64";
  CHECK_THROWS(android::adapt_game(game, x86, venture_apk, work));
  std::error_code ec;
  fs::remove(venture_apk, ec);
  fs::remove(game, ec);
  fs::remove_all(work, ec);
}

TEST_CASE("a game whose OpenXR loader Venture supplies may reach the headset's runtime broker") {
  const auto original = compile(quest_manifest());
  nh::Plan plan;
  plan.replace = {{"libvrapi.so", nh::Supply::vrapi}};
  const auto adapted = nh::adapt_manifest(original, plan);
  const auto doc = android::axml::parse(adapted);
  CHECK(child(doc.root, "uses-permission", nh::kOpenXrPermission));
  CHECK(child(doc.root, "uses-permission", nh::kOpenXrSystemPermission));
  CHECK_FALSE(child(doc.root, "uses-permission", "android.permission.INTERNET"));
  const auto* queries = child(doc.root, "queries");
  REQUIRE(queries);
  const auto* provider = child(*queries, "provider");
  REQUIRE(provider);
  CHECK(android::axml::text(*provider->attribute(0x01010018, "authorities")) == nh::kRuntimeBrokers);
  CHECK_FALSE(child(*queries, "package"));
  CHECK(nh::adapt_manifest(adapted, plan) == android::axml::serialize(android::axml::parse(adapted)));
  // A game that keeps its own loader and the Meta pieces gets nothing.
  CHECK(nh::adapt_manifest(original, nh::Plan{}) == android::axml::serialize(android::axml::parse(original)));
}
