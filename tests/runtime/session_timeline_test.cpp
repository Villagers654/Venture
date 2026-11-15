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

TEST_CASE("Session timeline: per-frame host wait, game frame, host end and display margin") {
  Sample s;
  s.minutes = 30;
  s.seconds = 60;
  s.frames.count = 6000;
  s.threads = 1;
  CHECK(format(s).find("wait_ms") == std::string::npos);  // no frames timed: nothing printed
  s.phases.waits = 6000;
  s.phases.wait_ns = 6000ll * 2'500'000;
  s.phases.frames = 6000;
  s.phases.app_ns = 6000ll * 5'000'000;
  s.phases.prep_ns = 6000ll * 500'000;
  s.phases.end_ns = 6000ll * 250'000;
  s.phases.margin_ns = 6000ll * 4'000'000;
  s.phases.min_margin_ns = -1'500'000;
  s.phases.late = 7;
  s.eye_width = 3072;
  s.eye_height = 3264;
  s.recommended_width = 3072;
  s.recommended_height = 3264;
  const auto line = format(s);
  CHECK(line.find(" wait_ms=2.50 app_ms=5.00 prep_ms=0.50 end_ms=0.25 margin_ms=4.00 min_margin_ms=-1.50 late=7") !=
        std::string::npos);
  CHECK(line.find(" eye=3072x3264 rec=3072x3264") != std::string::npos);
  CHECK(line.find("timeouts") == std::string::npos);
  s.timeouts = {12000, 12000ll * 600'000, 14'900'000};
  CHECK(format(s).find(" timeouts=12000 timeout_late_ms=0.60/14.90") != std::string::npos);
  CHECK(line.find("gpu_mhz") == std::string::npos);
  s.gpu_mhz = 2407;
  s.gpu_max_mhz = 3090;
  s.gpu_celsius = 61.4;
  CHECK(format(s).find(" gpu_mhz=2407/3090 gpu_c=61") != std::string::npos);
}

TEST_CASE("Session timeline: GPU clock of a busy engine") {
  venture::platform::GpuClock clock;
  CHECK_FALSE(venture::platform::gpu_clock("not an engine", clock));
  CHECK(clock.hz == 0);
  // Any engine this process has used (none in a plain test run) reads a plausible clock.
  std::vector<venture::platform::GpuEngineTime> engines;
  if (!venture::platform::gpu_engine_times(engines)) return;
  for (const auto& e : engines)
    if (venture::platform::gpu_clock(e.engine, clock) && clock.max_hz) CHECK(clock.hz <= clock.max_hz * 2);
}
