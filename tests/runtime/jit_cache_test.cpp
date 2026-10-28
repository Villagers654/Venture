#define XBYAK_STRICT_CHECK_MEM_REG_SIZE 0
#include <doctest/doctest.h>
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/profiling.h>
#include <dynarmic/interface/optimization_flags.h>
#include <dynarmic/common/fp/op/FPRecipStepFused.h>
#include <dynarmic/common/fp/op/FPRecipEstimate.h>
#include <dynarmic/common/fp/fpsr.h>
#include <dynarmic/common/fp/fpcr.h>
#include "platform/memory.h"
#include <dynarmic/backend/x64/a64_jitstate.h>
#include <dynarmic/backend/x64/a32_jitstate.h>
#include <dynarmic/backend/x64/block_of_code.h>
#include <algorithm>
#include <map>
#include <utility>
#include <optional>
#include <array>
#include <vector>
#include <new>
#include <xmmintrin.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <dynarmic/frontend/A64/a64_location_descriptor.h>
#include <dynarmic/frontend/A32/a32_location_descriptor.h>

namespace {
constexpr unsigned MiB = 1024 * 1024;
struct JitCacheTrace {
  inline static JitCacheTrace* current = nullptr;
  unsigned clears = 0;
  static void emitted(const void*, const void*, std::string_view name) {
    if (current && name == "cache_clear") ++current->clears;
  }
  JitCacheTrace() { current = this; Dynarmic::SetCodeRegistrationCallback(emitted); }
  ~JitCacheTrace() { Dynarmic::SetCodeRegistrationCallback(nullptr); current = nullptr; }
};
struct A64CacheCallbacks : Dynarmic::A64::UserCallbacks {
  using Addr = std::uint64_t;
  Dynarmic::A64::Jit* jit = nullptr;
  bool changed = false;
  unsigned services = 0;
  std::optional<std::uint32_t> MemoryReadCode(Addr p) override {
    if (p >= 0x100000) return (p % 256 < 128) ? 0x8b010000u :
        0x14000000u | (static_cast<std::uint32_t>((0x1000 - p) / 4) & 0x03ffffff);
    switch (p) {
    case 0x1000: return 0xd63f00c0; // blr x6
    case 0x1004: return 0x8b080000; // add x0,x0,x8
    case 0x1008: return 0xd4000021; // svc #1
    case 0x1100: return changed ? 0x91000800 : 0x91000400; // add x0,x0,#1/2
    case 0x1104: return 0xd65f03c0; // ret
    }
    return std::nullopt;
  }
  std::uint64_t MemoryRead(Addr, std::size_t) override { return 0; }
  void MemoryWrite(Addr, std::uint64_t, std::size_t) override {}
  std::uint8_t MemoryRead8(Addr) { return 0; }
  void MemoryWrite8(Addr, std::uint8_t) {}
  std::uint16_t MemoryRead16(Addr) { return 0; }
  void MemoryWrite16(Addr, std::uint16_t) {}
  std::uint32_t MemoryRead32(Addr) { return 0; }
  void MemoryWrite32(Addr, std::uint32_t) {}
  std::uint64_t MemoryRead64(Addr) { return 0; }
  void MemoryWrite64(Addr, std::uint64_t) {}
  Dynarmic::A64::Vector MemoryRead128(Addr) override { return {}; }
  void MemoryWrite128(Addr, Dynarmic::A64::Vector) override {}
  std::uint64_t MemoryAtomic(Addr, std::size_t, Dynarmic::A64::AtomicOp,
      std::uint64_t, std::uint64_t) override { return 0; }
  Dynarmic::A64::Vector MemoryAtomicCompareAndSwap128(Addr,
      Dynarmic::A64::Vector, Dynarmic::A64::Vector) override { return {}; }
  std::uint64_t GetCNTPCT() override { return 0; }
  void InterpreterFallback(Addr, std::size_t) { FAIL("unexpected fallback"); }
  void ExceptionRaised(Addr, Dynarmic::A64::Exception) override { FAIL("unexpected exception"); }
  void CallSVC(std::uint32_t) override {
    ++services;
    CHECK_FALSE(jit->Precompile(0x1100));
    jit->HaltExecution();
  }
  void AddTicks(std::uint64_t) override {}
  std::uint64_t GetTicksRemaining() override { return 1000000; }
};
struct A32CacheCallbacks : Dynarmic::A32::UserCallbacks {
  using Addr = std::uint32_t;
  Dynarmic::A32::Jit* jit = nullptr;
  bool changed = false;
  unsigned services = 0;
  std::optional<std::uint32_t> MemoryReadCode(Addr p) override {
    if (p >= 0x100000) return (p % 256 < 128) ? 0xe0800001u :
        0xea000000u | (static_cast<std::uint32_t>((0x1000 - p - 8) / 4) & 0x00ffffff);
    switch (p) {
    case 0x1000: return 0xe12fff36; // blx r6
    case 0x1004: return 0xe0800008; // add r0,r0,r8
    case 0x1008: return 0xef000001; // svc #1
    case 0x1100: return changed ? 0xe2800002 : 0xe2800001;
    case 0x1104: return 0xe12fff1e; // bx lr
    }
    return std::nullopt;
  }
  std::uint64_t MemoryRead(Addr p, std::size_t size) override { return size == 4 ? MemoryReadCode(p).value_or(0) : 0; }
  void MemoryWrite(Addr, std::uint64_t, std::size_t) override {}
  std::uint8_t MemoryRead8(Addr) { return 0; }
  void MemoryWrite8(Addr, std::uint8_t) {}
  std::uint16_t MemoryRead16(Addr) { return 0; }
  void MemoryWrite16(Addr, std::uint16_t) {}
  std::uint32_t MemoryRead32(Addr p) { return MemoryReadCode(p).value_or(0); }
  void MemoryWrite32(Addr, std::uint32_t) {}
  std::uint64_t MemoryRead64(Addr) { return 0; }
  void MemoryWrite64(Addr, std::uint64_t) {}
  void InterpreterFallback(Addr, std::size_t) { FAIL("unexpected fallback"); }
  void ExceptionRaised(Addr, Dynarmic::A32::Exception) override { FAIL("unexpected exception"); }
  void CallSVC(std::uint32_t) override {
    ++services;
    CHECK_FALSE(jit->Precompile(0x1100));
    jit->HaltExecution();
  }
  void AddTicks(std::uint64_t) override {}
  std::uint64_t GetTicksRemaining() override { return 1000000; }
};
}

TEST_CASE("A64 pretranslation preserves state and executes fresh code after invalidation") {
  A64CacheCallbacks cb;
  Dynarmic::A64::UserConfig config{};
  config.callbacks = &cb;
  config.enable_register_cache = true;
  config.fastmem_pointer = 0; config.fastmem_address_space_bits = 64;
  config.silently_mirror_fastmem = false;
  config.enable_cycle_counting = false;
  Dynarmic::A64::Jit jit(config); cb.jit = &jit;
  for (unsigned i = 0; i < 31; ++i) jit.SetRegister(i, 0x123400 + i);
  jit.SetSP(0x567800); jit.SetPC(0x876500); jit.SetPstate(0xa0000000);
  jit.SetFpcr(0x400000); jit.SetFpsr(0x15);
  const unsigned host = _mm_getcsr();
  for (const auto pc : {0x1000u, 0x1004u, 0x1100u}) {
    const auto d = Dynarmic::A64::LocationDescriptor{pc, Dynarmic::FP::FPCR{0x400000}};
    REQUIRE(jit.Precompile(d.UniqueHash()));
    REQUIRE(jit.Precompile(d.UniqueHash()));
  }
  CHECK(_mm_getcsr() == host);
  CHECK(cb.services == 0);
  for (unsigned i = 0; i < 31; ++i) CHECK(jit.GetRegister(i) == 0x123400 + i);
  CHECK(jit.GetPC() == 0x876500); CHECK(jit.GetSP() == 0x567800);
  CHECK(jit.GetPstate() == 0xa0000000);
  CHECK(jit.GetFpcr() == 0x400000); CHECK(jit.GetFpsr() == 0x15);
  for (unsigned expected : {3u, 4u}) {
    jit.SetPC(0x1000); jit.SetRegister(0, 0); jit.SetRegister(6, 0x1100); jit.SetRegister(8, 2);
    jit.Run(); CHECK(jit.GetRegister(0) == expected); CHECK(jit.GetPC() == 0x100c);
    cb.changed = true; jit.InvalidateCacheRange(0x1100, 4);
  }
}

