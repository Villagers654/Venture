#pragma once
#include "core/guest.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace venture {
class GuestThread;
class ReadinessQueue;
class ReadinessWaitSet;

// A source owns its queue; subscriptions retain the queue rather than the
// source or its File. Closing a watched file therefore still runs its destructor.
class ReadinessSource {
  std::atomic<ReadinessQueue*> queue_{nullptr};
  std::shared_ptr<ReadinessQueue> owner_;
  std::mutex init_mutex_;
  std::function<void()> changed_;
  std::shared_ptr<ReadinessQueue> acquire();
  friend class ReadinessWaitSet;
 public:
  ReadinessSource() = default;
  ~ReadinessSource();
  ReadinessSource(const ReadinessSource&) = delete;
  ReadinessSource& operator=(const ReadinessSource&) = delete;
  // Install before publishing the source. Native backends use a weak-owner
  // callback to arm/disarm OS readiness when the first/last subscription changes.
  template<class Changed> void set_changed(Changed changed) {
    static_assert(noexcept(changed()));
    changed_ = std::move(changed);
  }
  void notify(std::uint32_t events);
  std::uint32_t interests() const;
};

class ReadinessWaitSet {
  struct Impl;
  alignas(4) std::uint32_t owned_word_ = 0;
  GuestAddr word_;
  std::atomic<bool> parked_{false};
  std::unique_ptr<Impl> impl_;
  std::int64_t deadline_ = INT64_MAX;
  std::int64_t fallback_ns_ = 0;
  void notify();
  friend class ReadinessQueue;
 public:
  explicit ReadinessWaitSet(GuestAddr control_word = 0);
  ~ReadinessWaitSet();
  ReadinessWaitSet(const ReadinessWaitSet&) = delete;
  ReadinessWaitSet& operator=(const ReadinessWaitSet&) = delete;
  std::uint32_t sequence() const;
  void add(ReadinessSource&, std::uint32_t events);
  void clear();
  void reset_limits();
  void deadline(std::int64_t ns);
  void fallback(std::int64_t quantum_ns = 1'000'000);
  // Observe sequence, subscribe, recheck readiness, then park. EAGAIN and
  // deadline expiry cause a recheck; ETIMEDOUT remains visible to diagnostics.
  int wait(GuestThread&, std::uint32_t expected, std::int64_t deadline_ns);
};
} // namespace venture
