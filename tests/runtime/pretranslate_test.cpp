#include <doctest/doctest.h>
#include "common/environment.h"
#include "core/address_space.h"
#include "core/thread_runtime.h"
#include "cpu/portable_cache.h"
#include "cpu/pretranslate.h"
#include <chrono>
#include <cstring>
#include <thread>

using namespace venture;
namespace {
struct Code {
  GuestAddr address;
  explicit Code(std::initializer_list<std::uint32_t> words) {
    address = AddressSpace::get().allocate(4096, kProtRead | kProtWrite | kProtExec, "[pretranslate test]");
    if (!address) throw std::bad_alloc();
    std::memcpy(g2h(address), words.begin(), words.size() * 4);
  }
  ~Code() { AddressSpace::get().free(address); }
};
bool eventually(const auto& condition) {
  for (int i = 0; i < 1000; ++i) {
    if (condition()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}
}

// ctest runs this with VENTURE_JIT_PTC_DIR set to an empty directory.
TEST_CASE("background translation caches the callees and return sites of executed code before they run") {
  REQUIRE(environment::getenv("VENTURE_JIT_PTC_DIR"));
  // 0x00: stp x29,x30,[sp,#-16]!  0x04: cbz x0, 0x14  0x08: bl 0x20  0x0c: add x0,x0,#1
  // 0x10: b 0x18                  0x14: bl 0x28      0x18: ldp x29,x30,[sp],#16  0x1c: ret
  // 0x20: mov x0,#41; ret         0x28: mov x0,#7; ret
  Code code{0xa9bf7bfd, 0xb4000080, 0x94000006, 0x91000400, 0x14000002, 0x94000005, 0xa8c17bfd, 0xd65f03c0,
            0xd2800520, 0xd65f03c0, 0xd28000e0, 0xd65f03c0};
  auto* cache = PortableBlockCache::get();
  REQUIRE(cache);
  {
    GuestThread first(30); first.attach_to_current_host_thread();
    // Runs only the entry block and the x0 == 0 path: the other branch, its callee and return site never execute here.
    CHECK(first.call(code.address, {0}) == 7);
  }
  // A64 descriptors are the PC with the default FPCR mode bits (zero).
  CHECK(eventually([&] { return cache->contains(code.address + 0x08) && cache->contains(code.address + 0x20) && cache->contains(code.address + 0x0c); }));
  // Another guest thread installs those blocks and runs them correctly.
  std::thread([&] {
    GuestThread second(31); second.attach_to_current_host_thread();
    CHECK(second.call(code.address, {1}) == 42);
    CHECK(second.call(code.address, {0}) == 7);
  }).join();
}

namespace {
struct Bytes {
  std::vector<std::uint8_t> data;
  void u8(std::uint8_t v) { data.push_back(v); }
  void u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) data.push_back(std::uint8_t(v >> (8 * i))); }
  void s32(std::int64_t v) { u32(std::uint32_t(std::int32_t(v))); }
};
}

TEST_CASE("function entries come from .eh_frame_hdr's table, or from .eh_frame when the header has none") {
  const GuestAddr base = AddressSpace::get().allocate(4096, kProtRead | kProtWrite, "[unwind test]");
  REQUIRE(base);
  const GuestAddr eh_frame = base + 0x800, bare = base + 0xc00, table = base + 0xd00;
  const std::vector<std::pair<std::uint64_t, std::uint64_t>> code{{base, base + 0x400}}, segments{{base, base + 0x1000}};
  // .eh_frame: a "zR" CIE (FDE pointers pcrel|sdata4) and FDEs for 0x100, 0x200 and a non-code 0x900; then the terminator.
  Bytes f;
  f.u32(16); f.u32(0); f.u8(1); f.u8('z'); f.u8('R'); f.u8(0); f.u8(4); f.u8(0x78); f.u8(30); f.u8(1); f.u8(0x1b);
  while (f.data.size() < 20) f.u8(0);
  for (const std::uint64_t target : {0x100u, 0x200u, 0x900u}) {
    const auto record = f.data.size();
    f.u32(16); f.u32(std::uint32_t(record + 4));  // back to the CIE at 0
    f.s32(std::int64_t(base + target) - std::int64_t(eh_frame + f.data.size()));
    f.u32(0x40); f.u8(0); f.u8(0); f.u8(0); f.u8(0);
  }
  f.u32(0);
  std::memcpy(g2h(eh_frame), f.data.data(), f.data.size());
  // A header without its table (fde_count and table omitted), as some linkers write.
  Bytes h;
  h.u8(1); h.u8(0x1b); h.u8(0xff); h.u8(0xff); h.s32(std::int64_t(eh_frame) - std::int64_t(bare + 4));
  std::memcpy(g2h(bare), h.data.data(), h.data.size());
  CHECK(pretranslate::detail::function_entries(bare, 8, code, segments) == std::vector<std::uint64_t>{base + 0x100, base + 0x200});
  // The usual header: a sorted table of (initial location, FDE), datarel from the header.
  Bytes t;
  t.u8(1); t.u8(0x1b); t.u8(0x03); t.u8(0x3b); t.s32(std::int64_t(eh_frame) - std::int64_t(table + 4)); t.u32(2);
  t.s32(0x100 - std::int64_t(table - base)); t.s32(20 - std::int64_t(table - eh_frame));
  t.s32(0x200 - std::int64_t(table - base)); t.s32(40 - std::int64_t(table - eh_frame));
  std::memcpy(g2h(table), t.data.data(), t.data.size());
  CHECK(pretranslate::detail::function_entries(table, t.data.size(), code, segments) == std::vector<std::uint64_t>{base + 0x100, base + 0x200});
  AddressSpace::get().free(base);
}
