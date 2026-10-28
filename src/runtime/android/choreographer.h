#pragma once
#include <cstdint>
namespace venture::android {
// Display/XR calls this with CLOCK_MONOTONIC-compatible timestamps and its
// display period. Before host timing is known, the shared XR default is used.
// Guest callbacks always execute on the owning ALooper's guest thread.
void display_vsync(std::int64_t frame_time_ns, std::int64_t period_ns);
}  // namespace venture::android
