// Guest signal model (Linux semantics, AArch64 frame layout).
//
// The core owns the action table and delivery: synchronous faults raised by
// the CPU, and asynchronous signals queued by pthread_kill/tgkill/raise/kill.
// Delivery happens on the target guest thread inside its run loop: the
// thread is halted (or its blocking HLE wait is interrupted), an rt_sigframe
// (siginfo_t + ucontext_t with sigcontext/fpsimd_context) is built on the guest
// stack (or the sigaltstack), the handler runs as a guest call with LR pointing
// at a sigreturn trampoline, and the possibly-modified context is restored.
#pragma once

#include <cstdint>
#include <exception>
#include <string_view>

#include "core/guest.h"

namespace venture {
class GuestThread;
}

namespace venture::signals {

inline constexpr int kMaxSignal = 64;

// Linux signal numbers used by the core itself.
enum : int {
  kSigIll = 4, kSigTrap = 5, kSigAbrt = 6, kSigBus = 7, kSigFpe = 8, kSigKill = 9,
  kSigSegv = 11, kSigPipe = 13, kSigStkflt = 16, kSigChld = 17, kSigStop = 19, kSigSys = 31,
  kBionicSignalDebugger = 35,  // bionic platform/bionic/reserved_signals.h
};

// A synchronous fault redelivered this many times in a row to a handler that
// returned to the same instruction with identical registers, without anything
// repairing it, takes the default action instead of spinning forever.
inline constexpr unsigned kMaxUnresolvedFaults = 1000;

// Host-side representation of a guest sigaction.
struct Action {
  GuestAddr handler = 0;  // SIG_DFL = 0, SIG_IGN = 1, else guest function
  std::uint64_t mask = 0;
  std::uint32_t flags = 0;  // Linux SA_* bits (SA_SIGINFO, SA_ONSTACK, SA_RESTART, SA_NODEFER, SA_RESETHAND)
};

struct Info {  // the parts of siginfo_t Venture fills in
  int signo = 0;
  int code = 0;
  GuestAddr fault_address = 0;
  std::int32_t sender_pid = 0;
  std::int32_t sender_uid = 0;
  std::uint64_t value = 0;  // sigqueue sival
  std::uint64_t syndrome = 0; // ESR_EL1, when a synchronous fault supplies it
};

// Returns 0 or a negative Linux errno.
int set_action(int signo, const Action* action, Action* old_action);

// Android's dynamic linker runs debuggerd_init() before any app code
// (system/core/debuggerd/handler/debuggerd_handler.cpp): SIGABRT, SIGBUS,
// SIGFPE, SIGILL, SIGSEGV, SIGSTKFLT, SIGSYS, SIGTRAP and the bionic debugger
// signal get debuggerd_signal_handler with a full mask and SA_RESTART |
// SA_SIGINFO | SA_ONSTACK | SA_EXPOSE_TAGBITS. Engine crash reporters save it
// as their old action and chain to it. The handler logs a crash report, resets
// the signal to SIG_DFL and requeues it to the thread, so the default action
// terminates the process once the handler chain returns. The handler is a
// host function exposed at a guest-callable address for the process ISA.
void install_debuggerd_handlers();
GuestAddr debuggerd_handler();  // 0 before install_debuggerd_handlers()

// Queue a signal for a specific thread (pthread_kill/tgkill) or the process
// (kill/raise picks a thread that does not block it). Thread-safe.
int send_to_thread(std::int32_t tid, const Info& info);
int send_to_process(const Info& info);

// Called on the faulting thread by the run loop. If the guest has no
// handler, logs a crash report and terminates the process (Android behaviour).
void deliver_synchronous(GuestThread& thread, const Info& info);

// Diagnostic (VENTURE_TRACE_THREADS): logs every guest thread's registers and
// frame-pointer backtrace. Threads blocked in HLE calls report where they
// wait; a thread running translated code reports its last saved state.
void log_guest_threads(std::string_view reason);

// Host crash reports (the logger is terminal by then): the registers and frames
// of `thread` when host code it called faulted, such as an HLE libc call given
// a bad pointer, so the report names the guest caller. Null does nothing.
void write_host_fault_guest_frames(GuestThread* thread) noexcept;

// A host exception unwinding out of a guest call restores the caller's
// registers. The innermost call notes them first (once per exception object),
// so a thread that dies of the exception can still say where it was.
void note_unwinding_guest_call(GuestThread& thread) noexcept;
// Logs a guest thread stopped by a host exception: the error, and the guest
// registers and frames noted for that exception.
void log_guest_thread_failure(GuestThread& thread, std::string_view name, const std::exception_ptr& failure) noexcept;

// Called by the run loop and by interruptible waits: runs handlers for any
// deliverable pending signals of `thread`. Returns true if a handler ran
// (blocking calls without SA_RESTART should then fail with EINTR).
bool deliver_pending(GuestThread& thread);

// Signal masks (sigprocmask/pthread_sigmask) act on the calling thread.
int set_mask(GuestThread& thread, int how, const std::uint64_t* set, std::uint64_t* old_set);

// sigaltstack(2)
struct AltStack {
  GuestAddr base = 0;
  std::uint64_t size = 0;
  int flags = 0;  // SS_ONSTACK / SS_DISABLE
};
int set_alt_stack(GuestThread& thread, const AltStack* stack, AltStack* old_stack);

// Atomically consume a pending signal in `set`, preserving its queued Info.
// Does not invoke that signal's handler. Interruptible through core/wait.h;
// deadline is absolute monotonic nanoseconds, -1 means infinite. Returns the
// signal number, -EAGAIN on timeout, or another negative Linux errno.
// Used by libc sigwait/sigtimedwait and raw rt_sigtimedwait.
int wait_signal(GuestThread& thread, std::uint64_t set, Info* info, std::int64_t deadline_ns);

}  // namespace venture::signals
