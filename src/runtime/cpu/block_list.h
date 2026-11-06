#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include "core/guest.h"
namespace Dynarmic { struct CompileTiming; }

namespace venture {
// Default-off descriptor hints, one file per guest thread name. A named thread's
// earlier blocks are loaded off-thread and installed from the portable block cache
// while the thread waits. Never contains host code or guest data.
class JitBlockList {
 public:
  JitBlockList() = default;
  JitBlockList(JitBlockList&&) noexcept = default;
  JitBlockList& operator=(JitBlockList&&) noexcept = default;
  JitBlockList(const JitBlockList&) = delete;
  JitBlockList& operator=(const JitBlockList&) = delete;
  static bool enabled();
  // Owner thread: the name selects the hint file; its hints load on the background worker.
  void name(std::string_view thread_name, bool a32) noexcept;
  void record(std::uint64_t descriptor) noexcept;
  // The next loaded hint, oldest first; false until the load completes and once exhausted.
  bool next_hint(std::uint64_t& descriptor) noexcept;
  bool hints_pending() const noexcept { return queued_next_ < queued_.size() || (loading_ && loading_->ready.load(std::memory_order_acquire)); }
  std::size_t pending_hint_count() const noexcept {
    return queued_.size() - queued_next_ + (loading_ && loading_->ready.load(std::memory_order_acquire) ? loading_->descriptors.size() : 0);
  }
  // A copy for the background writer, only after new records: 5 s after a burst, else every 30 s.
  bool snapshot_due(std::int64_t now_ns) const noexcept;
  JitBlockList snapshot(std::int64_t now_ns);
  // Merges and writes now, under the directory's cross-process lock.
  void save() noexcept;
  // Queues a list for the background writer; never writes on the caller.
  static void retire(JitBlockList&& list) noexcept;
  // Waits until every queued load and save has finished (tests).
  static void flush() noexcept;
  struct Scope {
    explicit Scope(JitBlockList* list) noexcept;
    ~Scope();
    JitBlockList* previous;
  };
  static void compiled(std::uint64_t descriptor) noexcept;
  static void compilation(const Dynarmic::CompileTiming& timing) noexcept;
 private:
  friend struct JitBlockListTestAccess;
  struct Loading {
    std::atomic<bool> ready{false};
    std::vector<std::uint64_t> descriptors;
  };
  static std::mutex& persistence_mutex_for_test();
  static void snapshot_observer_for_test(void (*observer)());
  bool a32_ = false;
  std::filesystem::path file_;
  std::vector<std::uint64_t> descriptors_;
  std::size_t next_ = 0;
  std::uint64_t recorded_ = 0, snapshotted_ = 0;
  std::int64_t snapshot_ns_ = 0;
  std::shared_ptr<Loading> loading_;
  std::vector<std::uint64_t> queued_;
  std::size_t queued_next_ = 0;
};
} // namespace venture
