#pragma once
#include "xr/api.h"
#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace venture::xr {
// Draws equirect layers into the eyes of a private host projection chain, for
// hosts without XR_KHR_composition_layer_equirect(2). Sources are private
// single-layer copies of the guest images; the caller fills them (GuestImages)
// on the binding queue before render(). Caller serializes this object.
class SphereImages {
public:
  // `target`: the host chain's create info (two array layers, one per eye).
  SphereImages(const XrGraphicsBindingVulkanKHR &, PFN_vkGetInstanceProcAddr, const XrSwapchainCreateInfo &target,
               std::span<const XrSwapchainImageVulkanKHR> images);
  ~SphereImages();
  // The private image for one source, created or resized as needed; `fresh`
  // until a copy into it was reported with filled(). Unfilled sources are not drawn.
  XrResult source(std::uintptr_t key, VkFormat, VkExtent2D, VkImage &image, bool &fresh);
  void filled(std::uintptr_t key);
  // Frees the sources not named here.
  void retain(std::span<const std::uintptr_t> keys);
  // sphere.frag push constants, in its order.
  using Constants = std::array<float, 32>;
  struct Draw {
    std::uintptr_t source;
    std::uint32_t eye;
    Constants constants;
  };
  // Clears both eyes of host image `index`, then draws in order; the target
  // ends in COLOR_ATTACHMENT_OPTIMAL for its release on the binding queue.
  XrResult render(std::uint32_t index, std::span<const Draw>);
  const XrSwapchainCreateInfo &target() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace venture::xr
