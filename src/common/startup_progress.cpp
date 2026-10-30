#include "common/startup_progress.h"
#include "common/first_frame_progress.h"
#include "common/environment.h"
#include <charconv>
#include <chrono>

namespace venture::startup_diagnostics {
namespace {
const std::int64_t seconds = [] {
  const auto* text = environment::getenv("VENTURE_STARTUP_STALL_SECONDS");
  if (!text) return std::int64_t{0};
  std::int64_t value = 0;
  const std::string_view input(text);
  const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
  return parsed.ec == std::errc{} && parsed.ptr == input.data() + input.size() && value > 0 && value <= 3600
      ? value : std::int64_t{0};
}();
Trigger trigger;
std::atomic<bool> submitted{false};
std::atomic<std::uint64_t> frames{0};
std::atomic<int> host_state{0};
std::int64_t now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
const bool enabled = seconds > 0;
Reason Trigger::poll(std::int64_t time, std::int64_t timeout) {
  const auto began = armed_.load();
  if (!began || fired_.load()) return Reason::none;
  const bool progressed = progress_.load();
  if (!progressed && time - began < timeout) return Reason::none;
  if (fired_.exchange(true)) return Reason::none;
  return progressed ? Reason::progress : Reason::stall;
}
void arm() {
  submitted.store(true, std::memory_order_relaxed);
  if (enabled) trigger.arm(now());
}
bool first_frame_submitted() { return submitted.load(std::memory_order_relaxed); }
void note_frame_submitted() { frames.fetch_add(1, std::memory_order_relaxed); }
std::uint64_t frames_submitted() { return frames.load(std::memory_order_relaxed); }
void note_host_session_state(int state) { host_state.store(state, std::memory_order_relaxed); }
bool host_session_focused() { return host_state.load(std::memory_order_relaxed) == 5; }  // XR_SESSION_STATE_FOCUSED
std::int64_t first_frame_timeout_seconds() {
  static const auto value = [] {
    const auto* text = environment::getenv("VENTURE_FIRST_FRAME_SNAPSHOT_SECONDS");
    if (!text) return std::int64_t{0};
    std::int64_t seconds = 0;
    const std::string_view input(text);
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), seconds);
    return parsed.ec == std::errc{} && parsed.ptr == input.data() + input.size() && seconds > 0 && seconds <= 3600
        ? seconds : std::int64_t{0};
  }();
  return value;
}
void progress() { if (enabled) trigger.progress(); }
Reason take_snapshot_reason() { return trigger.poll(now(), seconds * 1'000'000'000); }
std::int64_t timeout_seconds() { return seconds; }
}
