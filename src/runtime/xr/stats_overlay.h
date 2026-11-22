// In-headset performance overlay: a small head-locked panel (an OpenXR quad
// layer, panel_width x panel_height pixels, 0.5 m wide, 1 m ahead and a little
// above the centre of view) on top of the game's layers, for OpenXR and VrApi
// games on any host runtime. It shows the headset's refresh rate, the game's
// frame rate with a graph of both, the longest frame interval of the last half
// second, the eye size the host gets, and the game process's CPU, GPU, RAM and
// VRAM use. A source the host cannot give shows a dash.
//
// VENTURE_STATS_OVERLAY=on (the launcher's "Performance overlay" setting, off
// by default) shows it. Off, Session::end reads one relaxed atomic and nothing
// else runs: no thread, no swapchain, no clock reads.
//
// Threads: a sampler thread, started by the first frame with the overlay on,
// samples the load and paints the panel twice a second. The frame thread
// (extend, in Session::end under the session lock) uploads a new picture only
// when the previous copy has finished and a swapchain image is free, never
// waiting for the GPU or the host, and adds the quad only when the host takes
// one more layer: the game's own layers are never dropped for it. Session::
// destroy releases its swapchain, space and Vulkan objects (reset_session).
#pragma once
#include "common/environment.h"
#include "xr/api.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace venture::xr::stats_overlay {
namespace detail {
inline std::atomic<int> state{-1};  // -1 until VENTURE_STATS_OVERLAY is read
}  // namespace detail
// Shown this run. One relaxed load after the first call.
inline bool enabled() {
  int state = detail::state.load(std::memory_order_relaxed);
  if (state < 0) [[unlikely]] {
    const std::string_view value = [] {
      const char *v = environment::getenv("VENTURE_STATS_OVERLAY");
      return v ? v : "";
    }();
    int unknown = -1;
    detail::state.compare_exchange_strong(unknown, value == "on" || value == "1" ? 1 : 0, std::memory_order_relaxed);
    state = detail::state.load(std::memory_order_relaxed);
  }
  return state != 0;
}
// Tests: shows or hides it regardless of VENTURE_STATS_OVERLAY.
void set_enabled(bool shown);

// ---- The frame thread (Session::end) -----------------------------------------------
// The game's End with the overlay on top, owning everything `info` points to
// (keeping `game`, the caller's owner) so an asynchronous host End may keep it.
// Null while there is no picture yet, the frame is at the host's layer limit,
// or the session has no Vulkan binding of Venture's (a native headset runtime).
struct Frame {
  std::shared_ptr<const void> game;
  std::vector<const XrCompositionLayerBaseHeader *> layers;
  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
};
std::shared_ptr<const Frame> extend(XrSession, const XrFrameEndInfo &, std::shared_ptr<const void> game);
// Session::destroy, under the session and queue locks.
void reset_session(XrSession);

// ---- The picture (pure) ------------------------------------------------------------
inline constexpr std::uint32_t panel_width = 960, panel_height = 80;
// Negative or zero: unknown, shown as a dash.
struct Snapshot {
  double headset_hz = -1, game_fps = -1, worst_ms = -1;
  std::uint32_t eye_width = 0, eye_height = 0;
  double cpu_percent = -1;   // of all logical processors
  double gpu_percent = -1;   // the busiest GPU engine
  double ram_gib = -1;       // the process's committed memory
  double vram_gib = -1;      // device-local memory the process uses
  double vram_percent = -1;  // of the driver's budget
};
// Premultiplied 0xAARRGGBB, row 0 at the top. `headset` and `game` are rate
// histories, oldest first.
std::vector<std::uint32_t> paint(const Snapshot &, std::span<const float> headset, std::span<const float> game);
// A frame of `layers` layers has room for the overlay under the host's limit
// (0: unknown, assumed to have room).
constexpr bool fits(std::size_t layers, std::uint32_t limit) { return !limit || layers < limit; }
}  // namespace venture::xr::stats_overlay
