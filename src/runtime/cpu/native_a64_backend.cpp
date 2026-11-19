// The native-a64 engine (docs/design/native-headsets.md 4.1): A64 guest code
// runs directly on a Linux AArch64 host. Identity addressing makes a guest
// address the host address, guest executable pages are host executable
// (platform::set_guest_code_executable), and stubs branch to the host entry
// routines (cpu/native_entry.h). This file adds what leaves guest code without
// a stub:
//  * raw system calls: one seccomp filter returns SECCOMP_RET_TRAP for system
//    calls whose instruction pointer is inside the A64 guest arena; the SIGSYS
//    handler turns it into CpuExit::Svc with the SVC's immediate (#0 is a Linux
//    system call for the thread loop; Venture's own trap SVCs, such as entry
//    hooks, work unchanged);
//  * faults and traps in guest code (SIGSEGV, SIGBUS, SIGILL, SIGTRAP) become
//    MemoryFault, UndefinedInstruction and Breakpoint;
//  * request_halt() from another thread sets the halt word and signals the
//    thread, whose handler leaves guest code with HaltRequested.
// Each handler runs on the guest thread pointer first: it reads the context at
// [TPIDR_EL0 - 16], copies the interrupted state into it, switches to the host
// thread pointer, and returns into venture_native_exit, which leaves
// venture_native_enter as a routine exit does. A context left by a signal has
// every register live (x17 too), so the next run() resumes it with
// rt_sigreturn (venture_native_resume_frame) instead of venture_native_enter.
// Signals in host code go to the handlers installed before.
//
// Every other handler in the process is host code and must not run on the
// guest thread pointer (host TLS, errno, the JVM's thread lookup). This file
// interposes sigaction and signal for the whole process: host code still sees
// its own actions, but the kernel runs a guard that switches to the host
// thread pointer and FPCR first when the signal interrupted guest code (or an
// entry routine on the guest thread pointer), then calls the host handler.
#include "cpu/native_a64.h"

#if VENTURE_NATIVE_A64
#include <dlfcn.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "common/log.h"
#include "core/address_space.h"
#include "core/guest_heap.h"
#include "core/hle.h"
#include "cpu/native_entry.h"
#include "platform/memory.h"

extern "C" void venture_native_exit();
extern "C" void venture_native_leave_unentered();
// Labels in the entry routines (native_entry_a64.S, native_a64_resume.S).
extern "C" void venture_native_resume_call();
extern "C" void venture_native_resume_call_return();
extern "C" void venture_native_enter_window();
extern "C" void venture_native_enter_branch();
extern "C" void venture_native_resume_frame_window();
extern "C" void venture_native_resume_frame_svc();
extern "C" std::uint32_t venture_native_resume_frame(venture::NativeContext* context, void* frame);

namespace venture {
namespace {
// Private exit reason: the Cpu's signal_exit says which CpuExit.
constexpr std::uint32_t kExitSignal = 16;

// Guest code runs from the image arena and the guest heap arena (code a game
// writes into malloc'd pages and makes executable). Executable guest mappings at fixed addresses outside both arenas add ranges
// (prepare_executable). A range is never removed: the guest keeps the address
// space it reserved there after unmapping, so no host code can appear in it.
struct GuestRange { std::uintptr_t begin = 0, end = 0; };
constexpr std::size_t kMaxGuestRanges = 256;
GuestRange guest_ranges[kMaxGuestRanges];
std::atomic<std::size_t> guest_range_count{0};  // published after a range is filtered
bool in_guest_code(std::uintptr_t pc) {
  const auto count = guest_range_count.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < count; ++i)
    if (pc >= guest_ranges[i].begin && pc < guest_ranges[i].end) return true;
  return false;
}

// Linux arm64 signal frame records (arch/arm64/include/uapi/asm/sigcontext.h).
struct Record { std::uint32_t magic, size; };
struct FpsimdRecord { Record head; std::uint32_t fpsr, fpcr; __uint128_t v[32]; };
struct EsrRecord { Record head; std::uint64_t esr; };
constexpr std::uint32_t kFpsimdMagic = 0x46508001u, kEsrMagic = 0x45535201u;
static_assert(sizeof(FpsimdRecord) == 528);
template<class T> T* find_record(mcontext_t& machine, std::uint32_t magic) {
  auto* p = reinterpret_cast<unsigned char*>(machine.__reserved);
  auto* const end = p + sizeof machine.__reserved;
  while (p + sizeof(Record) <= end) {
    auto* record = reinterpret_cast<Record*>(p);
    if (!record->magic || record->size < sizeof(Record)) break;
    if (record->magic == magic) return reinterpret_cast<T*>(p);
    p += record->size;
  }
  return nullptr;
}
inline std::uint64_t read_tpidr() { std::uint64_t v; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v)); return v; }
inline void write_tpidr(std::uint64_t v) { __asm__ volatile("msr tpidr_el0, %0" : : "r"(v)); }

