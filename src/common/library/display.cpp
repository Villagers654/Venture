#include "common/library/display.h"
#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <ranges>

namespace venture::library {
namespace {
bool preset(std::string_view id) {
  return std::ranges::any_of(headset_presets, [&](const auto &p) { return id == p.id; });
}
bool listed_rate(std::string_view rate) {
  return std::ranges::any_of(frame_rates, [&](int hz) { return rate == std::to_string(hz); });
}
bool side(int v) { return v >= min_custom_side && v <= max_custom_side; }
} // namespace

DisplaySettings global_display_defaults() { return {}; }
DisplaySettings game_display_defaults() {
  DisplaySettings d;
  d.resolution.clear();
  d.rate.clear();
  d.mirror.clear();
  d.mirror_eye.clear();
  d.pc_audio.clear();
  d.overlay.clear();
  return d;
}

DisplaySettings display_from_json(const Json &j, bool per_game) {
  auto d = per_game ? game_display_defaults() : global_display_defaults();
  if (!j.is_object()) return d;
  const auto resolution = str(j, "resolution");
  if (resolution == "runtime" || resolution == "percent" || resolution == "custom" || preset(resolution))
    d.resolution = resolution;
  const auto rate = str(j, "frameRate");
  if (rate == "runtime" || rate == "custom" || listed_rate(rate)) d.rate = rate;
  if (const auto mirror = str(j, "mirror"); mirror == "cropped" || mirror == "full" || mirror == "off") d.mirror = mirror;
  if (const auto eye = str(j, "mirrorEye"); eye == "left" || eye == "right") d.mirror_eye = eye;
  if (const auto pc = str(j, "pcAudio"); pc == "on" || pc == "off") d.pc_audio = pc;
  if (const auto o = str(j, "overlay"); o == "on" || o == "off") d.overlay = o;
  // A game's texture compression; the global one is a top-level setting.
  if (const auto mode = str(j, "textureCompression");
      per_game && std::ranges::find(texture_compression_modes, mode) != std::end(texture_compression_modes))
    d.texture_compression = mode;
  if (const auto t = number(j, "textureCompressionThreshold");
      per_game && t >= min_compression_threshold && t <= max_compression_threshold)
    d.texture_compression_threshold = int(t);
  if (const auto p = number(j, "percent"); p <= 1000 && std::ranges::find(resolution_percents, int(p)) != std::end(resolution_percents))
    d.percent = int(p);
  if (const auto w = number(j, "width"), h = number(j, "height"); w <= max_custom_side && h <= max_custom_side && side(int(w)) && side(int(h))) {
    d.width = int(w);
    d.height = int(h);
  }
  if (const auto hz = number(j, "customFrameRate"); hz >= min_custom_rate && hz <= max_custom_rate)
    d.custom_rate = int(hz);
  // A custom size needs one; without, it is the runtime's.
  if (d.resolution == "custom" && !d.width) d.resolution = per_game ? "" : "runtime";
  return d;
}

Json display_to_json(const DisplaySettings &d) {
  Json j{{"resolution", d.resolution}, {"percent", d.percent}, {"frameRate", d.rate}, {"customFrameRate", d.custom_rate},
         {"mirror", d.mirror}, {"mirrorEye", d.mirror_eye}, {"pcAudio", d.pc_audio}, {"overlay", d.overlay}};
  // Only a game's own choice; the global settings keep none here.
  if (!d.texture_compression.empty()) j["textureCompression"] = d.texture_compression;
  if (d.texture_compression_threshold) j["textureCompressionThreshold"] = d.texture_compression_threshold;
  if (d.width && d.height) {
    j["width"] = d.width;
    j["height"] = d.height;
  }
  return j;
}

DisplaySettings effective_display(const DisplaySettings &global, const DisplaySettings *game) {
  auto d = global;
  if (!game) return d;
  if (!game->resolution.empty()) {
    d.resolution = game->resolution;
    d.percent = game->percent;
    d.width = game->width;
    d.height = game->height;
  }
  if (!game->rate.empty()) {
    d.rate = game->rate;
    d.custom_rate = game->custom_rate;
  }
  if (!game->mirror.empty()) d.mirror = game->mirror;
  if (!game->mirror_eye.empty()) d.mirror_eye = game->mirror_eye;
  if (!game->pc_audio.empty()) d.pc_audio = game->pc_audio;
  if (!game->overlay.empty()) d.overlay = game->overlay;
  return d;
}

std::string resolution_environment(const DisplaySettings &d) {
  for (const auto &p : headset_presets)
    if (d.resolution == p.id) return std::format("{}x{}", p.width, p.height);
  if (d.resolution == "percent") return std::format("{}%", d.percent);
  if (d.resolution == "custom" && side(d.width) && side(d.height)) return std::format("{}x{}", d.width, d.height);
  return "runtime";
}

std::string frame_rate_environment(const DisplaySettings &d) {
  if (listed_rate(d.rate)) return d.rate;
  if (d.rate == "custom" && d.custom_rate >= min_custom_rate && d.custom_rate <= max_custom_rate)
    return std::to_string(d.custom_rate);
  return "runtime";
}

std::string mirror_environment(const DisplaySettings &d) {
  return d.mirror == "full" || d.mirror == "off" ? d.mirror : "cropped";
}

std::string mirror_eye_environment(const DisplaySettings &d) { return d.mirror_eye == "right" ? "right" : "left"; }

std::string pc_audio_environment(const DisplaySettings &d) { return d.pc_audio == "on" ? "on" : "off"; }

std::string overlay_environment(const DisplaySettings &d) { return d.overlay == "on" ? "on" : "off"; }

std::pair<std::string, int> texture_compression_environment(int global_mode, int global_threshold,
                                                            const DisplaySettings *game) {
  std::string mode = texture_compression_modes[std::clamp(global_mode, 0, 2)];
  int threshold = global_threshold;
  if (game && !game->texture_compression.empty()) {
    mode = game->texture_compression;
    // A game's own threshold counts only with its own "auto".
    if (game->texture_compression_threshold) threshold = game->texture_compression_threshold;
  }
  return {mode, mode == "auto" ? threshold : 0};
}

std::optional<HostDisplay> load_host_display(const fs::path &data) {
  try {
    const auto file = data / "host-display.json";
    std::error_code ec;
    if (!fs::exists(file, ec)) return std::nullopt;
    const auto j = paths::read_json(file);
    HostDisplay h;
    h.runtime = str(j, "runtime");
    h.system = str(j, "system");
    const auto &recommended = field(j, "recommended"), &max = field(j, "max");
    if (!recommended.is_array() || recommended.size() != 2 || !recommended[0].is_number_unsigned() ||
        !recommended[1].is_number_unsigned())
      return std::nullopt;
    h.width = recommended[0].get<int>();
    h.height = recommended[1].get<int>();
    if (max.is_array() && max.size() == 2 && max[0].is_number_unsigned() && max[1].is_number_unsigned()) {
      h.max_width = max[0].get<int>();
      h.max_height = max[1].get<int>();
    }
    if (const auto &rate = field(j, "rate"); rate.is_number()) h.rate = rate.get<double>();
    if (const auto &rates = field(j, "rates"); rates.is_array())
      for (const auto &r : rates)
        if (r.is_number() && r.get<double>() > 0) h.rates.push_back(r.get<double>());
    if (h.width <= 0 || h.height <= 0) return std::nullopt;
    return h;
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

std::pair<int, int> fit_eye_size(double width, double height, int max_width, int max_height) {
  double fit = 1;
  if (max_width > 0) fit = std::min(fit, max_width / width);
  if (max_height > 0) fit = std::min(fit, max_height / height);
  const auto round = [&](double v, int limit) {
    int out = std::max(min_custom_side, int(std::floor(v * fit + 0.5)));
    return limit > 0 ? std::min(out, limit) : out;
  };
  return {round(width, max_width), round(height, max_height)};
}

std::pair<int, int> percent_size(const HostDisplay &host, int percent) {
  const double k = std::sqrt(percent / 100.0);
  return fit_eye_size(host.width * k, host.height * k, host.max_width, host.max_height);
}
} // namespace venture::library
