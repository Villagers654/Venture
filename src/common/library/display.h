// Display settings: resolution and frame rate overrides, the desktop mirror and
// the game's audio on the PC and the performance overlay, for every game (Settings > Display) and per game
// (a game's Settings tab), and a game's own texture compression over the global
// one (Settings > Runtime). The launcher passes the effective choice to the
// runtime as VENTURE_RESOLUTION, VENTURE_FRAME_RATE, VENTURE_MIRROR and
// VENTURE_MIRROR_EYE (src/runtime/xr/display_override.h), VENTURE_PC_AUDIO
// (src/runtime/audio/output_mirror.h), VENTURE_STATS_OVERLAY
// (src/runtime/xr/stats_overlay.h) and VENTURE_TEXTURE_COMPRESSION(_THRESHOLD)
// (src/runtime/hle/vulkan/texture.cpp).
#pragma once
#include "common/json.h"
#include "common/paths.h"
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace venture::library {
// A headset's eye buffer at render scale 1.0, as that headset gives a game.
// Meta, Render Scale (developers.meta.com/vr/documentation/native/android/
// os-render-scale): Quest 3 1680 x 1760, Quest Pro 1440 x 1584, Quest 2
// 1440 x 1584, Quest 1 1216 x 1344. Oculus Go 1024 x 1024: Meta, "Everything
// You Need to Know To Develop for Oculus Go". Quest 3S matches Quest 3.
struct HeadsetPreset {
  const char *id, *name;
  int width, height;
};
inline constexpr HeadsetPreset headset_presets[] = {
    {"quest3", "Quest 3", 1680, 1760},  {"questpro", "Quest Pro", 1440, 1584},
    {"quest2", "Quest 2", 1440, 1584},  {"quest1", "Quest 1", 1216, 1344},
    {"go", "Oculus Go", 1024, 1024},
};
// Rates offered in the frame rate list, in hertz.
inline constexpr int frame_rates[] = {60, 72, 80, 90, 120, 144, 165, 180, 240};
// Percents of the runtime's pixel count offered for "percent".
inline constexpr int resolution_percents[] = {50, 60, 70, 80, 90, 110, 120, 130, 150, 200};
inline constexpr int min_custom_side = 16, max_custom_side = 16384;
inline constexpr int min_custom_rate = 20, max_custom_rate = 1000;
// A game's texture compression choices, in the order of the global setting's
// numbers (Settings::texture_compression: 0 always, 1 auto, 2 never).
inline constexpr const char *texture_compression_modes[] = {"always", "auto", "never"};
// Percents of GPU memory in use a compression threshold may name.
inline constexpr int min_compression_threshold = 10, max_compression_threshold = 95;

struct DisplaySettings {
  // "runtime", a headset preset id, "percent" or "custom". In a game's
  // settings, "" follows the global setting.
  std::string resolution = "runtime";
  int percent = 120;          // for "percent": of the runtime's pixel count, from resolution_percents
  int width = 0, height = 0;  // for "custom": per eye
  // "runtime", a rate from frame_rates in hertz, or "custom"; "" follows the global setting.
  std::string rate = "runtime";
  int custom_rate = 90;  // for "custom"
  // The desktop mirror window: "cropped" (the left eye filling the window, as
  // native PC VR games show it), "full" (the whole left eye) or "off"; ""
  // follows the global setting.
  std::string mirror = "cropped";
  // The eye the mirror shows: "left" or "right"; "" follows the global setting.
  std::string mirror_eye = "left";
  // The game's audio also on the PC's own output (the system default) while it
  // plays on the headset: "on" or "off"; "" follows the global setting.
  std::string pc_audio = "off";
  // The in-headset performance overlay (rates, eye sizes, PC load): "on" or
  // "off"; "" follows the global setting.
  std::string overlay = "off";
  // A game's decoded textures stored as BC7: a texture_compression_modes id;
  // "" follows the global setting, which Settings::texture_compression keeps
  // (empty here in the global settings).
  std::string texture_compression;
  // For a game's "auto": the percent of GPU memory in use past which new
  // textures are compressed; 0 follows the global threshold.
  int texture_compression_threshold = 0;
  bool operator==(const DisplaySettings &) const = default;
};
// The global settings with no game overrides; a game's starts as inherit.
DisplaySettings global_display_defaults();
DisplaySettings game_display_defaults();
// Settings JSON. Values from other or older versions (unknown choices, such as a
// headset frame rate preset) load as the default: the runtime's, or for a game
// the global setting.
DisplaySettings display_from_json(const Json &j, bool per_game);
Json display_to_json(const DisplaySettings &d);
// A game's settings over the global ones, field by field.
DisplaySettings effective_display(const DisplaySettings &global, const DisplaySettings *game);
// The runtime's VENTURE_RESOLUTION and VENTURE_FRAME_RATE values for effective settings.
std::string resolution_environment(const DisplaySettings &d);
std::string frame_rate_environment(const DisplaySettings &d);
// VENTURE_MIRROR for effective settings.
std::string mirror_environment(const DisplaySettings &d);
// VENTURE_MIRROR_EYE for effective settings.
std::string mirror_eye_environment(const DisplaySettings &d);
// VENTURE_PC_AUDIO for effective settings: "on" or "off".
std::string pc_audio_environment(const DisplaySettings &d);
// VENTURE_STATS_OVERLAY for effective settings: "on" or "off".
std::string overlay_environment(const DisplaySettings &d);
// VENTURE_TEXTURE_COMPRESSION for a game, its own choice over the global one
// (Settings::texture_compression and _threshold): "always", "auto" or "never",
// and for "auto" its VENTURE_TEXTURE_COMPRESSION_THRESHOLD percent (0 otherwise).
std::pair<std::string, int> texture_compression_environment(int global_mode, int global_threshold,
                                                            const DisplaySettings *game);

// The host display the last game launch reported (host-display.json, written by
// the runtime): recommended and maximum eye size, refresh rate and the rates the
// host lists (none when it sets its rate itself).
struct HostDisplay {
  std::string runtime, system;
  int width = 0, height = 0, max_width = 0, max_height = 0;
  double rate = 0;
  std::vector<double> rates;
};
std::optional<HostDisplay> load_host_display(const fs::path &data);
// An eye size of `width` x `height` fitted within a maximum (zero: none),
// keeping its aspect ratio, at least min_custom_side a side. The runtime sizes
// its overrides with it, the launcher shows the result.
std::pair<int, int> fit_eye_size(double width, double height, int max_width, int max_height);
// The eye size `percent` of the host's pixel count gives (each side by its square root).
std::pair<int, int> percent_size(const HostDisplay &host, int percent);
} // namespace venture::library
