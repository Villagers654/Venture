#include <doctest/doctest.h>
#include "core/thread_runtime.h"
#include "core/wait.h"
#include "platform/timed_wait.h"
#include "core/error.h"
#include "core/futex_metrics.h"
#include "core/signals.h"
#include "core/hle_runtime.h"
#include "wait_test_support.h"
#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include "../async_wait.h"

using namespace venture;
TEST_CASE("core futex registered wake without word change preserves exact count and bitset") {
  alignas(4) std::uint32_t word = 7;
  GuestThread waiting(169);
  constexpr unsigned count = 200;
  std::atomic<unsigned> entered{0}, finished{0};
  std::atomic<int> failure{0};
  std::thread worker([&] {
    for (unsigned i = 1; i <= count; ++i) {
      entered.store(i, std::memory_order_release);
      const auto result = futex_wait(waiting, h2g(&word), 7, monotonic_ns() + 2 * venture::test::hang_timeout_ns, 2);
      if (result) { failure.store(result); break; }
      finished.store(i, std::memory_order_release);
    }
  });
  unsigned wakes = 0;
  for (unsigned i = 1; i <= count; ++i) {
    const auto end = monotonic_ns() + venture::test::hang_timeout_ns;
    while (entered.load(std::memory_order_acquire) < i && !failure.load() && monotonic_ns() < end) test::wait_pause();
    // Exercise near-immediate and delayed notification while the
    // guest word remains unchanged. A bitset mismatch must never satisfy it.
    const auto at = monotonic_ns() + (i % 3 == 0 ? 20'000 : i % 3 == 1 ? 1'000 : 5'000);
    while (monotonic_ns() < at) test::wait_pause();
    CHECK(futex_wake(h2g(&word), 0, 2) == 0);
    CHECK(futex_wake(h2g(&word), 1, 1) == 0);
    int woke = 0;
    while (!woke && !failure.load() && monotonic_ns() < end) { woke = futex_wake(h2g(&word), 1, 2); test::wait_pause(); }
    wakes += woke;
    if (!woke || failure.load()) { waiting.interrupt(); break; }
  }
  worker.join();
  INFO("entered=", entered.load(), " finished=", finished.load(), " wakes=", wakes,
       " wait failure=", failure.load());
  CHECK(failure.load() == 0);
  CHECK(finished.load() == count);
  CHECK(wakes == count);
  CHECK(word == 7);
  CHECK(futex_wake(h2g(&word), 1, 2) == 0);
}
TEST_CASE("core futex wake count preserves other bitsets on the same word") {
  alignas(4) std::uint32_t word = 7;
  GuestThread first(166), second(167), other(168);
  std::array<GuestThread*, 3> guests{&first, &second, &other};
  std::array<std::thread, 3> workers;
  std::array<std::atomic<int>, 3> results;
  for (unsigned i = 0; i < 3; ++i) {
    results[i] = 999;
    workers[i] = std::thread([&, i] {
      results[i] = futex_wait(*guests[i], h2g(&word), 7,
                             monotonic_ns() + 2 * venture::test::hang_timeout_ns, i == 2 ? 2 : 1);
    });
  }
  unsigned woke = 0;
  const auto end = monotonic_ns() + venture::test::hang_timeout_ns;
  CHECK(futex_wake(h2g(&word), 0) == 0);
  CHECK(futex_wake(h2g(&word), 3, 4) == 0);
  while (woke < 2 && monotonic_ns() < end) {
    woke += futex_wake(h2g(&word), 2 - woke, 1);
    std::this_thread::yield();
  }
  CHECK(woke == 2);
  CHECK(results[2] == 999);
  int last = 0;
  while (!last && monotonic_ns() < end) {
    last = futex_wake(h2g(&word), 1, 2);
    std::this_thread::yield();
  }
  CHECK(last == 1);
  INFO("bitset 1 wakes=", woke, " bitset 2 wakes=", last,
       " results=", results[0].load(), ",", results[1].load(), ",", results[2].load());
  for (unsigned i = 0; i < 3; ++i) {
    if (results[i] == 999) guests[i]->interrupt();
    workers[i].join();
    CHECK(results[i] == 0);
  }
}

TEST_CASE("core futex value, timeout, bitset wake and interrupt") {
  alignas(4) std::uint32_t word = 7;
  GuestThread current(60);
  CHECK(futex_wait(current, h2g(&word), 6, -1) == -err::kEAGAIN);
  CHECK(futex_wait(current, h2g(&word), 7, monotonic_ns() + 2'000'000) == -err::kETIMEDOUT);
  CHECK(futex_wait(current, h2g(&word), 7, -1, 0) == -err::kEINVAL);
  CHECK(futex_wait(current, h2g(&word), 7, monotonic_ns() - 1) == -err::kETIMEDOUT);
  CHECK(sleep_until(current, monotonic_ns() - 1) == 0);
  // Sub-millisecond deadlines must not be rounded down to an early timeout.
  for (int i = 0; i < 3; ++i) {
    const auto end = monotonic_ns() + 100'000;
    CHECK(sleep_until(current, end) == 0);
    CHECK(monotonic_ns() >= end);
  }
  // A rounded native timeout must retry the fractional tail, not finish early.
  const auto fractional_end = monotonic_ns() + 3'500'000;
  CHECK(sleep_until(current, fractional_end) == 0);
  CHECK(monotonic_ns() >= fractional_end);
  GuestThread waiting(61);
  std::atomic<bool> entered{false};
  std::atomic<int> result{999};
  std::thread worker([&] {
    entered = true;
    result = futex_wait(waiting, h2g(&word), 7, monotonic_ns() + 2 * venture::test::hang_timeout_ns, 2);
    // A notification must not satisfy the next predicate when the same host
    // thread switches from a long wait to a sub-millisecond timeout.
    const auto end = monotonic_ns() + 100'000;
    CHECK(futex_wait(waiting, h2g(&word), 7, end, 2) == -err::kETIMEDOUT);
    CHECK(monotonic_ns() >= end);
  });
  // Wake retries are the enrollment condition; a pre-call flag is not proof
  // that the worker reached futex_wait on a loaded host.
  CHECK(futex_wake(h2g(&word), 1, 1) == 0);
  int woke = 0;
  auto deadline = monotonic_ns() + venture::test::hang_timeout_ns;
  while (!woke && result == 999 && monotonic_ns() < deadline) { woke = futex_wake(h2g(&word), 1, 2); std::this_thread::yield(); }
  CHECK(woke == 1);
  if (!woke) waiting.interrupt();
  worker.join();
  INFO("worker entered=", entered.load(), " woke=", woke, " result=", result.load());
  CHECK(result == 0);
  std::thread sleeper([&] { result = sleep_until(waiting, monotonic_ns() + 2 * venture::test::hang_timeout_ns); });
  waiting.interrupt(); // Also exercises interrupt-before-enqueue.
  sleeper.join(); CHECK(result == -err::kEINTR);
}
