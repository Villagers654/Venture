// SPDX-License-Identifier: GPL-3.0-or-later
// Scalar FP and word codegen against architectural models, with junk in the
// upper bits of every register and vector:
// - FCMP/FCMPE + FCSEL in all four width combinations, every condition and
//   compare outcome. A single-use compare is fused into the select, which
//   blends XMM operands with a compare mask (CMOV on host flags without AVX);
//   a 64-bit blend needs a 64-bit compare mask.
// - FADD/FSUB/FMUL/FDIV with operands from S/D reads (GetQ narrowed to a
//   32/64-bit load), full Q registers, FP loads and SCVTF, plus FNEG/FABS
//   chains. The backend keeps scalar results zero above their width where it
//   can prove it, commuting FADD/FMUL to do so, and writes them back without
//   clearing the upper lanes again.
// - FMADD/FMSUB against a fused host multiply-add, with NaN operands and
//   infinity times zero.
// - W reads of a 64-bit value forwarded within the block (LeastSignificantWord
//   aliases its source register; bits 32-63 must never leak into word results).
// Every configuration: register cache, AVX, Unsafe_InaccurateNaN, FPCR.DN.
#include <dynarmic/interface/A64/a64.h>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace Dynarmic;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

constexpr u32 svc = 0xd4000001;
constexpr u32 vex_features = 0x0ff8;  // Backend::X64::HostFeature AVX, AVX2, AVX512F-AVX512VBMI
constexpr u32 fpcr_dn = 1u << 25;
constexpr u64 junk_hi = 0x8badf00ddeadbeef;

alignas(16) std::uint8_t memory[64];

u64 address(const void* p) { return reinterpret_cast<std::uintptr_t>(p); }

struct Config { bool cache, avx, inaccurate_nan, dn; };

class Cpu final : private A64::UserCallbacks {
 public:
  explicit Cpu(const Config& config) {
    A64::UserConfig c;
    c.callbacks = this;
    c.enable_cycle_counting = false;
    c.check_halt_on_memory_access = true;
    c.fastmem_pointer = 0; c.fastmem_address_space_bits = 64; c.silently_mirror_fastmem = false;
    c.enable_register_cache = config.cache;
    if (!config.avx) c.disabled_host_features = vex_features;
    if (config.inaccurate_nan) {
      c.unsafe_optimizations = true;
      c.optimizations |= OptimizationFlag::Unsafe_InaccurateNaN;
    }
    jit = std::make_unique<A64::Jit>(c);
  }
  // Each program at its own address: no cache flush between programs.
  u64 load(const std::vector<u32>& words) {
    auto& block = code.emplace_back();
    if (words.size() >= block.size()) throw std::runtime_error("program too long");
    block.fill(svc);
    std::copy(words.begin(), words.end(), block.begin());
    return address(block.data());
  }
  void reset(const Config& config) {
    for (std::size_t i = 0; i < 31; ++i) jit->SetRegister(i, 0x5a5a5a5a00000000 | i);
    for (std::size_t i = 0; i < 32; ++i) jit->SetVector(i, {0xa5a5a5a5c3c3c300 | i, junk_hi});
    jit->SetFpcr(config.dn ? fpcr_dn : 0);
    jit->SetFpsr(0);
  }
  void run(u64 pc) {
    jit->ClearHalt(HaltReason::UserDefined1);
    jit->SetPC(pc);
    jit->Run();
  }
  std::unique_ptr<A64::Jit> jit;

 private:
  std::deque<std::array<u32, 32>> code;
  std::optional<u32> MemoryReadCode(u64 p) override {
    u32 word; std::memcpy(&word, reinterpret_cast<void*>(p), sizeof(word)); return word;
  }
  u64 MemoryRead(u64, std::size_t) override { throw std::runtime_error("unexpected read"); }
  void MemoryWrite(u64, u64, std::size_t) override { throw std::runtime_error("unexpected write"); }
  A64::Vector MemoryRead128(u64) override { throw std::runtime_error("unexpected read128"); }
  void MemoryWrite128(u64, A64::Vector) override { throw std::runtime_error("unexpected write128"); }
  u64 MemoryAtomic(u64, std::size_t, A64::AtomicOp, u64, u64) override { throw std::runtime_error("unexpected atomic"); }
  A64::Vector MemoryAtomicCompareAndSwap128(u64, A64::Vector, A64::Vector) override { throw std::runtime_error("unexpected CASP"); }
  void CallSVC(u32) override { jit->HaltExecution(HaltReason::UserDefined1); }
  void ExceptionRaised(u64, A64::Exception) override { throw std::runtime_error("unexpected exception"); }
  void AddTicks(u64) override {}
  u64 GetTicksRemaining() override { return 1000000; }
  u64 GetCNTPCT() override { return 0; }
};

