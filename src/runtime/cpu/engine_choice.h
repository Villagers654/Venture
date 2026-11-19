#pragma once
// Which engine executes guest code, decided from what the host CPU and kernel
// offer (docs/design/native-headsets.md, Linux arm64 host). One build per host
// architecture carries every engine it can use; nothing here names a device.
#include <cstddef>
#include <string>

#include "core/guest.h"

namespace venture {
enum class HostArch { x64, arm64, other };

enum class CpuEngine {
  dynarmic_x64,    // Dynarmic, x64 backend (A64 or A32 guests): Windows and Linux x64
  native_a64,      // A64 guest code runs directly on an AArch64 host, HLE thunks branch to host entry stubs
  dynarmic_arm64,  // Dynarmic, arm64 backend: A32 guests on AArch64, or A64 where direct execution is unavailable
  none,
};

// Probed once per process on the host (sysconf, getauxval, a trial seccomp
// filter in a child); the x64 build passes its own facts and always picks Dynarmic.
struct HostCpuFacts {
  HostArch arch = HostArch::other;
  std::size_t page_size = 0;
  // Raw guest SVCs can be routed to Venture: a seccomp filter that traps
  // system calls made from the guest code arena (SIGSYS), or syscall user dispatch.
  bool guest_syscall_trap = false;
};

struct EngineChoice {
  CpuEngine engine = CpuEngine::none;
  std::string reason;  // for the log and the self-test report
};

EngineChoice choose_engine(GuestIsa isa, const HostCpuFacts& host);
constexpr HostArch build_arch() {
#if defined(__x86_64__) || defined(_M_X64)
  return HostArch::x64;
#elif defined(__aarch64__) || defined(_M_ARM64)
  return HostArch::arm64;
#else
  return HostArch::other;
#endif
}
const char* engine_name(CpuEngine engine);
}  // namespace venture
