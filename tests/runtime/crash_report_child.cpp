#include "common/log.h"
#include "platform/crash_report.h"
#include "platform/memory.h"
#include "core/hle_runtime.h"
#include "core/process.h"
#include "core/process_runtime.h"
#include "core/address_space.h"
#include "core/signals.h"
#include "core/thread.h"
#include <cstdlib>
#include <fstream>
#include <string>

__attribute__((noinline)) void host_fault(std::uintptr_t address) {
  *reinterpret_cast<volatile unsigned*>(address) = 42;
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
  host_fault(std::strtoull(argv[3], nullptr, 0));
  return 5;
}
