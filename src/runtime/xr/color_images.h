#pragma once
#include "xr/color_space.h"
#include <array>
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
              std::span<const XrSwapchainImageVulkanKHR>, bool binding_queue_ordered = false, bool passthrough_alpha = false);
  ~ColorImages();
  // key (passthrough_alpha only): a linear-light color shown, opaque, wherever
  // the coverage after camera_opacity is below one half; the rest becomes the
  // opaque straight color. Nothing is blended with the key, so it stays exact.
  using Key = std::array<float, 3>;
  XrResult convert(std::uint32_t index, const ColorMatrix &,
                   bool external_queue_held = false, Alpha alpha = Alpha::preserve, float camera_opacity = 1,
                   const Key *key = nullptr);
  XrResult restore(std::uint32_t index);
  // Copy and convert into a separate presentation image; never write source.
  XrResult convert_from(VkImage source, std::uint32_t index, const ColorMatrix &, bool external_queue_held = false,
                        Alpha alpha = Alpha::preserve, float camera_opacity = 1, const Key *key = nullptr);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace venture::xr
