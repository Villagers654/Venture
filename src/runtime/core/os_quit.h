#pragma once
#include "common/environment.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string_view>

namespace venture::os_quit {
// External/menu quit as Quest task removal; VENTURE_MENU_QUIT=0 opts out.
// Ordinary app-requested exit keeps its existing exit_group path, including
// the first requested status.
inline const bool enabled = [] {
  const auto* value = environment::getenv("VENTURE_MENU_QUIT");
  return !value || std::string_view(value) != "0";
}();
inline std::atomic<bool> stopping{false};
inline std::atomic<unsigned> active_calls{0};
inline std::atomic<unsigned> active_writes{0};
inline std::atomic<std::int64_t> last_write_ns{0};
inline std::atomic<bool> host_requested{false};
inline std::atomic<unsigned> native_destroy_count{0};
inline std::atomic<std::int64_t> native_destroy_ns{0};
inline std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline void native_destroy_delivered() {
  if (!enabled || !host_requested.load()) return;
  native_destroy_ns.store(now_ns());
  ++native_destroy_count;
}
struct DiskWrite {
  bool active;
  explicit DiskWrite(bool writing = true) : active(enabled && writing) {
    if (active) ++active_writes;
  }
  ~DiskWrite() {
    if (active) { last_write_ns.store(now_ns()); --active_writes; }
  }
};
inline bool requested() { return enabled && stopping.load(std::memory_order_acquire); }
using ThreadCleanup = bool (*)() noexcept;
inline std::atomic<ThreadCleanup> thread_cleanup{nullptr};
inline std::atomic<bool> cleanup_failed{false};
inline thread_local unsigned call_depth = 0;
inline thread_local bool cleaning_thread = false;
struct Call {
  std::atomic<unsigned>* owner;
  explicit Call(std::atomic<unsigned>* depth = nullptr) : owner(depth) {
    if (enabled) { ++active_calls; ++call_depth; if (owner) ++*owner; }
  }
  ~Call() {
    if (!enabled) return;
    if (--call_depth == 0 && requested() && !cleaning_thread)
      if (auto cleanup = thread_cleanup.load()) {
        cleaning_thread = true;
        if (!cleanup()) cleanup_failed.store(true);
        cleaning_thread = false;
      }
    if (owner) --*owner;
    --active_calls;
  }
};
// Stop translated execution at its existing interruptible boundaries. No guest
// TLS destructors or per-thread allocator collection belong to an OS process
// end. The caller must bound the drain, classify any remaining host calls as
// failure, then close host resources before kernel termination.
void stop_guest_execution();
}  // namespace venture::os_quit
