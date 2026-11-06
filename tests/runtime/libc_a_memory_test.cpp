#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>

#include "core/address_space.h"
#include "core/guest_heap.h"
#include "core/error.h"
#include "hle/libc/libc_a.h"
#include "libc_a_test_support.h"
using namespace venture;

// North Star (Unity 2022, Linux): Unity's allocator fell back to libc free with
// 0x20000000000, the first page of the guest arena, and mimalloc faulted in the
// host reading a segment header there. free, realloc and malloc_usable_size of
// memory no guest heap allocation owns leave that memory alone and return.
TEST_CASE("libc A free, realloc and malloc_usable_size skip memory no heap allocation owns") {
  test::LibcFixture f;
  // A segment-aligned page of bytes that are no mimalloc metadata, as the
  // guest arena's first page is not; mimalloc would chase them as pointers.
  constexpr std::uint64_t segment = 64ull << 20, page = 4096;
  const auto base = AddressSpace::get().allocate(segment, kProtRead | kProtWrite, "[anon:not a heap]", segment);
  REQUIRE(base);
  struct Release {
    GuestAddr p;
    ~Release() { AddressSpace::get().free(p); }
  } release{base};
  std::memset(g2h(base), 0xa5, page);
  for (const auto foreign : {base, base + 0x100}) {
    CHECK_FALSE(guest_heap::owns(g2h(foreign)));
    f.invoke("free", {foreign});
    CHECK(f.invoke("malloc_usable_size", {foreign}) == 0);
    *f.thread.errno_location() = 0;
    CHECK(f.invoke("realloc", {foreign, 64}) == 0);
    CHECK(*f.thread.errno_location() == err::kENOMEM);
  }
  const auto* bytes = g2h<unsigned char>(base);
  CHECK(std::all_of(bytes, bytes + page, [](unsigned char b) { return b == 0xa5; }));
  // Heap blocks are unaffected.
  const auto block = f.invoke("malloc", {48});
  REQUIRE(block);
  CHECK(guest_heap::owns(g2h(block)));
  CHECK(f.invoke("malloc_usable_size", {block}) >= 48);
  const auto grown = f.invoke("realloc", {block, 4096});
  REQUIRE(grown);
  CHECK(f.invoke("malloc_usable_size", {grown}) >= 4096);
  f.invoke("free", {grown});
}
