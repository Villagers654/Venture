// Layouts of images that Venture itself reads at a point of the guest's queue
// timeline: VrApi eye images, which the guest renders and Venture copies to the
// XR runtime. VrApi on a Quest does not constrain the layout a guest leaves its
// images in (its compositor imports them outside Vulkan's layout rules), so a
// guest may submit an image in COLOR_ATTACHMENT_OPTIMAL as well as in
// SHADER_READ_ONLY_OPTIMAL. Venture's copy must name the layout the image is
// actually in. The guest's layout changes (barriers and render pass final
// layouts) are noted per command buffer as recorded and applied, in order,
// when a submission is accepted. Untracked images cost one atomic load, and
// a barrier batch naming none of the (few) tracked images takes no lock.
//
// OpenXR zero-copy eye images (hle/openxr/graphics.cpp) are tracked too: they
// alternate between the game and the host runtime, and a submission that
// changes one's layout or ends a render pass on it while it is not the game's
// marks its ImageOwnership violated. That is judged when the game calls
// vkQueueSubmit (guest_submitting_layouts), in its own call order: a deferred
// queue facade issues the submission to the driver later, possibly after the
// game released the image.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
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
std::unordered_map<VkImage, std::shared_ptr<ImageOwnership>> owners;
// Lock-free filter in front of `tracked`: every tracked image is in `hot`
// unless `hot_overflow` is set. Written under mutex; a slot is filled before
// the image enters `tracked`, so a reader never misses a tracked image.
constexpr std::size_t kHot = 32;
std::array<std::atomic<std::uint64_t>, kHot> hot{};
std::atomic<bool> hot_overflow{false};
std::uint64_t key(VkImage image) {
  std::uint64_t k = 0;
  std::memcpy(&k, &image, sizeof(image));
  return k;
}
bool maybe_tracked(VkImage image) {
  if (hot_overflow.load(std::memory_order_acquire)) return true;
  const auto k = key(image);
  if (!k) return false;
  for (const auto& h : hot)
    if (h.load(std::memory_order_acquire) == k) return true;
  return false;
}
// Caller holds mutex.
void add_hot(VkImage image) {
  const auto k = key(image);
  for (auto& h : hot)
    if (h.load(std::memory_order_relaxed) == k) return;
  for (auto& h : hot)
    if (!h.load(std::memory_order_relaxed)) {
      h.store(k, std::memory_order_release);
      return;
    }
  hot_overflow.store(true, std::memory_order_release);
}
// Caller holds mutex; `image` has left `tracked`.
void remove_hot(VkImage image) {
  const auto k = key(image);
  for (auto& h : hot)
    if (h.load(std::memory_order_relaxed) == k) h.store(0, std::memory_order_release);
  if (hot_overflow.load(std::memory_order_relaxed) && tracked.size() <= kHot) {
    for (auto& h : hot) h.store(0, std::memory_order_relaxed);
    std::size_t i = 0;
    for (const auto& [image, t] : tracked) hot[i++].store(key(image), std::memory_order_release);
    hot_overflow.store(false, std::memory_order_release);
  }
}

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
  if (std::none_of(barriers, barriers + count,
                   [](const Barrier& b) { return b.oldLayout != b.newLayout && maybe_tracked(b.image); }))
    return;
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
// Caller holds mutex.
void check_owners(VkCommandBuffer command) {
  auto it = notes.find(command);
  if (it == notes.end()) return;
  for (const auto& n : it->second)
    if (auto o = owners.find(n.image); o != owners.end() && !o->second->guest.load(std::memory_order_acquire)) {
      o->second->violation_layout.store(int(n.layout), std::memory_order_relaxed);
      o->second->violated.store(true, std::memory_order_release);
    }
}
}  // namespace

void track_layouts(VkImage image, std::uint32_t mips, std::uint32_t layers, VkImageLayout layout) {
  std::lock_guard lock(mutex);
  add_hot(image);
  auto& t = tracked[image];
  t.mips = std::max(1u, mips);
  t.layers = std::max(1u, layers);
  t.layouts.assign(std::size_t(t.mips) * t.layers, layout);
  tracked_count.store(tracked.size(), std::memory_order_release);
}
void untrack_layouts(VkImage image) {
  std::lock_guard lock(mutex);
  tracked.erase(image);
  owners.erase(image);
  remove_hot(image);
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
void guest_submitting_layouts(std::uint32_t count, const VkSubmitInfo* submits) {
  if (!tracked_count.load(std::memory_order_acquire) || !submits) return;
  std::lock_guard lock(mutex);
  if (notes.empty() || owners.empty()) return;
  for (std::uint32_t i = 0; i < count; ++i)
    for (std::uint32_t j = 0; submits[i].pCommandBuffers && j < submits[i].commandBufferCount; ++j)
      check_owners(submits[i].pCommandBuffers[j]);
}
void guest_submitting_layouts(std::uint32_t count, const VkSubmitInfo2* submits) {
  if (!tracked_count.load(std::memory_order_acquire) || !submits) return;
  std::lock_guard lock(mutex);
  if (notes.empty() || owners.empty()) return;
  for (std::uint32_t i = 0; i < count; ++i)
    for (std::uint32_t j = 0; submits[i].pCommandBufferInfos && j < submits[i].commandBufferInfoCount; ++j)
      check_owners(submits[i].pCommandBufferInfos[j].commandBuffer);
}
void watch_ownership(VkImage image, std::shared_ptr<ImageOwnership> owner) {
  std::lock_guard lock(mutex);
  if (owner) owners[image] = std::move(owner);
  else owners.erase(image);
}
std::vector<VkImageLayout> tracked_layouts(VkImage image) {
  std::lock_guard lock(mutex);
  auto it = tracked.find(image);
  return it == tracked.end() ? std::vector<VkImageLayout>{} : it->second.layouts;
}
}  // namespace venture::vulkan
