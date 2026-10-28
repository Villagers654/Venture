#pragma once
#include <cstdint>

namespace mock {
// A terminal image must be queued once and written before a harness tears down
// the session. Poll writer completion while frames keep flowing: waiting for
// GPU copies on the frame thread can deadlock an application's pending submit.
class TerminalCapture {
 public:
  bool request(bool past_end, bool enabled, std::uint64_t frame) {
    if (!past_end || !enabled || frame_) return false;
    frame_ = frame;
    return true;
  }
  bool complete(bool past_end, bool enabled, bool writer_idle) const {
    return past_end && (!enabled || (frame_ && writer_idle));
  }
  bool pending() const { return frame_ != 0; }
  std::uint64_t frame() const { return frame_; }
 private:
  std::uint64_t frame_ = 0;
};
}
