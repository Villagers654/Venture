#pragma once
#include "core/guest.h"
#include <array>
#include <atomic>
#include <span>
#include <string_view>
#include <cstddef>
#include <cstdint>

namespace venture::shutdown_trace {
// One explicitly selected mutex only. Prehistory permits selection at a
// worker's first blocking lock call without losing the preceding owner call.
struct MutexWatchEvent {
  std::int64_t ns{};
  GuestAddr address{};
  std::int32_t tid{}, result{};
  const char* operation{};
  std::array<std::uint32_t, 3> words{};
  std::array<GuestAddr, 5> callers{};
  bool readable{};
};
// Same immutable-prefix publication as profile::FutexHandoffs: one guest
// owner writes, snapshots read only the released prefix. No wrap, wait or I/O.
template<class Event, std::size_t Capacity> struct WatchRecords {
  std::array<Event, Capacity> rows{};
  std::atomic<std::size_t> published{0};
  std::atomic<std::uint64_t> dropped{0};
  std::size_t emitted = 0; // snapshot consumer only
  bool append(const Event& event) noexcept {
    const auto index = published.load(std::memory_order_relaxed);
    if (index == Capacity) { dropped.fetch_add(1, std::memory_order_relaxed); return false; }
    rows[index] = event;
    published.store(index + 1, std::memory_order_release);
    return true;
  }
  std::span<const Event> snapshot() const noexcept {
    return {rows.data(), published.load(std::memory_order_acquire)};
  }
};
template<std::size_t Capacity> using MutexWatchRecords = WatchRecords<MutexWatchEvent, Capacity>;
struct WordTransitionEvent {
  std::int64_t ns{};
  GuestAddr previous{}, value{}, pc{}, lr{};
  std::int32_t tid{};
  std::uint32_t hle{};
  std::array<GuestAddr, 8> callers{};
};
extern const bool word_watch_enabled;
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
struct DiagnosticBudget {
  static constexpr std::uint64_t limit = 250000;
  std::atomic<std::uint64_t> used{0};
  std::atomic<bool> reported{false};
  bool take() noexcept { return used.fetch_add(1, std::memory_order_relaxed) < limit; }
  bool claim_exhaustion() noexcept {
    return used.load(std::memory_order_relaxed) > limit && !reported.exchange(true);
  }
};
extern const bool mutex_watch_enabled;
void mutex_watch_image(std::string_view module, GuestAddr bias, GuestAddr begin, std::uint64_t size) noexcept;
void trace_mutex_event(void*, const char* operation, std::int32_t result) noexcept;
void snapshot_mutex_watch(const char* label) noexcept;
} // namespace venture::shutdown_trace