TEST_CASE("A32 pretranslation preserves state and respects invalidation") {
  A32CacheCallbacks cb;
  Dynarmic::A32::UserConfig config{}; config.callbacks = &cb;
  config.enable_cycle_counting = false;
  Dynarmic::A32::Jit jit(config); cb.jit = &jit;
  for (unsigned i = 0; i < 16; ++i) jit.Regs()[i] = 0x123400 + i;
  jit.SetCpsr(0xa0000010); jit.SetFpscr(0x400015);
  const unsigned host = _mm_getcsr();
  for (const auto pc : {0x1000u, 0x1004u, 0x1100u}) {
    const auto d = Dynarmic::A32::LocationDescriptor{pc, Dynarmic::A32::PSR{0xa0000010}, Dynarmic::A32::FPSCR{0x400015}};
    REQUIRE(jit.Precompile(d.UniqueHash()));
  }
  CHECK(_mm_getcsr() == host); CHECK(cb.services == 0);
  for (unsigned i = 0; i < 16; ++i) CHECK(jit.Regs()[i] == 0x123400 + i);
  CHECK(jit.Cpsr() == 0xa0000010); CHECK(jit.Fpscr() == 0x400015);
  for (unsigned expected : {3u, 4u}) {
    jit.Regs()[15] = 0x1000; jit.Regs()[0] = 0; jit.Regs()[6] = 0x1100; jit.Regs()[8] = 2;
    jit.Run(); CHECK(jit.Regs()[0] == expected); CHECK(jit.Regs()[15] == 0x100c);
    cb.changed = true; jit.InvalidateCacheRange(0x1100, 4);
  }
}

TEST_CASE("A64 pretranslated fastmem faults retain cached pre-instruction state") {
  struct Callbacks final : A64CacheCallbacks {
    unsigned reads = 0;
    std::optional<std::uint32_t> MemoryReadCode(Addr p) override {
      if (p == 0x1200) return 0xf9400020; // ldr x0,[x1]
      if (p == 0x1204) return 0x91000508; // add x8,x8,#1
      if (p == 0x1208) return 0xd4000021;
      return A64CacheCallbacks::MemoryReadCode(p);
    }
    std::uint64_t MemoryRead(Addr, std::size_t) override {
      ++reads; jit->HaltExecution(Dynarmic::HaltReason::MemoryAbort); return 0;
    }
  } cb;
  Dynarmic::A64::UserConfig config{}; config.callbacks = &cb;
  config.fastmem_pointer = 0; config.fastmem_address_space_bits = 64;
  config.silently_mirror_fastmem = false; config.check_halt_on_memory_access = true;
  config.enable_register_cache = true; config.enable_cycle_counting = false;
  Dynarmic::A64::Jit jit(config); cb.jit = &jit;
  REQUIRE(jit.Precompile(0x1200)); CHECK(cb.reads == 0); CHECK(cb.services == 0);
  for (auto source : {0ull, 0x00ff000000000000ull}) {
    jit.ClearHalt(Dynarmic::HaltReason::MemoryAbort);
    jit.SetPC(0x1200); jit.SetRegister(0, 123); jit.SetRegister(1, source);
    jit.SetRegister(8, 456); jit.SetRegister(30, 789);
    CHECK(Dynarmic::Has(jit.Run(), Dynarmic::HaltReason::MemoryAbort));
    CHECK(jit.GetPC() == 0x1200); CHECK(jit.GetRegister(0) == 123);
    CHECK(jit.GetRegister(1) == source); CHECK(jit.GetRegister(8) == 456);
    CHECK(jit.GetRegister(30) == 789);
  }
  CHECK(cb.services == 0);
  CHECK(cb.reads == 2);
}

TEST_CASE("Pretranslation stops before pressure clears the A64 code cache") {
  JitCacheTrace trace; A64CacheCallbacks cb;
  Dynarmic::A64::UserConfig config{}; config.callbacks = &cb;
  config.code_cache_size = 16 * MiB; config.enable_cycle_counting = false;
  Dynarmic::A64::Jit jit(config); cb.jit = &jit;
  unsigned blocks = 0;
  while (jit.Precompile(0x100000 + blocks * 256ull)) REQUIRE(++blocks < 100000);
  CHECK(blocks > 0); CHECK(trace.clears == 0); CHECK(cb.services == 0);
  CHECK(jit.GetPC() == 0);
  REQUIRE(jit.Precompile(0x100000)); // Existing hints remain usable at the limit.
}

TEST_CASE("Failed pretranslation invalidates before the next A64 execution") {
  struct Callbacks final : A64CacheCallbacks {
    std::optional<std::uint32_t> MemoryReadCode(Addr p) override {
      if (p == 0x1200) throw std::bad_alloc{};
      return A64CacheCallbacks::MemoryReadCode(p);
    }
  } cb;
  JitCacheTrace trace;
  Dynarmic::A64::UserConfig config{}; config.callbacks = &cb;
  config.enable_cycle_counting = false;
  Dynarmic::A64::Jit jit(config); cb.jit = &jit;
  REQUIRE(jit.Precompile(0x1000)); REQUIRE(jit.Precompile(0x1100));
  const auto host = _mm_getcsr();
  CHECK_THROWS_AS(jit.Precompile(0x1200), std::bad_alloc);
  CHECK(_mm_getcsr() == host);
  jit.SetPC(0x1000); jit.SetRegister(0, 0); jit.SetRegister(6, 0x1100); jit.SetRegister(8, 2);
  jit.Run(); CHECK(jit.GetRegister(0) == 3); CHECK(trace.clears == 1);
}

TEST_CASE("A32 registered SVC callbacks preserve state under Run and Step") {
  struct Callbacks : A32CacheCallbacks {
    unsigned known_calls = 0, generic_calls = 0;
    std::optional<std::uint32_t> MemoryReadCode(Addr p) override {
      switch (p) {
        case 0x1200: return 0xef005154u;
        case 0x1204: return 0xe0800001u; // add r0,r0,r1
        case 0x1208: return 0xef000001u;
        default: return std::nullopt;
      }
    }
    void invoke(std::uint32_t immediate) {
      CHECK(immediate == 0x5154);
      CHECK(jit->Regs()[0] == 11); CHECK(jit->Regs()[1] == 12);
      CHECK(jit->Regs()[14] == 0x3000);
      jit->Regs()[0] = 20; jit->Regs()[1] = 30; jit->Regs()[14] = 0x4000;
    }
    void CallSVCFast(std::uint32_t immediate) override { ++generic_calls; invoke(immediate); }
    void CallSVCFastKnown(std::uint32_t immediate, const void* metadata, void* state) override {
      CHECK(state != nullptr);
      CHECK(static_cast<Dynarmic::Backend::X64::A32JitState*>(state)->Reg[0] == jit->Regs()[0]);
      CHECK(metadata == this); ++known_calls; invoke(immediate);
    }
    void CallSVC(std::uint32_t immediate) override {
      CHECK(immediate == 1);
      CHECK(jit->Regs()[0] == 50); CHECK(jit->Regs()[1] == 30);
      CHECK(jit->Regs()[14] == 0x4000);
      jit->HaltExecution();
    }
  };
  for (bool known : {false, true}) for (bool cycles : {false, true}) {
    Callbacks cb;
    unsigned lookups = 0;
    Dynarmic::A32::UserConfig config{};
    config.callbacks = &cb; config.enable_cycle_counting = cycles;
    config.svc_fast = 0x5154; config.svc_fp_transfer = 0x5155;
    config.svc_lookup = [&](std::uint32_t pc, std::uint32_t svc) -> const void* {
      ++lookups; CHECK(pc == 0x1200); CHECK(svc == 0x5154);
      return known ? &cb : nullptr;
    };
    Dynarmic::A32::Jit jit(config); cb.jit = &jit;
    for (bool step : {false, true}) for (unsigned repeat = 0; repeat < 2; ++repeat) {
      jit.SetCpsr(0x10); jit.Regs()[15] = 0x1200;
      jit.Regs()[0] = 11; jit.Regs()[1] = 12; jit.Regs()[14] = 0x3000;
      if (!step) jit.Run();
      else for (unsigned n = 0; n < 4 && jit.Regs()[15] != 0x120c; ++n) jit.Step();
      REQUIRE(jit.Regs()[15] == 0x120c);
      CHECK(jit.Regs()[0] == 50); CHECK(jit.Regs()[1] == 30);
      CHECK(jit.Regs()[14] == 0x4000);
    }
    CHECK(lookups > 0);
    CHECK(cb.known_calls == (known ? 4 : 0));
    CHECK(cb.generic_calls == (known ? 0 : 4));
  }
}

