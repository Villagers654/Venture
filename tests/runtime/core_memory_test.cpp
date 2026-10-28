#include <doctest/doctest.h>
#include "core/address_space.h"
#include "core/error.h"
#include "platform/memory.h"
#include <array>
#include <cstring>
using namespace venture;
TEST_CASE("grow-down VMAs map only requested pages and honor the stack limit") {
  auto& space = AddressSpace::get();
  const auto old_limit = space.stack_limit();
  struct Restore { AddressSpace& s; std::uint64_t n; ~Restore() { s.set_stack_limit(n); } } restore{space, old_limit};
  space.set_stack_limit(3 * kGuestPageSize);
  auto p = space.map(0, kGuestPageSize, kProtRead | kProtWrite,
      kMapPrivate | kMapAnonymous | kMapGrowDown | kMapStack, nullptr, 0, "[grow-down test]");
  REQUIRE(p > 0);
  struct Unmap { AddressSpace& s; GuestAddr p; ~Unmap() {
    MemoryRegion r; if (s.find(p, &r)) s.unmap(r.start, r.size);
  } } unmap{space, GuestAddr(p)};
  MemoryRegion r;
  REQUIRE(space.find(p, &r));
  CHECK(r.size == kGuestPageSize);
  CHECK(r.grow_down);
  CHECK_FALSE(space.is_guest_accessible(p - 1, 1, true));
  *g2h<std::uint64_t>(p) = 47;
  REQUIRE(space.grow_stack(p - 1, kProtWrite));
  CHECK(*g2h<std::uint64_t>(p) == 47);
  CHECK(*g2h<std::uint64_t>(p - kGuestPageSize) == 0);
  REQUIRE(space.grow_stack(p - 2 * kGuestPageSize, kProtRead));
  CHECK_FALSE(space.grow_stack(p - 2 * kGuestPageSize - 1, kProtWrite));
  REQUIRE(space.find(p, &r));
  CHECK(r.start == p - 2 * kGuestPageSize);
  CHECK(r.size == 3 * kGuestPageSize);
  CHECK_FALSE(space.is_guest_accessible(r.start - 1, 1, true));
}

TEST_CASE("grow-down guard gap blocks collision and MAP_STACK alone does not grow") {
  auto& space = AddressSpace::get();
  auto p = space.map(0, kGuestPageSize, kProtRead | kProtWrite,
      kMapPrivate | kMapAnonymous | kMapGrowDown, nullptr, 0, "[guard-gap stack]");
  REQUIRE(p > 0);
  const auto lower = p - kStackGuardGap - 2 * kGuestPageSize;
  REQUIRE(space.map(lower, kGuestPageSize, kProtRead | kProtWrite,
      kMapPrivate | kMapAnonymous | kMapFixedNoReplace, nullptr, 0, "[guard-gap neighbor]") == lower);
  REQUIRE(space.grow_stack(p - kGuestPageSize, kProtWrite)); // exactly the required gap remains
  CHECK_FALSE(space.grow_stack(p - 2 * kGuestPageSize, kProtWrite));
  CHECK(space.unmap(p - kGuestPageSize, 2 * kGuestPageSize) == 0);
  CHECK(space.unmap(lower, kGuestPageSize) == 0);
  auto plain = space.map(0, kGuestPageSize, kProtRead | kProtWrite,
      kMapPrivate | kMapAnonymous | kMapStack, nullptr, 0, "[MAP_STACK only]");
  REQUIRE(plain > 0);
  CHECK_FALSE(space.grow_stack(plain - 1, kProtWrite));
  CHECK(space.unmap(plain, kGuestPageSize) == 0);
  CHECK(space.map(0, kGuestPageSize, kProtRead,
      kMapShared | kMapAnonymous | kMapGrowDown, nullptr, 0, "[shared stack]") == -err::kEINVAL);
}

