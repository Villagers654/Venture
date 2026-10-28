// SPDX-License-Identifier: GPL-3.0-or-later
// A BL to a compiler-rt outlined atomic helper (lse.S layout: ADRP/LDRB/CBZ on
// the LSE flag, one LSE instruction, RET, then the LL/SC fallback) runs as that
// LSE instruction in the calling block. Every helper family, size and ordering
// must leave the same architectural state as calling the helper (x15-x17 are
// call-clobbered and excepted), an access abort must stop at the helper's LSE
// instruction with X30 set by the BL, rewriting the helper must invalidate the
// caller's block, and a near-miss body must stay a call.
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace Dynarmic;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;

constexpr u32 bti_c = 0xd503245f;
constexpr u32 cbz_w16 = 0x34000070;    // cbz w16, .+12
constexpr u32 ret = 0xd65f03c0;
constexpr u32 brk0 = 0xd4200000;       // the LL/SC fallback must not run (flag set)
constexpr u32 add_x7 = 0x91000407;     // add x7, x0, #1: uses the result in the block
constexpr u32 svc0 = 0xd4000001;
constexpr u32 nop = 0xd503201f;
constexpr u64 abort_at = 0x80;         // unmapped
constexpr u64 x16_in = 0x1616161616161616;

void require(bool ok, const std::string& what) { if (!ok) throw std::runtime_error(what); }
std::string hex(u64 v) { char b[24]; std::snprintf(b, sizeof(b), "%#llx", static_cast<unsigned long long>(v)); return b; }

class Cpu final : private A64::UserCallbacks {
 public:
  Cpu(ExclusiveMonitor& monitor, bool inline_atomics) {
    A64::UserConfig c;
    c.callbacks = this; c.global_monitor = &monitor; c.processor_id = 0;
    c.fastmem_pointer = 0; c.fastmem_address_space_bits = 64; c.silently_mirror_fastmem = false;
    c.enable_register_cache = true; c.fastmem_exclusive_access = true; c.plain_exclusive_loads = true;
    c.check_halt_on_memory_access = true;
    c.enable_cycle_counting = false;
    c.cold_code_region = true; c.compact_block_links = true; c.enable_bounded_halt_polling = true;
    c.fast_dispatch_table_bits = 16; c.fast_dispatch_table_pages = true; c.nzcv_exit_liveness = true;
    c.dispatch_site_cache = true;
    c.inline_outlined_atomics = inline_atomics;
    c.unsafe_optimizations = true;
    c.optimizations |= OptimizationFlag::Unsafe_IgnoreGlobalMonitor; // As the runtime.
    jit = std::make_unique<A64::Jit>(c);
  }
  std::unique_ptr<A64::Jit> jit;
  bool hints_as_nop = true;

 private:
  std::optional<u32> MemoryReadCode(u64 p) override {
    u32 word; std::memcpy(&word, reinterpret_cast<void*>(p), sizeof(word));
    // As the runtime's reader: HINT-space instructions (BTI included) read as NOP.
    if (hints_as_nop && (word & 0xfffff01fu) == 0xd503201fu) return nop;
    return word;
  }
  void abort(u64 p) {
    require(p == abort_at, "callback access at " + hex(p));
    jit->HaltExecution(HaltReason::MemoryAbort);
  }
  u64 MemoryRead(u64 p, size_t) override { abort(p); return 0; }
  void MemoryWrite(u64 p, u64, size_t) override { abort(p); }
  bool MemoryWriteExclusive64(u64 p, u64, u64) override { abort(p); return false; }
  u64 MemoryAtomic(u64 p, size_t, A64::AtomicOp, u64, u64) override { abort(p); return 0; }
  A64::Vector MemoryRead128(u64 p) override { throw std::runtime_error("unexpected read128 at " + hex(p)); }
  void MemoryWrite128(u64 p, A64::Vector) override { throw std::runtime_error("unexpected write128 at " + hex(p)); }
  A64::Vector MemoryAtomicCompareAndSwap128(u64 p, A64::Vector, A64::Vector) override { throw std::runtime_error("unexpected CASP at " + hex(p)); }
  void CallSVC(u32) override { jit->HaltExecution(HaltReason::UserDefined1); }
  void ExceptionRaised(u64 pc, A64::Exception) override { throw std::runtime_error("unexpected exception at " + hex(pc)); }
  void AddTicks(u64) override {}
  u64 GetTicksRemaining() override { return 1000000; }
  u64 GetCNTPCT() override { return 0; }
};

enum Op { Add, Clr, Eor, Set, Swp, Cas };
const char* const op_names[] = {"ldadd", "ldclr", "ldeor", "ldset", "swp", "cas"};
const char* const order_names[] = {"", "a", "l", "al"};