void require(bool ok, const std::string& what) { if (!ok) throw std::runtime_error(what); }
std::string hex(u64 v) { char b[24]; std::snprintf(b, sizeof(b), "%#llx", static_cast<unsigned long long>(v)); return b; }
std::string vec(const A64::Vector& v) { return hex(v[1]) + ":" + hex(v[0]); }

// --- encodings ----------------------------------------------------------------
u32 type_bit(unsigned size) { return size == 64 ? 0x00400000 : 0; }
u32 fcmp(unsigned size, bool signaling, unsigned n, unsigned m) { return 0x1e202000 | type_bit(size) | m << 16 | n << 5 | (signaling ? 0x10 : 0); }
u32 fcsel(unsigned size, unsigned d, unsigned n, unsigned m, unsigned cond) { return 0x1e200c00 | type_bit(size) | m << 16 | cond << 12 | n << 5 | d; }
u32 fp3(u32 base, unsigned size, unsigned d, unsigned n, unsigned m) { return base | type_bit(size) | m << 16 | n << 5 | d; }
u32 fneg(unsigned size, unsigned d, unsigned n) { return 0x1e214000 | type_bit(size) | n << 5 | d; }
u32 fabs_(unsigned size, unsigned d, unsigned n) { return 0x1e20c000 | type_bit(size) | n << 5 | d; }
u32 scvtf_w(unsigned size, unsigned d, unsigned n) { return 0x1e220000 | type_bit(size) | n << 5 | d; }
u32 ldr_fp(unsigned size, unsigned t, unsigned n) { return (size == 64 ? 0xfd400000 : 0xbd400000) | n << 5 | t; }
u32 mov16b(unsigned d, unsigned n) { return 0x4ea01c00 | n << 16 | n << 5 | d; }
constexpr u32 fmul_s = 0x1e200800, fdiv_s = 0x1e201800, fadd_s = 0x1e202800, fsub_s = 0x1e203800;
constexpr u32 cmn_x30_0 = 0xb10003df;  // adds xzr, x30, #0

// --- models -------------------------------------------------------------------
u64 mask(unsigned size, u64 v) { return size == 32 ? u64(u32(v)) : v; }
u64 f32(float f) { return std::bit_cast<u32>(f); }
u64 f64(double d) { return std::bit_cast<u64>(d); }
u64 fp(unsigned size, double d) { return size == 32 ? f32(float(d)) : f64(d); }
bool is_nan(unsigned size, u64 b) {
  return size == 32 ? (b & 0x7f800000) == 0x7f800000 && (b & 0x007fffff)
                    : (b & 0x7ff0000000000000) == 0x7ff0000000000000 && (b & 0x000fffffffffffff);
}
bool is_snan(unsigned size, u64 b) { return is_nan(size, b) && !(b & (size == 32 ? 0x00400000 : 0x0008000000000000)); }
u64 quiet(unsigned size, u64 b) { return b | (size == 32 ? 0x00400000 : 0x0008000000000000); }
u64 sign_bit(unsigned size) { return size == 32 ? 0x80000000 : 0x8000000000000000; }
u64 default_nan(unsigned size) { return size == 32 ? 0x7fc00000 : 0x7ff8000000000000; }

