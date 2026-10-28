// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
namespace venture::test {
// Vulkan Fixed-Point Data Conversions permits either neighboring integer;
// nearest rounding is recommended, not required. Integral codes stay exact.
// https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#fundamentals-fixedfpconv
inline bool unorm8_matches(unsigned byte, double value) {
  if (!std::isfinite(value)) return false;
  const auto scaled = std::clamp(value, 0., 1.) * 255.;
  return byte == static_cast<unsigned>(std::floor(scaled)) ||
         byte == static_cast<unsigned>(std::ceil(scaled));
}
}
