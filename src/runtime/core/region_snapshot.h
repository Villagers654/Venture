#pragma once
#include "core/address_space.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

namespace venture {
// RCU-style immutable permissions snapshot. Readers use only lock-free atomics,
// including in JIT fault callbacks. Writers hold their address-space mutex.
class RegionSnapshot {
  struct Range {
    GuestAddr begin, end;
    std::uint32_t prot;
  };
  using Ranges = std::vector<Range>;
  static_assert(std::atomic<const Ranges*>::is_always_lock_free);
  static_assert(std::atomic<unsigned>::is_always_lock_free);
  mutable std::atomic<unsigned> readers_{0};
  std::atomic<const Ranges*> current_{nullptr};
  std::unique_ptr<Ranges> owner_;
  std::vector<std::unique_ptr<Ranges>> retired_;

  void install(std::unique_ptr<Ranges> next) {
    // Seq-cst reader admission, publication and retirement ensure a reader
    // that can see an old pointer is counted before it can be reclaimed.
    retired_.reserve(retired_.size() + 1);
    if (owner_) retired_.push_back(std::move(owner_));
    current_.store(next.get());
    owner_ = std::move(next);
    if (!readers_.load()) retired_.clear();
  }

 public:
  template <class Map>
  void publish(const Map& regions) {
    auto next = std::make_unique<Ranges>();
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
    auto next = std::make_unique<Ranges>();
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
    readers_.fetch_add(1);
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
    readers_.fetch_sub(1);
    return result;
  }
  bool accessible(GuestAddr p, std::uint64_t n, bool write) const {
    return permitted(p, n, write ? kProtWrite : kProtRead | kProtExec);
  }
};
}  // namespace venture