// A64 FCMP NZCV: unordered 0011, equal 0110, less 1000, greater 0010.
u32 fcmp_nzcv(unsigned size, u64 a, u64 b) {
  if (is_nan(size, a) || is_nan(size, b)) return 0x3;
  const double x = size == 32 ? double(std::bit_cast<float>(u32(a))) : std::bit_cast<double>(a);
  const double y = size == 32 ? double(std::bit_cast<float>(u32(b))) : std::bit_cast<double>(b);
  return x == y ? 0x6 : x < y ? 0x8 : 0x2;
}
bool condition_holds(unsigned cond, u32 nzcv) {
  const bool n = nzcv & 8, z = nzcv & 4, c = nzcv & 2, v = nzcv & 1;
  bool r = true;
  switch (cond >> 1) {
  case 0: r = z; break;
  case 1: r = c; break;
  case 2: r = n; break;
  case 3: r = v; break;
  case 4: r = c && !z; break;
  case 5: r = n == v; break;
  case 6: r = n == v && !z; break;
  default: return true;  // AL, NV
  }
  return (cond & 1) ? !r : r;
}

u64 host_op(unsigned op, unsigned size, u64 a, u64 b) {
  if (size == 32) {
    const float x = std::bit_cast<float>(u32(a)), y = std::bit_cast<float>(u32(b));
    return std::bit_cast<u32>(op == 0 ? x + y : op == 1 ? x - y : op == 2 ? x * y : x / y);
  }
  const double x = std::bit_cast<double>(a), y = std::bit_cast<double>(b);
  return std::bit_cast<u64>(op == 0 ? x + y : op == 1 ? x - y : op == 2 ? x * y : x / y);
}
// nullopt: any NaN is acceptable (InaccurateNaN).
std::optional<u64> fp_arith(unsigned op, unsigned size, u64 a, u64 b, const Config& config) {
  if (is_nan(size, a) || is_nan(size, b)) {
    if (config.inaccurate_nan) return std::nullopt;
    if (config.dn) return default_nan(size);
    if (is_snan(size, a)) return quiet(size, a);
    if (is_snan(size, b)) return quiet(size, b);
    return is_nan(size, a) ? a : b;
  }
  const u64 r = host_op(op, size, a, b);
  if (is_nan(size, r)) return config.inaccurate_nan ? std::nullopt : std::optional<u64>(default_nan(size));
  return r;
}

// A scalar in the low lane with junk above it.
A64::Vector scalar(unsigned size, u64 bits, u32 junk) {
  return {size == 32 ? u64(junk) << 32 | u32(bits) : bits, junk_hi ^ junk};
}
// A scalar result: zero above its width.
void expect_scalar(Cpu& cpu, unsigned v, unsigned size, std::optional<u64> want, const std::string& what) {
  const auto got = cpu.jit->GetVector(v);
  if (want) {
    const A64::Vector w{mask(size, *want), 0};
    require(got == w, what + ": v" + std::to_string(v) + " got " + vec(got) + " want " + vec(w));
  } else {
    require(got[1] == 0 && got[0] == mask(size, got[0]) && is_nan(size, got[0]), what + ": v" + std::to_string(v) + " got " + vec(got) + " want a NaN zero above its width");
  }
}

std::string describe(const Config& c) {
  return std::string(c.cache ? "cache" : "uncached") + (c.avx ? " avx" : " sse") + (c.inaccurate_nan ? " inaccurate-nan" : "") + (c.dn ? " DN" : "");
}

