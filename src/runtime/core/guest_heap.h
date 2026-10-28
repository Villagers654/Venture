#pragma once
#include "core/guest.h"
#include <memory>
#include <utility>

struct mi_heap_s;

namespace venture::guest_heap {
// Selected once with the address-space policy, before HLE registration. The
// libc allocation paths keep one allocator call and no per-call ISA branch.
struct Allocator {
  void* (*aligned)(std::uint64_t bytes, std::uint64_t alignment);
  void* (*zero_aligned)(std::uint64_t count, std::uint64_t bytes, std::uint64_t alignment);
  void* (*realloc_aligned)(void*, std::uint64_t bytes, std::uint64_t alignment);
  void (*free)(void*);
};
extern Allocator allocator;
void select32();
// A64: mimalloc takes its memory from one contiguous reservation, so the guest
// heap stays in a few adjacent 4 GiB windows as on a device (Unity 2019's
// allocator tracks at most five distinct upper-32-bit windows). Idempotent.
void select64();
// The A64 arena [first, second), or {0, 0} before select64 or when it fell back.
std::pair<GuestAddr, GuestAddr> arena64();
// Exclusive mimalloc arena; allocations fail with nullptr on exhaustion and
// never fall back to host memory. Allocation is thread-local, free is cross-thread.
void* allocate(std::uint64_t bytes, std::uint64_t alignment = 16, bool zero = false);
void deallocate(void*);
void* reallocate(void*, std::uint64_t bytes);
std::uint64_t usable_size(const void*);
struct ThreadHeapDeleter { void operator()(mi_heap_s*) const noexcept; };
using DetachedThreadHeap = std::unique_ptr<mi_heap_s, ThreadHeapDeleter>;
// Transfer the current thread's heap without running allocator cleanup. Destroy
// the returned owner on this host thread, outside runtime locks; live allocations
// are retained.
DetachedThreadHeap detach_thread();
void release_thread();  // after guest destructors; live allocations are retained
void prepare_thread();  // before an adopted GuestThread's thread_local owner
bool contains(GuestAddr);
// Check live heap pages intersecting a range; other mappings are checked by
// AddressSpace's immutable permissions snapshot.
bool accessible(GuestAddr, std::uint64_t size);
}  // namespace venture::guest_heap
