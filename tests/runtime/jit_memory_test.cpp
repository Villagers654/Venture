#define XBYAK_STRICT_CHECK_MEM_REG_SIZE 0
// Match Dynarmic's private setting (src/dynarmic/CMakeLists.txt): its register
// allocator intentionally emits memory operands with an inferred width. Header
// users must agree or their inline Xbyak definitions violate the ODR.
#include <doctest/doctest.h>
#include <dynarmic/backend/x64/a64_jitstate.h>
#include <dynarmic/backend/x64/block_of_code.h>
#include <dynarmic/interface/A64/a64.h>
#include "platform/memory.h"
#include "cpu/cpu.h"
#include "cpu/outlined_atomics.h"
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <vector>

using namespace Dynarmic::Backend::X64;
namespace {
constexpr std::uint64_t MiB = 1024 * 1024;
RunCodeCallbacks callbacks() {
  return {ArgCallback(+[](void*) -> const void* { return nullptr; }, 0),
          ArgCallback(+[](void*, std::uint64_t) {}, 0),
          ArgCallback(+[](void*) -> std::uint64_t { return 0; }, 0), false};
}

TEST_CASE("A64 SVC callbacks read and write cached state while JIT execution continues") {
  struct Callbacks final : Dynarmic::A64::UserCallbacks {
    Dynarmic::A64::Jit* jit = nullptr;
    unsigned calls = 0;
    unsigned fast_calls = 0;
    unsigned slow_calls = 0;
    unsigned known_calls = 0;
    std::uint32_t fast_immediate = 1;
    bool needs_slow_boundary = false;
    enum : std::uint64_t { text = 0x1000 };
    std::array<std::uint32_t, 26> words{
        0xd28001a2, 0xd28001c3, 0xd28001e4, 0xd2800205, 0xd2800226, 0xd2800247,
        0xd2800160, 0xd2800181, 0xd28001a8,
        0xd28001de, 0xd4000021, 0x8b010000, 0x8b080000,
        0x8b020000, 0x8b030000, 0x8b040000, 0x8b050000,
        0x8b060000, 0x8b070000, 0xd65f03c0,
        0xd4200000, 0xd4000041, 0x8b010000, 0x8b080000, 0xd4000061, 0xd4200000};
    std::optional<std::uint32_t> MemoryReadCode(std::uint64_t p) override {
      if (p < text || p >= text + sizeof(words)) return std::nullopt;
      return words[(p - text) / 4];
    }
    std::uint64_t MemoryRead(std::uint64_t, std::size_t) override { return 0; }
    void MemoryWrite(std::uint64_t, std::uint64_t, std::size_t) override {}
    std::uint8_t MemoryRead8(std::uint64_t) { return 0; }
    std::uint16_t MemoryRead16(std::uint64_t) { return 0; }
    std::uint32_t MemoryRead32(std::uint64_t) { return 0; }
    std::uint64_t MemoryRead64(std::uint64_t) { return 0; }
    Dynarmic::A64::Vector MemoryRead128(std::uint64_t) override { return {}; }
    void MemoryWrite8(std::uint64_t, std::uint8_t) {}
    void MemoryWrite16(std::uint64_t, std::uint16_t) {}
    void MemoryWrite32(std::uint64_t, std::uint32_t) {}
    void MemoryWrite64(std::uint64_t, std::uint64_t) {}
    void MemoryWrite128(std::uint64_t, Dynarmic::A64::Vector) override {}
    std::uint64_t MemoryAtomic(std::uint64_t, std::size_t, Dynarmic::A64::AtomicOp,
                               std::uint64_t, std::uint64_t) override { return 0; }
    Dynarmic::A64::Vector MemoryAtomicCompareAndSwap128(std::uint64_t,
        Dynarmic::A64::Vector, Dynarmic::A64::Vector) override { return {}; }
    void InterpreterFallback(std::uint64_t, std::size_t) { FAIL("unexpected fallback"); }
    void ExceptionRaised(std::uint64_t, Dynarmic::A64::Exception) override { FAIL("unexpected exception"); }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1000; }
    std::uint64_t GetCNTPCT() override { return 0; }
    void CallSVC(std::uint32_t immediate) override {
      ++slow_calls;
      dispatch(immediate);
    }
    void CallSVCFast(std::uint32_t immediate) override {
      ++fast_calls;
      dispatch(immediate);
    }
    void CallSVCFastKnown(std::uint32_t immediate, const void* metadata, void* state) override {
      CHECK(metadata == this);
      CHECK(state != nullptr);
      CHECK(static_cast<A64JitState*>(state)->reg[0] == jit->GetRegister(0));
      ++known_calls;
      dispatch(immediate);
    }
    void dispatch(std::uint32_t immediate) {
      ++calls;
      if (immediate == fast_immediate && !needs_slow_boundary) {
        CHECK(jit->GetRegister(0) == 11); CHECK(jit->GetRegister(1) == 12);
        for (unsigned r = 2; r < 8; ++r) CHECK(jit->GetRegister(r) == r + 11);
        CHECK(jit->GetRegister(8) == 13); CHECK(jit->GetRegister(30) == 14);
        jit->SetRegister(0, 10); jit->SetRegister(1, 20);
        for (unsigned r = 2; r < 8; ++r) jit->SetRegister(r, (r + 1) * 10);
        jit->SetRegister(8, 30); jit->SetRegister(30, text + 84);
        needs_slow_boundary = true;
      } else if (immediate == 2 || immediate == fast_immediate) {
        // The conditional fast handler declines without modifying state.
        CHECK(jit->GetRegister(0) == 390); CHECK(jit->GetRegister(1) == 20);
        CHECK(jit->GetRegister(8) == 30); CHECK(jit->GetRegister(30) == text + 84);
        jit->HaltExecution();
      } else {
        CHECK(immediate == 3);
        CHECK(jit->GetRegister(0) == 600);
        CHECK(jit->GetRegister(1) == 200);
        CHECK(jit->GetRegister(8) == 300);
        CHECK(jit->GetRegister(30) == 400);
        jit->HaltExecution();
      }
    }
  } callbacks;
  for (const bool known : {false, true}) {
  for (const auto immediate : {1u, venture::kSvcHleCallFast, venture::kSvcHleCallFastFp}) {
    callbacks.fast_immediate = immediate;
    callbacks.words[10] = 0xd4000001u | (immediate << 5);
    callbacks.words[21] = 0xd4000001u | ((immediate == 1 ? 2 : immediate) << 5);
    for (const bool count_cycles : {false, true}) {
      Dynarmic::A64::UserConfig config{};
      config.callbacks = &callbacks;
      config.fastmem_pointer = 0;
      config.fastmem_address_space_bits = 64;
      config.silently_mirror_fastmem = false;
      config.enable_register_cache = true;
      config.enable_cycle_counting = count_cycles;
      config.svc_fast = venture::kSvcHleCallFast;
      config.svc_fp_transfer = venture::kSvcHleCallFastFp;
      if (known) config.svc_lookup = [&](std::uint64_t, std::uint32_t) -> const void* { return &callbacks; };
      Dynarmic::A64::Jit jit(config);
      callbacks.jit = &jit;
      for (const bool single_step : {false, true}) {
        auto drive = [&](std::uint64_t stop_pc) {
          if (!single_step) jit.Run();
          else for (unsigned instruction = 0; instruction < 32 && jit.GetPC() != stop_pc; ++instruction)
            jit.Step();
          CHECK(jit.GetPC() == stop_pc);
        };
        for (unsigned repeat = 0; repeat < 3; ++repeat) {
          callbacks.needs_slow_boundary = false;
          jit.SetPC(Callbacks::text);
          drive(Callbacks::text + 88);
          CHECK(jit.GetRegister(0) == 390);
          CHECK(jit.GetRegister(1) == 20);
          for (unsigned r = 2; r < 8; ++r) CHECK(jit.GetRegister(r) == (r + 1) * 10);
          CHECK(jit.GetRegister(8) == 30);
          CHECK(jit.GetRegister(30) == Callbacks::text + 84);
        }
        // A slow boundary may invoke guest code only after Run/Step returned.
        // Save the suspended outer call, run its nested guest callback, then
        // restore it. Both invocations cross the cached-register HLE boundary.
        const auto outer_pc = jit.GetPC();
        const auto outer_registers = jit.GetRegisters();
        callbacks.needs_slow_boundary = false;
        jit.SetPC(Callbacks::text);
        drive(Callbacks::text + 88);
        CHECK(jit.GetRegister(0) == 390);
        jit.SetRegisters(outer_registers);
        jit.SetPC(outer_pc);
        // The slow handler writes cached state after its nested call returns.
        jit.SetRegister(0, 100); jit.SetRegister(1, 200);
        jit.SetRegister(8, 300); jit.SetRegister(30, 400);
        drive(Callbacks::text + 100);
        CHECK(jit.GetRegister(0) == 600);
      }
    }
  }
  }
  CHECK(callbacks.calls == 216);
  CHECK(callbacks.fast_calls == 64);
  CHECK(callbacks.known_calls == 64);
  CHECK(callbacks.slow_calls == 88);
}
}

