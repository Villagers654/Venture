#pragma once
#include "xr/api.h"
#include <algorithm>

namespace venture::xr {
inline VkExtent2D fit_presentation_extent(VkExtent2D guest, std::uint32_t maximum_width,
                                         std::uint32_t maximum_height) {
  if (!guest.width || !guest.height || !maximum_width || !maximum_height) return guest;
  const double scale = std::min({1.0, double(maximum_width) / guest.width, double(maximum_height) / guest.height});
  return {std::max(1u, std::uint32_t(guest.width * scale)), std::max(1u, std::uint32_t(guest.height * scale))};
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
