#include <cstdio>
#include "common/environment.h"
#include "control.h"
#include "xr/sampler.h"
#include "configuration.h"
#include "capture.h"
#include "terminal_capture.h"
#include "projection_selection.h"
#include "d3d11.h"
#include "input_timeline.h"
#include "scene_signature.h"
#include "motion.h"
#include "xr/hands.h"
#include "xr/math.h"
#include "xr/native.h"
#include <algorithm>
#include <limits>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <numbers>
#include <optional>
#include <openxr/openxr_loader_negotiation.h>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <fstream>
#include <format>
#include <iostream>
#include <thread>
#include <tuple>

namespace mock {
std::recursive_mutex mutex;
std::condition_variable_any haptic_changed;
std::condition_variable_any frame_changed;
MockXrScript script;
MockXrStats stats{};
std::uint64_t haptic_per_hand[2]{};
std::uintptr_t next_id = 0x10000;
template <class H> H token() {
  // VENTURE_MOCK_TYPED_HANDLES numbers each handle type from 1, as Virtual Desktop's VDXR does:
  // OpenXR handles are unique only within their type.
  static const bool typed = venture::environment::getenv("VENTURE_MOCK_TYPED_HANDLES") != nullptr;
  if (typed) {
    static std::uintptr_t next = 0;
    return reinterpret_cast<H>(++next);
  }
  next_id += 16;
  return reinterpret_cast<H>(next_id);
}
XrInstance instance{};
bool input_attachment_usage = false; // XR_KHR_swapchain_usage_input_attachment_bit enabled
XrSession session{};
XrSessionState session_state = XR_SESSION_STATE_IDLE;
XrTime session_event_start_ns{};
std::uint64_t session_waits{}, session_ends{};
XrGraphicsBindingVulkanKHR graphics{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
VkInstance vulkan_instance{};
bool running = false;
int frame_phase = 0;
bool pending_frame = false;
bool gate_wait = false, gated_wait = false;
// ventureMockXrGateEnd: xrEndFrame blocks; host objects destroyed meanwhile are counted.
bool gate_end = false, gated_end = false;
std::uint32_t destroys_during_end = 0;
bool restart_pending = false; // Runtime-initiated stop that returns to READY.
bool exit_pending = false;
bool session_lost = false;
XrTime begun_time = 0;
XrTime predicted = 0;
std::map<std::string, XrPath> paths;
std::map<XrPath, std::string> path_names;
struct Space {
  XrReferenceSpaceType type;
  XrPosef offset;
  XrAction action{};
  XrPath hand{};
  XrPosef sampled_pose{{0, 0, 0, 1}, {0, 0, 0}};
  XrTime sampled_time{};
  bool sampled_active{};
  XrSpaceVelocityData velocity{3, {}, {}};
};
std::map<XrSpace, Space> spaces;
XrPosef local_origin{{0, 0, 0, 1}, {0, 1.6f, 0}};
XrPosef world_pose(XrSpace space);
bool space_active(XrSpace space);
std::set<XrActionSet> action_sets;
std::map<XrActionSet, std::uint32_t> action_priorities;
struct Action { XrActionType type; std::vector<XrPath> subactions; std::string name; };
std::map<XrAction, Action> actions;
std::map<XrAction, XrActionSet> action_parents;
std::map<XrPath, std::vector<XrActionSuggestedBinding>> bindings;
std::set<XrActionSet> attached_sets;
struct ActionState {
  bool active = false, changed = false;
  float value = 0;
  XrVector2f vector{};
  XrTime last_change = 0;
  XrPath source{};
};
std::map<std::pair<XrAction, XrPath>, ActionState> action_states;
// VENTURE_MOCK_LOG_ACTIONS: the last logged answer per action and subaction.
std::map<std::pair<XrAction, XrPath>, std::tuple<float, bool, bool, int>> action_reads_logged;
InputTimeline timeline;
std::filesystem::path input_path;
std::filesystem::file_time_type input_write_time;
XrTime next_input_reload = 0;
XrTime input_origin = 0;
// Route gates (input_timeline.h "wait"): xrSyncActions advances the timeline and
// judges a held gate; xrEndFrame samples the eye-0 scene signature while one is
// held. The route clock (timeline seconds minus holds) drives VENTURE_MOCK_UNPACED.
std::atomic<double> route_offset_s{0};
std::atomic<bool> gate_sampling{false};
// Watches (input_timeline.h "watches") sample the same signature while one waits for its screen.
std::atomic<bool> watch_sampling{false};
bool signatures_wanted() {
  return gate_sampling.load(std::memory_order_relaxed) || watch_sampling.load(std::memory_order_relaxed);
}
mock::WatchRunner watch_runner;
XrTime watch_signature_ns = 0;
std::atomic<bool> gate_last_projection{false};
constexpr XrTime gate_sample_period_ns = 250'000'000;
struct GateState {
  double event_time = -1;
  XrTime started = 0, last_ns = 0, still_since = 0;
  std::optional<mock::SceneSignature> reference, last;
  bool changed = false;
  std::uint32_t samples = 0;
  double closest = -1;  // match gates: nearest distance seen, for tuning "within"
};
GateState gate_state;                                   // guarded by mutex
std::optional<mock::SceneSignature> latest_signature;   // guarded by mutex
std::optional<mock::SceneSignature> latest_signature_raw;  // 8-bit UNORM sources: bytes as stored
bool signature_rows_linear = false;                       // the rows in flight are 8-bit UNORM
XrTime latest_signature_ns = 0;
std::array<venture::xr::PendingCapturePtr, 3> signature_rows;  // Vulkan rows in flight
mock::SignatureWindow signature_rows_window;              // where they were taken
XrTime next_signature_ns = 0;
bool signature_error_reported = false, route_done_written = false, av_window_written = false;
TerminalCapture terminal_capture;
bool window_start_captured = false;
std::vector<std::string> gate_log, watch_log;
ViewConfiguration view_configuration_state;
using SwapchainGeometry = std::tuple<std::uint32_t, std::uint32_t, std::int64_t, std::uint32_t, std::size_t, std::uint32_t, std::uint32_t, std::int32_t, std::int32_t, bool>;
std::set<SwapchainGeometry> recorded_swapchains;
std::map<std::uint32_t, std::array<float, 8>> recorded_views; // last projection fov per layer
bool profile_changed = false;
bool trace_input() { return venture::environment::getenv("VENTURE_MOCK_TRACE_INPUT") != nullptr; }
// VENTURE_MOCK_SPACE_WARP models a host with XR_FB_space_warp (Meta's PC
// runtime): the extension, its system properties and the motion-vector and
// depth swapchain formats. Off by default so titles keep their usual path.
bool space_warp() { return venture::environment::getenv("VENTURE_MOCK_SPACE_WARP") != nullptr; }
// VENTURE_MOCK_PCM_RATE=<Hz> models a host with XR_FB_haptic_pcm that answers that device
// sample rate for every vibration action. 0 is Monado's answer for controllers whose driver
// reports no output limits. PCM payloads themselves stay unsupported, as core vibration is
// all the mock counts. Off by default.
std::optional<float> pcm_rate() {
  const char* text = venture::environment::getenv("VENTURE_MOCK_PCM_RATE");
  if (!text || !*text) return std::nullopt;
  return std::strtof(text, nullptr);
}
std::map<XrHandTrackerEXT, bool> hands;
std::deque<XrSessionState> events;
struct Chain {
  XrSwapchainCreateInfo info;
  XrSwapchainStateSamplerVulkanFB sampler = venture::xr::default_sampler();
  std::vector<VkImage> images;
  std::vector<VkDeviceMemory> memory;
  std::vector<void*> d3d_images;  // ID3D11Texture2D, when the session is D3D11
  std::size_t count() const { return d3d11::active() ? d3d_images.size() : images.size(); }
  std::deque<std::uint32_t> acquired;
  bool waited = false;
  std::uint32_t sequence = 0;
  std::uint32_t last_released = UINT32_MAX;
};
std::map<XrSwapchain, Chain> chains;
std::int64_t last_end_ns = 0;
XrTime last_display_time = 0;
// Layer subimages of the last accepted xrEndFrame, in submission order.
std::vector<XrSwapchainSubImage> last_subimages;
// Compositor pacing. A compositor cannot show a frame before the GPU finishes
// its images, and it lets the app pipeline one frame: xrWaitFrame for frame
// N+1 returns once frame N-1 is GPU-complete, while N may still render. Each
// xrEndFrame submits a fence-only batch to the binding queue; Vulkan signals it
// once all earlier work on that queue is done (vkQueueSubmit with submitCount 0).
VkQueue graphics_queue{};
std::uint32_t begun_frame = 0;  // xrWaitFrame count when the current frame began
struct FrameFence { std::uint32_t frame; VkFence fence; };
std::deque<FrameFence> frame_fences;  // submitted, oldest first
std::vector<VkFence> spare_fences;  // unsignaled, reusable
std::int64_t last_gpu_wait_ns = 0;
// VENTURE_MOCK_REALTIME: how long the last xrWaitFrame slept to the display grid,
// i.e. the frame's unused time. A paced 90 Hz frame log then still shows how
// much faster than the period the application could have run.
std::atomic<std::int64_t> last_pace_sleep_ns{0};
// Bound a wait like a GPU watchdog would (the Windows TDR default is 2 s): a
// hung device must not hang the app thread inside xrWaitFrame forever.
constexpr std::uint64_t gpu_wait_bound_ns = 2'000'000'000;
// VENTURE_MOCK_UNPACED=begin:end, in input-timeline seconds (input_time_s): frames
// in that span are neither paced by VENTURE_MOCK_REALTIME nor captured, measuring
// how fast the application runs without the 90 Hz grid or eye-dump stalls.
const std::optional<mock::TimeWindow>& unpaced_window() {
  static const auto window = [] {
    const char* text = venture::environment::getenv("VENTURE_MOCK_UNPACED");
    return mock::parse_time_window(text ? text : "");
  }();
  return window;
}
// VENTURE_MOCK_DUMP_DENSE=begin:end, in input-timeline seconds: eyes are dumped
// every VENTURE_MOCK_DUMP_EVERY frames only in that span (the scene proof a
// scorer reads) and ten times more sparsely elsewhere, so a long run does not
// write gigabytes of startup frames.
const std::optional<mock::TimeWindow>& dense_dump_window() {
  static const auto window = [] {
    const char* text = venture::environment::getenv("VENTURE_MOCK_DUMP_DENSE");
    return mock::parse_time_window(text ? text : "");
  }();
  return window;
}
double route_seconds(std::int64_t now) {
  return input_origin ? double(now - input_origin) / 1e9 - route_offset_s.load() : 0;
}
bool unpaced(std::int64_t now) {
  const auto& window = unpaced_window();
  return window && input_origin && window->contains(double(now - input_origin) / 1e9 - route_offset_s.load());
}
// frames.jsonl stays open for the session. Opening, appending and closing it
// per frame cost ~0.7 ms of the application's xrEndFrame thread, which is the
// thread being measured. Lines are flushed every frame_log_flush frames, at least
// once a second (harnesses read a log that stops growing as no frames submitted),
// when the session ends, and when the runtime unloads.
std::ofstream frame_log;
std::filesystem::path frame_log_path;
std::uint32_t frame_log_unflushed = 0;
std::int64_t frame_log_flushed_ns = 0;
bool frame_log_finished = false;
constexpr std::uint32_t frame_log_flush = 90;
void close_frame_log() {
  frame_log.exceptions(std::ios::goodbit);
  if (frame_log.is_open()) {
    frame_log.flush();
    frame_log.close();
    if (!frame_log) std::cerr << "[mock-xr] frame log shutdown flush failed\n";
  }
  frame_log.clear();
  frame_log_path.clear();
  frame_log_unflushed = 0;
}
void XRAPI_CALL finish_diagnostics() {
  std::lock_guard lock(mutex);
  frame_log_finished = true;
  close_frame_log();
}

CaptureWriter& capture_writer = *new CaptureWriter;
mock::SignatureWindow signature_window(const XrCompositionLayerProjectionView& view) {
  const float fov[4]{view.fov.angleLeft, view.fov.angleRight, view.fov.angleUp, view.fov.angleDown};
  return mock::signature_window(static_cast<std::uint32_t>(view.subImage.imageRect.extent.width),
                                static_cast<std::uint32_t>(view.subImage.imageRect.extent.height), fov);
}
// VENTURE_MOCK_CONTENT_HASH=1 on Vulkan: the D3D11 sampler's content sample (the three
// signature rows of eye 0, hashed), copied every frame into a ring of host-visible
// buffers and read three frames later, so a bridge that resubmits an unchanged image
// counts once in the h2h fresh-frame score. It also supplies held gates' signatures.
bool vulkan_content_enabled() {
  static const bool enabled = [] {
    const char* value = venture::environment::getenv("VENTURE_MOCK_CONTENT_HASH");
    return value && std::string_view(value) == "1";
  }();
  return enabled && !d3d11::active();
}
// 8-bit non-sRGB swapchain formats: OpenXR treats their values as linear, so signatures and eye dumps
// sRGB-encode them as a compositor does for display (matching sRGB-format submissions of the same scene).
// Apps and bridges also store display-encoded bytes in them, so gates and watches match either reading:
// a second signature of the bytes as stored travels with the encoded one.
bool linear8(std::int64_t format) {
  return format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM;
}
class VulkanContent {
 public:
  d3d11::ContentSample sample(VkImage image, const XrSwapchainCreateInfo& info, const XrSwapchainSubImage& sub,
                              const mock::SignatureWindow& window, std::uint32_t frame) {
    const auto width = static_cast<std::uint32_t>(sub.imageRect.extent.width);
    const auto pixel = pixel_bytes(info.format);
    if (!pixel || !width || sub.imageRect.extent.height < 4) return {};
    if ((width != width_ || info.format != format_) && !setup(width, info.format, pixel)) return {};
    d3d11::ContentSample result;
    auto& slot = ring_[next_];
    if (slot.submitted) {
      // The oldest copy was submitted three frames ago; waiting for it rarely blocks.
      fn<PFN_vkWaitForFences>("vkWaitForFences")(graphics.device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
      VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, slot.memory, 0, VK_WHOLE_SIZE};
      fn<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges")(graphics.device, 1, &range);
      const auto row_bytes = std::size_t(width) * pixel;
      const auto* base = static_cast<const unsigned char*>(slot.mapped);
      std::vector<unsigned char> rgba(base, base + 3 * row_bytes);  // one pass over mapped memory
      std::uint64_t hash = 1469598103934665603ull;
      for (auto byte : rgba) hash = (hash ^ byte) * 1099511628211ull;
      if (bgra(format_))
        for (std::size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
      const unsigned char* const rows[3]{rgba.data(), rgba.data() + row_bytes, rgba.data() + 2 * row_bytes};
      std::optional<mock::SceneSignature> raw;
      if (linear8(format_)) {
        raw = mock::scene_signature(rows, slot.window, false);
        venture::xr::encode_srgb_rgba8(rgba.data(), rgba.size());
      }
      result = {slot.frame, hash, mock::scene_signature(rows, slot.window, pixel == 8), raw};
      fn<PFN_vkResetFences>("vkResetFences")(graphics.device, 1, &slot.fence);
      slot.submitted = false;
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (fn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(slot.command, &begin) != VK_SUCCESS) return result;
    // OpenXR Vulkan swapchain contract: released color images are in COLOR_ATTACHMENT_OPTIMAL.
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, sub.imageArrayIndex, 1};
    const auto pipeline_barrier = fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    pipeline_barrier(slot.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     0, 0, nullptr, 0, nullptr, 1, &barrier);
    std::array<VkBufferImageCopy, 3> copies{};
    for (std::uint32_t row = 0; row < 3; ++row) {
      copies[row].bufferOffset = VkDeviceSize(row) * width * pixel;
      copies[row].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, sub.imageArrayIndex, 1};
      copies[row].imageOffset = {sub.imageRect.offset.x,
                                 sub.imageRect.offset.y + static_cast<std::int32_t>(window.rows[row]), 0};
      copies[row].imageExtent = {width, 1, 1};
    }
    fn<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(slot.command, image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.buffer, 3, copies.data());
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    pipeline_barrier(slot.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = slot.buffer;
    host.size = VK_WHOLE_SIZE;
    pipeline_barrier(slot.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                     0, 0, nullptr, 1, &host, 0, nullptr);
    if (fn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(slot.command) != VK_SUCCESS) return result;
    VkQueue queue{};
    fn<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(graphics.device, graphics.queueFamilyIndex, graphics.queueIndex, &queue);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &slot.command;
    if (fn<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &submit, slot.fence) != VK_SUCCESS) return result;
    slot.submitted = true;
    slot.frame = frame;
    slot.window = window;
    next_ = (next_ + 1) % ring_.size();
    return result;
  }
  // Copies in flight read swapchain images: finish them before an image goes away.
  void finish() {
    for (auto& slot : ring_)
      if (slot.submitted) {
        fn<PFN_vkWaitForFences>("vkWaitForFences")(graphics.device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
        fn<PFN_vkResetFences>("vkResetFences")(graphics.device, 1, &slot.fence);
        slot.submitted = false;
      }
  }
  void release() {
    if (!graphics.device) return;
    finish();
    for (auto& slot : ring_) {
      if (slot.fence) fn<PFN_vkDestroyFence>("vkDestroyFence")(graphics.device, slot.fence, nullptr);
      if (slot.buffer) fn<PFN_vkDestroyBuffer>("vkDestroyBuffer")(graphics.device, slot.buffer, nullptr);
      if (slot.memory) fn<PFN_vkFreeMemory>("vkFreeMemory")(graphics.device, slot.memory, nullptr);
      slot = {};
    }
    if (pool_) fn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(graphics.device, pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
    width_ = 0;
    format_ = 0;
    next_ = 0;
  }

 private:
  struct Slot {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* mapped = nullptr;
    VkCommandBuffer command{};
    VkFence fence{};
    bool submitted = false;
    std::uint32_t frame = 0;
    mock::SignatureWindow window{};
  };
  template <class F> static F fn(const char* name) {
    return reinterpret_cast<F>(venture::xr::native_vulkan_loader()(vulkan_instance, name));
  }
  static std::uint32_t pixel_bytes(std::int64_t format) {
    switch (format) {
      case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
      case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: return 4;
      case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
      default: return 0;
    }
  }
  static bool bgra(std::int64_t format) {
    return format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
  }
  bool setup(std::uint32_t width, std::int64_t format, std::uint32_t pixel) {
    release();
    const auto device = graphics.device;
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = graphics.queueFamilyIndex;
    if (fn<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device, &pi, nullptr, &pool_) != VK_SUCCESS) return false;
    VkPhysicalDeviceMemoryProperties properties{};
    fn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(graphics.physicalDevice, &properties);
    for (auto& slot : ring_) {
      VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      bi.size = VkDeviceSize(3) * width * pixel;
      bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      if (fn<PFN_vkCreateBuffer>("vkCreateBuffer")(device, &bi, nullptr, &slot.buffer) != VK_SUCCESS) break;
      VkMemoryRequirements requirements{};
      fn<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device, slot.buffer, &requirements);
      // The CPU reads these every frame: prefer cached system memory. The first host-visible
      // type can be device-local BAR memory, where each read crosses PCIe uncached (3.7 ms of
      // xrEndFrame per frame on NVIDIA/Windows, halving North Star from 292 to 142 fps).
      auto find = [&](VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
        std::uint32_t type = 0;
        while (type < properties.memoryTypeCount &&
               (!(requirements.memoryTypeBits & (1u << type)) ||
                (properties.memoryTypes[type].propertyFlags & want) != want ||
                (properties.memoryTypes[type].propertyFlags & avoid))) ++type;
        return type;
      };
      constexpr VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
      std::uint32_t type = find(visible | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (type == properties.memoryTypeCount) type = find(visible, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (type == properties.memoryTypeCount) type = find(visible, 0);
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, type};
      VkCommandBufferAllocateInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool_,
                                     VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
      VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      if (type == properties.memoryTypeCount ||
          fn<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &ai, nullptr, &slot.memory) != VK_SUCCESS ||
          fn<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device, slot.buffer, slot.memory, 0) != VK_SUCCESS ||
          fn<PFN_vkMapMemory>("vkMapMemory")(device, slot.memory, 0, VK_WHOLE_SIZE, 0, &slot.mapped) != VK_SUCCESS ||
          fn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device, &ci, &slot.command) != VK_SUCCESS ||
          fn<PFN_vkCreateFence>("vkCreateFence")(device, &fi, nullptr, &slot.fence) != VK_SUCCESS)
        break;
    }
    if (!ring_.back().fence) {
      std::fprintf(stderr, "[mock-xr] content hash: Vulkan readback ring unavailable\n");
      release();
      return false;
    }
    width_ = width;
    format_ = format;
    return true;
  }
  std::array<Slot, 3> ring_{};
  VkCommandPool pool_{};
  std::uint32_t width_ = 0, next_ = 0;
  std::int64_t format_ = 0;
};
VulkanContent vulkan_content;  // guarded by mutex
bool content_last_projection = false;
std::uint64_t content_selection_frame = 0;
// Eye-0 signature of the selected projection layer over the host view, for a held route
// gate. D3D11 reuses its three-row content sample; Vulkan reads three single-row
// copies, submitted every gate_sample_period_ns and read on the next sample.
void sample_scene(const XrFrameEndInfo& info, std::uint64_t frame) {
  const auto* projection = mock::select_projection(info, gate_last_projection.load(std::memory_order_relaxed));
  if (!projection || !projection->viewCount) return;
  const auto& sub = projection->views[0].subImage;
  const auto& chain = chains.at(sub.swapchain);
  if (chain.last_released >= chain.count()) return;
  const auto window = signature_window(projection->views[0]);
  const auto now = venture::xr::native_now_ns();
  if (d3d11::active()) {
    if (d3d11::content_hash_enabled()) return;  // record_frame's content sample supplies it
    const auto sample = d3d11::sample_content(chain.d3d_images[chain.last_released], chain.info, sub, window,
                                              static_cast<std::uint32_t>(frame));
    if (sample.frame) { latest_signature = sample.signature; latest_signature_raw = sample.raw; latest_signature_ns = now; }
    return;
  }
  if (vulkan_content_enabled()) return;  // record_frame's content sample supplies it
  if (now < next_signature_ns) return;
  next_signature_ns = now + gate_sample_period_ns;
  std::string error;
  if (signature_rows[0]) {
    std::array<std::vector<unsigned char>, 3> pixels;
    bool ok = true;
    for (std::size_t row = 0; row < 3; ++row)
      ok = ok && signature_rows[row] && venture::xr::finish_eye(*signature_rows[row], pixels[row], &error);
    if (ok) {
      const unsigned char* const rows[3]{pixels[0].data(), pixels[1].data(), pixels[2].data()};
      latest_signature_raw.reset();
      if (signature_rows_linear) {
        latest_signature_raw = mock::scene_signature(rows, signature_rows_window, false);
        for (auto& row : pixels) venture::xr::encode_srgb_rgba8(row.data(), row.size());
      }
      latest_signature = mock::scene_signature(rows, signature_rows_window, false);
      latest_signature_ns = now;
    }
    for (auto& row : signature_rows) row.reset();
  }
  signature_rows_window = window;
  signature_rows_linear = linear8(chain.info.format);
  for (std::size_t row = 0; row < 3 && error.empty(); ++row) {
    auto line = sub;
    line.imageRect.offset.y += static_cast<std::int32_t>(window.rows[row]);
    line.imageRect.extent.height = 1;
    signature_rows[row] = venture::xr::submit_eye(graphics, venture::xr::native_vulkan_loader(),
        chain.images[chain.last_released], static_cast<VkFormat>(chain.info.format), line, {}, &error);
  }
  if (!error.empty()) {
    for (auto& row : signature_rows) row.reset();
    if (!signature_error_reported) std::cerr << "[mock-xr] route gate signature unavailable: " << error << '\n';
    signature_error_reported = true;
  }
}
// Evidence for the harness. Its I/O failing (a full disk, an unwritable dump directory) is not the
// application's error: xrEndFrame still succeeds, or the application stops submitting. Recording
// stops for the rest of the session instead, and the harness reads the "diagnostics stopped" line.
void record_frame(const XrFrameEndInfo& info, const std::vector<XrSwapchainSubImage>& submitted) {
  if (frame_log_finished) return;
  const char* directory = venture::environment::getenv("VENTURE_MOCK_DUMP_DIR");
  if (!directory || !*directory) return;
  try {
    const std::filesystem::path root = std::filesystem::u8path(directory);
    if (const auto path = root / "frames.jsonl"; !frame_log.is_open() || path != frame_log_path) {
      close_frame_log();
      std::filesystem::create_directories(root);
      frame_log.open(path, std::ios::app);
      if (!frame_log) throw std::runtime_error("cannot open " + path.string());
      frame_log.exceptions(std::ios::badbit | std::ios::failbit);
      frame_log_path = path;
      std::ofstream workload(root / "workload.json");
      workload << std::format("{{\"profile\":\"{}\",\"requested_width\":{},\"requested_height\":{},\"period_ns\":{},\"budget_ms\":{}}}\n",
          view_configuration_state.profile, view_configuration_state.width, view_configuration_state.height,
          script.display_period, view_configuration_state.budget_ms);

    }
    std::uint64_t every = 90;
    if (const char* value = venture::environment::getenv("VENTURE_MOCK_DUMP_EVERY")) every = std::stoull(value);
    // Swapchains this frame's projection layers show: the eye images, whatever their shape.
    std::set<XrSwapchain> eye_chains;
    for (std::uint32_t layer = 0; layer < info.layerCount; ++layer) {
      if (info.layers[layer]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
      const auto& p = *reinterpret_cast<const XrCompositionLayerProjection*>(info.layers[layer]);
      for (std::uint32_t eye = 0; eye < p.viewCount; ++eye) eye_chains.insert(p.views[eye].subImage.swapchain);
    }
    for (const auto& sub : submitted) {
      const auto& chain = chains.at(sub.swapchain);
      const auto& spec = chain.info;
      const bool projection = eye_chains.contains(sub.swapchain);
      const SwapchainGeometry key{spec.width, spec.height, spec.format, spec.sampleCount,
          chain.count(), spec.arraySize, spec.faceCount, sub.imageRect.extent.width, sub.imageRect.extent.height, projection};
      if (!recorded_swapchains.insert(key).second) continue;
      const auto geometry = std::format("\"width\":{},\"height\":{},\"format\":{},\"sample_count\":{},\"image_count\":{},\"array_size\":{},\"face_count\":{},\"submitted_width\":{},\"submitted_height\":{},\"projection\":{}",
          spec.width, spec.height, spec.format, spec.sampleCount, chain.count(), spec.arraySize,
          spec.faceCount, sub.imageRect.extent.width, sub.imageRect.extent.height, projection);
      {
        std::ofstream metadata(root / "submitted-swapchains.jsonl", std::ios::app);
        metadata << std::format("{{\"first_frame\":{},{} }}\n", stats.ends + 1, geometry);
      }
    }
    // The fov each projection layer was rendered with, so runtimes can be checked for the same view.
    for (std::uint32_t layer = 0; layer < info.layerCount; ++layer) {
      if (info.layers[layer]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
      const auto& p = *reinterpret_cast<const XrCompositionLayerProjection*>(info.layers[layer]);
      std::array<float, 8> fov{};
      for (std::uint32_t eye = 0; eye < 2 && eye < p.viewCount; ++eye) {
        const auto& f = p.views[eye].fov;
        for (int i = 0; const float angle : {f.angleLeft, f.angleRight, f.angleUp, f.angleDown})
          fov[eye * 4 + i++] = std::round(angle * 1000) / 1000;
      }
      if (const auto found = recorded_views.find(layer); found != recorded_views.end() && found->second == fov) continue;
      recorded_views[layer] = fov;
      std::ofstream views(root / "submitted-views.jsonl", std::ios::app);
      views << std::format("{{\"first_frame\":{},\"layer\":{},\"fov\":[{},{},{},{},{},{},{},{}]}}\n", stats.ends + 1, layer,
          fov[0], fov[1], fov[2], fov[3], fov[4], fov[5], fov[6], fov[7]);
    }
    const auto frame = stats.ends + 1;
    const bool paced = !unpaced(venture::xr::native_now_ns());
    if (signatures_wanted()) sample_scene(info, frame);
    if (const auto& dense = dense_dump_window();
        dense && !(input_origin && dense->contains(route_seconds(venture::xr::native_now_ns()))))
      every *= 10;
    // Capture once at the full route end, after any neutral-pose evidence tail.
    // Do not flood the writer on every paced frame after the measured window.
    const auto& scored = unpaced_window();
    static const auto route_end = [] {
      const char* text = venture::environment::getenv("VENTURE_MOCK_ROUTE_END");
      return text && *text ? std::optional<double>(std::stod(text)) : std::nullopt;
    }();
    const auto end = route_end ? route_end : scored ? std::optional<double>(scored->end) : std::nullopt;
    const auto route_now = route_seconds(venture::xr::native_now_ns());
    const bool past_end = end && input_origin && route_now >= *end;
    const bool before_window_end = !scored || !input_origin || route_now < scored->end;
    // One capture just before the scored window opens (still paced), so every run has a frame at
    // each window edge for gameplay checks; the terminal capture is the other edge.
    const bool start_due = scored && input_origin && !window_start_captured && route_now < scored->begin &&
                           route_now >= scored->begin - 0.25;
    const bool periodic_due = every && paced && !terminal_capture.pending() && before_window_end &&
                              (frame == 1 || frame % every == 0 || start_due);
    const bool terminal_due = past_end && every && paced && info.layerCount && !terminal_capture.pending();
    bool capture_available = false;
    if (periodic_due || terminal_due) {
      std::size_t count = 0;
      for (std::uint32_t layer = 0; layer < info.layerCount; ++layer) {
        const auto type = info.layers[layer]->type;
        count += type == XR_TYPE_COMPOSITION_LAYER_PROJECTION ?
                   reinterpret_cast<const XrCompositionLayerProjection*>(info.layers[layer])->viewCount :
                 type == XR_TYPE_COMPOSITION_LAYER_CUBE_KHR ? 6 : 1;
      }
      capture_available = d3d11::active() || capture_writer.can_accept(count);
      if (start_due && capture_available) window_start_captured = true;
      if (periodic_due && !capture_available)
        std::cerr << "[mock-xr] skipping periodic capture: readback budget is full\n";
    }
    const bool terminal_shot = terminal_capture.request(past_end, terminal_due && capture_available, frame);
    if (terminal_shot || (periodic_due && capture_available)) {
      std::ofstream layer_metadata(root/"layers.jsonl",std::ios::app);
      layer_metadata.exceptions(std::ios::badbit|std::ios::failbit);
      auto eye_name=[](XrEyeVisibility eye) {
        return eye==XR_EYE_VISIBILITY_LEFT?"left":eye==XR_EYE_VISIBILITY_RIGHT?"right":"both";
      };
      auto capture = [&](const XrSwapchainSubImage& sub, const std::filesystem::path& path,const char* eye="unspecified") {
        const auto& chain = chains.at(sub.swapchain);
        if (chain.last_released >= chain.count())
          throw std::runtime_error("Submitted layer has no released image");
        if (d3d11::active()) {
          d3d11::capture(chain.d3d_images[chain.last_released], chain.info, sub, path);
          return;
        }
        std::string error;
        auto pending = venture::xr::submit_eye(graphics, venture::xr::native_vulkan_loader(),
            chain.images[chain.last_released], static_cast<VkFormat>(chain.info.format), sub, path, &error);
        if (!pending) throw std::runtime_error(error);
        if (linear8(chain.info.format)) venture::xr::encode_srgb_on_finish(*pending);
        capture_writer.push(std::move(pending));
        // Preserve which eye actually consumes an overlay. These records
        // describe submitted source images, not synthesized composite views.
        layer_metadata<<std::format("{{\"frame\":{},\"image\":\"{}\",\"eye_visibility\":\"{}\",\"capture_ns\":{},\"array_index\":{},\"rect\":[{},{},{},{}]}}\n",
          frame,path.filename().string(),eye,venture::xr::native_now_ns(),sub.imageArrayIndex,
          sub.imageRect.offset.x,sub.imageRect.offset.y,sub.imageRect.extent.width,sub.imageRect.extent.height);
        std::cerr << std::format(
            "[mock-xr] layer image: {} swapchain={:#x} image={:#x} released={} format={} array={} rect=[{},{} {}x{}]\n",
            path.string(), reinterpret_cast<std::uintptr_t>(sub.swapchain),
            reinterpret_cast<std::uintptr_t>(chain.images[chain.last_released]),
            chain.last_released, chain.info.format, sub.imageArrayIndex,
            sub.imageRect.offset.x, sub.imageRect.offset.y,
            sub.imageRect.extent.width, sub.imageRect.extent.height);
      };
      for (std::uint32_t layer = 0; layer < info.layerCount; ++layer) {
        const auto* header = info.layers[layer];
        std::cerr << std::format("[mock-xr] capture frame={} layer={} type={}\n",
            frame, layer, static_cast<int>(header->type));
        // These are submitted source images. Only projection images are eye
        // views; raw overlay captures do not pretend to be composited views.
        if (header->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
          const auto& projection = *reinterpret_cast<const XrCompositionLayerProjection*>(header);
          for (std::uint32_t eye = 0; eye < projection.viewCount; ++eye)
            capture(projection.views[eye].subImage,
                    root / std::format("frame-{:06}-layer-{}-eye-{}.png", frame, layer, eye),
                    projection.viewCount==1?"both":eye==0?"left":eye==1?"right":"additional-view");
        } else if (header->type == XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR) {
          // Preserve the submitted spherical source separately from projection eyes.
          // This headless runtime records layers; it does not pretend this is a composited eye.
          const auto& sphere = *reinterpret_cast<const XrCompositionLayerEquirect2KHR*>(header);
          capture(sphere.subImage, root / std::format("frame-{:06}-layer-{}-equirect2.png", frame, layer),eye_name(sphere.eyeVisibility));
        } else if (header->type == XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR) {
          const auto& sphere = *reinterpret_cast<const XrCompositionLayerEquirectKHR*>(header);
          capture(sphere.subImage, root / std::format("frame-{:06}-layer-{}-equirect.png", frame, layer),eye_name(sphere.eyeVisibility));
        } else if (header->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
          const auto& quad = *reinterpret_cast<const XrCompositionLayerQuad*>(header);
          capture(quad.subImage, root / std::format("frame-{:06}-layer-{}-quad.png", frame, layer),eye_name(quad.eyeVisibility));
        } else if (header->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) {
          const auto& cylinder = *reinterpret_cast<const XrCompositionLayerCylinderKHR*>(header);
          capture(cylinder.subImage, root / std::format("frame-{:06}-layer-{}-cylinder.png", frame, layer));
        } else if (header->type == XR_TYPE_COMPOSITION_LAYER_CUBE_KHR) {
          const auto& cube = *reinterpret_cast<const XrCompositionLayerCubeKHR*>(header);
          const auto& chain = chains.at(cube.swapchain);
          // OpenXR cube imageArrayIndex selects a cube; Vulkan stores its six
          // faces in consecutive array layers (XrSwapchainCreateInfo::faceCount).
          for (std::uint32_t face = 0; face < 6; ++face) {
            XrSwapchainSubImage sub{cube.swapchain,
                {{0, 0}, {static_cast<std::int32_t>(chain.info.width), static_cast<std::int32_t>(chain.info.height)}},
                cube.imageArrayIndex * 6 + face};
            capture(sub, root / std::format("frame-{:06}-layer-{}-cube-face-{}.png", frame, layer, face));
          }
        }
      }
    }
    std::string content;
    if ((d3d11::active() && d3d11::content_hash_enabled()) || vulkan_content_enabled()) {
      const bool selected_last = gate_last_projection.load(std::memory_order_relaxed);
      if (selected_last != content_last_projection) {
        content_last_projection = selected_last;
        content_selection_frame = frame;
      }
      if (const auto* projection = mock::select_projection(info, gate_last_projection.load(std::memory_order_relaxed))) {
        const auto& view = projection->views[0];
        const auto& chain = chains.at(view.subImage.swapchain);
        if (chain.last_released < chain.count()) {
          const auto sample = d3d11::active()
              ? d3d11::sample_content(chain.d3d_images[chain.last_released], chain.info, view.subImage,
                                      signature_window(view), static_cast<std::uint32_t>(frame))
              : vulkan_content.sample(chain.images[chain.last_released], chain.info, view.subImage,
                                      signature_window(view), static_cast<std::uint32_t>(frame));
          // Ignore delayed samples submitted before the projection selection changed.
          if (sample.frame && sample.frame >= content_selection_frame) {
            content = std::format(",\"content_frame\":{},\"content_hash\":\"{:016x}\"", sample.frame, sample.hash);
            if (signatures_wanted()) {
              latest_signature = sample.signature;
              latest_signature_raw = sample.raw;
              latest_signature_ns = venture::xr::native_now_ns();
            }
          }
        }
      }
    }
    const auto now = venture::xr::native_now_ns();
    frame_log << std::format("{{\"frame\":{},\"end_time_ns\":{},\"display_time_ns\":{},\"period_ns\":{},\"end_interval_ns\":{},\"layers\":{},\"waits\":{},\"gpu_wait_ns\":{},\"pace_sleep_ns\":{},\"begins\":{},\"acquired\":{},\"released\":{},\"input_time_s\":{},\"route_time_s\":{},\"right_trigger\":{},\"right_stick\":[{},{}],\"left_trigger\":{},\"left_stick\":[{},{}],\"paced\":{},\"haptic_left\":{},\"haptic_right\":{}{}}}\n",
                       frame, now, info.displayTime, script.display_period, last_end_ns ? now-last_end_ns : 0,
                       info.layerCount, stats.waits, last_gpu_wait_ns, last_pace_sleep_ns.load(),
                       stats.begins, stats.acquired, stats.released,
                       input_origin ? double(now-input_origin)/1e9 : 0, route_seconds(now),
                       script.right.trigger < 0 ? script.trigger : script.right.trigger,
                       script.right.thumbstick.x, script.right.thumbstick.y,
                       script.left.trigger < 0 ? script.trigger : script.left.trigger,
                       script.left.thumbstick.x, script.left.thumbstick.y, paced ? 1 : 0, haptic_per_hand[0], haptic_per_hand[1], content);
    if (++frame_log_unflushed >= frame_log_flush || now - frame_log_flushed_ns >= 1'000'000'000) {
      frame_log.flush();
      frame_log_unflushed = 0;
      frame_log_flushed_ns = now;
    }
    last_end_ns = now;
    // Gate results, and route-done.json once the route clock passes the route's
    // end or leaves the unpaced window: harnesses end the run there instead of
    // after a fixed duration.
    if (!gate_log.empty()) {
      std::ofstream gates(root / "gates.jsonl", std::ios::app);
      for (const auto& line : gate_log) gates << line << '\n';
      gate_log.clear();
    }
    if (!watch_log.empty()) {
      std::ofstream watches(root / "watches.jsonl", std::ios::app);
      for (const auto& line : watch_log) watches << line << '\n';
      watch_log.clear();
    }
    // Keep submitting while asynchronous readbacks finish. Only then tell the
    // supervisor that terminal evidence is ready and normal teardown can start.
    if (!route_done_written && end && input_origin && route_seconds(now) >= *end &&
        terminal_capture.complete(true, every != 0, capture_writer.idle())) {
      route_done_written = true;
      frame_log.flush();
      std::ofstream done(root / "route-done.json");
      done << std::format("{{\"frame\":{},\"completion_frame\":{},\"route_time_s\":{},\"input_time_s\":{},\"held_s\":{}}}\n",
                          terminal_capture.frame() ? terminal_capture.frame() : frame, frame,
                          route_seconds(now), double(now - input_origin) / 1e9, route_offset_s.load());
      std::cerr << "[mock-xr] route done at route time " << route_seconds(now) << " s\n";
    }
    // VENTURE_MOCK_AV_WINDOW_ROUTE_S: the AV acceptance window opens at this route time.
    // Gates make its host time unknown in advance, so publish it (written whole, then
    // renamed) for the runtime's audio capture and the acceptance check.
    static const auto av_window = [] {
      const char* text = venture::environment::getenv("VENTURE_MOCK_AV_WINDOW_ROUTE_S");
      return text && *text ? std::optional<double>(std::stod(text)) : std::nullopt;
    }();
    if (av_window && !av_window_written && input_origin && route_seconds(now) >= *av_window) {
      av_window_written = true;
      const auto marker = root / "av-window-start.json";
      {
        std::ofstream out(marker.string() + ".tmp");
        out << std::format("{{\"start_ns\":{},\"frame\":{},\"route_time_s\":{}}}\n", now, frame, route_seconds(now));
      }
      std::filesystem::rename(marker.string() + ".tmp", marker);
      std::cerr << "[mock-xr] AV window opens at route time " << route_seconds(now) << " s\n";
    }
    if (frame == 1 || frame % 90 == 0)
      std::cerr << "[mock-xr] submitted frame " << frame << " layers=" << info.layerCount << '\n';
  } catch (const std::exception& error) {
    std::cerr << "[mock-xr] diagnostics stopped: frame recording failed: " << error.what() << '\n';
    close_frame_log();
    frame_log_finished = true;
  }
}
template <class F> F vk(const char *name) {
  return reinterpret_cast<F>(venture::xr::native_vulkan_loader()(vulkan_instance, name));
}
// Called from xrEndFrame. XR_KHR_vulkan_enable lists xrEndFrame among the calls
// for which the application externally synchronizes the binding queue.
void fence_frame() {
  if (d3d11::active()) {
    d3d11::fence_frame(begun_frame);
    return;
  }
  if (!graphics_queue)
    return;
  VkFence fence{};
  if (!spare_fences.empty()) {
    fence = spare_fences.back();
    spare_fences.pop_back();
  } else {
    VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vk<PFN_vkCreateFence>("vkCreateFence")(graphics.device, &info, nullptr, &fence) != VK_SUCCESS)
      return;
  }
  if (vk<PFN_vkQueueSubmit>("vkQueueSubmit")(graphics_queue, 0, nullptr, fence) != VK_SUCCESS) {
    spare_fences.push_back(fence);
    return;
  }
  frame_fences.push_back({begun_frame, fence});
}
// Called from xrWaitFrame for frame `next` with the state lock held; blocks
// without it until frame next-2 (and so everything submitted before it) is done.
void wait_for_submitted_frames(std::unique_lock<std::recursive_mutex>& lock, std::uint32_t next) {
  if (d3d11::active()) {
    const auto value = d3d11::due(next);
    lock.unlock();
    const auto waited = d3d11::wait(value, gpu_wait_bound_ns);
    lock.lock();
    last_gpu_wait_ns = waited;
    return;
  }
  auto due = std::find_if(frame_fences.rbegin(), frame_fences.rend(),
                          [&](const FrameFence& f) { return f.frame + 2 <= next; });
  // A pipelined app ends frame N during this wait for N+1; it records the last
  // completed wait, so the duration is only published once the wait is over.
  if (due == frame_fences.rend()) {
    last_gpu_wait_ns = 0;
    return;
  }
  const auto device = graphics.device;
  const auto wait = vk<PFN_vkWaitForFences>("vkWaitForFences");
  const auto status = vk<PFN_vkGetFenceStatus>("vkGetFenceStatus");
  const auto reset = vk<PFN_vkResetFences>("vkResetFences");
  auto newest = due->fence;
  const auto start = venture::xr::native_now_ns();
  lock.unlock();
  const auto result = wait(device, 1, &newest, VK_TRUE, gpu_wait_bound_ns);
  lock.lock();
  last_gpu_wait_ns = venture::xr::native_now_ns() - start;
  if (result != VK_SUCCESS) {
    static std::atomic_uint reports{};
    if (reports.fetch_add(1, std::memory_order_relaxed) < 8)
      std::cerr << "[mock-xr] frame before last not GPU-complete after "
                << last_gpu_wait_ns / 1'000'000 << " ms: VkResult " << result << '\n';
  }
  // One queue completes in submission order: recycle the signaled prefix.
  while (!frame_fences.empty() && status(device, frame_fences.front().fence) == VK_SUCCESS) {
    auto fence = frame_fences.front().fence;
    frame_fences.pop_front();
    if (reset(device, 1, &fence) == VK_SUCCESS)
      spare_fences.push_back(fence);
    else
      vk<PFN_vkDestroyFence>("vkDestroyFence")(device, fence, nullptr);
  }
}
void release_frame_fences() {
  if (graphics.device) {
    std::vector<VkFence> pending;
    for (const auto& f : frame_fences)
      pending.push_back(f.fence);
    // A fence still pending after the bound is leaked rather than destroyed in use.
    if (pending.empty() ||
        vk<PFN_vkWaitForFences>("vkWaitForFences")(graphics.device, std::uint32_t(pending.size()),
                                                   pending.data(), VK_TRUE, gpu_wait_bound_ns) == VK_SUCCESS)
      spare_fences.insert(spare_fences.end(), pending.begin(), pending.end());
    for (auto fence : spare_fences)
      vk<PFN_vkDestroyFence>("vkDestroyFence")(graphics.device, fence, nullptr);
  }
  frame_fences.clear();
  spare_fences.clear();
  graphics_queue = {};
}
template <class T>
XrResult enumerate(std::uint32_t cap, std::uint32_t *count, T *out, const std::vector<T> &values) {
  if (!count || (cap && !out))
    return XR_ERROR_VALIDATION_FAILURE;
  *count = static_cast<std::uint32_t>(values.size());
  if (!cap)
    return XR_SUCCESS;
  if (cap < values.size())
    return XR_ERROR_SIZE_INSUFFICIENT;
  std::copy(values.begin(), values.end(), out);
  return XR_SUCCESS;
}
XrResult m_xrEnumerateInstanceExtensionProperties(const char *, std::uint32_t cap,
                                                  std::uint32_t *count,
                                                  XrExtensionProperties *out) {
  std::vector<XrExtensionProperties> extensions;
  if (d3d11::available()) {
    XrExtensionProperties e{XR_TYPE_EXTENSION_PROPERTIES};
    std::strcpy(e.extensionName, "XR_KHR_D3D11_enable");
    e.extensionVersion = 9;
    extensions.push_back(e);
  }
  for (auto name : {"XR_KHR_vulkan_enable", "XR_KHR_vulkan_enable2", "XR_EXT_hand_tracking", "XR_EXT_local_floor",
                    VENTURE_MOCK_TIME_EXTENSION, "XR_KHR_composition_layer_cube",
                    "XR_KHR_composition_layer_cylinder", "XR_KHR_composition_layer_equirect",
                    "XR_KHR_composition_layer_depth", "XR_KHR_swapchain_usage_input_attachment_bit",
                    "XR_KHR_composition_layer_equirect2",
                    "XR_KHR_composition_layer_color_scale_bias"}) {
    XrExtensionProperties e{XR_TYPE_EXTENSION_PROPERTIES};
    std::strcpy(e.extensionName, name);
    e.extensionVersion = 1;
    extensions.push_back(e);
  }
  if (space_warp()) {
    XrExtensionProperties e{XR_TYPE_EXTENSION_PROPERTIES};
    std::strcpy(e.extensionName, "XR_FB_space_warp");
    e.extensionVersion = 2;
    extensions.push_back(e);
  }
  if (pcm_rate()) {
    XrExtensionProperties e{XR_TYPE_EXTENSION_PROPERTIES};
    std::strcpy(e.extensionName, "XR_FB_haptic_pcm");
    e.extensionVersion = 1;
    extensions.push_back(e);
  }
  return enumerate(cap, count, out, extensions);
}
XrResult m_xrCreateInstance(const XrInstanceCreateInfo *info, XrInstance *out) {
  if (!info || !out || info->type != XR_TYPE_INSTANCE_CREATE_INFO || info->next)
    return XR_ERROR_VALIDATION_FAILURE;
  input_attachment_usage = false;
  for (std::uint32_t i = 0; i < info->enabledExtensionCount; ++i)
    if (std::string_view(info->enabledExtensionNames[i]) == "XR_KHR_swapchain_usage_input_attachment_bit")
      input_attachment_usage = true;
  try {
    const auto env = [](const char* name) -> std::string_view {
      const auto* value = venture::environment::getenv(name);
      return value ? value : "";
    };
    view_configuration_state = view_configuration(env("VENTURE_MOCK_PROFILE"), env("VENTURE_MOCK_VIEW_SIZE"),
        env("VENTURE_MOCK_VIEW_WIDTH"), env("VENTURE_MOCK_VIEW_HEIGHT"), env("VENTURE_MOCK_REFRESH_HZ"));
    if (!env("VENTURE_MOCK_PROFILE").empty() || !env("VENTURE_MOCK_REFRESH_HZ").empty())
      script.display_period = view_configuration_state.period_ns;
    recorded_swapchains.clear();
    recorded_views.clear();
    timeline = {};
    input_path.clear();
    if (const char* input = venture::environment::getenv("VENTURE_MOCK_INPUT")) {
      input_path = std::filesystem::u8path(input);
      timeline.load(input_path);
      input_write_time = std::filesystem::last_write_time(input_path);
      timeline.advance(0, script);
      std::cerr << "[mock-xr] loaded input timeline " << input << '\n';
    }
  } catch (const std::exception& error) {
    std::cerr << "[mock-xr] configuration failed: " << error.what() << '\n';
    return XR_ERROR_INITIALIZATION_FAILED;
  }
  // OpenXR spaces.adoc: LOCAL may be calibrated at launch, gravity-aligned;
  // STAGE is on the floor. Conflating them adds standing height a second time
  // in seated applications which place their camera origin at the driver's eye.
  const auto forward = venture::xr::rotate(script.head.orientation, {0, 0, -1});
  const auto yaw = std::atan2(-forward.x, -forward.z);
  local_origin = {{0, std::sin(yaw / 2), 0, std::cos(yaw / 2)}, script.head.position};
  instance = token<XrInstance>();
  frame_log_finished = false;
  *out = instance;
  return XR_SUCCESS;
}
XrResult m_xrDestroyInstance(XrInstance i) {
  std::lock_guard lock(mutex);
  if (i != instance)
    return XR_ERROR_HANDLE_INVALID;
  capture_writer.drain();
  finish_diagnostics();
  instance = {};
  return XR_SUCCESS;
}
XrResult m_xrGetInstanceProperties(XrInstance, XrInstanceProperties *out) {
  out->runtimeVersion = XR_MAKE_VERSION(1, 0, 1);
  std::strcpy(out->runtimeName, "Venture Mock OpenXR");
  return XR_SUCCESS;
}
XrResult m_xrGetSystem(XrInstance, const XrSystemGetInfo *info, XrSystemId *out) {
  if (info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY)
    return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
  if (!script.hmd || venture::environment::getenv("VENTURE_MOCK_NO_HMD"))
    return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
  *out = 1;
  return XR_SUCCESS;
}
XrResult m_xrGetSystemProperties(XrInstance, XrSystemId id, XrSystemProperties *out) {
  if (id != 1)
    return XR_ERROR_SYSTEM_INVALID;
  std::set<const void*> seen;
  bool hand_written = false;
  for (auto* node = static_cast<XrBaseOutStructure*>(out->next); node; node = node->next) {
    if (!seen.insert(node).second || seen.size() > 64)
      return XR_ERROR_VALIDATION_FAILURE;
    if (node->type == XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT && !hand_written) {
      reinterpret_cast<XrSystemHandTrackingPropertiesEXT*>(node)->supportsHandTracking = XR_TRUE;
      hand_written = true;
    }
    if (node->type == XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB && space_warp()) {
      auto& properties = *reinterpret_cast<XrSystemSpaceWarpPropertiesFB*>(node);
      properties.recommendedMotionVectorImageRectWidth = view_configuration_state.width / 4;
      properties.recommendedMotionVectorImageRectHeight = view_configuration_state.height / 4;
    }
  }
  out->systemId = 1;
  out->vendorId = 0;
  std::strcpy(out->systemName, "Scripted HMD");
  out->graphicsProperties = {4096, 4096, 16};
  // Parity runs report the compared runtime's limit (Monado: 16384x16384), so a guest swapchain above
  // 4096 takes the same host path on both arms.
  if (const char* text = venture::environment::getenv("VENTURE_MOCK_MAX_SWAPCHAIN")) {
    unsigned width = 0, height = 0;
    if (std::sscanf(text, "%ux%u", &width, &height) == 2 && width && height) {
      out->graphicsProperties.maxSwapchainImageWidth = width;
      out->graphicsProperties.maxSwapchainImageHeight = height;
    }
  }
  // A host that takes fewer composition layers than a Quest (SteamVR on Linux).
  if (const char* text = venture::environment::getenv("VENTURE_MOCK_MAX_LAYERS"); text && std::atoi(text) > 0)
    out->graphicsProperties.maxLayerCount = std::uint32_t(std::atoi(text));
  out->trackingProperties = {XR_TRUE, XR_TRUE};
  // SteamVR 2.18 with a Quest over Link reports zero limits and no tracking.
  if (const char* text = venture::environment::getenv("VENTURE_MOCK_ZERO_SYSTEM_LIMITS"); text && *text == '1') {
    out->graphicsProperties = {0, 0, 0};
    out->trackingProperties = {XR_FALSE, XR_FALSE};
  }
  return XR_SUCCESS;
}
XrResult m_xrEnumerateViewConfigurations(XrInstance, XrSystemId, std::uint32_t cap,
                                         std::uint32_t *count, XrViewConfigurationType *out) {
  return enumerate(cap, count, out,
                   std::vector<XrViewConfigurationType>{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO});
}
XrResult m_xrGetViewConfigurationProperties(XrInstance, XrSystemId, XrViewConfigurationType type,
                                            XrViewConfigurationProperties *out) {
  out->viewConfigurationType = type;
  out->fovMutable = XR_FALSE;
  return XR_SUCCESS;
}
XrResult m_xrEnumerateViewConfigurationViews(XrInstance, XrSystemId, XrViewConfigurationType,
                                             std::uint32_t cap, std::uint32_t *count,
                                             XrViewConfigurationView *out) {
  XrViewConfigurationView view{
      XR_TYPE_VIEW_CONFIGURATION_VIEW, nullptr, view_configuration_state.width, 4096, view_configuration_state.height, 4096, 1, 1};
  return enumerate(cap, count, out, std::vector<XrViewConfigurationView>{view, view});
}
XrResult m_xrEnumerateEnvironmentBlendModes(XrInstance, XrSystemId, XrViewConfigurationType,
                                            std::uint32_t cap, std::uint32_t *count,
                                            XrEnvironmentBlendMode *out) {
  return enumerate(cap, count, out,
                   std::vector<XrEnvironmentBlendMode>{XR_ENVIRONMENT_BLEND_MODE_OPAQUE});
}
XrResult m_xrGetVulkanGraphicsRequirements2KHR(XrInstance, XrSystemId,
                                               XrGraphicsRequirementsVulkanKHR *out) {
  out->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
  out->maxApiVersionSupported = XR_MAKE_VERSION(1, 4, 0);
  return XR_SUCCESS;
}
XrResult m_xrGetVulkanGraphicsRequirementsKHR(XrInstance i, XrSystemId s,
                                              XrGraphicsRequirementsVulkanKHR *out) {
  return m_xrGetVulkanGraphicsRequirements2KHR(i, s, out);
}
XrResult m_xrGetVulkanInstanceExtensionsKHR(XrInstance, XrSystemId, std::uint32_t cap,
                                            std::uint32_t *count, char *out) {
  return enumerate(cap, count, out, std::vector<char>{'\0'});
}
XrResult m_xrGetVulkanDeviceExtensionsKHR(XrInstance i, XrSystemId s, std::uint32_t c,
                                          std::uint32_t *n, char *out) {
  return m_xrGetVulkanInstanceExtensionsKHR(i, s, c, n, out);
}
std::set<VkInstance> created_instances;
XrResult m_xrCreateVulkanInstanceKHR(XrInstance, const XrVulkanInstanceCreateInfoKHR *info,
                                     VkInstance *out, VkResult *result) {
  auto create = reinterpret_cast<PFN_vkCreateInstance>(
      info->pfnGetInstanceProcAddr(nullptr, "vkCreateInstance"));
  *result = create(info->vulkanCreateInfo, info->vulkanAllocator, out);
  if (*result == VK_SUCCESS) {
    vulkan_instance = *out;
    created_instances.insert(*out);
  }
  return XR_SUCCESS;
}
XrResult graphics_device(VkInstance instance, VkPhysicalDevice *out) {
  vulkan_instance = instance;
  std::uint32_t n = 0;
  auto fn = vk<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
  if (fn(vulkan_instance, &n, nullptr) != VK_SUCCESS || !n)
    return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  std::vector<VkPhysicalDevice> devices(n);
  fn(vulkan_instance, &n, devices.data());
  // A PC headset hangs off the discrete GPU. Loader order is not a preference
  // (Mesa lists a hybrid laptop's iGPU first), so pick the first discrete GPU.
  auto properties = vk<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties");
  *out = devices.front();
  for (auto device : devices) {
    VkPhysicalDeviceProperties p{};
    properties(device, &p);
    if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { *out = device; break; }
  }
  return XR_SUCCESS;
}
XrResult m_xrGetVulkanGraphicsDevice2KHR(XrInstance, const XrVulkanGraphicsDeviceGetInfoKHR *info,
                                         VkPhysicalDevice *out) {
  // XR_KHR_vulkan_enable2: the instance must come from xrCreateVulkanInstanceKHR.
  // The Oculus runtime enforces this with XR_ERROR_RUNTIME_FAILURE.
  if (!created_instances.contains(info->vulkanInstance)) return XR_ERROR_RUNTIME_FAILURE;
  return graphics_device(info->vulkanInstance, out);
}
XrResult m_xrGetVulkanGraphicsDeviceKHR(XrInstance, XrSystemId, VkInstance v, VkPhysicalDevice *out) {
  if (script.refuse_app_vulkan_instances && !created_instances.contains(v)) return XR_ERROR_RUNTIME_FAILURE;
  return graphics_device(v, out);  // XR_KHR_vulkan_enable: application-created instances
}
XrResult m_xrCreateVulkanDeviceKHR(XrInstance, const XrVulkanDeviceCreateInfoKHR *info,
                                   VkDevice *out, VkResult *result) {
  // XR_KHR_vulkan_enable2 requires using the supplied resolver. It can wrap
  // allocation callbacks and feature requests before the native driver call.
  auto create = reinterpret_cast<PFN_vkCreateDevice>(
      info->pfnGetInstanceProcAddr(vulkan_instance, "vkCreateDevice"));
  *result = create(info->vulkanPhysicalDevice, info->vulkanCreateInfo, info->vulkanAllocator, out);
  return XR_SUCCESS;
}
XrResult m_xrCreateSession(XrInstance, const XrSessionCreateInfo *info, XrSession *out) {
  if (session)
    return XR_ERROR_LIMIT_REACHED;
  graphics_queue = {};
  if (XrResult bound; d3d11::bind(info->next, &bound)) {
    if (XR_FAILED(bound)) return bound;
    graphics = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    vulkan_instance = {};
  } else {
    auto b = static_cast<const XrGraphicsBindingVulkanKHR *>(info->next);
    if (!b || b->type != XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR || !b->device)
      return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    graphics = *b;
    vulkan_instance = b->instance;
    if (auto get_queue = vk<PFN_vkGetDeviceQueue>("vkGetDeviceQueue"))
      get_queue(b->device, b->queueFamilyIndex, b->queueIndex, &graphics_queue);
  }
  last_gpu_wait_ns = 0;
  session = token<XrSession>();
  *out = session;
  running = false;
  frame_phase = 0;
  pending_frame = false;
  restart_pending = exit_pending = session_lost = false;
  input_origin = 0;
  route_offset_s = 0;
  gate_sampling = false;
  watch_sampling = false;
  watch_runner = {};
  watch_signature_ns = 0;
  action_reads_logged.clear();
  gate_last_projection = false;
  route_done_written = false;
  terminal_capture = {};
  content_last_projection = false;
  content_selection_frame = 0;
  window_start_captured = false;
  av_window_written = false;
  attached_sets.clear();
  action_states.clear();
  profile_changed = false;
  session_state = XR_SESSION_STATE_IDLE;
  events = {XR_SESSION_STATE_IDLE, XR_SESSION_STATE_READY};
  return XR_SUCCESS;
}
XrResult m_xrDestroySession(XrSession s) {
  std::lock_guard lock(mutex);
  if (gated_end) ++destroys_during_end;
  if (s != session)
    return XR_ERROR_HANDLE_INVALID;
  if (!chains.empty())
    return XR_ERROR_CALL_ORDER_INVALID;
  capture_writer.drain();
  vulkan_content.release();
  release_frame_fences();
  d3d11::unbind();
  close_frame_log();
  session = {};
  running = false;
  restart_pending = exit_pending = session_lost = false;
  frame_changed.notify_all();
  spaces.clear();
  attached_sets.clear();
  action_states.clear();
  events.clear();
  return XR_SUCCESS;
}
XrResult m_xrBeginSession(XrSession s, const XrSessionBeginInfo *) {
  std::lock_guard lock(mutex);
  if (s != session)
    return XR_ERROR_HANDLE_INVALID;
  if (running)
    return XR_ERROR_SESSION_RUNNING;
  if (session_state != XR_SESSION_STATE_READY)
    return XR_ERROR_SESSION_NOT_READY;
  // Diagnostic: a runtime whose compositor failed to start refuses to begin
  // (Monado without a usable display target). VENTURE_MOCK_BEGIN_SESSION_RESULT
  // is a negative XrResult returned instead of starting the session.
  if (const auto* forced = venture::environment::getenv("VENTURE_MOCK_BEGIN_SESSION_RESULT")) {
    const std::string_view text = forced;
    std::int32_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value < 0)
      return static_cast<XrResult>(value);
  }
  running = true;
  // xrBeginSession: session frame state starts reset.
  pending_frame = false;
  frame_phase = 0;
  session_event_start_ns = venture::xr::native_now_ns();
  session_waits = 0;
  session_ends = 0;
  events = {XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_VISIBLE, XR_SESSION_STATE_FOCUSED};
  return XR_SUCCESS;
}
XrResult m_xrRequestExitSession(XrSession target) {
  std::lock_guard lock(mutex);
  if (target != session || !session) return XR_ERROR_HANDLE_INVALID;
  if (!running) return XR_ERROR_SESSION_NOT_RUNNING;
  if (!exit_pending && session_state != XR_SESSION_STATE_STOPPING)
    events.push_back(XR_SESSION_STATE_STOPPING);
  exit_pending = true;
  return XR_SUCCESS;
}
XrResult m_xrEndSession(XrSession target) {
  std::lock_guard lock(mutex);
  if (target != session || !session) return XR_ERROR_HANDLE_INVALID;
  if (session_lost) return XR_ERROR_SESSION_LOST;
  if (session_state != XR_SESSION_STATE_STOPPING)
    return XR_ERROR_SESSION_NOT_STOPPING;
  running = false;
  close_frame_log();
  if (exit_pending) {
    exit_pending = restart_pending = false;
    events.push_back(XR_SESSION_STATE_IDLE);
    events.push_back(XR_SESSION_STATE_EXITING);
  } else if (restart_pending) {
    restart_pending = false;
    events.push_back(XR_SESSION_STATE_IDLE);
    events.push_back(XR_SESSION_STATE_READY);
  }
  frame_changed.notify_all();
  return XR_SUCCESS;
}
XrResult m_xrPollEvent(XrInstance, XrEventDataBuffer *out) {
  std::lock_guard lock(mutex);
  if (events.empty() && profile_changed) {
    XrEventDataInteractionProfileChanged event{XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED};
    event.session = session;
    std::memcpy(out, &event, sizeof(event));
    profile_changed = false;
    return XR_SUCCESS;
  }
  if (events.empty())
    return XR_EVENT_UNAVAILABLE;
  // Some compositors synchronize only after the first completed frame, even
  // when that frame has no layers. Model that startup contract on request.
  if (venture::environment::getenv("VENTURE_MOCK_REQUIRE_INITIAL_END_FRAME") &&
      session_ends == 0 && events.front() >= XR_SESSION_STATE_SYNCHRONIZED &&
      events.front() <= XR_SESSION_STATE_FOCUSED)
    return XR_EVENT_UNAVAILABLE;
  // Diagnostic-only host lifecycle timing. Default remains immediate. Real
  // runtimes may spend substantial time synchronizing before visibility/focus.
  const auto* state_delay_value = venture::environment::getenv("VENTURE_MOCK_SESSION_EVENT_STEP_MS");
  const std::string_view state_delay = state_delay_value ? state_delay_value : "";
  std::int64_t milliseconds{};
  const auto parsed = std::from_chars(state_delay.data(), state_delay.data() + state_delay.size(), milliseconds);
  if (!state_delay.empty() && parsed.ec == std::errc{} && parsed.ptr == state_delay.data() + state_delay.size() &&
      milliseconds > 0 && milliseconds <= 60000 &&
      events.front() >= XR_SESSION_STATE_SYNCHRONIZED && events.front() <= XR_SESSION_STATE_FOCUSED) {
    const auto due = session_event_start_ns + milliseconds * 1000000 *
        (int(events.front()) - int(XR_SESSION_STATE_READY));
    if (venture::xr::native_now_ns() < due) return XR_EVENT_UNAVAILABLE;
  }
  XrEventDataSessionStateChanged event{XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED};
  event.session = session;
  event.state = events.front();
  events.pop_front();
  event.time = venture::xr::native_now_ns();
  session_state = event.state;
  std::memcpy(out, &event, sizeof(event));
  return XR_SUCCESS;
}
XrResult m_xrEnumerateReferenceSpaces(XrSession, std::uint32_t cap, std::uint32_t *count,
                                      XrReferenceSpaceType *out) {
  return enumerate(cap, count, out,
                   std::vector<XrReferenceSpaceType>{XR_REFERENCE_SPACE_TYPE_VIEW,
                                                     XR_REFERENCE_SPACE_TYPE_LOCAL,
                                                     XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR,
                                                     XR_REFERENCE_SPACE_TYPE_STAGE});
}
XrResult m_xrCreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo *info, XrSpace *out) {
  if (info->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_VIEW &&
      info->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_LOCAL &&
      info->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR &&
      info->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_STAGE)
    return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
  // LOCAL_FLOOR retains LOCAL's calibrated horizontal origin and yaw, with
  // its height projected onto the STAGE floor (OpenXR spaces.adoc).
  *out = token<XrSpace>();
  spaces[*out] = {info->referenceSpaceType, info->poseInReferenceSpace};
  return XR_SUCCESS;
}
XrResult m_xrGetReferenceSpaceBoundsRect(XrSession, XrReferenceSpaceType type, XrExtent2Df *out) {
  if (type != XR_REFERENCE_SPACE_TYPE_STAGE)
    return XR_SPACE_BOUNDS_UNAVAILABLE;
  *out = {2, 2};
  return XR_SUCCESS;
}
XrResult m_xrDestroySpace(XrSpace space) {
  { std::lock_guard lock(mutex); if (gated_end) ++destroys_during_end; }
  return spaces.erase(space) ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}
