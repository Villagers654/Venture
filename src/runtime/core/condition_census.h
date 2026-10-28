#pragma once
#include <cstdint>
#include <vector>

namespace venture::sync_census {
struct ConditionCounts {
  std::int32_t guest_tid = 0;
  std::uint64_t signals = 0, skipped_signals = 0, broadcasts = 0, skipped_broadcasts = 0;
};
// Gated A64 caller counts. A registered waiter may not yet be parked, but its
// notification must not be skipped even when futex_wake finds no host waiter.
void condition_notification(std::int32_t guest_tid, bool broadcast, bool skipped) noexcept;
std::vector<ConditionCounts> condition_snapshot();
std::uint64_t condition_dropped();
} // namespace venture::sync_census
