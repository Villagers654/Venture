#pragma once
#include <atomic>
#include <mutex>
#include <source_location>
#include "platform/profile.h"

namespace venture {
// Owner metadata never needs the lock being diagnosed. Keep the outermost
// acquisition site across recursion; readers make only bounded atomic attempts.
class RegistryMutex {
 public:
  struct Owner {
    bool stable = false;
    std::uint32_t tid = 0, line = 0;
    const char* file = "unknown";
    const char* function = "unknown";
  };
  explicit RegistryMutex(const bool& enabled) : enabled_(enabled) {}
  void lock(std::source_location site = std::source_location::current()) {
    mutex_.lock(); acquired(site);
  }
  bool try_lock(std::source_location site = std::source_location::current()) {
    if (!mutex_.try_lock()) return false;
    acquired(site); return true;
  }
  void unlock() {
    if (enabled_ && !--depth_) {
      sequence_.fetch_add(1, std::memory_order_acq_rel);
      tid_.store(0, std::memory_order_relaxed);
      sequence_.fetch_add(1, std::memory_order_release);
    }
    mutex_.unlock();
  }
  Owner owner() const {
    if (!enabled_) return {};
    for (int attempt = 0; attempt < 8; ++attempt) {
      const auto first = sequence_.load(std::memory_order_acquire);
      if (first & 1) continue;
      Owner out{true, tid_.load(std::memory_order_relaxed), line_.load(std::memory_order_relaxed),
                file_.load(std::memory_order_relaxed), function_.load(std::memory_order_relaxed)};
      std::atomic_thread_fence(std::memory_order_acquire);
      if (first == sequence_.load(std::memory_order_relaxed)) return out;
    }
    return {};
  }
 private:
  void acquired(std::source_location site) {
    if (!enabled_ || depth_++) return;
    sequence_.fetch_add(1, std::memory_order_acq_rel);
    tid_.store(platform::profile_thread_id(), std::memory_order_relaxed);
    line_.store(site.line(), std::memory_order_relaxed);
    file_.store(site.file_name(), std::memory_order_relaxed);
    function_.store(site.function_name(), std::memory_order_relaxed);
    sequence_.fetch_add(1, std::memory_order_release);
  }
  const bool& enabled_;
  std::recursive_mutex mutex_;
  unsigned depth_ = 0;
  std::atomic<unsigned> sequence_{0};
  std::atomic<std::uint32_t> tid_{0}, line_{0};
  std::atomic<const char*> file_{"unknown"}, function_{"unknown"};
};
class RegistryLock : public std::unique_lock<RegistryMutex> {
  using Base = std::unique_lock<RegistryMutex>;
 public:
  explicit RegistryLock(RegistryMutex& mutex, std::source_location site = std::source_location::current())
      : Base(mutex, std::defer_lock), site_(site) { lock(); }
  RegistryLock(RegistryMutex& mutex, std::try_to_lock_t,
               std::source_location site = std::source_location::current())
      : Base(mutex, std::defer_lock), site_(site) {
    if (mutex.try_lock(site)) Base::operator=(Base(mutex, std::adopt_lock));
  }
  void lock() {
    auto& target = *Base::mutex(); target.lock(site_);
    Base::operator=(Base(target, std::adopt_lock));
  }
 private:
  std::source_location site_;
};
}
