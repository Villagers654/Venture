#include "core/stall_watch.h"
#include "core/thread.h"
#include "core/thread_lifetime.h"
#include "platform/profile.h"
#include <algorithm>
#include <atomic>
#include <chrono>

namespace venture::stall_watch {
namespace {
constexpr std::size_t kSlots = 512, kDepth = 4, kPolls = 4, kVulkanEvents = 256, kXrEvents = 64, kFences = 1024;
constexpr auto relaxed = std::memory_order_relaxed;

struct Entry {
  std::atomic<std::uint8_t> kind{0};
  std::atomic<const char*> name{nullptr};
  std::atomic<std::uint64_t> object{0};
  std::atomic<std::int64_t> since{0};
};
struct Poll {
  std::atomic<std::uint64_t> fence{0}, checks{0};
  std::atomic<std::int64_t> since{0};
  std::uint64_t used = 0;  // owner only: its last check, in the owner's check count
};
struct Slot {
  std::atomic<bool> taken{false};
  std::atomic<std::uint32_t> os_tid{0};
  std::atomic<std::int32_t> guest_tid{0};
  std::atomic<std::uint32_t> depth{0};
  std::array<Entry, kDepth> ops;
  std::array<Poll, kPolls> polls;
  std::uint64_t checks = 0;  // owner only
};
std::array<Slot, kSlots> slots;
std::atomic<std::size_t> used_slots{0};  // high-water mark

void clear(Slot& s) {
  s.depth.store(0, relaxed);
  for (auto& p : s.polls) {
    p.fence.store(0, relaxed);
    p.checks.store(0, relaxed);
    p.used = 0;
  }
  s.checks = 0;
}
// This thread's slot, released when the thread exits (after guest TLS
// destructors on adopted threads: core/thread_lifetime.h).
struct Owner {
  Slot* slot = nullptr;
  ~Owner() {
    if (!slot) return;
    clear(*slot);
    slot->guest_tid.store(0, relaxed);
    slot->taken.store(false, std::memory_order_release);
  }
};
Slot* mine() noexcept {
  auto& owner = thread_lifetime::local<Owner, struct StallWatchOwner>();
  if (owner.slot) [[likely]] {
    if (!owner.slot->guest_tid.load(relaxed))
      if (auto* t = GuestThread::current()) owner.slot->guest_tid.store(t->tid(), relaxed);
    return owner.slot;
  }
  for (std::size_t i = 0; i < kSlots; ++i) {
    auto& s = slots[i];
    if (s.taken.load(relaxed) || s.taken.exchange(true, std::memory_order_acquire)) continue;
    clear(s);
    s.os_tid.store(platform::profile_thread_id(), relaxed);
    auto* t = GuestThread::current();
    s.guest_tid.store(t ? t->tid() : 0, relaxed);
    auto high = used_slots.load(relaxed);
    while (high < i + 1 && !used_slots.compare_exchange_weak(high, i + 1, relaxed)) {}
    owner.slot = &s;
    return &s;
  }
  return nullptr;  // more than 512 threads at once: not watched
}

// Fixed rings of the last events, each entry guarded by its own sequence (a
// seqlock): readers skip entries rewritten while they copied them.
struct RingEntry {
  std::atomic<std::uint64_t> sequence{0};
  std::atomic<std::uint8_t> kind{0};
  std::atomic<const char*> name{nullptr};
  std::atomic<std::uint32_t> os_tid{0};
  std::atomic<std::uint64_t> queue{0}, object{0}, submit{0};
  std::atomic<std::int64_t> start{0}, ns{0};
  std::atomic<int> result{0};
};
template <std::size_t N> struct Ring {
  std::array<RingEntry, N> entries;
  std::atomic<std::uint64_t> head{0};
  void add(Event kind, const char* name, std::uint64_t queue, std::uint64_t object, std::uint64_t submit,
           std::int64_t start, std::int64_t ns, int result) noexcept {
    const auto ticket = head.fetch_add(1, relaxed);
    auto& e = entries[ticket % N];
    e.sequence.store(ticket * 2 + 1, relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    e.kind.store(std::uint8_t(kind), relaxed);
    e.name.store(name, relaxed);
    e.os_tid.store(platform::profile_thread_id(), relaxed);
    e.queue.store(queue, relaxed);
    e.object.store(object, relaxed);
    e.submit.store(submit, relaxed);
    e.start.store(start, relaxed);
    e.ns.store(ns, relaxed);
    e.result.store(result, relaxed);
    e.sequence.store(ticket * 2 + 2, std::memory_order_release);
  }
  std::vector<EventView> read() const {
    std::vector<EventView> out;
    const auto end = head.load(std::memory_order_acquire);
    for (auto ticket = end > N ? end - N : 0; ticket < end; ++ticket) {
      const auto& e = entries[ticket % N];
      if (e.sequence.load(std::memory_order_acquire) != ticket * 2 + 2) continue;
      EventView v{Event(e.kind.load(relaxed)), e.name.load(relaxed), e.os_tid.load(relaxed), e.queue.load(relaxed),
                  e.object.load(relaxed), e.submit.load(relaxed), e.start.load(relaxed), e.ns.load(relaxed),
                  e.result.load(relaxed)};
      std::atomic_thread_fence(std::memory_order_acquire);
      if (e.sequence.load(relaxed) == ticket * 2 + 2) out.push_back(v);
    }
    return out;
  }
};
Ring<kVulkanEvents> vulkan_ring;
Ring<kXrEvents> xr_ring;

// Fences by handle, direct-mapped: a collision replaces the older fence.
struct FenceEntry {
  std::atomic<std::uint64_t> sequence{0};
  std::atomic<std::uint64_t> fence{0}, queue{0}, submit{0};
  std::atomic<std::int64_t> submitted{0}, issued{0};
  std::atomic<bool> deferred{false};
};
std::array<FenceEntry, kFences> fences;
std::atomic<std::uint64_t> submissions{0};
FenceEntry& fence_entry(std::uint64_t fence) {
  auto h = fence * 0x9E3779B97F4A7C15ull;
  return fences[(h >> 32) % kFences];
}
void write_fence(FenceEntry& e, std::uint64_t fence, std::uint64_t queue, std::uint64_t submit, std::int64_t submitted,
                 std::int64_t issued, bool deferred) {
  const auto sequence = e.sequence.load(relaxed);
  e.sequence.store(sequence | 1, relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  e.fence.store(fence, relaxed);
  e.queue.store(queue, relaxed);
  e.submit.store(submit, relaxed);
  e.submitted.store(submitted, relaxed);
  e.issued.store(issued, relaxed);
  e.deferred.store(deferred, relaxed);
  e.sequence.store((sequence | 1) + 1, std::memory_order_release);
}

std::atomic<std::uint64_t> pipeline_count{0}, pipeline_wall{0}, upload_count{0}, gc_count{0};
std::atomic<std::int64_t> gc_since{0};
}  // namespace

std::int64_t now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

Op::Op(Kind kind, const char* name, std::uint64_t object) noexcept
    : slot_(nullptr), name_(name), start_(now_ns()), depth_(0), kind_(kind) {
  auto* s = mine();
  if (!s) return;
  slot_ = s;
  depth_ = s->depth.load(relaxed);
  if (depth_ < kDepth) {
    auto& e = s->ops[depth_];
    e.since.store(start_, relaxed);
    e.object.store(object, relaxed);
    e.name.store(name, relaxed);
    e.kind.store(std::uint8_t(kind), relaxed);
  }
  s->depth.store(depth_ + 1, std::memory_order_release);
}
void Op::finish(int result) noexcept {
  if (finished_) return;
  finished_ = true;
  if (kind_ == Kind::xr) xr_ring.add(Event::xr, name_, 0, 0, 0, start_, now_ns() - start_, result);
  // An outer call that ended first (an XR call's finish) already unwound this one.
  if (auto* s = static_cast<Slot*>(slot_); s && s->depth.load(relaxed) > depth_)
    s->depth.store(depth_, std::memory_order_release);
}
Op::~Op() { finish(0); }

std::uint64_t guest_submit(std::uint64_t queue, std::uint64_t fence, bool deferred) noexcept {
  const auto id = submissions.fetch_add(1, relaxed) + 1;
  if (fence) {
    const auto now = now_ns();
    write_fence(fence_entry(fence), fence, queue, id, now, deferred ? 0 : now, deferred);
  }
  return id;
}
std::uint64_t native_submit(std::uint64_t /*native queue*/, std::uint64_t fence) noexcept {
  if (!fence) return 0;
  auto& e = fence_entry(fence);
  // The dispatcher issuing a held submission: the same fence, not yet issued.
  if (e.fence.load(relaxed) != fence || !e.deferred.load(relaxed) || e.issued.load(relaxed)) return 0;
  const auto id = e.submit.load(relaxed);
  write_fence(e, fence, e.queue.load(relaxed), id, e.submitted.load(relaxed), now_ns(), true);
  return id;
}

void fence_checked(std::uint64_t fence, bool signalled) noexcept {
  if (!fence) return;
  auto* s = mine();
  if (!s) return;
  ++s->checks;
  Poll* oldest = &s->polls[0];
  for (auto& p : s->polls) {
    if (p.fence.load(relaxed) == fence) {
      if (signalled) {
        p.fence.store(0, relaxed);
        p.checks.store(0, relaxed);
        return;
      }
      p.checks.store(p.checks.load(relaxed) + 1, relaxed);
      p.used = s->checks;
      return;
    }
    if (p.used < oldest->used) oldest = &p;
  }
  if (signalled) return;
  // A new unsignalled fence takes the least recently checked entry.
  oldest->fence.store(0, relaxed);
  oldest->since.store(now_ns(), relaxed);
  oldest->checks.store(1, relaxed);
  oldest->used = s->checks;
  oldest->fence.store(fence, std::memory_order_release);
}

void event(Event kind, const char* name, std::uint64_t queue, std::uint64_t object, std::uint64_t submit,
           std::int64_t start_ns, std::int64_t ns, int result) noexcept {
  if (kind == Event::xr) xr_ring.add(kind, name, queue, object, submit, start_ns, ns, result);
  else vulkan_ring.add(kind, name, queue, object, submit, start_ns, ns, result);
}

void pipelines(std::uint32_t count, std::uint64_t wall_ns) noexcept {
  pipeline_count.fetch_add(count, relaxed);
  pipeline_wall.fetch_add(wall_ns, relaxed);
}
void texture_upload() noexcept { upload_count.fetch_add(1, relaxed); }
void gc_began() noexcept {
  gc_count.fetch_add(1, relaxed);
  gc_since.store(now_ns(), relaxed);
}
void gc_ended() noexcept { gc_since.store(0, relaxed); }
Totals totals() noexcept {
  return {pipeline_count.load(relaxed), pipeline_wall.load(relaxed), upload_count.load(relaxed), gc_count.load(relaxed),
          gc_since.load(relaxed)};
}

std::vector<ThreadView> threads() {
  std::vector<ThreadView> out;
  const auto n = std::min(used_slots.load(std::memory_order_acquire), kSlots);
  for (std::size_t i = 0; i < n; ++i) {
    const auto& s = slots[i];
    if (!s.taken.load(std::memory_order_acquire)) continue;
    ThreadView t{s.os_tid.load(relaxed), s.guest_tid.load(relaxed), {}, {}};
    const auto depth = std::min<std::size_t>(s.depth.load(std::memory_order_acquire), kDepth);
    for (std::size_t d = 0; d < depth; ++d) {
      const auto& e = s.ops[d];
      t.ops.push_back({Kind(e.kind.load(relaxed)), e.name.load(relaxed), e.object.load(relaxed), e.since.load(relaxed)});
    }
    for (const auto& p : s.polls)
      if (const auto f = p.fence.load(std::memory_order_acquire))
        t.polls.push_back({f, p.checks.load(relaxed), p.since.load(relaxed)});
    if (!t.ops.empty() || !t.polls.empty()) out.push_back(std::move(t));
  }
  return out;
}

Longest longest() noexcept {
  Longest best;
  const auto n = std::min(used_slots.load(std::memory_order_acquire), kSlots);
  for (std::size_t i = 0; i < n; ++i) {
    const auto& s = slots[i];
    if (!s.taken.load(relaxed) || !s.depth.load(std::memory_order_acquire)) continue;
    const auto& e = s.ops[0];
    const auto since = e.since.load(relaxed);
    if (!since || (best.since_ns && since >= best.since_ns)) continue;
    best = {since, s.os_tid.load(relaxed), s.guest_tid.load(relaxed), Kind(e.kind.load(relaxed)), e.name.load(relaxed),
            e.object.load(relaxed)};
  }
  return best;
}

FenceView fence(std::uint64_t handle) noexcept {
  const auto& e = fence_entry(handle);
  for (int attempt = 0; attempt < 4; ++attempt) {
    const auto before = e.sequence.load(std::memory_order_acquire);
    if (before & 1) continue;
    FenceView v{e.fence.load(relaxed) == handle, e.queue.load(relaxed), e.submit.load(relaxed),
                e.submitted.load(relaxed), e.issued.load(relaxed), e.deferred.load(relaxed)};
    std::atomic_thread_fence(std::memory_order_acquire);
    if (e.sequence.load(relaxed) == before) return v.known ? v : FenceView{};
  }
  return {};
}

std::vector<EventView> vulkan_events() { return vulkan_ring.read(); }
std::vector<EventView> xr_calls() { return xr_ring.read(); }

const char* kind_name(Kind kind) noexcept {
  switch (kind) {
    case Kind::xr: return "xr";
    case Kind::submit: return "submit";
    case Kind::wait: return "wait";
    case Kind::present: return "present";
    case Kind::idle: return "idle";
    default: return "none";
  }
}

void reset_for_testing() noexcept {
  for (auto& e : fences) write_fence(e, 0, 0, 0, 0, 0, false);
  for (auto& e : vulkan_ring.entries) e.sequence.store(0, relaxed);
  for (auto& e : xr_ring.entries) e.sequence.store(0, relaxed);
  vulkan_ring.head.store(0, relaxed);
  xr_ring.head.store(0, relaxed);
  for (auto& s : slots)
    if (s.taken.load(relaxed)) clear(s);
}
}  // namespace venture::stall_watch