TEST_CASE("Bounded larger A64 cache retains linked blocks, RSB and cached registers") {
  unsigned cold_blocks = 0;
  for (const unsigned size : {16u, 32u}) {
    JitCacheTrace trace;
    A64CacheCallbacks cb;
    Dynarmic::A64::UserConfig config{};
    config.callbacks = &cb;
    config.code_cache_size = size * MiB;
    config.enable_cycle_counting = false;
    config.fastmem_pointer = 0;
    config.fastmem_address_space_bits = 64;
    config.silently_mirror_fastmem = false;
    config.enable_register_cache = true;
    config.check_halt_on_memory_access = true;
    Dynarmic::A64::Jit jit(config); cb.jit = &jit;
    const auto run = [&](std::uint64_t pc, unsigned expected) {
      jit.SetPC(pc); jit.SetRegister(0, 0); jit.SetRegister(1, 1);
      jit.SetRegister(6, 0x1100); jit.SetRegister(8, 2);
      jit.Run();
      REQUIRE(jit.GetRegister(0) == expected);
      REQUIRE(jit.GetRegister(1) == 1);
      REQUIRE(jit.GetRegister(8) == 2);
      REQUIRE(jit.GetRegister(30) == 0x1004);
      REQUIRE(jit.GetPC() == 0x100c);
    };
    for (unsigned i = 0; size == 16 ? trace.clears == 0 : i < cold_blocks; ++i) {
      REQUIRE(i < 100000);
      run(0x100000 + i * 256, 35);
      cold_blocks = size == 16 ? i + 1 : cold_blocks;
    }
    CHECK(trace.clears == (size == 16 ? 1 : 0));
    for (unsigned i = 0; i < 4; ++i) run(0x100000, 35);
    cb.changed = true; jit.InvalidateCacheRange(0x1100, 4);
    run(0x100000, 36);
    jit.ClearCache(); run(0x100000, 36);
    if (const auto bytes = venture::platform::committed_bytes(venture::h2g(jit.CodeCacheBegin()), size * MiB))
      CHECK(*bytes <= size * MiB);
  }
}

TEST_CASE("Bounded larger A32 cache retains linked blocks, RSB and fast dispatch") {
  unsigned cold_blocks = 0;
  for (const unsigned size : {16u, 32u}) {
    JitCacheTrace trace;
    A32CacheCallbacks cb;
    Dynarmic::A32::UserConfig config{};
    config.callbacks = &cb;
    config.code_cache_size = size * MiB;
    config.enable_cycle_counting = false;
    config.check_halt_on_memory_access = true;
    Dynarmic::A32::Jit jit(config); cb.jit = &jit; jit.SetCpsr(0x10);
    const auto run = [&](std::uint32_t pc, unsigned expected) {
      jit.Regs()[15] = pc; jit.Regs()[0] = 0; jit.Regs()[1] = 1;
      jit.Regs()[6] = 0x1100; jit.Regs()[8] = 2;
      jit.Run();
      REQUIRE(jit.Regs()[0] == expected);
      REQUIRE(jit.Regs()[1] == 1);
      REQUIRE(jit.Regs()[8] == 2);
      REQUIRE(jit.Regs()[14] == 0x1004);
      REQUIRE(jit.Regs()[15] == 0x100c);
    };
    for (unsigned i = 0; size == 16 ? trace.clears == 0 : i < cold_blocks; ++i) {
      REQUIRE(i < 100000);
      run(0x100000 + i * 256, 35);
      cold_blocks = size == 16 ? i + 1 : cold_blocks;
    }
    CHECK(trace.clears == (size == 16 ? 1 : 0));
    for (unsigned i = 0; i < 4; ++i) run(0x100000, 35);
    cb.changed = true; jit.InvalidateCacheRange(0x1100, 4);
    run(0x100000, 36);
    jit.ClearCache(); run(0x100000, 36);
    if (const auto bytes = venture::platform::committed_bytes(venture::h2g(jit.CodeCacheBegin()), size * MiB))
      CHECK(*bytes <= size * MiB);
  }
}

TEST_CASE("A64 fast dispatch keeps location zero empty across table sizes, clears and invalidation") {
  struct Callbacks final : A64CacheCallbacks {
    bool doubled = false;
    unsigned no_execute = 0;
    std::optional<std::uint32_t> MemoryReadCode(Addr p) override {
      switch (p) {
      case 0x3000: return 0xd63f00c0; // blr x6
      case 0x3004: return 0x910100c6; // add x6,x6,#0x40
      case 0x3008: return 0xf10004e7; // subs x7,x7,#1
      case 0x300c: return 0x54ffffa1; // b.ne 0x3000
      case 0x3010: return 0xd4000021; // svc #1
      case 0x3100: return 0xd63f00c0; // blr x6
      case 0x3104: return 0xf10004e7; // subs x7,x7,#1
      case 0x3108: return 0x54ffffc1; // b.ne 0x3100
      case 0x310c: return 0xd4000021; // svc #1
      }
      // 2048 leaf functions, one per 64 bytes: add x0,x0,#1 (#2 once changed); ret
      if (p >= 0x200000 && p < 0x200000 + 2048 * 0x40 && p % 0x40 < 8)
        return p % 0x40 ? 0xd65f03c0 : doubled ? 0x91000800 : 0x91000400;
      return std::nullopt;
    }
    void CallSVC(std::uint32_t) override { jit->HaltExecution(); }
    void ExceptionRaised(Addr pc, Dynarmic::A64::Exception exception) override {
      // PC 0, or ~0 (locations sign-extend their 56 PC bits).
      CHECK((pc == 0 || pc == ~Addr(0))); CHECK(exception == Dynarmic::A64::Exception::NoExecuteFault);
      ++no_execute; jit->HaltExecution();
    }
  };
  // sites: 0 shared handler, 1 inline lookups, 2 per-site slots.
  for (const bool pages : {false, true}) for (const unsigned bits : {10u, 20u}) for (const bool census : {false, true}) for (const int sites : {0, 1, 2}) {
    CAPTURE(pages);
    CAPTURE(bits);
    CAPTURE(census);
    CAPTURE(sites);
    Callbacks cb;
    std::array<std::uint64_t, 6> counts{};
    Dynarmic::A64::UserConfig config{}; config.callbacks = &cb;
    config.enable_cycle_counting = false; config.enable_register_cache = true;
    config.fastmem_pointer = 0; config.fastmem_address_space_bits = 64;
    config.silently_mirror_fastmem = false;
    config.fast_dispatch_table_bits = static_cast<std::uint8_t>(bits);
    config.fast_dispatch_table_pages = pages; // OS zero pages, or upstream's filled array
    if (census) config.fast_dispatch_census = counts.data();
    config.inline_fast_dispatch = sites == 1;
    config.dispatch_site_cache = sites == 2;
    Dynarmic::A64::Jit jit(config); cb.jit = &jit;
    // 2^10 entries cannot hold 2048 targets: evictions and refills on every pass.
    const auto calls = [&](std::uint64_t expected) {
      jit.SetPC(0x3000); jit.SetRegister(0, 0); jit.SetRegister(6, 0x200000); jit.SetRegister(7, 2048);
      jit.Run(); CHECK(jit.GetRegister(0) == expected); CHECK(jit.GetPC() == 0x3014);
    };
    // Location 0 (PC 0, FPCR 0) hashes to an entry that would otherwise hold
    // the zero empty tag with a null code pointer: it must miss, not jump to 0.
    const auto zero = [&](unsigned expected) {
      jit.SetPC(0x3000); jit.SetRegister(6, 0); jit.SetRegister(7, 1);
      jit.Run(); CHECK(cb.no_execute == expected);
    };
    // One site, one target: a slot keeps it until invalidation or a clear.
    const auto same = [&](std::uint64_t expected) {
      jit.SetPC(0x3100); jit.SetRegister(0, 0); jit.SetRegister(6, 0x200000); jit.SetRegister(7, 64);
      jit.Run(); CHECK(jit.GetRegister(0) == expected); CHECK(jit.GetPC() == 0x3110);
    };
    // A branch to ~0 matches an empty slot's tag and must still fault.
    const auto ones = [&](unsigned expected) {
      jit.SetPC(0x3100); jit.SetRegister(6, ~std::uint64_t(0)); jit.SetRegister(7, 1);
      jit.Run(); CHECK(cb.no_execute == expected);
    };
    zero(1); calls(2048); calls(2048); zero(2);
    jit.ClearCache(); zero(3); calls(2048);
    cb.doubled = true; jit.InvalidateCacheRange(0x200000, 2048 * 0x40); calls(4096); zero(4);
    same(128); same(128); ones(5); same(128);
    cb.doubled = false; jit.InvalidateCacheRange(0x200000, 0x40); same(64); ones(6);
    jit.ClearCache(); ones(7); same(64); same(64);
    if (census) {
      CHECK(counts[0] >= 4 * 2048);           // RSB pops (leaf returns)
      CHECK(counts[2] >= 4 * 2048);           // BLR fast-dispatch hints
      CHECK(counts[3] >= 3 * 2048);           // cold, post-clear and post-invalidation misses
      CHECK(counts[3] <= counts[1] + counts[2]);
      CHECK(counts[4] >= 2048);               // simulated tables are never cleared
      CHECK(counts[5] >= counts[4]);          // nested direct-mapped indexes: inclusion
      CHECK(counts[5] <= counts[1] + counts[2]);
    }
  }
}

