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

#include "core/session_timeline.h"

TEST_CASE("session timeline: one line per minute names fps, hitches, CPU and memory") {
  using namespace venture::session_timeline;
  Sample s;
  s.minutes = 31.2;
  s.seconds = 60;
  s.frames = {.count = 5400, .long_frames = 12, .worst_ns = 41'500'000};
  s.cpu_percent = 312.4;
  s.top = {{"UnityMain", 97.6}, {"UnityGfxDevice", 44.9}};
  s.private_bytes = 5ull << 30;
  s.gpu_used = 8200ull << 20;
  s.gpu_budget = 15000ull << 20;
  s.threads = 74;
  CHECK(format(s) == "t=31m fps=90.0 worst_frame_ms=41.5 long_frames=12 cpu=312% "
                     "top=\"UnityMain 98%, UnityGfxDevice 45%\" private_mib=5120 gpu_mib=8200/15000 threads=74");
  // The first minute has no CPU baseline, and unknown memory is left out.
  Sample first;
  first.minutes = 1;
  first.seconds = 60;
  first.frames.count = 0;
  first.threads = 20;
  CHECK(format(first) == "t=1m fps=0.0 worst_frame_ms=0.0 long_frames=0 threads=20");
}
