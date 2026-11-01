#pragma once
#include <cstdint>
#include <string_view>

namespace venture::signals {
// One-shot stall evidence: try the registry, copy last saved guest registers
// and bounded raw A64 FP chains, then release it before logging. No linker/name
// locks, signal delivery or wakeups. A32 retains PC/LR/SP (no EHABI lookup).
// With symbols, frames are then named through a non-blocking linker lookup.
bool try_log_guest_threads_raw(std::string_view reason, bool symbols = false);
// Capture native state first, waiting for the supervisor's bounded attempt,
// then copy raw guest state; without VENTURE_FIRST_FRAME_SNAPSHOT_SECONDS only
// named guest frames are logged. Call before posting lifecycle teardown.
bool capture_first_frame_stall();
// The same evidence, with named frames, when frames stop mid-game (a freeze);
// the native dump (which suspends the process) only when native_dump is set.
bool capture_frame_stall(std::int64_t seconds, bool native_dump);
}
