#pragma once
// The headset mix, copied to a second output (the PC's own, VENTURE_PC_AUDIO).
//
// The headset device's callback push()es each finished 48 kHz stereo block;
// the PC device's callback pull()s on its own clock. The two endpoints'
// clocks drift apart (a few hundred ppm), so the PC side resamples by a ratio
// a fill-level controller keeps within +-0.5 % of 1, holding the queue near a
// target delay. A starved queue plays silence and refills to the target; an
// overfull one skips to it, and one that overflowed (the PC device stalled)
// is stale and refills with fresh samples. Single producer, single consumer, lock-free, no
// allocation after construction: the headset side copies and returns, never
// waiting for or depending on the PC side.
#include <atomic>
#include <cstdint>
#include <string_view>
#include <vector>

namespace venture::audio {
// VENTURE_PC_AUDIO: "on" (or "1") plays the headset mix on the PC too.
inline bool pc_audio_enabled(std::string_view value) { return value == "on" || value == "1"; }
// What the PC output does for the current headset route.
enum class PcOutputPlan {
  off,              // the setting is off
  not_on_headset,   // the game plays on the system default already
  same_endpoint,    // the headset endpoint is the system default: play once
  default_unknown,  // the system default output could not be identified
  dropped,          // the PC output failed earlier in this process
  play,             // the mix also plays on the system default output
};
// default_is_headset: 1 the system default output is the headset endpoint,
// 0 it is another endpoint, -1 unknown.
PcOutputPlan plan_pc_output(bool enabled, bool on_headset, int default_is_headset, bool dropped);
const char* describe(PcOutputPlan plan);

class OutputMirror {
 public:
  static constexpr std::uint32_t channels = 2;
  static constexpr double max_ratio_offset = 0.005;
  explicit OutputMirror(std::uint32_t capacity_frames = 24000);
  // Control thread, while the PC device is stopped. target: the queue delay
  // the controller holds, in frames; activation (re)primes the consumer.
  void activate(std::uint32_t target_frames);
  void deactivate();
  bool active() const { return active_.load(std::memory_order_relaxed); }
  // Headset device thread: copies what fits, drops the rest.
  void push(const float* frames, std::uint32_t count);
  // PC device thread: fills count frames, silence while priming or starved.
  void pull(float* out, std::uint32_t count);
  // Consumer state, safe to read anywhere (relaxed copies).
  double ratio() const { return ratio_out_.load(std::memory_order_relaxed); }
  std::uint32_t queued() const;
  std::uint32_t target() const { return target_.load(std::memory_order_relaxed); }
  struct Stats {
    std::uint64_t pushed = 0, dropped = 0, starved = 0, resyncs = 0, primes = 0;
  };
  Stats stats() const;
  const std::uint32_t capacity;

 private:
  bool pop(float* frame);  // consumer: one frame from the queue
  std::vector<float> data_;
  std::atomic<bool> active_{false};
  std::atomic<std::uint32_t> target_{1920};
  std::atomic<std::uint64_t> generation_{0};
  alignas(64) std::atomic<std::uint64_t> write_{0};
  alignas(64) std::atomic<std::uint64_t> read_{0};
  std::atomic<std::uint64_t> pushed_{0}, dropped_{0}, starved_{0}, resyncs_{0}, primes_{0};
  std::atomic<double> ratio_out_{1};
  // Consumer only.
  alignas(64) std::uint64_t seen_generation_ = ~0ull, seen_dropped_ = 0;
  bool primed_ = false;
  float window_[4][channels]{};  // x[-1], x[0], x[1], x[2] around the read position
  double phase_ = 0;             // between x[0] and x[1]
  double fill_ = 0, ratio_ = 1;
};
}  // namespace venture::audio
