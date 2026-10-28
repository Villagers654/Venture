#pragma once
#include "common/environment.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <string_view>

namespace venture::sync_census {
// A sub-option of the existing sampling diagnostic. No clocks, counter
// updates, allocation or logging on synchronization paths when disabled.
inline const bool enabled = [] {
  const auto* profile = environment::getenv("VENTURE_PROFILE");
  const auto* option = environment::getenv("VENTURE_PROFILE_SYNC");
  return profile && *profile && option && std::string_view(option) == "1";
}();
enum class WakeSource { other, mutex, condition, rwlock, semaphore, once, barrier, looper, audio, spin, count };
inline constexpr std::array source_names{
    "other", "mutex", "condition", "rwlock", "semaphore", "once", "barrier", "looper", "audio", "spin"};
enum Metric { interrupts, interrupt_buckets, interrupt_waiters, interrupt_ns,
              looper_timeouts, looper_timeout_ready, audio_active_wakes, audio_idle_wakes, metric_count };
inline constexpr std::array metric_names{
    "interrupts", "interrupt_buckets", "interrupt_waiters", "interrupt_ns",
    "looper_timeouts", "looper_timeout_ready", "audio_active_wakes", "audio_idle_wakes"};
inline std::array<std::atomic<std::uint64_t>, metric_count> metrics{};
struct WakeCounts { std::atomic<std::uint64_t> calls{}, empty{}, threads{}, ns{}; };
inline std::array<WakeCounts, static_cast<unsigned>(WakeSource::count)> wakes{};
inline void add(Metric metric, std::uint64_t count = 1) {
  if (enabled) metrics[metric].fetch_add(count, std::memory_order_relaxed);
}
inline void wake(WakeSource source, int threads, std::uint64_t ns) {
  auto& row = wakes[static_cast<unsigned>(source)];
  row.calls.fetch_add(1, std::memory_order_relaxed);
  row.empty.fetch_add(threads == 0, std::memory_order_relaxed);
  row.threads.fetch_add(threads, std::memory_order_relaxed);
  row.ns.fetch_add(ns, std::memory_order_relaxed);
}
enum class IoSource { resolver, pipe_read, pipe_write, eventfd_read, eventfd_write,
                      poll, select, epoll, timerfd_read, timer_service, timer_dispatch, looper };
inline constexpr std::array io_names{"resolver", "pipe_read", "pipe_write", "eventfd_read", "eventfd_write",
    "poll", "select", "epoll", "timerfd_read", "timer_service", "timer_dispatch", "looper"};
struct IoSample {
  IoSource source;
  std::int32_t guest_tid;
  std::int64_t begin_ns, end_ns, ready_ns, deadline_ns;
  std::uint64_t sleeps, sleep_ns;
  bool ready, timed_out;
};
// Diagnostic only; samples share the profiler's monotonic clock. The normal
// path does no clock reads, allocation, or recorder calls.
class IoScope {
  IoSample sample_;
  void start(IoSource, std::int32_t, std::int64_t);
  void finish();
 public:
  explicit IoScope(IoSource source, std::int32_t tid, std::int64_t deadline = -1) {
    if (enabled) start(source, tid, deadline);
  }
  ~IoScope() { if (enabled) finish(); }
  void slept(std::int64_t begin, std::int64_t end) {
    if (!enabled) return;
    ++sample_.sleeps;
    sample_.sleep_ns += end > begin ? end - begin : 0;
  }
  void readiness(std::int64_t ns = -1) {
    if (enabled) { sample_.ready = true; sample_.ready_ns = ns; }
  }
  void timeout() { if (enabled) sample_.timed_out = true; }
};
}  // namespace venture::sync_census
