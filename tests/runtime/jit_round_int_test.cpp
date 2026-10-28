// SPDX-License-Identifier: GPL-3.0-or-later
// Every A64 FRINT form (N, P, M, Z, A, I, X) on S, D, 2S, 4S and 2D registers
// against the FP library's FPRoundInt, which the JIT's out-of-line fallback
// runs: result bits, zero upper lanes and the FPSR flags (IXC, IOC, IDC), for
// every FPCR rounding mode with FZ and DN on and off. Exact rounding (FRINTX)
// is emitted as ROUNDSS/SD/PS/PD whose precision exception becomes IXC; the
// configuration without SSE4.1 runs the fallback as a cross-check.
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/common/fp/fpcr.h>
#include <dynarmic/common/fp/fpsr.h>
#include <dynarmic/common/fp/op/FPRoundInt.h>
#include <dynarmic/common/fp/rounding_mode.h>
#include <array>
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

constexpr u32 svc = 0xd4000001;
constexpr u32 vex_features = 0x0ff8;  // Backend::X64::HostFeature AVX, AVX2, AVX512F-AVX512VBMI
constexpr u32 sse41_up = 0x0ffe;      // also SSE4.1 and SSE4.2: the fallback path
constexpr u64 junk = 0x8badf00ddeadbeef;

class Cpu final : private A64::UserCallbacks {
 public:
  explicit Cpu(u32 disabled_features) {
    A64::UserConfig c;
    c.callbacks = this;
    c.enable_cycle_counting = false;
    c.disabled_host_features = disabled_features;
    jit = std::make_unique<A64::Jit>(c);
  }
  u64 load(u32 word) {
    auto& block = code.emplace_back();
    block = {word, svc};
    return reinterpret_cast<std::uintptr_t>(block.data());
  }
  A64::Vector run(u64 pc, A64::Vector input, u32 fpcr) {
    jit->ClearHalt(HaltReason::UserDefined1);
    jit->SetVector(0, {junk, ~junk});
    jit->SetVector(1, input);
    jit->SetFpcr(fpcr);
    jit->SetFpsr(0);
    jit->SetPC(pc);
    jit->Run();
    return jit->GetVector(0);
  }
  std::unique_ptr<A64::Jit> jit;

