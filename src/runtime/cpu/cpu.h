// CPU backend interface: executes guest ARM64 code for one guest thread.
//
// One Cpu instance belongs to exactly one guest thread and is only driven
// from that thread's host thread; request_halt() is the single member that
// other threads may call, along with the optional profiling request below.
#pragma once
#include "cpu/block_list.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "core/guest.h"

namespace venture {
class GuestThread;
namespace hle { struct FunctionInfo; class Registers; }

struct Vec128 {
  std::uint64_t lo = 0;
  std::uint64_t hi = 0;
};

// Immediate operands of SVC instructions reserved by Venture. Guest code
// built for Android only ever uses SVC #0 (Linux system call).
inline constexpr std::uint32_t kSvcLinuxSyscall = 0;
inline constexpr std::uint32_t kSvcHleCall = 0x5150;      // X16 = HLE function id
inline constexpr std::uint32_t kSvcReturnToHost = 0x5151;  // end of a host->guest call
inline constexpr std::uint32_t kSvcHleCallFast = 0x5154;
inline constexpr std::uint32_t kSvcHleCallFastFp = 0x5155;
inline constexpr std::uint32_t kSvcHleCallFastGuestFp = 0x5156;  // A64: handler keeps the guest MXCSR
inline constexpr std::uint32_t kSvcEntryHook = 0x5157;  // engine policy hook in place of a function's entry

enum class CpuExit {
  Svc,                  // SVC executed; svc_immediate() says which. PC = next instruction.
  HaltRequested,        // request_halt() was called (signals, suspension, exit).
  MemoryFault,          // invalid guest access; fault() describes it. PC = faulting instruction.
  UndefinedInstruction, // PC = the instruction.
  Breakpoint,           // BRK/HLT. PC = the instruction.
  HleException,         // callback exception; rethrow only after Run releases its gate.
};

struct CpuFault {
  GuestAddr address = 0;
  bool write = false;
  bool execute = false;
};

struct CpuProfileSnapshot {
  GuestAddr pc = 0; bool translated = false;
  bool outlined_atomics = false;
  // Per helper: all entries, direct BL calls. Read only while CPU is suspended.
  std::array<std::uint64_t, 16> outlined_counts{};
};

class Cpu {
 public:
  virtual ~Cpu() = default;
  void bind_thread(GuestThread& thread) { hle_thread_ = &thread; }
  void rethrow_hle_exception();

  // Runs guest code from the current PC until one of the CpuExit conditions.
  virtual CpuExit run() = 0;
  // Used only to observe a non-local return from an interrupted A32 HLE wait.
  // Synthetic test CPUs already execute one operation per run.
  virtual CpuExit step() { return run(); }
  // Thread-safe. Makes the current or next run() return HaltRequested soon.
  virtual void request_halt() = 0;
  virtual std::uint32_t svc_immediate() const = 0;
  virtual CpuFault fault() const = 0;

  // Architectural state. A64: x(31) is invalid; use sp(). A32: x(0..15)
  // are r0..r15; v(0..31).lo are D0..D31, and pstate/fpcr are CPSR/FPSCR.
  virtual std::uint64_t x(int index) const = 0;
  virtual void set_x(int index, std::uint64_t value) = 0;
  virtual std::uint64_t sp() const = 0;
  virtual void set_sp(std::uint64_t value) = 0;
  virtual std::uint64_t pc() const = 0;
  virtual void set_pc(std::uint64_t value) = 0;
  virtual std::uint32_t pstate() const = 0;  // NZCV in bits 31..28
  virtual void set_pstate(std::uint32_t value) = 0;
  virtual std::uint32_t fpcr() const = 0;
  virtual void set_fpcr(std::uint32_t value) = 0;
  virtual std::uint32_t fpsr() const = 0;
  virtual void set_fpsr(std::uint32_t value) = 0;
  virtual Vec128 v(int index) const = 0;
  virtual void set_v(int index, Vec128 value) = 0;

