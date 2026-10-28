// Blocking primitives for guest threads.
//
// Every HLE operation that blocks (pthread mutex/cond/rwlock/once/join,
// semaphores, sleep, poll/epoll, blocking reads, Java monitors, ...) must block
// through these functions so that signal delivery, GC suspension and process
// exit can interrupt any blocked guest thread.
//
// Synchronization objects are implemented the way bionic implements them: as
// futex words inside the guest's own object memory (pthread_mutex_t etc.), so
// static initializers (all-zero PTHREAD_MUTEX_INITIALIZER) work and no side
// tables are needed.
#pragma once

#include <cstdint>
#include <vector>

#include "core/guest.h"
#include "core/sync_census.h"

namespace venture {
class GuestThread;

// Monotonic clock in nanoseconds (CLOCK_MONOTONIC for the guest).
std::int64_t monotonic_ns();

// futex(FUTEX_WAIT_BITSET): if *address == expected, sleep until woken,
// interrupted or the absolute monotonic deadline passes (deadline < 0: none).
// Returns 0, -EAGAIN (value differed), -ETIMEDOUT or -EINTR (a signal handler
// ran; callers implementing SA_RESTART semantics may retry).
// A condition wait passes deliver_signals=false while its mutex is released;
// pending handlers run at the guest boundary after the mutex is reacquired.
int futex_wait(GuestThread& thread, GuestAddr address, std::uint32_t expected,
               std::int64_t deadline_ns, std::uint32_t bitset = 0xffffffffu,
               bool deliver_signals = true);
// futex(FUTEX_WAKE_BITSET): wakes up to `count` waiters; returns the number woken.
int futex_wake(GuestAddr address, int count, std::uint32_t bitset = 0xffffffffu,
               sync_census::WakeSource source = sync_census::WakeSource::other);
inline int futex_wake(GuestAddr address, int count, sync_census::WakeSource source) {
  return futex_wake(address, count, 0xffffffffu, source);
}

// Sleeps until the deadline or an interrupting signal. Returns 0 or -EINTR.
int sleep_until(GuestThread& thread, std::int64_t deadline_ns, std::int64_t requested_ns = -1);
struct FutexWaitSnapshot {
  std::int32_t tid;
  GuestAddr address;
  std::uint32_t expected, value, bitset;
  bool readable, woken;
  std::int64_t deadline;
};
std::vector<FutexWaitSnapshot> futex_wait_snapshot();

}  // namespace venture
