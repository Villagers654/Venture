#pragma once
#include "core/guest.h"
#include <cstddef>
#include <cstdint>

namespace venture { class HleLibrary; }
namespace venture::hle::libc::zlib32 {
// Android NDK r30 zlib.h / zconf.h, armv7a clang record-layout probe:
// pointers and uLong are 4 bytes; z_stream is 56 bytes, aligned to 4.
struct WireStream {
  std::uint32_t next_in, avail_in, total_in, next_out, avail_out, total_out;
  std::uint32_t msg, state, zalloc, zfree, opaque;
  std::int32_t data_type;
  std::uint32_t adler, reserved;
};
static_assert(sizeof(WireStream) == 56 && offsetof(WireStream, state) == 28 &&
              offsetof(WireStream, adler) == 48);
void register_adapters(HleLibrary&);
}