int halt_signal_number = 0;

// Guest thread pointers in use, for handlers that interrupt host code still on
// one (the entry routines switch it a few instructions from guest code). Keys
// are written last and read first; a freed slot keeps a tombstone.
constexpr std::size_t kGuestTpSlots = 4096;
constexpr std::uint64_t kTombstone = 1;
struct GuestTpSlot { std::atomic<std::uint64_t> tp{0}; std::atomic<NativeContext*> context{nullptr}; };
GuestTpSlot guest_tps[kGuestTpSlots];
std::size_t guest_tp_hash(std::uint64_t tp) { return (tp >> 4) * 0x9e3779b97f4a7c15ull >> 52; }
static_assert(kGuestTpSlots == 1u << 12);
void register_guest_tp(std::uint64_t tp, NativeContext* context) {
  for (std::size_t i = 0, slot = guest_tp_hash(tp); i < kGuestTpSlots; ++i, slot = (slot + 1) % kGuestTpSlots) {
    auto& entry = guest_tps[slot];
    auto key = entry.tp.load(std::memory_order_acquire);
    if (key == tp) { entry.context.store(context, std::memory_order_release); return; }
    if (key != 0 && key != kTombstone) continue;
    entry.context.store(context, std::memory_order_release);
    if (entry.tp.compare_exchange_strong(key, tp, std::memory_order_acq_rel)) return;
  }
  log::warn("cpu", "native-a64: guest thread pointer table full");
}
void unregister_guest_tp(std::uint64_t tp) {
  for (std::size_t i = 0, slot = guest_tp_hash(tp); i < kGuestTpSlots; ++i, slot = (slot + 1) % kGuestTpSlots) {
    auto& entry = guest_tps[slot];
    const auto key = entry.tp.load(std::memory_order_acquire);
    if (key == 0) return;
    if (key == tp) { entry.tp.store(kTombstone, std::memory_order_release); return; }
  }
}
NativeContext* lookup_guest_tp(std::uint64_t tp) {
  for (std::size_t i = 0, slot = guest_tp_hash(tp); i < kGuestTpSlots; ++i, slot = (slot + 1) % kGuestTpSlots) {
    auto& entry = guest_tps[slot];
    const auto key = entry.tp.load(std::memory_order_acquire);
    if (key == 0) return nullptr;
    if (key == tp) return entry.context.load(std::memory_order_acquire);
  }
  return nullptr;
}
// The context whose guest thread pointer this thread is on, or null on the
// host's. Async-signal-safe.
NativeContext* context_on_guest_tp(std::uintptr_t pc) {
  const auto tp = read_tpidr();
  if (in_guest_code(pc)) return *reinterpret_cast<NativeContext**>(tp - VENTURE_NATIVE_TP_CONTEXT_OFFSET);
  return lookup_guest_tp(tp);
}

// Signals in guest code must not use the guest stack: it may be full, unmapped
// or not a stack at all. Every thread that runs guest code gets an alternate
// stack once, released when the thread exits (unless something replaced it).
class AlternateStack {
 public:
  static void ensure() { thread_local AlternateStack stack; }
 private:
  // Host handlers called through the guard run here too (the JVM's among them).
  static constexpr std::size_t kSize = 128 * 1024;
  AlternateStack() {
    stack_t current{};
    if (sigaltstack(nullptr, &current) || !(current.ss_flags & SS_DISABLE)) return;  // the thread has one
    memory_ = std::make_unique<unsigned char[]>(kSize);
    stack_t stack{};
    stack.ss_sp = memory_.get();
    stack.ss_size = kSize;
    if (sigaltstack(&stack, nullptr)) memory_.reset();
  }
  ~AlternateStack() {
    if (!memory_) return;
    stack_t current{};
    if (sigaltstack(nullptr, &current) == 0 && current.ss_sp == memory_.get() && !(current.ss_flags & SS_ONSTACK)) {
      stack_t off{};
      off.ss_flags = SS_DISABLE;
      sigaltstack(&off, nullptr);
    } else {
      memory_.release();  // still in use: never freed under it
    }
  }
  std::unique_ptr<unsigned char[]> memory_;
};

