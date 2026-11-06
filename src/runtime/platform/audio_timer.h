#pragma once
#include <chrono>

namespace venture::platform {
// Per-worker wake timer. It never changes the system timer resolution.
class AudioTimer {
 public:
  AudioTimer();
  ~AudioTimer();
  AudioTimer(const AudioTimer&) = delete;
  AudioTimer& operator=(const AudioTimer&) = delete;
  void wait(std::chrono::nanoseconds interval);

 private:
#ifdef _WIN32
  void* timer_ = nullptr;  // a high-resolution waitable timer
#endif
};
}
