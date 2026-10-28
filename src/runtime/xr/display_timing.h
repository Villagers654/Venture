#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>

namespace venture::xr {
// Updates are serialized by Session::mutex; Android readers need no XR lock.
struct DisplayTiming {
  static constexpr std::int64_t default_period = 11'111'111;
  enum class Source { unknown, remembered, predicted, host };
  std::atomic<std::int64_t> period{default_period};
  Source source = Source::unknown;
  std::int64_t candidate = 0;
  unsigned stable_frames = 0;
  float rate() const { return float(1e9 / double(period.load())); }
  // A new XR session needs fresh knowledge, but the physical display and
  // Android consumers retain the last known period across session teardown.
  void restart() {
    if (source != Source::remembered) source = Source::unknown;
    candidate = 0;
    stable_frames = 0;
  }
  void reset() {
    period = default_period;
    source = Source::unknown;
    candidate = 0;
    stable_frames = 0;
  }
  // The host's period from an earlier launch, before this host reports one.
  bool remember(std::int64_t value) {
    if (source != Source::unknown || value < 1'000'000 || value > 1'000'000'000) return false;
    period = value;
    source = Source::remembered;
    return true;
  }
  // Initial knowledge is not a mode change. Predicted-period changes must
  // persist for 30 frames; sub-0.01 Hz jitter never changes the visible mode.
  bool observe(std::int64_t value, Source origin) {
    if (value < 1'000'000 || value > 1'000'000'000) return false;
    if (source == Source::host && origin != Source::host) return false;
    // A remembered period stands in for the host until it reports one.
    if (source == Source::unknown || source == Source::remembered) {
      period = value;
      source = origin;
      return false;
    }
    if (std::abs(1e9 / double(value) - rate()) < .01) {
      candidate = 0;
      stable_frames = 0;
      source = origin;
      return false;
    }
    if (origin != Source::host) {
      if (!candidate || std::abs(1e9 / double(value) - 1e9 / double(candidate)) >= .01) {
        candidate = value;
        stable_frames = 0;
      }
      if (++stable_frames < 30) return false;
    }
    period = value;
    source = origin;
    candidate = 0;
    stable_frames = 0;
    return true;
  }
};
inline DisplayTiming& display_timing_state() {
  static DisplayTiming timing;
  return timing;
}
} // namespace venture::xr
