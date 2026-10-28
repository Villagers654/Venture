#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <array>
#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <vector>
#include "fp_round_instructions.h"

namespace {
struct Format {
  unsigned bits, fraction, bias;
  std::uint64_t sign, fraction_mask, exponent_mask, quiet;
  constexpr explicit Format(unsigned n) : bits(n), fraction(n == 16 ? 10 : n == 32 ? 23 : 52),
      bias(n == 16 ? 15 : n == 32 ? 127 : 1023), sign(1ull << (n - 1)),
      fraction_mask((1ull << fraction) - 1), exponent_mask(sign - 1 - fraction_mask),
      quiet(1ull << (fraction - 1)) {}
  constexpr std::uint64_t Integer(std::uint64_t n) const {
    if (!n) return 0;
    const auto top = std::bit_width(n) - 1;
    return (std::uint64_t(top + bias) << fraction) | ((n - (1ull << top)) << (fraction - top));
  }
};
struct Expected { std::uint64_t bits; std::uint32_t flags; };

// Independent unsigned-integer oracle. Split the IEEE significand at the
// binary point and compare the discarded bits with exactly one half. It uses
// neither host FP rounding nor Dynarmic's software FP implementation.
constexpr Expected Reference(const Format& f, std::uint64_t raw, unsigned mode, bool exact,
                   bool standard = false, bool flush = false) {
  const auto sign = raw & f.sign, magnitude = raw & (f.sign - 1);
  const auto exponent = (raw & f.exponent_mask) >> f.fraction;
  const auto fraction = raw & f.fraction_mask;
  if ((raw & f.exponent_mask) == f.exponent_mask) {
    if (!fraction) return {raw, 0};
    return {standard ? f.exponent_mask | f.quiet : raw | f.quiet,
            fraction & f.quiet ? 0u : 1u};
  }
  if (!magnitude) return {raw, 0};
  if (!exponent && (standard || flush)) return {sign, 128}; // A32 standard FPCR flushes input.
  const auto significand = fraction | (exponent ? 1ull << f.fraction : 0);
  const int power = int(exponent ? exponent : 1) - int(f.bias);
  if (power >= int(f.fraction)) return {raw, 0};
  const auto shift = unsigned(int(f.fraction) - power);
  auto integer = shift >= 64 ? 0 : significand >> shift;
  const auto discarded = shift >= 64 ? significand : significand & ((1ull << shift) - 1);
  if (!discarded) return {raw, 0};
  const bool over_half = shift < 64 && discarded > (1ull << (shift - 1));
  const bool half = shift < 64 && discarded == (1ull << (shift - 1));
  bool increment = false;
  switch (mode) {
    case 0: increment = over_half || (half && (integer & 1)); break;
    case 1: increment = !sign; break;
    case 2: increment = sign != 0; break;
    case 3: break;
    case 4: increment = over_half || half; break;
    default: throw std::runtime_error("invalid reference rounding mode");
  }
  return {sign | f.Integer(integer + increment), exact ? 16u : 0u};
}

static_assert(Format(16).Integer(1) == 0x3c00);
static_assert(Format(32).Integer(1) == 0x3f800000);
static_assert(Format(64).Integer(1) == 0x3ff0000000000000);
static_assert(Reference(Format(64), 0x4002000000000000, 0, true).bits == 0x4000000000000000);
static_assert(Reference(Format(64), 0x4002000000000000, 0, true).flags == 16);

std::vector<std::uint64_t> Inputs(const Format& f) {
  std::vector<std::uint64_t> positive{0, 1, f.fraction_mask,
      1ull << f.fraction, f.exponent_mask - 1, f.exponent_mask,
      f.exponent_mask | f.quiet | 7, f.exponent_mask | 7};
  // Exact quarters, both parities of ties, values adjacent to 0.5 and 1.0.
  for (std::uint64_t quarters : {1, 2, 3, 4, 5, 6, 7, 9, 10, 14, 31}) {
    const auto top = std::bit_width(quarters) - 1;
    positive.push_back((std::uint64_t(top + f.bias - 2) << f.fraction) |
                       ((quarters - (1ull << top)) << (f.fraction - top)));
  }
  const auto half = std::uint64_t(f.bias - 1) << f.fraction;
  const auto one = std::uint64_t(f.bias) << f.fraction;
  for (auto v : {half - 1, half + 1, one - 1, one + 1}) positive.push_back(v);
  auto result = positive;
  for (auto v : positive) result.push_back(v | f.sign);
  return result;
}

class Callbacks final : public Dynarmic::A64::UserCallbacks, public Dynarmic::A32::UserCallbacks {
 public:
  std::vector<std::uint32_t> code;
  Dynarmic::A64::Jit* a64 = nullptr;
  Dynarmic::A32::Jit* a32 = nullptr;
  std::optional<std::uint32_t> Fetch(std::uint64_t pc) {
    return pc >= 0x1000 && (pc - 0x1000) / 4 < code.size()
        ? std::optional(code.at((pc - 0x1000) / 4)) : std::nullopt;
  }
  std::optional<std::uint32_t> MemoryReadCode(std::uint64_t pc) override { return Fetch(pc); }
  std::optional<std::uint32_t> MemoryReadCode(std::uint32_t pc) override { return Fetch(pc); }
  std::uint64_t MemoryRead(std::uint64_t p, std::size_t n) override { throw std::runtime_error("unexpected data read"); }
  std::uint64_t MemoryRead(std::uint32_t p, std::size_t n) override { throw std::runtime_error("unexpected data read"); }
  void MemoryWrite(std::uint64_t p, std::uint64_t v, std::size_t n) override { throw std::runtime_error("unexpected data write"); }
  void MemoryWrite(std::uint32_t p, std::uint64_t v, std::size_t n) override { throw std::runtime_error("unexpected data write"); }
  Dynarmic::A64::Vector MemoryRead128(std::uint64_t) override { throw std::runtime_error("unexpected read128"); }
  void MemoryWrite128(std::uint64_t, Dynarmic::A64::Vector) override { throw std::runtime_error("unexpected write128"); }
  std::uint64_t MemoryAtomic(std::uint64_t, std::size_t, Dynarmic::A64::AtomicOp, std::uint64_t, std::uint64_t) override { throw std::runtime_error("unexpected atomic"); }
  Dynarmic::A64::Vector MemoryAtomicCompareAndSwap128(std::uint64_t, Dynarmic::A64::Vector, Dynarmic::A64::Vector) override { throw std::runtime_error("unexpected CASP"); }
  void CallSVC(std::uint32_t) override { if (a64) a64->HaltExecution(); else a32->HaltExecution(); }
  void ExceptionRaised(std::uint64_t, Dynarmic::A64::Exception exception) override {
    if (exception == Dynarmic::A64::Exception::Breakpoint) a64->HaltExecution();
    else throw std::runtime_error("A64 exception");
  }
  void ExceptionRaised(std::uint32_t, Dynarmic::A32::Exception exception) override {
    if (exception == Dynarmic::A32::Exception::Breakpoint) a32->HaltExecution();
    else throw std::runtime_error("A32 exception");
  }
  void AddTicks(std::uint64_t) override {}
  std::uint64_t GetTicksRemaining() override { return 1000000; }
  std::uint64_t GetCNTPCT() override { return 0; }
};

unsigned Mode(const RoundInstruction& inst, unsigned fpcr_mode) {
  if (inst.operation >= 5) return fpcr_mode;
  return inst.operation; // n,p,m,z,a == nearest,plus,minus,zero,away.
}
std::array<std::uint64_t, 2> Pack(const Format& f, unsigned lanes, std::uint64_t raw) {
  std::array<std::uint64_t, 2> result{};
  for (unsigned i = 0; i < lanes; ++i)
    result[i * f.bits / 64] |= raw << (i * f.bits % 64);
  return result;
}
std::array<std::uint64_t, 2> PackCase(const Format& f, unsigned lanes, std::uint64_t raw) {
  auto result = Pack(f, lanes, f.Integer(1));
  const auto offset = (lanes - 1) * f.bits;
  const auto mask = f.bits == 64 ? ~0ull : (1ull << f.bits) - 1;
  result[offset / 64] = (result[offset / 64] & ~(mask << (offset % 64))) | (raw << (offset % 64));
  return result;
}
} // namespace

