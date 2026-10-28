#pragma once
#include <cstddef>
#include <cstdint>
namespace venture::hle::libc {
// zlib.h under Android's LP64 ABI. Windows uLong is 32-bit; never reinterpret
// this as a host z_stream. state is an opaque HLE bridge owned until *End().
struct GuestZStream {
  unsigned char* next_in;
  std::uint32_t avail_in;
  std::uint64_t total_in;
  unsigned char* next_out;
  std::uint32_t avail_out;
  std::uint64_t total_out;
  const char* msg;
  void* state;
  std::uint64_t zalloc, zfree, opaque;
  std::int32_t data_type;
  std::uint64_t adler, reserved;
};
static_assert(sizeof(GuestZStream) == 112 && offsetof(GuestZStream, total_in) == 16 &&
              offsetof(GuestZStream, adler) == 96);
struct GuestGzHeader {
  std::int32_t text;
  std::uint64_t time;
  std::int32_t xflags, os;
  unsigned char* extra;
  std::uint32_t extra_len, extra_max;
  unsigned char* name;
  std::uint32_t name_max;
  unsigned char* comment;
  std::uint32_t comm_max;
  std::int32_t hcrc, done;
};
static_assert(sizeof(GuestGzHeader) == 80 && offsetof(GuestGzHeader, comment) == 56);
}  // namespace venture::hle::libc
