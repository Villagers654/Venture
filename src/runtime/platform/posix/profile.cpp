#include "platform/profile.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <fstream>
#include <link.h>
#include <linux/futex.h>
#include <mutex>
#include <sstream>
#include <string_view>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <thread>
#include <ucontext.h>
#include <unistd.h>

// Linux sampling: an otherwise unhandled real-time signal parks the target in its handler
// (the counterpart of SuspendThread + GetThreadContext): the handler publishes
// the interrupted RIP/RBP and waits on a futex until the sampler, which reads
// guest CPU state meanwhile, releases it. The handler never allocates, logs or
// locks. Per-thread CPU time comes from the kernel's thread CPU clocks, threads
// from /proc/self/task, modules from dl_iterate_phdr and their dynamic symbols.
namespace venture::platform {
namespace {
enum : std::uint32_t { idle, requested, captured, released, done };
struct Slot {
  std::atomic<std::uint32_t> state{idle};
  std::atomic<pid_t> target{0};
  std::uintptr_t rip = 0, rbp = 0;
};
Slot slot;
std::mutex sampler_mutex;  // samplers only; never taken by a parked target
long futex(std::atomic<std::uint32_t>& word, int op, std::uint32_t value, const timespec* timeout = nullptr) {
  return syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&word), op | FUTEX_PRIVATE_FLAG, value, timeout, nullptr, 0);
}
// Wait while `word == value`, up to `ns`. False on timeout.
bool wait_change(std::atomic<std::uint32_t>& word, std::uint32_t value, std::int64_t ns) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(ns);
  while (word.load(std::memory_order_acquire) == value) {
    const auto left = deadline - std::chrono::steady_clock::now();
    if (left <= std::chrono::nanoseconds::zero()) return false;
    const auto n = std::chrono::duration_cast<std::chrono::nanoseconds>(left).count();
    timespec timeout{static_cast<time_t>(n / 1'000'000'000), static_cast<long>(n % 1'000'000'000)};
    futex(word, FUTEX_WAIT, value, &timeout);
  }
  return true;
}
pid_t gettid_raw() { return static_cast<pid_t>(syscall(SYS_gettid)); }
void park(int, siginfo_t*, void* raw) {
  const int saved = errno;
  std::uint32_t expected = requested;
  // A stale or foreign delivery finds no request for this thread: return at once.
  if (slot.target.load(std::memory_order_acquire) == gettid_raw() &&
      slot.state.compare_exchange_strong(expected, captured, std::memory_order_acq_rel)) {
    const auto& context = static_cast<ucontext_t*>(raw)->uc_mcontext;
    slot.rip = static_cast<std::uintptr_t>(context.gregs[REG_RIP]);
    slot.rbp = static_cast<std::uintptr_t>(context.gregs[REG_RBP]);
    futex(slot.state, FUTEX_WAKE, 1);
    // Bounded: a sampler that vanished must not wedge the target.
    wait_change(slot.state, captured, 2'000'000'000);
    slot.state.store(done, std::memory_order_release);
    futex(slot.state, FUTEX_WAKE, 1);
  }
  errno = saved;
}
bool unclaimed(const struct sigaction& action) {
  return !(action.sa_flags & SA_SIGINFO) && action.sa_handler == SIG_DFL;
}
// No real-time signal is reserved for a library: drivers, the JVM and other
// in-process code install handlers wherever they like. Sampling therefore
// claims a signal nobody has a handler on (searching down from SIGRTMAX, away
// from the low numbers libraries tend to hard-code), and never replaces one.
int claim_signal() {
  struct sigaction action{};
  action.sa_sigaction = park;
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&action.sa_mask);
  for (int signal = SIGRTMAX; signal >= SIGRTMIN; --signal) {
    struct sigaction previous{};
    if (sigaction(signal, nullptr, &previous) || !unclaimed(previous)) continue;
    if (sigaction(signal, &action, &previous)) continue;
    if (unclaimed(previous)) return signal;
    sigaction(signal, &previous, nullptr);  // claimed meanwhile: give it back
  }
  return 0;
}
int sample_signal() {
  static const int signal = claim_signal();
  return signal;
}
// Sampling is available while the claimed signal still runs `park`. Code that
// later installs its own handler there owns the signal: sampling then reports
// unavailable instead of sending that handler requests it does not expect.
bool install_handler() {
  const int signal = sample_signal();
  struct sigaction current{};
  return signal && !sigaction(signal, nullptr, &current) && (current.sa_flags & SA_SIGINFO) &&
         current.sa_sigaction == park;
}
clockid_t thread_clock(std::uint32_t tid) {
  // Linux posix-cpu-timers ABI (MAKE_THREAD_CPUCLOCK(tid, CPUCLOCK_SCHED)), as
  // glibc's pthread_getcpuclockid; usable for any thread of this process.
  return static_cast<clockid_t>((~static_cast<clockid_t>(tid) << 3) | 6);
}
bool read_self(std::uintptr_t address, void* out, std::size_t size) {
  iovec local{out, size}, remote{reinterpret_cast<void*>(address), size};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}
