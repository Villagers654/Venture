#pragma once
// In-headset performance overlay, after RVR's statistics overlay: a head-locked
// OpenXR quad (1160x100) with the headset and game rates, a rate graph, VRS / NR
// state, the render size and frame preparation time, and the PC's CPU, GPU,
// RAM and VRAM. Windows only; elsewhere every call is a no-op.
//
// VENTURE_STATS_OVERLAY=1 shows it from the start; the launcher switches it
// during play through the event Local\VentureStatsOverlay-<runtime pid>.
// VRS is green while it shades (hle/vulkan vrs_active), NR while the mode's NR is on (xr/nr_toggle.h).
#include "xr/api.h"

namespace venture::xr::stats_overlay {
// Shown now: one atomic read (a watcher thread follows the event). Callers skip
// all overlay work, timing included, while it is false. The panel is painted
// and the PC load sampled on a worker thread; the render thread only uploads a
// new panel twice a second, never waiting for the GPU.
bool active();
// xrEndFrame, after the guest's layers are mapped to the host: the overlay's
// quad for this frame, or null (off, or not ready). `display_time` paces the
// headset rate. The returned layer stays valid until the next call.
const XrCompositionLayerBaseHeader* layer(XrSession host_session, const XrGraphicsBindingVulkanKHR& binding,
                                          PFN_vkGetInstanceProcAddr get, XrTime display_time);
// The guest frame's preparation (copies, Venture NR) on the CPU, in milliseconds.
void note_prepare(double milliseconds);
// The size the guest renders one eye at, and the size sent to the headset for one eye.
void note_render_size(std::uint32_t width, std::uint32_t height, std::uint32_t output_width,
                      std::uint32_t output_height);
}  // namespace venture::xr::stats_overlay
