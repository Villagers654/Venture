#include "xr/desktop_mirror.h"
#include "common/log.h"
#include "gfx/gpu_device_internal.h"
#include "gfx/queue_sync.h"
#include "hle/vulkan/internal.h"
#include "xr/display_override.h"
#include "xr/xr_images.h"

#include <SDL3/SDL.h>
#ifdef _WIN32
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#else
struct wl_display;
struct wl_surface;
#include <vulkan/vulkan_wayland.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace venture::xr {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// The shared queue's external synchronization (xr/queue.h), with the queue
// fixed when the mirror started rather than read from the session.
class QueueGuard {
  std::shared_ptr<std::recursive_mutex> mutex_;
  std::unique_lock<std::recursive_mutex> lock_;
  XrImages* provider_ = nullptr;
  bool locked_ = true;

 public:
  QueueGuard(VkQueue queue, bool shared) : mutex_(gfx::queue_mutex(queue)), lock_(*mutex_) {
    if (auto* images = xr_images(); shared && images && images->external_queue_active()) {
      provider_ = images;
      locked_ = provider_->lock_external_queue();
    }
  }
  ~QueueGuard() {
    if (provider_ && locked_) provider_->unlock_external_queue();
  }
  explicit operator bool() const { return locked_; }
};

struct Frame {
  VkCommandBuffer command{};
  VkFence fence{};
  bool pending = false;  // `fence` belongs to a submission not yet waited
};

class Mirror {
 public:
  // An image waits, or the mirror has yet to start.
  bool wants() const {
    return dormant_.load(std::memory_order_acquire) || slot_seen_.load(std::memory_order_acquire) == Slot::ready;
  }
  void capture(const XrGraphicsBindingVulkanKHR& binding, bool shared, const MirrorSource& source);
  void release_device(VkDevice device);

 private:
  // idle: the mirror thread owns everything. ready: an acquired window image
  // waits for the render thread, which may then take it (recording) and hand
  // it back submitted. The mirror thread reclaims only a ready image.
  enum class Slot { idle, ready, recording, submitted };
  template <class T> T fn(const char* name) const { return reinterpret_cast<T>(get_(binding_.instance, name)); }
  bool capable(const XrGraphicsBindingVulkanKHR& binding) const;
  bool record(Frame& frame, VkImage target, VkExtent2D extent, VkSemaphore rendered, const MirrorSource& source);
  bool blittable(VkFormat format, bool destination, bool& linear);
  void run();
  bool open_window();
  bool create_swapchain();
  void wait_frames();
  bool wait_acquire();
  struct Cost;
  void log_costs(const Cost& capture);
  void close_window();
  void set_slot(Slot slot) {
    slot_ = slot;
    slot_seen_.store(slot, std::memory_order_release);
  }

  std::mutex mutex_;
  std::condition_variable changed_;
  Slot slot_ = Slot::idle;
  std::atomic<Slot> slot_seen_{Slot::idle};
  std::atomic<bool> dormant_{true}, failed_{false};
  bool started_ = false, stop_ = false, user_closed_ = false, finished_ = false;
  std::thread thread_;
  // Fixed while the mirror runs.
  XrGraphicsBindingVulkanKHR binding_{};
  bool shared_ = false;
  VkQueue queue_{};
  // The private queue the mirror presents on.
  VkQueue present_queue_{};
  std::uint32_t present_family_ = 0;
  PFN_vkGetInstanceProcAddr get_{};
  MirrorMode mode_ = MirrorMode::off;
  // Mirror thread; while a slot is ready or recording the render thread reads
  // images_, extent_, rendered_, image_index_ and frames_[frame_index_] and
  // writes that frame.
  SDL_Window* window_{};
  bool video_ = false;
  VkSurfaceKHR surface_{};
  VkSwapchainKHR swapchain_{};
  VkFormat format_ = VK_FORMAT_UNDEFINED;
  VkExtent2D extent_{};
  std::vector<VkImage> images_;
  std::vector<VkSemaphore> rendered_;
  VkCommandPool pool_{};
  std::array<Frame, 2> frames_{};
  unsigned frame_index_ = 0;
  std::uint32_t image_index_ = 0;
  // Acquires signal a fence the mirror thread waits for, so the blit on the
  // XR queue never waits for the presentation engine.
  VkFence acquire_fence_{};
  bool acquiring_ = false;  // acquire_fence_ not yet signalled
  bool holding_ = false;    // image_index_ acquired, not yet presented
  // Render thread: blit support of formats seen, by format.
  std::vector<std::pair<VkFormat, int>> formats_;
  // Time spent in the XR loop (render thread) and in presents, for the log.
  struct Cost {
    std::uint64_t count = 0;
    Clock::duration total{}, max{};
    void add(Clock::duration d) {
      ++count;
      total += d;
      max = std::max(max, d);
    }
  } capture_cost_, present_cost_;
};

