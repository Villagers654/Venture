// linux_errno.h names must precede the host <errno.h> macros.
#include "android/errors.h"
#include "android/hardware_buffer.h"

#include <climits>
#include <cstring>

#include "android/handles.h"
#include "common/log.h"
#include "core/address_space.h"

namespace venture::android {
namespace {
Handles<HardwareBuffer> buffers;
std::mutex lifetime_mutex, allocator_mutex;
GpuAllocator gpu_allocator;

GpuAllocator allocator() {
  std::lock_guard lock(allocator_mutex);
  return gpu_allocator;
}
int bytes_per_pixel(std::uint32_t f) { return f == 1 || f == 2 ? 4 : f == 4 ? 2 : f == 0x21 ? 1 : 0; }
bool cpu_supported(const HardwareBufferDesc& d) {
  auto bpp = bytes_per_pixel(d.format);
  if (!bpp || !d.width || !d.height || d.layers != 1 || d.rfu0 || d.rfu1 || (d.usage & ~0x33ull))
    return false;
  if ((d.usage & 0xf) == 1 || (d.usage & 0xf0) == 0x10) return false;
  if (d.format == 0x21 && d.height != 1) return false;
  return std::uint64_t(d.width) * d.height <= std::uint64_t(INT32_MAX) / bpp;
}
// GPU buffers are device-local: CPU access to them is not provided.
bool gpu_request(const HardwareBufferDesc& d) {
  return (d.usage & ahb::gpu_mask) && !(d.usage & (ahb::cpu_read_mask | ahb::cpu_write_mask));
}
}  // namespace

HardwareBuffer::~HardwareBuffer() {
  if (memory) AddressSpace::get().free(memory);
}
GuestAddr publish_hardware_buffer(const HardwareBufferDesc& desc, std::shared_ptr<const GpuPayload> gpu) {
  auto* b = buffers.create();
  b->desc = desc;
  b->gpu = std::move(gpu);
  return h2g(b);
}
std::shared_ptr<HardwareBuffer> hardware_buffer(GuestAddr handle) { return buffers.get(handle); }
void acquire_hardware_buffer(GuestAddr handle) {
  std::lock_guard lock(lifetime_mutex);
  if (auto b = buffers.get(handle)) ++b->refs;
}
void release_hardware_buffer(GuestAddr handle) {
  std::lock_guard lock(lifetime_mutex);
  if (auto b = buffers.get(handle); b && --b->refs == 0) buffers.erase(b.get());
}
bool hardware_buffer_supported(const HardwareBufferDesc& d) {
  if (d.rfu0 || d.rfu1) return false;
  if (!gpu_request(d)) return cpu_supported(d);
  auto gpu = allocator();
  return gpu.supported && gpu.supported(d);
}
int allocate_hardware_buffer(const HardwareBufferDesc& d, GuestAddr* out) {
  *out = 0;
  if (!hardware_buffer_supported(d)) {
    log::warn("hardware-buffer", "Unsupported AHardwareBuffer format {:#x} usage {:#x}", d.format, d.usage);
    return -err::invalid;
  }
  auto desc = d;
  desc.stride = d.width;
  if (gpu_request(d)) {
    auto payload = allocator().allocate(desc);
    if (!payload) return -err::no_memory;
    *out = publish_hardware_buffer(desc, std::move(payload));
    return 0;
  }
  auto* b = buffers.create();
  b->desc = desc;
  b->memory = AddressSpace::get().allocate(std::uint64_t(d.width) * d.height * bytes_per_pixel(d.format),
                                           kProtRead | kProtWrite, "[hardware-buffer-cpu]");
  if (!b->memory) {
    buffers.erase(b);
    return -err::no_memory;
  }
  *out = h2g(b);
  return 0;
}
bool write_hardware_buffer(GuestAddr handle, std::span<const std::uint8_t> rgba) {
  auto b = buffers.get(handle);
  if (!b || (b->desc.format != 1 && b->desc.format != 2) || b->desc.layers != 1 ||
      rgba.size() != std::size_t(b->desc.width) * b->desc.height * 4)
    return false;
  if (b->gpu) {
    auto gpu = allocator();
    return gpu.write && gpu.write(*b->gpu, rgba);
  }
  if (!b->memory) return false;
  std::memcpy(g2h(b->memory), rgba.data(), rgba.size());  // CPU buffers are tightly packed (stride == width)
  return true;
}
void set_gpu_allocator(GpuAllocator value) {
  std::lock_guard lock(allocator_mutex);
  gpu_allocator = std::move(value);
}
}  // namespace venture::android
