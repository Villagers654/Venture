#include "android/errors.h"
#include "android/hardware_buffer.h"
#include "android/native_window.h"
#include "hle/libandroid/parts.h"

namespace venture::hle::android_hle {
namespace {
using namespace venture::android;
using Buffer = HardwareBuffer;
int is_supported(const HardwareBufferDesc* d) { return d && hardware_buffer_supported(*d); }
int allocate(const HardwareBufferDesc* d, GuestAddr* out) {
  if (!d || !out) return -err::invalid;
  return allocate_hardware_buffer(*d, out);
}
void acquire(Buffer* p) { acquire_hardware_buffer(h2g(p)); }
void release(Buffer* p) { release_hardware_buffer(h2g(p)); }
void describe(Buffer* p, HardwareBufferDesc* out) {
  if (auto b = hardware_buffer(h2g(p)); b && out) *out = b->desc;
}
int lock(Buffer* p, std::uint64_t usage, int fence, const Rect* rect, GuestAddr* out) {
  auto b = hardware_buffer(h2g(p));
  if (!b || !out || !(usage & 0x33) || (usage & ~0x33ull)) return -err::invalid;
  if ((usage & 0xf) == 1 || (usage & 0xf0) == 0x10) return -err::invalid;
  if (fence >= 0) {
    log::warn("hardware-buffer", "CPU lock cannot import a sync fence");
    return -err::unsupported;
  }
  if ((usage & 0xf) > (b->desc.usage & 0xf) || (usage & 0xf0) > (b->desc.usage & 0xf0)) return -err::invalid;
  if (rect && (rect->left < 0 || rect->top < 0 || rect->right < rect->left || rect->bottom < rect->top ||
               std::uint32_t(rect->right) > b->desc.width || std::uint32_t(rect->bottom) > b->desc.height))
    return -err::invalid;
  std::lock_guard guard(b->mutex);
  if (b->locked) return -err::busy;
  b->locked = true;
  *out = b->memory;
  return 0;
}
int unlock(Buffer* p, int* fence) {
  auto b = hardware_buffer(h2g(p));
  if (!b) return -err::invalid;
  std::lock_guard guard(b->mutex);
  if (!b->locked) return -err::invalid;
  b->locked = false;
  if (fence) *fence = -1;
  return 0;
}
}  // namespace
void register_hardware_buffers(HleLibrary& lib) {
  add<&allocate>(lib, "AHardwareBuffer_allocate");
  add<&is_supported>(lib, "AHardwareBuffer_isSupported");
  add<&acquire>(lib, "AHardwareBuffer_acquire");
  add<&release>(lib, "AHardwareBuffer_release");
  add<&describe>(lib, "AHardwareBuffer_describe");
  add<&lock>(lib, "AHardwareBuffer_lock");
  add<&unlock>(lib, "AHardwareBuffer_unlock");
}
}  // namespace venture::hle::android_hle
