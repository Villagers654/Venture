#include "libc_b/test.h"
#include "core/address_space.h"
#include "core/hle_runtime.h"
#include "core/mutex_watch.h"
#include <chrono>
#include <algorithm>
#include "core/thread_runtime.h"
#include "core/condition_census.h"
#include "hle/libc/condition_waiter.h"
#include "hle/libc/yield_backoff.h"
#include <array>
#include <barrier>
#include <climits>
#include <latch>
#include <future>
#include <thread>
#include "platform/thread_priority.h"
#ifdef _WIN32
#include <windows.h>
#endif
using namespace venture;
using namespace venture::hle::libc;
using namespace venture::test::libc_b;

TEST_CASE("libc B: zero initialized mutex contention and recursive types") {
  Thread parent;
  alignas(8) std::uint32_t mutex[10]{};
  int count = 0;
  std::vector<std::thread> workers;
  for (int i = 0; i < 4; ++i)
    workers.emplace_back([&, i] {
      Thread t(1010 + i);
      for (int j = 0; j < 500; ++j) {
        REQUIRE(call("pthread_mutex_lock", {h2g(mutex)}) == 0);
        ++count;
        REQUIRE(call("pthread_mutex_unlock", {h2g(mutex)}) == 0);
      }
    });
  for (auto& t : workers) t.join();
  CHECK(count == 2000);
  std::uint64_t attr = 0;
  REQUIRE(call("pthread_mutexattr_settype", {h2g(&attr), 1}) == 0);
  REQUIRE(call("pthread_mutex_init", {h2g(mutex), h2g(&attr)}) == 0);
  CHECK(call("pthread_mutex_lock", {h2g(mutex)}) == 0);
  CHECK(call("pthread_mutex_lock", {h2g(mutex)}) == 0);
  CHECK(call("pthread_mutex_unlock", {h2g(mutex)}) == 0);
  CHECK(call("pthread_mutex_destroy", {h2g(mutex)}) == err::ebusy);
  CHECK(call("pthread_mutex_unlock", {h2g(mutex)}) == 0);
  attr = 2;
  REQUIRE(call("pthread_mutex_init", {h2g(mutex), h2g(&attr)}) == 0);
  REQUIRE(call("pthread_mutex_lock", {h2g(mutex)}) == 0);
  CHECK(call("pthread_mutex_lock", {h2g(mutex)}) == err::edeadlk);
  CHECK(call("pthread_mutex_unlock", {h2g(mutex)}) == 0);
}
TEST_CASE("libc B: condition variable release reacquire and monotonic timeout") {
  Thread parent;
  alignas(8) std::uint32_t mutex[10]{}, cond[12]{};
  bool ready = false;
  std::thread worker([&] {
    Thread t(1020);
    REQUIRE(call("pthread_mutex_lock", {h2g(mutex)}) == 0);
    ready = true;
    CHECK(call("pthread_cond_signal", {h2g(cond)}) == 0);
    CHECK(call("pthread_mutex_unlock", {h2g(mutex)}) == 0);
  });
  REQUIRE(call("pthread_mutex_lock", {h2g(mutex)}) == 0);
  while (!ready) REQUIRE(call("pthread_cond_wait", {h2g(cond), h2g(mutex)}) == 0);
  CHECK(call("pthread_mutex_unlock", {h2g(mutex)}) == 0);
  worker.join();
  REQUIRE(call("pthread_mutex_lock", {h2g(mutex)}) == 0);
  auto n = monotonic_ns() + 2000000;
  Timespec time{n / 1000000000, n % 1000000000};
  CHECK(call("pthread_cond_clockwait", {h2g(cond), h2g(mutex), 1, h2g(&time)}) == err::etimedout);
  CHECK(call("pthread_mutex_unlock", {h2g(mutex)}) == 0);
}
TEST_CASE("libc B: A64 condition signal and broadcast preserve registered waiters and ordering") {
  Thread parent;
  alignas(8) std::uint32_t mutex[10]{}, cond[12]{};
  unsigned permits = 0, payload = 0;
  std::atomic<unsigned> finished = 0;
  std::array<std::thread, 2> workers;
  for (unsigned i = 0; i < workers.size(); ++i) workers[i] = std::thread([&, i] {
    Thread thread(1100 + i);
    REQUIRE(call("pthread_mutex_lock", {h2g(mutex)}) == 0);
    while (!permits) {
      auto end = monotonic_ns() + 30'000'000'000;
      Timespec deadline{end / 1'000'000'000, end % 1'000'000'000};
      const auto r = call("pthread_cond_clockwait", {h2g(cond), h2g(mutex), 1, h2g(&deadline)});
      CHECK(r == 0);
      if (r) break;
    }
    CHECK(permits > 0);
    CHECK(payload == 42);
    if (permits) --permits;
    CHECK(call("pthread_mutex_unlock", {h2g(mutex)}) == 0);
    ++finished;
  });
  auto until = monotonic_ns() + 30'000'000'000;
  while (std::atomic_ref(cond[1]).load() != 2 && monotonic_ns() < until) std::this_thread::yield();
  CHECK(std::atomic_ref(cond[1]).load() == 2);
  REQUIRE(call("pthread_mutex_lock", {h2g(mutex)}) == 0);
  permits = 1; payload = 42;
  CHECK(call("pthread_cond_signal", {h2g(cond)}) == 0);
  CHECK(call("pthread_mutex_unlock", {h2g(mutex)}) == 0);
  while (finished.load() != 1 && monotonic_ns() < until) std::this_thread::yield();
  CHECK(finished.load() == 1);
  CHECK(std::atomic_ref(cond[1]).load() == 1);
  REQUIRE(call("pthread_mutex_lock", {h2g(mutex)}) == 0);
  permits = 1;
  CHECK(call("pthread_cond_broadcast", {h2g(cond)}) == 0);
  CHECK(call("pthread_mutex_unlock", {h2g(mutex)}) == 0);
  for (auto& worker : workers) worker.join();
  CHECK(finished == 2);
  CHECK(cond[1] == 0);
}

