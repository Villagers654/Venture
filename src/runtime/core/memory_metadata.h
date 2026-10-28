#pragma once
#include "core/guest.h"
#include <string_view>
#include <string>
namespace venture {
struct MemoryRegion;
void append_proc_map(std::string& text, const MemoryRegion& region);
// Labels mapped ELF pages for /proc/self/maps without changing their protections.
void label_memory(GuestAddr address, std::uint64_t size, std::string_view name, std::uint64_t file_offset);
}
