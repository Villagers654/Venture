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
#include <chrono>
#include <thread>
#include <vector>
#include <bit>
#include <limits>

using namespace venture;
namespace {
std::uint32_t call(const char*, std::initializer_list<std::uint32_t>);
std::uint32_t call(HleLibrary&, const char*, std::initializer_list<std::uint32_t>);
thread_local std::uint32_t last_r1 = 0;  // r1 after this thread's last call (64-bit results)
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
HleLibrary& libstdcpp() {
  static HleLibrary lib("libstdc++.so");
  static const bool initialized = [] { hle::libc::register_libstdcpp(lib); return true; }();
  (void)initialized;
  return lib;
}
const hle::FunctionInfo& function(const char* name, HleLibrary& lib = library()) {
  auto thunk = lib.find_symbol(name);
  for (const auto& f : hle::registered_functions())
    if (f.thunk == thunk) {
      // Snapshots are values; fetch the stable registry entry via its ARM thunk id.
      auto* code = g2h<std::uint32_t>(thunk);
      if (code[0] == 0xe92d4010u) ++code; // condition waits open a real frame (push {r4, lr})
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
TEST_CASE("A32 iconv handles are guest addresses and its cursors stay 32-bit") {
  test::LowMemory32 memory;
  auto p = std::uint32_t(memory.address);
  auto* words = memory.at<std::uint32_t>();
  auto* errno_cell = GuestThread::adopt_current().errno_location();
  std::strcpy(memory.at<char>(0), "UTF-16BE"); std::strcpy(memory.at<char>(32), "u.t.f-008");
  std::strcpy(memory.at<char>(48), "UTF-16");
  std::memcpy(memory.at<char>(64), "A\xe2\x82\xac", 4);
  *errno_cell = 0;
  CHECK(call("iconv_open", {p, p + 48}) == UINT32_MAX);
  CHECK(*errno_cell == err::kEINVAL);
  auto cd = call("iconv_open", {p, p + 32});
  REQUIRE(cd != UINT32_MAX); REQUIRE(cd != 0);
  words[32] = p + 64; words[33] = 4; words[34] = 0xfeedbeef; // in, in_left, guard
  words[35] = p + 256; words[36] = 3; words[37] = 0xfeedbeef; // out, out_left, guard
  *errno_cell = 0;
  CHECK(call("iconv", {cd, p + 128, p + 132, p + 140, p + 144}) == UINT32_MAX);
  CHECK(*errno_cell == err::kE2BIG);
  CHECK(words[32] == p + 65); CHECK(words[33] == 3); CHECK(words[34] == 0xfeedbeef);
  CHECK(words[35] == p + 258); CHECK(words[36] == 1); CHECK(words[37] == 0xfeedbeef);
  words[36] = 8;
  CHECK(call("iconv", {cd, p + 128, p + 132, p + 140, p + 144}) == 0);
  CHECK(words[33] == 0); CHECK(words[36] == 6);
  CHECK(std::memcmp(memory.at<char>(256), "\x00\x41\x20\xac", 4) == 0);
  CHECK(call("iconv", {cd, 0, 0, 0, 0}) == 0);
  CHECK(call("iconv_close", {cd}) == 0);
  *errno_cell = 0;
  CHECK(call("iconv_close", {UINT32_MAX}) == UINT32_MAX);
  CHECK(*errno_cell == err::kEBADF);
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
std::uint32_t call(const char* name, std::initializer_list<std::uint32_t> args) { return call(library(), name, args); }
std::uint32_t call(HleLibrary& lib, const char* name, std::initializer_list<std::uint32_t> args) {
  auto& t = GuestThread::adopt_current();
  test::FakeCpu32 cpu;
  test::LowMemory32 memory;
  cpu.set_sp(memory.address);
  unsigned i = 0;
  for (auto a : args) { if (i < 4) cpu.set_x(i, a); else memory.at<std::uint32_t>()[i - 4] = a; ++i; }
  const auto& info = function(name, lib);
  REQUIRE(info.handler != hle::a32::diagnostic);
  hle::CallContext context(t, cpu, info);
  if (!info.try_handler || !info.try_handler(context)) info.handler(context);
  last_r1 = static_cast<std::uint32_t>(cpu.x(1));
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
TEST_CASE("A32 LP32-only relative condition waits time out and reacquire the mutex") {
  test::LowMemory32 memory;
  auto base = h2g32(memory.at<void>());
  auto* words = memory.at<std::uint32_t>();
  const auto mutex = base, cond = base + 4, timeout = base + 8;
  words[0] = 0; words[1] = 0; words[2] = 0; words[3] = 2000000; words[4] = 0xdeadbeef;
  REQUIRE(call("pthread_mutex_lock", {mutex}) == 0);
  auto start = std::chrono::steady_clock::now();
  CHECK(call("pthread_cond_timedwait_relative_np", {cond, mutex, timeout}) == err::kETIMEDOUT);
  CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(2));
  CHECK((words[0] >> 16) == GuestThread::current()->mutex_owner_index());
  words[3] = 1000000000;
  CHECK(call("pthread_cond_timedwait_relative_np", {cond, mutex, timeout}) == err::kEINVAL);
  start = std::chrono::steady_clock::now();
  CHECK(call("pthread_cond_timeout_np", {cond, mutex, 2}) == err::kETIMEDOUT);
  CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(2));
  CHECK((words[0] >> 16) == GuestThread::current()->mutex_owner_index());
  CHECK(call("pthread_mutex_unlock", {mutex}) == 0);
  CHECK(words[0] == 0); CHECK(words[4] == 0xdeadbeef);
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
TEST_CASE("A32 libstdc++ exports bionic's ARM names with 32-bit guard words") {
  auto& lib = libstdcpp();
  for (auto lp64 : {"_Znwm", "_Znam", "_ZdlPvm", "_ZnwmRKSt9nothrow_t", "_ZnamSt11align_val_t"})
    CHECK(lib.hle_symbol(lp64) == 0);
  for (auto name : {"__cxa_pure_virtual", "__cxa_deleted_virtual", "_ZnajRKSt9nothrow_t", "_ZdaPv",
                    "_ZdaPvRKSt9nothrow_t", "_ZdlPvSt11align_val_tRKSt9nothrow_t", "_ZdaPvjSt11align_val_t"})
    CHECK(function(name, lib).handler != hle::a32::diagnostic);
  test::LowMemory32 memory;
  const auto p = h2g32(memory.at<void>());
  auto* words = memory.at<std::uint32_t>();
  std::memset(words, 0xa5, 64);
  words[0] = 0;
  // ARM C++ ABI: compiled code tests bit 0 of the guard word; underway keeps it clear.
  CHECK(call(lib, "__cxa_guard_acquire", {p}) == 1);
  CHECK(words[0] != 0); CHECK((words[0] & 1) == 0);
  call(lib, "__cxa_guard_abort", {p});
  CHECK(words[0] == 0); CHECK(words[1] == 0xa5a5a5a5);
  REQUIRE(call(lib, "__cxa_guard_acquire", {p}) == 1);
  std::atomic<int> object{0}, initializers{0}, seen{0};
  std::vector<std::thread> waiters;
  for (int i = 0; i < 4; ++i)
    waiters.emplace_back([&] {
      if (call(lib, "__cxa_guard_acquire", {p})) initializers.fetch_add(1);
      if (object.load() == 42) seen.fetch_add(1);
    });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  object = 42;
  call(lib, "__cxa_guard_release", {p});
  for (auto& t : waiters) t.join();
  CHECK(initializers == 0); CHECK(seen == 4);
  CHECK(words[0] == 1); CHECK(words[1] == 0xa5a5a5a5);
  CHECK(call(lib, "__cxa_guard_acquire", {p}) == 0);
  // operator new/delete in ARM's unsigned int size_t mangling.
  const auto block = call(lib, "_Znwj", {24});
  REQUIRE(block != 0);
  std::memset(g2h<void>(block), 0x5a, 24);
  call(lib, "_ZdlPvj", {block, 24});
  const auto nothrow = static_cast<std::uint32_t>(lib.hle_symbol("_ZSt7nothrow"));
  REQUIRE(nothrow != 0);
  const auto array = call(lib, "_ZnajRKSt9nothrow_t", {16, nothrow});
  REQUIRE(array != 0);
  call(lib, "_ZdaPvRKSt9nothrow_t", {array, nothrow});
  const auto aligned = call(lib, "_ZnwjSt11align_val_t", {64, 256});
  REQUIRE(aligned != 0); CHECK(aligned % 256 == 0);
  call(lib, "_ZdlPvjSt11align_val_t", {aligned, 64, 256});
}
TEST_CASE("A32 libc process, numeric, inet, syslog and mremap adapters keep ARM layouts") {
  test::LowMemory32 memory;
  const auto p = h2g32(memory.at<void>());
  auto* words = memory.at<std::uint32_t>();
  std::memset(words, 0xa5, 4096);
  auto& thread = GuestThread::adopt_current();
  // struct rusage: two 32-bit timevals and 14 longs, 72 bytes.
  CHECK(call("getrusage", {0, p}) == 0);
  CHECK(words[1] < 1000000); CHECK(words[2] == 0); CHECK(words[17] == 0); CHECK(words[18] == 0xa5a5a5a5);
  std::strcpy(memory.at<char>(256), "inet");
  const auto group = call("getgrnam", {p + 256});
  REQUIRE(group != 0);
  const auto* g = g2h<std::uint32_t>(group);
  CHECK(std::strcmp(g2h<char>(g[0]), "inet") == 0); CHECK(g[1] == 0); CHECK(g[2] == 3003);
  CHECK(*g2h<std::uint32_t>(g[3]) == 0);
  std::strcpy(memory.at<char>(256), "no-such-group");
  CHECK(call("getgrnam", {p + 256}) == 0);
  // div_t/ldiv_t (8 bytes) and lldiv_t/imaxdiv_t (16) return through memory at r0.
  call("div", {p + 512, std::uint32_t(-7), 2});
  CHECK(std::int32_t(words[128]) == -3); CHECK(std::int32_t(words[129]) == -1); CHECK(words[130] == 0xa5a5a5a5);
  call("ldiv", {p + 512, 0x80000000u, UINT32_MAX});
  CHECK(words[128] == 0x80000000u); CHECK(words[129] == 0);
  const std::int64_t dividend = -9000000000ll, divisor = 7;
  for (auto name : {"lldiv", "imaxdiv"}) {
    std::memset(memory.at<void>(512), 0xa5, 32);
    call(name, {p + 512, 0xdeadbeef, std::uint32_t(dividend), std::uint32_t(std::uint64_t(dividend) >> 32),
                std::uint32_t(divisor), 0});
    CHECK(*memory.at<std::int64_t>(512) == dividend / divisor);
    CHECK(*memory.at<std::int64_t>(520) == dividend % divisor);
    CHECK(words[132] == 0xa5a5a5a5);
  }
  // intmax_t is 64-bit on ARM: r0:r1 results, 32-bit end pointers.
  std::strcpy(memory.at<char>(640), "  -9000000000xyz");
  words[170] = 0; words[171] = 0xa5a5a5a5;
  auto low = call("strtoimax", {p + 640, p + 680, 10});
  CHECK(std::int64_t(low | std::uint64_t(last_r1) << 32) == -9000000000ll);
  CHECK(words[170] == p + 640 + 13); CHECK(words[171] == 0xa5a5a5a5);
  std::strcpy(memory.at<char>(640), "18446744073709551615");
  low = call("strtoumax", {p + 640, 0, 10});
  CHECK(low == UINT32_MAX); CHECK(last_r1 == UINT32_MAX);
  CHECK(call("imaxabs", {std::uint32_t(-5), UINT32_MAX}) == 5); CHECK(last_r1 == 0);
  std::strcpy(memory.at<char>(640), "-9000000000");
  low = call("strtoimax_l", {p + 640, 0, 10, 0});
  CHECK(std::int64_t(low | std::uint64_t(last_r1) << 32) == -9000000000ll);
  // ARM long is 32-bit: strtol/strtoul saturate there with ERANGE, as bionic's ARM build does.
  std::strcpy(memory.at<char>(640), "9000000000");
  thread.set_errno(0);
  CHECK(call("strtol", {p + 640, p + 680, 10}) == 0x7fffffffu);
  CHECK(*thread.errno_location() == err::kERANGE); CHECK(words[170] == p + 640 + 10);
  std::strcpy(memory.at<char>(640), "-9000000000");
  CHECK(call("strtol", {p + 640, 0, 10}) == 0x80000000u);
  std::strcpy(memory.at<char>(640), "4294967296");
  thread.set_errno(0);
  CHECK(call("strtoul", {p + 640, 0, 10}) == UINT32_MAX);
  CHECK(*thread.errno_location() == err::kERANGE);
  std::strcpy(memory.at<char>(640), "-1");
  thread.set_errno(0);
  CHECK(call("strtoul", {p + 640, 0, 10}) == UINT32_MAX);
  CHECK(*thread.errno_location() == 0);
  std::strcpy(memory.at<char>(640), "  42");
  CHECK(call("atoi", {p + 640}) == 42);
  CHECK(call("strtol_l", {p + 640, 0, 16, 0}) == 0x42);
  // in_addr is one word, by value in r0; inet_ntoa's buffer is guest memory.
  std::strcpy(memory.at<char>(768), "1.2.3.4");
  CHECK(call("inet_aton", {p + 768, p + 800}) == 1);
  CHECK(words[200] == 0x04030201); CHECK(words[201] == 0xa5a5a5a5);
  const auto text = call("inet_ntoa", {0x04030201});
  REQUIRE(text != 0);
  CHECK(std::strcmp(g2h<char>(text), "1.2.3.4") == 0);
  CHECK(call("setlogmask", {0}) == 255);
  std::strcpy(memory.at<char>(832), "venture");
  std::strcpy(memory.at<char>(848), "a32 syslog %d %s");
  call("openlog", {p + 832, 1, 8});
  call("syslog", {6, p + 848, 42, p + 832});
  call("closelog", {});
  // Process images and popen stay unavailable: ENOSYS, never the ABI trap.
  std::strcpy(memory.at<char>(880), "sh");
  std::strcpy(memory.at<char>(888), "r");
  CHECK(call("popen", {p + 880, p + 888}) == 0);
  CHECK(*thread.errno_location() == err::kENOSYS);
  for (auto name : {"execl", "execlp", "execv", "execvp"}) {
    thread.set_errno(0);
    CHECK(call(name, {p + 880, 0}) == UINT32_MAX);
    CHECK(*thread.errno_location() == err::kENOSYS);
  }
  CHECK(call("execve", {p + 880, 0, 0}) == UINT32_MAX);
  // mremap(void*, size_t, size_t, int, ...): MAP_FAILED is the 32-bit -1.
  const auto mapped = call("mmap", {0, 4096, 3, 0x22, UINT32_MAX, 0});
  REQUIRE(mapped != UINT32_MAX);
  *g2h<std::uint32_t>(mapped) = 0x12345678;
  const auto grown = call("mremap", {mapped, 4096, 8192, 1});
  REQUIRE(grown != UINT32_MAX);
  CHECK(*g2h<std::uint32_t>(grown) == 0x12345678);
  CHECK(AddressSpace::get().is_guest_accessible(grown, 8192, true));
  CHECK(call("mremap", {grown, 8192, 4096, 4}) == UINT32_MAX);
  CHECK(*thread.errno_location() == err::kEINVAL);
  CHECK(call("munmap", {grown, 8192}) == 0);
}
TEST_CASE("A32 sendmsg and recvmsg carry 28-byte msghdr and 8-byte iovec records") {
  test::LowMemory32 memory;
  const auto p = h2g32(memory.at<void>());
  auto* words = memory.at<std::uint32_t>();
  std::memset(words, 0xa5, 4096);
  REQUIRE(call("socketpair", {1, 1, 0, p}) == 0);
  const auto a = words[0], b = words[1];
  struct Close {
    std::uint32_t a, b;
    ~Close() { call("close", {a}); call("close", {b}); }
  } close{a, b};
  std::memcpy(memory.at<char>(64), "ven", 3);
  std::memcpy(memory.at<char>(72), "ture", 4);
  const std::uint32_t send_iov[]{p + 64, 3, p + 72, 4}, send_msg[]{0, 0, p + 128, 2, 0, 0, 0};
  std::memcpy(memory.at<void>(128), send_iov, sizeof(send_iov));
  std::memcpy(memory.at<void>(256), send_msg, sizeof(send_msg));
  CHECK(call("sendmsg", {a, p + 256, 0}) == 7);
  const std::uint32_t recv_iov[]{p + 512, 16}, recv_msg[]{0, 0, p + 160, 1, 0, 0, 0x7777};
  std::memcpy(memory.at<void>(160), recv_iov, sizeof(recv_iov));
  std::memcpy(memory.at<void>(320), recv_msg, sizeof(recv_msg));
  CHECK(call("recvmsg", {b, p + 320, 0}) == 7);
  CHECK(std::memcmp(memory.at<char>(512), "venture", 7) == 0);
  CHECK(words[85] == 0); CHECK(words[86] == 0); CHECK(words[87] == 0xa5a5a5a5);
  // mmsghdr: the msghdr then the 32-bit sent length.
  std::memcpy(memory.at<void>(384), send_msg, sizeof(send_msg));
  CHECK(call("sendmmsg", {a, p + 384, 1, 0}) == 1);
  CHECK(words[(384 + 28) / 4] == 7); CHECK(words[(384 + 32) / 4] == 0xa5a5a5a5);
  CHECK(call("recvmsg", {b, p + 320, 0}) == 7);
}
TEST_CASE("A32 libm keeps bionic's legacy isnan/isinf exports callable") {
  // libOculusXRPlugin (Unity OpenXR Oculus plugin) imports isnan; it was a SIGSYS trap.
  static HleLibrary libm("libm.so");
  static const bool initialized = [] { hle::libc::register_libm(libm); return true; }();
  (void)initialized;
  auto words = [](double v) {
    const auto bits = std::bit_cast<std::uint64_t>(v);
    return std::pair{std::uint32_t(bits), std::uint32_t(bits >> 32)};  // softfp: r0:r1
  };
  for (auto name : {"isnan", "__isnan"}) {
    auto [lo, hi] = words(std::numeric_limits<double>::quiet_NaN());
    CHECK(call(libm, name, {lo, hi}) != 0);
    std::tie(lo, hi) = words(1.5);
    CHECK(call(libm, name, {lo, hi}) == 0);
  }
  auto [lo, hi] = words(-std::numeric_limits<double>::infinity());
  CHECK(call(libm, "isinf", {lo, hi}) != 0);
  CHECK(call(libm, "__isnanf", {std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN())}) != 0);
  CHECK(call(libm, "__isnanf", {std::bit_cast<std::uint32_t>(2.0f)}) == 0);
}
