#pragma once
#include "xr/api.h"
namespace venture::xr {
// Developer-only. Off unless VENTURE_XR_DUMP_DIR is set. GPU capture happens while
// waited images are still application-owned, never after release or EndFrame.
bool dev_dump_enabled();
void dev_dump_bind(const XrGraphicsBindingVulkanKHR&);
XrResult dev_create_swapchain(XrSession, const XrSwapchainCreateInfo*, XrSwapchain*);
XrResult dev_destroy_swapchain(XrSwapchain);
XrResult dev_release_swapchain(XrSwapchain, const XrSwapchainImageReleaseInfo*, VkImage);
void dev_dump_frame(const XrFrameEndInfo*, XrResult);
// Waits until every queued dump's images and layers.jsonl record are written.
void dev_dump_flush();
// Flushes dumps and closes the frame log; the session is ending.
void dev_dump_end();
}
