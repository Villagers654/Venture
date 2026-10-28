#pragma once
#include <cstdint>
#include <string_view>
#include "core/futex_metrics.h"

namespace Dynarmic { struct CompileTiming; }
namespace venture { class GuestThread; }
namespace venture::sync_census { struct IoSample; enum class WakeSource; }
namespace venture::profile {
// Immutable launch option. The normal run loop has no profiling instructions;
// attach selects a separate instantiation only when this option is enabled.
extern const bool enabled;
extern const bool compile_timing_enabled;
extern const bool futex_timing_enabled;
extern const bool futex_handoff_enabled;
extern const bool futex_handoff_cpu_enabled;
std::int64_t futex_cpu_clock();
std::int64_t futex_wake_cpu_begin();
// Opt-in exact event counts, separate from statistical final-PC attribution.
struct FutexEvent {
  void* counters = nullptr;
  std::int64_t begin_ns = 0;
  unsigned parks = 0;
  bool handoff = false;
  FutexHandoff timing;
  int sleep_bin = -2, lock_kind = 0;
  bool lock_contended = false;
  std::uint64_t lock_ns = 0;
  std::int64_t cpu_begin = -1;
};
FutexEvent futex_begin(GuestThread&);
void futex_end(const FutexEvent&, int result);
void futex_notifying(FutexEvent&, std::int64_t requested_ns, std::int64_t locked_ns);
// One completed wake feeds both diagnostic views; request_ns is needed only
// for primitive timing and shares the optional handoff request timestamp.
void futex_wake(unsigned notifications, sync_census::WakeSource, std::int64_t request_ns,
                std::int64_t cpu_units = -1);
// requested_ns -1 identifies internal sleeps, -4 an expired absolute sleep.
FutexEvent scheduling_begin(GuestThread&, std::int64_t requested_ns, bool yield = false);
void scheduling_end(const FutexEvent&);
void register_compile_timing(const Dynarmic::CompileTiming&) noexcept;
inline constexpr std::uint32_t host = 0, jit = UINT32_MAX;
inline constexpr std::uint32_t pipeline_create = UINT32_MAX - 1;
void start();
// VENTURE_PROFILE_EVENTS_ONLY=1 retains frame/futex/compile events without
// creating a sampling thread. Opt-in counters still have their own overhead.
void stop() noexcept;
// Opt-in quit diagnostic: reuse retained profiler handles after stop(), taking
// up to ten PC/CPU samples 100 ms apart before the failed worker is ended.
void shutdown_burst() noexcept;
void attach(GuestThread&);
void detach(GuestThread&);
void set_thread_name(std::int32_t tid, std::string_view name);
void mark(std::string_view);
// Successful projection submissions, in the same clock domain as samples.
void frame(std::uint64_t index);
void record_io(const sync_census::IoSample&) noexcept;
void register_translation(const void* begin, const void* end, std::string_view name);
// Owning-thread lifetime emission count, including after sampling stops.
// -1 when profiling/guest attachment is unavailable; cache clears do not reset it.
std::int64_t thread_emitted_blocks() noexcept;
std::uint32_t exchange_activity(std::uint32_t) noexcept;
class Activity {
  std::uint32_t previous_;
 public:
  explicit Activity(std::uint32_t value) : previous_(exchange_activity(value)) {}
  ~Activity() { exchange_activity(previous_); }
};
} // namespace venture::profile
