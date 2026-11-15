#pragma once
#include "xr/api.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace venture::hle::openxr {
// The guest's layer arrangement, described when it changes, for a report of a
// scene that shows wrong (a black menu, a missing panel): each layer's type,
// flags, space, eye, swapchain size, rect, geometry with position and
// orientation, and chained structures
// (named "not enabled" when the guest did not enable their extension, so the
// host never sees them). Rects and poses are left out of the comparison, so
// dynamic resolution and moving panels do not count as changes. xrEndFrame logs
// the result as [xr-layers] with the host layers it became; the debug report
// collects those lines from the whole log.
struct LayerLayoutQueries {
  std::function<std::optional<XrExtent2Di>(XrSwapchain)> chain_size;  // guest swapchain size
  std::function<bool(const char* extension)> enabled;                  // the guest enabled it
};
class LayerLayouts {
 public:
  // At most this many arrangements are described in full; returns to one of them
  // are named by number; after kMaxReports lines nothing more is reported.
  static constexpr std::size_t kMaxDescribed = 32;
  static constexpr unsigned kMaxReports = 200;
  // The line to log for this frame, or nothing when its arrangement did not change.
  std::optional<std::string> change(const XrFrameEndInfo&, const LayerLayoutQueries&);
 private:
  std::vector<std::vector<std::uint64_t>> seen_;
  std::vector<std::uint64_t> key_;
  std::size_t current_ = SIZE_MAX;
  unsigned reports_ = 0;
  bool unknown_ = false;  // the current arrangement was not described (limit reached)
};
// The types of the layers a frame submits to the host, as "projection, quad".
std::string layer_type_list(std::span<const XrCompositionLayerBaseHeader* const>);
} // namespace venture::hle::openxr
