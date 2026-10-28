#pragma once
#include <string_view>

namespace venture::signals {
// One-shot stall evidence: try the registry, copy last saved guest registers
// and bounded raw A64 FP chains, then release it before logging. No linker/name
// locks, signal delivery or wakeups. A32 retains PC/LR/SP (no EHABI lookup).
bool try_log_guest_threads_raw(std::string_view reason);
// Capture native state first, waiting for the supervisor's bounded attempt,
// then copy raw guest state. Call before posting lifecycle teardown.
bool capture_first_frame_stall();
}
