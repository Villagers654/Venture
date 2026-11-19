#include "common/android/game_adapter.h"

#include "common/android/archive.h"
#include "common/android/dex.h"
#include "common/android/elf.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <stdexcept>

namespace venture::android {
namespace {
constexpr std::string_view kArm64 = "arm64-v8a", kArm32 = "armeabi-v7a";
constexpr std::uint64_t kDexLimit = 512ull << 20;

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error(what); }

bool is_dex(std::string_view name) {
  if (!name.starts_with("classes") || !name.ends_with(".dex")) return false;
  const auto middle = name.substr(7, name.size() - 11);
  return std::ranges::all_of(middle, [](char c) { return c >= '0' && c <= '9'; });
}
}  // namespace

std::string next_dex_name(std::span<const std::string> entries) {
  std::set<std::string> taken(entries.begin(), entries.end());
  if (!taken.contains("classes.dex")) return "classes.dex";
  for (int n = 2;; ++n)
    if (auto name = "classes" + std::to_string(n) + ".dex"; !taken.contains(name)) return name;
}

std::vector<std::uint8_t> named_alias(std::span<const std::uint8_t> stub, std::string_view soname) {
  const auto placeholder = elf::soname(stub);
  if (placeholder.empty()) fail("The Platform alias library has no soname.");
  if (soname.empty() || soname.size() > placeholder.size()) fail("Library name too long for an alias: " + std::string(soname));
  std::string needle = placeholder;
  needle.push_back('\0');
  std::vector<std::uint8_t> out(stub.begin(), stub.end());
  const auto at = std::search(out.begin(), out.end(), needle.begin(), needle.end());
  if (at == out.end() || std::search(at + 1, out.end(), needle.begin(), needle.end()) != out.end())
    fail("The Platform alias library's soname is not unique.");
  std::fill(at, at + std::ptrdiff_t(placeholder.size()), std::uint8_t(0));
  std::copy(soname.begin(), soname.end(), at);
  return out;
}

ApkEdit adapt_game(const fs::path& game, const native_host::Plan& plan, const fs::path& venture_apk,
                   const fs::path& work) {
  if (!plan.blockers.empty()) fail(plan.blockers.front());
  if ((!plan.replace.empty() || plan.quest_layer) && plan.abi != kArm64 && plan.abi != kArm32)
    fail("Venture's libraries for headsets are built for arm64-v8a and armeabi-v7a; this game installs " +
         plan.abi + ".");
  Archive venture(venture_apk), source(game);
  auto asset = [&](std::string_view name) {
    const auto* entry = venture.find(name);
    if (!entry) fail("The Venture APK lacks " + std::string(name) + ".");
    return entry;
  };
  auto bytes = [&](std::string_view name) { return venture.read(*asset(name), 64 << 20); };
  auto file = [&](std::string_view name) {
    const auto* entry = asset(name);
    fs::create_directories(work);
    auto path = work / fs::path(std::string(name)).filename();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    venture.extract(*entry, [&](std::span<const std::uint8_t> chunk) {
      out.write(reinterpret_cast<const char*>(chunk.data()), std::streamsize(chunk.size()));
    });
    if (!out) fail("Cannot write " + path.string() + ".");
    return path;
  };
  const std::string assets(kGameAssets), lib = "lib/" + plan.abi + "/", ours = assets + "lib/" + plan.abi + "/";
  const auto loader = std::string(native_host::kPlatformLoader);
  // Where the APK keeps each library for this ABI: the 64-bit OpenXR loader
  // and Quest layer are the app's own, the 32-bit ones sit with the other
  // 32-bit game libraries.
  const bool a64 = plan.abi == kArm64;
  const std::string alias_asset = a64 ? assets + "platform_alias.so" : ours + "platform_alias.so",
                    openxr_loader = a64 ? "lib/arm64-v8a/libopenxr_loader.so" : ours + "libopenxr_loader.so",
                    quest_layer = a64 ? "assets/quest_layer/libventure_quest_layer.so" : ours + "libventure_quest_layer.so";

  ApkEdit edit;
  std::vector<std::uint8_t> alias;
  auto add_alias = [&](const std::string& name) {
    if (alias.empty()) alias = bytes(alias_asset);
    edit.bytes[lib + name] = named_alias(alias, name);
  };
  for (const auto& r : plan.replace) switch (r.with) {
      case native_host::Supply::vrapi: edit.files[lib + r.library] = file(ours + "libvrapi.so"); break;
      // The app's own copy, which its headset check loads too.
      case native_host::Supply::openxr_loader: edit.files[lib + r.library] = file(openxr_loader); break;
      // Meta's loader becomes Venture's; any other platform library becomes an
      // alias of it, so the game has one Platform instance.
      case native_host::Supply::platform:
        if (r.library == loader) edit.files[lib + loader] = file(ours + loader);
        else add_alias(r.library);
        break;
      case native_host::Supply::platform_alias: add_alias(r.library); break;
    }
  if (plan.platform_broker && !edit.files.contains(lib + loader)) edit.files[lib + loader] = file(ours + loader);
  if (plan.quest_layer) {
    edit.files[lib + "libventure_quest_layer.so"] = file(quest_layer);
    edit.bytes["assets/openxr/1/api_layers/implicit.d/venture_quest_layer.json"] =
        bytes("assets/quest_layer/venture_quest_layer.json");
  }

  // Java: Venture's classes for the Platform broker, and each Meta OS class
  // the game does not define itself.
  std::vector<std::string> names;
  std::set<std::string> defined;
  for (const auto& entry : source.entries()) {
    names.push_back(entry.name);
    if (is_dex(entry.name))
      for (auto& c : dex::classes(source.read(entry, kDexLimit))) defined.insert(std::move(c));
  }
  auto add_dex = [&](std::vector<std::uint8_t> data) {
    const auto name = next_dex_name(names);
    names.push_back(name);
    edit.bytes[name] = std::move(data);
  };
  if (plan.platform_broker) add_dex(bytes(assets + "classes.dex"));
  if (plan.meta_java)
    for (const auto& entry : venture.entries()) {
      if (!entry.name.starts_with(assets + "meta/") || !entry.name.ends_with(".dex")) continue;
      auto data = venture.read(entry, kDexLimit);
      const auto classes = dex::classes(data);
      if (std::ranges::none_of(classes, [&](const std::string& c) { return defined.contains(c); }))
        add_dex(std::move(data));
    }

  const auto* manifest = source.find("AndroidManifest.xml");
  if (!manifest) fail("The game's APK has no manifest.");
  edit.bytes["AndroidManifest.xml"] = native_host::adapt_manifest(source.read(*manifest), plan);
  return edit;
}
}  // namespace venture::android
