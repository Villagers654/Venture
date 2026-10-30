#include "platform/thread_priority.h"
#include "platform/profile.h"
#include "core/error.h"
#include "common/log.h"
#include "common/platform/posix/dbus.h"
#include <cerrno>
#include <atomic>
#include <memory>
#include <algorithm>
#include <pthread.h>
#include <sched.h>
#include <map>
#include <mutex>
#include <sys/resource.h>
#include <unistd.h>
#include <time.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
namespace venture::platform {
void lower_background_thread_priority() {
#ifdef __linux__
  // Linux nice is per thread; avoid changing the entire process on other hosts.
  setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
}
// The per-thread scheduler clock already counts nanoseconds, so frame CPU
// samples are exact times rather than calibrated cycle estimates.
std::int64_t thread_cpu_cycles(const ProfileThread& thread) {
  std::uint64_t ns = 0;  // id 0 would name the calling thread's clock
  return thread.id && profile_cpu_time(thread, ns) ? static_cast<std::int64_t>(ns) : -1;
}
double thread_cycles_per_ns() { return 1.0; }
const char* thread_cycles_method() { return "thread_cpu_clock_ns"; }
std::int64_t thread_cpu_cycles() { return thread_cpu_time_ns(); }
std::int64_t thread_cpu_time_ns() {
#ifdef CLOCK_THREAD_CPUTIME_ID
  timespec value{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value)) return -1;
  return std::int64_t(value.tv_sec) * 1'000'000'000 + value.tv_nsec;
#else
  return -1;
#endif
}
namespace {
struct Entry { int id = 0; std::atomic<bool> audio{false}; std::atomic<int> nice{0}; int fifo = 0; Entry* previous = nullptr; std::unique_ptr<AudioThreadPriority> priority; };
struct Threads { std::mutex mutex; std::map<std::int32_t, std::unique_ptr<Entry>> ids; };
thread_local Entry* current_entry = nullptr;
thread_local unsigned realtime_depth = 0;
Threads& threads() { static auto* value = new Threads; return *value; }
}
namespace {
thread_local unsigned elevated_depth = 0;
#ifdef __linux__
// A thread keeps its raised nice until it exits or the game lowers it: each
// raise is a RealtimeKit request, and RealtimeKit refuses a user's requests
// past its rate and thread limits, so raising again on every stream start
// left later audio at nice 0. -21 means none is held.
thread_local int held_nice = -21;
// The lowest nice the process may set without privilege (RLIMIT_NICE).
int permitted_nice() {
  rlimit limit{};
  if (getrlimit(RLIMIT_NICE, &limit)) return 20;
  if (limit.rlim_cur == RLIM_INFINITY) return -20;
  return 20 - static_cast<int>(std::min<rlim_t>(limit.rlim_cur, 40));
}
constexpr const char* rtkit = "org.freedesktop.RealtimeKit1";
constexpr const char* rtkit_path = "/org/freedesktop/RealtimeKit1";
// Raises the calling thread toward `nice`: directly where RLIMIT_NICE or
// CAP_SYS_NICE allows, else through RealtimeKit (the desktop service PipeWire
// clients use; it grants down to its MinNiceLevel), else through the Realtime
// portal (RealtimeKit for sandboxed processes, which cannot reach it), else to
// the nearest permitted value. Returns the nice applied and how.
std::pair<int, const char*> raise_nice(id_t tid, int nice, int current) {
  if (permitted_nice() <= nice && !setpriority(PRIO_PROCESS, tid, nice)) return {nice, "setpriority"};
  static const int rtkit_floor =
      dbus::i32_property(dbus::Bus::system, rtkit, rtkit_path, rtkit, "MinNiceLevel").value_or(20);
  if (const int granted = std::max(nice, rtkit_floor); granted < current &&
      dbus::call_ti(dbus::Bus::system, rtkit, rtkit_path, rtkit, "MakeThreadHighPriority", tid, granted))
    return {granted, "RealtimeKit"};
  static const int portal_floor = dbus::portal_min_nice().value_or(20);
  if (const int granted = std::max(nice, portal_floor); granted < current &&
      dbus::portal_thread_high_priority(std::uint64_t(getpid()), tid, granted))
    return {granted, "the Realtime portal"};
  if (const int floor = permitted_nice(); floor < current && !setpriority(PRIO_PROCESS, tid, floor))
    return {floor, "RLIMIT_NICE"};
  return {current, nullptr};
}
#endif
}  // namespace
// SCHED_FIFO where RLIMIT_RTPRIO or privilege allows, else a raised nice.
// RealtimeKit's real-time grant is not requested: it needs RLIMIT_RTTIME,
// which kills a thread that computes past the limit without blocking, and a
// guest callback compiling JIT code can.
AudioThreadPriority::AudioThreadPriority() {
  if (current_audio_priority_class() != AudioPriorityClass::normal) return;
  sched_param previous{};
  if (pthread_getschedparam(pthread_self(), &previous_policy_, &previous)) return;
  previous_priority_ = previous.sched_priority;
  sched_param requested{};
  requested.sched_priority = std::clamp(2, sched_get_priority_min(SCHED_FIFO), sched_get_priority_max(SCHED_FIFO));
#ifdef RLIMIT_RTPRIO
  rlimit limit{};
  if (!getrlimit(RLIMIT_RTPRIO, &limit) && limit.rlim_cur > 0)
    requested.sched_priority = std::min(requested.sched_priority, static_cast<int>(std::min<rlim_t>(limit.rlim_cur, 99)));
#endif
  const int fifo_error = pthread_setschedparam(pthread_self(), SCHED_FIFO, &requested);
  changed_ = fifo_error == 0;
  static std::atomic<bool> reported{false};
  if (changed_) {
    ++realtime_depth;
    if (!reported.exchange(true)) log::info("audio", "Audio threads run SCHED_FIFO {}", requested.sched_priority);
    return;
  }
#ifdef __linux__
  const auto tid = static_cast<id_t>(syscall(SYS_gettid));
  errno = 0;
  previous_nice_ = getpriority(PRIO_PROCESS, tid);
  if (errno) return;
  if (previous_nice_ <= held_nice) {
    niced_ = true;
    ++elevated_depth;
    return;
  }
  // Android's THREAD_PRIORITY_AUDIO.
  const auto [nice, method] = raise_nice(tid, -16, previous_nice_);
  niced_ = method != nullptr;
  if (niced_) {
    ++elevated_depth;
    held_nice = nice;
  }
  static std::atomic<bool> raised{false}, refused{false};
  if (niced_ ? raised.exchange(true) : refused.exchange(true)) return;
  if (niced_)
    log::info("audio", "Audio threads: SCHED_FIFO refused (error {}); nice {} through {}", fifo_error, nice, method);
  else
    log::warn("audio", "Audio threads: SCHED_FIFO refused (error {}) and no raised priority is permitted{}; "
              "they run at nice {}", fifo_error, raised.load() ? " for a later thread" : "", previous_nice_);
#endif
}
AudioThreadPriority::~AudioThreadPriority() {
#ifdef __linux__
  if (niced_) --elevated_depth;
#endif
  if (!changed_) return;
  sched_param previous{};
  previous.sched_priority = previous_priority_;
  pthread_setschedparam(pthread_self(), previous_policy_, &previous);
  --realtime_depth;
}
AudioPriorityClass current_audio_priority_class() {
  return realtime_depth ? AudioPriorityClass::realtime
         : elevated_depth ? AudioPriorityClass::elevated
                          : AudioPriorityClass::normal;
}
void apply_pending_thread_priority() {
  if (!current_entry) return;
  if (current_entry->audio.load(std::memory_order_acquire)) {
    if (!current_entry->priority) current_entry->priority = std::make_unique<AudioThreadPriority>();
  } else if (current_entry->priority) {
    current_entry->priority.reset();
#ifdef __linux__
    setpriority(PRIO_PROCESS, static_cast<id_t>(current_entry->id), current_entry->nice.load(std::memory_order_relaxed));
#endif
  }
}
int set_audio_thread_priority(std::int32_t tid, int fifo_priority) {
  if (fifo_priority != 0 && fifo_priority != 2 && fifo_priority != 3) return -err::kEINVAL;
  auto& state = threads();
  std::lock_guard lock(state.mutex);
  auto it = state.ids.find(tid);
  if (it == state.ids.end()) return -err::kESRCH;
  it->second->fifo = fifo_priority;
  auto nice = it->second->nice.load(std::memory_order_relaxed);
  it->second->audio = fifo_priority || nice == -16 || nice == -19;
  if (it->second.get() == current_entry) apply_pending_thread_priority();
  return 0;
}

