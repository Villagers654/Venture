#include <algorithm>
#include "core/thread_lifetime.h"
#include <atomic>
#include <climits>
#include <string>
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
void* nothrow_new(std::uint64_t n, GuestAddr) { return allocate(n); }
void* aligned_new(std::uint64_t n, std::uint64_t alignment) {
  auto p = allocate_aligned(alignment, n);
  if (!p) fatal("aligned operator new: guest allocation failed");
  return p;
}
void* aligned_nothrow_new(std::uint64_t n, std::uint64_t alignment, GuestAddr) {
  return allocate_aligned(alignment, n);
}
// Every operator delete form frees the block alone; its size, alignment and
// nothrow arguments only describe it. Each arity has its own signature so the
// A32 marshaller reads exactly the arguments the guest passed.
void delete2(void* p, std::uint64_t) { deallocate(p); }
void delete3(void* p, std::uint64_t, std::uint64_t) { deallocate(p); }
// bionic/libc/bionic/__cxa_guard.cpp. The guard's first 32-bit word carries
// the state on both ISAs: the ARM C++ ABI (IHI 0041, 3.2.3) gives ARM a 32-bit
// guard whose bit 0 compiled code tests, and the Itanium ABI's 64-bit guard on
// ARM64 is tested through its first byte. 1 = complete, so neither in-progress
// value sets the bit compiled code checks.
constexpr std::uint32_t kNotStarted = 0, kComplete = 1, kUnderway = 0x100, kUnderwayWithWaiter = 0x200;
std::vector<GuestAddr>& owned_guards() { return thread_lifetime::local<std::vector<GuestAddr>, struct OwnedGuards>(); }
int guard_acquire(CallContext& c, std::uint32_t* guard) {
  std::atomic_ref word(*guard);
  auto value = word.load(std::memory_order_acquire);
  for (;;) {
    if (value == kComplete) return 0;
    if (value == kNotStarted) {
      if (!word.compare_exchange_weak(value, kUnderway, std::memory_order_acquire, std::memory_order_acquire)) continue;
      owned_guards().push_back(h2g(guard));
      return 1;
    }
    // Bionic waits forever here when the initializer reenters its own static;
    // name the deadlock instead, as libc++abi does.
    if (std::find(owned_guards().begin(), owned_guards().end(), h2g(guard)) != owned_guards().end())
      fatal("recursive initialization of a C++ local static");
    if (value == kUnderway &&
        !word.compare_exchange_weak(value, kUnderwayWithWaiter, std::memory_order_acquire, std::memory_order_acquire))
      continue;
    futex_wait(c.thread(), h2g(guard), kUnderwayWithWaiter, -1);
    value = word.load(std::memory_order_acquire);
  }
}
void finish_guard(std::uint32_t* guard, std::uint32_t state) {
  std::erase(owned_guards(), h2g(guard));
  if (std::atomic_ref(*guard).exchange(state, std::memory_order_release) == kUnderwayWithWaiter)
    futex_wake(h2g(guard), INT_MAX);
}
void guard_release(std::uint32_t* guard) { finish_guard(guard, kComplete); }
void guard_abort(std::uint32_t* guard) { finish_guard(guard, kNotStarted); }
void pure_virtual() { fatal("__cxa_pure_virtual: pure virtual guest call"); }
void deleted_virtual() { fatal("__cxa_deleted_virtual: deleted virtual guest call"); }
const unsigned char nothrow_object = 0;
}  // namespace
void register_cxa_guards(HleLibrary& lib) {
  bind<&guard_acquire>(lib, "__cxa_guard_acquire");
  bind<&guard_release>(lib, "__cxa_guard_release");
  bind<&guard_abort>(lib, "__cxa_guard_abort");
}
void register_libstdcpp(HleLibrary& lib) {
  // Bionic's libstdc++.map.txt mangles size_t as unsigned long (m) on LP64 and
  // unsigned int (j) on ARM32. The other forms (sized and aligned) are the
  // same entry points under the guest's size_t mangling.
  const bool a32 = process_config().isa() == GuestIsa::a32;
  const auto size_name = [a32](std::string_view lp64) {
    std::string name(lp64);
    if (a32 && (name.starts_with("_Znwm") || name.starts_with("_Znam"))) name[4] = 'j';
    else if (a32 && (name.starts_with("_ZdlPvm") || name.starts_with("_ZdaPvm"))) name[6] = 'j';
    return name;
  };
  for (auto n : {"_Znwm", "_Znam"}) bind<&new_impl>(lib, size_name(n));
  for (auto n : {"_ZnwmRKSt9nothrow_t", "_ZnamRKSt9nothrow_t"}) bind<&nothrow_new>(lib, size_name(n));
  for (auto n : {"_ZnwmSt11align_val_t", "_ZnamSt11align_val_t"}) bind<&aligned_new>(lib, size_name(n));
  for (auto n : {"_ZnwmSt11align_val_tRKSt9nothrow_t", "_ZnamSt11align_val_tRKSt9nothrow_t"})
    bind<&aligned_nothrow_new>(lib, size_name(n));
  for (auto n : {"_ZdlPv", "_ZdaPv"}) bind<&deallocate>(lib, n);
  for (auto n : {"_ZdlPvm", "_ZdaPvm", "_ZdlPvRKSt9nothrow_t", "_ZdaPvRKSt9nothrow_t", "_ZdlPvSt11align_val_t",
                 "_ZdaPvSt11align_val_t"})
    bind<&delete2>(lib, size_name(n));
  for (auto n : {"_ZdlPvmSt11align_val_t", "_ZdaPvmSt11align_val_t", "_ZdlPvSt11align_val_tRKSt9nothrow_t",
                 "_ZdaPvSt11align_val_tRKSt9nothrow_t"})
    bind<&delete3>(lib, size_name(n));
  register_cxa_guards(lib);
  bind<&pure_virtual>(lib, "__cxa_pure_virtual");
  bind<&deleted_virtual>(lib, "__cxa_deleted_virtual");
  lib.add_data("_ZSt7nothrow", a32 ? low_data(std::as_bytes(std::span(&nothrow_object, 1))) : h2g(&nothrow_object));
}
}  // namespace venture::hle::libc
