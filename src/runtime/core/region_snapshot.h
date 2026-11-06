#pragma once
#include "core/address_space.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace venture {
// RCU-style immutable permissions snapshot. Readers use only lock-free atomics,
// including in JIT fault callbacks. Writers hold their address-space mutex.
//
// Reclamation uses two reader counters selected by the parity of a grace
// period counter, so it never needs an instant with no readers at all: while
// translation threads check executability per fetched instruction there may
// be none, and a single counter then kept every replaced snapshot (one copy
// of the whole map per mmap/munmap/mprotect) until the readers paused.
class RegionSnapshot {
  struct Range {
    GuestAddr begin, end;
    std::uint32_t prot;
  };
  using Ranges = std::vector<Range>;
  struct Retired {
    std::unique_ptr<Ranges> ranges;
    std::uint64_t period;
  };
  static_assert(std::atomic<const Ranges*>::is_always_lock_free);
  static_assert(std::atomic<unsigned>::is_always_lock_free);
  mutable std::atomic<unsigned> readers_[2]{};
  std::atomic<std::uint64_t> period_{0};
  std::atomic<const Ranges*> current_{nullptr};
  std::unique_ptr<Ranges> owner_;
  std::vector<Retired> retired_;
  std::vector<std::unique_ptr<Ranges>> spare_;

  // Ends the grace period if the readers that entered under the previous
  // parity are gone; new readers count under the current one meanwhile, so
  // this drains within one reader's critical section (a binary search).
  // A bounded wait keeps a descheduled reader from stalling the writer; the
  // next publish retries.
  void advance() {
    const auto period = period_.load();
    auto& previous = readers_[(period + 1) & 1];
    for (int spin = 0; previous.load(); ++spin) {
      if (spin == 64) return;
      std::this_thread::yield();
    }
    period_.store(period + 1);
  }
  void install(std::unique_ptr<Ranges> next) {
    // All operations are sequentially consistent. A reader that can hold the
    // replaced snapshot counted itself (under either parity) before the
    // replacement. Ending period p (read after it) finds parity p - 1 empty
    // and ending p + 1 finds parity p empty, both after the replacement, so by
    // period p + 2 every such reader has left; one that counts itself later
    // loads a newer snapshot. The parity sends new readers to the other
    // counter, so the one being checked drains.
    const Ranges* replaced = owner_.get();
    current_.store(next.get());
    if (replaced) retired_.push_back({std::move(owner_), period_.load()});
    owner_ = std::move(next);
    advance();
    advance();
    // Reclaimed buffers are kept for the next publishes, so a steady stream of
    // mmap/munmap/mprotect calls copies into existing capacity and never
    // allocates or frees a map-sized block on the guest thread.
    const auto period = period_.load();
    std::size_t kept = 0;
    for (auto& r : retired_) {
      if (period < r.period + 2) {
        if (&retired_[kept] != &r) retired_[kept] = std::move(r);
        ++kept;
        continue;
      }
      if (spare_.size() < 2) { r.ranges->clear(); spare_.push_back(std::move(r.ranges)); }
    }
    retired_.resize(kept);
  }
  std::unique_ptr<Ranges> buffer() {
    if (spare_.empty()) return std::make_unique<Ranges>();
    auto next = std::move(spare_.back());
    spare_.pop_back();
    return next;
  }
  struct ReadScope {
    std::atomic<unsigned>& readers;
    explicit ReadScope(std::atomic<unsigned>* counters, const std::atomic<std::uint64_t>& period)
        : readers(counters[period.load() & 1]) { readers.fetch_add(1); }
    ~ReadScope() { readers.fetch_sub(1); }
  };

 public:
  template <class Map>
  void publish(const Map& regions) {
    auto next = buffer();
    next->reserve(regions.size());
    for (const auto& [p, r] : regions) next->push_back({p, p + r.size, r.prot});
    install(std::move(next));
  }
  // Republishes after changes confined to [lo, hi). Outside that span the
  // current snapshot already matches the map (boundaries may differ only by
  // splits), so it is copied and only the span is read from the map.
  template <class Map>
  void publish(const Map& regions, GuestAddr lo, GuestAddr hi) {
    if (!owner_) return publish(regions);
    if (lo >= hi) return;
    const Ranges& old = *owner_;
    const auto by_begin = [](const Range& r, GuestAddr p) { return r.begin < p; };
    // Widen the span until no old range or map region crosses either end.
    const auto crosses = [&](GuestAddr p, GuestAddr& begin, GuestAddr& end) {
      bool found = false;
      begin = p, end = p;
      auto o = std::lower_bound(old.begin(), old.end(), p, by_begin);
      if (o != old.begin() && std::prev(o)->end > p) {
        begin = std::prev(o)->begin, end = std::prev(o)->end, found = true;
      }
      auto m = regions.lower_bound(p);
      if (m != regions.begin() && std::prev(m)->first + std::prev(m)->second.size > p) {
        begin = std::min(begin, std::prev(m)->first);
        end = std::max(end, std::prev(m)->first + std::prev(m)->second.size);
        found = true;
      }
      return found;
    };
    for (GuestAddr begin, end; crosses(lo, begin, end) && begin < lo;) lo = begin;
    for (GuestAddr begin, end; crosses(hi, begin, end) && end > hi;) hi = end;
    auto next = buffer();
    next->reserve(old.size() + 2);
    const auto head = std::lower_bound(old.begin(), old.end(), lo, by_begin);
    next->insert(next->end(), old.begin(), head);
    for (auto it = regions.lower_bound(lo); it != regions.end() && it->first < hi; ++it)
      next->push_back({it->first, it->first + it->second.size, it->second.prot});
    next->insert(next->end(), std::lower_bound(head, old.end(), hi, by_begin), old.end());
    install(std::move(next));
  }
  bool permitted(GuestAddr p, std::uint64_t n, std::uint32_t mask) const {
    if (!n || p + n < p) return false;
    ReadScope scope(readers_, period_);
    auto* ranges = current_.load();
    bool result = false;
    if (ranges) {
      auto it = std::upper_bound(ranges->begin(), ranges->end(), p,
                                 [](auto p, const Range& r) { return p < r.begin; });
      if (it != ranges->begin()) --it;
      auto end = p + n;
      for (; it != ranges->end() && p < end; ++it) {
        if (it->end <= p) continue;
        if (it->begin > p || !(it->prot & mask)) break;
        p = std::min(end, it->end);
      }
      result = p == end;
    }
    return result;
  }
  // Replaced snapshots still awaiting their grace period (writer side; tests).
  std::size_t retired() const { return retired_.size(); }
  bool accessible(GuestAddr p, std::uint64_t n, bool write) const {
    return permitted(p, n, write ? kProtWrite : kProtRead | kProtExec);
  }
};
}  // namespace venture
