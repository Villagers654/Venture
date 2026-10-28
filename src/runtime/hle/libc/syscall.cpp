#include "vfs.h"
#include "abi32.h"
#include "core/guest_storage.h"
#include "core/syscall_runtime.h"
#include "core/signals.h"
#include "common/log.h"
#include <array>
#include <set>

namespace venture::hle::libc {
namespace {
// Invoke the same HLE implementation without going through an ARM64 thunk.
// Save the argument registers: libc syscall() shifts them relative to SVC #0.
// uapi/linux/sched/types.h: SCHED_ATTR_SIZE_VER0 is 48 bytes, VER1 adds the
// utilization clamps.
struct SchedAttr {
  std::uint32_t size, policy;
  std::uint64_t flags;
  std::int32_t nice;
  std::uint32_t priority;
  std::uint64_t runtime, deadline, period;
  std::uint32_t util_min, util_max;
};
static_assert(sizeof(SchedAttr) == 56);
constexpr std::uint32_t kSchedAttrSize0 = 48;
std::int64_t invoke(GuestThread& t, const char* name, const std::uint64_t* a) {
  Handler handler = nullptr;
  {
    std::lock_guard lock(handlers_mutex());
    auto i = handlers().find(name);
    if (i != handlers().end()) handler = i->second;
  }
  if (!handler) return -err::enosys;
  auto& cpu = t.cpu();
  std::array<std::uint64_t, 8> saved;
  for (int i = 0; i < 8; ++i) {
    saved[i] = cpu.x(i);
    cpu.set_x(i, i < 6 ? a[i] : 0);
  }
  FunctionInfo info{handler, nullptr, "libc.so", name, 0};
  CallContext c(t, cpu, info);
  auto old_errno = *t.errno_location();
  t.set_errno(0);
  try {
    handler(c);
  } catch (...) {
    for (int i = 0; i < 8; ++i) cpu.set_x(i, saved[i]);
    t.set_errno(old_errno);
    throw;
  }
  auto value = static_cast<std::int64_t>(cpu.x(0));
  if (value == -1 && *t.errno_location()) value = -*t.errno_location();
  for (int i = 0; i < 8; ++i) cpu.set_x(i, saved[i]);
  t.set_errno(old_errno);
  return value;
}
std::int64_t futex(GuestThread& t, const std::uint64_t* a) {
  auto address = a[0];
  auto op = static_cast<unsigned>(a[1]);
  int command = op & 127;
  if (address % 4 || !address) return -err::einval;
  if (command == 1 || command == 10) {
    if (static_cast<int>(a[2]) < 0 || (command == 10 && !a[5])) return -err::einval;
    return futex_wake(address, static_cast<int>(a[2]),
                      command == 10 ? static_cast<unsigned>(a[5]) : UINT32_MAX);
  }
  if (command != 0 && command != 9) return -err::enosys;
  if (command == 9 && !a[5]) return -err::einval;
  std::int64_t end = -1;
  if (a[3]) {
    auto* time = g2h<Timespec>(a[3]);
    int r = deadline(time, (op & 256) ? 0 : 1, &end);
    if (r) return r;
    if (command == 0) {
      if (time->sec < 0) return -err::einval;
      auto now = monotonic_ns();
      end = end > INT64_MAX - now ? INT64_MAX : now + end;
    }
  }
  return futex_wait(t, address, static_cast<unsigned>(a[2]), end,
                    command == 9 ? static_cast<unsigned>(a[5]) : UINT32_MAX);
}
// Kernel sigaction differs from bionic's userspace struct (asm-generic/signal.h).
struct KernelAction {
  GuestAddr handler;
  std::uint64_t flags;
  GuestAddr restorer;
  std::uint64_t mask;
};
std::int64_t syscall_fn(CallContext& c, std::uint64_t number, VarArgs args) {
  std::uint64_t a[6];
  for (auto& v : a) v = args.next<std::uint64_t>();
  return result(syscall_impl(c.thread(), number, a));
}
bool try_syscall(CallContext& c) {
  const bool arm = c.cpu().isa() == GuestIsa::a32;
  const auto number = arm ? std::uint32_t(c.x(0)) : c.x(0);
  if (number != (arm ? 240u : 98u)) return false;
  const auto op = static_cast<unsigned>(c.x(2));
  // Only FUTEX_WAKE completes here. Waits, bitset operations and every other
  // syscall retain the run-loop path, including its signal/re-entry contract.
  // All consumed arguments fit in registers in both bionic calling ABIs.
  if ((op & 127) != 1) return false;
  const auto address = arm ? std::uint32_t(c.x(1)) : c.x(1);
  const auto count = static_cast<std::int32_t>(c.x(3));
  const auto value = !address || (address & 3) || count < 0
      ? -err::einval : futex_wake(address, count);
  c.return_x(arm ? std::uint32_t(result(value)) : std::uint64_t(result(value)));
  return true;
}
std::int64_t getrandom_fn(void* p, std::uint64_t n, unsigned flags) {
  if (flags & ~3u) return failure(err::einval);
  if (n && !p) return failure(err::efault);
  n = std::min<std::uint64_t>(n, 0x7ffff000);
  int r = host::random(p, n);
  return r ? result(r) : static_cast<std::int64_t>(n);
}
int getentropy_fn(void* p, std::uint64_t n) {
  if (n > 256) return failure(err::eio);
  return getrandom_fn(p, n, 0) < 0 ? -1 : 0;
}
// Linux arch/arm/tools/syscall.tbl (EABI). The mapping carries an exact
// argument count so syscall() never reads absent variadic stack arguments.
struct ArmCall { unsigned number; const char* name; unsigned words; };
constexpr ArmCall arm_calls[] = {
  {3,"read",3}, {4,"write",3}, {5,"open",3}, {6,"close",1}, {10,"unlink",1},
  {12,"chdir",1}, {19,"lseek",3}, {20,"getpid",0}, {37,"kill",2}, {38,"rename",2},
  {39,"mkdir",2}, {40,"rmdir",1}, {41,"dup",1}, {42,"pipe",1}, {54,"ioctl",3},
  {55,"fcntl",3}, {60,"umask",1}, {63,"dup2",2}, {64,"getppid",0},
  {75,"setrlimit",2}, {78,"gettimeofday",2}, {91,"munmap",2}, {92,"truncate",2},
  {93,"ftruncate",2}, {118,"fsync",1},
  {122,"uname",1}, {125,"mprotect",3}, {145,"readv",3}, {146,"writev",3},
  {148,"fdatasync",1}, {158,"sched_yield",0}, {162,"nanosleep",2}, {168,"poll",3},
  {186,"sigaltstack",2}, {191,"getrlimit",2}, {195,"stat64",2}, {196,"lstat64",2},
  {197,"fstat64",2}, {199,"getuid",0}, {200,"getgid",0}, {201,"geteuid",0},
  {202,"getegid",0}, {172,"prctl",5}, {220,"madvise",3}, {221,"fcntl64",3}, {224,"gettid",0},
  {238,"tkill",2}, {241,"sched_setaffinity",3}, {263,"clock_gettime",2}, {264,"clock_getres",2},
  {265,"clock_nanosleep",4}, {268,"tgkill",3}, {281,"socket",3}, {282,"bind",3},
  {283,"connect",3}, {284,"listen",2}, {285,"accept",3}, {286,"getsockname",3},
  {287,"getpeername",3}, {288,"socketpair",4}, {289,"send",4}, {290,"sendto",6},
  {291,"recv",4}, {292,"recvfrom",6}, {293,"shutdown",2}, {294,"setsockopt",5},
  {295,"getsockopt",5}, {322,"openat",4}, {323,"mkdirat",3}, {327,"fstatat64",4},
  {328,"unlinkat",3}, {329,"renameat",4}, {332,"readlinkat",4}, {334,"faccessat",4},
  {350,"timerfd_create",2}, {353,"timerfd_settime",4}, {354,"timerfd_gettime",2},
  {356,"eventfd",2}, {357,"epoll_create1",1}, {358,"dup3",3}, {359,"pipe2",2},
  {366,"accept4",4}, {369,"prlimit64",4}, {384,"getrandom",3}
};
const ArmCall* arm_call(unsigned n) {
  for (const auto& call : arm_calls) if (call.number == n) return &call;
  return nullptr;
}
std::int64_t invoke32(GuestThread& t, const char* name, std::span<const std::uint64_t> args) {
  Handler h = nullptr;
  { std::lock_guard lock(handlers_mutex()); auto i = handlers().find(name); if (i != handlers().end()) h = i->second; }
  if (!h) return -err::enosys;
  FunctionInfo info{h, nullptr, "libc.so", name, 0}; a32::prepare(info);
  if (info.handler == a32::diagnostic) return -err::enosys;
  auto& cpu = t.cpu();
  std::array<std::uint64_t, 4> regs{};
  for (unsigned i = 0; i < 4; ++i) regs[i] = cpu.x(i);
  auto sp = cpu.sp(); auto old_errno = *t.errno_location();
  struct Restore {
    Cpu& cpu; GuestThread& t; std::array<std::uint64_t,4> regs; GuestAddr sp; int error;
    ~Restore() { for (unsigned i = 0; i < 4; ++i) cpu.set_x(i, regs[i]); cpu.set_sp(sp); t.set_errno(error); }
  } restore{cpu, t, regs, sp, old_errno};
  GuestStorage scratch; auto bottom = scratch.resize(128); cpu.set_sp(bottom + 128);
  a32::CallBuilder32 call;
  for (std::size_t i = 0; i < args.size(); ++i) {
    auto type = i < info.a32->arguments.size() ? info.a32->arguments[i] : a32::Type{a32::Kind::U32};
    // The syscall supplies wire addresses/tokens. The registered adapter owns
    // each layout and Out cell; the outgoing call frame only carries its word.
    if (a32::needs_adapter(type) || a32::is_out(type.kind)) type = {a32::Kind::U32};
    call.add(type, args[i]);
  }
  call.apply(cpu, bottom); t.set_errno(0);
  CallContext context(t, cpu, info); info.handler(context);
  auto bits = std::uint32_t(cpu.x(0));
  if (bits == UINT32_MAX && *t.errno_location()) return -*t.errno_location();
  return info.a32->result.kind == a32::Kind::Ptr ? std::int64_t(bits) : std::int32_t(bits);
}
unsigned arm_words(unsigned n) {
  if (auto p = arm_call(n)) return p->words;
  switch (n) {
    case 45: return 1; // brk
    case 240: case 422: return 6; case 192: return 6; // futex, futex_time64, mmap2
    case 120: return 5; // clone
    case 174: case 175: return 4; // rt_sigaction, rt_sigprocmask
    case 256: return 1; // set_tid_address
    case 242: return 3; // sched_getaffinity
    case 180: case 181: return 6; // pread64/pwrite64 include EABI padding r3
    case 140: return 5; // _llseek: explicit high, low, output, whence
    default: return 0;
  }
}
std::int64_t arm_syscall(GuestThread& t, unsigned n, const std::uint64_t* a) {
  if (auto p = arm_call(n)) {
    auto r = invoke32(t, p->name, {a, p->words});
    return n == 265 && r > 0 ? -r : r;
  }
  switch (n) {
    case 45: return linux_program_break(std::uint32_t(a[0])); // ARM __NR_brk
    case 242: {
      // ARM kernel cpumask words are 32-bit. The shared guest CPU topology
      // has at most eight CPUs, so the raw syscall writes/returns one word;
      // bionic's sched_getaffinity wrapper instead returns zero and clears
      // the remainder of its caller's buffer.
      if (a[1] < 4) return -err::einval;
      const std::array<std::uint64_t, 3> args{a[0], 4, a[2]};
      auto r = invoke32(t, "sched_getaffinity", args);
      return r < 0 ? r : 4;
    }
    case 240: {
      std::array<std::uint64_t,6> args; std::copy_n(a,6,args.begin());
      Timespec wide{};
      if (a[3] && ((a[1] & 127) == 0 || (a[1] & 127) == 9)) {
        abi32::Timespec narrow{}; a32::read_bytes(a[3], &narrow, sizeof(narrow));
        wide = {narrow.sec, narrow.nsec}; args[3] = h2g(&wide);
      }
      return futex(t, args.data());
    }
    case 192: {
      std::array<std::uint64_t,6> args; std::copy_n(a,6,args.begin()); args[5] <<= 12;
      return invoke32(t,"mmap64",args);
    }
    case 174:
      if (a[3] != 8) return -err::einval;
      // ARM kernel sigaction and bionic sigaction64 have the same wire layout.
      return invoke32(t,"sigaction64",{a,3});
    case 175:
      if (a[3] != 8) return -err::einval;
      return invoke32(t,"sigprocmask64",{a,3});
    case 256: t.clear_child_tid = a[0]; return t.tid();
    case 180: case 181: {
      auto offset = std::int64_t(a[4] | (a[5] << 32));
      return n == 180 ? fd_pread(a[0],g2h(a[1]),a[2],offset) : fd_pwrite(a[0],g2h(a[1]),a[2],offset);
    }
    case 422: { // futex_time64: the timeout is already a 64-bit timespec
      std::array<std::uint64_t,6> args; std::copy_n(a,6,args.begin());
      return futex(t, args.data());
    }
    case 120: return -err::eperm; // raw clone; see the A64 case 220
    case 140: {
      auto r = fd_seek(a[0],std::int64_t((a[1] << 32) | a[2]),a[4]);
      if (r < 0) return r; a32::write_bytes(a[3],&r,8); return 0;
    }
    default: log::warn("syscall", "unimplemented ARM EABI syscall {}", n); return -err::enosys;
  }
}
std::int32_t syscall32(CallContext& c, unsigned number, a32::VarArgs32 args) {
  std::uint64_t a[6]{};
  for (unsigned i = 0; i < arm_words(number); ++i) a[i] = args.next<std::uint32_t>();
  return static_cast<std::int32_t>(result(arm_syscall(c.thread(),number,a)));
}
}  // namespace
std::int64_t syscall_impl(GuestThread& t, std::uint64_t n, const std::uint64_t* a) {
  const char* name = nullptr;
  // aarch64 uses asm-generic/unistd.h, NOT x86-64 syscall numbers.
  switch (n) {
    case 214: return linux_program_break(a[0]); // asm-generic __NR_brk
    case 17: {
      auto path = cwd();
      if (a[1] < path.size() + 1) return -err::erange;
      if (!a[0]) return -err::efault;
      std::memcpy(g2h(a[0]), path.c_str(), path.size() + 1);
      return path.size() + 1;
    }
    case 19:
      name = "eventfd";
      break;
    case 20:
      name = "epoll_create1";
      break;
    case 21:
      name = "epoll_ctl";
      break;
    case 22:
      name = "epoll_pwait";
      break;
    case 23:
      name = "dup";
      break;
    case 24:
      name = "dup3";
      break;
    case 25:
      return fd_fcntl(a[0], a[1], a[2]);
    case 29:
      return fd_ioctl(a[0], a[1], g2h(a[2]));
    case 34:
      name = "mkdirat";
      break;
    case 35:
      name = "unlinkat";
      break;
    case 38:
      name = "renameat";
      break;
    case 46:
      name = "ftruncate";
      break;
    case 48:
      name = "faccessat";
      break;
    case 49:
      name = "chdir";
      break;
    case 50:
      name = "fchdir";
      break;
    case 52:
      name = "fchmod";
      break;
    case 56:
      return fd_open(g2h<char>(a[1]), a[2], a[3], a[0]);
    case 57:
      return FdTable::get().close(a[0]);
    case 59:
      name = "pipe2";
      break;
    case 61:
      return fd_getdents(a[0], g2h(a[1]), a[2]);
    case 62:
      return fd_seek(a[0], a[1], a[2]);
    case 63:
      return fd_read(a[0], g2h(a[1]), a[2]);
    case 64:
      return fd_write(a[0], g2h(a[1]), a[2]);
    case 65:
      name = "readv";
      break;
    case 66:
      name = "writev";
      break;
    case 67:
      return fd_pread(a[0], g2h(a[1]), a[2], a[3]);
    case 68:
      return fd_pwrite(a[0], g2h(a[1]), a[2], a[3]);
    case 71:
      name = "sendfile";
      break;
    case 72: {
      std::uint64_t args[6];
      std::copy_n(a, 6, args);
      if (a[5]) {
        auto mask = g2h<std::uint64_t>(a[5]);
        if (mask[1] != 8) return -err::einval;
        args[5] = mask[0];
      }
      return invoke(t, "pselect", args);
    }
    case 73:
      if (a[3] && a[4] != 8) return -err::einval;
      name = "ppoll";
      break;
    case 78:
      name = "readlinkat";
      break;
    case 79:
      name = "fstatat";
      break;
    case 80:
      return fd_stat(a[0], g2h<Stat>(a[1]));
    case 82:
      name = "fsync";
      break;
    case 83:
      name = "fdatasync";
      break;
    case 85:
      name = "timerfd_create";
      break;
    case 86:
      name = "timerfd_settime";
      break;
    case 87:
      name = "timerfd_gettime";
      break;
    case 93:
      name = "pthread_exit";
      break;
    case 94:
      name = "_exit";
      break;
    case 96:
      return t.tid();
    case 98:
      return futex(t, a);
    case 101:
      name = "nanosleep";
      break;
    case 113:
      name = "clock_gettime";
      break;
    case 114:
      name = "clock_getres";
      break;
    case 102:
      name = "getitimer";
      break;
    case 103:
      name = "setitimer";
      break;
    case 107: {
      if (!a[2]) return -err::efault;
      GuestAddr id = 0;
      std::uint64_t args[6] = {a[0], a[1], h2g(&id), 0, 0, 0};
      auto r = invoke(t, "timer_create", args);
      if (!r) *g2h<std::int32_t>(a[2]) = static_cast<std::int32_t>(id);
      return r;
    }
    case 108:
      name = "timer_gettime";
      break;
    case 109:
      name = "timer_getoverrun";
      break;
    case 110:
      name = "timer_settime";
      break;
    case 111:
      name = "timer_delete";
      break;
    case 115: {
      auto r = invoke(t, "clock_nanosleep", a);
      return r > 0 ? -r : r;
    }
    case 123: {
      if (a[1] < 8) return -err::einval;
      auto r = invoke(t, "sched_getaffinity", a);
      return r < 0 ? r : 8;
    }
    case 122:
      name = "sched_setaffinity";
      break;
    case 124:
      name = "sched_yield";
      break;
    case 129:
      name = "kill";
      break;
    case 130:
      name = "tkill";
      break;
    case 131:
      name = "tgkill";
      break;
    case 132:
      name = "sigaltstack";
      break;
    case 133:
      if (a[1] != 8) return -err::einval;
      name = "sigsuspend";
      break;
    case 134: {
      if (a[3] != 8) return -err::einval;
      auto* in = g2h<KernelAction>(a[1]);
      auto* out = g2h<KernelAction>(a[2]);
      signals::Action next{}, old{};
      if (in) next = {in->handler, in->mask, static_cast<std::uint32_t>(in->flags)};
      int r = signals::set_action(a[0], in ? &next : nullptr, out ? &old : nullptr);
      if (!r && out) *out = {old.handler, old.flags, 0, old.mask};
      return r;
    }
    case 135:
      if (a[3] != 8) return -err::einval;
      return signals::set_mask(t, a[0], g2h<std::uint64_t>(a[1]), g2h<std::uint64_t>(a[2]));
    case 136:
      name = "sigpending";
      break;
    case 153:
      name = "times";
      break;
    case 160:
      name = "uname";
      break;
    case 137:
      if (a[3] != 8) return -err::einval;
      name = "sigtimedwait";
      break;
    case 163:
      name = "getrlimit";
      break;
    case 164:
      name = "setrlimit";
      break;
    case 165:
      name = "getrusage";
      break;
    case 166:
      name = "umask";
      break;
    case 167:
      name = "prctl";
      break;
    case 169:
      name = "gettimeofday";
      break;
    case 172:
      return 1000;
    case 173:
      return 1;
    case 174:
    case 175:
    case 176:
    case 177:
      return 10125;
    case 178:
      return t.tid();
    case 198:
      name = "socket";
      break;
    case 199:
      name = "socketpair";
      break;
    case 200:
      name = "bind";
      break;
    case 201:
      name = "listen";
      break;
    case 202:
      name = "accept";
      break;
    case 203:
      name = "connect";
      break;
    case 204:
      name = "getsockname";
      break;
    case 205:
      name = "getpeername";
      break;
    case 206:
      name = "sendto";
      break;
    case 207:
      name = "recvfrom";
      break;
    case 208:
      name = "setsockopt";
      break;
    case 209:
      name = "getsockopt";
      break;
    case 210:
      name = "shutdown";
      break;
    case 211:
      name = "sendmsg";
      break;
    case 212:
      name = "recvmsg";
      break;
    case 215:
      name = "munmap";
      break;
    case 216:
      name = "mremap";
      break;
    case 222:
      name = "mmap";
      break;
    case 226:
      name = "mprotect";
      break;
    case 227:
      name = "msync";
      break;
    case 232:
      name = "mincore";
      break;
    case 233:
      name = "madvise";
      break;
    case 242:
      name = "accept4";
      break;
    case 261:
      name = "prlimit64";
      break;
    case 279:
      name = "memfd_create";
      break;
    case 260:
      name = "wait4";
      break;
    case 270:
      name = "process_vm_readv";
      break;
    case 179:
      name = "sysinfo";
      break;
    case 278: {
      if (a[2] & ~3ull) return -err::einval;
      if (a[1] && !a[0]) return -err::efault;
      auto size = std::min<std::uint64_t>(a[1], 0x7ffff000);
      int r = host::random(g2h(a[0]), size);
      return r ? r : static_cast<std::int64_t>(size);
    }
    case 274: { // sched_setattr(pid, attr, flags)
      if (!a[1] || a[2]) return -err::einval;
      SchedAttr attr{};
      std::uint32_t size; std::memcpy(&size, g2h(a[1]), 4);
      if (!size) size = kSchedAttrSize0;
      if (size < kSchedAttrSize0 || size > 4096) return -err::e2big;
      std::memcpy(&attr, g2h(a[1]), std::min<std::size_t>(size, sizeof attr));
      if (attr.flags & ~std::uint64_t(1)) return -err::einval;  // SCHED_FLAG_RESET_ON_FORK only
      const int priority = int(attr.priority);
      const std::uint64_t policy = attr.policy | (attr.flags & 1 ? 0x40000000u : 0u);
      const std::uint64_t set[6]{a[0], policy, h2g(&priority)};
      if (auto r = invoke(t, "sched_setscheduler", set); r < 0) return r;
      if (attr.policy == 0 || attr.policy == 3) {  // SCHED_OTHER, SCHED_BATCH carry nice
        const std::uint64_t nice[6]{0, a[0], std::uint64_t(std::int64_t(attr.nice))};
        if (auto r = invoke(t, "setpriority", nice); r < 0) return r;
      }
      return 0;
    }
    case 275: { // sched_getattr(pid, attr, size, flags)
      if (!a[1] || a[3] || a[2] < kSchedAttrSize0 || a[2] > 4096) return -err::einval;
      const std::uint64_t who[6]{a[0]};
      const auto policy = invoke(t, "sched_getscheduler", who);
      if (policy < 0) return policy;
      SchedAttr attr{};
      attr.size = std::uint32_t(std::min<std::uint64_t>(a[2], sizeof attr));
      attr.policy = std::uint32_t(policy);
      if (policy == 0 || policy == 3) {
        const std::uint64_t which[6]{0, a[0]};
        const auto nice = invoke(t, "getpriority", which);
        attr.nice = nice >= -20 && nice <= 19 ? std::int32_t(nice) : 0;
      }
      attr.util_max = 1024;  // SCHED_CAPACITY_SCALE: no utilization clamp
      std::memcpy(g2h(a[1]), &attr, attr.size);
      return 0;
    }
    case 283:
      std::atomic_thread_fence(std::memory_order_seq_cst);
      return 0;
    case 220:
      // Raw clone: guest threads come from pthread_create. Refuse as a
      // restricted kernel does; raw-syscall callers commonly test for -1.
      return -err::eperm;
    default:
      break;
  }
  if (name) {
    auto r = invoke(t, name, a);
    if (r != -err::enosys) return r;
  }
  static std::mutex mutex;
  static std::set<std::uint64_t> logged;
  {
    std::lock_guard lock(mutex);
    if (logged.insert(n).second) log::warn("syscall", "unimplemented aarch64 syscall {}", n);
  }
  return -err::enosys;
}
void register_syscall(HleLibrary& lib) {
  if (process_config().isa() == GuestIsa::a32) abi32::adapt<&syscall32>("syscall");
  set_linux_syscall_handler([](GuestThread& t) {
    // Keep rt_sigreturn, set_tid_address and futex dispatch in the core.
    const auto n = t.cpu().syscall_number();
    if (t.cpu().isa() == GuestIsa::a32) {
      if (n == 1 || n == 248 || n == 119 || n == 173 || n == 0xf0002 || n == 0xf0005) return false;
      std::uint64_t args[6]{};
      for (unsigned i = 0; i < arm_words(n); ++i) args[i] = std::uint32_t(t.cpu().x(i));
      try { t.cpu().set_x(0, std::uint32_t(arm_syscall(t,n,args))); }
      catch (const a32::AbiError& e) { t.cpu().set_x(0,std::uint32_t(-e.error)); }
      return true;
    }
    if (n == 139 || n == 96 || n == 98) return false;
    std::uint64_t args[6];
    for (int i = 0; i < 6; ++i) args[i] = t.cpu().x(i);
    t.cpu().set_x(0, static_cast<std::uint64_t>(syscall_impl(t, n, args)));
    return true;
  });
  add(lib, "syscall", wrap<&syscall_fn>(), Execution::InJit, try_syscall);
  VENTURE_LIBC_BIND(lib, "getrandom", getrandom_fn);
  VENTURE_LIBC_BIND(lib, "getentropy", getentropy_fn);
}
}  // namespace venture::hle::libc
