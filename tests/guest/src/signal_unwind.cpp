// Original CPU-only fixture: real NDK C++/libunwind, no APK or graphics calls.
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <unwind.h>
#include <dlfcn.h>
#include <ucontext.h>
#include <link.h>
#include <sys/mman.h>

struct Result { std::uint32_t caught, destroyed, joined, mutex_owned, frames, interrupted_frame, handler, layout; };
static Result* result;
static unsigned mode;
static std::atomic<bool> ready{false};
static pthread_mutex_t mutex;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static bool unwind_image_valid;
static std::uintptr_t alternate_base;
static int check_image(dl_phdr_info* info, size_t, void*) {
  if (std::strcmp(info->dlpi_name, "[venture hle]") == 0) {
    auto* header = reinterpret_cast<const ElfW(Ehdr)*>(info->dlpi_addr);
    unwind_image_valid = std::memcmp(header->e_ident, ELFMAG, SELFMAG) == 0 &&
                         header->e_phnum == info->dlpi_phnum && info->dlpi_phnum == 3;
  }
  return 0;
}
struct Abort {};
struct Guard { ~Guard() { ++result->destroyed; } };
static _Unwind_Reason_Code trace(_Unwind_Context* context, void*) {
  ++result->frames;
  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(_Unwind_GetIP(context)), &info) && info.dli_sname &&
      (std::strcmp(info.dli_sname, "signal_unwind_spin") == 0 ||
       std::strcmp(info.dli_sname, "signal_unwind_worker") == 0)) ++result->interrupted_frame;
  return _URC_NO_REASON;
}
static void handler(int, siginfo_t*, void* raw) {
  ++result->handler;
  auto* uc = static_cast<ucontext_t*>(raw);
#if defined(__aarch64__)
  static_assert(offsetof(ucontext_t, uc_mcontext) == 176);
  result->layout = uc->uc_mcontext.sp != 0 && uc->uc_mcontext.pc != 0;
  std::uint32_t header[2];
  std::memcpy(header, uc->uc_mcontext.__reserved, sizeof(header));
  result->layout &= header[0] == 0x46508001 && header[1] == 528;
#else
  static_assert(offsetof(ucontext_t, uc_mcontext) == 20);
  static_assert(offsetof(ucontext_t, uc_regspace) == 232);
  result->layout = uc->uc_mcontext.arm_sp != 0 && uc->uc_mcontext.arm_pc != 0;
  result->layout &= uc->uc_regspace[0] == 0x56465001 && uc->uc_regspace[1] == 280;
#endif
  result->layout &= unwind_image_valid;
  if (mode == 10) result->layout &= reinterpret_cast<std::uintptr_t>(&uc) >= alternate_base &&
      reinterpret_cast<std::uintptr_t>(&uc) < alternate_base + 65536;
  if (mode == 1 || mode == 4) { _Unwind_Backtrace(trace, nullptr); asm volatile("" ::: "memory"); }
  else throw Abort{};
}
static void legacy_handler(int) {
  ++result->handler;
  result->layout = unwind_image_valid;
  if (mode == 11) {
    int marker;
    result->layout &= reinterpret_cast<std::uintptr_t>(&marker) >= alternate_base &&
        reinterpret_cast<std::uintptr_t>(&marker) < alternate_base + 65536;
  }
  if (mode == 7 || mode == 11) throw Abort{};
  _Unwind_Backtrace(trace, nullptr);
  asm volatile("" ::: "memory");
}
extern "C" __attribute__((noinline, visibility("default"))) void signal_unwind_spin() {
  ready.store(true);
  while (!result->handler) asm volatile("" ::: "memory");
}
extern "C" __attribute__((noinline, visibility("default"))) void* signal_unwind_worker(void*) {
  if (mode == 10 || mode == 11) {
    void* area = mmap(reinterpret_cast<void*>(0xd0000000), 65536, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (area == MAP_FAILED || reinterpret_cast<std::uintptr_t>(area) <= reinterpret_cast<std::uintptr_t>(&area))
      return reinterpret_cast<void*>(2);
    stack_t stack{area, 0, 65536};
    if (sigaltstack(&stack, nullptr)) return reinterpret_cast<void*>(3);
    alternate_base = reinterpret_cast<std::uintptr_t>(area);
  }
  if (mode == 1 || mode == 5) { signal_unwind_spin(); return nullptr; }
  if (pthread_mutex_lock(&mutex)) return reinterpret_cast<void*>(1);
  try {
    Guard guard;
    ready.store(true);
    if (mode == 0) throw Abort{};
    // Indirect call retains a C++ landing pad: the optimizer otherwise proves
    // this local spin function cannot throw under C++'s synchronous model.
    if (mode == 6) { void (*volatile spin)() = signal_unwind_spin; spin(); }
    else if (mode == 4 || mode == 8) {
      while (!result->handler) pthread_cond_wait(&condition, &mutex);
    } else {
      // mode 2: interruptible HLE sleep; mode 3: HLE condition wait.
      for (;;) { if (mode == 2) usleep(100000); else pthread_cond_wait(&condition, &mutex); }
    }
  } catch (const Abort&) {
    ++result->caught;
  }
  result->mutex_owned = pthread_mutex_unlock(&mutex) == 0;
  return nullptr;
}
extern "C" __attribute__((visibility("default"))) int signal_unwind_fixture(unsigned selected, Result* output) {
  mode = selected; result = output;
  std::set_terminate([] { std::fprintf(stderr, "FIXTURE uncaught guest C++ exception\n"); _exit(88); });
  struct sigaction action{};
  action.sa_sigaction = handler; action.sa_flags = SA_SIGINFO;
  if (mode == 5 || mode == 7 || mode == 8) { action.sa_handler = legacy_handler; action.sa_flags = 0; }
  sigemptyset(&action.sa_mask);
  if (mode == 10 || mode == 11) action.sa_flags |= SA_ONSTACK;
  if (mode == 11) { action.sa_handler = legacy_handler; action.sa_flags = SA_ONSTACK; }
  if (sigaction(SIGUSR1, &action, nullptr)) return 2;
  dl_iterate_phdr(check_image, nullptr);
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr); pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
  pthread_mutex_init(&mutex, &attr); pthread_mutexattr_destroy(&attr);
  pthread_t thread;
  if (pthread_create(&thread, nullptr, signal_unwind_worker, nullptr)) return 3;
  while (!ready.load()) usleep(1000);
  if (mode) {
    usleep(10000);
    if (mode == 9) pthread_mutex_lock(&mutex);
    if (pthread_kill(thread, SIGUSR1)) return 4;
    if (mode == 9) { usleep(20000); pthread_mutex_unlock(&mutex); }
  }
  void* returned{};
  result->joined = pthread_join(thread, &returned) == 0 && returned == nullptr;
  pthread_mutex_destroy(&mutex); pthread_cond_destroy(&condition);
  return 0;
}
