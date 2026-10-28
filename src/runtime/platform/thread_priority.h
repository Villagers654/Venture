#pragma once
#include "common/environment.h"
#include <cstdint>
namespace venture::platform {
// Measurement switch: VENTURE_THREAD_HINTS=0 keeps the earlier advisory-only
// hints and setpriority EPERM, for same-binary comparisons.
inline bool thread_hints_enabled() {
  static const bool enabled = [] { const auto* v = environment::getenv("VENTURE_THREAD_HINTS"); return !v || v[0] != '0'; }();
  return enabled;
}
struct ProfileThread;
// elevated: a raised ordinary priority (Linux nice) where real-time
// scheduling is not permitted.
enum class AudioPriorityClass { normal, mmcss, realtime, elevated };
// Scoped on the calling thread; restores its scheduling when audio work ends.
class AudioThreadPriority {
 public:
  AudioThreadPriority();
  ~AudioThreadPriority();
  AudioThreadPriority(const AudioThreadPriority&) = delete;
  AudioThreadPriority& operator=(const AudioThreadPriority&) = delete;
 private:
  [[maybe_unused]] void* task_ = nullptr;
  [[maybe_unused]] int previous_policy_ = 0;
  int previous_priority_ = 0;
  bool changed_ = false;
  [[maybe_unused]] int previous_nice_ = 0;
  [[maybe_unused]] bool niced_ = false;
};
AudioPriorityClass current_audio_priority_class();
// Cumulative CPU time for the calling native thread; -1 if unavailable.
// Platform resolution may be coarser than the nominal nanosecond unit.
std::int64_t thread_cpu_time_ns();
// Raw native CPU accounting cycles, not elapsed time; -1 if unavailable.
std::int64_t thread_cpu_cycles();
std::int64_t thread_cpu_cycles(const ProfileThread&);
// Startup TSC/QPC calibration; zero when the platform cannot provide it.
double thread_cycles_per_ns();
// What thread_cpu_cycles counts: TSC-estimated cycles (Windows) or exact
// thread CPU nanoseconds from the scheduler clock (Linux, rate 1).
const char* thread_cycles_method();
// MMCSS must be entered/reverted by the target thread, at its next guest entry.
void apply_pending_thread_priority();
// For a dedicated background worker only; its lowered priority lasts until exit.
void lower_background_thread_priority();
int set_audio_thread_priority(std::int32_t tid, int fifo_priority);
int get_audio_thread_scheduler(std::int32_t tid, int* policy, int* priority);
// Retain the native thread identity while its guest thread is attached.
void bind_thread_priority(std::int32_t tid);
void unbind_thread_priority(std::int32_t tid);
// Linux nice range [-20,19]; returns zero or a negative Linux errno.
int set_thread_priority(std::int32_t tid, int nice);
int get_thread_priority(std::int32_t tid, int* nice);
}
