#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace venture::platform {
struct ProcessMemory { std::uint64_t private_kib = 0, shared_pss_kib = 0; };
// Current process resident pages: private pages plus proportional shared pages.
bool process_memory(ProcessMemory& out);
struct ProcessResidentMemory { std::uint64_t resident_kib = 0, peak_resident_kib = 0; };
bool process_resident_memory(ProcessResidentMemory& out);
// Memory this process can still allocate before allocations fail, and the limit it
// counts against. Windows: commit (RAM plus page file, or a job's limit if lower);
// GPU drivers that charge video memory to the process draw on it too. Linux:
// CommitLimit - Committed_AS under strict overcommit, else MemAvailable (and a
// cgroup memory.max if lower). False when the host cannot say.
struct MemoryHeadroom { std::uint64_t available = 0, limit = 0; };
bool memory_headroom(MemoryHeadroom& out);
// System commit charge and limit and this process's commit (Windows private
// bytes). Linux: Committed_AS and CommitLimit, enforced only in strict
// overcommit mode, and the process's anonymous resident plus swapped memory.
struct CommitCharge {
  std::uint64_t system_used = 0, system_limit = 0, process = 0;
  bool enforced = true;
};
bool commit_charge(CommitCharge& out);
// Cumulative time each GPU engine has spent running this process's work, as the OS
// accounts it: Windows "GPU Engine" Running Time (Task Manager's per-process GPU
// usage); Linux DRM fdinfo drm-engine-* (amdgpu, i915, xe, panfrost, ...). Keyed by
// adapter and engine. False when the host cannot say (e.g. NVIDIA's Linux driver).
struct GpuEngineTime { std::string engine; std::uint64_t ns = 0; };
bool gpu_engine_times(std::vector<GpuEngineTime>& out);
bool process_debugger_attached();
// A hardware user shadow stack (x86 CET) is enforced for the calling thread:
// every RET must match a CALL, so code may not discard return addresses.
bool user_shadow_stack_active();
// Diagnostics only: the OS thread currently owning m, as "tid (name)", "none",
// or "unknown" where the platform's mutex does not expose its owner.
std::string mutex_owner_thread(std::recursive_mutex& m);
}
