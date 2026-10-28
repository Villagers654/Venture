#include "core/low_address_space.h"
#include "core/region_snapshot.h"
#include "core/memory_metadata.h"
#include "core/error.h"
#include "core/guest_heap.h"
#include "cpu/cpu.h"
#include "core/invalidation_stats.h"
#include "platform/low_memory.h"
#include "platform/memory.h"
#include "common/log.h"
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <stdexcept>

namespace venture::low32 {
namespace {
constexpr GuestAddr limit = 0x100000000ull;
struct Backing {
  std::uint64_t size;
  bool file = false;
};
struct State {
  std::mutex mutex;
  std::atomic<bool> initialized{false};
  bool attempted = false;
  Layout layout;
  std::vector<platform::LowRange> arena;
  std::map<GuestAddr, std::uint64_t> placeholders, allocations;
  std::map<GuestAddr, Backing> backing;
  std::map<GuestAddr, MemoryRegion> regions;
  // Region-free gaps inside the arena ranges (start -> end), kept in step with
  // `regions` so choose() visits gaps instead of every region.
  std::map<GuestAddr, GuestAddr> gaps;
  RegionSnapshot snapshot;
  std::string maps_text;
  bool maps_dirty = true;
};
State& state() {
  static auto* s = new State;
  return *s;
}
bool valid(GuestAddr p, std::uint64_t n) { return n && p >= 65536 && p < limit && n <= limit - p; }
bool owned(const State& s, GuestAddr p, std::uint64_t n) {
  if (!valid(p, n)) return false;
  for (auto r : s.arena)
    if (p >= r.start && p < r.start + r.size && n <= r.start + r.size - p) return true;
  return false;
}
bool overlap(const State& s, GuestAddr p, std::uint64_t n) {
  auto it = s.regions.lower_bound(p);
  return (it != s.regions.end() && it->first < p + n) ||
         (it != s.regions.begin() && std::prev(it)->first + std::prev(it)->second.size > p);
}
bool fits_gap(const State& s, GuestAddr p, std::uint64_t n, bool grow_down) {
  auto next = s.regions.lower_bound(p);
  if (next != s.regions.end() && p + n > next->second.gap_start()) return false;
  if (grow_down && next != s.regions.begin()) {
    const auto& previous = std::prev(next)->second;
    if (p < previous.start + previous.size ||
        p - (previous.start + previous.size) < kStackGuardGap) return false;
  }
  return true;
}
bool covered(const State& s, GuestAddr p, std::uint64_t n) {
  if (!valid(p, n)) return false;
  auto end = p + n;
  auto it = s.regions.upper_bound(p);
  if (it != s.regions.begin()) --it;
  for (; it != s.regions.end() && p < end; ++it) {
    if (it->first > p) return false;
    p = std::max(p, it->first + it->second.size);
  }
  return p >= end;
}
void split(State& s, GuestAddr p) {
  auto it = s.regions.upper_bound(p);
  if (it == s.regions.begin()) return;
  auto& r = (--it)->second;
  if (p >= r.start + r.size || p == r.start) return;
  auto tail = r;
  tail.size -= p - r.start;
  tail.file_offset += p - r.start;
  tail.start = p;
  r.size = p - r.start;
  s.maps_dirty = true;
  s.regions.emplace(p, std::move(tail));
}
void require(bool ok, const char* operation) {
  if (!ok) {
    log::error("memory", "A32 placeholder invariant failed: {}", operation);
    throw std::runtime_error(std::string("A32 placeholder invariant failed: ") + operation);
  }
}
// A new region at [p, p + n) splits the gap that contains it.
void take_gap(State& s, GuestAddr p, std::uint64_t n) {
  auto it = s.gaps.upper_bound(p);
  require(it != s.gaps.begin(), "gap");
  --it;
  const auto [begin, end] = *it;
  require(begin <= p && n <= end - p, "gap");
  s.gaps.erase(it);
  if (begin < p) s.gaps.emplace(begin, p);
  if (p + n < end) s.gaps.emplace(p + n, end);
}
// [p, p + n) holds no region now: replace the gaps around it, per arena range,
// with the one maximal gap bounded by the nearest regions or the range ends.
void release_gap(State& s, GuestAddr p, std::uint64_t n) {
  for (auto r : s.arena) {
    const auto range_end = r.start + r.size;
    const auto lo = std::max(p, r.start), hi = std::min(p + n, range_end);
    if (lo >= hi) continue;
    auto begin = r.start, end = range_end;
    if (auto next = s.regions.lower_bound(hi); next != s.regions.end()) end = std::min(end, next->first);
    if (auto prev = s.regions.lower_bound(lo); prev != s.regions.begin())
      begin = std::max(begin, std::prev(prev)->first + std::prev(prev)->second.size);
    for (auto it = s.gaps.lower_bound(begin); it != s.gaps.end() && it->first < end;) it = s.gaps.erase(it);
    s.gaps.emplace(begin, end);
  }
}
void split_placeholder(State& s, GuestAddr p) {
  auto it = s.placeholders.upper_bound(p);
  if (it == s.placeholders.begin()) return;
  --it;
  auto end = it->first + it->second;
  if (p <= it->first || p >= end) return;
  require(platform::split_placeholder(it->first, p - it->first), "split");
  it->second = p - it->first;
  s.placeholders.emplace(p, end - p);
}
void put_placeholder(State& s, GuestAddr p, std::uint64_t n) {
  auto it = s.placeholders.emplace(p, n).first;
  if (it != s.placeholders.begin()) {
    auto prev = std::prev(it);
    if (prev->first + prev->second == p) {
      require(platform::coalesce_placeholders(prev->first, prev->second + n), "coalesce left");
      prev->second += n;
      s.placeholders.erase(it);
      it = prev;
    }
  }
  auto next = std::next(it);
  if (next != s.placeholders.end() && it->first + it->second == next->first) {
    require(platform::coalesce_placeholders(it->first, it->second + next->second), "coalesce right");
    it->second += next->second;
    s.placeholders.erase(next);
  }
}
GuestAddr choose(State& s, std::uint64_t n, std::uint64_t alignment, bool fresh = false, bool grow_down = false) {
  // Small requests take the highest fitting address, large ones the lowest.
  const bool small = n < 1024 * 1024;
  auto fit = [&](GuestAddr begin, GuestAddr end) -> GuestAddr {
    if (auto next = s.regions.lower_bound(begin); next != s.regions.end())
      end = std::min(end, next->second.gap_start());
    if (grow_down) begin += kStackGuardGap;
    // Keep the kuser region available for the CPU packet even before it maps it.
    end = std::min(end, GuestAddr{0xffff0000});
    if (end <= begin || n > end - begin) return 0;
    auto p = small ? align_down(end - n, alignment) : align_up(begin, alignment);
    if (p < begin || p > end || n > end - p) return 0;
    return p;
  };
  // Gaps are disjoint, so visiting them in address order (downward for small
  // requests) makes the first fit the best one.
  if (fresh) {
    if (small) {
      for (auto it = s.placeholders.rbegin(); it != s.placeholders.rend(); ++it)
        if (auto p = fit(it->first, it->first + it->second)) return p;
    } else
      for (auto [p, size] : s.placeholders)
        if (auto q = fit(p, p + size)) return q;
    return 0;
  }
  if (small) {
    for (auto it = s.gaps.rbegin(); it != s.gaps.rend(); ++it)
      if (auto p = fit(it->first, it->second)) return p;
  } else
    for (auto [begin, end] : s.gaps)
      if (auto p = fit(begin, end)) return p;
  return 0;
}
bool ensure_private(State& s, GuestAddr p, std::uint64_t n) {
  split_placeholder(s, p);
  split_placeholder(s, p + n);
  auto end = p + n;
  while (p < end) {
    auto it = s.backing.upper_bound(p);
    if (it != s.backing.begin()) {
      auto prev = std::prev(it);
      if (p < prev->first + prev->second.size) {
        if (prev->second.file) return false;
        p = std::min(end, prev->first + prev->second.size);
        continue;
      }
    }
    auto hole = s.placeholders.find(p);
    if (hole == s.placeholders.end()) return false;
    auto size = hole->second;
    if (!platform::replace_placeholder(p, size)) return false;
    s.backing.emplace(p, Backing{size});
    s.placeholders.erase(hole);
    p += size;
  }
  return true;
}
template <class F>
bool each_backing(State& s, GuestAddr p, std::uint64_t n, F fn) {
  auto it = s.backing.upper_bound(p);
  if (it != s.backing.begin()) --it;
  auto end = p + n;
  for (; it != s.backing.end() && it->first < end; ++it) {
    auto begin = std::max(p, it->first), stop = std::min(end, it->first + it->second.size);
    if (begin < stop && !fn(begin, stop - begin, it->second.file)) return false;
  }
  return true;
}
void erase(State& s, GuestAddr p, std::uint64_t n) {
  split(s, p);
  split(s, p + n);
  std::vector<MemoryRegion> removed;
  auto first = s.regions.lower_bound(p), last = s.regions.lower_bound(p + n);
  for (auto it = first; it != last; ++it) removed.push_back(it->second);
  s.regions.erase(first, last);
  release_gap(s, p, n);
  s.maps_dirty = true;
  s.snapshot.publish(s.regions, p, p + n);
  for (const auto& r : removed) {
    if (r.prot & kProtExec) cpu_backend().invalidate(r.start, r.size, InvalidationCause::unmap);
  }
  // Also discard committed bytes of a failed mapping that was never published.
  require(each_backing(s, p, n, [](auto p, auto n, bool file) { return file || platform::decommit(p, n); }),
          "decommit");
  // Partial private unmaps retain the underlying reservation. Empty backing
  // spans are atomically returned to placeholders, never to the host allocator.
  // Only spans touching [p, p + n) can have lost their last region here.
  auto it = s.backing.upper_bound(p);
  if (it != s.backing.begin()) --it;
  for (; it != s.backing.end() && it->first < p + n;) {
    if (!overlap(s, it->first, it->second.size)) {
      auto base = it->first, size = it->second.size;
      require(platform::restore_placeholder(base, size, it->second.file), "restore");
      it = s.backing.erase(it);
      put_placeholder(s, base, size);
    } else
      ++it;
  }
}
std::int64_t map_locked(State& s, GuestAddr hint, std::uint64_t n, std::uint32_t prot, std::uint32_t flags,
                        const AddressSpace::FileReader* file, std::uint64_t off, std::string_view name,
                        std::uint64_t alignment) {
  if (!s.initialized.load()) throw std::runtime_error("A32 arena must be reserved before guest allocation");
  if (!n || n > limit - 65536 || (prot & ~7u) || (off & 4095) || (flags & 3) == 0 || (flags & 3) == 3)
    return -err::kEINVAL;
  constexpr auto supported = kMapPrivate | kMapShared | kMapFixed | kMapFixedNoReplace | kMapAnonymous |
                             kMapNoReserve | kMapGrowDown | 0x800 | 0x1000 | kMapStack;
  if (flags & ~supported) return -err::kEOPNOTSUPP;
  if ((flags & kMapGrowDown) && (!(flags & kMapAnonymous) || (flags & kMapShared))) return -err::kEINVAL;
  if (flags & kMapShared) {
    log::warn("memory", "A32 MAP_SHARED unavailable: {}", name);
    return -err::kEOPNOTSUPP;
  }
  if (!(flags & kMapAnonymous) && !file) return -err::kEBADF;
  n = align_up(n, 4096);
  if (!(flags & kMapAnonymous) && off > UINT64_MAX - n) return -err::kEOVERFLOW;
  GuestAddr p = align_down(hint, 4096);
  if (flags & (kMapFixed | kMapFixedNoReplace)) {
    if (hint != p || !owned(s, p, n)) return -err::kEINVAL;
    if ((flags & kMapFixedNoReplace) && overlap(s, p, n)) return -err::kEEXIST;
    erase(s, p, n);
  } else if (!owned(s, p, n) || overlap(s, p, n) || !fits_gap(s, p, n, flags & kMapGrowDown))
    p = choose(s, n, alignment, false, flags & kMapGrowDown);
  if (!p) return -err::kENOMEM;
  if (!ensure_private(s, p, n)) {
    erase(s, p, n);
    return -err::kENOMEM;
  }
  const bool backed = !(flags & kMapAnonymous);
  if ((prot || backed) && !each_backing(s, p, n, [&](auto p, auto n, bool) {
        return platform::commit(p, n, backed ? kProtRead | kProtWrite : prot);
      })) {
    erase(s, p, n);
    return -err::kENOMEM;
  }
  if (backed) {
    for (std::uint64_t delta = 0; delta < n; delta += 1ull << 20) {
      auto count = std::min<std::uint64_t>(n - delta, 1ull << 20);
      if (off > UINT64_MAX - delta || !(*file)(off + delta, g2h(p + delta), count)) {
        erase(s, p, n);
        return -err::kEIO;
      }
    }
    if (!each_backing(s, p, n, [&](auto p, auto n, bool) { return platform::protect(p, n, prot); })) {
      erase(s, p, n);
      return -err::kENOMEM;
    }
  }
  take_gap(s, p, n);
  s.regions.emplace(p, MemoryRegion{p, n, prot, std::string(name), off, bool(flags & kMapGrowDown)});
  s.maps_dirty = true;
  s.snapshot.publish(s.regions, p, p + n);
  if (prot & kProtExec) cpu_backend().invalidate(p, n, InvalidationCause::map);
  return p;
}
}  // namespace
Layout initialize() {
  auto& s = state();
  profile::TimedLock lock(s.mutex);
  if (s.initialized.load()) return s.layout;
  if (s.attempted) throw std::runtime_error("A32 arena startup previously failed");
  s.attempted = true;
  s.arena = platform::reserve_low_memory();
  // choose() visits gaps in address order; gaps never span two ranges.
  std::sort(s.arena.begin(), s.arena.end(), [](auto a, auto b) { return a.start < b.start; });
  for (auto r : s.arena) {
    s.placeholders.emplace(r.start, r.size);
    s.gaps.emplace(r.start, r.start + r.size);
    s.layout.total += r.size;
    s.layout.largest = std::max(s.layout.largest, r.size);
    ++s.layout.ranges;
    log::info("memory", "A32 reserved [{:#x}, {:#x}) ({} bytes)", r.start, r.start + r.size, r.size);
  }
  log::info("memory",
            "A32 arena: {} bytes total, {} bytes largest, {} ranges",
            s.layout.total, s.layout.largest, s.layout.ranges);
  s.initialized.store(true);
  return s.layout;
}
bool active() { return state().initialized.load(std::memory_order_relaxed); }
void publish_heap(GuestAddr p) {
  auto& s = state();
  profile::TimedLock lock(s.mutex);
  auto& r = s.regions.at(p);
  r.prot = kProtRead | kProtWrite;
  s.maps_dirty = true;
  s.snapshot.publish(s.regions, p, p + r.size);
}
Layout layout() {
  auto& s = state();
  profile::TimedLock lock(s.mutex);
  return s.layout;
}
std::int64_t map(GuestAddr p, std::uint64_t n, std::uint32_t prot, std::uint32_t flags,
                 const AddressSpace::FileReader* f, std::uint64_t off, std::string_view name,
                 std::uint64_t alignment) {
  auto& s = state();
  profile::TimedLock lock(s.mutex);
  return map_locked(s, p, n, prot, flags, f, off, name, alignment);
}
bool grow_stack(GuestAddr p, std::uint32_t access, std::uint64_t stack_limit) {
  p = align_down(p, kGuestPageSize);
  if (!valid(p, kGuestPageSize)) return false;
  auto& s = state(); profile::TimedLock lock(s.mutex);
  auto next = s.regions.upper_bound(p);
  if (next != s.regions.begin()) {
    const auto& previous = std::prev(next)->second;
    if (p < previous.start + previous.size) return false;
    if (!previous.grow_down && previous.prot && p - (previous.start + previous.size) < kStackGuardGap)
      return false;
  }
  if (next == s.regions.end()) return false;
  auto region = next->second;
  if (!region.grow_down || (region.prot & access) != access ||
      region.start + region.size - p > stack_limit) return false;
  const auto old_start = region.start, bytes = old_start - p;
  if (map_locked(s, p, bytes, region.prot, kMapPrivate | kMapAnonymous | kMapFixedNoReplace,
                 nullptr, 0, region.name, kGuestPageSize) < 0) return false;
  s.regions.erase(old_start);
  region.start = p; region.size += bytes;
  const auto end = p + region.size;
  s.regions.at(p) = std::move(region);
  s.maps_dirty = true;
  s.snapshot.publish(s.regions, p, end);
  return true;
}
std::int64_t map_file(int fd, std::uint64_t n, std::uint32_t prot, std::uint64_t off, std::string_view name) {
  if (!n || n > limit - 65536 || (off & 4095) || (prot & ~7u)) return -err::kEINVAL;
  if (prot & kProtWrite) return -err::kEOPNOTSUPP;
  n = align_up(n, 4096);
  if (off > UINT64_MAX - n) return -err::kEOVERFLOW;
  auto& s = state();
  profile::TimedLock lock(s.mutex);
  if (!s.initialized.load()) throw std::runtime_error("A32 arena not initialized");
  auto p = choose(s, n, 4096, true);
  if (!p) return -err::kENOMEM;
  // Per-page views permit Linux partial munmap/MAP_FIXED without unmapping
  // live neighboring views. Large private file mappings use FileReader copies.
  for (std::uint64_t i = 0; i < n; i += 4096) {
    split_placeholder(s, p + i);
    split_placeholder(s, p + i + 4096);
    auto error = platform::replace_file_placeholder(fd, p + i, 4096, off + i, prot);
    if (error) {
      erase(s, p, i);
      return error;
    }
    s.placeholders.erase(p + i);
    s.backing.emplace(p + i, Backing{4096, true});
  }
  take_gap(s, p, n);
  s.regions.emplace(p, MemoryRegion{p, n, prot, std::string(name), off});
  s.maps_dirty = true;
  s.snapshot.publish(s.regions, p, p + n);
  if (prot & kProtExec) cpu_backend().invalidate(p, n, InvalidationCause::map);
  return p;
}
std::int64_t unmap(GuestAddr p, std::uint64_t n) {
  if ((p & 4095) || !valid(p, n)) return -err::kEINVAL;
  auto& s = state();
  profile::TimedLock lock(s.mutex);
  erase(s, p, align_up(n, 4096));
  return 0;
}
std::int64_t protect(GuestAddr p, std::uint64_t n, std::uint32_t prot) {
  if ((p & 4095) || (prot & ~7u)) return -err::kEINVAL;
  if (!n) return 0;
  if (!valid(p, n)) return -err::kEINVAL;
  n = align_up(n, 4096);
  auto& s = state();
  profile::TimedLock lock(s.mutex);
  if (!covered(s, p, n)) return -err::kENOMEM;
  bool writable_file = false;
  each_backing(s, p, n, [&](auto, auto, bool file) {
    writable_file |= file && (prot & kProtWrite);
    return true;
  });
  if (writable_file) return -err::kEACCES;
  split(s, p);
  split(s, p + n);
  for (auto it = s.regions.lower_bound(p); it != s.regions.end() && it->first < p + n; ++it) {
    auto& r = it->second;
    if (!each_backing(s, r.start, r.size, [&](auto p, auto n, bool file) {
          // Guest instructions are read by the JIT, never executed as host code.
          // A read-only file section need not have host execute permission.
          auto host_prot = file && (prot & kProtExec) ? kProtRead : prot;
          return (file || platform::commit(p, n, prot)) && platform::protect(p, n, host_prot);
        }))
      return -err::kENOMEM;
    auto old = r.prot;
    r.prot = prot;
    s.maps_dirty = true;
    s.snapshot.publish(s.regions, r.start, r.start + r.size);
    if ((old | prot) & kProtExec) cpu_backend().invalidate(r.start, r.size, InvalidationCause::protect);
  }
  return 0;
}
std::int64_t advise(GuestAddr p, std::uint64_t n, int advice) {
  if (!n && !(p & 4095) && advice >= 0 && advice <= 4) return 0;
  if ((p & 4095) || !valid(p, n)) return -err::kEINVAL;
  auto& s = state();
  profile::TimedLock lock(s.mutex);
  n = align_up(n, 4096);
  if (!covered(s, p, n)) return -err::kENOMEM;
  if (advice >= 0 && advice < 4) return 0;
  if (advice != 4) return -err::kEINVAL;
  bool file = false;
  each_backing(s, p, n, [&](auto, auto, bool f) {
    file |= f;
    return true;
  });
  if (file) return -err::kEOPNOTSUPP;
  split(s, p);
  split(s, p + n);
  for (auto it = s.regions.lower_bound(p); it != s.regions.end() && it->first < p + n; ++it) {
    auto& r = it->second;
    if (r.prot & kProtExec) cpu_backend().invalidate(r.start, r.size, InvalidationCause::advise);
    if (!each_backing(s, r.start, r.size, [&](auto p, auto n, bool) {
          return platform::decommit(p, n) && (!r.prot || platform::commit(p, n, r.prot));
        }))
      return -err::kENOMEM;
  }
  return 0;
}
GuestAddr allocate(std::uint64_t n, std::uint32_t prot, std::string_view name, std::uint64_t alignment) {
  if (!alignment || (alignment & (alignment - 1)) || alignment >= limit) return 0;
  auto& s = state();
  profile::TimedLock lock(s.mutex);
  auto p = map_locked(s, 0, n, prot, kMapPrivate | kMapAnonymous, nullptr, 0, name,
                      std::max(alignment, std::uint64_t{4096}));
  if (p < 0) return 0;
  s.allocations.emplace(p, align_up(n, 4096));
  return p;
}
void free(GuestAddr p) {
  auto& s = state();
  profile::TimedLock lock(s.mutex);
  if (auto it = s.allocations.find(p); it != s.allocations.end()) {
    erase(s, p, it->second);
    s.allocations.erase(it);
  }
}
bool find(GuestAddr p, MemoryRegion* out) {
  auto& s = state();
  profile::TimedLock lock(s.mutex);
  auto it = s.regions.upper_bound(p);
  if (it == s.regions.begin() || p >= std::prev(it)->first + std::prev(it)->second.size) return false;
  if (out) *out = std::prev(it)->second;
  return true;
}
bool accessible(GuestAddr p, std::uint64_t n, bool write) {
  if (!valid(p, n)) return false;
  return state().snapshot.accessible(p, n, write) && guest_heap::accessible(p, n);
}
bool executable(GuestAddr p) { return state().snapshot.permitted(p, 1, kProtExec); }
bool query(GuestAddr p, std::uint64_t n, std::uint32_t prot) noexcept {
  if (!valid(p, n) || !prot || (prot & ~7u)) return false;
  if ((prot & kProtExec) && !state().snapshot.permitted(p, n, kProtExec)) return false;
  if ((prot & kProtRead) && !accessible(p, n, false)) return false;
  if ((prot & kProtWrite) && !accessible(p, n, true)) return false;
  return true;
}
bool covered(GuestAddr p, std::uint64_t n) {
  auto& s = state();
  profile::TimedLock lock(s.mutex);
  return covered(s, p, n);
}
void for_each(const std::function<void(const MemoryRegion&)>& visit) {
  std::vector<MemoryRegion> copy;
  {
    auto& s = state();
    profile::TimedLock lock(s.mutex);
    for (const auto& [p, r] : s.regions) copy.push_back(r);
  }
  for (const auto& r : copy) visit(r);
}
std::string proc_maps() {
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
std::uint64_t mapped_size() {
  auto& s = state();
  profile::TimedLock lock(s.mutex);
  std::uint64_t size = 0;
  for (const auto& [p, r] : s.regions) size += r.size;
  return size;
}
void label(GuestAddr p, std::uint64_t n, std::string_view name, std::uint64_t off) {
  auto& s = state();
  profile::TimedLock lock(s.mutex);
  if (!covered(s, p, n)) return;
  s.maps_dirty = true;
  split(s, p);
  split(s, p + n);
  for (auto it = s.regions.lower_bound(p); it != s.regions.end() && it->first < p + n; ++it) {
    it->second.name = name;
    it->second.file_offset = off + it->first - p;
  }
}
}  // namespace venture::low32
