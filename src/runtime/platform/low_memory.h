#pragma once
#include "core/guest.h"
#include <vector>

namespace venture::platform {
struct LowRange {
  GuestAddr start;
  std::uint64_t size;
};
// Process entry, before C++ constructors on Windows. Does not select a guest ISA.
// Failure is retained and reported if the process later selects A32.
void pre_reserve_low_memory() noexcept;
// Reserve the wrap guard without changing any existing host mapping. If false,
// A32 must use checked memory callbacks instead of identity fastmem.
bool reserve_low_memory_guard() noexcept;
// Before guest memory initialization: A64 selection returns only our early
// reservations to the host. Foreign mappings, including the guard, are retained.
void release_pre_reserved_low_memory();
// Startup only, before host libraries/worker threads. Rejects writable host pages
// below 4 GiB and reserves every hole. Reuses the early reservation if present.
// Throws on failure; reservations are process lifetime and never released.
std::vector<LowRange> reserve_low_memory();
// Serialized by the A32 address-space lock. The caller owns these placeholders.
bool split_placeholder(GuestAddr base, std::uint64_t first_size);
bool coalesce_placeholders(GuestAddr base, std::uint64_t size);
bool replace_placeholder(GuestAddr base, std::uint64_t size);
bool restore_placeholder(GuestAddr base, std::uint64_t size, bool file_view);
// Read-only file view; no writable shared mappings or high-address fallback.
int replace_file_placeholder(int fd, GuestAddr base, std::uint64_t size, std::uint64_t offset,
                             std::uint32_t prot);
}  // namespace venture::platform
