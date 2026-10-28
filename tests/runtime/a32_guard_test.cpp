#include <doctest/doctest.h>
#include "core/address_space.h"
#include "core/low_address_space.h"
#include "cpu/a32_backend.h"
#include "platform/low_memory.h"
#include "platform/memory.h"
#include <array>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <string>
extern "C" bool venture_test_guard_reserved;
#endif

using namespace venture;
namespace {
bool reserve_foreign_guard(GuestAddr guard) {
#ifdef _WIN32
  if (venture_test_guard_reserved) return true;
  if (platform::reserve(guard, 65536, true) == guard) return true;
  MEMORY_BASIC_INFORMATION info{};
  char module[MAX_PATH] = "";
  VirtualQuery(reinterpret_cast<void *>(guard), &info, sizeof(info));
  GetMappedFileNameA(GetCurrentProcess(), reinterpret_cast<void *>(guard), module, sizeof(module));
  MESSAGE("4 GiB already held: allocation base " << info.AllocationBase << ", state 0x" << std::hex
      << info.State << ", type 0x" << info.Type << ", mapped file '" << module << "'");
  return false;
#else
  return platform::reserve(guard, 65536, true) == guard;
#endif
}
}

TEST_CASE("A32 occupied wrap guard preserves host memory and checks wide accesses") {
  constexpr GuestAddr guard = 0x100000000ull;
  constexpr std::uint64_t canary = 0x123456789abcdef0;
  // This executable deliberately omits the runtime's early arena reservation.
  // Its own entry takes the guard instead, simulating a DLL/heap allocation that
  // predates the arena. Old startup rejected this mapping.
  REQUIRE(reserve_foreign_guard(guard));
  REQUIRE(platform::commit(guard, 65536, kProtRead | kProtWrite));
  *g2h<std::uint64_t>(guard) = canary;
  auto& as = AddressSpace::get();
  REQUIRE_NOTHROW(as.initialize(GuestIsa::a32));
  CHECK_FALSE(platform::reserve_low_memory_guard());
  CHECK_FALSE(as.is_guest_accessible(guard, 1, false));
  CHECK(platform::memory_accessible(guard, 65536, true));
  CHECK(*g2h<std::uint64_t>(guard) == canary);
  REQUIRE(as.map(guard - 4096, 4096, kProtRead | kProtWrite,
      kMapPrivate | kMapAnonymous | kMapFixedNoReplace, nullptr, 0, "[guard test]") == guard - 4096);
  auto code = as.allocate(4096, kProtRead | kProtWrite | kProtExec, "[guard test code]");
  REQUIRE(code != 0);
  // ARMv7: ldrd r0,r1,[r2]; svc #0x5151; strd r0,r1,[r2]; svc.
  // Unlike VLDR, Dynarmic emits one 64-bit access for LDRD, so this tests the
  // host load/store that could extend into the foreign guard allocation.
  const std::array words{0xe1c200d0u, 0xef005151u, 0xe1c200f0u, 0xef005151u,
      // ldrex r2,[r0]; add r2,#1; strex r3,r2,[r0]; cmp r3,#0;
      // bne retry; subs r1,#1; bne retry; svc #0x5151.
      0xe1902f9fu, 0xe2822001u, 0xe1803f92u, 0xe3530000u,
      0x1afffffau, 0xe2511001u, 0x1afffff8u, 0xef005151u};
  std::memcpy(g2h(code), words.data(), sizeof(words));
  auto backend = make_a32_backend({1, low32::query});
  auto cpu = backend->create_cpu();
  constexpr GuestAddr inside = guard - 16, crossing = guard - 4;
  *g2h<std::uint64_t>(inside) = canary;
  cpu->set_x(2, inside); cpu->set_pc(code);
  REQUIRE(cpu->run() == CpuExit::Svc);
  CHECK((cpu->x(0) | (cpu->x(1) << 32)) == canary);
  cpu->set_x(0, 42); cpu->set_x(1, 0); cpu->set_pc(code + 8);
  REQUIRE(cpu->run() == CpuExit::Svc);
  CHECK(*g2h<std::uint64_t>(inside) == 42);
  cpu->set_x(0, inside); cpu->set_x(1, 3); cpu->set_pc(code + 16);
  REQUIRE(cpu->run() == CpuExit::Svc);
  CHECK(*g2h<std::uint64_t>(inside) == 45);
  for (bool write : {false, true}) {
    *g2h<std::uint32_t>(crossing) = 0xdeadbeef;
    cpu->set_x(2, crossing); cpu->set_pc(code + (write ? 8 : 0));
    REQUIRE(cpu->run() == CpuExit::MemoryFault);
    CHECK(cpu->fault().address == crossing);
    CHECK(cpu->fault().write == write);
    CHECK(*g2h<std::uint32_t>(crossing) == 0xdeadbeef);
    CHECK(*g2h<std::uint64_t>(guard) == canary);
  }
  // The host mapping remains writable even after CPUs have run and faulted.
  *g2h<std::uint64_t>(guard) = 7;
  CHECK(*g2h<std::uint64_t>(guard) == 7);
}
