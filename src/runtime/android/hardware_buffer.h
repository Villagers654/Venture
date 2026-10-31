#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>

#include "core/guest.h"

namespace venture::android {
// AOSP libs/nativewindow/include/android/hardware_buffer.h, AArch64 ABI.
struct HardwareBufferDesc {
  std::uint32_t width, height, layers, format;
  std::uint64_t usage;
  std::uint32_t stride, rfu0;
  std::uint64_t rfu1;
};
static_assert(sizeof(HardwareBufferDesc) == 40 && offsetof(HardwareBufferDesc, usage) == 16);

namespace ahb {
inline constexpr std::uint64_t cpu_read_mask = 0xf, cpu_write_mask = 0xf0;
inline constexpr std::uint64_t gpu_sampled_image = 1ull << 8, gpu_framebuffer = 1ull << 9;
inline constexpr std::uint64_t protected_content = 1ull << 14, gpu_data_buffer = 1ull << 24;
inline constexpr std::uint64_t gpu_cube_map = 1ull << 25, gpu_mipmap_complete = 1ull << 26;
inline constexpr std::uint64_t gpu_mask =
    gpu_sampled_image | gpu_framebuffer | gpu_data_buffer | gpu_cube_map | gpu_mipmap_complete;
}  // namespace ahb

// Device memory behind a GPU-usage buffer, defined by the graphics backend.
struct GpuPayload {
  virtual ~GpuPayload() = default;
};

// One AHardwareBuffer. The guest handle is the object's address; references
// follow AHardwareBuffer_acquire/release and start at one.
struct HardwareBuffer {
  HardwareBufferDesc desc{};
  std::shared_ptr<const GpuPayload> gpu;  // GPU usage
  GuestAddr memory = 0;                   // CPU usage
  std::atomic<int> refs{1};
  std::mutex mutex;
  bool locked = false;
  ~HardwareBuffer();
};

GuestAddr publish_hardware_buffer(const HardwareBufferDesc& desc, std::shared_ptr<const GpuPayload> gpu);
std::shared_ptr<HardwareBuffer> hardware_buffer(GuestAddr handle);
void acquire_hardware_buffer(GuestAddr handle);
void release_hardware_buffer(GuestAddr handle);

// AHardwareBuffer_isSupported/allocate. Returns 0 or a negative errno.
bool hardware_buffer_supported(const HardwareBufferDesc& desc);
int allocate_hardware_buffer(const HardwareBufferDesc& desc, GuestAddr* out);

// GPU-usage buffers are allocated by the Vulkan HLE, which owns the host GPU
// translation. Without an allocator only CPU buffers exist.
struct GpuAllocator {
  std::function<bool(const HardwareBufferDesc&)> supported;
  std::function<std::shared_ptr<const GpuPayload>(const HardwareBufferDesc&)> allocate;
  // Fills an RGBA8 buffer's single layer with top-down RGBA pixels and
  // returns once the copy is complete on the GPU.
  std::function<bool(const GpuPayload&, std::span<const std::uint8_t>)> write;
};
void set_gpu_allocator(GpuAllocator allocator);
// Producer side of an image queue (AImageReader): fills a buffer of format
// R8G8B8A8_UNORM or R8G8B8X8_UNORM with width x height x 4 bytes of pixels.
bool write_hardware_buffer(GuestAddr handle, std::span<const std::uint8_t> rgba);
}  // namespace venture::android
