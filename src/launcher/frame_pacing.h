#pragma once
#include <algorithm>
#include <cstdint>

namespace venture::launcher {
// A focused text field is not input activity. Keep short animations smooth
// after real events, then refresh at 2 Hz for progress, logs and cursor blink.
// Event storms cannot bypass the frame limit; hidden windows only poll work.
class FramePacing {
public:
  explicit FramePacing(std::uint64_t now) : active_until_(now + 1200), next_(now) {}
  void wake(std::uint64_t now) {
    active_until_ = now + 1200;
    next_ = std::min(next_, rendered_ ? std::max(now, last_ + 17) : now);
  }
  int wait(std::uint64_t now, bool hidden) const {
    if (hidden) return 250;
    return now >= next_ ? 0 : int(std::min(next_ - now, std::uint64_t(250)));
  }
  bool ready(std::uint64_t now, bool hidden) const { return !hidden && now >= next_; }
  void presented(std::uint64_t now) {
    rendered_ = true;
    last_ = now;
    next_ = now + (now < active_until_ ? 17 : 500);
  }
private:
  std::uint64_t active_until_, next_, last_ = 0;
  bool rendered_ = false;
};
} // namespace venture::launcher
