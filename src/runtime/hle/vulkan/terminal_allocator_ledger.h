#pragma once
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>

namespace venture::vulkan {
// Allocation ownership across the terminal boundary. No guest callback executes under mutex_.
// A move-only reservation removes an old generation BEFORE free/realloc and
// owns its map node until completion. A delayed completion never erases by
// address, which may already identify a newly allocated generation.
class TerminalLedger {
  struct Entry { std::size_t size, alignment; bool host; };
  using Map = std::unordered_map<void*, Entry>;
  mutable std::mutex mutex_;
  Map entries_;
  bool terminal_ = false, healthy_ = true;
  std::size_t pending_ = 0, host_frees_ = 0;
  void release_known(Map::iterator it) noexcept {
    if (it->second.host) {
      ::operator delete(it->first, std::align_val_t(it->second.alignment));
      ++host_frees_;
    }
    entries_.erase(it); // Guest memory remains owned by the stopped process.
  }
  void* allocate_locked(std::size_t size, std::size_t alignment, bool stopped) noexcept {
    if (!(terminal_ || stopped) || !healthy_ || !std::has_single_bit(alignment) ||
        size > std::numeric_limits<std::size_t>::max() - alignment) return nullptr;
    alignment = std::max(alignment, alignof(std::max_align_t));
    void* result = ::operator new(std::max<std::size_t>(size, 1),
                                 std::align_val_t(alignment), std::nothrow);
    if (!result) return nullptr;
    try {
      if (!entries_.emplace(result, Entry{size, alignment, true}).second) {
        // A collision violates ownership. Do not free an ambiguous pointer.
        healthy_ = false;
        return nullptr;
      }
    } catch (...) {
      ::operator delete(result, std::align_val_t(alignment));
      healthy_ = false;
      return nullptr;
    }
    return result;
  }
 public:
  class Operation {
    friend class TerminalLedger;
    TerminalLedger* owner_ = nullptr;
    Map::node_type previous_;
    explicit Operation(TerminalLedger* owner) : owner_(owner) {}
   public:
    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;
    Operation(Operation&& other) noexcept
      : owner_(std::exchange(other.owner_, nullptr)), previous_(std::move(other.previous_)) {}
    ~Operation() {
      if (owner_) {
        std::lock_guard lock(owner_->mutex_);
        owner_->healthy_ = false; // Missing completion must not permit clean quit.
        --owner_->pending_;
      }
    }
  };
  TerminalLedger() = default;
  TerminalLedger(const TerminalLedger&) = delete;
  ~TerminalLedger() {
    // Caller must retain allocator storage until all native users have ended.
    for (const auto& [p, e] : entries_)
      if (e.host) ::operator delete(p, std::align_val_t(e.alignment));
  }
  bool healthy() const { std::lock_guard lock(mutex_); return healthy_; }
  std::size_t host_frees() const { std::lock_guard lock(mutex_); return host_frees_; }
  std::size_t pending() const { std::lock_guard lock(mutex_); return pending_; }
  // old is nonnull only for free/realloc; all callbacks, including allocation,
  // reserve through their complete bookkeeping, not merely guest execution.
  Operation begin(void* old = nullptr) noexcept {
    std::lock_guard lock(mutex_);
    Operation operation(this);
    ++pending_;
    if (terminal_) healthy_ = false;
    if (old) {
      operation.previous_ = entries_.extract(old);
      if (operation.previous_.empty()) healthy_ = false;
    }
    return operation;
  }
  bool complete(Operation&& op, void* result, std::size_t size,
                std::size_t alignment, bool released = false,
                bool callback_completed = true) noexcept {
    std::lock_guard lock(mutex_);
    if (op.owner_ != this) return healthy_ = false;
    op.owner_ = nullptr;
    --pending_;
    if (terminal_) return healthy_ = false;
    try {
      // A callback suppressed before entry did not change guest ownership.
      // The caller separately poisons any exception after entry. Failed
      // nonzero realloc likewise retains its original generation. Node insert
      // checks ownership and never overwrites a newer generation.
      if (!callback_completed || (!released && !result && size)) {
        if (!op.previous_.empty() && !entries_.insert(std::move(op.previous_)).inserted)
          healthy_ = false;
      } else if (!released && result) {
        if (!entries_.emplace(result, Entry{size, alignment, false}).second)
          healthy_ = false;
      }
    } catch (...) { healthy_ = false; }
    return healthy_;
  }
  bool seal(bool caller_proved_quiescence) noexcept {
    std::lock_guard lock(mutex_);
    if (!caller_proved_quiescence || pending_ || !healthy_ || terminal_) return false;
    terminal_ = true;
    return true;
  }
  // Host ownership applies once sealed, or earlier when the caller proves guest
  // execution has stopped: a stopped guest allocator cannot serve native calls
  // (a deferred submission pumped during OS quit failed OUT_OF_HOST_MEMORY on
  // Climb 2). Normal operations still in flight complete under their reservation.
  void* allocate(std::size_t size, std::size_t alignment, bool stopped = false) noexcept {
    std::lock_guard lock(mutex_);
    return allocate_locked(size, alignment, stopped);
  }
  bool release(void* pointer, bool stopped = false) noexcept {
    std::lock_guard lock(mutex_);
    if (!(terminal_ || stopped) || !healthy_) return false;
    if (!pointer) return true;
    auto it = entries_.find(pointer);
    if (it == entries_.end()) return healthy_ = false;
    release_known(it);
    return true;
  }
  void* reallocate(void* old, std::size_t size, std::size_t alignment, bool stopped = false) noexcept {
    if (!old) return allocate(size, alignment, stopped);
    std::lock_guard lock(mutex_);
    if (!(terminal_ || stopped) || !healthy_) return nullptr;
    auto it = entries_.find(old);
    if (it == entries_.end()) { healthy_ = false; return nullptr; }
    if (!size) { release_known(it); return nullptr; }
    const auto old_size = it->second.size;
    void* result = allocate_locked(size, alignment, stopped);
    if (!result) return nullptr;
    std::memcpy(result, old, std::min(old_size, size));
    release_known(entries_.find(old)); // insertion may have rehashed
    return result;
  }
};

} // namespace venture::vulkan
