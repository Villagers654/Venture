#pragma once
#include "xr/api.h"
#include "xr/display_timing.h"
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace venture::xr {
enum class SessionOwner { none, openxr, vrapi };
struct HostWaiter;

// Shared by both APIs. Callers keep the recursive lock while operating on the
// state, except wait() which manages its own lock. No caller may hold an outer
// API, session or GPU queue lock over wait(): it blocks for pacing. Frames stay
// on guest application threads, never a hidden scheduler. Take the GPU lock
// second.
struct Session {
  std::recursive_mutex mutex;
  // Frame-state changes wake both kinds of waiter: guest threads block on the
  // epoch word through futex_wait (core/wait.h: interruptible by signals, GC
  // suspension and exit), host-only callers on the condition variable.
  std::condition_variable_any frame_changed;
  alignas(4) std::uint32_t frame_epoch = 0;
  XrSession handle = XR_NULL_HANDLE;
  SessionOwner owner = SessionOwner::none;
  XrSessionState state = XR_SESSION_STATE_UNKNOWN;
  XrViewConfigurationType view_configuration = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  bool running = false;
  bool uses_shared_gpu = true;
  VkQueue queue = nullptr;  // native queue supplied by the graphics binding
  enum class Phase { idle, waited, begun } phase = Phase::idle;
  XrFrameState frame{XR_TYPE_FRAME_STATE};
  XrFrameState pending_frame{XR_TYPE_FRAME_STATE};
  bool wait_in_progress = false, pending_wait = false;
  // Host XR_ERROR_SESSION_LOST/INSTANCE_LOST (or LOSS_PENDING) for this handle.
  XrResult lost = XR_SUCCESS;
  // Bumped whenever the host resets frame state (begin/end session), so a host
  // wait that spans the reset cannot bind a frame the host discarded.
  std::uint64_t run_generation = 0;
  std::uint64_t frame_index = 0;
  std::uint64_t completed_startup_frames = 0;  // empty host frames ended for abandoned guest frames
  bool profile_eye_seen = false;
  std::uint64_t end_errors = 0, last_report_frame = 0;
  std::chrono::steady_clock::time_point last_report_time{};
  std::thread::id render_thread; // Thread that began the frame, not an EndFrame restriction.
  DisplayTiming& display_timing = display_timing_state();
  std::uint32_t guest_swap_interval = 1, logged_swap_interval = 0;
  // Adaptive frame lead (VENTURE_XR_FRAME_LEAD=1). The runtime wakes a frame a
  // fixed time before its display; a guest whose wait-to-GPU chain misses that
  // deadline starts the next frame early by the measured shortfall only. The
  // host wait itself stays on the runtime's schedule (host_waiter, issued
  // right after the previous host end: SteamVR answers a late wait with its
  // next wake-up), and xrEndFrame consumes it, then begins and ends the host
  // frame. Frames that fit keep lead 0 and the unchanged host call order.
  struct FrameLead {
    bool enabled = false;
    bool owed = false;        // the waited guest frame has no host wait yet
    bool begun_owed = false;  // the begun guest frame has no host frame yet
    bool host_valid = false;
    XrFrameState host{XR_TYPE_FRAME_STATE};  // last host wait result
    std::int64_t window_ns = 0;  // host display time - host wait return, unled frames
    std::int64_t chain_ns = 0;   // guest wait return -> guest end entry (average)
    std::int64_t last_chain_ns = 0, previous_chain_ns = 0;  // the last two frames
    // Start of the waited and of the begun guest frame; each frame keeps its own.
    std::int64_t lead_ns = 0, pending_start_ns = 0, frame_start_ns = 0;
    // Diagnostic VENTURE_XR_FRAME_LEAD_MS: a pinned lead, never adapted or released.
    std::int64_t fixed_ns = 0;
    // A guest that waits for N+1 while N is begun gives no frame/host-frame
    // association to adapt on; such a session keeps lead 0.
    // Counted in gameplay only: titles abandon begun frames before VISIBLE.
    bool pipelined = false;
    std::uint32_t overlapped_waits = 0;
    std::uint32_t clean = 0, recent_misses = 0, window_frames = 0;
    std::uint32_t saturated_windows = 0, cooldown = 0;
    // Shortest host period seen: a runtime throttling a late app to half rate
    // reports a doubled period, a miss on every frame.
    std::int64_t base_period_ns = 0;
    bool plain_waits = true;  // guest waits carry no next chains
    std::uint64_t led_frames = 0, misses = 0;
    // Led frames whose host display time differed from the guest's predicted
    // one, and the largest difference: the guest located poses for its time.
    std::uint64_t mispredicted = 0;
    std::int64_t max_misprediction_ns = 0;
    // Asynchronous submission (VENTURE_XR_ASYNC_END=1, with the lead): the host
    // waiter makes a led session's host xrEndFrame and then its next host wait.
    // A runtime that blocks in xrEndFrame until its next wake-up (SteamVR) then
    // holds the waiter instead of the guest's frame thread, and led waits can
    // start the next frame before that wake-up.
    bool async = false;
    std::uint64_t async_ends = 0;
  } lead;

  void observe_display_period(std::int64_t, DisplayTiming::Source);
  void observe_host_event(const XrEventDataBuffer&);
  XrResult create(SessionOwner, const XrGraphicsBindingVulkanKHR *binding = nullptr);
  XrResult destroy(SessionOwner);
  // Host xrBeginSession/xrEndSession for either owner; both reset frame state.
  XrResult begin_session(const XrSessionBeginInfo *);
  XrResult end_session();
  // The desktop runtime exposes only request-exit as a stop transport. Keep
  // its EXITING event private for an Activity-driven host quit; guest requests
  // retain the normal OpenXR exit contract.
  bool host_stop_requested = false, guest_exit_requested = false;
  XrResult request_host_stop();
  XrResult request_exit(SessionOwner expected = SessionOwner::none);
  bool deliver_host_event(const XrEventDataBuffer&) const;
  XrResult wait(const XrFrameWaitInfo *, XrFrameState *);
  XrResult begin(const XrFrameBeginInfo *);
  // `owned` keeps everything `info` points to alive after the call returns; with
  // it, a led session may submit asynchronously (FrameLead::async).
  XrResult end(const XrFrameEndInfo *, std::shared_ptr<const void> owned = {});
  // The next end() may submit asynchronously; the caller holds the mutex.
  bool async_end_ready() const { return lead.async && lead_active(); }
  // Waits until an asynchronous host xrEndFrame has returned: the submission
  // borrows host swapchain and space handles. Destroying any host object first
  // calls this. The caller may hold the API state lock, never a queue lock.
  void finish_host_end();
  // Records a lost session/instance result and releases every frame waiter.
  void lose(XrResult);
  void notify_frame_waiters();

private:
  void note(XrResult);
  void reset_frames();
  void observe_host_frame(const XrFrameState&, std::int64_t returned_ns, std::int64_t frame_chain_ns, bool led);
  std::shared_ptr<HostWaiter> host_waiter;
  bool lead_active() const;
  void request_host_wait();
  // Hands `info` (kept alive by `owned`) to the host waiter: host xrEndFrame,
  // then the next host wait (only the wait without `owned`). False if a host
  // wait is already outstanding.
  bool request_host_end(const XrFrameEndInfo& info, std::shared_ptr<const void> owned);
  bool host_wait_pending();
  // Blocks for the requested host wait; the caller holds no session mutex.
  // A cancelled take (loss, end, destroy) returns without a frame.
  XrResult take_host_wait(XrFrameState&, std::int64_t& returned_ns, std::uint64_t& generation);
  // Cancels takers and waits for an in-flight host wait to return: OpenXR
  // requires no frame call in flight at xrEndSession or xrDestroySession.
  void drain_host_wait();
  void cancel_host_wait();
};
Session &session();
// Creates a host swapchain for `info`, falling back when the host rejects its format. Neither
// VrApi nor guest OpenXR swapchain formats are negotiated with the host, and a host may reject
// a format it lists (or not offer it at all: SteamVR on Windows has no 8-bit UNORM). An 8-bit
// RGBA format then falls back to another 8-bit RGBA format, same encoding first. On success
// info.format holds the host format; the caller copies into it converting by value.
XrResult create_host_swapchain(XrSession, XrSwapchainCreateInfo &info, XrSwapchain *out);
using FrameObserver = void (*)(std::int64_t monotonic_time, std::int64_t period);
void set_frame_observer(FrameObserver);
using DisplayObserver = void (*)(std::int64_t period);
void set_display_observer(DisplayObserver);
} // namespace venture::xr
