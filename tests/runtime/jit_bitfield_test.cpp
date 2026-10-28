// SPDX-License-Identifier: GPL-3.0-or-later
// Every SBFM and UBFM encoding (32- and 64-bit, all immr/imms) and every valid
// 64-bit AND (immediate) against the architecture's DecodeBitMasks/ROR model,
// over edge and patterned inputs. The JIT translates the bitfield moves as
// shift-and-mask (LSL, LSR, ASR, UBFX, SBFX, UBFIZ, SBFIZ, UXT*, SXT*) and
// emits low-word AND masks as 32-bit operations, so each form is checked with
// the source register both overwritten and kept live for a second use.
// Word zero-extensions (UXTW, FP loads, FMOV from W) of 32-bit loads and W
// reads run with dirty upper register and vector bits.
#include <dynarmic/interface/A64/a64.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace {
using namespace Dynarmic;

std::uint64_t address(const void* p) { return reinterpret_cast<std::uintptr_t>(p); }

std::uint64_t ones(unsigned n) { return n >= 64 ? ~std::uint64_t(0) : (std::uint64_t(1) << n) - 1; }
std::uint64_t ror(std::uint64_t x, unsigned r, unsigned size) {
  r %= size;
  x &= ones(size);
  return r ? ((x >> r) | (x << (size - r))) & ones(size) : x;
}
std::uint64_t replicate(std::uint64_t element, unsigned esize, unsigned size) {
  std::uint64_t result = 0;
  for (unsigned i = 0; i < size; i += esize) result |= element << i;
  return result;
}

struct Masks { std::uint64_t wmask, tmask; };
// DecodeBitMasks from the Arm ARM.
std::optional<Masks> decode_bit_masks(unsigned n, unsigned imms, unsigned immr, bool immediate, unsigned size) {
  const unsigned combined = n << 6 | (~imms & 0x3f);
  if (!combined) return std::nullopt;
  const unsigned len = 31 - std::countl_zero(combined);
  if (len < 1) return std::nullopt;
  const unsigned levels = unsigned(ones(len));
  if (immediate && (imms & levels) == levels) return std::nullopt;
  const unsigned s = imms & levels, r = immr & levels;
  const unsigned esize = 1u << len;
  const unsigned d = (s - r) & (esize - 1);
  const std::uint64_t welem = ones(s + 1), telem = ones(d + 1);
  return Masks{replicate(ror(welem, r, esize), esize, size), replicate(telem, esize, size)};
}

// SBFM (signed) / UBFM from the Arm ARM; results are register-width, W zero-extended.
std::uint64_t bitfield(bool sign, unsigned size, unsigned immr, unsigned imms, std::uint64_t src) {
  const auto m = *decode_bit_masks(size == 64, imms, immr, false, size);
  const std::uint64_t bot = ror(src, immr, size) & m.wmask;
  if (!sign) return bot & m.tmask;
  const std::uint64_t top = (src >> imms) & 1 ? ones(size) : 0;
  return ((top & ~m.tmask) | (bot & m.tmask)) & ones(size);
}

class Cpu final : private A64::UserCallbacks {
 public:
  explicit Cpu(bool register_cache) {
    A64::UserConfig c;
    c.callbacks = this;
    c.enable_cycle_counting = false;
    c.check_halt_on_memory_access = true;
    c.fastmem_pointer = 0; c.fastmem_address_space_bits = 64; c.silently_mirror_fastmem = false;
    c.enable_register_cache = register_cache;
    jit = std::make_unique<A64::Jit>(c);
  }
  void load(std::initializer_list<std::uint32_t> words) {
    // Give each program a stable address instead of flushing every translated
    // block. Cache invalidation has dedicated tests; this matrix checks opcodes.
    auto& block = code.emplace_back();
    block.fill(0xd4000001);  // svc #0
    std::copy(words.begin(), words.end(), block.begin());
  }
  void run(std::uint64_t x0, std::uint64_t x6 = 0x5a5a5a5a00000006) {
    jit->ClearHalt(HaltReason::UserDefined1);
    for (std::size_t i = 0; i < 8; ++i) jit->SetRegister(i, 0x5a5a5a5a00000000 | i);
    for (std::size_t i = 0; i < 4; ++i) jit->SetVector(i, {~std::uint64_t(0), ~std::uint64_t(0)});
    jit->SetRegister(0, x0);
    jit->SetRegister(6, x6);
    jit->SetRegister(8, x0);
    jit->SetPC(address(code.back().data()));
    jit->Run();
  }
  std::unique_ptr<A64::Jit> jit;

