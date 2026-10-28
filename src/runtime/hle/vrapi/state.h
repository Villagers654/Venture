#pragma once
#include "hle/vrapi/vrapi.h"
#include "xr/math.h"
#include "xr/session.h"
#include "xr/xr_images.h"
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
namespace venture::hle::vrapi {
using namespace abi;
using namespace venture::xr;
struct HostChain {
  XrSwapchain handle{};
  std::vector<XrSwapchainImageVulkanKHR> images;
  bool sampler_current = false;
  VkFormat format{};  // host image format; differs from the ring's when the host lacks it
};
struct Ring {
  ImageDesc desc;
  std::vector<std::uint32_t> textures;
  std::vector<bool> owned;
  // Two host chains per slot, one per displayed row order (XrImages bottom_up).
  std::vector<HostChain> host;
  static std::size_t host_chain(int slot, bool bottom_up) {
    return std::size_t(slot) * 2 + (bottom_up ? 1 : 0);
  }
  // VrApi does not document initial sampler values. Use linear/edge sampling
  // for a fresh compositor texture, transparent border and no anisotropy.
  TextureSamplerState sampler{TextureFilter::linear, TextureFilter::linear,
      TextureWrap::clamp_edge, TextureWrap::clamp_edge, {0, 0, 0, 0}, 1};
};
struct Input {
  XrActionSet set{};
  XrPath hands[2]{};
  XrSpace grip[2]{}, aim[2]{};
  XrAction grip_action{}, aim_action{}, trigger{}, squeeze{}, stick{}, a{}, b{}, click{}, menu{},
      haptic{};
  XrAction touch_trigger{}, touch_stick{}, touch_a{}, touch_b{}, touch_rest{};
  XrHandTrackerEXT trackers[2]{};
  bool hands_supported = false;
};
struct State {
  std::recursive_mutex mutex;
  bool initialized = false;
  Init init{};
  Mobile *mobile{};
  std::uintptr_t generation = 0x51500000;
  XrSpace local{}, stage{}, view{}, app{};
  std::int32_t tracking_space = 0;
  XrPosef transform{{0, 0, 0, 1}, {0, 0, 0}};
  Input input;
  bool token = false;
  bool frame_bound = false; // Wait/begin/submission binds the index; prediction never does.
  bool next_token = false, wait_in_progress = false;
  std::uint64_t next_frame_index = 0;
  XrTime predicted_time = 0, next_predicted_time = 0;
  XrDuration predicted_period = 0, next_predicted_period = 0;
  // Monotonic time the current waited frame became the renderer's next frame:
  // its host wait returned or the previous frame completed.
  std::int64_t frame_ready_ns = 0;
  bool final_frame = false;
  std::deque<std::int64_t> submissions;
  std::uint64_t frame_index = 0;
  std::uint64_t prediction_index = 0; // Index associated with the prediction origin.
  std::int64_t input_frame_index = -1; // Latest frame whose prediction sampled input.
  bool prediction_input_sampled = false; // One legacy input sample until the next actual wait.
  std::uint32_t recenter_count = 0;
  std::map<TextureSwapChain *, std::unique_ptr<Ring>> rings;
  std::map<std::int32_t, std::int32_t> properties;
  std::map<std::int32_t, float> float_properties;
  std::deque<std::int32_t> events;
  std::int32_t cpu_level = 2, gpu_level = 2, latency_mode = 0;
  std::uint32_t perf_threads[2]{}; // Last tid hinted per vrapi_SetPerfThread type.
};
State &state();
bool valid(Mobile *);
std::int32_t result(XrResult, const char *operation);
void failure(const char *, std::int32_t);
XrResult pump_events();
enum class FrameUse { submit, wait };
// A waited frame still unbegun this long after its predicted display time, its
// predecessor's completion and the next wait finding it unbegun was abandoned
// by the application. A slow renderer begins its next frame within it once the
// previous one completes, and so does a renderer after a simulation stall.
inline constexpr std::int64_t abandoned_frame_ns = 2'000'000'000;
XrResult ensure_frame(std::uint64_t, Mobile *expected = nullptr, FrameUse = FrameUse::submit);
void complete_frame(); // Both API/session locks held; successful end or empty discard.
void wake_frame_waiters();
XrResult initialize_input();
void destroy_input();
void cancel_haptics(int hand = -1);
std::uint64_t expired_haptic_buffers();
XrResult sync_input();
XrTime time_from_seconds(double);
double seconds_from_time(XrTime);
Pose pose(XrPosef);
XrPosef pose(Pose);
Tracking tracking_at(XrTime, XrSpace target);
XrResult destroy_host_chains();
XrResult set_app_space(XrPosef);
} // namespace venture::hle::vrapi
