#include <doctest/doctest.h>
#include "core/address_space.h"
#include "core/hle_runtime.h"
#include "core/signals.h"
#include "core/signal_frame.h"
#include "core/thread_runtime.h"
#include "core/wait.h"
#include "cpu/state.h"
#include "hle/registry.h"
#include "core/linker.h"
#include "common/log.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cmath>
#include <type_traits>
#include <future>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "common/environment.h"
#include "core/hle.h"
#include "cpu/cpu.h"
#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif
using namespace venture;
namespace {
// Selects the JIT cold abort-state mode for CPUs created in this scope.
struct ColdAbortMode {
  explicit ColdAbortMode(const char* mode) { set(mode); }
  ~ColdAbortMode() { set(""); }
  static void set(const char* mode) {
#ifdef _WIN32
    _putenv_s("VENTURE_JIT_COLD_ABORT_STATE", mode);
#else
    if (*mode) setenv("VENTURE_JIT_COLD_ABORT_STATE", mode, 1); else unsetenv("VENTURE_JIT_COLD_ABORT_STATE");
#endif
    environment::refresh();
  }
};
// Preserve process-global diagnostic settings and signal actions even when a
// test assertion unwinds. These fixtures must not consume the caller's setting.
struct FaultCaptureScope {
  std::string previous_environment;
  bool had_environment = false;
  int signo = 0;
  signals::Action previous_action{};
  bool installed = false;
  FaultCaptureScope() {
    if (const auto* value = std::getenv("VENTURE_FAULT_MEMORY")) {
      had_environment = true;
      previous_environment = value;
    }
  }
  static void set_environment(const char* value) {
#ifdef _WIN32
    _putenv_s("VENTURE_FAULT_MEMORY", value ? value : "");
#else
    if (value) setenv("VENTURE_FAULT_MEMORY", value, 1);
    else unsetenv("VENTURE_FAULT_MEMORY");
#endif
    environment::refresh();
  }
  int install(int signal, const signals::Action& action, const char* spec) {
    signo = signal;
    const auto result = signals::set_action(signo, &action, &previous_action);
    if (result != 0) return result;
    installed = true;
    set_environment(spec);
    return 0;
  }
  int restore_action() {
    if (!installed) return 0;
    const auto result = signals::set_action(signo, &previous_action, nullptr);
    if (result == 0) installed = false;
    return result;
  }
  ~FaultCaptureScope() {
    restore_action();
    set_environment(had_environment ? previous_environment.c_str() : nullptr);
  }
};
// A failed access must deliver every register as it was before the access, whether
// the JIT stored earlier register writes before it (off), only on its abort exit
// (live, for values still used after the access) or always there (all).
void check_precise_register_bank(int thread_id, const char* mode) {
  ColdAbortMode cold(mode);
  GuestThread thread(thread_id); thread.attach_to_current_host_thread();
  auto& space = AddressSpace::get();
  const auto page = space.allocate(8192, kProtRead | kProtWrite, "[precise signal test]");
  REQUIRE(page);
  struct Free { AddressSpace& space; GuestAddr p; ~Free() { space.free(p); } } free{space, page};
  REQUIRE(space.protect(page, 4096, kProtRead) == 0);
  REQUIRE(space.protect(page + 4096, 4096, kProtNone) == 0);
  struct Expected {
    std::array<std::uint64_t, 31> x{};
    std::array<Vec128, 32> v{};
    std::uint64_t pc{}, sp{};
    std::uint32_t fpcr{}, fpsr{};
    unsigned calls{};
  } expected;
  auto handler = hle::make_thunk("test", "precise register bank", [](hle::CallContext& ctx) {
    auto& expected = *static_cast<Expected*>(ctx.user());
    const auto* info = g2h<signals::SigInfo>(ctx.x(1));
    auto* uc = g2h<signals::UContext>(ctx.x(2));
    CHECK(info->signo == signals::kSigSegv);
    CHECK(uc->mcontext.pc == expected.pc);
    CHECK(uc->mcontext.sp == expected.sp);
    CHECK(uc->mcontext.pstate == 0xa0000000);
    CHECK(uc->mcontext.fault_address == expected.x[0]);
    for (unsigned i = 0; i < expected.x.size(); ++i) {
      CAPTURE(i);
      CHECK(uc->mcontext.regs[i] == expected.x[i]);
    }
    const auto* fp = reinterpret_cast<const signals::FpsimdContext*>(uc->mcontext.reserved.data());
    CHECK(fp->magic == 0x46508001);
    CHECK(fp->fpcr == expected.fpcr);
    CHECK(fp->fpsr == expected.fpsr);
    for (unsigned i = 0; i < expected.v.size(); ++i) {
      CAPTURE(i);
      CHECK(fp->vregs[i].lo == expected.v[i].lo);
      CHECK(fp->vregs[i].hi == expected.v[i].hi);
    }
    ++expected.calls;
    uc->mcontext.pc += 4;  // Skip the failed access.
  }, &expected);
  signals::Action action{handler, 0, 4}, previous;
  REQUIRE(signals::set_action(signals::kSigSegv, &action, &previous) == 0);
  struct RestoreAction {
    signals::Action old;
    ~RestoreAction() { signals::set_action(signals::kSigSegv, &old, nullptr); }
  } restore{previous};
  // Scalar read/write, exclusive read/write, pair read/write, SIMD read/write, LSE add.
  // STXR reserves the read-only page first, then faults when writing it.
  for (const auto instruction : {0xf9400002u, 0xf9000002u, 0xc85f7c02u, 0xc8037c02u,
                                  0xa9400c02u, 0xa9000c02u, 0x3dc00002u, 0x3d800002u, 0xf8220003u}) {
    CAPTURE(instruction);
    const bool exclusive_write = instruction == 0xc8037c02u;
    std::vector<std::uint32_t> body;
    if (exclusive_write) body.push_back(0xc85f7c02); // LDXR X2,[X0]
    for (unsigned r = 1; r < 30; ++r) {
      const auto seed = 0xfedc000000001000ull + r;
      thread.cpu().set_x(r, seed);
      expected.x[r] = r % 2 ? seed + 1 : std::uint32_t(seed + 1);
      body.push_back((r % 2 ? 0x91000400u : 0x11000400u) | (r << 5) | r); // ADD #1 (X or W)
    }
    for (unsigned v = 0; v < 32; ++v) {
      const unsigned r = 3 + v % 27;
      const auto value = expected.x[r];
      if (v % 3 == 0) {
        expected.v[v] = {value, value};
        body.push_back(0x4e080c00u | (r << 5) | v); // DUP Vn.2D,Xr
      } else if (v % 3 == 1) {
        expected.v[v] = {value, 0};
        body.push_back(0x9e670000u | (r << 5) | v); // FMOV Dn,Xr
      } else {
        expected.v[v] = {std::uint32_t(value), 0};
        body.push_back(0x1e270000u | (r << 5) | v); // FMOV Sn,Wr
      }
    }
    body.push_back(0xd2b40002); // MOVZ X2,#0xa000,LSL #16
    body.push_back(0xd51b4202); // MSR NZCV,X2
    body.push_back(0xd2802042); // MOVZ X2,#0x102
    expected.x[2] = 0x102;
    body.push_back(0xd10043ff); // SUB SP,SP,#16
    const auto access_index = body.size();
    body.push_back(instruction);
    // Consume read results so dead-code elimination cannot erase the access.
    body.push_back(0xa9000fe2); // STP X2,X3,[SP]
    body.push_back(0x3d8003e2); // STR Q2,[SP]
    // Read and overwrite every register, so the pre-access writes are replaced
    // within the block while their values stay in use after the access.
    for (unsigned r = 1; r < 30; ++r) body.push_back(0x91000400u | (r << 5) | r); // ADD Xr,Xr,#1
    for (unsigned v = 0; v < 32; v += 3) body.push_back(0x4ee08400u | (v << 16) | (v << 5) | v); // ADD Vv.2D
    for (unsigned v = 0; v < 32; ++v) body.push_back(0x4e080c00u | v); // DUP Vn.2D,X0
    body.push_back(0xd51b421f); // MSR NZCV,XZR
    body.push_back(0x910043ff); // ADD SP,SP,#16
    body.push_back(0xd65f03c0); // RET
    const auto entry = hle::code_stub(body, "precise memory signal matrix");
    expected.x[0] = exclusive_write ? page : page + 4096;
    expected.x[30] = hle::call_return_trampoline(1);
    expected.pc = entry + 4 * access_index;
    expected.sp = thread.cpu().sp() - 16;
    expected.fpcr = thread.cpu().fpcr();
    expected.fpsr = thread.cpu().fpsr();
    // First fault enters fastmem recovery; the second exercises the resulting
    // callback-only retranslation.
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
      thread.cpu().clear_exclusive();
      thread.call(entry, {expected.x[0]});
    }
  }
  CHECK(expected.calls == 18);
}
} // namespace
TEST_CASE("A64 failed memory accesses deliver the complete pre-access register bank, precise stores") {
  check_precise_register_bank(96, "off");
}
TEST_CASE("A64 failed memory accesses deliver the complete pre-access register bank, live cold state") {
  check_precise_register_bank(97, "live");
}
TEST_CASE("A64 failed memory accesses deliver the complete pre-access register bank, all cold state") {
  check_precise_register_bank(98, "all");
}
namespace {
// Handler-visible state of one guest SIGSEGV.
struct SignalBank {
  std::array<std::uint64_t, 31> x{};
  std::array<Vec128, 32> v{};
  std::uint64_t pc{}, sp{}, pstate{}, fault{};
  std::uint64_t entry_sp{};  // the thread's SP at the call; threads differ
  std::uint32_t fpcr{}, fpsr{};
};
// X5, V7, SP and NZCV are written, one access succeeds, every other register is
// rewritten from its result (spills and reloads), then LDP faults on its second
// transfer before X5, V7, SP and NZCV are written again. Returns the bank seen
// on the fastmem-recovery run and on the callback-retranslation run.
constexpr std::uint64_t successive_loaded = 0x0123456789abcdefull;
std::vector<SignalBank> successive_access_banks(int thread_id, const char* mode, GuestAddr page) {
  ColdAbortMode cold(mode);
  GuestThread thread(thread_id); thread.attach_to_current_host_thread();
  std::vector<SignalBank> banks;
  auto handler = hle::make_thunk("test", "successive access bank", [](hle::CallContext& ctx) {
    auto& banks = *static_cast<std::vector<SignalBank>*>(ctx.user());
    auto* uc = g2h<signals::UContext>(ctx.x(2));
    CHECK(g2h<signals::SigInfo>(ctx.x(1))->signo == signals::kSigSegv);
    SignalBank bank;
    for (unsigned i = 0; i < bank.x.size(); ++i) bank.x[i] = uc->mcontext.regs[i];
    bank.pc = uc->mcontext.pc; bank.sp = uc->mcontext.sp;
    bank.pstate = uc->mcontext.pstate; bank.fault = uc->mcontext.fault_address;
    const auto* fp = reinterpret_cast<const signals::FpsimdContext*>(uc->mcontext.reserved.data());
    bank.fpcr = fp->fpcr; bank.fpsr = fp->fpsr;
    for (unsigned i = 0; i < bank.v.size(); ++i) bank.v[i] = {fp->vregs[i].lo, fp->vregs[i].hi};
    banks.push_back(bank);
    uc->mcontext.pc += 4;  // Skip the failed LDP.
  }, &banks);
  signals::Action action{handler, 0, 4}, previous;
  REQUIRE(signals::set_action(signals::kSigSegv, &action, &previous) == 0);
  struct RestoreAction {
    signals::Action old;
    ~RestoreAction() { signals::set_action(signals::kSigSegv, &old, nullptr); }
  } restore{previous};
  for (unsigned r = 1; r < 30; ++r) thread.cpu().set_x(r, 0xfedc000000002000ull + r);
  std::vector<std::uint32_t> body{
      0x910004a5,  // ADD X5,X5,#1
      0x4e080ca7,  // DUP V7.2D,X5
      0xd2b40002,  // MOVZ X2,#0xa000,LSL #16
      0xd51b4202,  // MSR NZCV,X2
      0xd10043ff,  // SUB SP,SP,#16
      0xf9400006,  // LDR X6,[X0] (succeeds)
  };
  // Rewrite every other register from the loaded value: more live values than
  // host registers, so the JIT spills and reloads across the next access.
  for (unsigned r = 7; r < 30; ++r) body.push_back(0x8b060000u | (r << 5) | r);  // ADD Xr,Xr,X6
  for (unsigned v = 8; v < 32; ++v) body.push_back(0x4e080c00u | (6 << 5) | v);  // DUP Vv.2D,X6
  body.push_back(0xa9400c02);  // LDP X2,X3,[X0] (second transfer faults)
  const auto fault_index = body.size() - 1;
  // Overwrite X5, V7, SP and NZCV after the fault, and consume the LDP result.
  body.push_back(0x8b0600a5);  // ADD X5,X5,X6
  body.push_back(0x4e080ca7);  // DUP V7.2D,X5
  body.push_back(0xa9000fe2);  // STP X2,X3,[SP]
  body.push_back(0xd51b421f);  // MSR NZCV,XZR
  body.push_back(0x910043ff);  // ADD SP,SP,#16
  for (unsigned r = 7; r < 30; ++r) body.push_back(0x8b060000u | (r << 5) | r);
  body.push_back(0xd65f03c0);  // RET
  const auto entry = hle::code_stub(body, "successive access signal");
  // First fault enters fastmem recovery; the second runs the callback-only retranslation.
  for (unsigned repeat = 0; repeat < 2; ++repeat) {
    for (unsigned r = 1; r < 30; ++r) thread.cpu().set_x(r, 0xfedc000000002000ull + r);
    const auto entry_sp = thread.cpu().sp();
    thread.call(entry, {page + 4096 - 8});
    REQUIRE(banks.size() == repeat + 1);
    banks.back().entry_sp = entry_sp;
  }
  for (const auto& bank : banks) {
    CHECK(bank.pc == entry + 4 * fault_index);
    CHECK(bank.sp == bank.entry_sp - 16);
    CHECK(bank.x[5] == 0xfedc000000002006ull);
    CHECK(bank.v[7].lo == 0xfedc000000002006ull);
    CHECK(bank.pstate == 0xa0000000);
    CHECK(bank.x[6] == successive_loaded);
    CHECK(bank.fault == page + 4096);
  }
  return banks;
}
void check_successive_access_matches_off(int thread_id, const char* mode) {
  auto& space = AddressSpace::get();
  const auto page = space.allocate(8192, kProtRead | kProtWrite, "[successive access test]");
  REQUIRE(page);
  struct Free { AddressSpace& space; GuestAddr p; ~Free() { space.free(p); } } free{space, page};
  std::memcpy(g2h<void>(page + 4096 - 8), &successive_loaded, sizeof(successive_loaded));
  REQUIRE(space.protect(page, 4096, kProtRead) == 0);
  REQUIRE(space.protect(page + 4096, 4096, kProtNone) == 0);
  const auto off = successive_access_banks(thread_id, "off", page);
  const auto banks = successive_access_banks(thread_id + 1, mode, page);
  for (unsigned run = 0; run < 2; ++run) {
    CAPTURE(run);
    const auto& a = off[run];
    const auto& b = banks[run];
    for (unsigned i = 0; i < a.x.size(); ++i) {
      CAPTURE(i);
      CHECK(b.x[i] == a.x[i]);
    }
    for (unsigned i = 0; i < a.v.size(); ++i) {
      CAPTURE(i);
      CHECK(b.v[i].lo == a.v[i].lo);
      CHECK(b.v[i].hi == a.v[i].hi);
    }
    CHECK(b.sp - b.entry_sp == a.sp - a.entry_sp);
    CHECK(b.pstate == a.pstate);
    CHECK(b.fault == a.fault);
    CHECK(b.fpcr == a.fpcr);
    CHECK(b.fpsr == a.fpsr);
  }
}
} // namespace
TEST_CASE("A64 a fault after a successful access in the same block matches precise stores, live cold state") {
  check_successive_access_matches_off(99, "live");
}
TEST_CASE("A64 a fault after a successful access in the same block matches precise stores, all cold state") {
  check_successive_access_matches_off(101, "all");
}
TEST_CASE("guest grow-down faults retry instructions and a stack-limit fault delivers SIGSEGV") {
  auto& space = AddressSpace::get();
  const auto old_limit = space.stack_limit();
  struct Restore { AddressSpace& s; std::uint64_t n; ~Restore() { s.set_stack_limit(n); } } restore{space, old_limit};
  space.set_stack_limit(2 * kGuestPageSize);
  const auto p = space.map(0, kGuestPageSize, kProtRead | kProtWrite,
      kMapPrivate | kMapAnonymous | kMapGrowDown | kMapStack, nullptr, 0, "[guest grow-down stack]");
  REQUIRE(p > 0);
  GuestThread thread(92); thread.attach_to_current_host_thread();
  struct Fault { int calls = 0; GuestAddr address; } fault{0, GuestAddr(p - 2 * kGuestPageSize)};
  auto handler = hle::make_thunk("test", "grow-down cap fault", [](hle::CallContext& ctx) {
    auto& f = *static_cast<Fault*>(ctx.user()); ++f.calls;
    auto* context = g2h<signals::UContext>(ctx.x(2));
    CHECK(ctx.x(0) == signals::kSigSegv);
    CHECK(context->mcontext.fault_address == f.address);
    context->mcontext.pc += 4;
    context->mcontext.regs[0] = 42;
  }, &fault);
  signals::Action action{handler, 0, 4}, previous;
  REQUIRE(signals::set_action(signals::kSigSegv, &action, &previous) == 0);
  constexpr std::array read_code{0xf9400000u, 0xd65f03c0u}; // ldr x0,[x0]; ret
  constexpr std::array write_code{0xf9000001u, 0xd65f03c0u}; // str x1,[x0]; ret
  auto reader = hle::code_stub(read_code, "grow-down read");
  auto writer = hle::code_stub(write_code, "grow-down write");
  CHECK(thread.call(reader, {std::uint64_t(p - 8)}) == 0);
  CHECK(fault.calls == 0);
  CHECK(thread.call(writer, {std::uint64_t(p - 8), 91}) == p - 8);
  CHECK(*g2h<std::uint64_t>(p - 8) == 91);
  CHECK(thread.call(reader, {fault.address}) == 42);
  CHECK(fault.calls == 1);
  CHECK_FALSE(space.is_guest_accessible(fault.address, 8, false));
  CHECK(signals::set_action(signals::kSigSegv, &previous, nullptr) == 0);
  CHECK(space.unmap(p - kGuestPageSize, 2 * kGuestPageSize) == 0);
}

