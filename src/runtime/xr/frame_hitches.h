#pragma once
#include <algorithm>
#include <cstdint>

namespace venture::xr {
// Hitches between submitted frames, per periodic report window. A window's
// average rate hides whether its lost frames were one long stall or many
// small misses; the worst interval and the frames that missed at least one
// display (over 1.5 display periods apart) tell a stutter report which it was.
struct FrameHitches {
  std::int64_t last_ns = 0;   // previous submission; kept across windows
  std::int64_t worst_ns = 0;  // longest interval in this window
  std::int64_t excess_ns = 0; // time long frames spent beyond one period
  std::uint32_t long_frames = 0;

  void submitted(std::int64_t now_ns, std::int64_t period_ns) {
    if (last_ns && now_ns > last_ns) {
      const auto interval = now_ns - last_ns;
      worst_ns = std::max(worst_ns, interval);
      if (period_ns > 0 && interval * 2 > period_ns * 3) {
        ++long_frames;
        excess_ns += interval - period_ns;
      }
    }
    last_ns = now_ns;
  }
  // Starts the next report window; the interval spanning the boundary counts
  // in the window it ends in.
  void next_window() { worst_ns = excess_ns = 0; long_frames = 0; }
  // A new session: the first frame has no predecessor.
  void restart() { *this = {}; }
};
}  // namespace venture::xr
