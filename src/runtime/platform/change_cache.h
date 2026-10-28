#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>

namespace venture::platform {
// Keeps the last answer of a host query until the host reports a change.
// invalidate() comes from OS change notifications on arbitrary threads. A query
// that races with a notification publishes under its starting generation, so
// the next call queries again. Failed queries are never cached. Until enable(),
// every call queries the host.
template <class T>
class ChangeCache {
 public:
  void invalidate() noexcept { generation_.fetch_add(1, std::memory_order_acq_rel); }
  void enable() noexcept { enabled_.store(true, std::memory_order_release); }
  template <class Query>
  T get(Query&& query) {
    const auto generation = generation_.load(std::memory_order_acquire);
    const bool enabled = enabled_.load(std::memory_order_acquire);
    if (enabled) {
      std::lock_guard lock(mutex_);
      if (valid_ && cached_generation_ == generation) return value_;
    }
    T value = query();
    if (enabled) {
      std::lock_guard lock(mutex_);
      if (!valid_ || generation >= cached_generation_) {
        value_ = value;
        cached_generation_ = generation;
        valid_ = true;
      }
    }
    return value;
  }

 private:
  std::atomic<std::uint64_t> generation_{0};
  std::atomic<bool> enabled_{false};
  std::mutex mutex_;
  bool valid_ = false;
  std::uint64_t cached_generation_ = 0;
  T value_{};
};

// A host query answered from ChangeCache. watch(changed, context) registers
// every change source and returns true only when all of them are live; the
// sources then call changed(context) from any thread. Otherwise every get()
// queries the host. Registration and query are injectable for tests.
template <class T>
class WatchedQuery {
 public:
  using Changed = void (*)(void*);
  WatchedQuery(const std::function<bool(Changed, void*)>& watch, std::function<T()> query)
      : query_(std::move(query)) {
    if (watch(&changed, this)) cache_.enable();
  }
  WatchedQuery(const WatchedQuery&) = delete;
  WatchedQuery& operator=(const WatchedQuery&) = delete;
  T get() { return cache_.get(query_); }

 private:
  static void changed(void* self) noexcept { static_cast<WatchedQuery*>(self)->cache_.invalidate(); }
  ChangeCache<T> cache_;
  std::function<T()> query_;
};
}  // namespace venture::platform
