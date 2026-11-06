// Resolution and frame rate overrides from the launcher's Display settings.
// By default games render at the host runtime's recommended size and run at
// its rate; VENTURE_RESOLUTION and VENTURE_FRAME_RATE opt into others:
//   VENTURE_RESOLUTION = runtime | <width>x<height> | <percent>%
//     A size is per eye; a percent scales the host's recommended pixel count
//     (each side by its square root), like SteamVR's render resolution.
//     Both are clamped to the host's maximum image size.
//   VENTURE_FRAME_RATE = runtime | <hertz>
//     A host with XR_FB_display_refresh_rate switches to a rate it lists and
//     keeps its own for any other. A host without it keeps its rate and the
//     game is paced to at most this one.
//   VENTURE_MIRROR = cropped | full | off
//     The desktop mirror window (xr/desktop_mirror.h): one eye cropped to
//     fill the window, the whole eye letterboxed, or no window. Unset is
//     off: the launcher passes its setting (default cropped); harness and
//     direct runtime launches get no window.
//   VENTURE_MIRROR_EYE = left | right
//     The eye the mirror shows; unset or anything else is the left.
#pragma once
#include <openxr/openxr.h>
#include <vulkan/vulkan.h>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace venture::xr {
struct DisplayOverride {
  // Per-eye size; zero keeps the host's recommendation.
  std::uint32_t width = 0, height = 0;
  // Fraction of the host's recommended pixel count; zero keeps it.
  double pixel_scale = 0;
  // Frame rate in hertz; zero keeps the host's.
  float rate = 0;
  bool resizes() const { return (width && height) || pixel_scale > 0; }
};
// Parses the two settings; anything malformed or out of range keeps the default.
DisplayOverride parse_display_override(std::string_view resolution, std::string_view rate);
// The override from the environment, read on each call.
DisplayOverride display_override();
// The recommended size the override gives for one host view, clamped to its maximum.
XrExtent2Di override_view_size(const DisplayOverride& o, const XrViewConfigurationView& host);
// Applies the override to host views of a primary stereo (or mono) configuration in place.
void apply_display_override(XrViewConfigurationView* views, std::uint32_t count);
// The rate a host with XR_FB_display_refresh_rate switches to, from the rates it
// lists; nullopt keeps the host's own (no override, or a rate it does not list).
std::optional<float> supported_override_rate(float rate, const float* rates, std::uint32_t count);

enum class MirrorMode { off, cropped, full };
MirrorMode parse_mirror_mode(std::string_view value);
// VENTURE_MIRROR, read once per process.
MirrorMode mirror_mode();
// The mirrored view's index in a projection: 0 left, 1 right.
std::uint32_t parse_mirror_eye(std::string_view value);
// VENTURE_MIRROR_EYE, read once per process.
std::uint32_t mirror_eye();
// The view the mirror shows of a projection with `view_count` views: the chosen
// eye, or the only view of a mono one.
inline std::uint32_t mirror_view(std::uint32_t view_count, std::uint32_t eye) { return eye < view_count ? eye : 0; }
// Host Vulkan extensions a mirror window presents with: VK_KHR_surface and the
// platform's surface (Win32, Wayland) on the instance, VK_KHR_swapchain on the
// device. Empty when the mirror is off.
const std::vector<const char*>& mirror_instance_extensions();
const char* mirror_device_extension();
// Graphics devices and instances created with those extensions enabled.
void note_mirror_capable(const void* handle);
bool mirror_capable(const void* handle);
// The mirror's private present queue, added to a device as it is created
// while the mirror is on: one more queue in a graphics family the device
// already uses, else in another graphics or compute family with one to
// spare, at the lowest priority. Without one the mirror stays off for that
// device. Presenting there, a present that blocks (a locked or hidden
// desktop) holds no queue the game uses.
class MirrorQueueRequest {
 public:
  // Rewrites `info`'s queue list (kept alive by this object) to add the queue.
  void add(std::span<const VkQueueFamilyProperties> families, VkDeviceCreateInfo& info);
  // Once the device exists.
  void created(VkDevice device) const;

 private:
  std::vector<VkDeviceQueueCreateInfo> queues_;
  std::vector<float> priorities_;
  std::optional<std::pair<std::uint32_t, std::uint32_t>> queue_;
};
// Family and index of a device's mirror queue.
std::optional<std::pair<std::uint32_t, std::uint32_t>> mirror_queue(VkDevice device);
void forget_mirror_queue(VkDevice device);
} // namespace venture::xr
