#pragma once
#include "xr/color_space.h"
#include <memory>
#include <span>

namespace venture::xr {
// Convert compositor-owned images in place, or copy application images into
// private presentation targets without modifying their pixels. Caller owns
// the XR images and serializes this object. CPU waits hold no queue lock.
class ColorImages {
public:
  enum class Alpha { preserve, premultiply, opaque };
  // Queue-ordered mode is only for native XR images whose producer/reuse is
  // synchronized with the session's binding VkQueue. Fences still guard this
  // object's reusable commands/descriptors and destruction.
  ColorImages(const XrGraphicsBindingVulkanKHR &, PFN_vkGetInstanceProcAddr,
              const XrSwapchainCreateInfo &,
              std::span<const XrSwapchainImageVulkanKHR>, bool binding_queue_ordered = false);
  ~ColorImages();
  XrResult convert(std::uint32_t index, const ColorMatrix &,
                   bool external_queue_held = false, Alpha alpha = Alpha::preserve, float camera_opacity = 1);
  XrResult restore(std::uint32_t index);
  // Copy and convert into a separate presentation image; never write source.
  XrResult convert_from(VkImage source, std::uint32_t index, const ColorMatrix &, bool external_queue_held = false,
                        Alpha alpha = Alpha::preserve, float camera_opacity = 1);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace venture::xr
