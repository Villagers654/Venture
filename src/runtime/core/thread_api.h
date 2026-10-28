// Additive interface needed by libc pthreads. Implemented by the core.
#pragma once
#include "core/thread.h"
#include <string>
#include <vector>
namespace venture::threads {
struct Options {
  GuestAddr stack = 0;  // zero: core allocates and owns the stack
  std::uint64_t stack_size = 1024 * 1024;
  std::uint64_t guard_size = kGuestPageSize;
  bool detached = false;
  // Run on the exiting thread, including explicit pthread_exit, before TLS
  // teardown. libc installs its four-pass pthread-key destructor runner.
  void (*on_exit)(GuestThread&) = nullptr;
};
// Errors are negative Linux errno; handles are GuestThread::pthread_self.
int create(const Options&, GuestAddr entry, GuestAddr arg, GuestAddr* handle);
int join(GuestThread& caller, GuestAddr handle, GuestAddr* result);
int detach(GuestAddr handle);
int describe(GuestAddr handle, Options* options, std::int32_t* tid, std::string* name);
int set_name(GuestAddr handle, const char* name);
// Snapshot of live guest tids and names for /proc; do not hold core locks
// while the caller formats the returned data or invokes guest callbacks.
std::vector<std::pair<std::int32_t, std::string>> list();
std::size_t count();
// Unwinds guest calls to the core run loop (must not exit the host process).
[[noreturn]] void exit(GuestThread&, GuestAddr result);
[[noreturn]] void exit_process(GuestThread&, int status);
}  // namespace venture::threads