  // A64 TPIDR_EL0 (bionic thread pointer) and TPIDRRO_EL0. A32 uses
  // TPIDRURW and TPIDRURO respectively; its bionic thread pointer is read-only.
  virtual std::uint64_t tpidr() const = 0;
  virtual void set_tpidr(std::uint64_t value) = 0;
  // Optional extension; register-only test CPUs need not implement TPIDRRO_EL0.
  virtual std::uint64_t tpidrro() const { return 0; }
  virtual void set_tpidrro(std::uint64_t) {}

  // Clears this core's exclusive monitor (context switch / signal delivery).
  virtual void clear_exclusive() = 0;

  // ISA-neutral register roles. Shared runtime code uses these instead of
  // hard-coded A64 register numbers. The defaults describe A64 so register-only
  // test CPUs work unchanged; backends override them directly (no extra
  // indirection on the A64 hot path). A32: LR = r14, HLE id = r12, syscall
  // number = r7.
  virtual GuestIsa isa() const { return GuestIsa::a64; }
  virtual std::uint64_t lr() const { return x(30); }
  virtual void set_lr(std::uint64_t value) { set_x(30, value); }
  virtual std::uint64_t hle_call_id() const { return x(16); }
  virtual std::uint64_t syscall_number() const { return x(8); }
  virtual std::uint64_t frame_pointer() const { return x(29); }
  // Size of the SVC/trap instruction that caused an Svc exit (PC - size = SVC).
  virtual std::uint32_t trap_instruction_size() const { return 4; }
  // Profiler only: caller pins Cpu lifetime and suspends the owning host thread.
  // Must be lock-free, allocation-free and noexcept. PC is the last PC spilled
  // by the JIT dispatcher, not necessarily the current linked block's PC.
  virtual CpuProfileSnapshot profile_snapshot(std::uintptr_t) const noexcept { return {}; }
  // Optional owner-thread load-time warming, before the first outer guest call.
  virtual void prepare_call(GuestAddr) {}
  // Move only hint data while protected; persistence must run after thread locks.
  virtual JitBlockList detach_jit_hints() noexcept { return {}; }
 protected:
  // Exceptions must never unwind through generated machine code. The SVC
  // callback records them; the thread loop rethrows after leaving Run.
  bool dispatch_hle_in_jit();
  bool dispatch_hle_in_jit(const hle::FunctionInfo&, hle::Registers);
  bool has_hle_exception() const { return bool(hle_exception_); }
 private:
  GuestThread* hle_thread_ = nullptr;
  std::exception_ptr hle_exception_;
};

struct CpuBackendInfo {
  std::string name;
  // Linux AT_HWCAP / AT_HWCAP2 bits the backend implements for guests.
  std::uint64_t hwcap = 0;
  std::uint64_t hwcap2 = 0;
};

// Live guest CPUs (threads) a backend supports at once; create_cpu() throws
// past this, and guest pthread_create reports EAGAIN.
inline constexpr std::size_t kGuestCpuSlots = 1024;

// Why translations are discarded; recorded only by VENTURE_TRACE_INVALIDATION.
enum class InvalidationCause : std::uint8_t { other, map, unmap, protect, advise, cache_flush, ic_operation, code_patch, count };

class CpuBackend {
 public:
  virtual ~CpuBackend() = default;
  virtual const CpuBackendInfo& info() const = 0;
  // Creates the CPU for a new guest thread. Must be called on that thread.
  virtual std::unique_ptr<Cpu> create_cpu() = 0;
  // Discards translations overlapping [address, address + size) in every Cpu
  // (code modified, unmapped or made non-executable). Thread-safe.
  virtual void invalidate(GuestAddr address, std::uint64_t size,
                          InvalidationCause cause = InvalidationCause::other) = 0;
};

// The process-wide backend (Dynarmic A64 by default).
CpuBackend& cpu_backend();

}  // namespace venture
