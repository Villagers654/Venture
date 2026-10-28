#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <vector>
#include "core/guest.h"
namespace Dynarmic { struct CompileTiming; }

namespace venture {
// Default-off descriptor hints. Never contains host code or guest data.
class JitBlockList {
 public:
  JitBlockList() = default;
  JitBlockList(JitBlockList&&) noexcept = default;
  JitBlockList& operator=(JitBlockList&&) noexcept = default;
  JitBlockList(const JitBlockList&) = delete;
  JitBlockList& operator=(const JitBlockList&) = delete;
  static bool enabled();
  bool prepared() const { return prepared_; }
  void prepare(GuestAddr entry, bool a32, const std::function<bool(std::uint64_t)>& compile);
  void record(std::uint64_t descriptor) noexcept;
  // Merges and writes now, under the directory's cross-process lock.
  void save() noexcept;
  // Queues a retired list for the background writer; never writes on the caller.
  static void retire(JitBlockList&& list) noexcept;
  // Waits until every queued list has been written (tests).
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
  static std::mutex& persistence_mutex_for_test();
  static void snapshot_observer_for_test(void (*observer)());
  bool prepared_ = false, a32_ = false;
  std::filesystem::path file_;
  std::vector<std::uint64_t> descriptors_;
  std::size_t next_ = 0;
};
} // namespace venture
