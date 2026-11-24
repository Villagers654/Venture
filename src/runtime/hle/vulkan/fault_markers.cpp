// GPU fault breadcrumbs: on by default where the driver keeps checkpoints for
// it (VK_NV_device_diagnostic_checkpoints); VK_AMD_buffer_marker words only
// with VENTURE_VULKAN_BREADCRUMBS=1, as they cost GPU time every frame.
//
// A guest command buffer writes a TOP_OF_PIPE marker when it begins and a
// BOTTOM_OF_PIPE marker at its end; each Venture pass recorded into it writes
// TOP_OF_PIPE and BOTTOM_OF_PIPE markers before it and a BOTTOM_OF_PIPE marker
// after it. With VK_NV_device_diagnostic_checkpoints, which the driver keeps
// for this purpose, each of those is one checkpoint whose pipeline progress the
// driver reports after a loss. With VENTURE_VULKAN_BREADCRUMBS=1 a driver
// without it gets VK_AMD_buffer_marker words in host memory instead; their
// BOTTOM_OF_PIPE writes made North Star's GPU frame 3% longer on an RTX 5090,
// which offers both (a tiler such as the Steam Frame's Adreno waits for idle at
// each one). After a
// device loss the words name the submission, command buffer and pass that had
// begun and not finished. Draws and dispatches are not hooked: they stay
// direct driver calls from the JIT (VENTURE_VULKAN_BREADCRUMBS=1 adds per-draw
// markers, breadcrumbs.cpp).
#include "fault_markers.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>

#include "breadcrumb_streams.h"
#include "common/environment.h"
#include "common/log.h"
#include "internal.h"
#include "vulkan.h"