TEST_CASE("HLE string faults use guest signals and retry repaired arguments") {
  register_all_hle_libraries();
  GuestThread thread(79); thread.attach_to_current_host_thread();
  auto* libc = Linker::get().open("libc.so", kRtldNow, nullptr);
  REQUIRE(libc);
  auto handler = hle::make_thunk("test", "repair strlen pointer", [](hle::CallContext& ctx) {
    auto* context = g2h<signals::UContext>(ctx.x(2));
    CHECK(context->mcontext.fault_address == 0);
    context->mcontext.regs[0] = h2g("repaired");
  });
  signals::Action action{handler, 0, 4}, previous;
  REQUIRE(signals::set_action(signals::kSigSegv, &action, &previous) == 0);
  CHECK(thread.call(libc->find_symbol("strlen"), {0}) == 8);
  CHECK(thread.call(libc->find_symbol("strnlen"), {0, 0}) == 0);
  CHECK(signals::set_action(signals::kSigSegv, &previous, nullptr) == 0);
  auto page = AddressSpace::get().allocate(kGuestPageSize*2, kProtRead | kProtWrite, "[string boundary]");
  REQUIRE(page);
  std::memcpy(g2h(page+kGuestPageSize-3), "ok", 3);
  REQUIRE(AddressSpace::get().protect(page+kGuestPageSize, kGuestPageSize, kProtNone) == 0);
  CHECK(thread.call(libc->find_symbol("strlen"), {page+kGuestPageSize-3}) == 2);
  // Block reads stop at the guest page end: a string ending before a
  // protected page compares and searches without a fault.
  const auto boundary = page+kGuestPageSize-3;
  const auto call_int = [&](const char* name, std::initializer_list<std::uint64_t> args) {
    return static_cast<int>(thread.call(libc->find_symbol(name), args));
  };
  CHECK(call_int("strcmp", {boundary, h2g("ok")}) == 0);
  CHECK(call_int("strcmp", {h2g("okay"), boundary}) > 0);
  CHECK(call_int("strncmp", {boundary, h2g("okay"), 4}) < 0);
  CHECK(call_int("strncmp", {boundary, h2g("oz"), 1}) == 0);
  CHECK(thread.call(libc->find_symbol("strstr"), {boundary, h2g("k")}) == boundary+1);
  CHECK(thread.call(libc->find_symbol("strstr"), {boundary, h2g("ok")}) == boundary);
  CHECK(thread.call(libc->find_symbol("strstr"), {boundary, h2g("oka")}) == 0);
  AddressSpace::get().free(page);
  Linker::get().close(libc);
}
TEST_CASE("HLE string comparisons and searches match across block boundaries") {
  register_all_hle_libraries();
  GuestThread thread(80); thread.attach_to_current_host_thread();
  auto* libc = Linker::get().open("libc.so", kRtldNow, nullptr);
  REQUIRE(libc);
  const auto strstr_at = [&](const std::string& hay, const std::string& needle) -> std::int64_t {
    const auto found = thread.call(libc->find_symbol("strstr"), {h2g(hay.c_str()), h2g(needle.c_str())});
    return found ? static_cast<std::int64_t>(found - h2g(hay.c_str())) : -1;
  };
  // Matches straddling the first short block and a following full block.
  CHECK(strstr_at(std::string(14, 'x') + "abcd", "abcd") == 14);
  CHECK(strstr_at(std::string(269, 'x') + "abcd" + std::string(50, 'x'), "abcd") == 269);
  CHECK(strstr_at(std::string(269, 'x') + "abcd" + std::string(50, 'x'), "abce") == -1);
  // Needles longer than a block keep their prefix across block reads.
  CHECK(strstr_at(std::string(600, 'a'), std::string(300, 'a')) == 0);
  CHECK(strstr_at(std::string(500, 'a') + "b", std::string(299, 'a') + "b") == 201);
  CHECK(strstr_at("short", std::string(300, 's')) == -1);
  CHECK(strstr_at("", "a") == -1);
  CHECK(strstr_at("abc", "") == 0);
  const auto compare = [&](const char* name, const std::string& a, const std::string& b, std::uint64_t n = 0) {
    const auto r = static_cast<int>(thread.call(libc->find_symbol(name), {h2g(a.c_str()), h2g(b.c_str()), n}));
    return (r > 0) - (r < 0);
  };
  for (std::size_t at : {0u, 15u, 16u, 271u, 272u, 700u}) {
    auto a = std::string(800, 'q'), b = a;
    b[at] = 'r';
    CHECK(compare("strcmp", a, b) == -1);
    CHECK(compare("strcmp", b, a) == 1);
    CHECK(compare("strncmp", a, b, at) == 0);
    CHECK(compare("strncmp", a, b, at + 1) == -1);
    CHECK(compare("strcmp", a.substr(0, at), a) == -1);
    CHECK(compare("strcmp", a, a.substr(0, at)) == 1);
    CHECK(compare("strncmp", a.substr(0, at), a, 2000) == -1);
  }
  CHECK(compare("strcmp", std::string(800, 'q'), std::string(800, 'q')) == 0);
  CHECK(compare("strcmp", "\xff", "a") == 1); // unsigned char comparison
  CHECK(compare("strncmp", "abc", "abd", 0) == 0);
  // Guest strcmp: word loop for equal alignment, bytes otherwise, every offset.
  alignas(16) char x[64], y[64];
  for (int ox = 0; ox < 8; ++ox)
    for (int oy = 0; oy < 8; ++oy)
      for (int length : {0, 1, 7, 8, 9, 23, 40}) {
        std::memset(x, 'k', sizeof x); std::memset(y, 'k', sizeof y);
        x[ox + length] = 0; y[oy + length] = 0;
        const auto order = [&](const char* a, const char* b) {
          const auto r = static_cast<int>(thread.call(libc->find_symbol("strcmp"), {h2g(a), h2g(b)}));
          return (r > 0) - (r < 0);
        };
        CAPTURE(ox);
        CAPTURE(oy);
        CAPTURE(length);
        CHECK(order(x + ox, y + oy) == 0);
        if (length) {
          y[oy + length - 1] = '\x90';  // unsigned: above 'k'
          CHECK(order(x + ox, y + oy) == -1);
          CHECK(order(y + oy, x + ox) == 1);
          y[oy + length - 1] = 'k';
        }
        y[oy + length] = 'k'; y[oy + length + 1] = 0;  // y one byte longer
        CHECK(order(x + ox, y + oy) == -1);
      }
  Linker::get().close(libc);
}
TEST_CASE("A64 bsearch calls the guest comparator and returns bionic's element") {
  register_all_hle_libraries();
  GuestThread thread(82); thread.attach_to_current_host_thread();
  auto* libc = Linker::get().open("libc.so", kRtldNow, nullptr);
  REQUIRE(libc);
  static int calls = 0;
  const auto compare = hle::make_thunk("bsearch-test", "compare", [](hle::CallContext& c) {
    ++calls;
    const int a = *g2h<int>(c.x(0)), b = *g2h<int>(c.x(1));
    c.return_x(static_cast<std::uint64_t>(static_cast<std::int64_t>((a > b) - (a < b))));
  });
  const int values[] = {1, 3, 3, 3, 5, 8, 13, 21, 34};
  const auto find = [&](int key, std::uint64_t count) -> std::int64_t {
    const auto r = thread.call(libc->find_symbol("bsearch"), {h2g(&key), h2g(values), count, sizeof(int), compare});
    return r ? static_cast<std::int64_t>((r - h2g(values)) / sizeof(int)) : -1;
  };
  for (int i = 0; i < 9; ++i) if (values[i] != 3) CHECK(find(values[i], 9) == i);
  CHECK(find(3, 9) == 2);  // the probe sequence's first equal element
  CHECK(find(0, 9) == -1);
  CHECK(find(4, 9) == -1);
  CHECK(find(40, 9) == -1);
  CHECK(find(1, 0) == -1);
  calls = 0;
  CHECK(find(34, 9) == 8);
  CHECK(calls == 3);
  Linker::get().close(libc);
}
#if defined(__x86_64__) || defined(_M_X64)
TEST_CASE("A64 libm in the JIT keeps the guest MXCSR: guest rounding, FPSR flags and errno") {
  register_all_hle_libraries();
  GuestThread thread(81); thread.attach_to_current_host_thread();
  auto* libm = Linker::get().open("libm.so", kRtldNow, nullptr);
  REQUIRE(libm);
  // Pure math binds to the guest-MXCSR fast call; scalbln keeps the host
  // environment (ldexp and scalbn are guest routines).
  CHECK(hle::resolve_thunk(Linker::get().symbol(libm, "atanf"), kSvcHleCallFastGuestFp));
  CHECK(hle::resolve_thunk(Linker::get().symbol(libm, "scalbln"), kSvcHleCallFastFp));
  // GuestThread::call restores FPSR, so a guest stub reports it: x0 = FPSR after blr x0.
  constexpr std::array<std::uint32_t, 5> probe_code{0xa9bf7bfdu, 0xd63f0000u, 0xd53b4420u, 0xa8c17bfdu, 0xd65f03c0u};
  const auto probe = hle::code_stub(probe_code, "libm FPSR probe");
  const auto run = [&](const char* name) {
    const std::uint64_t target[] = {Linker::get().symbol(libm, name)};
    return thread.call(probe, GuestCallArgs{target, {}});
  };
  const auto host_csr = _mm_getcsr();
  constexpr unsigned rounding[] = {0, 2u << 13, 1u << 13, 3u << 13};
  const auto guest_flags = [](unsigned csr) { return (csr & 1) | ((csr >> 1) & 0x1e); };
  // The reference runs the same host routine in a clean MXCSR with the guest rounding mode.
  const auto check = [&](const char* name, unsigned mode, auto x, auto reference) {
    using T = decltype(x);
    using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
    _mm_setcsr(0x1f80u | rounding[mode]);
    const T expected = reference(x);
    const auto flags = guest_flags(_mm_getcsr());
    _mm_setcsr(host_csr);
    thread.cpu().set_fpcr(mode << 22);
    thread.cpu().set_fpsr(0);
    thread.cpu().set_v(0, {std::bit_cast<Bits>(x), 0});
    const auto r = run(name);
    INFO(name << " mode " << mode);
    CHECK(Bits(r.v0.lo) == std::bit_cast<Bits>(expected));
    CHECK(r.x0 == flags);
    CHECK_FALSE(hle::guest_fp_environment);
    CHECK(_mm_getcsr() == host_csr);
  };
  for (unsigned mode = 0; mode < 4; ++mode) {
    check("sqrt", mode, 2.0, [](double v) { double (*volatile f)(double) = &std::sqrt; return f(v); });
    check("atanf", mode, 0.3f, [](float v) { float (*volatile f)(float) = &std::atan; return f(v); });
    check("expf", mode, 100.0f, [](float v) { float (*volatile f)(float) = &std::exp; return f(v); });
    check("floor", mode, 2.5, [](double v) { double (*volatile f)(double) = &std::floor; return f(v); });
  }
  // Invalid operation: guest FPSR.IOC and guest EDOM, host errno untouched.
  errno = 0;
  thread.cpu().set_fpcr(0); thread.cpu().set_fpsr(0);
  thread.cpu().set_v(0, {std::bit_cast<std::uint64_t>(-1.0), 0});
  *thread.errno_location() = 0;
  const auto invalid = run("sqrt");
  CHECK(std::isnan(std::bit_cast<double>(invalid.v0.lo)));
  CHECK((invalid.x0 & 1) == 1);
  CHECK(*thread.errno_location() == 33); // EDOM
  CHECK(errno == 0);
  // floorf quiets a signalling NaN without host FP operations; IOC still reaches FPSR.
  thread.cpu().set_fpsr(0);
  thread.cpu().set_v(0, {0x7f800001u, 0});
  const auto quieted = run("floorf");
  CHECK(std::uint32_t(quieted.v0.lo) == 0x7fc00001u);
  CHECK(quieted.x0 == 1);
  CHECK(_mm_getcsr() == host_csr);
  Linker::get().close(libm);
}
#endif
TEST_CASE("HLE native memory and string accesses repair invalid guest arguments in JIT dispatch") {
  register_all_hle_libraries();
  GuestThread thread(62902); thread.attach_to_current_host_thread();
  auto* libc = Linker::get().open("libc.so", kRtldNow, nullptr);
  REQUIRE(libc);
  const auto native_symbol = [](std::string_view name) {
    const auto functions = hle::registered_functions();
    auto found = std::find_if(functions.begin(), functions.end(), [&](const auto& f) {
      return f.library == "libc.so" && f.name == name;
    });
    REQUIRE(found != functions.end());
    CHECK(found->execution == hle::Execution::InJit);
    return found->thunk;
  };
  auto p = AddressSpace::get().allocate(3*kGuestPageSize, kProtRead | kProtWrite, "[HLE access fixture]");
  REQUIRE(p);
  std::memcpy(g2h(p), "okay", 5);
  REQUIRE(AddressSpace::get().protect(p+kGuestPageSize, kGuestPageSize, kProtNone) == 0);
  // Routines may touch any byte of the argument first, depending on the CPU's variant (native memcpy) or on
  // word-at-a-time scans (memchr, strcpy, strcmp); span is the bytes the call may access.
  struct Repair { unsigned argument; GuestAddr replacement, bad, span; unsigned calls = 0; } repair{};
  auto handler = hle::make_thunk("test", "repair declared guest buffer", [](hle::CallContext& ctx) {
    auto& repair = *static_cast<Repair*>(ctx.user());
    ++repair.calls;
    auto* uc = g2h<signals::UContext>(ctx.x(2));
    CHECK(uc->mcontext.fault_address - repair.bad < repair.span);
    uc->mcontext.regs[repair.argument] = repair.replacement;
  }, &repair);
  signals::Action action{handler, 0, 4}, previous;
  REQUIRE(signals::set_action(signals::kSigSegv, &action, &previous) == 0);
  const auto bad = p+kGuestPageSize, destination = p+2*kGuestPageSize;
  repair = {1, p, bad, 5};
  CHECK(thread.call(native_symbol("memcpy"), {destination, bad, 5}) == destination);
  CHECK(repair.calls == 1); CHECK(std::memcmp(g2h(destination), "okay", 5) == 0);
  repair = {1, p, 0x0000800000001000ull, 5}; // noncanonical on ordinary x86-64 user mappings
  CHECK(thread.call(native_symbol("memcpy"), {destination, repair.bad, 5}) == destination);
  CHECK(repair.calls == 1);
  repair = {0, destination, UINT64_MAX-8, 32};
  CHECK(thread.call(native_symbol("memcpy"), {repair.bad, p, 32}) == destination);
  CHECK(repair.calls == 1);
  repair = {1, p, UINT64_MAX-8, 32};
  CHECK(thread.call(native_symbol("memcpy"), {destination, repair.bad, 32}) == destination);
  CHECK(repair.calls == 1);
  repair = {0, destination, bad, 5};
  CHECK(thread.call(native_symbol("memset"), {bad, 0x61, 5}) == destination);
  CHECK(repair.calls == 1);
  repair = {0, p, bad, 5};
  CHECK(thread.call(libc->find_symbol("memchr"), {bad, 'k', 5}) == p+1);
  CHECK(repair.calls == 1);
  repair = {1, p, bad, 5};
  CHECK(thread.call(native_symbol("memcmp"), {p, bad, 5}) == 0);
  CHECK(repair.calls == 1);
  repair = {0, destination, bad, 5};
  CHECK(thread.call(libc->find_symbol("strcpy"), {bad, p}) == destination);
  CHECK(repair.calls == 1);
  repair = {1, p, bad, 5};
  // A64 strcmp is a guest routine whose faults are ordinary guest faults; the
  // argument repair contract belongs to the host handler.
  CHECK(thread.call(native_symbol("strcmp"), {p, bad}) == 0);
  CHECK(repair.calls == 1);
  CHECK(thread.call(native_symbol("memcpy"), {0, 0, 0}) == 0);
  CHECK(thread.call(native_symbol("memmove"), {destination+1, destination, 4}) == destination+1);
  CHECK(std::memcmp(g2h(destination), "ookay", 5) == 0);
  auto bus_handler = hle::make_thunk("test", "mapped backing error handler", [](hle::CallContext& ctx) {
    auto* info = g2h<signals::SigInfo>(ctx.x(1));
    auto* uc = g2h<signals::UContext>(ctx.x(2));
    CHECK(info->signo == signals::kSigBus); CHECK(info->code == 2);
    CHECK(uc->mcontext.fault_address == 0x1234);
    uc->mcontext.pc += 4; uc->mcontext.regs[0] = 42;
  });
  signals::Action bus_action{bus_handler, 0, 4}, old_bus;
  REQUIRE(signals::set_action(signals::kSigBus, &bus_action, &old_bus) == 0);
  auto bus_source = hle::make_thunk("test", "mapped backing error", [](hle::CallContext&) {
    throw hle::MemoryFault{0x1234, false, true};
  });
  CHECK(thread.call(bus_source, {}) == 42);
  CHECK(signals::set_action(signals::kSigBus, &old_bus, nullptr) == 0);
  CHECK(signals::set_action(signals::kSigSegv, &previous, nullptr) == 0);
  AddressSpace::get().free(p);
  Linker::get().close(libc);
}
TEST_CASE("core AArch64 signal UAPI layout") {
  CHECK(sizeof(signals::SigInfo) == 128);
  CHECK(offsetof(signals::UContext, mcontext) == 176);
  CHECK(sizeof(signals::SigContext) == 4384);
  CHECK(sizeof(signals::FpsimdContext) == 528);
  CHECK(offsetof(signals::RtSigFrame, context) + offsetof(signals::UContext, mcontext) == 304);
  CHECK(sizeof(signals::EsrContext) == 16);
  auto a64 = guest_stub(GuestIsa::a64, GuestStub::sigreturn);
  CHECK(a64.count == 2);
  CHECK(a64.words[0] == 0xd2801168);
  CHECK(a64.words[1] == 0xd4000001);
  CHECK(offsetof(signals::UContext32, mcontext) == 20);
  CHECK(offsetof(signals::UContext32, regspace) == 232);
  CHECK(sizeof(signals::SigFrame32) == 760);
  CHECK(sizeof(signals::RtSigFrame32) == 888);
  for (unsigned variant : {0u, 1u, 2u, 3u}) {
    auto arm = guest_stub(GuestIsa::a32, GuestStub::sigreturn, variant);
    CHECK(arm.count == 2);
    CHECK(arm.words[0] == ((variant & 1) ? 0xe3a070ad : 0xe3a07077));
    CHECK(arm.words[1] == 0xef000000);
  }
}
TEST_CASE("core synchronous signal restores modified PC and SIMD context on alternate stack") {
  GuestThread thread(70); thread.attach_to_current_host_thread();
  auto alt = AddressSpace::get().allocate(32768, kProtRead | kProtWrite, "[altstack test]");
  signals::AltStack stack{alt, 32768, 0};
  REQUIRE(signals::set_alt_stack(thread, &stack, nullptr) == 0);
  auto handler = hle::make_thunk("test", "segv", [](hle::CallContext& ctx) {
    CHECK(ctx.x(0) == signals::kSigSegv);
    auto* info = g2h<signals::SigInfo>(ctx.x(1));
    auto* context = g2h<signals::UContext>(ctx.x(2));
    CHECK(info->signo == signals::kSigSegv);
    CHECK(ctx.cpu().x(29) == ctx.x(1) + sizeof(signals::RtSigFrame));
    auto* record = g2h<signals::FrameRecord>(ctx.cpu().x(29));
    CHECK(record->fp == context->mcontext.regs[29]);
    CHECK(record->lr == context->mcontext.regs[30]);
    CHECK(context->mcontext.fault_address == 0);
    signals::AltStack active;
    CHECK(signals::set_alt_stack(ctx.thread(), nullptr, &active) == 0);
    CHECK((active.flags & 1) != 0);
    context->mcontext.pc += 4;
    context->mcontext.regs[0] = 42;
    auto* fp = reinterpret_cast<signals::FpsimdContext*>(context->mcontext.reserved.data());
    CHECK(fp->magic == 0x46508001);
    auto* esr = reinterpret_cast<const signals::EsrContext*>(context->mcontext.reserved.data() + sizeof(*fp));
    CHECK(esr->magic == 0x45535201);
    CHECK(esr->size == 16);
    auto* end = context->mcontext.reserved.data() + sizeof(*fp) + sizeof(*esr);
    CHECK(*reinterpret_cast<const std::uint64_t*>(end) == 0);
    fp->vregs[0] = {123, 456};
  });
  signals::Action action{handler, 0, 4 | 0x08000000}, previous;
  REQUIRE(signals::set_action(signals::kSigSegv, &action, &previous) == 0);
  constexpr std::array code{0xf9400000u, 0xd65f03c0u};
  auto fault = hle::code_stub(code, "fault");
  auto result = thread.call(fault, GuestCallArgs{});
  CHECK(result.x0 == 42);
  CHECK(result.v0.lo == 123);
  CHECK(result.v0.hi == 456);
  CHECK(thread.blocked_signals == 0);
  CHECK(signals::set_action(signals::kSigSegv, &previous, nullptr) == 0);
  AddressSpace::get().free(alt);
}
TEST_CASE("core pending signals masks, reset handler and restart flag") {
  GuestThread thread(71); thread.attach_to_current_host_thread();
  int calls = 0;
  auto handler = hle::make_thunk("test", "signal", [](hle::CallContext& ctx) {
    ++*static_cast<int*>(ctx.user());
  }, &calls);
  signals::Action action{handler, 0, 0x10000000 | 0x80000000}, previous;
  REQUIRE(signals::set_action(10, &action, &previous) == 0);
  std::uint64_t mask = 1ull << 9;
  CHECK(signals::set_mask(thread, 0, &mask, nullptr) == 0);
  CHECK(signals::send_to_thread(thread.tid(), {10, -6, 0, 1}) == 0);
  CHECK_FALSE(signals::deliver_pending(thread));
  CHECK(calls == 0);
  CHECK(signals::set_mask(thread, 1, &mask, nullptr) == 0);
  CHECK(signals::deliver_pending(thread));
  CHECK(calls == 1);
  CHECK(signals::restart_last_handler());
  signals::Action now;
  CHECK(signals::set_action(10, nullptr, &now) == 0);
  CHECK(now.handler == 0);
  CHECK(now.flags == (0x10000000 | 0x80000000));  // SA_RESETHAND resets only the handler
  CHECK(signals::set_action(10, &previous, nullptr) == 0);
}
TEST_CASE("A64 signal delivery and return restore modified cached registers") {
  GuestThread thread(79); thread.attach_to_current_host_thread();
  auto handler = hle::make_thunk("test", "cached signal context", [](hle::CallContext& ctx) {
    auto* context = g2h<signals::UContext>(ctx.x(2));
    CHECK(context->mcontext.regs[0] == 0);
    CHECK(context->mcontext.regs[1] == 11);
    CHECK(context->mcontext.regs[8] == 12);
    CHECK(context->mcontext.regs[30] != 0);
    context->mcontext.pc += 4;
    context->mcontext.regs[0] = 10;
    context->mcontext.regs[1] = 20;
    context->mcontext.regs[8] = 30;
    // Skip a BRK through the restored LR instead of the normal return address.
    context->mcontext.regs[30] += 4;
  });
  signals::Action action{handler, 0, 4}, previous;
  REQUIRE(signals::set_action(signals::kSigSegv, &action, &previous) == 0);
  // ADR LR targets BRK; the handler advances it to the LR restoration.
  constexpr std::array body{0xaa1e03f3u, 0xd2800161u, 0xd2800188u, 0x100000beu,
      0xf9400000u, 0x8b010000u, 0x8b080000u, 0xd65f03c0u, 0xd4200000u,
      0xaa1303feu, 0xd65f03c0u};
  auto entry = hle::code_stub(body, "cached signal body");
  CHECK(thread.call(entry, {0}) == 60);
  CHECK(signals::set_action(signals::kSigSegv, &previous, nullptr) == 0);
}
TEST_CASE("core signal nonlocal guest escape unwinds host handler frames") {
  GuestThread thread(72); thread.attach_to_current_host_thread();
  // This is the architectural effect of a guest siglongjmp: restore saved SP/LR,
  // jump into the interrupted function, and bypass the signal return trampoline.
  auto handler = hle::make_thunk("test", "nonlocal handler", [](hle::CallContext& ctx) {
    auto* context = g2h<signals::UContext>(ctx.x(2));
    ctx.cpu().set_pc(context->mcontext.pc + 4);
    ctx.cpu().set_sp(context->mcontext.sp);
    ctx.cpu().set_x(30, context->mcontext.regs[30]);
    ctx.cpu().set_x(0, 77);
    auto mask = context->sigmask;
    signals::set_mask(ctx.thread(), 2, &mask, nullptr);
  });
  signals::Action action{handler, 0, 4}, previous;
  REQUIRE(signals::set_action(signals::kSigSegv, &action, &previous) == 0);
  constexpr std::array code{0xf9400000u, 0xd65f03c0u};
  auto fault = hle::code_stub(code, "nonlocal fault");
  CHECK(thread.call(fault, {0}) == 77);
  CHECK(thread.blocked_signals == 0);
  CHECK(signals::set_action(signals::kSigSegv, &previous, nullptr) == 0);
}
TEST_CASE("core signal nonlocal escape crosses a nested HLE guest call") {
  GuestThread thread(73); thread.attach_to_current_host_thread();
  struct State { CpuState* outer = nullptr; GuestAddr inner = 0; bool unwound = false, resumed = false; } state;
  constexpr std::array code{0xf9400000u, 0xd65f03c0u};
  state.inner = hle::code_stub(code, "nested fault");
  auto handler = hle::make_thunk("test", "escape nested call", [](hle::CallContext& ctx) {
    auto& state = *static_cast<State*>(ctx.user());
    state.outer->restore(ctx.cpu());
    std::uint64_t mask = 0; signals::set_mask(ctx.thread(), 2, &mask, nullptr);
    ctx.return_x(88);
  }, &state);
  signals::Action action{handler, 0, 4}, previous;
  REQUIRE(signals::set_action(signals::kSigSegv, &action, &previous) == 0);
  auto outer = hle::make_thunk("test", "callback that escapes", [](hle::CallContext& ctx) {
    auto& state = *static_cast<State*>(ctx.user());
    CpuState saved(ctx.cpu()); state.outer = &saved;
    struct Unwind { bool& flag; ~Unwind() { flag = true; } } unwind{state.unwound};
    ctx.thread().call(state.inner, {0});
    state.resumed = true;
    ctx.return_x(999);
  }, &state);
  CHECK(thread.call(outer, {}) == 88);
  CHECK(state.unwound);
  CHECK_FALSE(state.resumed);
  CHECK(signals::set_action(signals::kSigSegv, &previous, nullptr) == 0);
}
namespace {
// debuggerd_init(): sigfillset, SA_RESTART | SA_SIGINFO | SA_ONSTACK | SA_EXPOSE_TAGBITS.
constexpr std::uint32_t debuggerd_flags = 0x10000000 | 4 | 0x08000000 | 0x800;
constexpr std::uint64_t all_but_kill_stop = ~((1ull << 8) | (1ull << 18));
constexpr std::array debuggerd_signals{4, 5, 6, 7, 8, 11, 16, 31, 35};
constexpr std::array null_load{0xf9400000u, 0xd65f03c0u};  // ldr x0, [x0]; ret
// An engine crash reporter (Unity, UE4, Crashlytics, Breakpad): it saved the
// old action when it installed itself, chains to it when that is a function,
// then returns so the faulting instruction re-executes.
struct Reporter { signals::Action chained{}; unsigned calls = 0; } reporter;
void crash_reporter(hle::CallContext& ctx) {
  ++reporter.calls;
  if (reporter.chained.handler > 1) ctx.thread().call(reporter.chained.handler, {ctx.x(0), ctx.x(1), ctx.x(2)});
}
int exit_status(GuestThread& thread, GuestAddr fn, std::uint64_t argument) {
  try { thread.call(fn, {argument}); } catch (const ProcessExit& exit) { return exit.status; }
  return -1;
}
struct LogCapture {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      std::format("venture-signal-log-{}", std::chrono::steady_clock::now().time_since_epoch().count());
  LogCapture() { log::set_file(path.string()); }
  ~LogCapture() { log::set_file(""); std::error_code ignored; std::filesystem::remove(path, ignored); }
  std::string text() {
    log::set_file("");
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
  }
};
// A guest address with nothing mapped (SEGV_MAPERR).
GuestAddr unmapped_address() {
  auto page = AddressSpace::get().allocate(kGuestPageSize, kProtRead | kProtWrite, "[unmapped]");
  REQUIRE(page);
  AddressSpace::get().free(page);
  return page + 0x18;
}
}
TEST_CASE("bionic debuggerd handlers are installed before guest code runs") {
  register_all_hle_libraries();
  const auto debuggerd = signals::debuggerd_handler();
  REQUIRE(debuggerd > 1);
  for (int signo = 1; signo <= 64; ++signo) {
    if (signo == 10) continue;  // SIGUSR1 belongs to other fixtures in this process
    CAPTURE(signo);
    signals::Action old;
    REQUIRE(signals::set_action(signo, nullptr, &old) == 0);
    if (std::find(debuggerd_signals.begin(), debuggerd_signals.end(), signo) != debuggerd_signals.end()) {
      CHECK(old.handler == debuggerd);
      CHECK(old.flags == debuggerd_flags);
      CHECK(old.mask == all_but_kill_stop);
    } else {
      CHECK(old.handler == 0);
    }
  }
  // The libc ABI an engine uses to save its old action (LP64 flags-first layout).
  GuestThread thread(84); thread.attach_to_current_host_thread();
  auto* libc = Linker::get().open("libc.so", kRtldNow, nullptr);
  REQUIRE(libc);
  struct { std::uint32_t flags, pad; std::uint64_t handler, mask, restorer; } old{};
  CHECK(thread.call(libc->find_symbol("sigaction"), {signals::kSigSegv, 0, h2g(&old)}) == 0);
  CHECK(old.handler == debuggerd);
  CHECK(old.flags == debuggerd_flags);
  CHECK(old.mask == all_but_kill_stop);
  // Other signals keep their own dispositions and handlers.
  int usr2 = 0;
  auto handler = hle::make_thunk("test", "SIGUSR2", [](hle::CallContext& ctx) { ++*static_cast<int*>(ctx.user()); }, &usr2);
  signals::Action action{handler, 0, 4}, previous;
  REQUIRE(signals::set_action(12, &action, &previous) == 0);
  CHECK(previous.handler == 0);
  CHECK(signals::send_to_thread(thread.tid(), {12, -6, 0, 1000}) == 0);
  CHECK(signals::deliver_pending(thread));
  CHECK(usr2 == 1);
  signals::Action now;
  CHECK(signals::set_action(12, &previous, &now) == 0);
  CHECK(now.handler == handler);
  CHECK(signals::set_action(signals::kSigSegv, nullptr, &now) == 0);
  CHECK(now.handler == debuggerd);
  Linker::get().close(libc);
}
TEST_CASE("fault capture fixture restores prior environment and action on unwind") {
  FaultCaptureScope outer;
  const auto first = hle::make_thunk("test", "outer fault action", [](hle::CallContext&) {});
  const auto second = hle::make_thunk("test", "inner fault action", [](hle::CallContext&) {});
  REQUIRE(outer.install(signals::kSigTrap, signals::Action{first, 0, 4}, "pc:0x10") == 0);
  bool caught = false;
  try {
    FaultCaptureScope inner;
    REQUIRE(inner.install(signals::kSigTrap, signals::Action{second, 0, 4}, "x1:0x10") == 0);
    throw 7;
  } catch (int value) {
    caught = value == 7;
  }
  CHECK(caught);
  CHECK(std::string(venture::environment::getenv("VENTURE_FAULT_MEMORY")) == "pc:0x10");
  signals::Action current;
  CHECK(signals::set_action(signals::kSigTrap, nullptr, &current) == 0);
  CHECK(current.handler == first);
  CHECK(current.flags == 4);
}

