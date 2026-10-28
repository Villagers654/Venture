#include "core/sync_census.h"
#include "core/condition_census.h"
#include "core/profile.h"
#include "core/wait.h"
#include <map>
#include <mutex>

namespace venture::sync_census {
namespace {
struct Conditions {
  std::mutex mutex;
  std::map<std::int32_t, ConditionCounts> threads;
};
std::atomic<std::uint64_t> condition_failures{0};
Conditions& conditions() { static auto* value = new Conditions; return *value; }
}
void condition_notification(std::int32_t tid, bool broadcast, bool skipped) noexcept {
  if (!enabled) return;
  try {
    auto& state = conditions();
    std::lock_guard lock(state.mutex);
    auto& row = state.threads[tid];
    row.guest_tid = tid;
    if (broadcast) { ++row.broadcasts; row.skipped_broadcasts += skipped; }
    else { ++row.signals; row.skipped_signals += skipped; }
  } catch (...) {
    condition_failures.fetch_add(1, std::memory_order_relaxed);
  }
}
std::vector<ConditionCounts> condition_snapshot() {
  std::vector<ConditionCounts> result;
  if (!enabled) return result;
  auto& state = conditions();
  std::lock_guard lock(state.mutex);
  for (const auto& [tid, row] : state.threads) result.push_back(row);
  return result;
}
std::uint64_t condition_dropped() { return condition_failures.load(std::memory_order_relaxed); }
void IoScope::start(IoSource source, std::int32_t tid, std::int64_t deadline) {
  sample_ = {source, tid, monotonic_ns(), 0, -1, deadline, 0, 0, false, false};
}
void IoScope::finish() {
  if (!sample_.sleeps && sample_.source != IoSource::timer_dispatch) return;
  sample_.end_ns = monotonic_ns();
  profile::record_io(sample_);
}
}  // namespace venture::sync_census