unsigned compare_select(Cpu& cpu, const Config& config) {
  unsigned checks = 0;
  const double inf = std::bit_cast<double>(u64(0x7ff0000000000000));
  for (const bool fused : {false, true})
  for (const bool signaling : {false, true})
    for (const unsigned cmp_size : {32u, 64u})
      for (const unsigned sel_size : {32u, 64u})
        for (unsigned cond = 0; cond < 14; ++cond) {
          // fcmp(e) n1, n2; fcsel d0, n3, n4, cond; fadd d5, n6, d0; mov v20.16b, v6.16b (v6 stays a full Q read).
          // Fused: cmn x30, #0 overwrites NZCV, so the select is the compare's only user.
          std::vector<u32> program{fcmp(cmp_size, signaling, 1, 2), fcsel(sel_size, 0, 3, 4, cond),
                                   fp3(fadd_s, sel_size, 5, 6, 0), mov16b(20, 6)};
          if (fused) program.push_back(cmn_x30_0);
          const u64 pc = cpu.load(program);
          const u64 qnan = cmp_size == 32 ? 0x7fc12345 : 0x7ff8000000012345;
          const u64 snan = cmp_size == 32 ? 0x7f812345 : 0x7ff0000000012345;
          const std::array<std::pair<u64, u64>, 8> pairs{{
              {fp(cmp_size, 1.0), fp(cmp_size, 2.0)}, {fp(cmp_size, 2.0), fp(cmp_size, 2.0)},
              {fp(cmp_size, 3.0), fp(cmp_size, 2.0)}, {qnan, fp(cmp_size, 2.0)},
              {fp(cmp_size, 2.0), qnan}, {snan, fp(cmp_size, 2.0)},
              {fp(cmp_size, -0.0), fp(cmp_size, 0.0)}, {fp(cmp_size, inf), fp(cmp_size, -inf)}}};
          for (const auto& [a, b] : pairs)
            for (const u32 junk : {0xffffffffu, 0u}) {
              const u64 then_ = fp(sel_size, 1.25), else_ = fp(sel_size, -4.5), addend = fp(sel_size, 10.0);
              cpu.reset(config);
              cpu.jit->SetVector(1, scalar(cmp_size, a, junk));
              cpu.jit->SetVector(2, scalar(cmp_size, b, ~junk));
              cpu.jit->SetVector(3, scalar(sel_size, then_, ~junk));
              cpu.jit->SetVector(4, scalar(sel_size, else_, junk));
              const A64::Vector v6 = scalar(sel_size, addend, junk);
              cpu.jit->SetVector(6, v6);
              cpu.run(pc);
              const std::string what = describe(config) + (fused ? " fused" : "") + (signaling ? " fcmpe " : " fcmp ") +
                                       std::to_string(cmp_size) + " fcsel " + std::to_string(sel_size) + " cond " +
                                       std::to_string(cond) + " a=" + hex(a) + " b=" + hex(b) + " junk=" + hex(junk);
              const u32 nzcv = fcmp_nzcv(cmp_size, a, b);
              const u32 final_nzcv = fused ? 0 : nzcv;  // x30 + 0: no flags
              require((cpu.jit->GetPstate() >> 28) == final_nzcv, what + ": NZCV " + hex(cpu.jit->GetPstate() >> 28) + " want " + hex(final_nzcv));
              const bool ioc = signaling ? is_nan(cmp_size, a) || is_nan(cmp_size, b) : is_snan(cmp_size, a) || is_snan(cmp_size, b);
              require(bool(cpu.jit->GetFpsr() & 1) == ioc, what + ": FPSR.IOC " + hex(cpu.jit->GetFpsr()));
              const u64 selected = condition_holds(cond, nzcv) ? then_ : else_;
              expect_scalar(cpu, 0, sel_size, selected, what + " select");
              expect_scalar(cpu, 5, sel_size, host_op(0, sel_size, addend, selected), what + " add of the select");
              require(cpu.jit->GetVector(20) == v6, what + ": full copy of v6");
              ++checks;
            }
        }
  return checks;
}