TEST_CASE("fault memory capture precedes a repairing handler's mutation") {
  GuestThread thread(193); thread.attach_to_current_host_thread();
  static std::array<std::uint64_t, 2> observed;
  static std::uint64_t repaired = 0x5150;
  observed = {0x1122334455667788ull, 0x8877665544332211ull};
  auto handler = hle::make_thunk("test", "mutate observed fault memory", [](hle::CallContext& ctx) {
    observed = {0xaaaaaaaaaaaaaaaaull, 0xbbbbbbbbbbbbbbbbull};
    g2h<signals::UContext>(ctx.x(2))->mcontext.regs[0] = h2g(&repaired);
  });
  signals::Action action{handler, 0, 4};
  FaultCaptureScope scope;
  REQUIRE(scope.install(signals::kSigSegv, action, "x1:0x10") == 0);
  LogCapture capture;
  CHECK(thread.call(hle::code_stub(null_load, "observed repairing fault"), {0, h2g(observed.data())}) == repaired);
  CHECK(scope.restore_action() == 0);
  const auto output = capture.text();
  CHECK(output.find("1122334455667788 8877665544332211") != std::string::npos);
  CHECK(output.find("aaaaaaaaaaaaaaaa bbbbbbbbbbbbbbbb") == std::string::npos);
  CHECK(observed[0] == 0xaaaaaaaaaaaaaaaaull);
  CHECK(observed[1] == 0xbbbbbbbbbbbbbbbbull);
}

