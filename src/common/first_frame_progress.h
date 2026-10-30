#pragma once
#include <cstdint>

namespace venture::startup_diagnostics {
bool first_frame_submitted();
// Frames submitted to the host this process, across sessions.
void note_frame_submitted();
std::uint64_t frames_submitted();
// The host session's last XrSessionState; a game is expected to render only while focused.
void note_host_session_state(int state);
bool host_session_focused();
std::int64_t first_frame_timeout_seconds();
// Monitor-only, one-shot capture. An early stop also captures before teardown;
// reaching the bound requests evidence without cancelling a slow startup.
class FirstFrameTrigger {
  bool done_ = false;
 public:
  bool poll(std::int64_t elapsed_seconds, std::int64_t timeout_seconds, bool submitted, bool stopping) {
    if (done_ || timeout_seconds <= 0) return false;
    if (submitted) { done_ = true; return false; }
    if (!stopping && elapsed_seconds < timeout_seconds) return false;
    done_ = true;
    return true;
  }
};
// One-shot capture when submitted frames stop advancing mid-game (a freeze):
// the game is still running but its render loop has not produced a frame for
// `stall_seconds`. A paused (unfocused) or quitting game is excluded by
// `stopping`, which also restarts the wait.
class FrameStallTrigger {
  bool done_ = false;
  std::uint64_t frames_ = 0;
  std::int64_t since_ = 0;
 public:
  static constexpr std::int64_t stall_seconds = 5;
  bool poll(std::int64_t now_seconds, std::uint64_t frames, bool stopping) {
    if (stopping) since_ = now_seconds;
    if (done_ || stopping || !frames) return false;
    if (frames != frames_) { frames_ = frames; since_ = now_seconds; return false; }
    if (now_seconds - since_ < stall_seconds) return false;
    done_ = true;
    return true;
  }
};
}
