#include "cpu/engine_choice.h"

namespace venture {
EngineChoice choose_engine(GuestIsa isa, const HostCpuFacts& host) {
  switch (host.arch) {
    case HostArch::x64:
      return {CpuEngine::dynarmic_x64, "x64 host: ARM guest code is translated"};
    case HostArch::arm64:
      // AArch32 code cannot run inside an AArch64 process (and many current
      // cores have no AArch32 at all), so A32 is always translated.
      if (isa == GuestIsa::a32) return {CpuEngine::dynarmic_arm64, "A32 guest on an AArch64 host is translated"};
      // Direct execution shares the host's page tables: the guest's 4 KiB
      // mprotect/mmap granularity must be the host's, and raw guest system
      // calls must reach Venture instead of the kernel.
      if (host.page_size != kGuestPageSize)
        return {CpuEngine::dynarmic_arm64, "host pages are " + std::to_string(host.page_size) +
                                               " bytes, not the guest's 4096: translated with a 4 KiB page table"};
      if (!host.guest_syscall_trap)
        return {CpuEngine::dynarmic_arm64, "the kernel cannot trap guest system calls: translated"};
      return {CpuEngine::native_a64, "A64 guest code runs directly on the host CPU"};
    case HostArch::other:
      break;
  }
  return {CpuEngine::none, "no engine for this host CPU"};
}

const char* engine_name(CpuEngine engine) {
  switch (engine) {
    case CpuEngine::dynarmic_x64: return "dynarmic-x64";
    case CpuEngine::native_a64: return "native-a64";
    case CpuEngine::dynarmic_arm64: return "dynarmic-arm64";
    case CpuEngine::none: break;
  }
  return "none";
}
}  // namespace venture