Mirror& mirror() {
  // Process lifetime: the runtime ends with _Exit/TerminateProcess, and a
  // device's release hook stops the thread before Vulkan teardown.
  static Mirror* instance = [] {
    vulkan::add_device_release_hook([](VkDevice device) {
      mirror().release_device(device);
      forget_mirror_queue(device);
    });
    return new Mirror;
  }();
  return *instance;
}

bool contains(const std::vector<std::string>& names, std::string_view name) {
  return std::ranges::find(names, name) != names.end();
}

bool Mirror::capable(const XrGraphicsBindingVulkanKHR& binding) const {
  if (!mirror_queue(binding.device)) return false;
  if (binding.device == gfx::gpu_device_state().device) {
    const auto& instance = gfx::enabled_gpu_instance_extensions();
    return std::ranges::all_of(mirror_instance_extensions(), [&](const char* n) { return contains(instance, n); }) &&
           contains(gfx::enabled_gpu_device_extensions(), mirror_device_extension());
  }
  return mirror_capable(binding.instance) && mirror_capable(binding.device);
}

void Mirror::capture(const XrGraphicsBindingVulkanKHR& binding, bool shared, const MirrorSource& source) {
  if (!wants()) return;
  Frame* frame;
  VkImage target;
  VkExtent2D extent;
  VkSemaphore rendered;
  {
    std::lock_guard lock(mutex_);
    if (user_closed_ || failed_) return;
    if (!started_) {
      dormant_ = false;
      if (!capable(binding)) {
        failed_ = true;
        log::warn("mirror", "Desktop mirror unavailable: the graphics device lacks window presentation");
        return;
      }
      started_ = true;
      stop_ = false;
      finished_ = false;
      binding_ = binding;
      shared_ = shared;
      mode_ = mirror_mode();
      get_ = gfx::gpu_device().get_instance_proc_addr;
      fn<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(binding.device, binding.queueFamilyIndex, binding.queueIndex, &queue_);
      const auto present = *mirror_queue(binding.device);
      present_family_ = present.first;
      fn<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(binding.device, present.first, present.second, &present_queue_);
      thread_ = std::thread([this] { run(); });
      return;
    }
    if (slot_ != Slot::ready || binding.device != binding_.device) return;
    set_slot(Slot::recording);
    frame = &frames_[frame_index_];
    target = images_[image_index_];
    extent = extent_;
    rendered = rendered_[image_index_];
  }
  const auto start = Clock::now();
  const bool submitted = record(*frame, target, extent, rendered, source);
  const auto spent = Clock::now() - start;
  {
    std::lock_guard lock(mutex_);
    capture_cost_.add(spent);
    set_slot(submitted ? Slot::submitted : Slot::ready);
  }
  changed_.notify_all();
}

