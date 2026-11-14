#include "core/session_timeline.h"
#include "platform/process_info.h"
#include <doctest/doctest.h>

using venture::session_timeline::Sample;
using venture::session_timeline::format;

TEST_CASE("Session timeline: GPU busy time and GPU time per frame") {
  Sample s;
  s.minutes = 31;
  s.seconds = 60;
  s.frames.count = 6000;
  s.cpu_percent = 80;
  s.top = {{"UnityMain", 27}, {"tid 37228 nvoglv64.dll", 12}};
  s.gpu_busy_ns = 30'000'000'000;  // half of the minute
  s.threads = 217;
  const auto line = format(s);
  CHECK(line.find("fps=100.0") != std::string::npos);
  CHECK(line.find("top=\"UnityMain 27%, tid 37228 nvoglv64.dll 12%\"") != std::string::npos);
  CHECK(line.find(" gpu_busy=50% gpu_frame_ms=5.00") != std::string::npos);

  s.gpu_busy_ns = -1;  // the host cannot say
  CHECK(format(s).find("gpu_") == std::string::npos);
}

TEST_CASE("Session timeline: per-process GPU engine times are named and monotonic") {
  std::vector<venture::platform::GpuEngineTime> first, second;
  if (!venture::platform::gpu_engine_times(first)) return;  // no GPU accounting on this host
  CHECK(venture::platform::gpu_engine_times(second));
  for (const auto& e : first) {
    CHECK(!e.engine.empty());
    for (const auto& later : second)
      if (later.engine == e.engine) CHECK(later.ns >= e.ns);
  }
}

TEST_CASE("session timeline: GPU counter growth past 2^63") {
  using venture::session_timeline::counter_delta;
  // The H3VR2 3D engine counter read 18446744037894407444 (100 ns units) and still grew.
  const std::uint64_t before = 18446744037894407444ull * 100, now = 18446744037897440881ull * 100;
  REQUIRE(counter_delta(now, before));
  CHECK(*counter_delta(now, before) == 303343700ull);
  CHECK(counter_delta(5, 5) == std::uint64_t{0});
  CHECK_FALSE(counter_delta(4, 5));  // went back
}
