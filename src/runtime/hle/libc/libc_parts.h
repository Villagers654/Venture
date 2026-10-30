// The libc HLE is split into parts implemented in separate files; libc.cpp
// creates the HleLibrary objects (libc.so, libm.so, libdl.so, ...) and calls
// every register_* function below.
#pragma once

#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

#include "cpu/cpu.h"

namespace venture {
class HleLibrary;
}

namespace venture::hle::libc {
// Called only after all guest calls have unwound; gzip owns native buffers.
bool close_gzip_for_os_quit();

// --- part A: computation and formatting ------------------------------------
void register_strings(HleLibrary& libc);    // string.h, strings.h, wchar.h, memcpy family
void register_memory(HleLibrary& libc);     // malloc family, posix_memalign, malloc_usable_size
void register_stdlib(HleLibrary& libc);     // strto*, ato*, abs/div, qsort, bsearch, rand*, env, getauxval
void register_ctype_locale(HleLibrary& libc);  // ctype tables (_ctype_ etc.), locale (C/C.UTF-8 only)
void register_formatting(HleLibrary& libc); // snprintf/vsnprintf/sprintf/asprintf, sscanf/vsscanf
void register_time_format(HleLibrary& libc);  // localtime_r/gmtime_r/mktime/strftime/timegm (UTC)
void register_misc(HleLibrary& libc);       // __system_property_*, __stack_chk_*, __cxa_atexit/finalize, abort hooks
// Immutable launch properties shared with Android's Java read API. Unlike the
// legacy 92-byte libc buffer, this preserves long read-only property values.
std::string_view system_property(std::string_view name);
// Java Os shares bionic's guest environment, never the host process environment.
// Returns zero or a Linux errno value; strings are copied by setenv.
int set_environment(std::string_view name, std::string_view value, bool overwrite);
const char* error_string(int error);
void register_libm(HleLibrary& libm);       // libm.so
void register_liblog(HleLibrary& liblog);   // liblog.so (__android_log_*)
// The guest's last ERROR/FATAL Android log line ("tag: message"), or empty.
std::string last_guest_error();
void register_libz(HleLibrary& libz);       // libz.so (zlib)
void register_libstdcpp(HleLibrary& lib);   // system libstdc++.so (operator new/delete, __cxa_guard_*)

// printf-family formatting engine shared with the stdio part (bionic semantics).
struct PrintfConversion {
  unsigned width = 4; // promoted argument bytes; * width/precision are signed int
  bool is_signed = false;
  bool pointer = false; // %s, %p, %n (the latter's pointee width is separate)
  bool operator==(const PrintfConversion&) const = default;
};
class PrintfArgs {
 public:
  virtual ~PrintfArgs() = default;
  virtual std::uint64_t next_int() = 0;
  virtual double next_double() = 0;
  // Binary128 %L arguments; existing adapters remain source compatible.
  // Override when adapting a guest cursor (Part A's PrintArgs does so).
  virtual Vec128 next_quad();
  virtual std::uint64_t next_integer(PrintfConversion) { return next_int(); }
};
// Formatting failures set guest errno and throw this exception. Stdio callers
// catch it and return -1; liblog and libz map it to their documented errors.
class PrintfError final : public std::exception {
 public:
  explicit PrintfError(int value) : error(value) {}
  const char* what() const noexcept override { return "guest printf formatting failed"; }
  int error;
};
std::string format_printf(const char* format, PrintfArgs& args);
std::string format_printf32(const char* format, PrintfArgs& args);

// --- part B: operating-system services --------------------------------------
void register_threads(HleLibrary& libc);    // pthread_*, sched_*, sem_*, gettid, futex users
void register_signals(HleLibrary& libc);    // sigaction, signal, sigprocmask, raise, kill, sigaltstack, setjmp family
void register_files(HleLibrary& libc);      // fds, open/read/write/..., stat, dirs, stdio FILE*, VFS
void register_net(HleLibrary& libc);        // sockets, getaddrinfo, poll, select, epoll, eventfd, pipe, socketpair
void register_memory_map(HleLibrary& libc); // mmap, munmap, mprotect, madvise, msync, mlock
// Linux brk syscall result: current break on failure, without changing errno.
GuestAddr linux_program_break(GuestAddr requested);
void register_process(HleLibrary& libc);    // getpid, uname, prctl, exit/_exit/abort, sysconf, getrlimit, ...
void register_clock(HleLibrary& libc);      // clock_gettime, nanosleep, usleep, gettimeofday, time, clock
void register_syscall(HleLibrary& libc);    // syscall()
void register_libdl(HleLibrary& libdl);     // dlopen/dlsym/dlclose/dlerror/dladdr/dl_iterate_phdr
void register_loader(HleLibrary& loader);  // ld-android.so: the linker's __loader_* entry points
void shape_dlopen(HleLibrary& libdl, const HleLibrary& loader);  // A64 dlopen calls __loader_dlopen like bionic

}  // namespace venture::hle::libc
