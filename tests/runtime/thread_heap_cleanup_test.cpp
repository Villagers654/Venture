#include <doctest/doctest.h>
#include <mimalloc.h>
#include "core/guest_heap.h"
#include "core/hle_runtime.h"
#include "core/thread_runtime.h"
#include <atomic>
#include <array>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <algorithm>
#include <vector>

using namespace venture;
namespace {
thread_local bool block_cleanup = false;
struct CleanupGate {
  std::promise<void> entered;
  std::shared_future<void> release;
};
void deferred_free(bool force, unsigned long long, void* user) {
  if (!force || !block_cleanup) return;
  block_cleanup = false;
  auto& gate = *static_cast<CleanupGate*>(user);
  gate.entered.set_value();
  gate.release.wait();
}
}
TEST_CASE("slow guest heap cleanup permits unrelated thread creation and join") {
  using namespace std::chrono_literals;
  std::promise<void> release;
  CleanupGate gate{{}, release.get_future().share()};
  auto entered = gate.entered.get_future();
  // Synthetic ARM fixture: mov r0, #37; bx lr.
  const std::array code{0xe3a00025u, 0xe12fff1eu};
  const auto quick = hle::code_stub(code, "cleanup independent worker");
  // This isolated executable uses mimalloc's actual deletion callback. No
  // runtime test hook is needed. Only the explicitly armed native thread blocks.
  mi_register_deferred_free(deferred_free, &gate);
  std::atomic<void*> retained{nullptr};
  auto slow = std::async(std::launch::async, [&] {
    GuestThread thread(9100); thread.attach_to_current_host_thread();
    auto* value = guest_heap::allocate(sizeof(std::uint32_t));
    if (!value) throw std::bad_alloc();
    *static_cast<std::uint32_t*>(value) = 123;
    retained = value;
    block_cleanup = true;
    thread.detach_from_current_host_thread();
  });
  const bool blocked = entered.wait_for(30s) == std::future_status::ready;
  INFO("cleanup entered=", blocked, " retained allocation=", retained.load() != nullptr);
  CHECK(blocked);
  if (!blocked) { release.set_value(); slow.get(); return; }
  bool still_registered = false;
  CHECK(try_for_each_guest_thread([&](GuestThread& thread) { still_registered |= thread.tid() == 9100; }));
  CHECK_FALSE(still_registered);
  auto independent = std::async(std::launch::async, [&] {
    GuestThread parent(9101); parent.attach_to_current_host_thread();
    auto child = create_guest_thread(quick, 0);
    std::uint64_t result = 0;
    const bool joined = child && join_guest_thread(child->pthread_self, &result) == 0 && result == 37;
    parent.detach_from_current_host_thread();
    return joined;
  });
  const auto independent_status = independent.wait_for(30s);
  INFO("independent join ready=", independent_status == std::future_status::ready,
       " cleanup still blocked=", slow.wait_for(0s) == std::future_status::timeout);
  CHECK(independent_status == std::future_status::ready);
  CHECK(slow.wait_for(0s) == std::future_status::timeout);
  // Always release before joining, including when testing the old locking scope.
  release.set_value();
  CHECK(independent.get());
  slow.get();
  CHECK(*static_cast<std::uint32_t*>(retained.load()) == 123);
  guest_heap::deallocate(retained.load());
  // Keep the callback installed through native TLS finalization. All remaining
  // threads have block_cleanup=false and never dereference the completed gate.
}

