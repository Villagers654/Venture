#pragma once
#include "cpu/cpu.h"
#include <cstddef>

namespace venture {
// Integration with the low arena: lock-free region snapshot query, with Linux
// PROT bits. Reject wrapped ranges and foreign pages. No OS queries on this path.
using A32MemoryQuery = bool (*)(GuestAddr, std::uint64_t, std::uint32_t) noexcept;
struct A32BackendConfig {
  std::size_t monitor_slots = kGuestCpuSlots;
  A32MemoryQuery memory_query = nullptr;
  // Strict reservation invalidation, including value-restoring writes.
  bool use_global_monitor = false;
  // Callback-only execution also permits deterministic mapping-race fixtures.
  bool enable_fastmem = true;
};
std::unique_ptr<CpuBackend> make_a32_backend(A32BackendConfig config);
// Install before the first A32 Cpu is created; the arena owns query lifetime.
void set_a32_memory_query(A32MemoryQuery query);
CpuBackend& a32_cpu_backend();
// VENTURE_JIT_CODE_STATS=<file>: Dynarmic writes every emitted block's code
// there (dynarmic/interface/code_stats.h, tools/jit_code_stats.py), and this
// writes the A32 JIT's runtime-callback counts to <file>.callbacks.json.
void write_jit_code_stats() noexcept;
} // namespace venture
