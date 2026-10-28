#include "platform/low_memory.h"
#include "core/address_space.h"
#include "core/error.h"
#include "common/log.h"
#include <windows.h>
#include <io.h>
#include <algorithm>
#include <stdexcept>
#include <string>

namespace venture::platform {
namespace {
constexpr GuestAddr limit = 0x100000000ull;
// No heap, logging, mutex constructors or other C++ initialization on the early
// CRT path. At most one hole per 64 KiB allocation unit can be reserved.
struct Reservations {
  SRWLOCK lock = SRWLOCK_INIT;
  LowRange ranges[65536]{};
  std::size_t count = 0;
  bool attempted = false, guard_attempted = false, guarded = false;
  const char* failure = nullptr;
  GuestAddr failed_address = 0;
  DWORD error = 0, guard_error = 0;
};
constinit Reservations reservations;
void record_failure(const char* operation, GuestAddr p) {
  reservations.failure = operation;
  reservations.failed_address = p;
  reservations.error = GetLastError();
}
void guard_locked() {
  if (reservations.guard_attempted) return;
  reservations.guard_attempted = true;
  reservations.guarded = VirtualAlloc2(GetCurrentProcess(), g2h(limit), 65536,
      MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0) == g2h(limit);
  if (!reservations.guarded) reservations.guard_error = GetLastError();
  // An existing allocation belongs to the host. Never release or protect it;
  // checked A32 memory callbacks reject accesses extending beyond 4 GiB.
}
[[noreturn]] void fail(const char* operation, GuestAddr p) {
  log::error("memory", "A32 {} at {:#x}: Win32 error {}", operation, p, GetLastError());
  throw std::runtime_error(std::string("A32 low arena: ") + operation + " failed; no high-address fallback");
}
}  // namespace
bool reserve_low_memory_guard() noexcept {
  AcquireSRWLockExclusive(&reservations.lock);
  guard_locked();
  const bool guarded = reservations.guarded;
  ReleaseSRWLockExclusive(&reservations.lock);
  return guarded;
}
void release_pre_reserved_low_memory() {
  AcquireSRWLockExclusive(&reservations.lock);
  const auto count = reservations.count;
  const bool guard = reservations.guarded;
  GuestAddr failed = 0;
  DWORD error = 0;
  while (reservations.count) {
    const auto r = reservations.ranges[reservations.count - 1];
    if (!VirtualFree(g2h(r.start), 0, MEM_RELEASE)) {
      failed = r.start; error = GetLastError(); break;
    }
    --reservations.count;
  }
  if (!error && reservations.guarded) {
    if (!VirtualFree(g2h(limit), 0, MEM_RELEASE)) {
      failed = limit; error = GetLastError();
    } else reservations.guarded = false;
  }
  if (!error) {
    reservations.attempted = reservations.guard_attempted = false;
    reservations.failure = nullptr;
    reservations.error = reservations.guard_error = 0;
  }
  ReleaseSRWLockExclusive(&reservations.lock);
  if (error) { SetLastError(error); fail("early reservation release", failed); }
  if (count || guard)
    log::info("memory", "A64 released early low arena: {} ranges, wrap guard={}", count, guard);
}
void pre_reserve_low_memory() noexcept {
  AcquireSRWLockExclusive(&reservations.lock);
  if (reservations.attempted) {
    ReleaseSRWLockExclusive(&reservations.lock);
    return;
  }
  reservations.attempted = true;
  guard_locked(); // Claim the boundary before any enumeration or allocations.
  // Reserve each hole immediately, without allocating a vector between query
  // and reservation. Include address zero in the admission check.
  unsigned retries = 0;
  for (GuestAddr p = 0; p < limit;) {
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQuery(g2h(p), &m, sizeof(m))) { record_failure("enumeration", p); break; }
    auto end = std::min(limit, h2g(m.BaseAddress) + m.RegionSize);
    if (end <= p) { record_failure("invalid host memory map", p); break; }
    if (m.State == MEM_COMMIT &&
        (m.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
      record_failure("host-owned writable pages below 4 GiB", p);
      break;
    }
    if (m.State == MEM_FREE) {
      // VirtualQuery reports holes between KUSER_SHARED_DATA pages as FREE,
      // but the containing 64 KiB allocation unit is owned by Windows. New
      // VirtualAlloc2 placeholders still require allocation-granular bases
      // (only replacement lifts that restriction). Reserve every free unit;
      // leave inaccessible fringes of foreign units outside the guest map.
      // Microsoft: devblogs.microsoft.com/oldnewthing/20221216-00/?p=107598
      auto begin = align_up(std::max(p, GuestAddr{65536}), 65536);
      auto stop = align_down(end, 65536);
      if (begin < stop) {
        if (VirtualAlloc2(GetCurrentProcess(), g2h(begin), stop - begin,
            MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0) != g2h(begin)) {
          // Another allocator may have claimed part of the queried hole. Query
          // it again and apply the same admission policy to the new mapping.
          if (GetLastError() == ERROR_INVALID_ADDRESS && ++retries <= 64) continue;
          record_failure("placeholder reservation", begin);
          break;
        }
        reservations.ranges[reservations.count++] = {begin, stop - begin};
      }
    }
    retries = 0;
    p = end;
  }
  ReleaseSRWLockExclusive(&reservations.lock);
}
std::vector<LowRange> reserve_low_memory() {
  pre_reserve_low_memory();
  if (reservations.failure) {
    SetLastError(reservations.error);
    fail(reservations.failure, reservations.failed_address);
  }
  if (!reservations.guarded)
    log::warn("memory", "A32 wrap guard unavailable (Win32 error {}); using checked memory callbacks (identity fastmem disabled)",
              reservations.guard_error);
  return {reservations.ranges, reservations.ranges + reservations.count};
}
bool split_placeholder(GuestAddr p, std::uint64_t n) {
  return VirtualFree(g2h(p), n, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != 0;
}
bool coalesce_placeholders(GuestAddr p, std::uint64_t n) {
  return VirtualFree(g2h(p), n, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS) != 0;
}
bool replace_placeholder(GuestAddr p, std::uint64_t n) {
  return VirtualAlloc2(GetCurrentProcess(), g2h(p), n, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS,
                       nullptr, 0) == g2h(p);
}
bool restore_placeholder(GuestAddr p, std::uint64_t n, bool file) {
  const bool result = file ? UnmapViewOfFile2(GetCurrentProcess(), g2h(p), MEM_PRESERVE_PLACEHOLDER) != 0
                           : VirtualFree(g2h(p), n, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != 0;
  if (!result)
    log::error("memory", "A32 restore [{:#x}, {:#x}), file={}: Win32 error {}", p, p + n, file,
               GetLastError());
  return result;
}
int replace_file_placeholder(int fd, GuestAddr p, std::uint64_t n, std::uint64_t off, std::uint32_t prot) {
  if (prot & kProtWrite) return -err::kEOPNOTSUPP;
  auto h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (h == INVALID_HANDLE_VALUE) return -err::kEBADF;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(h, &size)) return -err::kEIO;
  if (off >= static_cast<std::uint64_t>(size.QuadPart) ||
      n > align_up(static_cast<std::uint64_t>(size.QuadPart) - off, 4096))
    return -err::kEOPNOTSUPP;
  auto section = CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (!section) return GetLastError() == ERROR_ACCESS_DENIED ? -err::kEACCES : -err::kENOMEM;
  // memoryapi.h: MEM_REPLACE_PLACEHOLDER removes the 64 KiB base/offset
  // restriction; both must match the placeholder and be page aligned.
  auto view = MapViewOfFile3(section, GetCurrentProcess(), g2h(p), off, n, MEM_REPLACE_PLACEHOLDER,
                             PAGE_READONLY, nullptr, 0);
  CloseHandle(section);
  if (view != g2h(p)) return -err::kENOMEM;
  if (!(prot & (kProtRead | kProtExec))) {
    DWORD old;
    if (!VirtualProtect(view, n, PAGE_NOACCESS, &old)) {
      if (!restore_placeholder(p, n, true)) fail("file view rollback", p);
      return -err::kEACCES;
    }
  }
  return 0;
}
}  // namespace venture::platform
