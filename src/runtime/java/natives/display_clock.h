#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace venture::java::detail {
// System.nanoTime and steady_clock have independent epochs. A bracketed JNI
// sample bounds their offset error; delayed samples must not move that epoch.
class DisplayClock {
 public:
  bool valid() const { return valid_; }
  bool sample_due(std::int64_t now) const { return !valid_ || now >= next_check_; }
  bool sample(std::int64_t before, std::int64_t java, std::int64_t after) {
    const auto latency = after - before;
    if (latency < 0 || latency > 250'000) {
      if (valid_) next_check_ = after + 1'000'000'000;
      return false;
    }
    const auto offset = java - (before + latency / 2);
    const auto uncertainty = latency / 2;
    if (!valid_ || std::abs(offset - offset_) >
        std::max<std::int64_t>(100'000, uncertainty_ + uncertainty)) {
      offset_ = offset;
      uncertainty_ = uncertainty;
    }
    valid_ = true;
    next_check_ = after + 5'000'000'000;
    return true;
  }
  std::int64_t map(std::int64_t monotonic) const { return monotonic + offset_; }
 private:
  bool valid_ = false;
  std::int64_t offset_ = 0, uncertainty_ = 0, next_check_ = 0;
};
}  // namespace venture::java::detail