// size: 0..3 for 1/2/4/8 bytes. order: bit 0 acquire, bit 1 release.
u32 lse(Op op, unsigned size, unsigned order) {
  const bool acquire = order & 1, release = order & 2;
  if (op == Cas)  // cas s0, s1, [x2]
    return 0x08a07c41u | size << 30 | (acquire ? 1u << 22 : 0) | (release ? 1u << 15 : 0);
  const u32 o3opc = op == Swp ? 8 : static_cast<u32>(op);  // ld<op>/swp s0, s0, [x1]
  return 0x38200020u | size << 30 | (acquire ? 1u << 23 : 0) | (release ? 1u << 22 : 0) | o3opc << 12;
}
u32 fallback_first(Op op, unsigned size) {
  if (op == Cas && size == 0) return 0x53001c10;  // uxtb w16, w0
  if (op == Cas && size == 1) return 0x53003c10;  // uxth w16, w0
  return size == 3 ? 0xaa0003f0 : 0x2a0003f0;     // mov x16, x0 / mov w16, w0
}

// Caller at code[0], helper at code[kHelper], LSE flag byte at word kFlag.
constexpr size_t kHelper = 16, kFlag = 1100, kWords = 1200;
struct Layout {
  std::vector<u32> code = std::vector<u32>(kWords, nop);
  u64 at(size_t index) const { return reinterpret_cast<std::uintptr_t>(code.data() + index); }
  void build(u32 lse_word, u32 fallback, bool bti) {
    code[0] = 0x94000000u | static_cast<u32>(kHelper);  // bl helper
    code[1] = add_x7;
    code[2] = svc0;
    size_t h = kHelper;
    if (bti) code[h++] = bti_c;
    const u64 flag = at(kFlag);
    const s64 pages = static_cast<s64>(flag >> 12) - static_cast<s64>(at(h) >> 12);
    code[h++] = 0x90000010u | static_cast<u32>(pages & 3) << 29 | static_cast<u32>((pages >> 2) & 0x7ffff) << 5;
    code[h++] = 0x39400210u | static_cast<u32>(flag & 0xfff) << 10;
    code[h++] = cbz_w16;
    code[h++] = lse_word;
    code[h++] = ret;
    code[h++] = fallback;
    code[h++] = brk0;
    std::memset(code.data() + kFlag, 0, 4);
    std::memset(reinterpret_cast<unsigned char*>(code.data() + kFlag) + (flag & 3), 1, 1);
  }
  u64 lse_address(bool bti) const { return at(kHelper + (bti ? 4 : 3)); }
};

struct State { std::array<u64, 31> x{}; u64 sp = 0, pc = 0; u32 nzcv = 0; u64 memory = 0; HaltReason halt{}; };

State run(Cpu& cpu, const Layout& layout, Op op, u64 a, u64 b, u64* word, u64 address) {
  auto& jit = *cpu.jit;
  jit.ClearHalt(HaltReason::UserDefined1); jit.ClearHalt(HaltReason::MemoryAbort);
  for (size_t r = 0; r < 31; ++r) jit.SetRegister(r, 0x5a5a000000000000ull | r);
  jit.SetRegister(16, x16_in);
  jit.SetPstate(0x60000000);
  if (op == Cas) { jit.SetRegister(0, a); jit.SetRegister(1, b); jit.SetRegister(2, address); }
  else { jit.SetRegister(0, a); jit.SetRegister(1, address); }
  jit.SetPC(layout.at(0));
  State s;
  s.halt = jit.Run();
  for (size_t r = 0; r < 31; ++r) s.x[r] = jit.GetRegister(r);
  s.sp = jit.GetSP(); s.pc = jit.GetPC(); s.nzcv = jit.GetPstate(); s.memory = word ? *word : 0;
  return s;
}

std::string diff(const State& a, const State& b) {
  for (size_t r = 0; r < 31; ++r)
    if (r != 15 && r != 16 && r != 17 && a.x[r] != b.x[r]) return "x" + std::to_string(r) + " " + hex(a.x[r]) + " vs " + hex(b.x[r]);
  if (a.sp != b.sp) return "sp";
  if (a.pc != b.pc) return "pc " + hex(a.pc) + " vs " + hex(b.pc);
  if (a.nzcv != b.nzcv) return "nzcv";
  if (a.memory != b.memory) return "memory " + hex(a.memory) + " vs " + hex(b.memory);
  return {};
}