XrResult m_xrLocateSpace(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation *out) {
  if (!spaces.contains(space) || !spaces.contains(base))
    return XR_ERROR_HANDLE_INVALID;
  if (time <= 0)
    return XR_ERROR_TIME_INVALID;
  const bool active = space_active(space) && space_active(base);
  out->locationFlags = active ? 15 : 0;
  out->pose = venture::xr::compose(venture::xr::inverse(world_pose(base)), world_pose(space));
  if (out->next) {
    auto velocity = static_cast<XrSpaceVelocity *>(out->next);
    if (velocity->type != XR_TYPE_SPACE_VELOCITY)
      return XR_ERROR_VALIDATION_FAILURE;
    const auto relative = space == base ? XrSpaceVelocityData{3, {}, {}}
        : relative_velocity(world_pose(space), spaces.at(space).velocity,
                            world_pose(base), spaces.at(base).velocity);
    velocity->velocityFlags = active ? relative.velocityFlags : 0;
    velocity->linearVelocity = relative.linearVelocity;
    velocity->angularVelocity = relative.angularVelocity;
    // VENTURE_MOCK_NO_VELOCITY: a runtime that reports no velocities, leaving the (then undefined)
    // values NaN, as the OpenXR spec allows.
    static const bool no_velocity = venture::environment::getenv("VENTURE_MOCK_NO_VELOCITY") != nullptr;
    if (no_velocity) {
      constexpr float nan = std::numeric_limits<float>::quiet_NaN();
      velocity->velocityFlags = 0;
      velocity->linearVelocity = velocity->angularVelocity = {nan, nan, nan};
    }
  }
  return XR_SUCCESS;
}
XrResult m_xrLocateViews(XrSession, const XrViewLocateInfo *info, XrViewState *state,
                         std::uint32_t cap, std::uint32_t *count, XrView *out) {
  if (info->displayTime <= 0)
    return XR_ERROR_TIME_INVALID;
  std::vector<XrView> views(2, {XR_TYPE_VIEW});
  if (!spaces.contains(info->space))
    return XR_ERROR_HANDLE_INVALID;
  auto head = venture::xr::compose(venture::xr::inverse(world_pose(info->space)), script.head);
  for (int eye = 0; eye < 2; ++eye) {
    views[eye].pose = venture::xr::compose(head, {{0, 0, 0, 1}, {eye ? 0.032f : -0.032f, 0, 0}});
    views[eye].fov = {mock::host_fov[0], mock::host_fov[1], mock::host_fov[2], mock::host_fov[3]};
  }
  state->viewStateFlags = script.active ? 15 : 0;
  return enumerate(cap, count, out, views);
}
XrResult m_xrWaitFrame(XrSession, const XrFrameWaitInfo *, XrFrameState *out) {
  std::unique_lock lock(mutex);
  if (session_lost) return XR_ERROR_SESSION_LOST;
  // VENTURE_MOCK_LOSE_AFTER_S: the host runtime goes away (a crashed or closed SteamVR/Monado) this many seconds
  // after the first frame wait, so a real title's path through session loss can be exercised.
  static const double lose_after = [] {
    const char* text = venture::environment::getenv("VENTURE_MOCK_LOSE_AFTER_S");
    return text ? std::strtod(text, nullptr) : 0.;
  }();
  static XrTime first_wait = 0;
  if (lose_after > 0) {
    const auto now = venture::xr::native_now_ns();
    if (!first_wait) first_wait = now;
    else if (now - first_wait >= XrTime(lose_after * 1e9)) {
      session_lost = true;
      events.push_back(XR_SESSION_STATE_LOSS_PENDING);
      frame_changed.notify_all();
      return XR_ERROR_SESSION_LOST;
    }
  }
  if (!running)
    return XR_ERROR_SESSION_NOT_RUNNING;
  // xrWaitFrame's next call is paced by BeginFrame, not EndFrame...
  const auto ready = [] { return !pending_frame || !running || session_lost; };
  frame_changed.wait(lock, ready);
  if (session_lost) return XR_ERROR_SESSION_LOST;
  if (!running) return XR_ERROR_SESSION_NOT_RUNNING;
  // ...and, as by a compositor, by GPU completion of the frame before last.
  wait_for_submitted_frames(lock, stats.waits + 1);
  frame_changed.wait(lock, ready);
  if (session_lost) return XR_ERROR_SESSION_LOST;
  if (!running) return XR_ERROR_SESSION_NOT_RUNNING;
  if (gate_wait) {
    gated_wait = true;
    frame_changed.notify_all();
    frame_changed.wait(lock, [] { return !gate_wait || !running || session_lost; });
    gated_wait = false;
    frame_changed.notify_all();
    if (session_lost) return XR_ERROR_SESSION_LOST;
    if (!running) return XR_ERROR_SESSION_NOT_RUNNING;
  }
  ++stats.waits;
  ++session_waits;
  // Unpaced frames come faster than the display period: predict from now, or the
  // lead would build up and be slept off in one wait when pacing resumes.
  const auto wait_now = venture::xr::native_now_ns();
  predicted = unpaced(wait_now) ? wait_now + script.display_period
                                : std::max(predicted + script.display_period, wait_now + script.display_period);
  out->predictedDisplayTime = predicted;
  out->predictedDisplayPeriod = script.display_period;
  // Session-event delivery and the compositor's render request are independent.
  // A normal delayed compositor still requests the first image before VISIBLE.
  // Preserve the explicit pulse-then-false diagnostic for historical replay.
  const bool pulse = venture::environment::getenv("VENTURE_MOCK_INITIAL_RENDER_PULSE") != nullptr;
  out->shouldRender = script.should_render &&
      (!pulse || session_state == XR_SESSION_STATE_VISIBLE || session_state == XR_SESSION_STATE_FOCUSED ||
       session_waits == 1);
  pending_frame = true;
  last_pace_sleep_ns = 0;
  if (const char* paced = venture::environment::getenv("VENTURE_MOCK_REALTIME");
      paced && std::strcmp(paced, "0") != 0 && !unpaced(venture::xr::native_now_ns())) {
    const auto start = venture::xr::native_now_ns();
    const auto delay = predicted - script.display_period - start;
    lock.unlock();
    if (delay > 0) {
      std::this_thread::sleep_for(std::chrono::nanoseconds(delay));
      last_pace_sleep_ns = venture::xr::native_now_ns() - start;
    }
  }
  return XR_SUCCESS;
}
XrResult m_xrBeginFrame(XrSession, const XrFrameBeginInfo *) {
  std::lock_guard lock(mutex);
  if (session_lost) return XR_ERROR_SESSION_LOST;
  if (!running) return XR_ERROR_SESSION_NOT_RUNNING;
  if (!pending_frame)
    return XR_ERROR_CALL_ORDER_INVALID;
  auto result = frame_phase == 2 ? XR_FRAME_DISCARDED : XR_SUCCESS;
  ++stats.begins;
  frame_phase = 2;
  begun_time = predicted;
  begun_frame = stats.waits;
  pending_frame = false;
  frame_changed.notify_all();
  return result;
}
XrResult m_xrEndFrame(XrSession, const XrFrameEndInfo *info) {
  std::unique_lock lock(mutex);
  if (gate_end) {
    gated_end = true;
    frame_changed.notify_all();
    frame_changed.wait(lock, [] { return !gate_end || session_lost; });
    gated_end = false;
    frame_changed.notify_all();
  }
  if (session_lost) return XR_ERROR_SESSION_LOST;
  if (!running) return XR_ERROR_SESSION_NOT_RUNNING;
  if (frame_phase != 2 || info->displayTime <= 0)
    return XR_ERROR_CALL_ORDER_INVALID;
  if (XR_FAILED(script.end_frame_result)) {
    if (!script.retain_failed_end) frame_phase = 0;
    return script.end_frame_result;
  }
  if (const char* text = venture::environment::getenv("VENTURE_MOCK_MAX_LAYERS");
      text && std::atoi(text) > 0 && info->layerCount > std::uint32_t(std::atoi(text))) {
    frame_phase = 0;
    return XR_ERROR_LAYER_LIMIT_EXCEEDED;
  }
  auto subimage = [](const XrSwapchainSubImage &sub) {
    auto it = chains.find(sub.swapchain);
    if (it == chains.end())
      return false;
    auto &c = it->second;
    return sub.imageArrayIndex < c.info.arraySize && sub.imageRect.offset.x >= 0 &&
           sub.imageRect.offset.y >= 0 && sub.imageRect.extent.width > 0 &&
           sub.imageRect.extent.height > 0 &&
           sub.imageRect.offset.x + sub.imageRect.extent.width <= int(c.info.width) &&
           sub.imageRect.offset.y + sub.imageRect.extent.height <= int(c.info.height);
  };
  auto invalid_layer = [](XrStructureType type, std::string_view fields) {
    static std::atomic_uint reports{};
    if (reports.fetch_add(1, std::memory_order_relaxed) < 8)
      std::cerr << "[mock-xr] rejected layer type=" << int(type) << ' ' << fields << '\n';
    return XR_ERROR_LAYER_INVALID;
  };
  std::vector<XrSwapchainSubImage> submitted;
  for (std::uint32_t i = 0; i < info->layerCount; ++i) {
    auto layer = info->layers[i];
    if (!layer || !spaces.contains(layer->space))
      return XR_ERROR_HANDLE_INVALID;
    switch (layer->type) {
    case XR_TYPE_COMPOSITION_LAYER_PROJECTION: {
      auto &p = *reinterpret_cast<const XrCompositionLayerProjection *>(layer);
      if (p.viewCount != 2 || !p.views)
        return invalid_layer(layer->type, std::format("views={}", p.viewCount));
      for (std::uint32_t eye = 0; eye < p.viewCount; ++eye)
        if (!subimage(p.views[eye].subImage))
          return XR_ERROR_SWAPCHAIN_RECT_INVALID;
      for (std::uint32_t eye = 0; eye < p.viewCount; ++eye)
        for (auto node = static_cast<const XrBaseInStructure*>(p.views[eye].next); node; node = node->next) {
          if (node->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
            const auto& depth = *reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(node);
            if (!subimage(depth.subImage) || depth.nearZ == depth.farZ)
              return invalid_layer(layer->type, "projection depth info");
            ++stats.projection_depth;
          }
          if (node->type != XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB) continue;
          auto& warp = *reinterpret_cast<const XrCompositionLayerSpaceWarpInfoFB*>(node);
          if (!space_warp() || !subimage(warp.motionVectorSubImage) || !subimage(warp.depthSubImage))
            return invalid_layer(layer->type, "space-warp info");
          ++stats.space_warp;
        }
      for (std::uint32_t eye = 0; eye < p.viewCount; ++eye)
        submitted.push_back(p.views[eye].subImage);
      ++stats.projection;
      break;
    }
    case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR: {
      auto &p = *reinterpret_cast<const XrCompositionLayerCylinderKHR *>(layer);
      // XrCompositionLayerCylinderKHR permits zero/+infinity radius (infinite
      // distance) and centralAngle in [0,2pi). Khronos OpenXR 1.1 reference:
      // registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerCylinderKHR.html
      constexpr float pi = std::numbers::pi_v<float>;
      if (!subimage(p.subImage) || !(p.radius >= 0) || !(p.aspectRatio > 0) ||
          !(p.centralAngle >= 0 && p.centralAngle < 2*pi))
        return invalid_layer(layer->type, std::format("cylinder subimage={} radius={} aspect={} angle={}",
            subimage(p.subImage), p.radius, p.aspectRatio, p.centralAngle));
      submitted.push_back(p.subImage);
      ++stats.cylinder;
      break;
    }
    case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR: {
      auto &p = *reinterpret_cast<const XrCompositionLayerCubeKHR *>(layer);
      if (!chains.contains(p.swapchain) || chains.at(p.swapchain).info.faceCount != 6 ||
          p.imageArrayIndex >= chains.at(p.swapchain).info.arraySize)
        return invalid_layer(layer->type, std::format("cube swapchain={} faces={} array={}",
            chains.contains(p.swapchain), chains.contains(p.swapchain) ? chains.at(p.swapchain).info.faceCount : 0, p.imageArrayIndex));
      ++stats.cube;
      break;
    }
    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR: {
      auto &p = *reinterpret_cast<const XrCompositionLayerEquirectKHR *>(layer);
      if (!subimage(p.subImage) || p.radius < 0)
        return invalid_layer(layer->type, std::format("equirect subimage={} radius={}", subimage(p.subImage), p.radius));
      submitted.push_back(p.subImage);
      ++stats.equirect;
      break;
    }
    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR: {
      const auto& p = *reinterpret_cast<const XrCompositionLayerEquirect2KHR*>(layer);
      // OpenXR XrCompositionLayerEquirect2KHR: nonnegative radius (including
      // positive infinity), horizontal [0,2pi], vertical [-pi/2,pi/2].
      constexpr float pi = std::numbers::pi_v<float>;
      if (!subimage(p.subImage) || chains.at(p.subImage.swapchain).info.faceCount != 1 ||
          !(p.radius >= 0) || !(p.centralHorizontalAngle >= 0 && p.centralHorizontalAngle <= 2*pi) ||
          !(p.lowerVerticalAngle >= -pi/2 && p.upperVerticalAngle <= pi/2 &&
            p.lowerVerticalAngle <= p.upperVerticalAngle) ||
          p.eyeVisibility < XR_EYE_VISIBILITY_BOTH || p.eyeVisibility > XR_EYE_VISIBILITY_RIGHT)
        {
          const auto chain = chains.find(p.subImage.swapchain);
          const auto& rect = p.subImage.imageRect;
          return invalid_layer(layer->type, std::format(
              "equirect2 subimage={} swapchain={} handle={:#x} rect=[{},{} {}x{}] array={} limits={}x{} arrays={} radius={} horizontal={} vertical=[{},{}] eye={}",
              subimage(p.subImage), chain != chains.end(), reinterpret_cast<std::uintptr_t>(p.subImage.swapchain), rect.offset.x, rect.offset.y,
              rect.extent.width, rect.extent.height, p.subImage.imageArrayIndex,
              chain != chains.end() ? chain->second.info.width : 0,
              chain != chains.end() ? chain->second.info.height : 0,
              chain != chains.end() ? chain->second.info.arraySize : 0,
              p.radius, p.centralHorizontalAngle, p.lowerVerticalAngle, p.upperVerticalAngle, int(p.eyeVisibility)));
        }
      submitted.push_back(p.subImage);
      ++stats.equirect;
      break;
    }
    case XR_TYPE_COMPOSITION_LAYER_QUAD: {
      auto &p = *reinterpret_cast<const XrCompositionLayerQuad *>(layer);
      if (!subimage(p.subImage) || p.size.width <= 0 || p.size.height <= 0)
        return invalid_layer(layer->type, std::format("quad subimage={} rect=[{},{} {}x{}] array={} size={}x{}",
            subimage(p.subImage), p.subImage.imageRect.offset.x, p.subImage.imageRect.offset.y,
            p.subImage.imageRect.extent.width, p.subImage.imageRect.extent.height, p.subImage.imageArrayIndex,
            p.size.width, p.size.height));
      submitted.push_back(p.subImage);
      ++stats.quad;
      break;
    }
    default:
      return invalid_layer(layer->type, "unsupported layer");
    }
  }
  record_frame(*info, submitted);
  fence_frame();
  last_subimages = std::move(submitted);
  ++stats.ends;
  last_display_time = info->displayTime;
  ++session_ends;
  frame_phase = 0;
  return XR_SUCCESS;
}
XrResult m_xrEnumerateSwapchainFormats(XrSession, std::uint32_t cap, std::uint32_t *count,
                                       std::int64_t *out) {
  if (d3d11::active()) return enumerate(cap, count, out, d3d11::formats());
  // SteamVR and Monado both list RGBA16F (space warp motion vectors use it).
  std::vector<std::int64_t> formats{VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB,
                                    VK_FORMAT_R16G16B16A16_SFLOAT};
  // Depth format support and motion-vector synthesis are independent host
  // capabilities. Tests can model a depth-capable desktop compositor.
  if (script.depth_formats || space_warp() || venture::environment::getenv("VENTURE_MOCK_DEPTH_FORMATS")) {
    // Like a desktop runtime, offer depth/stencil the device can attach: D24S8,
    // else D32S8 (RADV has no D24S8).
    auto stencil = VK_FORMAT_D24_UNORM_S8_UINT;
    const auto* listed = venture::environment::getenv("VENTURE_MOCK_DEPTH_FORMATS");
    if (listed && std::string_view(listed) == "nostencil") {
      // SteamVR on AMD (Windows): no depth/stencil format at all.
      formats.insert(formats.end(), {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM});
      return enumerate(cap, count, out, formats);
    }
    if (listed && std::string_view(listed) == "radv") {
      stencil = VK_FORMAT_D32_SFLOAT_S8_UINT;  // Monado on RADV: D32_SFLOAT, D16, D32S8
    } else if (graphics.physicalDevice) {
      VkFormatProperties support{};
      vk<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(
          graphics.physicalDevice, stencil, &support);
      if (!(support.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
        stencil = VK_FORMAT_D32_SFLOAT_S8_UINT;
    }
    formats.insert(formats.end(), {stencil, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM});
  }
  return enumerate(cap, count, out, formats);
}
void destroy_images(Chain &c) {
  if (d3d11::active()) {
    stats.live_images -= std::uint32_t(c.d3d_images.size());
    d3d11::destroy_images(c.d3d_images);
    return;
  }
  vk<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(graphics.device);
  for (auto image : c.images) {
    vk<PFN_vkDestroyImage>("vkDestroyImage")(graphics.device, image, nullptr);
    --stats.live_images;
  }
  for (auto memory : c.memory)
    vk<PFN_vkFreeMemory>("vkFreeMemory")(graphics.device, memory, nullptr);
}
// Diagnostic budget; zero/unset leaves the mock unlimited. Tests can use the
// same policy through MockXrScript without changing process environment.
std::uint32_t swapchain_limit() {
  auto limit = script.swapchain_limit;
  if (const char* value = venture::environment::getenv("VENTURE_MOCK_SWAPCHAIN_LIMIT")) {
    std::uint32_t parsed = 0;
    const auto end = value + std::strlen(value);
    const auto result = std::from_chars(value, end, parsed);
    if (result.ec == std::errc{} && result.ptr == end) limit = parsed;
  }
  return limit;
}
XrResult m_xrCreateSwapchain(XrSession, const XrSwapchainCreateInfo *info, XrSwapchain *out) {
  *out = XR_NULL_HANDLE;
  const auto limit = swapchain_limit();
  if (limit && chains.size() >= limit) {
    ++stats.swapchain_limit_rejections;
    std::clog << "[mock-xr] swapchain budget exhausted live=" << chains.size() << " limit=" << limit << '\n';
    return XR_ERROR_LIMIT_REACHED;
  }
  if (info->createFlags & ~(XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT | XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT))
    return XR_ERROR_VALIDATION_FAILURE;
  if (info->createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT)
    return XR_ERROR_FEATURE_UNSUPPORTED;
  // Like Monado: the input-attachment usage bit needs its extension enabled.
  if ((info->usageFlags & XR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_KHR) && !input_attachment_usage)
    return XR_ERROR_VALIDATION_FAILURE;
  if (info->next || !info->width || !info->height || info->sampleCount != 1)
    return XR_ERROR_VALIDATION_FAILURE;
  Chain c{};
  c.info = *info;
  // OpenXR 1.1 XrSwapchainCreateFlagBits: STATIC_IMAGE requires one image,
  // acquired/released only once during the entire swapchain lifetime.
  const auto image_count = info->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT ? 1 : 3;
  if (d3d11::active()) {
    if (const auto result = d3d11::create_images(*info, image_count, c.d3d_images); XR_FAILED(result))
      return result;
    stats.live_images += image_count;
  }
  for (int i = 0; i < image_count && !d3d11::active(); ++i) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = static_cast<VkFormat>(info->format);
    ci.extent = {info->width, info->height, 1};
    ci.mipLevels = info->mipCount;
    ci.arrayLayers = info->arraySize * info->faceCount;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    // OpenXR's Vulkan graphics binding requires at least the Vulkan usages
    // corresponding to XrSwapchainCreateInfo::usageFlags. Nexus samples its
    // color swapchain; allocating it only as an attachment breaks that contract.
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)
      ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
      ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT)
      ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT)
      ci.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_KHR)
      ci.usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    if (info->usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT)
      ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (info->faceCount == 6)
      ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VkImage image{};
    if (vk<PFN_vkCreateImage>("vkCreateImage")(graphics.device, &ci, nullptr, &image) !=
        VK_SUCCESS) {
      destroy_images(c);
      return XR_ERROR_OUT_OF_MEMORY;
    }
    c.images.push_back(image);
    ++stats.live_images;
    VkMemoryRequirements requirements;
    vk<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(graphics.device, image,
                                                                         &requirements);
    VkPhysicalDeviceMemoryProperties properties;
    vk<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(
        graphics.physicalDevice, &properties);
    std::uint32_t type = 0;
    while (type < properties.memoryTypeCount && !(requirements.memoryTypeBits & (1u << type)))
      ++type;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size,
                            type};
    VkDeviceMemory memory{};
    if (type == properties.memoryTypeCount ||
        vk<PFN_vkAllocateMemory>("vkAllocateMemory")(graphics.device, &ai, nullptr, &memory) !=
            VK_SUCCESS) {
      destroy_images(c);
      return XR_ERROR_OUT_OF_MEMORY;
    }
    c.memory.push_back(memory);
    if (vk<PFN_vkBindImageMemory>("vkBindImageMemory")(graphics.device, image, memory, 0) !=
        VK_SUCCESS) {
      destroy_images(c);
      return XR_ERROR_RUNTIME_FAILURE;
    }
  }
  *out = token<XrSwapchain>();
  static std::atomic_uint reports{};
  if (venture::environment::getenv("VENTURE_MOCK_DUMP_DIR") && reports.fetch_add(1, std::memory_order_relaxed) < 8)
    std::cerr << std::format("[mock-xr] created swapchain={:#x} {}x{} arrays={} faces={} format={} flags={:#x} images={}\n",
        reinterpret_cast<std::uintptr_t>(*out), info->width, info->height,
        info->arraySize, info->faceCount, info->format, info->createFlags, image_count);
  chains.emplace(*out, std::move(c));
  stats.live_swapchains = std::uint32_t(chains.size());
  stats.peak_swapchains = std::max(stats.peak_swapchains, stats.live_swapchains);
  if (limit) std::clog << "[mock-xr] swapchain budget create live=" << chains.size() << " limit=" << limit << '\n';
  return XR_SUCCESS;
}
XrResult m_xrUpdateSwapchainFB(XrSwapchain h, const XrSwapchainStateBaseHeaderFB* state) {
  if (!chains.contains(h)) return XR_ERROR_HANDLE_INVALID;
  if (!state || state->type != XR_TYPE_SWAPCHAIN_STATE_SAMPLER_VULKAN_FB) return XR_ERROR_VALIDATION_FAILURE;
  const auto& sampler = *reinterpret_cast<const XrSwapchainStateSamplerVulkanFB*>(state);
  if (!venture::xr::valid_sampler(sampler)) return XR_ERROR_VALIDATION_FAILURE;
  if (XR_FAILED(script.sampler_update_result)) return script.sampler_update_result;
  chains.at(h).sampler = sampler;
  return XR_SUCCESS;
}
XrResult m_xrGetSwapchainStateFB(XrSwapchain h, XrSwapchainStateBaseHeaderFB* state) {
  if (!chains.contains(h)) return XR_ERROR_HANDLE_INVALID;
  if (!state || state->type != XR_TYPE_SWAPCHAIN_STATE_SAMPLER_VULKAN_FB || state->next) return XR_ERROR_VALIDATION_FAILURE;
  *reinterpret_cast<XrSwapchainStateSamplerVulkanFB*>(state) = chains.at(h).sampler;
  return XR_SUCCESS;
}
XrResult m_xrDestroySwapchain(XrSwapchain h) {
  { std::lock_guard lock(mutex); if (gated_end) ++destroys_during_end; }
  auto it = chains.find(h);
  if (it == chains.end())
    return XR_ERROR_HANDLE_INVALID;
  capture_writer.drain();
  vulkan_content.finish();
  destroy_images(it->second);
  chains.erase(it);
  stats.live_swapchains = std::uint32_t(chains.size());
  if (const auto limit = swapchain_limit())
    std::clog << "[mock-xr] swapchain budget destroy live=" << chains.size() << " limit=" << limit << '\n';
  return XR_SUCCESS;
}
XrResult m_xrEnumerateSwapchainImages(XrSwapchain h, std::uint32_t cap, std::uint32_t *count,
                                      XrSwapchainImageBaseHeader *out) {
  auto it = chains.find(h);
  if (it == chains.end())
    return XR_ERROR_HANDLE_INVALID;
  *count = static_cast<std::uint32_t>(it->second.count());
  if (!cap)
    return XR_SUCCESS;
  if (cap < *count)
    return XR_ERROR_SIZE_INSUFFICIENT;
  if (d3d11::active()) return d3d11::enumerate_images(it->second.d3d_images, out);
  auto images = reinterpret_cast<XrSwapchainImageVulkanKHR *>(out);
  for (std::uint32_t i = 0; i < *count; ++i) {
    if (images[i].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR)
      return XR_ERROR_VALIDATION_FAILURE;
    images[i].image = it->second.images[i];
  }
  return XR_SUCCESS;
}
XrResult m_xrAcquireSwapchainImage(XrSwapchain h, const XrSwapchainImageAcquireInfo *,
                                   std::uint32_t *out) {
  auto &c = chains.at(h);
  if ((c.info.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) && c.sequence)
    return XR_ERROR_CALL_ORDER_INVALID;
  if (c.acquired.size() == c.count())
    return XR_ERROR_CALL_ORDER_INVALID;
  // Deliberately not 0,1,2: catches the VrApi ring-index/acquire-index confusion.
  *out = std::uint32_t((++c.sequence * 2) % c.count());
  c.acquired.push_back(*out);
  ++stats.acquired;
  return XR_SUCCESS;
}
XrResult m_xrWaitSwapchainImage(XrSwapchain h, const XrSwapchainImageWaitInfo *) {
  if (script.swapchain_wait_result != XR_SUCCESS && script.swapchain_wait_result != XR_SESSION_LOSS_PENDING)
    return script.swapchain_wait_result;
  auto &c = chains.at(h);
  if (c.acquired.empty() || c.waited)
    return XR_ERROR_CALL_ORDER_INVALID;
  c.waited = true;
  return script.swapchain_wait_result;
}
XrResult m_xrReleaseSwapchainImage(XrSwapchain h, const XrSwapchainImageReleaseInfo *) {
  auto &c = chains.at(h);
  if (c.acquired.empty() || !c.waited)
    return XR_ERROR_CALL_ORDER_INVALID;
  c.last_released = c.acquired.front();
  c.acquired.pop_front();
  c.waited = false;
  ++stats.released;
  return XR_SUCCESS;
}
XrResult m_xrStringToPath(XrInstance, const char *text, XrPath *out) {
  if (!text || *text != '/')
    return XR_ERROR_PATH_FORMAT_INVALID;
  auto [it, inserted] = paths.emplace(text, paths.size() + 1);
  if (inserted)
    path_names[it->second] = text;
  *out = it->second;
  return XR_SUCCESS;
}
XrResult m_xrPathToString(XrInstance, XrPath path, std::uint32_t cap, std::uint32_t *count,
                          char *out) {
  auto it = path_names.find(path);
  if (it == path_names.end())
    return XR_ERROR_PATH_INVALID;
  std::vector<char> value(it->second.begin(), it->second.end());
  value.push_back('\0');
  return enumerate(cap, count, out, value);
}
XrResult m_xrCreateActionSet(XrInstance, const XrActionSetCreateInfo *info, XrActionSet *out) {
  *out = token<XrActionSet>();
  action_sets.insert(*out);
  action_priorities[*out] = info->priority;
  return XR_SUCCESS;
}
XrResult m_xrDestroyActionSet(XrActionSet h) {
  action_priorities.erase(h);
  for (auto it = action_parents.begin(); it != action_parents.end();) {
    if (it->second == h) {
      actions.erase(it->first);
      it = action_parents.erase(it);
    } else
      ++it;
  }
  return action_sets.erase(h) ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}
