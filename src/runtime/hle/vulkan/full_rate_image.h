#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <vulkan/vulkan.h>

namespace venture::vulkan {
// Layout of a runtime-owned identity map, independent of the guest's chosen
// density/shading-rate API. Ordinary guest images have no entry.
std::optional<VkImageLayout> full_rate_image_layout(VkDevice, VkImage);
// Immutable compatibility image for applications that require a density-map
// handle even when rendering at full resolution. It contains (1,1) density.
class FullRateImage {
public:
  FullRateImage(VkPhysicalDevice, VkDevice, std::uint32_t family,
                std::uint32_t queue_index, std::uint32_t layers,
                std::uint32_t eye_width = 1, std::uint32_t eye_height = 1);
  ~FullRateImage();
  VkImage image() const;
  std::uint32_t width() const;
  std::uint32_t height() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
