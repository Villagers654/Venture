#pragma once
// The game page's VR Optimization tab: foveated rendering for one game, eye tracked or with a fixed
// foveated area, in one of three modes:
//   Venture NR  NVIDIA DLSS Neural Rendering on the foveated region inside Venture (xr/venture_nr.h), with
//               the system's OpenXR runtime (SteamVR, Virtual Desktop...). NVIDIA RTX.
//   VDXR NR     the game runs on a VDXR build with DLSS-NR (VirtualDesktop-OpenXR's dlssnr fork, eye tracked),
//               selected for this game only (XR_RUNTIME_JSON). NVIDIA RTX and Virtual Desktop.
//   VRS only    no NR. Any GPU with fragment shading rate, any runtime.
// Foveated variable rate shading (hle/vulkan/render_area.cpp) can be added to each mode; with NR it follows
// the NR region. Saved in <data>/vr-optimization.json.
//
// Most values also apply live, while the game runs: Venture reads HKCU\SOFTWARE\Venture\VRS and \NR, and
// VDXR reads HKCU\SOFTWARE\VirtualDesktop-OpenXR\DLSSNR (over its machine-wide settings) and watches it.
#include "common/paths.h"
#include "common/process.h"
#include <map>
#include <string>
#include <vector>

namespace venture::launcher {

enum class VrMode : int {
  venture_nr = 0,  // the system's OpenXR runtime, Venture NR
  vdxr_nr = 1,     // the VDXR DLSS-NR runtime for this game
  vrs_only = 2,    // the system's runtime, no NR
};

struct VrProfile {
  int mode = int(VrMode::vrs_only);
  // NR look (both NR modes).
  int preset = 2;            // 0 NVIDIA's choice, 1 model A, 2 model B
  int style = 0;             // 0 Neutral, 1 Natural, 2 Cinematic
  int intensity = 100;       // percent
  int local_tone = 0;        // percent (0: no local tone change)
  int local_structure = 70;  // percent
  int skin_structure = 50;   // percent
  int nr_width = 35, nr_height = 35;  // the NR region, percent of each eye's width and height
  bool eye_tracking = true;  // the foveated area follows the gaze, else a fixed stereo-aligned center
  int fixed_distance = 200;  // cm: the fixed center's stereo distance straight ahead (0: infinity)
  bool debug = false;        // green outline around the NR region
  // NR cost: working resolution (its correction applied to the full-size region) and the correction cache.
  int working_scale = 100;   // percent of the region's size
  bool cache = false;        // NR at most every `interval` frames, its last correction between
  int interval = 2;
  int transition = 8;        // percent of the region over which its edge fades into the game's image
  int roundness = 0;         // Venture NR: the region's corners (percent: 0 square, 100 an ellipse)
  // VDXR only.
  int gpu_budget_tenths = 0;    // adaptive cache: NR every frame while it costs less (0.1 ms; 0 = always cache)
  int cache_max_rotation = 20;  // tenths of a degree of head rotation before the cache refreshes
  bool split = false;           // A/B: NR on the region's left half only (VDXR and Venture NR)
  bool motion = true;           // motion vectors from optical flow
  bool use_depth = true;        // the game's depth, when it submits one
  bool mirror = false;          // VDXR's mirror window (the right eye after NR), for an OBS window capture
  bool gaze_per_eye = true;     // each eye's own gaze (else both eyes' average)
  int gaze_smoothing = 0;       // percent kept from the last position
  int gaze_quantization = 8;    // px: the region moves in steps
  int gaze_jump_reset = 30;     // percent of the region: a larger jump restarts NR's history (0: never)
  int gaze_hold_ms = 400;       // gaze lost (blink): the region stays in place
  int motion_scale_x = 100, motion_scale_y = 100;  // percent
  int flow_radius = 2;          // search radius, in blocks
  int flow_penalty = 20;        // thousandths: prefer the predicted motion
  // Foveated VRS: full rate in a vrs_width x vrs_height % area (or the NR region plus vrs_margin), 2x2 or
  // 4x4 (vrs_rate) outside.
  bool vrs = false;
  bool vrs_debug = false;       // red outline around the full-rate area (Venture NR and VDXR NR)
  int vrs_width = 45, vrs_height = 45;
  int vrs_rate = 2;
  int vrs_margin = 10;          // percent of the image around the NR region
  int vrs_gaze_smoothing = 50;  // Venture's own gaze: percent kept from the last position
  int vrs_gaze_hold_ms = 300;
  // Supersampling: each eye renders at this percent of the headset's size (100 = native, up to 200) and the
  // compositor scales it down; VRS keeps the periphery's cost down (VENTURE_RENDER_SCALE).
  int supersample = 100;
  // The in-headset performance overlay (VENTURE_STATS_OVERLAY): frame times, VRS and NR state.
  bool overlay = false;
  // Toggle NR with a controller shortcut: 0 both thumbstick clicks, 1 the left one, 2 the right one, held
  // nr_shortcut_ms.
  bool nr_toggle = false;
  int nr_shortcut = 0;
  int nr_shortcut_ms = 2000;