unsigned arithmetic(Cpu& cpu, const Config& config) {
  unsigned checks = 0;
  constexpr std::array<u32, 4> ops{fadd_s, fsub_s, fmul_s, fdiv_s};
  constexpr std::array<const char*, 4> op_names{"fadd", "fsub", "fmul", "fdiv"};
  // Operand sources: 1 = an S/D read only, 6 = a register also read as a full Q,
  // 7 = an FP load, 8 = SCVTF of w10.
  constexpr std::array<unsigned, 4> regs{1, 6, 7, 8};
  constexpr std::array<double, 6> values{1.5, -2.25, 1000.0, 0.1, 3.0, 7.0e-3};
  constexpr std::array<s32, 6> ints{3, -7, 100000, -1, 2, 12345};
  for (const unsigned size : {32u, 64u}) {
    // Distinct payloads: with two NaNs of the same kind the first operand's wins.
    // A QNaN first operand with an SNaN second makes the result the second
    // operand's (quieted) register, which x64 does not do by itself.
    const u64 qa = size == 32 ? 0xffc54321 : 0xfff8000000054321, qb = size == 32 ? 0x7fc0000b : 0x7ff800000000000b;
    const u64 sa = size == 32 ? 0x7f812345 : 0x7ff0000000012345, sb = size == 32 ? 0xff80000d : 0xfff000000000000d;
    // {v1, v6, memory} overrides: normal values, then NaN placements.
    struct Inputs { std::optional<u64> v1, v6, mem; };
    const std::array<Inputs, 11> nan_inputs{{{qa, {}, {}}, {{}, sa, {}}, {{}, {}, qb}, {sa, qb, {}},
                                             {qa, qb, {}}, {{}, sa, sb}, {qb, {}, qa}, {sb, sa, {}},
                                             {qa, sb, {}}, {{}, sa, qb}, {qb, {}, sa}}};
    const auto run_case = [&](u64 pc, const std::string& name, unsigned k, const Inputs& in, auto&& expect) {
      const u64 v1 = in.v1.value_or(fp(size, values[k % 6]));
      const u64 v6 = in.v6.value_or(fp(size, values[(k + 1) % 6]));
      const u64 mem = in.mem.value_or(fp(size, values[(k + 2) % 6]));
      const s32 i = ints[k % 6];
      cpu.reset(config);
      cpu.jit->SetVector(1, scalar(size, v1, k & 1 ? 0xffffffffu : 0x12345678u));
      const A64::Vector q6 = scalar(size, v6, 0xffffffffu);
      cpu.jit->SetVector(6, q6);
      std::memcpy(memory, &mem, size / 8);
      cpu.jit->SetRegister(9, address(memory));
      cpu.jit->SetRegister(10, 0xdeadbeef00000000 | u32(i));
      cpu.run(pc);
      const u64 converted = size == 32 ? f32(float(i)) : f64(double(i));
      const std::array<u64, 4> source{v1, v6, mem, converted};
      expect(source, name + " " + describe(config) + " v1=" + hex(v1) + " v6=" + hex(v6) + " mem=" + hex(mem) + " w10=" + std::to_string(i));
      require(cpu.jit->GetVector(20) == q6, name + ": full copy of v6");
      ++checks;
    };
    for (unsigned op = 0; op < 4; ++op)
      for (unsigned a = 0; a < 4; ++a)
        for (unsigned b = 0; b < 4; ++b) {
          std::vector<u32> words;
          if (a == 2 || b == 2) words.push_back(ldr_fp(size, 7, 9));
          if (a == 3 || b == 3) words.push_back(scvtf_w(size, 8, 10));
          words.push_back(fp3(ops[op], size, 0, regs[a], regs[b]));
          words.push_back(mov16b(20, 6));
          const u64 pc = cpu.load(words);
          const std::string name = std::string(op_names[op]) + " " + std::to_string(size) + " v0, v" + std::to_string(regs[a]) + ", v" + std::to_string(regs[b]);
          const auto expect = [&](const std::array<u64, 4>& source, const std::string& what) {
            expect_scalar(cpu, 0, size, fp_arith(op, size, source[a], source[b], config), what);
          };
          for (unsigned k = 0; k < 6; ++k) run_case(pc, name, k, {}, expect);
          for (unsigned k = 0; k < nan_inputs.size(); ++k) run_case(pc, name + " (NaN)", k, nan_inputs[k], expect);
        }
    // Chains: a negated full-Q read as the first operand of an FP load, and an
    // absolute value (zero above) with a full-Q read as the second.
    for (const unsigned op : {0u, 2u}) {
      const u64 pc = cpu.load({ldr_fp(size, 7, 9), fneg(size, 11, 6), fp3(ops[op], size, 0, 11, 7),
                               fabs_(size, 12, 6), fp3(ops[op], size, 5, 12, 6), mov16b(20, 6)});
      const std::string name = std::string(op_names[op]) + " " + std::to_string(size) + " chain";
      const auto expect = [&](const std::array<u64, 4>& source, const std::string& what) {
        const u64 negated = source[1] ^ sign_bit(size), absolute = source[1] & ~sign_bit(size);
        expect_scalar(cpu, 11, size, negated, what + " fneg");
        expect_scalar(cpu, 12, size, absolute, what + " fabs");
        expect_scalar(cpu, 0, size, fp_arith(op, size, negated, source[2], config), what + " op(-v6, load)");
        expect_scalar(cpu, 5, size, fp_arith(op, size, absolute, source[1], config), what + " op(|v6|, v6)");
      };
      for (unsigned k = 0; k < 6; ++k) run_case(pc, name, k, {}, expect);
      for (unsigned k = 0; k < nan_inputs.size(); ++k) run_case(pc, name + " (NaN)", k, nan_inputs[k], expect);
    }
  }
  return checks;
}

