#pragma once
#include <chrono>
#include <optional>
#include <array>
#include <span>
#include "platform/runtime_child.h"
#include "common/environment.h"
#include "common/quit_bound.h"

namespace venture::platform {
// Delivery preparation has its own bound; it cannot consume posted teardown
// grace. Non-lifecycle fixtures retain their request-based cancellation.
class RuntimeTeardownGrace {
 public:
  using Clock = std::chrono::steady_clock;
  explicit RuntimeTeardownGrace(bool acknowledged,
      std::chrono::milliseconds posted_bound =
          configured_quit_backstop() + std::chrono::seconds(5))
      : acknowledged_(acknowledged), posted_bound_(posted_bound) {}
  bool request(Clock::time_point now) {
    if (requested_) return false;
    requested_ = now;
    if (!acknowledged_) posted(now);
    return true;
  }
  void posted(Clock::time_point now) { if (!posted_) posted_ = now; }
  // The worker waits for a file save that is its only remaining work: its
  // window grows to save_quit_bound plus the destroy settle bound, and this
  // deadline keeps the same flush margin beyond it.
  void saving() { saving_ = true; }
  bool expired(Clock::time_point now) const {
    // Once the worker commits to its exit stages (guest/Java stop, save
    // flush, Vulkan/XR/audio shutdown, log flush), those stages have their own
    // 10 s budget: a destroy settle that used its full bound must not leave
    // them cut off by the posted deadline mid-shutdown.
    if (terminal_) return now - *terminal_ >= std::chrono::seconds(10);
    if (exit_requested_ && now - *exit_requested_ >= std::chrono::minutes(2)) return true;
    if (posted_)
      return now - *posted_ >= (saving_ ? std::max<std::chrono::milliseconds>(
                                              posted_bound_, save_quit_bound + destroy_settle_bound + std::chrono::seconds(5))
                                        : posted_bound_);
    return requested_ && now - *requested_ >= std::chrono::minutes(2);
  }
  template<class Report> void consume(std::span<const char> bytes, Clock::time_point now, Report report) {
    for (const unsigned char byte : bytes) {
      if (!used_ && byte == 1) { posted(now); continue; }
      if (!used_ && byte == 3) { saving(); continue; }
      if (!used_ && byte != 2) continue;
      wire_[used_++] = byte;
      if (used_ != wire_.size()) continue;
      used_ = 0;
      auto decode = [&](unsigned offset) {
        unsigned value = 0;
        for (unsigned i = 0; i != 4; ++i) value |= unsigned(wire_[offset+i]) << (8*i);
        return value;
      };
      const auto wait = static_cast<RuntimeExitWait>(wire_[1]);
      if (wait == RuntimeExitWait::os_termination) os_termination = true;
      if (!exit_requested_) exit_requested_ = now;
      if (wait >= RuntimeExitWait::cache_flush && wait <= RuntimeExitWait::restart_publication)
        terminal_.reset(); // Restart diagnostics have their own preparation budget.
      if (wait >= RuntimeExitWait::log_flush && !terminal_) terminal_ = now;
      // A competing arbitration request must not hide the committing waiter.
      if (!terminal_ || wait >= RuntimeExitWait::log_flush) {
        exit_wait = wait; exit_wait_tid = decode(2);
      }
      report(wait, decode(2), decode(6));
    }
  }
  RuntimeExitWait exit_wait = RuntimeExitWait::none;
  unsigned exit_wait_tid = 0;
  bool os_termination = false;
 private:
  std::array<unsigned char, 10> wire_{};
  unsigned used_ = 0;
  std::optional<Clock::time_point> exit_requested_, terminal_;
  bool acknowledged_, saving_ = false;
  const std::chrono::milliseconds posted_bound_;
  std::optional<Clock::time_point> requested_, posted_;
};
}
