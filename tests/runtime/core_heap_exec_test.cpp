#include <doctest/doctest.h>
#include "core/address_space.h"
#include "core/error.h"
#include "core/guest_heap.h"
#include "core/thread_runtime.h"
#include <cstdint>
#include <cstring>
#include <vector>

// Runs in its own process (venture_tests_core_heap_exec): it selects the A64 guest heap arena,
// after which allocations from mimalloc segments made earlier in the process are not guest heap.
using namespace venture;

// Hook libraries (Flamingo under Scotland2) posix_memalign a page, write a
// trampoline and mprotect it PROT_READ|PROT_EXEC, as Android allows.
TEST_CASE("core CPU executes page-aligned guest heap memory after mprotect") {
  AddressSpace::get().initialize(GuestIsa::a64);
  const auto [heap_begin, heap_end] = guest_heap::arena64();
  REQUIRE(heap_begin);
  auto& space = AddressSpace::get();
  GuestThread thread(196); thread.attach_to_current_host_thread();
  // A huge block takes a fresh segment, from the arena: earlier tests in this
  // process may have left mimalloc OS segments from before select64.
  void* block = nullptr;
  std::vector<void*> outside;
  for (int i = 0; i < 8 && !block; ++i) {
    auto* p = guest_heap::allocator.aligned(32ull << 20, 4096);
    REQUIRE(p);
    if (h2g(p) >= heap_begin && h2g(p) < heap_end) block = p;
    else outside.push_back(p);
  }
  for (auto* p : outside) guest_heap::allocator.free(p);
  REQUIRE(block);
  const auto page = h2g(block);
  REQUIRE(page % 4096 == 0);
  auto* words = static_cast<std::uint32_t*>(block);
  words[0] = 0xd2800020; words[1] = 0xd65f03c0;  // mov x0,#1; ret
  CHECK_FALSE(space.is_guest_executable(page));
  CHECK_FALSE(space.find(page, nullptr));
  REQUIRE(space.protect(page, 4096, kProtRead | kProtExec) == 0);
  CHECK(space.is_guest_executable(page));
  CHECK_FALSE(space.is_guest_executable(page + 4096));
  MemoryRegion region;
  REQUIRE(space.find(page, &region));
  CHECK(region.start == page);
  CHECK(region.size == 4096);
  CHECK(region.prot == (kProtRead | kProtExec));
  CHECK(thread.call(page, {}) == 1);
  // Rewriting through RW drops the region and the translation.
  REQUIRE(space.protect(page, 4096, kProtRead | kProtWrite) == 0);
  CHECK_FALSE(space.find(page, nullptr));
  CHECK_FALSE(space.is_guest_executable(page));
  words[0] = 0xd2800040;  // mov x0,#2
  REQUIRE(space.protect(page, 4096, kProtRead | kProtWrite | kProtExec) == 0);
  CHECK(thread.call(page, {}) == 2);
  // Re-protecting an executable range invalidates it too.
  words[0] = 0xd2800060;  // mov x0,#3
  REQUIRE(space.protect(page, 4096, kProtRead | kProtExec) == 0);
  CHECK(thread.call(page, {}) == 3);
  CHECK(space.proc_maps().find("[anon:libc_malloc]") != std::string::npos);
  // Host protection stays mimalloc's: it writes its free list into a freed
  // block, which may still be guest read-only.
  std::memset(block, 0x5a, 4096);
  guest_heap::allocator.free(block);
  REQUIRE(space.protect(page, 4096, kProtRead | kProtWrite) == 0);
  CHECK_FALSE(space.find(page, nullptr));
  CHECK(space.protect(page + 16, 4096, kProtRead) == -err::kEINVAL);
  // Ranges that leave the heap arena are still unmapped memory.
  CHECK(space.protect(heap_end - 4096, 8192, kProtRead) == -err::kENOMEM);
  CHECK(space.protect(heap_begin - 4096, 8192, kProtRead) == -err::kENOMEM);
}
