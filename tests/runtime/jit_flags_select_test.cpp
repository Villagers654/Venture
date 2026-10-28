// SPDX-License-Identifier: GPL-3.0-or-later
// CMP; CSEL and CMP; CCMP; CSET in one block, for every condition over edge
// operands, against a reference model. The JIT forwards the compare's flags
// to the selects in registers instead of reloading the stored guest flags,
// and selects directly on host FLAGS while they still hold the compare's
// result: several selects after one compare, immediate operands (CSET) and
// CSINC's add between selects must all keep them intact.
#include <dynarmic/interface/A64/a64.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace {
using namespace Dynarmic;

std::uint64_t address(const void* p) { return reinterpret_cast<std::uintptr_t>(p); }

// NZCV (bits 3..0) of a - b, as SUBS computes it.
unsigned subs_flags(std::uint64_t a, std::uint64_t b) {
  const std::uint64_t r = a - b;
  const unsigned n = r >> 63, z = r == 0, c = a >= b;
  const unsigned v = (((a ^ b) & (a ^ r)) >> 63) & 1;
  return n << 3 | z << 2 | c << 1 | v;
}
unsigned subs_flags32(std::uint32_t a, std::uint32_t b) {
  const std::uint32_t r = a - b;
  const unsigned n = r >> 31, z = r == 0, c = a >= b;
  const unsigned v = (((a ^ b) & (a ^ r)) >> 31) & 1;
  return n << 3 | z << 2 | c << 1 | v;
}
bool holds(unsigned cond, unsigned f) {
  const bool n = f & 8, z = f & 4, c = f & 2, v = f & 1;
  bool r = false;
  switch (cond >> 1) {
  case 0: r = z; break;
  case 1: r = c; break;
  case 2: r = n; break;
  case 3: r = v; break;
  case 4: r = c && !z; break;
  case 5: r = n == v; break;
  case 6: r = n == v && !z; break;
  case 7: r = true; break;
  }
  return (cond & 1) && cond != 15 ? !r : r;
}

class Cpu final : private A64::UserCallbacks {
 public:
  Cpu() {
    A64::UserConfig c;
    c.callbacks = this;
    c.enable_cycle_counting = false;
    c.check_halt_on_memory_access = true;
    c.fastmem_pointer = 0; c.fastmem_address_space_bits = 64; c.silently_mirror_fastmem = false;
    c.enable_register_cache = true;
    jit = std::make_unique<A64::Jit>(c);
  }
  void run(const std::array<std::uint32_t, 5>& code, const std::array<std::uint64_t, 7>& x) {
    jit->ClearCache();
    jit->ClearHalt(HaltReason::UserDefined1);
    for (std::size_t i = 0; i < x.size(); ++i) jit->SetRegister(i, x[i]);
    jit->SetPC(address(code.data()));
    jit->Run();
  }
  std::unique_ptr<A64::Jit> jit;

 private:
  std::optional<std::uint32_t> MemoryReadCode(std::uint64_t p) override {
    std::uint32_t word; std::memcpy(&word, reinterpret_cast<void*>(p), sizeof(word)); return word;
  }
  std::uint64_t MemoryRead(std::uint64_t, std::size_t) override { throw std::runtime_error("unexpected read"); }
  void MemoryWrite(std::uint64_t, std::uint64_t, std::size_t) override { throw std::runtime_error("unexpected write"); }
  A64::Vector MemoryRead128(std::uint64_t) override { throw std::runtime_error("unexpected read128"); }
  void MemoryWrite128(std::uint64_t, A64::Vector) override { throw std::runtime_error("unexpected write128"); }
  std::uint64_t MemoryAtomic(std::uint64_t, std::size_t, A64::AtomicOp, std::uint64_t, std::uint64_t) override { throw std::runtime_error("unexpected atomic"); }
  A64::Vector MemoryAtomicCompareAndSwap128(std::uint64_t, A64::Vector, A64::Vector) override { throw std::runtime_error("unexpected CASP"); }
  void CallSVC(std::uint32_t) override { jit->HaltExecution(HaltReason::UserDefined1); }
  void ExceptionRaised(std::uint64_t, A64::Exception) override { throw std::runtime_error("unexpected exception"); }
  void AddTicks(std::uint64_t) override {}
  std::uint64_t GetTicksRemaining() override { return 1000000; }
  std::uint64_t GetCNTPCT() override { return 0; }
};

