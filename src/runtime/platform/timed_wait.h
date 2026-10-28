#pragma once
#include <cstdint>
#include <memory>
#include <mutex>

namespace venture::platform {
// Steady-clock nanoseconds, equal to std::chrono::steady_clock::now().
std::int64_t steady_ns() noexcept;
// One waiting host thread, with notifications from other threads. The caller
// protects its predicate with lock and rechecks it after every wakeup. Both
// wait_until and notify must be entered with that predicate mutex held.
class TimedWait {
  struct State;
  std::unique_ptr<State> state_;
 public:
  TimedWait();
  ~TimedWait();
  // Absolute steady-clock nanoseconds. Always returns with lock held:
  // zero (possibly spurious wake), -ETIMEDOUT, or another negative Linux errno.
  // notifiable: notify() may end the wait (a futex). A wait only its deadline
  // ends (a sleep) gets the platform's most precise timeout.
  int wait_until(std::unique_lock<std::mutex>& lock, std::int64_t deadline, bool notifiable = true);
  void notify();
};
}