class NativeA64Cpu final : public NativeContextCpu {
 public:
  NativeA64Cpu() : NativeContextCpu(state_) {
    std::memset(&state_, 0, sizeof state_);
    state_.owner = this;
  }
  ~NativeA64Cpu() override {
    if (registered_tp_) unregister_guest_tp(registered_tp_);
  }
  CpuExit run() override;
  void request_halt() override {
    NativeContextCpu::request_halt();
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (in_guest_.load(std::memory_order_seq_cst) && halt_signal_number)
      syscall(SYS_tgkill, getpid(), tid_.load(std::memory_order_relaxed), halt_signal_number);
  }
  CpuFault fault() const override { return fault_; }
  void set_x(int index, std::uint64_t value) override {
    NativeContextCpu::set_x(index, value);
    if (index == 17) full_resume_ = true;  // venture_native_enter would clobber it
  }
  // The native dispatcher's view of Cpu's in-place HLE dispatch.
  bool dispatch_in_place() { return dispatch_hle_in_jit(); }
  bool hle_exception_pending() const { return has_hle_exception(); }

  // Written by the signal handlers on this thread.
  CpuExit signal_exit = CpuExit::HaltRequested;
  CpuFault fault_{};

 private:
  std::uint32_t resume_with_sigreturn();

  alignas(16) NativeContext state_;
  alignas(16) std::uint64_t scratch_tls_[4]{};
  bool full_resume_ = false;
  std::uint64_t registered_tp_ = 0;
  std::atomic<bool> in_guest_{false};
  std::atomic<pid_t> tid_{0};  // the thread in run()
};

std::uint32_t NativeA64Cpu::resume_with_sigreturn() {
  // rt_sigframe: siginfo, then the ucontext the kernel restores.
  struct alignas(16) Frame { unsigned char info[128]; ucontext_t uc; };
  static_assert(offsetof(Frame, uc) == 128);
  Frame frame;
  std::memset(&frame, 0, sizeof frame);
  auto& machine = frame.uc.uc_mcontext;
  for (int i = 0; i < 31; ++i) machine.regs[i] = state_.x[i];
  machine.sp = state_.sp;
  machine.pc = state_.pc;
  machine.pstate = state_.nzcv & 0xf0000000u;  // EL0t, AArch64
  auto* fp = reinterpret_cast<FpsimdRecord*>(machine.__reserved);
  fp->head = {kFpsimdMagic, sizeof(FpsimdRecord)};
  fp->fpsr = state_.fpsr;
  fp->fpcr = state_.fpcr;
  std::memcpy(fp->v, state_.q, sizeof fp->v);
  // The record after it stays zero: the end of the list.
  pthread_sigmask(SIG_SETMASK, nullptr, &frame.uc.uc_sigmask);
  sigaltstack(nullptr, &frame.uc.uc_stack);
  return venture_native_resume_frame(&state_, &frame);
}

