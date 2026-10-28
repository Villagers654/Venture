// A register-file-only Cpu for unit tests: lets tests drive HLE handlers
// through the real AAPCS64 marshaling without a JIT.
#pragma once

#include <array>
#include <cstdint>

#include "cpu/cpu.h"

namespace venture::test {

class FakeCpu : public Cpu {
 public:
  CpuExit run() override { return CpuExit::HaltRequested; }
  void request_halt() override {}
  std::uint32_t svc_immediate() const override { return 0; }
  CpuFault fault() const override { return {}; }
  std::uint64_t x(int i) const override { return xs[i]; }
  void set_x(int i, std::uint64_t value) override { xs[i] = value; }
  std::uint64_t sp() const override { return sp_; }
  void set_sp(std::uint64_t value) override { sp_ = value; }
  std::uint64_t pc() const override { return pc_; }
  void set_pc(std::uint64_t value) override { pc_ = value; }
  std::uint32_t pstate() const override { return pstate_; }
  void set_pstate(std::uint32_t value) override { pstate_ = value; }
  std::uint32_t fpcr() const override { return fpcr_; }
  void set_fpcr(std::uint32_t value) override { fpcr_ = value; }
  std::uint32_t fpsr() const override { return fpsr_; }
  void set_fpsr(std::uint32_t value) override { fpsr_ = value; }
  Vec128 v(int i) const override { return vs[i]; }
  void set_v(int i, Vec128 value) override { vs[i] = value; }
  std::uint64_t tpidr() const override { return tpidr_; }
  void set_tpidr(std::uint64_t value) override { tpidr_ = value; }
  void clear_exclusive() override {}

  // Points SP at a local buffer so stack-passed arguments can be staged.
  void use_stack(std::uint64_t* words) { sp_ = reinterpret_cast<std::uint64_t>(words); }

  std::array<std::uint64_t, 31> xs{};
  std::array<Vec128, 32> vs{};

 private:
  std::uint64_t sp_ = 0, pc_ = 0, tpidr_ = 0;
  std::uint32_t pstate_ = 0, fpcr_ = 0, fpsr_ = 0;
};

}  // namespace venture::test
