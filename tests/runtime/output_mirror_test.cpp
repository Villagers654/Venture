// The PC output's copy of the headset mix (audio/output_mirror.h), with fake
// devices: a headset pushing 10 ms blocks on one clock and a PC output pulling
// uneven blocks on another, in simulated time. No audio device.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "audio/output_mirror.h"

using namespace venture::audio;

namespace {
// The headset's mix: a ramp over 1024 frames on the left, its negative on the right.
float ramp(std::uint64_t frame) { return float(frame % 1024) / 1024; }

struct Devices {
  OutputMirror mirror;
  double headset_rate, pc_rate;  // frames per simulated second
  double headset_time = 0, pc_time = 0;
  std::uint64_t produced = 0, pulls = 0;
  std::vector<float> block = std::vector<float>(4096);
  bool headset_running = true, pc_running = true;
  explicit Devices(double headset_ppm, double pc_ppm = 0, std::uint32_t capacity = 24000)
      : mirror(capacity), headset_rate(48000 * (1 + headset_ppm * 1e-6)), pc_rate(48000 * (1 + pc_ppm * 1e-6)) {}
  void produce() {
    float frames[480 * 2];
    for (int i = 0; i < 480; ++i) {
      frames[2 * i] = ramp(produced + i);
      frames[2 * i + 1] = -ramp(produced + i);
    }
    produced += 480;
    mirror.push(frames, 480);
  }
  // Runs both devices for seconds of simulated time; check sees each PC block.
  template <class Check>
  void run(double seconds, Check&& check) {
    const double end = std::max(headset_time, pc_time) + seconds;
    while (std::min(headset_time, pc_time) < end) {
      if (headset_time <= pc_time) {
        if (headset_running) produce();
        headset_time += 480 / headset_rate;
      } else {
        // Uneven pulls averaging 10 ms, as a resampling device asks.
        const std::uint32_t frames = pulls++ % 2 ? 441 : 519;
        if (pc_running) {
          mirror.pull(block.data(), frames);
          check(block.data(), frames);
        }
        pc_time += frames / pc_rate;
      }
    }
  }
  void run(double seconds) {
    run(seconds, [](const float*, std::uint32_t) {});
  }
};

// The PC output plays the headset's samples in order, stretched by the ratio:
// no frame dropped or repeated (Catmull-Rom reproduces a ramp exactly).
struct Continuity {
  const OutputMirror& mirror;
  float previous = -1;
  std::uint64_t checked = 0, broken = 0;
  void operator()(const float* out, std::uint32_t frames) {
    const double step = mirror.ratio() / 1024;
    for (std::uint32_t i = 0; i < frames; ++i) {
      const float v = out[2 * i];
      if (std::abs(out[2 * i + 1] + v) > 1e-6f) ++broken;  // the right channel stays the left's negative
      // Away from the ramp's wrap (where the interpolation falls steeply).
      if (previous > 0.05f && previous < 0.9f && v > 0.05f && v < 0.9f && v > previous - 1e-3f) {
        ++checked;
        if (std::abs((v - previous) - step) > 2e-5) ++broken;
      }
      previous = v;
    }
  }
};

void drift_case(double headset_ppm, double pc_ppm) {
  Devices d(headset_ppm, pc_ppm);
  d.mirror.activate(2 * (480 + 480) + 480);
  d.run(20);  // priming and settling
  const auto settled = d.mirror.stats();
  CHECK(settled.primes == 1);
  CHECK(settled.starved == 0);
  CHECK(settled.resyncs == 0);
  Continuity continuity{d.mirror};
  double ratio_sum = 0;
  std::uint64_t ratio_samples = 0;
  std::uint32_t low = ~0u, high = 0;
  d.run(120, [&](const float* out, std::uint32_t frames) {
    continuity(out, frames);
    ratio_sum += d.mirror.ratio();
    ++ratio_samples;
    low = std::min(low, d.mirror.queued());
    high = std::max(high, d.mirror.queued());
  });
  const auto stats = d.mirror.stats();
  // Two minutes of drift without a glitch: no starving, resync or drop.
  CHECK(stats.primes == 1);
  CHECK(stats.starved == 0);
  CHECK(stats.resyncs == 0);
  CHECK(stats.dropped == 0);
  CHECK(continuity.checked > 4'000'000);
  CHECK(continuity.broken == 0);
  // The ratio settles on the clocks' ratio: input frames per output frame.
  const double expected = (1 + headset_ppm * 1e-6) / (1 + pc_ppm * 1e-6);
  CHECK(ratio_sum / ratio_samples == doctest::Approx(expected).epsilon(30e-6));
  // The queue holds near the target: a few blocks either side, never empty.
  CHECK(low > 480);
  CHECK(high < d.mirror.target() + 4 * 480);
}
}  // namespace

