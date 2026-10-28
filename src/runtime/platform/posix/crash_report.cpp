#include "core/pc_trace.h"
#include "core/api_result_trace.h"
#include "platform/crash_report.h"
#include "platform/memory.h"
#include "common/log.h"
#include <csignal>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <iterator>
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <ucontext.h>

namespace venture::platform {
namespace {
constexpr int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
struct sigaction previous[std::size(signals)]{};
thread_local char alternate_stack[65536];
std::atomic_flag reporting = ATOMIC_FLAG_INIT;

void location(void* address, char* output, std::size_t size) {
  Dl_info module{};
  const char* name = "unknown";
  std::uintptr_t base = 0;
  if (dladdr(address, &module)) {
    name = module.dli_fname;
    for (const char* p = name; *p; ++p) if (*p == '/') name = p + 1;
    base = reinterpret_cast<std::uintptr_t>(module.dli_fbase);
  }
  std::snprintf(output, size, "%s+0x%llx", name,
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address) - base));
}
void host_signal(int signal, siginfo_t* info, void* raw_context) {
  const struct sigaction* prior = nullptr;
  for (unsigned i = 0; i < std::size(signals); ++i) if (signals[i] == signal) prior = &previous[i];
  // Existing handlers get first refusal. The guarded-read and Dynarmic
  // handlers installed after us instead call us only for unhandled host faults.
  if (prior && prior->sa_handler != SIG_DFL && prior->sa_handler != SIG_IGN) {
    if (prior->sa_flags & SA_SIGINFO) prior->sa_sigaction(signal, info, raw_context);
    else prior->sa_handler(signal);
    return;
  }
  if (!reporting.test_and_set(std::memory_order_relaxed)) {
    venture::log::crash_write("", 0);
    venture::api_result_trace::crash_flush();
    venture::pc_trace::flush();
    void* address = info->si_addr;
#if defined(__x86_64__)
    address = reinterpret_cast<void*>(static_cast<ucontext_t*>(raw_context)->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
    address = reinterpret_cast<void*>(static_cast<ucontext_t*>(raw_context)->uc_mcontext.pc);
#endif
    char where[1024], line[1200];
    location(address, where, sizeof(where));
    auto n = std::snprintf(line, sizeof(line), " E [runtime] Host exception 0x%x at %s (thread %ld)\n",
                           signal, where, static_cast<long>(syscall(SYS_gettid)));
    venture::log::crash_write(line, n);
    void* frames[16];
    const int count = backtrace(frames, std::size(frames));
    for (int i = 0; i < count; ++i) {
      location(frames[i], where, sizeof(where));
      n = std::snprintf(line, sizeof(line), " E [host-stack]   #%d %s\n", i, where);
      venture::log::crash_write(line, n);
    }
  }
  struct sigaction action{};
  action.sa_handler = SIG_DFL;
  sigemptyset(&action.sa_mask);
  sigaction(signal, &action, nullptr);
  // Delivery is blocked while inside the handler; re-raise on this thread and
  // let return restore the original mask. Do not change the core-dump policy.
  raise(signal);
}
}
void prepare_crash_stack() {
  stack_t current{};
  if (sigaltstack(nullptr, &current) == 0 && !(current.ss_flags & SS_DISABLE)) return;
  stack_t stack{};
  stack.ss_sp = alternate_stack;
  stack.ss_size = sizeof(alternate_stack);
  sigaltstack(&stack, nullptr);
}
void install_crash_report(std::string_view, std::string_view) {
  static bool installed = false;
  if (installed) return;
  prepare_crash_stack();
  // Warm libgcc's unwinder before a fatal signal: backtrace's first call may
  // otherwise load it and allocate in the signal handler.
  void* warm[1];
  backtrace(warm, 1);
  struct sigaction action{};
  action.sa_sigaction = host_signal;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&action.sa_mask);
  for (unsigned i = 0; i < std::size(signals); ++i) sigaction(signals[i], nullptr, &previous[i]);
  for (int signal : signals) sigaction(signal, &action, nullptr);
  installed = true;
  // POSIX has a single action per signal. Install the guarded handler last so
  // resolved guest accesses return before reaching this host-fatal fallback.
  std::uint32_t value;
  read_memory(0, &value, sizeof(value));
}
}
