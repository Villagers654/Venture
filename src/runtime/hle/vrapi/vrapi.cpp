#include "common/first_frame_progress.h"
#include "common/log.h"
#include "core/thread.h"
#include "core/av_telemetry.h"
#include "core/wait.h"
#include "hle/openxr/state.h"
#include "hle/vrapi/state.h"
#include "core/engine_policy.h"
#include "hle/vrapi/vulkan_images.h"
#include "core/process.h"
#include "xr/clock.h"
#include "xr/haptic_output.h"
#include "xr/menu_chord.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

namespace venture::hle::vrapi {
State &state() {
  static State s;
  return s;
}
bool valid(Mobile *m) { return m && m == state().mobile && session().owner == SessionOwner::vrapi; }
void failure(const char *operation, std::int32_t error) {
  log::warn("vrapi", "{} failed: {}", operation, error);
}
std::int32_t result(XrResult r, const char *operation) {
  if (XR_SUCCEEDED(r))
    return success;
  failure(operation, r);
  if (r == XR_ERROR_HANDLE_INVALID || r == XR_ERROR_VALIDATION_FAILURE)
    return invalid;
  if (r == XR_ERROR_SESSION_NOT_RUNNING || r == XR_ERROR_SESSION_NOT_READY)
    return not_ready;
  if (r == XR_ERROR_CALL_ORDER_INVALID)
    return invalid_operation;
  if (r == XR_ERROR_FUNCTION_UNSUPPORTED || r == XR_ERROR_FEATURE_UNSUPPORTED)
    return unsupported;
  return unavailable;
}
Pose pose(XrPosef p) {
  return {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
          {p.position.x, p.position.y, p.position.z}};
}
XrPosef pose(Pose p) {
  return {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
          {p.position.x, p.position.y, p.position.z}};
}
double seconds_from_time(XrTime t) {
  std::int64_t ns = 0;
  return XR_SUCCEEDED(time_to_monotonic(t, &ns)) ? double(ns) * 1e-9 : 0;
}
XrTime time_from_seconds(double t) {
  if (!std::isfinite(t) || t < 0 || t > double(std::numeric_limits<std::int64_t>::max()) * 1e-9)
    return 0;
  auto ns = t == 0 ? monotonic_ns() : static_cast<std::int64_t>(t * 1e9);
  XrTime xr = 0;
  return XR_SUCCEEDED(monotonic_to_time(ns, &xr)) ? xr : 0;
}
XrResult pump_events() {
  auto &s = session();
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  XrResult r;
  while ((r = ::xrPollEvent(host_xr().instance, &event)) == XR_SUCCESS) {
    if (!s.deliver_host_event(event)) {
      event = {XR_TYPE_EVENT_DATA_BUFFER};
      continue;
    }
    s.observe_host_event(event);
    if (event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
      const auto &e = reinterpret_cast<const XrEventDataInteractionProfileChanged &>(event);
      venture::xr::clear_haptic_bindings(e.session);
      venture::xr::note_menu_profiles_changed(e.session);
    }
    if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
      const auto &e = reinterpret_cast<const XrEventDataSessionStateChanged &>(event);
      if (e.session == s.handle) {
        auto visible = [](XrSessionState state) {
          return state == XR_SESSION_STATE_VISIBLE || state == XR_SESSION_STATE_FOCUSED;
        };
        if (visible(s.state) != visible(e.state)) {
          state().events.push_back(visible(e.state) ? 2 : 3);
          // VrApi's recenter-on-mount contract also notifies applications
          // through RECENTER_COUNT (Mobile SDK migration guide, 1.0.4).
          // MOUNTED maps to host visibility, as in GetSystemStatusInt. The
          // host owns its LOCAL origin; report the new mount without moving
          // it again or changing an application's selected tracking transform.
          if (visible(e.state)) ++state().recenter_count;
        }
        if ((s.state == XR_SESSION_STATE_FOCUSED) != (e.state == XR_SESSION_STATE_FOCUSED))
          state().events.push_back(e.state == XR_SESSION_STATE_FOCUSED ? 4 : 5);
        s.state = e.state;
      log::info("xr", "Host session state={}", int(e.state));
      startup_diagnostics::note_host_session_state(int(e.state));
        // Session::begin_session/end_session reset the shared frame state, so
        // a frame waited before STOPPING cannot gate waits after READY.
        if (e.state == XR_SESSION_STATE_READY && !s.running) {
          XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO, nullptr,
                                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
          auto start = s.begin_session(&bi);
          if (XR_FAILED(start))
            return start;
        }
        if (e.state == XR_SESSION_STATE_STOPPING && s.running) {
          auto stop = s.end_session();
          if (XR_FAILED(stop))
            return stop;
          state().token = state().next_token = state().frame_bound = false;
          wake_frame_waiters();
        }
        if (e.state == XR_SESSION_STATE_LOSS_PENDING) s.lose(XR_ERROR_SESSION_LOST);
        if (e.state == XR_SESSION_STATE_LOSS_PENDING || e.state == XR_SESSION_STATE_EXITING) {
          s.running = false;
          wake_frame_waiters();
        }
      }
    }
    event = {XR_TYPE_EVENT_DATA_BUFFER};
  }
  return r == XR_EVENT_UNAVAILABLE ? XR_SUCCESS : r;
}
void wake_frame_waiters() { session().notify_frame_waiters(); }
// VrApi.h Tracking permits prediction queries on any thread. Frame Timing
// assigns legacy pacing to SubmitFrame2; PhaseSync moves it to WaitFrame.
// Only wait/submission obtains a host frame. Prediction extrapolates the last
// host display time and period without entering the runtime's pacing wait.
// No API/session lock spans that wait; frame_epoch wakes guest waiters on
// begin, completion, session restart, loss or teardown.
void complete_frame() {
  auto &v = state();
  v.token = v.next_token;
  v.frame_bound = v.next_token;
  if (v.next_token) {
    v.prediction_index = v.frame_index = v.next_frame_index;
    v.predicted_time = v.next_predicted_time;
    v.predicted_period = v.next_predicted_period;
    v.frame_ready_ns = monotonic_ns();
    v.next_token = false;
  }
  wake_frame_waiters();
}
XrResult ensure_frame(std::uint64_t frame_index, Mobile *expected, FrameUse use) {
  auto &v = state();
  auto &s = session();
  std::unique_lock lock(v.mutex);
  std::unique_lock sl(s.mutex);
  const auto mobile = expected ? expected : v.mobile;
  // When this Wait(N+k) first found N unbegun.
  std::int64_t blocked_since = 0;
  std::uint64_t blocked_frame = 0;
  for (;;) {
    if (!valid(mobile)) return XR_ERROR_HANDLE_INVALID;
    auto events = pump_events();
    if (XR_FAILED(events)) return events;
    if (XR_FAILED(s.lost)) return s.lost;
    if (!s.running) return XR_ERROR_SESSION_NOT_RUNNING;
    if (v.final_frame) return XR_ERROR_CALL_ORDER_INVALID;
    std::int64_t deadline = -1;
    if (v.token) {
      if (v.frame_index == frame_index) return XR_SUCCESS;
      // Submission must match the current binding, never the queued wait.
      if (use == FrameUse::submit || frame_index < v.frame_index)
        return XR_ERROR_CALL_ORDER_INVALID;
      if (v.next_token && v.next_frame_index == frame_index) return XR_SUCCESS;
      if (use == FrameUse::wait && !v.next_token && !v.wait_in_progress &&
          s.phase == Session::Phase::waited) {
        // PhaseSync: Wait(N+k) blocks below until the renderer begins N, however
        // far the index advanced. UE4 can also abandon an unbegun N (splash
        // frame 1197, then simulation frame 1336). Only a frame still unbegun
        // long after its display time is known to be abandoned: balance its
        // host wait with an empty frame, then obtain a fresh prediction. A slow
        // N-1 (seconds per frame on SwiftShader) delays Begin(N) past N's
        // display time, so the bound also starts when N-1 completed. A
        // simulation stall (load, GC) holds N's render commands back, so it
        // starts no earlier than this wait finding N unbegun.
        if (!blocked_since || blocked_frame != v.frame_index) {
          blocked_since = monotonic_ns();
          blocked_frame = v.frame_index;
        }
        std::int64_t display = 0;
        if (XR_FAILED(time_to_monotonic(s.frame.predictedDisplayTime, &display))) display = 0;
        const auto abandoned =
            std::max({display, v.frame_ready_ns, blocked_since}) + abandoned_frame_ns;
        if (monotonic_ns() >= abandoned) {
          auto discarded = s.begin(nullptr);
          if (XR_FAILED(discarded)) return discarded;
          XrFrameEndInfo empty{XR_TYPE_FRAME_END_INFO};
          empty.displayTime = s.frame.predictedDisplayTime;
          empty.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
          discarded = s.end(&empty);
          if (XR_FAILED(discarded)) return discarded;
          complete_frame();
          continue;
        }
        deadline = abandoned;
      }
    }
    if (!v.wait_in_progress && !v.next_token && !s.pending_wait &&
        (!v.token || s.phase == Session::Phase::begun)) break;
    // PhaseSync simulation may reach Wait(N+1) before the renderer begins N.
    auto *thread = GuestThread::current();
    if (!thread) return XR_ERROR_CALL_ORDER_INVALID;
    const auto epoch = std::atomic_ref(s.frame_epoch).load(std::memory_order_acquire);
    sl.unlock();
    lock.unlock();
    futex_wait(*thread, h2g(&s.frame_epoch), epoch, deadline);
    lock.lock();
    sl.lock();
  }
  v.wait_in_progress = true;
  XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
  XrFrameState frame{XR_TYPE_FRAME_STATE};
  sl.unlock();
  lock.unlock();
  auto r = s.wait(&wi, &frame);
  lock.lock();
  sl.lock();
  v.wait_in_progress = false;
  if (!valid(mobile)) r = XR_ERROR_HANDLE_INVALID;
  if (XR_SUCCEEDED(r)) {
    const bool prediction_sampled = !v.token && v.prediction_input_sampled;
    if (v.token) {
      v.next_token = true;
      v.next_frame_index = frame_index;
      v.next_predicted_time = frame.predictedDisplayTime;
      v.next_predicted_period = frame.predictedDisplayPeriod;
    } else {
      v.token = true;
      v.frame_bound = true;
      v.prediction_index = v.frame_index = frame_index;
      v.predicted_time = frame.predictedDisplayTime;
      v.predicted_period = frame.predictedDisplayPeriod;
      v.frame_ready_ns = monotonic_ns();
    }
    // A legacy prediction may have already sampled this cycle's input.
    // Reuse it even if a timing lookup requested a different frame index.
    // Queued PhaseSync waits still acquire their next input snapshot.
    if (!prediction_sampled) sync_input();
    v.input_frame_index = static_cast<std::int64_t>(frame_index);
    v.prediction_input_sampled = false;
  }
  wake_frame_waiters();
  return r;
}
XrResult set_app_space(XrPosef offset) {
  auto &v = state();
  XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  info.referenceSpaceType =
      v.tracking_space == 0 ? XR_REFERENCE_SPACE_TYPE_LOCAL : XR_REFERENCE_SPACE_TYPE_STAGE;
  info.poseInReferenceSpace = offset;
  XrSpace replacement{};
  auto r = ::xrCreateReferenceSpace(session().handle, &info, &replacement);
  if (XR_SUCCEEDED(r)) {
    if (v.app)
      ::xrDestroySpace(v.app);
    v.app = replacement;
    v.transform = offset;
  }
  return r;
}
namespace {
Matrix view_matrix(XrPosef p) {
  p = inverse(p);
  auto q = p.orientation;
  Matrix m{};
  m.m[0][0] = 1 - 2 * (q.y * q.y + q.z * q.z);
  m.m[0][1] = 2 * (q.x * q.y - q.z * q.w);
  m.m[0][2] = 2 * (q.x * q.z + q.y * q.w);
  m.m[0][3] = p.position.x;
  m.m[1][0] = 2 * (q.x * q.y + q.z * q.w);
  m.m[1][1] = 1 - 2 * (q.x * q.x + q.z * q.z);
  m.m[1][2] = 2 * (q.y * q.z - q.x * q.w);
  m.m[1][3] = p.position.y;
  m.m[2][0] = 2 * (q.x * q.z - q.y * q.w);
  m.m[2][1] = 2 * (q.y * q.z + q.x * q.w);
  m.m[2][2] = 1 - 2 * (q.x * q.x + q.y * q.y);
  m.m[2][3] = p.position.z;
  m.m[3][3] = 1;
  return m;
}
Matrix projection_matrix(XrFovf f) {
  float l = std::tan(f.angleLeft), r = std::tan(f.angleRight), u = std::tan(f.angleUp),
        d = std::tan(f.angleDown);
  Matrix m{};
  m.m[0][0] = 2 / (r - l);
  m.m[0][2] = (r + l) / (r - l);
  m.m[1][1] = 2 / (u - d);
  m.m[1][2] = (u + d) / (u - d);
  m.m[2][2] = -1;
  m.m[2][3] = -0.2f;
  m.m[3][2] = -1;
  return m;
}
std::uint32_t flags(XrSpaceLocationFlags flags) {
  return ((flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) ? 1u : 0u) |
         ((flags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) ? 2u : 0u) |
         ((flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) ? 4u : 0u) |
         ((flags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ? 8u : 0u);
}
} // namespace
Tracking tracking_at(XrTime time, XrSpace target) {
  Tracking out{};
  XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
  XrSpaceLocation location{XR_TYPE_SPACE_LOCATION, &velocity};
  if (!time || !target || XR_FAILED(::xrLocateSpace(target, state().app, time, &location)))
    return out;
  // A Quest never reports NaN; undefined host values become neutral (hle/openxr/spaces.cpp).
  hle::openxr::sanitize(location.locationFlags, location.pose);
  out.status = flags(location.locationFlags) | 128;
  out.head.pose = pose(location.pose);
  out.head.time = seconds_from_time(time);
  out.head.prediction = out.head.time - double(monotonic_ns()) * 1e-9;
  if (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT)
    out.head.linear_velocity = {velocity.linearVelocity.x, velocity.linearVelocity.y,
                                velocity.linearVelocity.z};
  if (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT)
    out.head.angular_velocity = {velocity.angularVelocity.x, velocity.angularVelocity.y,
                                 velocity.angularVelocity.z};
  if (target == state().view) {
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO, nullptr,
                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, time, state().app};
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    std::uint32_t count = 0;
    if (XR_SUCCEEDED(::xrLocateViews(session().handle, &li, &vs, 2, &count, views)) && count == 2) {
      hle::openxr::sanitize_views(vs, count, views);
      for (int eye = 0; eye < 2; ++eye) {
        out.eye[eye].view = view_matrix(views[eye].pose);
        out.eye[eye].projection = projection_matrix(views[eye].fov);
      }
    }
  }
  return out;
}
namespace guest {
std::int32_t vrapi_Initialize(const Init *info) {
  std::lock_guard lock(state().mutex);
  if (!info || info->type != 1 || info->product != 1 || info->major != 1 || !info->java.vm ||
      !info->java.activity)
    return -1;
  const bool vulkan = info->graphics_api == vulkan_api && process_config().isa() == GuestIsa::a64;
  if (info->graphics_api != 0x10000 + 0x300 && info->graphics_api != 0x10000 + 0x200 && !vulkan) {
    failure("Initialize graphics API", unsupported);
    return -1;
  }
  if (!host_xr().available) {
    failure("Initialize HMD", unavailable);
    return -1;
  }
  state().init = *info;
  if (!state().initialized) invalidate_host_procedures();
  state().initialized = true;
  return success;
}
void vrapi_Shutdown() {
  std::lock_guard lock(state().mutex);
  if (state().mobile)
    vrapi_LeaveVrMode(state().mobile);
  if (state().mobile)
    return;
  while (!state().rings.empty()) {
    auto size = state().rings.size();
    vrapi_DestroyTextureSwapChain(state().rings.begin()->first);
    if (state().rings.size() == size)
      return;
  }
  if (state().initialized) invalidate_host_procedures();
  state().initialized = false;
  vrapi_DestroySystemVulkan();
  state().properties.clear();
  state().float_properties.clear();
  state().events.clear();
}
Mobile *vrapi_EnterVrMode(const Mode *mode) {
  std::unique_lock lock(state().mutex);
  std::unique_lock sl(session().mutex);
  auto &v = state();
  const bool vulkan = v.init.graphics_api == vulkan_api;
  if (!v.initialized || !mode || mode->type != (vulkan ? 5 : 2) || !mode->java.vm || !mode->java.activity ||
      !xr_images()) {
    failure("EnterVrMode", not_initialized);
    return nullptr;
  }
  if (v.mobile) {
    failure("EnterVrMode duplicate", invalid_operation);
    return nullptr;
  }
  XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
  if (vulkan && XR_FAILED(bind_vulkan_queue(*reinterpret_cast<const ModeVulkan *>(mode), binding))) {
    failure("EnterVrMode Vulkan queue", invalid);
    return nullptr;
  }
  if (XR_FAILED(session().create(SessionOwner::vrapi, vulkan ? &binding : nullptr)))
    return nullptr;
  v.mobile = reinterpret_cast<Mobile *>(++v.generation);
  v.token = v.next_token = false;
  v.frame_bound = false;
  v.predicted_time = v.predicted_period = 0;
  v.input_frame_index = -1;
  v.prediction_input_sampled = false;
  v.tracking_space = 0;
  v.final_frame = false;
  v.submissions.clear();
  auto create = [&](XrReferenceSpaceType type, XrSpace &out) {
    XrReferenceSpaceCreateInfo ci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    ci.referenceSpaceType = type;
    ci.poseInReferenceSpace.orientation.w = 1;
    return ::xrCreateReferenceSpace(session().handle, &ci, &out);
  };
  if (XR_FAILED(create(XR_REFERENCE_SPACE_TYPE_LOCAL, v.local)) ||
      XR_FAILED(create(XR_REFERENCE_SPACE_TYPE_VIEW, v.view))) {
    vrapi_LeaveVrMode(v.mobile);
    return nullptr;
  }
  create(XR_REFERENCE_SPACE_TYPE_STAGE, v.stage);
  if (XR_FAILED(set_app_space({{0, 0, 0, 1}, {0, 0, 0}})) || XR_FAILED(initialize_input()) ||
      XR_FAILED(pump_events())) {
    vrapi_LeaveVrMode(v.mobile);
    return nullptr;
  }
  const auto mobile = v.mobile;
  sl.unlock();
  lock.unlock();
  if (auto* thread = GuestThread::current()) engine_policy::on_vrapi_session(*thread);
  return mobile;
}
void vrapi_LeaveVrMode(Mobile *mobile) {
  std::lock_guard lock(state().mutex);
  std::lock_guard sl(session().mutex);
  if (!valid(mobile)) {
    failure("LeaveVrMode", invalid);
    return;
  }
  auto release = destroy_host_chains();
  if (XR_FAILED(release)) {
    failure("LeaveVrMode bridges", release);
    return;
  }
  destroy_input();
  auto &v = state();
  for (auto space : {v.app, v.local, v.stage, v.view})
    if (space)
      ::xrDestroySpace(space);
  v.app = v.local = v.stage = v.view = {};
  auto r = session().destroy(SessionOwner::vrapi);
  if (XR_FAILED(r))
    failure("LeaveVrMode destroy", r);
  v.mobile = nullptr;
  v.token = v.next_token = false;
  wake_frame_waiters();
}
double vrapi_GetTimeInSeconds() { return double(monotonic_ns()) * 1e-9; }
double vrapi_GetPredictedDisplayTime(Mobile *mobile, std::int64_t index) {
  if (index < 0 || !mobile) return 0;
  std::lock_guard lock(state().mutex);
  std::lock_guard sl(session().mutex);
  auto &v = state();
  auto &s = session();
  if (!valid(mobile) || XR_FAILED(pump_events()) || XR_FAILED(s.lost) ||
      !s.running || v.final_frame) return 0;
  const auto requested = static_cast<std::uint64_t>(index);
  // Before the first submission, or after a load that outlasted the cached
  // display time, use the host clock and observed display period. Once a
  // frame is waited, its prediction remains unchanged until it is retired.
  if (!v.token) {
    const auto now = time_from_seconds(0);
    if (!now) return 0;
    const double delta = requested >= v.prediction_index
        ? double(requested - v.prediction_index) : -double(v.prediction_index - requested);
    if (!v.predicted_time || double(v.predicted_time) + delta * double(v.predicted_period) < double(now)) {
      v.prediction_index = requested;
      v.predicted_period = s.display_timing.period.load();
      v.predicted_time = now + v.predicted_period;
    }
  }
  // Timing lookups never replace an outstanding waited frame's input.
  // Legacy lookups share one sample until an actual wait opens the next
  // submission cycle, regardless of future/backward prediction indices.
  if (!v.token && !v.prediction_input_sampled) {
    if (XR_FAILED(sync_input())) return 0;
    v.input_frame_index = index;
    v.prediction_input_sampled = true;
  }
  const bool next = v.next_token && requested >= v.next_frame_index;
  const auto origin = next ? v.next_frame_index : v.prediction_index;
  const auto time = next ? v.next_predicted_time : v.predicted_time;
  const auto period = next ? v.next_predicted_period : v.predicted_period;
  const double delta = requested >= origin ? double(requested - origin) : -double(origin - requested);
  const auto predicted_seconds = seconds_from_time(time) + delta * double(period) * 1e-9;
  if (av::enabled()) {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point reported{}; // protected by state().mutex
    const auto now = Clock::now();
    if (now - reported >= std::chrono::milliseconds(250)) {
      reported = now;
      av::emit({{"kind", "vrapi_prediction"}, {"requested_frame_index", index},
                {"input_frame_index", v.input_frame_index},
                {"prediction_frame_index", v.prediction_index},
                {"bound_frame_index", v.frame_index}, {"next_frame_index", v.next_frame_index},
                {"token", v.token}, {"next_token", v.next_token},
                {"native_predicted_time", s.frame.predictedDisplayTime},
                {"origin_frame_index", origin}, {"origin_time", time}, {"period", period},
                {"predicted_seconds", predicted_seconds}, {"monotonic_ns", monotonic_ns()}});
    }
  }
  return predicted_seconds;
}
Tracking vrapi_GetPredictedTracking2(Mobile *mobile, double seconds) {
  std::lock_guard lock(state().mutex);
  std::lock_guard sl(session().mutex);
  if (!valid(mobile))
    return {};
  return tracking_at(time_from_seconds(seconds), state().view);
}
TrackingHead vrapi_GetPredictedTracking(Mobile *mobile, double seconds) {
  const auto t = vrapi_GetPredictedTracking2(mobile, seconds);
  return {t.status, 0, t.head};
}
std::int32_t vrapi_WaitFrame(Mobile *mobile, std::uint64_t index) {
  if (!mobile) return invalid;
  return result(ensure_frame(index, mobile, FrameUse::wait), "WaitFrame");
}
std::int32_t vrapi_BeginFrame(Mobile *mobile, std::uint64_t index) {
  std::lock_guard lock(state().mutex);
  std::lock_guard sl(session().mutex);
  auto &v = state();
  auto &s = session();
  if (!valid(mobile) || !v.token)
    return invalid_operation;
  // OpenXR Frame Submission: beginning the next waited frame discards an
  // older begun frame that the renderer chose not to submit. UE4 does this
  // while changing render targets after its loading screen.
  const bool discard = v.next_token && v.next_frame_index == index &&
                       s.phase == Session::Phase::begun;
  if (!discard && ((v.frame_bound && v.frame_index != index) || s.phase != Session::Phase::waited))
    return invalid_operation;
  XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
  auto r = s.begin(&bi);
  if (XR_SUCCEEDED(r)) {
    if (discard) complete_frame(); // Promote the same wait consumed by xrBeginFrame.
    v.frame_index = index;
    v.frame_bound = true;
    wake_frame_waiters();
  }
  return result(r, "BeginFrame");
}
std::int32_t vrapi_SetTrackingSpace(Mobile *mobile, std::int32_t type) {
  std::lock_guard lock(state().mutex);
  std::lock_guard sl(session().mutex);
  if (!valid(mobile) || (type != 0 && type != 1 && type != 3))
    return invalid;
  if (type != 0 && !state().stage)
    return unavailable;
  auto previous = state().tracking_space;
  state().tracking_space = type;
  auto r = set_app_space({{0, 0, 0, 1}, {0, 0, 0}});
  if (XR_FAILED(r))
    state().tracking_space = previous;
  return result(r, "SetTrackingSpace");
}
Pose vrapi_LocateTrackingSpace(Mobile *mobile, std::int32_t target) {
  std::lock_guard lock(state().mutex);
  std::lock_guard sl(session().mutex);
  if (!valid(mobile))
    return {};
  XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
  auto r = ::xrLocateSpace(target == 0 ? state().local : state().stage, state().app,
                           time_from_seconds(0), &loc);
  if (XR_FAILED(r)) {
    failure("LocateTrackingSpace", r);
    return {};
  }
  hle::openxr::sanitize(loc.locationFlags, loc.pose);
  return pose(loc.pose);
}
Pose vrapi_GetTrackingTransform(Mobile *mobile, std::int32_t which) {
  std::lock_guard lock(state().mutex);
  std::lock_guard sl(session().mutex);
  if (!valid(mobile) || which < 0 || which > 3) {
    failure("GetTrackingTransform", invalid);
    return {};
  }
  // VrApi_Types.h ovrTrackingTransform: IDENTITY=0, CURRENT=1,
  // SYSTEM_CENTER_EYE_LEVEL=2, SYSTEM_CENTER_FLOOR_LEVEL=3.
  // VrApi.h specifies a base pose in tracking-system coordinates, not in the
  // current application space. Using app here cancels the floor offset on a
  // repeated GetTrackingTransform/SetTrackingTransform pair (UE4/OVRPlugin).
  if (which == 0)
    return {{0, 0, 0, 1}, {0, 0, 0}};
  if (which == 1)
    return pose(state().transform);
  const auto target = which == 2 ? state().local : state().stage;
  const auto base = state().tracking_space == 0 ? state().local : state().stage;
  if (!target || !base) {
    failure("GetTrackingTransform", unavailable);
    return {};
  }
  XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
  const auto r = ::xrLocateSpace(target, base, time_from_seconds(0), &loc);
  if (XR_FAILED(r)) {
    failure("GetTrackingTransform", r);
    return {};
  }
  hle::openxr::sanitize(loc.locationFlags, loc.pose);
  return pose(loc.pose);
}
void set_tracking_transform(Mobile *mobile, const Pose *value) {
  std::lock_guard lock(state().mutex);
  std::lock_guard sl(session().mutex);
  if (!valid(mobile) || !value) {
    failure("SetTrackingTransform", invalid);
    return;
  }
  auto p = pose(*value);
  auto q = p.orientation;
  float yaw = std::atan2(2 * (q.w * q.y + q.x * q.z), 1 - 2 * (q.y * q.y + q.z * q.z));
  p.orientation = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
  result(set_app_space(p), "SetTrackingTransform");
}
void vrapi_RecenterPose(Mobile *mobile) {
  std::lock_guard lock(state().mutex);
  std::lock_guard sl(session().mutex);
  if (!valid(mobile))
    return;
  XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
  if (XR_SUCCEEDED(::xrLocateSpace(state().view, state().local, time_from_seconds(0), &loc)) &&
      (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
    auto p = pose(loc.pose);
    set_tracking_transform(mobile, &p);
    ++state().recenter_count;
  }
}
std::int32_t vrapi_PollEvent(Event *event) {
  std::lock_guard lock(state().mutex);
  std::lock_guard sl(session().mutex);
  if (!event || !state().initialized)
    return invalid;
  auto r = pump_events();
  if (XR_FAILED(r))
    return result(r, "PollEvent");
  event->type = 0;
  // VrApi_Types.h ovrSuccess_EventUnavailable; SDK 1.50 Appl.cpp and
  // OVRPlugin drain events while PollEvent == ovrSuccess. Returning success
  // with EVENT_NONE traps the render thread in their polling loop.
  if (state().events.empty()) return event_unavailable;
  event->type = state().events.front();
  state().events.pop_front();
  return success;
}
} // namespace guest
} // namespace venture::hle::vrapi
