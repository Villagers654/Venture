// The native entry: how A64 guest code running directly on an AArch64 host
// calls host functions (docs/design/native-headsets.md, 4.1). One mechanism
// for every native host:
//
//  * Every host function is an HLE function with an id. Its guest-callable
//    code is a 32-byte slot that loads the id into x16 and branches to one host
//    routine per call kind (NativeCall):
//
//        movz x16, #id[15:0]
//        movk x16, #id[31:16], lsl #16
//        nop                       // 16-byte far-branch patch area
//        nop
//        ldr  x17, #8
//        br   x17
//        .quad <routine>
//
//    NativeEntry writes it (a GuestEntry for set_guest_entry()). A shared
//    library built ahead of time, where a literal address would need a text
//    relocation, uses the image form instead (cpu/native_image_slot.inc): the
//    same first four words, then adrp/add/br to the routine and a nop.
//  * The routine saves the guest registers into a NativeContext and calls
//    venture_native_dispatch(context), which runs the handler through the
//    host's dispatcher (set_native_dispatcher()).
//
// The host is chosen when Venture is built, by OS, never by device:
//  * Linux: the context is at [TPIDR_EL0 - 16]; the routine switches
//    TPIDR_EL0, x18, sp and FPCR to the host values saved by
//    venture_native_enter() before any host code runs. RunLoop kinds
//    (hle_call, entry_hook, return_to_host) and any dispatch that does not
//    complete leave venture_native_enter() with the exit reason.
//  * Android: guest and host share the thread, so the context is a frame on
//    the caller's stack, nothing is switched, and every kind runs its handler
//    in place (there is no run loop to exit to).
#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "cpu/cpu.h"
#include "cpu/guest_entry.h"
#include "cpu/native_context.h"

namespace venture {

// One host routine per kind, in this order.
enum class NativeCall : std::uint8_t {
  hle_call,                // RunLoop handlers
  hle_call_fast,           // Execution::InJit
  hle_call_fast_fp,        // Execution::InJitFp
  hle_call_fast_guest_fp,  // Execution::InJitGuestFp
  entry_hook,
  return_to_host,          // end of a host->guest call (also call_return)
};
inline constexpr std::size_t kNativeCallKinds = 6;

// The SVC immediate the trap entry uses for the same kind; NativeContext::kind
// and svc_immediate carry it.
std::uint32_t native_call_svc(NativeCall kind) noexcept;
std::optional<NativeCall> native_call_for_svc(std::uint32_t svc) noexcept;

// Routine addresses, indexed by NativeCall.
using NativeRoutines = std::array<std::uint64_t, kNativeCallKinds>;

class NativeEntry final : public GuestEntry {
 public:
  explicit NativeEntry(const NativeRoutines& routines) : routines_(routines) {}
  const char* name() const noexcept override { return "native-a64"; }
  // A64 slots as above; A32 and the other stub kinds are the trap entry's
  // (A32 guests always run on Dynarmic; sigreturn and TLSDESC stubs are guest code).
  StubCode stub(GuestIsa isa, GuestStub kind, std::uint32_t argument, std::uint32_t svc) const override;

 private:
  NativeRoutines routines_;
};

// What the host does with one call: run the handler for context.x[16] and
// return VENTURE_NATIVE_EXIT_NONE when it completed, or the reason to leave
// venture_native_enter() (Linux only; on Android every call must complete).
using NativeDispatcher = std::uint32_t (*)(NativeContext& context);
void set_native_dispatcher(NativeDispatcher dispatcher) noexcept;

extern "C" std::uint32_t venture_native_dispatch(NativeContext* context) noexcept;

#if defined(__aarch64__)
// The assembled routines (cpu/native_entry_a64.S) and the entry using them.
const NativeRoutines& native_routines() noexcept;
const GuestEntry& native_entry() noexcept;
#if !defined(__ANDROID__)
// Linux: runs guest code from context.pc with context's registers until an
// exit; returns the VENTURE_NATIVE_EXIT_* reason. Resumes with every guest
// register restored except x17, which holds the resume pc (AAPCS64 IP1, dead
// at every call boundary); a resume that needs x17 (an arbitrary interrupted
// pc) goes through rt_sigreturn instead.
extern "C" std::uint32_t venture_native_enter(NativeContext* context);
#endif
#endif

// A Cpu over a NativeContext, for handlers: CallContext reads arguments and
// writes results through it, so handlers do not change. A host's own Cpu
// (the native-a64 engine) derives from it and implements run().
class NativeContextCpu : public Cpu {
 public:
  explicit NativeContextCpu(NativeContext& context) : context_(context) {}
  NativeContext& context() const { return context_; }

  CpuExit run() override;
  void request_halt() override;
  std::uint32_t svc_immediate() const override { return context_.svc_immediate; }
  CpuFault fault() const override { return {}; }
  std::uint64_t x(int index) const override { return context_.x[index]; }
  void set_x(int index, std::uint64_t value) override { context_.x[index] = value; }
  std::uint64_t sp() const override { return context_.sp; }
  void set_sp(std::uint64_t value) override { context_.sp = value; }
  std::uint64_t pc() const override { return context_.pc; }
  void set_pc(std::uint64_t value) override { context_.pc = value; }
  std::uint32_t pstate() const override { return context_.nzcv; }
  void set_pstate(std::uint32_t value) override { context_.nzcv = value & 0xf0000000u; }
  std::uint32_t fpcr() const override { return context_.fpcr; }
  void set_fpcr(std::uint32_t value) override { context_.fpcr = value; }
  std::uint32_t fpsr() const override { return context_.fpsr; }
  void set_fpsr(std::uint32_t value) override { context_.fpsr = value; }
  Vec128 v(int index) const override { return {context_.q[index][0], context_.q[index][1]}; }
  void set_v(int index, Vec128 value) override { context_.q[index][0] = value.lo, context_.q[index][1] = value.hi; }
  std::uint64_t tpidr() const override { return context_.tpidr; }
  void set_tpidr(std::uint64_t value) override { context_.tpidr = value; }
  void clear_exclusive() override {}
  // A native entry has no SVC: its PC is the caller's return address (x30).
  // The PC becomes venture_native_repeat_call with x17 = the stub, so resuming
  // repeats the call and stepping over one instruction (a signal handler
  // skipping the faulting call) returns to the caller, as with the trap stub.
  void rewind_hle_call(GuestAddr thunk) override;

 private:
  NativeContext& context_;
};

}  // namespace venture
