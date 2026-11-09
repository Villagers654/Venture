// Vulkan Video decode (FFmpeg's Vulkan hwaccel) and GPU color conversion.
//
// FFmpeg creates its own VkDevice, with video decode queues, on the physical GPU
// Venture renders with. Decoded pictures stay VkImages; a compute pass
// converts a picture to RGBA8 at the consumer's size, into an image whose
// memory is exported to the consumer's device (gfx::SharedImage), or into a
// readback buffer for CPU consumers. Works on any Vulkan 1.3 driver offering
// the codec's video decode extension (Windows and Linux, any vendor).
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

struct AVBufferRef;
struct AVFrame;
namespace venture::gfx { struct SharedImage; }
namespace venture::media {
class Library;
namespace vulkan_video {
// A new reference to the process-wide FFmpeg Vulkan device on the host GPU
// (gfx::host_gpu() when known), or null when Vulkan or the device is unusable.
AVBufferRef* device_reference(Library&);

// Converts AV_PIX_FMT_VULKAN pictures (NV12 or P010). One per decoder; calls
// are serialized by its owner.
class Converter {
 public:
  explicit Converter(std::shared_ptr<Library> library);
  ~Converter();
  Converter(const Converter&) = delete;
  Converter& operator=(const Converter&) = delete;
  // Exactly one of image (GPU consumer) and rgba (CPU readback) is non-null.
  // False when this picture cannot be converted on the GPU; the caller then
  // downloads it instead.
  bool convert(AVFrame* picture, int width, int height,
               std::shared_ptr<const gfx::SharedImage>* image, std::vector<std::uint8_t>* rgba);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};
}  // namespace vulkan_video
}  // namespace venture::media
