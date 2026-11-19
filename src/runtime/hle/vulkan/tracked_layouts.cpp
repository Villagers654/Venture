// Layouts of images that Venture itself reads at a point of the guest's queue
// timeline: VrApi eye images, which the guest renders and Venture copies to the
// XR runtime. VrApi on a Quest does not constrain the layout a guest leaves its
// images in (its compositor imports them outside Vulkan's layout rules), so a
// guest may submit an image in COLOR_ATTACHMENT_OPTIMAL as well as in
// SHADER_READ_ONLY_OPTIMAL. Venture's copy must name the layout the image is
// actually in. The guest's layout changes (barriers and render pass final
// layouts) are noted per command buffer as recorded and applied, in order,
// when a submission is accepted. Untracked images cost one atomic load.
#include <algorithm>
#include <atomic>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "internal.h"

namespace venture::vulkan {
namespace {
struct Tracked {
  std::uint32_t mips = 1, layers = 1;
  std::vector<VkImageLayout> layouts;  // mip-major: [mip * layers + layer]
};
struct Note {
  VkImage image;
  VkImageSubresourceRange range;
  VkImageLayout layout;
};
std::mutex mutex;
std::atomic<std::size_t> tracked_count{0};
std::unordered_map<VkImage, Tracked> tracked;
std::unordered_map<VkCommandBuffer, std::vector<Note>> notes;

void apply(Tracked& t, const VkImageSubresourceRange& r, VkImageLayout layout) {
  const auto mips = r.levelCount == VK_REMAINING_MIP_LEVELS ? t.mips - std::min(t.mips, r.baseMipLevel) : r.levelCount;
  const auto layers =
      r.layerCount == VK_REMAINING_ARRAY_LAYERS ? t.layers - std::min(t.layers, r.baseArrayLayer) : r.layerCount;
  for (std::uint32_t m = r.baseMipLevel; m < t.mips && m < r.baseMipLevel + mips; ++m)
    for (std::uint32_t l = r.baseArrayLayer; l < t.layers && l < r.baseArrayLayer + layers; ++l)
      t.layouts[std::size_t(m) * t.layers + l] = layout;
}
template <class Barrier>
void note_barriers(VkCommandBuffer command, std::uint32_t count, const Barrier* barriers) {
  if (!tracked_count.load(std::memory_order_acquire) || !barriers) return;
  std::lock_guard lock(mutex);
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto& b = barriers[i];
    // A barrier without a layout transition leaves the layout.
    if (b.oldLayout == b.newLayout || !tracked.contains(b.image)) continue;
    notes[command].push_back({b.image, b.subresourceRange, b.newLayout});
  }
}
void submit(VkCommandBuffer command) {
  auto it = notes.find(command);
  if (it == notes.end()) return;
  for (const auto& n : it->second)
    if (auto t = tracked.find(n.image); t != tracked.end()) apply(t->second, n.range, n.layout);
}
}  // namespace

void track_layouts(VkImage image, std::uint32_t mips, std::uint32_t layers, VkImageLayout layout) {
  std::lock_guard lock(mutex);
  auto& t = tracked[image];
  t.mips = std::max(1u, mips);
  t.layers = std::max(1u, layers);
  t.layouts.assign(std::size_t(t.mips) * t.layers, layout);
  tracked_count.store(tracked.size(), std::memory_order_release);
}
void untrack_layouts(VkImage image) {
  std::lock_guard lock(mutex);
  tracked.erase(image);
  tracked_count.store(tracked.size(), std::memory_order_release);
  for (auto it = notes.begin(); it != notes.end();) {
    std::erase_if(it->second, [&](const Note& n) { return n.image == image; });
    it = it->second.empty() ? notes.erase(it) : std::next(it);
  }
}
bool layouts_tracked(VkImage image) {
  if (!tracked_count.load(std::memory_order_acquire)) return false;
  std::lock_guard lock(mutex);
  return tracked.contains(image);
}
void note_layout(VkCommandBuffer command, VkImage image, const VkImageSubresourceRange& range, VkImageLayout layout) {
  if (!tracked_count.load(std::memory_order_acquire)) return;
  std::lock_guard lock(mutex);
  if (tracked.contains(image)) notes[command].push_back({image, range, layout});
}
void note_layouts(VkCommandBuffer command, std::uint32_t count, const VkImageMemoryBarrier* barriers) {
  note_barriers(command, count, barriers);
}
void note_layouts(VkCommandBuffer command, const VkDependencyInfo* info) {
  if (info) note_barriers(command, info->imageMemoryBarrierCount, info->pImageMemoryBarriers);
}
void note_executed_layouts(VkCommandBuffer primary, std::uint32_t count, const VkCommandBuffer* secondaries) {
  if (!tracked_count.load(std::memory_order_acquire) || !secondaries) return;
  std::lock_guard lock(mutex);
  for (std::uint32_t i = 0; i < count; ++i) {
    auto it = notes.find(secondaries[i]);
    if (it == notes.end()) continue;
    // Copied: the secondary may run again, here or in another primary.
    auto copy = it->second;
    auto& into = notes[primary];
    into.insert(into.end(), copy.begin(), copy.end());
  }
}
void forget_command_layouts(VkCommandBuffer command) {
  if (!tracked_count.load(std::memory_order_acquire)) return;
  std::lock_guard lock(mutex);
  notes.erase(command);
}
void submitted_layouts(std::uint32_t count, const VkSubmitInfo* submits) {
  if (!tracked_count.load(std::memory_order_acquire) || !submits) return;
  std::lock_guard lock(mutex);
  if (notes.empty()) return;
  for (std::uint32_t i = 0; i < count; ++i)
    for (std::uint32_t j = 0; submits[i].pCommandBuffers && j < submits[i].commandBufferCount; ++j)
      submit(submits[i].pCommandBuffers[j]);
}
void submitted_layouts(std::uint32_t count, const VkSubmitInfo2* submits) {
  if (!tracked_count.load(std::memory_order_acquire) || !submits) return;
  std::lock_guard lock(mutex);
  if (notes.empty()) return;
  for (std::uint32_t i = 0; i < count; ++i)
    for (std::uint32_t j = 0; submits[i].pCommandBufferInfos && j < submits[i].commandBufferInfoCount; ++j)
      submit(submits[i].pCommandBufferInfos[j].commandBuffer);
}
std::vector<VkImageLayout> tracked_layouts(VkImage image) {
  std::lock_guard lock(mutex);
  auto it = tracked.find(image);
  return it == tracked.end() ? std::vector<VkImageLayout>{} : it->second.layouts;
}
}  // namespace venture::vulkan
