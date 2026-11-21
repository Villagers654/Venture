#pragma once
#include "xr/api.h"
#include <array>
#include <optional>
#if defined(_WIN32)
#define VENTURE_MOCK_EXPORT __declspec(dllexport)
#else
#define VENTURE_MOCK_EXPORT __attribute__((visibility("default")))
#endif
struct MockXrController {
  XrPosef aim{{0, 0, 0, 1}, {0.25f, 1.25f, -0.35f}};
  XrPosef grip = aim;
  // Negative trigger inherits MockXrScript::trigger for existing API callers.
  float trigger = -1, squeeze = 0;
  XrVector2f thumbstick{};
  bool primary = false, secondary = false, menu = false, thumbstick_click = false;
  bool trigger_touch = false, primary_touch = false, secondary_touch = false,
       thumbstick_touch = false, thumbrest_touch = false, active = true;
  // Inputs by component path ("/input/trackpad/dpad_up", "/input/view/click") for
  // controllers whose inputs have no field above; they win over the fields.
  struct Raw {
    char path[40];
    float value;
  };
  std::array<Raw, 6> raw{};
};
struct MockXrScript {
  XrPosef head{{0, 0, 0, 1}, {0, 1.6f, 0}};
  float trigger = 0.25f;
  bool active = true;
  bool should_render = true;
  XrResult sampler_update_result = XR_SUCCESS;
  XrResult swapchain_wait_result = XR_SUCCESS;
  XrResult end_frame_result = XR_SUCCESS;
  // Both GPU queries refuse instances the runtime did not create (Oculus with an AMD GPU).
  bool refuse_app_vulkan_instances = false;
  // Both GPU queries refuse this instance, even one from xrCreateVulkanInstanceKHR,
  // and xrCreateVulkanDeviceKHR fails without calling vkCreateDevice (Oculus on an
  // AMD + NVIDIA laptop: RUNTIME_FAILURE, then HANDLE_INVALID; issue 92).
  VkInstance refused_vulkan_instance = VK_NULL_HANDLE;
  bool refuse_vulkan_devices = false;
  // The enable1 query of an instance from xrCreateVulkanInstanceKHR fails, and
  // the runtime then refuses that instance's enable2 query too (Oculus with an
  // AMD GPU answered H3VR2's queries this way; the device fell back and the
  // runtime later called through null in xrDestroySwapchain).
  bool enable1_spoils_runtime_instances = false;
  bool retain_failed_end = false;
  bool depth_formats = false;
  std::uint32_t swapchain_limit = 0; // zero disables the per-session host budget
  bool hmd = true;
  XrDuration display_period = 11'111'111;
  MockXrController left{.aim = {{0, 0, 0, 1}, {-0.25f, 1.25f, -0.35f}},
                        .grip = {{0, 0, 0, 1}, {-0.25f, 1.25f, -0.35f}}};
  MockXrController right;
  bool hand_tracking_active = true;
  XrPosef left_hand{{0, 0, 0, 1}, {-0.25f, 1.25f, -0.35f}};
  XrPosef right_hand{{0, 0, 0, 1}, {0.25f, 1.25f, -0.35f}};
  std::optional<std::array<XrPosef, 26>> left_hand_joints, right_hand_joints;
  // Both eyes' view angles (left/right/up/down); unset uses scene_signature.h's host_fov.
  std::optional<std::array<float, 4>> view_fov;
};
struct MockXrStats {
  std::uint32_t waits, begins, ends, copies, acquired, released, bindings, live_images;
  std::uint32_t projection, cylinder, cube, equirect, quad, haptic_pulses, haptic_stops;
  std::uint32_t live_swapchains, peak_swapchains, swapchain_limit_rejections;
  std::uint32_t space_warp;  // projection views carrying XrCompositionLayerSpaceWarpInfoFB
  std::uint32_t projection_depth;  // projection views carrying native KHR depth
  // VENTURE_MOCK_PASSTHROUGH: the last frame's passthrough layers, environment
  // blend mode and HTC passthrough alpha; live host passthrough objects.
  std::uint32_t passthrough_fb, passthrough_htc, blend_mode;
  float passthrough_htc_alpha;
  std::uint32_t live_passthrough;
  std::uint32_t swapchain_destroys;  // xrDestroySwapchain calls that destroyed a swapchain
};
extern "C" VENTURE_MOCK_EXPORT void ventureMockXrRefreshEnvironment();
extern "C" VENTURE_MOCK_EXPORT void ventureMockXrSetScript(const MockXrScript *);
// The interaction profile the mock reports for both hands (null: the input timeline's).
// The next xrPollEvent reports the change.
extern "C" VENTURE_MOCK_EXPORT void ventureMockXrSetProfile(const char *profile);
// The binding paths last suggested for `profile`, one per line, into `out`
// (truncated to `capacity`); returns the size the whole text needs.
extern "C" VENTURE_MOCK_EXPORT std::uint32_t ventureMockXrSuggestedBindings(const char *profile, char *out,
                                                                          std::uint32_t capacity);
