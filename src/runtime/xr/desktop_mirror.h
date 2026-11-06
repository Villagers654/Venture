// The desktop mirror: a normal resizable window showing one of the game's eyes
// (the left unless VENTURE_MIRROR_EYE picks the right), like the spectator
// window of a native PC VR game (Unreal's default SingleEyeCroppedToFill,
// Unity's left-eye mirror). VENTURE_MIRROR selects it (xr/display_override.h);
// off creates nothing.
//
// The render thread records one GPU blit from the host swapchain image it just
// filled (after the copy and color conversion, before xrReleaseSwapchainImage,
// on the binding queue) straight into a desktop swapchain image the mirror
// thread acquired beforehand and whose acquire already completed. Nothing is
// read back to the CPU and no image is allocated besides the window's own
// swapchain. The mirror thread owns the window, acquires, paces to the
// desktop refresh rate and presents with MAILBOX or IMMEDIATE on a private
// low-priority queue, so the XR loop never waits for the desktop: a present
// that blocks (a locked or hidden desktop) holds no queue the game uses, and
// without an acquired image ready a frame is simply not mirrored.
//
// Closing the window hides the mirror for the rest of the run; the game keeps
// running. A minimized window costs nothing.
#pragma once
#include "xr/api.h"

namespace venture::xr {
struct MirrorSource {
  VkImage image = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  std::uint32_t layer = 0;
  // The eye's pixels in `image`, row 0 at the top (what the host samples).
  XrRect2Di rect{};
  // The eye's optical axis within `rect`, 0..1 from its left and top edges;
  // a cropped mirror centres on it.
  float axis_x = 0.5f, axis_y = 0.5f;
};
// The optical axis of a view with this field of view.
void mirror_axis(const XrFovf& fov, float& x, float& y);
// True while the mirror takes a frame now: an acquired window image waits, or
// the window has yet to open. Cheap; false whenever the mirror is off, closed
// or blocked.
bool mirror_ready();
// Render thread, with `source.image` in COLOR_ATTACHMENT_OPTIMAL (an acquired
// and filled color swapchain image, single sample) and no Vulkan work of its
// own pending. Never blocks on the window. The first call starts the window.
void mirror_capture(const XrGraphicsBindingVulkanKHR& binding, bool shared_queue, const MirrorSource& source);
} // namespace venture::xr
