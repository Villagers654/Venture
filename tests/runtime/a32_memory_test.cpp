#include <doctest/doctest.h>
#include "core/address_space.h"
#include "core/memory_metadata.h"
#include "core/error.h"
#include "core/guest_heap.h"
#include "core/guest_storage.h"
#include "core/low_address_space.h"
#include "core/region_snapshot.h"
#include "core/thread.h"
#include "core/thread_runtime.h"
#include "core/tls32.h"
#include "core/token32.h"
#include "platform/memory.h"
#include "hle/libc/libc_a.h"
#include "hle/libc/vfs.h"
#include "hle/registry.h"
#include "platform/libc_host.h"
#include "fake_cpu32.h"
#include <chrono>
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <array>
#include <atomic>
#include <cstring>
#include <future>
#include <map>
#include <random>
#include <thread>
#include <vector>
using namespace venture;
TEST_CASE("A32 grow-down mappings preserve ILP32 ownership and the stack cap") {
  auto& space = AddressSpace::get();
  const auto old_limit = space.stack_limit();
  struct Restore { AddressSpace& s; std::uint64_t n; ~Restore() { s.set_stack_limit(n); } } restore{space, old_limit};
  space.set_stack_limit(2 * kGuestPageSize);
  const auto p = space.map(0, kGuestPageSize, kProtRead | kProtWrite,
      kMapPrivate | kMapAnonymous | kMapGrowDown | kMapStack, nullptr, 0, "[A32 grow-down]");
  REQUIRE(p > 0);
  REQUIRE(std::uint64_t(p) < 0x100000000ull);
  REQUIRE(space.grow_stack(p - 1, kProtWrite));
  MemoryRegion r;
  REQUIRE(space.find(p, &r));
  CHECK(r.start == p - kGuestPageSize);
  CHECK(r.size == 2 * kGuestPageSize);
  CHECK(r.grow_down);
  std::uint32_t word = 79, read = 0;
  REQUIRE(platform::write_memory(r.start, &word, sizeof(word)));
  REQUIRE(platform::read_memory(r.start, &read, sizeof(read)));
  CHECK(read == word);
  CHECK_FALSE(space.grow_stack(r.start - 1, kProtWrite));
  CHECK(space.unmap(r.start, r.size) == 0);
}

TEST_CASE("A32 proc maps reflects mapping protection labels and removal") {
  auto& space = AddressSpace::get();
  auto p = space.allocate(8192, 3, "[proc A32]");
  REQUIRE(p != 0);
  const auto original = space.proc_maps();
  CHECK(original.find("[proc A32]") != std::string::npos);
  REQUIRE(space.protect(p + 4096, 4096, 1) == 0);
  label_memory(p, 8192, "/data/app/proc-a32.so", 4096);
  const auto changed = space.proc_maps();
  // File mappings report a device and nonzero inode, as fs/proc/task_mmu.c does.
  CHECK(changed.find("r--p 00002000 fd:00 ") != std::string::npos);
  CHECK(changed.find(" /data/app/proc-a32.so\n") != std::string::npos);
  CHECK(original.find("/data/app/proc-a32.so") == std::string::npos);
  CHECK(changed.find("[proc A32]") == std::string::npos);
  space.free(p);
  CHECK(space.proc_maps().find("/data/app/proc-a32.so") == std::string::npos);
}
namespace {
// Storage adapters now use the descriptor-selected ILP32 marshaler.
std::uint64_t invoke(Library& lib, std::string_view name, std::initializer_list<std::uint64_t> args = {}) {
  auto thunk = lib.find_symbol(name);
  for (hle::FunctionId id = 1;; ++id) {
    const auto& info = hle::function(id);
    if (info.thunk != thunk) continue;
    test::FakeCpu32 cpu;
    int reg = 0; for (auto value : args) cpu.set_x(reg++, value);
    GuestThread thread(997);
    hle::CallContext ctx(thread, cpu, info);
    info.handler(ctx);
    return cpu.x(0);
  }
}
}  // namespace

