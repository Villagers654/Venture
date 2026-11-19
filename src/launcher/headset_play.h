// SPDX-License-Identifier: GPL-3.0-or-later
// Play on an Android headset (docs/design/native-headsets.md, 5.2): a game
// runs as its own app, adapted for this headset, signed with the user's game
// key and installed through PackageInstaller. Android only.
#pragma once
#include "common/android/native_host.h"
#include "common/json.h"
#include "common/library/library.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace venture::launcher::headset {
// What this headset reports about itself (section 6): the app's Java probe
// (ABIs, SDK, launch markers, the Platform service), whether a VrApi
// implementation loads, and the OpenXR runtime's version and extensions
// through the Khronos loader. Probed once per process.
const android::native_host::HostProfile& host();

// Development (--smoke-headset): the OpenXR runtime the probe reports, from a
// JSON file, for an emulator that has none: {"runtime": name, "api_minor": N,
// "extensions": {name: version, ...}}. Call before the first probe.
void use_runtime(const std::filesystem::path& file);

// Installs `install` on the headset if this version is not installed yet (or
// Venture's adaptation changed), then starts it. Records it for the Platform
// broker with `account`, and serves its asset downloads with `assets` (see
// serve_assets; empty: none). `status` gets a short sentence for each step.
// Throws with a sentence for the user when the game cannot run or the install
// fails.
void play(const library::Install& install, const std::string& account, const std::filesystem::path& data,
          std::function<Json(const Json&)> assets, std::stop_token stop,
          const std::function<void(std::string)>& status);

// The asset file requests (asset.*) of a game Venture installed, which its
// Platform loader sends through the broker: answered by `handler` (the
// launcher's download::asset_launcher_handler for the install Play started),
// with finished downloads copied to the game's OBB folder and deleted ones
// removed there. nullopt when Venture has not started that game since it
// opened (the game then reports downloads as unavailable).
void serve_assets(std::string package, std::filesystem::path install, std::function<Json(const Json&)> handler);
std::optional<Json> asset_request(std::string_view package, const Json& request);

// The headset check for the debug report (headset_check.h): this headset's
// probes and, for each of `installs` (named by `name`), its plan here and the
// state of its copy on the headset. `logs` gets the logs the Venture
// libraries in `package`'s game wrote (android_game/game_log.h), when it has
// run. Never throws: a failed probe is reported in the text.
struct Check {
  std::string text;
  std::vector<std::filesystem::path> logs;
};
Check check(const std::vector<library::Install>& installs,
            const std::function<std::string(const library::Install&)>& name, std::string_view package,
            const std::filesystem::path& data);
// The Linux arm64 build's check (headset_check.h, LinuxFacts): `runtime`
// --host-check runs with Play's `environment` (library::launch_environment),
// its output kept in `data`/logs/host-check.log. Never throws.
Check check_linux(const std::filesystem::path& runtime,
                  const std::vector<std::pair<std::string, std::string>>& environment,
                  const std::filesystem::path& data);

// Installs a downloaded Venture APK over this copy (in-app update). Android
// asks the user to confirm; the install restarts Venture. Throws on failure.
void update_self(const std::filesystem::path& apk);
}  // namespace venture::launcher::headset
