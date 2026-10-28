#include <algorithm>
#include <atomic>
#include <climits>
#include <vector>

#include "core/wait.h"
#include "hle/libc/libc_a.h"
namespace venture::hle::libc {
namespace {
void* new_impl(std::uint64_t n) {
  auto p = allocate(n);
  if (!p) fatal("operator new: guest allocation failed");
  return p;
}
void* aligned_new(std::uint64_t n, std::uint64_t alignment) {
  auto p = allocate_aligned(alignment, n);
  if (!p) fatal("aligned operator new: guest allocation failed");
  return p;
}
void* aligned_nothrow_new(std::uint64_t n, std::uint64_t alignment, GuestAddr) {
  return allocate_aligned(alignment, n);
}
// Itanium C++ ABI 3.3.2, ARM64: only the first byte is read by generated guest
// code. Use a 32-bit atomic word: initialized=1, in-progress=0x100.
thread_local std::vector<GuestAddr> owned_guards;
int guard_acquire(CallContext& c, std::uint32_t* guard) {
  std::atomic_ref word(*guard);
  for (;;) {
    auto value = word.load(std::memory_order_acquire);
    if (value & 1) return 0;
    if (!value && word.compare_exchange_weak(value, 0x100, std::memory_order_acquire)) {
      owned_guards.push_back(h2g(guard));
      return 1;
    }
    if (std::find(owned_guards.begin(), owned_guards.end(), h2g(guard)) != owned_guards.end())
      fatal("recursive initialization of a C++ local static");
    futex_wait(c.thread(), h2g(guard), 0x100, -1);
  }
}
void finish_guard(std::uint32_t* guard, std::uint32_t state) {
  std::erase(owned_guards, h2g(guard));
  std::atomic_ref(*guard).store(state, std::memory_order_release);
  futex_wake(h2g(guard), INT_MAX);
}
void guard_release(std::uint32_t* guard) { finish_guard(guard, 1); }
void guard_abort(std::uint32_t* guard) { finish_guard(guard, 0); }
void pure_virtual() { fatal("__cxa_pure_virtual: pure virtual guest call"); }
void deleted_virtual() { fatal("__cxa_deleted_virtual: deleted virtual guest call"); }
const unsigned char nothrow_object = 0;
}  // namespace
void register_libstdcpp(HleLibrary& lib) {
  for (auto n : {"_Znwm", "_Znam"}) bind<&new_impl>(lib, n);
  for (auto n : {"_ZnwmRKSt9nothrow_t", "_ZnamRKSt9nothrow_t"}) bind<&allocate>(lib, n);
  for (auto n : {"_ZnwmSt11align_val_t", "_ZnamSt11align_val_t"}) bind<&aligned_new>(lib, n);
  for (auto n : {"_ZnwmSt11align_val_tRKSt9nothrow_t", "_ZnamSt11align_val_tRKSt9nothrow_t"})
    bind<&aligned_nothrow_new>(lib, n);
  for (auto n :
       {"_ZdlPv", "_ZdaPv", "_ZdlPvm", "_ZdaPvm", "_ZdlPvRKSt9nothrow_t", "_ZdaPvRKSt9nothrow_t",
        "_ZdlPvSt11align_val_t", "_ZdaPvSt11align_val_t", "_ZdlPvmSt11align_val_t", "_ZdaPvmSt11align_val_t",
        "_ZdlPvSt11align_val_tRKSt9nothrow_t", "_ZdaPvSt11align_val_tRKSt9nothrow_t"})
    bind<&deallocate>(lib, n);
  bind<&guard_acquire>(lib, "__cxa_guard_acquire");
  bind<&guard_release>(lib, "__cxa_guard_release");
  bind<&guard_abort>(lib, "__cxa_guard_abort");
  bind<&pure_virtual>(lib, "__cxa_pure_virtual");
  bind<&deleted_virtual>(lib, "__cxa_deleted_virtual");
  lib.add_data("_ZSt7nothrow", process_config().isa() == GuestIsa::a32
                                   ? low_data(std::as_bytes(std::span(&nothrow_object, 1)))
                                   : h2g(&nothrow_object));
}
}  // namespace venture::hle::libc
