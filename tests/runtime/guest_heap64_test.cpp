#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <mimalloc.h>
#include "core/guest_heap.h"
#include "platform/memory.h"
#include <set>
#include <vector>

using namespace venture;

// Unity 2019's default allocator tracks the upper 32 bits of each allocation in
// five slots and corrupts its bookkeeping beyond them ("Using memoryadresses
// from more than 16GB of memory"). The A64 guest heap must stay compact even
// when other host reservations land in between.
TEST_CASE("A64 guest heap stays in one contiguous arena") {
  guest_heap::select64();
  guest_heap::select64();  // idempotent
  std::vector<void*> blocks;
  std::vector<GuestAddr> host;
  std::set<std::uint64_t> windows;
  for (int i = 0; i < 16; ++i) {
    for (std::uint64_t size : {16ull, 4096ull, 1ull << 20, 48ull << 20}) {
      auto* p = guest_heap::allocator.aligned(size, 16);
      REQUIRE(p);
      blocks.push_back(p);
      windows.insert(reinterpret_cast<std::uintptr_t>(p) >> 32);
    }
    host.push_back(platform::reserve(0, 256ull << 20, false));  // unrelated host reservations
  }
  CHECK(windows.size() == 1);
  const auto [begin, end] = guest_heap::arena64();
  REQUIRE(begin < end);
  CHECK(begin % (4ull << 30) == 0);
  for (auto* p : blocks) {
    const auto a = reinterpret_cast<GuestAddr>(p);
    CHECK((a >= begin && a < end));
  }
  for (auto* p : blocks) guest_heap::allocator.free(p);
  for (auto h : host)
    if (h) platform::release(h, 256ull << 20);
}
