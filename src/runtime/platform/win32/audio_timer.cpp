#include "platform/audio_timer.h"
#include "common/log.h"
#include <windows.h>
#include <stdexcept>

namespace venture::platform {
AudioTimer::AudioTimer() {
  // Microsoft CreateWaitableTimerExW: HIGH_RESOLUTION (0x2) is supported
  // since Windows 10 1803 and avoids millisecond waits rounding to a clock tick.
  // https://learn.microsoft.com/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw
  timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0x2,
                                TIMER_MODIFY_STATE | SYNCHRONIZE);
  if (!timer_ && GetLastError() == ERROR_INVALID_PARAMETER) {
    log::warn("audio", "High-resolution wake timers unavailable; using the system timer cadence");
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0,
                                  TIMER_MODIFY_STATE | SYNCHRONIZE);
  }
  if (!timer_) throw std::runtime_error("Cannot create audio wake timer");
}
AudioTimer::~AudioTimer() { if (timer_) CloseHandle(timer_); }
void AudioTimer::wait(std::chrono::nanoseconds interval) {
  const auto ns = interval.count();
  if (ns <= 0) return;
  LARGE_INTEGER due;
  // SetWaitableTimer takes negative relative times in 100 ns units.
  due.QuadPart = -(ns / 100 + (ns % 100 != 0));
  if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE) ||
      WaitForSingleObject(timer_, INFINITE) != WAIT_OBJECT_0)
    throw std::runtime_error("Cannot wait on audio wake timer");
}
}
