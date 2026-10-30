#include "core/pc_trace.h"
#include "common/environment.h"
#include "core/signals.h"
#include "core/api_result_trace.h"
#include "core/startup_stack.h"
#include "common/first_frame_progress.h"
#include "platform/runtime_child.h"
#include "core/signal_frame.h"
#include "core/a32_abi.h"
#include "core/a32_unwind.h"
#include "core/hle_unwind.h"
#include "core/address_space.h"
#include "core/error.h"
#include "core/wait.h"
#include "core/hle_runtime.h"
#include "core/linker.h"
#include "core/process.h"
#include "core/thread_runtime.h"
#include "core/thread_api.h"
#include "platform/memory.h"
#include "common/log.h"
#include <array>
#include <bit>
#include <cerrno>
#include <limits>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

namespace venture::signals {
namespace {
constexpr std::uint32_t sa_siginfo = 4, sa_expose_tagbits = 0x800, sa_onstack = 0x08000000,
                        sa_restart = 0x10000000, sa_nodefer = 0x40000000, sa_resethand = 0x80000000;
constexpr std::uint64_t bit(int n) { return 1ull << (n - 1); }
constexpr auto unmaskable = bit(kSigKill) | bit(kSigStop);
constexpr std::int32_t guest_pid = 1000, guest_uid = 10125;
std::mutex mutex;
std::array<Action, 65> actions{};
// The last synchronous fault delivered to a guest handler on a thread.
struct LastFault {
  int signo = 0, code = 0;
  GuestAddr pc = 0, address = 0;
  std::array<std::uint64_t, 33> registers{};  // X0..X30 (r0..r15), SP, PSTATE
  bool resumed = false;  // the handler returned to the faulting instruction
  unsigned count = 0;    // consecutive identical deliveries
  std::uint64_t exits = 0;  // thread CPU exits when the fault was taken, then when the handler returned
};
struct ThreadSignals {
  AltStack alt{0, 0, 2}; std::array<Info, 65> infos{};
  LastFault fault; int reported = 0;  // signal whose crash report debuggerd logged
};
std::map<GuestThread*, ThreadSignals> states;
std::atomic<GuestAddr> debuggerd_address{0};
thread_local bool last_restart = false;
std::atomic<bool> process_stopped{false};
std::mutex stop_mutex;
std::condition_variable continued;
bool default_ignored(int signal) { return signal == 17 || signal == 18 || signal == 23 || signal == 28; }
void wait_stopped(GuestThread& thread) {
  std::unique_lock lock(stop_mutex);
  continued.wait(lock, [&] { return !process_stopped.load() || thread.exit_requested.load() ||
      (thread.pending_signals.load() & bit(kSigKill)); });
  check_guest_exit(thread);
}
bool valid(int n) { return n > 0 && n <= 64; }
bool on_alt(GuestThread& thread, const AltStack& alt) {
  // Linux include/linux/sched/signal.h:on_sig_stack, downward-growing stacks:
  // SP is the next free slot. An adjacent ordinary stack may end at alt.base.
  auto sp = thread.cpu().sp(); return !(alt.flags & 2) && sp > alt.base && sp - alt.base <= alt.size;
}
// Registers a backtrace starts from: the live CPU or a saved ucontext.
struct Frame {
  GuestIsa isa = GuestIsa::a64;
  GuestAddr pc = 0, lr = 0, fp = 0;
  std::array<std::uint32_t, 16> arm{};
  // A64 X0..X30, SP and PSTATE at the fault, for the crash report's register block.
  std::array<std::uint64_t, 31> x{};
  std::uint64_t sp = 0, pstate = 0;
  bool registers = false;
};
Frame cpu_frame(Cpu& cpu) {
  Frame frame{cpu.isa(), cpu.pc(), cpu.lr(), cpu.frame_pointer(), {}};
  if (frame.isa == GuestIsa::a32) for (unsigned i = 0; i < 16; ++i) frame.arm[i] = static_cast<std::uint32_t>(cpu.x(i));
  else for (unsigned i = 0; i < 31; ++i) frame.x[i] = cpu.x(i);
  frame.sp = cpu.sp(); frame.pstate = cpu.pstate(); frame.registers = true;
  return frame;
}
// Host crash reports write guest frames after the logger turned terminal.
bool crash_output = false;
template <class... A> void report(std::format_string<A...> format, A&&... args) {
  if (!crash_output) return log::error("signal", format, std::forward<A>(args)...);
  const auto line = " E [signal] " + std::format(format, std::forward<A>(args)...) + "\n";
  log::crash_write(line.data(), line.size());
}
// debuggerd's tombstone register block (libdebuggerd/tombstone.cpp print_thread_registers): four per line.
void log_registers(const Frame& frame) {
  if (!frame.registers) return;
  std::string line;
  const auto add = [&](std::string_view name, std::uint64_t value, int digits) {
    line += std::format("  {:<4}{:0{}x}", name, value, digits);
    if (line.size() > 4 * (6 + digits) - 1) { report("  {}", line); line.clear(); }
  };
  if (frame.isa == GuestIsa::a32) {
    constexpr const char* names[] = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "ip", "sp", "lr", "pc"};
    for (unsigned i = 0; i < 16; ++i) add(names[i], frame.arm[i], 8);
    add("cpsr", frame.pstate, 8);
  } else {
    for (unsigned i = 0; i < 29; ++i) add(std::format("x{}", i), frame.x[i], 16);
    add("x29", frame.x[29], 16); add("lr", frame.x[30], 16); add("sp", frame.sp, 16); add("pc", frame.pc, 16);
    add("pst", frame.pstate, 16);
  }
  if (!line.empty()) report("  {}", line);
}
// Diagnostic: VENTURE_FAULT_MEMORY="x19+0x40:0x100,sp:0x80" dumps guest memory
// relative to a fault register (xN, sp, pc; rN on A32), 16 bytes per line.
void log_fault_memory(const Frame& frame) {
  const auto* spec = venture::environment::getenv("VENTURE_FAULT_MEMORY");
  if (!spec || !frame.registers) return;
  struct RestoreErrno { int value = errno; ~RestoreErrno() { errno = value; } } restore_errno;
  // Optional one-level indirection: register+offset@field-offset:length.
  // Offsets after @ are signed, e.g. x1@+0x8:0x10. No guest execution,
  // pointer chasing, or unbounded reads; failed reads remain diagnostic only.
  const auto signed_offset = [](std::string_view text, std::int64_t& result) {
    if (text.empty()) return false;
    const std::string value(text);
    char* end = nullptr;
    errno = 0;
    result = std::strtoll(value.c_str(), &end, 0);
    return errno != ERANGE && end == value.c_str() + value.size();
  };
  const auto add_offset = [](std::uint64_t base, std::int64_t offset, std::uint64_t& result) {
    if (offset >= 0) {
      const auto magnitude = static_cast<std::uint64_t>(offset);
      if (base > std::numeric_limits<std::uint64_t>::max() - magnitude) return false;
      result = base + magnitude;
    } else {
      const auto magnitude = static_cast<std::uint64_t>(-(offset + 1)) + 1;
      if (base < magnitude) return false;
      result = base - magnitude;
    }
    return true;
  };
  std::string_view rest(spec);
  while (!rest.empty()) {
    const auto comma = rest.find(',');
    const auto item = rest.substr(0, comma);
    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    const auto colon = item.find(':');
    const auto indirect = item.find('@');
    const auto direct = item.substr(0, std::min(indirect, colon));
    const auto plus = direct.find('+');
    const auto base = direct.substr(0, plus);
    std::uint64_t value = 0;
    if (base == "sp") value = (frame.isa == GuestIsa::a32) ? frame.arm[13] : frame.sp;
    else if (base == "pc") value = frame.pc;
    else if (base.size() > 1 && (base[0] == 'x' || base[0] == 'r')) {
      const std::string number(base.substr(1));
      char* end = nullptr;
      const auto n = std::strtoul(number.c_str(), &end, 10);
      if (indirect != std::string_view::npos &&
          (end != number.c_str() + number.size() || n >= (frame.isa == GuestIsa::a32 ? 16u : 31u))) {
        log::error("signal", "  fault memory: bad indirect register"); continue;
      }
      value = (frame.isa == GuestIsa::a32) ? (n < 16 ? frame.arm[n] : 0) : (n < 31 ? frame.x[n] : 0);
    } else { log::error("signal", "  fault memory: bad register in '{}'", item); continue; }
    const auto offset = plus != std::string_view::npos ?
        std::strtoull(std::string(direct.substr(plus + 1)).c_str(), nullptr, 0) : 0;
    const auto length = std::min<std::uint64_t>(colon == std::string_view::npos ? 0x100 :
        std::strtoull(std::string(item.substr(colon + 1)).c_str(), nullptr, 0), 0x4000);
    std::uint64_t address = value + offset;
    if (indirect != std::string_view::npos) {
      if (colon != std::string_view::npos) {
        const std::string count(item.substr(colon + 1));
        char* end = nullptr;
        errno = 0;
        std::strtoull(count.c_str(), &end, 0);
        if (count.empty() || count.front() == '-' || errno == ERANGE ||
            end != count.c_str() + count.size()) {
          log::error("signal", "  fault memory: invalid indirect length"); continue;
        }
      }
      std::int64_t location_offset = 0, field_offset = 0;
      if (indirect >= colon || item.find('@', indirect + 1) != std::string_view::npos ||
          (plus != std::string_view::npos && !signed_offset(direct.substr(plus + 1), location_offset)) ||
          !signed_offset(item.substr(indirect + 1, colon - indirect - 1), field_offset) ||
          !add_offset(value, location_offset, address)) {
        log::error("signal", "  fault memory: invalid indirect expression"); continue;
      }
      std::uint64_t pointer = 0;
      bool readable;
      if (frame.isa == GuestIsa::a32) {
        std::uint32_t pointer32 = 0;
        readable = platform::read_memory(address, &pointer32, sizeof(pointer32));
        pointer = pointer32;
      } else {
        readable = platform::read_memory(address, &pointer, sizeof(pointer));
      }
      if (!readable) { log::error("signal", "  fault memory: indirect pointer unreadable"); continue; }
      if (!pointer) { log::error("signal", "  fault memory: null indirect pointer"); continue; }
      if (!add_offset(pointer, field_offset, address) ||
          address > std::numeric_limits<std::uint64_t>::max() - length) {
        log::error("signal", "  fault memory: indirect range overflow"); continue;
      }
      log::error("signal", "  indirect memory {} {:#x} -> {:#x}, {:#x} bytes:", base, value, address, length);
    } else {
      log::error("signal", "  memory {} {:#x}+{:#x}, {:#x} bytes:", base, value, offset, length);
    }
    for (std::uint64_t done = 0; done < length; done += 16) {
      const auto at = address + done;
      if (at < address) break;
      std::array<std::uint64_t, 2> row{};
      const auto bytes = std::min<std::uint64_t>(sizeof(row), length - done);
      if (!platform::read_memory(at, row.data(), bytes)) { log::error("signal", "    {:016x} unreadable", at); break; }
      log::error("signal", "    {:016x} {:016x} {:016x}", at, row[0], row[1]);
    }
  }
}
void log_backtrace(Frame frame) {
  auto pc = frame.pc, fp = frame.fp;
  auto& arm = frame.arm;
  for (int i = 0; i < 48; ++i) {
    DlInfo symbol;
    Linker::get().try_address_info(pc, &symbol);
    report("  #{:02} {:#x} {}+{:#x} ({}+{:#x})", i, pc, symbol.path, pc - symbol.base, symbol.symbol,
           pc - (symbol.symbol_address ? symbol.symbol_address : symbol.base));
    if (frame.isa == GuestIsa::a32) {
      std::int32_t count = 0;
      auto table = Linker::get().try_find_exidx(pc, &count);
      // HLE trampolines are leaf calls without ELF unwind tables.
      if (!table && i == 0 && arm[14] && arm[14] != arm[15]) {
        arm[15] = arm[14]; pc = arm[15]; continue;
      }
      if (auto error = unwind_a32_frame(arm, table, count, i != 0)) {
        report("  exidx unwind stopped: {}", error); break;
      }
      if (!arm[15]) break;
      pc = arm[15]; continue;
    }
    if (i == 0 && frame.lr && frame.lr != pc) { pc = frame.lr; continue; }
    std::array<std::uint64_t, 2> record{};
    if (!fp || (fp & 15) || !platform::read_memory(fp, record.data(), sizeof(record)) || record[0] <= fp) break;
    fp = record[0]; pc = record[1];
  }
}
void fault_summary(GuestThread& thread, const Info& info, std::string_view outcome = {}) {
  log::error("signal", "guest tid {} signal {} code {} at PC {:#x}, fault {:#x}{}",
             thread.tid(), info.signo, info.code, thread.cpu().pc(), info.fault_address, outcome);
}
void trace_fault(GuestThread& thread, const Info& info) {
  pc_trace::freeze();
  fault_summary(thread, info);
  // Capture before signal-frame construction or a chained handler can overwrite
  // memory belonging to the interrupted context. The observer is opt-in.
  log_fault_memory(cpu_frame(thread.cpu()));
  log_backtrace(cpu_frame(thread.cpu()));
}
[[noreturn]] void terminate(GuestThread& thread, const Info& info) {
  // Latch before diagnostics, which may block while other threads exit.
  record_guest_process_failure(128 + info.signo);
  // Normal fault summaries must precede crash_write, which makes the logger
  // terminal. The PC observer already froze its first relevant fault state.
  bool reported;
  { std::lock_guard lock(mutex); reported = states[&thread].reported == info.signo; }
  // debuggerd already logged the backtrace; keep the one-line summary.
  if (reported) fault_summary(thread, info, ", terminated by the default action");
  else trace_fault(thread, info);
  api_result_trace::flush();
  pc_trace::flush();
  if (venture::environment::getenv("VENTURE_TRACE_THREADS")) log_guest_threads("fatal signal");
  exit_guest_process(128 + info.signo);
}
// debuggerd/util.cpp signal_has_si_addr: manually sent signals carry a sender instead.
bool has_fault_address(const Info& info) {
  if (info.code == 0 || info.code == -1 || info.code == -6) return false;  // SI_USER, SI_QUEUE, SI_TKILL
  switch (info.signo) {
    case kSigBus: case kSigFpe: case kSigIll: case kSigTrap: return true;
    case kSigSegv: return info.code != 8;  // SEGV_MTEAERR
    default: return false;
  }
}
std::string_view signal_name(int signo) {
  switch (signo) {
    case kSigIll: return "SIGILL"; case kSigTrap: return "SIGTRAP"; case kSigAbrt: return "SIGABRT";
    case kSigBus: return "SIGBUS"; case kSigFpe: return "SIGFPE"; case kSigSegv: return "SIGSEGV";
    case kSigStkflt: return "SIGSTKFLT"; case kSigSys: return "SIGSYS";
    default: return "?";
  }
}
// debuggerd/util.cpp get_sigcode (asm-generic/siginfo.h values).
std::string_view code_name(const Info& info) {
  static constexpr std::string_view ill[] = {"ILL_ILLOPC", "ILL_ILLOPN", "ILL_ILLADR", "ILL_ILLTRP",
                                             "ILL_PRVOPC", "ILL_PRVREG", "ILL_COPROC", "ILL_BADSTK"};
  static constexpr std::string_view fpe[] = {"FPE_INTDIV", "FPE_INTOVF", "FPE_FLTDIV", "FPE_FLTOVF",
                                             "FPE_FLTUND", "FPE_FLTRES", "FPE_FLTINV", "FPE_FLTSUB"};
  static constexpr std::string_view segv[] = {"SEGV_MAPERR", "SEGV_ACCERR", "SEGV_BNDERR", "SEGV_PKUERR",
                                              "?", "?", "?", "SEGV_MTEAERR", "SEGV_MTESERR"};
  static constexpr std::string_view bus[] = {"BUS_ADRALN", "BUS_ADRERR", "BUS_OBJERR", "BUS_MCEERR_AR", "BUS_MCEERR_AO"};
  static constexpr std::string_view trap[] = {"TRAP_BRKPT", "TRAP_TRACE", "TRAP_BRANCH", "TRAP_HWBKPT"};
  auto pick = [&](std::span<const std::string_view> names) {
    return info.code >= 1 && static_cast<std::size_t>(info.code) <= names.size() ? names[info.code - 1] : std::string_view{};
  };
  std::string_view name;
  switch (info.signo) {
    case kSigIll: name = pick(ill); break; case kSigFpe: name = pick(fpe); break;
    case kSigSegv: name = pick(segv); break; case kSigBus: name = pick(bus); break;
    case kSigTrap: name = pick(trap); break; case kSigSys: if (info.code == 1) name = "SYS_SECCOMP"; break;
  }
  if (!name.empty()) return name;
  switch (info.code) {
    case 0: return "SI_USER"; case 0x80: return "SI_KERNEL"; case -1: return "SI_QUEUE";
    case -2: return "SI_TIMER"; case -6: return "SI_TKILL";
    default: return "?";
  }
}
// debuggerd_handler.cpp log_signal_summary, followed by the faulting backtrace.
void report_crash(GuestThread& thread, const Info& info, const Frame& frame) {
  if (info.signo == kBionicSignalDebugger) {
    log::info("libc", "Requested dump for pid {} ({})", guest_pid, process_config().package);
    log_backtrace(frame);
    return;
  }
  pc_trace::freeze();
  std::string sender, extra;
  if (info.code <= 0 && info.sender_pid && info.sender_pid != guest_pid)
    sender = std::format(" from pid {}, uid {}", info.sender_pid, info.sender_uid);
  if (has_fault_address(info)) extra = std::format(", fault addr {:#x}", info.fault_address);
  log::error("libc", "Fatal signal {} ({}), code {} ({}{}){} in tid {}, pid {} ({})", info.signo,
             signal_name(info.signo), info.code, code_name(info), sender, extra, thread.tid(), guest_pid,
             process_config().package);
  log_registers(frame);
  log_fault_memory(frame);
  log_backtrace(frame);
  std::lock_guard lock(mutex);
  states[&thread].reported = info.signo;
}
// debuggerd_handler.cpp resend_signal: signal(sig, SIG_DFL), then
// rt_tgsigqueueinfo to the crashing thread with the original siginfo.
void resend_signal(GuestThread& thread, const Info& info) {
  if (info.signo == kBionicSignalDebugger) return;
  const Action default_action{0, 0, sa_restart};
  set_action(info.signo, &default_action, nullptr);
  send_to_thread(thread.tid(), info);
}
// Linux arch/arm/include/uapi/asm/{sigcontext,ucontext}.h (ILP32 ucontext prefix).
struct ArmSigContext {
  std::uint32_t trap_no, error_code, oldmask;
  std::array<std::uint32_t, 16> regs;  // r0..r10, fp, ip, sp, lr, pc
  std::uint32_t cpsr, fault_address;
};
struct ArmUContextHead { std::uint32_t flags, link, ss_sp; std::int32_t ss_flags; std::uint32_t ss_size; ArmSigContext mcontext; };
static_assert(offsetof(ArmUContextHead, mcontext) == 20 && sizeof(ArmUContextHead) == 104);
// Guest-callable debuggerd_signal_handler(int, siginfo_t*, void*). Crash
// reporters reach it by calling the old action they saved.
void debuggerd_signal_handler(hle::CallContext& ctx) {
  auto& thread = ctx.thread();
  auto& cpu = ctx.cpu();
  const bool a32 = cpu.isa() == GuestIsa::a32;
  const auto pointer = [&](int i) -> GuestAddr { return a32 ? ctx.x(i) & UINT32_MAX : ctx.x(i); };
  const int signo = static_cast<std::int32_t>(ctx.x(0));
  Action current;
  if (set_action(signo, nullptr, &current)) {
    // bionic: rt_tgsigqueueinfo fails, and fatal_errno() calls _exit(1).
    log::error("libc", "failed to resend signal during crash: invalid signal {}", signo);
    exit_guest_process(1);
  }
  Info info{signo, 0, 0, guest_pid, guest_uid, 0};  // the SI_USER dummy_info
  Frame frame = cpu_frame(cpu);
  // have_siginfo(): without SA_SIGINFO on the current action, info and
  // context are undefined.
  if ((current.flags & sa_siginfo) && pointer(1)) {
    std::array<std::byte, 128> raw{};
    if (platform::read_memory(pointer(1), raw.data(), raw.size())) {
      const std::size_t fields = a32 ? 12 : 16;  // the siginfo union
      std::memcpy(&info.code, raw.data() + 8, 4);
      if (has_fault_address(info)) {
        std::memcpy(&info.fault_address, raw.data() + fields, a32 ? 4 : 8);
      } else {
        std::memcpy(&info.sender_pid, raw.data() + fields, 4);
        std::memcpy(&info.sender_uid, raw.data() + fields + 4, 4);
        std::memcpy(&info.value, raw.data() + fields + 8, a32 ? 4 : 8);
      }
    }
    if (a32) {
      ArmUContextHead uc;
      if (pointer(2) && platform::read_memory(pointer(2), &uc, sizeof(uc))) {
        const auto& r = uc.mcontext.regs;
        frame = {GuestIsa::a32, r[15], r[14], (uc.mcontext.cpsr & 0x20) ? r[7] : r[11], r};
        frame.sp = r[13]; frame.pstate = uc.mcontext.cpsr; frame.registers = true;
      }
    } else {
      SigContext sc;
      constexpr auto bytes = offsetof(SigContext, reserved);
      if (pointer(2) && platform::read_memory(pointer(2) + offsetof(UContext, mcontext), &sc, bytes)) {
        frame = {GuestIsa::a64, sc.pc, sc.regs[30], sc.regs[29], {}};
        frame.x = sc.regs; frame.sp = sc.sp; frame.pstate = sc.pstate; frame.registers = true;
      }
    }
  }
  report_crash(thread, info, frame);
  resend_signal(thread, info);
}
// Records a synchronous fault delivered to a guest handler and returns how many
// consecutive deliveries found the same instruction, address and registers
// right after the handler resumed it, with no other CPU exit in between. Guest
// code that makes progress (any HLE call, syscall or interrupt) and reaches the
// same fault again starts a new count.
unsigned record_fault(GuestThread& thread, const Info& info) {
  auto& cpu = thread.cpu();
  LastFault fault{info.signo, info.code, cpu.pc(), info.fault_address, {}, false, 1, thread.cpu_exits};
  for (int i = 0; i < (cpu.isa() == GuestIsa::a32 ? 16 : 31); ++i) fault.registers[i] = cpu.x(i);
  fault.registers[31] = cpu.sp(); fault.registers[32] = cpu.pstate();
  std::lock_guard lock(mutex);
  auto& last = states[&thread].fault;
  if (last.resumed && last.exits + 1 == fault.exits && last.signo == fault.signo && last.code == fault.code &&
      last.pc == fault.pc && last.address == fault.address && last.registers == fault.registers)
    fault.count = last.count + 1;
  last = fault;
  return fault.count;
}
GuestAddr sigreturn_trampoline(bool realtime = true, bool leaf = false) {
  if (leaf && process_config().isa() == GuestIsa::a32) {
    static auto rt_leaf = hle::code_stub(GuestStub::sigreturn, "rt_sigreturn leaf", 3);
    static auto old_leaf = hle::code_stub(GuestStub::sigreturn, "sigreturn leaf", 2);
    return realtime ? rt_leaf : old_leaf;
  }
  static auto rt = hle::code_stub(GuestStub::sigreturn, "rt_sigreturn", 1);
  if (realtime || process_config().isa() == GuestIsa::a64) return rt;
  static auto old = hle::code_stub(GuestStub::sigreturn, "sigreturn", 0);
  return old;
}
bool deliver(GuestThread& thread, const Info& info, bool synchronous) {
  Action action;
  AltStack alt;
  {
    std::lock_guard lock(mutex);
    action = actions[info.signo]; alt = states[&thread].alt;
    // Linux get_signal: SA_RESETHAND (SA_ONESHOT) resets only sa_handler;
    // sa_flags and sa_mask stay, so a chained handler still sees SA_SIGINFO.
    if (action.flags & sa_resethand) actions[info.signo].handler = 0;
  }
  if (synchronous && (thread.blocked_signals & bit(info.signo))) terminate(thread, info);
  if (action.handler == 1 && !synchronous && info.signo != kSigKill && info.signo != kSigStop) return false;
  if (!action.handler || action.handler == 1 || info.signo == kSigKill || info.signo == kSigStop) {
    if (!synchronous && default_ignored(info.signo)) return false;
    if (!synchronous && info.signo >= 19 && info.signo <= 22) {
      process_stopped.store(true);
      for_each_guest_thread([&](GuestThread& other) { if (&other != &thread) other.interrupt(); });
      wait_stopped(thread); return false;
    }
    terminate(thread, info);
  }
  auto& cpu = thread.cpu();
  if (action.handler == debuggerd_address.load(std::memory_order_relaxed)) {
    // Delivered straight to debuggerd: the handler would report, reset to
    // SIG_DFL and requeue the signal, whose default action then ends the
    // process. Run the runtime's debuggerd reporter host-side.
    if (info.signo != kBionicSignalDebugger) record_guest_process_failure(128 + info.signo);
    report_crash(thread, info, cpu_frame(cpu));
    if (info.signo == kBionicSignalDebugger) { last_restart = true; return true; }
    resend_signal(thread, info);
    terminate(thread, info);
  }
  const auto fault_pc = cpu.pc();
  if (synchronous) {
    // Bound repeated faults when the guest handler returns without repair.
    const auto count = record_fault(thread, info);
    if (count == 1) trace_fault(thread, info);
    if (count >= kMaxUnresolvedFaults) {
      log::error("signal", "guest tid {} signal {} at PC {:#x} occurred {} times in a row, handler {:#x} never "
                 "resolved it; taking the default action", thread.tid(), info.signo, fault_pc, count, action.handler);
      terminate(thread, info);
    }
  }
  const auto handler_returned = [&] {
    if (synchronous) {
      std::lock_guard lock(mutex);
      auto& last = states[&thread].fault;
      if (last.signo == info.signo && last.pc == fault_pc) {
        last.resumed = cpu.pc() == fault_pc; last.exits = thread.cpu_exits;
      }
    }
    last_restart = action.flags & sa_restart;
  };
  if (cpu.isa() == GuestIsa::a32) {
    const bool realtime = (action.flags & sa_siginfo) != 0;
    RtSigFrame32 frame{};
    auto& uc = frame.sig.context;
    uc.flags = realtime ? 0 : 0x5ac3c35a;
    uc.sigmask = thread.blocked_signals;
    uc.stack = {std::uint32_t(alt.base), on_alt(thread, alt) ? 1 : alt.flags, std::uint32_t(alt.size)};
    auto& sc = uc.mcontext;
    sc.oldmask = std::uint32_t(thread.blocked_signals); sc.fault_address = std::uint32_t(info.fault_address);
    sc.error = (info.syndrome & (1ull << 6)) ? 1u << 11 : 0; // compat FSR WnR
    for (int i = 0; i < 16; ++i) sc.regs[i] = std::uint32_t(cpu.x(i));
    sc.cpsr = cpu.pstate();
    VfpContext32 fp; fp.fpscr = cpu.fpcr();
    for (int i = 0; i < 32; ++i) fp.regs[i] = cpu.v(i).lo;
    std::memcpy(uc.regspace.data(), &fp, sizeof(fp));
    frame.info.signo = info.signo; frame.info.code = info.code;
    if (has_fault_address(info) || synchronous) {
      auto address = std::uint32_t(info.fault_address); std::memcpy(frame.info.fields.data(), &address, 4);
    } else {
      std::memcpy(frame.info.fields.data(), &info.sender_pid, 4);
      std::memcpy(frame.info.fields.data() + 4, &info.sender_uid, 4);
      auto value = std::uint32_t(info.value); std::memcpy(frame.info.fields.data() + 8, &value, 4);
    }
    auto top = ((action.flags & sa_onstack) && !(alt.flags & 2) && !on_alt(thread, alt)) ? alt.base + alt.size : cpu.sp();
    const auto bytes = realtime ? sizeof(frame) : sizeof(frame.sig);
    if (top > (1ull << 32) || top < bytes) terminate(thread, {kSigSegv, 1, top});
    const auto address = align_down(top - bytes, 8);
    if (!platform::write_memory(address, realtime ? static_cast<const void*>(&frame) : &frame.sig, bytes))
      terminate(thread, {kSigSegv, 1, address});
    const auto mask = (thread.blocked_signals | action.mask | ((action.flags & sa_nodefer) ? 0 : bit(info.signo))) & ~unmaskable;
    thread.blocked_signals = mask; thread.signal_mask_snapshot.store(mask);
    cpu.clear_exclusive(); cpu.set_sp(address);
    // Signal entry discards interrupted Thumb IT state and chooses the handler ISA.
    cpu.set_pstate((cpu.pstate() & ~(0x0600fc20u)) | ((action.handler & 1) ? 0x20 : 0));
    cpu.set_pc(action.handler); // set_pc selects ARM/Thumb from the address tag
    cpu.set_x(0, info.signo);
    cpu.set_x(1, realtime ? address : 0);
    cpu.set_x(2, realtime ? address + offsetof(RtSigFrame32, sig) : 0);
    cpu.set_lr(sigreturn_trampoline(realtime, hle::is_unwind_leaf(sc.regs[15])));
    thread.run_until_return(-1);
    handler_returned();
    return true;
  }
  RtSigFrame frame{};
  frame.info.signo = info.signo; frame.info.code = info.code;
  if (info.fault_address || synchronous) std::memcpy(frame.info.fields.data(), &info.fault_address, 8);
  else {
    std::memcpy(frame.info.fields.data(), &info.sender_pid, 4);
    std::memcpy(frame.info.fields.data() + 4, &info.sender_uid, 4);
    std::memcpy(frame.info.fields.data() + 8, &info.value, 8);
  }
  auto& uc = frame.context;
  uc.sigmask = thread.blocked_signals;
  uc.stack = {alt.base, on_alt(thread, alt) ? 1 : alt.flags, 0, alt.size};
  auto& sc = uc.mcontext;
  sc.fault_address = info.fault_address; sc.sp = cpu.sp(); sc.pc = cpu.pc(); sc.pstate = cpu.pstate();
  for (int i = 0; i < 31; ++i) sc.regs[i] = cpu.x(i);
  FpsimdContext fp;
  fp.fpcr = cpu.fpcr(); fp.fpsr = cpu.fpsr();
  for (int i = 0; i < 32; ++i) fp.vregs[i] = cpu.v(i);
  std::memcpy(sc.reserved.data(), &fp, sizeof(fp));
  if (synchronous) {
    EsrContext esr; esr.esr = info.syndrome;
    std::memcpy(sc.reserved.data() + sizeof(fp), &esr, sizeof(esr));
  }
  auto top = ((action.flags & sa_onstack) && !(alt.flags & 2) && !on_alt(thread, alt)) ? alt.base + alt.size : cpu.sp();
  const auto bytes = sizeof(frame) + sizeof(FrameRecord);
  if (top < bytes) terminate(thread, {kSigSegv, 1, top});
  auto address = align_down(top - bytes, 16);
  if (!platform::write_memory(address, &frame, sizeof(frame))) terminate(thread, {kSigSegv, 1, address});
  // Linux places a frame-chain record above rt_sigframe and points x29 at it.
  const FrameRecord record{cpu.x(29), cpu.lr()};
  if (!platform::write_memory(address + sizeof(frame), &record, sizeof(record)))
    terminate(thread, {kSigSegv, 1, address + sizeof(frame)});
  auto mask = (thread.blocked_signals | action.mask | ((action.flags & sa_nodefer) ? 0 : bit(info.signo))) & ~unmaskable;
  thread.blocked_signals = mask; thread.signal_mask_snapshot.store(mask);
  cpu.clear_exclusive(); cpu.set_sp(address); cpu.set_pc(action.handler);
  cpu.set_x(29, address + sizeof(frame));
  cpu.set_x(0, info.signo);
  cpu.set_x(1, (action.flags & sa_siginfo) ? address : 0);
  cpu.set_x(2, (action.flags & sa_siginfo) ? address + offsetof(RtSigFrame, context) : 0);
  cpu.set_lr(sigreturn_trampoline());
  thread.run_until_return(-1);
  handler_returned();
  return true;
}
}
int set_action(int signo, const Action* action, Action* old_action) {
  if (!valid(signo) || (action && (signo == kSigKill || signo == kSigStop))) return -err::kEINVAL;
  std::lock_guard lock(mutex);
  if (old_action) *old_action = actions[signo];
  if (action) {
    actions[signo] = *action;
    actions[signo].mask &= ~unmaskable;  // Linux do_sigaction never stores SIGKILL/SIGSTOP
  }
  return 0;
}
void install_debuggerd_handlers() {
  static const GuestAddr handler = [] {
    constexpr std::string_view name = "<debuggerd_signal_handler>";
    if (process_config().isa() == GuestIsa::a32) hle::a32::register_adapter("libc.so", name, debuggerd_signal_handler);
    return hle::make_thunk("libc.so", name, debuggerd_signal_handler);
  }();
  debuggerd_address.store(handler);
  // debuggerd_init(): sigfillset mask, SA_RESTART | SA_SIGINFO | SA_ONSTACK |
  // SA_EXPOSE_TAGBITS; debuggerd_register_handlers() signal list.
  const Action action{handler, ~0ull, sa_restart | sa_siginfo | sa_onstack | sa_expose_tagbits};
  for (int signo : {kSigAbrt, kSigBus, kSigFpe, kSigIll, kSigSegv, kSigStkflt, kSigSys, kSigTrap, kBionicSignalDebugger})
    set_action(signo, &action, nullptr);
}
GuestAddr debuggerd_handler() { return debuggerd_address.load(); }
int send_to_thread(std::int32_t tid, const Info& info) {
  if (info.signo < 0 || info.signo > 64) return -err::kEINVAL;
  if (info.signo == 18) { process_stopped.store(false); continued.notify_all(); }
  int result = -err::kESRCH;
  for_each_guest_thread([&](GuestThread& thread) {
    if (thread.tid() != tid) return;
    result = 0;
    if (!info.signo) return;
    {
      std::lock_guard lock(mutex);
      const auto& action = actions[info.signo];
      if (info.signo != kSigKill && info.signo != kSigStop &&
          (action.handler == 1 || (!action.handler && default_ignored(info.signo)))) return;
      if (!(thread.pending_signals.load() & bit(info.signo))) states[&thread].infos[info.signo] = info;
      thread.pending_signals.fetch_or(bit(info.signo));
    }
    if (!(thread.signal_mask_snapshot.load() & bit(info.signo))) thread.interrupt();
  });
  return result;
}
int send_to_process(const Info& info) {
  if (info.signo < 0 || info.signo > 64) return -err::kEINVAL;
  int target = 0;
  for_each_guest_thread([&](GuestThread& t) {
    if (!target && (!info.signo || !(t.signal_mask_snapshot.load() & bit(info.signo)))) target = t.tid();
  });
  if (!target) for_each_guest_thread([&](GuestThread& t) { if (!target) target = t.tid(); });
  return target ? send_to_thread(target, info) : -err::kESRCH;
}
void deliver_synchronous(GuestThread& thread, const Info& info) { deliver(thread, info, true); }
bool deliver_pending(GuestThread& thread) {
  bool ran = false;
  last_restart = false;
  if (process_stopped.load()) wait_stopped(thread);
  while (auto pending = thread.pending_signals.load() & ~thread.blocked_signals) {
    auto signo = static_cast<int>(std::countr_zero(pending)) + 1;
    Info info;
    {
      std::lock_guard lock(mutex);
      info = states[&thread].infos[signo]; info.signo = signo;
      thread.pending_signals.fetch_and(~bit(signo));
    }
    ran |= deliver(thread, info, false);
  }
  return ran;
}
int set_mask(GuestThread& thread, int how, const std::uint64_t* set, std::uint64_t* old_set) {
  if (old_set) *old_set = thread.blocked_signals;
  if (!set) return 0;
  auto mask = thread.blocked_signals;
  if (how == 0) mask |= *set;
  else if (how == 1) mask &= ~*set;
  else if (how == 2) mask = *set;
  else return -err::kEINVAL;
  thread.blocked_signals = mask & ~unmaskable;
  thread.signal_mask_snapshot.store(thread.blocked_signals);
  return 0;
}
int set_alt_stack(GuestThread& thread, const AltStack* stack, AltStack* old_stack) {
  std::lock_guard lock(mutex);
  auto& alt = states[&thread].alt;
  bool active = on_alt(thread, alt);
  if (old_stack) { *old_stack = alt; if (active) old_stack->flags |= 1; }
  if (!stack) return 0;
  if (active) return -err::kEPERM;
  if (stack->flags != 0 && stack->flags != 1 && stack->flags != 2) return -err::kEINVAL;
  if (!(stack->flags & 2) && stack->size < (thread.cpu().isa() == GuestIsa::a32 ? 2048u : 5120u)) return -err::kENOMEM;
  if (thread.cpu().isa() == GuestIsa::a32 && !(stack->flags & 2) &&
      (stack->base > UINT32_MAX || stack->size > (1ull << 32) - stack->base)) return -err::kEINVAL;
  alt = *stack;
  if (alt.flags & 2) { alt.base = 0; alt.size = 0; }
  else alt.flags &= ~1; // SS_ONSTACK is reported from the current SP
  return 0;
}
void return_from_signal(GuestThread& thread, bool realtime) {
  RtSigFrame frame;
  auto& cpu = thread.cpu();
  if (cpu.isa() == GuestIsa::a32) {
    SigFrame32 wire;
    const auto address = cpu.sp() + (realtime ? sizeof(SigInfo32) : 0);
    if ((cpu.sp() & 7) || !platform::read_memory(address, &wire, sizeof(wire))) terminate(thread, {kSigSegv, 1, cpu.sp()});
    const auto& sc = wire.context.mcontext;
    if ((sc.cpsr & 0x1f) != 0x10 ||
        (sc.regs[15] & ((sc.cpsr & 0x20) ? 1u : 3u))) terminate(thread, {kSigSegv, 1, sc.regs[15]});
    VfpContext32 fp; std::memcpy(&fp, wire.context.regspace.data(), sizeof(fp));
    if (fp.magic != 0x56465001 || fp.size != sizeof(fp)) terminate(thread, {kSigSegv, 1, address});
    for (int i = 0; i < 16; ++i) cpu.set_x(i, sc.regs[i]);
    for (int i = 0; i < 32; ++i) cpu.set_v(i, {fp.regs[i], 0});
    cpu.set_pstate(sc.cpsr); cpu.set_fpcr(fp.fpscr); cpu.clear_exclusive();
    thread.blocked_signals = wire.context.sigmask & ~unmaskable;
    thread.signal_mask_snapshot.store(thread.blocked_signals);
    if (realtime) {
      // Compat rt_sigreturn restores uc_stack after the interrupted SP. Linux
      // suppresses sigaltstack errors other than an unreadable frame; legacy
      // sigreturn does not restore an alternate-stack descriptor.
      const AltStack restored{wire.context.stack.sp, wire.context.stack.size, wire.context.stack.flags};
      (void)set_alt_stack(thread, &restored, nullptr);
    }
    return;
  }
  if (!platform::read_memory(cpu.sp(), &frame, sizeof(frame))) terminate(thread, {kSigSegv, 1, cpu.sp()});
  auto& sc = frame.context.mcontext;
  FpsimdContext fp;
  std::memcpy(&fp, sc.reserved.data(), sizeof(fp));
  if (fp.magic != 0x46508001 || fp.size != 528 || (sc.sp & 15) || (sc.pc & 3)) terminate(thread, {kSigSegv, 1, sc.pc});
  for (int i = 0; i < 31; ++i) cpu.set_x(i, sc.regs[i]);
  for (int i = 0; i < 32; ++i) cpu.set_v(i, fp.vregs[i]);
  cpu.set_pc(sc.pc); cpu.set_sp(sc.sp); cpu.set_pstate(static_cast<std::uint32_t>(sc.pstate) & 0xf0000000);
  cpu.set_fpcr(fp.fpcr); cpu.set_fpsr(fp.fpsr); cpu.clear_exclusive();
  thread.blocked_signals = frame.context.sigmask & ~unmaskable;
  thread.signal_mask_snapshot.store(thread.blocked_signals);
}
void write_host_fault_guest_frames(GuestThread* thread) noexcept {
  if (!thread) return;
  try {
    crash_output = true;
    report("Host fault in a call from guest tid {}; guest registers and frames:", thread->tid());
    const auto frame = cpu_frame(thread->cpu());
    log_registers(frame);
    log_backtrace(frame);
  } catch (...) {
  }
}
void log_guest_threads(std::string_view reason) {
  std::map<std::int32_t, std::string> names;
  for (auto& [tid, name] : threads::list()) names[tid] = name;
  log::error("signal", "guest thread dump: {}", reason);
  for_each_guest_thread([&](GuestThread& thread) {
    auto& cpu = thread.cpu();
    const auto name = names.find(thread.tid());
    log::error("signal", "tid {} \"{}\" PC {:#x} LR {:#x} SP {:#x}", thread.tid(),
               name == names.end() ? std::string() : name->second, cpu.pc(), cpu.lr(), cpu.sp());
    log_backtrace(cpu_frame(cpu));
  });
}
bool capture_first_frame_stall() {
  const auto acknowledged = platform::request_runtime_stall_dump();
  try_log_guest_threads_raw("no-first-frame");
  return acknowledged;
}
bool capture_frame_stall() {
  log::error("startup-snapshot", "No frame submitted for {} s while the game has focus; capturing thread state",
             startup_diagnostics::FrameStallTrigger::stall_seconds);
  const auto acknowledged = platform::request_runtime_stall_dump();
  try_log_guest_threads_raw("frame-stall", true);
  return acknowledged;
}
bool try_log_guest_threads_raw(std::string_view reason, bool symbols) {
  struct Row {
    std::int32_t tid = 0;
    GuestAddr pc = 0, lr = 0, sp = 0;
    std::array<GuestAddr, 32> frames{};
    unsigned count = 0;
  };
  std::array<Row, 128> rows{};
  unsigned count = 0, dropped = 0;
  const auto acquired = try_for_each_guest_thread([&](GuestThread& thread) {
    if (count == rows.size()) { ++dropped; return; }
    auto& row = rows[count++];
    auto& cpu = thread.cpu();
    row.tid = thread.tid(); row.pc = cpu.pc(); row.lr = cpu.lr(); row.sp = cpu.sp();
    if (cpu.isa() != GuestIsa::a64) return;
    auto fp = cpu.frame_pointer();
    while (fp && !(fp & 15) && row.count < row.frames.size()) {
      std::array<std::uint64_t, 2> record{};
      if (!platform::read_memory(fp, record.data(), sizeof(record))) break;
      row.frames[row.count++] = record[1];
      if (record[0] <= fp || record[0] - fp > 16 * 1024 * 1024) break;
      fp = record[0];
    }
  });
  log::error("startup-snapshot", "raw guest stacks reason={} registry_available={} dropped={} state=last-saved",
             reason, acquired, dropped);
  // Named after the registry is released; the lookup only try-locks the linker.
  auto name = [&](GuestAddr pc) {
    DlInfo info;
    if (!symbols || !Linker::get().try_address_info(pc, &info) || info.path.empty()) return std::string();
    return std::format(" {}+{:#x} ({})", info.path, pc - info.base, info.symbol);
  };
  for (unsigned i = 0; i < count; ++i) {
    const auto& row = rows[i];
    log::error("startup-snapshot", "guest tid={} pc={:#x}{} lr={:#x}{} sp={:#x}", row.tid, row.pc, name(row.pc),
               row.lr, name(row.lr), row.sp);
    for (unsigned f = 0; f < row.count; ++f)
      log::error("startup-snapshot", "guest tid={} frame={} pc={:#x}{}", row.tid, f, row.frames[f], name(row.frames[f]));
  }
  if (!acquired) {
    const auto owner = thread_registry_owner();
    log::error("startup-snapshot", "registry owner native_tid={} stable={}", owner.tid, owner.stable);
  }
  return acquired;
}
void forget_thread(GuestThread& thread) { std::lock_guard lock(mutex); states.erase(&thread); }
bool restart_last_handler() { return last_restart; }
void interrupt_stopped_threads() { continued.notify_all(); }
int wait_signal(GuestThread& thread, std::uint64_t set, Info* info, std::int64_t deadline) {
  set &= ~unmaskable;
  if (!set) return -err::kEINVAL;
  std::uint64_t previous;
  set_mask(thread, 0, &set, &previous);
  struct Restore { GuestThread& thread; std::uint64_t mask; ~Restore() { set_mask(thread, 2, &mask, nullptr); } } restore{thread, previous};
  int wake = 0;
  for (;;) {
    {
      std::lock_guard lock(mutex);
      const auto pending = thread.pending_signals.load() & set;
      if (pending) {
        const int signo = std::countr_zero(pending) + 1;
        thread.pending_signals.fetch_and(~bit(signo));
        if (info) { *info = states[&thread].infos[signo]; info->signo = signo; }
        return signo;
      }
    }
    if (deadline >= 0 && monotonic_ns() >= deadline) return -err::kEAGAIN;
    if (wake == -err::kEINTR) return wake;
    // send_to_thread sets interrupted before waking the epoch-backed core wait,
    // so signal arrival between dequeue and enqueue cannot be lost.
    wake = sleep_until(thread, deadline);
  }
}
}
