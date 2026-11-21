#pragma once
// Stall watch: always-on bookkeeping the stall finder (core/stall_finder.h)
// reads when a game freezes, and the session timeline's per-minute counters.
// Writers are the game's and Venture's own threads; they only store to
// per-thread slots and fixed rings with relaxed atomics (one clock read per
// blocking call they bracket), never lock, allocate or log. Readers (the
// supervisor, once a stall is under way) copy what they need and tolerate a
// torn entry: everything here is evidence for a report, never a decision.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace venture::stall_watch {
std::int64_t now_ns() noexcept;  // steady clock

// A blocking call in progress on this thread: XR frame calls, Vulkan submits,
// fence and semaphore waits, presents, queue and device idles. Nests up to
// four deep (guest xrEndFrame > host xrEndFrame > vkQueueSubmit).
enum class Kind : std::uint8_t { none, xr, submit, wait, present, idle };
class Op {
 public:
  Op(Kind kind, const char* name, std::uint64_t object = 0) noexcept;
  ~Op();
  Op(const Op&) = delete;
  Op& operator=(const Op&) = delete;
  // Ends the call early with its result (XR calls; the destructor otherwise).
  void finish(int result) noexcept;
  std::int64_t started() const noexcept { return start_; }

 private:
  void* slot_;
  const char* name_;
  std::int64_t start_;
  std::uint32_t depth_;
  Kind kind_;
  bool finished_ = false;
};

// Vulkan fences, by handle value. guest_submit: the game submitted work that
// signals `fence` (0: none) on `queue`; deferred when Venture's queue
// dispatcher holds it back until its waits are issued. native_submit: the
// dispatcher gave a held submission to the driver (matched by its fence).
// Both return the guest submission's id (1, 2, ...; 0 when none matches).
std::uint64_t guest_submit(std::uint64_t queue, std::uint64_t fence, bool deferred) noexcept;
std::uint64_t native_submit(std::uint64_t queue, std::uint64_t fence) noexcept;
// A status check (vkGetFenceStatus, or a wait that timed out) found `fence`
// unsignalled, or signalled. Repeated checks of one fence by one thread cost a
// compare and an add; only a new fence reads the clock.
void fence_checked(std::uint64_t fence, bool signalled) noexcept;

// The last Vulkan submits and waits (waits only when they blocked >= 1 ms or
// timed out) and the last XR frame calls, with their timings.
enum class Event : std::uint8_t { submit, wait, present, idle, xr };
void event(Event kind, const char* name, std::uint64_t queue, std::uint64_t object, std::uint64_t submit,
           std::int64_t start_ns, std::int64_t ns, int result) noexcept;

// Counters for the session timeline.
void pipelines(std::uint32_t count, std::uint64_t wall_ns) noexcept;
void texture_upload() noexcept;
void gc_began() noexcept;  // JVMTI GC events (at a safepoint: atomics only)
void gc_ended() noexcept;
struct Totals {
  std::uint64_t pipelines = 0, pipeline_ns = 0, uploads = 0, gcs = 0;
  std::int64_t gc_since_ns = 0;  // the collection running now began then; 0 none
};
Totals totals() noexcept;

// ---- readers ----------------------------------------------------------
struct OpView {
  Kind kind = Kind::none;
  const char* name = nullptr;
  std::uint64_t object = 0;
  std::int64_t since_ns = 0;
};
struct PollView {
  std::uint64_t fence = 0, checks = 0;
  std::int64_t since_ns = 0;  // first unsignalled check of this run
};
struct ThreadView {
  std::uint32_t os_tid = 0;
  std::int32_t guest_tid = 0;  // 0: a host thread
  std::vector<OpView> ops;     // outermost first
  std::vector<PollView> polls;
};
// Every thread with a call in progress or a fence it keeps finding unsignalled.
std::vector<ThreadView> threads();
// The longest call in progress (its outermost entry) for the watchdog: no
// allocation, a scan of the used slots.
struct Longest {
  std::int64_t since_ns = 0;  // 0: none
  std::uint32_t os_tid = 0;
  std::int32_t guest_tid = 0;
  Kind kind = Kind::none;
  const char* name = nullptr;
  std::uint64_t object = 0;
};
Longest longest() noexcept;
struct FenceView {
  bool known = false;     // a submission of it was seen (the table keeps the last 1024 fences)
  std::uint64_t queue = 0, submit = 0;
  std::int64_t submitted_ns = 0, issued_ns = 0;  // issued: reached the driver (0: not yet)
  bool deferred = false;
};
FenceView fence(std::uint64_t fence) noexcept;
struct EventView {
  Event kind{};
  const char* name = nullptr;
  std::uint32_t os_tid = 0;
  std::uint64_t queue = 0, object = 0, submit = 0;
  std::int64_t start_ns = 0, ns = 0;
  int result = 0;
};
std::vector<EventView> vulkan_events();  // oldest first
std::vector<EventView> xr_calls();
const char* kind_name(Kind) noexcept;

// Tests: forget everything recorded (threads keep their slots).
void reset_for_testing() noexcept;
}  // namespace venture::stall_watch