CpuExit NativeA64Cpu::run() {
  auto& c = state_;
  // A CPU without a guest thread (tests, pretranslation helpers) still needs
  // the context slot below its thread pointer: a private one.
  if (!c.tpidr) c.tpidr = reinterpret_cast<std::uint64_t>(&scratch_tls_[2]);
  // The routines and handlers find the context through the guest TLS block.
  *reinterpret_cast<NativeContext**>(c.tpidr - VENTURE_NATIVE_TP_CONTEXT_OFFSET) = &c;
  if (c.tpidr != registered_tp_) {
    if (registered_tp_) unregister_guest_tp(registered_tp_);
    register_guest_tp(c.tpidr, &c);
    registered_tp_ = c.tpidr;
  }
  AlternateStack::ensure();
  thread_local const pid_t thread_id = static_cast<pid_t>(syscall(SYS_gettid));
  tid_.store(thread_id, std::memory_order_relaxed);
  in_guest_.store(true, std::memory_order_seq_cst);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  std::uint32_t reason;
  if (full_resume_) {
    full_resume_ = false;
    reason = resume_with_sigreturn();
    // A halt before guest code ran leaves the context as it was: resume it the same way.
    if (reason == VENTURE_NATIVE_EXIT_HALT) full_resume_ = true;
  } else {
    reason = venture_native_enter(&c);
  }
  in_guest_.store(false, std::memory_order_relaxed);
  switch (reason) {
    case VENTURE_NATIVE_EXIT_SVC:
      // The trap entry's call_return stub stops 8 bytes in; the thread loop checks it.
      if (c.svc_immediate == kSvcReturnToHost) c.pc = c.x[30] + 8;
      return CpuExit::Svc;
    case VENTURE_NATIVE_EXIT_HALT:
      std::atomic_ref<std::uint32_t>(c.halt).store(0, std::memory_order_relaxed);
      return CpuExit::HaltRequested;
    case VENTURE_NATIVE_EXIT_HLE_EXCEPTION:
      return CpuExit::HleException;
    case kExitSignal:
      full_resume_ = true;
      if (signal_exit == CpuExit::HaltRequested)
        std::atomic_ref<std::uint32_t>(c.halt).store(0, std::memory_order_relaxed);
      return signal_exit;
    default:
      throw std::logic_error("native-a64: unknown exit reason");
  }
}

std::uint32_t dispatch(NativeContext& context) {
  auto& cpu = *static_cast<NativeA64Cpu*>(context.owner);
  hle::GuestFpScope fp(context.kind == kSvcHleCallFastGuestFp);
  if (cpu.dispatch_in_place()) return VENTURE_NATIVE_EXIT_NONE;
  return cpu.hle_exception_pending() ? VENTURE_NATIVE_EXIT_HLE_EXCEPTION : VENTURE_NATIVE_EXIT_SVC;
}

// sigaction as libc implements it, past the interposer below.
using SigactionFunction = int (*)(int, const struct sigaction*, struct sigaction*);
SigactionFunction real_sigaction_function() {
  static const auto function = reinterpret_cast<SigactionFunction>(dlsym(RTLD_NEXT, "sigaction"));
  return function;
}
int real_sigaction(int signal, const struct sigaction* action, struct sigaction* old) {
  return real_sigaction_function()(signal, action, old);
}

// The actions host code installed, per signal: what its sigaction calls see.
// For the engine's own signals the kernel action stays the engine's handler,
// which chains to these outside guest code; for every other signal the kernel
// runs guard() whenever host code installed a handler. Two slots per signal so
// a handler reads one while sigaction writes the other.
struct HostAction {
  struct sigaction slots[2]{};
  std::atomic<int> current{0};
  const struct sigaction& get() const { return slots[current.load(std::memory_order_acquire)]; }
  void set(const struct sigaction& action) {
    const int next = 1 - current.load(std::memory_order_relaxed);
    slots[next] = action;
    current.store(next, std::memory_order_release);
  }
};
HostAction host_actions[_NSIG];
std::mutex host_actions_mutex;
std::atomic<bool> guarding{false};
constexpr int kSignals[] = {SIGSYS, SIGSEGV, SIGBUS, SIGILL, SIGTRAP};
bool engine_signal(int signal) {
  return std::find(std::begin(kSignals), std::end(kSignals), signal) != std::end(kSignals) ||
         (halt_signal_number && signal == halt_signal_number);
}
bool is_handler(const struct sigaction& action) {
  return (action.sa_flags & SA_SIGINFO) || (action.sa_handler != SIG_DFL && action.sa_handler != SIG_IGN);
}

