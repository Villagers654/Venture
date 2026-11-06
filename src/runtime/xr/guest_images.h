#pragma once
#include "xr/api.h"
#include "xr/xr_images.h"
#include <memory>
#include <span>

namespace venture::xr {
// Private guest backing. Copies use the session binding queue; a slot's fence
// is waited before returning it to the guest, reusing commands, or destruction.
class GuestImages {
public:
  // `staged`: the slots allocate no images; an XrImages provider fills
  // staged() (GLES rows, one image per layer) and copies read from it. Its
  // images() entries are null and only give the slot count.
  GuestImages(const XrGraphicsBindingVulkanKHR &, PFN_vkGetInstanceProcAddr,
              const XrSwapchainCreateInfo &, bool staged = false);
  ~GuestImages();
  std::span<const XrSwapchainImageVulkanKHR> images() const;
  std::span<StagedImage> staged();
  // Bytes of private image memory (zero when staged).
  std::uint64_t private_bytes() const;
  XrResult wait(std::uint32_t index, XrDuration timeout = XR_INFINITE_DURATION);
  // A destination extent other than the guest size (or {}) scales the base level.
  // flip_rows reverses rows (XR_FB_composition_layer_image_layout VERTICAL_FLIP).
  // Staged slots hold GLES rows; their copies turn them top-down first.
  XrResult copy(std::uint32_t index, VkImage destination,
                VkFormat destination_format, VkExtent2D destination_extent = {},
                bool flip_rows = false, std::int32_t source_layer = -1);
  XrResult drain();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace venture::xr
