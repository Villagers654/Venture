#include "platform/timed_wait.h"
#include "core/error.h"
#include "common/environment.h"
#include "common/log.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <windows.h>

namespace venture::platform {
std::int64_t steady_ns() noexcept {
  // libc++'s steady_clock is QueryPerformanceCounter scaled by two 64-bit
  // divisions per call. A frequency that divides 10^9 (10 MHz on current
  // Windows) scales by one multiply to exactly the same value.
  static const std::int64_t scale = [] {
    LARGE_INTEGER frequency;
    return QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0 &&
                   1'000'000'000 % frequency.QuadPart == 0
               ? 1'000'000'000 / frequency.QuadPart : 0;
  }();
  if (!scale)
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  return counter.QuadPart * scale;
}
namespace {
std::int64_t now_ns() { return steady_ns(); }
// A high-resolution waitable timer still fires on the 0.5 ms timer tick: on the
// reference machine a 1-250 us wait returns after ~520 us and longer waits
// overshoot by the same amount, where Linux returns within its 50 us timer
// slack. Guest spin-backoff sleeps and short futex timeouts sit on game critical
// paths, so waits that short yield the core until the deadline instead. Longer
// waits keep the timer: yielding through their tail would keep 1 ms polling
// loops half busy (VENTURE_WAIT_TAIL_NS, 0 = timer only).
const std::int64_t tail_ns = [] {
  const auto* value = environment::getenv("VENTURE_WAIT_TAIL_NS");
  return value ? std::max<std::int64_t>(0, std::strtoll(value, nullptr, 10)) : 600'000;
}();
// Only sleeps need that precision: a notified futex wait wakes at once on the
// timer path too. UE4 polls render-thread events with 0.5-1 ms futex timeouts
// that mostly expire; yielding through them cost AW2's game thread ~0.7 ms of
// CPU per frame under SteamVR pacing, while AC Nexus kept its sleep-tail fps
// (VENTURE_FUTEX_TAIL_NS, default 0 = timer only).
const std::int64_t futex_tail_ns = [] {
  const auto* value = environment::getenv("VENTURE_FUTEX_TAIL_NS");
  return value ? std::max<std::int64_t>(0, std::strtoll(value, nullptr, 10)) : 0;
}();
int failure(const char* operation, DWORD error) {
  log::error("wait", "{} failed: Win32 error {}", operation, error);
  return error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY
      ? -err::kENOMEM : -err::kEIO;
}
}
struct TimedWait::State {
  std::condition_variable cv;
  bool using_cv = false; // Protected by the caller's predicate mutex.
  bool yielding = false;  // Protected by the caller's predicate mutex.
  std::atomic<bool> yield_notified{false};
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  HANDLE timer = nullptr;
  int error = 0;
  State() {
    if (!event) { error = failure("CreateEventW", GetLastError()); return; }
    // Documented since Windows 10 1803: high-resolution waitable timers avoid
    // the scheduler-tick rounding of millisecond condition-variable timeouts.
    // learn.microsoft.com/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw
    // libc++ support/win32/thread_win32.cpp instead truncates positive fractions
    // of a millisecond to zero, repeatedly spinning until the deadline.
    timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                  TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (!timer && GetLastError() == ERROR_INVALID_PARAMETER) {
      timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);
      if (timer) log::warn("wait", "High-resolution timers unavailable; using system timer resolution");
    }
    if (!timer) error = failure("CreateWaitableTimerExW", GetLastError());
  }
  ~State() {
    if (timer) CloseHandle(timer);
    if (event) CloseHandle(event);
  }
};
TimedWait::TimedWait() : state_(std::make_unique<State>()) {}
TimedWait::~TimedWait() = default;
int TimedWait::wait_until(std::unique_lock<std::mutex>& lock, std::int64_t deadline, bool notifiable) {
  if (state_->error) return state_->error;
  const auto now = now_ns();
  if (deadline <= now) return -err::kETIMEDOUT;
  const auto remaining = deadline - now;
  // Keep the low-latency condition-variable notification path for waits with
  // a whole millisecond to spare. Its non-predicate overload returns after one
  // native wait, so a rounded-down timeout comes back here for the precise tail
  // instead of looping through libc++'s zero-millisecond timeout path.
  state_->using_cv = remaining >= 2'000'000;
  if (state_->using_cv) {
    const auto end = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(deadline));
    return state_->cv.wait_until(lock, end) == std::cv_status::timeout ? -err::kETIMEDOUT : 0;
  }
  if (remaining <= (notifiable ? futex_tail_ns : tail_ns)) {
    state_->yielding = true;
    state_->yield_notified.store(false, std::memory_order_relaxed);
    lock.unlock();
    while (!state_->yield_notified.load(std::memory_order_acquire) && now_ns() < deadline)
      if (!SwitchToThread()) YieldProcessor();
    lock.lock();
    state_->yielding = false;
    return state_->yield_notified.exchange(false, std::memory_order_relaxed) ? 0 : -err::kETIMEDOUT;
  }
  LARGE_INTEGER due;
  // Relative negative 100 ns ticks, rounded up without overflowing nanoseconds.
  due.QuadPart = -(remaining / 100 + (remaining % 100 != 0));
  if (!SetWaitableTimer(state_->timer, &due, 0, nullptr, nullptr, FALSE))
    return failure("SetWaitableTimer", GetLastError());
  const HANDLE handles[]{state_->event, state_->timer};
  lock.unlock();
  const auto result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
  const auto error = result == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
  lock.lock();
  if (result != WAIT_OBJECT_0 + 1 && !CancelWaitableTimer(state_->timer))
    return failure("CancelWaitableTimer", GetLastError());
  if (result == WAIT_FAILED) return failure("WaitForMultipleObjects", error);
  if (result == WAIT_OBJECT_0) return 0;
  if (result == WAIT_OBJECT_0 + 1) return now_ns() >= deadline ? -err::kETIMEDOUT : 0;
  return failure("WaitForMultipleObjects unexpected result", result);
}
void TimedWait::notify() {
  if (state_->using_cv) { state_->cv.notify_one(); return; }
  if (state_->yielding) { state_->yield_notified.store(true, std::memory_order_release); return; }
  if (!state_->error && !SetEvent(state_->event)) failure("SetEvent", GetLastError());
}
}
