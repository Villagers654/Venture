#include "core/readiness.h"
#include "core/wait.h"
#include "core/error.h"
#include <algorithm>
#include <map>

namespace venture {
class ReadinessQueue {
 public:
  struct Subscription {
    std::shared_ptr<ReadinessQueue> queue;
    ReadinessWaitSet& waiter;
    std::uint32_t events;
    Subscription* previous = nullptr;
    Subscription* next = nullptr;
    Subscription(std::shared_ptr<ReadinessQueue> q, ReadinessWaitSet& w, std::uint32_t e)
        : queue(std::move(q)), waiter(w), events(e) {
      {
        std::lock_guard lock(queue->mutex_);
        next = queue->first_;
        if (next) next->previous = this;
        queue->first_ = this;
        queue->waiting_.store(true, std::memory_order_release);
      }
      if (queue->changed_) queue->changed_();
    }
    ~Subscription() {
      {
        std::lock_guard lock(queue->mutex_);
        if (previous) previous->next = next;
        else queue->first_ = next;
        if (next) next->previous = previous;
        queue->waiting_.store(queue->first_ != nullptr, std::memory_order_release);
      }
      if (queue->changed_) queue->changed_();
    }
    void extend(std::uint32_t e) {
      bool changed;
      {
        std::lock_guard lock(queue->mutex_);
        changed = (e & ~events) != 0;
        events |= e;
      }
      if (changed && queue->changed_) queue->changed_();
    }
  };
  explicit ReadinessQueue(std::function<void()> changed) : changed_(std::move(changed)) {}
  void notify(std::uint32_t events) {
    if (!waiting_.load(std::memory_order_acquire)) return;
    std::lock_guard lock(mutex_);
    for (auto* node = first_; node; node = node->next)
      if (node->events & events) node->waiter.notify();
  }
  std::uint32_t interests() const {
    if (!waiting_.load(std::memory_order_acquire)) return 0;
    std::lock_guard lock(mutex_);
    std::uint32_t result = 0;
    for (auto* node = first_; node; node = node->next) result |= node->events;
    return result;
  }
 private:
  mutable std::mutex mutex_;
  std::atomic<bool> waiting_{false};
  Subscription* first_ = nullptr;
  const std::function<void()> changed_;
};
std::shared_ptr<ReadinessQueue> ReadinessSource::acquire() {
  if (!queue_.load(std::memory_order_acquire)) {
    std::lock_guard lock(init_mutex_);
    if (!owner_) {
      owner_ = std::make_shared<ReadinessQueue>(changed_);
      queue_.store(owner_.get(), std::memory_order_release);
    }
  }
  return owner_;
}
ReadinessSource::~ReadinessSource() { notify(UINT32_MAX); }
void ReadinessSource::notify(std::uint32_t events) {
  if (auto* queue = queue_.load(std::memory_order_acquire)) queue->notify(events);
}
std::uint32_t ReadinessSource::interests() const {
  if (auto* queue = queue_.load(std::memory_order_acquire)) return queue->interests();
  return 0;
}
struct ReadinessWaitSet::Impl {
  std::map<ReadinessQueue*, ReadinessQueue::Subscription> subscriptions;
};
ReadinessWaitSet::ReadinessWaitSet(GuestAddr word) : word_(word ? word : h2g(&owned_word_)) {}
ReadinessWaitSet::~ReadinessWaitSet() = default;
std::uint32_t ReadinessWaitSet::sequence() const { return std::atomic_ref(*g2h<std::uint32_t>(word_)).load(); }
void ReadinessWaitSet::add(ReadinessSource& source, std::uint32_t events) {
  if (!impl_) impl_ = std::make_unique<Impl>();
  auto queue = source.acquire();
  // Error/hangup/invalid descriptor readiness is never masked by poll events.
  events |= 8 | 16 | 32;
  const auto [it, added] = impl_->subscriptions.try_emplace(queue.get(), queue, *this, events);
  if (!added) it->second.extend(events);
}
void ReadinessWaitSet::clear() {
  if (impl_) impl_->subscriptions.clear();
  reset_limits();
}
void ReadinessWaitSet::reset_limits() {
  deadline_ = INT64_MAX;
  fallback_ns_ = 0;
}
void ReadinessWaitSet::deadline(std::int64_t ns) { if (ns >= 0) deadline_ = std::min(deadline_, ns); }
void ReadinessWaitSet::fallback(std::int64_t ns) { if (!fallback_ns_ || ns < fallback_ns_) fallback_ns_ = ns; }
void ReadinessWaitSet::notify() {
  std::atomic_ref(*g2h<std::uint32_t>(word_)).fetch_add(1);
  // Coalesce several ready fds into one wake. Sequential consistency pairs
  // publication with wait's check, including notification before enqueue.
  if (parked_.exchange(false)) futex_wake(word_, 1);
}
int ReadinessWaitSet::wait(GuestThread& thread, std::uint32_t expected, std::int64_t end) {
  end = std::min(end, deadline_);
  if (fallback_ns_) end = std::min(end, monotonic_ns() + fallback_ns_);
  parked_.store(true);
  struct Reset { std::atomic<bool>& value; ~Reset() { value.store(false); } } reset{parked_};
  if (sequence() != expected) return 0;
  const auto status = futex_wait(thread, word_, expected, end == INT64_MAX ? -1 : end);
  return status == -err::kEAGAIN ? 0 : status;
}
} // namespace venture
