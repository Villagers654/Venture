#pragma once
// Opt-in public API results only. Callers supply registry API literals, never
// arguments, handles, guest names, or payloads. Bounded storage survives thread exit.
#include "common/log.h"
#include "platform/profile.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <utility>

namespace venture::api_result_trace {
enum class State : unsigned { pending, returned, exception };
enum class Phase : unsigned { vulkan_fast, vulkan_slow, vulkan_delivery, vulkan_native, xr };
constexpr std::uint64_t api_id(const char* name) {
  std::uint64_t value = 14695981039346656037ull;
  for (; *name; ++name) value = (value ^ static_cast<unsigned char>(*name)) * 1099511628211ull;
  return value;
}
inline std::int64_t now() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Record {
  std::uint64_t sequence{}, api{};
  const char* name{};
  std::int64_t entry_ns{}, exit_ns{};
  std::int32_t result{};
  State state{};
  Phase phase{};
};
// Single producer per ring, concurrent terminal reader. Every field is atomic;
// seq_cst publication prevents a racing wrap/update from returning mixed fields.
// Readers never wait for a suspended/faulting writer and report unstable slots.
template<std::size_t Capacity> class Ring {
  static_assert(Capacity > 0);
  struct Slot {
    std::atomic<std::uint64_t> version{}, sequence{}, api{};
    std::atomic<const char*> name{};
    std::atomic<std::int64_t> entry{}, exit{};
    std::atomic<std::int32_t> result{};
    std::atomic<State> state{};
    std::atomic<Phase> phase{};
  };
  std::array<Slot, Capacity> slots_{};
  std::atomic<std::uint64_t> total_{};
 public:
  std::uint64_t begin(const char* name, Phase phase, std::int64_t time) noexcept {
    const auto sequence = total_.load() + 1;
    auto& s = slots_[(sequence - 1) % Capacity];
    s.version.fetch_add(1);
    s.sequence.store(sequence); s.api.store(api_id(name)); s.name.store(name);
    s.entry.store(time); s.exit.store(0); s.result.store(0);
    s.state.store(State::pending); s.phase.store(phase);
    s.version.fetch_add(1);
    total_.store(sequence);
    return sequence;
  }
  void finish(std::uint64_t sequence, State state, std::int32_t result, std::int64_t time) noexcept {
    auto& s = slots_[(sequence - 1) % Capacity];
    // A deeply nested call may have wrapped the ring while its parent was pending.
    if (s.sequence.load() != sequence) return;
    s.version.fetch_add(1);
    s.exit.store(time); s.result.store(result); s.state.store(state);
    s.version.fetch_add(1);
  }
  std::uint64_t total() const noexcept { return total_.load(); }
  bool read(std::uint64_t sequence, Record& out) const noexcept {
    if (!sequence) return false;
    const auto& s = slots_[(sequence - 1) % Capacity];
    const auto version = s.version.load();
    if (version & 1) return false;
    Record r{s.sequence.load(), s.api.load(), s.name.load(), s.entry.load(), s.exit.load(),
             s.result.load(), s.state.load(), s.phase.load()};
    if (s.version.load() != version || r.sequence != sequence) return false;
    out = r;
    return true;
  }
  template<class Visitor> std::uint64_t visit(Visitor&& visitor) const {
    const auto end = total();
    const auto first = end > Capacity ? end - Capacity : 0;
    std::uint64_t unstable = 0;
    for (auto i = first + 1; i <= end; ++i) {
      Record record;
      if (read(i, record)) visitor(record); else ++unstable;
    }
    return unstable;
  }
};
inline constexpr std::size_t thread_capacity = 256, record_capacity = 256;
struct Storage {
  std::array<Ring<record_capacity>, thread_capacity> threads{};
  std::array<std::atomic<std::uint64_t>, thread_capacity> thread_ids{};
  std::atomic<std::uint64_t> claimed{}, dropped_calls{};
};
inline Storage storage;
inline std::atomic<bool> armed{}, flushed{};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::int64_t>::is_always_lock_free);
static_assert(std::atomic<std::int32_t>::is_always_lock_free);
static_assert(std::atomic<const char*>::is_always_lock_free);
static_assert(std::atomic<State>::is_always_lock_free);
static_assert(std::atomic<Phase>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
// Read the host launch setting before main, not on the first API call. In
// particular the disabled path must not lazily allocate an environment snapshot
// or execute a function-local initialization guard on every dispatch.
inline const bool enabled_at_launch = [] {
  const auto* setting = std::getenv("VENTURE_API_RESULT_TRACE");
  return setting && setting[0] == '1' && setting[1] == '\0';
}();
inline bool enabled() noexcept { return enabled_at_launch; }
inline void flush() noexcept;
inline Ring<record_capacity>* thread_ring() noexcept {
  static const int registered = std::atexit(flush);
  (void)registered;
  armed.store(true);
  // Never recycle a departed thread's slot: its last failure remains retained.
  // Exhaustion is counted explicitly, without an unbounded thread registry.
  static thread_local const auto slot = [] {
    const auto index = storage.claimed.fetch_add(1);
    if (index < thread_capacity) storage.thread_ids[index].store(platform::profile_thread_id());
    return index;
  }();
  if (slot >= thread_capacity) { storage.dropped_calls.fetch_add(1); return nullptr; }
  return &storage.threads[slot];
}
struct NoCall { void finish(std::int32_t) const noexcept {} };
class Call {
  Ring<record_capacity>* ring_{};
  std::uint64_t sequence_{};
 public:
  Call(const char* api, Phase phase) noexcept : ring_(thread_ring()) {
    if (ring_) sequence_ = ring_->begin(api, phase, now());
  }
  Call(const Call&) = delete;
  ~Call() {
    if (ring_) ring_->finish(sequence_, State::exception, 0, now());
  }
  void finish(std::int32_t result) noexcept {
    if (ring_) { ring_->finish(sequence_, State::returned, result, now()); ring_ = nullptr; }
  }
};
// The disabled specialization invokes the same body with compile-time no-ops:
// one admission branch, no ring/TLS/clock/allocation work on the disabled path.
template<bool RegistryResult = true, class Body>
decltype(auto) observe(const char* api, Phase phase, Body&& body) {
  if constexpr (RegistryResult) {
    if (enabled()) { Call call(api, phase); return std::forward<Body>(body)(call); }
  }
  NoCall call;
  return std::forward<Body>(body)(call);
}
// No environment lookup, TLS initialization, locks, heap, streams, or formatter
// in the fatal host handler. All snapshot fields use always-lock-free atomics.
struct Line {
  char data[512]{};
  std::size_t size{};
  void text(const char* value) noexcept {
    while (*value && size < sizeof(data) - 1) data[size++] = *value++;
  }
  void number(std::uint64_t value) noexcept {
    char digits[20]; unsigned count = 0;
    do { digits[count++] = char('0' + value % 10); value /= 10; } while (value);
    while (count && size < sizeof(data) - 1) data[size++] = digits[--count];
  }
  void signed_number(std::int64_t value) noexcept {
    if (value < 0) { text("-"); number(0 - static_cast<std::uint64_t>(value)); }
    else number(static_cast<std::uint64_t>(value));
  }
  void field(const char* key, std::uint64_t value) noexcept { text(key); number(value); }
  void write() noexcept { text("\n"); log::crash_write(data, size); }
};
inline void crash_flush() noexcept {
  if (!armed.load() || flushed.exchange(true)) return;
  const auto claimed = storage.claimed.load();
  Line header;
  header.text("[api-result] snapshot"); header.field(" threads=", claimed);
  header.field(" thread_capacity=", thread_capacity); header.field(" record_capacity=", record_capacity);
  header.field(" dropped_calls=", storage.dropped_calls.load()); header.write();
  for (std::size_t i = 0; i < claimed && i < thread_capacity; ++i) {
    const auto& ring = storage.threads[i];
    const auto unstable = ring.visit([&](const Record& r) {
      Line line; line.text("[api-result]"); line.field(" thread_slot=", i);
      line.field(" seq=", r.sequence); line.field(" api_id=", r.api);
      // Name is a registry literal stored by the call site, not a guest pointer.
      line.text(" api="); line.text(r.name);
      line.field(" phase=", unsigned(r.phase)); line.field(" entry_ns=", r.entry_ns);
      line.field(" exit_ns=", r.exit_ns); line.field(" state=", unsigned(r.state));
      line.text(" result="); line.signed_number(r.result); line.write();
    });
    Line footer; footer.text("[api-result]"); footer.field(" thread_slot=", i);
    footer.field(" host_thread=", storage.thread_ids[i].load()); footer.field(" total=", ring.total());
    footer.field(" overwritten=", ring.total() > record_capacity ? ring.total() - record_capacity : 0);
    footer.field(" unstable=", unstable); footer.write();
  }
}
inline void flush() noexcept {
  if (enabled()) crash_flush();
}
} // namespace venture::api_result_trace
