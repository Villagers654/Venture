#pragma once
// In-headset performance overlay: a head-locked OpenXR quad (1160x100) above
// the view with the headset and game rates, a rate graph, the eye size sent to
// the headset and the one the game renders, the frame preparation time, and the
// PC's CPU, GPU, RAM and VRAM load (NVML on NVIDIA, else PDH). Windows only;
// elsewhere every call is a no-op.
//
// VENTURE_STATS_OVERLAY=on (the launcher's "Performance overlay" setting, off
// by default) shows it from the start. The manual-reset event
// Local\VentureStatsOverlay-<runtime pid> shows (set) or hides (reset) it
// during play, for tools outside the launcher. The panel is painted and the PC
// load sampled on a worker thread; the render thread only uploads a new panel
// twice a second through a small ring of buffers, never waiting for the GPU.
#include "xr/api.h"

namespace venture::xr::stats_overlay {
// Shown now: one atomic read (a watcher thread follows the event). Callers skip
// all overlay work, timing included, while it is false.
bool active();
// xrEndFrame, after the guest's layers are mapped to the host: the overlay's
// quad for this frame, or null (off, or not ready). `display_time` paces the
// headset rate. The returned layer stays valid until the next call.
const XrCompositionLayerBaseHeader* layer(XrSession host_session, const XrGraphicsBindingVulkanKHR& binding,
                                          PFN_vkGetInstanceProcAddr get, XrTime display_time);
// The guest frame's preparation (copies, conversions) on the CPU, in milliseconds.
void note_prepare(double milliseconds);
// The size the guest renders one eye at, and the size sent to the headset for
// one eye.
void note_render_size(std::uint32_t width, std::uint32_t height, std::uint32_t output_width,
                      std::uint32_t output_height);
}  // namespace venture::xr::stats_overlay
