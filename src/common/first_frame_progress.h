#pragma once
#include <cstdint>

namespace venture::startup_diagnostics {
bool first_frame_submitted();
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
}