// FMADD (subtract = false) or FMSUB d0, n1, m2, a3.
u32 fused_op(unsigned size, bool subtract) { return 0x1f000000 | type_bit(size) | 2u << 16 | (subtract ? 0x8000 : 0) | 3u << 10 | 1u << 5; }
std::optional<u64> fp_fused(bool subtract, unsigned size, u64 n, u64 m, u64 a, const Config& config) {
  const bool any_nan = is_nan(size, n) || is_nan(size, m) || is_nan(size, a);
  const auto is_inf = [&](u64 b) { return (b & ~sign_bit(size)) == (size == 32 ? 0x7f800000 : 0x7ff0000000000000); };
  const auto is_zero = [&](u64 b) { return (b & ~sign_bit(size)) == 0; };
  const bool invalid_product = (is_inf(n) && is_zero(m)) || (is_zero(n) && is_inf(m));
  if (any_nan || invalid_product) {
    if (config.inaccurate_nan) return std::nullopt;
    if (config.dn) return default_nan(size);
    if (!any_nan) return default_nan(size);
    // SNaNs first, in the order a, n, m; then QNaNs. FMSUB negates n first.
    if (subtract && is_nan(size, n)) n ^= sign_bit(size);
    for (const u64 v : {a, n, m}) if (is_snan(size, v)) return quiet(size, v);
    if (is_nan(size, a) && invalid_product) return default_nan(size);
    for (const u64 v : {a, n, m}) if (is_nan(size, v)) return v;
  }
  if (size == 32) {
    const float x = std::bit_cast<float>(u32(n)), y = std::bit_cast<float>(u32(m)), z = std::bit_cast<float>(u32(a));
    return std::bit_cast<u32>(std::fma(subtract ? -x : x, y, z));
  }
  const double x = std::bit_cast<double>(n), y = std::bit_cast<double>(m), z = std::bit_cast<double>(a);
  return std::bit_cast<u64>(std::fma(subtract ? -x : x, y, z));
}

unsigned fused(Cpu& cpu, const Config& config) {
  unsigned checks = 0;
  for (const unsigned size : {32u, 64u}) {
    const u64 inf = size == 32 ? 0x7f800000 : 0x7ff0000000000000;
    const u64 qnan = size == 32 ? 0xffc54321 : 0xfff8000000054321, snan = size == 32 ? 0x7f812345 : 0x7ff0000000012345;
    const u64 one = fp(size, 1.0), third = fp(size, 1.0 / 3.0), big = fp(size, 3.0e7), zero = 0;
    // {n, m, a}: an exact product, rounding that only a fused multiply-add
    // gets right, cancellation, then NaNs in each position and inf * 0.
    const std::array<std::array<u64, 3>, 12> inputs{{{fp(size, 1.5), fp(size, -2.25), fp(size, 7.0)},
                                                     {third, fp(size, 3.0), fp(size, -1.0)},
                                                     {big, big, fp(size, -9.0e14)},
                                                     {one, zero, zero ^ sign_bit(size)},
                                                     {qnan, one, third}, {one, snan, third}, {one, third, qnan},
                                                     {snan, one, qnan}, {qnan, third, snan},
                                                     {inf, zero, one}, {zero, inf, qnan}, {inf, zero, snan}}};
    for (const bool subtract : {false, true}) {
      const u64 pc = cpu.load({fused_op(size, subtract)});
      for (const auto& [n, m, a] : inputs) {
        cpu.reset(config);
        cpu.jit->SetVector(1, scalar(size, n, 0x12345678u));
        cpu.jit->SetVector(2, scalar(size, m, 0xffffffffu));
        cpu.jit->SetVector(3, scalar(size, a, 0x9abcdef0u));
        cpu.run(pc);
        const std::string what = std::string(subtract ? "fmsub " : "fmadd ") + std::to_string(size) + " " + describe(config) + " n=" + hex(n) + " m=" + hex(m) + " a=" + hex(a);
        expect_scalar(cpu, 0, size, fp_fused(subtract, size, n, m, a, config), what);
        ++checks;
      }
    }
  }
  return checks;
}

