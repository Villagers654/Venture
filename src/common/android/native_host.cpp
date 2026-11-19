#include "common/android/native_host.h"

#include "common/android/archive.h"
#include "common/android/axml.h"
#include "common/android/elf.h"
#include "common/quest_xr_surface.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string_view>

namespace venture::android::native_host {
namespace {
constexpr std::string_view kArm64 = "arm64-v8a", kArm32 = "armeabi-v7a";

// Game libraries by the file names Meta's SDKs ship them under. A copy under
// another name (an SDK's renamed platform loader) is recognised by its exports
// (ApkProfile::platform_copies).
struct Library {
  std::string_view name;
  Supply supply;
};
constexpr Library kMetaLibraries[]{
    {"libvrapi.so", Supply::vrapi},
    {"libopenxr_loader.so", Supply::openxr_loader},
    {"libovrplatformloader.so", Supply::platform},
    {"libovrplatform.so", Supply::platform},
};
// Present when the game drives VR through OpenXR or VrApi.
constexpr std::string_view kXrLibraries[]{"libvrapi.so", "libopenxr_loader.so", "libOVRPlugin.so"};

bool contains(const std::vector<std::string>& list, std::string_view value) {
  return std::ranges::find(list, value) != list.end();
}
}  // namespace

ApkProfile profile(const Manifest& manifest, std::span<const std::string> entry_names) {
  ApkProfile out;
  out.package = manifest.package;
  out.min_sdk = manifest.min_sdk;
  out.target_sdk = manifest.target_sdk;
  out.required_features = manifest.required_features;
  out.native_libraries = manifest.native_libraries;
  std::set<std::string> activity, alias;
  for (const auto& filter : manifest.launch_filters)
    (filter.alias ? alias : activity).insert(filter.categories.begin(), filter.categories.end());
  out.activity_categories.assign(activity.begin(), activity.end());
  out.alias_categories.assign(alias.begin(), alias.end());
  // lib/<abi>/<name>.so, directly in the slice: the installer extracts nothing deeper.
  for (std::string_view entry : entry_names) {
    if (!entry.starts_with("lib/")) continue;
    entry.remove_prefix(4);
    const auto slash = entry.find('/');
    if (slash == 0 || slash == std::string_view::npos) continue;
    const auto abi = entry.substr(0, slash), name = entry.substr(slash + 1);
    if (name.empty() || name.find('/') != std::string_view::npos || !name.ends_with(".so")) continue;
    out.slices[std::string(abi)].emplace_back(name);
  }
  for (auto& [abi, libraries] : out.slices) std::ranges::sort(libraries);
  return out;
}

bool is_platform_loader(std::span<const std::string> exports) {
  return std::ranges::any_of(exports, [](const std::string& name) {
    return name == "ovr_PlatformInitializeAndroid" || name == "ovr_PlatformInitializeAndroidAsynchronous";
  });
}

ApkProfile profile(const fs::path& apk) {
  Archive archive(apk);
  std::vector<std::string> names;
  names.reserve(archive.entries().size());
  for (const auto& entry : archive.entries()) names.push_back(entry.name);
  auto out = profile(read_apk(apk), names);
  // A platform loader is a few MB; engines are far larger and never one.
  constexpr std::uint64_t kLoaderLimit = 16 << 20;
  for (const auto& [abi, libraries] : out.slices)
    for (const auto& library : libraries) {
      if (std::ranges::any_of(kMetaLibraries, [&](const Library& l) { return l.name == library; })) continue;
      const auto* entry = archive.find("lib/" + abi + "/" + library);
      if (!entry || entry->size > kLoaderLimit) continue;
      if (is_platform_loader(elf::exports(archive.read(*entry, kLoaderLimit))))
        out.platform_copies[abi].push_back(library);
    }
  return out;
}

Plan plan(const ApkProfile& apk, const HostProfile& host) {
  Plan out;
  if (host.android_api > 0 && apk.min_sdk > host.android_api)
    out.blockers.push_back("The game needs Android API " + std::to_string(apk.min_sdk) + "; the headset has " +
                           std::to_string(host.android_api) + ".");

  // The device's preferred ABI that the game ships wins. An ARM32-only game on
  // a CPU without AArch32 runs through Venture's translator instead.
  if (apk.slices.empty()) {
    out.execution = Execution::native;
  } else {
    for (const auto& abi : host.abis)
      if (apk.slices.contains(abi)) {
        out.execution = Execution::native;
        out.abi = abi;
        break;
      }
    if (out.execution == Execution::unsupported && apk.slices.contains(std::string(kArm32)) &&
        contains(host.abis, kArm64)) {
      out.execution = Execution::translated_a32;
      out.abi = kArm32;
    }
    if (out.execution == Execution::unsupported) {
      std::string shipped;
      for (const auto& [abi, libraries] : apk.slices) shipped += (shipped.empty() ? "" : ", ") + abi;
      out.blockers.push_back("The game ships native code only for " + shipped +
                             ", which this headset's CPU cannot run.");
    }
  }

  static const std::vector<std::string> none;
  const auto slice = apk.slices.find(out.abi);
  const auto& libraries = slice != apk.slices.end() ? slice->second : none;
  const bool xr = std::ranges::any_of(kXrLibraries, [&](auto name) { return contains(libraries, name); });

  // Meta's libraries stand in for services a Meta headset provides; replace
  // only those the device does not provide itself.
  for (const auto& library : kMetaLibraries) {
    if (!contains(libraries, library.name)) continue;
    const bool provided = library.supply == Supply::platform ? host.platform_service : host.vrapi;
    if (!provided) out.replace.push_back({std::string(library.name), library.supply});
  }
  if (!host.platform_service)
    if (const auto copies = apk.platform_copies.find(out.abi); copies != apk.platform_copies.end())
      for (const auto& copy : copies->second) out.replace.push_back({copy, Supply::platform_alias});
  out.platform_broker = std::ranges::any_of(out.replace, [](const Replacement& r) {
    return r.with == Supply::platform || r.with == Supply::platform_alias;
  });
  out.meta_java = !host.vrapi;  // Meta's Java classes come with its VR OS

  if (xr) {
    if (!host.xr_extensions.contains("XR_KHR_opengl_es_enable") &&
        !host.xr_extensions.contains("XR_KHR_vulkan_enable") &&
        !host.xr_extensions.contains("XR_KHR_vulkan_enable2"))
      out.blockers.push_back("The headset's OpenXR runtime offers neither OpenGL ES nor Vulkan rendering.");
    // The Android loader interface belongs to the device; Venture answers the
    // Quest extensions its runtime lacks, as the PC runtime does.
    const bool a64 = out.execution == Execution::native && out.abi == kArm64;
    for (const auto& extension : quest_xr::kExtensions) {
      if (extension.kind != quest_xr::Kind::emulated) continue;
      if (extension.isa == quest_xr::Isa::a64 && !a64) continue;
      if (!host.xr_extensions.contains(std::string(extension.name)))
        out.layer_extensions.emplace_back(extension.name);
    }
    out.lower_api_version = host.xr_api_major == 1 && host.xr_api_minor == 0;
    out.quest_layer = !out.layer_extensions.empty() || out.lower_api_version;
  }

  // The device's shell starts what its own VR apps declare. Aliases do not
  // count: some shells start only real activities.
  for (const auto& category : host.launch_categories)
    if (!contains(apk.activity_categories, category) && !contains(out.add_categories, category))
      out.add_categories.push_back(category);
  out.set_metadata = host.launch_metadata;
  return out;
}

namespace {
constexpr std::uint32_t kNameId = 0x01010003, kValueId = 0x01010024, kTargetActivityId = 0x01010202,
                        kExportedId = 0x01010010, kAuthoritiesId = 0x01010018, kInitOrderId = 0x0101001a;

std::string attribute_text(const axml::Node& node, std::uint32_t id, std::string_view name) {
  const auto* a = node.attribute(id, name);
  if (!a && id) a = node.attribute(0, name);  // shrunk manifests keep IDs; hand-made ones may keep names
  return a ? axml::text(*a) : std::string();
}
// "com.x.y.Main" from "com.x.y.Main", ".Main" or "Main" in package com.x.y.
std::string class_name(std::string name, const std::string& package) {
  if (name.starts_with(".")) return package + name;
  if (name.find('.') == std::string::npos) return package + "." + name;
  return name;
}
bool is_main_filter(const axml::Node& filter) {
  if (filter.text || filter.name != "intent-filter") return false;
  for (const auto& child : filter.children)
    if (!child.text && child.name == "action" && attribute_text(child, kNameId, "name") == "android.intent.action.MAIN")
      return true;
  return false;
}
axml::Node named(std::string tag, std::string name) {
  axml::Node node{.name = std::move(tag)};
  node.set(kNameId, "name", std::move(name));
  return node;
}
}  // namespace

std::vector<std::uint8_t> adapt_manifest(std::span<const std::uint8_t> xml, const Plan& plan) {
  auto document = axml::parse(xml);
  auto& root = document.root;
  const auto package = attribute_text(root, 0, "package");
  axml::Node* application = nullptr;
  for (auto& child : root.children)
    if (!child.text && child.name == "application") {
      application = &child;
      break;
    }
  if (!application) throw std::runtime_error("The APK manifest has no application.");

  if (!plan.add_categories.empty()) {
    axml::Node* activity = nullptr;
    axml::Node* filter = nullptr;
    for (auto& child : application->children) {
      if (child.text || child.name != "activity") continue;
      for (auto& f : child.children)
        if (is_main_filter(f)) {
          activity = &child;
          filter = &f;
          break;
        }
      if (activity) break;
    }
    if (!activity) {
      // Only an alias launches: the activity it names gets a MAIN filter.
      std::string target;
      for (auto& child : application->children)
        if (!child.text && child.name == "activity-alias" && std::ranges::any_of(child.children, is_main_filter)) {
          target = class_name(attribute_text(child, kTargetActivityId, "targetActivity"), package);
          break;
        }
      for (auto& child : application->children)
        if (!target.empty() && !child.text && child.name == "activity" &&
            class_name(attribute_text(child, kNameId, "name"), package) == target) {
          activity = &child;
          break;
        }
      if (!activity) throw std::runtime_error("The APK manifest has no activity to launch.");
      auto& added = activity->children.emplace_back(axml::Node{.name = "intent-filter"});
      added.children.push_back(named("action", "android.intent.action.MAIN"));
      filter = &added;
    }
    for (const auto& category : plan.add_categories) {
      const bool present = std::ranges::any_of(filter->children, [&](const axml::Node& c) {
        return !c.text && c.name == "category" && attribute_text(c, kNameId, "name") == category;
      });
      if (!present) filter->children.push_back(named("category", category));
    }
  }

  // The Khronos Android loader Venture supplies (as libopenxr_loader.so or
  // inside libvrapi.so) finds the runtime through the runtime broker's content
  // provider, which Android 11 hides unless the game declares it.
  const bool khronos_loader = std::ranges::any_of(plan.replace, [](const Replacement& r) {
    return r.with == Supply::vrapi || r.with == Supply::openxr_loader;
  });
  if (plan.platform_broker || khronos_loader) {
    auto has = [](const axml::Node& parent, std::string_view tag, std::string_view name) {
      return std::ranges::any_of(parent.children, [&](const axml::Node& c) {
        return !c.text && c.name == tag && attribute_text(c, kNameId, "name") == name;
      });
    };
    auto queried = [&](auto&& match) {
      return std::ranges::any_of(root.children, [&](const axml::Node& c) {
        return !c.text && c.name == "queries" && std::ranges::any_of(c.children, match);
      });
    };
    // Elements before <application>, where aapt puts them.
    std::vector<axml::Node> before;
    auto permission = [&](std::string_view name) {
      if (!has(root, "uses-permission", name)) before.push_back(named("uses-permission", std::string(name)));
    };
    axml::Node queries{.name = "queries"};
    if (plan.platform_broker) {
      permission("android.permission.INTERNET");
      // The Platform broker binds the Venture app's service.
      if (!queried([](const axml::Node& c) {
            return !c.text && c.name == "package" && attribute_text(c, kNameId, "name") == kVenturePackage;
          }))
        queries.children.push_back(named("package", std::string(kVenturePackage)));
    }
    if (khronos_loader) {
      permission(kOpenXrPermission);
      permission(kOpenXrSystemPermission);
      if (!queried([](const axml::Node& c) {
            return !c.text && c.name == "provider" &&
                   attribute_text(c, kAuthoritiesId, "authorities").find(kRuntimeBroker) != std::string::npos;
          })) {
        axml::Node provider{.name = "provider"};
        provider.set(kAuthoritiesId, "authorities", std::string(kRuntimeBrokers));
        queries.children.push_back(std::move(provider));
      }
    }
    if (!queries.children.empty()) before.push_back(std::move(queries));
    const auto index = std::ranges::find_if(root.children, [](const axml::Node& c) {
      return !c.text && c.name == "application";
    }) - root.children.begin();
    root.children.insert(root.children.begin() + index, before.begin(), before.end());
    application = &root.children[std::size_t(index) + before.size()];
    if (plan.platform_broker && !has(*application, "provider", kPlatformProvider)) {
      auto provider = named("provider", std::string(kPlatformProvider));
      provider.set(kAuthoritiesId, "authorities", package + ".venture_platform");
      provider.set_value(kExportedId, "exported", axml::kBoolean, 0);
      // Started before the game's own providers, so its first Platform call finds the library ready.
      provider.set_value(kInitOrderId, "initOrder", axml::kIntDec, 1000);
      application->children.push_back(std::move(provider));
    }
  }

  for (const auto& [key, value] : plan.set_metadata) {
    axml::Node* entry = nullptr;
    for (auto& child : application->children)
      if (!child.text && child.name == "meta-data" && attribute_text(child, kNameId, "name") == key) entry = &child;
    if (!entry) entry = &application->children.emplace_back(named("meta-data", key));
    entry->set(kValueId, "value", value);
  }
  return axml::serialize(document);
}
}  // namespace venture::android::native_host