TEST_CASE("guest heaps survive short-lived threads whose blocks other threads free") {
  using namespace std::chrono_literals;
  // Blocks handed between threads, as a render thread passes buffers to the
  // thread that joins it.
  std::mutex mutex;
  std::vector<void*> handed;
  std::atomic<bool> stop{false};
  std::atomic<int> next_tid{9200};
  auto take = [&] {
    std::lock_guard lock(mutex);
    if (handed.empty()) return static_cast<void*>(nullptr);
    auto* p = handed.back();
    handed.pop_back();
    return p;
  };
  // Long-lived threads free what the short-lived ones hand over.
  std::vector<std::thread> freers;
  for (int i = 0; i < 3; ++i)
    freers.emplace_back([&] {
      GuestThread thread(next_tid++);
      thread.attach_to_current_host_thread();
      while (!stop) {
        if (auto* p = take()) guest_heap::deallocate(p);
        else std::this_thread::yield();
        if (auto* q = guest_heap::allocate(48)) guest_heap::deallocate(q);
      }
      thread.detach_from_current_host_thread();
    });
  auto deadline = std::chrono::steady_clock::now() + 4s;
  int rounds = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    std::vector<std::future<void>> wave;
    for (int i = 0; i < 6; ++i)
      wave.push_back(std::async(std::launch::async, [&, seed = rounds * 7 + i] {
        GuestThread thread(next_tid++);
        thread.attach_to_current_host_thread();
        std::vector<void*> own;
        for (int n = 0; n < 200; ++n) {
          auto* p = guest_heap::allocate(16 + std::uint64_t((seed * 31 + n * 17) % 600));
          if (!p) continue;
          if (n % 3 == 0) {
            std::lock_guard lock(mutex);
            handed.push_back(p);
          } else {
            own.push_back(p);
          }
          if (n % 5 == 0 && !own.empty()) {
            guest_heap::deallocate(own.back());
            own.pop_back();
          }
        }
        for (auto* p : own) guest_heap::deallocate(p);
        thread.detach_from_current_host_thread();
      }));
    // Host callback threads that run guest code and exit without detaching.
    for (int i = 0; i < 3; ++i)
      wave.push_back(std::async(std::launch::async, [&, seed = rounds * 5 + i] {
        GuestThread::adopt_current();
        for (int n = 0; n < 100; ++n) {
          auto* p = guest_heap::allocate(24 + std::uint64_t((seed * 13 + n * 7) % 300));
          if (!p) continue;
          if (n % 2) {
            std::lock_guard lock(mutex);
            handed.push_back(p);
          } else if (auto* q = take()) {
            guest_heap::deallocate(q);
            guest_heap::deallocate(p);
          } else {
            guest_heap::deallocate(p);
          }
        }
      }));
    for (auto& f : wave) {
      bool done = f.wait_for(30s) == std::future_status::ready;
      REQUIRE_MESSAGE(done, "a guest thread's heap teardown hung in round ", rounds);
    }
    ++rounds;
  }
  stop = true;
  for (auto& t : freers) t.join();
  while (auto* p = take()) guest_heap::deallocate(p);
  MESSAGE("rounds: ", rounds);
}

// Runs twice: as is (the allocated-block bits) and with VENTURE_HEAP_CHECK=1.
TEST_CASE("a block freed again on another thread is ignored") {
  GuestThread owner(9400);
  owner.attach_to_current_host_thread();
  // A second live block keeps the page in use, so the first free stays on the
  // page's local free list when the other thread frees it again.
  auto* keep = guest_heap::allocate(64);
  auto* p = guest_heap::allocate(64);
  REQUIRE(keep);
  REQUIRE(p);
  guest_heap::deallocate(p);
  // mimalloc trusts this second free from another thread: the block would sit
  // on both free lists, and the owner's heap teardown would never finish.
  std::async(std::launch::async, [p] {
    GuestThread other(9401);
    other.attach_to_current_host_thread();
    guest_heap::deallocate(p);
    other.detach_from_current_host_thread();
  }).get();
  std::vector<void*> blocks;
  for (int i = 0; i < 64; ++i) blocks.push_back(guest_heap::allocate(64));
  std::ranges::sort(blocks);
  CHECK(std::ranges::adjacent_find(blocks) == blocks.end());  // never handed out twice
  for (auto* b : blocks) guest_heap::deallocate(b);
  guest_heap::deallocate(keep);
  owner.detach_from_current_host_thread();
}

extern "C" std::size_t _mi_current_thread_count(void);
TEST_CASE("mimalloc releases the thread data of exited host threads") {
  // Each thread initializes mimalloc's thread data; its exit must release it.
  auto touch = [] { mi_free(mi_malloc(64)); };
  touch();
  const auto before = _mi_current_thread_count();
  std::vector<std::thread> threads;
  for (int i = 0; i < 16; ++i) threads.emplace_back(touch);
  for (auto& t : threads) t.join();
  CHECK(_mi_current_thread_count() == before);
}
