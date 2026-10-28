#include "platform/timed_wait.h"
#include "core/error.h"
#include <chrono>
#include <condition_variable>

namespace venture::platform {
std::int64_t steady_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct TimedWait::State { std::condition_variable cv; };
TimedWait::TimedWait() : state_(std::make_unique<State>()) {}
TimedWait::~TimedWait() = default;
int TimedWait::wait_until(std::unique_lock<std::mutex>& lock, std::int64_t deadline, bool) {
  const auto end = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(deadline));
  return state_->cv.wait_until(lock, end) == std::cv_status::timeout ? -err::kETIMEDOUT : 0;
}
void TimedWait::notify() { state_->cv.notify_one(); }
}
