#pragma once
#include "xr/api.h"
#include <array>

namespace venture::xr {
using ColorMatrix = std::array<double, 9>; // row major, linear RGB
inline constexpr std::array<XrColorSpaceFB, 8> color_spaces{
    XR_COLOR_SPACE_UNMANAGED_FB, XR_COLOR_SPACE_REC2020_FB,
    XR_COLOR_SPACE_REC709_FB,    XR_COLOR_SPACE_RIFT_CV1_FB,
    XR_COLOR_SPACE_RIFT_S_FB,    XR_COLOR_SPACE_QUEST_FB,
    XR_COLOR_SPACE_P3_FB,        XR_COLOR_SPACE_ADOBE_RGB_FB};
bool valid_color_space(XrColorSpaceFB);
// UNMANAGED bypasses gamut conversion. Texture formats, not these enums,
// determine transfer functions. No tone mapping or alpha transformation.
ColorMatrix color_matrix(XrColorSpaceFB source, XrColorSpaceFB destination);
std::array<double, 3> transform_color(const ColorMatrix &,
                                      std::array<double, 3>);
} // namespace venture::xr
