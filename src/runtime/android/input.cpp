#include "android/errors.h"
#include "android/input.h"
#include "android/handles.h"
#include "android/looper.h"
#include "common/log.h"
#include <deque>
#include <unordered_map>

namespace venture::android {
struct InputQueue {
  struct Pending {
    std::shared_ptr<InputEvent> event;
    std::function<void(bool)> completion;
  };
  std::mutex mutex;
  std::deque<Pending> pending;
  std::unordered_map<GuestAddr, Pending> in_flight;
  std::weak_ptr<Looper> looper;
  bool closed = false;
};
namespace {
Handles<InputQueue> queues{"AInputQueue"};
std::mutex events_mutex;
std::unordered_map<GuestAddr, std::weak_ptr<InputEvent>> events;
}  // namespace
InputQueue* create_input_queue() { return queues.create(); }
void destroy_input_queue(InputQueue* p) {
  auto q = queues.get(p);
  if (!q) return;
  {
    std::lock_guard lock(q->mutex);
    q->closed = true;
  }
  detach_input_queue(p);
  std::deque<InputQueue::Pending> pending;
  {
    std::lock_guard lock(q->mutex);
    pending.swap(q->pending);
    for (auto& [key, item] : q->in_flight) pending.push_back(std::move(item));
    q->in_flight.clear();
  }
  {
    std::lock_guard lock(events_mutex);
    for (auto& item : pending) {
      events.erase(h2g(item.event.get()));
      handles32::retire(h2g(item.event.get()));
    }
  }
  queues.erase(p);
  for (auto& item : pending)
    if (item.completion) item.completion(false);
}
bool enqueue_input(InputQueue* p, InputEvent event, std::function<void(bool)> completion) {
  auto q = queues.get(p);
  if (!q || (event.type != 1 && event.type != 2) || (event.type == 2 && event.pointers.empty())) return false;
  auto e = std::make_shared<InputEvent>(std::move(event));
  std::shared_ptr<Looper> l;
  {
    std::lock_guard lock(q->mutex);
    if (q->closed) return false;
    {
      std::lock_guard event_lock(events_mutex);
      events[h2g(e.get())] = e;
      handles32::publish(e, "AInputEvent");
    }
    q->pending.push_back({std::move(e), std::move(completion)});
    l = q->looper.lock();
  }
  if (l) l->wake();
  return true;
}
void attach_input_queue(InputQueue* p, Looper* lp, int ident, GuestAddr cb, GuestAddr data) {
  auto q = queues.get(p);
  auto l = pin_looper(lp);
  if (!q || !l) return;
  std::lock_guard queue_lock(q->mutex);
  if (q->closed) return;
  if (auto old = q->looper.lock()) old->remove_source(h2g(p));
  q->looper = l;
  l->add_source(h2g(p), ident, cb, data, [weak = std::weak_ptr(q)] {
    auto queue = weak.lock();
    if (!queue) return false;
    std::lock_guard lock(queue->mutex);
    return !queue->pending.empty();
  });
}
void detach_input_queue(InputQueue* p) {
  auto q = queues.get(p);
  if (!q) return;
  std::lock_guard lock(q->mutex);
  if (auto l = q->looper.lock()) l->remove_source(h2g(p));
  q->looper.reset();
}
int input_has_events(InputQueue* p) {
  auto q = queues.get(p);
  if (!q) return -err::invalid;
  std::lock_guard lock(q->mutex);
  return !q->pending.empty();
}
int input_get_event(InputQueue* p, GuestAddr* event) {
  auto q = queues.get(p);
  if (!q || !event) return -err::invalid;
  std::lock_guard lock(q->mutex);
  if (q->pending.empty()) return -err::again;
  *event = h2g(q->pending.front().event.get());
  q->in_flight.emplace(*event, std::move(q->pending.front()));
  q->pending.pop_front();
  return 0;
}
void input_finish_event(InputQueue* p, GuestAddr event, bool handled) {
  auto q = queues.get(p);
  if (!q) return;
  InputQueue::Pending item;
  {
    std::lock_guard lock(q->mutex);
    auto it = q->in_flight.find(event);
    if (it == q->in_flight.end()) {
      log::warn("input", "finishEvent: event is not in flight");
      return;
    }
    item = std::move(it->second);
    q->in_flight.erase(it);
  }
  {
    std::lock_guard lock(events_mutex);
    events.erase(event);
    handles32::retire(event);
  }
  if (item.completion) item.completion(handled);
}
std::shared_ptr<const InputEvent> pin_input_event(GuestAddr event) {
  std::lock_guard lock(events_mutex);
  auto it = events.find(event);
  return it == events.end() ? nullptr : it->second.lock();
}
}  // namespace venture::android