extern "C" VENTURE_MOCK_EXPORT MockXrStats ventureMockXrStats();
extern "C" VENTURE_MOCK_EXPORT XrTime ventureMockXrLastDisplayTime();
// Hold a host frame wait before it returns a prediction; queries may proceed.
extern "C" VENTURE_MOCK_EXPORT void ventureMockXrGateWait(bool hold);
extern "C" VENTURE_MOCK_EXPORT bool ventureMockXrAwaitGatedWait(std::uint32_t milliseconds);
// Holds xrEndFrame until released (or the session is lost); host sessions,
// spaces and swapchains destroyed while an End is held are counted.
extern "C" VENTURE_MOCK_EXPORT void ventureMockXrGateEnd(bool hold);
extern "C" VENTURE_MOCK_EXPORT bool ventureMockXrAwaitGatedEnd(std::uint32_t milliseconds);
extern "C" VENTURE_MOCK_EXPORT std::uint32_t ventureMockXrDestroysDuringEnd();
// Copies up to capacity subimages of the last accepted frame; returns the total.
extern "C" VENTURE_MOCK_EXPORT std::uint32_t ventureMockXrLastSubImages(XrSwapchainSubImage *out,
                                                             std::uint32_t capacity);
// Copies up to capacity projection view fields of view of the last accepted frame; returns the total.
extern "C" VENTURE_MOCK_EXPORT std::uint32_t ventureMockXrLastProjectionFov(XrFovf *out, std::uint32_t capacity);
// Copies up to capacity quad layers of the last accepted frame; returns the total.
extern "C" VENTURE_MOCK_EXPORT std::uint32_t ventureMockXrLastQuads(XrCompositionLayerQuad *out,
                                                         std::uint32_t capacity);
// The Vulkan image a swapchain released last (0 if none), to read what was submitted.
extern "C" VENTURE_MOCK_EXPORT std::uint64_t ventureMockXrReleasedImage(XrSwapchain);
// Runtime-initiated stop (headset sleep): queues STOPPING; after the
// application's xrEndSession the session returns through IDLE to READY.
extern "C" VENTURE_MOCK_EXPORT void ventureMockXrRestartSession();
// Runtime-initiated quit (SteamVR's dashboard quitting the app): queues
// STOPPING; after the application's xrEndSession, IDLE then EXITING.
extern "C" VENTURE_MOCK_EXPORT void ventureMockXrEndSession();
// Host session loss: queues LOSS_PENDING; frame calls return SESSION_LOST.
extern "C" VENTURE_MOCK_EXPORT void ventureMockXrLoseSession();
extern "C" VENTURE_MOCK_EXPORT bool ventureMockXrAwaitHaptics(std::uint32_t count,
                                                    std::uint32_t milliseconds);
// Waits until at least `count` xrStopHapticFeedback calls have been accepted.
extern "C" VENTURE_MOCK_EXPORT bool ventureMockXrAwaitHapticStops(std::uint32_t count,
                                                        std::uint32_t milliseconds);
// Every haptic output call (xrApplyHapticFeedback, xrStopHapticFeedback) takes this long,
// without holding the mock's own lock: a slow runtime's haptics. VENTURE_MOCK_HAPTIC_DELAY_MS
// sets it for a whole run; this overrides it (negative: back to the environment).
extern "C" VENTURE_MOCK_EXPORT void ventureMockXrSetHapticDelay(std::int64_t microseconds);
// While held, every haptic output call blocks (before its delay, without the mock's lock)
// until released; the await waits until one is blocked there.
extern "C" VENTURE_MOCK_EXPORT void ventureMockXrGateHaptics(bool hold);
extern "C" VENTURE_MOCK_EXPORT bool ventureMockXrAwaitGatedHaptic(std::uint32_t milliseconds);
// One accepted output on a hand: a core pulse (amplitude, duration) or, with
// VENTURE_MOCK_PCM_RATE above 0, a native PCM buffer (peak amplitude, samples, rate).
struct MockXrHapticRecord {
  std::uint64_t time_ns;  // steady clock, when the call was accepted
  std::uint32_t hand;     // 0 left, 1 right
  float amplitude;
  std::int64_t duration_ns;
  std::uint32_t pcm_samples;  // 0 for a core pulse
  float pcm_rate;
  std::uint32_t append;       // a PCM buffer appended to the previous one
};
// Copies up to capacity records (oldest first) and returns how many there are; clear drops them.
extern "C" VENTURE_MOCK_EXPORT std::uint32_t ventureMockXrHapticLog(MockXrHapticRecord *out, std::uint32_t capacity);
extern "C" VENTURE_MOCK_EXPORT void ventureMockXrClearHapticLog();
