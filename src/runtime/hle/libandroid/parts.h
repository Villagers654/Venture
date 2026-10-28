#pragma once
#include "android/errors.h"
#include "core/hle.h"
#include "core/linker.h"
#include "core/thread.h"
#include "common/log.h"
#include "hle/libandroid/a32.h"
namespace venture::hle::android_hle {
namespace detail {
// The current core wrap<> rejects host `long` even on LP64 systems where
// int64_t aliases it. Preserve the exact 64-bit ABI without changing that shared
// contract: pass through the distinct, explicitly checked long-long type.
template <class T>
using AbiType = std::conditional_t<std::is_same_v<T, std::int64_t>, decltype(0LL),
                                   std::conditional_t<std::is_same_v<T, std::uint64_t>, decltype(0ULL), T>>;
static_assert(sizeof(AbiType<std::int64_t>) == 8);
template <auto F, class Signature>
struct PortableCall;
template <auto F, class R, class... A>
struct PortableCall<F, R (*)(A...)> {
  static AbiType<R> invoke(AbiType<A>... args) { return F(args...); }
};
}  // namespace detail
template <auto F>
void add(HleLibrary& lib, const char* name) {
  abi32::install<F>(lib.soname(), name);
  lib.add_function(name,
                   make_thunk(lib.soname(), name, wrap<&detail::PortableCall<F, decltype(F)>::invoke>()));
}
inline int fail(int error) {
  if (auto* t = GuestThread::current()) t->set_errno(error);
  return -1;
}
void register_assets(HleLibrary&);
void register_windows(HleLibrary&);
void register_looper(HleLibrary&);
void register_configuration(HleLibrary&);
void register_input(HleLibrary&);
void register_choreographer(HleLibrary&);
void register_services(HleLibrary&);
void register_hardware_buffers(HleLibrary&);
}  // namespace venture::hle::android_hle
