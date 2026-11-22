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
  return Mode::dynamic;
}

const char* mode_name(Mode mode) {
  switch (mode) {
    case Mode::fixed: return "fixed";
    case Mode::off: return "off";
    default: return "dynamic";
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

RateField::RateField(const Pattern& pattern, const Fov& fov, const Centre& centre) {
  if (pattern.level == Level::none) return;
  tl_ = std::tan(fov.left);
  tr_ = std::tan(fov.right);
  tu_ = std::tan(fov.up);
  td_ = std::tan(fov.down);
  float cx = centre.x, cy = centre.y, cz = centre.z;
  if (!pattern.eye_tracked && pattern.vertical_offset_degrees != 0) {
    const float a = pattern.vertical_offset_degrees / kDegrees;
    cx = 0;
    cy = std::sin(a);
    cz = -std::cos(a);
  }
  const float length = std::sqrt(cx * cx + cy * cy + cz * cz);
  // A centre behind the view (or none) leaves the whole view at full rate.
  if (!(length > 0) || cz / length > -0.05f) return;
  cx_ = cx / length;
  cy_ = cy / length;
  cz_ = cz / length;
  ox_ = -cx_ / cz_;
  oy_ = -cy_ / cz_;
  const auto r = regions(pattern);
  half_cos_ = std::cos(std::min(r.half, 179.0f) / kDegrees);
  quarter_cos_ = std::cos(std::min(r.quarter, 179.0f) / kDegrees);
  none_ = false;
}

Rate RateField::at(float u, float v) const {
  if (none_) return Rate::full;
  // The point's direction on the z = -1 plane, and the cosine of its angle
  // from the centre: dot / |point| (the centre is unit length).
  const float px = tl_ + (tr_ - tl_) * u, py = tu_ + (td_ - tu_) * v;
  const float cosine = (px * cx_ + py * cy_ - cz_) / std::sqrt(px * px + py * py + 1);
  if (cosine <= quarter_cos_) return Rate::quarter;
  if (cosine > half_cos_) return Rate::full;
  // Half rate along the axis the point lies away from the centre on: the
  // radial direction, where acuity falls fastest.
  return std::abs(px - ox_) >= std::abs(py - oy_) ? Rate::half_x : Rate::half_y;
}

Rate rate_at(const Pattern& pattern, const Fov& fov, const Centre& centre, float u, float v) {
  return RateField(pattern, fov, centre).at(u, v);
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
  const RateField field(pattern, fov, centre);
  for (std::uint32_t y = 0; y < height; ++y)
    for (std::uint32_t x = 0; x < width; ++x) {
      const float u = std::min(1.0f, (float(x) + 0.5f) * float(texel_width) / float(framebuffer_width));
      const float v = std::min(1.0f, (float(y) + 0.5f) * float(texel_height) / float(framebuffer_height));
      out[std::size_t(y) * width + x] = shading_rate_value(field.at(u, v));
    }
}

void fill_densities(std::span<std::uint8_t> out, std::uint32_t width, std::uint32_t height, const Pattern& pattern,
                    const Fov& fov, const Centre& centre) {
  if (out.size() < std::size_t(width) * height * 2) return;
  const RateField field(pattern, fov, centre);
  for (std::uint32_t y = 0; y < height; ++y)
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto rate = field.at((float(x) + 0.5f) / float(width), (float(y) + 0.5f) / float(height));
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