TEST_CASE("fault memory captures retired stack before signal frame construction") {
  GuestThread thread(194); thread.attach_to_current_host_thread();
  static bool overwritten;
  overwritten = false;
  auto handler = hle::make_thunk("test", "resume synthetic breakpoint", [](hle::CallContext& ctx) {
    auto* uc = g2h<signals::UContext>(ctx.x(2));
    const auto* retired = g2h<std::uint64_t>(uc->mcontext.sp - 32);
    overwritten = retired[0] != 0x1122334455667788ull || retired[1] != 0x8877665544332211ull;
    uc->mcontext.pc += 4;
    uc->mcontext.regs[0] = 42;
  });
  signals::Action action{handler, 0, 4};
  FaultCaptureScope scope;
  REQUIRE(scope.install(signals::kSigTrap, action, "sp+-0x20:0x10") == 0);
  // sub sp,sp,#32; stp x1,x2,[sp]; add sp,sp,#32; brk #0; ret
  constexpr std::array retired_stack{0xd10083ffu, 0xa9000be1u, 0x910083ffu, 0xd4200000u, 0xd65f03c0u};
  LogCapture capture;
  CHECK(thread.call(hle::code_stub(retired_stack, "synthetic retired stack"),
                    {0, 0x1122334455667788ull, 0x8877665544332211ull}) == 42);
  CHECK(scope.restore_action() == 0);
  CHECK(overwritten);
  CHECK(capture.text().find("1122334455667788 8877665544332211") != std::string::npos);
}