int get_audio_thread_scheduler(std::int32_t tid, int* policy, int* priority) {
  auto& state = threads();
  std::lock_guard lock(state.mutex);
  auto it = state.ids.find(tid);
  if (it == state.ids.end()) return -err::kESRCH;
  *policy = it->second->fifo ? 1 : 0;
  *priority = it->second->fifo;
  return 0;
}
void bind_thread_priority(std::int32_t tid) {
  auto& state = threads();
  std::lock_guard lock(state.mutex);
  auto entry = std::make_unique<Entry>();
  entry->previous = current_entry;
  if (current_entry) current_entry->priority.reset();
#ifdef __linux__
  entry->id = static_cast<int>(syscall(SYS_gettid));
#endif
  current_entry = entry.get();
  state.ids[tid] = std::move(entry);
}

void unbind_thread_priority(std::int32_t tid) {
  auto& state = threads(); std::lock_guard lock(state.mutex); if (auto it = state.ids.find(tid); it != state.ids.end()) {
    current_entry->priority.reset(); current_entry = it->second->previous; state.ids.erase(it);
    apply_pending_thread_priority();
  }
}
int get_thread_priority(std::int32_t tid, int* nice) {
  auto& state = threads();
  std::lock_guard lock(state.mutex);
  auto it = state.ids.find(tid);
  if (it == state.ids.end()) return -err::kESRCH;
  *nice = it->second->nice.load(std::memory_order_relaxed);
  return 0;
}
int set_thread_priority(std::int32_t tid, int nice) {
  if (nice < -20 || nice > 19) return -err::kEINVAL;
  auto& state = threads(); std::lock_guard lock(state.mutex);
  auto it = state.ids.find(tid);
  if (it == state.ids.end()) return -err::kESRCH;
  if (nice == -16 || nice == -19 || it->second->fifo) {
    it->second->nice = nice;
    it->second->audio = true;
    if (it->second.get() == current_entry) apply_pending_thread_priority();
    return 0;
  }
#ifdef __linux__
  // Android lets apps raise their own threads (e.g. THREAD_PRIORITY_DISPLAY),
  // as Win32 ABOVE_NORMAL/HIGHEST need no privilege. Unprivileged Linux may only
  // lower nice to 20 - RLIMIT_NICE, so a raise beyond that applies the nearest
  // permitted value instead of failing the guest with SecurityException.
  const auto id = static_cast<id_t>(it->second->id);
  errno = 0;
  const int current = getpriority(PRIO_PROCESS, id);
  if (errno == 0) {
    int applied = nice;
    if (rlimit limit{}; !getrlimit(RLIMIT_NICE, &limit) && limit.rlim_cur != RLIM_INFINITY) {
      const int permitted = 20 - static_cast<int>(std::min<rlim_t>(limit.rlim_cur, 40));
      // Restoring zero from a lowered thread is also a priority raise. Keep
      // an already permitted priority, while still honoring requests to lower it.
      applied = std::max(nice, std::min(current, permitted));
    }
    if (applied == current || !setpriority(PRIO_PROCESS, id, applied)) {
      static std::atomic_flag clamp_reported = ATOMIC_FLAG_INIT;
      if (applied != nice && !clamp_reported.test_and_set(std::memory_order_relaxed))
        log::warn("scheduling", "Host priority permissions limit requested nice {} to {}; guest priority retained", nice, applied);
      it->second->nice = nice;
      it->second->audio = false;
      if (it->second.get() == current_entry) apply_pending_thread_priority();
      return 0;
    }
  }
  switch (errno) {
    case EACCES: case EPERM: return -err::kEPERM;
    case ESRCH: return -err::kESRCH;
    default: return -err::kEINVAL;
  }
#else
  return -err::kEOPNOTSUPP;
#endif
}
}
