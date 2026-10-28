#pragma once
#include "core/guest.h"
#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace venture {
class GuestThread;
namespace shutdown_trace {
extern const bool enabled;
enum class Wait { none, futex, sleep, condition, join };
struct Record {
  GuestAddr pc{}, lr{}, fp{}, sp{}, object{}, associated_mutex{}, futex{};
  std::array<GuestAddr, 10> saved{};  // x19-x28
  std::uint32_t last_hle{}, expected{};
  std::int32_t target_tid{};
  std::int64_t sampled_ns{};
  bool entered{};
  Wait wait = Wait::none;
};
struct State { std::mutex mutex; Record record; };
// Only the owning thread reads Cpu registers. Observers read a locked copy.
void publish(GuestThread& thread);
Record read(GuestThread& thread);
class HleScope {
 public:
  HleScope(GuestThread& thread, std::uint32_t id);
  ~HleScope();
 private:
  GuestThread& thread_;
  std::uint32_t previous_{};
  bool nested_{};
};
class WaitScope {
 public:
  WaitScope(GuestThread& thread, Wait kind, GuestAddr object, GuestAddr mutex = 0, std::int32_t target = 0);
  ~WaitScope();
 private:
  GuestThread& thread_;
  Record previous_;
};
class FutexScope {
 public:
  FutexScope(GuestThread& thread, GuestAddr address, std::uint32_t expected, bool sleeping);
  ~FutexScope();
 private:
  GuestThread& thread_;
  Record previous_;
};
void phase(const char* name);
void begin();
void stop();
void snapshot(const char* label);
// Optional subsystems register read-only evidence sources; core never calls
// guest code or holds the guest registry while invoking these sources.
void add_snapshot_source(void (*source)(const char*));
struct WatchLocation { std::string module; GuestAddr offset{}; };
std::optional<WatchLocation> parse_watch_location(std::string_view text);
// Explicit developer-selected word only; disabled without snapshots and a watch.
// Safe OS memory reads, no suspension, guest calls, or loader-lock waits.
void sample_watch(std::string_view step) noexcept;
void trace_destructor(std::string_view operation, GuestAddr callback, GuestAddr dso) noexcept;
}
}
