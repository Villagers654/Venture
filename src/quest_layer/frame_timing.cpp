// Frame timing for the Quest layer's records (frame_timing.h).
#include "quest_layer/frame_timing.h"

#include "quest_layer/records.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace venture::quest_layer {
namespace {
std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

FrameTiming::FrameTiming(std::uint32_t session_number) : number_(session_number), thread_([this] { run(); }) {}

FrameTiming::~FrameTiming() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
  drain();
  if (frames_) records::write(summary("ended"));
}

void FrameTiming::ended() {
  const auto n = written_.load(std::memory_order_relaxed);
  ring_[n % kRing] = {now_ns(), period_.load(std::memory_order_relaxed)};
  written_.store(n + 1, std::memory_order_release);
}

void FrameTiming::run() {
  auto next_summary = std::chrono::steady_clock::now() + std::chrono::minutes(1);
  std::unique_lock lock(mutex_);
  while (!stop_) {
    wake_.wait_for(lock, std::chrono::seconds(1), [this] { return stop_; });
    if (stop_) break;
    lock.unlock();
    drain();
    if (std::chrono::steady_clock::now() >= next_summary) {
      next_summary += std::chrono::minutes(1);
      if (frames_) records::write(summary("so far"));
    }
    lock.lock();
  }
}

void FrameTiming::drain() {
  const auto written = written_.load(std::memory_order_acquire);
  if (written - read_ > kRing) {  // lapped: those frames are gone
    lost_ += written - read_ - kRing;
    read_ = written - kRing;
    last_end_ = 0;
  }
  for (; read_ < written; ++read_) {
    const auto frame = ring_[read_ % kRing];
    ++frames_;
    if (!first_end_) first_end_ = frame.end_ns;
    if (last_end_) {
      const auto interval = frame.end_ns - last_end_;
      ++intervals_;
      bins_[std::min<std::size_t>(static_cast<std::size_t>(std::max<std::int64_t>(interval, 0) / kBinNs), kBins - 1)]++;
      if (frame.period_ns > 0) {
        period_sum_ += frame.period_ns;
        ++periods_;
        // Each display period past the first one without a new frame.
        const auto periods = std::llround(static_cast<double>(interval) / static_cast<double>(frame.period_ns));
        if (periods > 1) dropped_ += static_cast<std::uint64_t>(periods - 1);
      }
    }
    last_end_ = frame.end_ns;
  }
}

std::string FrameTiming::summary(const char* when) const {
  const auto percentile = [&](double p) {
    if (!intervals_) return 0.0;
    const auto rank = static_cast<std::uint64_t>(std::ceil(p * static_cast<double>(intervals_)));
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < kBins; ++i) {
      seen += bins_[i];
      if (seen >= rank) return static_cast<double>((static_cast<std::int64_t>(i) + 1) * kBinNs) / 1e6;
    }
    return static_cast<double>(kBins * kBinNs) / 1e6;
  };
  const double seconds = static_cast<double>(last_end_ - first_end_) / 1e9;
  const double fps = seconds > 0 ? static_cast<double>(intervals_) / seconds : 0.0;
  char text[320];
  std::snprintf(text, sizeof text,
                "frame timing, session %u %s: %llu frames in %.1f s, %.1f fps average; frame time p95 %.2f ms, "
                "p99 %.2f ms; %llu dropped",
                number_, when, static_cast<unsigned long long>(frames_), seconds, fps, percentile(0.95),
                percentile(0.99), static_cast<unsigned long long>(dropped_));
  std::string line = text;
  if (period_sum_ && periods_) {
    std::snprintf(text, sizeof text, " (display %.1f Hz)",
                  1e9 / (static_cast<double>(period_sum_) / static_cast<double>(periods_)));
    line += text;
  }
  if (lost_) line += "; " + std::to_string(lost_) + " frames not counted";
  return line;
}
}  // namespace venture::quest_layer
