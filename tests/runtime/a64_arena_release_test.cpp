#include <doctest/doctest.h>
#include "core/address_space.h"
#include "core/process_runtime.h"
#include "platform/low_memory.h"
#include "platform/memory.h"

using namespace venture;
TEST_CASE("A64 selection releases every early reservation and preserves foreign mappings") {
  constexpr GuestAddr foreign = 0x40000000, guard = 0x100000000ull;
  constexpr std::uint32_t canary = 0xabcdef12;
  // Windows may already have placed something at either fixed address in a
  // fresh process (ASLR); the test then has nothing to prove.
  for (const GuestAddr address : {foreign, guard}) {
    if (platform::reserve(address, 65536, true) != address) {
      MESSAGE("Address ", address, " is taken before the test; skipped");
      return;
    }
    platform::release(address, 65536);
  }
  REQUIRE(platform::reserve(foreign, 65536, true) == foreign);
  REQUIRE(platform::commit(foreign, 4096, kProtRead | kProtWrite));
  *g2h<std::uint32_t>(foreign) = canary;
  REQUIRE(platform::protect(foreign, 4096, kProtRead));
  bool host_guard = false;
  SUBCASE("free guard") {}
  SUBCASE("host-owned writable guard") { host_guard = true; }
  if (host_guard) {
    REQUIRE(platform::reserve(guard, 65536, true) == guard);
    REQUIRE(platform::commit(guard, 65536, kProtRead | kProtWrite));
    *g2h<std::uint32_t>(guard) = canary;
  }
  platform::pre_reserve_low_memory();
  const auto ours = platform::reserve_low_memory();
  REQUIRE_FALSE(ours.empty());
  CHECK(platform::reserve_low_memory_guard() == !host_guard);
  select_process_isa(GuestIsa::a64);
  // Every original placeholder is now available to ordinary host allocation.
  // The saved list also detects a forgotten hole rather than probing one page.
  for (const auto r : ours) {
    REQUIRE(platform::reserve(r.start, r.size, true) == r.start);
    platform::release(r.start, r.size);
  }
  CHECK(*g2h<std::uint32_t>(foreign) == canary);
  CHECK(platform::memory_accessible(foreign, 4096, false));
  CHECK_FALSE(platform::memory_accessible(foreign, 4096, true));
  if (host_guard) {
    CHECK(platform::memory_accessible(guard, 65536, true));
    CHECK(*g2h<std::uint32_t>(guard) == canary);
  } else {
    REQUIRE(platform::reserve(guard, 65536, true) == guard);
  }
  platform::release(guard, 65536);
  platform::release(foreign, 65536);
}