TEST_CASE("A32 arena reserves low holes and wrap guard, rejects high fallback") {
  auto layout = low32::layout();
  CHECK(layout.total > 3ull * 1024 * 1024 * 1024);
  CHECK(layout.largest > 1024ull * 1024 * 1024);
  auto& as = AddressSpace::get();
  CHECK(as.allocate(0x100000000ull, kProtNone, "too big") == 0);
  CHECK(as.map(0x100000000ull, 4096, 3, kMapPrivate | kMapAnonymous | kMapFixed, nullptr, 0, "high") ==
        -err::kEINVAL);
  CHECK_FALSE(as.is_guest_accessible(0x100000000ull, 1, false));
  CHECK_FALSE(as.is_guest_accessible(0x7ffe0000, 4, false));
  CHECK_FALSE(as.is_guest_accessible(UINT64_MAX - 2, 8, true));
  CHECK_FALSE(platform::memory_accessible(0x100000000ull, 65536, false));
  CHECK(h2g32(nullptr) == 0);
  CHECK(h2g32(g2h(0xffffffff)) == 0xffffffff);
  CHECK_THROWS_AS(h2g32(g2h(0x100000000ull)), std::overflow_error);
  CHECK_THROWS_AS(as.initialize(GuestIsa::a64), std::runtime_error);
}
TEST_CASE("A32 placeholder cycles preserve neighbors and MAP_FIXED splits") {
  auto& as = AddressSpace::get();
  for (int cycle = 0; cycle < 30; ++cycle) {
    auto p = as.allocate(5 * 4096, 3, "[cycle]");
    REQUIRE(p != 0);
    CHECK(p + 5 * 4096 <= 0x100000000ull);
    std::memset(g2h(p), 0x5a, 5 * 4096);
    REQUIRE(as.protect(p + 4096, 4096, kProtRead) == 0);
    CHECK_FALSE(as.is_guest_accessible(p + 4096, 1, true));
    CHECK(as.is_guest_accessible(p, 5 * 4096, false));
    CHECK_FALSE(as.is_guest_accessible(p, 5 * 4096, true));
    REQUIRE(as.unmap(p + 8192, 4096) == 0);
    CHECK_FALSE(as.is_guest_accessible(p + 8192, 1, false));
    CHECK_FALSE(platform::memory_accessible(p + 8192, 1, false));
    CHECK(as.map(p, 4096, 3, kMapPrivate | kMapAnonymous | kMapFixedNoReplace, nullptr, 0, "exists") ==
          -err::kEEXIST);
    REQUIRE(as.map(p + 4096, 3 * 4096, 3, kMapPrivate | kMapAnonymous | kMapFixed, nullptr, 0, "replace") ==
            p + 4096);
    CHECK(*g2h<unsigned char>(p) == 0x5a);
    CHECK(*g2h<unsigned char>(p + 4 * 4096) == 0x5a);
    CHECK(*g2h<unsigned char>(p + 4096) == 0);
    CHECK(as.is_guest_accessible(p, 5 * 4096, true));
    REQUIRE(as.advise(p + 4096, 4096, 4) == 0);
    as.free(p);
    CHECK_FALSE(as.is_guest_accessible(p, 1, false));
  }
}
TEST_CASE("A32 small mappings take the highest free gap and large ones the lowest") {
  auto& as = AddressSpace::get();
  auto a = as.allocate(4096, 3, "[high a]"), b = as.allocate(4096, 3, "[high b]");
  REQUIRE(a); REQUIRE(b);
  CHECK(b < a);
  as.free(a);
  CHECK(as.allocate(4096, 3, "[high c]") == a);
  auto big = as.allocate(2 << 20, 3, "[low big]"), next = as.allocate(2 << 20, 3, "[low next]");
  REQUIRE(big); REQUIRE(next);
  CHECK(big < b);
  CHECK(next > big);
  as.free(big);
  CHECK(as.allocate(2 << 20, 3, "[low again]") == big);
  for (auto p : {a, b, big, next}) as.free(p);
}
TEST_CASE("A32 freed holes merge with free neighbours and reuse restores the first placement") {
  auto& as = AddressSpace::get();
  constexpr std::uint64_t page = 4096;
  const auto map = [&](GuestAddr hint, std::uint64_t n, std::uint32_t flags) -> GuestAddr {
    auto result = as.map(hint, n, 3, kMapPrivate | kMapAnonymous | flags, nullptr, 0, "[merge]");
    return result > 0 ? static_cast<GuestAddr>(result) : 0;
  };
  // x is the highest 3-page fit, so no higher gap holds 3 pages: x comes back
  // only if the three separately freed pages merge into one gap.
  const auto x = map(0, 3 * page, 0);
  REQUIRE(x);
  for (auto q : {x + page, x + 2 * page, x}) REQUIRE(as.unmap(q, page) == 0);
  CHECK(map(0, 3 * page, 0) == x);
  // A fixed mapping inside the hole splits it; unmapping it merges it back.
  REQUIRE(as.unmap(x + page, page) == 0);
  REQUIRE(map(x + page, page, kMapFixed) == x + page);
  REQUIRE(as.unmap(x, 3 * page) == 0);
  // Random small and large mappings, partial unmaps and protections never
  // overlap, and freeing everything restores the original gaps.
  std::mt19937 random(7);
  std::map<GuestAddr, std::uint64_t> pages;  // live page -> owning mapping
  std::vector<std::pair<GuestAddr, std::uint64_t>> live;
  for (int step = 0; step < 1500; ++step) {
    const auto action = random() % 8;
    if (live.size() < 48 && (action < 4 || live.empty())) {
      const auto n = random() % 10 == 0 ? (256 + random() % 512) * page : (1 + random() % 16) * page;
      const auto q = map(0, n, 0);
      REQUIRE(q);
      REQUIRE(q + n <= 0x100000000ull);
      for (std::uint64_t i = 0; i < n; i += page) REQUIRE(pages.emplace(q + i, q).second);
      CHECK(as.is_guest_accessible(q, n, true));
      live.emplace_back(q, n);
    } else if (action < 6) {
      const auto k = random() % live.size();
      auto [q, n] = live[k];
      REQUIRE(as.unmap(q, n) == 0);
      for (std::uint64_t i = 0; i < n; i += page) pages.erase(q + i);
      live.erase(live.begin() + k);
    } else {
      const auto [q, n] = live[random() % live.size()];
      const auto at = q + (random() % (n / page)) * page;
      if (action == 6 && pages.count(at)) {
        REQUIRE(as.protect(at, page, kProtRead) == 0);
        CHECK_FALSE(as.is_guest_accessible(at, 1, true));
        CHECK(as.is_guest_accessible(at, 1, false));
      } else if (pages.erase(at)) {
        REQUIRE(as.unmap(at, page) == 0);
        CHECK_FALSE(as.is_guest_accessible(at, 1, false));
      }
    }
  }
  for (auto [q, n] : live) REQUIRE(as.unmap(q, n) == 0);
  CHECK(map(0, 3 * page, 0) == x);
  REQUIRE(as.unmap(x, 3 * page) == 0);
}
TEST_CASE("Region snapshot partial publishes match full publishes") {
  // Randomized splits, inserts, erases and protections on a region map; each
  // change republishes only its span into one snapshot and everything into
  // the other, then both must answer every probe the same way.
  constexpr GuestAddr page = 4096, top = 64 * page;
  std::map<GuestAddr, MemoryRegion> regions;
  RegionSnapshot partial, full;
  const auto split = [&](GuestAddr p) {
    auto it = regions.upper_bound(p);
    if (it == regions.begin()) return;
    auto& r = (--it)->second;
    if (p <= r.start || p >= r.start + r.size) return;
    auto tail = r;
    tail.start = p, tail.size = r.start + r.size - p;
    r.size = p - r.start;
    regions.emplace(p, tail);
  };
  std::mt19937 random(11);
  for (int step = 0; step < 4000; ++step) {
    const GuestAddr lo = random() % 63 * page, hi = lo + (1 + random() % std::min<GuestAddr>(8, (top - lo) / page)) * page;
    split(lo);
    split(hi);
    auto first = regions.lower_bound(lo), last = regions.lower_bound(hi);
    switch (random() % 3) {
      case 0:  // erase, as munmap
        regions.erase(first, last);
        break;
      case 1:  // replace, as MAP_FIXED
        regions.erase(first, last);
        regions.emplace(lo, MemoryRegion{lo, hi - lo, static_cast<std::uint32_t>(random() % 8), "", 0});
        break;
      default:  // protect existing regions only
        for (auto it = first; it != last; ++it) it->second.prot = random() % 8;
    }
    // A write may also coalesce a neighbour that overlaps its span.
    if (random() % 4 == 0 && last != regions.end() && last != regions.begin()) {
      auto prev = std::prev(last);
      if (prev->first + prev->second.size == last->first && prev->second.prot == last->second.prot) {
        prev->second.size += last->second.size;
        regions.erase(last);
      }
    }
    partial.publish(regions, lo, hi);
    full.publish(regions);
    for (int probe = 0; probe < 16; ++probe) {
      const GuestAddr p = random() % (top + page), n = 1 + random() % (4 * page);
      const auto mask = 1u + random() % 7;
      REQUIRE(partial.permitted(p, n, mask) == full.permitted(p, n, mask));
    }
  }
}
TEST_CASE("A32 private files preserve bytes and reject writable shared") {
  AddressSpace::FileReader reader = [](auto off, void* out, auto n) {
    for (std::uint64_t i = 0; i < n; ++i) static_cast<unsigned char*>(out)[i] = (off + i) % 251;
    return true;
  };
  auto& as = AddressSpace::get();
  auto p = as.map(0, 3 * 4096, 1, kMapPrivate, &reader, 4096, "file");
  REQUIRE(p > 0);
  CHECK(*g2h<unsigned char>(p + 123) == (4096 + 123) % 251);
  CHECK_FALSE(as.is_guest_accessible(p, 1, true));
  REQUIRE(as.protect(p, 4096, 3) == 0);
  *g2h<unsigned char>(p) = 99;
  CHECK(as.map(0, 4096, 3, kMapShared, &reader, 0, "shared") == -err::kEOPNOTSUPP);
  REQUIRE(as.unmap(p, 3 * 4096) == 0);
}
TEST_CASE("A32 read-only native file views restore placeholders at page boundaries") {
  auto path = std::filesystem::temp_directory_path() /
              ("venture-a32-file-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup {
    std::filesystem::path path;
    int fd = -1;
    ~Cleanup() {
      if (fd >= 0) platform::close_host_fd(fd);
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
  } cleanup{path};
  std::array<char, 4 * 4096> bytes;
  for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>(i % 113);
  {
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), bytes.size());
  }
  int error = 0;
  auto file = hle::libc::host::open({"file", path, path.parent_path()}, 0, 0600, &error);
  REQUIRE(file);
  cleanup.fd = file->duplicate_host_fd();
  REQUIRE(cleanup.fd >= 0);
  auto& as = AddressSpace::get();
  auto p = as.map_readonly_file(cleanup.fd, 3 * 4096, kProtRead, 4096, "file");
  REQUIRE(p > 0);
  CHECK(std::memcmp(g2h(p), bytes.data() + 4096, 3 * 4096) == 0);
  CHECK(as.protect(p, 4096, kProtRead | kProtWrite) == -err::kEACCES);
  REQUIRE(as.unmap(p + 4096, 4096) == 0);
  CHECK_FALSE(as.is_guest_accessible(p + 4096, 1, false));
  REQUIRE(as.map(p + 4096, 4096, 3, kMapPrivate | kMapAnonymous | kMapFixed, nullptr, 0, "private") ==
          p + 4096);
  CHECK(*g2h<unsigned char>(p + 4096) == 0);
  CHECK(std::memcmp(g2h(p + 8192), bytes.data() + 3 * 4096, 4096) == 0);
  REQUIRE(as.unmap(p, 3 * 4096) == 0);
  auto shared = as.map_shared_file(cleanup.fd, 4096, kProtRead, kMapShared, 0, "readonly shared");
  REQUIRE(shared > 0);
  CHECK(*g2h<char>(shared + 17) == bytes[17]);
  CHECK(as.map_shared_file(cleanup.fd, 4096, 3, kMapShared, 0, "writable shared") == -err::kEOPNOTSUPP);
  REQUIRE(as.unmap(shared, 4096) == 0);
}
TEST_CASE("A32 immutable cache and scoped mutable storage stay low") {
  auto* text = low_string("Venture");
  CHECK(h2g32(text) != 0);
  CHECK(text == low_string("Venture"));
  CHECK(std::strcmp(text, "Venture") == 0);
  CHECK_FALSE(AddressSpace::get().is_guest_accessible(h2g(text), 1, true));
  GuestAddr p;
  {
    GuestStorage storage;
    p = h2g(storage.string("first"));
    CHECK(h2g32(g2h(p)) != 0);
    CHECK(std::strcmp(storage.string("second"), "second") == 0);
  }
  CHECK_FALSE(AddressSpace::get().is_guest_accessible(p, 1, false));
}
TEST_CASE("A32 libc guest-visible allocations and data exports stay low") {
  register_all_hle_libraries();
  auto* lib = Linker::get().open("libc.so", kRtldNow, nullptr);
  REQUIRE(lib);
  for (auto name : {"environ", "_ctype_", "_tolower_tab_", "_toupper_tab_", "tzname", "timezone", "daylight",
                    "__stack_chk_guard", "optarg", "optind", "__sF", "stdin", "stdout", "stderr"}) {
    auto p = lib->find_symbol(name);
    INFO(name);
    CHECK(p != 0);
    CHECK(p < 0x100000000ull);
  }
  auto env_cell = lib->find_symbol("environ");
  CHECK(*g2h<std::uint32_t>(*g2h<std::uint32_t>(env_cell)) == 0);
  auto ctype = *g2h<std::uint32_t>(lib->find_symbol("_ctype_"));
  CHECK(AddressSpace::get().is_guest_accessible(ctype, 257, false));
  auto* p = hle::libc::allocate(500);
  REQUIRE(p);
  CHECK(h2g32(p) != 0);
  p = hle::libc::reallocate(p, 999);
  REQUIRE(p);
  CHECK(h2g32(p) != 0);
  CHECK(hle::libc::usable_size(p) >= 999);
  hle::libc::deallocate(p);
  CHECK(h2g32(hle::libc::error_location()) != 0);
  CHECK(h2g32(g2h(invoke(*lib, "strerror", {34}))) != 0);
  CHECK(h2g32(g2h(invoke(*lib, "strerror", {std::uint64_t(-999)}))) != 0);
  auto name = h2g(low_string("VENTURE_A32_TEST"));
  auto value = h2g(low_string("one"));
  REQUIRE(invoke(*lib, "setenv", {name, value, 1}) == 0);
  auto old = invoke(*lib, "getenv", {name});
  CHECK(h2g32(g2h(old)) != 0);
  CHECK(std::strcmp(g2h<char>(old), "one") == 0);
  REQUIRE(hle::libc::set_environment("VENTURE_A32_TEST", "from Java", true) == 0);
  auto from_java = invoke(*lib, "getenv", {name});
  CHECK(h2g32(g2h(from_java)) != 0);
  CHECK(std::strcmp(g2h<char>(from_java), "from Java") == 0);
  REQUIRE(hle::libc::set_environment("VENTURE_A32_TEST", "ignored", false) == 0);
  CHECK(invoke(*lib, "getenv", {name}) == from_java);
  REQUIRE(invoke(*lib, "setenv", {name, h2g(low_string("two")), 1}) == 0);
  CHECK(std::strcmp(g2h<char>(old), "one") == 0);
  GuestStorage put;
  auto text = h2g(put.string("VENTURE_A32_TEST=put"));
  REQUIRE(invoke(*lib, "putenv", {text}) == 0);
  CHECK(invoke(*lib, "getenv", {name}) == text + std::string_view("VENTURE_A32_TEST=").size());
  g2h<char>(text)[std::string_view("VENTURE_A32_TEST=").size()] = 'c';
  CHECK(std::strcmp(g2h<char>(invoke(*lib, "getenv", {name})), "cut") == 0);
  REQUIRE(invoke(*lib, "unsetenv", {name}) == 0);
  CHECK(invoke(*lib, "getenv", {name}) == 0);
  auto duplicate = invoke(*lib, "strdup", {value});
  CHECK(h2g32(g2h(duplicate)) != 0);
  CHECK(std::strcmp(g2h<char>(duplicate), "one") == 0);
  invoke(*lib, "free", {duplicate});
  auto zero = invoke(*lib, "calloc", {9, 37});
  REQUIRE(zero);
  CHECK(*g2h<char>(zero + 332) == 0);
  invoke(*lib, "free", {zero});
}
TEST_CASE("A32 exclusive heaps allocate, align, realloc and free across threads") {
  auto& as = AddressSpace::get();
  void* p = guest_heap::allocate(5000, 4096, true);
  REQUIRE(p);
  CHECK((h2g32(p) & 4095) == 0);
  CHECK(as.is_guest_accessible(h2g(p), 5000, true));
  CHECK(*static_cast<int*>(p) == 0);
  std::memset(p, 0x5a, 5000);
  REQUIRE(as.protect(h2g(p), 4096, kProtRead) == 0);
  CHECK(as.is_guest_accessible(h2g(p), 1, false));
  CHECK_FALSE(as.is_guest_accessible(h2g(p), 1, true));
  REQUIRE(as.protect(h2g(p), 4096, kProtRead | kProtWrite) == 0);
  auto future = std::async(std::launch::async, [p] {
    auto* next = guest_heap::reallocate(p, 9000);
    if (!next) return static_cast<void*>(nullptr);
    CHECK(static_cast<unsigned char*>(next)[4999] == 0x5a);
    guest_heap::release_thread();
    return next;
  });
  auto* next = future.get();
  REQUIRE(next);
  CHECK(h2g32(next) != 0);
  guest_heap::deallocate(next);
  CHECK_FALSE(as.is_guest_accessible(h2g(next), 1, true));
  CHECK(guest_heap::allocate(1ull << 32) == nullptr);
  CHECK(guest_heap::allocate(1, 3) == nullptr);
  std::vector<std::thread> workers;
  for (int t = 0; t < 8; ++t)
    workers.emplace_back([] {
      for (int i = 1; i <= 100; ++i) {
        auto* p = guest_heap::allocate(i * 43);
        REQUIRE(p);
        CHECK(h2g32(p) != 0);
        std::memset(p, 0x3c, i * 43);
        guest_heap::deallocate(p);
      }
    });
  for (auto& worker : workers) worker.join();
  std::vector<void*> full;
  for (int i = 0; i < 40; ++i) {
    auto* p = guest_heap::allocate(32ull << 20);
    if (!p) break;
    CHECK(h2g32(p) != 0);
    full.push_back(p);
  }
  CHECK(full.size() < 40);  // arena exhaustion, not a synthetic size rejection
  for (auto* p : full) guest_heap::deallocate(p);
  auto* recovered = guest_heap::allocate(4096);
  REQUIRE(recovered);
  guest_heap::deallocate(recovered);
}
TEST_CASE("A32 adopted thread heap outlives guest thread-exit handlers") {
  static std::atomic<bool> cleaned = false;
  cleaned = false;
  set_guest_thread_exit_handler(+[](GuestThread&) {
    auto* p = guest_heap::allocate(123);
    cleaned = p != nullptr;
    guest_heap::deallocate(p);
  });
  void* returned = nullptr;
  std::thread worker([&] {
    GuestThread::adopt_current();
    returned = guest_heap::allocate(500);
  });
  worker.join();
  set_guest_thread_exit_handler(nullptr);
  CHECK(cleaned.load());
  REQUIRE(returned);
  CHECK(h2g32(returned) != 0);
  guest_heap::deallocate(returned);
}
TEST_CASE("A32 compact TLS slots, low errno and guest stack") {
  GuestThread thread(998);
  CHECK(h2g32(thread.errno_location()) != 0);
  thread.set_errno(34);
  CHECK(*thread.errno_location() == 34);
  thread.attach_to_current_host_thread();
  CHECK(thread.cpu().isa() == GuestIsa::a32);
  CHECK(thread.call(0xffff0fe0, {}) == thread.thread_pointer); // real __kuser_get_tls
  auto* slots = g2h<std::uint32_t>(thread.thread_pointer);
  CHECK(slots[-1] == thread.thread_pointer - 4096);
  CHECK(slots[0] == thread.thread_pointer + tls::a32::surplus + 8);
  CHECK(slots[1] == thread.pthread_self);
  CHECK(slots[2] == 0);
  CHECK(slots[3] == 0);
  CHECK(slots[7] == 0);
  CHECK((slots[5] & 0xff) == 0);
  MemoryRegion region;
  REQUIRE(AddressSpace::get().find(thread.thread_pointer, &region));
  CHECK(region.size == tls::a32::allocation_size);
  CHECK(thread.stack_base + thread.stack_size < 0x100000000ull);
  std::array<std::byte, 4> init{std::byte{42}};
  auto module = tls::add_module(init, 128, 32);
  auto address = tls::get_addr(thread, module.id, 0);
  CHECK(address % 32 == 0);
  CHECK(*g2h<unsigned char>(address) == 42);
  CHECK(g2h<std::uint32_t>(slots[0])[module.id] == address);
  CHECK_THROWS_AS(tls::add_module({}, 65536, 32), std::runtime_error);
}
TEST_CASE("A32 JIT faults use arena permissions and live heap pages") {
  auto& as = AddressSpace::get();
  auto code = as.allocate(4096, kProtRead | kProtWrite, "[fault probe]");
  REQUIRE(code);
  const std::array<std::uint32_t, 2> words{0xe5901000, 0xef000000 | kSvcReturnToHost}; // ldr r1,[r0]; svc
  std::memcpy(g2h(code), words.data(), sizeof(words));
  REQUIRE(as.protect(code, 4096, kProtRead | kProtExec) == 0);
  auto* data = guest_heap::allocate(65536, 4096);
  REQUIRE(data);
  *static_cast<std::uint32_t*>(data) = 42;
  auto cpu = cpu_backend().create_cpu();
  cpu->set_pc(code); cpu->set_x(0, h2g(data));
  REQUIRE(cpu->run() == CpuExit::Svc);
  CHECK(cpu->x(1) == 42);
  REQUIRE(as.protect(h2g(data), 4096, kProtNone) == 0);
  cpu->set_pc(code);
  CHECK(cpu->run() == CpuExit::MemoryFault);
  CHECK(cpu->fault().address == h2g(data));
  REQUIRE(as.protect(h2g(data), 4096, kProtRead | kProtWrite) == 0);
  CHECK_FALSE(low32::query(code, 4, kProtRead | kProtWrite));
  CHECK(low32::query(code, 4, kProtRead | kProtExec));
  guest_heap::deallocate(data);
  CHECK_FALSE(low32::query(h2g(data), 4, kProtRead));
  cpu.reset(); as.free(code);
}
TEST_CASE("Token32 typed, null, local, global, weak and generation lifetimes") {
  Token32Registry registry;
  auto object = std::make_shared<int>(42);
  CHECK_FALSE(registry.insert(std::shared_ptr<int>{}));
  auto global = registry.insert(object);
  CHECK(*registry.resolve(global) == 42);
  CHECK_FALSE(registry.resolve(Token32<double>{global.value}));
  Token32<int> local;
  {
    Token32Registry::Scope scope(registry);
    local = registry.insert(object, TokenLifetime::local, &scope);
    CHECK(registry.resolve(local) == object);
    CHECK_FALSE(std::async(std::launch::async, [&] { return bool(registry.resolve(local)); }).get());
  }
  CHECK_FALSE(registry.resolve(local));
  CHECK(registry.resolve(global) == object);
  auto weak = registry.insert(object, TokenLifetime::weak);
  auto pin = registry.resolve(global);
  CHECK(registry.erase(global));
  CHECK_FALSE(registry.erase(global));
  object.reset();
  CHECK(registry.resolve(weak) == pin);
  pin.reset();
  CHECK_FALSE(registry.resolve(weak));
  auto replacement = registry.insert(std::make_shared<int>(7));
  CHECK(replacement.value != global.value);
  CHECK_FALSE(registry.resolve(global));
  CHECK(registry.erase(replacement));
  // Exhaust a slot's generation; it must retire rather than resurrect a token.
  auto stale = registry.insert(std::make_shared<int>(1));
  registry.erase(stale);
  for (int i = 0; i < 4200; ++i) {
    auto t = registry.insert(std::make_shared<int>(i));
    registry.erase(t);
  }
  CHECK_FALSE(registry.resolve(stale));
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i)
    threads.emplace_back([&] {
      for (int n = 0; n < 100; ++n) {
        auto token = registry.insert(std::make_shared<int>(n));
        auto retained = registry.resolve(token);
        REQUIRE(retained);
        CHECK(registry.erase(token));
        CHECK(*retained == n);
        CHECK_FALSE(registry.resolve(token));
      }
    });
  for (auto& thread : threads) thread.join();
}
TEST_CASE("A32 lock-free snapshots race mapping writers") {
  auto& as = AddressSpace::get();
  auto p = as.allocate(8192, 3, "[snapshot]");
  REQUIRE(p);
  std::atomic<bool> done = false;
  std::thread reader([&] {
    while (!done.load()) {
      as.is_guest_accessible(p, 8192, false);
      as.is_guest_executable(p);
    }
  });
  for (int i = 0; i < 100; ++i) {
    REQUIRE(as.protect(p, 4096, 1) == 0);
    REQUIRE(as.protect(p, 4096, 3) == 0);
  }
  done = true;
  reader.join();
  as.free(p);
}
TEST_CASE("A32 maps the real First Contact image spans" *
          doctest::skip(std::getenv("VENTURE_A32_IMAGE_DIR") == nullptr)) {
  // Test-only ELF32 header reader. Production normalization belongs to a32-loader.
  struct Header {
    std::uint8_t ident[16];
    std::uint16_t type, machine;
    std::uint32_t version, entry, phoff, shoff, flags;
    std::uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
  };
  struct Segment {
    std::uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, alignment;
  };
  static_assert(sizeof(Header) == 52 && sizeof(Segment) == 32);
  auto& as = AddressSpace::get();
  std::vector<GuestAddr> images;
  for (auto name : {"libUE4.so", "libOVRPlugin.so", "libgnustl_shared.so"}) {
    std::ifstream file(std::filesystem::path(std::getenv("VENTURE_A32_IMAGE_DIR")) / name, std::ios::binary);
    REQUIRE(file.good());
    Header h{};
    file.read(reinterpret_cast<char*>(&h), sizeof(h));
    REQUIRE(h.ident[4] == 1);
    REQUIRE(h.machine == 40);
    REQUIRE(h.phentsize == sizeof(Segment));
    std::vector<Segment> segments(h.phnum);
    file.seekg(h.phoff);
    file.read(reinterpret_cast<char*>(segments.data()), segments.size() * sizeof(Segment));
    REQUIRE(file.good());
    GuestAddr begin = UINT64_MAX, end = 0;
    for (auto s : segments)
      if (s.type == 1 && s.memsz) {
        begin = std::min(begin, align_down(s.vaddr, 4096));
        end = std::max(end, align_up(std::uint64_t{s.vaddr} + s.memsz, 4096));
      }
    REQUIRE(end > begin);
    auto p = as.allocate(end - begin, kProtNone, name);
    REQUIRE(p);
    images.push_back(p);
    CHECK(p + end - begin <= 0x100000000ull);
    for (auto s : segments)
      if (s.type == 1 && s.memsz) {
        auto first = p + align_down(s.vaddr, 4096) - begin;
        auto bytes = align_up(std::uint64_t{s.vaddr} + s.memsz, 4096) - align_down(s.vaddr, 4096);
        REQUIRE(as.protect(first, bytes, kProtRead | kProtWrite) == 0);
        file.seekg(s.offset);
        file.read(g2h<char>(p + s.vaddr - begin), s.filesz);
        REQUIRE(file.good());
        if (s.memsz > s.filesz) CHECK(*g2h<unsigned char>(p + s.vaddr - begin + s.memsz - 1) == 0);
        auto prot = ((s.flags & 4) ? kProtRead : 0) | ((s.flags & 2) ? kProtWrite : 0) |
                    ((s.flags & 1) ? kProtExec : 0);
        REQUIRE(as.protect(first, bytes, prot) == 0);
        CHECK(as.is_guest_accessible(p + s.vaddr - begin, s.filesz, false));
      }
    if (std::string_view(name) == "libUE4.so") {
      // v7 libgcc helpers censused by tools/a32_cpu_game_probe.py. Execute in
      // the mapped image, with the production stack, kuser page and query.
      auto barrier = p + 0x4dd788c - begin, fetch_add = p + 0x4dd6aa8 - begin;
      REQUIRE(*g2h<std::uint32_t>(barrier) == 0xe59f3000);
      REQUIRE(*g2h<std::uint32_t>(barrier + 8) == 0xffff0fa0);
      REQUIRE(*g2h<std::uint32_t>(fetch_add) == 0xe92d40f8);
      REQUIRE(*g2h<std::uint32_t>(fetch_add + 52) == 0xffff0fc0);
      GuestThread thread(996); thread.attach_to_current_host_thread();
      CHECK_NOTHROW(thread.call(barrier, {}));
      auto* value = static_cast<std::uint32_t*>(guest_heap::allocate(4));
      REQUIRE(value); *value = 35;
      CHECK(thread.call(fetch_add, {h2g(value), 7}) == 35);
      CHECK(*value == 42);
      guest_heap::deallocate(value);
      std::cout << "First Contact mapped libUE4 barrier/fetch-add executed through arena kuser page\n";
    }
    std::cout << "First Contact " << name << ": " << end - begin << " bytes at 0x" << std::hex << p
              << std::dec << '\n';
  }
  for (auto p : images) as.free(p);
}
