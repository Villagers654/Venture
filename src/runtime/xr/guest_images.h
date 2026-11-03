#pragma once
#include "xr/api.h"
#include <memory>
#include <span>

namespace venture::xr {
// Private guest backing. Copies use the session binding queue; a slot's fence
// is waited before returning it to the guest, reusing commands, or destruction.
class GuestImages {
public:
  GuestImages(const XrGraphicsBindingVulkanKHR &, PFN_vkGetInstanceProcAddr,
              const XrSwapchainCreateInfo &);
  ~GuestImages();
  std::span<const XrSwapchainImageVulkanKHR> images() const;
  XrResult wait(std::uint32_t index, XrDuration timeout = XR_INFINITE_DURATION);
  // A destination extent other than the guest size (or {}) scales the base level.
  // flip_rows reverses rows (XR_FB_composition_layer_image_layout VERTICAL_FLIP).
  XrResult copy(std::uint32_t index, VkImage destination,
                VkFormat destination_format, VkExtent2D destination_extent = {},
                bool flip_rows = false, std::int32_t source_layer = -1);
  XrResult drain();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace venture::xr
