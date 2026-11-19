// SPDX-License-Identifier: GPL-3.0-or-later
#include "launcher/headset_check.h"

#include <algorithm>
#include <format>

namespace venture::launcher::headset {
namespace nh = android::native_host;
namespace {
std::string join(const std::vector<std::string>& items, std::string_view separator = ", ") {
  std::string out;
  for (const auto& item : items) out += (out.empty() ? "" : std::string(separator)) + item;
  return out.empty() ? "none" : out;
}
const char* yes(bool value) { return value ? "yes" : "no"; }
const char* supply_name(nh::Supply supply) {
  switch (supply) {
    case nh::Supply::vrapi: return "Venture's VrApi (on the headset's OpenXR runtime)";
    case nh::Supply::openxr_loader: return "the Khronos loader (finds the headset's runtime)";
    case nh::Supply::platform: return "Venture's Platform loader";
    case nh::Supply::platform_alias: return "an alias of Venture's Platform loader";
  }
  return "?";
}
}  // namespace

std::string describe_host(const nh::HostProfile& host, const HostFacts& facts) {
  std::string out;
  if (!facts.device.empty()) out += std::format("Device: {}\n", facts.device);
  out += std::format("Android API {}, page size {}\n", host.android_api, facts.page_size);
  out += std::format("ABIs Venture uses, preferred first: {}\n", join(host.abis));
  out += std::format("32-bit ABIs (Build.SUPPORTED_32_BIT_ABIS): {}\n", join(facts.abis32));
  const bool a32 = std::ranges::find(host.abis, "armeabi-v7a") != host.abis.end();
  out += a32 ? "ARM32 games: run natively with Venture's 32-bit libraries\n"
             : "ARM32 games: cannot run here (the CPU has no 32-bit mode)\n";
  if (facts.runtime.empty())
    out += "OpenXR: no runtime answered through the Khronos loader\n";
  else
    out += std::format("OpenXR: {}, accepts version 1.{}\n", facts.runtime, host.xr_api_minor);
  for (const auto& r : facts.runtimes)
    out += std::format("Runtime broker ({}, {}): {} {}{}\n", r.broker, r.abi, r.package, r.library,
                       r.present ? "" : " (library not found)");
  if (facts.runtimes.empty()) out += "Runtime brokers: none answered\n";
  std::vector<std::string> extensions;
  for (const auto& [name, version] : host.xr_extensions) extensions.push_back(std::format("{} v{}", name, version));
  out += std::format("OpenXR extensions ({}): {}\n", extensions.size(), join(extensions));
  out += std::format("Meta VrApi on the device: {}; Oculus Platform service: {}\n", yes(host.vrapi),
                     yes(host.platform_service));
  out += std::format("Venture may install apps: {}\n", yes(facts.install_permission));
  out += std::format("Venture may place expansion files in Android/obb: {}\n", yes(facts.obb_access));
  out += std::format("Launch categories its VR apps share: {}\n", join(host.launch_categories));
  std::vector<std::string> metadata;
  for (const auto& [key, value] : host.launch_metadata) metadata.push_back(key + "=" + value);
  out += std::format("Launch meta-data its VR apps share: {}\n", join(metadata));
  return out;
}

std::string describe_linux(const LinuxFacts& facts) {
  std::string out;
  out += std::format("Sandbox: {}\n", facts.sandbox.empty() ? "none" : facts.sandbox);
  if (!facts.session.empty()) out += std::format("Launcher display: {}\n", facts.session);
  if (!facts.runtime.empty()) {
    out += facts.runtime;
    if (out.back() != '\n') out += '\n';
  }
  if (!facts.runtime_error.empty())
    out += std::format("venture-runtime --host-check: {}\n", facts.runtime_error);
  else if (facts.runtime.empty())
    out += "venture-runtime --host-check: printed nothing\n";
  return out;
}

std::string describe_game(const GameFacts& game, const nh::HostProfile& host) {
  std::string out = std::format("\n--- {} ({}) {} ---\n", game.name.empty() ? game.package : game.name, game.package,
                                game.version);
  if (game.installed_version < 0)
    out += "On the headset: not installed\n";
  else
    out += std::format("On the headset: version code {}, signed by Venture: {}, adapted by this Venture: {}\n",
                       game.installed_version, yes(game.signed_by_venture), yes(game.adapted_current));
  if (!game.apk) {
    out += std::format("APK: could not be read ({})\n", game.apk_error.empty() ? "missing" : game.apk_error);
    return out;
  }
  const auto& apk = *game.apk;
  std::vector<std::string> slices;
  for (const auto& [abi, libraries] : apk.slices) slices.push_back(std::format("{} ({} libraries)", abi, libraries.size()));
  out += std::format("APK: API {} to {}, native code: {}\n", apk.min_sdk, apk.target_sdk,
                     slices.empty() ? std::string("none") : join(slices));
  const auto plan = nh::plan(apk, host);
  switch (plan.execution) {
    case nh::Execution::native:
      out += plan.abi.empty() ? "Runs: natively (no native code)\n" : std::format("Runs: natively, {}\n", plan.abi);
      break;
    case nh::Execution::translated_a32:
      out += "Runs: not here; its ARM32 code needs a CPU with a 32-bit mode\n";
      break;
    case nh::Execution::unsupported: out += "Runs: not here\n"; break;
  }
  for (const auto& r : plan.replace) out += std::format("Replaced: {} with {}\n", r.library, supply_name(r.with));
  if (plan.quest_layer)
    out += std::format("Quest layer: answers {}{}\n", join(plan.layer_extensions),
                       plan.lower_api_version ? "; runs the game's OpenXR 1.1 on the runtime's 1.0" : "");
  if (plan.meta_java) out += "Meta's Java classes: added where the game lacks them\n";
  if (!plan.add_categories.empty()) out += std::format("Launch categories added: {}\n", join(plan.add_categories));
  for (const auto& [key, value] : plan.set_metadata) out += std::format("Meta-data set: {}={}\n", key, value);
  for (const auto& blocker : plan.blockers) out += std::format("Blocked: {}\n", blocker);
  return out;
}
}  // namespace venture::launcher::headset
