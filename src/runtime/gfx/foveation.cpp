#include "gfx/foveation.h"

#include <algorithm>
#include <cmath>

namespace venture::gfx::foveation {
namespace {
constexpr float kDegrees = 57.29577951308232f;
}

Mode parse_mode(std::string_view value) {
  if (value == "fixed") return Mode::fixed;
  if (value == "off") return Mode::off;
  return Mode::automatic;
}

const char* mode_name(Mode mode) {
  switch (mode) {
    case Mode::fixed: return "fixed";
    case Mode::off: return "off";
    default: return "automatic";
  }
}

Level effective_level(Level requested) { return requested == Level::none ? Level::medium : requested; }

Regions regions(const Pattern& pattern) {
  // Degrees from the centre where half rate, then quarter rate, begins.
  // Fixed regions sit around the lens centre and stay wide: the eye moves
  // across them. Eye-tracked ones follow the gaze, so they are tighter.
  if (pattern.eye_tracked) {
    switch (pattern.level) {
      case Level::none: return {};
      case Level::low: return {28, 40};
      case Level::medium: return {24, 34};
      case Level::high: return {20, 30};
    }
  }
  // On a Quest 3 field of view these shade about 85%, 67% and 54% of the eye
  // image's fragments (eye-tracked: 47%, 39% and 35%), close to the savings
  // Meta measured for its levels (Low +6.5%, Medium +11.5%, High +21% frame
  // rate in an ALU-bound scene; eye-tracked 36-52% GPU time).
  switch (pattern.level) {
    case Level::none: return {};
    case Level::low: return {46, 58};
    case Level::medium: return {38, 50};
    case Level::high: return {32, 44};
  }
  return {};
}

Rate rate_at(const Pattern& pattern, const Fov& fov, const Centre& centre, float u, float v) {
  if (pattern.level == Level::none) return Rate::full;
  // The point's direction in view space, on the z = -1 plane.
  const float tl = std::tan(fov.left), tr = std::tan(fov.right);
  const float tu = std::tan(fov.up), td = std::tan(fov.down);
  const float px = tl + (tr - tl) * u, py = tu + (td - tu) * v;
  float cx = centre.x, cy = centre.y, cz = centre.z;
  if (!pattern.eye_tracked && pattern.vertical_offset_degrees != 0) {
    const float a = pattern.vertical_offset_degrees / kDegrees;
    cx = 0;
    cy = std::sin(a);
    cz = -std::cos(a);
  }
  const float centre_length = std::sqrt(cx * cx + cy * cy + cz * cz);
  const float point_length = std::sqrt(px * px + py * py + 1);
  if (centre_length <= 0) return Rate::full;
  const float cosine = std::clamp((px * cx + py * cy - cz) / (point_length * centre_length), -1.0f, 1.0f);
  const float eccentricity = std::acos(cosine) * kDegrees;
  const auto r = regions(pattern);
  if (eccentricity >= r.quarter) return Rate::quarter;
  if (eccentricity < r.half) return Rate::full;
  // Half rate along the axis the point lies away from the centre on: the
  // radial direction, where acuity falls fastest.
  const float ox = -cx / cz, oy = -cy / cz;  // the centre on the same plane (cz < 0)
  return std::abs(px - ox) >= std::abs(py - oy) ? Rate::half_x : Rate::half_y;
}

std::uint8_t shading_rate_value(Rate rate) {
  switch (rate) {
    case Rate::half_x: return 1 << 2;      // 2x1
    case Rate::half_y: return 1;           // 1x2
    case Rate::quarter: return 1 << 2 | 1; // 2x2
    default: return 0;                     // 1x1
  }
}

void density_value(Rate rate, std::uint8_t& x, std::uint8_t& y) {
  // 0.5 rounds up so the GPU's density never falls below half.
  x = rate == Rate::half_x || rate == Rate::quarter ? 128 : 255;
  y = rate == Rate::half_y || rate == Rate::quarter ? 128 : 255;
}

void fill_shading_rates(std::span<std::uint8_t> out, std::uint32_t width, std::uint32_t height,
                        std::uint32_t texel_width, std::uint32_t texel_height, std::uint32_t framebuffer_width,
                        std::uint32_t framebuffer_height, const Pattern& pattern, const Fov& fov,
                        const Centre& centre) {
  if (out.size() < std::size_t(width) * height || !framebuffer_width || !framebuffer_height) return;
  for (std::uint32_t y = 0; y < height; ++y)
    for (std::uint32_t x = 0; x < width; ++x) {
      const float u = std::min(1.0f, (float(x) + 0.5f) * float(texel_width) / float(framebuffer_width));
      const float v = std::min(1.0f, (float(y) + 0.5f) * float(texel_height) / float(framebuffer_height));
      out[std::size_t(y) * width + x] = shading_rate_value(rate_at(pattern, fov, centre, u, v));
    }
}

void fill_densities(std::span<std::uint8_t> out, std::uint32_t width, std::uint32_t height, const Pattern& pattern,
                    const Fov& fov, const Centre& centre) {
  if (out.size() < std::size_t(width) * height * 2) return;
  for (std::uint32_t y = 0; y < height; ++y)
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto rate = rate_at(pattern, fov, centre, (float(x) + 0.5f) / float(width), (float(y) + 0.5f) / float(height));
      const auto i = (std::size_t(y) * width + x) * 2;
      density_value(rate, out[i], out[i + 1]);
    }
}

float shaded_fraction(std::span<const std::uint8_t> shading_rates) {
  if (shading_rates.empty()) return 1;
  double sum = 0;
  for (const auto value : shading_rates) sum += 1.0 / double((1u << (value >> 2)) * (1u << (value & 3)));
  return float(sum / double(shading_rates.size()));
}
}  // namespace venture::gfx::foveation
