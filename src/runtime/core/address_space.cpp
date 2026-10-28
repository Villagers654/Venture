#include "core/address_space.h"
#include "core/invalidation_stats.h"
#include "core/error.h"
#include "core/memory_metadata.h"
#include "core/low_address_space.h"
#include "core/region_snapshot.h"
#include "core/guest_heap.h"
#include "cpu/cpu.h"
#include "cpu/a32_backend.h"
#include "cpu/kuser.h"
#include "platform/memory.h"
#include "common/log.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <map>
#include <mutex>
#include <vector>
#include <format>

namespace venture {
namespace {
// Written only during startup before any guest/host callback threads exist.
// A failed A32 admission retains this policy so no later call can fall back high.
bool a32_policy = false;
bool a32_ready = false;
struct Reservation { GuestAddr start; std::uint64_t size; };
struct State {
  std::mutex mutex;
  std::map<GuestAddr, MemoryRegion> regions;
  std::map<GuestAddr, std::uint64_t> allocations;
  struct SharedView { platform::FileMapping mapping; std::uint64_t size; };
  std::map<GuestAddr, SharedView> shared_views;
  std::vector<Reservation> reservations;
  GuestAddr arena = 0;
  RegionSnapshot snapshot;
  std::string maps_text;
  bool maps_dirty = true;
  std::atomic<std::uint64_t> stack_limit{kDefaultStackLimit};
  static constexpr std::uint64_t arena_size = 1ull << 40;
};
State& state() { static auto* s = new State; return *s; }
// Republishes the lock-free snapshot after a change confined to [lo, hi).
void publish(State& s, GuestAddr lo, GuestAddr hi) {
  s.maps_dirty = true;
  s.snapshot.publish(s.regions, lo, hi);
}
bool valid_range(GuestAddr p, std::uint64_t n) { return n && n <= (1ull << 47) && p < (1ull << 47) && n <= (1ull << 47) - p; }
bool overlaps(const State& s, GuestAddr begin, GuestAddr end) {
  auto it = s.regions.lower_bound(begin);
  if (it != s.regions.end() && it->first < end) return true;
  if (it != s.regions.begin()) { --it; return it->first + it->second.size > begin; }
  return false;
}
void split(State& s, GuestAddr at) {
  auto it = s.regions.upper_bound(at);
  if (it == s.regions.begin()) return;
  --it;
  auto& r = it->second;
  if (at <= r.start || at >= r.start + r.size) return;
  auto tail = r;
  tail.file_offset += at - r.start;
  tail.size -= at - r.start;
  tail.start = at;
  r.size = at - r.start;
  s.maps_dirty = true;
  s.regions.emplace(at, std::move(tail));
}
bool covered(const State& s, GuestAddr begin, GuestAddr end) {
  auto it = s.regions.upper_bound(begin);
  if (it != s.regions.begin()) --it;
  for (; it != s.regions.end() && begin < end; ++it) {
    if (it->first > begin) return false;
    begin = std::max(begin, it->first + it->second.size);
  }
  return begin >= end;
}
bool reserve_range(State& s, GuestAddr p, std::uint64_t n) {
  const auto gran = platform::reservation_granularity();
  auto begin = align_down(p, gran), end = align_up(p + n, gran);
  // Existing runtime reservations may contain holes at 4 KiB granularity.
  while (begin < end) {
    auto next = end;
    bool found = false;
    for (const auto& r : s.reservations) {
      if (begin >= r.start && begin < r.start + r.size) {
        begin = std::min(end, r.start + r.size); found = true; break;
      }
      if (r.start > begin) next = std::min(next, r.start);
    }
    if (found) continue;
    if (!platform::reserve(begin, next - begin, true)) return false;
    s.reservations.push_back({begin, next - begin});
    begin = next;
  }
  return true;
}
GuestAddr choose(State& s, std::uint64_t size, std::uint64_t alignment, bool grow_down = false) {
  const auto below = grow_down ? kStackGuardGap : 0;
  if (!s.arena) {
    s.arena = platform::reserve(0x20000000000ull, State::arena_size, false);
    if (s.arena) s.reservations.push_back({s.arena, State::arena_size});
  }
  if (s.arena) {
    if (grow_down) {
      // Put grow-down VMAs at the high end of a hole. Bottom-up packing would
      // leave exactly stack_guard_gap below them and forbid their first growth.
      auto top = s.arena + State::arena_size;
      for (auto it = s.regions.rbegin(); it != s.regions.rend(); ++it) {
        const auto& r = it->second;
        if (r.start >= top) { top = std::min(top, r.gap_start()); continue; }
        if (r.start + r.size <= s.arena) break;
        const auto bottom = std::max(s.arena, r.start + r.size) + below;
        if (top >= size) {
          auto p = align_down(top - size, alignment);
          if (p >= bottom) return p;
        }
        top = std::min(top, r.gap_start());
      }
      if (top >= size) {
        auto p = align_down(top - size, alignment);
        if (p >= s.arena + below) return p;
      }
    }
    auto p = align_up(s.arena + below, alignment);
    for (auto it = s.regions.lower_bound(s.arena); it != s.regions.end() && it->first < s.arena + State::arena_size; ++it) {
      if (p + size <= it->second.gap_start()) break;
      p = align_up(it->first + it->second.size + below, alignment);
    }
    if (p <= s.arena + State::arena_size && size <= s.arena + State::arena_size - p) return p;
  }
  auto reserve_size = align_up(size + alignment + below, platform::reservation_granularity());
  auto p = platform::reserve(0, reserve_size, false);
  if (p) s.reservations.push_back({p, reserve_size});
  return p ? align_up(p + below, alignment) : 0;
}
bool fits_gap(const State& s, GuestAddr p, std::uint64_t size, bool grow_down) {
  auto next = s.regions.lower_bound(p);
  if (next != s.regions.end() && p + size > next->second.gap_start()) return false;
  if (grow_down && next != s.regions.begin()) {
    const auto& previous = std::prev(next)->second;
    if (p < previous.start + previous.size ||
        p - (previous.start + previous.size) < kStackGuardGap) return false;
  }
  return true;
}
void erase(State& s, GuestAddr p, std::uint64_t n) {
  split(s, p); split(s, p + n);
  auto first = s.regions.lower_bound(p), last = s.regions.lower_bound(p + n);
  std::vector<MemoryRegion> removed;
  for (auto it = first; it != last; ++it) removed.push_back(it->second);
  s.regions.erase(first, last);
  publish(s, p, p + n);
  for (const auto& r : removed) {
    if (r.prot & kProtExec) cpu_backend().invalidate(r.start, r.size, InvalidationCause::unmap);
    auto view = s.shared_views.upper_bound(r.start);
    const bool shared = view != s.shared_views.begin() && r.start < std::prev(view)->first + std::prev(view)->second.size;
    if (!shared) platform::decommit(r.start, r.size);
  }
  for (auto it = s.shared_views.lower_bound(p); it != s.shared_views.end() && it->first < p+n;) {
    platform::unmap_shared_file(it->second.mapping);
    it = s.shared_views.erase(it);
  }
}
std::int64_t map_locked(State& s, GuestAddr hint, std::uint64_t size, std::uint32_t prot,
                        std::uint32_t flags, const AddressSpace::FileReader* file,
                        std::uint64_t file_offset, std::string_view name, std::uint64_t alignment) {
  if (!valid_range(0, size) || (prot & ~7u) || (file_offset & 4095) ||
      (flags & (kMapShared | kMapPrivate)) == 0 ||
      (flags & (kMapShared | kMapPrivate)) == (kMapShared | kMapPrivate)) return -err::kEINVAL;
  constexpr auto supported = kMapShared | kMapPrivate | kMapFixed | kMapAnonymous | kMapNoReserve | kMapFixedNoReplace | kMapGrowDown;
  // Linux ignores legacy MAP_DENYWRITE/MAP_EXECUTABLE and treats MAP_STACK as a no-op.
  if (flags & ~(supported | 0x800u | 0x1000u | kMapStack)) {
    log::warn("memory", "unsupported mmap flags {:#x}", flags); return -err::kEOPNOTSUPP;
  }
  if (!(flags & kMapAnonymous) && !file) return -err::kEBADF;
  if ((flags & kMapGrowDown) && (!(flags & kMapAnonymous) || (flags & kMapShared))) return -err::kEINVAL;
  if ((flags & kMapShared) && !(flags & kMapAnonymous)) {
    log::warn("memory", "file MAP_SHARED is not implemented: {}", name);
    return -err::kEOPNOTSUPP;
  }
  size = align_up(size, kGuestPageSize);
  const bool fixed = flags & (kMapFixed | kMapFixedNoReplace);
  GuestAddr p = hint;
  if (fixed) {
    if (!p || (p & 4095) || !valid_range(p, size)) return -err::kEINVAL;
    if ((flags & kMapFixedNoReplace) && overlaps(s, p, p + size)) return -err::kEEXIST;
    for (const auto& [base, view] : s.shared_views)
      if (p < base+view.size && base < p+size) return -err::kEOPNOTSUPP;
    if (!reserve_range(s, p, size)) return -err::kENOMEM;
    erase(s, p, size);
  } else {
    p = align_down(hint, kGuestPageSize);
    if (!p || !valid_range(p, size) || overlaps(s, p, p + size) ||
        !fits_gap(s, p, size, flags & kMapGrowDown) || !reserve_range(s, p, size))
      p = choose(s, size, alignment, flags & kMapGrowDown);
    if (!p) return -err::kENOMEM;
  }
  const bool backed = !(flags & kMapAnonymous);
  if ((prot || backed) && !platform::commit(p, size, backed ? kProtRead | kProtWrite : prot)) return -err::kENOMEM;
  if (backed) {
    for (std::uint64_t off = 0; off < size; off += 1ull << 20) {
      auto count = std::min<std::uint64_t>(size - off, 1ull << 20);
      if (!(*file)(file_offset + off, g2h(p + off), count)) {
        platform::decommit(p, size); return -err::kEIO;
      }
    }
    if (!platform::protect(p, size, prot)) { platform::decommit(p, size); return -err::kENOMEM; }
  }
  s.regions.emplace(p, MemoryRegion{p, size, prot, std::string(name), file_offset, bool(flags & kMapGrowDown)});
  publish(s, p, p + size);
  if (prot & kProtExec) cpu_backend().invalidate(p, size, InvalidationCause::map);
  return static_cast<std::int64_t>(p);
}
}
AddressSpace& AddressSpace::get() { static AddressSpace instance; return instance; }
void AddressSpace::initialize(GuestIsa isa) {
  if (isa == GuestIsa::a32) {
    auto& s = state();
    profile::TimedLock lock(s.mutex);
    if (!s.reservations.empty() || !s.regions.empty())
      throw std::runtime_error("A32 ISA selected after guest allocation");
    if (a32_policy) {
      if (!a32_ready) throw std::runtime_error("A32 memory startup previously failed");
      return;
    }
    a32_policy = true;
    low32::initialize();
    guest_heap::select32();
    set_a32_memory_query(low32::query);
    if (auto error = install_kuser_page()) {
      log::error("memory", "A32 kuser page installation failed: Linux errno {}", -error);
      throw std::runtime_error("A32 kuser page installation failed");
    }
    a32_ready = true;
  } else if (a32_policy) {
    throw std::runtime_error("cannot change ISA after A32 arena selection");
  } else {
    guest_heap::select64();
  }
}
bool AddressSpace::is_guest_accessible(GuestAddr p, std::uint64_t n, bool write) const {
  if (a32_policy) return low32::accessible(p, n, write);
  return state().snapshot.accessible(p, n, write);
}
std::uint64_t AddressSpace::stack_limit() const { return state().stack_limit.load(std::memory_order_relaxed); }
void AddressSpace::set_stack_limit(std::uint64_t n) { state().stack_limit.store(n, std::memory_order_relaxed); }
bool AddressSpace::grow_stack(GuestAddr p, std::uint32_t access) {
  if (!(access & 7) || (access & ~7u)) return false;
  if (a32_policy) return low32::grow_stack(p, access, stack_limit());
  if (!(access & kProtExec)) p &= 0x00ff'ffff'ffff'ffffull; // A64 TBI data addresses
  p = align_down(p, kGuestPageSize);
  if (p < 65536 || !valid_range(p, kGuestPageSize)) return false;
  auto& s = state(); profile::TimedLock lock(s.mutex);
  auto next = s.regions.upper_bound(p);
  if (next != s.regions.begin()) {
    const auto& previous = std::prev(next)->second;
    if (p < previous.start + previous.size) return false; // protection fault, not growth
    if (!previous.grow_down && previous.prot && p - (previous.start + previous.size) < kStackGuardGap)
      return false;
  }
  if (next == s.regions.end()) return false;
  auto region = next->second;
  if (!region.grow_down || (region.prot & access) != access ||
      region.start + region.size - p > stack_limit()) return false;
  const auto old_start = region.start, bytes = old_start - p;
  // Linux expand_downwards: commit the fault-to-VMA interval, keep the upper
  // bound, and preserve stack_guard_gap to the preceding accessible VMA.
  if (map_locked(s, p, bytes, region.prot, kMapPrivate | kMapAnonymous | kMapFixedNoReplace,
                 nullptr, 0, region.name, kGuestPageSize) < 0) return false;
  s.regions.erase(old_start);
  region.start = p; region.size += bytes;
  s.regions.at(p) = std::move(region);
  publish(s, p, old_start);
  return true;
}
std::int64_t AddressSpace::map_readonly_file(int fd, std::uint64_t n, std::uint32_t prot, std::uint64_t off,
                                             std::string_view name) {
  if (a32_policy) return low32::map_file(fd, n, prot, off, name);
  return -err::kEOPNOTSUPP;
}
std::int64_t AddressSpace::map(GuestAddr hint, std::uint64_t size, std::uint32_t prot, std::uint32_t flags,
                             const FileReader* file, std::uint64_t offset, std::string_view name) {
  if (a32_policy) return low32::map(hint, size, prot, flags, file, offset, name);
  auto& s = state(); profile::TimedLock lock(s.mutex);
  return map_locked(s, hint, size, prot, flags, file, offset, name, kGuestPageSize);
}
std::int64_t AddressSpace::unmap(GuestAddr p, std::uint64_t size) {
  if (a32_policy) return low32::unmap(p, size);
  if ((p & 4095) || !valid_range(p, size)) return -err::kEINVAL;
  auto& s = state(); profile::TimedLock lock(s.mutex);
  size = align_up(size, kGuestPageSize);
  for (const auto& [base, view] : s.shared_views)
    if (p < base+view.size && base < p+size && (p > base || p+size < base+view.size)) {
      log::warn("memory", "partial shared file unmapping is unavailable"); return -err::kEOPNOTSUPP;
    }
  erase(s, p, align_up(size, kGuestPageSize)); return 0;
}
std::int64_t AddressSpace::protect(GuestAddr p, std::uint64_t size, std::uint32_t prot) {
  if (a32_policy) return low32::protect(p, size, prot);
  if ((p & 4095) || (prot & ~7u)) return -err::kEINVAL;
  if (!size) return 0;
  if (!valid_range(p, size)) return -err::kEINVAL;
  size = align_up(size, kGuestPageSize);
  auto& s = state(); profile::TimedLock lock(s.mutex);
  if (!covered(s, p, p + size)) return -err::kENOMEM;
  split(s, p); split(s, p + size);
  std::int64_t result = 0;
  std::vector<std::pair<GuestAddr, std::uint64_t>> stale;
  for (auto it = s.regions.lower_bound(p); it != s.regions.end() && it->first < p + size; ++it) {
    auto& r = it->second;
    // Commit also handles untouched PROT_NONE reservations, and never discards existing bytes.
    auto view = s.shared_views.upper_bound(r.start);
    const bool shared = view != s.shared_views.begin() && r.start < std::prev(view)->first + std::prev(view)->second.size;
    if ((!shared && !platform::commit(r.start, r.size, prot)) || !platform::protect(r.start, r.size, prot)) {
      result = -err::kENOMEM; break;
    }
    if ((r.prot | prot) & kProtExec) stale.emplace_back(r.start, r.size);
    r.prot = prot;
  }
  // One republish for the whole range, before translations of it are dropped.
  publish(s, p, p + size);
  for (const auto& [start, n] : stale) cpu_backend().invalidate(start, n, InvalidationCause::protect);
  return result;
}
std::int64_t AddressSpace::advise(GuestAddr p, std::uint64_t size, int advice) {
  if (a32_policy) return low32::advise(p, size, advice);
  if (!size && !(p & 4095) && advice >= 0 && advice <= 4) return 0;
  if ((p & 4095) || !valid_range(p, size)) return -err::kEINVAL;
  auto& s = state(); profile::TimedLock lock(s.mutex);
  size = align_up(size, kGuestPageSize);
  if (!covered(s, p, p + size)) return -err::kENOMEM;
  if (advice == 0 || advice == 1 || advice == 2 || advice == 3) return 0;
  if (advice != 4) return -err::kEINVAL;
  // MADV_DONTNEED may leave resident shared file pages in place; it must never
  // replace their coherent backing with zero-filled anonymous pages.
  for (const auto& [base, view] : s.shared_views)
    if (p < base+view.size && base < p+size) return -err::kEOPNOTSUPP;
  split(s, p); split(s, p + size);
  for (auto it = s.regions.lower_bound(p); it != s.regions.end() && it->first < p + size; ++it) {
    auto& r = it->second;
    if (r.prot & kProtExec) cpu_backend().invalidate(r.start, r.size, InvalidationCause::advise);
    if (!platform::decommit(r.start, r.size) || (r.prot && !platform::commit(r.start, r.size, r.prot))) return -err::kENOMEM;
  }
  return 0;
}
std::int64_t AddressSpace::map_shared_file(int fd, std::uint64_t size, std::uint32_t prot,
                                         std::uint32_t flags, std::uint64_t offset, std::string_view name) {
  if (flags & kMapGrowDown) return -err::kEINVAL;
  if (a32_policy) {
    if ((flags & 3) != kMapShared || (flags & ~(kMapShared | kMapNoReserve | 0x800u | 0x1000u)))
      return -err::kEOPNOTSUPP;
    if (!(prot & kProtWrite)) return low32::map_file(fd, size, prot, offset, name);
    log::warn("memory", "A32 writable shared file mmap unavailable: {}", name);
    return -err::kEOPNOTSUPP;
  }
  if (!valid_range(0, size) || (prot & ~7u) || (offset & 4095) || (flags & 3) != kMapShared) return -err::kEINVAL;
  if (flags & ~(kMapShared | kMapNoReserve | 0x800u | 0x1000u)) {
    log::warn("memory", "unsupported shared file mmap flags {:#x}", flags); return -err::kEOPNOTSUPP;
  }
  auto& s = state(); profile::TimedLock lock(s.mutex);
  platform::FileMapping view;
  if (auto error = platform::map_shared_file(fd, offset, size, prot, view)) return error;
  const auto rounded = align_up(size, kGuestPageSize);
  if (!valid_range(view.address, rounded) || overlaps(s, view.address, view.address+rounded)) {
    platform::unmap_shared_file(view); return -err::kENOMEM;
  }
  s.shared_views.emplace(view.address, State::SharedView{view, rounded});
  s.regions.emplace(view.address, MemoryRegion{view.address, rounded, prot, std::string(name), offset});
  publish(s, view.address, view.address + rounded);
  return view.address;
}
std::int64_t AddressSpace::sync(GuestAddr p, std::uint64_t size, bool synchronous) {
  if (a32_policy) {
    if (p & 4095) return -err::kEINVAL;
    return !size || low32::covered(p, size) ? 0 : -err::kENOMEM;
  }
  if ((p & 4095) || (size && !valid_range(p, size))) return -err::kEINVAL;
  if (!size) return 0;
  auto& s = state(); profile::TimedLock lock(s.mutex);
  if (!covered(s, p, p+size)) return -err::kENOMEM;
  for (const auto& [base, view] : s.shared_views) {
    const auto begin = std::max(base, p), end = std::min(base+view.size, p+size);
    if (begin < end) if (auto error = platform::sync_shared_file(begin, end-begin, synchronous)) return error;
  }
  return 0;
}
GuestAddr AddressSpace::allocate(std::uint64_t size, std::uint32_t prot, std::string_view name, std::uint64_t alignment) {
  if (a32_policy) return low32::allocate(size, prot, name, alignment);
  if (!alignment || (alignment & (alignment - 1)) || alignment > (1ull << 40)) return 0;
  auto& s = state(); profile::TimedLock lock(s.mutex);
  auto p = map_locked(s, 0, size, prot, kMapPrivate | kMapAnonymous, nullptr, 0, name, std::max<std::uint64_t>(alignment, 4096));
  if (p < 0) return 0;
  s.allocations.emplace(p, align_up(size, 4096)); return p;
}
void AddressSpace::free(GuestAddr p) {
  if (a32_policy) {
    low32::free(p);
    return;
  }
  auto& s = state(); profile::TimedLock lock(s.mutex);
  if (auto it = s.allocations.find(p); it != s.allocations.end()) { erase(s, p, it->second); s.allocations.erase(it); }
}
bool AddressSpace::find(GuestAddr p, MemoryRegion* out) const {
  if (a32_policy) return low32::find(p, out);
  auto& s = state(); profile::TimedLock lock(s.mutex);
  auto it = s.regions.upper_bound(p);
  if (it == s.regions.begin()) return false;
  --it;
  if (p >= it->first + it->second.size) return false;
  if (out) *out = it->second;
  return true;
}
bool AddressSpace::is_guest_executable(GuestAddr p) const {
  if (a32_policy) return low32::executable(p);
  // Translation cannot take the mutation mutex: a mapper may be awaiting this CPU's halt.
  return state().snapshot.permitted(p, 1, kProtExec);
}
void AddressSpace::for_each(const std::function<void(const MemoryRegion&)>& visit) const {
  if (a32_policy) {
    low32::for_each(visit);
    return;
  }
  std::vector<MemoryRegion> copy;
  { auto& s = state(); profile::TimedLock lock(s.mutex); for (const auto& [p, r] : s.regions) copy.push_back(r); }
  for (const auto& r : copy) visit(r);
}
void append_proc_map(std::string& text, const MemoryRegion& r) {
  // fs/proc/task_mmu.c: file mappings carry the file's device and inode;
  // anonymous and named-anonymous ([stack], [anon:...]) ones print 00:00 0.
  // Tools tell file-backed modules from anonymous memory by a nonzero inode.
  std::uint32_t inode = 0;
  if (r.name.starts_with('/')) {
    inode = 2166136261u;
    for (unsigned char c : r.name) inode = (inode ^ c) * 16777619u;
    inode = inode ? inode : 1;
  }
  std::format_to(std::back_inserter(text), "{:012x}-{:012x} {}{}{}p {:08x} {} {} {}\n",
                 r.start, r.start + r.size, (r.prot & 1) ? 'r' : '-',
                 (r.prot & 2) ? 'w' : '-', (r.prot & 4) ? 'x' : '-', r.file_offset,
                 inode ? "fd:00" : "00:00", inode, r.name);
}
std::string AddressSpace::proc_maps() const {
  if (a32_policy) return low32::proc_maps();
  auto& s = state();
  profile::TimedLock lock(s.mutex);
  if (s.maps_dirty) {
    std::string text;
    text.reserve(s.regions.size() * 96);
    for (const auto& [p, r] : s.regions) append_proc_map(text, r);
    s.maps_text = std::move(text);
    s.maps_dirty = false;
  }
  return s.maps_text;
}
std::uint64_t AddressSpace::mapped_size() const {
  if (a32_policy) return low32::mapped_size();
  auto& s = state();
  profile::TimedLock lock(s.mutex);
  std::uint64_t size = 0;
  for (const auto& [p, r] : s.regions) size += r.size;
  return size;
}
void label_memory(GuestAddr p, std::uint64_t size, std::string_view name, std::uint64_t file_offset) {
  if (a32_policy) {
    low32::label(p, size, name, file_offset);
    return;
  }
  auto& s = state(); profile::TimedLock lock(s.mutex);
  if (!valid_range(p, size) || !covered(s, p, p + size)) return;
  s.maps_dirty = true;
  split(s, p); split(s, p + size);
  for (auto it = s.regions.lower_bound(p); it != s.regions.end() && it->first < p + size; ++it) {
    it->second.name = name; it->second.file_offset = file_offset + it->first - p;
  }
}
}
