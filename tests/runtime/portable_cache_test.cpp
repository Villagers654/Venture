// The JIT block cache (cpu/portable_cache.cpp) under concurrent stores, lookups and
// flushes beside its background writer. ctest runs this with VENTURE_JIT_PTC_DIR set
// to an empty directory, so the first batch also builds the file's dictionary.
#include <doctest/doctest.h>
#include "common/environment.h"
#include "cpu/portable_cache.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using namespace venture;
namespace {
// Far from any guest address, so nothing else in the process stores these.
constexpr std::uint64_t kBase = 0x5a00000000000000ull;
// A block's bytes: a size and content that depend only on its descriptor.
std::vector<std::uint8_t> block(std::uint64_t descriptor) {
  std::uint64_t x = descriptor * 0x9e3779b97f4a7c15ull;
  std::vector<std::uint8_t> bytes(64 + std::size_t(x % 1500));
  for (auto& b : bytes) {
    x ^= x >> 29;
    x *= 0xbf58476d1ce4e5b9ull;
    // Mostly repeated bytes, so records compress like real blocks.
    b = std::uint8_t((x >> 61) ? 0x90 : x >> 40);
  }
  return bytes;
}
bool same(std::pair<const std::uint8_t*, std::size_t> found, const std::vector<std::uint8_t>& expected) {
  return found.second == expected.size() && std::equal(expected.begin(), expected.end(), found.first);
}
}  // namespace

TEST_CASE("stores, lookups and flushes from many threads beside the background writer keep every block intact") {
  REQUIRE(environment::getenv("VENTURE_JIT_PTC_DIR"));
  auto* cache = PortableBlockCache::get();
  REQUIRE(cache);
  REQUIRE(cache->accepting());
  constexpr unsigned kStorers = 6, kFlushers = 3, kReaders = 4, kPerStorer = 6000;
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> wrong{0}, found{0};
  std::vector<std::thread> threads;
  for (unsigned t = 0; t < kStorers; ++t)
    threads.emplace_back([&, t] {
      // Overlapping ranges: the same block is stored again while an earlier copy is being written.
      for (unsigned i = 0; i < kPerStorer; ++i) {
        const auto descriptor = kBase + (t / 2) * kPerStorer + i;
        const auto bytes = block(descriptor);
        cache->store(descriptor, bytes.data(), bytes.size());
      }
    });
  for (unsigned t = 0; t < kFlushers; ++t)
    threads.emplace_back([&] {
      while (!stop.load()) cache->flush();
    });
  for (unsigned t = 0; t < kReaders; ++t)
    threads.emplace_back([&, t] {
      std::vector<std::uint8_t> copied;
      for (std::uint64_t n = t; !stop.load(); n = n * 6364136223846793005ull + 1442695040888963407ull) {
        const auto descriptor = kBase + (n >> 33) % ((kStorers / 2) * kPerStorer);
        const auto expected = block(descriptor);
        if (const auto hit = cache->find(descriptor); hit.first) {
          found.fetch_add(1);
          if (!same(hit, expected)) wrong.fetch_add(1);
        }
        if (cache->copy(descriptor, copied) && copied != expected) wrong.fetch_add(1);
        (void)cache->contains(descriptor);
      }
    });
  for (unsigned t = 0; t < kStorers; ++t) threads[t].join();
  // Lookups and flushes go on a little after the last store.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  stop = true;
  for (unsigned t = kStorers; t < threads.size(); ++t) threads[t].join();
  cache->flush();
  CHECK(wrong.load() == 0);
  CHECK(found.load() > 0);
  // The unwritten-bytes count (it bounds the writer's backlog) returns to zero, replaced copies included.
  CHECK(cache->unwritten_bytes() == 0);
  // Every block is in the file now, and reads back as stored.
  unsigned missing = 0, differ = 0;
  for (std::uint64_t i = 0; i < (kStorers / 2) * kPerStorer; ++i) {
    const auto descriptor = kBase + i;
    const auto hit = cache->find(descriptor);
    if (!hit.first) ++missing;
    else if (!same(hit, block(descriptor))) ++differ;
  }
  CHECK(missing == 0);
  CHECK(differ == 0);
}

// Explored marks across runs: ctest runs these three in order, each in its own process
// on the same directory (the cache opens its file once per process).
namespace {
constexpr std::uint64_t kMarked = 0x5b00000000000000ull;
constexpr unsigned kFirst = 100, kLater = 10;
void store_range(PortableBlockCache* cache, std::uint64_t from, std::uint64_t to) {
  for (auto d = from; d < to; ++d) {
    const auto bytes = block(d);
    cache->store(d, bytes.data(), bytes.size());
  }
  cache->flush();
}
unsigned intact(PortableBlockCache* cache, std::uint64_t from, std::uint64_t to) {
  unsigned n = 0;
  for (auto d = from; d < to; ++d) n += same(cache->find(d), block(d));
  return n;
}
}  // namespace

TEST_CASE("explored marks 1: a mark covers the explored records written before it" * doctest::skip()) {
  auto* cache = PortableBlockCache::get();
  REQUIRE(cache);
  store_range(cache, kMarked, kMarked + kFirst);
  // Odd records are not explored yet: the mark lists them.
  CHECK(cache->mark_explored([](std::uint64_t d) { return d % 2 == 0; }));
  store_range(cache, kMarked + kFirst, kMarked + kFirst + kLater);
  // Nothing newly explored: no mark.
  CHECK_FALSE(cache->mark_explored([](std::uint64_t) { return false; }));
  // Only an earlier run's marks count.
  for (auto d = kMarked; d < kMarked + kFirst + kLater; ++d) CHECK_FALSE(cache->explored(d));
  CHECK(intact(cache, kMarked, kMarked + kFirst + kLater) == kFirst + kLater);
}

TEST_CASE("explored marks 2: the next run skips covered records and marks the rest" * doctest::skip()) {
  auto* cache = PortableBlockCache::get();
  REQUIRE(cache);
  unsigned even = 0, odd = 0, later = 0;
  for (auto d = kMarked; d < kMarked + kFirst; ++d) (d % 2 ? odd : even) += cache->explored(d);
  for (auto d = kMarked + kFirst; d < kMarked + kFirst + kLater; ++d) later += cache->explored(d);
  CHECK(even == kFirst / 2);
  CHECK(odd == 0);
  CHECK(later == 0);
  // The marks sit between records without hiding any.
  CHECK(intact(cache, kMarked, kMarked + kFirst + kLater) == kFirst + kLater);
  // The rest is explored now, and one more record written after this run's mark stays out.
  std::vector<std::uint64_t> asked;
  CHECK(cache->mark_explored([&](std::uint64_t d) { asked.push_back(d); return true; }));
  // Only what no mark covered yet is asked about: the odd ones and the later ones.
  CHECK(asked.size() == kFirst / 2 + kLater);
  store_range(cache, kMarked + kFirst + kLater, kMarked + kFirst + kLater + 1);
}

TEST_CASE("explored marks 3: the last mark counts" * doctest::skip()) {
  auto* cache = PortableBlockCache::get();
  REQUIRE(cache);
  unsigned explored = 0;
  for (auto d = kMarked; d < kMarked + kFirst + kLater; ++d) explored += cache->explored(d);
  CHECK(explored == kFirst + kLater);
  CHECK_FALSE(cache->explored(kMarked + kFirst + kLater));
  CHECK(intact(cache, kMarked, kMarked + kFirst + kLater + 1) == kFirst + kLater + 1);
}