namespace venture::vulkan {
namespace fault_markers {
std::string format_name(VkFormat format) {
  static constexpr std::uint32_t astc[][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
                                              {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
  const auto f = std::uint32_t(format);
  if (f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) {
    const auto i = (f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) / 2;
    return std::format("ASTC {}x{}{}", astc[i][0], astc[i][1], (f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) % 2 ? " sRGB" : "");
  }
  if (f >= VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK && f <= VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK) {
    const auto i = f - VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK;
    return std::format("ASTC {}x{} HDR", astc[i][0], astc[i][1]);
  }
  switch (format) {
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK: return "ETC2 RGB8";
    case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK: return "ETC2 RGB8 sRGB";
    case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK: return "ETC2 RGB8A1";
    case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK: return "ETC2 RGB8A1 sRGB";
    case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK: return "ETC2 RGBA8";
    case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK: return "ETC2 RGBA8 sRGB";
    case VK_FORMAT_EAC_R11_UNORM_BLOCK: return "EAC R11";
    case VK_FORMAT_EAC_R11_SNORM_BLOCK: return "EAC R11 signed";
    case VK_FORMAT_EAC_R11G11_UNORM_BLOCK: return "EAC RG11";
    case VK_FORMAT_EAC_R11G11_SNORM_BLOCK: return "EAC RG11 signed";
    case VK_FORMAT_BC7_UNORM_BLOCK: return "BC7";
    case VK_FORMAT_BC7_SRGB_BLOCK: return "BC7 sRGB";
    case VK_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case VK_FORMAT_R8G8B8A8_SRGB: return "RGBA8 sRGB";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "RGBA16F";
    case VK_FORMAT_R32G32_SFLOAT: return "RG32F";
    case VK_FORMAT_R32_SFLOAT: return "R32F";
    case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D24_UNORM_S8_UINT: return "24-bit depth";
    case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return "32-bit float depth";
    default: return std::format("format {}", int(format));
  }
}
namespace {
// "a 2048x2048 linear image", or empty for a pass without an image.
std::string image_text(const Site& s) {
  if (!s.image.width) return {};
  auto size = std::format("{}x{}", s.image.width, s.image.height);
  if (s.image.depth > 1) size += std::format("x{}", s.image.depth);
  return std::format("a {} {}image", size, s.tiling == VK_IMAGE_TILING_LINEAR ? "linear " : "");
}
std::string details(const Pass& p, bool groups = true) {
  std::string s = p.site.image.width ? std::format("mip {} at {}x{}, {} {}", p.site.mip, p.width, p.height,
                                                     p.site.image.depth > 1 ? "slice" : "layer",
                                                     p.site.image.depth > 1 ? p.site.slice : p.site.layer)
                                     : std::format("{}x{} texels", p.width, p.height);
  if (p.source_bytes) s += std::format("; reads from byte {} of a {}-byte source", p.source_offset, p.source_bytes);
  if (groups) s += std::format("; {}x{}x{} workgroups", p.groups[0], p.groups[1], p.groups[2]);
  return s;
}
}  // namespace
std::string describe(const Pass& p) {
  const auto image = image_text(p.site);
  switch (p.kind) {
    case PassKind::decode:
      return std::format("{} texture decode{}{} ({}; {} output)", format_name(p.source), image.empty() ? "" : " of ",
                         image, details(p), format_name(p.output));
    case PassKind::decode_image:
      return std::format("{} texture decode into a sparse {} companion{}{} ({})", format_name(p.source),
                         format_name(p.output), image.empty() ? "" : " of ", image, details(p));
    case PassKind::bc7_encode:
      return std::format("BC7 encode of a decoded mip{}{} ({})", image.empty() ? "" : " of ", image, details(p));
    case PassKind::decoded_copy:
      return std::format("copy of a decoded {} mip ({}) into {} ({})", format_name(p.source), format_name(p.output),
                         image.empty() ? "its image" : image, details(p, false));
    case PassKind::depth_conversion:
      return std::format("conversion of {} depth words from {} to {} ({}x{}x{} workgroups)", p.width,
                         format_name(p.source), format_name(p.output), p.groups[0], p.groups[1], p.groups[2]);
  }
  return "pass";
}

Position locate(std::size_t passes, std::uint32_t started, std::uint32_t completed) {
  if (passes >= max_passes) return {};
  const auto end = ended(passes);
  if (!started && !completed) return {Position::not_started};
  if (completed == end) return {Position::completed};
  if (completed > end || started > end || completed == began) return {};
  if (!completed) return {Position::in_commands, 0, passes && started >= pass_began(0)};
  if (completed % 2) {
    // pass_ended(k): pass k finished; the game's commands after it ran.
    const std::size_t done = (completed - 3) / 2 + 1;
    return {Position::in_commands, done, done < passes && started >= pass_began(done)};
  }
  // pass_began(k): everything before pass k finished; pass k had not.
  return {Position::in_pass, (completed - 2) / 2};
}

Slots::Slots(std::uint32_t count) : owners_(count) {
  for (std::uint32_t i = 0; i < count; ++i) free_.push_back(i);
}
std::optional<std::uint32_t> Slots::acquire(std::uint32_t recording) {
  if (free_.empty()) return std::nullopt;
  const auto slot = free_.front();
  free_.pop_front();
  owners_[slot] = recording;
  return slot;
}

namespace {
// Where a command buffer stood, as the verdict's sentence: "inside Venture's
// ..." or "in the game's commands ...".
std::string position_text(const Position& p, const std::vector<Pass>& passes) {
  if (p.state == Position::in_pass) return "inside Venture's " + describe(passes.at(p.pass));
  std::string s;
  if (passes.empty()) s = "in the game's commands (it holds no Venture pass)";
  else if (!p.pass) s = std::format("in the game's commands before its first Venture pass ({})", describe(passes[0]));
  else if (p.pass == passes.size())
    s = std::format("in the game's commands after its last Venture pass ({}) completed", describe(passes.back()));
  else
    s = std::format("in the game's commands after Venture pass {} of {} ({}) completed", p.pass, passes.size(),
                    describe(passes[p.pass - 1]));
  if (p.next_pass_started && p.pass < passes.size())
    s += std::format("; its next Venture pass ({}) had begun issuing", describe(passes[p.pass]));
  return s;
}
}  // namespace

QueueVerdict read_queue(std::string_view queue, const std::deque<Submitted>& history, const Lookup& lookup) {
  QueueVerdict v;
  constexpr std::size_t described = 4;  // in-flight command buffers named per queue
  std::size_t unmarked = 0, unreadable = 0;
  for (const auto& s : history) {
    const auto where = std::format("submission {}, command buffer {} of {} ({:#x})", s.serial, s.index + 1,
                                   s.count, s.command);
    if (!s.recording) { ++unmarked; continue; }
    const auto r = lookup(s.recording);
    if (!r || !r->passes) { ++v.completed; continue; }
    if (!r->readable) { ++unreadable; continue; }
    const auto p = locate(r->passes->size(), r->started, r->completed);
    if (p.state == Position::completed) { ++v.completed; continue; }
    if (p.state == Position::not_started) { ++v.waiting; continue; }
    ++v.running;
    if (p.state == Position::unknown) {
      if (v.running <= described)
        v.lines.push_back(std::format("{}: {}: marker words {} and {} do not fit its {} Venture passes", queue, where,
                                      r->started, r->completed, r->passes->size()));
      continue;
    }
    auto text = position_text(p, *r->passes);
    if (r->simultaneous) text += " (simultaneous use: its executions share these markers)";
    // A running primary whose own markers sit in the game's commands may be
    // inside a secondary it executes; name those that had begun.
    std::string secondary;
    if (p.state == Position::in_commands)
      for (const auto id : r->secondaries) {
        const auto sr = lookup(id);
        if (!sr || !sr->passes || !sr->readable) continue;
        const auto sp = locate(sr->passes->size(), sr->started, sr->completed);
        if (sp.state != Position::in_commands && sp.state != Position::in_pass) continue;
        if (secondary.empty()) secondary = position_text(sp, *sr->passes);
        if (sp.state == Position::in_pass) { secondary = position_text(sp, *sr->passes); break; }
      }
    if (v.running <= described) {
      v.lines.push_back(std::format("{}: {} had begun and not finished: {}", queue, where, text));
      if (!secondary.empty())
        v.lines.push_back(std::format("{}: {}: a secondary command buffer it executes stopped {}", queue, where,
                                      secondary));
    }
    // The oldest running command buffer is the queue's stop, unless a later
    // one stopped inside a Venture pass.
    const bool venture = p.state == Position::in_pass || secondary.starts_with("inside Venture's");
    if (v.stopped.empty() || (venture && !v.venture)) {
      v.stopped = secondary.starts_with("inside Venture's") ? secondary : text;
      v.where = where;
      v.venture = venture;
    }
  }
  if (!history.empty())
    v.lines.push_back(std::format("{}: of its last {} submitted command buffers {} completed, {} had begun and not "
                                  "finished, {} had not started{}{}",
                                  queue, history.size(), v.completed, v.running, v.waiting,
                                  unmarked ? std::format(", {} carried no markers", unmarked) : "",
                                  unreadable ? std::format(", {} markers were overwritten", unreadable) : ""));
  return v;
}

std::string verdict(const std::vector<QueueVerdict>& queues) {
  const QueueVerdict* chosen = nullptr;
  std::size_t waiting = 0, submitted = 0;
  for (const auto& q : queues) {
    waiting += q.waiting;
    submitted += q.completed + q.waiting + q.running;
    if (!q.stopped.empty() && (!chosen || (q.venture && !chosen->venture))) chosen = &q;
  }
  if (chosen) return std::format("the GPU stopped {} ({})", chosen->stopped, chosen->where);
  if (!submitted) return "no marked guest command buffer was submitted before the loss";
  return std::format("every guest command buffer that began had finished ({} had not started): the GPU stopped "
                     "outside the game's command buffers, in Venture's own submissions (private texture uploads, "
                     "swapchain and compositor copies), ANGLE or another process",
                     waiting);
}

std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> checkpoint_progress(
    const std::deque<Submitted>& history, std::uint64_t top, std::uint64_t bottom,
    const std::function<std::size_t(std::uint32_t recording)>& passes) {
  std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> result;
  auto newest = [&](std::uint64_t checkpoint) -> std::ptrdiff_t {
    const auto recording = std::uint32_t(checkpoint >> 32);
    if (!recording) return -1;
    for (auto i = std::ptrdiff_t(history.size()) - 1; i >= 0; --i)
      if (history[std::size_t(i)].recording == recording) return i;
    return -1;
  };
  const auto b = newest(bottom), t = newest(top);
  for (std::ptrdiff_t i = 0; i < std::ptrdiff_t(history.size()); ++i) {
    const auto id = history[std::size_t(i)].recording;
    if (!id) continue;
    const auto end = ended(passes(id));
    std::pair<std::uint32_t, std::uint32_t> words{0, 0};
    if (i < b) words = {end, end};
    else if (i == b) words = {i == t ? std::uint32_t(top) : end, std::uint32_t(bottom)};
    else if (i < t) words = {end, 0};  // later ones began, so it was issued in full
    else if (i == t) words = {std::uint32_t(top), 0};
    result[id] = words;
  }
  return result;
}
}  // namespace fault_markers

namespace {
using namespace fault_markers;
// Marker word pairs per device: 8192 * 8 bytes of system memory.
constexpr std::uint32_t slot_count = 8192;
constexpr std::uint64_t rate_sample = 5000;
// Recordings of freed command buffers kept for the report.
constexpr std::size_t kept_released = 256;
struct Device {
  FaultMarkers kind = FaultMarkers::none;
  VkBuffer buffer{};
  VkDeviceMemory memory{};
  std::uint32_t* words{};
  bool coherent = true;
  PFN_vkCmdWriteBufferMarkerAMD write{};
  PFN_vkCmdSetCheckpointNV checkpoint{};
  PFN_vkGetQueueCheckpointDataNV checkpoints{};
  PFN_vkFlushMappedMemoryRanges flush{};
  PFN_vkInvalidateMappedMemoryRanges invalidate{};
  Slots slots{slot_count};
  History history;
  std::map<VkQueue, std::uint64_t> serials;
  // Over the device's life, logged once at rate_sample command buffers and at
  // its release (the markers' cost).
  std::uint64_t marked_commands = 0, marked_passes = 0, writes = 0;
  std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
};
struct Recording {
  VkDevice device{};
  std::uint64_t command = 0;
  std::optional<std::uint32_t> slot;
  std::vector<Pass> passes;
  std::vector<std::uint32_t> secondaries;
  bool simultaneous = false;
  bool open_pass = false;
};
std::mutex mutex;
std::unordered_map<VkDevice, Device> devices;
std::unordered_map<VkCommandBuffer, std::uint32_t> current;
std::unordered_map<std::uint32_t, Recording> recordings;
std::deque<std::uint32_t> released;
std::uint32_t next_recording = 0;
// Lets command buffers of devices without markers skip the lock.
std::atomic<bool> any_device{false};

template <class F> F proc(VkDevice d, const char* name) { return reinterpret_cast<F>(raw_device_proc(d, name)); }
// Requires mutex.
void flush(VkDevice d, const Device& device) {
  if (device.coherent || !device.flush) return;
  VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, device.memory, 0, VK_WHOLE_SIZE};
  device.flush(d, 1, &range);
}
void clear(Device& device, std::uint32_t slot) {
  if (device.words) device.words[2 * slot] = device.words[2 * slot + 1] = 0;
}
// One marker event: buffer markers to the recording's words at the given
// stages, or one checkpoint (whose stages the driver reports itself).
void mark(Device& device, std::uint32_t id, const Recording& r, VkCommandBuffer command, bool top, bool bottom,
          std::uint32_t value) {
  if (device.write) {
    if (!r.slot) return;
    const VkDeviceSize offset = VkDeviceSize(*r.slot) * 8;
    if (top) device.write(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, device.buffer, offset, value);
    if (bottom) device.write(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, device.buffer, offset + 4, value);
    device.writes += unsigned(top) + unsigned(bottom);
  } else if (device.checkpoint) {
    device.checkpoint(command, reinterpret_cast<const void*>(std::uintptr_t(std::uint64_t(id) << 32 | value)));
    ++device.writes;
  }
}
// Requires mutex. The command buffer's current recording and its device.
std::pair<std::uint32_t, Recording*> recording_of(VkCommandBuffer command) {
  auto it = current.find(command);
  if (it == current.end()) return {0, nullptr};
  auto r = recordings.find(it->second);
  if (r == recordings.end()) return {0, nullptr};
  return {it->second, &r->second};
}
Device* device_of(const Recording& r) {
  auto it = devices.find(r.device);
  return it == devices.end() || it->second.kind == FaultMarkers::none ? nullptr : &it->second;
}
// Requires mutex. Forgets the recording; a re-recorded command buffer's earlier
// submissions completed (VUID-vkBeginCommandBuffer-commandBuffer-00049).
void drop(std::uint32_t id) {
  auto it = recordings.find(id);
  if (it == recordings.end()) return;
  if (it->second.slot)
    if (auto d = devices.find(it->second.device); d != devices.end()) d->second.slots.release(*it->second.slot);
  recordings.erase(it);
}
template <class Submit>
void submitted(VkQueue queue, std::uint32_t count, const Submit* submits) {
  if (!any_device.load(std::memory_order_relaxed) || !count) return;
  const auto binding = queue_binding(queue);
  if (!binding) return;
  std::lock_guard lock(mutex);
  auto d = devices.find(binding->device);
  if (d == devices.end() || d->second.kind == FaultMarkers::none) return;
  auto& device = d->second;
  const auto serial = ++device.serials[queue];
  const auto key = std::uint64_t(reinterpret_cast<std::uintptr_t>(queue));
  bool cleared = false;
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint32_t n;
    if constexpr (std::is_same_v<Submit, VkSubmitInfo>) n = submits[i].commandBufferCount;
    else n = submits[i].commandBufferInfoCount;
    for (std::uint32_t j = 0; j < n; ++j) {
      VkCommandBuffer command;
      if constexpr (std::is_same_v<Submit, VkSubmitInfo>) command = submits[i].pCommandBuffers[j];
      else command = submits[i].pCommandBufferInfos[j].commandBuffer;
      Submitted s{serial, i, j, n, h2g(command), 0};
      if (auto [id, r] = recording_of(command); r) {
        s.recording = id;
        // Not pending, so its previous execution finished: clear its words and
        // those of the secondaries it runs. A simultaneous-use buffer may still
        // run one, whose markers stay.
        auto reset = [&](std::uint32_t rid, const Recording& rec) {
          if (rec.simultaneous || !rec.slot || device.slots.owner(*rec.slot) != rid) return;
          clear(device, *rec.slot);
          cleared = true;
        };
        reset(id, *r);
        for (const auto sid : r->secondaries)
          if (auto sr = recordings.find(sid); sr != recordings.end()) reset(sid, sr->second);
      }
      device.history.add(key, s);
    }
  }
  if (cleared) flush(binding->device, device);
}
std::string queue_name(VkQueue queue) {
  const auto binding = queue_binding(queue);
  return std::format("queue {:#x} (family {}, index {})", h2g(queue), binding ? int(binding->family) : -1,
                     binding ? int(binding->index) : -1);
}
// Requires mutex.
std::vector<QueueVerdict> verdicts(VkDevice d, Device& device) {
  std::vector<QueueVerdict> result;
  if (device.words && !device.coherent && device.invalidate) {
    // Best effort: a lost device may refuse; the words are read anyway.
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, device.memory, 0, VK_WHOLE_SIZE};
    device.invalidate(d, 1, &range);
  }
  for (const auto& [key, history] : device.history.queues()) {
    const auto queue = reinterpret_cast<VkQueue>(std::uintptr_t(key));
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> checkpointed;
    if (device.checkpoints) {
      std::uint32_t n = 0;
      device.checkpoints(queue, &n, nullptr);
      std::vector<VkCheckpointDataNV> data(n, {VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
      if (n) device.checkpoints(queue, &n, data.data());
      std::uint64_t top = 0, bottom = 0;
      for (std::uint32_t i = 0; i < std::min<std::size_t>(n, data.size()); ++i) {
        const auto value = std::uint64_t(reinterpret_cast<std::uintptr_t>(data[i].pCheckpointMarker));
        if (data[i].stage & VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT) top = value;
        if (data[i].stage & VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT) bottom = value;
      }
      checkpointed = checkpoint_progress(history, top, bottom, [&](std::uint32_t id) {
        auto r = recordings.find(id);
        return r == recordings.end() ? std::size_t(0) : r->second.passes.size();
      });
    }
    result.push_back(read_queue(queue_name(queue), history, [&](std::uint32_t id) -> std::optional<Readout> {
      auto it = recordings.find(id);
      if (it == recordings.end()) return std::nullopt;
      const auto& r = it->second;
      Readout out{&r.passes};
      out.simultaneous = r.simultaneous;
      out.secondaries = r.secondaries;
      if (device.words) {
        out.readable = r.slot && device.slots.owner(*r.slot) == id;
        if (out.readable) {
          out.started = device.words[2 * *r.slot];
          out.completed = device.words[2 * *r.slot + 1];
        }
      } else if (auto c = checkpointed.find(id); c != checkpointed.end()) {
        std::tie(out.started, out.completed) = c->second;
      }
      return out;
    }));
  }
  return result;
}
}  // namespace

FaultMarkers fault_markers_for(std::span<const VkExtensionProperties> extensions) {
  const auto* v = environment::getenv("VENTURE_VULKAN_BREADCRUMBS");
  if (v && std::string_view(v) == "0") return FaultMarkers::none;
  auto has = [&](std::string_view name) {
    return std::ranges::any_of(extensions, [&](const auto& e) { return name == e.extensionName; });
  };
  if (has(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME)) return FaultMarkers::checkpoints;
  if (v && std::string_view(v) == "1" && has(VK_AMD_BUFFER_MARKER_EXTENSION_NAME)) return FaultMarkers::buffer_marker;
  return FaultMarkers::none;
}
const char* fault_marker_extension(FaultMarkers kind) {
  switch (kind) {
    case FaultMarkers::buffer_marker: return VK_AMD_BUFFER_MARKER_EXTENSION_NAME;
    case FaultMarkers::checkpoints: return VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME;
    default: return nullptr;
  }
}
void prepare_fault_markers(VkDevice d, FaultMarkers kind) {
  std::lock_guard lock(mutex);
  auto& device = devices[d];
  device.kind = FaultMarkers::none;
  if (kind == FaultMarkers::checkpoints) {
    device.checkpoint = proc<PFN_vkCmdSetCheckpointNV>(d, "vkCmdSetCheckpointNV");
    device.checkpoints = proc<PFN_vkGetQueueCheckpointDataNV>(d, "vkGetQueueCheckpointDataNV");
    if (device.checkpoint && device.checkpoints) device.kind = kind;
  } else if (kind == FaultMarkers::buffer_marker) {
    // System memory keeps what the GPU wrote across a reset (marker_memory_type).
    auto destroy = [&] {
      if (device.memory) proc<PFN_vkFreeMemory>(d, "vkFreeMemory")(d, device.memory, nullptr);
      if (device.buffer) proc<PFN_vkDestroyBuffer>(d, "vkDestroyBuffer")(d, device.buffer, nullptr);
      device.memory = VK_NULL_HANDLE;
      device.buffer = VK_NULL_HANDLE;
    };
    device.write = proc<PFN_vkCmdWriteBufferMarkerAMD>(d, "vkCmdWriteBufferMarkerAMD");
    device.flush = proc<PFN_vkFlushMappedMemoryRanges>(d, "vkFlushMappedMemoryRanges");
    device.invalidate = proc<PFN_vkInvalidateMappedMemoryRanges>(d, "vkInvalidateMappedMemoryRanges");
    VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    b.size = VkDeviceSize(slot_count) * 8;
    b.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkMemoryRequirements req{};
    VkPhysicalDeviceMemoryProperties props{};
    const auto physical = device_physical(d);
    reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        raw_physical_proc(physical, "vkGetPhysicalDeviceMemoryProperties"))(physical, &props);
    void* mapped{};
    if (device.write && proc<PFN_vkCreateBuffer>(d, "vkCreateBuffer")(d, &b, nullptr, &device.buffer) == VK_SUCCESS) {
      proc<PFN_vkGetBufferMemoryRequirements>(d, "vkGetBufferMemoryRequirements")(d, device.buffer, &req);
      const auto chosen = breadcrumb::marker_memory_type(props, req.memoryTypeBits);
      VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      a.allocationSize = req.size;
      a.memoryTypeIndex = chosen.type;
      device.coherent = chosen.coherent;
      if (chosen.type < props.memoryTypeCount &&
          proc<PFN_vkAllocateMemory>(d, "vkAllocateMemory")(d, &a, nullptr, &device.memory) == VK_SUCCESS &&
          proc<PFN_vkBindBufferMemory>(d, "vkBindBufferMemory")(d, device.buffer, device.memory, 0) == VK_SUCCESS &&
          proc<PFN_vkMapMemory>(d, "vkMapMemory")(d, device.memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
        device.words = static_cast<std::uint32_t*>(mapped);
        std::fill_n(device.words, std::size_t(slot_count) * 2, 0u);
        flush(d, device);
        device.kind = kind;
        note_device_memory(1);
        log::info("vulkan-fault", "GPU breadcrumbs: VK_AMD_buffer_marker into memory type {} (flags {:#x})",
                  a.memoryTypeIndex, props.memoryTypes[a.memoryTypeIndex].propertyFlags);
      }
    }
    if (device.kind == FaultMarkers::none) destroy();
  }
  if (device.kind == FaultMarkers::checkpoints)
    log::info("vulkan-fault", "GPU breadcrumbs: VK_NV_device_diagnostic_checkpoints");
  if (device.kind == FaultMarkers::none) {
    devices.erase(d);
    if (kind != FaultMarkers::none) log::warn("vulkan-fault", "GPU breadcrumbs unavailable: setting up {} failed",
                                              fault_marker_extension(kind));
  }
  any_device.store(!devices.empty(), std::memory_order_relaxed);
}
void release_fault_markers(VkDevice d) {
  std::lock_guard lock(mutex);
  auto it = devices.find(d);
  if (it == devices.end()) return;
  auto& device = it->second;
  if (device.kind != FaultMarkers::none)
    log::info("vulkan-fault", "GPU breadcrumbs: {} guest command buffers and {} Venture passes marked ({} marker writes)",
              device.marked_commands, device.marked_passes, device.writes);
  if (device.words) {
    proc<PFN_vkUnmapMemory>(d, "vkUnmapMemory")(d, device.memory);
    proc<PFN_vkFreeMemory>(d, "vkFreeMemory")(d, device.memory, nullptr);
    note_device_memory(-1);
  }
  if (device.buffer) proc<PFN_vkDestroyBuffer>(d, "vkDestroyBuffer")(d, device.buffer, nullptr);
  devices.erase(it);
  std::erase_if(recordings, [&](const auto& r) { return r.second.device == d; });
  std::erase_if(current, [&](const auto& c) { return !recordings.contains(c.second); });
  std::erase_if(released, [&](std::uint32_t id) { return !recordings.contains(id); });
  any_device.store(!devices.empty(), std::memory_order_relaxed);
}
void fault_markers_begin(VkCommandBuffer command, const VkCommandBufferBeginInfo& info) {
  if (!any_device.load(std::memory_order_relaxed)) return;
  const auto d = command_device(command);
  // Markers need a graphics, compute or transfer pool (VUID-vkCmdWriteBufferMarkerAMD-commandBuffer-cmdpool).
  if (!(command_queue_flags(command) & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)))
    return;
  std::lock_guard lock(mutex);
  auto dev = devices.find(d);
  if (dev == devices.end() || dev->second.kind == FaultMarkers::none) return;
  auto& device = dev->second;
  if (auto it = current.find(command); it != current.end()) drop(it->second);
  if (!++next_recording) ++next_recording;
  const auto id = next_recording;
  auto& r = recordings[id];
  r.device = d;
  r.command = h2g(command);
  r.simultaneous = info.flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
  current[command] = id;
  if (++device.marked_commands == rate_sample) {
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - device.created).count();
    log::info("vulkan-fault", "GPU breadcrumbs: {} guest command buffers, {} Venture passes and {} marker writes in "
              "the device's first {:.1f} s", device.marked_commands, device.marked_passes, device.writes, seconds);
  }
  if (device.words) {
    r.slot = device.slots.acquire(id);
    if (!r.slot) return;  // every word pair is held by a live command buffer
    clear(device, *r.slot);
    flush(d, device);
  }
  mark(device, id, r, command, true, false, began);
}
void fault_markers_end(VkCommandBuffer command) {
  if (!any_device.load(std::memory_order_relaxed)) return;
  std::lock_guard lock(mutex);
  auto [id, r] = recording_of(command);
  if (!r) return;
  if (auto* device = device_of(*r); device && r->passes.size() < max_passes)
    mark(*device, id, *r, command, false, true, ended(r->passes.size()));
}
void fault_markers_execute(VkCommandBuffer primary, std::uint32_t count, const VkCommandBuffer* secondaries) {
  if (!any_device.load(std::memory_order_relaxed)) return;
  std::lock_guard lock(mutex);
  auto [id, r] = recording_of(primary);
  if (!r) return;
  for (std::uint32_t i = 0; i < count; ++i)
    if (auto s = current.find(secondaries[i]); s != current.end()) r->secondaries.push_back(s->second);
}
void fault_markers_free(VkCommandBuffer command) {
  if (!any_device.load(std::memory_order_relaxed)) return;
  std::lock_guard lock(mutex);
  auto it = current.find(command);
  if (it == current.end()) return;
  // Quest titles may free a command buffer whose work still runs: keep its
  // record for the report, and its words until they are handed out again.
  released.push_back(it->second);
  current.erase(it);
  while (released.size() > kept_released) {
    drop(released.front());
    released.pop_front();
  }
}
void fault_markers_submit(VkQueue queue, std::uint32_t count, const VkSubmitInfo* submits) {
  submitted(queue, count, submits);
}
void fault_markers_submit(VkQueue queue, std::uint32_t count, const VkSubmitInfo2* submits) {
  submitted(queue, count, submits);
}
FaultPass::FaultPass(VkCommandBuffer command, const Pass& pass) : command_(command) {
  if (!any_device.load(std::memory_order_relaxed)) return;
  std::lock_guard lock(mutex);
  auto [id, r] = recording_of(command);
  if (!r || r->open_pass || r->passes.size() + 1 >= max_passes) return;
  auto* device = device_of(*r);
  if (!device) return;
  index_ = r->passes.size();
  r->passes.push_back(pass);
  r->open_pass = true;
  recording_ = id;
  ++device->marked_passes;
  // Bottom: the game's commands before the pass finished; top: the pass began.
  mark(*device, id, *r, command, true, true, pass_began(index_));
}
FaultPass::~FaultPass() {
  if (!recording_) return;
  std::lock_guard lock(mutex);
  auto [id, r] = recording_of(command_);
  if (!r || id != recording_) return;
  r->open_pass = false;
  if (auto* device = device_of(*r)) mark(*device, id, *r, command_, false, true, pass_ended(index_));
}
FaultMarkers device_fault_markers(VkDevice d) {
  std::lock_guard lock(mutex);
  auto it = devices.find(d);
  return it == devices.end() ? FaultMarkers::none : it->second.kind;
}
std::vector<QueueVerdict> fault_marker_verdicts(VkDevice d) {
  std::lock_guard lock(mutex);
  auto it = devices.find(d);
  return it == devices.end() ? std::vector<QueueVerdict>{} : verdicts(d, it->second);
}
std::string live_fault_markers(std::vector<std::string>& lines) {
  // The stall finder must not wait behind a recording thread: a few tries only.
  std::unique_lock lock(mutex, std::defer_lock);
  for (int i = 0; i < 20 && !lock.try_lock(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  if (!lock.owns_lock()) return "GPU breadcrumbs busy: not read";
  if (devices.empty()) return {};
  std::size_t completed = 0, waiting = 0, running = 0;
  std::string busy;
  bool checkpoints = false;
  for (auto& [d, device] : devices) {
    // VK_NV_device_diagnostic_checkpoints may be read only after a device loss.
    if (device.kind == FaultMarkers::checkpoints) { checkpoints = true; continue; }
    if (device.kind != FaultMarkers::buffer_marker) continue;
    for (const auto& q : verdicts(d, device)) {
      completed += q.completed;
      waiting += q.waiting;
      running += q.running;
      for (const auto& line : q.lines) lines.push_back(line);
      if (busy.empty() && q.running && !q.stopped.empty()) busy = std::format("{} had begun and not finished: {}", q.where, q.stopped);
    }
  }
  if (running) return "GPU busy: " + busy;
  if (waiting) return std::format("GPU: {} submitted guest command buffers had not started and none was running", waiting);
  if (completed) return std::format("GPU idle: all {} recent guest command buffers had completed", completed);
  return checkpoints ? "GPU state: breadcrumbs on this driver are read only after a device loss" : "";
}
void report_fault_markers(VkDevice d) {
  std::lock_guard lock(mutex);
  auto it = devices.find(d);
  if (it == devices.end()) {
    log::warn("vulkan-fault", "GPU breadcrumbs: none on this device (the driver offers neither {} nor {}, or "
              "VENTURE_VULKAN_BREADCRUMBS=0)", VK_AMD_BUFFER_MARKER_EXTENSION_NAME,
              VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
    return;
  }
  const auto queues = verdicts(d, it->second);
  for (const auto& q : queues)
    for (const auto& line : q.lines) log::warn("vulkan-fault", "breadcrumbs: {}", line);
  // debug_report's likely cause quotes this line (launcher/diagnosis.cpp).
  log::warn("vulkan-fault", "GPU fault verdict: {}", verdict(queues));
}
}  // namespace venture::vulkan
