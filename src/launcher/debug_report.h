#pragma once
#include "common/paths.h"
#include <stop_token>
#include <string>
#include <vector>

namespace venture::launcher {
struct DebugReport {
  fs::path data;            // Venture's data folder
  std::string version;      // the launcher's version
  std::string package;      // the game; empty picks the most recent launch
  std::string game;         // its name, when known
  std::string app_id;       // its library id, which keys its own settings; empty: global settings only
  fs::path plan;            // its installation manifest, when installed
  // Text removed wherever it appears: account ids, keys, aliases and names.
  std::vector<std::string> private_text;
};
// Writes one plain-text report about a game's last launch into
// `data`/reports and returns its path:
// - what happened (the run's times and result, likely cause, next steps);
// - the system, GPUs and VR devices, the OpenXR and Vulkan setup, the VR
//   runtime services and processes, and Venture's settings: the game's own
//   overrides, the effective values with their source, and the launch's
//   environment from them;
// - the errors in other logs written during the run: SteamVR's, WiVRn's and
//   Envision's, the system journal and crash records (Linux), and Windows'
//   event log;
// - the launch's logs and the launcher's.
// The home folder, user and computer names, account details and anything
// shaped like a token are removed; credentials and crash dumps are never read.
fs::path write_debug_report(const DebugReport &request);
// Uploads a written report to dpaste.com as an unlisted paste that expires
// after 90 days and returns its link. Throws when it cannot.
std::string upload_debug_report(const fs::path &file, std::stop_token stop = {});
// Exposed for tests.
std::string redact(std::string text, const std::vector<std::string> &private_text);
} // namespace venture::launcher
