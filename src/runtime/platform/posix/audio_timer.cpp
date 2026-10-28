#include "platform/audio_timer.h"
#include <thread>

namespace venture::platform {
AudioTimer::AudioTimer() = default;
AudioTimer::~AudioTimer() = default;
void AudioTimer::wait(std::chrono::nanoseconds interval) {
  std::this_thread::sleep_for(interval);
}
}