 private:
  std::deque<std::array<u32, 2>> code;
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

std::string hex(u64 v) { char b[24]; std::snprintf(b, sizeof(b), "%#llx", static_cast<unsigned long long>(v)); return b; }

struct Form { const char* name; u32 scalar; u32 vector; std::optional<FP::RoundingMode> rounding; bool exact; };
// Scalar single (type 00) and vector 2S/4S (sz 0) encodings; double adds bit 22.
constexpr std::array<Form, 7> forms{{
    {"frintn", 0x1e244000, 0x0e218800, FP::RoundingMode::ToNearest_TieEven, false},
    {"frintp", 0x1e24c000, 0x0ea18800, FP::RoundingMode::TowardsPlusInfinity, false},
    {"frintm", 0x1e254000, 0x0e219800, FP::RoundingMode::TowardsMinusInfinity, false},
    {"frintz", 0x1e25c000, 0x0ea19800, FP::RoundingMode::TowardsZero, false},
    {"frinta", 0x1e264000, 0x2e218800, FP::RoundingMode::ToNearest_TieAwayFromZero, false},
    {"frintx", 0x1e274000, 0x2e219800, std::nullopt, true},
    {"frinti", 0x1e27c000, 0x2ea19800, std::nullopt, false},
}};

const std::vector<u32> singles{
    0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x3f000000, 0xbf000000, 0x3fc00000, 0xbfc00000,
    0x40200000, 0xc0200000, 0x3e800000, 0xbf400000, 0x3f7fffff, 0x3f800001, 0x4b000000, 0x4afffffe,
    0x4affffff, 0xcaffffff, 0x4b7fffff, 0x7f7fffff, 0x00800000, 0x80800000, 0x00000001, 0x807fffff,
    0x00400000, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc12345, 0x7f800001, 0xff812345, 0x42f6e979,
    0xc2f6e979, 0x3dcccccd, 0x40490fdb, 0xc0490fdb};
const std::vector<u64> doubles{
    0x0000000000000000, 0x8000000000000000, 0x3ff0000000000000, 0xbff0000000000000, 0x3fe0000000000000,
    0xbfe0000000000000, 0x3ff8000000000000, 0xbff8000000000000, 0x4004000000000000, 0xc004000000000000,
    0x3fefffffffffffff, 0x3ff0000000000001, 0x4330000000000000, 0x432fffffffffffff, 0xc32fffffffffffff,
    0x433fffffffffffff, 0x7fefffffffffffff, 0x0010000000000000, 0x8010000000000000, 0x0000000000000001,
    0x800fffffffffffff, 0x7ff0000000000000, 0xfff0000000000000, 0x7ff8000000000000, 0xfff8000000012345,
    0x7ff0000000000001, 0xfff0000000012345, 0x405edd2f1a9fbe77, 0xc05edd2f1a9fbe77, 0x400921fb54442d18};

template<typename T>
u64 reference(T value, const Form& form, u32 fpcr_value, FP::FPSR& fpsr) {
  const FP::FPCR fpcr{fpcr_value};
  return FP::FPRoundInt<T>(value, fpcr, form.rounding.value_or(fpcr.RMode()), form.exact, fpsr);
}

unsigned check(Cpu& cpu, const char* config) {
  unsigned checks = 0;
  for (const Form& form : forms)
    for (const unsigned esize : {32u, 64u})
      for (const unsigned lanes : {0u, 2u, 4u}) {  // 0: scalar
        if (esize == 64 && lanes == 4) continue;
        const u32 word = lanes ? form.vector | (esize == 64 ? 0x00400000 : 0) | (lanes * esize == 128 ? 0x40000000 : 0) | 1u << 5
                               : form.scalar | (esize == 64 ? 0x00400000 : 0) | 1u << 5;
        const u64 pc = cpu.load(word);
        const std::size_t count = esize == 32 ? singles.size() : doubles.size();
        const unsigned per_run = lanes ? lanes : 1;
        for (u32 rmode = 0; rmode < 4; ++rmode)
          for (const u32 fz : {0u, 1u})
            for (const u32 dn : {0u, 1u}) {
              const u32 fpcr = rmode << 22 | fz << 24 | dn << 25;
              for (std::size_t start = 0; start < count; ++start) {
                // Each run's lanes: a rotation of the inputs, so every value meets every lane.
                std::array<u64, 4> in{};
                for (unsigned i = 0; i < per_run; ++i)
                  in[i] = esize == 32 ? singles[(start + i * 7) % count] : doubles[(start + i * 5) % count];
                A64::Vector input;
                if (esize == 32) {
                  input = {in[0] | in[1] << 32, in[2] | in[3] << 32};
                  if (!lanes) input = {in[0] | junk << 32, junk};
                  else if (lanes == 2) input[1] = junk;
                } else {
                  input = {in[0], lanes == 2 ? in[1] : junk};
                }
                FP::FPSR fpsr{};
                std::array<u64, 4> out{};
                for (unsigned i = 0; i < per_run; ++i)
                  out[i] = esize == 32 ? reference<u32>(u32(in[i]), form, fpcr, fpsr) : reference<u64>(in[i], form, fpcr, fpsr);
                const A64::Vector want = esize == 32 ? A64::Vector{out[0] | out[1] << 32, out[2] | out[3] << 32} : A64::Vector{out[0], out[1]};
                const A64::Vector got = cpu.run(pc, input, fpcr);
                const u32 got_fpsr = cpu.jit->GetFpsr();
                if (got != want || got_fpsr != fpsr.Value()) {
                  throw std::runtime_error(std::string(config) + " " + form.name + (lanes ? " " + std::to_string(lanes) + (esize == 32 ? "s" : "d") : std::string(esize == 32 ? " s" : " d")) +
                                           " fpcr=" + hex(fpcr) + " input=" + hex(input[1]) + ":" + hex(input[0]) + " got " + hex(got[1]) + ":" + hex(got[0]) +
                                           " fpsr " + hex(got_fpsr) + " want " + hex(want[1]) + ":" + hex(want[0]) + " fpsr " + hex(fpsr.Value()));
                }
                ++checks;
              }
            }
      }
  return checks;
}
}  // namespace

int main() {
  try {
    unsigned checks = 0;
    {
      Cpu cpu(0);
      checks += check(cpu, "avx");
    }
    {
      Cpu cpu(vex_features);
      checks += check(cpu, "sse4.1");
    }
    {
      Cpu cpu(sse41_up);
      checks += check(cpu, "sse2 fallback");
    }
    std::printf("jit_round_int: %u cases passed\n", checks);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}
