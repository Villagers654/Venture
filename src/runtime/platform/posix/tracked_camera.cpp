// SteamVR's camera interface is not offered on Linux: its camera support there
// is unverified, and the frame-buffer call is known to fail in Vulkan clients.
#include "xr/tracked_camera.h"

namespace venture::xr {
bool tracked_camera_present() { return false; }
std::unique_ptr<TrackedCamera> open_tracked_camera(unsigned) { return nullptr; }
} // namespace venture::xr