TEST_CASE("fault indirect observer reads one bounded checked level before the handler") {
  GuestThread thread(195); thread.attach_to_current_host_thread();
  static std::array<std::uint64_t, 2049> payload;
  static std::uint64_t pointer, repaired = 42;
  payload.fill(0);
  payload[1] = 0x1122334455667788ull;
  payload.back() = 0xdeadbeefcafebabeull;
  pointer = h2g(payload.data());
  GuestAddr location = h2g(&pointer);
  std::string spec = "x1@+0x8:0x8";
  std::string expected = "1122334455667788 0000000000000000";
  bool capped = false;
  GuestAddr allocation = 0;
  struct Free { GuestAddr& address; ~Free() { if (address) AddressSpace::get().free(address); } } free{allocation};
  SUBCASE("one-level field snapshot precedes handler mutation") {}
  SUBCASE("null pointer is diagnostic only") { pointer = 0; expected = "null indirect pointer"; }
  SUBCASE("unreadable pointer location is diagnostic only") {
    allocation = AddressSpace::get().allocate(kGuestPageSize, kProtNone, "[fault observer test]");
    REQUIRE(allocation);
    location = allocation; expected = "indirect pointer unreadable";
  }
  SUBCASE("unreadable pointed memory is diagnostic only") {
    allocation = AddressSpace::get().allocate(kGuestPageSize, kProtNone, "[fault observer test]");
    REQUIRE(allocation);
    pointer = allocation; expected = "unreadable";
  }
  SUBCASE("pointer arithmetic overflow is rejected") {
    pointer = ~std::uint64_t{0} - 3; expected = "indirect range overflow";
  }
  SUBCASE("pointer arithmetic underflow is rejected") {
    pointer = 4; spec = "x1@-0x8:0x8"; expected = "indirect range overflow";
  }
  SUBCASE("negative field offset reads the preceding field") {
    pointer = h2g(payload.data() + 2); spec = "x1@-0x8:0x8";
  }
  SUBCASE("pointer location overflow is rejected") {
    location = ~std::uint64_t{0} - 3; spec = "x1+0x8@+0:0x8"; expected = "invalid indirect expression";
  }
  SUBCASE("recursive indirection is rejected") {
    spec = "x1@+0@+0:0x8"; expected = "invalid indirect expression";
  }
  SUBCASE("malformed field offset is rejected") {
    spec = "x1@junk:0x8"; expected = "invalid indirect expression";
  }
  SUBCASE("invalid register is rejected") {
    spec = "x99@+0:0x8"; expected = "bad indirect register";
  }
  SUBCASE("empty field offset is rejected") {
    spec = "x1@:0x8"; expected = "invalid indirect expression";
  }
  SUBCASE("empty length is rejected") {
    spec = "x1@+0:"; expected = "invalid indirect length";
  }
  SUBCASE("negative length is rejected") {
    spec = "x1@+0:-1"; expected = "invalid indirect length";
  }
  SUBCASE("length is capped at sixteen KiB") {
    spec = "x1@+0:0x10000"; expected = "0x4000 bytes:"; capped = true;
  }
  SUBCASE("a short read does not cross a protected page") {
    allocation = AddressSpace::get().allocate(2 * kGuestPageSize, kProtRead | kProtWrite, "[fault observer test]");
    REQUIRE(allocation);
    pointer = allocation + kGuestPageSize - 1;
    *g2h<std::uint8_t>(pointer) = 0x5a;
    REQUIRE(AddressSpace::get().protect(allocation + kGuestPageSize, kGuestPageSize, kProtNone) == 0);
    spec = "x1@+0:0x1"; expected = "000000000000005a 0000000000000000";
  }
  auto handler = hle::make_thunk("test", "repair indirect observer fault", [](hle::CallContext& ctx) {
    payload[1] = 0xaaaaaaaaaaaaaaaaull;
    g2h<signals::UContext>(ctx.x(2))->mcontext.regs[0] = h2g(&repaired);
  });
  FaultCaptureScope scope;
  REQUIRE(scope.install(signals::kSigSegv, signals::Action{handler, 0, 4}, spec.c_str()) == 0);
  LogCapture capture;
  CHECK(thread.call(hle::code_stub(null_load, "synthetic indirect fault"), {0, location}) == repaired);
  CHECK(scope.restore_action() == 0);
  const auto output = capture.text();
  CHECK(output.find(expected) != std::string::npos);
  CHECK(output.find("aaaaaaaaaaaaaaaa") == std::string::npos);
  if (capped) CHECK(output.find("deadbeefcafebabe") == std::string::npos);
}

