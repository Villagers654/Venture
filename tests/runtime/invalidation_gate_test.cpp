#include <doctest/doctest.h>
#include "cpu/invalidation_gate.h"
#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>
using namespace venture;

TEST_CASE("invalidation gate never lets a run start with a completed invalidation unapplied") {
  // Each invalidator queues increasing ids and records how many have
  // completed. Every run checks that it applied at least that many: a run
  // that began after wait() returned must have seen the range.
  constexpr int threads = 4, per_thread = 3000;
  InvalidationGate gate;
  std::array<std::atomic<int>, threads> completed{};
  std::array<int, threads> applied{};
  std::atomic<bool> halt = false;
  std::atomic<int> finished = 0;
  bool ok = true;
  std::vector<std::thread> invalidators;
  // The owner thread is the one that constructed the gate; run it here.
  for (int t = 0; t < threads; ++t) {
    invalidators.emplace_back([&, t] {
      for (int i = 0; i < per_thread; ++i) {
        gate.wait(gate.queue(t, i, [&] { halt.store(true); }));
        completed[t].store(i + 1);
      }
      finished.fetch_add(1);
    });
  }
  while (finished.load() < threads) {
    std::array<int, threads> done{};
    for (int t = 0; t < threads; ++t) done[t] = completed[t].load();
    gate.enter([&](std::uint64_t t, std::uint64_t i) {
      ok &= applied[t] == static_cast<int>(i);
      applied[t] = static_cast<int>(i) + 1;
    });
    for (int t = 0; t < threads; ++t) ok &= applied[t] >= done[t];
    // "Execute" until halted, as Dynarmic checks its halt flag between blocks.
    for (int spin = 0; spin < 1000 && !halt.exchange(false); ++spin) std::this_thread::yield();
    gate.leave();
  }
  for (auto& thread : invalidators) thread.join();
  gate.enter([&](std::uint64_t t, std::uint64_t i) { applied[t] = static_cast<int>(i) + 1; });
  gate.leave();
  CHECK(ok);
  for (int t = 0; t < threads; ++t) CHECK(applied[t] == per_thread);
}