TEST_CASE("A64 round integral scalar and vector H S D obey independent IEEE expectations") {
  Callbacks callbacks;
  for (const auto& inst : a64_round_instructions) {
    callbacks.code.push_back(inst.word); callbacks.code.push_back(0xd4000001);
  }
  Dynarmic::ExclusiveMonitor monitor(1);
  Dynarmic::A64::UserConfig config;
  config.callbacks = &callbacks; config.global_monitor = &monitor; config.enable_cycle_counting = false;
  // Mask only the JIT's detected SSE4.1 feature; cannot enable absent features.
  for (const auto disabled_features : {0u, 1u << 1}) {
  CAPTURE(disabled_features);
  config.disabled_host_features = disabled_features;
  Dynarmic::A64::Jit cpu(config); callbacks.a64 = &cpu;
  for (unsigned i = 0; i < std::size(a64_round_instructions); ++i) {
    const auto& inst = a64_round_instructions[i]; const Format format(inst.bits);
    for (const bool fz : {false, true}) for (unsigned mode = 0; mode < 4; ++mode) for (auto raw : Inputs(format)) {
      INFO(std::string(inst.name));
      CAPTURE(mode);
      CAPTURE(raw);
      CAPTURE(fz);
      const auto expected = Reference(format, raw, Mode(inst, mode), inst.operation == 6, false, fz && inst.bits != 16);
      cpu.SetFpcr((mode << 22) | (fz ? 1u << 24 : 0)); cpu.SetFpsr(2); // Preserve an unrelated sticky DZC.
      auto input = PackCase(format, inst.lanes, raw);
      if (inst.vector && inst.lanes * inst.bits == 64) input[1] = 0x7f80000100000001;
      cpu.SetVector(1, input); cpu.SetVector(0, {~0ull, ~0ull});
      cpu.SetPC(0x1000 + i * 8); cpu.Run();
      CHECK(cpu.GetVector(0) == PackCase(format, inst.lanes, expected.bits));
      CHECK((cpu.GetFpsr() & 0x9f) == (expected.flags | 2));
      // A non-exact operation must preserve an already-set IXC too.
      cpu.SetFpsr(16); cpu.SetPC(0x1000 + i * 8); cpu.Run();
      CHECK((cpu.GetFpsr() & 16) == 16);
    }
  }
  }
}

TEST_CASE("UserConfig aggregates retain existing positional arguments") {
  Callbacks callbacks;
  Dynarmic::A32::UserConfig arm{&callbacks};
  CHECK(arm.callbacks == &callbacks);
  CHECK(arm.disabled_host_features == 0);
  Dynarmic::A64::UserConfig a64{};
  CHECK(a64.disabled_host_features == 0);
}