TEST_CASE("a handler may retry an identical fault before repairing it") {
  GuestThread thread(85); thread.attach_to_current_host_thread();
  static std::uint64_t value = 0x5150;
  static unsigned calls = 0;
  calls = 0;
  auto handler = hle::make_thunk("test", "repair on third delivery", [](hle::CallContext& ctx) {
    if (++calls == 3) g2h<signals::UContext>(ctx.x(2))->mcontext.regs[0] = h2g(&value);
  });
  signals::Action action{handler, 0, 4}, previous;
  REQUIRE(signals::set_action(signals::kSigSegv, &action, &previous) == 0);
  CHECK(thread.call(hle::code_stub(null_load, "retried load"), {0}) == 0x5150);
  CHECK(calls == 3);
  CHECK(signals::set_action(signals::kSigSegv, &previous, nullptr) == 0);
}
TEST_CASE("an identical fault reached again after progress is not an unresolved loop") {
  // A write barrier: the store faults on a read-only page with the same
  // registers every iteration, the handler unprotects and returns, and the
  // loop re-protects the page through an HLE call before storing again.
  GuestThread thread(89); thread.attach_to_current_host_thread();
  static GuestAddr page = 0;
  static unsigned faults = 0, iterations = 0;
  faults = iterations = 0;
  page = AddressSpace::get().allocate(kGuestPageSize, kProtRead, "[write barrier]");
  REQUIRE(page);
  auto handler = hle::make_thunk("test", "write barrier fault", [](hle::CallContext& ctx) {
    ++faults;
    CHECK(g2h<signals::UContext>(ctx.x(2))->mcontext.fault_address == page);
    CHECK(AddressSpace::get().protect(page, kGuestPageSize, kProtRead | kProtWrite) == 0);
  });
  constexpr unsigned rounds = signals::kMaxUnresolvedFaults + 200;
  auto reprotect = hle::make_thunk("test", "write barrier reprotect", [](hle::CallContext& ctx) {
    CHECK(AddressSpace::get().protect(page, kGuestPageSize, kProtRead) == 0);
    ctx.return_x(++iterations < rounds ? page : 0);
  });
  // mov x9, x30; loop: str x1, [x0]; blr x5; cbnz x0, loop; ret x9
  constexpr std::array barrier{0xaa1e03e9u, 0xf9000001u, 0xd63f00a0u, 0xb5ffffc0u, 0xd65f0120u};
  signals::Action action{handler, 0, 4}, previous;
  REQUIRE(signals::set_action(signals::kSigSegv, &action, &previous) == 0);
  CHECK(thread.call(hle::code_stub(barrier, "write barrier loop"), {page, 0x5150, 0, 0, 0, reprotect}) == 0);
  CHECK(faults == rounds);
  CHECK(iterations == rounds);
  CHECK(*g2h<std::uint64_t>(page) == 0x5150);
  CHECK(signals::set_action(signals::kSigSegv, &previous, nullptr) == 0);
  AddressSpace::get().free(page);
}
// Each case below ends its process; CTest runs each alone with --no-skip.
TEST_CASE("debuggerd ends a fault whose crash reporter chains and returns" * doctest::skip()) {
  register_all_hle_libraries();
  GuestThread thread(86); thread.attach_to_current_host_thread();
  signals::Action action{hle::make_thunk("test", "crash reporter", crash_reporter), 0, 4 | 0x08000000};
  REQUIRE(signals::set_action(signals::kSigSegv, &action, &reporter.chained) == 0);
  REQUIRE(reporter.chained.handler == signals::debuggerd_handler());
  CHECK(exit_status(thread, hle::code_stub(null_load, "null load"), 0) == 128 + signals::kSigSegv);
  CHECK(reporter.calls == 1);
  signals::Action now;
  CHECK(signals::set_action(signals::kSigSegv, nullptr, &now) == 0);
  CHECK(now.handler == 0);  // debuggerd's signal(SIGSEGV, SIG_DFL)
  CHECK(now.flags == 0x10000000);
}
TEST_CASE("debuggerd re-raises SIGABRT for a crash reporter that chains and returns" * doctest::skip()) {
  register_all_hle_libraries();
  GuestThread thread(87); thread.attach_to_current_host_thread();
  auto* libc = Linker::get().open("libc.so", kRtldNow, nullptr);
  REQUIRE(libc);
  signals::Action action{hle::make_thunk("test", "abort reporter", crash_reporter), 0, 4};
  REQUIRE(signals::set_action(signals::kSigAbrt, &action, &reporter.chained) == 0);
  REQUIRE(reporter.chained.handler == signals::debuggerd_handler());
  CHECK(exit_status(thread, libc->find_symbol("raise"), signals::kSigAbrt) == 128 + signals::kSigAbrt);
  CHECK(reporter.calls == 1);
}
TEST_CASE("a returning handler whose chain ends in SIG_DFL cannot spin on one fault" * doctest::skip()) {
  register_all_hle_libraries();
  GuestThread thread(88); thread.attach_to_current_host_thread();
  const signals::Action default_action{};  // something reset SIGSEGV before the reporter installed itself
  REQUIRE(signals::set_action(signals::kSigSegv, &default_action, nullptr) == 0);
  signals::Action action{hle::make_thunk("test", "orphan reporter", crash_reporter), 0, 4};
  REQUIRE(signals::set_action(signals::kSigSegv, &action, &reporter.chained) == 0);
  REQUIRE(reporter.chained.handler == 0);
  CHECK(exit_status(thread, hle::code_stub(null_load, "unrepaired load"), 0) == 128 + signals::kSigSegv);
  CHECK(reporter.calls == signals::kMaxUnresolvedFaults - 1);
}
TEST_CASE("debuggerd reports the fault for a one-shot SA_SIGINFO crash reporter" * doctest::skip()) {
  register_all_hle_libraries();
  GuestThread thread(90); thread.attach_to_current_host_thread();
  signals::Action action{hle::make_thunk("test", "one-shot reporter", crash_reporter), 0, 0x80000000 | 4};
  REQUIRE(signals::set_action(signals::kSigSegv, &action, &reporter.chained) == 0);
  REQUIRE(reporter.chained.handler == signals::debuggerd_handler());
  const auto fault = unmapped_address();
  const auto load = hle::code_stub(null_load, "one-shot fault");
  LogCapture capture;
  CHECK(exit_status(thread, load, fault) == 128 + signals::kSigSegv);
  CHECK(reporter.calls == 1);
  // debuggerd's have_siginfo() still sees SA_SIGINFO after the one-shot reset.
  const auto output = capture.text();
  CHECK(output.find(std::format("Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr {:#x}", fault)) !=
        std::string::npos);
  CHECK(output.find("SI_USER") == std::string::npos);
  CHECK(output.find(std::format("[signal] guest tid 90 signal 11 code 1 at PC {:#x}, fault {:#x}, terminated by "
                                "the default action", load, fault)) != std::string::npos);
  // The tombstone register block, read from the reporter's ucontext: the faulting load's address operand and PC.
  CHECK(output.find(std::format("x0  {:016x}", fault)) != std::string::npos);
  CHECK(output.find(std::format("pc  {:016x}", load)) != std::string::npos);
}
TEST_CASE("a fault delivered straight to debuggerd keeps the sentinel's summary line" * doctest::skip()) {
  register_all_hle_libraries();
  GuestThread thread(91); thread.attach_to_current_host_thread();
  const auto fault = unmapped_address();
  const auto load = hle::code_stub(null_load, "unhandled fault");
  LogCapture capture;
#ifdef _WIN32
  _putenv_s("VENTURE_FAULT_MEMORY", "pc:0x10,x0+0x10:0x10");
#else
  setenv("VENTURE_FAULT_MEMORY", "pc:0x10,x0+0x10:0x10", 1);
#endif
  environment::refresh();
  CHECK(exit_status(thread, load, fault) == 128 + signals::kSigSegv);
#ifdef _WIN32
  _putenv_s("VENTURE_FAULT_MEMORY", "");
#else
  unsetenv("VENTURE_FAULT_MEMORY");
#endif
  environment::refresh();
  const auto output = capture.text();
  // The diagnostic memory dump: readable code at the PC, and the unmapped fault operand.
  CHECK(output.find(std::format("memory pc {:#x}+0x0, 0x10 bytes:", load)) != std::string::npos);
  CHECK(output.find(std::format("    {:016x} ", load)) != std::string::npos);
  CHECK(output.find(std::format("    {:016x} unreadable", fault + 0x10)) != std::string::npos);
  CHECK(output.find(std::format("Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr {:#x}", fault)) !=
        std::string::npos);
  CHECK(output.find(std::format("[signal] guest tid 91 signal 11 code 1 at PC {:#x}, fault {:#x}, terminated by "
                                "the default action", load, fault)) != std::string::npos);
  CHECK(output.find(std::format("x0  {:016x}", fault)) != std::string::npos);
  CHECK(output.find(std::format("pc  {:016x}", load)) != std::string::npos);
}