XrResult m_xrCreateAction(XrActionSet parent, const XrActionCreateInfo *info, XrAction *out) {
  if (!action_sets.contains(parent)) return XR_ERROR_HANDLE_INVALID;
  if (attached_sets.contains(parent)) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
  if (info->countSubactionPaths && !info->subactionPaths) return XR_ERROR_VALIDATION_FAILURE;
  for (std::uint32_t i = 0; i < info->countSubactionPaths; ++i)
    if (!path_names.contains(info->subactionPaths[i])) return XR_ERROR_PATH_INVALID;
  *out = token<XrAction>();
  Action action{info->actionType, {}, info->actionName};
  if (info->countSubactionPaths)
    action.subactions.assign(info->subactionPaths, info->subactionPaths + info->countSubactionPaths);
  actions[*out] = std::move(action);
  action_parents[*out] = parent;
  return XR_SUCCESS;
}
XrResult m_xrDestroyAction(XrAction h) {
  action_parents.erase(h);
  return actions.erase(h) ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}
XrResult m_xrCreateActionSpace(XrSession, const XrActionSpaceCreateInfo *info, XrSpace *out) {
  if (!actions.contains(info->action)) return XR_ERROR_HANDLE_INVALID;
  if (actions.at(info->action).type != XR_ACTION_TYPE_POSE_INPUT) return XR_ERROR_ACTION_TYPE_MISMATCH;
  *out = token<XrSpace>();
  spaces[*out] = {XR_REFERENCE_SPACE_TYPE_LOCAL, info->poseInActionSpace, info->action, info->subactionPath};
  return XR_SUCCESS;
}
XrResult m_xrSuggestInteractionProfileBindings(XrInstance,
                                               const XrInteractionProfileSuggestedBinding *info) {
  if (!path_names.contains(info->interactionProfile)) return XR_ERROR_PATH_INVALID;
  for (std::uint32_t i = 0; i < info->countSuggestedBindings; ++i) {
    if (!actions.contains(info->suggestedBindings[i].action)) return XR_ERROR_HANDLE_INVALID;
    if (!path_names.contains(info->suggestedBindings[i].binding)) return XR_ERROR_PATH_INVALID;
  }
  bindings[info->interactionProfile] = {info->suggestedBindings,
                                        info->suggestedBindings + info->countSuggestedBindings};
  if (trace_input())
    for (const auto& binding : bindings[info->interactionProfile])
      std::cerr << "[mock-input] bind " << path_names[info->interactionProfile] << ' '
                << actions.at(binding.action).name << " -> " << path_names[binding.binding] << '\n';
  ++stats.bindings;
  return XR_SUCCESS;
}
XrResult m_xrEnumerateBoundSourcesForAction(XrSession,
                                            const XrBoundSourcesForActionEnumerateInfo *info,
                                            std::uint32_t capacity, std::uint32_t *count,
                                            XrPath *out) {
  std::vector<XrPath> sources;
  auto it = paths.find(timeline.profile());
  if (it != paths.end())
    for (auto binding : bindings[it->second])
      if (binding.action == info->action)
        sources.push_back(binding.binding);
  return enumerate(capacity, count, out, sources);
}
XrResult m_xrAttachSessionActionSets(XrSession s, const XrSessionActionSetsAttachInfo *info) {
  if (s != session) return XR_ERROR_HANDLE_INVALID;
  if (!attached_sets.empty()) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
  for (std::uint32_t i = 0; i < info->countActionSets; ++i)
    if (!action_sets.contains(info->actionSets[i])) return XR_ERROR_HANDLE_INVALID;
  attached_sets.insert(info->actionSets, info->actionSets + info->countActionSets);
  profile_changed = true;
  return XR_SUCCESS;
}
// Resolve physical sources, never action names: OVR/Unity choose their own names.
ActionState source_state(XrPath path) {
  const auto& name = path_names.at(path);
  bool left = name.starts_with("/user/hand/left/");
  if (!left && !name.starts_with("/user/hand/right/")) return {};
  const auto& c = left ? script.left : script.right;
  ActionState s;
  s.active = script.active && c.active;
  s.source = path;
  const auto part = std::string_view(name).substr(left ? 15 : 16);
  const auto trigger = c.trigger < 0 ? script.trigger : c.trigger;
  if (part == "/input/trigger/value") s.value = trigger;
  else if (part == "/input/trigger/click" || part == "/input/select/click") s.value = trigger > 0.5f;
  else if (part == "/input/squeeze/value") s.value = c.squeeze;
  else if (part == "/input/squeeze/click") s.value = c.squeeze > 0.5f;
  else if (part == "/input/thumbstick" || part == "/input/trackpad") s.vector = c.thumbstick;
  else if (part == "/input/thumbstick/x" || part == "/input/trackpad/x") s.value = c.thumbstick.x;
  else if (part == "/input/thumbstick/y" || part == "/input/trackpad/y") s.value = c.thumbstick.y;
  else if (part == "/input/a/click" || part == "/input/x/click") s.value = c.primary;
  else if (part == "/input/b/click" || part == "/input/y/click") s.value = c.secondary;
  else if (part == "/input/menu/click") s.value = c.menu;
  else if (part == "/input/thumbstick/click" || part == "/input/trackpad/click") s.value = c.thumbstick_click;
  else if (part == "/input/trigger/touch") s.value = c.trigger_touch || trigger > 0;
  else if (part == "/input/a/touch" || part == "/input/x/touch") s.value = c.primary_touch || c.primary;
  else if (part == "/input/b/touch" || part == "/input/y/touch") s.value = c.secondary_touch || c.secondary;
  else if (part == "/input/thumbstick/touch") s.value = c.thumbstick_touch || c.thumbstick_click;
  else if (part == "/input/thumbrest/touch" || part == "/input/trackpad/touch") s.value = c.thumbrest_touch;
  else if (part != "/input/aim/pose" && part != "/input/grip/pose" && part != "/output/haptic") return {};
  if (!s.active) { s.value = 0; s.vector = {}; }
  return s;
}
XrResult m_xrSyncActions(XrSession s, const XrActionsSyncInfo *info) {
  if (s != session) return XR_ERROR_HANDLE_INVALID;
  if (!running) return XR_ERROR_SESSION_NOT_RUNNING;
  if (session_state != XR_SESSION_STATE_FOCUSED) {
    action_states.clear();
    return XR_SESSION_NOT_FOCUSED;
  }
  for (std::uint32_t i = 0; i < info->countActiveActionSets; ++i)
    if (!attached_sets.contains(info->activeActionSets[i].actionSet)) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    else if (info->activeActionSets[i].subactionPath && !path_names.contains(info->activeActionSets[i].subactionPath))
      return XR_ERROR_PATH_INVALID;
  const auto now = venture::xr::native_now_ns();
  if (!input_origin) { input_origin = now; std::cerr << "[mock-input] timeline started\n"; }
  // Reload on edits so long headless navigation sessions can append future
  // events. Loading is transactional; a partial/invalid edit keeps the previous
  // script and reports the error. The session's time origin never resets.
  if (!input_path.empty() && now >= next_input_reload) {
    next_input_reload = now + 1'000'000'000;
    try {
      const auto modified = std::filesystem::last_write_time(input_path);
      if (modified != input_write_time) {
        input_write_time = modified;
        const auto old_profile = timeline.profile();
        timeline.load(input_path);
        if (old_profile != timeline.profile()) profile_changed = true;
        std::cerr << "[mock-input] reloaded timeline\n";
      }
    } catch (const std::exception& error) {
      std::cerr << "[mock-input] timeline edit rejected: " << error.what() << '\n';
    }
  }
  watch_runner.restore(script);
  const bool left_was_active = script.left.active, right_was_active = script.right.active;
  if (timeline.advance(double(now - input_origin) / 1e9, script)) {
    std::cerr << "[mock-input] t=" << double(now-input_origin)/1e9
              << " right trigger=" << script.right.trigger << " A=" << script.right.primary
              << " stick=" << script.right.thumbstick.x << ',' << script.right.thumbstick.y << '\n';
    if (left_was_active != script.left.active || right_was_active != script.right.active) profile_changed = true;
  }
  route_offset_s.store(-timeline.route_time(0));
  if (!timeline.watches().empty()) {
    const double wall = double(now - input_origin) / 1e9, route = timeline.route_time(wall);
    std::optional<mock::SceneSignature> signature, raw;
    {
      std::lock_guard lock(mutex);
      if (latest_signature && latest_signature_ns > watch_signature_ns) {
        signature = latest_signature;
        raw = latest_signature_raw;
        watch_signature_ns = latest_signature_ns;
      }
    }
    const bool was_pressing = watch_runner.pressing();
    const auto fired = watch_runner.update(timeline.watches(), route, wall, signature ? &*signature : nullptr,
                                            raw ? &*raw : nullptr, script);
    if (fired >= 0) {
      const auto line = std::format("{{\"watch\":{},\"route_time_s\":{:.3f},\"time_s\":{:.3f}}}", fired, route, wall);
      std::cerr << "[mock-input] watch " << line << '\n';
      std::lock_guard lock(mutex);
      watch_log.push_back(line);
    }
    if (was_pressing && !watch_runner.pressing()) std::cerr << "[mock-input] watch press done\n";
    watch_sampling = watch_runner.sampling(timeline.watches(), route);
  }
  if (const auto* gate = timeline.holding()) {
    std::lock_guard lock(mutex);
    auto& g = gate_state;
    if (g.event_time != timeline.hold_time()) {
      g = {};
      g.event_time = timeline.hold_time();
      g.started = now;
      gate_last_projection = gate->last_projection;
      latest_signature.reset();
      for (auto& row : signature_rows) row.reset();
      next_signature_ns = 0;
      gate_sampling = true;
      std::cerr << "[mock-input] holding at route time " << g.event_time << '\n';
    }
    if (latest_signature && latest_signature_ns > g.started && latest_signature_ns - g.last_ns >= gate_sample_period_ns) {
      const auto& signature = *latest_signature;
      ++g.samples;
      if (!g.reference) g.reference = signature;
      if (!gate->match.empty()) {
        // Matching a known screen: "changed" means within reach of it; stable while it stays there.
        float distance = 1;
        for (std::size_t at = 0; at + 96 <= gate->match.size(); at += 96) {
          mock::SceneSignature target{};
          std::copy_n(gate->match.begin() + static_cast<std::ptrdiff_t>(at), 96, target.begin());
          distance = std::min(distance, mock::signature_distance(signature, target));
          if (latest_signature_raw) distance = std::min(distance, mock::signature_distance(*latest_signature_raw, target));
        }
        if (g.closest < 0 || distance < g.closest) g.closest = distance;
        g.changed = distance <= gate->within;
        if (!g.changed) g.still_since = 0;
        else if (!g.still_since) g.still_since = latest_signature_ns;
      } else {
        if (!g.changed && mock::signature_distance(signature, *g.reference) >= gate->change) g.changed = true;
        if (g.last && mock::signature_distance(signature, *g.last) <= gate->still) {
          if (!g.still_since) g.still_since = g.last_ns;
        } else {
          g.still_since = 0;
        }
      }
      g.last = signature;
      g.last_ns = latest_signature_ns;
    }
    const bool settled = g.changed && g.still_since && now - g.still_since >= XrTime(gate->stable * 1e9);
    const bool expired = now - g.started >= XrTime(gate->timeout * 1e9);
    if (settled || expired) {
      const auto line = std::format("{{\"route_time_s\":{},\"held_s\":{},\"samples\":{},\"changed\":{},\"timeout\":{},\"closest\":{:.4f}}}",
          g.event_time, double(now - g.started) / 1e9, g.samples, g.changed, !settled, g.closest);
      std::cerr << "[mock-input] gate " << line << '\n';
      gate_log.push_back(line);
      timeline.open(!settled);
      gate_sampling = false;
      gate_last_projection = false;
      latest_signature.reset();
      latest_signature_raw.reset();
    }
  }
  const auto profile = paths.find(timeline.profile());
  auto enabled = [&](XrAction action, const std::string& source) {
    for (std::uint32_t i = 0; i < info->countActiveActionSets; ++i) {
      const auto& set = info->activeActionSets[i];
      if (set.actionSet == action_parents.at(action) &&
          (!set.subactionPath || source.starts_with(path_names.at(set.subactionPath) + "/"))) return true;
    }
    return false;
  };
  auto physical_source = [](const std::string& source) {
    const auto input = source.find("/input/");
    return input == std::string::npos ? source : source.substr(0, source.find('/', input + 7));
  };
  std::map<std::string, std::uint32_t> priorities;
  if (profile != paths.end()) for (const auto& binding : bindings[profile->second]) {
    const auto& source = path_names.at(binding.binding);
    if (!actions.contains(binding.action) || !enabled(binding.action, source)) continue;
    auto& priority = priorities[physical_source(source)];
    priority = std::max(priority, action_priorities.at(action_parents.at(binding.action)));
  }
  for (const auto& [action, details] : actions) {
    std::vector<XrPath> queries = details.subactions;
    queries.push_back(XR_NULL_PATH);
    for (const auto query : queries) {
      ActionState next;
      if (profile != paths.end()) for (const auto& binding : bindings[profile->second]) {
        if (binding.action != action) continue;
        const auto& source = path_names.at(binding.binding);
        if (query && !source.starts_with(path_names.at(query) + "/")) continue;
        if (!enabled(action, source) || priorities[physical_source(source)] > action_priorities.at(action_parents.at(action))) continue;
        auto value = source_state(binding.binding);
        if (!value.active) continue;
        if (details.type == XR_ACTION_TYPE_BOOLEAN_INPUT) value.value = value.value > 0.5f;
        if (!next.active || std::abs(value.value) > std::abs(next.value) ||
            value.vector.x*value.vector.x + value.vector.y*value.vector.y >
                next.vector.x*next.vector.x + next.vector.y*next.vector.y) next = value;
      }
      auto& previous = action_states[{action, query}];
      // OpenXR input.adoc: activation itself is not a changedSinceLastSync edge.
      next.changed = next.active && previous.active &&
          (next.value != previous.value || next.vector.x != previous.vector.x || next.vector.y != previous.vector.y);
      next.last_change = next.active ? (next.changed || !previous.active ? now : previous.last_change) : 0;
      if (trace_input() && (next.changed || next.active != previous.active))
        std::cerr << "[mock-input] action " << details.name << " hand=" << path_names[query]
                  << " active=" << next.active << " value=" << next.value
                  << " vector=" << next.vector.x << ',' << next.vector.y << '\n';
      previous = next;
    }
  }
  // Estimate observed motion once per input sample, rather than once per
  // locate call: multiple clients locating the same space must see the same
  // velocity. Include action/reference-space offsets in the sampled pose.
  for (auto& [handle, space] : spaces) {
    const auto current = world_pose(handle);
    const bool active = space_active(handle);
    space.velocity = active && space.sampled_active && space.sampled_time
        ? pose_velocity(space.sampled_pose, current, double(now - space.sampled_time) / 1e9)
        : XrSpaceVelocityData{active && !space.action ? XrSpaceVelocityFlags(3) : 0, {}, {}};
    space.sampled_pose = current;
    space.sampled_time = now;
    space.sampled_active = active;
  }
  return XR_SUCCESS;
}
XrResult m_xrGetCurrentInteractionProfile(XrSession s, XrPath hand, XrInteractionProfileState *out) {
  if (s != session) return XR_ERROR_HANDLE_INVALID;
  const auto it = path_names.find(hand);
  if (it == path_names.end()) return XR_ERROR_PATH_INVALID;
  if (it->second != "/user/hand/left" && it->second != "/user/hand/right") return XR_ERROR_PATH_UNSUPPORTED;
  out->interactionProfile = XR_NULL_PATH;
  if (attached_sets.empty() || !script.active || !(it->second.ends_with("left") ? script.left.active : script.right.active)) return XR_SUCCESS;
  return m_xrStringToPath(instance, timeline.profile().c_str(), &out->interactionProfile);
}
XrResult get_action_state(XrSession s, const XrActionStateGetInfo* info, XrActionType type, ActionState& out) {
  if (s != session || !actions.contains(info->action)) return XR_ERROR_HANDLE_INVALID;
  const auto& action = actions.at(info->action);
  if (action.type != type) return XR_ERROR_ACTION_TYPE_MISMATCH;
  if (!attached_sets.contains(action_parents.at(info->action))) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
  if (info->subactionPath && std::ranges::find(action.subactions, info->subactionPath) == action.subactions.end())
    return XR_ERROR_PATH_UNSUPPORTED;
  out = action_states[{info->action, info->subactionPath}];
  if (session_state != XR_SESSION_STATE_FOCUSED) out = {};
  return XR_SUCCESS;
}
XrPosef world_pose(XrSpace space) {
  const auto& s = spaces.at(space);
  XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};
  if (s.action) {
    auto it = action_states.find({s.action, s.hand});
    if (it != action_states.end() && it->second.source) {
      const auto& source = path_names.at(it->second.source);
      const auto& c = source.starts_with("/user/hand/left/") ? script.left : script.right;
      pose = source.ends_with("/aim/pose") ? c.aim : c.grip;
    }
  } else if (s.type == XR_REFERENCE_SPACE_TYPE_VIEW) pose = script.head;
  else if (s.type == XR_REFERENCE_SPACE_TYPE_LOCAL) pose = local_origin;
  else if (s.type == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR) {
    pose = local_origin;
    pose.position.y = 0;
  }
  return venture::xr::compose(pose, s.offset);
}
bool space_active(XrSpace space) {
  const auto& s = spaces.at(space);
  if (!script.active) return false;
  if (!s.action) return true;
  auto it = action_states.find({s.action, s.hand});
  return it != action_states.end() && it->second.active;
}
// VENTURE_MOCK_LOG_ACTIONS=begin:end, in route seconds: log button and axis states as
// the application reads them (each read whose answer differs from the previous read of
// that action and subaction), to tell a press the app never saw from one it ignored.
void log_action_read(const XrActionStateGetInfo& info, const ActionState& state) {
  static const auto window = [] {
    const char* text = venture::environment::getenv("VENTURE_MOCK_LOG_ACTIONS");
    return mock::parse_time_window(text ? text : "");
  }();
  if (!window || !input_origin) return;
  const double route = route_seconds(venture::xr::native_now_ns());
  if (!window->contains(route)) return;
  auto& last = action_reads_logged;
  const auto answer = std::tuple{state.value, state.active, state.changed, int(session_state)};
  const auto [it, added] = last.try_emplace({info.action, info.subactionPath}, answer);
  if (!added && it->second == answer) return;
  it->second = answer;
  const auto name = [](XrPath path) {
    const auto found = path_names.find(path);
    return found == path_names.end() ? std::string("-") : found->second;
  };
  std::cerr << std::format("[mock-input] read route={:.3f} action={} sub={} source={} value={} active={} changed={} "
                           "last_change_s={:.3f} session_state={}\n",
                           route, actions.at(info.action).name, name(info.subactionPath), name(state.source),
                           state.value, int(state.active), int(state.changed),
                           state.last_change ? double(state.last_change - input_origin) / 1e9 : -1.0,
                           int(session_state));
}
XrResult m_xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo *info,
                                   XrActionStateBoolean *out) {
  ActionState state;
  auto result = get_action_state(session, info, XR_ACTION_TYPE_BOOLEAN_INPUT, state);
  if (XR_FAILED(result)) return result;
  log_action_read(*info, state);
  out->currentState = state.value != 0;
  out->isActive = state.active;
  out->changedSinceLastSync = state.changed;
  out->lastChangeTime = state.last_change;
  return XR_SUCCESS;
}
XrResult m_xrGetActionStateFloat(XrSession session, const XrActionStateGetInfo *info, XrActionStateFloat *out) {
  ActionState state;
  auto result = get_action_state(session, info, XR_ACTION_TYPE_FLOAT_INPUT, state);
  if (XR_FAILED(result)) return result;
  log_action_read(*info, state);
  out->currentState = state.value;
  out->isActive = state.active;
  out->changedSinceLastSync = state.changed;
  out->lastChangeTime = state.last_change;
  return XR_SUCCESS;
}
XrResult m_xrGetActionStateVector2f(XrSession session, const XrActionStateGetInfo *info,
                                    XrActionStateVector2f *out) {
  ActionState state;
  auto result = get_action_state(session, info, XR_ACTION_TYPE_VECTOR2F_INPUT, state);
  if (XR_FAILED(result)) return result;
  out->currentState = state.vector;
  out->isActive = state.active;
  out->changedSinceLastSync = state.changed;
  out->lastChangeTime = state.last_change;
  return XR_SUCCESS;
}
XrResult m_xrGetActionStatePose(XrSession session, const XrActionStateGetInfo *info, XrActionStatePose *out) {
  ActionState state;
  auto result = get_action_state(session, info, XR_ACTION_TYPE_POSE_INPUT, state);
  if (XR_FAILED(result)) return result;
  out->isActive = state.active;
  return XR_SUCCESS;
}
XrResult m_xrGetDeviceSampleRateFB(XrSession s, const XrHapticActionInfo *info, XrDevicePcmSampleRateGetInfoFB *out) {
  const auto rate = pcm_rate();
  if (!rate) return XR_ERROR_FUNCTION_UNSUPPORTED;
  if (!info || info->type != XR_TYPE_HAPTIC_ACTION_INFO || info->next || !out ||
      out->type != XR_TYPE_DEVICE_PCM_SAMPLE_RATE_STATE_FB)
    return XR_ERROR_VALIDATION_FAILURE;
  if (s != session || !actions.contains(info->action)) return XR_ERROR_HANDLE_INVALID;
  if (actions.at(info->action).type != XR_ACTION_TYPE_VIBRATION_OUTPUT) return XR_ERROR_ACTION_TYPE_MISMATCH;
  out->sampleRate = *rate;
  return XR_SUCCESS;
}
XrResult m_xrApplyHapticFeedback(XrSession s, const XrHapticActionInfo *info,
                                 const XrHapticBaseHeader *header) {
  if (!info || info->type != XR_TYPE_HAPTIC_ACTION_INFO || info->next)
    return XR_ERROR_VALIDATION_FAILURE;
  if (s != session || !actions.contains(info->action))
    return XR_ERROR_HANDLE_INVALID;
  if (info->subactionPath && !path_names.contains(info->subactionPath)) return XR_ERROR_PATH_INVALID;
  const auto &subactions = actions.at(info->action).subactions;
  if (info->subactionPath && std::find(subactions.begin(), subactions.end(), info->subactionPath) == subactions.end())
    return XR_ERROR_PATH_UNSUPPORTED;
  if (actions.at(info->action).type != XR_ACTION_TYPE_VIBRATION_OUTPUT)
    return XR_ERROR_ACTION_TYPE_MISMATCH;
  if (!attached_sets.contains(action_parents.at(info->action))) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
  if (session_state != XR_SESSION_STATE_FOCUSED) return XR_SESSION_NOT_FOCUSED;
  if (!header || header->type != XR_TYPE_HAPTIC_VIBRATION || header->next)
    return XR_ERROR_VALIDATION_FAILURE;
  auto &v = *reinterpret_cast<const XrHapticVibration *>(header);
  // Core duration may be clamped to the device range. Accept zero as a no-op,
  // without counting it as evidence of a nonzero controller pulse.
  if (!std::isfinite(v.amplitude) || v.amplitude < 0 || v.amplitude > 1 ||
      !std::isfinite(v.frequency) || v.frequency < 0 ||
      (v.duration < 0 && v.duration != XR_MIN_HAPTIC_DURATION))
    return XR_ERROR_VALIDATION_FAILURE;
  if (v.amplitude > 0 && v.duration != 0 && script.active) {
    auto profile = paths.find(timeline.profile());
    if (profile != paths.end()) {
      bool counted[2]{};
      for (const auto &binding : bindings[profile->second]) {
        if (binding.action != info->action) continue;
        const auto &name = path_names.at(binding.binding);
        if (!name.ends_with("/output/haptic")) continue;
        if (!name.starts_with("/user/hand/left/") && !name.starts_with("/user/hand/right/")) continue;
        int hand = name.starts_with("/user/hand/left/") ? 0 : 1;
        auto root = hand ? "/user/hand/right" : "/user/hand/left";
        if (info->subactionPath && path_names.at(info->subactionPath) != root) continue;
        if (!(hand ? script.right.active : script.left.active) || counted[hand]) continue;
        counted[hand] = true;
        ++stats.haptic_pulses;
        ++haptic_per_hand[hand];
      }
    }
  }
  haptic_changed.notify_all();
  return XR_SUCCESS;
}
XrResult m_xrStopHapticFeedback(XrSession s, const XrHapticActionInfo *info) {
  if (!info || info->type != XR_TYPE_HAPTIC_ACTION_INFO || info->next)
    return XR_ERROR_VALIDATION_FAILURE;
  if (s != session || !actions.contains(info->action))
    return XR_ERROR_HANDLE_INVALID;
  if (info->subactionPath && !path_names.contains(info->subactionPath)) return XR_ERROR_PATH_INVALID;
  const auto &subactions = actions.at(info->action).subactions;
  if (info->subactionPath && std::find(subactions.begin(), subactions.end(), info->subactionPath) == subactions.end())
    return XR_ERROR_PATH_UNSUPPORTED;
  if (actions.at(info->action).type != XR_ACTION_TYPE_VIBRATION_OUTPUT)
    return XR_ERROR_ACTION_TYPE_MISMATCH;
  if (!attached_sets.contains(action_parents.at(info->action))) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
  if (session_state != XR_SESSION_STATE_FOCUSED) return XR_SESSION_NOT_FOCUSED;
  ++stats.haptic_stops;
  return XR_SUCCESS;
}
XrResult m_xrCreateHandTrackerEXT(XrSession, const XrHandTrackerCreateInfoEXT *info,
                                  XrHandTrackerEXT *out) {
  *out = token<XrHandTrackerEXT>();
  hands[*out] = info->hand == XR_HAND_LEFT_EXT;
  return XR_SUCCESS;
}
XrResult m_xrDestroyHandTrackerEXT(XrHandTrackerEXT hand) {
  return hands.erase(hand) ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}
