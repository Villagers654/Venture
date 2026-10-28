#include <mimalloc.h>

#include <atomic>
#include <limits>

#include "common/log.h"
#include "core/guest_heap.h"
#include "core/process.h"
#include "hle/libc/libc_a.h"
namespace venture::hle::libc {
namespace {
std::atomic<std::uint64_t> allocated_bytes{0};
void* record(void* p) {
  if (p)
    allocated_bytes.fetch_add(mi_usable_size(p), std::memory_order_relaxed);
  else
    set_error(err::kENOMEM);
  return p;
}
}  // namespace
void* allocate(std::uint64_t size) { return record(guest_heap::allocator.aligned(size, 16)); }
void deallocate(void* p) {
  if (p) allocated_bytes.fetch_sub(mi_usable_size(p), std::memory_order_relaxed);
  guest_heap::allocator.free(p);
}
std::uint64_t usable_size(const void* p) { return p ? mi_usable_size(p) : 0; }
void* allocate_aligned(std::uint64_t alignment, std::uint64_t size) {
  if (!alignment || (alignment & (alignment - 1))) {
    set_error(err::kEINVAL);
    return nullptr;
  }
  return record(guest_heap::allocator.aligned(size, std::max<std::uint64_t>(16, alignment)));
}
void* reallocate(void* p, std::uint64_t size) {
  if (!p) return allocate(size);
  if (!size) {
    deallocate(p);
    return nullptr;
  }
  auto old = usable_size(p);
  auto out = guest_heap::allocator.realloc_aligned(p, size, 16);
  if (!out) {
    set_error(err::kENOMEM);
    return nullptr;
  }
  allocated_bytes.fetch_sub(old, std::memory_order_relaxed);
  return record(out);
}
namespace {
void* zero_allocate(std::uint64_t count, std::uint64_t size) {
  if (size && count > UINT64_MAX / size) {
    set_error(err::kENOMEM);
    return nullptr;
  }
  return record(guest_heap::allocator.zero_aligned(count, size, 16));
}
int posix_allocate(void** out, std::uint64_t alignment, std::uint64_t size) {
  if (alignment < sizeof(void*) || (alignment & (alignment - 1))) return err::kEINVAL;
  auto saved = *error_location();
  auto p = allocate_aligned(alignment, size);
  set_error(saved);
  if (!p) return err::kENOMEM;
  *out = p;
  return 0;
}
int posix_allocate32(void** out, std::uint64_t alignment, std::uint64_t size) {
  if (alignment < 4 || (alignment & (alignment - 1))) return err::kEINVAL;
  auto saved = *error_location();
  auto p = allocate_aligned(alignment, size);
  set_error(saved);
  if (!p) return err::kENOMEM;
  *out = p;
  return 0;  // descriptor Out<Ptr> owns the four-byte guest write
}
void* page_allocate(std::uint64_t n) { return allocate_aligned(4096, n); }
void* rounded_page_allocate(std::uint64_t n) {
  if (n > UINT64_MAX - 4095) {
    set_error(err::kENOMEM);
    return nullptr;
  }
  return page_allocate(n ? align_up(n, 4096) : 4096);
}
// Bionic LP64 mallinfo uses ten size_t fields (bionic/libc/include/malloc.h).
void mallinfo_handler(CallContext& c) {
  auto out = g2h<std::uint64_t>(c.x(8));
  std::fill_n(out, 10, 0);
  out[0] = out[7] = allocated_bytes.load(std::memory_order_relaxed);
}
int mallopt_impl(int, int) {
  log::warn("libc", "mallopt: unsupported allocator tuning");
  return 0;
}
}  // namespace
void register_memory(HleLibrary& lib) {
  // A64 allocations call mimalloc directly: no guest callbacks, guest waits
  // or address space changes, so they complete inside the JIT. The A32
  // arena maps guest memory on first use and stays in the run loop.
  const auto execution = process_config().isa() == GuestIsa::a64 ? Execution::InJit : Execution::RunLoop;
  bind<&allocate>(lib, "malloc", execution);
  bind<&deallocate>(lib, "free", execution);
  bind<&zero_allocate>(lib, "calloc", execution);
  bind<&reallocate>(lib, "realloc", execution);
  bind<&allocate_aligned>(lib, "memalign", execution);
  bind<&allocate_aligned>(lib, "aligned_alloc", execution);
  if (process_config().isa() == GuestIsa::a32)
    bind<&posix_allocate32>(lib, "posix_memalign");
  else
    bind<&posix_allocate>(lib, "posix_memalign", execution);
  bind<&page_allocate>(lib, "valloc", execution);
  bind<&rounded_page_allocate>(lib, "pvalloc", execution);
  bind<&usable_size>(lib, "malloc_usable_size", execution);
  bind(lib, "mallinfo", mallinfo_handler);
  bind(lib, "mallinfo2", mallinfo_handler);
  bind<&mallopt_impl>(lib, "mallopt");
}
}  // namespace venture::hle::libc
