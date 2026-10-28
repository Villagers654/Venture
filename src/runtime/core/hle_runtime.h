#pragma once
#include <span>
#include <vector>
#include "core/hle.h"
#include "cpu/stubs.h"
namespace venture::hle {
// Checked HLE memory operations unwind to dispatch, which delivers a guest
// SIGSEGV at the SVC instruction. A guest handler can repair state or longjmp.
struct MemoryFault { GuestAddr address; bool write = false, bus = false; };
bool dispatch_in_jit(GuestThread& thread, FunctionId id);
bool dispatch_registered_in_jit(GuestThread& thread, const FunctionInfo& info, Registers registers = {});
// Translation-only lookup. Exact registered instruction bytes are required;
// modified thunks and entry into their bodies use ordinary SVC dispatch.
const FunctionInfo* resolve_thunk(GuestAddr entry, std::uint32_t svc);
void deliver_memory_fault(GuestThread& thread, FunctionId id, const MemoryFault& fault);
GuestAddr missing_symbol(std::string_view library, std::string_view name);
GuestAddr call_return_trampoline(std::uint32_t depth);
// Runtime stubs that do not dispatch to a host handler (e.g. TLS and sigreturn).
GuestAddr code_stub(std::span<const std::uint32_t> instructions, std::string_view name);
// Stable snapshots for diagnostics; no additional work on dispatch.
std::vector<FunctionInfo> registered_functions();
std::uint64_t registered_functions_generation() noexcept;
GuestAddr code_stub(GuestStub kind, std::string_view name, std::uint32_t argument = 0);
// Opt-in diagnostics. VENTURE_HLE_CENSUS is read once at process startup; disabled
// dispatch only tests this immutable flag (no census allocation or atomics).
extern const bool census_enabled;
// With that census, VENTURE_HLE_TIMING=1 and an explicit
// VENTURE_HLE_TIMING_WINDOW=begin:end (seconds since census initialization)
// retain bounded per-second/per-host-thread XR and Vulkan synchronization API
// counts, wall time and calling-thread CPU counters, without a sampling thread.
// Only diagnostic registration substitutes handlers; ordinary dispatch is unchanged.
void write_census() noexcept;  // snapshot at a run limit; also written at normal process exit
void census_guest_threads(std::size_t live);
void census_file_mmap(std::string_view path, int fd, std::uint64_t size, int prot, int flags,
                      std::int64_t offset, std::int64_t result);
}
