// The PC runtime's eye gaze (for its own foveated rendering) and performance
// overlay, which the shared XR session code calls, have nothing to do in a
// game on the headset: the headset's runtime foveates and the overlay draws
// with Venture's Vulkan device, which a native headset session does not have
// (stats_overlay.h: extend is null there).
#include "xr/eye_gaze.h"
#include "xr/stats_overlay.h"

namespace venture::xr::eye_gaze {
bool requested() { return false; }
bool available() { return false; }
XrActionSet action_set(XrInstance) { return XR_NULL_HANDLE; }
void add_binding(XrInstance, std::string_view, std::vector<XrActionSuggestedBinding>&) {}
void before_attach(XrInstance) {}
void attached(XrSession) {}
void forget_session(XrSession) {}
std::optional<XrVector3f> direction(XrTime) { return std::nullopt; }
}  // namespace venture::xr::eye_gaze

namespace venture::xr::stats_overlay {
void set_enabled(bool) {}
std::shared_ptr<const Frame> extend(XrSession, const XrFrameEndInfo&, std::shared_ptr<const void>) { return nullptr; }
void reset_session(XrSession) {}
}  // namespace venture::xr::stats_overlay
