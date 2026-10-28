#pragma once
#include <cstdint>
namespace venture::hle::libc {
// bionic/libc/include/time.h, AArch64 LP64: nine int fields, padding, long, pointer.
struct GuestTm {
  std::int32_t sec, min, hour, mday, mon, year, wday, yday, isdst;
  std::int64_t gmtoff;
  const char* zone;
};
static_assert(sizeof(GuestTm) == 56);
}  // namespace venture::hle::libc
