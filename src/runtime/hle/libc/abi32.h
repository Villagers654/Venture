#pragma once
#include "core/a32_abi.h"

namespace venture::hle::libc::abi32 {
// Explicit adapters only: F accepts ILP32 wire records or deliberately stages
// widened Out cells. The descriptor still controls scalar/softfp allocation.
template<auto F> void adapt(const char* name, const char* library = "libc.so") {
  a32::register_adapter(library, name, [](CallContext& c) { a32::invoke(F, c); });
}
// bionic bits/timespec.h and sys/time.h: ARM time_t and long are 32-bit.
struct Timespec { std::int32_t sec, nsec; };
struct Timeval { std::int32_t sec, usec; };
static_assert(sizeof(Timespec) == 8 && sizeof(Timeval) == 8);
}
