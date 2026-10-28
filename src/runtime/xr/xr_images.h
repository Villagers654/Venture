// Persistent guest texture rings and destination copies. angle_images.cpp
// adapts gfx/xr_images.h after the GLES merge; tests inject a Vulkan fixture.
// Image content stays on the GPU.
#pragma once
#include "xr/api.h"
#include "xr/sampler.h"
#include <span>

namespace venture::xr {
struct ImageDesc {
  std::int64_t gl_format;
  VkFormat vk_format;
  std::uint32_t width, height, layers, samples, mip_count, face_count;
  // Format of the copy destination when the host lacks vk_format (UNDEFINED: same). Copies then convert by value.
  VkFormat host_format = VK_FORMAT_UNDEFINED;
  // Destination mip count when presentation uses fewer levels than the guest
  // texture. Zero preserves the source count; the guest storage is unchanged.
  std::uint32_t host_mip_count = 0;
};
class XrImages {
public:
  virtual ~XrImages() = default;
  // Persistent guest GL names, independent of OpenXR's acquire order. Called
  // on the render thread with the shared GPU queue lock held.
  virtual XrResult create_ring(const ImageDesc &, std::span<std::uint32_t> textures) = 0;
  virtual void destroy_ring(std::span<const std::uint32_t> textures) = 0;
  // Must order ANGLE rendering (including the supplied VrApi completion fence),
  // copy/resolve every layer and restore XR-required image layout. No host wait
  // is needed if submission uses the shared ordered queue. Success means the
  // destination is ready for xrReleaseSwapchainImage. Destination row 0 is the
  // top of the displayed image (Vulkan/OpenXR). bottom_up says guest memory
  // row 0 is its bottom (the GLES framebuffer convention), so rows reverse.
  virtual XrResult copy_to_image(std::uint32_t texture, VkImage destination, const ImageDesc &,
                                 std::uint64_t completion_fence, bool bottom_up) = 0;
  // Order each producer before a single destination copy. Non-GLES providers
  // reject EGL sync identities rather than silently ignoring them.
  virtual XrResult wait_producer_sync(std::uint64_t fence) {
    return fence ? XR_ERROR_FEATURE_UNSUPPORTED : XR_SUCCESS;
  }
  virtual bool owns_texture(std::uint32_t texture, const ImageDesc &) const = 0;
  // Vulkan-only providers have no application GLES sampler state to update.
  virtual XrResult update_sampler(std::span<const std::uint32_t>, const ImageDesc&,
                                  const SamplerOpenGLES&) { return XR_SUCCESS; }
  virtual bool application_uses_gles() const { return false; }
  virtual bool begin_handoff_batch() { return false; }
  virtual XrResult end_handoff_batch() { return XR_SUCCESS; }
  // Drop cached bridge objects before the runtime destroys their VkImages.
  // Providers without destination caches have nothing to release.
  virtual XrResult release_images(std::span<const VkImage>) { return XR_SUCCESS; }
  // Native XR calls may submit to ANGLE's shared queue. Take this external
  // queue lock after GpuDevice::queue_lock, and never hold it during GL calls.
  virtual bool external_queue_active() const { return false; }
  virtual bool lock_external_queue() { return true; }
  virtual void unlock_external_queue() {}
};
// Install before guest XR initialization. The provider must outlive all XR
// sessions. nullptr removes it only when no guest session exists.
void install_xr_images(XrImages *provider);
XrImages *xr_images();
// Available automatically in a merged build containing venture_gfx. Installation
// does not create a GL context or GPU device; ring creation uses the caller's
// current GLES share group. Returns false when that backend is not built.
bool install_angle_xr_images();
} // namespace venture::xr
