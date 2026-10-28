// GLES-to-OpenXR copy bridge on the one shared Vulkan device.
// Protocol (r3b-angle-vulkan/report.md, "Concrete ... frame protocol"):
//  * XR agent acquires AND successfully waits an image, then calls acquire().
//  * Guest renders into textures()[index], with its usual FBO/multiview setup.
//  * Call release() on a thread with the same GL share group current; only
//  after
//    success may XR release the image. The copy is submitted, not CPU-idled.
//  * XR queue operations hold GpuDevice::queue_lock then lock_angle_queue().
//    Do not hold ANGLE's nonrecursive queue lock while invoking this API or GL.
//  * Destroy before XR swapchain/session and before the EGL share group.
// The XR swapchain must include TRANSFER_DST usage. First version supports
// single-sample color, RGBA8/sRGB/RGBA16F/RGB10_A2. Unsupported shapes fail
// with a diagnostic. Arrays and mip chains use explicit single-level 2D
// bridges so each exported EGLImage identifies one unambiguous subresource.
#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>
namespace venture::gfx {
struct XrImageDescription {
  std::uint32_t width = 0, height = 0, layers = 1;
  VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  VkImageUsageFlags usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  std::span<const VkImage> images;
  std::uint32_t mip_count = 1;
  // Eligible immutable, single-level 2D guest storage; zero allocates a bridge.
  std::uint32_t source_texture = 0;
  std::uintptr_t source_group = 0;
  // The caller supplies the original array/mip texture to every release().
  // Destination mip_count may select only its base or a prefix of its levels.
  // Export subresources only; no guest-visible aggregate texture is needed.
  bool external_subresources = false;
};
class XrImageBridge {
 public:
  static std::unique_ptr<XrImageBridge> create(const XrImageDescription&,
                                               std::string* error);
  ~XrImageBridge();
  // Same-thread, same-share-group handoffs can share one queue submission.
  // End the batch before releasing any host XR image. Borrowed sources wait
  // for GPU completion before ANGLE reacquires them; ordinary bridges do not.
  static bool begin_batch();
  static bool end_batch(std::string* error, std::int64_t* ownership_wait_ns = nullptr);
  std::span<const std::uint32_t> textures() const;
  struct StorageCensus {
    std::uint32_t aggregate_textures = 0, subresource_textures = 0;
    std::uint64_t aggregate_bytes = 0, subresource_bytes = 0;
  };
  // Logical payload requested by successful GL storage allocations, excluding
  // borrowed guest storage, host XR images and driver allocation overhead.
  StorageCensus storage_census() const;
  bool gpu_ownership() const;
  // No caller may hold the GLES queue lock across acquire/release waits.
  bool acquire(std::uint32_t index, std::string* error, std::int64_t* wait_ns = nullptr);
  // bottom_up (GLES framebuffer rows) reverses rows into the XR image, whose
  // row 0 is the top; false copies rows unchanged.
  // Layered/mipmapped callers may supply their original GLES texture,
  // avoiding a copy into textures()[index] before subresource extraction.
  // An optional GL timestamp query marks the end of the GLES copy and is
  // submitted by the same ANGLE ownership release, before the Vulkan blit.
  // With GPU ownership, begin/end instead bracket the subsequent ownership
  // read/wait, including its GPU cost without counting the blit twice.
  bool release(std::uint32_t index, std::string* error, bool bottom_up = true,
               std::uint32_t source_texture = 0, std::uint32_t end_query = 0,
               std::int64_t* ownership_wait_ns = nullptr,
               std::uint32_t begin_query = 0);
  // Opt-in diagnostic (VENTURE_VULKAN_GPU_TIMING): GPU timestamps in nanoseconds
  // bracketing the last release of `index`, available once after acquire()
  // has waited for that submission. False when timing is off or unavailable.
  bool take_gpu_span(std::uint32_t index, std::uint64_t& begin_ns,
                     std::uint64_t& end_ns);
  XrImageBridge(const XrImageBridge&) = delete;
  XrImageBridge& operator=(const XrImageBridge&) = delete;

 private:
  XrImageBridge();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace venture::gfx
