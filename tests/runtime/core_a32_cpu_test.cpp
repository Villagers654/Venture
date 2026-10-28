#include <doctest/doctest.h>
#include "a32_test_memory.h"
#include "core/guest_call32.h"
#include "core/wait.h"
#include "core/thread.h"
#include "cpu/a32_backend.h"
#include "cpu/state.h"
#include "cpu/stubs.h"
#include "platform/memory.h"
#include <dynarmic/interface/profiling.h>
#include <algorithm>
#include <thread>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <bit>
#include <vector>

using namespace venture;
namespace {
constexpr std::uint32_t svc = 0xef000000 | kSvcReturnToHost;
constexpr GuestAddr data = test::A32Memory::base + 0x8000;
// Reserve filler code outside the small fixture. The registration callback
// measures actual emitted bytes, so this test cannot silently miss cache growth
// if the emitter's block size or initial commitment changes.
constexpr GuestAddr filler_base = test::A32Memory::base + test::A32Memory::size;
constexpr std::uint64_t filler_size = 16 * 1024 * 1024 + 4096;
struct FillerCode {
  FillerCode() {
    const auto mapped = AddressSpace::get().map(filler_base, filler_size,
        kProtRead | kProtWrite | kProtExec, kMapPrivate | kMapAnonymous | kMapFixedNoReplace,
        nullptr, 0, "[A32 cache growth fixture]");
    if (mapped != static_cast<std::int64_t>(filler_base))
      throw std::runtime_error("cannot map A32 cache growth code");
  }
  ~FillerCode() { AddressSpace::get().unmap(filler_base, filler_size); }
};
struct TranslationSpan {
  inline static thread_local TranslationSpan* current = nullptr;
  std::uintptr_t begin = UINTPTR_MAX, end = 0;
  unsigned blocks = 0;
  static void emitted(const void* first, const void* last, std::string_view) {
    if (!current) return;
    current->begin = std::min(current->begin, reinterpret_cast<std::uintptr_t>(first));
    current->end = std::max(current->end, reinterpret_cast<std::uintptr_t>(last));
    ++current->blocks;
  }
  TranslationSpan() { current = this; Dynarmic::SetCodeRegistrationCallback(emitted); }
  ~TranslationSpan() { Dynarmic::SetCodeRegistrationCallback(nullptr); current = nullptr; }
  std::size_t size() const { return end > begin ? end - begin : 0; }
};
GuestAddr stub(test::A32Memory& m, unsigned offset, GuestStub kind, std::uint32_t id = 0) {
  auto code = guest_stub(GuestIsa::a32, kind, id);
  std::memcpy(g2h(m.base + offset), code.words.data(), code.count * 4);
  return m.base + offset;
}
}
TEST_CASE("A32 ARM and Thumb execution, interworking and full state") {
  test::A32Memory m;
  auto backend = make_a32_backend({4, m.query}); auto cpu = backend->create_cpu();
  auto arm = m.arm(0, {0xe0800001, svc}); // add r0,r0,r1
  cpu->set_x(0, 40); cpu->set_x(1, 2); cpu->set_pc(arm);
  CHECK(cpu->run() == CpuExit::Svc); CHECK(cpu->x(0) == 42); CHECK(cpu->svc_immediate() == kSvcReturnToHost);
  auto thumb = m.thumb(0x100, {0x3001, 0x4770}); // adds r0,#1; bx lr
  auto caller = m.arm(0x200, {0xe12fff33, svc}); // blx r3
  cpu->set_x(0, 41); cpu->set_x(3, thumb); cpu->set_pc(caller);
  CHECK(cpu->run() == CpuExit::Svc); CHECK(cpu->x(0) == 42); CHECK((cpu->pstate() & 0x20) == 0);
  cpu->set_pc(thumb); CHECK(cpu->pc() == (thumb & ~1ull)); CHECK((cpu->pstate() & 0x20) != 0);
  CHECK_THROWS_AS(cpu->x(16), std::out_of_range); CHECK_THROWS_AS(cpu->set_x(30, 1), std::out_of_range);
  CHECK_THROWS_AS(cpu->set_pc(1ull << 32), std::overflow_error);
  for (int i = 0; i < 15; ++i) cpu->set_x(i, i * 17);
  for (int i = 0; i < 32; ++i) cpu->set_v(i, {0x1234567800000000ull + i, 0});
  cpu->set_pstate(0xa4008c30); // flags, Thumb and split IT bits.
  cpu->set_fpcr(0xa140009f); cpu->set_tpidrro(0x12345000); cpu->set_tpidr(0x56789000);
  CpuState32 saved(*cpu);
  cpu->set_pc(arm); cpu->set_fpcr(0); cpu->set_tpidr(0); cpu->set_tpidrro(0);
  for (int i = 0; i < 15; ++i) cpu->set_x(i, 0);
  for (int i = 0; i < 32; ++i) cpu->set_v(i, {});
  saved.restore(*cpu); CpuState32 restored(*cpu);
  CHECK(restored.r == saved.r); CHECK(restored.d == saved.d); CHECK(restored.cpsr == saved.cpsr);
  CHECK(restored.fpscr == saved.fpscr); CHECK(restored.tpidruro == saved.tpidruro); CHECK(restored.tpidrurw == saved.tpidrurw);
}
namespace {
struct A32AccessRace {
    inline static GuestAddr target;
    inline static std::uint32_t trigger;
    inline static bool armed, succeeded, readonly;
    static bool query(GuestAddr p, std::uint64_t n, std::uint32_t prot) noexcept {
      const bool allowed = test::A32Memory::query(p, n, prot);
      if (allowed && armed && p == target && prot == trigger) {
        armed = false;
        std::thread mutation([p] {
          succeeded = readonly ? platform::protect(align_down(p, 4096), 4096, kProtRead)
                               : platform::decommit(align_down(p, 4096), 4096);
        });
        mutation.join(); // deterministic: the old query succeeded, the access cannot
      }
      return allowed;
    }
  };

}
TEST_CASE("A32 precise read, write, execute faults and undefined instruction") {
  test::A32Memory m; auto backend = make_a32_backend({2, m.query}); auto cpu = backend->create_cpu();
  auto load = m.arm(0, {0xe3a0102a, 0xe5902000, 0xe2811001, svc});
  cpu->set_pc(load); cpu->set_x(0, 0); cpu->set_x(2, 0x1234);
  CHECK(cpu->run() == CpuExit::MemoryFault); CHECK(cpu->fault().address == 0);
  CHECK(cpu->pc() == load + 4); CHECK(cpu->x(1) == 42); CHECK(cpu->x(2) == 0x1234);
  auto write = m.arm(0x100, {0xe5801000, svc});
  m.protect(8, kProtRead); cpu->set_pc(write); cpu->set_x(0, data); cpu->set_x(1, 99);
  CHECK(cpu->run() == CpuExit::MemoryFault); CHECK(cpu->fault().write); CHECK(cpu->pc() == write);
  CHECK(*g2h<std::uint32_t>(data) == 0);
  cpu->set_pc(data); CHECK(cpu->run() == CpuExit::MemoryFault); CHECK(cpu->fault().execute);
  auto undef = m.arm(0x200, {0xe7f000f0, svc}); cpu->set_pc(undef);
  CHECK(cpu->run() == CpuExit::UndefinedInstruction); CHECK(cpu->pc() == undef);
}
TEST_CASE("A32 ARM and Thumb VFPv3 D32 and NEON") {
  test::A32Memory m; auto backend = make_a32_backend({2, m.query}); auto cpu = backend->create_cpu();
  auto ret = stub(m, 0x200, GuestStub::return_to_host);
  auto arm = m.arm(0, {0xf2000840, 0xee7efbad, 0xe12fff1e});
  auto thumb = m.thumb(0x100, {0xef00, 0x0840, 0xee7e, 0xfbad, 0x4770});
  for (auto p : {arm, thumb}) {
    cpu->set_v(0, {0x0101010101010101, 0}); cpu->set_v(1, {0x0202020202020202, 0});
    cpu->set_v(30, {std::bit_cast<std::uint64_t>(1.5), 0});
    cpu->set_v(29, {std::bit_cast<std::uint64_t>(2.25), 0});
    cpu->set_lr(ret); cpu->set_pc(p);
    CHECK(cpu->run() == CpuExit::Svc);
    CHECK(cpu->v(0).lo == 0x0202020202020202); CHECK(cpu->v(1).lo == 0x0404040404040404);
    CHECK(std::bit_cast<double>(cpu->v(31).lo) == 3.75);
  }
}
TEST_CASE("A32 LDREX STREX, kuser cmpxchg ABI and barrier") {
  test::A32Memory m; m.install_kuser();
  auto backend = make_a32_backend({4, m.query});
  // ldrex r2,[r0]; add r2,#1; strex r3,r2,[r0]; cmp r3,#0; bne retry;
  // subs r1,#1; bne retry; svc return
  auto increment = m.arm(0, {0xe1902f9f, 0xe2822001, 0xe1803f92, 0xe3530000,
                            0x1afffffa, 0xe2511001, 0x1afffff8, svc});
  std::array<std::thread, 2> workers;
  for (auto& worker : workers) worker = std::thread([&] {
    auto cpu = backend->create_cpu(); cpu->set_x(0, data); cpu->set_x(1, 10000); cpu->set_pc(increment);
    CHECK(cpu->run() == CpuExit::Svc);
  });
  for (auto& worker : workers) worker.join();
  CHECK(*g2h<std::uint32_t>(data) == 20000);
  auto cpu = backend->create_cpu(); auto ret = stub(m, 0x100, GuestStub::return_to_host);
  cpu->set_lr(ret); cpu->set_x(0, 20000); cpu->set_x(1, 42); cpu->set_x(2, data); cpu->set_pc(kKuserAddress + 0xfc0);
  CHECK(cpu->run() == CpuExit::Svc); CHECK(cpu->x(0) == 0); CHECK((cpu->pstate() & (1u << 29)) != 0);
  CHECK(*g2h<std::uint32_t>(data) == 42);
  cpu->set_x(0, 20000); cpu->set_pc(kKuserAddress + 0xfc0);
  CHECK(cpu->run() == CpuExit::Svc); CHECK(cpu->x(0) != 0); CHECK((cpu->pstate() & (1u << 29)) == 0);
  CHECK(*g2h<std::uint32_t>(data) == 42);
  cpu->set_pc(kKuserAddress + 0xfa0); CHECK(cpu->run() == CpuExit::Svc);
  // A DMB must preserve every guest flag, including flags consumed in-block.
  auto flags = m.arm(0x200, {0xe3500000, 0xf57ff05b, 0x03a01007, 0x13a01009, svc});
  for (unsigned value : {0u, 1u}) {
    cpu->set_x(0, value); cpu->set_pc(flags); CHECK(cpu->run() == CpuExit::Svc);
    CHECK(cpu->x(1) == (value == 0 ? 7 : 9));
    CHECK((cpu->pstate() & 0xf0000000u) == (value == 0 ? 0x60000000u : 0x20000000u));
  }
  cpu->set_tpidrro(0x34567000); cpu->set_pc(kKuserAddress + 0xfe0);
  CHECK(cpu->run() == CpuExit::Svc); CHECK(cpu->x(0) == 0x34567000);
  CHECK(*g2h<std::uint32_t>(kKuserAddress + 0xffc) == 3);
}
TEST_CASE("A32 invalidation replaces every ARM and Thumb block in a range") {
  test::A32Memory m; auto backend = make_a32_backend({1, m.query}); auto cpu = backend->create_cpu();
  std::vector<GuestAddr> entries;
  for (unsigned i = 0; i < 6; ++i) {
    entries.push_back(m.arm(i * 32, {0xe3a00007, svc}));
    entries.push_back(m.thumb(0x200 + i * 32, {0x2007, 0x4770}));
  }
  auto outside = m.arm(0x2000, {0xe3a00007, svc});
  auto ret = stub(m, 0x3000, GuestStub::return_to_host);
  auto run = [&](GuestAddr p, unsigned expected) {
    cpu->set_lr(ret); cpu->set_pc(p);
    REQUIRE(cpu->run() == CpuExit::Svc); CHECK(cpu->x(0) == expected);
  };
  for (auto p : entries) run(p, 7);
  run(outside, 7);
  for (auto p : entries) {
    if (p & 1) *g2h<std::uint16_t>(p & ~1ull) = 0x2009;
    else *g2h<std::uint32_t>(p) = 0xe3a00009;
  }
  *g2h<std::uint32_t>(outside) = 0xe3a00009;
  backend->invalidate(m.base, 4096);
  for (auto p : entries) run(p, 9);
  run(outside, 7); // Blocks outside the range keep their cached translation.
  backend->invalidate(outside, 4);
  run(outside, 9);
  // Insert a wider block after several overlapping suffix blocks. ICL must
  // index every covered byte as well as handle broad retirement queries.
  std::array<std::uint32_t, 16> wide;
  wide.fill(0xe1a00000); wide[14] = 0xe3a00007; wide[15] = svc;
  auto start = m.base + 0x500;
  std::memcpy(g2h(start), wide.data(), sizeof(wide));
  for (unsigned i = 1; i <= 6; ++i) run(start + i * 4, 7);
  run(start, 7);
  *g2h<std::uint32_t>(start + 56) = 0xe3a00009;
  backend->invalidate(start, 4);
  run(start, 9);
  for (unsigned i = 1; i <= 6; ++i) run(start + i * 4, 7);
  backend->invalidate(start + 56, 4);
  for (unsigned i = 1; i <= 6; ++i) run(start + i * 4, 9);
}
TEST_CASE("A32 fault keeps register, SP, VFP and flag writes made earlier in its block") {
  // Get/set elimination with precise aborts (see the A64 case): earlier writes survive.
  test::A32Memory m; auto backend = make_a32_backend({2, m.query}); auto cpu = backend->create_cpu();
  auto block = m.arm(0x300, {0xe3a0102a,   // mov r1,#42
                             0xee001a10,   // vmov s0,r1
                             0xe24dd010,   // sub sp,sp,#16
                             0xe351002a,   // cmp r1,#42 (Z,C)
                             0xe5902000,   // ldr r2,[r0]
                             0xe2811001,   // add r1,r1,#1
                             0xee001a10,   // vmov s0,r1
                             0xe28dd010,   // add sp,sp,#16
                             0xe3510064,   // cmp r1,#100 (N)
                             svc});
  cpu->set_x(13, 0x10000); cpu->set_pstate(0x10); cpu->set_v(0, {0x700000007ull, 0});
  cpu->set_x(0, 0); cpu->set_x(1, 0); cpu->set_x(2, 9); cpu->set_pc(block);
  REQUIRE(cpu->run() == CpuExit::MemoryFault);
  CHECK(cpu->pc() == block + 16); CHECK(cpu->fault().address == 0);
  CHECK(cpu->x(1) == 42); CHECK(cpu->x(2) == 9);
  CHECK(cpu->v(0).lo == 0x70000002aull);
  CHECK(cpu->sp() == 0x10000 - 16);
  CHECK((cpu->pstate() >> 28) == 0b0110);
  *g2h<std::uint32_t>(data) = 5;
  cpu->set_x(13, 0x10000); cpu->set_x(0, data); cpu->set_pc(block);
  REQUIRE(cpu->run() == CpuExit::Svc);
  CHECK(cpu->x(1) == 43); CHECK(cpu->x(2) == 5);
  CHECK(cpu->v(0).lo == 0x70000002bull);
  CHECK(cpu->sp() == 0x10000);
  CHECK((cpu->pstate() >> 28) == 0b1000);
}
