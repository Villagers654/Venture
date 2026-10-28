// SPDX-License-Identifier: GPL-3.0-or-later
// Scalar FP to integer conversions (FCVT{N,A,P,M,Z}{S,U}, every source and
// destination width) against an architectural model computed in double
// precision: round the exact input in the instruction's mode, then saturate.
// Out of range or NaN raises IOC only; an in-range result that differs from the
// input raises IXC; with FPCR.FZ a single/double denormal input is zero and
// raises IDC (FZ16, not FZ, governs half precision).
// Values are checked on every path. FPSR is checked for half-precision sources,
// which use the architectural FPToFixed; the single/double paths compute the
// value inline and do not raise IOC/IDC (a deliberate performance trade-off
// inherited from Dynarmic: it would cost a range check per conversion).
// Every configuration: register cache, AVX, optimizations, junk upper lanes.
#include <dynarmic/interface/A64/a64.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace Dynarmic;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
constexpr u64 base = 0x10000;
constexpr u32 vex_features = 0x0ff8;  // Backend::X64::HostFeature AVX, AVX2, AVX512F-AVX512VBMI
constexpr u32 fpcr_fz = 1u << 24;

struct Callbacks : A64::UserCallbacks {
  std::vector<u32> words;
  A64::Jit* jit = nullptr;
  std::optional<u32> MemoryReadCode(u64 p) override {
    if (p < base || (p - base) / 4 >= words.size() || p % 4) return {};
    return words[(p - base) / 4];
  }
  u64 MemoryRead(u64, std::size_t) override { throw std::runtime_error("unexpected read"); }
  A64::Vector MemoryRead128(u64) override { throw std::runtime_error("unexpected read128"); }
  void MemoryWrite(u64, u64, std::size_t) override { throw std::runtime_error("unexpected write"); }
  void MemoryWrite128(u64, A64::Vector) override { throw std::runtime_error("unexpected write128"); }
  void InterpreterFallback(u64, std::size_t) { throw std::runtime_error("unexpected fallback"); }
  void ExceptionRaised(u64, A64::Exception) override { throw std::runtime_error("unexpected exception"); }
  void CallSVC(u32) override { jit->HaltExecution(HaltReason::UserDefined1); }
  void AddTicks(u64) override {}
  u64 GetTicksRemaining() override { return 1000; }
  u64 MemoryAtomic(u64, std::size_t, A64::AtomicOp, u64, u64) override { throw std::runtime_error("unexpected atomic"); }
  A64::Vector MemoryAtomicCompareAndSwap128(u64, A64::Vector, A64::Vector) override { throw std::runtime_error("unexpected atomic128"); }
  u64 GetCNTPCT() override { return 0; }
};

struct Cpu {
  Callbacks cb;
  A64::Jit jit;
  static A64::UserConfig configuration(Callbacks& cb, bool passes, bool cache, bool avx) {
    A64::UserConfig config;
    config.callbacks = &cb;
    config.enable_cycle_counting = false;
    config.enable_register_cache = cache;
    if (cache) { config.fastmem_pointer = 0; config.fastmem_address_space_bits = 64; config.silently_mirror_fastmem = false; }
    if (!avx) config.disabled_host_features = vex_features;
    config.code_cache_size = 16 * 1024 * 1024;
    config.optimizations = passes ? all_safe_optimizations : no_optimizations;
    return config;
  }
  Cpu(bool passes, bool cache, bool avx) : jit(configuration(cb, passes, cache, avx)) { cb.jit = &jit; }
};

enum class Mode { N, A, P, M, Z };
struct Op { const char* name; Mode mode; u32 rmode, opcode; };
constexpr Op ops[] = {
    {"fcvtn", Mode::N, 0, 0}, {"fcvta", Mode::A, 0, 4}, {"fcvtp", Mode::P, 1, 0},
    {"fcvtm", Mode::M, 2, 0}, {"fcvtz", Mode::Z, 3, 0},
};

