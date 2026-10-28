#include "platform/memory.h"
#include "platform/process_info.h"
#include "core/address_space.h"
#include "core/error.h"
#include <cerrno>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <string>
#include <sstream>

namespace venture::platform {
std::optional<std::uint64_t> committed_bytes(GuestAddr, std::uint64_t) {
  // Linux anonymous mappings use overcommit; resident pages are not commit.
  return std::nullopt;
}
bool process_debugger_attached() {
  std::ifstream input("/proc/self/status");
  for (std::string line; std::getline(input, line);)
    if (line.starts_with("TracerPid:")) return std::stoul(line.substr(10)) != 0;
  throw std::runtime_error("Cannot query process debugger state");
}
bool user_shadow_stack_active() {
#if defined(__linux__) && defined(__x86_64__)
  // arch_prctl(ARCH_SHSTK_STATUS) from Linux asm/prctl.h: bit 0 is
  // ARCH_SHSTK_SHSTK. Kernels without user shadow stacks fail the call.
  constexpr int arch_shstk_status = 0x5005;
  unsigned long features = 0;
  return syscall(SYS_arch_prctl, arch_shstk_status, &features) == 0 && (features & 1);
#else
  return false;
#endif
}
std::string mutex_owner_thread(std::recursive_mutex& m) {
#if defined(__linux__) && defined(__GLIBC__)
  // glibc records the owner's kernel TID in the mutex; a racy read is fine here.
  const int tid = m.native_handle()->__data.__owner;
  if (!tid) return "none";
  std::ifstream comm("/proc/self/task/" + std::to_string(tid) + "/comm");
  std::string name;
  std::getline(comm, name);
  return std::to_string(tid) + " (" + name + ")";
#else
  (void)m;
  return "unknown";
#endif
}
bool process_memory(ProcessMemory& out) {
  // Linux Documentation/filesystems/proc.rst: smaps_rollup Pss includes the
  // proportional share of mapped pages; Private_Clean/Dirty are unshared.
  std::ifstream input("/proc/self/smaps_rollup");
  std::uint64_t pss = 0, private_kib = 0;
  bool found = false;
  for (std::string line; std::getline(input, line);) {
    std::istringstream fields(line);
    std::string key;
    std::uint64_t value;
    if (!(fields >> key >> value)) continue;
    if (key == "Pss:") { pss = value; found = true; }
    if (key == "Private_Clean:" || key == "Private_Dirty:" || key == "Private_Hugetlb:")
      private_kib += value;
  }
  if (!found) return false;
  out = {private_kib, pss > private_kib ? pss - private_kib : 0};
  return true;
}
bool process_resident_memory(ProcessResidentMemory& out) {
  std::ifstream input("/proc/self/status");
  ProcessResidentMemory result;
  unsigned found = 0;
  for (std::string line; std::getline(input, line);) {
    std::istringstream fields(line);
    std::string key;
    std::uint64_t value;
    if (!(fields >> key >> value)) continue;
    if (key == "VmRSS:") { result.resident_kib = value; found |= 1; }
    if (key == "VmHWM:") { result.peak_resident_kib = value; found |= 2; }
  }
  if (found != 3) return false;
  out = result;
  return true;
}
namespace {
__attribute__((target("cx16"))) bool cas128(GuestAddr p, const void* value, const void* expected) {
  unsigned __int128 v, e;
  std::memcpy(&v, value, 16); std::memcpy(&e, expected, 16);
  return __sync_bool_compare_and_swap(g2h<unsigned __int128>(p), e, v);
}
int protection(std::uint32_t p) {
  return ((p & (kProtRead | kProtExec)) ? PROT_READ : 0) | ((p & kProtWrite) ? PROT_WRITE : 0);
}
}
std::uint64_t reservation_granularity() { return 4096; }
GuestAddr reserve(GuestAddr address, std::uint64_t size, bool exact) {
  auto p = mmap(g2h(address), size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS |
                (exact ? MAP_FIXED_NOREPLACE : 0), -1, 0);
  return p == MAP_FAILED ? 0 : h2g(p);
}
void release(GuestAddr address, std::uint64_t size) { munmap(g2h(address), size); }
bool commit(GuestAddr address, std::uint64_t size, std::uint32_t prot) { return protect(address, size, prot); }
bool decommit(GuestAddr address, std::uint64_t size) {
  return mprotect(g2h(address), size, PROT_NONE) == 0 && madvise(g2h(address), size, MADV_DONTNEED) == 0;
}
bool protect(GuestAddr address, std::uint64_t size, std::uint32_t prot) { return mprotect(g2h(address), size, protection(prot)) == 0; }
int map_shared_file(int fd, std::uint64_t offset, std::uint64_t size, std::uint32_t prot, FileMapping& out) {
  auto* view = mmap(nullptr, size, protection(prot), MAP_SHARED, fd, static_cast<off_t>(offset));
  if (view == MAP_FAILED) {
    switch (errno) { case EACCES: return -err::kEACCES; case EBADF: return -err::kEBADF;
      case EINVAL: return -err::kEINVAL; default: return -err::kENOMEM; }
  }
  out = {h2g(view), h2g(view), size}; return 0;
}
void unmap_shared_file(const FileMapping& mapping) { munmap(g2h(mapping.base), mapping.size); }
int sync_shared_file(GuestAddr address, std::uint64_t size, bool synchronous) {
  return msync(g2h(address), size, synchronous ? MS_SYNC : MS_ASYNC) == 0 ? 0 : -err::kEIO;
}
#if defined(__linux__) && defined(__x86_64__)
namespace {
bool guarded_access_ready();
bool guarded_read(GuestAddr address, void* out, std::size_t size);
}
#endif
bool read_memory(GuestAddr address, void* out, std::size_t size) {
  if (address + size < address) return false;
#if defined(__linux__) && defined(__x86_64__)
  // As on Windows, a naturally aligned 1, 2, 4 or 8-byte read is one load of
  // that width (ARMv8 single-copy atomicity); process_vm_readv promises none.
  if (size && size <= 8 && !(size & (size - 1)) && !(address & (size - 1)) && guarded_access_ready())
    return guarded_read(address, out, size);
  // Other sizes are a fault-contained memcpy, like the Windows SEH copy: HLE
  // string functions read guest blocks on every call, and process_vm_readv
  // (plus getpid) cost two syscalls each.
  if (guarded_access_ready()) {
    NativeMemoryFault fault;
    return native_copy(h2g(out), address, size, fault);
  }
#endif
  iovec local{out, size}, remote{g2h(address), size};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}
bool write_memory(GuestAddr address, const void* data, std::size_t size) {
  if (address + size < address) return false;
  iovec local{const_cast<void*>(data), size}, remote{g2h(address), size};
  return process_vm_writev(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}
bool memory_accessible(GuestAddr address, std::size_t size, bool write) {
  if (address + size < address) return false;
  std::ifstream maps("/proc/self/maps");
  std::string line;
  auto cursor = address;
  while (std::getline(maps, line) && cursor < address + size) {
    unsigned long long start{}, end{};
    char permissions[5]{};
    if (std::sscanf(line.c_str(), "%llx-%llx %4s", &start, &end, permissions) != 3) continue;
    if (start > cursor) return false;
    if (cursor >= start && cursor < end) {
      if (permissions[0] != 'r' || (write && permissions[1] != 'w')) return false;
      cursor = end;
    }
  }
  return cursor >= address + size;
}
bool compare_exchange(GuestAddr address, const void* value, void* expected, std::size_t size) {
  if (address & (size - 1)) return false;
  // Callback path is used only after fastmem faults. Never dereference a bad address.
  if (!memory_accessible(address, size, true)) return false;
  switch (size) {
    case 1: return __atomic_compare_exchange_n(g2h<std::uint8_t>(address), static_cast<std::uint8_t*>(expected), *static_cast<const std::uint8_t*>(value), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    case 2: return __atomic_compare_exchange_n(g2h<std::uint16_t>(address), static_cast<std::uint16_t*>(expected), *static_cast<const std::uint16_t*>(value), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    case 4: return __atomic_compare_exchange_n(g2h<std::uint32_t>(address), static_cast<std::uint32_t*>(expected), *static_cast<const std::uint32_t*>(value), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    case 8: return __atomic_compare_exchange_n(g2h<std::uint64_t>(address), static_cast<std::uint64_t*>(expected), *static_cast<const std::uint64_t*>(value), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    case 16: return cas128(address, value, expected);
    default: return false;
  }
}
}

#if defined(__linux__) && defined(__x86_64__)
#include <csignal>
#include <ucontext.h>
#include <csetjmp>
// System V AMD64 calling convention: rdi, rsi, rdx, rcx.
#define VENTURE_A0 "%rdi"
#define VENTURE_A1 "%rsi"
#define VENTURE_A2 "%rdx"
#define VENTURE_A3 "%rcx"
#include "platform/guarded_atomics.inc"

namespace venture::platform {
#include "platform/native_memory.inc"
namespace {
struct NativeMemoryScope {
  sigjmp_buf recovery; NativeMemoryCall call; NativeMemoryScope* previous;
  volatile GuestAddr fault_address{}; volatile bool fault_write{}, fault_bus{};
};
thread_local NativeMemoryScope* native_memory_scope = nullptr;
struct sigaction previous_segv{}, previous_bus{};
// Installed on first use, so it runs before (and chains to) handlers installed
// earlier, such as Dynarmic's fastmem handler.
void guarded_fault(int signal, siginfo_t* info, void* raw_context) {
  auto& rip = static_cast<ucontext_t*>(raw_context)->uc_mcontext.gregs[REG_RIP];
  if (guarded_ip(static_cast<std::uintptr_t>(rip))) {
    rip = reinterpret_cast<greg_t>(venture_guarded_fault);
    return;
  }
  if (auto* scope = native_memory_scope; scope && info->si_code > 0) {
    const auto& machine = static_cast<ucontext_t*>(raw_context)->uc_mcontext;
    const bool write = (machine.gregs[REG_ERR] & 2) != 0;
    const bool execute = (machine.gregs[REG_ERR] & 16) != 0;
    if (!execute && native_fault_matches(scope->call, h2g(info->si_addr), write)) {
      scope->fault_address = h2g(info->si_addr); scope->fault_write = write;
      scope->fault_bus = signal == SIGBUS;
      // sigsetjmp(..., 0) avoids a mask-query syscall on every successful CRT
      // call. Restore the interrupted mask only on this fault path.
      sigprocmask(SIG_SETMASK, &static_cast<ucontext_t*>(raw_context)->uc_sigmask, nullptr);
      siglongjmp(scope->recovery, 1);
    }
  }
  const auto& previous = signal == SIGSEGV ? previous_segv : previous_bus;
  if (previous.sa_flags & SA_SIGINFO) return previous.sa_sigaction(signal, info, raw_context);
  if (previous.sa_handler == SIG_IGN) return;
  if (previous.sa_handler == SIG_DFL) {
    std::signal(signal, SIG_DFL);  // the faulting instruction re-executes with the default action
    return;
  }
  previous.sa_handler(signal);
}
bool guarded_access_ready() {
  static const bool ready = [] {
    struct sigaction action{};
    action.sa_sigaction = guarded_fault;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&action.sa_mask);
    // Snapshot both previous actions before the handler can run: a fault on
    // another thread must never chain through an empty snapshot (SIG_DFL).
    return sigaction(SIGSEGV, nullptr, &previous_segv) == 0 && sigaction(SIGBUS, nullptr, &previous_bus) == 0 &&
           sigaction(SIGSEGV, &action, nullptr) == 0 && sigaction(SIGBUS, &action, nullptr) == 0;
  }();
  return ready;
}
}
bool native_memory(NativeMemoryOp op, GuestAddr destination, GuestAddr source, std::size_t size,
                   int value, std::uint64_t& result, NativeMemoryFault& fault, bool source_guest) {
  if (!guarded_access_ready()) { fault = {source, false}; return false; }
  NativeMemoryScope scope{{}, {op, destination, source, size, value}, native_memory_scope};
  scope.call.source_guest = source_guest;
  if (!native_ranges_valid(scope.call)) { fault = scope.call.fault; return false; }
  if (sigsetjmp(scope.recovery, 0)) {
    native_memory_scope = scope.previous;
    fault = {scope.fault_address, scope.fault_write, scope.fault_bus}; return false;
  }
  native_memory_scope = &scope;
  invoke_native_memory(scope.call);
  native_memory_scope = scope.previous;
  result = scope.call.result; return true;
}
bool guarded_access_ip(std::uintptr_t ip) { return guarded_ip(ip); }
}
#else
namespace venture::platform {
bool guarded_access_ip(std::uintptr_t) { return false; }
bool native_memory(NativeMemoryOp, GuestAddr, GuestAddr source, std::size_t, int,
                   std::uint64_t&, NativeMemoryFault& fault, bool) {
  fault = {source, false}; return false;
}
// Hosts without the guarded routines check the mapping first; a racing unmap
// remains fatal there.
bool atomic_rmw(GuestAddr address, unsigned size, AtomicRmw op, std::uint64_t value,
                std::uint64_t desired, std::uint64_t& old) {
  if ((size != 1 && size != 2 && size != 4 && size != 8) || (address & (size - 1)) ||
      !memory_accessible(address, size, true))
    return false;
  const auto apply = [&]<class T>(T* target) {
    T previous{};
    switch (op) {
      case AtomicRmw::add: previous = __atomic_fetch_add(target, static_cast<T>(value), __ATOMIC_SEQ_CST); break;
      case AtomicRmw::exchange: previous = __atomic_exchange_n(target, static_cast<T>(value), __ATOMIC_SEQ_CST); break;
      case AtomicRmw::compare_exchange:
        previous = static_cast<T>(value);
        __atomic_compare_exchange_n(target, &previous, static_cast<T>(desired), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        break;
    }
    old = previous;
    return true;
  };
  switch (size) {
    case 1: return apply(g2h<std::uint8_t>(address));
    case 2: return apply(g2h<std::uint16_t>(address));
    case 4: return apply(g2h<std::uint32_t>(address));
    default: return apply(g2h<std::uint64_t>(address));
  }
}
bool guarded_load32(GuestAddr address, std::uint32_t& value) {
  return read_memory(address, &value, sizeof(value));
}
bool guarded_store(GuestAddr address, const void* data, std::size_t size) {
  return write_memory(address, data, size);
}
}
#endif

namespace venture::platform {
bool native_copy(GuestAddr d, GuestAddr s, std::size_t n, NativeMemoryFault& f) {
  std::uint64_t result;
  return native_memory(NativeMemoryOp::copy, d, s, n, 0, result, f);
}
bool native_move(GuestAddr d, GuestAddr s, std::size_t n, NativeMemoryFault& f) {
  std::uint64_t result;
  return native_memory(NativeMemoryOp::move, d, s, n, 0, result, f);
}
bool native_fill(GuestAddr d, int v, std::size_t n, NativeMemoryFault& f) {
  std::uint64_t result;
  return native_memory(NativeMemoryOp::fill, d, 0, n, v, result, f);
}
bool native_compare(GuestAddr a, GuestAddr b, std::size_t n, int& result, NativeMemoryFault& f) {
  std::uint64_t value;
  if (!native_memory(NativeMemoryOp::compare, a, b, n, 0, value, f)) return false;
  result = static_cast<int>(value); return true;
}
}
