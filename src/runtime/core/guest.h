// Basic guest-side vocabulary shared by the whole runtime.
//
// Venture uses identity addressing: a guest virtual address is the host
// virtual address of the same byte. Guest pointers can therefore be used
// directly by host code, and host allocations can be handed to the guest.
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <stdexcept>

namespace venture {

using GuestAddr = std::uint64_t;

// Guest instruction set. Exactly one per process, chosen from the APK slice /
// ELF e_machine before HLE registration and never changed afterwards. A32
// guests live entirely in the host's low 4 GiB (docs/ARCHITECTURE.md, "ARM32
// guests"); A64 is the default and its paths are unaffected by A32 support.
enum class GuestIsa : std::uint8_t { a64, a32 };

template <class T = void>
inline T* g2h(GuestAddr address) {
  return reinterpret_cast<T*>(static_cast<std::uintptr_t>(address));
}

inline GuestAddr h2g(const volatile void* pointer) {
  return static_cast<GuestAddr>(reinterpret_cast<std::uintptr_t>(pointer));
}

// ARM ILP32 pointers are identities, never truncated host addresses.
inline std::uint32_t h2g32(const volatile void* pointer) {
  const auto address = h2g(pointer);
  if (address >= (1ull << 32)) throw std::overflow_error("host pointer does not fit A32 guest address space");
  return static_cast<std::uint32_t>(address);
}

// Linux/Android page size presented to the guest (sysconf, AT_PAGESZ, mmap).
inline constexpr std::size_t kGuestPageSize = 4096;

inline constexpr std::uint64_t align_down(std::uint64_t value, std::uint64_t alignment) {
  return value & ~(alignment - 1);
}
inline constexpr std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

// The guest is LP64 (aarch64-linux-android): `long` is 64-bit. Host code that
// implements guest APIs must use fixed-width types; `long` is 32-bit on Windows.
using guest_long = std::int64_t;
using guest_ulong = std::uint64_t;
using guest_size_t = std::uint64_t;
using guest_ssize_t = std::int64_t;
using guest_off_t = std::int64_t;
using guest_time_t = std::int64_t;
using guest_pid_t = std::int32_t;
using guest_uid_t = std::uint32_t;
using guest_mode_t = std::uint32_t;

}  // namespace venture
