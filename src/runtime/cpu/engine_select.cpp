#include "cpu/native_a64.h"

#include <cstdlib>
#include <cstring>
#include <string_view>

#include "common/log.h"
#include "core/process.h"
#include "cpu/guest_entry.h"
#include "cpu/native_entry.h"
#include "platform/memory.h"

#if VENTURE_NATIVE_A64
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace venture {
namespace {
#if VENTURE_NATIVE_A64
// Whether an unprivileged process may install a seccomp filter that traps by
// instruction pointer. Tried in a child, so this process keeps no filter and
// no no_new_privs bit when the answer is no.
bool seccomp_trap_available() {
  const pid_t child = fork();
  if (child < 0) return false;
  if (child == 0) {
    sock_filter program[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, instruction_pointer) + 4),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0xffffffffu, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    sock_fprog filter{static_cast<unsigned short>(std::size(program)), program};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) _exit(1);
    _exit(syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &filter) == 0 ? 0 : 1);
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {}
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif
}  // namespace

HostCpuFacts probe_host_cpu_facts() {
  HostCpuFacts facts{.arch = build_arch(), .page_size = kGuestPageSize};
#if VENTURE_NATIVE_A64
  facts.page_size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  facts.guest_syscall_trap = seccomp_trap_available();
#endif
  return facts;
}

const EngineChoice& process_engine() {
  static const EngineChoice choice = [] {
    const auto isa = process_config().isa();
    const auto host = probe_host_cpu_facts();
    auto chosen = choose_engine(isa, host);
    if (const char* forced = std::getenv("VENTURE_CPU_ENGINE");
        forced && std::string_view(forced) == "dynarmic" && chosen.engine == CpuEngine::native_a64)
      chosen = {CpuEngine::dynarmic_arm64, "VENTURE_CPU_ENGINE=dynarmic: translated"};
#if !VENTURE_NATIVE_A64
    // This build has no direct execution engine (x64, Android): translate.
    if (chosen.engine == CpuEngine::native_a64)
      chosen = {CpuEngine::dynarmic_arm64, "no native-a64 engine in this build: translated"};
#else
    if (chosen.engine == CpuEngine::native_a64) {
      platform::set_guest_code_executable(true);
      set_guest_entry(native_entry());
    }
#endif
    if (host.arch != HostArch::x64)
      log::info("cpu", "engine {}: {}", engine_name(chosen.engine), chosen.reason);
    return chosen;
  }();
  return choice;
}

bool guest_code_runs_natively() { return process_engine().engine == CpuEngine::native_a64; }

}  // namespace venture