XrResult m_xrLocateHandJointsEXT(XrHandTrackerEXT hand, const XrHandJointsLocateInfoEXT *info,
                                 XrHandJointLocationsEXT *out) {
  if (!hands.contains(hand))
    return XR_ERROR_HANDLE_INVALID;
  if (!info || !out)
    return XR_ERROR_VALIDATION_FAILURE;
  if (!spaces.contains(info->baseSpace))
    return XR_ERROR_HANDLE_INVALID;
  if (out->jointCount != 26 || !out->jointLocations)
    return XR_ERROR_VALIDATION_FAILURE;
  const auto &model = venture::xr::hand_model(hands[hand]);
  const auto &overrides = hands[hand] ? script.left_hand_joints : script.right_hand_joints;
  const auto &jointPoses = overrides ? *overrides : model.joints;
  // XR_EXT_hand_tracking locates joints in the requested baseSpace, not in
  // wrist-local bind space. Timeline wrists, like controller poses, are STAGE.
  const auto wrist = venture::xr::compose(venture::xr::inverse(world_pose(info->baseSpace)),
                                     hands[hand] ? script.left_hand : script.right_hand);
  const bool active = script.active && script.hand_tracking_active;
  out->isActive = active;
  // An inactive hand's joints are undefined (XR_EXT_hand_tracking): a strict runtime's NaN.
  constexpr float nan = std::numeric_limits<float>::quiet_NaN();
  for (int j = 0; j < 26; ++j) {
    out->jointLocations[j] = active ? XrHandJointLocationEXT{XrSpaceLocationFlags(15),
                                                             venture::xr::compose(wrist, jointPoses[j]),
                                                             model.radii[j]}
                                    : XrHandJointLocationEXT{0, {{nan, nan, nan, nan}, {nan, nan, nan}}, nan};
  }
  if (out->next) {
    auto v = static_cast<XrHandJointVelocitiesEXT *>(out->next);
    if (v->type != XR_TYPE_HAND_JOINT_VELOCITIES_EXT || v->jointCount != 26 || !v->jointVelocities)
      return XR_ERROR_VALIDATION_FAILURE;
    for (int j = 0; j < 26; ++j)
      v->jointVelocities[j] = active ? XrHandJointVelocityEXT{XrSpaceVelocityFlags(3), {}, {}}
                                     : XrHandJointVelocityEXT{0, {nan, nan, nan}, {nan, nan, nan}};
  }
  return XR_SUCCESS;
}
XrResult m_xrResultToString(XrInstance, XrResult result, char out[XR_MAX_RESULT_STRING_SIZE]) {
  std::snprintf(out, XR_MAX_RESULT_STRING_SIZE, "XrResult(%d)", int(result));
  return XR_SUCCESS;
}
XrResult m_xrStructureTypeToString(XrInstance, XrStructureType type,
                                   char out[XR_MAX_STRUCTURE_NAME_SIZE]) {
  std::snprintf(out, XR_MAX_STRUCTURE_NAME_SIZE, "XrStructureType(%d)", int(type));
  return XR_SUCCESS;
}
struct Counter {
  std::int64_t value;
};
XrResult counter_to_time(XrInstance, const Counter *counter, XrTime *out) {
  *out = venture::xr::native_counter_to_ns(counter->value);
  return XR_SUCCESS;
}
struct Timespec {
  std::int64_t sec, nsec;
};
XrResult timespec_to_time(XrInstance, const Timespec *ts, XrTime *out) {
  *out = ts->sec * 1'000'000'000 + ts->nsec;
  return XR_SUCCESS;
}
XrResult m_xrGetInstanceProcAddr(XrInstance, const char *, PFN_xrVoidFunction *);
XrResult XRAPI_CALL d3d11_requirements(XrInstance instance, XrSystemId system, void *out) {
  return d3d11::requirements(instance, system, out);
}
#include "dispatch.inc"
XrResult m_xrGetInstanceProcAddr(XrInstance, const char *name, PFN_xrVoidFunction *out) {
  if (!out || !name)
    return XR_ERROR_VALIDATION_FAILURE;
  *out = nullptr;
  if (std::strcmp(name, "xrVentureFinishDiagnostics") == 0)
    *out = reinterpret_cast<PFN_xrVoidFunction>(&finish_diagnostics);
  if (std::strcmp(name, "xrGetD3D11GraphicsRequirementsKHR") == 0 && d3d11::available())
    *out = reinterpret_cast<PFN_xrVoidFunction>(&d3d11_requirements);
  if (std::strcmp(name, "xrConvertWin32PerformanceCounterToTimeKHR") == 0)
    *out = reinterpret_cast<PFN_xrVoidFunction>(&counter_to_time);
  if (std::strcmp(name, "xrConvertTimespecTimeToTimeKHR") == 0)
    *out = reinterpret_cast<PFN_xrVoidFunction>(&timespec_to_time);
  for (const auto &entry : dispatch)
    if (std::strcmp(name, entry.name) == 0)
      *out = entry.function;
  return *out ? XR_SUCCESS : XR_ERROR_FUNCTION_UNSUPPORTED;
}
} // namespace mock
extern "C" VENTURE_MOCK_EXPORT XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(
    const XrNegotiateLoaderInfo *loader, XrNegotiateRuntimeRequest *request) {
  if (!loader || !request || loader->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
      loader->minInterfaceVersion > 1 || loader->maxInterfaceVersion < 1 ||
      request->structSize != sizeof(XrNegotiateRuntimeRequest))
    return XR_ERROR_INITIALIZATION_FAILED;
  request->runtimeInterfaceVersion = 1;
  request->runtimeApiVersion = XR_MAKE_VERSION(1, 0, 0);
  request->getInstanceProcAddr = &mock::m_xrGetInstanceProcAddr;
  return XR_SUCCESS;
}
extern "C" VENTURE_MOCK_EXPORT void ventureMockXrRefreshEnvironment() { venture::environment::refresh(); }
extern "C" VENTURE_MOCK_EXPORT void ventureMockXrSetScript(const MockXrScript *s) {
  std::lock_guard lock(mock::mutex);
  if (s)
    mock::script = *s;
}
extern "C" VENTURE_MOCK_EXPORT void ventureMockXrRestartSession() {
  std::lock_guard lock(mock::mutex);
  mock::restart_pending = true;
  mock::events.push_back(XR_SESSION_STATE_STOPPING);
}
extern "C" VENTURE_MOCK_EXPORT void ventureMockXrLoseSession() {
  std::lock_guard lock(mock::mutex);
  mock::session_lost = true;
  mock::events.push_back(XR_SESSION_STATE_LOSS_PENDING);
  mock::frame_changed.notify_all();
}
extern "C" VENTURE_MOCK_EXPORT MockXrStats ventureMockXrStats() {
  std::lock_guard lock(mock::mutex);
  return mock::stats;
}
extern "C" VENTURE_MOCK_EXPORT XrTime ventureMockXrLastDisplayTime() {
  std::lock_guard lock(mock::mutex);
  return mock::last_display_time;
}
extern "C" VENTURE_MOCK_EXPORT void ventureMockXrGateWait(bool hold) {
  std::lock_guard lock(mock::mutex);
  mock::gate_wait = hold;
  mock::frame_changed.notify_all();
}
extern "C" VENTURE_MOCK_EXPORT bool ventureMockXrAwaitGatedWait(std::uint32_t milliseconds) {
  std::unique_lock lock(mock::mutex);
  return mock::frame_changed.wait_for(lock, std::chrono::milliseconds(milliseconds),
                                      [] { return mock::gated_wait; });
}
extern "C" VENTURE_MOCK_EXPORT std::uint32_t ventureMockXrLastSubImages(XrSwapchainSubImage *out,
                                                             std::uint32_t capacity) {
  std::lock_guard lock(mock::mutex);
  const auto count = static_cast<std::uint32_t>(mock::last_subimages.size());
  for (std::uint32_t i = 0; out && i < count && i < capacity; ++i)
    out[i] = mock::last_subimages[i];
  return count;
}
extern "C" VENTURE_MOCK_EXPORT bool ventureMockXrAwaitHaptics(std::uint32_t count,
                                                    std::uint32_t milliseconds) {
  std::unique_lock lock(mock::mutex);
  return mock::haptic_changed.wait_for(lock, std::chrono::milliseconds(milliseconds),
                                       [&] { return mock::stats.haptic_pulses >= count; });
}
extern "C" VENTURE_MOCK_EXPORT void ventureMockXrGateEnd(bool hold) {
  std::lock_guard lock(mock::mutex);
  mock::gate_end = hold;
  mock::frame_changed.notify_all();
}
extern "C" VENTURE_MOCK_EXPORT bool ventureMockXrAwaitGatedEnd(std::uint32_t milliseconds) {
  std::unique_lock lock(mock::mutex);
  return mock::frame_changed.wait_for(lock, std::chrono::milliseconds(milliseconds),
                                      [] { return mock::gated_end; });
}
extern "C" VENTURE_MOCK_EXPORT std::uint32_t ventureMockXrDestroysDuringEnd() {
  std::lock_guard lock(mock::mutex);
  return mock::destroys_during_end;
}