double round_mode(double v, Mode mode) {
  switch (mode) {
  case Mode::N: return std::nearbyint(v);  // default host mode: ties to even
  case Mode::A: return std::round(v);
  case Mode::P: return std::ceil(v);
  case Mode::M: return std::floor(v);
  case Mode::Z: return std::trunc(v);
  }
  return v;
}

struct Expected { u64 value; u32 fpsr; };

Expected model(double v, bool denormal, bool fz, Mode mode, bool unsigned_, unsigned ibits) {
  constexpr u32 ioc = 1, ixc = 0x10, idc = 0x80;
  const u64 max = unsigned_ ? (ibits == 64 ? ~u64(0) : (u64(1) << ibits) - 1) : (u64(1) << (ibits - 1)) - 1;
  const u64 min = unsigned_ ? 0 : (u64(1) << (ibits - 1));  // as bits, masked below
  if (std::isnan(v)) return {0, ioc};
  if (denormal && fz) return {0, idc};
  const double r = round_mode(v, mode);
  const double hi = unsigned_ ? std::ldexp(1.0, int(ibits)) : std::ldexp(1.0, int(ibits) - 1);
  const double lo = unsigned_ ? 0.0 : -std::ldexp(1.0, int(ibits) - 1);
  if (r >= hi) return {max, ioc};
  if (r < lo) return {min, ioc};
  u64 bits;
  if (r < 0) bits = u64(0) - u64(-r);
  else bits = u64(r);
  if (ibits == 32) bits &= 0xffffffffu;
  return {bits, r != v ? ixc : 0u};
}

double half_value(u32 h) {
  const int exp = int((h >> 10) & 0x1f);
  const double m = double(h & 0x3ff);
  const double mag = exp == 0 ? std::ldexp(m, -24) : exp == 31 ? (m != 0 ? NAN : INFINITY) : std::ldexp(1024 + m, exp - 25);
  return (h & 0x8000) ? -mag : mag;
}

std::vector<u64> inputs(int ftype) {
  if (ftype == 3) {
    std::vector<u64> out;
    for (u32 h = 0; h < 0x10000; h += 5) out.push_back(h);
    for (u32 h : {0x3400u, 0xb400u, 0x3800u, 0xb800u, 0x3c00u, 0xbc00u, 0x3e00u, 0xbe00u, 0x4100u, 0xc100u,
                  0x7bffu, 0xfbffu, 0x7c00u, 0xfc00u, 0x7e01u, 0xfd00u, 0x0001u, 0x8001u, 0x3bffu, 0xbbffu})
      out.push_back(h);
    return out;
  }
  const bool dbl = ftype == 1;
  std::vector<double> vals = {0.0, -0.0, 0.3, -0.3, 0.5, -0.5, 0.7, -0.7, 1.0, -1.0, 1.5, -1.5, 2.5, -2.5,
                              -0.999999, 0.999999, 1e-30, -1e-30, 123456.75, -123456.75,
                              2147483647.0, 2147483648.0, 2147483647.5, -2147483648.0, -2147483648.5, -2147483649.0,
                              4294967295.0, 4294967295.5, 4294967296.0, 9.2233720368547758e18, -9.2233720368547758e18,
                              1.8446744073709552e19, 3e19, -3e19, INFINITY, -INFINITY};
  std::vector<u64> out;
  for (double d : vals) {
    if (dbl) { u64 b; std::memcpy(&b, &d, 8); out.push_back(b); }
    else { const float f = float(d); u32 b; std::memcpy(&b, &f, 4); out.push_back(b); }
  }
  // NaNs, denormals and the extremes of both signs.
  if (dbl) {
    for (u64 b : {0x7ff8000000000001ull, 0xfff4000000000000ull, 0x0000000000000001ull, 0x800fffffffffffffull,
                  0x7fefffffffffffffull, 0xffefffffffffffffull, 0x0010000000000000ull, 0x8010000000000000ull})
      out.push_back(b);
  } else {
    for (u32 b : {0x7fc00001u, 0xffa00000u, 0x00000001u, 0x807fffffu, 0x7f7fffffu, 0xff7fffffu, 0x00800000u, 0x80800000u})
      out.push_back(b);
  }
  std::mt19937_64 rng(dbl ? 0xd0b1e : 0xf10a7);
  for (int i = 0; i < 400; ++i) {
    // Values clustered near the integer boundaries and the range limits.
    const int e = int(rng() % 70) - 4;
    double d = std::ldexp(double(rng() % 4096) / 1024.0, e) * ((rng() & 1) ? -1 : 1);
    if (rng() % 4 == 0) d = std::floor(d) + double(rng() % 3) * 0.5;
    if (dbl) { u64 b; std::memcpy(&b, &d, 8); out.push_back(b); }
    else { const float f = float(d); u32 b; std::memcpy(&b, &f, 4); out.push_back(b); }
  }
  return out;
}

