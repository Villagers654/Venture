#include "audio/output_mirror.h"

#include <algorithm>
#include <cstring>

namespace venture::audio {
PcOutputPlan plan_pc_output(bool enabled, bool on_headset, int default_is_headset, bool dropped) {
  if (!enabled) return PcOutputPlan::off;
  if (dropped) return PcOutputPlan::dropped;
  if (!on_headset) return PcOutputPlan::not_on_headset;
  if (default_is_headset < 0) return PcOutputPlan::default_unknown;
  if (default_is_headset > 0) return PcOutputPlan::same_endpoint;
  return PcOutputPlan::play;
}
const char* describe(PcOutputPlan plan) {
  switch (plan) {
    case PcOutputPlan::off: return "off";
    case PcOutputPlan::not_on_headset: return "off, the game plays on the system default output";
    case PcOutputPlan::same_endpoint: return "off, the headset endpoint is the system default output";
    case PcOutputPlan::default_unknown: return "off, the system default output could not be identified";
    case PcOutputPlan::dropped: return "off, it failed earlier";
    case PcOutputPlan::play: return "the headset mix also plays on the system default output";
  }
  return "";
}

OutputMirror::OutputMirror(std::uint32_t capacity_frames)
    : capacity(std::max<std::uint32_t>(capacity_frames, 1024)), data_(std::size_t(capacity) * channels) {}

void OutputMirror::activate(std::uint32_t target_frames) {
  // Room for the controller to hold the target and absorb a burst either way.
  target_.store(std::clamp<std::uint32_t>(target_frames, 256, capacity / 2), std::memory_order_relaxed);
  generation_.fetch_add(1, std::memory_order_release);
  active_.store(true, std::memory_order_release);
}
void OutputMirror::deactivate() { active_.store(false, std::memory_order_release); }

void OutputMirror::push(const float* frames, std::uint32_t count) {
  if (!active_.load(std::memory_order_relaxed) || !count) return;
  const auto write = write_.load(std::memory_order_relaxed);
  const auto read = read_.load(std::memory_order_acquire);
  const auto room = capacity - std::uint32_t(write - read);
  const auto n = std::min(count, room);
  const auto at = std::uint32_t(write % capacity);
  const auto first = std::min(n, capacity - at);
  std::memcpy(&data_[std::size_t(at) * channels], frames, std::size_t(first) * channels * sizeof(float));
  std::memcpy(data_.data(), frames + std::size_t(first) * channels, std::size_t(n - first) * channels * sizeof(float));
  write_.store(write + n, std::memory_order_release);
  pushed_.fetch_add(n, std::memory_order_relaxed);
  if (n < count) dropped_.fetch_add(count - n, std::memory_order_relaxed);
}

std::uint32_t OutputMirror::queued() const {
  const auto read = read_.load(std::memory_order_acquire);
  return std::uint32_t(write_.load(std::memory_order_acquire) - read);
}

bool OutputMirror::pop(float* frame) {
  const auto read = read_.load(std::memory_order_relaxed);
  if (read == write_.load(std::memory_order_acquire)) return false;
  std::memcpy(frame, &data_[std::size_t(read % capacity) * channels], channels * sizeof(float));
  read_.store(read + 1, std::memory_order_release);
  return true;
}

void OutputMirror::pull(float* out, std::uint32_t count) {
  if (const auto generation = generation_.load(std::memory_order_acquire); generation != seen_generation_) {
    seen_generation_ = generation;
    seen_dropped_ = dropped_.load(std::memory_order_relaxed);
    primed_ = false;
  }
  const double target = target_.load(std::memory_order_relaxed);
  auto queue = queued();
  if (const auto dropped = dropped_.load(std::memory_order_relaxed); dropped != seen_dropped_) {
    // The queue overflowed while this device stalled: all of it predates the
    // samples the headset dropped. Start again from fresh ones.
    seen_dropped_ = dropped;
    read_.store(read_.load(std::memory_order_relaxed) + queue, std::memory_order_release);
    queue = 0;
    primed_ = false;
    resyncs_.fetch_add(1, std::memory_order_relaxed);
  } else if (primed_ && queue > 2 * target) {
    // Far above the target (a stall short of overflowing): skip back to it.
    primed_ = false;
    resyncs_.fetch_add(1, std::memory_order_relaxed);
  }
  if (!primed_) {
    // Wait for the target delay, then start from it: older frames are stale.
    if (queue < target + 3) {
      std::fill_n(out, std::size_t(count) * channels, 0.0f);
      return;
    }
    read_.store(read_.load(std::memory_order_relaxed) + (queue - std::uint32_t(target) - 3), std::memory_order_release);
    for (int i = 1; i < 4; ++i) pop(window_[i]);
    std::memcpy(window_[0], window_[1], sizeof window_[0]);
    phase_ = 0;
    fill_ = target;
    ratio_ = 1;
    primed_ = true;
    primes_.fetch_add(1, std::memory_order_relaxed);
    queue = queued();
  }
  // Fill level, smoothed over about half a second of output, sets the ratio:
  // more queued than the target consumes faster. A clock drift d settles
  // with the queue d / 0.02 of the target away from it (1 % at 200 ppm);
  // the loop's time constant is target / 960 seconds (2.5 s for 50 ms).
  fill_ += std::min(1.0, count / 24000.0) * (double(queue) - fill_);
  ratio_ = 1 + std::clamp(0.02 * (fill_ - target) / target, -max_ratio_offset, max_ratio_offset);
  ratio_out_.store(ratio_, std::memory_order_relaxed);
  for (std::uint32_t i = 0; i < count; ++i) {
    // Catmull-Rom between x[0] and x[1]: transparent at ratios this close to 1.
    const double t = phase_;
    for (std::uint32_t c = 0; c < channels; ++c) {
      const double x0 = window_[0][c], x1 = window_[1][c], x2 = window_[2][c], x3 = window_[3][c];
      out[std::size_t(i) * channels + c] =
          float(x1 + 0.5 * t * (x2 - x0 + t * (2 * x0 - 5 * x1 + 4 * x2 - x3 + t * (3 * (x1 - x2) + x3 - x0))));
    }
    phase_ += ratio_;
    while (phase_ >= 1) {
      phase_ -= 1;
      std::memmove(window_[0], window_[1], sizeof window_[0] * 3);
      if (!pop(window_[3])) {
        // Starved: silence, then refill to the target before playing again.
        std::fill_n(out + std::size_t(i + 1) * channels, std::size_t(count - i - 1) * channels, 0.0f);
        primed_ = false;
        starved_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
  }
}

OutputMirror::Stats OutputMirror::stats() const {
  return {pushed_.load(std::memory_order_relaxed), dropped_.load(std::memory_order_relaxed),
          starved_.load(std::memory_order_relaxed), resyncs_.load(std::memory_order_relaxed),
          primes_.load(std::memory_order_relaxed)};
}
}  // namespace venture::audio