// Calls host code's handler for signal on the host thread pointer and FPCR.
// The FPCR comes back with the interrupted context at sigreturn; the thread
// pointer is put back here.
void call_host_handler(int signal, siginfo_t* info, void* raw) {
  auto* uc = static_cast<ucontext_t*>(raw);
  const auto action = host_actions[signal].get();
  NativeContext* context = context_on_guest_tp(uc->uc_mcontext.pc);
  const auto interrupted_tp = read_tpidr();
  if (context) {
    write_tpidr(context->host_tpidr);
    __asm__ volatile("msr fpcr, %0" : : "r"(context->host_fpcr));
  }
  if (action.sa_flags & SA_SIGINFO) {
    action.sa_sigaction(signal, info, raw);
  } else if (action.sa_handler == SIG_DFL) {
    if (signal != halt_signal_number) {
      // The default action: a fault re-executes into it; other signals are raised again.
      struct sigaction reset{};
      reset.sa_handler = SIG_DFL;
      sigemptyset(&reset.sa_mask);
      real_sigaction(signal, &reset, nullptr);
      if (signal != SIGSEGV && signal != SIGBUS && signal != SIGILL && signal != SIGTRAP) raise(signal);
    }
  } else if (action.sa_handler != SIG_IGN) {
    action.sa_handler(signal);
  }
  if (action.sa_flags & SA_RESETHAND) {
    struct sigaction reset{};
    reset.sa_handler = SIG_DFL;
    host_actions[signal].set(reset);
  }
  if (context) write_tpidr(interrupted_tp);
}
void chain(int signal, siginfo_t* info, void* raw) { call_host_handler(signal, info, raw); }
void guard(int signal, siginfo_t* info, void* raw) { call_host_handler(signal, info, raw); }

// The kernel action for a host action on a signal the engine does not own.
struct sigaction guarded(const struct sigaction& action) {
  if (!is_handler(action)) return action;
  struct sigaction kernel = action;
  kernel.sa_sigaction = guard;
  kernel.sa_flags = (action.sa_flags | SA_SIGINFO | SA_ONSTACK) & ~SA_RESETHAND;
  return kernel;
}
bool interposable(int signal) { return signal > 0 && signal < _NSIG && signal != SIGKILL && signal != SIGSTOP; }

// From now on every host handler runs through guard(): the ones installed so
// far are wrapped here, later ones by the interposer.
void start_guarding() {
  std::lock_guard lock(host_actions_mutex);
  for (int signal = 1; signal < _NSIG; ++signal) {
    if (!interposable(signal) || engine_signal(signal)) continue;
    struct sigaction current{};
    if (real_sigaction(signal, nullptr, &current)) continue;  // reserved by libc
    host_actions[signal].set(current);
    if (is_handler(current)) {
      const auto kernel = guarded(current);
      real_sigaction(signal, &kernel, nullptr);
    }
  }
  guarding.store(true, std::memory_order_release);
}

int interposed_sigaction(int signal, const struct sigaction* action, struct sigaction* old) {
  if (!guarding.load(std::memory_order_acquire) || !interposable(signal)) return real_sigaction(signal, action, old);
  if (signal == halt_signal_number) {
    // Taken, as for a signal libc reserves: a sampler looking for a free one moves on.
    if (action) { errno = EINVAL; return -1; }
    return real_sigaction(signal, nullptr, old);
  }
  std::lock_guard lock(host_actions_mutex);
  if (engine_signal(signal)) {
    if (old) *old = host_actions[signal].get();
    if (action) host_actions[signal].set(*action);
    return 0;
  }
  struct sigaction kernel_old{};
  if (action) {
    const auto kernel = guarded(*action);
    if (real_sigaction(signal, &kernel, &kernel_old)) return -1;
  } else if (real_sigaction(signal, nullptr, &kernel_old)) {
    return -1;
  }
  if (old) *old = host_actions[signal].get();
  if (action) host_actions[signal].set(*action);
  return 0;
}
sighandler_t interposed_signal(int signal, sighandler_t handler, int flags, bool mask_self) {
  struct sigaction action{}, old{};
  action.sa_handler = handler;
  sigemptyset(&action.sa_mask);
  if (mask_self && signal > 0 && signal < _NSIG) sigaddset(&action.sa_mask, signal);
  action.sa_flags = flags;
  if (interposed_sigaction(signal, &action, &old)) return SIG_ERR;
  return old.sa_handler;
}
}  // namespace
}  // namespace venture

namespace venture {
// cpu/native_a64_interpose.cpp: the process's sigaction and signal.
int native_a64_sigaction(int signal, const struct sigaction* action, struct sigaction* old) {
  return interposed_sigaction(signal, action, old);
}
NativeSignalHandler native_a64_signal(int signal, NativeSignalHandler handler, NativeSignalSemantics semantics) {
  return semantics == NativeSignalSemantics::bsd ? interposed_signal(signal, handler, SA_RESTART, true)
                                                 : interposed_signal(signal, handler, SA_RESETHAND | SA_NODEFER, false);
}
}  // namespace venture

