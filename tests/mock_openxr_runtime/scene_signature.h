#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mock {
// Coarse scene signature for route gates: mean luma of 32 bins on each of three
// rows (1/4, 1/2 and 3/4 of the host view), 0..1. Channel order does not matter
// (unweighted RGB mean); half-float pixels are clamped to [0, 1]. The rows and
// bins are placed in the host's view angles (host_fov) through the fov the layer
// was rendered with, so runtimes that render other fields of view still match.
constexpr std::size_t signature_bins = 32;
using SceneSignature = std::array<float, 3 * signature_bins>;
inline float half_to_float(std::uint16_t h) {
  const int exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
  float value = exponent == 0 ? std::ldexp(float(mantissa), -24)
              : exponent == 31 ? 1.0f : std::ldexp(float(mantissa | 0x400), exponent - 25);
  return (h & 0x8000) ? -value : value;
}
// The mock's view fov (xrLocateViews), as tangents are taken of angleLeft/Right/Up/Down.
constexpr float host_fov[4]{-0.8f, 0.8f, 0.8f, -0.8f};
// Rows (from the sub-image top) and the pixel span [x0, x1) that cover host_fov in
// an image of width x height rendered with `fov` (left, right, up, down radians).
struct SignatureWindow {
  std::array<std::uint32_t, 3> rows{};
  std::uint32_t x0 = 0, x1 = 0;
};
inline SignatureWindow signature_window(std::uint32_t width, std::uint32_t height, const float fov[4]) {
  const double l = std::tan(fov[0]), r = std::tan(fov[1]), u = std::tan(fov[2]), d = std::tan(fov[3]);
  const double hl = std::tan(host_fov[0]), hr = std::tan(host_fov[1]);
  const double hu = std::tan(host_fov[2]), hd = std::tan(host_fov[3]);
  auto clamp = [](double v, std::uint32_t size) {
    return std::uint32_t(v < 0 ? 0 : v > size - 1 ? size - 1 : v);
  };
  SignatureWindow window;
  if (r <= l || u <= d) return {{height / 4, height / 2, height * 3 / 4}, 0, width};
  for (std::size_t row = 0; row < 3; ++row)
    window.rows[row] = clamp((u - (hu - (hu - hd) * double(row + 1) / 4)) / (u - d) * height, height);
  window.x0 = clamp((hl - l) / (r - l) * width, width);
  window.x1 = std::max(window.x0 + 1, clamp((hr - l) / (r - l) * width, width) + 1);
  return window;
}
// rows[i] points at a sub-image row's pixels (4 channels, 8-bit or (fp16) 16-bit);
// the bins split window.x0..x1.
inline SceneSignature scene_signature(const unsigned char* const rows[3], const SignatureWindow& window, bool fp16) {
  const std::uint32_t width = window.x1 - window.x0;
  SceneSignature out{};
  for (std::size_t row = 0; row < 3; ++row)
    for (std::size_t bin = 0; bin < signature_bins; ++bin) {
      const std::uint32_t begin = window.x0 + std::uint32_t(bin * width / signature_bins);
      const std::uint32_t end = window.x0 + std::uint32_t((bin + 1) * width / signature_bins);
      double sum = 0; std::uint32_t count = 0;
      for (std::uint32_t x = begin; x < end; x += 4, ++count)
        for (int c = 0; c < 3; ++c) {
          if (fp16) {
            std::uint16_t h; std::memcpy(&h, rows[row] + (std::size_t(x) * 4 + c) * 2, 2);
            const float v = half_to_float(h);
            sum += v < 0 ? 0 : v > 1 ? 1 : v;
          } else sum += rows[row][std::size_t(x) * 4 + c] / 255.0;
        }
      out[row * signature_bins + bin] = count ? float(sum / (3.0 * count)) : 0;
    }
  return out;
}
inline float signature_distance(const SceneSignature& a, const SceneSignature& b) {
  double sum = 0;
  for (std::size_t i = 0; i < a.size(); ++i) sum += std::fabs(double(a[i]) - b[i]);
  return float(sum / a.size());
}
}
