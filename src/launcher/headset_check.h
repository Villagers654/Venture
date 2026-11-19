// SPDX-License-Identifier: GPL-3.0-or-later
// The headset check (docs/design/native-headsets.md, section 9): what an
// Android headset reports about itself and what Venture plans for each
// installed game, as plain text for the one-button debug report. The facts
// are gathered on the device (launcher/platform/android/headset_play.cpp);
// the text is made here, host independent, so it is tested on x64.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/android/native_host.h"

// The builds with a headset check page (section 9): the Android APK and the
// Linux arm64 build. PC builds have none.
#if defined(__ANDROID__) || (defined(__linux__) && defined(__aarch64__))
#define VENTURE_HEADSET_CHECK 1
#else
#define VENTURE_HEADSET_CHECK 0
#endif

namespace venture::launcher::headset {
// What the check reports beyond the plan's HostProfile.
struct HostFacts {
  std::vector<std::string> abis32;  // Build.SUPPORTED_32_BIT_ABIS
  std::string device;               // manufacturer and model
  std::string runtime;              // the OpenXR runtime's name and version; empty: none
  std::int64_t page_size = 0;
  bool install_permission = false;  // Venture may install apps
  // What each runtime broker names for each ABI (Installer.runtimes).
  struct Runtime {
    std::string broker, abi, package, library;
    bool present = false;
  };
  std::vector<Runtime> runtimes;
};

// One installed game, as the check sees it.
struct GameFacts {
  std::string name, package, version;
  std::optional<android::native_host::ApkProfile> apk;  // empty: its APK could not be read
  std::string apk_error;
  std::int64_t installed_version = -1;  // on the headset; -1: not installed
  bool signed_by_venture = false;
  bool adapted_current = false;  // installed from this version and this Venture
};

std::string describe_host(const android::native_host::HostProfile& host, const HostFacts& facts);

// The Linux arm64 build's check (the Steam Frame and other arm64 Linux
// devices): the sandbox and session the launcher runs in, and what
// `venture-runtime --host-check` printed where Play runs it (runtime/host_check.h):
// the CPU engine for each guest ISA, the GLES path and the OpenXR runtime.
// Gathered in launcher/platform/posix/headset_check.cpp.
struct LinuxFacts {
  std::string sandbox;        // e.g. "Flatpak org.venture_emu.Venture 0.3.0, runtime ..."; empty: none
  std::string session;        // the display server the launcher draws on
  std::string runtime;        // the runtime's lines
  std::string runtime_error;  // why there are none
};
std::string describe_linux(const LinuxFacts& facts);
// The game's plan on this host (native_host::plan), and its install state.
std::string describe_game(const GameFacts& game, const android::native_host::HostProfile& host);
}  // namespace venture::launcher::headset