TEST_CASE("A64 inline fast dispatch and RSB pops look up targets with the FPCR they run under") {
  struct Callbacks final : A64CacheCallbacks {
    std::optional<std::uint32_t> MemoryReadCode(Addr p) override {
      switch (p) {
      case 0x4000: return 0xd51b4401; // msr fpcr,x1
      case 0x4004: return 0xd61f00c0; // br x6
      case 0x4200: return 0xd61f00c0; // br x6
      case 0x4100: return 0x1e210800; // fmul s0,s0,s1
      case 0x4104: return 0xd4000021; // svc #1
      case 0x4300: return 0x94000040; // bl 0x4400
      case 0x4304: return 0x1e210800; // fmul s0,s0,s1
      case 0x4308: return 0xd4000021; // svc #1
      case 0x4400: return 0xd51b4401; // msr fpcr,x1
      case 0x4404: return 0x14000003; // b 0x4410
      case 0x4410: return 0xd65f03c0; // ret
      }
      return std::nullopt;
    }
    void CallSVC(std::uint32_t) override { jit->HaltExecution(); }
  };
  constexpr std::uint32_t fz = 1u << 24;
  for (const int sites : {0, 1, 2, 3}) {
    CAPTURE(sites);
    Callbacks cb;
    Dynarmic::A64::UserConfig config{}; config.callbacks = &cb;
    config.enable_cycle_counting = false; config.inline_fast_dispatch = sites == 1;
    config.inline_rsb_pop = sites == 1; config.dispatch_site_cache = sites == 2;
    config.compact_rsb_pop = sites == 3;
    Dynarmic::A64::Jit jit(config); cb.jit = &jit;
    // Smallest normal * 0.5 is denormal: kept with FZ clear, flushed to +0 with FZ set.
    const auto multiply = [&](std::uint64_t pc, std::uint32_t entry_fpcr, std::uint32_t written_fpcr) {
      jit.SetFpcr(entry_fpcr); jit.SetPC(pc); jit.SetRegister(1, written_fpcr); jit.SetRegister(6, 0x4100);
      jit.SetVector(0, {0x00800000, 0}); jit.SetVector(1, {0x3f000000, 0});
      jit.Run(); REQUIRE(jit.GetPC() == 0x4108);
      return jit.GetVector(0)[0] & 0xffffffff;
    };
    // The callee writes FPCR, then returns from a separate block: its pop runs
    // under the new FPCR, so a changed value misses the RSB entry pushed by bl.
    const auto call = [&](std::uint32_t entry_fpcr, std::uint32_t written_fpcr) {
      jit.SetFpcr(entry_fpcr); jit.SetPC(0x4300); jit.SetRegister(1, written_fpcr);
      jit.SetVector(0, {0x00800000, 0}); jit.SetVector(1, {0x3f000000, 0});
      jit.Run(); REQUIRE(jit.GetPC() == 0x430c);
      return jit.GetVector(0)[0] & 0xffffffff;
    };
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
      // The FPCR write ends its block, which must dispatch with the new value.
      CHECK(multiply(0x4000, 0, 0) == 0x00400000);
      CHECK(multiply(0x4000, 0, fz) == 0);
      CHECK(multiply(0x4000, fz, 0) == 0x00400000);
      // Without a write, each entry-FPCR variant of the branch block looks up its own target.
      CHECK(multiply(0x4200, fz, 0) == 0);
      CHECK(multiply(0x4200, 0, 0) == 0x00400000);
      CHECK(call(0, 0) == 0x00400000);
      CHECK(call(0, fz) == 0);
      CHECK(call(fz, fz) == 0);
      CHECK(call(fz, 0) == 0x00400000);
    }
  }
}

TEST_CASE("A64 supervisor-call returns link to the next block and follow a changed PC") {
  struct Callbacks final : A64CacheCallbacks {
    std::uint32_t next = 0x91000400; // add x0,x0,#1
    int mode = 0;                    // svc #2: 0 returns, 1 redirects PC
    std::optional<std::uint32_t> MemoryReadCode(Addr p) override {
      switch (p) {
      case 0x8000: return 0xd4000041; // svc #2
      case 0x8004: return next;
      case 0x8008: return 0xd4000021; // svc #1
      case 0x8100: return 0x91019000; // add x0,x0,#100
      case 0x8104: return 0xd4000021; // svc #1
      }
      return std::nullopt;
    }
    void CallSVC(std::uint32_t svc) override {
      if (svc == 1) return jit->HaltExecution();
      if (mode == 1) jit->SetPC(0x8100);
    }
  };
  for (const bool link : {false, true}) {
    CAPTURE(link);
    Callbacks cb;
    Dynarmic::A64::UserConfig config{}; config.callbacks = &cb;
    config.enable_cycle_counting = false; config.svc_return_link = link;
    Dynarmic::A64::Jit jit(config); cb.jit = &jit;
    const auto run = [&](int mode) {
      cb.mode = mode; jit.SetFpcr(0); jit.SetPC(0x8000); jit.SetRegister(0, 0);
      jit.Run();
      return std::pair{jit.GetRegister(0), jit.GetPC()};
    };
    for (unsigned repeat = 0; repeat < 3; ++repeat) CHECK(run(0) == std::pair<std::uint64_t, std::uint64_t>{1, 0x800c});
    CHECK(run(1) == std::pair<std::uint64_t, std::uint64_t>{100, 0x8108});
    CHECK(run(0) == std::pair<std::uint64_t, std::uint64_t>{1, 0x800c});
    // A linked successor that is invalidated and rewritten runs its new code.
    cb.next = 0x91000800; // add x0,x0,#2
    jit.InvalidateCacheRange(0x8004, 4);
    CHECK(run(0) == std::pair<std::uint64_t, std::uint64_t>{2, 0x800c});
  }
}

TEST_CASE("A64 host return stack returns through calls, overflow, mismatches, halts and invalidation") {
  constexpr auto bl = [](std::uint64_t from, std::uint64_t to) {
    return 0x94000000u | (static_cast<std::uint32_t>((to - from) / 4) & 0x03ffffff);
  };
  constexpr auto cbnz_x1 = [](std::uint64_t from, std::uint64_t to) {
    return 0xb5000001u | ((static_cast<std::uint32_t>((to - from) / 4) & 0x7ffff) << 5);
  };
  constexpr std::uint32_t ret = 0xd65f03c0, halt = 0xd4000021, add1 = 0x91000400, add10 = 0x91002800,
      add100 = 0x91019000, add50 = 0x9100c800;
  struct Callbacks final : A64CacheCallbacks {
    std::map<Addr, std::uint32_t> code;
    std::optional<std::uint32_t> MemoryReadCode(Addr p) override {
      const auto found = code.find(p);
      return found == code.end() ? std::nullopt : std::optional{found->second};
    }
    void CallSVC(std::uint32_t) override { jit->HaltExecution(); }
  };
  for (const bool host : {false, true}) {
    CAPTURE(host);
    Callbacks cb;
    auto& c = cb.code;
    // Nested: 0xa000 calls 0xa100, which saves x30 in x19 and calls 0xa200.
    c[0xa000] = bl(0xa000, 0xa100); c[0xa004] = add1; c[0xa008] = halt;
    c[0xa100] = 0xaa1e03f3; c[0xa104] = bl(0xa104, 0xa200); c[0xa108] = add10; c[0xa10c] = 0xaa1303fe; c[0xa110] = ret;
    c[0xa200] = add100; c[0xa204] = ret;
    // 3000 calls without a return (beyond the stack's 1024 entries), then one.
    c[0xb000] = bl(0xb000, 0xb100); c[0xb004] = add1; c[0xb008] = halt;
    c[0xb100] = 0xd1000421; c[0xb104] = cbnz_x1(0xb104, 0xb000); c[0xb108] = ret;
    // The callee returns elsewhere: movz x30,#0xc200.
    c[0xc000] = bl(0xc000, 0xc100); c[0xc004] = add1; c[0xc008] = halt;
    c[0xc100] = 0xd2800000u | (0xc200u << 5) | 30; c[0xc104] = ret;
    c[0xc200] = add50; c[0xc204] = halt;
    // The callee halts; its return runs in a later Run.
    c[0xd000] = bl(0xd000, 0xd100); c[0xd004] = add1; c[0xd008] = halt;
    c[0xd100] = halt; c[0xd104] = ret;
    Dynarmic::A64::UserConfig config{}; config.callbacks = &cb;
    config.enable_cycle_counting = false; config.host_return_stack = host;
    Dynarmic::A64::Jit jit(config); cb.jit = &jit;
    const auto run = [&](std::uint64_t pc, std::uint64_t x1 = 0) {
      jit.SetPC(pc); jit.SetRegister(0, 0); jit.SetRegister(1, x1);
      jit.Run();
      return std::pair{jit.GetRegister(0), jit.GetPC()};
    };
    using Result = std::pair<std::uint64_t, std::uint64_t>;
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
      CHECK(run(0xa000) == Result{111, 0xa00c});
      CHECK(run(0xb000, 3000) == Result{1, 0xb00c});
      CHECK(run(0xc000) == Result{50, 0xc208});
      CHECK(run(0xa000) == Result{111, 0xa00c});
      CHECK(run(0xd000) == Result{0, 0xd104});
      jit.Run();
      CHECK(Result{jit.GetRegister(0), jit.GetPC()} == Result{1, 0xd00c});
    }
    // Invalidating the continuation, or the caller holding the landing, while
    // the callee is halted: the return runs the current code.
    CHECK(run(0xd000) == Result{0, 0xd104});
    c[0xd004] = 0x91000800; // add x0,x0,#2
    jit.InvalidateCacheRange(0xd004, 4);
    jit.Run();
    CHECK(Result{jit.GetRegister(0), jit.GetPC()} == Result{2, 0xd00c});
    CHECK(run(0xd000) == Result{0, 0xd104});
    jit.InvalidateCacheRange(0xd000, 4);
    jit.Run();
    CHECK(Result{jit.GetRegister(0), jit.GetPC()} == Result{2, 0xd00c});
    CHECK(run(0xd000) == Result{0, 0xd104});
    jit.ClearCache();
    jit.Run();
    CHECK(Result{jit.GetRegister(0), jit.GetPC()} == Result{2, 0xd00c});
    // Reset() replaces the whole JIT state, the stack's bounds included.
    jit.Reset();
    CHECK(run(0xa000) == Result{111, 0xa00c});
    CHECK(run(0xb000, 3000) == Result{1, 0xb00c});
  }
}

