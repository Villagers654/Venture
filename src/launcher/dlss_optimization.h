#pragma once
// Per-game "DLSS VR Optimization" (the game page tab): Venture's experimental
// render scale + DLSS path (src/runtime/xr/dlss.h) and the Cheeky Foveated
// DLSS Vulkan layer on top of it. Saved in <data>/dlss-optimization.json.
// At launch, apply_dlss_optimization() adds the runtime's variables and deploys
// Cheeky beside the runtime (its Vulkan layer only activates from the runtime's
// folder). Cheeky (github.com/ClarkCheekyKent/CheekyFoveatedDLSS) and NVIDIA's
// DLLs are not shipped: the tab points at folders the user provides.
#include "common/json.h"
#include "common/paths.h"
#include "common/process.h"
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace venture::launcher {
struct DlssGameProfile {
  bool enabled = false;          // render scale + DLSS in Venture
  bool nr_thumbstick = false;    // Toggle NR: both thumbstick clicks held 2 s (VENTURE_NR_THUMBSTICK)
  int source_percent = 50;       // per-eye render size, percent of the headset's (30..100)
  bool cheeky = true;            // Cheeky layer over Venture's DLSS
  bool eye_tracking = true;      // Cheeky CenterMode: gaze (1) or fixed (0)
  bool super_resolution = true;  // Cheeky foveated SR (Enabled)
  bool sr_debug = false;         // AlignmentBorder
  int sr_width = 55, sr_height = 45;  // percent
  bool neural_rendering = false;      // Cheeky DLSS-NR (NrEnabled)
  bool nr_debug = false;              // NrAlignmentBorder
  bool nr_foveated = true;
  int nr_width = 56, nr_height = 56;  // percent
  // Other Cheeky settings by INI key (cheeky_options()); absent keys keep
  // their default.
  std::map<std::string, float, std::less<>> values;

  float value(std::string_view key) const;
  void set(std::string_view key, float value);
};

// SR's starting settings, set for a new profile and when SR is turned on:
// debug view, 45 x 45 % area, 80 % source, FSR1 EASU periphery at 0.30,
// center supersampling 1.00.
void apply_sr_defaults(DlssGameProfile&);
// NR's starting settings, set for a new profile and when NR is turned on:
// debug view, foveated 30 x 30 %, no adaptive cache, one pass at working scale
// 0.80, intensity and strengths 1.00, NR model B, Neutral style.
void apply_nr_defaults(DlssGameProfile&);

// A Cheeky INI setting the tab edits, with Cheeky 0.5.5's default.
struct CheekyOption {
  const char* key;
  float initial;
  bool integer;  // toggles and choices
  bool toggle = false;  // "true" / "false" in a live command
};
// Every setting in `values`. Per-pass NR settings exist as Nr<Name> (pass 1),
// NrPass2<Name> and NrPass3<Name>.
const std::vector<CheekyOption>& cheeky_options();
std::string nr_pass_key(int pass, std::string_view name);

struct DlssOptimization {
  fs::path cheeky_dir;  // a Cheeky Standalone package: CheekyVulkanLayer.dll + CheekyFoveatedDLSS/
  fs::path nvidia_dir;  // nvngx_dlss.dll (and nvngx_dlssnr.dll for NR)
  std::map<std::string, DlssGameProfile, std::less<>> games;  // by package

  static DlssOptimization load(const fs::path& data);
  void save(const fs::path& data) const;
  DlssGameProfile profile(std::string_view package) const;
};

// What the tab reports about the files it needs beside the runtime.
struct DlssReadiness {
  bool bridge = false;        // venture_ngx.dll beside the runtime
  bool dlss_model = false;    // nvngx_dlss.dll in the NVIDIA folder or beside the runtime
  bool cheeky_package = false;
  bool nr_model = false;      // nvngx_dlssnr.dll
};
DlssReadiness dlss_readiness(const DlssOptimization&, const fs::path& runtime);

// Apply in game: writes the game's Cheeky settings to the INI beside the
// runtime and asks the running runtime to apply them (xr/cheeky_live.h).
// Returns the request id, or empty with `error` set.
std::string apply_in_game(const DlssOptimization&, std::string_view package, const fs::path& runtime,
                          std::string& error);
// The runtime's answer to a request: applied, refused, or not yet answered.
std::optional<bool> in_game_status(const fs::path& runtime, std::string_view request);

// Adds the runtime's variables for this game and deploys Cheeky. Returns
// problems worth showing (the launch still proceeds with what works).
std::vector<std::string> apply_dlss_optimization(const DlssOptimization&, std::string_view package,
                                                 const fs::path& runtime, Environment& environment);
}  // namespace venture::launcher