TEST_CASE("libc B: rwlock exclusion semaphores and reusable barriers") {
  Thread parent;
  alignas(8) std::uint32_t rw[14]{}, sem[4]{}, barrier[8]{};
  REQUIRE(call("pthread_rwlock_rdlock", {h2g(rw)}) == 0);
  REQUIRE(call("pthread_rwlock_rdlock", {h2g(rw)}) == 0);
  CHECK(call("pthread_rwlock_trywrlock", {h2g(rw)}) == err::ebusy);
  CHECK(call("pthread_rwlock_unlock", {h2g(rw)}) == 0);
  CHECK(call("pthread_rwlock_unlock", {h2g(rw)}) == 0);
  REQUIRE(call("pthread_rwlock_wrlock", {h2g(rw)}) == 0);
  CHECK(call("pthread_rwlock_tryrdlock", {h2g(rw)}) == err::ebusy);
  CHECK(call("pthread_rwlock_unlock", {h2g(rw)}) == 0);
  REQUIRE(call("sem_init", {h2g(sem), 0, 0}) == 0);
  CHECK(call("sem_trywait", {h2g(sem)}) == -1);
  CHECK(*parent.thread.errno_location() == err::eagain);
  std::thread worker([&] {
    Thread t(1030);
    CHECK(call("sem_post", {h2g(sem)}) == 0);
  });
  CHECK(call("sem_wait", {h2g(sem)}) == 0);
  worker.join();
  REQUIRE(call("pthread_barrier_init", {h2g(barrier), 0, 3}) == 0);
  std::atomic<int> serial = 0;
  std::vector<std::thread> workers;
  for (int i = 0; i < 3; ++i)
    workers.emplace_back([&, i] {
      Thread t(1040 + i);
      for (int n = 0; n < 100; ++n)
        if (call("pthread_barrier_wait", {h2g(barrier)}) == -1) ++serial;
    });
  for (auto& t : workers) t.join();
  CHECK(serial == 100);
}
TEST_CASE("libc B: pthread keys are per guest thread and reuse clears live threads") {
  Thread parent;
  int key;
  REQUIRE(call("pthread_key_create", {h2g(&key), 0}) == 0);
  REQUIRE(call("pthread_setspecific", {static_cast<unsigned>(key), 0x1234}) == 0);
  std::barrier phases(2);
  std::jthread worker([&] {
    Thread child(1049);
    CHECK(call("pthread_getspecific", {static_cast<unsigned>(key)}) == 0);
    CHECK(call("pthread_setspecific", {static_cast<unsigned>(key), 0xbeef}) == 0);
    CHECK(call("pthread_getspecific", {static_cast<unsigned>(key)}) == 0xbeef);
    phases.arrive_and_wait();
    phases.arrive_and_wait();
    CHECK(call("pthread_getspecific", {static_cast<unsigned>(key)}) == 0);
    phases.arrive_and_wait();
  });
  phases.arrive_and_wait();
  CHECK(call("pthread_getspecific", {static_cast<unsigned>(key)}) == 0x1234);
  CHECK(call("pthread_key_delete", {static_cast<unsigned>(key)}) == 0);
  int replacement = -1;
  CHECK(call("pthread_key_create", {h2g(&replacement), 0}) == 0);
  CHECK(replacement == key);
  CHECK(call("pthread_getspecific", {static_cast<unsigned>(key)}) == 0);
  phases.arrive_and_wait();
  phases.arrive_and_wait();
  worker.join();
  CHECK(call("pthread_key_delete", {static_cast<unsigned>(key)}) == 0);
}

#ifdef _WIN32
#endif
TEST_CASE("libc B: pthread_exit runs cleanup handlers before their stack frames are reused") {
  Thread parent(1300);
  // A handler with its own frame: sub sp,sp,#96; str xzr,[sp,#72]; ++*x0; add sp,sp,#96; ret.
  const std::array code{0xd10183ffu, 0xf90027ffu, 0xf9400001u, 0x91000421u, 0xf9000001u, 0x910183ffu, 0xd65f03c0u};
  struct Fixture { GuestAddr handler, count; } fixture{
      hle::code_stub(code, "cleanup handler"),
      AddressSpace::get().allocate(4096, kProtRead | kProtWrite, "[cleanup count]")};
  REQUIRE(fixture.count);
  auto start = hle::make_thunk("test", "cleanup exit", [](hle::CallContext& ctx) {
    const auto& f = *static_cast<Fixture*>(ctx.user());
    const auto top = ctx.cpu().sp();
    // Records live in the exiting routine's frame, as pthread_cleanup_push places them.
    ctx.cpu().set_sp(top - 128);
    call("__pthread_cleanup_push", {top - 32, f.handler, f.count});
    call("__pthread_cleanup_push", {top - 64, f.handler, f.count});
    call("pthread_exit", {7});
  }, &fixture);
  auto child = create_guest_thread(start, 0);
  REQUIRE(child != nullptr);
  std::uint64_t result = 0;
  CHECK(join_guest_thread(child->pthread_self, &result) == 0);
  CHECK(result == 7);
  CHECK(*g2h<std::uint64_t>(fixture.count) == 2);
}