namespace venture {
namespace {
std::uintptr_t address_of(void (*routine)()) { return reinterpret_cast<std::uintptr_t>(routine); }

// The halt signal between the routines' last halt check and the first guest
// instruction would find no guest pc and be lost (the thread then runs guest
// code with the halt pending). Those windows leave as halts; the last
// instruction of each, which only transfers control, counts as guest code at
// its target. Returns true when the signal was handled here.
bool halt_window(mcontext_t& machine) {
  const auto pc = machine.pc;
  NativeContext* context = nullptr;
  std::uintptr_t resume = 0;
  if (pc == address_of(venture_native_enter_branch)) {
    machine.pc = machine.regs[17];
    return false;
  }
  if (pc == address_of(venture_native_resume_call_return)) {
    machine.pc = machine.regs[30];
    return false;
  }
  if (pc >= address_of(venture_native_enter_window) && pc < address_of(venture_native_enter_branch)) {
    context = reinterpret_cast<NativeContext*>(machine.regs[0]);
    resume = address_of(venture_native_leave_unentered);
  } else if (pc >= address_of(venture_native_resume_frame_window) && pc <= address_of(venture_native_resume_frame_svc)) {
    context = reinterpret_cast<NativeContext*>(machine.regs[0]);
    resume = address_of(venture_native_leave_unentered);
  } else if (pc >= address_of(venture_native_resume_call) && pc < address_of(venture_native_resume_call_return)) {
    // x20-x28 and q8-q31 are the guest's: venture_native_exit stores them.
    context = reinterpret_cast<NativeContext*>(machine.regs[19]);
    resume = address_of(venture_native_exit);
  } else {
    return false;
  }
  write_tpidr(context->host_tpidr);
  if (auto* fp = find_record<FpsimdRecord>(machine, kFpsimdMagic)) fp->fpcr = static_cast<std::uint32_t>(context->host_fpcr);
  machine.regs[0] = VENTURE_NATIVE_EXIT_HALT;
  machine.regs[18] = context->host_x18;
  machine.regs[19] = reinterpret_cast<std::uint64_t>(context);
  machine.sp = context->host_sp;
  machine.pc = resume;
  return true;
}

// Runs on the interrupted thread's state. Until write_tpidr() below, no host
// TLS, libc call or allocation: TPIDR_EL0 may be the guest's.
void guest_signal(int signal, siginfo_t* info, void* raw) {
  auto* uc = static_cast<ucontext_t*>(raw);
  auto& machine = uc->uc_mcontext;
  if (signal == halt_signal_number && halt_window(machine)) return;
  if (!in_guest_code(machine.pc)) return chain(signal, info, raw);
  const auto guest_tp = read_tpidr();
  auto* context = *reinterpret_cast<NativeContext**>(guest_tp - VENTURE_NATIVE_TP_CONTEXT_OFFSET);
  auto& cpu = *static_cast<NativeA64Cpu*>(context->owner);
  for (int i = 0; i < 31; ++i) context->x[i] = machine.regs[i];
  context->sp = machine.sp;
  context->pc = machine.pc;
  context->tpidr = guest_tp;
  context->nzcv = static_cast<std::uint32_t>(machine.pstate) & 0xf0000000u;
  auto* fp = find_record<FpsimdRecord>(machine, kFpsimdMagic);
  if (fp) {
    context->fpsr = fp->fpsr;
    context->fpcr = fp->fpcr;
    std::memcpy(context->q, fp->v, sizeof fp->v);
    fp->fpcr = static_cast<std::uint32_t>(context->host_fpcr);
  }
  switch (signal) {
    case SIGSYS: {
      // The pc is past the SVC; its immediate says which.
      std::uint32_t svc;
      std::memcpy(&svc, reinterpret_cast<const void*>(machine.pc - 4), 4);
      context->svc_immediate = (svc >> 5) & 0xffffu;
      cpu.signal_exit = CpuExit::Svc;
      break;
    }
    case SIGSEGV:
    case SIGBUS: {
      const auto address = reinterpret_cast<std::uintptr_t>(info->si_addr);
      bool write = false;
      if (auto* esr = find_record<EsrRecord>(machine, kEsrMagic); esr && ((esr->esr >> 26) & 0x3f) == 0x24)
        write = (esr->esr >> 6) & 1;  // data abort from EL0: WnR
      cpu.fault_ = {address, write, address == machine.pc};
      cpu.signal_exit = CpuExit::MemoryFault;
      break;
    }
    case SIGILL: {
      // HLT is undefined at EL0; it is a breakpoint, as for the translator.
      std::uint32_t instruction;
      std::memcpy(&instruction, reinterpret_cast<const void*>(machine.pc), 4);
      cpu.signal_exit = (instruction & 0xffe0001fu) == 0xd4400000u ? CpuExit::Breakpoint : CpuExit::UndefinedInstruction;
      break;
    }
    case SIGTRAP: cpu.signal_exit = CpuExit::Breakpoint; break;
    default: cpu.signal_exit = CpuExit::HaltRequested; break;
  }
  write_tpidr(context->host_tpidr);
  // Return into venture_native_exit on the host stack; x20-x28 and q8-q31 still
  // hold the guest's values, which it stores.
  machine.regs[0] = kExitSignal;
  machine.regs[18] = context->host_x18;
  machine.regs[19] = reinterpret_cast<std::uint64_t>(context);
  machine.sp = context->host_sp;
  machine.pc = reinterpret_cast<std::uint64_t>(&venture_native_exit);
}

bool unclaimed(const struct sigaction& action) {
  return !(action.sa_flags & SA_SIGINFO) && action.sa_handler == SIG_DFL;
}

void install_signals() {
  struct sigaction action{};
  action.sa_sigaction = guest_signal;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
  sigemptyset(&action.sa_mask);
  std::lock_guard lock(host_actions_mutex);
  // Snapshot every previous action before ours can run.
  for (int signal : kSignals) {
    struct sigaction current{};
    real_sigaction(signal, nullptr, &current);
    host_actions[signal].set(current);
  }
  for (int signal : kSignals)
    if (real_sigaction(signal, &action, nullptr)) throw std::runtime_error("native-a64: cannot install a signal handler");
  // The halt signal: a real-time signal nobody handles, from the low end
  // (sampling claims from the high end).
  for (int signal = SIGRTMIN; signal <= SIGRTMAX; ++signal) {
    struct sigaction current{};
    if (real_sigaction(signal, nullptr, &current) || !unclaimed(current)) continue;
    host_actions[signal].set(current);
    if (real_sigaction(signal, &action, nullptr)) continue;
    halt_signal_number = signal;
    break;
  }
  if (!halt_signal_number) throw std::runtime_error("native-a64: no free real-time signal to halt guest code");
}

// SECCOMP_RET_TRAP for system calls made from any of the ranges; everything
// else, including every other architecture's calls, is allowed. Each range is
// one 64-bit begin <= ip <= last compare on the two 32-bit halves.
bool install_filter(const std::vector<GuestRange>& ranges) {
  const auto lo = [](std::uint64_t v) { return static_cast<std::uint32_t>(v); };
  const auto hi = [](std::uint64_t v) { return static_cast<std::uint32_t>(v >> 32); };
  constexpr std::uint32_t ip_lo = offsetof(seccomp_data, instruction_pointer);
  constexpr std::uint32_t ip_hi = ip_lo + 4;
  constexpr std::size_t block = 10;
  const std::size_t allow = 2 + block * ranges.size(), trap = allow + 1;
  std::vector<sock_filter> program;
  const auto load = [&](std::uint32_t offset) { program.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offset)); };
  // Jump targets are absolute instruction indices.
  const auto jump = [&](std::uint16_t op, std::uint32_t k, std::size_t yes, std::size_t no) {
    const auto here = program.size() + 1;
    program.push_back(BPF_JUMP(BPF_JMP | op | BPF_K, k, static_cast<std::uint8_t>(yes - here),
                               static_cast<std::uint8_t>(no - here)));
  };
  load(offsetof(seccomp_data, arch));
  jump(BPF_JEQ, AUDIT_ARCH_AARCH64, 2, allow);
  for (const auto& range : ranges) {
    const auto base = program.size(), next = base + block, last = static_cast<std::uint64_t>(range.end) - 1;
    load(ip_hi);                                       // base + 0
    jump(BPF_JGT, hi(range.begin), base + 5, base + 2);
    jump(BPF_JEQ, hi(range.begin), base + 3, next);
    load(ip_lo);                                       // base + 3
    jump(BPF_JGE, lo(range.begin), base + 5, next);
    load(ip_hi);                                       // base + 5: ip >= begin
    jump(BPF_JGT, hi(last), next, base + 7);
    jump(BPF_JEQ, hi(last), base + 8, trap);
    load(ip_lo);                                       // base + 8
    jump(BPF_JGT, lo(last), next, trap);
  }
  program.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
  program.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP));
  sock_fprog filter{static_cast<unsigned short>(program.size()), program.data()};
  // Filters stack and none can be changed or removed; every thread gets this
  // one too (TSYNC). Each later filter adds ranges to the first one's.
  return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
         syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &filter) == 0;
}

