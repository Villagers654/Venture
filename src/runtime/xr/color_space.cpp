#include "xr/color_space.h"
#include <algorithm>
#include <stdexcept>

namespace venture::xr {
namespace {
constexpr ColorMatrix identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
struct XY {
  double x, y;
};
struct Primaries {
  XY r, g, b, white;
};
// Enum-specific coordinates: Khronos XrColorSpaceFB. Standard Rec709/2020
// and Adobe RGB: Khronos Data Format specification, section 14. Meta's
// Unity table differs from the OpenXR table; the OpenXR ABI uses Khronos.
Primaries primaries(XrColorSpaceFB space) {
  constexpr XY d65{.3127, .3290}, d75{.298, .318};
  switch (space) {
  case XR_COLOR_SPACE_REC709_FB:
    return {{.640, .330}, {.300, .600}, {.150, .060}, d65};
  case XR_COLOR_SPACE_REC2020_FB:
    return {{.708, .292}, {.170, .797}, {.131, .046}, d65};
  case XR_COLOR_SPACE_RIFT_CV1_FB:
    return {{.666, .334}, {.238, .714}, {.139, .053}, d75};
  case XR_COLOR_SPACE_RIFT_S_FB:
    return {{.640, .330}, {.292, .586}, {.156, .058}, d75};
  case XR_COLOR_SPACE_QUEST_FB:
    return {{.661, .338}, {.228, .718}, {.142, .042}, d75};
  case XR_COLOR_SPACE_P3_FB:
    return {{.680, .320}, {.265, .690}, {.150, .060}, {.313, .329}};
  case XR_COLOR_SPACE_ADOBE_RGB_FB:
    return {{.640, .330}, {.210, .710}, {.150, .060}, d65};
  default:
    throw std::invalid_argument("Unknown XR color space");
  }
}
ColorMatrix multiply(const ColorMatrix &a, const ColorMatrix &b) {
  ColorMatrix out{};
  for (unsigned r = 0; r < 3; ++r)
    for (unsigned c = 0; c < 3; ++c)
      for (unsigned k = 0; k < 3; ++k)
        out[r * 3 + c] += a[r * 3 + k] * b[k * 3 + c];
  return out;
}
ColorMatrix inverse(const ColorMatrix &a) {
  ColorMatrix b{a[4] * a[8] - a[5] * a[7], a[2] * a[7] - a[1] * a[8],
                a[1] * a[5] - a[2] * a[4], a[5] * a[6] - a[3] * a[8],
                a[0] * a[8] - a[2] * a[6], a[2] * a[3] - a[0] * a[5],
                a[3] * a[7] - a[4] * a[6], a[1] * a[6] - a[0] * a[7],
                a[0] * a[4] - a[1] * a[3]};
  const auto determinant = a[0] * b[0] + a[1] * b[3] + a[2] * b[6];
  for (auto &v : b)
    v /= determinant;
  return b;
}
std::array<double, 3> xyz(XY p) {
  return {p.x / p.y, 1, (1 - p.x - p.y) / p.y};
}
ColorMatrix rgb_xyz(const Primaries &p) {
  auto r = xyz(p.r), g = xyz(p.g), b = xyz(p.b);
  ColorMatrix a{r[0], g[0], b[0], r[1], g[1], b[1], r[2], g[2], b[2]};
  auto scale = transform_color(inverse(a), xyz(p.white));
  for (unsigned row = 0; row < 3; ++row)
    for (unsigned col = 0; col < 3; ++col)
      a[row * 3 + col] *= scale[col];
  return a;
}
} // namespace
bool valid_color_space(XrColorSpaceFB space) {
  return std::find(color_spaces.begin(), color_spaces.end(), space) !=
         color_spaces.end();
}
std::array<double, 3> transform_color(const ColorMatrix &m,
                                      std::array<double, 3> rgb) {
  std::array<double, 3> out{};
  for (unsigned row = 0; row < 3; ++row)
    for (unsigned col = 0; col < 3; ++col)
      out[row] += m[row * 3 + col] * rgb[col];
  return out;
}
ColorMatrix color_matrix(XrColorSpaceFB source, XrColorSpaceFB destination) {
  if (!valid_color_space(source) || !valid_color_space(destination))
    throw std::invalid_argument("Unknown XR color space");
  if (source == destination || source == XR_COLOR_SPACE_UNMANAGED_FB ||
      destination == XR_COLOR_SPACE_UNMANAGED_FB)
    return identity;
  const auto src = primaries(source), dst = primaries(destination);
  // ICC.1:2022 Annex E.3, linear Bradford adaptation (full adaptation).
  constexpr ColorMatrix bradford{.8951, .2664, -.1614, -.7502, 1.7135,
                                 .0367, .0389, -.0685, 1.0296};
  auto sw = transform_color(bradford, xyz(src.white)),
       dw = transform_color(bradford, xyz(dst.white));
  ColorMatrix scale{dw[0] / sw[0], 0, 0, 0, dw[1] / sw[1], 0, 0, 0,
                    dw[2] / sw[2]};
  auto adapt = multiply(multiply(inverse(bradford), scale), bradford);
  return multiply(multiply(inverse(rgb_xyz(dst)), adapt), rgb_xyz(src));
}
} // namespace venture::xr
