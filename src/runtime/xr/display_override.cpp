#include "xr/display_override.h"
#include "common/environment.h"
#include "common/library/display.h"
#include "common/log.h"
#include "xr/eye_scale.h"
#include "xr/host_xr.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <mutex>
#include <tuple>
#include <string>

namespace venture::xr {
namespace {
template <class T>
bool parse_number(std::string_view text, T& out) {
  const auto* end = text.data() + text.size();
  auto [ptr, ec] = std::from_chars(text.data(), end, out);
  return ec == std::errc{} && ptr == end;
}
constexpr std::uint32_t kMinSide = library::min_custom_side, kMaxSide = library::max_custom_side;
// The launcher offers 50-200%; a little beyond still makes sense, far beyond is a typo.
constexpr double kMinPercent = 10, kMaxPercent = 400;
constexpr float kMinRate = library::min_custom_rate, kMaxRate = library::max_custom_rate;
} // namespace

DisplayOverride parse_display_override(std::string_view resolution, std::string_view rate) {
  DisplayOverride o;
  if (resolution.ends_with('%')) {
    double percent = 0;
    if (parse_number(resolution.substr(0, resolution.size() - 1), percent) && percent >= kMinPercent &&
        percent <= kMaxPercent)
      o.pixel_scale = percent / 100;
  } else if (const auto x = resolution.find('x'); x != std::string_view::npos) {
    std::uint32_t w = 0, h = 0;
    if (parse_number(resolution.substr(0, x), w) && parse_number(resolution.substr(x + 1), h) && w >= kMinSide &&
        h >= kMinSide && w <= kMaxSide && h <= kMaxSide) {
      o.width = w;
      o.height = h;
    }
  }
  float hz = 0;
  if (rate != "runtime" && parse_number(rate, hz) && std::isfinite(hz) && hz >= kMinRate && hz <= kMaxRate) o.rate = hz;
  return o;
}

DisplayOverride display_override() {
  const char* resolution = environment::getenv("VENTURE_RESOLUTION");
  const char* rate = environment::getenv("VENTURE_FRAME_RATE");
  auto o = parse_display_override(resolution ? resolution : "", rate ? rate : "");
  // Report each distinct setting once, including one that was not understood.
  static std::mutex mutex;
  static std::string reported;
  const std::string key = std::string(resolution ? resolution : "") + "|" + (rate ? rate : "");
  std::lock_guard lock(mutex);
  if (key != reported) {
    reported = key;
    const bool resolution_set = resolution && *resolution && std::string_view(resolution) != "runtime";
    const bool rate_set = rate && *rate && std::string_view(rate) != "runtime";
    if (resolution_set && !o.resizes())
      log::warn("xr", "Resolution setting '{}' not understood; using the host runtime's size", resolution);
    if (rate_set && !o.rate)
      log::warn("xr", "Frame rate setting '{}' not understood; using the host runtime's rate", rate);
    if (o.resizes() || o.rate)
      log::info("xr", "Display override: resolution {}, frame rate {}", resolution_set ? resolution : "runtime",
                rate_set ? rate : "runtime");
  }
  return o;
}

XrExtent2Di override_view_size(const DisplayOverride& o, const XrViewConfigurationView& host) {
  double w = host.recommendedImageRectWidth, h = host.recommendedImageRectHeight;
  if (o.width && o.height) {
    w = o.width;
    h = o.height;
  } else if (o.pixel_scale > 0) {
    const double k = std::sqrt(o.pixel_scale);
    w *= k;
    h *= k;
  }
  // The launcher shows the same size for a percent (library/display.h).
  const auto [width, height] = library::fit_eye_size(w, h, int(host.maxImageRectWidth), int(host.maxImageRectHeight));
  return {width, height};
}

void apply_display_override(XrViewConfigurationView* views, std::uint32_t count) {
  if (!views || !count) return;
  const auto o = display_override();
  for (std::uint32_t i = 0; i < count; ++i) {
    if (o.resizes()) {
      const auto size = override_view_size(o, views[i]);
      views[i].recommendedImageRectWidth = std::uint32_t(size.width);
      views[i].recommendedImageRectHeight = std::uint32_t(size.height);
    }
    // The recommended size (the target) already includes the host's
    // supersampling, so it is also the most a game is offered: engines clamp
    // their own resolution scale to the maximum (OpenXR allows a maximum equal
    // to the recommendation). A game known to multiply the size it is told is
    // told the target divided by its multiplier (eye_scale.h); the maximum
    // stays the target.
    const std::uint32_t target_width = views[i].recommendedImageRectWidth;
    const std::uint32_t target_height = views[i].recommendedImageRectHeight;
    if (!target_width || !target_height) continue;
    views[i].maxImageRectWidth = target_width;
    views[i].maxImageRectHeight = target_height;
    if (const double k = eye_scale::compensation(); k > 1) {
      const auto side = [&](std::uint32_t v) {
        return std::max(std::min(v, kMinSide), std::uint32_t(std::lround(v / k)));
      };
      views[i].recommendedImageRectWidth = side(target_width);
      views[i].recommendedImageRectHeight = side(target_height);
    }
  }
}

ReportedEye reported_eye() {
  XrViewConfigurationView views[2]{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
  std::uint32_t count = 0;
  if (!host_xr().instance ||
      XR_FAILED(::xrEnumerateViewConfigurationViews(host_xr().instance, host_xr().system,
                                                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, views)) ||
      !count)
    return {};
  apply_display_override(views, count);
  ReportedEye eye;
  for (std::uint32_t i = 0; i < count; ++i) {
    eye.told.width = std::max(eye.told.width, views[i].recommendedImageRectWidth);
    eye.told.height = std::max(eye.told.height, views[i].recommendedImageRectHeight);
    eye.target.width = std::max(eye.target.width, views[i].maxImageRectWidth);
    eye.target.height = std::max(eye.target.height, views[i].maxImageRectHeight);
  }
  return eye;
}

std::optional<float> supported_override_rate(float rate, const float* rates, std::uint32_t count) {
  if (!(rate > 0) || !rates) return std::nullopt;
  for (std::uint32_t i = 0; i < count; ++i)
    if (std::abs(rates[i] - rate) < 0.5f) return rates[i];
  return std::nullopt;
}
MirrorMode parse_mirror_mode(std::string_view value) {
  if (value == "cropped") return MirrorMode::cropped;
  if (value == "full") return MirrorMode::full;
  return MirrorMode::off;
}
MirrorMode mirror_mode() {
  static const MirrorMode mode = [] {
    const char* value = venture::environment::getenv("VENTURE_MIRROR");
    return parse_mirror_mode(value ? value : "");
  }();
  return mode;
}
std::uint32_t parse_mirror_eye(std::string_view value) { return value == "right" ? 1 : 0; }
std::uint32_t mirror_eye() {
  static const std::uint32_t eye = [] {
    const char* value = venture::environment::getenv("VENTURE_MIRROR_EYE");
    return parse_mirror_eye(value ? value : "");
  }();
  return eye;
}
const std::vector<const char*>& mirror_instance_extensions() {
  static const std::vector<const char*> names = [] {
    std::vector<const char*> out;
    if (mirror_mode() == MirrorMode::off) return out;
#ifdef _WIN32
    out = {"VK_KHR_surface", "VK_KHR_win32_surface"};
#else
    out = {"VK_KHR_surface", "VK_KHR_wayland_surface"};
#endif
    return out;
  }();
  return names;
}
const char* mirror_device_extension() { return "VK_KHR_swapchain"; }
namespace {
std::mutex capable_mutex;
std::vector<const void*> capable;
}
void note_mirror_capable(const void* handle) {
  std::lock_guard lock(capable_mutex);
  if (std::ranges::find(capable, handle) == capable.end()) capable.push_back(handle);
}
bool mirror_capable(const void* handle) {
  std::lock_guard lock(capable_mutex);
  return std::ranges::find(capable, handle) != capable.end();
}
namespace {
std::mutex queues_mutex;
std::vector<std::tuple<VkDevice, std::uint32_t, std::uint32_t>> mirror_queues;
}
std::optional<std::pair<std::uint32_t, std::uint32_t>> mirror_queue(VkDevice device) {
  std::lock_guard lock(queues_mutex);
  for (const auto& [d, family, index] : mirror_queues)
    if (d == device) return std::pair{family, index};
  return std::nullopt;
}
void forget_mirror_queue(VkDevice device) {
  std::lock_guard lock(queues_mutex);
  std::erase_if(mirror_queues, [&](const auto& q) { return std::get<0>(q) == device; });
}
void MirrorQueueRequest::add(std::span<const VkQueueFamilyProperties> families, VkDeviceCreateInfo& info) {
  queue_.reset();
  queues_.assign(info.pQueueCreateInfos, info.pQueueCreateInfos + info.queueCreateInfoCount);
  auto used = [&](std::uint32_t family) {
    std::uint32_t count = 0;
    for (const auto& q : queues_)
      if (q.queueFamilyIndex == family) count += q.queueCount;
    return count;
  };
  auto spare = [&](std::uint32_t family, VkQueueFlags flags) {
    return family < families.size() && (families[family].queueFlags & flags) && used(family) < families[family].queueCount;
  };
  std::optional<std::uint32_t> family;
  // Preferably next to the device's own graphics queue, so the swapchain needs no sharing between families.
  for (const auto& q : queues_)
    if (!q.flags && spare(q.queueFamilyIndex, VK_QUEUE_GRAPHICS_BIT)) {
      family = q.queueFamilyIndex;
      break;
    }
  for (VkQueueFlags flags : {VkQueueFlags(VK_QUEUE_GRAPHICS_BIT), VkQueueFlags(VK_QUEUE_COMPUTE_BIT)})
    for (std::uint32_t f = 0; !family && f < families.size(); ++f)
      if (spare(f, flags)) family = f;
  if (!family) {
    log::info("mirror", "Desktop mirror off for this device: no queue to spare for presenting");
    return;
  }
  auto existing = std::ranges::find_if(queues_, [&](const auto& q) { return !q.flags && q.queueFamilyIndex == *family; });
  if (existing != queues_.end()) {
    priorities_.assign(existing->pQueuePriorities, existing->pQueuePriorities + existing->queueCount);
    queue_ = std::pair{*family, existing->queueCount};
    existing->queueCount++;
  } else {
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = *family;
    q.queueCount = 1;
    priorities_.clear();
    queue_ = std::pair{*family, 0u};
    queues_.push_back(q);
    existing = queues_.end() - 1;
  }
  // Lowest priority: the mirror never competes with the game's own work.
  priorities_.push_back(0.0f);
  existing->pQueuePriorities = priorities_.data();
  info.queueCreateInfoCount = static_cast<std::uint32_t>(queues_.size());
  info.pQueueCreateInfos = queues_.data();
}
void MirrorQueueRequest::created(VkDevice device) const {
  // Also replaces what a destroyed device with the same handle left.
  std::lock_guard lock(queues_mutex);
  std::erase_if(mirror_queues, [&](const auto& q) { return std::get<0>(q) == device; });
  if (queue_) mirror_queues.emplace_back(device, queue_->first, queue_->second);
}
} // namespace venture::xr
