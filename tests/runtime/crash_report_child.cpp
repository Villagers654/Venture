#include "common/log.h"
#include "platform/crash_report.h"
#include "platform/memory.h"
#include "platform/runtime_child.h"
#include "core/hle_runtime.h"
#include "core/process.h"
#include "core/process_runtime.h"
#include "core/address_space.h"
#include "core/signals.h"
#include "core/thread.h"
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <thread>
#include <fstream>
#include <string>

__attribute__((noinline)) void host_fault(std::uintptr_t address) {
  *reinterpret_cast<volatile unsigned*>(address) = 42;
}
__attribute__((noinline)) void null_call(void (*function)()) {
  void (*volatile target)() = function;  // the compiler cannot drop the call
  target();
}
int translated_fault(int) {
  venture::signals::log_guest_threads("frame-chain crash fixture");
  host_fault(0);
  return 0;
}
int main(int argc, char** argv) {
  if (argc != 4) return 2;
  const std::string_view mode(argv[2]);
  const bool jit = mode == "jit64" || mode == "jit32";
  const bool arm = mode == "jit32";
  venture::select_process_isa(arm ? venture::GuestIsa::a32 : venture::GuestIsa::a64);
  if (jit) venture::AddressSpace::get().initialize(arm ? venture::GuestIsa::a32 : venture::GuestIsa::a64);
  venture::log::set_file(argv[1]);
  venture::platform::install_crash_report(argv[1], "org.venture.crashtest");
  venture::log::set_flush_interval(std::chrono::hours(1));
  venture::log::warn("test", "start crash test");
  venture::log::info("test", "earlier buffered info survives");
  // Prove the regression actually exercises pending userspace log bytes.
  std::ifstream before(argv[1], std::ios::binary);
  const std::string text{std::istreambuf_iterator<char>(before), {}};
  if (text.find("earlier buffered info") != text.npos) return 3;
  before.close();
#ifdef _WIN32
  // A __fastfail no in-process handler sees, and the supervisor's lookup of
  // Windows Error Reporting's record of it (main.cpp report_foreign_worker_exit).
  if (mode == "fastfail") {
    std::printf("%lu\n", GetCurrentProcessId());
    std::fflush(stdout);
    __fastfail(7);  // FAST_FAIL_FATAL_APP_EXIT
  }
  if (mode == "wer-lookup") {
    std::printf("%s\n", venture::platform::system_crash_record(std::strtoul(argv[3], nullptr, 0),
                                                              std::chrono::seconds(10)).c_str());
    return 0;
  }
#endif
  if (mode == "guest") {
    std::uint32_t out = 0;
    if (venture::platform::read_memory(0, &out, sizeof(out))) return 4;
    venture::log::flush();
    return 0;
  }
  if (std::string_view(argv[2]) == "native-source") {
    std::uint32_t destination = 0;
    std::uint64_t result;
    venture::platform::NativeMemoryFault fault;
    venture::platform::native_memory(venture::platform::NativeMemoryOp::copy,
        venture::h2g(&destination), std::strtoull(argv[3], nullptr, 0), 4, 0, result, fault, false);
    return 7;
  }
  if (mode == "native-host-compare") {
    std::uint32_t a = 42, b = 42;
    venture::platform::NativeMemoryFault fault;
    auto& result = *reinterpret_cast<int*>(std::strtoull(argv[3], nullptr, 0));
    venture::platform::native_compare(venture::h2g(&a), venture::h2g(&b), 4, result, fault);
    return 6;
  }
  if (std::string_view(argv[2]) == "native-host") {
    std::uint32_t source = 42, destination = 0;
    venture::platform::NativeMemoryFault fault;
    // The CRT's declared buffers are valid. A broken host result reference
    // remains a host bug after leaving the leaf recovery scope.
    auto& result = *reinterpret_cast<std::uint64_t*>(std::strtoull(argv[3], nullptr, 0));
    venture::platform::native_memory(venture::platform::NativeMemoryOp::copy,
        venture::h2g(&destination), venture::h2g(&source), 4, 0, result, fault);
    return 6;
  }
  if (jit) {
    venture::GuestThread thread(7191); thread.attach_to_current_host_thread();
    // The production HLE callback and crash reporter see real translated frames.
    // Guest unwinding is separate from the host's diagnostic RBP chain.
    const auto thunk = venture::hle::make_thunk("libc.so", "abs", venture::hle::wrap<&translated_fault>(),
                                             nullptr, venture::hle::Execution::InJit);
    thread.call(thunk, {0});
    return 6;
  }
  if (mode == "null-call") null_call(nullptr);
  // Deliberate stops, which the C runtime ends through __fastfail (0xC0000409)
  // on Windows without any exception handler seeing them.
  if (mode == "abort") std::abort();
  if (mode == "fatal-reason") {
    venture::log::set_fatal_reason("JIT assertion failed: fixture (Unreachable code!)");
    std::terminate();
  }
  if (mode == "thread-exception") {
    std::thread([] { throw std::runtime_error("fixture worker failure"); }).join();
  }
#ifdef _WIN32
  if (mode == "invalid-parameter") {
    volatile int descriptor = -7;
    _close(descriptor);
  }
#endif
#ifdef _WIN32
  // A fault while the process heap lock is held (heap corruption caught in
  // RtlFreeHeap): the dump cannot finish, and the process must still end.
  if (mode == "heap-locked") HeapLock(GetProcessHeap());
#endif
  host_fault(std::strtoull(argv[3], nullptr, 0));
  return 5;
}
