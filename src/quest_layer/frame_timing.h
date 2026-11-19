#pragma once
// Frame timing for one session, for the Quest layer's records (records.h,
// docs/design/native-headsets.md, sections 9 and 10): average fps, frame time
// p95 and p99 and dropped frames, from the game's xrWaitFrame and xrEndFrame.
//
// The game's frame loop pays a timestamp and a ring-buffer write per frame
// (ended()) and an atomic store per xrWaitFrame (waited()). A thread of the
// layer drains the ring once a second into the session's histogram, writes a
// summary of the session so far every minute (games are often closed without
// ending their session) and the final one when the session ends.
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace venture::quest_layer {
class FrameTiming {
public:
  explicit FrameTiming(std::uint32_t session_number);
  ~FrameTiming();  // drains, writes the final summary
  FrameTiming(const FrameTiming&) = delete;
  FrameTiming& operator=(const FrameTiming&) = delete;

  // xrWaitFrame returned with this predicted display period.
  void waited(std::int64_t period_ns) { period_.store(period_ns, std::memory_order_relaxed); }
  // xrEndFrame was called: one frame submitted.
  void ended();

private:
  struct Frame {
    std::int64_t end_ns;
    std::int64_t period_ns;
  };
  static constexpr std::uint64_t kRing = 4096;  // > 1 s of frames at any display rate
  // Frame times in 0.05 ms bins up to 250 ms; the last bin takes the rest.
  static constexpr std::int64_t kBinNs = 50'000;
  static constexpr std::size_t kBins = 5000;

  void run();
  void drain();
  std::string summary(const char* when) const;

  std::uint32_t number_;
  std::array<Frame, kRing> ring_{};
  std::atomic<std::uint64_t> written_{0};
  std::atomic<std::int64_t> period_{0};

  // The drain's state (its thread, or the destructor after it stopped).
  std::uint64_t read_ = 0, lost_ = 0;
  std::int64_t first_end_ = 0, last_end_ = 0, period_sum_ = 0;
  std::uint64_t frames_ = 0, intervals_ = 0, periods_ = 0, dropped_ = 0;
  std::array<std::uint32_t, kBins> bins_{};

  std::mutex mutex_;
  std::condition_variable wake_;
  bool stop_ = false;
  std::thread thread_;
};
}  // namespace venture::quest_layer
