#include "xr/stats_overlay.h"
#include "common/environment.h"
#include "common/log.h"
#include "core/memory_pressure.h"
#include "gfx/gpu_device.h"
#include "platform/process_info.h"
#include "xr/dev_dump.h"
#include "xr/host_xr.h"
#include "xr/queue.h"
#include "xr/session.h"
#include "xr/system_keyboard.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

namespace venture::xr::stats_overlay {
namespace {
using Clock = std::chrono::steady_clock;
constexpr XrPosef kPose{{0, 0, 0, 1}, {0, 0.22f, -1.0f}};  // in VIEW space
constexpr float kWidth = 0.5f;                             // meters across
constexpr auto kInterval = std::chrono::milliseconds(500);

// ---- The picture ----
// Premultiplied 0xAARRGGBB.
constexpr std::uint32_t kLabel = 0xffd8d8d8u, kValue = 0xff58d8ffu, kGreen = 0xff58ee8au, kAmber = 0xffffbf3fu,
                        kRed = 0xffff5757u, kViolet = 0xffc07affu, kBand = 0xf2303030u, kRule = 0xff4a9effu,
                        kSeparator = 0xff5c5c5cu, kGrid = 0xff4a4a4au;
constexpr int kBandTop = 6, kBandBottom = 74;
constexpr int kSeparators[] = {140, 262, 386, 540, 680, 816};
constexpr int kGraphLeft = 274, kGraphRight = 374, kGraphTop = 18, kGraphBottom = 62;

// A 5x7 font, one byte per row, bit 4 the leftmost column.
const std::uint8_t *glyph(char c) {
  static constexpr std::uint8_t digits[10][7] = {
      {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
      {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
      {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
      {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
      {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}};
  static constexpr std::uint8_t letters[26][7] = {
      {0x0E, 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11}, {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
      {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C},
      {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
      {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
      {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C},
      {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
      {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11},
      {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
      {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
      {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
      {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},
      {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},
      {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}};
  static constexpr std::uint8_t dot[7] = {0, 0, 0, 0, 0, 0x0C, 0x0C}, slash[7] = {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10},
                                percent[7] = {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}, dash[7] = {0, 0, 0, 0x1F, 0, 0, 0},
                                x[7] = {0, 0, 0x11, 0x0A, 0x04, 0x0A, 0x11}, z[7] = {0, 0, 0x1F, 0x02, 0x04, 0x08, 0x1F},
                                m[7] = {0, 0, 0x1A, 0x15, 0x15, 0x11, 0x11}, s[7] = {0, 0, 0x0E, 0x10, 0x0E, 0x01, 0x1E};
  if (c >= '0' && c <= '9') return digits[c - '0'];
  if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
  switch (c) {
    case '.': return dot;
    case '/': return slash;
    case '%': return percent;
    case '-': return dash;
    case 'x': return x;
    case 'z': return z;
    case 'm': return m;
    case 's': return s;
    default: return nullptr;  // a space
  }
}
struct Canvas {
  std::vector<std::uint32_t> pixels = std::vector<std::uint32_t>(std::size_t(panel_width) * panel_height, 0);
  void fill(int x0, int y0, int x1, int y1, std::uint32_t color) {
    for (int y = std::max(y0, 0); y < std::min(y1, int(panel_height)); ++y)
      for (int x = std::max(x0, 0); x < std::min(x1, int(panel_width)); ++x)
        pixels[std::size_t(y) * panel_width + std::size_t(x)] = color;
  }
  // `scale` pixels per font pixel, 6 * scale per character.
  void text(int x, int y, int scale, std::string_view s, std::uint32_t color) {
    for (const char c : s) {
      if (const auto *rows = glyph(c))
        for (int row = 0; row < 7; ++row)
          for (int column = 0; column < 5; ++column)
            if (rows[row] & (0x10 >> column))
              fill(x + column * scale, y + row * scale, x + (column + 1) * scale, y + (row + 1) * scale, color);
      x += 6 * scale;
    }
  }
  void line(int x0, int y0, int x1, int y1, std::uint32_t color) {
    const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    for (int e = dx + dy;;) {
      fill(x0, y0, x0 + 1, y0 + 1, color);
      if (x0 == x1 && y0 == y1) break;
      const int e2 = 2 * e;
      if (e2 >= dy) e += dy, x0 += sx;
      if (e2 <= dx) e += dx, y0 += sy;
    }
  }
};
std::string number(double value, const char *format_1, const char *suffix = "") {
  if (!(value > 0) || !std::isfinite(value)) return "-";
  return std::vformat(format_1, std::make_format_args(value)) + suffix;
}
std::uint32_t load_color(double percent) {
  return !(percent >= 0) ? kLabel : percent < 75 ? kGreen : percent < 90 ? kAmber : kRed;
}
void graph(Canvas &c, std::span<const float> series, float top, std::uint32_t color) {
  if (series.size() < 2) return;
  const auto y = [&](float v) {
    return kGraphBottom - int(std::lround(std::clamp(v / top, 0.f, 1.f) * float(kGraphBottom - kGraphTop)));
  };
  const auto x = [&](std::size_t i) {
    return kGraphLeft + int(i * std::size_t(kGraphRight - kGraphLeft) / (series.size() - 1));
  };
  for (std::size_t i = 1; i < series.size(); ++i) c.line(x(i - 1), y(series[i - 1]), x(i), y(series[i]), color);
}

// ---- Shared state ----
// The frame thread's measurements, read by the sampler.
struct Measured {
  std::atomic<double> game_fps{-1}, worst_ms{-1};
  std::atomic<std::uint32_t> eye_width{0}, eye_height{0};
  std::atomic<std::int64_t> last_frame_ns{0};
} measured;
// The sampler's newest picture, taken by the frame thread.
struct Picture {
  std::mutex mutex;
  std::vector<std::uint32_t> pixels;
  std::uint64_t generation = 0;
} picture;
std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

// ---- The sampler thread ----
std::uint64_t process_cpu_ns() {
#ifdef _WIN32
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
  const auto value = [](const FILETIME &t) { return (std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
  return (value(kernel) + value(user)) * 100;
#else
  timespec t{};
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t) != 0) return 0;
  return std::uint64_t(t.tv_sec) * 1'000'000'000u + std::uint64_t(t.tv_nsec);
#endif
}
void sample() {
  constexpr double gib = 1024.0 * 1024.0 * 1024.0;
  const unsigned processors = std::max(1u, std::thread::hardware_concurrency());
  std::array<std::vector<float>, 2> history;  // headset, game
  constexpr std::size_t kHistory = 60;
  std::uint64_t cpu_before = process_cpu_ns();
  auto wall_before = Clock::now();
  std::map<std::string, std::uint64_t> engines_before;
  std::vector<platform::GpuEngineTime> engines;
  for (bool first = true;; first = false) {
    if (!first) std::this_thread::sleep_for(kInterval);
    // Nothing to show between sessions or while the game shows no frames.
    if (now_ns() - measured.last_frame_ns.load(std::memory_order_relaxed) > 2'000'000'000) continue;
    Snapshot s;
    const auto wall = Clock::now();
    const double wall_ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(wall - wall_before).count());
    wall_before = wall;
    const auto cpu = process_cpu_ns();
    if (!first && wall_ns > 0 && cpu >= cpu_before)
      s.cpu_percent = std::min(100.0, double(cpu - cpu_before) / wall_ns / processors * 100);
    cpu_before = cpu;
    // Each engine's running time for this process (Windows GPU Engine counters,
    // Linux DRM fdinfo): the busiest engine's share of the interval.
    if (platform::gpu_engine_times(engines)) {
      std::map<std::string, std::uint64_t> current;
      std::uint64_t busiest = 0;
      bool any = false;
      for (const auto &e : engines) {
        current[e.engine] = e.ns;
        if (const auto old = engines_before.find(e.engine); old != engines_before.end() && e.ns >= old->second) {
          busiest = std::max(busiest, e.ns - old->second);
          any = true;
        }
      }
      if (any && wall_ns > 0) s.gpu_percent = std::min(100.0, double(busiest) / wall_ns * 100);
      engines_before = std::move(current);
    }
    if (platform::CommitCharge commit; platform::commit_charge(commit) && commit.process)
      s.ram_gib = double(commit.process) / gib;
    if (std::uint64_t usage = 0, budget = 0; memory_pressure::gpu_memory(usage, budget) && usage) {
      s.vram_gib = double(usage) / gib;
      if (budget) s.vram_percent = 100.0 * double(usage) / double(budget);
    }
    s.headset_hz = session().display_timing.rate();
    s.game_fps = measured.game_fps.load(std::memory_order_relaxed);
    s.worst_ms = measured.worst_ms.load(std::memory_order_relaxed);
    s.eye_width = measured.eye_width.load(std::memory_order_relaxed);
    s.eye_height = measured.eye_height.load(std::memory_order_relaxed);
    for (auto [series, value] : {std::pair{&history[0], s.headset_hz}, std::pair{&history[1], s.game_fps}}) {
      series->push_back(float(std::max(value, 0.0)));
      if (series->size() > kHistory) series->erase(series->begin());
    }
    auto pixels = paint(s, history[0], history[1]);
    std::lock_guard lock(picture.mutex);
    picture.pixels = std::move(pixels);
    ++picture.generation;
  }
}
void start_sampler() {
  static std::once_flag once;
  std::call_once(once, [] {
    log::info("overlay", "Performance overlay on");
    // Process lifetime, like the session timeline's thread: it holds no
    // Vulkan or OpenXR object, so it needs no teardown.
    std::thread(sample).detach();
  });
}

// ---- The frame thread ----
// The game's frame rate and longest interval, over half-second windows.
struct Window {
  std::int64_t start_ns = 0, last_ns = 0, worst_ns = 0;
  std::uint32_t frames = 0;
} window;
void note_frame(const XrFrameEndInfo &info) {
  const auto now = now_ns();
  if (window.last_ns && now > window.last_ns) window.worst_ns = std::max(window.worst_ns, now - window.last_ns);
  window.last_ns = now;
  measured.last_frame_ns.store(now, std::memory_order_relaxed);
  if (!window.start_ns) window.start_ns = now;
  ++window.frames;
  if (const auto elapsed = now - window.start_ns; elapsed >= 500'000'000) {
    measured.game_fps.store(double(window.frames - 1) * 1e9 / double(elapsed), std::memory_order_relaxed);
    measured.worst_ms.store(double(window.worst_ns) / 1e6, std::memory_order_relaxed);
    window.start_ns = now;
    window.frames = 1;
    window.worst_ns = 0;
  }
  for (std::uint32_t i = 0; i < info.layerCount; ++i)
    if (info.layers[i] && info.layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
      const auto *projection = reinterpret_cast<const XrCompositionLayerProjection *>(info.layers[i]);
      if (projection->viewCount && projection->views) {
        const auto &extent = projection->views[0].subImage.imageRect.extent;
        measured.eye_width.store(std::uint32_t(std::max(extent.width, 0)), std::memory_order_relaxed);
        measured.eye_height.store(std::uint32_t(std::max(extent.height, 0)), std::memory_order_relaxed);
      }
      break;
    }
}

// Frame-thread state, under the session lock.
struct Render {
  XrSession session{};
  bool failed = false, limited = false;
  std::uint32_t layer_limit = 0;
  XrSpace view{};
  XrSwapchain chain{};
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  std::vector<XrSwapchainImageVulkanKHR> images;
  std::uint32_t index = 0;
  bool acquired = false, waited = false, content = false;
  std::uint64_t uploaded = 0;
  XrGraphicsBindingVulkanKHR binding{};
  VkCommandPool pool{};
  VkCommandBuffer command{};
  VkFence fence{};
  bool in_flight = false;
  VkBuffer staging{};
  VkDeviceMemory memory{};
  void *mapped = nullptr;
  bool coherent = false;
};
Render &render() {
  static Render value;
  return value;
}
template <class F> F vk(const char *name) {
  return reinterpret_cast<F>(gfx::gpu_device().get_instance_proc_addr(render().binding.instance, name));
}
bool checked(VkResult result, const char *what) {
  if (result == VK_SUCCESS) return true;
  log::warn("overlay", "{} failed: VkResult={}", what, int(result));
  return false;
}
// The caller holds the session lock and the queue lock, and no host End is in flight.
void destroy_render() {
  auto &r = render();
  if (r.binding.device) {
    // Only at teardown, and bounded: the copy is one small image.
    if (r.in_flight) vk<PFN_vkWaitForFences>("vkWaitForFences")(r.binding.device, 1, &r.fence, VK_TRUE, 1'000'000'000);
    if (r.mapped) vk<PFN_vkUnmapMemory>("vkUnmapMemory")(r.binding.device, r.memory);
    if (r.staging) vk<PFN_vkDestroyBuffer>("vkDestroyBuffer")(r.binding.device, r.staging, nullptr);
    if (r.memory) vk<PFN_vkFreeMemory>("vkFreeMemory")(r.binding.device, r.memory, nullptr);
    if (r.fence) vk<PFN_vkDestroyFence>("vkDestroyFence")(r.binding.device, r.fence, nullptr);
    if (r.pool) vk<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(r.binding.device, r.pool, nullptr);
  }
  if (r.chain) {
    if (r.acquired) {
      if (!r.waited) {
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
        ::xrWaitSwapchainImage(r.chain, &wait);
      }
      XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      ::xrReleaseSwapchainImage(r.chain, &release);
    }
    dev_destroy_swapchain(r.chain);
  }
  if (r.view) ::xrDestroySpace(r.view);
  r = {};
}
// The session goes on without the overlay; what was made goes with the
// session (reset_session), as the caller may hold the queue lock here.
void fail(const char *what) {
  log::warn("overlay", "The performance overlay is unavailable this session: {}", what);
  render().failed = true;
}
bool create_gpu() {
  auto &r = render();
  r.binding = session().binding;
  const auto device = r.binding.device;
  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = r.binding.queueFamilyIndex;
  if (!checked(vk<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device, &pci, nullptr, &r.pool), "vkCreateCommandPool"))
    return false;
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, r.pool,
                                  VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
  if (!checked(vk<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device, &cai, &r.command),
               "vkAllocateCommandBuffers"))
    return false;
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (!checked(vk<PFN_vkCreateFence>("vkCreateFence")(device, &fci, nullptr, &r.fence), "vkCreateFence")) return false;
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = VkDeviceSize(panel_width) * panel_height * 4;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if (!checked(vk<PFN_vkCreateBuffer>("vkCreateBuffer")(device, &bci, nullptr, &r.staging), "vkCreateBuffer"))
    return false;
  VkMemoryRequirements requirements{};
  vk<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device, r.staging, &requirements);
  VkPhysicalDeviceMemoryProperties properties{};
  vk<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(r.binding.physicalDevice,
                                                                                    &properties);
  std::uint32_t type = properties.memoryTypeCount;
  for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
    const auto flags = properties.memoryTypes[i].propertyFlags;
    if ((requirements.memoryTypeBits & (1u << i)) && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
      type = i;
      if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) break;
    }
  }
  if (type == properties.memoryTypeCount) return false;
  r.coherent = properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, type};
  return checked(vk<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &mai, nullptr, &r.memory), "vkAllocateMemory") &&
         checked(vk<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device, r.staging, r.memory, 0),
                 "vkBindBufferMemory") &&
         checked(vk<PFN_vkMapMemory>("vkMapMemory")(device, r.memory, 0, VK_WHOLE_SIZE, 0, &r.mapped), "vkMapMemory");
}
bool create_image(XrSession handle) {
  auto &r = render();
  r.info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
  r.info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                      XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  // sRGB first: the picture's bytes are sRGB; a host without 8-bit sRGB gets
  // linear values (create_host_swapchain's fallback, keyboard::convert).
  r.info.format = VK_FORMAT_R8G8B8A8_SRGB;
  r.info.sampleCount = r.info.faceCount = r.info.arraySize = r.info.mipCount = 1;
  r.info.width = panel_width;
  r.info.height = panel_height;
  XrResult result;
  {
    HostQueueLock queue(session().uses_shared_gpu);
    if (!queue) return false;
    result = create_host_swapchain(handle, r.info, &r.chain);
  }
  if (XR_FAILED(result)) return false;
  std::uint32_t count = 0;
  if (XR_FAILED(::xrEnumerateSwapchainImages(r.chain, 0, &count, nullptr)) || !count || count > 32) return false;
  r.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
  return XR_SUCCEEDED(::xrEnumerateSwapchainImages(r.chain, count, &count,
      reinterpret_cast<XrSwapchainImageBaseHeader *>(r.images.data())));
}
// Acquired and waited for: ready to write. False to try again next frame; a
// failure is reported through `failed`.
bool ready(bool &failed) {
  auto &r = render();
  if (!r.acquired) {
    HostQueueLock queue(session().uses_shared_gpu);
    if (!queue || XR_FAILED(::xrAcquireSwapchainImage(r.chain, nullptr, &r.index)) || r.index >= r.images.size())
      return !(failed = true);
    r.acquired = true;
    r.waited = false;
  }
  if (!r.waited) {
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, 0};
    const auto result = ::xrWaitSwapchainImage(r.chain, &wait);
    if (result == XR_TIMEOUT_EXPIRED) return false;
    if (XR_FAILED(result)) return !(failed = true);
    r.waited = true;
  }
  return true;
}
// Uploads the sampler's newest picture. Never waits: a copy still in flight or
// a swapchain image not yet free leaves it for a later frame.
void upload(XrSession handle) {
  auto &r = render();
  {
    std::lock_guard lock(picture.mutex);
    if (!picture.generation || picture.generation == r.uploaded) return;
  }
  if (!r.binding.device && !create_gpu()) return fail("staging memory");
  if (!r.chain && !create_image(handle)) return fail("swapchain");
  if (r.in_flight) {
    if (vk<PFN_vkGetFenceStatus>("vkGetFenceStatus")(r.binding.device, r.fence) != VK_SUCCESS) return;
    r.in_flight = false;
  }
  bool failed = false;
  if (!ready(failed)) {
    if (failed) fail("swapchain image");
    return;
  }
  std::uint64_t generation;
  {
    std::lock_guard lock(picture.mutex);
    if (picture.pixels.size() != std::size_t(panel_width) * panel_height) return;
    keyboard::convert(picture.pixels.data(), picture.pixels.size(), r.info.format, static_cast<std::uint8_t *>(r.mapped));
    generation = picture.generation;
  }
  if (!r.coherent) {
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, r.memory, 0, VK_WHOLE_SIZE};
    vk<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges")(r.binding.device, 1, &range);
  }
  HostQueueLock queue(session().uses_shared_gpu);
  if (!queue) return fail("queue lock");
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (!checked(vk<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(r.command, 0), "vkResetCommandBuffer") ||
      !checked(vk<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(r.command, &begin), "vkBeginCommandBuffer"))
    return fail("command buffer");
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // the whole image is written
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = r.images[r.index].image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(r.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {panel_width, panel_height, 1};
  vk<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(r.command, r.staging, barrier.image,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  // OpenXR: a released Vulkan color image is in COLOR_ATTACHMENT_OPTIMAL.
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(r.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &r.command;
  if (!checked(vk<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(r.command), "vkEndCommandBuffer") ||
      !checked(vk<PFN_vkResetFences>("vkResetFences")(r.binding.device, 1, &r.fence), "vkResetFences") ||
      !checked(vk<PFN_vkQueueSubmit>("vkQueueSubmit")(session().queue, 1, &submit, r.fence), "vkQueueSubmit"))
    return fail("upload");
  r.in_flight = true;
  // The copy is queued ahead of the release on the binding's queue, as OpenXR
  // requires; the runtime waits for it, not this thread.
  XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  if (XR_FAILED(dev_release_swapchain(r.chain, &release, r.images[r.index].image))) return fail("swapchain release");
  r.acquired = r.waited = false;
  r.content = true;
  r.uploaded = generation;
}
std::uint32_t host_layer_limit() {
  XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
  auto &host = host_xr();
  if (host.available && XR_SUCCEEDED(::xrGetSystemProperties(host.instance, host.system, &properties)))
    return properties.graphicsProperties.maxLayerCount;
  return 0;
}
}  // namespace

void set_enabled(bool shown) { detail::state.store(shown ? 1 : 0, std::memory_order_relaxed); }

std::shared_ptr<const Frame> extend(XrSession handle, const XrFrameEndInfo &info, std::shared_ptr<const void> game) {
  note_frame(info);
  start_sampler();
  auto &r = render();
  if (r.session != handle) {
    if (r.session) {
      HostQueueLock queue(session().uses_shared_gpu);
      destroy_render();
    }
    r.session = handle;
    r.layer_limit = host_layer_limit();
  }
  // A native headset session has no Vulkan binding of Venture's to draw with.
  if (r.failed || !session().binding.device) return nullptr;
  if (!fits(info.layerCount, r.layer_limit)) {
    if (!r.limited)
      log::info("overlay", "The frame has the host's {} layers; the performance overlay waits for room", r.layer_limit);
    r.limited = true;
    return nullptr;
  }
  if (!r.view) {
    XrReferenceSpaceCreateInfo ci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, XR_REFERENCE_SPACE_TYPE_VIEW,
                                  {{0, 0, 0, 1}, {0, 0, 0}}};
    if (XR_FAILED(::xrCreateReferenceSpace(handle, &ci, &r.view))) {
      fail("view space");
      return nullptr;
    }
  }
  upload(handle);
  if (r.failed || !r.content) return nullptr;
  auto frame = std::make_shared<Frame>();
  frame->game = std::move(game);
  frame->layers.assign(info.layers, info.layers + info.layerCount);
  // Premultiplied alpha: the picture is drawn that way.
  frame->quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  frame->quad.space = r.view;
  frame->quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  frame->quad.subImage = {r.chain, {{0, 0}, {std::int32_t(panel_width), std::int32_t(panel_height)}}, 0};
  frame->quad.pose = kPose;
  frame->quad.size = {kWidth, kWidth * float(panel_height) / float(panel_width)};
  frame->layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&frame->quad));
  frame->info = info;
  frame->info.layerCount = std::uint32_t(frame->layers.size());
  frame->info.layers = frame->layers.data();
  return frame;
}

void reset_session(XrSession handle) {
  auto &r = render();
  if (r.session == handle) destroy_render();
  window = {};
}

std::vector<std::uint32_t> paint(const Snapshot &s, std::span<const float> headset, std::span<const float> game) {
  Canvas c;
  c.fill(0, kBandTop, int(panel_width), kBandBottom, kBand);
  c.fill(0, kBandTop, int(panel_width), kBandTop + 2, kRule);
  c.fill(0, kBandBottom - 2, int(panel_width), kBandBottom, kRule);
  for (const int x : kSeparators) c.fill(x, kBandTop + 10, x + 1, kBandBottom - 10, kSeparator);
  const double hz = s.headset_hz > 0 ? s.headset_hz : 0;
  c.text(16, 16, 2, "HEADSET", kLabel);
  c.text(16, 36, 3, number(s.headset_hz, "{:.0f}", " Hz"), kValue);
  c.text(152, 16, 2, "GAME", kLabel);
  const auto fps_color = !(s.game_fps > 0) ? kLabel
                         : !(hz > 0)       ? kValue
                         : s.game_fps >= hz * 0.95 ? kGreen
                         : s.game_fps >= hz * 0.75 ? kAmber
                                                   : kRed;
  c.text(152, 36, 3, number(s.game_fps, "{:.1f}"), fps_color);
  // Headset (green) and game (violet) rates, up to a little above the headset's.
  const float top = float(std::max(hz, 60.0) * 1.1);
  c.line(kGraphLeft, kGraphBottom, kGraphRight, kGraphBottom, kGrid);
  graph(c, headset, top, kGreen);
  graph(c, game, top, kViolet);
  // The longest interval between the game's frames: a stutter shows here even
  // when the average rate looks fine.
  c.text(398, 16, 2, "WORST", kLabel);
  const double period_ms = hz > 0 ? 1000 / hz : 0;
  const auto worst_color = !(s.worst_ms > 0) || !(period_ms > 0) ? kValue
                           : s.worst_ms <= period_ms * 1.5       ? kGreen
                           : s.worst_ms <= period_ms * 3         ? kAmber
                                                                 : kRed;
  c.text(398, 36, 3, number(s.worst_ms, s.worst_ms >= 100 ? "{:.0f}" : "{:.1f}", " ms"), worst_color);
  c.text(552, 16, 2, "EYE", kLabel);
  c.text(552, 42, 2, s.eye_width && s.eye_height ? std::format("{}x{}", s.eye_width, s.eye_height) : "-", kValue);
  const auto percent = [](const char *name, double value) {
    return value >= 0 ? std::format("{} {:.0f}%", name, value) : std::format("{} -", name);
  };
  const auto gib = [](const char *name, double value) {
    if (!(value > 0)) return std::format("{} -", name);
    return value < 10 ? std::format("{} {:.1f}G", name, value) : std::format("{} {:.0f}G", name, value);
  };
  c.text(692, 20, 2, percent("CPU", s.cpu_percent), load_color(s.cpu_percent));
  c.text(692, 46, 2, gib("RAM", s.ram_gib), kLabel);
  c.text(828, 20, 2, percent("GPU", s.gpu_percent), load_color(s.gpu_percent));
  c.text(828, 46, 2, gib("VRAM", s.vram_gib), load_color(s.vram_percent));
  return std::move(c.pixels);
}
}  // namespace venture::xr::stats_overlay
