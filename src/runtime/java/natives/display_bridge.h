#pragma once
#include <cstdint>
namespace venture::java {
bool update_framework_display_period(std::int64_t);
// Supply logical mirror geometry and actual refresh. Updates are posted to the
// main Looper. host_vsync=true waits for dispatch_framework_vsync(); otherwise
// a monotonic timer supports flat/debug mode. Sizes are not HMD eye textures.
bool configure_framework_display(std::int32_t width, std::int32_t height,
                                 std::int32_t density_dpi, float refresh_hz,
                                 bool host_vsync);
bool dispatch_framework_vsync(std::int64_t frame_time_ns);
bool dispatch_framework_xr_vsync(std::int64_t monotonic_time_ns, std::int64_t period_ns);
}  // namespace venture::java
