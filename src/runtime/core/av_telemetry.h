#pragma once
#include <nlohmann/json.hpp>

namespace venture::av {
// Call from non-realtime threads only. Bounded queue-only admission; all
// filesystem work is performed by the process-wide writer thread.
bool enabled();
void emit(nlohmann::json event) noexcept;
// Stop admission and drain queued events to the OS before kernel exit.
// Non-realtime only; bounded externally by post-exit supervision.
void finish() noexcept;
}