unsigned equivalence() {
  unsigned checks = 0;
  const u64 values[] = {0x0123456789abcdefull, 0xff00ff00f0f0f0f0ull};
  for (unsigned op = Add; op <= Cas; ++op) for (unsigned size = 0; size < 4; ++size) for (unsigned order = 0; order < 4; ++order)
  for (const bool bti : {false, true}) for (const bool cas_hits : {false, true}) {
    if (op != Cas && cas_hits) continue;
    const std::string what = std::string(op_names[op]) + order_names[order] + " size " + std::to_string(1u << size) +
                             (bti ? " bti" : "") + (op == Cas ? (cas_hits ? " hit" : " miss") : "");
    Layout layout;
    layout.build(lse(static_cast<Op>(op), size, order), fallback_first(static_cast<Op>(op), size), bti);
    ExclusiveMonitor m_call(1), m_inline(1);
    Cpu call(m_call, false), inlined(m_inline, true);
    for (int pass = 0; pass < 2; ++pass) {  // compile, then the cached block
      const u64 initial = values[pass];
      const u64 a = op == Cas ? (cas_hits ? initial : ~initial) : values[1 - pass];
      u64 word = initial;  // one word, so both runs see the same address
      const auto c = run(call, layout, static_cast<Op>(op), a, 0x3141592653589793ull, &word, reinterpret_cast<std::uintptr_t>(&word));
      word = initial;
      const auto i = run(inlined, layout, static_cast<Op>(op), a, 0x3141592653589793ull, &word, reinterpret_cast<std::uintptr_t>(&word));
      require(Has(c.halt, HaltReason::UserDefined1) && Has(i.halt, HaltReason::UserDefined1), what + ": halt");
      require(c.x[30] == layout.at(1) && i.x[30] == layout.at(1), what + ": x30");
      require(c.x[16] == 1, what + ": the called helper did not take its LSE path");
      require(i.x[16] == x16_in, what + ": not inlined (x16 " + hex(i.x[16]) + ")");
      require(c.memory != initial || op == Cas || a == 0, what + ": no effect");
      if (const auto d = diff(c, i); !d.empty()) throw std::runtime_error(what + " pass " + std::to_string(pass) + ": " + d);
      ++checks;
    }
    // An abort at the access stops at the helper's LSE instruction, X30 already set.
    for (auto* cpu : {&call, &inlined}) {
      const auto s = run(*cpu, layout, static_cast<Op>(op), 1, 2, nullptr, abort_at);
      require(Has(s.halt, HaltReason::MemoryAbort), what + ": abort halt");
      require(s.pc == layout.lse_address(bti), what + ": abort pc " + hex(s.pc));
      require(s.x[30] == layout.at(1), what + ": abort x30");
      require(s.x[0] == 1, what + ": abort x0");
      ++checks;
    }
  }
  return checks;
}

// Rewriting the inlined helper and invalidating its range recompiles the caller.
unsigned invalidation() {
  Layout layout;
  layout.build(lse(Add, 3, 3), fallback_first(Add, 3), true);
  ExclusiveMonitor monitor(1);
  Cpu cpu(monitor, true);
  cpu.hints_as_nop = false;  // a reader that returns BTI as is
  u64 word = 0xf0;
  const auto first = run(cpu, layout, Add, 0x0f, 0, &word, reinterpret_cast<std::uintptr_t>(&word));
  require(word == 0xff, "invalidation: ldadd " + hex(word));
  require(first.x[16] == x16_in, "raw BTI: not inlined");
  layout.code[kHelper + 4] = lse(Eor, 3, 3);
  cpu.jit->InvalidateCacheRange(layout.at(kHelper + 4), 4);
  word = 0xff;
  const auto s = run(cpu, layout, Eor, 0x0f, 0, &word, reinterpret_cast<std::uintptr_t>(&word));
  require(s.x[16] == x16_in, "invalidation: not inlined after the rewrite");
  require(word == 0xf0 && s.x[0] == 0xff, "invalidation: stale caller block (memory " + hex(word) + ")");
  return 1;
}

// Bodies that differ from lse.S stay ordinary calls.
unsigned near_misses() {
  unsigned checks = 0;
  struct Miss { const char* name; int index; u32 word; };
  const Miss misses[] = {
      {"fallback", 6, nop},                 // no MOV x16 after RET
      {"no ret", 5, nop},                   // the LSE instruction falls through
      {"other register", 4, 0xf8e00022},    // ldaddal x0, x2, [x1]
      {"smax", 4, 0xf8e04020},              // ldsmaxal: not an lse.S helper
  };
  for (const auto& miss : misses) {
    Layout layout;
    layout.build(lse(Add, 3, 3), fallback_first(Add, 3), true);
    layout.code[kHelper + miss.index] = miss.word;
    if (miss.index == 5) layout.code[kHelper + 6] = ret;  // keep a return after the fallthrough
    ExclusiveMonitor monitor(1);
    Cpu cpu(monitor, true);
    u64 word = 5;
    const auto s = run(cpu, layout, Add, 1, 0, &word, reinterpret_cast<std::uintptr_t>(&word));
    require(Has(s.halt, HaltReason::UserDefined1), std::string("near miss ") + miss.name + ": halt");
    require(s.x[16] == 1, std::string("near miss ") + miss.name + ": inlined");
    ++checks;
  }
  return checks;
}
}  // namespace

int main() {
  try {
    const unsigned checks = equivalence() + invalidation() + near_misses();
    std::printf("jit_outlined_atomics: %u checks passed\n", checks);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "jit_outlined_atomics: %s\n", e.what());
    return 1;
  }
}
