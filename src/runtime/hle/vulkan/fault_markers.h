#pragma once
// GPU fault breadcrumbs: markers around every guest command buffer and every
// Venture pass recorded into one (texture decodes, BC7 encodes, depth
// conversions), read back after a device loss to name the command buffer and
// pass the GPU stopped in (fault_markers.cpp). The bookkeeping and the readout
// verdict are pure functions here, so tests drive them without a device.
#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace venture::vulkan {
namespace fault_markers {
enum class PassKind : std::uint8_t { decode, decode_image, bc7_encode, decoded_copy, depth_conversion };
// The guest image subresource a texture pass works on (image width 0: none).
struct Site {
  VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
  VkExtent3D image{};
  std::uint32_t mip = 0, layer = 0, slice = 0;
};
struct Pass {
  PassKind kind{};
  VkFormat source{}, output{};
  std::uint32_t width = 0, height = 0;  // texels; a depth conversion's word count is its width
  VkDeviceSize source_offset = 0, source_bytes = 0;  // where the pass reads in its source buffer, and its size
  std::uint32_t groups[3]{};
  Site site{};
};
// "ASTC 8x8 texture decode of a 2048x2048 linear image (mip 0, ...)".
std::string describe(const Pass& pass);
std::string format_name(VkFormat format);

// Each recording of a command buffer owns two marker words. TOP_OF_PIPE writes
// go to `started` (every earlier command has begun), BOTTOM_OF_PIPE writes to
// `completed` (every earlier command has finished). Values rise in recording
// order: 1 when it began, then two per Venture pass, then its end.
inline constexpr std::uint32_t began = 1;
constexpr std::uint32_t pass_began(std::size_t k) { return std::uint32_t(2 + 2 * k); }
constexpr std::uint32_t pass_ended(std::size_t k) { return std::uint32_t(3 + 2 * k); }
constexpr std::uint32_t ended(std::size_t passes) { return std::uint32_t(2 + 2 * passes); }
// Passes past this are not marked (their values would overflow the words).
inline constexpr std::size_t max_passes = std::size_t(1) << 30;

// Where a recording with `passes` Venture passes stood when its words read
// `started` and `completed`.
struct Position {
  enum State : std::uint8_t { not_started, in_commands, in_pass, completed, unknown } state = unknown;
  std::size_t pass = 0;            // in_pass: that pass; in_commands: passes completed before
  bool next_pass_started = false;  // in_commands: the next pass had begun issuing
};
Position locate(std::size_t passes, std::uint32_t started, std::uint32_t completed);

// Marker word pairs, handed out longest-released first so a freed command
// buffer's still-running execution cannot write into a new owner's words.
class Slots {
 public:
  explicit Slots(std::uint32_t count);
  std::optional<std::uint32_t> acquire(std::uint32_t recording);
  void release(std::uint32_t slot) { free_.push_back(slot); }
  // The recording that acquired the slot last.
  std::uint32_t owner(std::uint32_t slot) const { return slot < owners_.size() ? owners_[slot] : 0; }
  std::size_t available() const { return free_.size(); }

 private:
  std::vector<std::uint32_t> owners_;
  std::deque<std::uint32_t> free_;
};

// A guest command buffer as submitted, in its queue's submission order.
struct Submitted {
  std::uint64_t serial = 0;             // the queue's submission call count
  std::uint32_t batch = 0;              // VkSubmitInfo index in the call
  std::uint32_t index = 0, count = 0;   // command buffer index and count in that batch
  std::uint64_t command = 0;            // the guest's handle
  std::uint32_t recording = 0;          // 0: never marked
};
class History {
 public:
  static constexpr std::size_t depth = 256;
  void add(std::uint64_t queue, const Submitted& s) {
    auto& q = queues_[queue];
    q.push_back(s);
    if (q.size() > depth) q.pop_front();
  }
  const std::map<std::uint64_t, std::deque<Submitted>>& queues() const { return queues_; }

 private:
  std::map<std::uint64_t, std::deque<Submitted>> queues_;
};

// One recording at the loss. passes == nullptr: it was re-recorded or
// forgotten since, so that submission completed.
struct Readout {
  const std::vector<Pass>* passes = nullptr;
  std::uint32_t started = 0, completed = 0;
  bool readable = true;  // its words still hold its own markers
  bool simultaneous = false;
  std::vector<std::uint32_t> secondaries;  // recordings it executes
};
using Lookup = std::function<std::optional<Readout>(std::uint32_t recording)>;
struct QueueVerdict {
  std::vector<std::string> lines;
  std::string stopped;            // "inside Venture's ...", or empty when nothing was running
  std::string where;              // "submission 135, command buffer 1 of 2"
  bool venture = false;           // stopped inside a Venture pass
  std::size_t completed = 0, waiting = 0, running = 0;
};
QueueVerdict read_queue(std::string_view queue, const std::deque<Submitted>& history, const Lookup& lookup);
// The one-line verdict over all queues, for the log and the debug report.
std::string verdict(const std::vector<QueueVerdict>& queues);

// Readouts from NV checkpoints: `top` and `bottom` are the last checkpoints
// (recording << 32 | value) that reached the top and the bottom of the pipe.
// Submissions before the one holding `bottom` completed; those after the one
// holding `top` had not started.
std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> checkpoint_progress(
    const std::deque<Submitted>& history, std::uint64_t top, std::uint64_t bottom,
    const std::function<std::size_t(std::uint32_t recording)>& passes);
}  // namespace fault_markers

// The mechanism a host device's extensions allow: the driver's own
// VK_NV_device_diagnostic_checkpoints where offered, VK_AMD_buffer_marker
// (AMD, Mesa) otherwise, whose completion writes cost GPU time where both are
// offered.
// VENTURE_VULKAN_BREADCRUMBS=0 turns breadcrumbs off.
enum class FaultMarkers { none, buffer_marker, checkpoints };
FaultMarkers fault_markers_for(std::span<const VkExtensionProperties> extensions);
const char* fault_marker_extension(FaultMarkers kind);
void prepare_fault_markers(VkDevice device, FaultMarkers kind);
void release_fault_markers(VkDevice device);
// Guest (and texture layer) command buffer lifetime, bottom of the layer stack.
void fault_markers_begin(VkCommandBuffer command, const VkCommandBufferBeginInfo& info);
void fault_markers_end(VkCommandBuffer command);
void fault_markers_execute(VkCommandBuffer primary, std::uint32_t count, const VkCommandBuffer* secondaries);
void fault_markers_free(VkCommandBuffer command);
// Before the driver sees a guest submission on its native queue.
void fault_markers_submit(VkQueue queue, std::uint32_t count, const VkSubmitInfo* submits);
void fault_markers_submit(VkQueue queue, std::uint32_t count, const VkSubmitInfo2* submits);
void report_fault_markers(VkDevice device);
// Tests: the queues' verdicts as a report would log them.
std::vector<fault_markers::QueueVerdict> fault_marker_verdicts(VkDevice device);
FaultMarkers device_fault_markers(VkDevice device);
// Brackets one Venture pass recorded into a guest command buffer.
class FaultPass {
 public:
  FaultPass(VkCommandBuffer command, const fault_markers::Pass& pass);
  ~FaultPass();
  FaultPass(const FaultPass&) = delete;
  FaultPass& operator=(const FaultPass&) = delete;

 private:
  VkCommandBuffer command_{};
  std::uint32_t recording_ = 0;
  std::size_t index_ = 0;
};
}  // namespace venture::vulkan
