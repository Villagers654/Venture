#include "core/wait.h"
#include "core/trace_threads.h"
#include "core/diagnostic_trace.h"
#include "common/environment.h"
#include "core/thread_runtime.h"
#include "core/signals.h"
#include "core/error.h"
#include "core/profile.h"
#include "platform/memory.h"
#include "platform/timed_wait.h"
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <format>
#include <mutex>
#include <utility>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#if defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#define VENTURE_SPIN_WAITPKG 1
#endif
#endif

namespace venture {
namespace {
const bool trace_futex = environment::getenv("VENTURE_TRACE_FUTEX") != nullptr;
// Futex waiters may poll their wake flag up to VENTURE_FUTEX_SPIN_NS before
// parking. A wake that lands meanwhile needs no native alert (the condition
// variable has no sleeper) and the waiter resumes without a scheduler round
// trip. Off by default: at 50 us a PAUSE spin gained 1.5-2% fps on Batman and
// cost 2% on North Star; the UMWAIT spin gained 0.3% and cost 0.6%.
const std::int64_t spin_ns = [] {
  const char* text = environment::getenv("VENTURE_FUTEX_SPIN_NS");
  return text ? std::max<std::int64_t>(0, std::atoll(text)) : 0;
}();
// A thread spins only while its waits usually end within the window: every
// futex wait woken within it earns two credits and every other one (woken
// later, timed out or interrupted) costs one, whether or not it spun. Long
// waiters (frame pacing, background jobs) stop burning their spin.
thread_local int spin_credit = 16;
inline void spin_pause() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
  _mm_pause();
#elif defined(__aarch64__)
  asm volatile("yield");
#endif
}
#ifdef VENTURE_SPIN_WAITPKG
// With WAITPKG the spinning waiter idles in C0.1 on its flag's cache line
// (UMONITOR/UMWAIT) until the waker writes it, instead of issuing PAUSE.
const bool spin_umwait = [] {
  unsigned a = 0, b = 0, c = 0, d = 0;
  return __get_cpuid_count(7, 0, &a, &b, &c, &d) && (c & (1u << 5));
}();
__attribute__((target("waitpkg"))) void umwait_for(const std::atomic<bool>& flag) noexcept {
  _umonitor(const_cast<std::atomic<bool>*>(&flag));
  if (!flag.load(std::memory_order_acquire)) _umwait(1, __rdtsc() + 20'000);
}
#endif
void spin_on(const std::atomic<bool>& flag) noexcept {
#ifdef VENTURE_SPIN_WAITPKG
  if (spin_umwait) return umwait_for(flag);
#endif
  for (int i = 0; i < 32 && !flag.load(std::memory_order_acquire); ++i) spin_pause();
}
template<class... Args>
void record_futex(std::format_string<Args...> format, Args&&... args) noexcept {
  try {
    std::array<char, 512> text;
    const auto formatted = std::format_to_n(text.data(), text.size(), format, std::forward<Args>(args)...);
    diagnostic_trace("futex-trace", std::string_view(text.data(), std::min(std::size_t(formatted.size), text.size())));
  } catch (...) {}
}
struct Waiter {
  GuestThread* thread;
  GuestAddr address;
  std::uint32_t bitset;
  std::atomic<bool> woken{false}; // Written under the bucket lock, polled without it while spinning.
  bool parked = false;              // Under the bucket lock: a spinning waiter needs no notification.
  std::condition_variable cv;
  platform::TimedWait* timed = nullptr;
  Waiter* previous = nullptr;
  Waiter* next = nullptr;
  std::uint32_t expected = 0;
  std::int64_t deadline = -1;
  profile::FutexEvent* event = nullptr;
  void notify() {
    if (timed) timed->notify(); else cv.notify_one();
  }
};
struct Bucket {
  std::mutex mutex;
  Waiter* first = nullptr;
  Waiter* last = nullptr;
  void append(Waiter& waiter) {
    waiter.previous = last;
    if (last) last->next = &waiter;
    else first = &waiter;
    last = &waiter;
  }
  void remove(Waiter& waiter) {
    if (waiter.previous) waiter.previous->next = waiter.next;
    else first = waiter.next;
    if (waiter.next) waiter.next->previous = waiter.previous;
    else last = waiter.previous;
  }
};
std::array<Bucket, 256> buckets;
Bucket& bucket(GuestAddr p) {
  // Fold every address byte into the key. Using only the low word bits puts
  // equal-offset futexes on every guest page into one locked waiter list.
  // Addresses and bitsets are still checked in full when selecting wakes.
  auto key = p >> 2;
  key ^= key >> 32;
  key ^= key >> 16;
  key ^= key >> 8;
  return buckets[key % buckets.size()];
}
int wait(GuestThread& thread, GuestAddr address, std::uint32_t expected, std::int64_t deadline,
         std::uint32_t bitset, bool sleeping, bool deliver_signals = true,
         profile::FutexEvent* event = nullptr) {
  if (!bitset || (!sleeping && (address & 3))) return -err::kEINVAL;
  auto& b = bucket(address);
  Waiter w{};
  w.thread = &thread; w.address = address; w.bitset = bitset;
  w.event = event && event->handoff ? event : nullptr;
  if (w.event) {
    auto& timing = w.event->timing;
    timing.address = address; timing.expected = expected;
    timing.bitset = bitset; timing.deadline_ns = deadline;
  }
  w.expected = expected; w.deadline = deadline;
  const auto epoch = thread.interrupt_epoch.load();
  std::unique_lock lock(b.mutex, std::defer_lock);
  if (event && event->counters && (sleeping || &b == &buckets[0])) {
    event->lock_kind = sleeping ? 1 : 2;
    const auto before = monotonic_ns();
    if (!lock.try_lock()) { event->lock_contended = true; lock.lock(); }
    event->lock_ns = monotonic_ns() - before;
  } else lock.lock();
  if (!sleeping) {
    std::uint32_t value{};
    if (!platform::read_memory(address, &value, 4)) return -err::kEFAULT;
    if (std::atomic_ref(*g2h<std::uint32_t>(address)).load(std::memory_order_acquire) != expected) return -err::kEAGAIN;
  }
  if (deadline >= 0) {
    // Reuse native timer/event resources across the owning thread's waits.
    // Initialize before enqueueing so an allocation failure cannot leave a
    // pointer to this stack waiter in the bucket.
    if (!thread.timed_wait) thread.timed_wait = std::make_unique<platform::TimedWait>();
    w.timed = thread.timed_wait.get();
  }
  b.append(w);
  if (w.event) w.event->timing.enqueue_ns = monotonic_ns();
  shutdown_trace::FutexScope wait_trace(thread, address, expected, sleeping);
  auto ready = [&] { return w.woken.load() || thread.interrupted.load() || thread.interrupt_epoch.load() != epoch; };
  const auto wait_begin = spin_ns > 0 && !sleeping ? monotonic_ns() : 0;
  if (wait_begin && spin_credit > 0 && (deadline < 0 || deadline > wait_begin)) {
    // The bucket keeps the waiter registered, so a wake still selects it; the
    // lock is retaken before the waiter can leave, keeping it alive for the waker.
    lock.unlock();
    const auto until = deadline < 0 ? wait_begin + spin_ns : std::min(deadline, wait_begin + spin_ns);
    do spin_on(w.woken);
    while (!ready() && monotonic_ns() < until);
    lock.lock();
  }
  bool signalled = true;
  int wait_error = 0;
  if (deadline < 0) {
    while (!ready()) {
      if (event) {
        ++event->parks;
        if (event->handoff && !event->timing.park_ns) event->timing.park_ns = monotonic_ns();
      }
      w.parked = true;
      w.cv.wait(lock);
    }
  }
  else {
    while (!ready() && !wait_error) {
      if (event) {
        ++event->parks;
        if (event->handoff && !event->timing.park_ns) event->timing.park_ns = monotonic_ns();
      }
      w.parked = true;
      wait_error = w.timed->wait_until(lock, deadline, !sleeping);
    }
    signalled = ready();
  }
  if (w.event) w.event->timing.resumed_ns = monotonic_ns();
  b.remove(w);
  lock.unlock();
  if (wait_begin)
    spin_credit = w.woken.load() && monotonic_ns() - wait_begin <= spin_ns ? std::min(spin_credit + 2, 16)
                                                                            : std::max(spin_credit - 1, 0);
  if (w.woken.load()) return 0;
  if (wait_error && wait_error != -err::kETIMEDOUT) return wait_error;
  if (!signalled) return sleeping ? 0 : -err::kETIMEDOUT;
  thread.interrupted.store(false);
  check_guest_exit(thread);
  if (deliver_signals) signals::deliver_pending(thread);
  return -err::kEINTR;
}
}
std::int64_t monotonic_ns() { return platform::steady_ns(); }
int futex_wait(GuestThread& thread, GuestAddr address, std::uint32_t expected, std::int64_t deadline, std::uint32_t bitset, bool deliver_signals) {
  const bool recording = trace_futex && trace_guest_tid(thread.tid());
  const auto entered = recording ? monotonic_ns() : 0;
  auto perform_wait = [&] {
    if (!profile::futex_timing_enabled) return wait(thread, address, expected, deadline, bitset, false, deliver_signals);
    auto event = profile::futex_begin(thread);
    try {
      const auto result = wait(thread, address, expected, deadline, bitset, false, deliver_signals, &event);
      profile::futex_end(event, result);
      return result;
    } catch (...) {
      profile::futex_end(event, -err::kEINTR);
      throw;
    }
  };
  const auto result = perform_wait();
  if (recording) record_futex("ns={} tid={} wait-enter object={:#x} expected={} deadline={} bitset={:#x}", entered, thread.tid(), address, expected, deadline, bitset);
  if (recording) record_futex("ns={} tid={} wait-exit object={:#x} result={}", monotonic_ns(), thread.tid(), address, result);
  return result;
}
int futex_wake(GuestAddr address, int count, std::uint32_t bitset, sync_census::WakeSource source) {
  const auto* thread = trace_futex ? GuestThread::current() : nullptr;
  const bool recording = trace_futex && trace_guest_tid(thread ? thread->tid() : 0);
  const auto entered = recording ? monotonic_ns() : 0;
  if ((address & 3) || count < 0 || !bitset) return -err::kEINVAL;
  const auto cpu_begin = profile::futex_handoff_cpu_enabled ? profile::futex_wake_cpu_begin() : -1;
  auto& b = bucket(address);
  // Both diagnostic views share one request timestamp and completion hook.
  const auto requested = (sync_census::enabled || profile::futex_handoff_enabled) ? monotonic_ns() : 0;
  int woke = 0;
  {
    std::lock_guard lock(b.mutex);
    const auto cpu_locked = cpu_begin >= 0 ? profile::futex_cpu_clock() : -1;
    const auto locked = profile::futex_handoff_enabled ? monotonic_ns() : 0;
    for (auto* w = b.first; w; w = w->next) if (!w->woken.load(std::memory_order_relaxed) && w->address == address && (w->bitset & bitset) && woke < count) {
      w->woken.store(true, std::memory_order_release); ++woke;
      if (w->event) profile::futex_notifying(*w->event, requested, locked);
      const auto cpu_notify = cpu_begin >= 0 && w->event ? profile::futex_cpu_clock() : -1;
      if (w->parked) w->notify();
      if (w->event) {
        const auto cpu_end = cpu_notify >= 0 ? profile::futex_cpu_clock() : -1;
        auto& timing = w->event->timing;
        timing.notify_end_ns = monotonic_ns();
        if (cpu_end >= cpu_notify && cpu_notify >= cpu_locked && cpu_locked >= cpu_begin && cpu_begin >= 0) {
          timing.wake_cpu = cpu_end - cpu_begin;
          timing.bucket_cpu = cpu_locked - cpu_begin;
          timing.notify_cpu = cpu_end - cpu_notify;
        }
      }
    }
  }
  // No matching waiter means no notify/alert call. Publish both diagnostic
  // views once, outside the bucket so registration never holds its lock.
  if (sync_census::enabled || profile::futex_timing_enabled) {
    const auto cpu_end = cpu_begin >= 0 ? profile::futex_cpu_clock() : -1;
    profile::futex_wake(woke, source, requested,
                       cpu_begin >= 0 && cpu_end >= cpu_begin ? cpu_end - cpu_begin : -1);
  }
  // Guest notifications complete before optional text recording. The census
  // hook above runs once, outside the bucket; neither hook writes to disk.
  if (recording) record_futex("ns={} tid={} wake-enter object={:#x} count={} bitset={:#x}", entered, thread ? thread->tid() : 0, address, count, bitset);
  if (recording) record_futex("ns={} tid={} wake-exit object={:#x} result={}", monotonic_ns(), thread ? thread->tid() : 0, address, woke);
  return woke;
}
int sleep_until(GuestThread& thread, std::int64_t deadline, std::int64_t requested_ns) {
  if (!profile::futex_timing_enabled) return wait(thread, 0, 0, deadline, ~0u, true);
  auto event = profile::scheduling_begin(thread, requested_ns);
  try {
    const auto result = wait(thread, 0, 0, deadline, ~0u, true, true, &event);
    profile::scheduling_end(event);
    return result;
  } catch (...) {
    profile::scheduling_end(event);
    throw;
  }
}
std::vector<FutexWaitSnapshot> futex_wait_snapshot() {
  std::vector<FutexWaitSnapshot> result;
  for (auto& b : buckets) {
    std::lock_guard lock(b.mutex);
    for (auto* w = b.first; w; w = w->next) {
      std::uint32_t value = 0;
      const bool readable = w->address && platform::read_memory(w->address, &value, sizeof(value));
      result.push_back({w->thread->tid(), w->address, w->expected, value, w->bitset, readable, w->woken.load(), w->deadline});
    }
  }
  return result;
}
void interrupt_waits(GuestThread& thread) {
  const auto begin = sync_census::enabled ? monotonic_ns() : 0;
  std::uint64_t scanned = 0;
  for (auto& b : buckets) {
    std::lock_guard lock(b.mutex);
    for (auto* w = b.first; w; w = w->next) {
      if (sync_census::enabled) ++scanned;
      if (w->thread == &thread) w->notify();
    }
  }
  if (sync_census::enabled) {
    sync_census::add(sync_census::interrupts);
    sync_census::add(sync_census::interrupt_buckets, buckets.size());
    sync_census::add(sync_census::interrupt_waiters, scanned);
    sync_census::add(sync_census::interrupt_ns, monotonic_ns() - begin);
  }
}
}
