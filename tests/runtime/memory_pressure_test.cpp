#include "core/memory_pressure.h"
#include "platform/process_info.h"
#include <doctest/doctest.h>

using venture::memory_pressure::Level;
using venture::memory_pressure::classify;
using venture::memory_pressure::classify_gpu;

namespace {
constexpr std::uint64_t GiB = 1ull << 30, MiB = 1ull << 20;
}

TEST_CASE("Memory pressure: commit headroom levels on a 19 GiB limit (16 GB RAM plus page file)") {
  const auto limit = 19 * GiB;
  CHECK(classify(8 * GiB, limit, Level::normal) == Level::normal);
  CHECK(classify(2 * GiB + 1, limit, Level::normal) == Level::normal);
  CHECK(classify(2 * GiB - 1, limit, Level::normal) == Level::tight);
  CHECK(classify(1 * GiB - 1, limit, Level::normal) == Level::critical);
  // Raised levels clear only 512 MiB above their thresholds.
  CHECK(classify(2 * GiB + 256 * MiB, limit, Level::tight) == Level::tight);
  CHECK(classify(2 * GiB + 600 * MiB, limit, Level::tight) == Level::normal);
  CHECK(classify(1 * GiB + 256 * MiB, limit, Level::critical) == Level::critical);
  CHECK(classify(1 * GiB + 600 * MiB, limit, Level::critical) == Level::tight);
  CHECK(classify(0, limit, Level::normal) == Level::critical);
}

TEST_CASE("Memory pressure: thresholds scale with a large commit limit") {
  const auto limit = 128 * GiB;  // tight below 8 GiB, critical below 4 GiB
  CHECK(classify(9 * GiB, limit, Level::normal) == Level::normal);
  CHECK(classify(7 * GiB, limit, Level::normal) == Level::tight);
  CHECK(classify(3 * GiB, limit, Level::normal) == Level::critical);
}

TEST_CASE("Memory pressure: GPU memory against its budget") {
  CHECK(classify_gpu(5 * GiB, 0, Level::normal) == Level::normal);  // no budget known
  CHECK(classify_gpu(7 * GiB, 10 * GiB, Level::normal) == Level::normal);
  CHECK(classify_gpu(96 * GiB / 10, 10 * GiB, Level::normal) == Level::tight);
  CHECK(classify_gpu(92 * GiB / 10, 10 * GiB, Level::tight) == Level::tight);
  CHECK(classify_gpu(89 * GiB / 10, 10 * GiB, Level::tight) == Level::normal);
  CHECK(classify_gpu(11 * GiB, 10 * GiB, Level::normal) == Level::critical);
  CHECK(classify_gpu(97 * GiB / 10, 10 * GiB, Level::critical) == Level::critical);
}

TEST_CASE("Memory pressure: the host reports its allocation headroom") {
  venture::platform::MemoryHeadroom room;
  REQUIRE(venture::platform::memory_headroom(room));
  CHECK(room.limit > 0);
  CHECK(room.available <= room.limit);
}
