#pragma once
#include <cstdint>
#include <mutex>
#include <string>

namespace venture::platform {
struct ProcessMemory { std::uint64_t private_kib = 0, shared_pss_kib = 0; };
// Current process resident pages: private pages plus proportional shared pages.
bool process_memory(ProcessMemory& out);
struct ProcessResidentMemory { std::uint64_t resident_kib = 0, peak_resident_kib = 0; };
bool process_resident_memory(ProcessResidentMemory& out);
bool process_debugger_attached();
// A hardware user shadow stack (x86 CET) is enforced for the calling thread:
// every RET must match a CALL, so code may not discard return addresses.
bool user_shadow_stack_active();
// Diagnostics only: the OS thread currently owning m, as "tid (name)", "none",
// or "unknown" where the platform's mutex does not expose its owner.
std::string mutex_owner_thread(std::recursive_mutex& m);
}
