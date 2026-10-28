#include "android/errors.h"
#include "android/looper.h"
#include "android/libc_bridge.h"
#include "android/handles.h"
#include "core/thread.h"
#include "core/address_space.h"
#include "core/wait.h"
#include "core/shutdown_trace.h"
#include "core/readiness.h"
#include "core/a32_types.h"
#include "common/log.h"
#include <algorithm>
#include <climits>
#include <utility>

namespace venture::android {
namespace {
Handles<Looper>& loopers() {
  static auto* table = new Handles<Looper>("ALooper");
  return *table;
}
struct ThreadLooper {
  Looper* ptr = nullptr;
  ~ThreadLooper() {
    if (ptr) release_looper(ptr);
  }
};
thread_local ThreadLooper local;
std::mutex lifetime_mutex;
GuestAddr fd_key(int fd) { return 0x8000000000000000ull | static_cast<std::uint32_t>(fd); }
// AOSP looper.h event bits are NOT identical to Linux POLL* bits.
short poll_events(int e) { return static_cast<short>((e & 1 ? 1 : 0) | (e & 2 ? 4 : 0)); }
int looper_events(short e) {
  return (e & 1 ? 1 : 0) | (e & 4 ? 2 : 0) | (e & 8 ? 4 : 0) | (e & 16 ? 8 : 0) | (e & 32 ? 16 : 0);
}
void snapshot_loopers(const char* label) {
  for (auto& looper : loopers().snapshot()) {
    auto d = looper->diagnostic();
    if (!d) { log::info("shutdown-looper", "snapshot={} state=busy", label); continue; }
    log::info("shutdown-looper", "snapshot={} sequence={:#x} guest_tid={} host_tid={} cpu_available={} cpu_ns={} calls={} wake_returns={} active={} timeout={} last_result={}",
        label, d->sequence, d->guest_tid, d->host_tid, d->cpu_available, d->cpu_ns,
        d->calls, d->wake_returns, d->active, d->timeout, d->last_result);
    for (const auto& f : d->fds)
      log::info("shutdown-looper", "snapshot={} sequence={:#x} fd={} ident={} callback={:#x} requested={} ready={} unread_bytes={}",
          label, d->sequence, f.fd, f.ident, f.callback, f.events, f.ready, f.unread);
  }
}
}  // namespace
Looper::Looper(int options) : options_(options) {
  shutdown_trace::add_snapshot_source(snapshot_loopers);
  sequence_address_ = AddressSpace::get().allocate(4, kProtRead | kProtWrite, "[looper-wake]");
  if (!sequence_address_) throw std::bad_alloc();
  *g2h<std::uint32_t>(sequence_address_) = 0;
}
Looper::~Looper() { AddressSpace::get().free(sequence_address_); }
std::shared_ptr<Looper> pin_looper(Looper* p) { return loopers().get(p); }
Looper* prepare_looper(int options) {
  if (!local.ptr) local.ptr = loopers().create(options);
  return local.ptr;
}
Looper* thread_looper() { return local.ptr; }
void acquire_looper(Looper* p) {
  std::lock_guard lock(lifetime_mutex);
  if (auto l = pin_looper(p)) ++l->refs;
}
void release_looper(Looper* p) {
  std::lock_guard lock(lifetime_mutex);
  if (auto l = pin_looper(p); l && --l->refs == 0) loopers().erase(p);
}
void Looper::wake() {
  {
    std::lock_guard lock(mutex_);
    explicit_wake_ = true;
    ++sequence();
  }
  futex_wake(sequence_address_, INT_MAX, sync_census::WakeSource::looper);
}
int Looper::add_fd(int fd, int ident, int events, GuestAddr callback, GuestAddr data) {
  if (fd < 0 || (!callback && (!(options_ & 1) || ident < 0))) return -1;
  GuestPollFd check{fd, 0, 0};
  if (libc_poll_guest_fds(std::span(&check, 1)) < 0 || (check.revents & 32)) return -1;
  {
    std::lock_guard lock(mutex_);
    requests_[fd_key(fd)] = {fd, callback ? -2 : ident, events, callback, data, ++generation_, {}};
    ++sequence();
  }
  futex_wake(sequence_address_, INT_MAX, sync_census::WakeSource::looper);
  return 1;
}
int Looper::remove_fd(int fd) {
  int removed;
  {
    std::lock_guard lock(mutex_);
    removed = static_cast<int>(requests_.erase(fd_key(fd)));
    ++sequence();
  }
  futex_wake(sequence_address_, INT_MAX, sync_census::WakeSource::looper);
  return removed;
}
void Looper::post(std::function<void()> fn, std::int64_t deadline) {
  {
    std::lock_guard lock(mutex_);
    tasks_.emplace(deadline, std::move(fn));
    ++sequence();
  }
  futex_wake(sequence_address_, INT_MAX, sync_census::WakeSource::looper);
}
void Looper::add_source(GuestAddr key, int ident, GuestAddr callback, GuestAddr data,
                        std::function<bool()> ready) {
  {
    std::lock_guard lock(mutex_);
    requests_[key] = {-1, callback ? -2 : ident, 1, callback, data, ++generation_, std::move(ready)};
    ++sequence();
  }
  futex_wake(sequence_address_, INT_MAX, sync_census::WakeSource::looper);
}
void Looper::remove_source(GuestAddr key) {
  {
    std::lock_guard lock(mutex_);
    requests_.erase(key);
    ++sequence();
  }
  futex_wake(sequence_address_, INT_MAX, sync_census::WakeSource::looper);
}
std::optional<Looper::Diagnostic> Looper::diagnostic() {
  Diagnostic d;
  std::optional<platform::ProfileThread> owner;
  {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return std::nullopt;
    d.sequence = sequence_address_; d.guest_tid = diagnostic_guest_tid_;
    owner = diagnostic_owner_;
    for (const auto& [key, r] : requests_) if (r.fd >= 0 && d.fds.size() < 64)
      d.fds.push_back({r.fd, r.ident, r.events, r.callback, 0, -1});
  }
  if (owner) {
    d.host_tid = owner->id;
    d.cpu_available = platform::profile_cpu_time(*owner, d.cpu_ns);
  }
  d.calls = diagnostic_calls_.load(); d.wake_returns = diagnostic_wake_returns_.load();
  d.active = diagnostic_active_.load(); d.timeout = diagnostic_timeout_.load();
  d.last_result = diagnostic_result_.load();
  for (auto& fd : d.fds) {
    GuestPollFd poll{fd.fd, poll_events(fd.events), 0};
    if (libc_poll_guest_fds(std::span(&poll, 1)) >= 0) fd.ready = poll.revents;
    fd.unread = libc_guest_fd_available(fd.fd);
  }
  return d;
}
int Looper::poll(int timeout, int* fd, int* events, GuestAddr* data) {
  if (!shutdown_trace::enabled) return poll_impl(timeout, fd, events, data);
  auto& thread = GuestThread::adopt_current();
  {
    std::lock_guard lock(mutex_);
    if (!diagnostic_owner_ || diagnostic_owner_->id != platform::profile_thread_id())
      diagnostic_owner_ = platform::profile_current_thread();
    diagnostic_guest_tid_ = thread.tid();
  }
  ++diagnostic_calls_; diagnostic_active_ = true; diagnostic_timeout_ = timeout;
  try {
    auto result = poll_impl(timeout, fd, events, data);
    diagnostic_result_ = result;
    if (result == -1) ++diagnostic_wake_returns_;
    diagnostic_active_ = false;
    return result;
  } catch (...) { diagnostic_active_ = false; throw; }
}
int Looper::poll_impl(int timeout, int* out_fd, int* out_events, GuestAddr* out_data) {
  if (out_fd) *out_fd = 0;
  if (out_events) *out_events = 0;
  if (out_data) *out_data = 0;
  auto& thread = GuestThread::adopt_current();
  auto end = timeout < 0 ? INT64_MAX : monotonic_ns() + std::int64_t(timeout) * 1000000;
  sync_census::IoScope trace(sync_census::IoSource::looper, thread.tid(), end == INT64_MAX ? -1 : end);
  ReadinessWaitSet wait(sequence_address_);
  bool previous_timeout = false;
  for (;;) {
    std::vector<std::function<void()>> tasks;
    std::vector<std::pair<GuestAddr, Request>> snapshot;
    std::uint32_t seq;
    std::int64_t task_end = INT64_MAX;
    bool woke;
    {
      std::lock_guard lock(mutex_);
      seq = sequence().load();
      woke = std::exchange(explicit_wake_, false);
      auto now = monotonic_ns();
      auto last = tasks_.upper_bound(now);
      for (auto it = tasks_.begin(); it != last; ++it) tasks.push_back(std::move(it->second));
      tasks_.erase(tasks_.begin(), last);
      if (!tasks_.empty()) task_end = tasks_.begin()->first;
      if (responses_.empty()) snapshot.assign(requests_.begin(), requests_.end());
    }
    bool callback_ran = !tasks.empty();
    for (auto& task : tasks) task();  // callbacks may reenter or mutate the looper
    std::vector<GuestPollFd> fds;
    for (auto& [key, request] : snapshot)
      if (request.fd >= 0) fds.push_back({request.fd, poll_events(request.events), 0});
    if (!fds.empty() && libc_poll_guest_fds(fds) < 0) return -4;
    if (sync_census::enabled && previous_timeout &&
        std::any_of(fds.begin(), fds.end(), [](const auto& fd) { return fd.revents != 0; }))
      sync_census::add(sync_census::looper_timeout_ready);
    std::size_t fd_index = 0;
    for (auto& [key, request] : snapshot) {
      int events = request.fd >= 0 ? looper_events(fds[fd_index++].revents) : request.ready() ? 1 : 0;
      if (events) {
        std::lock_guard lock(mutex_);
        responses_.push_back({key, request, events});
      }
    }
    std::deque<Response> identifiers;
    for (;;) {
      Response response;
      {
        std::lock_guard lock(mutex_);
        if (responses_.empty()) break;
        response = responses_.front();
        responses_.pop_front();
        auto it = requests_.find(response.key);
        if (it == requests_.end() || it->second.generation != response.request.generation) continue;
      }
      auto& req = response.request;
      if (sync_census::enabled && req.fd >= 0)
        trace.readiness(libc_guest_fd_readiness_time(req.fd, poll_events(response.events)));
      if (!req.callback) {
        identifiers.push_back(std::move(response));
        continue;
      }
      auto keep =
          thread.cpu().isa() == GuestIsa::a32
              ? thread
                    .call32(req.callback,
                            hle::a32::CallBuilder32{}.value(req.fd).value(response.events).pointer(req.data))
                    .x0
              : thread.call(req.callback, {static_cast<std::uint64_t>(req.fd),
                                           static_cast<std::uint64_t>(response.events), req.data});
      callback_ran = true;
      if (!keep) {
        std::lock_guard lock(mutex_);
        auto it = requests_.find(response.key);
        if (it != requests_.end() && it->second.generation == req.generation) requests_.erase(it);
      }
    }
    // Android dispatches every collected callback before returning a non-callback
    // identifier. Retain other identifiers for the next pollOnce call.
    {
      std::lock_guard lock(mutex_);
      while (!identifiers.empty()) {
        auto response = std::move(identifiers.front());
        identifiers.pop_front();
        auto it = requests_.find(response.key);
        if (it == requests_.end() || it->second.generation != response.request.generation) continue;
        if (out_fd) *out_fd = response.request.fd;
        if (out_events) *out_events = response.events;
        if (out_data) *out_data = response.request.data;
        responses_.insert(responses_.end(), identifiers.begin(), identifiers.end());
        return response.request.ident;
      }
    }
    if (callback_ran) return -2;
    if (woke) return -1;
    auto now = monotonic_ns();
    if (now >= end) { trace.timeout(); return -3; }
    wait.clear();
    if (libc_watch_guest_fds(fds, wait) < 0) return -4;
    // A descriptor can become ready before its subscription is installed.
    // Recheck after registration, then use the original sequence for enqueue.
    if (!fds.empty()) {
      if (libc_poll_guest_fds(fds) < 0) return -4;
      if (std::any_of(fds.begin(), fds.end(), [](const auto& fd) { return fd.revents != 0; })) continue;
    }
    if (sequence().load() != seq) continue;
    auto deadline = std::min(end, task_end);
    // Non-fd input sources without a notifier retain their existing fallback.
    if (std::any_of(snapshot.begin(), snapshot.end(), [](const auto& item) { return item.second.fd < 0; }))
      wait.fallback(2'000'000);
    const auto sleep_begin = sync_census::enabled ? monotonic_ns() : 0;
    int status = wait.wait(thread, seq, deadline);
    if (sync_census::enabled) trace.slept(sleep_begin, monotonic_ns());
    previous_timeout = status == -err::timed_out;
    if (previous_timeout) sync_census::add(sync_census::looper_timeouts);
    if (status == -err::interrupted) return -1;
    if (status < 0 && status != -err::again && status != -err::timed_out) return -4;
  }
}
}  // namespace venture::android
