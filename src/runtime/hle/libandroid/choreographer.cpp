#include "android/errors.h"
#include "hle/libandroid/parts.h"
#include "android/choreographer.h"
#include "android/handles.h"
#include "android/looper.h"
#include "core/wait.h"
#include "xr/display_timing.h"
#include <bit>
#include <climits>
#include <map>

namespace venture::android {
namespace {
std::atomic<std::int64_t> notified_period{0}, display_phase{0};
std::int64_t display_period() { return xr::display_timing_state().period.load(); }
std::atomic<bool> rephase_pending{false};
std::int64_t next_frame(std::int64_t after) {
  auto period = display_period(), phase = display_phase.load();
  if (after < phase) return phase;
  auto remainder = (after - phase) % period;
  return after <= INT64_MAX - period ? after + period - remainder : INT64_MAX;
}
struct FrameRequest {
  std::int64_t after;
  std::function<void()> invoke;
  std::uint64_t generation = 0;
  bool dispatched = false;
};
struct Choreographer {
  explicit Choreographer(std::shared_ptr<Looper> l) : looper(l) {}
  std::weak_ptr<Looper> looper;
  std::mutex mutex;
  std::map<std::pair<GuestAddr, GuestAddr>, std::uint64_t> refresh;
  std::uint64_t generation = 0;
  std::vector<std::shared_ptr<FrameRequest>> pending;
};
// Called with the owning Choreographer mutex. Old timer tasks invalidate
// themselves after a rate/phase update; each guest callback still runs once.
void schedule_frame(const std::shared_ptr<Choreographer>& c, const std::shared_ptr<FrameRequest>& request) {
  if (auto l = c->looper.lock()) {
    const auto generation = ++request->generation;
    l->post([weak = std::weak_ptr(c), request, generation] {
      auto c = weak.lock();
      if (!c) return;
      {
        std::lock_guard lock(c->mutex);
        if (request->dispatched || request->generation != generation) return;
        request->dispatched = true;
        std::erase(c->pending, request);
      }
      request->invoke();
    }, next_frame(request->after));
  }
}
Handles<Choreographer>& choreographers() {
  static auto* table = new Handles<Choreographer>("AChoreographer");
  return *table;
}
std::mutex all_mutex;
std::vector<std::weak_ptr<Choreographer>> all;
void refresh_call(std::weak_ptr<Choreographer> weak, std::pair<GuestAddr, GuestAddr> key,
                  std::uint64_t generation, std::int64_t period) {
  auto c = weak.lock();
  if (!c) return;
  {
    std::lock_guard lock(c->mutex);
    auto it = c->refresh.find(key);
    if (it == c->refresh.end() || it->second != generation) return;
  }
  auto& thread = GuestThread::adopt_current();
  if (thread.cpu().isa() == GuestIsa::a32) {
    // NDK choreographer.h: refreshRateCallback(int64_t vsyncPeriodNanos,
    // void*), base AAPCS32: r0/r1 hold the period, r2 holds data.
    thread.call32(key.first, hle::a32::CallBuilder32{}.value(period).pointer(key.second));
    return;
  }
  thread.call(key.first, {static_cast<std::uint64_t>(period), key.second});
}
}  // namespace
void display_vsync(std::int64_t time, std::int64_t period) {
  if (time < 0 || period < 1000000 || period > 1000000000) return;
  bool rephase = false;
  if (time) {
    const auto previous = display_phase.exchange(time);
    const bool pending = rephase_pending.exchange(false);
    rephase = !previous || pending;
  } else rephase_pending = true;
  const bool changed = notified_period.exchange(period) != period;
  if (!changed && !rephase) return;
  std::vector<std::shared_ptr<Choreographer>> snapshot;
  {
    std::lock_guard lock(all_mutex);
    std::erase_if(all, [](const auto& w) { return w.expired(); });
    for (auto& w : all)
      if (auto c = w.lock()) snapshot.push_back(std::move(c));
  }
  for (auto& c : snapshot)
    if (auto l = c->looper.lock()) {
      std::lock_guard lock(c->mutex);
      if (changed) for (auto& [key, gen] : c->refresh)
        l->post([w = std::weak_ptr(c), key, gen, period] {
          refresh_call(w, key, gen, period);
        });
      for (auto& request : c->pending) schedule_frame(c, request);
    }
}
}  // namespace venture::android
namespace venture::hle::android_hle {
namespace {
using namespace venture::android;
struct LocalChoreographer {
  Choreographer* ptr = nullptr;
  ~LocalChoreographer() {
    if (ptr) choreographers().erase(ptr);
  }
};
thread_local LocalChoreographer local;
struct FrameData {
  std::int64_t time, period, id;
};
Handles<FrameData> frames("AChoreographerFrameCallbackData");
std::atomic<std::int64_t> next_vsync_id{1};
Choreographer* instance() {
  if (!local.ptr) {
    auto l = pin_looper(thread_looper());
    if (!l) return nullptr;
    local.ptr = choreographers().create(l);
    std::lock_guard lock(all_mutex);
    all.push_back(choreographers().get(local.ptr));
  }
  return local.ptr;
}
enum class Callback { legacy, wide, vsync };
void post(Choreographer* ptr, GuestAddr cb, GuestAddr data, std::int64_t delay_ms, Callback kind) {
  auto c = choreographers().get(ptr);
  if (!c || !cb) return;
  auto l = c->looper.lock();
  if (!l) return;
  auto now = monotonic_ns();
  if (delay_ms < 0) delay_ms = 0;
  if (delay_ms > (INT64_MAX - now) / 1000000) {
    log::warn("choreographer", "Frame delay overflow");
    return;
  }
  auto request = std::make_shared<FrameRequest>();
  request->after = now + delay_ms * 1000000;
  request->invoke = [weak = std::weak_ptr(c), cb, data, kind] {
        auto c = weak.lock();
        if (!c) return;
        auto period = display_period();
        auto time = next_frame(monotonic_ns()) - period;
        auto& thread = GuestThread::adopt_current();
        if (kind != Callback::vsync) {
          // NDK choreographer.h: frameCallback(long, void*) uses a 32-bit
          // timestamp on ARM; frameCallback64(int64_t, void*) uses r0/r1,
          // followed by data in r2 (ARM IHI 0042 base PCS).
          if (thread.cpu().isa() == GuestIsa::a32) {
            a32::CallBuilder32 args;
            if (kind == Callback::wide) args.value(time);
            else args.value(static_cast<std::int32_t>(time));
            thread.call32(cb, args.pointer(data));
          } else thread.call(cb, {static_cast<std::uint64_t>(time), data});
        } else {
          auto* frame = frames.create(FrameData{time, period, next_vsync_id++});
          // Callback data is opaque and valid only during this callback.
          try {
            thread.call(cb, {handles32::encode(h2g(frame), "AChoreographerFrameCallbackData"), data});
          } catch (...) {
            frames.erase(frame);
            throw;
          }
          frames.erase(frame);
        }
      };
  std::lock_guard lock(c->mutex);
  c->pending.push_back(request);
  schedule_frame(c, request);
}
void post_frame(Choreographer* c, GuestAddr cb, GuestAddr data) { post(c, cb, data, 0, Callback::legacy); }
void post_frame64(Choreographer* c, GuestAddr cb, GuestAddr data) { post(c, cb, data, 0, Callback::wide); }
void post_delayed(Choreographer* c, GuestAddr cb, GuestAddr data, std::int64_t ms) {
  post(c, cb, data, ms, Callback::legacy);
}
void post_delayed64(Choreographer* c, GuestAddr cb, GuestAddr data, std::uint32_t ms) {
  post(c, cb, data, ms, Callback::wide);
}
void post_vsync(Choreographer* c, GuestAddr cb, GuestAddr data) { post(c, cb, data, 0, Callback::vsync); }
void register_refresh(Choreographer* ptr, GuestAddr cb, GuestAddr data) {
  auto c = choreographers().get(ptr);
  if (!c || !cb) return;
  auto l = c->looper.lock();
  if (!l) return;
  auto key = std::make_pair(cb, data);
  std::uint64_t generation;
  {
    std::lock_guard lock(c->mutex);
    if (c->refresh.contains(key)) return;
    generation = ++c->generation;
    c->refresh.emplace(key, generation);
  }
  l->post([weak = std::weak_ptr(c), key, generation] {
    refresh_call(weak, key, generation, display_period());
  });
}
void unregister_refresh(Choreographer* ptr, GuestAddr cb, GuestAddr data) {
  if (auto c = choreographers().get(ptr)) {
    std::lock_guard lock(c->mutex);
    c->refresh.erase({cb, data});
  }
}
std::int64_t frame_time(FrameData* p) {
  auto f = frames.get(p);
  return f ? f->time : 0;
}
std::uint64_t timeline_count(FrameData* p) { return frames.get(p) ? 1 : 0; }
std::uint64_t preferred(FrameData*) { return 0; }
template <int Field>
std::int64_t timeline(FrameData* p, std::uint64_t index) {
  auto f = frames.get(p);
  if (!f || index) return -1;
  return Field == 0 ? f->id : Field == 1 ? f->time + f->period : f->time + f->period - 1000000;
}
}  // namespace
void register_choreographer(HleLibrary& lib) {
  add<&instance>(lib, "AChoreographer_getInstance");
  add<&post_frame>(lib, "AChoreographer_postFrameCallback");
  add<&post_frame64>(lib, "AChoreographer_postFrameCallback64");
  add<&post_delayed>(lib, "AChoreographer_postFrameCallbackDelayed");
  add<&post_delayed64>(lib, "AChoreographer_postFrameCallbackDelayed64");
  add<&post_vsync>(lib, "AChoreographer_postVsyncCallback");
  add<&register_refresh>(lib, "AChoreographer_registerRefreshRateCallback");
  add<&unregister_refresh>(lib, "AChoreographer_unregisterRefreshRateCallback");
  add<&frame_time>(lib, "AChoreographerFrameCallbackData_getFrameTimeNanos");
  add<&timeline_count>(lib, "AChoreographerFrameCallbackData_getFrameTimelinesLength");
  add<&preferred>(lib, "AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex");
  add<&timeline<0>>(lib, "AChoreographerFrameCallbackData_getFrameTimelineVsyncId");
  add<&timeline<1>>(lib, "AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos");
  add<&timeline<2>>(lib, "AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos");
}
}  // namespace venture::hle::android_hle
