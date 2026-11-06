#pragma once
#include "hle/openxr/state.h"

namespace venture::hle::openxr {
// CPU pixels into an internal guest swapchain (one made by create_swapchain with
// surface = true) on the session's Vulkan device: a mapped staging buffer, a
// buffer-to-image copy into image 0, and that image published as released, so
// ordinary presentation carries it to the host like any guest image.
class ImageUpload {
 public:
  ImageUpload() = default;
  ImageUpload(const ImageUpload&) = delete;
  ImageUpload& operator=(const ImageUpload&) = delete;
  ~ImageUpload();
  // Room for width * height RGBA8 pixels.
  XrResult initialize(std::uint32_t width, std::uint32_t height);
  std::uint8_t* pixels() const { return static_cast<std::uint8_t*>(mapped_); }
  // Copies the staging pixels into the whole of the chain's image 0 and releases it.
  XrResult publish(Swapchain& chain);
 private:
  template<class F> F vk(const char* name) const;
  XrResult checked(VkResult result, const char* operation) const;
  XrGraphicsBindingVulkanKHR binding_{};
  VkQueue queue_{};
  VkCommandPool pool_{};
  VkCommandBuffer command_{};
  VkFence fence_{};
  VkBuffer staging_{};
  VkDeviceMemory memory_{};
  void* mapped_ = nullptr;
  bool coherent_ = false;
};
} // namespace venture::hle::openxr