TEST_CASE("A64 asynchronous signal interrupts a linked forward chain and returns") {
  constexpr std::size_t links = 256;
  const auto code = AddressSpace::get().allocate(4096, kProtRead | kProtWrite | kProtExec, "[signal linked chain]");
  REQUIRE(code);
  auto* words = g2h<std::uint32_t>(code);
  std::fill_n(words, links, 0xb400003fu); // cbz xzr,+4
  words[links] = 0x14000000u | ((-static_cast<std::int32_t>(links)) & 0x03ffffff);
  auto handler = hle::make_thunk("test", "linked chain interrupt", [](hle::CallContext& ctx) {
    auto* context = g2h<signals::UContext>(ctx.x(2));
    context->mcontext.pc = context->mcontext.regs[30];
    context->mcontext.regs[0] = 42;
  });
  signals::Action action{handler, 0, 4}, previous;
  REQUIRE(signals::set_action(10, &action, &previous) == 0);
  std::atomic<bool> ready{false}; std::promise<std::uint64_t> result;
  auto completed = result.get_future();
  std::thread worker([&] {
    try {
      GuestThread thread(83); thread.attach_to_current_host_thread();
      ready.store(true); result.set_value(thread.call(code, {}));
    } catch (...) { result.set_exception(std::current_exception()); }
  });
  while (!ready.load() && completed.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const auto start = std::chrono::steady_clock::now();
  const int sent = signals::send_to_thread(83, {10, -6, 0, 1});
  if (completed.wait_for(std::chrono::seconds(2)) != std::future_status::ready) std::terminate();
  worker.join();
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
  CHECK(sent == 0); CHECK(completed.get() == 42);
  std::printf("signal linked-chain delivery-and-return latency_us=%.3f\n", us);
  CHECK(signals::set_action(10, &previous, nullptr) == 0);
  AddressSpace::get().free(code);
}