TEST_CASE("A64 conditional exits store NZCV on the paths whose successors may read it") {
  struct Callbacks final : A64CacheCallbacks {
    std::array<std::uint32_t, 8> code{};
    std::optional<std::uint32_t> MemoryReadCode(Addr p) override {
      if (p < 0x6000 || p >= 0x6000 + 4 * code.size()) return std::nullopt;
      return code[(p - 0x6000) / 4];
    }
    void CallSVC(std::uint32_t) override { jit->HaltExecution(); }
  };
  constexpr std::uint32_t mrs_x9 = 0xd53b4209, cmp_x3 = 0xeb03007f, mov_x10 = 0xd280002a, svc = 0xd4000001;
  constexpr std::uint32_t nop = 0xd503201f, cset_x4_eq = 0x9a9f17e4, z_and_c = 0x60000000;
  // cmp x0,x1; cmp w0,w1; cmn x0,x1; cmn w0,w1; subs x2,x0,x1; cmp x0,#5.
  struct Producer { std::uint32_t word; bool sub, wide, imm; };
  constexpr std::array<Producer, 6> producers{{{0xeb01001f, true, true, false}, {0x6b01001f, true, false, false},
      {0xab01001f, false, true, false}, {0x2b01001f, false, false, false}, {0xeb010002, true, true, false},
      {0xf100141f, true, true, true}}};
  constexpr std::array<std::uint64_t, 9> values{0, 1, 5, 0x7fffffff, 0x80000000, 0xffffffff,
      0x7fffffffffffffff, 0x8000000000000000, 0xffffffffffffffff};
  // AddWithCarry from the Arm ARM: NZCV of a + (sub ? ~b : b) + sub.
  const auto reference = [](const Producer& p, std::uint64_t a, std::uint64_t b) {
    const unsigned bits = p.wide ? 64 : 32;
    const std::uint64_t mask = p.wide ? ~std::uint64_t(0) : 0xffffffff;
    const auto sext = [&](std::uint64_t x) -> __int128 { return p.wide ? __int128(std::int64_t(x)) : __int128(std::int32_t(x)); };
    a &= mask;
    const std::uint64_t y = (p.sub ? ~(p.imm ? 5 : b) : b) & mask;
    const unsigned __int128 sum = (unsigned __int128)a + y + p.sub;
    const std::uint64_t r = std::uint64_t(sum) & mask;
    const bool n = (r >> (bits - 1)) & 1, z = r == 0, c = (sum >> bits) & 1;
    const bool v = sext(a) + sext(y) + p.sub != sext(r);
    return std::uint32_t(n) << 31 | std::uint32_t(z) << 30 | std::uint32_t(c) << 29 | std::uint32_t(v) << 28;
  };
  const auto holds = [](unsigned cond, std::uint32_t nzcv) {
    const bool n = nzcv >> 31, z = (nzcv >> 30) & 1, c = (nzcv >> 29) & 1, v = (nzcv >> 28) & 1;
    const bool base[] = {z, c, n, v, c && !z, n == v, n == v && !z};
    return (cond & 1) ? !base[cond >> 1] : base[cond >> 1];
  };
  for (const bool liveness : {false, true}) {
    CAPTURE(liveness);
    Callbacks cb;
    Dynarmic::A64::UserConfig config{}; config.callbacks = &cb;
    config.enable_cycle_counting = false; config.nzcv_exit_liveness = liveness;
    Dynarmic::A64::Jit jit(config); cb.jit = &jit;
    const auto run = [&](std::uint64_t a, std::uint64_t b) {
      jit.SetPC(0x6000); jit.SetPstate(0xf0000000);
      jit.SetRegister(0, a); jit.SetRegister(1, b); jit.SetRegister(9, 0x99); jit.SetRegister(10, 0x1010);
      jit.Run();
    };
    for (const auto& p : producers) for (unsigned cond = 0; cond < 14; ++cond) {
      CAPTURE(p.word);
      CAPTURE(cond);
      const std::uint32_t bcond = 0x54000000 | cond;
      // Layout 0: fallthrough reads NZCV (mrs), the taken successor overwrites it.
      // Layout 1: the reverse. Layout 2: both successors overwrite it.
      for (unsigned layout = 0; layout < 3; ++layout) {
        CAPTURE(layout);
        if (layout == 0) cb.code = {p.word, bcond | 3 << 5, mrs_x9, svc, cmp_x3, mov_x10, svc, svc};
        if (layout == 1) cb.code = {p.word, bcond | 4 << 5, cmp_x3, mov_x10, svc, mrs_x9, svc, svc};
        if (layout == 2) cb.code = {p.word, bcond | 4 << 5, cmp_x3, mov_x10, svc, cmp_x3, svc, svc};
        jit.ClearCache();
        for (const auto a : values) for (const auto b : values) {
          run(a, b);
          const auto nzcv = reference(p, a, b);
          const bool taken = holds(cond, nzcv);
          const bool reads = layout == 0 ? !taken : layout == 1 && taken;
          CHECK(jit.GetRegister(9) == (reads ? nzcv : 0x99));
          CHECK(jit.GetPstate() == (reads ? nzcv : z_and_c));
          CHECK(jit.GetRegister(10) == ((layout == 0) == taken ? 1u : 0x1010u));
        }
      }
    }
    // The taken successor stops overwriting NZCV: invalidating just its code
    // must recompile the branch block so that exit stores NZCV again.
    cb.code = {0xeb01001f, 0x54000061, svc, svc, cmp_x3, cset_x4_eq, svc, svc};  // cmp x0,x1; b.ne +12
    jit.ClearCache();
    run(1, 2);
    CHECK(jit.GetRegister(4) == 1);
    cb.code[4] = nop;
    jit.InvalidateCacheRange(0x6010, 4);
    run(1, 2);
    CHECK(jit.GetRegister(4) == 0);
    cb.code[4] = cmp_x3;
    jit.InvalidateCacheRange(0x6010, 4);
    run(1, 2);
    CHECK(jit.GetRegister(4) == 1);
  }
}

