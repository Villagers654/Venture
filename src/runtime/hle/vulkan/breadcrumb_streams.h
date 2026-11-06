#pragma once
// Breadcrumb readout logic without a device (breadcrumbs.cpp, tests).
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>

namespace venture::vulkan::breadcrumb {
// The marker buffer's memory type, or memory.memoryTypeCount when none fits.
// Markers must outlive a GPU reset: host memory that is not device-local
// (system RAM) keeps what the GPU wrote, VRAM behind a BAR may not. Coherent
// memory is preferred, cached among it; then cached non-coherent memory; then
// any host-visible coherent type (the previous choice). Protected and AMD
// device-coherent types need features breadcrumbs do not enable.
struct MarkerMemory {
  std::uint32_t type;
  bool coherent;
};
inline MarkerMemory marker_memory_type(const VkPhysicalDeviceMemoryProperties& memory, std::uint32_t allowed) {
  constexpr VkMemoryPropertyFlags excluded = VK_MEMORY_PROPERTY_PROTECTED_BIT |
      VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD | VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD;
  auto rank = [](VkMemoryPropertyFlags f) {
    const bool host = f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, coherent = f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
               cached = f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT, local = f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (!host) return 0;
    if (!local && coherent) return cached ? 5 : 4;
    if (!local && cached) return 3;
    return coherent ? 1 : 0;
  };
  MarkerMemory best{memory.memoryTypeCount, false};
  int best_rank = 0;
  for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
    const auto flags = memory.memoryTypes[i].propertyFlags;
    if (!(allowed & (1u << i)) || (flags & excluded)) continue;
    if (const int r = rank(flags); r > best_rank) {
      best_rank = r;
      best = {i, (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0};
    }
  }
  return best;
}

// One marker stream per queue. Every recorded command buffer owns a buffer
// slot its markers overwrite in recording order, and a queue runs its command
// buffers in submission order. The newest submitted command buffer with a
// written slot therefore holds the queue's last completed marker, however many
// command buffers are in flight on it or on other queues. Marker ids are
// recording order only, so they never order work across command buffers.
struct Submission {
  std::uint64_t command;              // the guest command buffer, for the report
  std::vector<std::uint32_t> slots;   // its own slot, then those of executed secondaries
  bool marked = true;                 // recorded at least one marker; others cannot show progress
};
struct QueueProgress {
  std::uint64_t queue;
  std::uint32_t marker = 0;   // last completed marker (0: none in the retained history)
  std::uint64_t command = 0;  // the command buffer that wrote it
  std::size_t later = 0;      // marked submissions after it that completed none
};
class Streams {
 public:
  static constexpr std::size_t history = 256;
  void submitted(std::uint64_t queue, Submission submission) {
    auto& q = queues_[queue];
    q.push_back(std::move(submission));
    if (q.size() > history) q.pop_front();
  }
  void forget(const std::function<bool(std::uint64_t queue)>& matches) {
    std::erase_if(queues_, [&](const auto& q) { return matches(q.first); });
  }
  // read(slot) returns the slot's current marker, 0 when none was written.
  std::vector<QueueProgress> progress(const std::function<std::uint32_t(std::uint32_t)>& read) const {
    std::vector<QueueProgress> result;
    for (const auto& [queue, list] : queues_) {
      QueueProgress p{queue};
      for (auto it = list.rbegin(); it != list.rend(); ++it) {
        if (!it->marked) continue;
        // A secondary's markers were recorded apart from its primary's; the
        // newest id among them is the best available estimate of the last.
        std::uint32_t last = 0;
        for (auto slot : it->slots) last = std::max(last, read(slot));
        if (last) { p.marker = last; p.command = it->command; break; }
        ++p.later;
      }
      result.push_back(p);
    }
    return result;
  }
 private:
  std::map<std::uint64_t, std::deque<Submission>> queues_;
};
}  // namespace venture::vulkan::breadcrumb
