#pragma once
#include "xr/api.h"
#include <algorithm>
#include <cmath>
#include <optional>

namespace venture::xr {
// A projection view rendered with a field of view that contains the host's
// (Unity's Symmetric Projection widens each eye to a symmetric frustum): the
// share of the view's rect, per side, that lies outside the host's field of
// view. Projection images are linear in the tangent of the view angle, so that
// share is exact. Nothing when the fields match (within `epsilon`, in tangent
// units) or when the host's reaches past the view's: then no crop shows it.
struct FovCrop {
  double left = 0, right = 0, up = 0, down = 0;
};
inline std::optional<FovCrop> fov_crop(const XrFovf& view, const XrFovf& host, double epsilon = 1e-4) {
  const double vl = std::tan(view.angleLeft), vr = std::tan(view.angleRight);
  const double vu = std::tan(view.angleUp), vd = std::tan(view.angleDown);
  const double hl = std::tan(host.angleLeft), hr = std::tan(host.angleRight);
  const double hu = std::tan(host.angleUp), hd = std::tan(host.angleDown);
  for (const double v : {vl, vr, vu, vd, hl, hr, hu, hd})
    if (!std::isfinite(v)) return std::nullopt;
  if (!(vr - vl > epsilon) || !(vu - vd > epsilon) || !(hr - hl > epsilon) || !(hu - hd > epsilon))
    return std::nullopt;
  if (hl < vl - epsilon || hr > vr + epsilon || hu > vu + epsilon || hd < vd - epsilon) return std::nullopt;
  FovCrop crop{std::max(0.0, hl - vl) / (vr - vl), std::max(0.0, vr - hr) / (vr - vl),
               std::max(0.0, vu - hu) / (vu - vd), std::max(0.0, hd - vd) / (vu - vd)};
  if (crop.left * (vr - vl) <= epsilon && crop.right * (vr - vl) <= epsilon && crop.up * (vu - vd) <= epsilon &&
      crop.down * (vu - vd) <= epsilon)
    return std::nullopt;
  return crop;
}
// `rect` without the crop's sides. `up_low` says whether the view's up edge is
// at the rect's lowest row offset (Vulkan images; a GLES image has its origin
// at the bottom-left, and XR_FB_composition_layer_image_layout's vertical flip
// swaps either).
inline XrRect2Di crop_view_rect(XrRect2Di rect, const FovCrop& crop, bool up_low) {
  const auto side = [](std::int32_t extent, double share) {
    return std::int32_t(std::lround(double(extent) * std::clamp(share, 0.0, 1.0)));
  };
  const auto low_y = up_low ? crop.up : crop.down, high_y = up_low ? crop.down : crop.up;
  const auto x0 = side(rect.extent.width, crop.left), x1 = rect.extent.width - side(rect.extent.width, crop.right);
  const auto y0 = side(rect.extent.height, low_y), y1 = rect.extent.height - side(rect.extent.height, high_y);
  return {{rect.offset.x + x0, rect.offset.y + y0}, {std::max(1, x1 - x0), std::max(1, y1 - y0)}};
}
inline VkExtent2D fit_presentation_extent(VkExtent2D guest, std::uint32_t maximum_width,
                                         std::uint32_t maximum_height) {
  if (!guest.width || !guest.height || !maximum_width || !maximum_height) return guest;
  const double scale = std::min({1.0, double(maximum_width) / guest.width, double(maximum_height) / guest.height});
  return {std::max(1u, std::uint32_t(guest.width * scale)), std::max(1u, std::uint32_t(guest.height * scale))};
}
// The host chain for a projection-view image whose largest eye rect is `rect`:
// scaled so that rect fits the eye size the game was told (`eye`, the host's
// recommended size or the Display setting). That size already includes the
// host's supersampling; a game multiplier on top only adds pixels the host
// compositor and stream scale away. Overshoots within 5% stay 1:1, where an
// exact copy beats a slight resample.
inline VkExtent2D eye_presentation_extent(VkExtent2D guest, VkExtent2D rect, VkExtent2D eye) {
  if (!guest.width || !guest.height || !rect.width || !rect.height || !eye.width || !eye.height) return guest;
  const double scale = std::min({1.0, double(eye.width) / rect.width, double(eye.height) / rect.height});
  if (scale > 0.95) return guest;
  const auto side = [&](std::uint32_t v) { return std::max(1u, std::uint32_t(v * scale + 0.5)); };
  return {side(guest.width), side(guest.height)};
}
inline XrRect2Di map_presentation_rect(XrRect2Di rect, VkExtent2D guest, VkExtent2D host) {
  if (!guest.width || !guest.height || !host.width || !host.height || (guest.width == host.width && guest.height == host.height)) return rect;
  const auto edge = [](std::int64_t value, std::uint32_t from, std::uint32_t to) {
    return std::int32_t((value * to + from / 2) / from);
  };
  auto x0 = edge(rect.offset.x, guest.width, host.width), y0 = edge(rect.offset.y, guest.height, host.height);
  const auto x1 = edge(std::int64_t(rect.offset.x) + rect.extent.width, guest.width, host.width);
  const auto y1 = edge(std::int64_t(rect.offset.y) + rect.extent.height, guest.height, host.height);
  // A valid subpixel crop can round its start to the far edge. Keep its
  // required one-pixel footprint inside the host image; do not clamp invalid
  // guest rectangles into validity.
  if (rect.offset.x >= 0 && rect.extent.width > 0 && std::int64_t(rect.offset.x) + rect.extent.width <= guest.width)
    x0 = std::min(x0, std::int32_t(host.width - 1));
  if (rect.offset.y >= 0 && rect.extent.height > 0 && std::int64_t(rect.offset.y) + rect.extent.height <= guest.height)
    y0 = std::min(y0, std::int32_t(host.height - 1));
  return {{x0, y0}, {std::max(1, x1 - x0), std::max(1, y1 - y0)}};
}
} // namespace venture::xr