TEST_CASE("guarded stores preserve protections and contain destination faults") {
  auto& space = AddressSpace::get();
  const auto p = space.allocate(8192, kProtRead | kProtWrite, "[guarded store test]");
  REQUIRE(p != 0);
  const std::array<std::uint64_t, 2> value{17, 23};
  REQUIRE(platform::guarded_store(p + 4096 - 8, value.data(), sizeof(value)));
  CHECK(*g2h<std::uint64_t>(p + 4096 - 8) == 17);
  CHECK(*g2h<std::uint64_t>(p + 4096) == 23);
  REQUIRE(space.protect(p + 4096, 4096, kProtRead) == 0);
  CHECK_FALSE(platform::guarded_store(p + 4096, value.data(), sizeof(value)));
  CHECK(*g2h<std::uint64_t>(p + 4096) == 23);
  CHECK_FALSE(platform::guarded_store(p + 4096 - 8, value.data(), sizeof(value)));
  CHECK_FALSE(platform::guarded_store(0, value.data(), sizeof(value)));
  CHECK_FALSE(platform::guarded_store(UINT64_MAX - 7, value.data(), sizeof(value)));
  space.free(p);
  CHECK_FALSE(platform::guarded_store(p, value.data(), sizeof(value)));
}
TEST_CASE("core memory 4 KiB splits, protections and fixed replacement") {
  auto& space = AddressSpace::get();
  auto p = space.allocate(4 * 4096, kProtRead | kProtWrite, "[map test]");
  REQUIRE(p != 0);
  *g2h<std::uint32_t>(p) = 42;
  *g2h<std::uint32_t>(p + 2 * 4096) = 99;
  CHECK(space.protect(p + 4096, 4096, kProtRead) == 0);
  std::uint32_t value = 7;
  CHECK_FALSE(platform::write_memory(p + 4096, &value, 4));
  CHECK(space.unmap(p + 4096, 4096) == 0);
  MemoryRegion region;
  CHECK_FALSE(space.find(p + 4096, &region));
  CHECK(space.find(p + 8192, &region));
  CHECK(space.map(p + 8192, 4096, kProtRead | kProtWrite, kMapFixedNoReplace | kMapPrivate | kMapAnonymous, nullptr, 0, "[replace]") == -err::kEEXIST);
  CHECK(space.map(p + 4096, 8192, kProtRead | kProtWrite, kMapFixed | kMapPrivate | kMapAnonymous, nullptr, 0, "[replace]") == static_cast<std::int64_t>(p + 4096));
  CHECK(*g2h<std::uint32_t>(p) == 42);
  CHECK(*g2h<std::uint32_t>(p + 8192) == 0);
  CHECK(space.protect(p + 1, 4096, kProtRead) == -err::kEINVAL);
  CHECK(space.unmap(p + 1, 4096) == -err::kEINVAL);
  space.free(p);
  CHECK_FALSE(space.find(p, &region));
}
TEST_CASE("core memory republishes executable and access state for the changed span only") {
  auto& space = AddressSpace::get();
  auto p = space.allocate(4 * 4096, kProtRead | kProtWrite, "[exec test]");
  REQUIRE(p != 0);
  CHECK_FALSE(space.is_guest_executable(p + 4096));
  CHECK(space.protect(p + 4096, 2 * 4096, kProtRead | kProtExec) == 0);
  CHECK_FALSE(space.is_guest_executable(p));
  CHECK(space.is_guest_executable(p + 4096));
  CHECK(space.is_guest_executable(p + 3 * 4096 - 1));
  CHECK_FALSE(space.is_guest_executable(p + 3 * 4096));
  CHECK(space.is_guest_accessible(p, 4096, true));
  CHECK_FALSE(space.is_guest_accessible(p + 4096, 1, true));
  CHECK(space.is_guest_accessible(p + 3 * 4096, 4096, true));
  CHECK(space.protect(p + 2 * 4096, 4096, kProtRead | kProtWrite) == 0);
  CHECK(space.is_guest_executable(p + 4096));
  CHECK_FALSE(space.is_guest_executable(p + 2 * 4096));
  CHECK(space.is_guest_accessible(p + 2 * 4096, 4096, true));
  CHECK(space.unmap(p + 4096, 4096) == 0);
  CHECK_FALSE(space.is_guest_executable(p + 4096));
  CHECK_FALSE(space.is_guest_accessible(p + 4096, 1, false));
  CHECK(space.is_guest_accessible(p, 4096, true));
  CHECK(space.map(p + 4096, 4096, kProtRead | kProtExec, kMapFixed | kMapPrivate | kMapAnonymous, nullptr, 0, "[exec test]") == static_cast<std::int64_t>(p + 4096));
  CHECK(space.is_guest_executable(p + 4096));
  space.free(p);
  CHECK_FALSE(space.is_guest_executable(p + 4096));
  CHECK_FALSE(space.is_guest_accessible(p, 4 * 4096, false));
}
TEST_CASE("core memory file private mapping and unsupported shared backing") {
  AddressSpace::FileReader reader = [](std::uint64_t offset, void* out, std::uint64_t n) {
    auto* p = static_cast<unsigned char*>(out);
    for (std::uint64_t i = 0; i < n; ++i) p[i] = static_cast<unsigned char>((offset + i) % 251);
    return true;
  };
  auto& space = AddressSpace::get();
  auto result = space.map(0, 8192, kProtRead, kMapPrivate, &reader, 4096, "file-test");
  REQUIRE(result > 0);
  auto p = static_cast<GuestAddr>(result);
  CHECK(*g2h<unsigned char>(p + 4097) == (8193 % 251));
  CHECK(space.map(0, 4096, kProtRead, kMapShared, &reader, 0, "shared-test") == -err::kEOPNOTSUPP);
  CHECK(space.unmap(p, 8192) == 0);
}
TEST_CASE("core memory supports 2 GiB reservations without truncation") {
  auto& space = AddressSpace::get();
  constexpr std::uint64_t bytes = 2ull * 1024 * 1024 * 1024;
  auto p = space.allocate(bytes, kProtNone, "[2 GiB]");
  REQUIRE(p != 0);
  MemoryRegion region;
  REQUIRE(space.find(p + bytes - 1, &region));
  CHECK(region.size == bytes);
  CHECK(space.protect(p, 4096, kProtRead | kProtWrite) == 0);
  CHECK(space.protect(p + bytes - 4096, 4096, kProtRead | kProtWrite) == 0);
  *g2h<std::uint64_t>(p) = 0x1234;
  *g2h<std::uint64_t>(p + bytes - 8) = 0xabcd;
  CHECK(*g2h<std::uint64_t>(p) == 0x1234);
  CHECK(*g2h<std::uint64_t>(p + bytes - 8) == 0xabcd);
  space.free(p);
}
