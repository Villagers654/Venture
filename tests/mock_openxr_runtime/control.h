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
};
struct MockXrStats {
  std::uint32_t waits, begins, ends, copies, acquired, released, bindings, live_images;
  std::uint32_t projection, cylinder, cube, equirect, quad, haptic_pulses, haptic_stops;
  std::uint32_t live_swapchains, peak_swapchains, swapchain_limit_rejections;
  std::uint32_t space_warp;  // projection views carrying XrCompositionLayerSpaceWarpInfoFB
  std::uint32_t projection_depth;  // projection views carrying native KHR depth
};
extern "C" VENTURE_MOCK_EXPORT void ventureMockXrRefreshEnvironment();
extern "C" VENTURE_MOCK_EXPORT void ventureMockXrSetScript(const MockXrScript *);
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
// Runtime-initiated stop (headset sleep): queues STOPPING; after the
// application's xrEndSession the session returns through IDLE to READY.
extern "C" VENTURE_MOCK_EXPORT void ventureMockXrRestartSession();
// Host session loss: queues LOSS_PENDING; frame calls return SESSION_LOST.
extern "C" VENTURE_MOCK_EXPORT void ventureMockXrLoseSession();
extern "C" VENTURE_MOCK_EXPORT bool ventureMockXrAwaitHaptics(std::uint32_t count,
                                                    std::uint32_t milliseconds);
