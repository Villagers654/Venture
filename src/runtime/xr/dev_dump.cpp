#include "common/environment.h"
#include "xr/dev_dump.h"
#include "xr/capture.h"
#include "gfx/gpu_device.h"
#include <algorithm>
#include "common/log.h"
#include "common/json.h"
#include "common/paths.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace venture::xr {
namespace {
std::mutex mutex;
XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
struct Chain {
  XrSwapchainCreateInfo info;
  std::vector<std::vector<unsigned char>> pixels;
  std::uint64_t captured = 0;
  std::string error;
};
std::map<XrSwapchain, Chain> chains;
std::uint64_t frame = 0;
std::chrono::steady_clock::time_point previous{};
// Kept open: the record is written right after the host xrEndFrame returns,
// which on SteamVR is the frame's wake-up, so a per-frame directory check and
// open/close spent the guest's deadline budget in every measured run.
std::ofstream frames_log;
std::filesystem::path frames_root;
// PNG encoding of a 2800x2856 eye takes ~100 ms; doing it after the host
// xrEndFrame stalled the guest's next frame on every requested dump (a gate
// sampler asking twice a second slowed frame-paced cutscenes to ~12 fps). A
// writer thread encodes the images, then appends the frame's layers.jsonl
// record, so a record is only visible once every file it names exists.
struct EyeWrite {
  std::filesystem::path path;
  std::uint32_t width, height;
  std::vector<unsigned char> pixels;
  XrRect2Di rect;
};
struct DumpJob {
  std::filesystem::path root;
  Json record;
  std::vector<EyeWrite> eyes;
};
void unname_failed(Json& node, const std::map<std::string, std::string>& failed) {
  if (node.is_object()) {
    if (auto file = node.find("file"); file != node.end() && file->is_string()) {
      if (auto it = failed.find(file->get<std::string>()); it != failed.end()) {
        node.erase(file);
        node["error"] = it->second;
      }
    }
  }
  if (node.is_structured())
    for (auto& child : node) unname_failed(child, failed);
}
class DumpWriter {
 public:
  // Bounded: a writer that falls behind drops dumps rather than holding frames.
  bool push(DumpJob job) {
    {
      std::lock_guard lock(mutex_);
      if (queue_.size() >= 4) return false;
      queue_.push_back(std::move(job));
      if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
    }
    ready_.notify_one();
    return true;
  }
  // Waits for queued dumps; the session is ending.
  void drain() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [&] { return queue_.empty() && !busy_; });
  }
 private:
  void run() {
    for (;;) {
      std::unique_lock lock(mutex_);
      ready_.wait(lock, [&] { return !queue_.empty(); });
      auto job = std::move(queue_.front());
      queue_.pop_front();
      busy_ = true;
      lock.unlock();
      std::map<std::string, std::string> failed;
      for (const auto& eye : job.eyes) {
        try { write_eye(eye.path, eye.width, eye.height, eye.pixels, eye.rect); }
        catch (const std::exception& e) { failed[paths::utf8(eye.path.filename())] = e.what(); }
      }
      // A record never names a file that was not written: a failed image
      // carries its error instead, as an image that could not be captured does.
      if (!failed.empty()) unname_failed(job.record, failed);
      try { std::ofstream(job.root / "layers.jsonl", std::ios::app) << job.record.dump() << '\n'; }
      catch (const std::exception& e) { log::warn("xr-dump", "Dump record failed: {}", e.what()); }
      lock.lock();
      busy_ = false;
      if (queue_.empty()) idle_.notify_all();
    }
  }
  std::mutex mutex_;
  std::condition_variable ready_, idle_;
  std::deque<DumpJob> queue_;
  bool busy_ = false;
  std::thread thread_;
};
DumpWriter& writer() {
  static auto* w = new DumpWriter;  // lives for the process; drained at session end
  return *w;
}
std::vector<EyeWrite>* pending_eyes = nullptr;  // the dump being built, under `mutex`
std::filesystem::path directory() {
  const auto* p = venture::environment::getenv("VENTURE_XR_DUMP_DIR");
  return p && *p ? paths::from_utf8(p) : std::filesystem::path{};
}
// A file named dump-request in the dump directory asks for the next frame (consumed once), so a harness can
// capture on demand with VENTURE_XR_DUMP_EVERY set high and keep periodic readbacks out of its timing.
// Polled every 100 ms: a remove per frame cost the render thread ~0.04 ms inside xrEndFrame (NtCreateFile)
// in every SteamVR parity run (AW2 profile, 10-27).
bool requested() {
  static std::uint64_t checked = ~0ull;
  static auto next_poll = std::chrono::steady_clock::time_point{};
  static bool request = false;
  if (checked != frame) {
    checked = frame;
    request = false;
    if (const auto now = std::chrono::steady_clock::now(); now >= next_poll) {
      next_poll = now + std::chrono::milliseconds(100);
      std::error_code ec;
      request = std::filesystem::remove(directory() / "dump-request", ec);
    }
  }
  return request;
}
bool due() {
  auto* p = venture::environment::getenv("VENTURE_XR_DUMP_EVERY");
  const auto every = p ? std::strtol(p, nullptr, 10) : 300;
  return frame == 0 || (frame + 1) % std::max(1L, every) == 0 || requested();
}
Json orientation(const XrQuaternionf& q) { return {q.x, q.y, q.z, q.w}; }
Json pose(const XrPosef& p) {
  return {{"orientation", orientation(p.orientation)},
          {"position", {p.position.x, p.position.y, p.position.z}}};
}
Json dump_image(const std::filesystem::path& root, const std::string& file,
                const XrSwapchainSubImage& sub) {
  Json image{{"swapchain", reinterpret_cast<std::uintptr_t>(sub.swapchain)},
             {"array_slice", sub.imageArrayIndex},
             {"rect", {sub.imageRect.offset.x, sub.imageRect.offset.y,
                        sub.imageRect.extent.width, sub.imageRect.extent.height}}};
  auto it = chains.find(sub.swapchain);
  if (it == chains.end()) { image["error"] = "untracked swapchain"; return image; }
  const auto& c = it->second;
  image["source_frame"] = c.captured;
  image["format"] = c.info.format;
  if (!c.captured || sub.imageArrayIndex >= c.pixels.size()) {
    image["error"] = c.error.empty() ? "no captured array slice" : c.error;
    return image;
  }
  const auto& rect = sub.imageRect;
  if (rect.offset.x < 0 || rect.offset.y < 0 || rect.extent.width <= 0 || rect.extent.height <= 0 ||
      std::uint64_t(rect.offset.x) + rect.extent.width > c.info.width ||
      std::uint64_t(rect.offset.y) + rect.extent.height > c.info.height) {
    image["error"] = "Invalid eye crop";
    return image;
  }
  pending_eyes->push_back({root / file, c.info.width, c.info.height, c.pixels[sub.imageArrayIndex], rect});
  image["file"] = file;
  return image;
}
Json dump_layer(const std::filesystem::path& root, std::uint32_t order,
                const XrCompositionLayerBaseHeader* base) {
  Json layer{{"order", order}};
  if (!base) { layer["error"] = "null layer"; return layer; }
  layer.update({{"type", int(base->type)}, {"flags", base->layerFlags},
                {"space", reinterpret_cast<std::uintptr_t>(base->space)},
                {"blend_texture_source_alpha", bool(base->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT)},
                {"unpremultiplied_alpha", bool(base->layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT)}});
  // Keep alpha modifiers and all next-type tags, without interpreting unknown payloads.
  layer["next_types"] = Json::array();
  unsigned count = 0;
  for (auto* next = static_cast<const XrBaseInStructure*>(base->next);
       next && count++ < 64; next = next->next) {
    layer["next_types"].push_back(int(next->type));
    if (next->type == XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR) {
      const auto& c = *reinterpret_cast<const XrCompositionLayerColorScaleBiasKHR*>(next);
      layer["color_scale"] = {c.colorScale.r, c.colorScale.g, c.colorScale.b, c.colorScale.a};
      layer["color_bias"] = {c.colorBias.r, c.colorBias.g, c.colorBias.b, c.colorBias.a};
    } else if (next->type == XR_TYPE_COMPOSITION_LAYER_ALPHA_BLEND_FB) {
      const auto& a = *reinterpret_cast<const XrCompositionLayerAlphaBlendFB*>(next);
      layer["alpha_blend"] = {int(a.srcFactorColor), int(a.dstFactorColor),
                               int(a.srcFactorAlpha), int(a.dstFactorAlpha)};
    }
  }
  auto image = [&](const XrSwapchainSubImage& sub) {
    return dump_image(root, std::format("frame-{:06}-layer-{}-image.png", frame, order), sub);
  };
  switch (base->type) {
  case XR_TYPE_COMPOSITION_LAYER_PROJECTION: {
    layer["kind"] = "projection";
    const auto& p = *reinterpret_cast<const XrCompositionLayerProjection*>(base);
    layer["views"] = Json::array();
    for (std::uint32_t eye = 0; eye < p.viewCount; ++eye) {
      const auto& v = p.views[eye];
      layer["views"].push_back({{"eye", eye}, {"pose", pose(v.pose)},
          {"fov", {v.fov.angleLeft, v.fov.angleRight, v.fov.angleUp, v.fov.angleDown}},
          {"image", dump_image(root, std::format("frame-{:06}-layer-{}-eye-{}.png", frame, order, eye), v.subImage)}});
    }
    break;
  }
  case XR_TYPE_COMPOSITION_LAYER_QUAD: {
    layer["kind"] = "quad";
    const auto& q = *reinterpret_cast<const XrCompositionLayerQuad*>(base);
    layer.update({{"pose", pose(q.pose)}, {"eye_visibility", int(q.eyeVisibility)},
                  {"size", {q.size.width, q.size.height}}, {"image", image(q.subImage)}});
    break;
  }
  case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR: {
    layer["kind"] = "cylinder";
    const auto& c = *reinterpret_cast<const XrCompositionLayerCylinderKHR*>(base);
    layer.update({{"pose", pose(c.pose)}, {"eye_visibility", int(c.eyeVisibility)},
                  {"radius", c.radius}, {"central_angle", c.centralAngle},
                  {"aspect_ratio", c.aspectRatio}, {"image", image(c.subImage)}});
    break;
  }
  case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR: {
    layer["kind"] = "equirect";
    const auto& e = *reinterpret_cast<const XrCompositionLayerEquirectKHR*>(base);
    layer.update({{"pose", pose(e.pose)}, {"eye_visibility", int(e.eyeVisibility)},
                  {"radius", e.radius}, {"scale", {e.scale.x, e.scale.y}},
                  {"bias", {e.bias.x, e.bias.y}}, {"image", image(e.subImage)}});
    break;
  }
  case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR: {
    layer["kind"] = "equirect2";
    const auto& e = *reinterpret_cast<const XrCompositionLayerEquirect2KHR*>(base);
    layer.update({{"pose", pose(e.pose)}, {"eye_visibility", int(e.eyeVisibility)},
                  {"radius", e.radius}, {"central_horizontal_angle", e.centralHorizontalAngle},
                  {"upper_vertical_angle", e.upperVerticalAngle},
                  {"lower_vertical_angle", e.lowerVerticalAngle}, {"image", image(e.subImage)}});
    break;
  }
  case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR: {
    layer["kind"] = "cube";
    const auto& c = *reinterpret_cast<const XrCompositionLayerCubeKHR*>(base);
    layer.update({{"orientation", orientation(c.orientation)}, {"eye_visibility", int(c.eyeVisibility)},
                  {"image_array_index", c.imageArrayIndex}, {"faces", Json::array()}});
    auto it = chains.find(c.swapchain);
    for (std::uint32_t face = 0; face < 6; ++face) {
      XrSwapchainSubImage sub{c.swapchain, {}, c.imageArrayIndex * 6 + face};
      if (it != chains.end()) sub.imageRect.extent = {int(it->second.info.width), int(it->second.info.height)};
      layer["faces"].push_back(dump_image(root,
          std::format("frame-{:06}-layer-{}-face-{}.png", frame, order, face), sub));
    }
    break;
  }
  default:
    layer["kind"] = "unknown";
    // Passthrough and future/vendor layers need runtime-specific capture. Never omit them.
    layer["image_error"] = "layer has no supported generic swapchain image payload";
    break;
  }
  return layer;
}
}
bool dev_dump_enabled() { return !directory().empty(); }
void dev_dump_bind(const XrGraphicsBindingVulkanKHR& b) {
  if (!dev_dump_enabled()) return;
  std::lock_guard lock(mutex);
  binding = b;
  binding.next = nullptr;
  chains.clear(); frame = 0; previous = {};
}
XrResult dev_create_swapchain(XrSession s, const XrSwapchainCreateInfo* info, XrSwapchain* out) {
  auto copy = *info;
  const bool enabled = dev_dump_enabled();
  if (enabled && (copy.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT))
    copy.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
  auto result = ::xrCreateSwapchain(s, &copy, out);
  log::info("xr", "Host swapchain result={} format={} size={}x{} arrays={} samples={} usage={}",
      int(result), copy.format, copy.width, copy.height, copy.arraySize, copy.sampleCount, copy.usageFlags);
  if (enabled && XR_SUCCEEDED(result)) {
    std::lock_guard lock(mutex);
    chains[*out] = {copy, {}, 0};
  }
  return result;
}
XrResult dev_destroy_swapchain(XrSwapchain s) {
  auto result = ::xrDestroySwapchain(s);
  if (dev_dump_enabled() && XR_SUCCEEDED(result)) {
    std::lock_guard lock(mutex); chains.erase(s);
  }
  return result;
}
XrResult dev_release_swapchain(XrSwapchain s, const XrSwapchainImageReleaseInfo* info, VkImage image) {
  // Caller holds the native queue lock and has waited the oldest acquired image.
  // Readback restores COLOR_ATTACHMENT_OPTIMAL before the runtime can take it.
  if (dev_dump_enabled()) {
    std::lock_guard lock(mutex);
    auto it = chains.find(s);
    if (it != chains.end()) {
      auto& c = it->second;
      c.pixels.clear(); c.captured = 0; c.error.clear();
      if ((due() || (c.info.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT)) && c.info.sampleCount == 1 &&
          (c.info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)) {
        c.pixels.resize(std::size_t(c.info.arraySize) * c.info.faceCount);
        auto get = gfx::gpu_device().get_instance_proc_addr;
        for (std::uint32_t layer = 0; layer < c.pixels.size(); ++layer) {
          XrSwapchainSubImage sub{s, {{0, 0}, {int(c.info.width), int(c.info.height)}}, layer};
          std::string error;
          if (!read_eye(binding, get, image, VkFormat(c.info.format), sub, c.pixels[layer], &error)) {
            log::warn("xr-dump", "Readback failed: {}", error); c.error = error; c.pixels.clear(); break;
          }
        }
        if (!c.pixels.empty()) c.captured = frame + 1;
      } else {
        c.error = "capture not scheduled, or unsupported sample count/image usage";
      }
    }
  }
  auto result = ::xrReleaseSwapchainImage(s, info);
  if (dev_dump_enabled() && XR_FAILED(result)) {
    std::lock_guard lock(mutex);
    if (auto it = chains.find(s); it != chains.end()) it->second.pixels.clear();
  }
  return result;
}
void dev_dump_flush() { writer().drain(); }
void dev_dump_end() {
  dev_dump_flush();
  std::lock_guard lock(mutex);
  frames_log.close();
  frames_root.clear();
}
void dev_dump_frame(const XrFrameEndInfo* info, XrResult result) {
  if (!dev_dump_enabled()) return;
  std::lock_guard lock(mutex);
  try {
    const auto root = directory();
    if (!frames_log.is_open() || frames_root != root) {
      frames_log.close();
      std::filesystem::create_directories(root);
      frames_log.open(root / "frames.jsonl", std::ios::app);
      frames_root = root;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto interval = previous.time_since_epoch().count()
        ? std::chrono::duration<double, std::milli>(now - previous).count() : 0.;
    previous = now;
    frames_log << std::format(
        "{{\"frame\":{},\"result\":{},\"end_interval_ms\":{},\"display_time_ns\":{},\"layers\":{}}}\n",
        frame + 1, int(result), interval, info->displayTime, info->layerCount);
    // Buffered; flushed every 60 frames, on an error and with each dump.
    const bool dump_now = XR_SUCCEEDED(result) && due();
    if (XR_FAILED(result) || dump_now || (frame + 1) % 60 == 0) frames_log.flush();
    if (XR_FAILED(result)) return;
    ++frame;
    if (!dump_now) return;
    Json record{{"frame", frame}, {"display_time_ns", info->displayTime},
                {"environment_blend_mode", int(info->environmentBlendMode)}, {"layers", Json::array()}};
    std::vector<EyeWrite> eyes;
    pending_eyes = &eyes;
    for (std::uint32_t layer = 0; layer < info->layerCount; ++layer)
      record["layers"].push_back(dump_layer(root, layer, info->layers[layer]));
    pending_eyes = nullptr;
    if (!writer().push({root, std::move(record), std::move(eyes)}))
      log::warn("xr-dump", "Dump of frame {} dropped: the writer is behind", frame);
  } catch (const std::exception& e) { log::warn("xr-dump", "Dump failed: {}", e.what()); }
}
}