unsigned failures = 0;

unsigned run(Cpu& cpu) {
  unsigned cases = 0;
  auto& jit = cpu.jit;
  for (int ftype : {0, 1, 3}) {
    const bool dbl = ftype == 1, half = ftype == 3;
    const auto values = inputs(ftype);
    for (const Op& op : ops) for (bool unsigned_ : {false, true}) for (unsigned ibits : {32u, 64u}) {
      const u32 insn = 0x1e200000 | (ibits == 64 ? 1u << 31 : 0) | (u32(ftype) << 22) | (op.rmode << 19) |
                       ((op.opcode | (unsigned_ ? 1 : 0)) << 16) | (0 << 5) | 0;
      cpu.cb.words = {insn, 0xd4000021};
      jit.ClearCache();
      for (bool fz : {false, true}) for (u64 bits : values) {
        double v;
        bool denormal;
        if (half) { v = half_value(u32(bits)); denormal = false; }
        else if (dbl) { std::memcpy(&v, &bits, 8); denormal = (bits & 0x7ff0000000000000ull) == 0 && (bits << 1) != 0; }
        else { u32 b = u32(bits); float f; std::memcpy(&f, &b, 4); v = f; denormal = (b & 0x7f800000u) == 0 && (b << 1) != 0; }
        const Expected e = model(v, denormal, fz, op.mode, unsigned_, ibits);
        jit.SetRegister(0, 0xfedcba9876543210ull);
        // Junk in every bit above the source width.
        jit.SetVector(0, {dbl ? bits : half ? (0x55aa55aa55aa0000ull | bits) : (0x55aa55aa00000000ull | bits), 0x0123456789abcdefull});
        jit.SetFpcr(fz ? fpcr_fz : 0);
        jit.SetFpsr(0);
        jit.SetPC(base);
        jit.ClearHalt(HaltReason::UserDefined1);
        jit.Run();
        const u64 got = jit.GetRegister(0);
        const u32 fpsr = jit.GetFpsr() & 0x9f;
        if (got != e.value || (half && fpsr != e.fpsr)) {
          char msg[256];
          std::snprintf(msg, sizeof msg, "%s%c %c<-%c input=%#llx fz=%d: got %#llx fpsr %#x, expected %#llx fpsr %#x",
                        op.name, unsigned_ ? 'u' : 's', ibits == 64 ? 'x' : 'w', dbl ? 'd' : half ? 'h' : 's',
                        (unsigned long long)bits, int(fz), (unsigned long long)got, fpsr,
                        (unsigned long long)e.value, e.fpsr);
          if (++failures <= 40) std::fprintf(stderr, "%s\n", msg);
        }
        ++cases;
      }
    }
  }
  return cases;
}
}  // namespace

int main() {
  try {
    unsigned cases = 0;
    for (bool passes : {false, true}) for (bool cache : {false, true}) for (bool avx : {false, true}) {
      Cpu cpu(passes, cache, avx);
      cases += run(cpu);
    }
    if (failures) {
      std::fprintf(stderr, "jit_fp_to_int: %u of %u cases failed\n", failures, cases);
      return 1;
    }
    std::printf("jit_fp_to_int: %u cases passed\n", cases);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "jit_fp_to_int: %s\n", e.what());
    return 1;
  }
}