std::string task_file(std::uint32_t tid, const char* name) {
  std::ifstream input("/proc/self/task/" + std::to_string(tid) + "/" + name);
  std::stringstream text; text << input.rdbuf(); return text.str();
}
ProfileThread describe(std::uint32_t tid) {
  ProfileThread t{tid, task_file(tid, "comm"), std::make_shared<std::uint32_t>(tid)};
  if (!t.name.empty() && t.name.back() == '\n') t.name.pop_back();
  // proc_pid_stat(5): starttime is field 22, counted after the parenthesized comm.
  const auto stat = task_file(tid, "stat");
  if (const auto close = stat.rfind(')'); close != stat.npos) {
    std::istringstream fields(stat.substr(close + 2));
    std::string field;
    for (int i = 3; i <= 22 && fields >> field; ++i)
      if (i == 22) t.creation_ticks = std::strtoull(field.c_str(), nullptr, 10);
  }
  return t;
}
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}
// Dynamic symbol count from DT_HASH, or by walking DT_GNU_HASH chains.
std::size_t symbol_count(const ElfW(Word)* hash, const std::uint32_t* gnu) {
  if (hash) return hash[1];
  if (!gnu) return 0;
  const auto buckets = gnu[0], offset = gnu[1], bloom = gnu[2];
  const auto* bucket = gnu + 4 + bloom * (sizeof(ElfW(Addr)) / 4);
  const auto* chain = bucket + buckets;
  std::uint32_t last = 0;
  for (std::uint32_t i = 0; i < buckets; ++i) last = std::max(last, bucket[i]);
  if (last < offset) return offset;
  while (!(chain[last - offset] & 1)) ++last;
  return last + 1;
}
std::vector<ProfileSymbol> exports(const dl_phdr_info& info) {
  std::vector<ProfileSymbol> out;
  const ElfW(Dyn)* dynamic = nullptr;
  for (int i = 0; i < info.dlpi_phnum; ++i)
    if (info.dlpi_phdr[i].p_type == PT_DYNAMIC)
      dynamic = reinterpret_cast<const ElfW(Dyn)*>(info.dlpi_addr + info.dlpi_phdr[i].p_vaddr);
  if (!dynamic) return out;
  // ld.so relocates most d_ptr entries in place; the vDSO's stay link-time.
  const auto pointer = [&](ElfW(Addr) value) { return value < info.dlpi_addr ? value + info.dlpi_addr : value; };
  const ElfW(Sym)* symbols = nullptr; const char* strings = nullptr;
  const ElfW(Word)* hash = nullptr; const std::uint32_t* gnu = nullptr;
  for (auto* d = dynamic; d->d_tag != DT_NULL; ++d) {
    if (d->d_tag == DT_SYMTAB) symbols = reinterpret_cast<const ElfW(Sym)*>(pointer(d->d_un.d_ptr));
    else if (d->d_tag == DT_STRTAB) strings = reinterpret_cast<const char*>(pointer(d->d_un.d_ptr));
    else if (d->d_tag == DT_HASH) hash = reinterpret_cast<const ElfW(Word)*>(pointer(d->d_un.d_ptr));
    else if (d->d_tag == DT_GNU_HASH) gnu = reinterpret_cast<const std::uint32_t*>(pointer(d->d_un.d_ptr));
  }
  if (!symbols || !strings) return out;
  const auto count = symbol_count(hash, gnu);
  for (std::size_t i = 1; i < count; ++i) {
    const auto& s = symbols[i];
    if (ELF64_ST_TYPE(s.st_info) != STT_FUNC && ELF64_ST_TYPE(s.st_info) != STT_GNU_IFUNC) continue;
    if (s.st_shndx == SHN_UNDEF || !s.st_value) continue;
    const auto begin = info.dlpi_addr + s.st_value;
    out.push_back({begin, begin + std::max<ElfW(Xword)>(s.st_size, 1), strings + s.st_name});
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.begin < b.begin; });
  return out;
}
}  // namespace
bool profile_supported() { return install_handler(); }
int profile_sample_signal() noexcept { return install_handler() ? sample_signal() : 0; }
std::uint32_t profile_thread_id() noexcept { return static_cast<std::uint32_t>(gettid_raw()); }
ProfilePowerThrottling profile_power_throttling() noexcept {
  // Linux has no per-process EcoQoS. The execution-speed limit it does have is
  // a cgroup v2 CPU quota: report it as the throttling state (bit 0).
  try {
    std::ifstream cgroup("/proc/self/cgroup");
    std::string line, path;
    while (std::getline(cgroup, line)) if (line.starts_with("0::")) path = line.substr(3);
    if (path.empty()) return {};
    std::ifstream max("/sys/fs/cgroup" + path + "/cpu.max");
    std::string quota;
    const bool limited = (max >> quota) && quota != "max";
    return {true, limited ? 1u : 0u, limited ? 1u : 0u, 0};
  } catch (...) { return {}; }
}
ProfileThread profile_current_thread() { return describe(profile_thread_id()); }
std::vector<ProfileThread> profile_threads() {
  std::vector<ProfileThread> out;
  const auto self = profile_thread_id();
  if (auto* directory = opendir("/proc/self/task")) {
    while (auto* entry = readdir(directory)) {
      const auto tid = static_cast<std::uint32_t>(std::strtoul(entry->d_name, nullptr, 10));
      if (tid && tid != self) out.push_back(describe(tid));
    }
    closedir(directory);
  }
  return out;
}
bool profile_cpu_time(const ProfileThread& t, std::uint64_t& ns) noexcept {
  timespec value{};
  if (clock_gettime(thread_clock(t.id), &value)) return false;
  ns = std::uint64_t(value.tv_sec) * 1'000'000'000 + std::uint64_t(value.tv_nsec);
  return true;
}
std::int64_t profile_cpu_cycles(const ProfileThread&) noexcept { return -1; }
bool profile_sample(const ProfileThread& t, const Cpu* cpu, const std::atomic<std::uint32_t>* activity, ProfileSample& out) noexcept {
  if (!install_handler() || t.id == profile_thread_id()) return false;
  std::lock_guard lock(sampler_mutex);
  slot.target.store(static_cast<pid_t>(t.id), std::memory_order_release);
  slot.state.store(requested, std::memory_order_release);
  if (syscall(SYS_tgkill, getpid(), static_cast<pid_t>(t.id), sample_signal())) {
    slot.state.store(idle, std::memory_order_release);
    return false;
  }
  // A thread blocking the signal (or gone) leaves it pending; cancel the
  // request so a late delivery returns immediately.
  if (!wait_change(slot.state, requested, 50'000'000)) {
    std::uint32_t expected = requested;
    if (slot.state.compare_exchange_strong(expected, idle, std::memory_order_acq_rel)) {
      slot.target.store(0, std::memory_order_release);
      return false;
    }
  }
  if (activity) out.activity = activity->load(std::memory_order_relaxed);
  out.ip = slot.rip;
  if (cpu) {
    const auto snapshot = cpu->profile_snapshot(out.ip);
    out.guest_pc = snapshot.pc; out.translated = snapshot.translated;
    out.outlined_atomics = snapshot.outlined_atomics; out.outlined_counts = snapshot.outlined_counts;
    if (out.inspect_state) {
      out.spilled_register_count = cpu->isa() == GuestIsa::a64 ? 31 : 16;
      for (unsigned i = 0; i < out.spilled_register_count; ++i) out.spilled_registers[i] = cpu->x(i);
      if (cpu->isa() == GuestIsa::a64) {
        for (unsigned i = 0; i < out.register_memory.size(); ++i) {
          auto& probe = out.register_memory[i];
          probe.address = out.spilled_registers[19 + i];
          probe.readable = probe.address && read_self(probe.address, probe.words.data(), sizeof(probe.words));
          if (!probe.readable) probe.words = {};
        }
        out.host_rbp = slot.rbp;
        for (std::uint64_t fp = out.spilled_registers[29]; out.frame_count < out.frame_returns.size() && fp && !(fp & 7);) {
          std::array<std::uint64_t, 2> record{};
          if (!read_self(fp, record.data(), sizeof(record))) break;
          out.frame_returns[out.frame_count++] = record[1];
          if (record[0] <= fp || record[0] - fp > (1u << 20)) break;
          if (out.frame_count <= 2) {
            const auto words = std::min<std::uint64_t>((record[0] - fp - 16) / 8, 64);
            const auto take = std::min<std::uint64_t>(words, out.stack_words.size() - out.stack_word_count);
            if (take && read_self(fp + 16, out.stack_words.data() + out.stack_word_count, take * 8))
              out.stack_word_count += static_cast<unsigned>(take);
          }
          fp = record[0];
        }
      }
    }
  }
  slot.state.store(released, std::memory_order_release);
  futex(slot.state, FUTEX_WAKE, 1);
  wait_change(slot.state, released, 2'000'000'000);
  slot.target.store(0, std::memory_order_release);
  slot.state.store(idle, std::memory_order_release);
  profile_cpu_time(t, out.cpu_ns);
  return true;
}
std::vector<ProfileModule> profile_modules() {
  std::vector<ProfileModule> out;
  dl_iterate_phdr([](dl_phdr_info* info, std::size_t, void* data) {
    auto& modules = *static_cast<std::vector<ProfileModule>*>(data);
    std::uintptr_t begin = UINTPTR_MAX, end = 0;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
      const auto& p = info->dlpi_phdr[i];
      if (p.p_type != PT_LOAD) continue;
      begin = std::min<std::uintptr_t>(begin, info->dlpi_addr + p.p_vaddr);
      end = std::max<std::uintptr_t>(end, info->dlpi_addr + p.p_vaddr + p.p_memsz);
    }
    if (begin >= end) return 0;
    std::string path = info->dlpi_name ? info->dlpi_name : "";
    if (path.empty() && modules.empty()) path = profile_executable().string();  // the main program
    const auto slash = path.rfind('/');
    ProfileModule module{begin, end, lower(slash == path.npos ? path : path.substr(slash + 1)), path,
                         path.starts_with("/usr/") || path.starts_with("/lib"), false, exports(*info)};
    module.vulkan_driver = std::ranges::any_of(module.exports, [](const auto& s) { return s.name == "vk_icdGetInstanceProcAddr"; });
    modules.push_back(std::move(module));
    return 0;
  }, &out);
  return out;
}
std::vector<ProfileSymbol> profile_wait_symbols() {
  // A thread blocked in the kernel is parked right after a SYSCALL instruction.
  // glibc's cancellable wrappers are not exported, so mark every SYSCALL return
  // site in libc's executable segments instead of naming wrappers. The sample
  // signal is SA_RESTART: a wait the kernel restarts (-ERESTARTSYS, as in a DRM
  // fence ioctl or an untimed futex) shows the handler the SYSCALL itself, the
  // IP rewound by its two bytes, so that address marks a wait as well.
  std::vector<ProfileSymbol> out;
  dl_iterate_phdr([](dl_phdr_info* info, std::size_t, void* data) {
    auto& sites = *static_cast<std::vector<ProfileSymbol>*>(data);
    const std::string_view name = info->dlpi_name ? info->dlpi_name : "";
    if (!name.ends_with("/libc.so.6")) return 0;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
      const auto& p = info->dlpi_phdr[i];
      if (p.p_type != PT_LOAD || !(p.p_flags & PF_X)) continue;
      const auto* code = reinterpret_cast<const unsigned char*>(info->dlpi_addr + p.p_vaddr);
      for (std::size_t j = 0; j + 1 < p.p_memsz; ++j)
        if (code[j] == 0x0f && code[j + 1] == 0x05) {
          const auto at = reinterpret_cast<std::uintptr_t>(code + j);
          sites.push_back({at, at + 1, "libc!syscall"});
          sites.push_back({at + 2, at + 3, "libc!syscall"});
        }
    }
    return 1;
  }, &out);
  return out;
}
std::uintptr_t profile_image_base() {
  std::uintptr_t base = 0;
  // The first dl_iterate_phdr entry is the main program; its bias is the PIE base.
  dl_iterate_phdr([](dl_phdr_info* info, std::size_t, void* data) {
    *static_cast<std::uintptr_t*>(data) = info->dlpi_addr;
    return 1;
  }, &base);
  return base;
}
std::filesystem::path profile_executable() {
  std::error_code error;
  return std::filesystem::read_symlink("/proc/self/exe", error);
}
void profile_delay(std::int64_t ns) {
  // The sampler's own wakeups should not be coalesced by the default 50 us slack.
  thread_local const bool slack = prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0) == 0;
  (void)slack;
  std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
}
}  // namespace venture::platform
