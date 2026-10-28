#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <memory>
#include "core/error.h"

namespace venture::profile {
struct FutexHandoff {
  std::uint64_t address = 0, waiter_lr = 0, waker_lr = 0;
  std::int64_t begin_ns = 0, enqueue_ns = 0, park_ns = 0,
               wake_request_ns = 0, wake_lock_ns = 0, notify_begin_ns = 0, notify_end_ns = 0,
               resumed_ns = 0, end_ns = 0, spin_ns = 0, deadline_ns = -1;
  std::int32_t waker_tid = 0;
  std::uint32_t waiter_activity = 0, waker_activity = 0, parks = 0, expected = 0, bitset = 0;
  int result = 0;
  // Native calling-thread CPU clock units. -1 means unavailable, never zero
  // overhead. Prefix ends before bucket unlock; caller counters cover unlock.
  std::int64_t wake_cpu = -1, bucket_cpu = -1, notify_cpu = -1, wait_cpu = -1;
};
// One owner publishes immutable rows; the report reads only the released
// prefix. No wraparound, writer/reader mutex or allocation in a wait bucket.
template <std::size_t Capacity = 8192> struct FutexHandoffs {
  const std::size_t capacity;
  std::unique_ptr<FutexHandoff[]> rows;
  explicit FutexHandoffs(std::size_t size = Capacity)
      : capacity(size), rows(std::make_unique<FutexHandoff[]>(size)) {}
  std::atomic<std::size_t> published{0};
  std::atomic<std::uint64_t> dropped{0};
  void append(const FutexHandoff& row) {
    const auto index = published.load(std::memory_order_relaxed);
    if (index == capacity) { dropped.fetch_add(1, std::memory_order_relaxed); return; }
    rows[index] = row;
    published.store(index + 1, std::memory_order_release);
  }
  std::span<const FutexHandoff> snapshot() const {
    return {rows.get(), published.load(std::memory_order_acquire)};
  }
};
// Cumulative counters, read at projection boundaries. A wait starts and
// completes in different frames when parked across a boundary.
struct FutexCounters {
  enum Field { waits, wakes, empty_wakes, notifications, parks, completions,
               eagain, timeouts, interrupts, errors, wait_ns,
               lt2us, lt20us, lt200us, longer,
               parked_lt2us, parked_lt20us, parked_lt200us, parked_longer,
               sleep_zero, sleep_lt100us, sleep_lt1ms, sleep_longer, sleep_abs_expired,
               sleep_zero_ns, sleep_lt100us_ns, sleep_lt1ms_ns, sleep_longer_ns, sleep_abs_expired_ns,
               internal_sleeps, internal_sleep_ns, yields, yield_ns,
               sleep_lock_calls, sleep_lock_contended, sleep_lock_ns, sleep_lock_contended_ns,
               futex0_lock_calls, futex0_lock_contended, futex0_lock_ns, futex0_lock_contended_ns,
               wake_cpu_units, wake_cpu_samples, count };
  using Snapshot = std::array<std::uint64_t, count>;
  static constexpr std::array<const char*, count> names{
      "waits", "wakes", "empty_wakes", "notifications", "park_attempts", "completions",
      "eagain", "timeouts", "interrupts", "errors", "wait_ns",
      "lt2us", "2to20us", "20to200us", "ge200us",
      "parked_lt2us", "parked_2to20us", "parked_20to200us", "parked_ge200us",
      "sleep_zero", "sleep_lt100us", "sleep_lt1ms", "sleep_ge1ms", "sleep_abs_expired",
      "sleep_zero_ns", "sleep_lt100us_ns", "sleep_lt1ms_ns", "sleep_ge1ms_ns", "sleep_abs_expired_ns",
      "internal_sleeps", "internal_sleep_ns", "yields", "yield_ns",
      "sleep_lock_calls", "sleep_lock_contended", "sleep_lock_ns", "sleep_lock_contended_ns",
      "futex0_lock_calls", "futex0_lock_contended", "futex0_lock_ns", "futex0_lock_contended_ns",
      "wake_cpu_units", "wake_cpu_samples"};
  std::array<std::atomic<std::uint64_t>, count> values{};
  void add(Field field, std::uint64_t value = 1) { values[field].fetch_add(value, std::memory_order_relaxed); }
  static unsigned duration_bin(std::uint64_t ns) { return ns < 2'000 ? 0 : ns < 20'000 ? 1 : ns < 200'000 ? 2 : 3; }
  static int sleep_bin(std::int64_t requested_ns) {
    return requested_ns == -4 ? 4 : requested_ns < 0 ? -1 : requested_ns == 0 ? 0 : requested_ns < 100'000 ? 1 : requested_ns < 1'000'000 ? 2 : 3;
  }
  void schedule(int bin, std::uint64_t elapsed_ns) {
    if (bin == -3) { add(yields); add(yield_ns, elapsed_ns); }
    else if (bin == -1) { add(internal_sleeps); add(internal_sleep_ns, elapsed_ns); }
    else if (bin >= 0 && bin <= 4) {
      add(static_cast<Field>(sleep_zero + bin));
      add(static_cast<Field>(sleep_zero_ns + bin), elapsed_ns);
    }
  }
  void bucket_lock(int kind, bool contended, std::uint64_t ns) {
    if (!kind) return;
    const auto first = kind == 1 ? sleep_lock_calls : futex0_lock_calls;
    add(first); if (contended) add(static_cast<Field>(first + 1));
    add(static_cast<Field>(first + 2), ns);
    if (contended) add(static_cast<Field>(first + 3), ns);
  }
  void complete(std::uint64_t ns, unsigned park_count, int result) {
    add(completions); add(wait_ns, ns); add(parks, park_count);
    add(static_cast<Field>(lt2us + duration_bin(ns)));
    if (park_count) add(static_cast<Field>(parked_lt2us + duration_bin(ns)));
    if (result == -err::kEAGAIN) add(eagain);
    else if (result == -err::kETIMEDOUT) add(timeouts);
    else if (result == -err::kEINTR) add(interrupts);
    else if (result) add(errors);
  }
  Snapshot snapshot() const {
    Snapshot result{};
    for (unsigned i = 0; i < count; ++i) result[i] = values[i].load(std::memory_order_relaxed);
    return result;
  }
};
} // namespace venture::profile
