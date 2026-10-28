#include "core/invalidation_stats.h"
#include "core/invalidation_report.h"
#include "common/environment.h"
#include "common/log.h"
#include "platform/profile.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <format>
#include <string>
#include <vector>

namespace venture::profile {
const bool invalidation_tracing = [] {
  const auto* value = environment::getenv("VENTURE_TRACE_INVALIDATION");
  return value && std::string_view(value) == "1";
}();
namespace {
constexpr std::size_t kCauses = static_cast<std::size_t>(InvalidationCause::count);
// Events at or above this wall time are also kept individually (bounded).
constexpr std::uint64_t kEventFloorNs = 100'000;
constexpr std::size_t kEventLimit = 100'000;
struct Totals {
  std::uint64_t events = 0, bytes = 0, cpus = 0, running = 0, wall_ns = 0, max_ns = 0, ge_1ms = 0;
  void add(std::uint64_t wall) { ++events; wall_ns += wall; max_ns = std::max(max_ns, wall); ge_1ms += wall >= 1'000'000; }
  Json json() const {
    return {{"events", events}, {"total_ms", wall_ns / 1e6}, {"max_ms", max_ns / 1e6}, {"ge_1ms", ge_1ms}};
  }
};
struct Event {
  std::int64_t start_ns; std::uint64_t wall_ns, bytes; std::uint32_t host_tid; std::uint16_t running; std::uint8_t kind;
  const char* file = nullptr; std::uint32_t line = 0;  // address-space lock events: the locking site
};
struct State {
  std::mutex mutex;
  std::array<Totals, kCauses> causes{};
  Totals apply, lock_waits, lock_holds;
  std::uint64_t applied_ranges = 0, dropped = 0;
  std::vector<Event> events;
};
State& state() { static auto* s = new State; return *s; }
// Event kinds after the causes.
constexpr std::uint8_t kApply = kCauses, kLockWait = kCauses + 1, kLockHold = kCauses + 2;
const char* name(std::uint8_t kind) {
  static constexpr const char* names[] = {"other", "map", "unmap", "protect", "advise", "cache_flush",
                                          "ic_operation", "code_patch", "apply", "address_space_lock",
                                          "address_space_lock_hold"};
  static_assert(std::size(names) == kCauses + 3);
  return names[kind];
}
// file:line of a lock site; source_location paths use '\' on Windows.
std::string site_name(const char* file, std::uint32_t line) {
  const std::string_view path(file);
  return std::format("{}:{}", path.substr(path.find_last_of("/\\") + 1), line);
}
void registered() {
  static const bool done = std::atexit(report_invalidation_at_exit) == 0;
  (void)done;
}
void keep(State& s, std::int64_t start_ns, std::uint64_t wall_ns, std::uint64_t bytes, unsigned running, std::uint8_t kind,
          const std::source_location* site = nullptr) {
  if (wall_ns < kEventFloorNs) return;
  if (s.events.size() >= kEventLimit) { ++s.dropped; return; }
  s.events.push_back({start_ns, wall_ns, bytes, platform::profile_thread_id(),
                      static_cast<std::uint16_t>(std::min(running, 0xffffu)), kind,
                      site ? site->file_name() : nullptr, site ? site->line() : 0});
}
}  // namespace
void record_invalidation(InvalidationCause cause, std::uint64_t bytes, unsigned cpus, unsigned running,
                         std::int64_t start_ns, std::uint64_t wall_ns) noexcept {
  try {
    registered();
    auto& s = state(); std::lock_guard lock(s.mutex);
    const auto kind = static_cast<std::uint8_t>(cause);
    auto& t = s.causes[kind];
    t.add(wall_ns); t.bytes += bytes; t.cpus += cpus; t.running += running;
    keep(s, start_ns, wall_ns, bytes, running, kind);
  } catch (...) { /* Diagnostics must never change invalidation. */ }
}
void record_invalidation_apply(unsigned ranges, std::uint64_t wall_ns) noexcept {
  try {
    registered();
    auto& s = state(); std::lock_guard lock(s.mutex);
    s.apply.add(wall_ns); s.applied_ranges += ranges;
    keep(s, monotonic_ns() - static_cast<std::int64_t>(wall_ns), wall_ns, 0, 0, kApply);
  } catch (...) {}
}
void record_address_space_wait(std::int64_t start_ns, std::uint64_t wall_ns, const std::source_location& site) noexcept {
  try {
    registered();
    auto& s = state(); std::lock_guard lock(s.mutex);
    s.lock_waits.add(wall_ns);
    keep(s, start_ns, wall_ns, 0, 0, kLockWait, &site);
  } catch (...) {}
}
void record_address_space_hold(std::int64_t start_ns, std::uint64_t wall_ns, const std::source_location& site) noexcept {
  try {
    registered();
    auto& s = state(); std::lock_guard lock(s.mutex);
    s.lock_holds.add(wall_ns);
    keep(s, start_ns, wall_ns, 0, 0, kLockHold, &site);
  } catch (...) {}
}
Json invalidation_report() {
  auto& s = state(); std::lock_guard lock(s.mutex);
  Json causes = Json::object();
  for (std::size_t i = 0; i < kCauses; ++i) {
    const auto& t = s.causes[i];
    if (!t.events) continue;
    auto row = t.json();
    row["bytes"] = t.bytes; row["cpus_queued"] = t.cpus; row["cpus_running"] = t.running;
    causes[name(static_cast<std::uint8_t>(i))] = std::move(row);
  }
  auto apply = s.apply.json(); apply["ranges"] = s.applied_ranges;
  Json events = Json::array();
  for (const auto& e : s.events) {
    Json row{{"start_ns", e.start_ns}, {"kind", name(e.kind)}, {"wall_ms", e.wall_ns / 1e6},
             {"bytes", e.bytes}, {"cpus_running", e.running}, {"host_tid", e.host_tid}};
    if (e.file) row["site"] = site_name(e.file, e.line);
    events.push_back(std::move(row));
  }
  return {{"causes", std::move(causes)}, {"owner_apply", std::move(apply)},
          {"address_space_lock_waits", s.lock_waits.json()}, {"address_space_lock_holds_ge_1ms", s.lock_holds.json()},
          {"events", std::move(events)},
          {"events_dropped", s.dropped},
          {"notes", "VENTURE_TRACE_INVALIDATION=1. Per cause: wall time on the invalidating thread from queueing "
                    "the range on every CPU through the end of each caught run (halt plus wait). cpus_running counts "
                    "CPUs caught inside Jit::Run. owner_apply: the owning thread handing queued ranges to its JIT "
                    "before its next run (the JIT drops the blocks as that run starts; recompiles show as compile time). address_space_lock_waits: contended acquisitions only (event site: where the waiter locks). address_space_lock_holds_ge_1ms: holds of 1 ms or more, start_ns at acquisition, site and host_tid of the holder. Whole process, not windowed; "
                    "events list only waits >= 0.1 ms, start_ns on the monotonic clock."}};
}
void report_invalidation_at_exit() noexcept {
  static std::atomic<bool> reported{false};
  if (!reported.exchange(true)) report_invalidation("exit");
}
void report_invalidation(std::string_view phase) noexcept {
  try {
    auto& s = state(); std::lock_guard lock(s.mutex);
    std::string causes;
    for (std::size_t i = 0; i < kCauses; ++i) {
      const auto& t = s.causes[i];
      if (!t.events) continue;
      if (!causes.empty()) causes += ";";
      causes += std::format("{} events={} total_ms={:.3f} max_ms={:.3f} ge1ms={} running={}", name(static_cast<std::uint8_t>(i)),
                            t.events, t.wall_ns / 1e6, t.max_ns / 1e6, t.ge_1ms, t.running);
    }
    // The longest address-space lock hold names its critical section.
    const Event* longest = nullptr;
    for (const auto& e : s.events)
      if (e.kind == kLockHold && (!longest || e.wall_ns > longest->wall_ns)) longest = &e;
    const auto longest_site = longest ? site_name(longest->file, longest->line) : std::string("none");
    log::info("profile", "Code invalidation {}: [{}] apply events={} ranges={} total_ms={:.3f} max_ms={:.3f}; address-space lock waits={} total_ms={:.3f} max_ms={:.3f} ge1ms={}; holds>=1ms={} total_ms={:.3f} max_ms={:.3f} at {}",
              phase, causes, s.apply.events, s.applied_ranges, s.apply.wall_ns / 1e6, s.apply.max_ns / 1e6,
              s.lock_waits.events, s.lock_waits.wall_ns / 1e6, s.lock_waits.max_ns / 1e6, s.lock_waits.ge_1ms,
              s.lock_holds.events, s.lock_holds.wall_ns / 1e6, s.lock_holds.max_ns / 1e6, longest_site);
  } catch (...) {}
}
}  // namespace venture::profile
