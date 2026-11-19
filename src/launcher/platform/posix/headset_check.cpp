// SPDX-License-Identifier: GPL-3.0-or-later
// The Linux arm64 build's headset check (headset_check.h, LinuxFacts):
// gathered where the launcher runs, with venture-runtime --host-check started
// the way Play starts it, inside the same sandbox.
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>

#include "common/environment.h"
#include "common/paths.h"
#include "common/process.h"
#include "launcher/headset_check.h"
#include "launcher/headset_play.h"

namespace venture::launcher::headset {
namespace {
// "key=value" lines of an INI-style file such as /.flatpak-info, by section.
std::string ini_value(const fs::path& file, std::string_view section, std::string_view key) {
  std::ifstream in(file);
  std::string line, current;
  while (std::getline(in, line)) {
    if (line.starts_with('[')) current = line;
    else if (current == section && line.starts_with(std::string(key) + "="))
      return line.substr(key.size() + 1);
  }
  return {};
}

std::string sandbox() {
  const char* id = environment::getenv("FLATPAK_ID");
  if (!id || !*id) return {};
  // Flatpak describes the sandbox in /.flatpak-info.
  const fs::path info = "/.flatpak-info";
  std::string out = std::format("Flatpak {}", id);
  if (auto branch = ini_value(info, "[Instance]", "branch"); !branch.empty()) out += " (" + branch + ")";
  if (auto runtime = ini_value(info, "[Application]", "runtime"); !runtime.empty()) out += ", runtime " + runtime;
  if (auto version = ini_value(info, "[Instance]", "flatpak-version"); !version.empty())
    out += ", Flatpak " + version;
  return out;
}

std::string session() {
  const char* wayland = environment::getenv("WAYLAND_DISPLAY");
  const char* desktop = environment::getenv("XDG_CURRENT_DESKTOP");
  std::string out = wayland && *wayland ? std::format("Wayland ({})", wayland) : "no Wayland display";
  if (desktop && *desktop) out += std::format(", desktop {}", desktop);
  // gamescope (SteamOS's game mode and the Frame's VR shell) names its own display.
  if (const char* gamescope = environment::getenv("GAMESCOPE_WAYLAND_DISPLAY"); gamescope && *gamescope)
    out += ", inside gamescope";
  return out;
}
}  // namespace

Check check_linux(const fs::path& runtime, const std::vector<std::pair<std::string, std::string>>& environment,
                  const fs::path& data) {
  LinuxFacts facts{.sandbox = sandbox(), .session = session()};
  const auto log = data / "logs" / "host-check.log";
  try {
    std::error_code ec;
    fs::create_directories(log.parent_path(), ec);
    fs::remove(log, ec);
    Process process;
    process.spawn(runtime, {"--host-check"}, log, false, nullptr, environment);
    // Creating an OpenXR instance can start the VR runtime's service.
    if (auto code = process.wait(20000); !code) {
      process.terminate();
      facts.runtime_error = "no answer in 20 seconds";
    } else if (*code != 0) {
      facts.runtime_error = std::format("exited with code {}", *code);
    }
    facts.runtime = paths::read(log, 64 * 1024);
  } catch (const std::exception& e) {
    facts.runtime_error = e.what();
  }
  return {.text = describe_linux(facts)};
}
}  // namespace venture::launcher::headset