  VrMode vr_mode() const { return VrMode(mode); }
  bool venture_nr() const { return vr_mode() == VrMode::venture_nr; }
  bool vdxr_nr() const { return vr_mode() == VrMode::vdxr_nr; }
  bool any_nr() const { return venture_nr() || vdxr_nr(); }
};

struct VrOptimization {
  fs::path runtime_json;  // the VDXR DLSS-NR build's manifest (virtualdesktop-openxr.json)
  fs::path nvidia_dir;    // the folder with nvngx_dlssnr.dll (Venture NR)
  std::map<std::string, VrProfile, std::less<>> games;  // by package

  static VrOptimization load(const fs::path& data);
  void save(const fs::path& data) const;
  VrProfile profile(std::string_view package) const;
};

// What Venture NR needs beside the runtime and in the NVIDIA folder.
struct VrReadiness {
  bool bridge = false;    // venture_ngx.dll beside the runtime (tools/ngx-bridge)
  bool nr_model = false;  // nvngx_dlssnr.dll in the NVIDIA folder or beside the runtime
  bool vdxr = false;      // the VDXR manifest exists
};
VrReadiness vr_readiness(const VrOptimization&, const fs::path& runtime);

// What the PC offers the modes: the GPU (NR needs an NVIDIA RTX), Virtual Desktop's streamer (VDXR NR) and
// the active OpenXR runtime.
struct VrSystemInfo {
  std::string gpu;  // the first hardware adapter's name (NVIDIA's when there is one)
  bool nvidia = false, rtx = false, amd = false;
  bool virtual_desktop = false;  // Virtual Desktop Streamer installed
  bool steamvr = false;          // SteamVR is the active OpenXR runtime
};
VrSystemInfo detect_vr_system();

// The live values a running game reads (they also start each launch with this game's values). False with
// `error` set when the key cannot be written.
bool write_vdxr_settings(const VrProfile&, std::string& error);  // HKCU\SOFTWARE\VirtualDesktop-OpenXR\DLSSNR
bool write_venture_vrs(const VrProfile&, std::string& error);    // HKCU\SOFTWARE\Venture\VRS
bool write_venture_nr(const VrProfile&, std::string& error);     // HKCU\SOFTWARE\Venture\NR
// Makes SteamVR the active OpenXR runtime (HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime); Windows asks for
// administrator rights. False with `error` set when SteamVR is not found or the change is refused.
bool enable_steamvr_runtime(std::string& error);

// At launch: the runtime's variables for this game's profile, its live values, and the VDXR runtime for
// the VDXR mode. Returns problems worth showing (the launch proceeds with what works).
std::vector<std::string> apply_vr_optimization(const VrOptimization&, std::string_view package,
                                               const fs::path& runtime, Environment& environment);

}  // namespace venture::launcher