unsigned words(Cpu& cpu, const Config& config) {
  unsigned checks = 0;
  const u64 pc = cpu.load({
      0x8b040062,  // add x2, x3, x4 (x4 = 0): a 64-bit value; every W read of x2 below is forwarded
      0x8b22c020,  // add x0, x1, w2, sxtw
      0x8b224025,  // add x5, x1, w2, uxtw
      0x11000446,  // add w6, w2, #1
      0x2a0203e7,  // mov w7, w2
      0x53037c49,  // lsr w9, w2, #3
      0x131f7c4a,  // asr w10, w2, #31
      0x93407c4b,  // sxtw x11, w2
      0x1e22004c,  // scvtf s12, w2
      0xb90001a2,  // str w2, [x13]
      0x6b0e005f,  // cmp w2, w14
      0x1a9fa7ef,  // cset w15, lt
      0x1ad10850,  // udiv w16, w2, w17
      0x5ac01052,  // clz w18, w2
      0x11001453,  // add w19, w2, #5
      0x5ac00854,  // rev w20, w2
      0x1b027c55,  // mul w21, w2, w2
      0x2a0203e8,  // mov w8, w2
      0x1e620056,  // scvtf d22, w2
  });
  constexpr std::array<u64, 8> inputs{0xdeadbeef80000001, 0xffffffff7fffffff, 0x0000000100000000, 0x12345678fffffff0,
                                      0x8000000000000005, 0x7fffffffffffffff, 0, 0xffffffffffffffff};
  const u64 x1 = 0x1000000000000000, x14 = 0xabcdef0000000005, x17 = 0x5555555500000007;
  for (const u64 x : inputs) {
    cpu.reset(config);
    cpu.jit->SetRegister(1, x1);
    cpu.jit->SetRegister(3, x);
    cpu.jit->SetRegister(4, 0);
    cpu.jit->SetRegister(13, address(memory + 16));
    cpu.jit->SetRegister(14, x14);
    cpu.jit->SetRegister(17, x17);
    std::memset(memory + 16, 0xcc, 8);
    cpu.run(pc);
    const u32 w = u32(x);
    const std::string what = "words " + describe(config) + " x2=" + hex(x);
    const auto reg = [&](unsigned r, u64 want, const char* name) {
      require(cpu.jit->GetRegister(r) == want, what + " " + name + ": x" + std::to_string(r) + " got " + hex(cpu.jit->GetRegister(r)) + " want " + hex(want));
    };
    reg(2, x, "x2");
    reg(0, x1 + u64(s64(s32(w))), "add sxtw");
    reg(5, x1 + w, "add uxtw");
    reg(6, u32(w + 1), "add w #1");
    reg(7, w, "mov w");
    reg(9, w >> 3, "lsr w");
    reg(10, u32(s32(w) >> 31), "asr w");
    reg(11, u64(s64(s32(w))), "sxtw");
    expect_scalar(cpu, 12, 32, f32(float(s32(w))), what + " scvtf s");
    u64 stored; std::memcpy(&stored, memory + 16, 8);
    require(stored == (0xcccccccc00000000 | w), what + " str w: " + hex(stored));
    reg(15, s32(w) < s32(u32(x14)) ? 1 : 0, "cmp/cset");
    reg(16, w / u32(x17), "udiv");
    reg(18, u64(std::countl_zero(w)), "clz");
    reg(19, u32(w + 5), "add w19");
    reg(20, (w >> 24) | ((w >> 8) & 0xff00) | ((w << 8) & 0xff0000) | (w << 24), "rev");
    reg(21, u32(w * w), "mul");
    reg(8, w, "mov w8");
    expect_scalar(cpu, 22, 64, f64(double(s32(w))), what + " scvtf d");
    ++checks;
  }
  return checks;
}
}  // namespace

int main() {
  try {
    unsigned checks = 0;
    for (const bool cache : {true, false})
      for (const bool avx : {true, false})
        for (const bool inaccurate_nan : {false, true})
          for (const bool dn : {false, true}) {
            const Config config{cache, avx, inaccurate_nan, dn};
            Cpu cpu(config);
            checks += compare_select(cpu, config);
            checks += arithmetic(cpu, config);
            checks += words(cpu, config);
            checks += fused(cpu, config);
          }
    std::printf("jit_scalar_fp: %u cases passed\n", checks);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}
