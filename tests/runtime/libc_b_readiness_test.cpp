#include "libc_b/test.h"
#include "core/thread_runtime.h"
#include <array>
#include <atomic>
#include <chrono>
#include <latch>
#include <thread>
#include <vector>

#include "../async_wait.h"

using namespace venture;
using namespace venture::hle::libc;
using namespace venture::test::libc_b;
namespace {
bool parked(std::int32_t tid, const std::atomic<bool>& done) {
  const auto end = monotonic_ns() + test::hang_timeout_ns;
  while (!done.load() && monotonic_ns() < end) {
    for (const auto& wait : futex_wait_snapshot()) if (wait.tid == tid && !wait.woken) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}
struct Event { std::uint32_t events, pad; std::uint64_t data; };
}
TEST_CASE("libc B: readiness writes wake parked eventfd pipe and socketpair pollers") {
  Thread parent;
  for (unsigned kind = 0; kind < 3; ++kind) {
    int fds[2];
    if (kind == 0) fds[0] = fds[1] = static_cast<int>(call("eventfd", {0, o_nonblock}));
    else if (kind == 1) REQUIRE(call("pipe2", {h2g(fds), o_nonblock}) == 0);
    else REQUIRE(call("socketpair", {1, 1 | o_nonblock, 0, h2g(fds)}) == 0);
    REQUIRE(fds[0] >= 0);
    std::atomic<bool> done = false;
    std::atomic<int> tid = 0;
    int result = -1;
    PollFd poll{fds[0], poll_in, 0};
    std::thread worker([&] {
      Thread thread(1300 + kind);
      tid = thread.thread.tid();
      result = fd_poll(&poll, 1, 30'000);
      done = true;
    });
    while (!tid.load()) std::this_thread::yield();
    CHECK(parked(tid, done));
    std::uint64_t value = 1;
    CHECK(fd_write(fds[1], &value, kind ? 1 : 8) == (kind ? 1 : 8));
    worker.join();
    CHECK(result == 1);
    CHECK(poll.revents == poll_in);
    CHECK(fd_read(fds[0], &value, kind ? 1 : 8) == (kind ? 1 : 8));
    CHECK(FdTable::get().close(fds[0]) == 0);
    if (fds[1] != fds[0]) CHECK(FdTable::get().close(fds[1]) == 0);
  }
}
TEST_CASE("libc B: readiness descriptor close wakes poll despite a retained open-file object") {
  Thread parent;
  const auto fd = static_cast<int>(call("eventfd", {0, o_nonblock}));
  REQUIRE(fd >= 0);
  auto retained = FdTable::get().file(fd);
  std::atomic<int> tid = 0;
  std::atomic<bool> done = false;
  PollFd poll{fd, poll_in, 0};
  int result = -1;
  std::thread worker([&] {
    Thread thread(1303); tid = thread.thread.tid();
    result = fd_poll(&poll, 1, 30'000); done = true;
  });
  while (!tid.load()) std::this_thread::yield();
  CHECK(parked(tid, done));
  CHECK(FdTable::get().close(fd) == 0);
  worker.join();
  CHECK(retained != nullptr);
  CHECK(result == 1);
  CHECK(poll.revents == poll_nval);
}
TEST_CASE("libc B: readiness epoll one-shot dispatches once and MOD wakes the next waiter") {
  Thread parent;
  int fds[2]; REQUIRE(call("pipe2", {h2g(fds), o_nonblock}) == 0);
  const auto ep = static_cast<unsigned>(call("epoll_create1", {0}));
  Event event{1 | 0x40000000u, 0, 0x1234};
  REQUIRE(call("epoll_ctl", {ep, 1, unsigned(fds[0]), h2g(&event)}) == 0);
  std::array<std::atomic<int>, 2> tids{}, results;
  std::array<std::atomic<bool>, 2> done{};
  std::array<Event, 2> output{};
  std::array<std::thread, 2> workers;
  for (unsigned i = 0; i < 2; ++i) {
    results[i] = 999;
    workers[i] = std::thread([&, i] {
      Thread thread(1306 + i); tids[i] = thread.thread.tid();
      results[i] = static_cast<int>(call("epoll_wait", {ep, h2g(&output[i]), 1, test::hang_timeout_ms}));
      done[i] = true;
    });
  }
  for (unsigned i = 0; i < 2; ++i) {
    while (!tids[i].load()) std::this_thread::yield();
    CHECK(parked(tids[i], done[i]));
  }
  CHECK(fd_write(fds[1], "x", 1) == 1);
  const auto end = monotonic_ns() + test::hang_timeout_ns;
  while (!done[0] && !done[1] && monotonic_ns() < end) std::this_thread::yield();
  CHECK(unsigned(done[0]) + unsigned(done[1]) == 1);
  // Data remains unread: MOD alone must rearm the one-shot and wake its waiter.
  CHECK(call("epoll_ctl", {ep, 3, unsigned(fds[0]), h2g(&event)}) == 0);
  for (auto& worker : workers) worker.join();
  for (unsigned i = 0; i < 2; ++i) { CHECK(results[i] == 1); CHECK(output[i].data == 0x1234); }
  CHECK(FdTable::get().close(ep) == 0);
  CHECK(FdTable::get().close(fds[0]) == 0); CHECK(FdTable::get().close(fds[1]) == 0);
}
