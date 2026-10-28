#pragma once
#include "fake_cpu.h"
#include "core/address_space.h"
#include <stdexcept>
namespace venture::test {
// Register-only fixtures use the production arena in their separate A32 process.
class LowMemory32 {
  bool mapped_ = false;
 public:
  explicit LowMemory32(bool high = false, bool executable = false) {
    auto& as = AddressSpace::get(); auto prot = kProtRead | kProtWrite | (executable ? kProtExec : 0);
    if (!high) address = as.allocate(65536, prot, "[A32 ABI fixture]");
    else for (GuestAddr hint = 0x90000000; hint < 0xf0000000; hint += 0x1000000) {
      auto result = as.map(hint, 65536, prot, kMapPrivate | kMapAnonymous | kMapFixedNoReplace, nullptr, 0, "[A32 high fixture]");
      if (result >= 0) { address = result; mapped_ = true; break; }
    }
    if (!address || address > UINT32_MAX - 65535) throw std::runtime_error("no low address for A32 fixture");
  }
  ~LowMemory32() { if (mapped_) AddressSpace::get().unmap(address, 65536); else AddressSpace::get().free(address); }
  LowMemory32(const LowMemory32&) = delete;
  template<class T> T* at(unsigned offset = 0) { return g2h<T>(address + offset); }
  GuestAddr address = 0;
};
class FakeCpu32 final : public Cpu {
 public:
  GuestIsa isa() const override { return GuestIsa::a32; }
  CpuExit run() override { return CpuExit::HaltRequested; }
  void request_halt() override {}
  std::uint32_t svc_immediate() const override { return 0; }
  CpuFault fault() const override { return {}; }
  std::uint64_t x(int i) const override { if (i < 0 || i > 15) throw std::out_of_range("A32 register"); return i == 13 ? state.sp() : i == 15 ? state.pc() : state.x(i); }
  void set_x(int i, std::uint64_t v) override {
    if (i < 0 || i > 15) throw std::out_of_range("A32 register");
    v = static_cast<std::uint32_t>(v);
    if (i == 13) state.set_sp(v); else if (i == 15) state.set_pc(v); else state.set_x(i, v);
  }
  std::uint64_t sp() const override { return state.sp(); }
  void set_sp(std::uint64_t v) override { state.set_sp(v); }
  std::uint64_t pc() const override { return state.pc(); }
  void set_pc(std::uint64_t v) override { state.set_pc(v); }
  std::uint32_t pstate() const override { return state.pstate(); }
  void set_pstate(std::uint32_t v) override { state.set_pstate(v); }
  std::uint32_t fpcr() const override { return state.fpcr(); }
  void set_fpcr(std::uint32_t v) override { state.set_fpcr(v); }
  std::uint32_t fpsr() const override { return state.fpsr(); }
  void set_fpsr(std::uint32_t v) override { state.set_fpsr(v); }
  Vec128 v(int i) const override { if (i < 0 || i >= 32) throw std::out_of_range("A32 D register"); return {state.v(i).lo, 0}; }
  void set_v(int i, Vec128 v) override { if (i < 0 || i >= 32) throw std::out_of_range("A32 D register"); state.set_v(i, {v.lo, 0}); }
  std::uint64_t tpidr() const override { return state.tpidr(); }
  void set_tpidr(std::uint64_t v) override { state.set_tpidr(v); }
  void clear_exclusive() override {}
  std::uint64_t lr() const override { return x(14); }
  void set_lr(std::uint64_t v) override { set_x(14, v); }
  std::uint64_t hle_call_id() const override { return x(12); }
  std::uint64_t syscall_number() const override { return x(7); }
  FakeCpu state; // may poison upper bits to verify callee-side narrowing
};
}