bool Mirror::blittable(VkFormat format, bool destination, bool& linear) {
  auto it = std::ranges::find(formats_, format, &std::pair<VkFormat, int>::first);
  if (it == formats_.end()) {
    VkFormatProperties properties{};
    fn<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(binding_.physicalDevice, format,
                                                                                        &properties);
    const auto features = properties.optimalTilingFeatures;
    formats_.push_back({format, int((features & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ? 1 : 0) |
                                    int((features & VK_FORMAT_FEATURE_BLIT_DST_BIT) ? 2 : 0) |
                                    int((features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? 4 : 0)});
    it = formats_.end() - 1;
  }
  linear = it->second & 4;
  return it->second & (destination ? 2 : 1);
}

// Integer boxes for the blit: the source region and where it lands.
struct Box {
  std::int32_t x0, y0, x1, y1;
};
void regions(MirrorMode mode, const MirrorSource& s, VkExtent2D window, Box& from, Box& to) {
  const double rw = std::max(1, s.rect.extent.width), rh = std::max(1, s.rect.extent.height);
  const double ww = window.width, wh = window.height;
  double x0 = 0, y0 = 0, w = rw, h = rh;
  to = {0, 0, std::int32_t(window.width), std::int32_t(window.height)};
  if (mode == MirrorMode::full) {
    // The whole eye, letterboxed.
    const double scale = std::min(ww / rw, wh / rh);
    const auto dw = std::int32_t(std::lround(rw * scale)), dh = std::int32_t(std::lround(rh * scale));
    to.x0 = (std::int32_t(window.width) - dw) / 2;
    to.y0 = (std::int32_t(window.height) - dh) / 2;
    to.x1 = to.x0 + dw;
    to.y1 = to.y0 + dh;
  } else {
    // Unreal's spectator default: the eye's flat middle (80% of its width,
    // 60% of its height, leaving out the lens-masked rim) cropped to fill the
    // window, here centred on the optical axis.
    w = rw * 0.8;
    h = rh * 0.6;
    if (w / h > ww / wh) w = h * ww / wh;
    else h = w * wh / ww;
    x0 = std::clamp(s.axis_x * rw - w / 2, 0.0, rw - w);
    y0 = std::clamp(s.axis_y * rh - h / 2, 0.0, rh - h);
  }
  from = {s.rect.offset.x + std::int32_t(std::lround(x0)), s.rect.offset.y + std::int32_t(std::lround(y0)),
          s.rect.offset.x + std::int32_t(std::lround(x0 + w)), s.rect.offset.y + std::int32_t(std::lround(y0 + h))};
  from.x1 = std::max(from.x1, from.x0 + 1);
  from.y1 = std::max(from.y1, from.y0 + 1);
}

bool Mirror::record(Frame& frame, VkImage target, VkExtent2D extent, VkSemaphore rendered, const MirrorSource& source) {
  bool source_linear = false, unused = false;
  if (!blittable(source.format, false, source_linear) || !blittable(format_, true, unused)) {
    static std::atomic_bool reported{false};
    if (!reported.exchange(true))
      log::warn("mirror", "Desktop mirror cannot blit format {} to {}", int(source.format), int(format_));
    return false;
  }
  if (fn<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(frame.command, 0) != VK_SUCCESS) return false;
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (fn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(frame.command, &begin) != VK_SUCCESS) return false;
  const auto barrier = fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
  auto transition = [&](VkImage image, std::uint32_t layer, VkImageLayout before, VkImageLayout after,
                        VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
                        VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = before;
    b.newLayout = after;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
    barrier(frame.command, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
  };
  // The eye image was just written by the presentation copy or conversion.
  transition(source.image, source.layer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
             VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
             VK_PIPELINE_STAGE_TRANSFER_BIT);
  transition(target, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
             VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  Box from{}, to{};
  regions(mode_, source, extent, from, to);
  if (to.x0 > 0 || to.y0 > 0 || to.x1 < std::int32_t(extent.width) || to.y1 < std::int32_t(extent.height)) {
    const VkClearColorValue black{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    fn<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(frame.command, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                         &black, 1, &range);
    transition(target, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT);
  }
  if (to.x1 > to.x0 && to.y1 > to.y0) {
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, source.layer, 1};
    blit.srcOffsets[0] = {from.x0, from.y0, 0};
    blit.srcOffsets[1] = {from.x1, from.y1, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {to.x0, to.y0, 0};
    blit.dstOffsets[1] = {to.x1, to.y1, 1};
    fn<PFN_vkCmdBlitImage>("vkCmdBlitImage")(frame.command, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                                             source_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
  }
  // Back to the layout OpenXR requires at release.
  transition(source.image, source.layer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
             VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  transition(target, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
             VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  if (fn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(frame.command) != VK_SUCCESS) return false;
  if (fn<PFN_vkResetFences>("vkResetFences")(binding_.device, 1, &frame.fence) != VK_SUCCESS) return false;
  // The acquire already completed (wait_acquire); the present waits for `rendered`.
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &frame.command;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &rendered;
  QueueGuard queue(queue_, shared_);
  if (!queue) return false;
  if (const auto r = fn<PFN_vkQueueSubmit>("vkQueueSubmit")(queue_, 1, &submit, frame.fence); r != VK_SUCCESS) {
    log::warn("mirror", "Desktop mirror submission failed: {}", int(r));
    return false;
  }
  frame.pending = true;
  return true;
}

bool Mirror::open_window() {
  // The runtime keeps its own signal handling, system timer resolution and
  // focus: SDL would install SIGINT/SIGTERM handlers, raise the Windows timer
  // to 1 ms and activate the new window over the launcher or the game.
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "0");
  SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
  SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    log::warn("mirror", "Desktop mirror unavailable: {}", SDL_GetError());
    return false;
  }
  video_ = true;
  auto title = application_name();
  if (title.empty()) title = "Venture";
  window_ = SDL_CreateWindow(title.c_str(), 1280, 720,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
  if (!window_) {
    log::warn("mirror", "Desktop mirror window failed: {}", SDL_GetError());
    return false;
  }
#ifndef _WIN32
  // SDL has no libdecor here (cmake/Dependencies.cmake), so compositors
  // without server-side decorations (GNOME) leave the window unframed. The
  // mirror takes no input: dragging it moves the window, its borders resize
  // it, and the compositor's close shortcut closes it.
  SDL_SetWindowHitTest(
      window_,
      [](SDL_Window* window, const SDL_Point* point, void*) {
        int w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        constexpr int edge = 8;
        const bool left = point->x < edge, right = point->x >= w - edge;
        const bool top = point->y < edge, bottom = point->y >= h - edge;
        if (top && left) return SDL_HITTEST_RESIZE_TOPLEFT;
        if (top && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
        if (bottom && left) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (top) return SDL_HITTEST_RESIZE_TOP;
        if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;
        if (left) return SDL_HITTEST_RESIZE_LEFT;
        if (right) return SDL_HITTEST_RESIZE_RIGHT;
        return SDL_HITTEST_DRAGGABLE;
      },
      nullptr);
#endif
  const auto properties = SDL_GetWindowProperties(window_);
  VkResult r = VK_ERROR_INITIALIZATION_FAILED;
#ifdef _WIN32
  VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
  info.hinstance = static_cast<HINSTANCE>(SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, nullptr));
  info.hwnd = static_cast<HWND>(SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
  if (info.hwnd)
    r = fn<PFN_vkCreateWin32SurfaceKHR>("vkCreateWin32SurfaceKHR")(binding_.instance, &info, nullptr, &surface_);
#else
  VkWaylandSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};
  info.display = static_cast<wl_display*>(SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
  info.surface = static_cast<wl_surface*>(SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
  if (info.display && info.surface)
    r = fn<PFN_vkCreateWaylandSurfaceKHR>("vkCreateWaylandSurfaceKHR")(binding_.instance, &info, nullptr, &surface_);
#endif
  if (r != VK_SUCCESS) {
    log::warn("mirror", "Desktop mirror surface failed: {}", int(r));
    return false;
  }
  VkBool32 supported = VK_FALSE;
  fn<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>("vkGetPhysicalDeviceSurfaceSupportKHR")(
      binding_.physicalDevice, present_family_, surface_, &supported);
  if (!supported) {
    log::warn("mirror", "Desktop mirror unavailable: queue family {} cannot present to the desktop", present_family_);
    return false;
  }
  VkFenceCreateInfo acquire{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (fn<PFN_vkCreateFence>("vkCreateFence")(binding_.device, &acquire, nullptr, &acquire_fence_) != VK_SUCCESS)
    return false;
  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool.queueFamilyIndex = binding_.queueFamilyIndex;
  if (fn<PFN_vkCreateCommandPool>("vkCreateCommandPool")(binding_.device, &pool, nullptr, &pool_) != VK_SUCCESS)
    return false;
  for (auto& frame : frames_) {
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = pool_;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (fn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(binding_.device, &allocate, &frame.command) !=
            VK_SUCCESS ||
        fn<PFN_vkCreateFence>("vkCreateFence")(binding_.device, &fence, nullptr, &frame.fence) != VK_SUCCESS)
      return false;
  }
  return true;
}

// Only when no frame or acquire is pending: the old swapchain retires into the new one.
bool Mirror::create_swapchain() {
  VkSurfaceCapabilitiesKHR caps{};
  if (fn<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>("vkGetPhysicalDeviceSurfaceCapabilitiesKHR")(
          binding_.physicalDevice, surface_, &caps) != VK_SUCCESS)
    return false;
  VkExtent2D extent = caps.currentExtent;
  if (extent.width == 0xffffffffu) {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    extent = {std::clamp(std::uint32_t(std::max(w, 0)), caps.minImageExtent.width, caps.maxImageExtent.width),
              std::clamp(std::uint32_t(std::max(h, 0)), caps.minImageExtent.height, caps.maxImageExtent.height)};
  }
  if (!extent.width || !extent.height) return false;  // minimized
  if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
    log::warn("mirror", "Desktop mirror unavailable: the desktop surface takes no transfers");
    failed_ = true;
    return false;
  }
  std::uint32_t count = 0;
  const auto formats_of = fn<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>("vkGetPhysicalDeviceSurfaceFormatsKHR");
  formats_of(binding_.physicalDevice, surface_, &count, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(count);
  formats_of(binding_.physicalDevice, surface_, &count, formats.data());
  formats.resize(count);
  if (formats.empty()) return false;
  // An sRGB swapchain encodes like the headset compositor: OpenXR images in
  // sRGB formats are decoded and linear ones are taken as linear.
  auto chosen = formats.front();
  for (auto wanted : {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_A8B8G8R8_SRGB_PACK32})
    if (auto it = std::ranges::find_if(formats, [&](const auto& f) {
          return f.format == wanted && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        it != formats.end()) {
      chosen = *it;
      break;
    }
  const auto modes_of = fn<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>("vkGetPhysicalDeviceSurfacePresentModesKHR");
  modes_of(binding_.physicalDevice, surface_, &count, nullptr);
  std::vector<VkPresentModeKHR> modes(count);
  modes_of(binding_.physicalDevice, surface_, &count, modes.data());
  modes.resize(count);
  // Presenting holds the shared queue. FIFO can block there for a display
  // refresh, or indefinitely for a hidden Wayland window, stalling the game.
  VkPresentModeKHR mode;
  if (std::ranges::find(modes, VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()) mode = VK_PRESENT_MODE_MAILBOX_KHR;
  else if (std::ranges::find(modes, VK_PRESENT_MODE_IMMEDIATE_KHR) != modes.end()) mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
  else {
    log::warn("mirror", "Desktop mirror unavailable: the desktop offers only synchronized presentation");
    failed_ = true;
    return false;
  }
  VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  info.surface = surface_;
  info.minImageCount = std::max(caps.minImageCount + 1, 3u);
  if (caps.maxImageCount) info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
  info.imageFormat = chosen.format;
  info.imageColorSpace = chosen.colorSpace;
  info.imageExtent = extent;
  info.imageArrayLayers = 1;
  info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  // Blitted on the XR queue, presented on the mirror's own.
  const std::uint32_t families[] = {binding_.queueFamilyIndex, present_family_};
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (present_family_ != binding_.queueFamilyIndex) {
    info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    info.queueFamilyIndexCount = 2;
    info.pQueueFamilyIndices = families;
  }
  info.preTransform = caps.currentTransform;
  info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  for (auto alpha : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                     VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
    if (caps.supportedCompositeAlpha & alpha) {
      info.compositeAlpha = alpha;
      break;
    }
  info.presentMode = mode;
  info.clipped = VK_TRUE;
  info.oldSwapchain = swapchain_;
  VkSwapchainKHR created{};
  if (const auto r = fn<PFN_vkCreateSwapchainKHR>("vkCreateSwapchainKHR")(binding_.device, &info, nullptr, &created);
      r != VK_SUCCESS) {
    log::warn("mirror", "Desktop mirror swapchain failed: {}", int(r));
    return false;
  }
  if (swapchain_) fn<PFN_vkDestroySwapchainKHR>("vkDestroySwapchainKHR")(binding_.device, swapchain_, nullptr);
  swapchain_ = created;
  holding_ = false;
  const auto images_of = fn<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR");
  images_of(binding_.device, swapchain_, &count, nullptr);
  images_.resize(count);
  images_of(binding_.device, swapchain_, &count, images_.data());
  images_.resize(count);
  // A present may still wait on a semaphore of the old chain; never destroy
  // them early, reuse them in queue order.
  while (rendered_.size() < images_.size()) {
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s{};
    if (fn<PFN_vkCreateSemaphore>("vkCreateSemaphore")(binding_.device, &semaphore, nullptr, &s) != VK_SUCCESS)
      return false;
    rendered_.push_back(s);
  }
  if (format_ != chosen.format || extent_.width != extent.width || extent_.height != extent.height)
    log::info("mirror", "Desktop mirror {}x{} format={} present_mode={} images={} present_queue_family={}", extent.width,
              extent.height, int(chosen.format), int(mode), images_.size(), present_family_);
  format_ = chosen.format;
  extent_ = extent;
  return true;
}

void Mirror::wait_frames() {
  for (auto& frame : frames_)
    if (frame.pending) {
      fn<PFN_vkWaitForFences>("vkWaitForFences")(binding_.device, 1, &frame.fence, VK_TRUE, UINT64_MAX);
      frame.pending = false;
    }
}

// Waits briefly for the pending acquire; false while it is still pending.
bool Mirror::wait_acquire() {
  if (!acquiring_) return true;
  if (fn<PFN_vkWaitForFences>("vkWaitForFences")(binding_.device, 1, &acquire_fence_, VK_TRUE, 16'000'000) !=
      VK_SUCCESS)
    return false;
  fn<PFN_vkResetFences>("vkResetFences")(binding_.device, 1, &acquire_fence_);
  acquiring_ = false;
  holding_ = true;
  return true;
}

void Mirror::close_window() {
  if (binding_.device) wait_frames();
  // An acquire the presentation engine never completes keeps its fence and
  // swapchain alive; the process outlives neither for long.
  if (acquiring_ && fn<PFN_vkWaitForFences>("vkWaitForFences")(binding_.device, 1, &acquire_fence_, VK_TRUE,
                                                               100'000'000) != VK_SUCCESS) {
    log::warn("mirror", "Desktop mirror acquire never completed");
    acquire_fence_ = {};
    swapchain_ = {};
  }
  acquiring_ = holding_ = false;
  if (acquire_fence_) fn<PFN_vkDestroyFence>("vkDestroyFence")(binding_.device, acquire_fence_, nullptr);
  acquire_fence_ = {};
  if (swapchain_) fn<PFN_vkDestroySwapchainKHR>("vkDestroySwapchainKHR")(binding_.device, swapchain_, nullptr);
  for (auto s : rendered_) fn<PFN_vkDestroySemaphore>("vkDestroySemaphore")(binding_.device, s, nullptr);
  for (auto& frame : frames_) {
    if (frame.fence) fn<PFN_vkDestroyFence>("vkDestroyFence")(binding_.device, frame.fence, nullptr);
    frame = {};
  }
  if (pool_) fn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(binding_.device, pool_, nullptr);
  if (surface_) fn<PFN_vkDestroySurfaceKHR>("vkDestroySurfaceKHR")(binding_.instance, surface_, nullptr);
  swapchain_ = {};
  surface_ = {};
  pool_ = {};
  rendered_.clear();
  images_.clear();
  extent_ = {};
  format_ = VK_FORMAT_UNDEFINED;
  if (window_) SDL_DestroyWindow(window_);
  window_ = nullptr;
  if (video_) SDL_QuitSubSystem(SDL_INIT_VIDEO);
  video_ = false;
}

// Mirror thread; `capture` is a copy of capture_cost_ taken under the mutex.
void Mirror::log_costs(const Cost& capture) {
  auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
  log::info("mirror", "Desktop mirror showed {} frames; render thread avg {:.3f} max {:.3f} ms, present avg {:.3f} max {:.3f} ms",
            present_cost_.count, ms(capture.total) / double(std::max<std::uint64_t>(capture.count, 1)), ms(capture.max),
            ms(present_cost_.total) / double(present_cost_.count), ms(present_cost_.max));
}

void Mirror::run() {
  bool hidden = false, resize = true, close = false;
  auto period = 1s / 60.0;
  Clock::time_point last_present{};
  auto refresh = [&] {
    if (const auto* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_)); mode && mode->refresh_rate > 1)
      period = 1s / double(mode->refresh_rate);
  };
  auto pump = [&] {
    SDL_Event event;
    while (SDL_PollEvent(&event)) switch (event.type) {
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: close = true; break;
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_HIDDEN:
        case SDL_EVENT_WINDOW_OCCLUDED: hidden = true; break;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_SHOWN:
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_MAXIMIZED: hidden = false; resize = true; break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: resize = true; break;
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED: refresh(); break;
        default: break;
      }
  };
  auto stopping = [&] {
    std::lock_guard lock(mutex_);
    return stop_;
  };
  auto idle_wait = [&](Clock::duration d) {
    std::unique_lock lock(mutex_);
    changed_.wait_for(lock, d, [&] { return stop_; });
  };
  bool fatal = !open_window();
  if (!fatal) {
    SDL_ShowWindow(window_);
    refresh();
  }
  while (!fatal && !close && !stopping()) {
    pump();
    if (close) break;
    if (hidden) {
      idle_wait(50ms);
      continue;
    }
    if (!wait_acquire()) continue;
    if (resize || !swapchain_) {
      wait_frames();
      if (!create_swapchain()) {
        if (failed_) break;
        idle_wait(50ms);
        continue;
      }
      resize = false;
    }
    if (const auto next = last_present + std::chrono::duration_cast<Clock::duration>(period); Clock::now() < next) {
      idle_wait(std::min<Clock::duration>(next - Clock::now(), 16ms));
      continue;
    }
    auto& frame = frames_[frame_index_];
    if (frame.pending) {
      if (fn<PFN_vkWaitForFences>("vkWaitForFences")(binding_.device, 1, &frame.fence, VK_TRUE, 100'000'000) !=
          VK_SUCCESS)
        continue;
      frame.pending = false;
    }
    VkResult r = VK_SUCCESS;
    if (!holding_) {
      std::uint32_t index = 0;
      r = fn<PFN_vkAcquireNextImageKHR>("vkAcquireNextImageKHR")(binding_.device, swapchain_, 50'000'000,
                                                                 VK_NULL_HANDLE, acquire_fence_, &index);
      if (r == VK_TIMEOUT || r == VK_NOT_READY) continue;
      if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        resize = true;
        continue;
      }
      if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        log::warn("mirror", "Desktop mirror acquire failed: {}", int(r));
        break;
      }
      if (r == VK_SUBOPTIMAL_KHR) resize = true;
      image_index_ = index;
      acquiring_ = true;
      if (!wait_acquire()) continue;
    }
    const auto index = image_index_;
    {
      std::lock_guard lock(mutex_);
      set_slot(Slot::ready);
    }
    // The render thread takes the image with its next eye frame.
    bool submitted = false;
    for (;;) {
      std::unique_lock lock(mutex_);
      changed_.wait_for(lock, 16ms, [&] { return stop_ || slot_ == Slot::submitted; });
      if (slot_ == Slot::submitted) {
        set_slot(Slot::idle);
        submitted = true;
        break;
      }
      if (stop_ && slot_ == Slot::ready) {
        set_slot(Slot::idle);
        break;
      }
      lock.unlock();
      pump();
      lock.lock();
      if ((close || hidden) && slot_ == Slot::ready) {
        set_slot(Slot::idle);
        break;
      }
    }
    // A reclaimed image stays acquired for the next frame.
    if (!submitted) continue;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &rendered_[index];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &index;
    // The mirror's own queue: a present that blocks holds nothing the game uses.
    const auto start = Clock::now();
    r = fn<PFN_vkQueuePresentKHR>("vkQueuePresentKHR")(present_queue_, &present);
    holding_ = false;
    last_present = Clock::now();
    present_cost_.add(last_present - start);
    if (present_cost_.count == 1800) {  // about 30 s in
      Cost capture;
      {
        std::lock_guard lock(mutex_);
        capture = capture_cost_;
      }
      log_costs(capture);
    }
    frame_index_ ^= 1;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) resize = true;
    else if (r != VK_SUCCESS) {
      log::warn("mirror", "Desktop mirror present failed: {}", int(r));
      break;
    }
  }
  close_window();
  std::lock_guard lock(mutex_);
  set_slot(Slot::idle);
  finished_ = true;
  changed_.notify_all();
  if (present_cost_.count) log_costs(capture_cost_);
  if (close) {
    user_closed_ = true;
    log::info("mirror", "Desktop mirror closed");
  } else if (!stop_) {
    failed_ = true;
  }
}

void Mirror::release_device(VkDevice device) {
  {
    std::lock_guard lock(mutex_);
    if (!started_ || device != binding_.device) return;
    stop_ = true;
  }
  changed_.notify_all();
  std::unique_lock lock(mutex_);
  // Bounded: a driver that never returns from a present must not hang the
  // device's teardown (or the game's quit) with it.
  if (changed_.wait_for(lock, 2s, [&] { return finished_; })) {
    lock.unlock();
    if (thread_.joinable()) thread_.join();
    lock.lock();
  } else {
    log::warn("mirror", "Desktop mirror thread did not stop; leaving it");
    thread_.detach();
    failed_ = true;
  }
  started_ = false;
  stop_ = false;
  dormant_ = !user_closed_ && !failed_;
  binding_ = {};
  queue_ = present_queue_ = {};
  formats_.clear();
}
} // namespace

void mirror_axis(const XrFovf& fov, float& x, float& y) {
  const float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight);
  const float u = std::tan(fov.angleUp), d = std::tan(fov.angleDown);
  x = r - l > 1e-6f ? std::clamp(-l / (r - l), 0.f, 1.f) : 0.5f;
  y = u - d > 1e-6f ? std::clamp(u / (u - d), 0.f, 1.f) : 0.5f;
}
bool mirror_ready() { return mirror_mode() != MirrorMode::off && mirror().wants(); }
void mirror_capture(const XrGraphicsBindingVulkanKHR& binding, bool shared_queue, const MirrorSource& source) {
  if (mirror_mode() == MirrorMode::off || !source.image) return;
  mirror().capture(binding, shared_queue, source);
}
} // namespace venture::xr
