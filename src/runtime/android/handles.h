#pragma once
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "core/guest.h"
#include "android/handles32.h"

namespace venture::android {
// Opaque NDK handles are checked by membership, before dereferencing. Shared
// ownership pins an object during an HLE call racing with close/release.
template <class T>
class Handles {
 public:
  explicit Handles(std::string_view a32_tag = {}) : a32_tag_(a32_tag) {}
  template <class... A>
  T* create(A&&... args) {
    auto value = std::make_shared<T>(std::forward<A>(args)...);
    std::lock_guard lock(mutex_);
    auto* p = value.get();
    handles32::publish(value, a32_tag_);
    values_.emplace(h2g(p), std::move(value));
    return p;
  }
  std::shared_ptr<T> get(const T* ptr) { return get(h2g(ptr)); }
  std::shared_ptr<T> get(GuestAddr address) {
    std::lock_guard lock(mutex_);
    auto it = values_.find(address);
    return it == values_.end() ? nullptr : it->second;
  }
  void erase(const T* ptr) {
    std::shared_ptr<T> dying;
    {
      std::lock_guard lock(mutex_);
      auto it = values_.find(h2g(ptr));
      if (it != values_.end()) {
        dying = std::move(it->second);
        values_.erase(it);
      }
    }
    if (dying) handles32::retire(h2g(ptr));
  }
  std::vector<std::shared_ptr<T>> snapshot() {
    std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<T>> result;
    result.reserve(values_.size());
    for (const auto& [address, value] : values_) result.push_back(value);
    return result;
  }

 private:
  std::string_view a32_tag_;
  std::mutex mutex_;
  std::unordered_map<GuestAddr, std::shared_ptr<T>> values_;
};
}  // namespace venture::android
