#pragma once
#include "core/address_space.h"

namespace venture::low32 {
// Record mimalloc's logical RW span without committing it. Accessibility also
// checks the allocator's live-page bitmap before a CPU callback dereferences it.
void publish_heap(GuestAddr start);
struct Layout {
  std::uint64_t total = 0, largest = 0;
  std::uint32_t ranges = 0;
};
// Call once, after ISA selection and before HLE/library initialization.
// Repeated calls are harmless; failure is terminal (no partial retry).
Layout initialize();
bool active();
Layout layout();
std::int64_t map(GuestAddr, std::uint64_t, std::uint32_t, std::uint32_t, const AddressSpace::FileReader*,
                 std::uint64_t, std::string_view, std::uint64_t alignment = kGuestPageSize);
std::int64_t map_file(int fd, std::uint64_t size, std::uint32_t prot, std::uint64_t offset,
                      std::string_view name);
std::int64_t unmap(GuestAddr, std::uint64_t);
std::int64_t protect(GuestAddr, std::uint64_t, std::uint32_t);
std::int64_t advise(GuestAddr, std::uint64_t, int);
GuestAddr allocate(std::uint64_t, std::uint32_t, std::string_view, std::uint64_t);
void free(GuestAddr);
bool find(GuestAddr, MemoryRegion*);
bool accessible(GuestAddr, std::uint64_t, bool);
bool grow_stack(GuestAddr, std::uint32_t access, std::uint64_t stack_limit);
bool query(GuestAddr, std::uint64_t, std::uint32_t prot) noexcept;
bool executable(GuestAddr);
bool covered(GuestAddr, std::uint64_t);
void for_each(const std::function<void(const MemoryRegion&)>&);
std::string proc_maps();
std::uint64_t mapped_size();
void label(GuestAddr, std::uint64_t, std::string_view, std::uint64_t);
}  // namespace venture::low32
