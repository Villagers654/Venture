// The process's sigaction and signal family on a Linux AArch64 host with the
// native-a64 engine (cpu/native_a64.h). This object is linked straight into
// each executable, outside any archive, so --exclude-libs keeps its symbols
// global and --export-dynamic-symbol exports them: every library, the JVM
// among them, binds to these rather than libc's. Raw rt_sigaction system calls
// and libc's internal callers (siginterrupt) are not seen.
#include "cpu/native_a64.h"

#if VENTURE_NATIVE_A64
#define VENTURE_EXPORT extern "C" __attribute__((visibility("default")))

VENTURE_EXPORT int sigaction(int signal, const struct sigaction* action, struct sigaction* old) noexcept {
  return venture::native_a64_sigaction(signal, action, old);
}
VENTURE_EXPORT sighandler_t signal(int signal, sighandler_t handler) noexcept {
  return venture::native_a64_signal(signal, handler, venture::NativeSignalSemantics::bsd);  // glibc's signal()
}
VENTURE_EXPORT sighandler_t bsd_signal(int signal, sighandler_t handler) noexcept {
  return venture::native_a64_signal(signal, handler, venture::NativeSignalSemantics::bsd);
}
VENTURE_EXPORT sighandler_t sysv_signal(int signal, sighandler_t handler) noexcept {
  return venture::native_a64_signal(signal, handler, venture::NativeSignalSemantics::sysv);
}
VENTURE_EXPORT sighandler_t __sysv_signal(int signal, sighandler_t handler) noexcept {
  return venture::native_a64_signal(signal, handler, venture::NativeSignalSemantics::sysv);
}
#endif
