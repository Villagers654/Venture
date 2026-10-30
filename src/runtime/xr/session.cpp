#include "core/xr_call_trace.h"
#include "xr/menu_chord.h"
#include "common/first_frame_progress.h"
#include "common/startup_progress.h"
#include "xr/dev_dump.h"
#include "xr/session.h"
#include "xr/display_memory.h"
#include "common/log.h"
#include "common/environment.h"
#include "core/profile.h"
#include "core/pipeline_stats.h"
#include "core/thread.h"
#include "core/wait.h"
#include "gfx/gpu_device.h"
#include "xr/queue.h"
#include "gfx/queue_sync.h"
#include "hle/vulkan/timing.h"
#include "xr/xr_images.h"
#include "xr/clock.h"
#include "xr/haptic_output.h"
#include <algorithm>
#include <string>
#include <string_view>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <array>
#include <limits>
#include <optional>
#include <vector>

namespace venture::xr {
namespace {
std::atomic<XrImages *> images{nullptr};
std::atomic<FrameObserver> frame_observer{nullptr};
std::atomic<DisplayObserver> display_observer{nullptr};
void report_metrics(XrSession session) {
  auto& host = host_xr();
  if (!host.enabled_extensions.contains("XR_META_performance_metrics")) return;
  XrPerformanceMetricsStateMETA state{XR_TYPE_PERFORMANCE_METRICS_STATE_META};
  if (XR_FAILED((host_call<PFN_xrGetPerformanceMetricsStateMETA, "xrGetPerformanceMetricsStateMETA">(session, &state))) || !state.enabled) return;
  std::uint32_t count{};
  auto enumerate = host_proc<PFN_xrEnumeratePerformanceMetricsCounterPathsMETA, "xrEnumeratePerformanceMetricsCounterPathsMETA">();
  auto query = host_proc<PFN_xrQueryPerformanceMetricsCounterMETA, "xrQueryPerformanceMetricsCounterMETA">();
  if (!enumerate || !query || XR_FAILED(enumerate(host.instance, 0, &count, nullptr)) || !count) return;
  std::vector<XrPath> paths(count);
  if (XR_FAILED(enumerate(host.instance, count, &count, paths.data()))) return;
  for (auto path : paths) {
    std::array<char, XR_MAX_PATH_LENGTH> name{};
    std::uint32_t length{};
    XrPerformanceMetricsCounterMETA counter{XR_TYPE_PERFORMANCE_METRICS_COUNTER_META};
    if (XR_FAILED(xrPathToString(host.instance, path, name.size(), &length, name.data())) ||
        XR_FAILED(query(session, path, &counter))) continue;
    const char* unit = "generic";
    switch (counter.counterUnit) {
    case XR_PERFORMANCE_METRICS_COUNTER_UNIT_PERCENTAGE_META: unit = "%"; break;
    case XR_PERFORMANCE_METRICS_COUNTER_UNIT_MILLISECONDS_META: unit = "ms"; break;
    case XR_PERFORMANCE_METRICS_COUNTER_UNIT_BYTES_META: unit = "bytes"; break;
    case XR_PERFORMANCE_METRICS_COUNTER_UNIT_HERTZ_META: unit = "Hz"; break;
    default: break;
    }
    if (counter.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META)
      log::info("xr-metrics", "{}={} {}", name.data(), counter.floatValue, unit);
    else if (counter.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_UINT_VALUE_VALID_BIT_META)
      log::info("xr-metrics", "{}={} {}", name.data(), counter.uintValue, unit);
  }
}
}
void set_frame_observer(FrameObserver observer) { frame_observer = observer; }
void set_display_observer(DisplayObserver observer) {
  display_observer = observer;
  if (observer) observer(session().display_timing.period.load());
}
void install_xr_images(XrImages *provider) { images.store(provider); }
XrImages *xr_images() { return images.load(); }
Session &session() {
  static Session s;
  return s;
}

void Session::observe_display_period(std::int64_t value, DisplayTiming::Source source) {
  const auto previous = display_timing.period.load();
  const bool unknown = display_timing.source == DisplayTiming::Source::unknown ||
                       display_timing.source == DisplayTiming::Source::remembered;
  display_timing.observe(value, source);
  if (display_timing.period.load() != previous || (unknown && display_timing.source != DisplayTiming::Source::unknown)) {
    remember_display_period(display_timing.period.load());
    if (auto observer = display_observer.load()) observer(display_timing.period.load());
  }
}

void Session::observe_host_event(const XrEventDataBuffer& event) {
  if (event.type != XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB) return;
  const auto& changed = reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB&>(event);
  if (std::isfinite(changed.toDisplayRefreshRate) && changed.toDisplayRefreshRate > 0)
    observe_display_period(std::int64_t(std::llround(1e9 / changed.toDisplayRefreshRate)),
                           DisplayTiming::Source::host);
}

bool Session::deliver_host_event(const XrEventDataBuffer& event) const {
  if (event.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) return true;
  const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
  return changed.session != handle || changed.state != XR_SESSION_STATE_EXITING ||
         !host_stop_requested || guest_exit_requested;
}
XrResult Session::request_host_stop() {
  std::unique_lock lock(mutex, std::try_to_lock);
  if (!lock.owns_lock()) return XR_ERROR_SESSION_NOT_READY;
  if (host_stop_requested) return XR_SUCCESS;
  if (!handle || !running) return XR_ERROR_SESSION_NOT_RUNNING;
  const auto result = ::xrRequestExitSession(handle);
  if (XR_SUCCEEDED(result)) host_stop_requested = true;
  return result;
}
XrResult Session::request_exit(SessionOwner expected) {
  std::lock_guard lock(mutex);
  if (expected != SessionOwner::none && owner != expected) return XR_ERROR_FEATURE_UNSUPPORTED;
  if (!handle || !running) return XR_ERROR_SESSION_NOT_RUNNING;
  // The host already requested STOPPING. Publish the guest's explicit intent
  // without submitting a duplicate request to the desktop runtime.
  const auto result = host_stop_requested ? XR_SUCCESS : ::xrRequestExitSession(handle);
  if (XR_SUCCEEDED(result)) guest_exit_requested = true;
  return result;
}

XrResult Session::create(SessionOwner requested, const XrGraphicsBindingVulkanKHR *binding) {
  if (handle)
    return XR_ERROR_LIMIT_REACHED;
  auto &h = host_xr();
  if (!h.available)
    return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
  auto &gpu = gfx::gpu_device();
  XrGraphicsBindingVulkanKHR b{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
  if (binding)
    b = *binding;
  else {
    b.instance = gpu.instance;
    b.physicalDevice = gpu.physical_device;
    b.device = gpu.device;
    b.queueFamilyIndex = gpu.queue_family;
    b.queueIndex = gpu.queue_index;
  }
  b.next = nullptr;
  if (!b.device || !b.instance || !b.physicalDevice)
    return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  if (binding) {
    // Guest Vulkan may own its VkDevice (gpu_device.h contract). Validate
    // runtime selection in that VkInstance's handle namespace, not by comparing
    // VkPhysicalDevice pointers belonging to different Vulkan instances.
    VkPhysicalDevice selected{};
    auto r = host_graphics_device(h.system, b.instance, &selected);
    if (XR_FAILED(r) || selected != b.physicalDevice)
      return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  }
  XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
  auto result = host_call<PFN_xrGetVulkanGraphicsRequirements2KHR, "xrGetVulkanGraphicsRequirements2KHR">(h.instance, h.system, &requirements);
  if (XR_FAILED(result))
    return result;
  XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
  info.systemId = h.system;
  info.next = &b;
  result = xrCreateSession(h.instance, &info, &handle);
  log::info("xr", "Host xrCreateSession result={}", int(result));
  if (XR_SUCCEEDED(result)) {
    dev_dump_bind(b);
    frame_index = end_errors = last_report_frame = 0;
    last_report_time = {};
    owner = requested;
    host_stop_requested = guest_exit_requested = false;
    auto get_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(
        gpu.get_instance_proc_addr(b.instance, "vkGetDeviceQueue"));
    get_queue(b.device, b.queueFamilyIndex, b.queueIndex, &queue);
    uses_shared_gpu = queue == gpu.queue;
    gfx::register_queue(b.device, queue);
    state = XR_SESSION_STATE_IDLE;
    running = false;
    phase = Phase::idle;
    render_thread = {};
    frame = {XR_TYPE_FRAME_STATE};
    pending_frame = {XR_TYPE_FRAME_STATE};
    wait_in_progress = pending_wait = false;
    lost = XR_SUCCESS;
    lead = {};
    const auto* lead_setting = venture::environment::getenv("VENTURE_XR_FRAME_LEAD");
    lead.enabled = lead_setting && std::string_view(lead_setting) == "1";
    if (const auto* fixed = venture::environment::getenv("VENTURE_XR_FRAME_LEAD_MS"); lead.enabled && fixed) {
      const auto ms = std::strtod(fixed, nullptr);
      if (std::isfinite(ms) && ms > 0 && ms <= 8) lead.fixed_ns = lead.lead_ns = std::int64_t(ms * 1e6);
    }
    const auto* async_setting = venture::environment::getenv("VENTURE_XR_ASYNC_END");
    lead.async = lead.enabled && async_setting && std::string_view(async_setting) == "1";
    display_timing.restart();
    guest_swap_interval = 1;
    logged_swap_interval = 0;
    if (h.enabled_extensions.contains("XR_FB_display_refresh_rate")) {
      float rate = 0;
      if (XR_SUCCEEDED((host_call<PFN_xrGetDisplayRefreshRateFB, "xrGetDisplayRefreshRateFB">(handle, &rate))) && std::isfinite(rate) && rate > 0)
        observe_display_period(std::int64_t(std::llround(1e9 / rate)), DisplayTiming::Source::host);
    }
    if (auto observer = display_observer.load()) observer(display_timing.period.load());
  }
  return result;
}
// Issues the runtime's xrWaitFrame for led frames as soon as it is requested,
// independent of guest progress, after the host xrEndFrame of an asynchronous
// submission. One per session, retired at destroy after its last request has
// returned; detached and shared, so process exit never joins it.
struct HostWaiter {
  std::mutex mutex;
  std::condition_variable changed;
  enum class State { idle, requested, done } state = State::idle;
  bool cancelled = false, retired = false;
  XrSession session = XR_NULL_HANDLE;
  std::uint64_t generation = 0;
  XrFrameState frame{XR_TYPE_FRAME_STATE};
  XrResult result = XR_SUCCESS;
  std::int64_t returned_ns = 0;
  // An asynchronous submission: its owner keeps the layers alive until the
  // host xrEndFrame returns. A failed End is this request's result, unwaited.
  std::shared_ptr<const void> end_owner;
  XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
  bool shared_queue = true;
  bool ending = false;  // an asynchronous host End is requested or in flight
  void run() {
    std::unique_lock lock(mutex);
    for (;;) {
      changed.wait(lock, [&] { return state == State::requested || retired; });
      if (state != State::requested) return;
      const auto target = session;
      auto owner = std::move(end_owner);
      const auto end = end_info;
      const bool shared = shared_queue;
      lock.unlock();
      XrResult r = XR_SUCCESS;
      if (owner) {
        {
          std::optional<vulkan::ScopedHostWait> lock_timing(std::in_place, "xr_end_queue_lock");
          HostQueueLock queue(shared);
          lock_timing.reset();
          r = !queue ? XR_ERROR_RUNTIME_FAILURE
                     : [&] { vulkan::ScopedHostWait timing("xr_end_frame"); return xrEndFrame(target, &end); }();
        }
        dev_dump_frame(&end, r);
        owner.reset();
      }
      XrFrameState waited{XR_TYPE_FRAME_STATE};
      // A cancelled request (loss, end, destroy) still made its promised End.
      lock.lock();
      const bool skip = cancelled;
      ending = false;
      changed.notify_all();
      lock.unlock();
      if (XR_SUCCEEDED(r)) r = skip ? XR_ERROR_SESSION_NOT_RUNNING : xrWaitFrame(target, nullptr, &waited);
      const auto now = monotonic_ns();
      lock.lock();
      frame = waited;
      frame.next = nullptr;
      result = r;
      returned_ns = now;
      state = State::done;
      changed.notify_all();
    }
  }
};
XrResult Session::destroy(SessionOwner requested) {
  if (!handle || owner != requested)
    return XR_ERROR_HANDLE_INVALID;
  // Before the queue lock: an asynchronous host End in flight takes it.
  drain_host_wait();
  HostQueueLock queue(uses_shared_gpu);
  if (!queue)
    return XR_ERROR_RUNTIME_FAILURE;
  reset_menu_session(handle);
  if (host_waiter) {
    {
      std::lock_guard waiter_lock(host_waiter->mutex);
      host_waiter->retired = true;
      host_waiter->changed.notify_all();
    }
    host_waiter.reset();
  }
  auto result = xrDestroySession(handle);
  if (XR_SUCCEEDED(result)) {
    log::info("xr", "Session ended: submitted={} xrEndFrame_errors={}", frame_index, end_errors);
    clear_haptic_bindings(handle);
    dev_dump_end();
    handle = XR_NULL_HANDLE;
    owner = SessionOwner::none;
    phase = Phase::idle;
    running = false;
    pending_wait = false;
    render_thread = {};
    notify_frame_waiters();
  }
  return result;
}
void Session::notify_frame_waiters() {
  std::atomic_ref(frame_epoch).fetch_add(1, std::memory_order_release);
  futex_wake(h2g(&frame_epoch), std::numeric_limits<int>::max());
  frame_changed.notify_all();
}
void Session::lose(XrResult result) {
  if (XR_FAILED(lost)) return;
  lost = result;
  cancel_host_wait();
  log::warn("xr", "Session lost: {}", int(result));
  notify_frame_waiters();
}
void Session::note(XrResult result) {
  if (result == XR_ERROR_SESSION_LOST || result == XR_ERROR_INSTANCE_LOST) lose(result);
}
void Session::reset_frames() {
  // OpenXR xrBeginSession: frame state starts reset when a session begins
  // running; xrEndSession abandons any waited or begun frame. A host wait
  // still in flight retires itself against run_generation.
  ++run_generation;
  phase = Phase::idle;
  pending_wait = false;
  render_thread = {};
  frame = {XR_TYPE_FRAME_STATE};
  pending_frame = {XR_TYPE_FRAME_STATE};
  lead.owed = lead.begun_owed = lead.host_valid = false;
  notify_frame_waiters();
}
bool Session::lead_active() const {
  return lead.enabled && owner == SessionOwner::openxr && !lead.pipelined && lead.plain_waits &&
         lead.lead_ns > 0 && state == XR_SESSION_STATE_FOCUSED;
}
void Session::finish_host_end() {
  std::shared_ptr<HostWaiter> waiter;
  {
    std::lock_guard lock(mutex);
    waiter = host_waiter;
  }
  if (!waiter) return;
  std::unique_lock lock(waiter->mutex);
  waiter->changed.wait(lock, [&] { return !waiter->ending; });
}
void Session::request_host_wait() {
  request_host_end({XR_TYPE_FRAME_END_INFO}, nullptr);
}
bool Session::request_host_end(const XrFrameEndInfo& info, std::shared_ptr<const void> owned) {
  if (!host_waiter) {
    host_waiter = std::make_shared<HostWaiter>();
    std::thread([waiter = host_waiter] { waiter->run(); }).detach();
  }
  std::lock_guard lock(host_waiter->mutex);
  if (host_waiter->state != HostWaiter::State::idle) return false;
  host_waiter->ending = owned != nullptr;
  host_waiter->end_owner = std::move(owned);
  host_waiter->end_info = info;
  host_waiter->shared_queue = uses_shared_gpu;
  host_waiter->cancelled = false;
  host_waiter->session = handle;
  host_waiter->generation = run_generation;
  host_waiter->state = HostWaiter::State::requested;
  host_waiter->changed.notify_all();
  return true;
}
bool Session::host_wait_pending() {
  if (!host_waiter) return false;
  std::lock_guard lock(host_waiter->mutex);
  if (host_waiter->state == HostWaiter::State::idle) return false;
  if (host_waiter->generation == run_generation) return true;
  // A wait from before a session reset: retire it once it has returned.
  if (host_waiter->state == HostWaiter::State::done) host_waiter->state = HostWaiter::State::idle;
  return false;
}
XrResult Session::take_host_wait(XrFrameState& out, std::int64_t& returned_ns, std::uint64_t& generation) {
  std::unique_lock lock(host_waiter->mutex);
  host_waiter->changed.wait(lock, [&] {
    return host_waiter->state == HostWaiter::State::done || host_waiter->cancelled;
  });
  generation = host_waiter->generation;
  if (host_waiter->cancelled) return XR_ERROR_SESSION_NOT_RUNNING;
  out = host_waiter->frame;
  returned_ns = host_waiter->returned_ns;
  host_waiter->state = HostWaiter::State::idle;
  return host_waiter->result;
}
void Session::cancel_host_wait() {
  if (!host_waiter) return;
  std::lock_guard lock(host_waiter->mutex);
  if (host_waiter->state == HostWaiter::State::idle) return;
  host_waiter->cancelled = true;
  host_waiter->changed.notify_all();
}
void Session::drain_host_wait() {
  if (!host_waiter) return;
  cancel_host_wait();
  std::unique_lock lock(host_waiter->mutex);
  host_waiter->changed.wait(lock, [&] { return host_waiter->state != HostWaiter::State::requested; });
  host_waiter->state = HostWaiter::State::idle;
}
void Session::observe_host_frame(const XrFrameState& host, std::int64_t returned_ns,
                                 std::int64_t frame_chain_ns, bool led) {
  constexpr std::int64_t step = 1'000'000, decay = 250'000;
  if (host.predictedDisplayPeriod > 0 &&
      (!lead.base_period_ns || host.predictedDisplayPeriod < lead.base_period_ns))
    lead.base_period_ns = host.predictedDisplayPeriod;
  const auto period = lead.base_period_ns;
  // Adapt in gameplay only; loading and menus skip displays for other reasons.
  if (lead.host_valid && !lead.pipelined && period > 0 && state == XR_SESSION_STATE_FOCUSED) {
    // A runtime that skips a display for a late frame predicts two periods on.
    // Only a frame that fit a period missed on the deadline: a longer one is a
    // hitch or throughput-bound, which lead cannot help and would only delay.
    // The average must fit too: loading frames (HITMAN startup averages 64 ms)
    // skip displays whatever the lead, including their occasional short frames.
    const bool skipped = host.predictedDisplayTime - lead.host.predictedDisplayTime > period * 3 / 2;
    const bool steady = lead.chain_ns > 0 && lead.chain_ns < period * 9 / 10;
    if (skipped && steady && frame_chain_ns > 0 && frame_chain_ns < period * 9 / 10) {
      ++lead.misses;
      ++lead.recent_misses;
      lead.clean = 0;
    } else if (!skipped && ++lead.clean >= 1800 && lead.lead_ns > 0 && !lead.fixed_ns) {
      lead.lead_ns = std::max<std::int64_t>(0, lead.lead_ns - decay);
      lead.clean = 0;
    }
    if (lead.fixed_ns) {
      lead.recent_misses = 0;
    } else {
      // Lead is a deadline shortfall, at most half a period. At the cap, ten
      // consecutive two-miss epochs (each under 60 frames, no clean window in
      // between) mean lead is not the cure: drop it for 7200 frames.
      const auto cap = period / 2;
      if (lead.cooldown) --lead.cooldown;
      if (lead.recent_misses >= 2) {
        if (lead.lead_ns >= cap) {
          if (++lead.saturated_windows >= 10) {
            lead.lead_ns = 0;
            lead.saturated_windows = 0;
            lead.cooldown = 7200;
            log::info("xr", "Frame lead released: displays still missed at {:.2f}ms lead", cap / 1e6);
          }
        } else if (!lead.cooldown) {
          lead.lead_ns = std::min<std::int64_t>(lead.lead_ns + step, cap);
        }
        lead.recent_misses = lead.window_frames = 0;
      } else if (++lead.window_frames >= 60) {
        lead.recent_misses = lead.window_frames = lead.saturated_windows = 0;
      }
    }
  }
  std::int64_t display;
  if (!led && XR_SUCCEEDED(time_to_monotonic(host.predictedDisplayTime, &display))) {
    const auto window = display - returned_ns;
    if (window > 0) lead.window_ns = lead.window_ns ? (lead.window_ns * 7 + window) / 8 : window;
  }
  lead.host = host;
  lead.host.next = nullptr;
  lead.host_valid = true;
}
XrResult Session::begin_session(const XrSessionBeginInfo *info) {
  if (!handle) return XR_ERROR_HANDLE_INVALID;
  auto result = xrBeginSession(handle, info);
  if (XR_SUCCEEDED(result))
    log::info("xr", "Host xrBeginSession result={}", int(result));
  else
    // The session never runs and the game waits for frames that never come
    // (Monado whose compositor could not start). Say so instead of idling.
    log::error("xr", "Host runtime refused xrBeginSession (result={}); no frames will be rendered", int(result));
  note(result);
  if (XR_SUCCEEDED(result)) {
    running = true;
    view_configuration = info->primaryViewConfigurationType;
    reset_frames();
  }
  return result;
}
XrResult Session::end_session() {
  if (!handle) return XR_ERROR_HANDLE_INVALID;
  drain_host_wait();
  auto result = xrEndSession(handle);
  note(result);
  if (XR_SUCCEEDED(result)) {
    running = false;
    reset_frames();
  }
  return result;
}
XrResult Session::wait(const XrFrameWaitInfo *info, XrFrameState *out) {
  xr_call_trace::Call trace("session.xrWaitFrame");
  return trace.finish([&]() -> XrResult {
  std::unique_lock lock(mutex);
  if (wait_in_progress)
    return XR_ERROR_CALL_ORDER_INVALID;
  // OpenXR xrWaitFrame: the next wait blocks until the previous BeginFrame,
  // independently of EndFrame (specs/1.1/man/html/xrWaitFrame.html). This
  // single recursion level is the only lock held here; callers hold none.
  auto *thread = GuestThread::current();
  // A begun frame that owes its host wait takes it in xrEndFrame; the next
  // wait follows that, never precedes it.
  while ((pending_wait || (lead.begun_owed && phase == Phase::begun)) && running && handle &&
         XR_SUCCEEDED(lost)) {
    if (!thread) {
      frame_changed.wait(lock);
      continue;
    }
    const auto epoch = std::atomic_ref(frame_epoch).load(std::memory_order_acquire);
    lock.unlock();
    // Interrupts deliver signals here, or unwind the thread on process exit.
    futex_wait(*thread, h2g(&frame_epoch), epoch, -1);
    lock.lock();
  }
  if (!handle) return XR_ERROR_HANDLE_INVALID;
  if (XR_FAILED(lost)) return lost;
  if (!running) return XR_ERROR_SESSION_NOT_RUNNING;
  if (wait_in_progress) return XR_ERROR_CALL_ORDER_INVALID;
  if (phase == Phase::begun && !lead.pipelined && state == XR_SESSION_STATE_FOCUSED &&
      ++lead.overlapped_waits >= 10) {
    lead.pipelined = true;
    lead.lead_ns = 0;
  }
  wait_in_progress = true;
  const auto generation = run_generation;
  lead.plain_waits = !out->next && (!info || !info->next);
  // A requested host wait for this frame is already with the runtime: never
  // issue a second one. Led frames start lead ms before it is due back.
  const bool proxied = host_wait_pending();
  std::int64_t lead_start = -1;
  XrFrameState led_frame{XR_TYPE_FRAME_STATE};
  // OpenXR only: its xrEndFrame is the sole led End caller and holds this
  // mutex exactly once, which it releases while taking the host wait.
  if (proxied && lead.host_valid && lead.window_ns > 0 && thread && lead.plain_waits &&
      lead.host.shouldRender && (state == XR_SESSION_STATE_VISIBLE || state == XR_SESSION_STATE_FOCUSED)) {
    led_frame = lead.host;
    led_frame.predictedDisplayTime += lead.host.predictedDisplayPeriod;
    bool host_failed = false;
    {
      std::lock_guard waiter_lock(host_waiter->mutex);
      if (host_waiter->state == HostWaiter::State::done) {
        host_failed = XR_FAILED(host_waiter->result);
        if (!host_failed) led_frame = host_waiter->frame;
      }
    }
    std::int64_t display;
    const auto period = led_frame.predictedDisplayPeriod;
    // A failed host wait is returned now rather than after another guest frame.
    if (!host_failed && XR_SUCCEEDED(time_to_monotonic(led_frame.predictedDisplayTime, &display))) {
      const auto start = display - lead.window_ns - lead.lead_ns;
      // A stale prediction (the guest paused) takes the host's result instead.
      if (start > monotonic_ns() - period / 2) lead_start = start;
    }
  }
  if (lead_start >= 0) {
    while (monotonic_ns() < lead_start && running && handle && XR_SUCCEEDED(lost) &&
           generation == run_generation) {
      const auto epoch = std::atomic_ref(frame_epoch).load(std::memory_order_acquire);
      lock.unlock();
      futex_wait(*thread, h2g(&frame_epoch), epoch, lead_start);
      lock.lock();
    }
    wait_in_progress = false;
    if (!handle) return XR_ERROR_HANDLE_INVALID;
    if (XR_FAILED(lost)) return lost;
    if (!running || generation != run_generation) return XR_ERROR_SESSION_NOT_RUNNING;
    out->predictedDisplayTime = led_frame.predictedDisplayTime;
    out->predictedDisplayPeriod = led_frame.predictedDisplayPeriod;
    out->shouldRender = led_frame.shouldRender;
    trace.frame(out->predictedDisplayTime, out->predictedDisplayPeriod, out->shouldRender);
    pending_frame = *out;
    pending_frame.next = nullptr;
    pending_wait = true;
    lead.owed = true;
    lead.pending_start_ns = monotonic_ns();
    ++lead.led_frames;
    if (phase != Phase::begun) {
      frame = pending_frame;
      phase = Phase::waited;
    }
    return XR_SUCCESS;
  }
  // A simulation thread may wait for frame N+1 while the render thread ends N.
  // No API-state or GPU queue lock may be held across the runtime's pacing wait.
  lock.unlock();
  auto result = [&] {
    vulkan::ScopedHostWait timing("xr_wait_frame");
    if (!proxied) return xrWaitFrame(handle, info, out);
    // Unled: the requested wait is this frame's host wait.
    XrFrameState taken{XR_TYPE_FRAME_STATE};
    std::int64_t returned;
    std::uint64_t taken_generation;
    const auto r = take_host_wait(taken, returned, taken_generation);
    out->predictedDisplayTime = taken.predictedDisplayTime;
    out->predictedDisplayPeriod = taken.predictedDisplayPeriod;
    out->shouldRender = taken.shouldRender;
    return taken_generation == generation ? r : XR_ERROR_SESSION_NOT_RUNNING;
  }();
  lock.lock();
  wait_in_progress = false;
  note(result);
  if (!handle) result = XR_ERROR_HANDLE_INVALID;
  else if (XR_FAILED(lost)) result = lost;
  else if (!running || generation != run_generation) result = XR_ERROR_SESSION_NOT_RUNNING;
  if (XR_SUCCEEDED(result)) {
    trace.frame(out->predictedDisplayTime, out->predictedDisplayPeriod, out->shouldRender);
    const auto host_should_render = out->shouldRender;
    // Forward the runtime's render request independently of delivered session
    // events. A compositor may need an initial image before reporting VISIBLE;
    // suppressing that request can prevent startup synchronization entirely.
    if (venture::environment::getenv("VENTURE_TRACE_XR_FRAMES")) {
      static std::atomic<unsigned> traced_waits{};
      static std::atomic<int> previous{-1};
      const auto key = (int(state) << 2) | (bool(host_should_render) << 1) | bool(out->shouldRender);
      const bool changed = previous.exchange(key, std::memory_order_relaxed) != key;
      const auto sample = traced_waits.fetch_add(1, std::memory_order_relaxed);
      // A runtime may change its application period without changing focus or
      // render state. Keep bounded periodic evidence beyond the startup burst.
      if (sample < 64 || changed || sample % 300 == 0)
        log::info("xr-frame", "host_wait state={} host_render={} guest_render={} period={}",
            int(state), bool(host_should_render), bool(out->shouldRender), out->predictedDisplayPeriod);
    }
    pending_frame = *out;
    pending_frame.next = nullptr;
    pending_wait = true;
    lead.owed = false;
    lead.pending_start_ns = monotonic_ns();
    observe_host_frame(*out, lead.pending_start_ns, lead.last_chain_ns, false);
    if (phase != Phase::begun) {
      frame = pending_frame;
      phase = Phase::waited;
    }
    observe_display_period(out->predictedDisplayPeriod, DisplayTiming::Source::predicted);
  }
  lock.unlock();
  if (XR_SUCCEEDED(result))
    if (auto observer = frame_observer.load()) {
      std::int64_t time;
      if (XR_SUCCEEDED(time_to_monotonic(out->predictedDisplayTime, &time)))
        observer(time, display_timing.period.load());
    }
  return result;

  }());
}
XrResult Session::begin(const XrFrameBeginInfo *info) {
  xr_call_trace::Call trace("session.xrBeginFrame");
  return trace.finish([&]() -> XrResult {
  if (XR_FAILED(lost)) return lost;
  if (!running) return XR_ERROR_SESSION_NOT_RUNNING;
  if (!pending_wait)
    return XR_ERROR_CALL_ORDER_INVALID;
  if (lead.owed) {
    // The host frame begins in xrEndFrame. An abandoned led frame never
    // reached the host; the next wait paces on the host again.
    const bool abandoned = phase == Phase::begun;
    frame = pending_frame;
    pending_wait = false;
    phase = Phase::begun;
    render_thread = std::this_thread::get_id();
    lead.owed = false;
    lead.begun_owed = true;
    lead.frame_start_ns = lead.pending_start_ns;
    if (abandoned) lead.host_valid = false;
    notify_frame_waiters();
    return abandoned ? XR_FRAME_DISCARDED : XR_SUCCESS;
  }
  HostQueueLock queue(uses_shared_gpu);
  if (!queue)
    return XR_ERROR_RUNTIME_FAILURE;
  bool completed_discard = false;
  // A guest that begins a frame over an abandoned one while nothing is
  // visible yet gets XR_FRAME_DISCARDED, and the abandoned host frame is
  // ended with no layers. Quest titles (HITMAN) skip xrEndFrame until they
  // are visible, but Monado synchronizes the session only after a first
  // completed frame, so they stayed READY forever. No image, visibility
  // event or counted guest frame is invented.
  if (phase == Phase::begun && owner == SessionOwner::openxr &&
      (state == XR_SESSION_STATE_READY || state == XR_SESSION_STATE_SYNCHRONIZED)) {
    auto& host = host_xr();
    std::uint32_t count = 0;
    auto modes_result = xrEnumerateEnvironmentBlendModes(host.instance, host.system,
        view_configuration, 0, &count, nullptr);
    if (XR_SUCCEEDED(modes_result) && count) {
      std::vector<XrEnvironmentBlendMode> modes(count);
      modes_result = xrEnumerateEnvironmentBlendModes(host.instance, host.system,
          view_configuration, count, &count, modes.data());
      if (XR_SUCCEEDED(modes_result) && count) {
        XrFrameEndInfo empty{XR_TYPE_FRAME_END_INFO, nullptr, frame.predictedDisplayTime,
                            modes.front(), 0, nullptr};
        auto ended = xrEndFrame(handle, &empty);
        note(ended);
        if (XR_FAILED(ended)) return ended;
        phase = Phase::waited;
        render_thread = {};
        completed_discard = true;
        // One per display period until the guest ends a frame; the first few and then a count.
        if (const auto n = ++completed_startup_frames; n <= 3 || n % 120 == 0)
          log::info("xr-frame", "completed abandoned startup host frame display_time={} layers=0 count={}",
                    empty.displayTime, n);
      }
    }
  }
  auto result = xrBeginFrame(handle, info);
  note(result);
  if (XR_SUCCEEDED(result)) {
    frame = pending_frame;
    pending_wait = false;
    phase = Phase::begun;
    render_thread = std::this_thread::get_id();
    lead.begun_owed = false;
    lead.frame_start_ns = lead.pending_start_ns;
    notify_frame_waiters();
  }
  return result == XR_SUCCESS && completed_discard ? XR_FRAME_DISCARDED : result;

  }());
}
XrResult Session::end(const XrFrameEndInfo *info, std::shared_ptr<const void> owned) {
  xr_call_trace::Call trace("session.xrEndFrame");
  return trace.finish([&]() -> XrResult {
  if (info) { trace.display(info->displayTime); trace.layers(info->layerCount); }
  // OpenXR Frame Submission (rendering.adoc): synchronized Begin/End may run
  // on different threads. Callers hold mutex; phase enforces the call order.
  if (XR_FAILED(lost)) return lost;
  if (!running) return XR_ERROR_SESSION_NOT_RUNNING;
  if (phase != Phase::begun)
    return XR_ERROR_CALL_ORDER_INVALID;
  if (lead.frame_start_ns) {
    const auto chain = monotonic_ns() - lead.frame_start_ns;
    lead.frame_start_ns = 0;
    lead.chain_ns = lead.chain_ns ? (lead.chain_ns * 7 + chain) / 8 : chain;
    lead.previous_chain_ns = lead.last_chain_ns;
    lead.last_chain_ns = chain;
  }
  // A led frame takes its host wait and begin now, with its GPU work already
  // queued. As in wait(), the pacing wait holds neither this mutex (the
  // caller's single hold) nor a GPU queue lock; begun_owed keeps any other
  // waiter behind it until the host frame has begun.
  XrFrameEndInfo led_info;
  if (lead.begun_owed) {
    const auto generation = run_generation;
    const auto host = handle;
    XrFrameState host_frame{XR_TYPE_FRAME_STATE};
    std::int64_t returned = 0;
    const bool proxied = host_wait_pending();
    mutex.unlock();
    auto waited = [&] {
      vulkan::ScopedHostWait timing("xr_wait_frame");
      if (!proxied) return xrWaitFrame(host, nullptr, &host_frame);
      std::uint64_t taken_generation;
      const auto r = take_host_wait(host_frame, returned, taken_generation);
      return taken_generation == generation ? r : XR_ERROR_SESSION_NOT_RUNNING;
    }();
    if (!proxied) returned = monotonic_ns();
    mutex.lock();
    note(waited);
    // A take cancelled by a known loss returns the loss, as a plain wait does.
    if (!handle) waited = XR_ERROR_HANDLE_INVALID;
    else if (XR_FAILED(lost)) waited = lost;
    else if (XR_SUCCEEDED(waited) && (!running || generation != run_generation))
      waited = XR_ERROR_SESSION_NOT_RUNNING;
    if (XR_SUCCEEDED(waited)) {
      // This host frame follows the previous guest frame's submission. A
      // requested wait returned at the runtime's own wake-up.
      observe_host_frame(host_frame, returned, lead.previous_chain_ns, !proxied);
      observe_display_period(host_frame.predictedDisplayPeriod, DisplayTiming::Source::predicted);
      HostQueueLock begin_queue(uses_shared_gpu);
      if (!begin_queue) waited = XR_ERROR_RUNTIME_FAILURE;
      else waited = xrBeginFrame(handle, nullptr);
      note(waited);
    }
    lead.begun_owed = false;
    if (XR_FAILED(waited)) {
      // A reset session already retired this frame.
      if (generation == run_generation) {
        phase = pending_wait ? Phase::waited : Phase::idle;
        if (pending_wait) frame = pending_frame;
        render_thread = {};
      }
      lead.host_valid = false;
      ++end_errors;
      notify_frame_waiters();
      return waited;
    }
    notify_frame_waiters();
    // The runtime reprojects each layer from the pose it was rendered with;
    // xrEndFrame takes the host's display time for this host frame.
    if (const auto delta = std::abs(host_frame.predictedDisplayTime - frame.predictedDisplayTime);
        delta > host_frame.predictedDisplayPeriod / 4) {
      ++lead.mispredicted;
      lead.max_misprediction_ns = std::max<std::int64_t>(lead.max_misprediction_ns, delta);
    }
    led_info = *info;
    led_info.displayTime = host_frame.predictedDisplayTime;
    info = &led_info;
  }
  // An asynchronous submission returns success now; a failed host End is the
  // result of the guest's next wait or End, which take this request.
  const bool async = owned && async_end_ready() && request_host_end(*info, std::move(owned));
  std::optional<HostQueueLock> queue;
  XrResult result = XR_SUCCESS;
  if (async) {
    ++lead.async_ends;
  } else {
    std::optional<vulkan::ScopedHostWait> lock_timing(std::in_place, "xr_end_queue_lock");
    queue.emplace(uses_shared_gpu);
    lock_timing.reset();
    if (!*queue)
      return XR_ERROR_RUNTIME_FAILURE;
    result = [&] { vulkan::ScopedHostWait timing("xr_end_frame"); return xrEndFrame(handle, info); }();
    dev_dump_frame(info, result);
    note(result);
  }
  // xrEndFrame recommends discarding a recoverably rejected frame instead
  // of retrying End with different parameters. Retire the begun binding on
  // every native End attempt; a queued Wait still belongs to the next frame.
  // Keep the original error and count only successful submissions below.
  phase = pending_wait ? Phase::waited : Phase::idle;
  if (pending_wait)
    frame = pending_frame;
  render_thread = {};
  if (lead.enabled) notify_frame_waiters();
  // The next host wait is issued as soon as this frame is submitted, never
  // before: SteamVR maps xrWaitFrame to WaitGetPoses and xrEndFrame to Submit,
  // and a wait issued between Begin and End attributed each submission to the
  // following frame, so every frame was presented twice (60 fps at 120 Hz).
  if (XR_SUCCEEDED(result) && lead_active() && !async) request_host_wait();
  if (XR_SUCCEEDED(result)) {
    ++frame_index;
    vulkan::timing_end_frame(info->displayTime, guest_swap_interval, display_timing.rate());
    if (frame_index == 1) { profile::mark("first_frame_submission"); startup_diagnostics::arm(); }
    startup_diagnostics::note_frame_submitted();
    if (profile::enabled) {
      for (std::uint32_t i = 0; i < info->layerCount; ++i)
        if (info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
          if (!profile_eye_seen) {
            profile_eye_seen = true;
            profile::mark("first_eye_frame");
          }
          profile::frame(frame_index);
          break;
        }
    }
    // Display time should advance with the PC's clock; a runtime whose clock
    // drifts makes games run fast or slow. Logged once: the rate between the
    // freshest frame (largest display-time lead) of a window 5-7 s after the
    // first frame and one 20 s later, as a stalled frame's display time is stale.
    if (!clock_fact_logged) {
      const auto now_ns = monotonic_ns();
      if (frame_index == 1) clock_start_ns = now_ns;
      const auto since = now_ns - clock_start_ns;
      auto sample = [&](ClockSample &best) {
        if (!best.ns || info->displayTime - best.display > now_ns - best.ns)
          best = {info->displayTime, now_ns};
      };
      if (since >= 5'000'000'000 && since < 7'000'000'000) sample(clock_first);
      else if (since >= 25'000'000'000 && since < 27'000'000'000) sample(clock_last);
      else if (since >= 27'000'000'000 && clock_first.ns && clock_last.ns) {
        clock_fact_logged = true;
        const double ratio = double(clock_last.display - clock_first.display) / double(clock_last.ns - clock_first.ns);
        log::info("facts", "Display clock rate {:.4f} over {:.0f} s; refresh {:.2f} Hz", ratio,
                  (clock_last.ns - clock_first.ns) / 1e9, display_timing.rate());
      }
    }
    if (frame_index == 1) {
      // The headset's per-eye field of view as the host reports it; in debug reports.
      XrReferenceSpaceCreateInfo view_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
      view_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
      view_info.poseInReferenceSpace.orientation.w = 1;
      XrSpace view_space{};
      if (XR_SUCCEEDED(xrCreateReferenceSpace(handle, &view_info, &view_space))) {
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                info->displayTime, view_space};
        XrViewState view_state{XR_TYPE_VIEW_STATE};
        XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        std::uint32_t count = 0;
        constexpr double degrees = 57.29577951308232;
        if (XR_SUCCEEDED(xrLocateViews(handle, &locate, &view_state, 2, &count, views)) && count == 2)
          log::info("facts", "Host eye FOV (left/right/up/down degrees): left eye {:.1f}/{:.1f}/{:.1f}/{:.1f}, "
                    "right eye {:.1f}/{:.1f}/{:.1f}/{:.1f}; eye separation {:.1f} mm",
                    views[0].fov.angleLeft * degrees, views[0].fov.angleRight * degrees,
                    views[0].fov.angleUp * degrees, views[0].fov.angleDown * degrees,
                    views[1].fov.angleLeft * degrees, views[1].fov.angleRight * degrees,
                    views[1].fov.angleUp * degrees, views[1].fov.angleDown * degrees,
                    std::hypot(views[1].pose.position.x - views[0].pose.position.x,
                               views[1].pose.position.y - views[0].pose.position.y,
                               views[1].pose.position.z - views[0].pose.position.z) * 1000);
        xrDestroySpace(view_space);
      }
    }
    if (frame_index == 1 || frame_index % 300 == 0) {
      const auto now = std::chrono::steady_clock::now();
      const auto seconds = last_report_frame
                               ? std::chrono::duration<double>(now - last_report_time).count()
                               : 0.;
      const auto rate = seconds > 0 ? (frame_index - last_report_frame) / seconds : 0.;
      log::info("xr", "Frames submitted={} xrEndFrame_errors={} submission_fps={:.2f} runtime_period_hz={:.2f}",
                frame_index, end_errors, rate, display_timing.rate());
      if (lead.enabled)
        log::info("xr", "Frame lead={:.2f}ms led={} missed_displays={} window={:.2f}ms chain={:.2f}ms "
                  "mispredicted={} max_misprediction={:.2f}ms async_ends={}",
                  lead.lead_ns / 1e6, lead.led_frames, lead.misses, lead.window_ns / 1e6, lead.chain_ns / 1e6,
                  lead.mispredicted, lead.max_misprediction_ns / 1e6, lead.async_ends);
      last_report_time = now;
      last_report_frame = frame_index;
      report_metrics(handle);
      profile::report_pipelines("periodic");
    }
  } else {
    ++end_errors;
  }
  return result;

  }());
}
XrResult create_host_swapchain(XrSession handle, XrSwapchainCreateInfo &info, XrSwapchain *out) {
  static std::mutex mutex;
  static XrSession listed{};
  static std::vector<std::int64_t> formats;
  // The input-attachment usage bit is valid only with XR_KHR_swapchain_usage_input_attachment_bit (or
  // the MND original) enabled; Monado refuses the swapchain otherwise. Quest offers it, so guests
  // ask for it through XR_META_vulkan_swapchain_create_info.
  const auto& enabled = host_xr().enabled_extensions;
  if ((info.usageFlags & XR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_KHR) &&
      !enabled.contains("XR_KHR_swapchain_usage_input_attachment_bit") &&
      !enabled.contains("XR_MND_swapchain_usage_input_attachment_bit")) {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true))
      log::warn("xr", "Host lacks XR_KHR_swapchain_usage_input_attachment_bit; swapchains are created without input-attachment usage");
    info.usageFlags &= ~XrSwapchainUsageFlags(XR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_KHR);
  }
  std::vector<std::int64_t> order{info.format};
  {
    std::lock_guard lock(mutex);
    if (listed != handle) {
      std::uint32_t n = 0;
      formats.clear();
      if (XR_SUCCEEDED(::xrEnumerateSwapchainFormats(handle, 0, &n, nullptr))) {
        formats.resize(n);
        if (XR_FAILED(::xrEnumerateSwapchainFormats(handle, n, &n, formats.data()))) n = 0;
        formats.resize(n);
      }
      listed = handle;
      std::string text;
      for (auto f : formats) text += (text.empty() ? "" : ",") + std::to_string(f);
      log::info("xr", "Host swapchain formats: {}", text);
    }
    switch (info.format) {
    case VK_FORMAT_R8G8B8A8_UNORM: order.insert(order.end(), {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB}); break;
    case VK_FORMAT_B8G8R8A8_UNORM: order.insert(order.end(), {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB}); break;
    case VK_FORMAT_R8G8B8A8_SRGB: order.insert(order.end(), {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM}); break;
    case VK_FORMAT_B8G8R8A8_SRGB: order.insert(order.end(), {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}); break;
    default: break;
    }
    auto offered = [&](std::int64_t f) { return std::find(formats.begin(), formats.end(), f) != formats.end(); };
    std::stable_partition(order.begin() + 1, order.end(), offered);
  }
  const auto wanted = info.format;
  XrResult result = XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
  for (auto format : order) {
    info.format = format;
    result = dev_create_swapchain(handle, &info, out);
    if (result != XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED) break;
  }
  if (XR_FAILED(result)) info.format = wanted;
  else if (info.format != wanted)
    log::info("xr", "Host swapchain format {} unavailable; using {} with a converting copy", wanted, info.format);
  return result;
}
} // namespace venture::xr