TEST_CASE("JIT cache reserves constants and grows committed pages on use") {
  // The common x64 cache serves both A32 and A64. Exercise real RIP-relative
  // constant loads, publication, growth and cache reuse without a guest image.
  BlockOfCode code(callbacks(), JitStateInfo{A64JitState{}}, 32 * MiB, [](BlockOfCode&) {});
  code.PreludeComplete();
  const auto base = venture::h2g(code.getCode());
  if (auto committed = venture::platform::committed_bytes(base, 32 * MiB)) {
    CHECK(*committed <= 2 * MiB);
    CHECK(*venture::platform::committed_bytes(base, 2 * MiB) <= 64 * 1024);
  }
  std::array<std::uint64_t (*)(), 3> loads{};
  code.EnableWriting();
  code.EnsureMemoryCommitted(MiB);
  for (std::uint64_t i = 0; i < 9000; ++i) {
    auto entry = code.getCurr<std::uint64_t (*)()>();
    code.mov(code.rax, code.Const(code.qword, 0x1234567800000000ull + i));
    code.ret();
    if (i == 0) loads[0] = entry;
    if (i == 4096) loads[1] = entry;
    if (i == 8999) loads[2] = entry;
  }
  code.DisableWriting();
  CHECK(loads[0]() == 0x1234567800000000ull);
  CHECK(loads[1]() == 0x1234567800001000ull);
  CHECK(loads[2]() == 0x1234567800002327ull);
  if (auto pool = venture::platform::committed_bytes(base, 2 * MiB)) {
    CHECK(*pool >= 9000 * 16);
    CHECK(*pool < 256 * 1024);
    CHECK(*venture::platform::committed_bytes(base, 32 * MiB) < 3 * MiB);
  }
  code.ClearCache();
  code.EnableWriting();
  // Allocation crosses the initial code commitment and must grow safely.
  auto* island = static_cast<std::byte*>(code.AllocateFromCodeSpace(2 * MiB));
  CHECK(island[0] == std::byte{0});
  CHECK(island[2 * MiB - 1] == std::byte{0});
  auto reload = code.getCurr<std::uint64_t (*)()>();
  code.mov(code.rax, code.Const(code.qword, 0x1234567800001000ull));
  code.ret();
  code.DisableWriting();
  CHECK(reload() == 0x1234567800001000ull);
  if (auto committed = venture::platform::committed_bytes(base, 32 * MiB)) CHECK(*committed < 4 * MiB);
}

