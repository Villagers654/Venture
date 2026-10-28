// ARM RTABI32 4.3.4: all twelve helpers return void. memset takes (dest,n,c),
// unlike ISO C memset. The 4/8 suffix promises alignment, not a length multiple.
#include "hle/libc/libc_a.h"
#include "core/process.h"
#include "core/hle_runtime.h"
#include "platform/memory.h"
#include <cstring>
namespace venture::hle::libc {
namespace {
void access(platform::NativeMemoryOp op, void* dest, const void* src, std::uint64_t n, int value = 0) {
  check_memory_range(h2g(dest), n, true);
  if (op != platform::NativeMemoryOp::fill) check_memory_range(h2g(src), n, false);
  std::uint64_t result;
  platform::NativeMemoryFault fault;
  if (!platform::native_memory(op, h2g(dest), h2g(src), n, value, result, fault))
    throw MemoryFault{fault.address, fault.write, fault.bus};
}
void copy(void* dest, const void* src, std::uint64_t n) { access(platform::NativeMemoryOp::copy, dest, src, n); }
void move(void* dest, const void* src, std::uint64_t n) { access(platform::NativeMemoryOp::move, dest, src, n); }
void set(void* dest, std::uint64_t n, int c) { access(platform::NativeMemoryOp::fill, dest, nullptr, n, c); }
void clear(void* dest, std::uint64_t n) { access(platform::NativeMemoryOp::fill, dest, nullptr, n); }
}
void register_aeabi(HleLibrary& lib) {
  if (process_config().isa() != GuestIsa::a32) return;
  for (const char* suffix : {"", "4", "8"}) {
    bind<&copy>(lib, std::string("__aeabi_memcpy") + suffix, Execution::InJit);
    bind<&move>(lib, std::string("__aeabi_memmove") + suffix, Execution::InJit);
    bind<&set>(lib, std::string("__aeabi_memset") + suffix, Execution::InJit);
    bind<&clear>(lib, std::string("__aeabi_memclr") + suffix, Execution::InJit);
  }
}
}