TEST_CASE("PC output: the setting's value and the plan for a route") {
  CHECK(pc_audio_enabled("on"));
  CHECK(pc_audio_enabled("1"));
  CHECK_FALSE(pc_audio_enabled("off"));
  CHECK_FALSE(pc_audio_enabled(""));
  CHECK_FALSE(pc_audio_enabled("yes"));
  CHECK(plan_pc_output(false, true, 0, false) == PcOutputPlan::off);
  CHECK(plan_pc_output(true, true, 0, false) == PcOutputPlan::play);
  // The game on the system default already, or the headset being it: play once.
  CHECK(plan_pc_output(true, false, -1, false) == PcOutputPlan::not_on_headset);
  CHECK(plan_pc_output(true, true, 1, false) == PcOutputPlan::same_endpoint);
  // An unidentified default could be the headset: no second copy.
  CHECK(plan_pc_output(true, true, -1, false) == PcOutputPlan::default_unknown);
  CHECK(plan_pc_output(true, true, 0, true) == PcOutputPlan::dropped);
}

TEST_CASE("PC output: an inactive mirror takes nothing from the headset mix") {
  OutputMirror mirror;
  const float frames[4] = {0.5f, 0.5f, 0.5f, 0.5f};
  mirror.push(frames, 2);
  CHECK(mirror.queued() == 0);
  CHECK(mirror.stats().pushed == 0);
}

TEST_CASE("PC output: the PC clock running slow or fast against the headset's is absorbed") {
  SUBCASE("headset 300 ppm fast") { drift_case(300, 0); }
  SUBCASE("PC 450 ppm fast") { drift_case(0, 450); }
  SUBCASE("both off, 2000 ppm apart") { drift_case(-1000, 1000); }
}

TEST_CASE("PC output: a duplicated block is the headset's mix, delayed by the target") {
  Devices d(0);
  const std::uint32_t target = 1440;
  d.mirror.activate(target);
  // Silence while priming, then the ramp from where it was target frames ago.
  std::uint64_t silent = 0, sound = 0;
  bool delay_checked = false;
  d.run(1, [&](const float* out, std::uint32_t frames) {
    for (std::uint32_t i = 0; i < frames; ++i) {
      if (out[2 * i] == 0 && !sound) {
        ++silent;
        continue;
      }
      if (!sound++ && !delay_checked) {
        // The first played frame is the one queued target (+ 3 interpolation) frames before the newest.
        CHECK(out[2 * i] == doctest::Approx(ramp(d.produced - target - 3)).epsilon(1e-5));
        delay_checked = true;
      }
    }
  });
  CHECK(delay_checked);
  CHECK(silent < target + 1040);
  CHECK(d.mirror.stats().starved == 0);
}

TEST_CASE("PC output: a stalled headset leaves silence, and the copy resumes at the target delay") {
  Devices d(0);
  d.mirror.activate(1920);
  d.run(2);
  d.headset_running = false;
  bool silent_tail = false;
  d.run(0.5, [&](const float* out, std::uint32_t frames) {
    silent_tail = std::all_of(out, out + std::size_t(frames) * 2, [](float v) { return v == 0; });
  });
  CHECK(silent_tail);
  CHECK(d.mirror.stats().starved == 1);
  d.headset_running = true;
  Continuity continuity{d.mirror};
  d.run(1);  // primes again
  d.run(5, std::ref(continuity));
  CHECK(d.mirror.stats().primes == 2);
  CHECK(d.mirror.stats().starved == 1);
  CHECK(continuity.broken == 0);
}

TEST_CASE("PC output: a stalled PC output never holds up the headset, and restarts from fresh samples") {
  Devices d(0, 0, 4800);
  d.mirror.activate(1920);
  d.run(1);
  d.pc_running = false;
  // The headset keeps pushing into a full queue: what does not fit is dropped.
  d.run(1);
  CHECK(d.mirror.queued() == d.mirror.capacity);
  CHECK(d.mirror.stats().dropped > 40000);
  d.pc_running = true;
  // The backlog is stale: the PC output starts again from fresh samples.
  bool resumed = false;
  d.run(0.2, [&](const float* out, std::uint32_t frames) {
    for (std::uint32_t i = 0; i < frames && !resumed; ++i)
      if (out[2 * i] != 0) {
        CHECK(out[2 * i] == doctest::Approx(ramp(d.produced - d.mirror.target() - 3)).epsilon(1e-5));
        resumed = true;
      }
  });
  CHECK(resumed);
  CHECK(d.mirror.stats().resyncs == 1);
  CHECK(d.mirror.stats().primes == 2);
  CHECK(d.mirror.queued() < d.mirror.target() + 1000);
}

TEST_CASE("PC output: reactivation primes again from the newest samples") {
  Devices d(0);
  d.mirror.activate(1920);
  d.run(1);
  // The PC device closes; the headset pushes nothing meanwhile.
  d.pc_running = false;
  d.mirror.deactivate();
  const auto pushed = d.mirror.stats().pushed;
  d.run(0.2);
  CHECK(d.mirror.stats().pushed == pushed);
  d.mirror.activate(960);
  d.pc_running = true;
  d.run(1);
  CHECK(d.mirror.stats().starved == 0);
  CHECK(d.mirror.stats().primes == 2);
  CHECK(d.mirror.target() == 960);
}
