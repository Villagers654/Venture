#pragma once
#include "core/wait.h"
#include "cpu/cpu.h"
#include <cstdint>
#include <mutex>
#include <source_location>
#include <string_view>

namespace venture::profile {
// VENTURE_TRACE_INVALIDATION=1 (diagnostic, off by default): exact wall time of
// cross-thread code invalidation (queue, halt and the wait for each CPU's
// in-progress run), of the owning thread applying queued ranges, and of
// contended address-space lock acquisitions. Off, each site costs one branch.
extern const bool invalidation_tracing;
void record_invalidation(InvalidationCause cause, std::uint64_t bytes, unsigned cpus, unsigned running,
                         std::int64_t start_ns, std::uint64_t wall_ns) noexcept;
void record_invalidation_apply(unsigned ranges, std::uint64_t wall_ns) noexcept;
void record_address_space_wait(std::int64_t start_ns, std::uint64_t wall_ns, const std::source_location& site) noexcept;
void record_address_space_hold(std::int64_t start_ns, std::uint64_t wall_ns, const std::source_location& site) noexcept;
void report_invalidation(std::string_view phase) noexcept;
// The "exit" report, once: from atexit, or from a terminal exit that skips atexit.
void report_invalidation_at_exit() noexcept;

// The address-space mutex. With tracing on, contended acquisitions are timed, and so is every hold (holds of 1 ms or
// more are recorded with the locking site and thread, so a long hold names its critical section).
template <class Mutex>
class TimedLock {
  std::unique_lock<Mutex> lock_;
  std::source_location site_;
  std::int64_t acquired_ = 0;
 public:
  explicit TimedLock(Mutex& mutex, std::source_location site = std::source_location::current())
      : lock_(mutex, std::defer_lock), site_(site) {
    if (!invalidation_tracing) { lock_.lock(); return; }
    if (!lock_.try_lock()) {
      const auto begin = monotonic_ns();
      lock_.lock();
      record_address_space_wait(begin, static_cast<std::uint64_t>(monotonic_ns() - begin), site_);
    }
    acquired_ = monotonic_ns();
  }
  ~TimedLock() {
    if (!invalidation_tracing) return;
    const auto held = static_cast<std::uint64_t>(monotonic_ns() - acquired_);
    if (held >= 1'000'000) record_address_space_hold(acquired_, held, site_);
  }
  TimedLock(const TimedLock&) = delete;
  TimedLock& operator=(const TimedLock&) = delete;
};
}  // namespace venture::profile
