#include "platform/low_memory.h"
#include "core/address_space.h"
#include "core/error.h"
#include "common/log.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>

namespace venture::platform {
namespace {
constexpr GuestAddr limit = 0x100000000ull;
void* exact_none(GuestAddr p, std::uint64_t n, int flags) {
  auto out = mmap(g2h(p), n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | flags, -1, 0);
  if (out != MAP_FAILED && out != g2h(p)) {
    munmap(out, n);
    return MAP_FAILED;
  }
  return out;
}
std::mutex guard_mutex;
bool guard_attempted = false, guarded = false;
std::mutex reservation_mutex;
bool reservation_attempted = false;
std::vector<LowRange> reservations;
std::exception_ptr reservation_error;
void reserve_arena() {
  // PIE is required: both text and brk must live outside guest address space.
  if (reinterpret_cast<std::uintptr_t>(&reserve_low_memory) < limit || h2g(sbrk(0)) < limit)
    throw std::runtime_error("A32 low arena requires a PIE image and brk above 4 GiB");
  std::ifstream maps("/proc/self/maps");
  if (!maps) throw std::runtime_error("A32 low arena: cannot enumerate /proc/self/maps");
  std::vector<LowRange> holes;
  GuestAddr cursor = 65536;
  std::string line;
  while (std::getline(maps, line)) {
    unsigned long long begin{}, end{};
    char perms[5]{};
    if (std::sscanf(line.c_str(), "%llx-%llx %4s", &begin, &end, perms) != 3)
      throw std::runtime_error("A32 low arena: invalid /proc/self/maps");
    if (begin >= limit) break;
    if (perms[1] == 'w') {
      log::error("memory", "A32 admission refused: host-writable [{:#x}, {:#x})", begin, end);
      throw std::runtime_error("A32 low arena: host-owned writable pages below 4 GiB");
    }
    if (begin > cursor) holes.push_back({cursor, begin - cursor});
    cursor = std::max(cursor, std::min(limit, static_cast<GuestAddr>(end)));
  }
  if (cursor < limit) holes.push_back({cursor, limit - cursor});
  // Old kernels may ignore NOREPLACE. Always check the actual result.
  reservations.reserve(holes.size());
  for (auto r : holes) {
    if (exact_none(r.start, r.size, MAP_FIXED_NOREPLACE) == MAP_FAILED)
      throw std::runtime_error("A32 low arena: exact reservation failed; no high-address fallback");
    reservations.push_back(r);
  }
}
}  // namespace
bool reserve_low_memory_guard() noexcept {
  std::lock_guard lock(guard_mutex);
  if (!guard_attempted) {
    guard_attempted = true;
    guarded = exact_none(limit, 65536, MAP_FIXED_NOREPLACE) != MAP_FAILED;
  }
  return guarded;
}
void pre_reserve_low_memory() noexcept {
  std::lock_guard lock(reservation_mutex);
  if (reservation_attempted) return;
  reservation_attempted = true;
  reserve_low_memory_guard();
  try { reserve_arena(); }
  catch (...) { reservation_error = std::current_exception(); }
}
void release_pre_reserved_low_memory() {
  std::lock_guard lock(reservation_mutex);
  while (!reservations.empty()) {
    const auto r = reservations.back();
    if (munmap(g2h(r.start), r.size))
      throw std::runtime_error("A64 early low reservation release failed");
    reservations.pop_back();
  }
  std::lock_guard guard_lock(guard_mutex);
  if (guarded && munmap(g2h(limit), 65536))
    throw std::runtime_error("A64 early wrap guard release failed");
  guard_attempted = guarded = reservation_attempted = false;
  reservation_error = nullptr;
}
std::vector<LowRange> reserve_low_memory() {
  pre_reserve_low_memory();
  if (reservation_error) std::rethrow_exception(reservation_error);
  if (!reserve_low_memory_guard())
    log::warn("memory", "A32 wrap guard occupied; using checked memory callbacks (identity fastmem disabled)");
  return reservations;
}
bool split_placeholder(GuestAddr, std::uint64_t) { return true; }
bool coalesce_placeholders(GuestAddr, std::uint64_t) { return true; }
bool replace_placeholder(GuestAddr, std::uint64_t) { return true; }
bool restore_placeholder(GuestAddr p, std::uint64_t n, bool) {
  return exact_none(p, n, MAP_FIXED) != MAP_FAILED;
}
int replace_file_placeholder(int fd, GuestAddr p, std::uint64_t n, std::uint64_t off, std::uint32_t prot) {
  if (prot & kProtWrite) return -err::kEOPNOTSUPP;
  struct stat st{};
  if (fstat(fd, &st)) return -err::kEBADF;
  if (off >= static_cast<std::uint64_t>(st.st_size) || n > align_up(st.st_size - off, 4096))
    return -err::kEOPNOTSUPP;
  auto view = mmap(g2h(p), n, (prot & (kProtRead | kProtExec)) ? PROT_READ : PROT_NONE,
                   MAP_SHARED | MAP_FIXED, fd, static_cast<off_t>(off));
  return view == g2h(p) ? 0 : errno == EACCES ? -err::kEACCES : -err::kENOMEM;
}
}  // namespace venture::platform
