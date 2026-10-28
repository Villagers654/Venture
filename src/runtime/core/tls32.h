#pragma once
#include "core/tls.h"

namespace venture::tls::a32 {
// API 32 bionic/libc/platform/bionic/tls_defines.h, ARM variant I:
// TP points at slot 0; slots are 4 bytes, -1 is BIONIC_TLS, 0 DTV,
// 1 THREAD_ID, 2 APP (not errno), 3/4 GL, 5 GUARD, 6 SANITIZER, 7 ART.
inline constexpr std::uint64_t surplus = 64 * 1024;
inline constexpr std::uint64_t allocation_size = surplus + 2 * kGuestPageSize;
inline constexpr std::uint32_t max_modules = 128;
Module add_module(std::span<const std::byte>, std::uint64_t, std::uint64_t, std::uint64_t);
void attach(GuestThread&);
void detach(GuestThread&);
GuestAddr get_addr(GuestThread&, std::uint64_t, std::uint64_t);
}  // namespace venture::tls::a32
