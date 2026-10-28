#include <doctest/doctest.h>
#include "fake_cpu32.h"
#include "core/hle_runtime.h"
#include "core/thread.h"
#include "core/error.h"
#include "hle/libc/libc_parts.h"
#include "core/linker.h"
#include "core/process.h"
#include "core/syscall.h"
#include "core/a32_unwind.h"
#include "cpu/state.h"
#include "hle/libc/vfs.h"
#include "platform/low_memory.h"
#include "platform/memory.h"
#include <cstring>
#include <atomic>
#include <thread>
#include <vector>
#include <bit>
#include <limits>

using namespace venture;
namespace {
std::uint32_t call(const char*, std::initializer_list<std::uint32_t>);
HleLibrary& library() {
  static HleLibrary lib("libc.so");
  static const bool initialized = [] {
    hle::libc::register_threads(lib); hle::libc::register_signals(lib);
    hle::libc::register_files(lib); hle::libc::register_clock(lib);
    hle::libc::register_process(lib); hle::libc::register_time_format(lib);
    hle::libc::register_ctype_locale(lib); hle::libc::register_memory_map(lib);
    hle::libc::register_net(lib); hle::libc::register_stdlib(lib); hle::libc::register_strings(lib);
    hle::libc::register_syscall(lib); return true;
  }();
  (void)initialized;
  return lib;
}
const hle::FunctionInfo& function(const char* name) {
  auto thunk = library().find_symbol(name);
  for (const auto& f : hle::registered_functions())
    if (f.thunk == thunk) {
      // Snapshots are values; fetch the stable registry entry via its ARM thunk id.
      auto* code = g2h<std::uint32_t>(thunk);
      auto id = (code[0] & 0xfff) | ((code[0] >> 4) & 0xf000) |
                ((code[1] & 0xfff) << 16) | ((code[1] & 0xf0000) << 12);
      return hle::function(id);
    }
  throw std::runtime_error(name);
}
TEST_CASE("A32 libc records preserve adjacent bytes and low FILE legacy fields") {
  test::LowMemory32 memory;
  auto p = h2g32(memory.at<void>());
  auto* words = memory.at<std::uint32_t>();
  std::memset(words, 0xa5, 4096);
  CHECK(call("sigemptyset", {p}) == 0); CHECK(words[0] == 0); CHECK(words[1] == 0xa5a5a5a5);
  CHECK(call("sigaddset", {p, 32}) == 0); CHECK(words[0] == 0x80000000);
  CHECK(call("sigaddset", {p, 33}) == UINT32_MAX); CHECK(words[1] == 0xa5a5a5a5);
  CHECK(call("sigemptyset64", {p}) == 0); CHECK(words[0] == 0); CHECK(words[1] == 0); CHECK(words[2] == 0xa5a5a5a5);
  words[0] = 1; words[1] = 0x1234; words[2] = 0x10000000; words[3] = 0;
  CHECK(call("sigaction", {10, p, 0}) == 0);
  CHECK(call("sigaction", {10, 0, p + 32}) == 0);
  CHECK(words[8] == 1); CHECK(words[9] == 0x1234); CHECK(words[10] == 0x10000000); CHECK(words[12] == 0xa5a5a5a5);
  CHECK(call("clock_gettime", {1, p + 128}) == 0);
  CHECK(words[34] == 0xa5a5a5a5); CHECK(words[33] < 1000000000);
  words[40] = 0; // epoch
  CHECK(call("gmtime_r", {p + 160, p + 192}) == p + 192);
  CHECK(words[48 + 5] == 70); CHECK(words[48 + 11] == 0xa5a5a5a5);
  CHECK(std::strcmp(g2h<char>(words[48 + 10]), "UTC") == 0);
  CHECK(call("getrlimit", {7, p + 256}) == 0);
  CHECK(words[64] == 32768); CHECK(words[66] == 0xa5a5a5a5);
  CHECK(call("sysinfo", {p + 320}) == 0);
  auto usable = std::uint64_t(words[84]) * words[93];
  CHECK(usable == guest_usable_memory());
  CHECK(usable > 0);
  CHECK(usable < (1ull << 32)); // ARMv7 size_t consumers must see usable memory
  CHECK(call("sysconf", {0x62}) * 4096ull == usable);
  CHECK(words[96] == 0xa5a5a5a5);
  CHECK(call("fstat", {1, p + 512}) == 0);
  CHECK(words[128 + 26] == 0xa5a5a5a5); CHECK((words[128 + 4] & 0170000) == 0020000);
  auto globals = library().find_symbol("__sF");
  for (int i = 0; i < 3; ++i) {
    CHECK(*g2h<std::int16_t>(globals + 84 * i + 14) == i);
    CHECK(call("fileno", {std::uint32_t(globals + 84 * i)}) == std::uint32_t(i));
  }
  CHECK(*g2h<std::uint32_t>(library().find_symbol("stdout")) == globals + 84);
  std::memcpy(memory.at<char>(768), "abc", 4); std::memcpy(memory.at<char>(784), "r", 2);
  auto file = call("fmemopen", {p + 768, 3, p + 784}); REQUIRE(file != 0);
  CHECK(call("fread", {p + 800, 1, 4, file}) == 3);
  CHECK(std::memcmp(memory.at<char>(800), "abc", 3) == 0);
  CHECK((*g2h<std::int16_t>(file + 12) & 0x20) != 0);
  words[210] = 0; words[211] = 0xfeedbeef;
  CHECK(call("fgetpos", {file, p + 840}) == 0);
  CHECK(words[210] == 3); CHECK(words[211] == 0xfeedbeef);
  CHECK(call("clearerr", {file}) == file); // void return preserves r0
  CHECK((*g2h<std::int16_t>(file + 12) & 0x60) == 0);
  CHECK(call("fclose", {file}) == 0);
  words[240] = 0; words[241] = 0xfeedbeef;
  std::memcpy(memory.at<char>(980), "\xe2\x82\xac", 3);
  CHECK(call("mbrtowc", {p + 1000, p + 980, 1, p + 960}) == 0xfffffffe);
  CHECK(words[241] == 0xfeedbeef);
  CHECK(call("mbrtowc", {p + 1000, p + 981, 2, p + 960}) == 2);
  CHECK(words[250] == 0x20ac); CHECK(words[240] == 0); CHECK(words[241] == 0xfeedbeef);
  for (auto [path, expected] : {std::pair{"/data/app/game/", "game"}, {"///", "/"}, {"", "."}}) {
    std::strcpy(memory.at<char>(1100), path);
    auto name = call("basename", {p + 1100});
    CHECK(std::strcmp(g2h<char>(name), expected) == 0);
    CHECK(std::strcmp(memory.at<char>(1100), path) == 0);
  }
  CHECK(std::strcmp(g2h<char>(call("basename", {0})), ".") == 0);
}

TEST_CASE("A32 sbrk retains headroom across growth shrink and unrelated allocations") {
  auto& space = AddressSpace::get();
  const auto initial = call("sbrk", {0});
  REQUIRE(initial != UINT32_MAX);
  const auto unrelated = space.allocate(kGuestPageSize, kProtRead | kProtWrite, "[sbrk-unrelated]");
  REQUIRE(unrelated != 0);
  constexpr std::uint32_t growth = 16 * kGuestPageSize;
  REQUIRE(call("sbrk", {growth}) == initial);
  CHECK(call("sbrk", {0}) == initial + growth);
  auto* memory = g2h<unsigned char>(initial);
  CHECK(memory[growth - 1] == 0);
  memory[0] = 42;
  memory[growth - 1] = 99;
  REQUIRE(call("sbrk", {0u - growth}) == initial + growth);
  CHECK(call("sbrk", {0}) == initial);
  MemoryRegion region;
  REQUIRE(space.find(initial + growth - 1, &region));
  CHECK(region.prot == kProtNone);
  CHECK(region.name == "[heap]");
  const auto while_shrunk = space.allocate(kGuestPageSize, kProtRead | kProtWrite, "[sbrk-unrelated]");
  REQUIRE(while_shrunk != 0);
  REQUIRE(call("sbrk", {growth}) == initial);
  CHECK(memory[0] == 42);
  CHECK(memory[growth - 1] == 0);
  CHECK(call("brk", {initial}) == 0);
  space.free(while_shrunk);
  space.free(unrelated);
}
TEST_CASE("A32 brk rolls back a growth whose commit fails partway") {
  auto& space = AddressSpace::get();
  auto& thread = GuestThread::adopt_current();
  const auto initial = call("sbrk", {0});
  REQUIRE(initial != UINT32_MAX);
  MemoryRegion region;
  REQUIRE((!space.find(initial - 1, &region) || region.name != "[heap]"));  // break at its base
  auto end = static_cast<GuestAddr>(initial);
  while (space.find(end, &region) && region.name == "[heap]") end = region.start + region.size;
  // Rebuild the reservation from separate host allocations, so the second can fail alone.
  constexpr std::uint32_t page = kGuestPageSize;
  const auto mapped = initial + page, second = mapped + 4 * page;
  REQUIRE(space.unmap(initial, end - initial) == 0);
  constexpr auto fixed = kMapPrivate | kMapAnonymous | kMapFixed;
  REQUIRE(space.map(initial, page, kProtRead | kProtWrite, fixed, nullptr, 0, "[heap]") == initial);
  REQUIRE(space.map(mapped, second - mapped, kProtNone, fixed, nullptr, 0, "[heap]") == mapped);
  REQUIRE(space.map(second, end - second, kProtNone, fixed, nullptr, 0, "[heap]") == second);
#ifdef _WIN32
  REQUIRE(platform::restore_placeholder(second, end - second, false));  // placeholders never commit
#else
  platform::release(second, end - second);  // mprotect fails on unmapped pages
#endif
  thread.set_errno(0);
  CHECK(call("brk", {second + page}) == UINT32_MAX);
  CHECK(*thread.errno_location() == err::kENOMEM);
  CHECK(call("sbrk", {0}) == initial);
  REQUIRE(space.find(mapped, &region));
  CHECK(region.prot == kProtNone);
#ifdef _WIN32
  REQUIRE(platform::replace_placeholder(second, end - second));
#else
  REQUIRE(platform::restore_placeholder(second, end - second, false));
#endif
  CHECK(call("brk", {second + page}) == 0);
  CHECK(call("brk", {initial}) == 0);
}
TEST_CASE("A32 sorting callbacks receive low ARM pointers and preserve size_t widths") {
  test::LowMemory32 memory, code(false, true);
  const std::uint32_t compare[]{0xe5902000, 0xe5913000, 0xe0420003, 0xe12fff1e};
  std::memcpy(code.at<void>(), compare, sizeof(compare));
  auto* values = memory.at<std::int32_t>();
  const std::int32_t input[]{17, 3, 8, 0, 14, -2};
  std::memcpy(values, input, sizeof(input));
  values[6] = 0x12345678;
  call("qsort", {std::uint32_t(memory.address), 6, 4, std::uint32_t(code.address)});
  const std::int32_t sorted[]{-2, 0, 3, 8, 14, 17};
  CHECK(std::memcmp(values, sorted, sizeof(sorted)) == 0); CHECK(values[6] == 0x12345678);
  *memory.at<std::int32_t>(64) = 8;
  CHECK(call("bsearch", {std::uint32_t(memory.address + 64), std::uint32_t(memory.address), 6, 4,
                          std::uint32_t(code.address)}) == memory.address + 12);
  *memory.at<std::int32_t>(64) = 9;
  CHECK(call("bsearch", {std::uint32_t(memory.address + 64), std::uint32_t(memory.address), 6, 4,
                          std::uint32_t(code.address)}) == 0);
}
TEST_CASE("A32 socket records, poll and socklen outputs retain wire boundaries") {
  test::LowMemory32 memory;
  auto p = std::uint32_t(memory.address);
  auto server = call("socket", {2, 2 | 0x800, 0}), client = call("socket", {2, 2 | 0x800, 0});
  REQUIRE(std::int32_t(server) >= 0); REQUIRE(std::int32_t(client) >= 0);
  struct Close {
    std::uint32_t a, b;
    ~Close() { call("close", {a}); call("close", {b}); }
  } close{server, client};
  const unsigned char loopback[16]{2, 0, 0, 0, 127, 0, 0, 1};
  std::memcpy(memory.at<void>(), loopback, sizeof(loopback));
  REQUIRE(call("bind", {server, p, 16}) == 0);
  *memory.at<std::uint64_t>(32) = 0xfeedface00000010ull;
  REQUIRE(call("getsockname", {server, p, p + 32}) == 0);
  CHECK(*memory.at<std::uint32_t>(32) == 16); CHECK(*memory.at<std::uint32_t>(36) == 0xfeedface);
  REQUIRE(call("connect", {client, p, 16}) == 0);
  *memory.at<std::uint64_t>(48) = 0xabcdef0100000010ull;
  REQUIRE(call("getpeername", {client, p + 64, p + 48}) == 0);
  CHECK(*memory.at<std::uint32_t>(52) == 0xabcdef01);
  CHECK(std::memcmp(memory.at<void>(), memory.at<void>(64), 16) == 0);
  std::memcpy(memory.at<void>(128), "venture", 2);
  REQUIRE(call("sendto", {client, p + 128, 2, 0, p, 16}) == 2);
  *memory.at<std::int32_t>(256) = server;
  *memory.at<std::uint16_t>(260) = 1;
  *memory.at<std::uint32_t>(264) = 0x87654321;
  REQUIRE(call("poll", {p + 256, 1, 1000}) == 1);
  CHECK(*memory.at<std::uint16_t>(262) == 1); CHECK(*memory.at<std::uint32_t>(264) == 0x87654321);
  *memory.at<std::uint64_t>(48) = 0xabcdef0100000010ull;
  REQUIRE(call("recvfrom", {server, p + 160, 2, 0, p + 64, p + 48}) == 2);
  CHECK(std::memcmp(memory.at<void>(160), "venture", 2) == 0); CHECK(*memory.at<std::uint32_t>(52) == 0xabcdef01);
  REQUIRE(call("getnameinfo", {p, 16, p + 320, 32, p + 352, 16, 3}) == 0);
  CHECK(std::strcmp(memory.at<char>(320), "127.0.0.1") == 0);
}
TEST_CASE("A32 setjmp restores ARM core and VFP registers without overwriting sigjmp padding") {
  auto& t = GuestThread::adopt_current(); test::LowMemory32 memory;
  test::FakeCpu32 cpu; auto p = h2g32(memory.at<void>());
  auto* words = memory.at<std::uint32_t>(); words[64] = 0xfeedbeef;
  for (int i = 4; i <= 11; ++i) cpu.set_x(i, 0x123400 + i);
  for (int i = 8; i <= 15; ++i) cpu.set_v(i, {0x1234567800000000ull + i, 0});
  cpu.set_sp(p + 8192); cpu.set_lr(0x12345679); cpu.set_fpcr(0x800010);
  cpu.set_x(0,p); cpu.set_x(1,1);
  t.blocked_signals = 1ull << 40;
  auto& save = function("sigsetjmp"); hle::CallContext save_ctx(t,cpu,save); save.handler(save_ctx);
  CHECK(words[64] == 0xfeedbeef); CHECK(words[29] == 0x12345679); CHECK(words[30] == p + 8192);
  for (int i = 4; i <= 11; ++i) cpu.set_x(i,0);
  cpu.set_v(8, {}); cpu.set_fpcr(0); cpu.set_sp(p + 4096); t.blocked_signals = 0;
  cpu.set_x(0,p); cpu.set_x(1,0);
  auto& restore = function("siglongjmp"); hle::CallContext restore_ctx(t,cpu,restore); restore.handler(restore_ctx);
  CHECK(cpu.x(0) == 1); CHECK(cpu.pc() == 0x12345679); CHECK(cpu.sp() == p + 8192);
  CHECK(cpu.x(11) == 0x12340b); CHECK(cpu.v(8).lo == 0x1234567800000008ull); CHECK(cpu.fpcr() == 0x800010);
  CHECK(t.blocked_signals == (1ull << 40)); t.blocked_signals = 0; t.signal_mask_snapshot.store(0);
}
std::uint32_t call(const char* name, std::initializer_list<std::uint32_t> args) {
  auto& t = GuestThread::adopt_current();
  test::FakeCpu32 cpu;
  test::LowMemory32 memory;
  cpu.set_sp(memory.address);
  unsigned i = 0;
  for (auto a : args) { if (i < 4) cpu.set_x(i, a); else memory.at<std::uint32_t>()[i - 4] = a; ++i; }
  const auto& info = function(name);
  REQUIRE(info.handler != hle::a32::diagnostic);
  hle::CallContext context(t, cpu, info);
  if (!info.try_handler || !info.try_handler(context)) info.handler(context);
  return static_cast<std::uint32_t>(cpu.x(0));
}
}
TEST_CASE("A32 pthread wire sizes, static types and live owner indices") {
  test::LowMemory32 memory;
  auto p = h2g32(memory.at<void>());
  auto* words = memory.at<std::uint32_t>();
  words[1] = 0xdeadbeef;
  CHECK(call("pthread_mutex_init", {p, 0}) == 0);
  CHECK(words[1] == 0xdeadbeef);
  for (auto type : {0u, 0x4000u, 0x8000u}) {
    words[0] = type;
    REQUIRE(call("pthread_mutex_lock", {p}) == 0);
    CHECK((words[0] >> 16) == GuestThread::current()->mutex_owner_index());
    CHECK(call("pthread_mutex_trylock", {p}) == (type == 0x4000 ? 0 : err::kEBUSY));
    if (type == 0x4000) {
      CHECK((words[0] & 0x1ffc) == 4);
      CHECK(call("pthread_mutex_unlock", {p}) == 0);
    }
    CHECK(call("pthread_mutex_unlock", {p}) == 0);
    CHECK(words[0] == type);
    CHECK(words[1] == 0xdeadbeef);
  }
  CHECK(call("pthread_cond_init", {p, 0}) == 0);
  CHECK(call("pthread_cond_signal", {p}) == 0);
  CHECK(words[0] == 4); CHECK(words[1] == 0xdeadbeef);
  CHECK(call("sem_init", {p, 0, 1}) == 0);
  CHECK(call("sem_wait", {p}) == 0);
  CHECK(call("sem_trywait", {p}) == UINT32_MAX);
  CHECK(*GuestThread::current()->errno_location() == err::kEAGAIN);
  CHECK(call("sem_post", {p}) == 0); CHECK(words[1] == 0xdeadbeef);
  words[6] = 0xdeadbeef;
  CHECK(call("pthread_attr_init", {p}) == 0);
  CHECK(words[2] == 1024 * 1024); CHECK(words[3] == 4096); CHECK(words[6] == 0xdeadbeef);
  words[9] = 0xdeadbeef;
  CHECK(call("pthread_attr_getstacksize", {p, p + 32}) == 0);
  CHECK(words[8] == 1024 * 1024); CHECK(words[9] == 0xdeadbeef);
  words[10] = 0xdeadbeef;
  CHECK(call("pthread_rwlock_init", {p, 0}) == 0);
  CHECK(call("pthread_rwlock_rdlock", {p}) == 0);
  CHECK(call("pthread_rwlock_rdlock", {p}) == 0);
  CHECK(call("pthread_rwlock_unlock", {p}) == 0);
  CHECK(call("pthread_rwlock_unlock", {p}) == 0);
  CHECK(words[10] == 0xdeadbeef);
}
TEST_CASE("A32 syscall and SVC use ARM numbers, wire pointers and Linux error conventions") {
  library();
  auto& thread = GuestThread::adopt_current();
  auto& cpu = thread.cpu();
  CpuState32 saved(cpu);
  struct Restore { CpuState32& saved; Cpu& cpu; ~Restore() { saved.restore(cpu); } } restore{saved, cpu};
  test::LowMemory32 memory;
  auto p = h2g32(memory.at<void>());
  auto* words = memory.at<std::uint32_t>();
  std::memset(words, 0xa5, 4096);
  CHECK(call("syscall", {20}) == 1000);
  CHECK(call("syscall", {263, 1, p}) == 0);
  CHECK(words[1] < 1000000000); CHECK(words[2] == 0xa5a5a5a5);
  CHECK(call("syscall", {197, 1, p + 128}) == 0);
  CHECK(words[32 + 26] == 0xa5a5a5a5);
  CHECK(call("syscall", {6, UINT32_MAX}) == UINT32_MAX);
  CHECK(*thread.errno_location() == err::kEBADF);
  CHECK(call("syscall", {0xdead}) == UINT32_MAX);
  CHECK(*thread.errno_location() == err::kENOSYS);
  cpu.set_x(7, 263); cpu.set_x(0, 1); cpu.set_x(1, p + 16);
  thread.set_errno(123);
  linux_syscall32(thread);
  CHECK(cpu.x(0) == 0); CHECK(words[6] == 0xa5a5a5a5);
  CHECK(*thread.errno_location() == 123);
  cpu.set_x(7, 6); cpu.set_x(0, UINT32_MAX);
  linux_syscall32(thread);
  CHECK(std::int32_t(cpu.x(0)) == -err::kEBADF); CHECK(*thread.errno_location() == 123);
  auto mapped = call("syscall", {192, 0, 4096, 3, 0x22, UINT32_MAX, 0});
  REQUIRE(mapped != UINT32_MAX);
  CHECK(AddressSpace::get().is_guest_accessible(mapped, 4096, true));
  CHECK(call("syscall", {91, mapped, 4096}) == 0);
  CHECK(!AddressSpace::get().is_guest_accessible(mapped, 4096, false));
  // EABI rt_sigaction takes an eight-byte kernel sigset, never sizeof(sigset_t).
  CHECK(call("syscall", {174, 10, 0, 0, 4}) == UINT32_MAX);
  CHECK(*thread.errno_location() == err::kEINVAL);
}
