#pragma once
#include "core/invalidation_stats.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace venture {
// Cross-thread code invalidation for one JIT thread. The owning thread enters
// and leaves Jit::Run with atomics only, and locks just to apply ranges that
// are pending. Other threads queue ranges, halt the JIT, and wait for the run
// they caught to end.
//
// The epoch is odd while the owner runs; entry and exit each add one. All
// operations are sequentially consistent, so a queue() that reads an even
// epoch is ordered before the next entry, which then sees the pending flag.
class InvalidationGate {
 public:
  // Owner, before Jit::Run: apply and clear the pending ranges.
  template <class Apply>
  void enter(Apply&& apply) {
    epoch_.fetch_add(1);
    if (!pending_.load()) return;
    const auto begin = profile::invalidation_tracing ? monotonic_ns() : 0;
    std::lock_guard lock(mutex_);
    pending_.store(false);
    for (const auto& [p, n] : ranges_) apply(p, n);
    if (profile::invalidation_tracing)
      profile::record_invalidation_apply(static_cast<unsigned>(ranges_.size()), static_cast<std::uint64_t>(monotonic_ns() - begin));
    ranges_.clear();
  }
  // Owner, after Jit::Run returns or throws.
  void leave() {
    epoch_.fetch_add(1);
    // A waiter registers under the mutex before testing the epoch, so either
    // it sees this exit or this exit sees it and wakes it after it sleeps.
    if (waiters_.load()) {
      { std::lock_guard lock(mutex_); }
      stopped_.notify_all();
    }
  }
  // Any thread: queue [p, p + n); `halt` stops a running JIT. Returns the
  // epoch to wait for, or 0 when the owner is outside Run (the next entry
  // applies the range).
  template <class Halt>
  std::uint64_t queue(std::uint64_t p, std::uint64_t n, Halt&& halt) {
    std::lock_guard lock(mutex_);
    ranges_.emplace_back(p, n);
    pending_.store(true);
    halt();
    const auto epoch = epoch_.load();
    return epoch & 1 ? epoch + 1 : 0;
  }
  // Any thread: block until the run that queue() caught has ended. The owner
  // never waits for itself.
  void wait(std::uint64_t epoch) {
    if (!epoch || owner_ == std::this_thread::get_id()) return;
    std::unique_lock lock(mutex_);
    waiters_.fetch_add(1);
    stopped_.wait(lock, [&] { return epoch_.load() >= epoch; });
    waiters_.fetch_sub(1);
  }

 private:
  std::mutex mutex_;
  std::condition_variable stopped_;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges_;
  std::atomic<bool> pending_{false};
  std::atomic<std::uint64_t> epoch_{0};
  std::atomic<unsigned> waiters_{0};
  const std::thread::id owner_ = std::this_thread::get_id();
};
}  // namespace venture
