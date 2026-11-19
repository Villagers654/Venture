#pragma once
// Controls files: a player's controls for one controller, saved to share and
// read back, and SteamVR binding files for Venture (made in SteamVR's binding
// UI, on Venture's fixed "venture_controls" action set) read as Venture controls.
#include "common/controls.h"
#include <string>
#include <string_view>

namespace venture::controls {

// {"ventureControls": 1, "controllers": {"<profile>": <settings entry>}}.
std::string export_file(std::string_view profile, const Choice& choice);

struct Imported {
  Config config;             // the controllers the file sets, each in full
  int mapped = 0, skipped = 0;  // SteamVR bindings Venture took and left out
  bool steamvr = false;
  std::string error;         // nothing usable in the file
};
// A Venture controls file, or a SteamVR binding file. A SteamVR file names its
// controller by controller_type; one Venture does not know is read for
// `fallback_profile` (the controller the page shows).
Imported import_file(std::string_view text, std::string_view fallback_profile);

}  // namespace venture::controls
