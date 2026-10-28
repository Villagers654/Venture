#include "xr/haptic_output.h"
#include "core/wait.h"
#include "core/av_telemetry.h"
#include "hle/vrapi/state.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <thread>

namespace venture::hle::vrapi {
namespace {
// VrApi_Input.h exposes an amplitude byte stream with a capability-selected
// sample duration. Approximate that envelope with timed core OpenXR pulses;
// no claim of PCM carrier reproduction or hardware sample-rate control.
constexpr std::int64_t sample_ns = 4'000'000;
std::atomic<std::uint64_t> expired_buffers{0};
struct Buffer {
  XrSession session;
  XrHapticActionInfo action;
  std::int64_t start;
  std::vector<std::uint8_t> samples;
  std::size_t played = 0;
  std::int64_t submitted = 0;
};
struct Haptics {
  std::mutex mutex;
  std::condition_variable_any changed;
  std::deque<Buffer> buffers[2];
  std::jthread worker;
  Haptics() {
    (void)av::enabled();
    av::emit({{"kind", "haptic_buffer_monitor"}, {"api", "vrapi_buffer"}, {"version", 1}});
    worker = std::jthread([this](std::stop_token stop) { run(stop); });
  }
  ~Haptics() {
    worker.request_stop();
    changed.notify_all();
  }
  void run(std::stop_token stop) {
    std::unique_lock lock(mutex);
    while (!stop.stop_requested()) {
      std::int64_t delay = 1'000'000'000;
      auto now = monotonic_ns();
      for (unsigned hand = 0; hand < 2; ++hand) {
        auto& queue = buffers[hand];
        if (queue.empty())
          continue;
        auto &b = queue.front();
        auto due = b.start + std::int64_t(b.played) * sample_ns;
        if (now < due) {
          delay = std::min(delay, due - now);
          continue;
        }
        auto index = std::size_t(std::max(std::int64_t(b.played), (now - b.start) / sample_ns));
        if (index >= b.samples.size()) {
          if (b.played < b.samples.size()) {
            expired_buffers.fetch_add(1, std::memory_order_relaxed);
            const auto peak = *std::max_element(b.samples.begin() + b.played, b.samples.end());
            av::emit({{"kind", "haptic_buffer_drop"}, {"api", "vrapi_buffer"}, {"reason", "expired"},
                {"hand", hand == 0 ? "/user/hand/left" : "/user/hand/right"},
                {"submit_ns", b.submitted}, {"scheduled_start_ns", b.start}, {"worker_ns", now},
                {"buffer_samples", b.samples.size()}, {"played_samples", b.played},
                {"expired_samples", b.samples.size() - b.played}, {"amplitude", float(peak) / 255}});
          }
          queue.pop_front();
          delay = 0;
          continue;
        }
        auto end = index + 1;
        while (end < b.samples.size() && b.samples[end] == b.samples[index])
          ++end;
        XrHapticVibration pulse{XR_TYPE_HAPTIC_VIBRATION, nullptr,
                                std::int64_t(end - index) * sample_ns, XR_FREQUENCY_UNSPECIFIED,
                                float(b.samples[index]) / 255};
        auto r = venture::xr::apply_haptic("vrapi_buffer", b.session, b.action, pulse);
        if (XR_FAILED(r)) {
          failure("buffered haptic pulse", r);
          queue.clear();
          continue;
        }
        b.played = end;
        delay = std::min(delay,
                         std::max<std::int64_t>(0, b.start + std::int64_t(end) * sample_ns - now));
      }
      // Woken by new buffers, cancellation or shutdown; only haptics run on
      // this worker. Frame pacing remains on the application's render thread.
      changed.wait_for(lock, std::chrono::nanoseconds(delay));
    }
  }
};
Haptics &haptics() {
  static Haptics h;
  return h;
}
} // namespace
std::uint64_t expired_haptic_buffers() { return expired_buffers.load(std::memory_order_relaxed); }
void cancel_haptics(int hand) {
  auto &h = haptics();
  std::lock_guard lock(h.mutex);
  for (int i = 0; i < 2; ++i)
    if (hand < 0 || hand == i) {
      h.buffers[i].clear();
      if (session().handle && state().input.haptic) {
        XrHapticActionInfo ai{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, state().input.haptic,
                              state().input.hands[i]};
        auto r = venture::xr::stop_haptic("vrapi_buffer", session().handle, ai);
        if (XR_FAILED(r))
          failure("cancel haptics", r);
      }
    }
  h.changed.notify_all();
}
namespace guest {
std::int32_t vrapi_SetHapticVibrationBuffer(Mobile *mobile, std::uint32_t id,
                                            const HapticBuffer *buffer) {
  std::lock_guard lock(state().mutex);
  auto rejected = [&](std::int32_t value) {
    XrHapticActionInfo ai{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, state().input.haptic,
        id >= 1 && id <= 2 ? state().input.hands[id - 1] : XR_NULL_PATH};
    venture::xr::haptic_event("vrapi_buffer", session().handle, ai, 0, 0, 0,
        "validate_guest_buffer", XR_ERROR_VALIDATION_FAILURE,
        {{"stage", "guest_request"}, {"host_query_safe", false}, {"device_id", id}, {"guest_result", value},
         {"buffer_present", buffer != nullptr},
         {"hand", id == 1 ? "/user/hand/left" : id == 2 ? "/user/hand/right" : "invalid"}});
    return value;
  };
  if (!valid(mobile) || id < 1 || id > 2 || !buffer || !std::isfinite(buffer->time) ||
      buffer->time < 0 || buffer->count > 256 || (buffer->count && !buffer->samples))
    return rejected(invalid);
  auto now = monotonic_ns();
  if (buffer->time > double(now) * 1e-9 + 2)
    return rejected(invalid);
  auto start = buffer->time ? std::int64_t(buffer->time * 1e9) : now;
  auto &h = haptics();
  std::lock_guard queue_lock(h.mutex);
  auto &queue = h.buffers[id - 1];
  if (queue.size() >= 8)
    return rejected(invalid_operation);
  // A new stream at or before the old tail replaces that tail. Explicit end
  // markers cancel any later work; already submitted finite pulses expire.
  while (!queue.empty() && queue.back().start >= start)
    queue.pop_back();
  if (!queue.empty()) {
    auto &tail = queue.back();
    if (tail.start + std::int64_t(tail.samples.size()) * sample_ns > start) {
      tail.samples.resize(std::size_t((start - tail.start) / sample_ns));
      tail.played = std::min(tail.played, tail.samples.size());
    }
  }
  if (buffer->count) {
    Buffer b{
        session().handle,
        {XR_TYPE_HAPTIC_ACTION_INFO, nullptr, state().input.haptic, state().input.hands[id - 1]},
        start,
        std::vector<std::uint8_t>(buffer->samples, buffer->samples + buffer->count)};
    b.submitted = now;
    queue.push_back(std::move(b));
  } else if (buffer->terminated) {
    queue.clear();
    XrHapticActionInfo ai{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, state().input.haptic,
                          state().input.hands[id - 1]};
    return result(venture::xr::stop_haptic("vrapi_buffer", session().handle, ai, "guest_request"), "EndHapticBuffer");
  }
  float amplitude = 0;
  for (uint32_t i = 0; i < buffer->count; ++i) amplitude = std::max(amplitude, float(buffer->samples[i]) / 255);
  XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, state().input.haptic, state().input.hands[id - 1]};
  venture::xr::haptic_event("vrapi_buffer", session().handle, info, amplitude,
      int64_t(buffer->count) * sample_ns, 0, "enqueue_core_sequence", XR_SUCCESS,
      {{"stage", "guest_request"}, {"submit_ns", now}, {"scheduled_start_ns", start}, {"buffer_size", buffer->count}});
  h.changed.notify_all();
  return success;
}
} // namespace guest
} // namespace venture::hle::vrapi