TEST_CASE("JIT cache stores code through a writable alias and never flips protection") {
  // W^X by dual mapping: Xbyak computes addresses in the executable view, which
  // stays read+execute through emission and block-link patching, while bytes
  // are stored through a writable view of the same memory.
  BlockOfCode code(callbacks(), JitStateInfo{A64JitState{}}, 32 * MiB, [](BlockOfCode&) {});
  code.PreludeComplete();
  const auto executable_only = [](const void* p) {
    std::uint64_t old = 0;
    const auto address = venture::h2g(p);
    // A locked add of zero needs write access but leaves the byte as it was, so a
    // writable (e.g. RWX) view fails this check without corrupting the code it probes.
    return !venture::platform::memory_accessible(address, 1, true) && venture::platform::memory_accessible(address, 1, false) &&
           !venture::platform::atomic_rmw(address, 1, venture::platform::AtomicRmw::add, 0, 0, old);
  };
  code.EnableWriting();
  code.EnsureMemoryCommitted(MiB);
  auto entry = code.getCurr<std::uint64_t (*)()>();
  const auto* patch_site = code.getCurr();
  code.mov(code.rax, 0x1111111111111111ull);  // 10-byte movabs, like a link patch.
  code.ret();
  const auto* writable = code.GetWritablePointer(patch_site);
  CHECK(static_cast<const void*>(writable) != static_cast<const void*>(patch_site));
  CHECK(std::memcmp(writable, patch_site, 11) == 0);
  CHECK(executable_only(patch_site));  // Inside a writing scope too.
  CHECK(venture::platform::memory_accessible(venture::h2g(writable), 1, true));
  code.DisableWriting();
  CHECK(entry() == 0x1111111111111111ull);

  // Rewrite the published block in place, as Dynarmic links and unlinks blocks.
  code.EnableWriting();
  const auto* append = code.getCurr();
  code.SetCodePtr(patch_site);
  code.mov(code.rax, 0x2222222222222222ull);
  code.SetCodePtr(append);
  // Code-space data written through the alias (as unwind records are).
  auto* island = static_cast<std::uint64_t*>(code.AllocateFromCodeSpace(sizeof(std::uint64_t)));
  *code.GetWritablePointer(island) = 0x3333333333333333ull;
  auto reader = code.getCurr<std::uint64_t (*)()>();
  code.mov(code.rax, code.ptr[code.rip + island]);
  code.ret();
  code.DisableWriting();
  CHECK(entry() == 0x2222222222222222ull);
  CHECK(reader() == 0x3333333333333333ull);
  CHECK(executable_only(patch_site));
  CHECK(executable_only(island));
}


