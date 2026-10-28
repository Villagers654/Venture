#include "cpu/kuser.h"
#include "core/address_space.h"
#include <algorithm>
#include <cstring>

namespace venture {
std::span<const std::uint32_t, 1024> kuser_page() {
  static const auto page = [] {
    std::array<std::uint32_t, 1024> p;
    p.fill(0xe7f000f0); // UDF: unused helper slots must fail visibly.
    const auto put = [&](unsigned offset, std::initializer_list<std::uint32_t> words) {
      std::copy(words.begin(), words.end(), p.begin() + offset / 4);
    };
    // Linux arch/arm/kernel/entry-armv.S, __kuser_helper_start:
    // r0=old, r1=new, r2=address; only r0/r3/ip/flags may change. C=success.
    // The loop lives out of line to fit the fixed 32-byte public helper slots.
    put(0xf00, {0xf57ff05b,       // dmb ish
                0xe1923f9f,     // ldrex r3,[r2]
                0xe0533000,     // subs r3,r3,r0
                0x1a000003,     // bne failure
                0xe182cf91,     // strex ip,r1,[r2]
                0xe35c0000,     // cmp ip,#0
                0x1afffff9,     // bne retry (ldrex)
                0xe3a03000,     // mov r3,#0
                0xe2730000,     // rsbs r0,r3,#0 (C iff equal)
                0xf57ff01f,     // clrex (also clear after compare mismatch)
                0xf57ff05b,     // dmb ish
                0xe12fff1e});   // bx lr
    put(0xfa0, {0xf57ff05b, 0xe12fff1e}); // __kuser_memory_barrier
    put(0xfc0, {0xeaffffce});              // b 0xf00: __kuser_cmpxchg
    put(0xfe0, {0xee1d0f70, 0xe12fff1e}); // mrc p15,0,r0,c13,c0,3; bx lr
    // Documentation/arm/kernel_user_helpers.txt: 3 adds DMB; 5 adds cmpxchg64.
    p[0xffc / 4] = 3;
    return p;
  }();
  return page;
}
int install_kuser_page() {
  AddressSpace::FileReader read = [](std::uint64_t offset, void* out, std::uint64_t size) {
    if (offset > 4096 || size > 4096 - offset) return false;
    std::memcpy(out, reinterpret_cast<const std::byte*>(kuser_page().data()) + offset, size);
    return true;
  };
  auto mapped = AddressSpace::get().map(kKuserAddress, 4096, kProtRead | kProtExec,
      kMapPrivate | kMapFixedNoReplace, &read, 0, "[kuser]");
  return mapped < 0 ? static_cast<int>(mapped) : 0;
}
} // namespace venture
