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
} // namespace venture