TEST_CASE("Outlined atomic matching validates every instruction and relocation") {
  using namespace venture::outlined_atomics;
  for (unsigned op = 0; op < 2; ++op) for (unsigned order = 0; order < 4; ++order)
  for (bool bti : {false, true}) {
    const bool acq = order & 1, rel = order & 2;
    std::vector<std::uint32_t> words;
    if (bti) words.push_back(0xd503245f);
    const std::array<std::uint32_t, 4> prefix{0xd0000bb0, 0x39638210, 0x34000070,
      op == 0 ? 0xf8200020u | (acq ? 0x800000u : 0) | (rel ? 0x400000u : 0)
              : 0xc8a07c41u | (acq ? 0x400000u : 0) | (rel ? 0x8000u : 0)};
    words.insert(words.end(), prefix.begin(), prefix.end());
    const std::vector<std::uint32_t> tail = op == 0
      ? std::vector<std::uint32_t>{0xd65f03c0,0xaa0003f0,0xc85f7c20u | (acq ? 0x8000u : 0),
          0x8b100011,0xc80f7c31u | (rel ? 0x8000u : 0),0x35ffffaf,0xd65f03c0}
      : std::vector<std::uint32_t>{0xd65f03c0,0xaa0003f0,0xc85f7c40u | (acq ? 0x8000u : 0),
          0xeb10001f,0x54000061,0xc8117c41u | (rel ? 0x8000u : 0),0x35ffff91,0xd65f03c0};
    words.insert(words.end(), tail.begin(), tail.end());
    auto match = recognize(std::span<const std::uint32_t>(words));
    REQUIRE(match); CHECK(match->index() == op * 4 + order); CHECK(match->words == words.size());
    for (std::size_t n = 0; n < words.size(); ++n) {
      CHECK_FALSE(recognize(std::span<const std::uint32_t>(words.data(), n)));
      for (unsigned bit = 0; bit < 32; ++bit) {
        const auto index = n - unsigned(bti);
        const bool relocation = (n >= unsigned(bti)) &&
          ((index == 0 && ((0x60ffffe0u >> bit) & 1)) ||
           (index == 1 && ((0x003ffc00u >> bit) & 1)));
        if (relocation) continue;
        auto changed = words; changed[n] ^= 1u << bit;
        CHECK_FALSE(recognize(std::span<const std::uint32_t>(changed)));
      }
    }
    words[unsigned(bti)] ^= 0x60ffffe0u;
    words[unsigned(bti) + 1] ^= 0x003ffc00u;
    CHECK(recognize(std::span<const std::uint32_t>(words)));
  }
}