 private:
  std::deque<std::array<std::uint32_t, 8>> code;
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
std::string hex(std::uint64_t v) { char b[24]; std::snprintf(b, sizeof(b), "%#llx", static_cast<unsigned long long>(v)); return b; }
}  // namespace

int main(int argc, char** argv) {
  try {
    // Each cache mode is an independent exhaustive matrix; no arguments runs both.
    const std::string mode = argc == 2 ? argv[1] : "all";
    require(argc <= 2 && (mode == "all" || mode == "cache" || mode == "uncached"),
            "usage: jit_bitfield_test [cache|uncached]");
    constexpr std::array<std::uint64_t, 10> inputs{
        0, 1, 0xffffffffffffffff, 0x8000000000000000, 0x7fffffffffffffff, 0x0000000080000000,
        0x00000000ffffffff, 0x0123456789abcdef, 0xfedcba9876543210, 0xa5a5a5a5c3c3c3c3};
    constexpr std::uint32_t svc = 0xd4000001;
    unsigned cases = 0;
    for (const bool cache : {true, false}) {
      if ((mode == "cache" && !cache) || (mode == "uncached" && cache)) continue;
      Cpu cpu(cache);
      for (const unsigned size : {32u, 64u}) {
        const std::uint32_t sf = size == 64 ? 1u : 0u;
        for (const bool sign : {true, false}) {
          for (unsigned immr = 0; immr < size; ++immr) for (unsigned imms = 0; imms < size; ++imms) {
            const std::uint32_t op = sf << 31 | (sign ? 0u : 2u) << 29 | 0x26u << 23 | sf << 22 | immr << 16 | imms << 10;
            const std::string name = std::string(sign ? "SBFM" : "UBFM") + (size == 64 ? " x" : " w") +
                                     " immr=" + std::to_string(immr) + " imms=" + std::to_string(imms);
            // xbfm x2, x0; xbfm x0, x0 (in place); orr x3, xzr, x8 (x0's original stays in x8); svc.
            cpu.load({op | 0u << 5 | 2u, op | 0u << 5 | 0u, 0xaa0803e3u, svc});
            for (const auto x : inputs) {
              cpu.run(x);
              const auto expected = bitfield(sign, size, immr, imms, x & ones(size));
              require(cpu.jit->GetRegister(2) == expected, name + " x=" + hex(x) + " got " + hex(cpu.jit->GetRegister(2)) + " want " + hex(expected));
              require(cpu.jit->GetRegister(0) == expected, name + " in place x=" + hex(x));
              require(cpu.jit->GetRegister(3) == x, name + " source copy x=" + hex(x));
              ++cases;
            }
            // Live source: xbfm x2, x0; add x4, x0, x2; svc.
            cpu.load({op | 0u << 5 | 2u, 0x8b020004u, svc, svc});
            for (const auto x : inputs) {
              cpu.run(x);
              const auto expected = bitfield(sign, size, immr, imms, x & ones(size));
              require(cpu.jit->GetRegister(4) == x + expected, name + " live source x=" + hex(x));
              ++cases;
            }
          }
        }
      }
      // AND x2, x0, #imm and AND x0, x0, #imm for every valid 64-bit logical immediate.
      for (unsigned n = 0; n < 2; ++n) for (unsigned immr = 0; immr < 64; ++immr) for (unsigned imms = 0; imms < 64; ++imms) {
        const auto masks = decode_bit_masks(n, imms, immr, true, 64);
        if (!masks) continue;
        const std::uint32_t op = 0x92000000u | n << 22 | immr << 16 | imms << 10;
        cpu.load({op | 2u, op, 0x8b080004u, svc});
        for (const auto x : inputs) {
          cpu.run(x);
          const auto expected = x & masks->wmask;
          require(cpu.jit->GetRegister(2) == expected, "AND #" + hex(masks->wmask) + " x=" + hex(x));
          require(cpu.jit->GetRegister(0) == expected, "AND in place #" + hex(masks->wmask) + " x=" + hex(x));
          require(cpu.jit->GetRegister(4) == expected + x, "AND live #" + hex(masks->wmask) + " x=" + hex(x));
          ++cases;
        }
      }
      // ANDS x2, x0, #0xffffffff: flags come from the 64-bit result.
      cpu.load({0xf2407c02u, svc, svc, svc});
      for (const auto x : inputs) {
        cpu.run(x);
        const std::uint64_t r = x & 0xffffffff;
        require(cpu.jit->GetRegister(2) == r, "ANDS value x=" + hex(x));
        require((cpu.jit->GetPstate() >> 28) == (r ? 0u : 4u), "ANDS flags x=" + hex(x));
        ++cases;
      }
      // Word zero-extensions of loads and W reads, the source used once and twice.
      constexpr std::array<std::array<std::uint32_t, 2>, 3> words{{{0x80000001, 0xfffffffe}, {0x7fffffff, 0}, {0x12345678, 0x9abcdef0}}};
      const auto reg = [](unsigned i) { return 0x5a5a5a5a00000000 | i; };
      for (const auto& w : words) {
        alignas(8) std::array<std::uint32_t, 2> data = w;
        const auto x0 = address(data.data());
        const std::uint64_t d0 = w[0], d1 = w[1];
        // ldr w1, [x0]; add x2, x3, w1, uxtw; add x4, x5, w1, uxtw.
        cpu.load({0xb9400001u, 0x8b214062u, 0x8b2140a4u});
        cpu.run(x0);
        require(cpu.jit->GetRegister(1) == d0 && cpu.jit->GetRegister(2) == reg(3) + d0 &&
                cpu.jit->GetRegister(4) == reg(5) + d0, "ldr w uxtw twice d=" + hex(d0));
        // ldr s0, [x0]; ldr w1, [x0, #4]; fmov s2, w1; add x2, x3, w1, uxtw.
        cpu.load({0xbd400000u, 0xb9400401u, 0x1e270022u, 0x8b214062u});
        cpu.run(x0);
        require(cpu.jit->GetVector(0) == A64::Vector{d0, 0}, "ldr s d=" + hex(d0));
        require(cpu.jit->GetVector(2) == A64::Vector{d1, 0}, "fmov s, w of a load d=" + hex(d1));
        require(cpu.jit->GetRegister(1) == d1 && cpu.jit->GetRegister(2) == reg(3) + d1, "ldr w fmov uxtw d=" + hex(d1));
        // ldp w1, w2, [x0]; add x4, x5, w2, uxtw; add x3, x3, w1, uxtw; ldp s0, s1, [x0].
        cpu.load({0x29400801u, 0x8b2240a4u, 0x8b214063u, 0x2d400400u});
        cpu.run(x0);
        require(cpu.jit->GetRegister(1) == d0 && cpu.jit->GetRegister(2) == d1 && cpu.jit->GetRegister(4) == reg(5) + d1 &&
                cpu.jit->GetRegister(3) == reg(3) + d0, "ldp w uxtw d=" + hex(d0));
        require(cpu.jit->GetVector(0) == A64::Vector{d0, 0} && cpu.jit->GetVector(1) == A64::Vector{d1, 0}, "ldp s d=" + hex(d0));
        // add x2, x3, w6, uxtw; add x4, x5, w6, uxtw; add x7, x3, w1, uxtw (W reads, upper bits set).
        const std::uint64_t x6 = 0xdeadbeef00000000 | d0;
        cpu.load({0x8b264062u, 0x8b2640a4u, 0x8b214067u});
        cpu.run(x0, x6);
        require(cpu.jit->GetRegister(2) == reg(3) + d0 && cpu.jit->GetRegister(4) == reg(5) + d0 &&
                cpu.jit->GetRegister(7) == reg(3) + 1, "w read uxtw d=" + hex(d0));
        // ubfx x2, x6, #0, #32 (uxtw); add x4, x6, x2 (source still live); fmov s3, w6.
        cpu.load({0xd3407cc2u, 0x8b0200c4u, 0x1e2700c3u});
        cpu.run(x0, x6);
        require(cpu.jit->GetRegister(2) == d0 && cpu.jit->GetRegister(4) == x6 + d0 &&
                cpu.jit->GetVector(3) == A64::Vector{d0, 0}, "uxtw live source d=" + hex(d0));
        cases += 5;
      }
    }
    std::printf("jit bitfield: ok (%u cases)\n", cases);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "jit bitfield: %s\n", e.what());
    return 1;
  }
}