void require(bool ok, const std::string& what) { if (!ok) throw std::runtime_error(what); }
}  // namespace

int main() {
  try {
    constexpr std::array<std::uint64_t, 7> edges{0, 1, 2, 0x7fffffffffffffff, 0x8000000000000000, 0xffffffffffffffff, 0x123456789};
    constexpr std::uint32_t cmp = 0xeb01001f, svc = 0xd4000001, nop = 0xd503201f;
    Cpu cpu;
    unsigned cases = 0;
    for (const auto a : edges) for (const auto b : edges) {
      const unsigned flags = subs_flags(a, b);
      for (unsigned cond = 0; cond < 16; ++cond) {
        // CMP x0,x1; CSEL x2,x3,x4,cond.
        cpu.run({cmp, 0x9a840062 | cond << 12, svc, nop, nop}, {a, b, 0, 33, 44, 0, 0});
        require(cpu.jit->GetRegister(2) == (holds(cond, flags) ? 33u : 44u), "csel cond " + std::to_string(cond));
        require((cpu.jit->GetPstate() >> 28) == flags, "csel left guest flags");
        // CMP x0,x1; CCMP x5,x6,#0b1010,cond; CSET x2,cond2 (CSINC x2,xzr,xzr,!cond2).
        const unsigned cond2 = (cond * 7 + 3) % 14;
        const auto c5 = edges[(cond + 1) % edges.size()], c6 = edges[(cond + 4) % edges.size()];
        cpu.run({cmp, 0xfa4600aa | cond << 12, 0x9a9f07e2 | (cond2 ^ 1) << 12, svc, nop}, {a, b, 0, 0, 0, c5, c6});
        const unsigned after = holds(cond, flags) ? subs_flags(c5, c6) : 0b1010;
        require(cpu.jit->GetRegister(2) == (holds(cond2, after) ? 1u : 0u), "ccmp cond " + std::to_string(cond));
        require((cpu.jit->GetPstate() >> 28) == after, "ccmp guest flags");
        // CMP x0,x1; CSEL x2,x3,x4,cond; CSET x5,cond2; CSEL x6,x4,x3,cond3.
        const unsigned cond3 = (cond * 5 + 1) % 16;
        cpu.run({cmp, 0x9a840062 | cond << 12, 0x9a9f07e5 | (cond2 ^ 1) << 12, 0x9a830086 | cond3 << 12, svc},
                {a, b, 0, 33, 44, 0, 0});
        require(cpu.jit->GetRegister(2) == (holds(cond, flags) ? 33u : 44u), "multi csel cond " + std::to_string(cond));
        require(cpu.jit->GetRegister(5) == (holds(cond2, flags) ? 1u : 0u), "multi cset cond " + std::to_string(cond2));
        require(cpu.jit->GetRegister(6) == (holds(cond3, flags) ? 44u : 33u), "multi csel cond " + std::to_string(cond3));
        require((cpu.jit->GetPstate() >> 28) == flags, "multi left guest flags");
        // CMP w0,w1; CSEL w2,w3,w4,cond; CSINC x5,x3,x4,cond3 (adds between selects).
        const unsigned flags32 = subs_flags32(std::uint32_t(a), std::uint32_t(b));
        const std::uint64_t x3 = 0xfffffffe00000021, x4 = 0x100000002c;
        cpu.run({0x6b01001f, 0x1a840062 | cond << 12, 0x9a840465 | cond3 << 12, svc, nop}, {a, b, 0, x3, x4, 0, 0});
        require(cpu.jit->GetRegister(2) == (holds(cond, flags32) ? std::uint32_t(x3) : std::uint32_t(x4)), "w csel cond " + std::to_string(cond));
        require(cpu.jit->GetRegister(5) == (holds(cond3, flags32) ? x3 : x4 + 1), "csinc cond " + std::to_string(cond3));
        require((cpu.jit->GetPstate() >> 28) == flags32, "w guest flags");
        cases += 4;
      }
    }
    std::printf("jit flags select: ok (%u cases)\n", cases);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "jit flags select: %s\n", e.what());
    return 1;
  }
}
