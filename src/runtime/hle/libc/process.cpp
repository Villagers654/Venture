#include "platform/thread_priority.h"
#include "vfs.h"
#include "abi32.h"
#include "core/guest_storage.h"
#include "core/thread_api.h"
#include "core/thread_runtime.h"
#include "core/signals.h"
#include "core/address_space.h"
#include "core/hle_runtime.h"
#include "cpu/cpu.h"
#include "common/log.h"
#include <algorithm>
#include <climits>
#include <atomic>
#include <array>
#include <cstring>
#include <stdexcept>
namespace venture::hle::libc {
namespace {
int pid() { return kGuestPid; }
int ppid() { return 1; }
unsigned uid() { return 10125; }
int setuid_fn(unsigned id) { return id == 10125 ? 0 : failure(err::eperm); }
int pagesize() { return 4096; }
struct Utsname {
  char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};
int uname_fn(Utsname* p) {
  if (!p) return failure(err::efault);
  *p = {};
  std::strcpy(p->sysname, "Linux");
  std::strcpy(p->nodename, "localhost");
  std::strcpy(p->release, "5.10.0-venture");
  std::strcpy(p->version, "#1 SMP PREEMPT");
  std::strcpy(p->machine, process_config().isa() == GuestIsa::a32 ? "armv8l" : "aarch64");
  std::strcpy(p->domainname, "localdomain");
  return 0;
}
// bionic libc/include/bits/sysconf.h numbers, NOT the host CRT's constants.
std::int64_t sysconf_fn(int n) {
  switch (n) {
    case 0:
      return 131072;
    case 5:
      return 32768;
    case 6:
      return 100;
    case 0xb:
      return 32768;
    case 0x19:
      return 200809;
    case 0x1b:
      return 32768;
    case 0x20:
      return 700;
    case 0x25:
      return 65536;
    case 0x26:
      return 1024;
    case 0x27:
    case 0x28:
      return 4096;
    case 0x34:
      return 32;
    case 0x36:
      return INT_MAX;
    case 0x47:
    case 0x48:
      return 1024;
    case 0x49:
      return 256;
    case 0x4a:
      return 4;
    case 0x4b:
      return 256;
    case 0x4c:
      return 16384;
    case 0x4d:
      return 32768;
    case 0x60:
    case 0x61:
      return cpu_count();
    case 0x62:
      return guest_usable_memory() / 4096;
    case 0x63:
      return guest_usable_memory() / 8192;
    case 0x6f:
      return 64;
    case 0x9e:
      return 65;
    case 0x3a:
    case 0x3b:
    case 0x3e:
    case 0x42:
    case 0x43:
    case 0x4f:
    case 0x50:
    case 0x51:
    case 0x55:
    case 0x64:
    case 0x6c:
    case 0x6d:
    case 0x6e:
    case 0x70:
    case 0x72:
    case 0x76:
    case 0x7a:
    case 0x7f:
      return 200809;
    default:
      return failure(err::einval);
  }
}
struct Limit {
  std::uint64_t current, maximum;
};
std::mutex limits_mutex;
std::array<Limit, 16> limits = [] {
  std::array<Limit, 16> l;
  for (auto& x : l) x = {UINT64_MAX, UINT64_MAX};
  l[3] = {kDefaultStackLimit, UINT64_MAX};
  l[4] = {0, UINT64_MAX};
  l[6] = {32768, 32768};
  l[7] = {32768, 32768};
  l[8] = {65536, 65536};
  return l;
}();
int getrlimit_fn(int resource, Limit* out) {
  if (resource < 0 || resource >= 16) return failure(err::einval);
  if (!out) return failure(err::efault);
  std::lock_guard lock(limits_mutex);
  *out = limits[resource];
  return 0;
}
int setrlimit_fn(int resource, const Limit* in) {
  if (resource < 0 || resource >= 16 || !in || in->current > in->maximum)
    return failure(err::einval);
  std::lock_guard lock(limits_mutex);
  if (in->maximum > limits[resource].maximum) return failure(err::eperm);
  if (resource != 3 && resource != 4 &&
      (in->current != limits[resource].current || in->maximum != limits[resource].maximum))
    return failure(err::enotsup);
  limits[resource] = *in;
  if (resource == 3) AddressSpace::get().set_stack_limit(in->current);
  return 0;
}
int prlimit_fn(int process, int resource, const Limit* in, Limit* out) {
  if (process && process != 1000) return failure(err::esrch);
  if (out && getrlimit_fn(resource, out)) return -1;
  return in ? setrlimit_fn(resource, in) : 0;
}
int getrusage_fn(int who, void* p) {
  if (who != 0 && who != -1 && who != 1) return failure(err::einval);
  if (!p) return failure(err::efault);
  std::memset(p, 0, 144);
  if (who != -1) {
    auto ns = host::cpu_time(who == 1);
    *static_cast<Timeval*>(p) = {ns / 1000000000, (ns / 1000) % 1000000};
  }
  return 0;
}
// bionic arm: cacheflush(start, end, flags) wraps __ARM_NR_cacheflush.
int cacheflush_fn(std::uint32_t start, std::uint32_t end, std::uint32_t flags) {
  if (flags || end < start) return failure(err::einval);
  cpu_backend().invalidate(start, end - start, InvalidationCause::cache_flush);
  return 0;
}
std::atomic<int> dumpable{1};
int prctl_fn(int option, std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) {
  auto& thread = *GuestThread::current();
  switch (option) {
    case 3:
      return dumpable.load();
    case 4:
      if (a > 1) return failure(err::einval);
      dumpable = a;
      return 0;
    case 15: {
      if (!a) return failure(err::efault);
      char name[16]{};
      std::memcpy(name, g2h(a), 15);
      return result(threads::set_name(thread.pthread_self, name));
    }
    case 16: {
      if (!a) return failure(err::efault);
      std::string name;
      int r = threads::describe(thread.pthread_self, nullptr, nullptr, &name);
      if (r) return result(r);
      std::memset(g2h(a), 0, 16);
      std::memcpy(g2h(a), name.data(), std::min<std::size_t>(15, name.size()));
      return 0;
    }
    default:
      log::warn("libc", "unsupported prctl option {}", option);
      return failure(err::einval);
  }
}
// prctl(int, ...): the ARM variadic tail is four unsigned long words.
int prctl32(int option, a32::VarArgs32 a) {
  std::uint64_t words[4];
  for (auto& w : words) w = a.next<std::uint32_t>();
  return prctl_fn(option, words[0], words[1], words[2], words[3]);
}
void exit_immediate(int status) { threads::exit_process(*GuestThread::current(), status); }
void exit_fn(int status) {
  // Part A owns __cxa_finalize and atexit callbacks. Invoke its guest thunk
  // through the same linker so no extra cross-part ABI is required.
  auto fn = Linker::get().symbol(nullptr, "__cxa_finalize");
  if (fn) GuestThread::current()->call(fn, {0});
  flush_stdio();
  threads::exit_process(*GuestThread::current(), status);
}
void abort_fn() {
  // A crash reporter can call exit(0) from its SIGABRT handler. abort remains
  // fatal even when that handler prevents our second raise from being reached.
  record_guest_process_failure_on_exit(134);
  auto& t = *GuestThread::current();
  std::uint64_t bit = 1ull << 5;
  signals::set_mask(t, 1, &bit, nullptr);
  signals::deliver_synchronous(t, {.signo = 6});
  signals::Action reset{};
  signals::set_action(6, &reset, nullptr);
  signals::deliver_synchronous(t, {.signo = 6});
  threads::exit_process(t, 134);
}
int unsupported() {
  log::warn("libc", "guest process creation/execution is unavailable");
  return failure(err::enosys);
}
int system_fn(const char* command) { return command ? unsupported() : 0; }
int spawn_fn(GuestAddr, const char*, GuestAddr, GuestAddr, GuestAddr, GuestAddr) {
  unsupported();
  return err::enosys;
}
int wait_fn(GuestAddr) { return failure(err::echild); }
int waitpid_fn(int, GuestAddr, int) { return failure(err::echild); }
int wait4_fn(int, GuestAddr, int, GuestAddr) { return failure(err::echild); }
int isatty_fn(int fd) {
  failure(FdTable::get().file(fd) ? err::enotty : err::ebadf);
  return 0;
}
const char* ttyname_fn(int fd) {
  isatty_fn(fd);
  return nullptr;
}
const char* login_fn() { return "u0_a125"; }
int hostname_fn(char* p, std::uint64_t n) {
  constexpr char name[] = "localhost";
  if (n < sizeof(name)) return failure(err::enametoolong);
  std::memcpy(p, name, sizeof(name));
  return 0;
}
int setsid_fn() { return 1000; }
int setpgid_fn(int p, int group) {
  return (p == 0 || p == 1000) && (group == 0 || group == 1000) ? 0 : failure(err::eperm);
}
int personality_fn(std::uint32_t value) {
  return value == 0xffffffffu || value == 0 ? 0 : failure(err::einval);
}
int priority_tid(int who) {
  // Retain the existing virtual process/user selection for PRIO_PGRP/USER.
  return !who || who == 1000 || who == 10125 ? GuestThread::current()->tid() : who;
}
int getpriority_fn(int which, int who) {
  if (which < 0 || which > 2) return failure(err::einval);
  int nice = 0;
  const auto result = platform::get_thread_priority(priority_tid(who), &nice);
  return result ? failure(-result) : nice;
}
int setpriority_fn(int which, int who, int value) {
  if (which < 0 || which > 2) return failure(err::einval);
  const auto target = priority_tid(who);
  int old = 0;
  const auto found = platform::get_thread_priority(target, &old);
  if (found) return failure(-found);
  value = std::clamp(value, -20, 19);
  if (value == old) return 0;
  if (!platform::thread_hints_enabled() && value != -16 && value != -19 && old != -16 && old != -19)
    return failure(err::eperm);
  // Android init raises RLIMIT_NICE to 40, so an app may set any nice value on
  // its own threads. The platform maps audio bands to host audio scheduling and
  // the other bands to ordinary host priorities.
  const auto result = platform::set_thread_priority(target, value);
  static std::atomic<int> reports{0};
  if (reports.fetch_add(1, std::memory_order_relaxed) < 64)
    log::info("libc", "setpriority tid={} nice {} -> {} result={}", target, old, value, result);
  return result ? failure(-result) : 0;
}
// bionic nice(): the calling thread's nice value moved by incr, then read back
// (CRYENGINE lowers its worker priority this way, The Climb).
int nice_fn(int incr) {
  int old = 0;
  if (const auto found = platform::get_thread_priority(priority_tid(0), &old)) return failure(-found);
  if (setpriority_fn(0, 0, std::clamp(old + incr, -20, 19)) < 0) return -1;
  return getpriority_fn(0, 0);
}
// bionic pwd.h LP64 includes pw_gecos between gid and dir.
struct Passwd {
  GuestAddr name, password;
  std::uint32_t uid, gid;
  GuestAddr gecos, dir, shell;
};
static_assert(sizeof(Passwd) == 48);
int getpwuid_r_fn(unsigned id, Passwd* out, char* buffer, std::uint64_t size, Passwd** resultp) {
  if (!out || !buffer || !resultp) return err::einval;
  *resultp = nullptr;
  if (id != 10125) return 0;
  std::string name = "u0_a125", dir = process_config().guest_data_dir();
  auto need = name.size() + 1 + 2 + 1 + dir.size() + 1 + 16;
  if (size < need) return err::erange;
  auto copy = [&](const char* s) {
    auto p = buffer;
    auto n = std::strlen(s) + 1;
    std::memcpy(buffer, s, n);
    buffer += n;
    return h2g(p);
  };
  *out = {};
  out->name = copy(name.c_str());
  out->password = copy("x");
  out->uid = out->gid = id;
  out->gecos = copy("");
  out->dir = copy(dir.c_str());
  out->shell = copy("/system/bin/sh");
  *resultp = out;
  return 0;
}
int getpwnam_r_fn(const char* name, Passwd* out, char* buffer, std::uint64_t size,
                  Passwd** resultp) {
  return getpwuid_r_fn(name && std::strcmp(name, "u0_a125") == 0 ? 10125 : UINT32_MAX, out, buffer,
                       size, resultp);
}
Passwd* getpwuid_fn(unsigned id) {
  thread_local Passwd pwd;
  thread_local char buffer[8192];
  Passwd* out;
  int r = getpwuid_r_fn(id, &pwd, buffer, sizeof(buffer), &out);
  if (r) {
    failure(r);
    return nullptr;
  }
  return out;
}
struct Group {
  GuestAddr name, password;
  std::uint32_t gid, pad;
  GuestAddr members;
};
Group* getgrnam_fn(const char* name) {
  if (!name) return nullptr;
  unsigned id;
  if (std::strcmp(name, "inet") == 0)
    id = 3003;
  else if (std::strcmp(name, "sdcard_rw") == 0)
    id = 1015;
  else if (std::strcmp(name, "sdcard_r") == 0)
    id = 1028;
  else if (std::strcmp(name, "u0_a125") == 0)
    id = 10125;
  else
    return nullptr;
  thread_local std::string group_name;
  thread_local Group g;
  thread_local GuestAddr members[1]{};
  group_name = name;
  g = {h2g(group_name.c_str()), 0, id, 0, h2g(members)};
  return &g;
}
struct Sysinfo {
  std::int64_t uptime;
  std::uint64_t loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap;
  std::uint16_t procs, pad;
  std::uint32_t pad2;
  std::uint64_t totalhigh, freehigh;
  std::uint32_t unit, pad3;
};
static_assert(sizeof(Sysinfo) == 112);
int sysinfo_fn(Sysinfo* out) {
  if (!out) return failure(err::efault);
  *out = {};
  out->uptime = monotonic_ns() / 1000000000;
  out->totalram = guest_usable_memory();
  out->freeram = out->totalram / 2;
  out->procs = 1;
  out->unit = 1;
  return 0;
}
int terminal_fn(int fd) { return failure(FdTable::get().file(fd) ? err::enotty : err::ebadf); }
// Linux uapi linux/sysinfo.h, ARM unsigned long fields, 64 bytes. Units
// are KiB so even a Quest Pro's 12 GiB total fits the 32-bit counters.
struct Sysinfo32 {
  std::int32_t uptime;
  std::uint32_t loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap;
  std::uint16_t procs, pad;
  std::uint32_t totalhigh, freehigh, unit;
  unsigned char reserved[8];
};
static_assert(sizeof(Sysinfo32) == 64);
int sysinfo32(Sysinfo32* out) {
  if (!out) return failure(err::efault);
  *out = {}; out->uptime = static_cast<std::int32_t>(monotonic_ns() / 1000000000);
  out->totalram = static_cast<std::uint32_t>(guest_usable_memory() / 1024);
  out->freeram = out->totalram / 2; out->procs = 1; out->unit = 1024; return 0;
}
struct Limit32 { std::uint32_t current, maximum; };
int getrlimit32(int resource, Limit32* out) {
  if (!out) return failure(err::efault);
  Limit wide; auto r = getrlimit_fn(resource, &wide);
  if (!r) *out = {std::uint32_t(std::min(wide.current, std::uint64_t(UINT32_MAX))), std::uint32_t(std::min(wide.maximum, std::uint64_t(UINT32_MAX)))};
  return r;
}
int setrlimit32(int resource, const Limit32* in) {
  if (!in) return failure(err::efault);
  Limit wide{in->current == UINT32_MAX ? UINT64_MAX : in->current, in->maximum == UINT32_MAX ? UINT64_MAX : in->maximum};
  return setrlimit_fn(resource, &wide);
}
int prlimit32(int process, int resource, const Limit32* in, Limit32* out) {
  if (process && process != 1000) return failure(err::esrch);
  if (out && getrlimit32(resource, out)) return -1;
  return in ? setrlimit32(resource, in) : 0;
}
// bionic pwd.h deliberately omits pw_gecos on LP32 (pw_passwd alias).
struct Passwd32 { std::uint32_t name, password, uid, gid, dir, shell; };
int getpwuid_r32(unsigned id, Passwd32* out, char* buffer, std::uint64_t size, std::uint32_t* resultp) {
  if (!out || !buffer || !resultp) return err::einval;
  *resultp = 0; Passwd wide{}, *found = nullptr;
  int r = getpwuid_r_fn(id, &wide, buffer, size, &found);
  if (!r && found) {
    *out = {std::uint32_t(wide.name), std::uint32_t(wide.password), wide.uid, wide.gid, std::uint32_t(wide.dir), std::uint32_t(wide.shell)};
    *resultp = h2g32(out);
  }
  return r;
}
int getpwnam_r32(const char* name, Passwd32* out, char* buffer, std::uint64_t size, std::uint32_t* resultp) {
  return getpwuid_r32(name && !std::strcmp(name, "u0_a125") ? 10125 : UINT32_MAX, out, buffer, size, resultp);
}
Passwd32* getpwuid32(unsigned id) {
  thread_local GuestStorage storage;
  auto p = storage.resize(sizeof(Passwd32) + 8192); std::uint32_t out = 0;
  int r = getpwuid_r32(id, g2h<Passwd32>(p), g2h<char>(p + sizeof(Passwd32)), 8192, &out);
  if (r) { failure(r); return nullptr; } return g2h<Passwd32>(out);
}
}  // namespace
void register_process(HleLibrary& lib) {
  if (process_config().isa() == GuestIsa::a32) {
    abi32::adapt<&uname_fn>("uname"); abi32::adapt<&prctl32>("prctl"); abi32::adapt<&sysinfo32>("sysinfo");
    abi32::adapt<&getrlimit32>("getrlimit"); abi32::adapt<&setrlimit32>("setrlimit"); abi32::adapt<&prlimit32>("prlimit");
    abi32::adapt<&getrlimit_fn>("getrlimit64"); abi32::adapt<&setrlimit_fn>("setrlimit64"); abi32::adapt<&prlimit_fn>("prlimit64");
    abi32::adapt<&getpwuid_r32>("getpwuid_r"); abi32::adapt<&getpwnam_r32>("getpwnam_r"); abi32::adapt<&getpwuid32>("getpwuid");
  }
#define B(n, f) VENTURE_LIBC_BIND(lib, n, f)
  B("getpid", pid);
  B("getppid", ppid);
  B("getuid", uid);
  B("geteuid", uid);
  B("getgid", uid);
  B("getegid", uid);
  B("setuid", setuid_fn);
  B("setgid", setuid_fn);
  B("getpagesize", pagesize);
  // Functions that return a constant export guest code (MOVW/MOVZ, return):
  // a host call costs far more than the function, and getpagesize alone took
  // 1.5% of North Star's main thread through the run loop. The handlers stay
  // registered for syscall().
  const auto constant = [&lib](const char* name, std::uint32_t low) {
    if (low > 0xffffu) throw std::logic_error("constant libc function exceeds a 16-bit move");
    const std::array<std::uint32_t, 2> code =
        process_config().isa() == GuestIsa::a32 ? std::array{0xe3000000u | (low >> 12) << 16 | (low & 0xfffu), 0xe12fff1eu}
                                                : std::array{0x52800000u | low << 5, 0xd65f03c0u};
    lib.add_function(name, code_stub(code, name));
  };
  constant("getpid", pid());
  constant("getppid", ppid());
  constant("getuid", uid());
  constant("geteuid", uid());
  constant("getgid", uid());
  constant("getegid", uid());
  constant("getpagesize", pagesize());
  B("uname", uname_fn);
  B("sysconf", sysconf_fn);
  B("getrlimit", getrlimit_fn);
  B("getrlimit64", getrlimit_fn);
  B("setrlimit", setrlimit_fn);
  B("setrlimit64", setrlimit_fn);
  B("prlimit", prlimit_fn);
  B("prlimit64", prlimit_fn);
  B("getrusage", getrusage_fn);
  B("prctl", prctl_fn);
  if (process_config().isa() == GuestIsa::a32) B("cacheflush", cacheflush_fn);
  B("exit", exit_fn);
  B("_exit", exit_immediate);
  B("_Exit", exit_immediate);
  B("abort", abort_fn);
  B("fork", unsupported);
  B("vfork", unsupported);
  B("execve", unsupported);
  B("system", system_fn);
  B("posix_spawn", spawn_fn);
  B("posix_spawnp", spawn_fn);
  B("wait", wait_fn);
  B("waitpid", waitpid_fn);
  B("execl", unsupported);
  B("execv", unsupported);
  B("execvp", unsupported);
  B("execvpe", unsupported);
  B("wait4", wait4_fn);
  B("isatty", isatty_fn);
  B("ttyname", ttyname_fn);
  B("getlogin", login_fn);
  B("gethostname", hostname_fn);
  B("setsid", setsid_fn);
  B("setpgid", setpgid_fn);
  B("personality", personality_fn);
  B("getpriority", getpriority_fn);
  B("setpriority", setpriority_fn);
  B("nice", nice_fn);
  B("getpwuid", getpwuid_fn);
  B("getpwuid_r", getpwuid_r_fn);
  B("getpwnam_r", getpwnam_r_fn);
  B("getgrnam", getgrnam_fn);
  B("sysinfo", sysinfo_fn);
  B("tcflush", terminal_fn);
  B("tcgetattr", terminal_fn);
  B("tcsetattr", terminal_fn);
#undef B
}
}  // namespace venture::hle::libc