TEST_CASE("A64 FRECPS infinity-times-zero lanes match the Arm reference") {
  struct Callbacks final : A64CacheCallbacks {
    std::uint32_t instruction = 0;
    std::optional<std::uint32_t> MemoryReadCode(Addr p) override {
      if (p == 0x5000) return instruction;
      if (p == 0x5004) return 0xd4000021; // svc #1
      return std::nullopt;
    }
    void CallSVC(std::uint32_t) override { jit->HaltExecution(); }
  };
  // Infinities times signed zeros, NaN inputs (fallback) and ordinary values.
  const std::array<std::pair<std::uint32_t, std::uint32_t>, 12> singles{{
      {0x7f800000, 0}, {0, 0x7f800000}, {0xff800000, 0}, {0x7f800000, 0x80000000},
      {0xff800000, 0x80000000}, {0x80000000, 0xff800000}, {0x7fc00001, 0}, {0x7f800000, 0x7f800001},
      {0x7f800000, 0x40000000}, {0x3fc00000, 0x40000000}, {0x00000001, 0x7f800000}, {0x3f800000, 0x3f800000}}};
  const auto widen = [](std::uint32_t f) -> std::uint64_t {
    // Same class and sign as a double: zeros, infinities, NaNs and normal values.
    const std::uint64_t sign = std::uint64_t(f >> 31) << 63;
    if ((f & 0x7fffffff) == 0) return sign;
    if ((f & 0x7f800000) == 0x7f800000) return sign | 0x7ff0000000000000ull | std::uint64_t(f & 0x7fffff) << 29;
    if ((f & 0x7f800000) == 0) return sign | 1; // denormal
    return sign | (std::uint64_t((f >> 23 & 0xff) - 127 + 1023) << 52) | std::uint64_t(f & 0x7fffff) << 29;
  };
  for (const std::uint32_t fpcr : {0u, 1u << 24}) {
    CAPTURE(fpcr);
    for (const std::uint32_t instruction : {0x5e21fc00u, 0x5e61fc00u, 0x4e21fc00u, 0x4e61fc00u}) {
      CAPTURE(instruction);
      Callbacks cb; cb.instruction = instruction;
      Dynarmic::A64::UserConfig config{}; config.callbacks = &cb;
      config.enable_cycle_counting = false;
      Dynarmic::A64::Jit jit(config); cb.jit = &jit;
      // Bit 28 is set for scalar (0x5e...) and clear for vector (0x4e...) encodings.
      const bool doubles = instruction & 0x00400000, vector = !(instruction & 0x10000000);
      const std::size_t lanes = vector ? (doubles ? 2 : 4) : 1;
      for (std::size_t first = 0; first < singles.size(); first += lanes) {
        std::array<std::uint64_t, 2> a{}, b{}, expected{};
        Dynarmic::FP::FPSR fpsr;
        for (std::size_t lane = 0; lane < lanes && first + lane < singles.size(); ++lane) {
          const auto [x, y] = singles[first + lane];
          if (doubles) {
            a[lane] = widen(x); b[lane] = widen(y);
            expected[lane] = Dynarmic::FP::FPRecipStepFused<std::uint64_t>(a[lane], b[lane], Dynarmic::FP::FPCR{fpcr}, fpsr);
          } else {
            const unsigned shift = 32 * (lane & 1);
            a[lane / 2] |= std::uint64_t(x) << shift; b[lane / 2] |= std::uint64_t(y) << shift;
            expected[lane / 2] |= std::uint64_t(Dynarmic::FP::FPRecipStepFused<std::uint32_t>(x, y, Dynarmic::FP::FPCR{fpcr}, fpsr)) << shift;
          }
        }
        jit.SetFpcr(fpcr); jit.SetFpsr(0); jit.SetPC(0x5000);
        jit.SetVector(0, {a[0], a[1]}); jit.SetVector(1, {b[0], b[1]});
        jit.Run(); REQUIRE(jit.GetPC() == 0x5008);
        const auto result = jit.GetVector(0);
        CAPTURE(first);
        // Infinity times zero raises no IOC (bit 0); only signalling NaN inputs do.
        // Under FZ, a flushed denormal sets IDC (bit 7) and, paired with an
        // infinity, still no IOC.
        CHECK((jit.GetFpsr() & 0x81) == (fpsr.Value() & 0x81));
        if (vector) {
          CHECK(result[0] == expected[0]); CHECK(result[1] == expected[1]);
        } else {
          CHECK((result[0] & (doubles ? ~0ull : 0xffffffffull)) == expected[0]);
        }
      }
    }
  }
}

TEST_CASE("A64 FRECPE inline estimates match the Arm reference") {
  struct Callbacks final : A64CacheCallbacks {
    std::uint32_t instruction = 0;
    std::optional<std::uint32_t> MemoryReadCode(Addr p) override {
      if (p == 0x5000) return instruction;
      if (p == 0x5004) return 0xd4000021; // svc #1
      return std::nullopt;
    }
    void CallSVC(std::uint32_t) override { jit->HaltExecution(); }
  };
  // Every sign, biased exponent and table index, with low fraction bits clear,
  // all set and mixed; plus NaN payloads. Exponents 0 and 253-255 reach the
  // reference fallback, the rest the table.
  std::vector<std::uint32_t> inputs;
  for (std::uint32_t sign = 0; sign < 2; ++sign)
    for (std::uint32_t exponent = 0; exponent < 256; ++exponent)
      for (std::uint32_t index = 0; index < 256; ++index)
        for (const std::uint32_t low : {0u, 0x7fffu, 0x2a5bu})
          inputs.push_back(sign << 31 | exponent << 23 | index << 15 | low);
  for (const std::uint32_t fpcr : {0u, 1u << 24, 1u << 25, 3u << 22, 1u << 23 | 1u << 24}) {
    CAPTURE(fpcr);
    // FRECPE S0, S1; FRECPE V0.4S, V1.4S; FRECPE V0.2S, V1.2S.
    for (const std::uint32_t instruction : {0x5ea1d820u, 0x4ea1d820u, 0x0ea1d820u}) {
      CAPTURE(instruction);
      for (const bool inline_estimate : {false, true}) {
        CAPTURE(inline_estimate);
        Callbacks cb; cb.instruction = instruction;
        Dynarmic::A64::UserConfig config{}; config.callbacks = &cb;
        config.enable_cycle_counting = false;
        if (!inline_estimate) config.optimizations &= ~Dynarmic::OptimizationFlag::InlineRecipEstimate;
        Dynarmic::A64::Jit jit(config); cb.jit = &jit;
        const std::size_t lanes = instruction == 0x4ea1d820u ? 4 : instruction == 0x0ea1d820u ? 2 : 1;
        std::size_t mismatches = 0;
        for (std::size_t first = 0; first < inputs.size(); first += lanes) {
          std::array<std::uint32_t, 4> operand{}, expected{};
          // The reference handles the four lanes the JIT reads; D-register forms read zero upper lanes.
          Dynarmic::FP::FPSR fpsr;
          for (std::size_t lane = 0; lane < 4; ++lane) {
            if (lane < lanes && first + lane < inputs.size()) operand[lane] = inputs[first + lane];
            if (lanes != 1 || lane == 0)
              expected[lane] = Dynarmic::FP::FPRecipEstimate<std::uint32_t>(operand[lane], Dynarmic::FP::FPCR{fpcr}, fpsr);
          }
          if (lanes == 2) expected[2] = expected[3] = 0;
          jit.SetFpcr(fpcr); jit.SetFpsr(0); jit.SetPC(0x5000);
          jit.SetVector(1, {operand[0] | std::uint64_t(operand[1]) << 32, operand[2] | std::uint64_t(operand[3]) << 32});
          jit.Run(); REQUIRE(jit.GetPC() == 0x5008);
          const auto result = jit.GetVector(0);
          const std::array<std::uint32_t, 4> got{std::uint32_t(result[0]), std::uint32_t(result[0] >> 32),
                                                 std::uint32_t(result[1]), std::uint32_t(result[1] >> 32)};
          bool same = (jit.GetFpsr() & 0x9f) == (fpsr.Value() & 0x9f);
          for (std::size_t lane = 0; lane < (lanes == 1 ? 1 : 4); ++lane) same = same && got[lane] == expected[lane];
          if (!same && ++mismatches <= 4) {
            INFO("first ", first, " operand ", operand[0], " got ", got[0], " expected ", expected[0],
                 " fpsr ", jit.GetFpsr(), " reference fpsr ", fpsr.Value());
            CHECK(same);
          }
        }
        CHECK(mismatches == 0);
      }
    }
  }
}

