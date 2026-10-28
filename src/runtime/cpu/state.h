#pragma once
#include <array>
#include "cpu/cpu.h"

namespace venture {
// ARM user context: CPSR includes T and both IT fields. D0-D31 cover the full
// VFPv3/NEON register bank. TPIDRURO is bionic's read-only thread pointer;
// TPIDRURW is a separate guest-writable word (ARM ARM system registers).
struct CpuState32 {
  std::array<std::uint32_t, 16> r{};
  std::array<std::uint64_t, 32> d{};
  std::uint32_t cpsr{}, fpscr{}, tpidruro{}, tpidrurw{};
  explicit CpuState32(Cpu& c) : cpsr(c.pstate()), fpscr(c.fpcr()),
      tpidruro(static_cast<std::uint32_t>(c.tpidrro())), tpidrurw(static_cast<std::uint32_t>(c.tpidr())) {
    for (int i = 0; i < 16; ++i) r[i] = static_cast<std::uint32_t>(c.x(i));
    for (int i = 0; i < 32; ++i) d[i] = c.v(i).lo;
  }
  void restore(Cpu& c) const {
    for (int i = 0; i < 16; ++i) c.set_x(i, r[i]);
    for (int i = 0; i < 32; ++i) c.set_v(i, {d[i], 0});
    c.set_pstate(cpsr); c.set_fpcr(fpscr);
    c.set_tpidrro(tpidruro); c.set_tpidr(tpidrurw);
  }
};
// Complete user-visible context, used by nested host calls and signal frames.
struct CpuState {
  std::array<std::uint64_t, 31> x{};
  std::array<Vec128, 32> v{};
  std::uint64_t sp{}, pc{}, tp{}, tpro{};
  std::uint32_t pstate{}, fpcr{}, fpsr{};
  explicit CpuState(Cpu& c) : sp(c.sp()), pc(c.pc()), tp(c.tpidr()), tpro(c.tpidrro()), pstate(c.pstate()), fpcr(c.fpcr()), fpsr(c.fpsr()) {
    for (int i = 0; i < 31; ++i) x[i] = c.x(i);
    for (int i = 0; i < 32; ++i) v[i] = c.v(i);
  }
  void restore(Cpu& c) const {
    for (int i = 0; i < 31; ++i) c.set_x(i, x[i]);
    for (int i = 0; i < 32; ++i) c.set_v(i, v[i]);
    c.set_sp(sp); c.set_pc(pc); c.set_tpidr(tp); c.set_tpidrro(tpro);
    c.set_pstate(pstate); c.set_fpcr(fpcr); c.set_fpsr(fpsr);
  }
};
}
