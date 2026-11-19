#pragma once
// The process's CPU engine and the native-a64 engine (docs/design/native-headsets.md 4.1).
//
// process_engine() decides once, from what the host offers (cpu/engine_choice.h):
// on a Linux AArch64 host with 4 KiB pages whose kernel traps system calls by
// instruction pointer (seccomp), A64 guest code runs directly on the CPU
// (CpuEngine::native_a64); otherwise Dynarmic translates it. x64 builds always
// translate. VENTURE_CPU_ENGINE=dynarmic forces translation on any host.
#include <cstdint>

#include "cpu/cpu.h"
#include "cpu/engine_choice.h"
#if defined(__aarch64__) && defined(__linux__) && !defined(__ANDROID__)
#include <signal.h>
#endif

namespace venture {

// What this host offers, probed once (the page size, a trial seccomp filter in a child).
HostCpuFacts probe_host_cpu_facts();

// The engine for this process's guest ISA. The first call (AddressSpace::initialize,
// before any guest mapping or stub) also prepares it: for native-a64 guest
// executable pages become host executable and stubs use the native entry.
const EngineChoice& process_engine();

// Guest PROT_EXEC pages are host executable: native-a64 runs them in place.
bool guest_code_runs_natively();

#if defined(__aarch64__) && defined(__linux__) && !defined(__ANDROID__)
#define VENTURE_NATIVE_A64 1
// The native-a64 backend: installs the seccomp filter that traps system calls
// made from the A64 guest arena, and the signal handlers that leave guest code.
CpuBackend& native_a64_backend();
// What the interposed sigaction and signal family do (cpu/native_a64_interpose.cpp,
// linked straight into executables so libraries bind to it): libc's behaviour
// until the backend starts; then host code sees its own actions while the
// kernel runs a guard that puts the host thread pointer back before calling a
// handler that interrupted guest code.
enum class NativeSignalSemantics { bsd, sysv };
int native_a64_sigaction(int signal, const struct ::sigaction* action, struct ::sigaction* old);
using NativeSignalHandler = void (*)(int);
NativeSignalHandler native_a64_signal(int signal, NativeSignalHandler handler, NativeSignalSemantics semantics);
#else
#define VENTURE_NATIVE_A64 0
#endif

}  // namespace venture