#ifdef _WIN32
namespace {
struct CallbackException { std::uint64_t value; };
struct A64ThrowCallbacks : A64CacheCallbacks {
  bool throwing = true;
  bool throw_bad_alloc = false;
  void CallSVC(std::uint32_t imm) override {
    if (!throwing) { A64CacheCallbacks::CallSVC(imm); return; }
    CHECK(jit->GetRegister(0) == 3);
    CHECK(jit->GetRegister(1) == 1);
    CHECK(jit->GetRegister(8) == 2);
    CHECK(jit->GetRegister(30) == 0x1004);
    if (throw_bad_alloc) throw std::bad_alloc{};
    throw CallbackException{0x123456789abcdef0ull};
  }
};
struct A32ThrowCallbacks : A32CacheCallbacks {
  bool throwing = true;
  bool throw_bad_alloc = false;
  void CallSVC(std::uint32_t imm) override {
    if (!throwing) { A32CacheCallbacks::CallSVC(imm); return; }
    CHECK(jit->Regs()[0] == 3);
    if (throw_bad_alloc) throw std::bad_alloc{};
    throw CallbackException{0x123456789abcdef0ull};
  }
};

// The generated caller seeds every Windows nonvolatile GPR and XMM register.
// A C++ wrapper catches the callback exception outside Run/Step, then returns
// to this caller, which checks the seeds and restores its own caller's state.
std::uint64_t check_nonvolatile(void* state, std::uint64_t (*invoke)(void*)) {
  using namespace Dynarmic::Backend::X64;
  RunCodeCallbacks callbacks{ArgCallback(+[](void*) -> const void* { return nullptr; }, 0), ArgCallback(+[](void*, std::uint64_t) {}, 0), ArgCallback(+[](void*) -> std::uint64_t { return 0; }, 0), false};
  BlockOfCode code(std::move(callbacks), JitStateInfo{A64JitState{}}, 16 * MiB, [](BlockOfCode&) {});
  code.PreludeComplete(); code.EnableWriting(); code.EnsureMemoryCommitted(MiB);
  auto entry = code.getCurr<std::uint64_t (*)()>();
  ABI_PushCalleeSaveRegistersAndAdjustStack(code);
  for (size_t index = 0; index < ABI_ALL_CALLEE_SAVE.size(); ++index) {
    if (!ABI_ALL_CALLEE_SAVE[index]) continue;
    const auto reg = HostLoc(index);
    if (HostLocIsGPR(reg)) code.mov(HostLocToReg64(reg), 0x1122334455660000ull + static_cast<unsigned>(reg));
    else code.movaps(HostLocToXmm(reg), code.Const(code.xword, 0x123456789abcdef0ull + static_cast<unsigned>(reg),
        0xfedcba9876543210ull ^ static_cast<unsigned>(reg)));
  }
  code.mov(code.ABI_PARAM1, reinterpret_cast<std::uintptr_t>(state));
  code.CallFunction(invoke);
  Xbyak::Label failed, done;
  code.mov(code.r10, 0x123456789abcdef0ull);
  code.cmp(code.rax, code.r10); code.jne(failed, Xbyak::CodeGenerator::T_NEAR);
  for (size_t index = 0; index < ABI_ALL_CALLEE_SAVE.size(); ++index) {
    if (!ABI_ALL_CALLEE_SAVE[index]) continue;
    const auto reg = HostLoc(index);
    if (HostLocIsGPR(reg)) {
      code.mov(code.r10, 0x1122334455660000ull + static_cast<unsigned>(reg));
      code.cmp(HostLocToReg64(reg), code.r10);
    } else {
      code.movaps(code.xmm0, HostLocToXmm(reg));
      code.pcmpeqd(code.xmm0, code.Const(code.xword, 0x123456789abcdef0ull + static_cast<unsigned>(reg),
        0xfedcba9876543210ull ^ static_cast<unsigned>(reg)));
      code.pmovmskb(code.eax, code.xmm0); code.cmp(code.eax, 0xffff);
    }
    code.jne(failed, Xbyak::CodeGenerator::T_NEAR);
  }
  code.mov(code.eax, 1); code.jmp(done);
  code.L(failed); code.xor_(code.eax, code.eax);
  code.L(done); ABI_PopCalleeSaveRegistersAndAdjustStack(code); code.ret();
  code.DisableWriting();
  return entry();
}

template<class Jit>
std::uint64_t catch_run(void* state) {
  const auto host_control = _mm_getcsr() & 0xffc0;
  try { static_cast<Jit*>(state)->Run(); }
  catch (const CallbackException& e) { CHECK((_mm_getcsr() & 0xffc0) == host_control); return e.value; }
  return 0;
}
template<class Jit>
std::uint64_t catch_bad_alloc(void* state) {
  const auto host_control = _mm_getcsr() & 0xffc0;
  try { static_cast<Jit*>(state)->Run(); }
  catch (const std::bad_alloc&) { CHECK((_mm_getcsr() & 0xffc0) == host_control); return 0x123456789abcdef0ull; }
  return 0;
}
template<class Jit>
std::uint64_t catch_step(void* state) {
  const auto host_control = _mm_getcsr() & 0xffc0;
  try { static_cast<Jit*>(state)->Step(); }
  catch (const CallbackException& e) { CHECK((_mm_getcsr() & 0xffc0) == host_control); return e.value; }
  return 0;
}
}

TEST_CASE("Windows A64 callback exceptions unwind Run and Step with nonvolatile state intact") {
  for (const bool cache : {false, true}) {
    for (const bool chain : {false, true}) {
      for (const bool cycles : {false, true}) {
        A64ThrowCallbacks cb;
        Dynarmic::A64::UserConfig config{};
        config.callbacks = &cb; config.enable_cycle_counting = cycles;
        config.fastmem_pointer = 0; config.fastmem_address_space_bits = 64;
        config.silently_mirror_fastmem = false; config.enable_register_cache = cache;
        config.enable_block_frame_chain = chain;
        Dynarmic::A64::Jit jit(config); cb.jit = &jit;
        jit.SetPC(0x1000); jit.SetRegister(0, 0); jit.SetRegister(1, 1);
        jit.SetRegister(6, 0x1100); jit.SetRegister(8, 2);
        CHECK(check_nonvolatile(&jit, catch_run<Dynarmic::A64::Jit>) == 1);
        // A thrown callback leaves the JIT reusable, including Step and Run re-entry.
        jit.SetPC(0x1008);
        CHECK(check_nonvolatile(&jit, catch_step<Dynarmic::A64::Jit>) == 1);
        // The throwing Step bypasses its epilogue; clear its outstanding halt bit.
        jit.ClearHalt(Dynarmic::HaltReason::Step);
        cb.throw_bad_alloc = true; jit.SetPC(0x1000); jit.SetRegister(0, 0);
        CHECK(check_nonvolatile(&jit, catch_bad_alloc<Dynarmic::A64::Jit>) == 1);
        cb.throwing = false; jit.SetPC(0x1000); jit.SetRegister(0, 0); jit.Run();
        CHECK(jit.GetRegister(0) == 3);
      }
    }
  }
}

TEST_CASE("Windows A32 callback exceptions unwind Run and Step with nonvolatile state intact") {
  for (const bool cycles : {false, true}) {
    A32ThrowCallbacks cb;
    Dynarmic::A32::UserConfig config{};
    config.callbacks = &cb; config.enable_cycle_counting = cycles;
    config.enable_block_frame_chain = false;
    Dynarmic::A32::Jit jit(config); cb.jit = &jit; jit.SetCpsr(0x10);
    jit.Regs()[15] = 0x1000; jit.Regs()[0] = 0; jit.Regs()[1] = 1;
    jit.Regs()[6] = 0x1100; jit.Regs()[8] = 2;
    CHECK(check_nonvolatile(&jit, catch_run<Dynarmic::A32::Jit>) == 1);
    jit.Regs()[15] = 0x1008;
    CHECK(check_nonvolatile(&jit, catch_step<Dynarmic::A32::Jit>) == 1);
    // The throwing Step bypasses its epilogue; clear its outstanding halt bit.
    jit.ClearHalt(Dynarmic::HaltReason::Step);
    cb.throw_bad_alloc = true; jit.Regs()[15] = 0x1000; jit.Regs()[0] = 0;
    CHECK(check_nonvolatile(&jit, catch_bad_alloc<Dynarmic::A32::Jit>) == 1);
    cb.throwing = false; jit.Regs()[15] = 0x1000; jit.Regs()[0] = 0; jit.Run();
    CHECK(jit.Regs()[0] == 3);
  }
}
namespace {
struct A64MemoryThrowCallbacks : A64CacheCallbacks {
  std::uint32_t instruction = 0;
  bool bad_alloc = false;
  unsigned calls = 0;
  std::optional<std::uint32_t> MemoryReadCode(Addr p) override {
    if (p == 0x2000) return instruction;
    if (p == 0x2004) return 0xd4000021; // svc #1
    return std::nullopt;
  }
  [[noreturn]] void raise(Addr address) {
    CHECK(address == 1); ++calls;
    if (bad_alloc) throw std::bad_alloc{};
    throw CallbackException{0x123456789abcdef0ull};
  }
  std::uint64_t MemoryRead(Addr p, std::size_t) override { raise(p); }
  void MemoryWrite(Addr p, std::uint64_t, std::size_t) override { raise(p); }
  Dynarmic::A64::Vector MemoryRead128(Addr p) override { raise(p); }
  void MemoryWrite128(Addr p, Dynarmic::A64::Vector) override { raise(p); }
  Dynarmic::A64::Vector MemoryAtomicCompareAndSwap128(Addr p,
      Dynarmic::A64::Vector, Dynarmic::A64::Vector) override { raise(p); }
  std::uint64_t MemoryAtomic(Addr p, std::size_t, Dynarmic::A64::AtomicOp,
      std::uint64_t, std::uint64_t) override { raise(p); }
};
struct A32MemoryThrowCallbacks : A32CacheCallbacks {
  std::uint32_t instruction = 0;
  unsigned calls = 0;
  std::optional<std::uint32_t> MemoryReadCode(Addr p) override {
    if (p == 0x2000) return instruction;
    if (p == 0x2004) return 0xef000001;
    return std::nullopt;
  }
  std::uint64_t MemoryRead(Addr p, std::size_t) override {
    CHECK(p == 1); ++calls; throw CallbackException{0x123456789abcdef0ull};
  }
  void MemoryWrite(Addr p, std::uint64_t, std::size_t) override {
    CHECK(p == 1); ++calls; throw CallbackException{0x123456789abcdef0ull};
  }
};
}

