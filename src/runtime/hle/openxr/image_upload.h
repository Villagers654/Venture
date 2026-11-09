#pragma once
#include "hle/openxr/state.h"
#include "gfx/shared_image_import.h"
#include <array>

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
  // Whether publish(chain, image) can import pictures from the media
  // decoder's device: the session device enables the import extension.
  bool imports_shared_images() const { return shared_images_; }
  // Copies a same-size shared image (gfx/shared_image.h) into image 0 on the
  // GPU and releases it; XR_ERROR_VALIDATION_FAILURE when it cannot be used.
  XrResult publish(Swapchain& chain, const gfx::SharedImage& image);
 private:
  XrResult submit();
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
  bool shared_images_ = false;
  std::array<std::uint8_t, 16> device_uuid_{};
  gfx::SharedImageImports imports_;
};
} // namespace venture::hle::openxr
