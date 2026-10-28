#pragma once
#include "core/guest.h"
#include <array>
#include <span>

namespace venture {
inline constexpr GuestAddr kKuserAddress = 0xffff0000;
std::span<const std::uint32_t, 1024> kuser_page();
// Called by the A32 arena owner after reserving the low address space. Maps a
// guest R-X (host read-only) page at the fixed kernel ABI address; negative errno.
int install_kuser_page();
} // namespace venture