TEST_CASE("Windows A64 memory callback exceptions unwind nested generated frames") {
  for (const bool fastmem : {false, true}) {
    for (const std::uint32_t instruction : {
        0xf9400020u, // ldr x0,[x1]
        0xf9000020u, // str x0,[x1]
        0x3dc00020u, // ldr q0,[x1]
        0x3d800020u, // str q0,[x1]
        0xf8e80020u, // ldaddal x8,x0,[x1]
        0x48227c24u  // casp x2,x3,x4,x5,[x1]
    }) {
      CAPTURE(fastmem);
      CAPTURE(instruction);
      A64MemoryThrowCallbacks cb; cb.instruction = instruction;
      Dynarmic::A64::UserConfig config{};
      config.callbacks = &cb; config.enable_cycle_counting = false;
      if (fastmem) config.fastmem_pointer = 0;
      config.fastmem_address_space_bits = 64;
      config.silently_mirror_fastmem = false; config.enable_register_cache = fastmem;
      if (fastmem) config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
      Dynarmic::A64::Jit jit(config); cb.jit = &jit; jit.SetFpcr(1u << 22);
      for (unsigned repeat = 0; repeat < 3; ++repeat) {
        // The second pass reuses the table; the third recreates it after clearing.
        if (repeat == 2) jit.ClearCache();
        jit.SetPC(0x2000); jit.SetRegister(1, 1); jit.SetRegister(8, 2);
        cb.bad_alloc = repeat == 1;
        CHECK(check_nonvolatile(&jit, cb.bad_alloc ? catch_bad_alloc<Dynarmic::A64::Jit> : catch_run<Dynarmic::A64::Jit>) == 1);
        CHECK(cb.calls == repeat + 1);
      }
    }
  }
}

TEST_CASE("Windows A32 memory callback exceptions unwind nested generated frames") {
  for (const bool fastmem : {false, true}) {
    for (const std::uint32_t instruction : {0xe5910000u, 0xe5810000u}) {
      CAPTURE(fastmem);
      CAPTURE(instruction); // ldr/str r0,[r1]
      A32MemoryThrowCallbacks cb; cb.instruction = instruction;
      Dynarmic::A32::UserConfig config{};
      config.callbacks = &cb; config.enable_cycle_counting = false;
      if (fastmem) config.fastmem_pointer = 0;
      Dynarmic::A32::Jit jit(config); cb.jit = &jit;
      jit.SetCpsr(0x10); jit.SetFpscr(1u << 22);
      for (unsigned repeat = 0; repeat < 2; ++repeat) {
        if (repeat) jit.ClearCache();
        jit.Regs()[15] = 0x2000; jit.Regs()[1] = 1;
        CHECK(check_nonvolatile(&jit, catch_run<Dynarmic::A32::Jit>) == 1);
        CHECK(cb.calls == repeat + 1);
      }
    }
  }
}

TEST_CASE("Windows monitored LSE callback exceptions release the monitor for re-entry") {
  Dynarmic::ExclusiveMonitor monitor(1);
  A64MemoryThrowCallbacks cb; cb.instruction = 0xf8e80020;
  Dynarmic::A64::UserConfig config{};
  config.callbacks = &cb; config.enable_cycle_counting = false;
  config.global_monitor = &monitor;
  Dynarmic::A64::Jit jit(config); cb.jit = &jit;
  for (unsigned repeat = 0; repeat < 2; ++repeat) {
    jit.SetPC(0x2000); jit.SetRegister(1, 1); jit.SetRegister(8, 2);
    CHECK(check_nonvolatile(&jit, catch_run<Dynarmic::A64::Jit>) == 1);
    CHECK(monitor.DoAtomicOperation([] { return 42; }) == 42);
  }
}

TEST_CASE("Windows A64 exclusive vector callback exceptions unwind the 72-byte helper frame") {
  struct Callbacks : A64MemoryThrowCallbacks {
    std::optional<std::uint32_t> MemoryReadCode(Addr p) override {
      if (p == 0x2000) return 0xc87f0c22; // ldxp x2,x3,[x1]
      if (p == 0x2004) return 0xc8240c22; // stxp w4,x2,x3,[x1]
      if (p == 0x2008) return 0xd4000021;
      return std::nullopt;
    }
    Dynarmic::A64::Vector MemoryRead128(Addr) override { return {}; }
    bool MemoryWriteExclusive128(Addr p, Dynarmic::A64::Vector, Dynarmic::A64::Vector) override { raise(p); }
  };
  for (const bool fastmem : {false, true}) {
    Callbacks cb;
    Dynarmic::ExclusiveMonitor monitor(1);
    Dynarmic::A64::UserConfig config{};
    config.global_monitor = &monitor;
    config.callbacks = &cb; config.enable_cycle_counting = false;
    config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
    if (fastmem) config.fastmem_pointer = 0;
    config.fastmem_address_space_bits = 64; config.silently_mirror_fastmem = false;
    config.enable_register_cache = fastmem; config.fastmem_exclusive_access = fastmem;
    Dynarmic::A64::Jit jit(config); cb.jit = &jit; jit.SetFpcr(1u << 22);
    jit.SetPC(0x2000); jit.SetRegister(1, 1);
    CHECK(check_nonvolatile(&jit, catch_run<Dynarmic::A64::Jit>) == 1);
    CHECK(cb.calls == 1);
  }
}

TEST_CASE("Windows A64 cache-pressure lookup exceptions unwind the active dispatcher") {
  JitCacheTrace trace;
  struct Callbacks : A64CacheCallbacks {
    JitCacheTrace& trace;
    bool stop = false;
    explicit Callbacks(JitCacheTrace& trace) : trace(trace) {}
    std::optional<std::uint32_t> MemoryReadCode(Addr p) override {
      if (stop) return 0xd4000021;
      if (trace.clears) throw CallbackException{0x123456789abcdef0ull};
      if (p % 256 < 128) return 0x8b010000; // add x0,x0,x1
      return 0x14000000u | ((256 - p % 256) / 4); // next cold block
    }
  } cb(trace);
  Dynarmic::A64::UserConfig config{};
  config.callbacks = &cb; config.enable_cycle_counting = false; config.code_cache_size = 16 * MiB;
  config.fastmem_pointer = 0; config.fastmem_address_space_bits = 64;
  config.silently_mirror_fastmem = false; config.enable_register_cache = true;
  Dynarmic::A64::Jit jit(config); cb.jit = &jit;
  jit.SetPC(0x100000); jit.SetRegister(0, 0); jit.SetRegister(1, 1);
  CHECK(check_nonvolatile(&jit, catch_run<Dynarmic::A64::Jit>) == 1);
  CHECK(trace.clears == 1);
  CHECK(jit.GetRegister(0) > 0);
  cb.stop = true; jit.Run();
}

TEST_CASE("Windows A32 cache-pressure lookup exceptions unwind the active dispatcher") {
  JitCacheTrace trace;
  struct Callbacks : A32CacheCallbacks {
    JitCacheTrace& trace;
    bool stop = false;
    explicit Callbacks(JitCacheTrace& trace) : trace(trace) {}
    std::optional<std::uint32_t> MemoryReadCode(Addr p) override {
      if (stop) return 0xef000001;
      if (trace.clears) throw CallbackException{0x123456789abcdef0ull};
      if (p % 256 < 128) return 0xe0800001; // add r0,r0,r1
      return 0xea000000u | ((256 - p % 256 - 8) / 4);
    }
  } cb(trace);
  Dynarmic::A32::UserConfig config{};
  config.callbacks = &cb; config.enable_cycle_counting = false; config.code_cache_size = 16 * MiB;
  Dynarmic::A32::Jit jit(config); cb.jit = &jit; jit.SetCpsr(0x10);
  jit.Regs()[15] = 0x100000; jit.Regs()[0] = 0; jit.Regs()[1] = 1;
  CHECK(check_nonvolatile(&jit, catch_run<Dynarmic::A32::Jit>) == 1);
  CHECK(trace.clears == 1);
  CHECK(jit.Regs()[0] > 0);
  cb.stop = true; jit.Run();
}
#endif