class NativeA64Backend final : public CpuBackend {
 public:
  NativeA64Backend() {
    const auto [begin, end] = AddressSpace::get().arena64();
    if (!begin) throw std::runtime_error("native-a64: no A64 guest arena");
    guest_heap::select64();
    const auto [heap_begin, heap_end] = guest_heap::arena64();
    std::vector<GuestRange> ranges{{begin, end}};
    if (heap_begin) ranges.push_back({heap_begin, heap_end});
    std::copy(ranges.begin(), ranges.end(), guest_ranges);
    install_signals();
    start_guarding();
    if (!install_filter(ranges)) throw std::runtime_error("native-a64: cannot install the guest system call filter");
    guest_range_count.store(ranges.size(), std::memory_order_release);
    set_native_dispatcher(dispatch);
    info_.name = "native-a64";
    info_.hwcap = getauxval(AT_HWCAP);
    info_.hwcap2 = getauxval(AT_HWCAP2);
    log::info("cpu", "native-a64: guest code in [{:#x}, {:#x}) and heap [{:#x}, {:#x}) runs directly; system calls from it trap, halt signal {}",
              begin, end, heap_begin, heap_end, halt_signal_number);
  }
  const CpuBackendInfo& info() const override { return info_; }
  std::unique_ptr<Cpu> create_cpu() override { return std::make_unique<NativeA64Cpu>(); }
  bool prepare_executable(GuestAddr address, std::uint64_t size) override {
    if (!size) return true;
    std::lock_guard lock(mutex_);
    // The parts no range covers, widened to whole reservations (the guest
    // owns those addresses for good), then cut back to what is uncovered.
    const auto granularity = platform::reservation_granularity();
    std::vector<GuestRange> missing{{(address / granularity) * granularity,
                                     ((address + size + granularity - 1) / granularity) * granularity}};
    const auto count = guest_range_count.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < count; ++i) {
      const auto& covered = guest_ranges[i];
      std::vector<GuestRange> rest;
      for (const auto& piece : missing) {
        if (covered.end <= piece.begin || covered.begin >= piece.end) { rest.push_back(piece); continue; }
        if (piece.begin < covered.begin) rest.push_back({piece.begin, covered.begin});
        if (covered.end < piece.end) rest.push_back({covered.end, piece.end});
      }
      missing = std::move(rest);
    }
    if (missing.empty()) return true;
    if (count + missing.size() > kMaxGuestRanges || !install_filter(missing)) {
      log::error("cpu", "native-a64: cannot trap system calls from guest code at [{:#x}, {:#x}); refusing to make it executable",
                 address, address + size);
      return false;
    }
    std::copy(missing.begin(), missing.end(), guest_ranges + count);
    guest_range_count.store(count + missing.size(), std::memory_order_release);
    for (const auto& piece : missing)
      log::info("cpu", "native-a64: guest code range [{:#x}, {:#x}) added", piece.begin, piece.end);
    return true;
  }
  void invalidate(GuestAddr address, std::uint64_t size, InvalidationCause cause) override {
    // Venture wrote guest code (a loaded image, a patch): make the instruction
    // stream see it. Gone or inaccessible ranges have nothing to synchronize.
    if (cause == InvalidationCause::unmap || cause == InvalidationCause::advise || !size) return;
    if (!AddressSpace::get().is_guest_accessible(address, size, false)) return;
    auto* begin = g2h<char>(address);
    __builtin___clear_cache(begin, begin + size);
  }

 private:
  CpuBackendInfo info_;
  std::mutex mutex_;
};
}  // namespace

CpuBackend& native_a64_backend() {
  static auto* backend = new NativeA64Backend;
  return *backend;
}

}  // namespace venture
#endif
