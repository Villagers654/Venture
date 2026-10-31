#include "texture.h"
#include "texture_memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <thread>
#include <format>
#include <stdexcept>
#include <vector>

#include "chain.h"
#include "common/log.h"
#include "gfx/gpu_device.h"
#include "gfx/pipeline_trace.h"
#include "gfx/queue_sync.h"
#include "gfx/vulkan_memory.h"
#include "internal.h"
#include "external.h"
#include "timing.h"
#include "platform/thread_priority.h"

namespace venture::vulkan {
namespace {
#include "texture_decode.inc"
namespace float_array {
#include "texture_decode_float_array.inc"
}
namespace srgb_array {
#include "texture_decode_srgb_array.inc"
}
namespace float_volume {
#include "texture_decode_float_volume.inc"
}
namespace srgb_volume {
#include "texture_decode_srgb_volume.inc"
}
namespace unorm_array {
#include "texture_decode_unorm_array.inc"
}
namespace unorm_volume {
#include "texture_decode_unorm_volume.inc"
}
namespace depth {
#include "texture_depth.inc"
}
namespace bc7 {
#include "texture_bc7.inc"
}
template <class F>
F vk(VkDevice device, const char* name) {
  auto f = reinterpret_cast<F>(raw_device_proc(device, name));
  if (!f) throw std::runtime_error(std::string("missing Vulkan device command ") + name);
  return f;
}
template <class F>
F phys(VkPhysicalDevice physical, const char* name) {
  return reinterpret_cast<F>(raw_physical_proc(physical, name));
}
struct TextureFailure : std::runtime_error {
  VkResult result;
  explicit TextureFailure(VkResult result)
      : std::runtime_error("Vulkan texture operation failed: " + std::to_string(result)), result(result) {}
};
void checked(VkResult result) {
  if (result != VK_SUCCESS) throw TextureFailure(result);
}
std::uint32_t ceil_div(std::uint32_t a, std::uint32_t b) { return a / b + (a % b != 0); }
std::atomic<bool> emulation_forced{false};
bool emulated(VkPhysicalDevice physical, VkFormat format) {
  if (!texture_format(format)) return false;
  if (emulation_forced.load(std::memory_order_relaxed)) return true;
  VkFormatProperties properties{};
  phys<PFN_vkGetPhysicalDeviceFormatProperties>(physical, "vkGetPhysicalDeviceFormatProperties")(
      physical, format, &properties);
  return !(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
}
// VUID-VkImageCreateInfo-samples-02258: samples must be one of the counts that
// vkGetPhysicalDeviceImageFormatProperties reports for the image's parameters.
// Engines tuned for Adreno request 2x unconditionally; SwiftShader supports only
// 1x and 4x, accepts the image, and its draws then divide by a zero sample count
// on a host worker thread. Detected invalid usage fails with VALIDATION_FAILED.
bool supported_sample_count(VkPhysicalDevice physical, const VkImageCreateInfo& info) {
  if (info.samples == VK_SAMPLE_COUNT_1_BIT ||
      (info.tiling != VK_IMAGE_TILING_OPTIMAL && info.tiling != VK_IMAGE_TILING_LINEAR))
    return true;
  auto query = phys<PFN_vkGetPhysicalDeviceImageFormatProperties>(physical, "vkGetPhysicalDeviceImageFormatProperties");
  VkImageFormatProperties properties{};
  // Other unsupported parameters remain the host's to report.
  if (!query || query(physical, info.format, info.imageType, info.tiling, info.usage, info.flags, &properties) !=
                    VK_SUCCESS)
    return true;
  if (properties.sampleCounts & info.samples) return true;
  log::error("vulkan", "vkCreateImage: {} samples unsupported for format {} usage {:#x} (supported {:#x})",
             unsigned(info.samples), int(info.format), info.usage, properties.sampleCounts);
  return false;
}

struct Buffer {
  VkDevice device{};
  VkBuffer buffer{};
  VkDeviceMemory memory{};
  VkDeviceSize size{};
  VkDeviceSize allocation_size{};
  VkMemoryPropertyFlags properties{};
  void* mapped{};
  PFN_vkUnmapMemory unmap{};
  // Forced guest exit can leave these owners alive until static destruction,
  // after the HLE dispatch registry is gone. Resolve cleanup while the device
  // is registered; normal vkDestroyDevice releases all texture owners first.
  PFN_vkDestroyBuffer destroy_buffer{};
  PFN_vkFreeMemory free_memory{};
  ~Buffer() {
    if (mapped) unmap(device, memory);
    if (buffer) destroy_buffer(device, buffer, nullptr);
    if (memory) free_memory(device, memory, nullptr);
  }
};
// Memory for the layer's private texture resources: the lowest-index allowed
// DEVICE_LOCAL type, preferring one the host cannot map (on discrete GPUs a
// host-visible device-local type is the small BAR window), else the lowest
// allowed type. Types that need features the layer never enables are skipped.
VkResult allocate(VkDevice device, const VkMemoryRequirements& req, VkDeviceMemory* out) {
  VkPhysicalDeviceMemoryProperties memory{};
  auto physical = device_physical(device);
  phys<PFN_vkGetPhysicalDeviceMemoryProperties>(physical, "vkGetPhysicalDeviceMemoryProperties")(physical,
                                                                                                 &memory);
  const auto type = gfx::gpu_only_memory_type(memory, req.memoryTypeBits);
  if (type == memory.memoryTypeCount) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = type;
  const auto allocate_memory = vk<PFN_vkAllocateMemory>(device, "vkAllocateMemory");
  auto result = allocate_memory(device, &ai, nullptr, out);
  if (result != VK_ERROR_OUT_OF_DEVICE_MEMORY) return result;
  // VRAM is full (an 8 GB card in a large level): keep the game running from system memory.
  const auto spill = gfx::spill_memory_type(memory, req.memoryTypeBits);
  if (spill == memory.memoryTypeCount || spill == type) return result;
  ai.memoryTypeIndex = spill;
  result = allocate_memory(device, &ai, nullptr, out);
  static std::atomic<bool> reported{false};
  if (result == VK_SUCCESS && !reported.exchange(true))
    log::warn("vulkan", "GPU memory is full; decoded textures now also use system memory (slower)");
  return result;
}
VkResult make_buffer(VkDevice device, VkDeviceSize size, const VkImageCreateInfo* image_info,
                     std::shared_ptr<Buffer>& out, bool cpu_written = false, bool require_cached = false, bool mapped = false) {
  auto b = std::make_shared<Buffer>();
  b->device = device;
  b->size = size;
  b->destroy_buffer = vk<PFN_vkDestroyBuffer>(device, "vkDestroyBuffer");
  b->free_memory = vk<PFN_vkFreeMemory>(device, "vkFreeMemory");
  b->unmap = vk<PFN_vkUnmapMemory>(device, "vkUnmapMemory");
  VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  ci.size = size;
  ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
             VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (image_info && image_info->sharingMode == VK_SHARING_MODE_CONCURRENT) {
    ci.sharingMode = image_info->sharingMode;
    ci.queueFamilyIndexCount = image_info->queueFamilyIndexCount;
    ci.pQueueFamilyIndices = image_info->pQueueFamilyIndices;
  }
  if (auto r = vk<PFN_vkCreateBuffer>(device, "vkCreateBuffer")(device, &ci, nullptr, &b->buffer); r != VK_SUCCESS)
    return r;
  VkMemoryRequirements req{};
  vk<PFN_vkGetBufferMemoryRequirements>(device, "vkGetBufferMemoryRequirements")(device, b->buffer, &req);
  b->allocation_size = req.size;
  if (cpu_written) {
    VkPhysicalDeviceMemoryProperties properties{};
    phys<PFN_vkGetPhysicalDeviceMemoryProperties>(device_physical(device), "vkGetPhysicalDeviceMemoryProperties")(
        device_physical(device), &properties);
    std::uint32_t tried = 0;
    VkResult result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    for (auto wanted : std::array<VkMemoryPropertyFlags, 3>{VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                           VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)}) {
      if (!mapped && !(wanted & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) continue;
      if (require_cached && wanted != (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) continue;
      for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if (!(req.memoryTypeBits & (1u << i)) || (tried & (1u << i)) ||
            (flags & (VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD | VK_MEMORY_PROPERTY_PROTECTED_BIT |
                      VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)) || (flags & wanted) != wanted) continue;
        tried |= 1u << i;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size; ai.memoryTypeIndex = i;
        VkDeviceMemory memory{};
        result = vk<PFN_vkAllocateMemory>(device, "vkAllocateMemory")(device, &ai, nullptr, &memory);
        if (result == VK_ERROR_DEVICE_LOST) return result;
        if (result != VK_SUCCESS) continue;
        b->memory = memory;
        if (mapped) {
          void* pointer{};
          result = vk<PFN_vkMapMemory>(device, "vkMapMemory")(
              device, b->memory, 0, VK_WHOLE_SIZE, 0, &pointer);
          if (result != VK_SUCCESS) {
            b->free_memory(device, b->memory, nullptr); b->memory = {};
            if (result == VK_ERROR_DEVICE_LOST) return result;
            continue;
          }
          b->mapped = pointer;
        }
        b->properties = flags;
        break;
      }
      if (result == VK_SUCCESS) break;
    }
    if (result != VK_SUCCESS) return result;
  } else if (auto r = allocate(device, req, &b->memory); r != VK_SUCCESS) return r;
  if (auto r = vk<PFN_vkBindBufferMemory>(device, "vkBindBufferMemory")(device, b->buffer, b->memory, 0);
      r != VK_SUCCESS)
    return r;
  out = std::move(b);
  return VK_SUCCESS;
}
// Scratch for commands that cannot return an error.
std::shared_ptr<Buffer> buffer(VkDevice device, VkDeviceSize size) {
  std::shared_ptr<Buffer> b;
  checked(make_buffer(device, size, nullptr, b));
  return b;
}
struct Pipeline {
  VkDevice device{};
  VkDescriptorSetLayout descriptors{};
  VkPipelineLayout layout{};
  VkPipeline pipeline{};
  PFN_vkDestroyPipeline destroy_pipeline{};
  PFN_vkDestroyPipelineLayout destroy_layout{};
  PFN_vkDestroyDescriptorSetLayout destroy_descriptors{};
  ~Pipeline() {
    if (pipeline) destroy_pipeline(device, pipeline, nullptr);
    if (layout) destroy_layout(device, layout, nullptr);
    if (descriptors) destroy_descriptors(device, descriptors, nullptr);
  }
};
// Private host image for TextureStorage::planes[1..].
struct Companion {
  VkDevice device{};
  VkImage image{};
  VkDeviceMemory memory{};
  std::shared_ptr<void> decoded_allocation;
  PFN_vkDestroyImage destroy_image{};
  PFN_vkFreeMemory free_memory{};
  PFN_vkDestroyImageView destroy_view{};
  bool lazy = false;
  bool active = false;
  VkFormat storage_format{};
  VkMemoryRequirements requirements{};
  VkSparseImageMemoryRequirements sparse{};
  std::vector<VkImageView> storage_views;
  std::shared_ptr<Buffer> activation;
  ~Companion() {
    for (auto view : storage_views) if (view) destroy_view(device, view, nullptr);
    if (image) destroy_image(device, image, nullptr);
    if (memory) free_memory(device, memory, nullptr);
  }
};
struct TextureProducer {
  VkDevice device{};
  VkFence fence{};
  VkSemaphore semaphore{};
  bool consumed = false;
  bool fence_submitted = false;
  PFN_vkDestroyFence destroy_fence{};
  PFN_vkDestroySemaphore destroy_semaphore{};
  ~TextureProducer() {
    if (fence) destroy_fence(device, fence, nullptr);
    if (semaphore) destroy_semaphore(device, semaphore, nullptr);
  }
};
// Staging remains accessible while a retirement is queued. The gate protects
// one 4 KiB CPU read at a time on the worker, never its allocation or completion.
// Foreground restoration also uses the gate to serialize accesses to this source.
// Cancellation waits for that read outside texture_lock before any GPU write.
struct RetainedSource {
  std::shared_ptr<Buffer> staging;
  std::unique_ptr<std::byte[]> bytes;
  std::mutex gate;
  std::atomic<bool> cancelled{false};
};
struct Image {
  VkDevice device{};
  VkImage image{};
  std::shared_ptr<void> decoded_allocation;
  std::mutex binding_mutex;
  VkMemoryRequirements guest_requirements{};
  bool memory_bound = false;
  TextureFormat format;  // the guest's create format: block geometry for copies
  TextureStorage storage;
  VkImageCreateInfo info{};
  std::shared_ptr<Buffer> shadow;
  VkDeviceSize source_size{};
  std::vector<std::uint32_t> source_families;
  bool pageable_source = false;
  std::chrono::steady_clock::time_point source_used{};
  std::shared_ptr<RetainedSource> retained;
  std::vector<std::unique_ptr<Companion>> companions;
  std::vector<VkDeviceSize> mip_offsets;
  VkDeviceSize layer_stride{};
  bool shadow_concurrent = false;
  std::set<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> uploaded;
  std::map<VkQueue, std::shared_ptr<TextureProducer>> producers;
  VkImage plane(std::size_t index) const { return index ? companions.at(index - 1)->image : image; }
};
struct Commands {
  VkResult recording_error = VK_SUCCESS; // Sticky until begin/reset, reported by vkEndCommandBuffer.
  std::vector<std::shared_ptr<void>> resources;
  std::pair<VkDevice, VkCommandPool> decode_pool{};
  std::vector<std::shared_ptr<Buffer>> decoded;
  bool reuse_decoded = true;
  std::vector<std::shared_ptr<Buffer>> decoded_planes;
  // Decode binds only compute state. Replay application compute state in order
  // (descriptor layout compatibility and push-constant layout history matter).
  std::vector<std::function<void()>> compute_state;
  struct Upload { std::shared_ptr<Image> image; std::uint32_t mip, layer, z; };
  std::vector<Upload> uploads;
  std::vector<VkCommandBuffer> secondary;
  bool may_wait = false;
};
std::recursive_mutex texture_lock;
struct LazyTextureDevice {
  VkQueue queue{};
  PFN_vkQueueWaitIdle wait_idle{};
  std::uint32_t family{};
  VkPhysicalDeviceFeatures features{};
  std::map<VkQueue, bool> finite_queues;
  std::vector<std::shared_ptr<TextureProducer>> pending;
};
std::map<VkDevice, LazyTextureDevice> lazy_devices;
std::map<std::pair<VkDevice, VkImageView>, std::weak_ptr<Image>> companion_views;
VkResult materialize_companion(const std::shared_ptr<Image>& image, std::size_t plane);
std::map<VkDevice, std::shared_future<std::shared_ptr<Pipeline>>> pipelines;
std::map<std::pair<VkDevice, unsigned>, std::shared_future<std::shared_ptr<Pipeline>>> image_pipelines;
struct SourceWorker {
  std::shared_future<void> pending;
  std::chrono::steady_clock::time_point next_scan{};
  VkImage cursor{};
  std::size_t scanned{};
};
std::map<VkDevice, SourceWorker> source_workers;
std::function<void()> source_worker_hook, source_copy_hook;
std::atomic<bool> pageable_sources_forced{false}, have_pageable_sources{false};
bool pageable_sources_enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("VENTURE_TEXTURE_PAGEABLE_SHADOWS");
    return !value || std::strcmp(value, "0") != 0;
  }();
  return enabled || pageable_sources_forced.load(std::memory_order_relaxed);
}
void finish_pipeline_workers() {
  std::vector<std::shared_future<std::shared_ptr<Pipeline>>> pending;
  std::vector<std::shared_future<void>> sources;
  {
    std::lock_guard lock(texture_lock);
    for (const auto& [device, future] : pipelines) pending.push_back(future);
    for (const auto& [key, future] : image_pipelines) pending.push_back(future);
    for (const auto& [device, worker] : source_workers) sources.push_back(worker.pending);
  }
  for (auto& future : pending) if (future.valid()) future.wait();
  for (auto& future : sources) if (future.valid()) future.wait();
}
void register_pipeline_exit() {
  // Dynamic registration runs before static dispatch maps are destroyed. An
  // app can call exit with a live device while its worker still resolves Vulkan.
  static const bool registered = std::atexit(finish_pipeline_workers) == 0;
  if (!registered) throw std::runtime_error("Cannot register decoder worker shutdown");
}
std::map<VkImage, std::shared_ptr<Image>> images;
std::map<VkCommandBuffer, Commands> command_state;
// Guest D24 images the Chain created with 32-bit float depth (host_depth_format).
std::map<VkImage, VkDevice> depth_aliases;
// Reset returns scratch allocations to the parent pool, including when the
// guest requests RELEASE_RESOURCES (VkCommandBufferResetFlagBits). Reuse only
// within that pool: exclusive buffers keep their queue-family ownership.
std::map<std::pair<VkDevice, VkCommandPool>, std::vector<std::shared_ptr<Buffer>>> scratch_pools;
struct ScratchBudget { VkDeviceSize bytes{}; std::size_t count{}; };
std::map<VkDevice, ScratchBudget> scratch_budgets;
constexpr VkDeviceSize scratch_byte_limit = 64 * 1024 * 1024;
constexpr std::size_t scratch_count_limit = 64;

std::shared_ptr<Buffer> decoded_buffer(VkDevice device, VkCommandBuffer cmd, std::size_t plane,
                                       VkDeviceSize size) {
  const auto key = std::pair{device, command_pool(cmd)};
  std::shared_ptr<Buffer> result;
  {
    std::lock_guard lock(texture_lock);
    auto& state = command_state[cmd];
    if (state.reuse_decoded && plane < state.decoded_planes.size()) {
      const auto& previous = state.decoded_planes[plane];
      if (previous && previous->size >= size) return previous;
      // A larger decode needs new storage: recorded descriptors still refer
      // to the old buffer. Allocate only the requested size, as before.
    }
    auto& idle = scratch_pools[key];
    auto best = idle.end();
    for (auto it = idle.begin(); it != idle.end(); ++it)
      if ((*it)->size >= size && (best == idle.end() || (*it)->size < (*best)->size)) best = it;
    if (best != idle.end()) {
      result = std::move(*best);
      idle.erase(best);
      auto& budget = scratch_budgets.at(device);
      budget.bytes -= result->allocation_size;
      --budget.count;
    }
  }
  if (!result) result = buffer(device, size);
  std::lock_guard lock(texture_lock);
  auto& state = command_state[cmd];
  state.decode_pool = key;
  state.decoded.push_back(result);
  if (state.reuse_decoded) {
    if (state.decoded_planes.size() <= plane) state.decoded_planes.resize(plane + 1);
    state.decoded_planes[plane] = result;
  }
  return result;
}
struct DescriptorTemplate {
  std::vector<VkDescriptorUpdateTemplateEntry> entries;
  VkPipelineBindPoint point{};
};
std::map<VkDescriptorUpdateTemplate, DescriptorTemplate> descriptor_templates;

std::shared_ptr<Image> image(VkImage handle) {
  std::lock_guard lock(texture_lock);
  auto it = images.find(handle);
  return it == images.end() ? nullptr : it->second;
}
// A rare restore can exhaust every mappable type while device-local memory
// remains available. Use the existing private queue for one-time initialization;
// putting update commands in the guest command buffer would replay stale bytes.
VkResult restore_device_source(const Image& img, const VkImageCreateInfo& sharing,
                               const std::byte* bytes, std::shared_ptr<Buffer>& out) {
  VkQueue queue{};
  std::uint32_t family{};
  {
    std::lock_guard lock(texture_lock);
    auto config = lazy_devices.find(img.device);
    if (config == lazy_devices.end()) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    queue = config->second.queue; family = config->second.family;
  }
  const auto d = img.device;
  std::shared_ptr<Buffer> next;
  struct Upload {
    VkDevice d; VkQueue queue;
    VkCommandPool pool{}; VkFence fence{};
    bool submitted = false, complete = false;
    ~Upload() {
      if (submitted && !complete) {
        auto mutex = gfx::queue_mutex(queue);
        std::lock_guard lock(*mutex);
        vk<PFN_vkQueueWaitIdle>(d, "vkQueueWaitIdle")(queue);
      }
      if (pool) vk<PFN_vkDestroyCommandPool>(d, "vkDestroyCommandPool")(d, pool, nullptr);
      if (fence) vk<PFN_vkDestroyFence>(d, "vkDestroyFence")(d, fence, nullptr);
    }
  } upload{d, queue};
  auto result = make_buffer(d, img.source_size, &sharing, next);
  if (result != VK_SUCCESS) return result;
  VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.queueFamilyIndex = family;
  if (auto r = vk<PFN_vkCreateCommandPool>(d, "vkCreateCommandPool")(d, &pi, nullptr, &upload.pool);
      r != VK_SUCCESS) return r;
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = upload.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
  VkCommandBuffer cmd{};
  if (auto r = vk<PFN_vkAllocateCommandBuffers>(d, "vkAllocateCommandBuffers")(d, &ai, &cmd);
      r != VK_SUCCESS) return r;
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (auto r = vk<PFN_vkBeginCommandBuffer>(d, "vkBeginCommandBuffer")(cmd, &bi); r != VK_SUCCESS) return r;
  for (VkDeviceSize offset = 0; offset < next->size; offset += 65536)
    vk<PFN_vkCmdUpdateBuffer>(d, "vkCmdUpdateBuffer")(cmd, next->buffer, offset,
        std::min<VkDeviceSize>(65536, next->size - offset), bytes + offset);
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  vk<PFN_vkCmdPipelineBarrier>(d, "vkCmdPipelineBarrier")(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
  if (auto r = vk<PFN_vkEndCommandBuffer>(d, "vkEndCommandBuffer")(cmd); r != VK_SUCCESS) return r;
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (auto r = vk<PFN_vkCreateFence>(d, "vkCreateFence")(d, &fi, nullptr, &upload.fence);
      r != VK_SUCCESS) return r;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
  {
    auto mutex = gfx::queue_mutex(queue);
    std::lock_guard lock(*mutex);
    result = vk<PFN_vkQueueSubmit>(d, "vkQueueSubmit")(queue, 1, &submit, upload.fence);
    upload.submitted = result == VK_SUCCESS;
  }
  if (result != VK_SUCCESS) return result;
  result = vk<PFN_vkWaitForFences>(d, "vkWaitForFences")(d, 1, &upload.fence, VK_TRUE, UINT64_MAX);
  if (result != VK_SUCCESS) return result;
  upload.complete = true; out = std::move(next);
  return VK_SUCCESS;
}
// Recorded commands retain the buffer itself until reset/free, including
// executable commands that may replay. No retirement future is awaited here.
std::shared_ptr<Buffer> source_buffer(const std::shared_ptr<Image>& img, VkCommandBuffer cmd = {}) {
  std::unique_lock lock(texture_lock);
  if (cmd) {
    auto state = command_state.find(cmd);
    if (state != command_state.end() && state->second.recording_error != VK_SUCCESS) return {};
  }
  auto retained = img->retained;
  auto staging = img->shadow;
  if (retained) {
    retained->cancelled.store(true, std::memory_order_release);
    lock.unlock();
    // An already-running read can finish one 4 KiB chunk. Allocation and all
    // other textures in the batch never hold this source's guard.
    std::unique_lock gate(retained->gate);
    lock.lock();
    if (img->shadow) staging = img->shadow;
    if (!staging) {
      VkImageCreateInfo sharing{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      if (!img->source_families.empty()) {
        sharing.sharingMode = VK_SHARING_MODE_CONCURRENT;
        sharing.queueFamilyIndexCount = std::uint32_t(img->source_families.size());
        sharing.pQueueFamilyIndices = img->source_families.data();
      }
      lock.unlock();
      // Keep retained bytes intact on every allocation, mapping or upload failure.
      auto result = make_buffer(img->device, img->source_size, &sharing, staging, true, false, true);
      if (result == VK_SUCCESS) {
        std::memcpy(staging->mapped, retained->bytes.get(), std::size_t(img->source_size));
        if (!(staging->properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
          VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
          range.memory = staging->memory; range.size = VK_WHOLE_SIZE;
          result = vk<PFN_vkFlushMappedMemoryRanges>(img->device, "vkFlushMappedMemoryRanges")(
              img->device, 1, &range);
        }
      }
      if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) {
        staging.reset();
        result = restore_device_source(*img, sharing, retained->bytes.get(), staging);
      }
      if (result != VK_SUCCESS) {
        if (!cmd) throw TextureFailure(result); // Result-returning view creation handles this directly.
        lock.lock();
        auto& error = command_state[cmd].recording_error;
        if (error == VK_SUCCESS) error = result;
        return {}; // Retained exact bytes survive; no dependent command is recorded.
      }
      lock.lock();
      img->shadow = staging;
      // An uncached/noncoherent/device-local fallback stays resident. Repeated
      // access must not repeatedly fault or read slow uncached memory on the CPU.
      img->pageable_source = (staging->properties & (VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    if (img->retained == retained) img->retained.reset();
  }
  img->source_used = std::chrono::steady_clock::now();
  if (cmd) command_state[cmd].resources.push_back(staging);
  lock.unlock(); // Releasing a restored CPU copy must not hold the global lock.
  return staging;
}
void retire_sources(VkDevice device, bool force_idle) {
  if (!have_pageable_sources.load(std::memory_order_relaxed)) return;
  std::lock_guard lock(texture_lock);
  auto& worker = source_workers[device];
  const auto now = std::chrono::steady_clock::now();
  if (worker.pending.valid() && worker.pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    return;
  if (!force_idle && now < worker.next_scan) return;
  worker.next_scan = now + std::chrono::milliseconds(250);
  worker.scanned = 0;
  struct Task { std::weak_ptr<Image> image; std::shared_ptr<RetainedSource> retained; };
  std::vector<Task> batch;
  VkDeviceSize bytes = 0;
  try {
    auto it = images.upper_bound(worker.cursor);
    const auto count = std::min<std::size_t>(64, images.size());
    for (std::size_t n = 0; n < count; ++n) {
      if (it == images.end()) it = images.begin();
      auto& [handle, img] = *it++;
      worker.cursor = handle; ++worker.scanned;
      if (img->device != device || !img->pageable_source || img->retained || !img->shadow ||
          img->shadow.use_count() != 1 ||
          (!force_idle && now - img->source_used < std::chrono::seconds(5))) continue;
      auto retained = std::make_shared<RetainedSource>();
      batch.push_back({img, retained});
      retained->staging = img->shadow;
      img->retained = retained;
      bytes += img->source_size;
      if (bytes >= 32 * 1024 * 1024) break;
    }
    if (batch.empty()) return;
    auto hook = source_worker_hook, copy_hook = source_copy_hook;
    worker.pending = std::async(std::launch::async, [batch, hook, copy_hook]() mutable {
      platform::lower_background_thread_priority();
      try { if (hook) hook(); }
      catch (...) { for (auto& task : batch) task.retained->cancelled.store(true); }
      for (auto& task : batch) {
        auto& retained = *task.retained;
        std::unique_ptr<std::byte[]> bytes;
        try {
          if (!retained.cancelled.load(std::memory_order_acquire)) {
            auto& buffer = *retained.staging;
            bytes = std::make_unique_for_overwrite<std::byte[]>(std::size_t(buffer.size));
            for (VkDeviceSize offset = 0; offset < buffer.size; offset += 4096) {
              std::lock_guard gate(retained.gate);
              if (retained.cancelled.load(std::memory_order_acquire)) break;
              std::memcpy(bytes.get() + offset, static_cast<std::byte*>(buffer.mapped) + offset,
                  std::size_t(std::min<VkDeviceSize>(4096, buffer.size - offset)));
              if (offset == 0 && copy_hook) copy_hook();
            }
          }
        } catch (...) { bytes.reset(); /* Keep staging if allocation or copying fails. */ }
        std::shared_ptr<Buffer> release;
        {
          std::lock_guard lock(texture_lock);
          auto img = task.image.lock();
          if (img && img->retained == task.retained && !retained.cancelled.load() && bytes) {
            retained.bytes = std::move(bytes);
            img->shadow.reset();
          } else if (img && img->retained == task.retained) img->retained.reset();
          release = std::move(retained.staging);
        }
        // Native buffer/memory cleanup runs on the worker, outside texture_lock.
        release.reset();
      }
      // A completed std::async state may retain its callable. Do not let the
      // device's shared_future retain restored CPU copies or test callbacks.
      batch.clear(); hook = {}; copy_hook = {};
    }).share();
  } catch (...) {
    for (auto& task : batch) {
      task.retained->cancelled.store(true);
      if (auto img = task.image.lock(); img && img->retained == task.retained) img->retained.reset();
    }
  }
}
// image_pipeline mode of the in-place depth word conversion (shaders/depth_convert.comp).
constexpr unsigned depth_mode = 7;
// image_pipeline mode of the BC7 encoder (shaders/bc7_encode.comp): RGBA8 words in, blocks out.
constexpr unsigned bc7_mode = 8;
std::shared_ptr<Pipeline> create_pipeline(VkDevice device, unsigned image_mode) {
  auto created = std::make_shared<Pipeline>();
  created->device = device;
  created->destroy_pipeline = vk<PFN_vkDestroyPipeline>(device, "vkDestroyPipeline");
  created->destroy_layout = vk<PFN_vkDestroyPipelineLayout>(device, "vkDestroyPipelineLayout");
  created->destroy_descriptors = vk<PFN_vkDestroyDescriptorSetLayout>(device, "vkDestroyDescriptorSetLayout");
  VkDescriptorSetLayoutBinding bindings[3] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, image_mode && image_mode != bc7_mode ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  di.bindingCount = image_mode == depth_mode ? 1 : image_mode && image_mode != bc7_mode ? 3 : 2;
  di.pBindings = bindings;
  checked(vk<PFN_vkCreateDescriptorSetLayout>(device, "vkCreateDescriptorSetLayout")(device, &di, nullptr,
                                                                                     &created->descriptors));
  VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
  VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  li.setLayoutCount = 1;
  li.pSetLayouts = &created->descriptors;
  li.pushConstantRangeCount = 1;
  li.pPushConstantRanges = &range;
  checked(vk<PFN_vkCreatePipelineLayout>(device, "vkCreatePipelineLayout")(device, &li, nullptr,
                                                                           &created->layout));
  VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  si.codeSize = sizeof(decode_shader);
  si.pCode = decode_shader;
  switch (image_mode) {
    case 1: si.pCode = float_array::decode_shader; si.codeSize = sizeof(float_array::decode_shader); break;
    case 2: si.pCode = srgb_array::decode_shader; si.codeSize = sizeof(srgb_array::decode_shader); break;
    case 3: si.pCode = float_volume::decode_shader; si.codeSize = sizeof(float_volume::decode_shader); break;
    case 4: si.pCode = srgb_volume::decode_shader; si.codeSize = sizeof(srgb_volume::decode_shader); break;
    case 5: si.pCode = unorm_array::decode_shader; si.codeSize = sizeof(unorm_array::decode_shader); break;
    case 6: si.pCode = unorm_volume::decode_shader; si.codeSize = sizeof(unorm_volume::decode_shader); break;
    case depth_mode: si.pCode = depth::decode_shader; si.codeSize = sizeof(depth::decode_shader); break;
    case bc7_mode: si.pCode = bc7::decode_shader; si.codeSize = sizeof(bc7::decode_shader); break;
    default: break;
  }
  VkShaderModule shader{};
  checked(vk<PFN_vkCreateShaderModule>(device, "vkCreateShaderModule")(device, &si, nullptr, &shader));
  VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  ci.layout = created->layout;
  ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              nullptr,
              0,
              VK_SHADER_STAGE_COMPUTE_BIT,
              shader,
              "main",
              nullptr};
  auto result = gfx::create_decode_pipeline(
      device, VK_NULL_HANDLE, 1, &ci, nullptr, &created->pipeline);
  vk<PFN_vkDestroyShaderModule>(device, "vkDestroyShaderModule")(device, shader, nullptr);
  checked(result);
  return created;
}
std::shared_ptr<Pipeline> pipeline(VkDevice device) {
  register_pipeline_exit();
  std::shared_future<std::shared_ptr<Pipeline>> pending;
  {
    std::lock_guard lock(texture_lock);
    auto [it, inserted] = pipelines.try_emplace(device);
    if (inserted) {
      try { it->second = std::async(std::launch::async, create_pipeline, device, 0u).share(); }
      catch (...) { pipelines.erase(it); throw; }
    }
    pending = it->second;
  }
  // First upload can race device setup. All callers share the same result or
  // exception, and never hold texture_lock while waiting for the worker.
  return pending.get();
}
std::shared_ptr<Pipeline> image_pipeline(VkDevice device, unsigned mode) {
  register_pipeline_exit();
  std::shared_future<std::shared_ptr<Pipeline>> pending;
  {
    std::lock_guard lock(texture_lock);
    auto [it, inserted] = image_pipelines.try_emplace(std::pair{device, mode});
    if (inserted) {
      try { it->second = std::async(std::launch::async, create_pipeline, device, mode).share(); }
      catch (...) { image_pipelines.erase(it); throw; }
    }
    pending = it->second;
  }
  return pending.get();
}
void memory_barrier(VkDevice device, VkCommandBuffer cmd, VkPipelineStageFlags src_stage,
                    VkAccessFlags src_access, VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = dst_access;
  vk<PFN_vkCmdPipelineBarrier>(device, "vkCmdPipelineBarrier")(cmd, src_stage, dst_stage, 0, 1, &barrier, 0,
                                                               nullptr, 0, nullptr);
}
// Extent of one depth slice of a mip; 3D mips hold mip_depth() such slices.
VkExtent3D mip_extent(const Image& img, std::uint32_t mip) {
  return {std::max(1u, img.info.extent.width >> mip), std::max(1u, img.info.extent.height >> mip), 1};
}
std::uint32_t mip_depth(const Image& img, std::uint32_t mip) {
  return img.info.imageType == VK_IMAGE_TYPE_3D ? std::max(1u, img.info.extent.depth >> mip) : 1;
}
VkDeviceSize slice_bytes(const Image& img, std::uint32_t mip) {
  auto e = mip_extent(img, mip);
  return VkDeviceSize(ceil_div(e.width, img.format.width)) * ceil_div(e.height, img.format.height) *
         img.format.block_bytes;
}
// Shadow storage is layer-major, then mip, then depth slice: 3D images keep
// their 2D-block slices contiguous within each mip, like array layers.
VkDeviceSize raw_offset(const Image& img, std::uint32_t mip, std::uint32_t layer, std::uint32_t z = 0) {
  return img.layer_stride * layer + img.mip_offsets.at(mip) + slice_bytes(img, mip) * z;
}
// The (layer, z) addressed by slice k of a copy: 3D images step through depth
// from offset_z (their layer is the base), other images through array layers.
std::pair<std::uint32_t, std::uint32_t> copy_slice(const Image& img, std::uint32_t base_layer,
                                                   std::int32_t offset_z, std::uint32_t k) {
  if (img.info.imageType == VK_IMAGE_TYPE_3D) return {base_layer, std::uint32_t(offset_z) + k};
  return {base_layer + k, 0};
}
std::uint32_t copy_slices(const Image& img, std::uint32_t layer_count, std::uint32_t depth) {
  return img.info.imageType == VK_IMAGE_TYPE_3D ? depth : layer_count;
}
// Decode binds only compute state; replay the application's afterwards.
void restore_compute(VkCommandBuffer cmd) {
  std::lock_guard lock(texture_lock);
  for (auto& restore : command_state[cmd].compute_state) restore();
}
void decode_mip(VkCommandBuffer cmd, const std::shared_ptr<Image>& img, std::uint32_t mip,
                std::uint32_t layer, std::uint32_t z, VkImageLayout layout) {
  const auto d = img->device;
  auto source = source_buffer(img, cmd);
  if (!source) return;
  if (!command_supports_compute(cmd))
    throw std::runtime_error("compressed uploads require a compute-capable queue family");
  const auto extent = mip_extent(*img, mip);
  const auto& planes = img->storage.planes;
  // BC7 planes decode to RGBA8 scratch (texels), then encode into their own (blocks).
  std::vector<std::shared_ptr<Buffer>> decoded, texels(planes.size());
  for (std::size_t i = 0; i < planes.size(); ++i) {
    decoded.push_back(i && img->companions[i - 1]->lazy ? nullptr :
        decoded_buffer(d, cmd, i, plane_bytes(planes[i], extent.width, extent.height)));
    if (bc7_format(planes[i].format))
      texels[i] = decoded_buffer(d, cmd, planes.size() + i, VkDeviceSize(extent.width) * extent.height * 4);
  }
  // This execution dependency also finishes the previous decode's transfer
  // reads before a new dispatch writes reused output scratch. Each plane has
  // separate storage because all planes are dispatched before their copies.
  memory_barrier(d, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | (img->pageable_source ? VK_PIPELINE_STAGE_HOST_BIT : 0),
                 VK_ACCESS_SHADER_READ_BIT | (img->pageable_source ? VK_ACCESS_HOST_READ_BIT : 0));
  {
    std::lock_guard lock(texture_lock);
    command_state[cmd].resources.push_back(img);
    if (std::ranges::any_of(img->companions, [](const auto& c) { return c->lazy; }))
      command_state[cmd].uploads.push_back({img, mip, layer, z});
  }
  // Each recorded dispatch's descriptor pool is retained by the command buffer
  // before the next plane's decode can fail, and a failure still hands the
  // application its compute state back.
  try {
    for (std::size_t i = 0; i < planes.size(); ++i) {
      DecodeRegion region{texture_format(planes[i].decode), extent.width, extent.height,
                          ceil_div(extent.width, img->format.width),
                          static_cast<std::uint32_t>(raw_offset(*img, mip, layer, z) / 4)};
      region.format.decoded = planes[i].format;
      region.format.texel_bytes = planes[i].texel_bytes;
      std::shared_ptr<DecodeResources> resources;
      if (i && img->companions[i - 1]->lazy) {
        auto& companion = *img->companions[i - 1];
        resources = record_decode_image(d, cmd, source->buffer, source->size,
            companion.storage_views.at(mip), companion.activation->buffer, region,
            img->info.imageType == VK_IMAGE_TYPE_3D, img->info.imageType == VK_IMAGE_TYPE_3D ? z : layer);
      } else if (texels[i]) {
        region.format.decoded = planes[i].format == VK_FORMAT_BC7_SRGB_BLOCK ? VK_FORMAT_R8G8B8A8_SRGB
                                                                              : VK_FORMAT_R8G8B8A8_UNORM;
        resources = record_decode(d, cmd, source->buffer, source->size, texels[i]->buffer, texels[i]->size, region);
        {
          std::lock_guard lock(texture_lock);
          command_state[cmd].resources.push_back(std::move(resources));
        }
        memory_barrier(d, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        resources = record_encode_bc7(d, cmd, texels[i]->buffer, texels[i]->size, decoded[i]->buffer,
                                      decoded[i]->size, extent.width, extent.height);
      } else {
        resources = record_decode(d, cmd, source->buffer, source->size, decoded[i]->buffer,
                                  decoded[i]->size, region);
      }
      std::lock_guard lock(texture_lock);
      command_state[cmd].resources.push_back(std::move(resources));
    }
  } catch (...) {
    restore_compute(cmd);
    throw;
  }
  memory_barrier(d, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  if (std::ranges::any_of(img->companions, [](const auto& c) { return c->lazy; }))
    memory_barrier(d, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1};
  copy.imageOffset.z = std::int32_t(z);
  copy.imageExtent = extent;
  for (std::size_t i = 0; i < planes.size(); ++i) {
    if (!decoded[i]) continue;
    vk<PFN_vkCmdCopyBufferToImage>(d, "vkCmdCopyBufferToImage")(cmd, decoded[i]->buffer, img->plane(i), layout,
                                                                1, &copy);
  }
  // Finish reads of the compressed shadow before a later upload overwrites it.
  memory_barrier(d, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  restore_compute(cmd);
}

constexpr VkImageUsageFlags emulated_usage =
    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr VkImageCreateFlags emulated_flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT |
                                              VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
                                              VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
// Host image for a guest image of an emulated compressed format. Fails for
// shapes the decoder cannot back, among them VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_
// COMPATIBLE_BIT (views of the raw blocks) and chains other than a format list.
bool host_image_info(const VkImageCreateInfo& guest, TextureStorage& storage, VkImageCreateInfo& host,
                     bool compressed = false) {
  std::span<const VkFormat> views;
  for (auto* next = static_cast<const VkBaseInStructure*>(guest.pNext); next; next = next->pNext) {
    if (next->sType != VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) return false;
    const auto& list = *reinterpret_cast<const VkImageFormatListCreateInfo*>(next);
    views = {list.pViewFormats, list.viewFormatCount};
  }
  // 3D images of 2D-block formats are stacks of 2D slices (ASTC "sliced 3D").
  // A LINEAR image is created although queries report linear unsupported (as
  // Quest drivers allow it; Unreal's CPU-access textures): it is stored like an
  // optimal one. Its memory is not the image's, like any emulated image's.
  if ((guest.imageType != VK_IMAGE_TYPE_2D && guest.imageType != VK_IMAGE_TYPE_3D) ||
      (guest.tiling != VK_IMAGE_TILING_OPTIMAL && guest.tiling != VK_IMAGE_TILING_LINEAR) ||
      guest.samples != VK_SAMPLE_COUNT_1_BIT || (guest.usage & ~emulated_usage) ||
      (guest.flags & ~emulated_flags))
    return false;
  storage = texture_storage(guest.format, guest.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, views, compressed);
  if (!storage) return false;
  host = guest;
  host.pNext = nullptr;  // A host format list is only a hint; MUTABLE alone permits the views.
  host.format = storage.planes[0].format;
  host.tiling = VK_IMAGE_TILING_OPTIMAL;
  host.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  host.flags &= ~VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  if (!storage.planes[0].view_formats.empty()) host.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  return true;
}

VkResult write_activation(const std::shared_ptr<Buffer>& buffer, std::uint32_t value) {
  void* address{};
  auto r = vk<PFN_vkMapMemory>(buffer->device, "vkMapMemory")(
      buffer->device, buffer->memory, 0, sizeof(value), 0, &address);
  if (r != VK_SUCCESS) return r;
  std::memcpy(address, &value, sizeof(value));
  vk<PFN_vkUnmapMemory>(buffer->device, "vkUnmapMemory")(buffer->device, buffer->memory);
  return VK_SUCCESS;
}
VkResult try_lazy_companion(VkDevice d, const VkImageCreateInfo& info, Companion& companion, bool& created) {
  created = false;
  if ((info.imageType != VK_IMAGE_TYPE_2D && info.imageType != VK_IMAGE_TYPE_3D) ||
      info.tiling != VK_IMAGE_TILING_OPTIMAL || (info.flags & VK_IMAGE_CREATE_PROTECTED_BIT))
    return VK_SUCCESS;
  std::lock_guard lock(texture_lock);
  auto config = lazy_devices.find(d);
  if (config == lazy_devices.end() ||
      (info.imageType == VK_IMAGE_TYPE_2D && !config->second.features.sparseResidencyImage2D) ||
      (info.imageType == VK_IMAGE_TYPE_3D && !config->second.features.sparseResidencyImage3D))
    return VK_SUCCESS;
  if (info.format != VK_FORMAT_R16G16B16A16_SFLOAT && info.format != VK_FORMAT_R8G8B8A8_SRGB &&
      info.format != VK_FORMAT_R8G8B8A8_UNORM)
    return VK_SUCCESS;
  auto ci = info;
  ci.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
  ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
  if (ci.format == VK_FORMAT_R8G8B8A8_SRGB) {
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  }
  std::uint32_t count = 0;
  auto physical = device_physical(d);
  phys<PFN_vkGetPhysicalDeviceSparseImageFormatProperties>(physical,
      "vkGetPhysicalDeviceSparseImageFormatProperties")(
      physical, ci.format, ci.imageType, ci.samples, ci.usage, ci.tiling, &count, nullptr);
  if (!count) return VK_SUCCESS;
  auto r = vk<PFN_vkCreateImage>(d, "vkCreateImage")(d, &ci, nullptr, &companion.image);
  if (r == VK_ERROR_FORMAT_NOT_SUPPORTED) return VK_SUCCESS;
  if (r != VK_SUCCESS) return r;
  auto query = vk<PFN_vkGetImageSparseMemoryRequirements>(d, "vkGetImageSparseMemoryRequirements");
  query(d, companion.image, &count, nullptr);
  std::vector<VkSparseImageMemoryRequirements> requirements(count);
  if (count) query(d, companion.image, &count, requirements.data());
  // Metadata must be resident even before first use. Keep the existing eager
  // implementation for those formats instead of adding a second allocator.
  if (count != 1 || requirements[0].formatProperties.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT) {
    companion.destroy_image(d, companion.image, nullptr);
    companion.image = VK_NULL_HANDLE;
    return VK_SUCCESS;
  }
  companion.sparse = requirements[0];
  companion.storage_format = ci.format;
  vk<PFN_vkGetImageMemoryRequirements>(d, "vkGetImageMemoryRequirements")(
      d, companion.image, &companion.requirements);
  if (auto result = make_buffer(d, sizeof(std::uint32_t), &ci, companion.activation, true);
      result != VK_SUCCESS) return result;
  if (auto result = write_activation(companion.activation, 0); result != VK_SUCCESS) return result;
  companion.destroy_view = vk<PFN_vkDestroyImageView>(d, "vkDestroyImageView");
  for (std::uint32_t mip = 0; mip < ci.mipLevels; ++mip) {
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = companion.image; view.format = ci.format;
    view.viewType = ci.imageType == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, ci.arrayLayers};
    VkImageView handle{};
    if (auto result = vk<PFN_vkCreateImageView>(d, "vkCreateImageView")(d, &view, nullptr, &handle);
        result != VK_SUCCESS) return result;
    companion.storage_views.push_back(handle);
  }
  companion.lazy = true;
  created = true;
  return VK_SUCCESS;
}
std::atomic<int> compression_forced{-1};
// Decoded textures (RGBA8, 4 to 8 times their ASTC size; they filled 8-12 GB
// GPUs in Batman: Arkham Shadow, Asgard's Wrath 2 and Resident Evil 4) are
// stored as BC7 by default. VENTURE_TEXTURE_COMPRESSION (Settings > Runtime):
// always (1), never (0), or auto (2): once the GPU memory in use passes
// VENTURE_TEXTURE_COMPRESSION_THRESHOLD percent of the budget (default 50).
int compression_mode() {
  if (auto forced = compression_forced.load(std::memory_order_relaxed); forced >= 0) return forced;
  static const int mode = [] {
    const char* value = std::getenv("VENTURE_TEXTURE_COMPRESSION");
    if (!value) return 1;
    if (!std::strcmp(value, "never") || !std::strcmp(value, "0")) return 0;
    if (!std::strcmp(value, "auto") || !std::strcmp(value, "2")) return 2;
    return 1;
  }();
  return mode;
}
// Percent of the GPU memory budget in use past which automatic compression
// stores new textures as BC7. Textures decoded before it keep their size: at
// 50% a game fits about 2.5x the budget in decoded textures, at 90% 1.3x.
VkDeviceSize compression_threshold() {
  static const VkDeviceSize percent = [] {
    const char* value = std::getenv("VENTURE_TEXTURE_COMPRESSION_THRESHOLD");
    const long parsed = value ? std::strtol(value, nullptr, 10) : 0;
    return VkDeviceSize(parsed >= 1 && parsed <= 100 ? parsed : 50);
  }();
  return percent;
}
// Device-local memory this process uses and the budget the driver gives it
// (VK_EXT_memory_budget); zero limit when the host cannot say. Past the budget
// the OS moves resources to system memory and frames slow down.
struct GpuMemory {
  VkDeviceSize usage = 0, limit = 0;
};
std::atomic<VkPhysicalDevice> last_physical{nullptr};
GpuMemory gpu_memory(VkPhysicalDevice physical) {
  static std::mutex mutex;
  static std::map<VkPhysicalDevice, bool> support;
  bool supported;
  {
    std::lock_guard lock(mutex);
    auto [it, inserted] = support.try_emplace(physical, false);
    if (inserted) {
      auto enumerate = phys<PFN_vkEnumerateDeviceExtensionProperties>(physical,
          "vkEnumerateDeviceExtensionProperties");
      std::uint32_t count = 0;
      if (enumerate && enumerate(physical, nullptr, &count, nullptr) == VK_SUCCESS) {
        std::vector<VkExtensionProperties> extensions(count);
        if (enumerate(physical, nullptr, &count, extensions.data()) == VK_SUCCESS)
          it->second = std::ranges::any_of(extensions, [](const auto& e) {
            return std::string_view(e.extensionName) == VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
          });
      }
    }
    supported = it->second;
  }
  auto query = phys<PFN_vkGetPhysicalDeviceMemoryProperties2>(physical, "vkGetPhysicalDeviceMemoryProperties2");
  if (!supported || !query) return {};
  VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
  VkPhysicalDeviceMemoryProperties2 memory{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budget};
  query(physical, &memory);
  GpuMemory m;
  for (std::uint32_t i = 0; i < memory.memoryProperties.memoryHeapCount; ++i)
    if (memory.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
      m.usage += budget.heapUsage[i];
      m.limit += budget.heapBudget[i];
    }
  last_physical.store(physical, std::memory_order_relaxed);
  // Each whole GiB crossed, up or down, goes to the log: a few lines per run
  // that show in a debug report whether GPU memory grows with each level.
  static std::atomic<VkDeviceSize> logged_gib{0};
  if (const auto gib = m.usage >> 30; m.limit && logged_gib.exchange(gib) != gib)
    log::info("vulkan", "GPU memory in use: {} of {} MiB", m.usage >> 20, m.limit >> 20);
  return m;
}
// Samples GPU memory at most once a second as images are created, so its log
// lines appear whatever the compression mode.
void watch_gpu_memory(VkPhysicalDevice physical) {
  static std::atomic<std::int64_t> next{0};
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  auto due = next.load(std::memory_order_relaxed);
  if (now < due || !next.compare_exchange_strong(due, now + 1, std::memory_order_relaxed)) return;
  gpu_memory(physical);
}
// Whether `incoming` more bytes would take device-local memory past the
// threshold share of the budget.
bool memory_pressure(VkPhysicalDevice physical, VkDeviceSize incoming) {
  const auto [usage, limit] = gpu_memory(physical);
  if (!limit || usage + incoming <= limit / 100 * compression_threshold()) return false;
  static std::atomic<bool> reported{false};
  if (!reported.exchange(true))
    log::info("vulkan", "GPU memory {}% used ({} of {} MiB); new textures are stored as BC7 (quarter size)",
              compression_threshold(), usage >> 20, limit >> 20);
  return true;
}
// BC7 storage needs a 2D image, the BC feature (enabled with the device when
// supported, host_texture_features) and 256-invocation encode groups.
bool compressible(VkPhysicalDevice physical, const VkImageCreateInfo& host) {
  if (host.imageType != VK_IMAGE_TYPE_2D) return false;
  // An entry point the host lacks (a minimal or mock driver) means no compression.
  auto get_features = phys<PFN_vkGetPhysicalDeviceFeatures>(physical, "vkGetPhysicalDeviceFeatures");
  auto get_properties = phys<PFN_vkGetPhysicalDeviceProperties>(physical, "vkGetPhysicalDeviceProperties");
  auto get_limits = phys<PFN_vkGetPhysicalDeviceImageFormatProperties>(physical, "vkGetPhysicalDeviceImageFormatProperties");
  if (!get_features || !get_properties || !get_limits) return false;
  VkPhysicalDeviceFeatures features{};
  get_features(physical, &features);
  VkPhysicalDeviceProperties props{};
  get_properties(physical, &props);
  if (!features.textureCompressionBC || props.limits.maxComputeWorkGroupInvocations < 256 ||
      props.limits.maxComputeWorkGroupSize[0] < 16 || props.limits.maxComputeWorkGroupSize[1] < 16)
    return false;
  VkImageFormatProperties limits{};
  return get_limits(physical, host.format, host.imageType, host.tiling, host.usage, host.flags, &limits) == VK_SUCCESS &&
         host.extent.width <= limits.maxExtent.width && host.extent.height <= limits.maxExtent.height &&
         host.mipLevels <= limits.maxMipLevels && host.arrayLayers <= limits.maxArrayLayers;
}
// Diagnostic: VENTURE_VULKAN_MEMORY_CENSUS=1 logs, every 10 s, the guest's
// device memory and what its images hold, by kind, to show where VRAM goes.
namespace census {
enum Kind { msaa, depth, attachment, sampled, other, decoded, kinds };
constexpr const char* names[kinds] = {"msaa", "depth", "attachment", "sampled", "other", "decoded"};
struct State {
  std::mutex mutex;
  std::map<VkImage, Kind> pending;
  std::map<VkImage, std::pair<Kind, VkDeviceSize>> bound;
  std::map<VkDeviceMemory, std::pair<VkDeviceSize, bool>> memories;
  std::array<VkDeviceSize, kinds> bytes{};
  std::array<std::size_t, kinds> count{};
  VkDeviceSize local = 0, host = 0;
  std::jthread reporter;
};
bool enabled() {
  static const bool on = [] {
    const char* v = std::getenv("VENTURE_VULKAN_MEMORY_CENSUS");
    return v && *v && std::strcmp(v, "0") != 0;
  }();
  return on;
}
State& state() {
  static State s;
  static std::once_flag started;
  std::call_once(started, [] {
    s.reporter = std::jthread([](std::stop_token stop) {
      while (!stop.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        std::lock_guard lock(s.mutex);
        std::string kinds_text;
        for (int k = 0; k < kinds; ++k)
          kinds_text += std::format(" {}={:.0f}MiB/{}", names[k], double(s.bytes[k]) / 1048576, s.count[k]);
        log::info("vulkan-census", "device-local={:.0f}MiB host={:.0f}MiB images:{}", double(s.local) / 1048576,
                  double(s.host) / 1048576, kinds_text);
      }
    });
  });
  return s;
}
Kind classify(const VkImageCreateInfo& info) {
  if (info.samples > VK_SAMPLE_COUNT_1_BIT) return msaa;
  if (info.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) return depth;
  if (info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) return attachment;
  if (info.usage & VK_IMAGE_USAGE_SAMPLED_BIT) return sampled;
  return other;
}
void created(VkImage image, Kind kind) {
  auto& s = state();
  std::lock_guard lock(s.mutex);
  s.pending[image] = kind;
}
void bound(VkDevice d, VkImage image, VkDeviceSize size = 0) {
  auto& s = state();
  if (!size) {
    VkMemoryRequirements requirements{};
    vk<PFN_vkGetImageMemoryRequirements>(d, "vkGetImageMemoryRequirements")(d, image, &requirements);
    size = requirements.size;
  }
  std::lock_guard lock(s.mutex);
  auto it = s.pending.find(image);
  if (it == s.pending.end()) return;
  s.bound[image] = {it->second, size};
  s.bytes[it->second] += size;
  ++s.count[it->second];
  s.pending.erase(it);
}
void destroyed(VkImage image) {
  auto& s = state();
  std::lock_guard lock(s.mutex);
  s.pending.erase(image);
  if (auto it = s.bound.find(image); it != s.bound.end()) {
    s.bytes[it->second.first] -= it->second.second;
    --s.count[it->second.first];
    s.bound.erase(it);
  }
}
}  // namespace census

VkResult VKAPI_CALL create_image(VkDevice d, const VkImageCreateInfo* info,
                                 const VkAllocationCallbacks* alloc, VkImage* out) {
  auto create = vk<PFN_vkCreateImage>(d, "vkCreateImage");
  const auto physical = device_physical(d);
  if (!info || !out) return VK_ERROR_INITIALIZATION_FAILED;
  if (!supported_sample_count(physical, *info)) return VK_ERROR_VALIDATION_FAILED;
  watch_gpu_memory(physical);
  if (!emulated(physical, info->format)) {
    const auto guest_depth = take_guest_depth_format(info->format);
    auto result = create(d, info, alloc, out);
    if (result == VK_SUCCESS && guest_depth != VK_FORMAT_UNDEFINED) {
      std::lock_guard lock(texture_lock);
      depth_aliases[*out] = d;
    }
    if (result == VK_SUCCESS && census::enabled()) {
      census::created(*out, census::classify(*info));
      if (std::uint64_t(info->extent.width) * info->extent.height * info->arrayLayers * info->samples >= (8u << 20))
        log::info("vulkan-census", "large image format={} {}x{}x{} samples={} usage={:#x} flags={:#x}",
                  int(info->format), info->extent.width, info->extent.height, info->arrayLayers,
                  unsigned(info->samples), info->usage, info->flags);
    }
    return result;
  }
  auto img = std::make_shared<Image>();
  VkImageCreateInfo host{};
  if (!host_image_info(*info, img->storage, host)) {
    log::error("vulkan", "unsupported emulated compressed image: format={} type={} extent={}x{}x{} "
                        "mips={} layers={} samples={} tiling={} usage={:#x} flags={:#x} chain={}",
               int(info->format), int(info->imageType), info->extent.width, info->extent.height,
               info->extent.depth, info->mipLevels, info->arrayLayers, unsigned(info->samples),
               int(info->tiling), info->usage, info->flags,
               info->pNext ? int(static_cast<const VkBaseInStructure*>(info->pNext)->sType) : -1);
    return VK_ERROR_FORMAT_NOT_SUPPORTED;
  }
  if (info->tiling == VK_IMAGE_TILING_LINEAR) {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true))
      log::warn("vulkan", "Linear compressed image (format {}) is stored as an optimal decoded image; "
                          "CPU access to its memory does not reach it", int(info->format));
  }
  if (const auto mode = compression_mode(); mode != 0) {
    VkDeviceSize decoded = 0;
    for (const auto& plane : img->storage.planes)
      for (std::uint32_t mip = 0; mip < info->mipLevels; ++mip)
        decoded += plane_bytes(plane, std::max(1u, info->extent.width >> mip),
                               std::max(1u, info->extent.height >> mip)) * info->arrayLayers;
    TextureStorage storage;
    VkImageCreateInfo compressed{};
    const bool shaped = host_image_info(*info, storage, compressed, true) && bc7_format(storage.planes[0].format);
    if (mode == 1 && !(shaped && compressible(physical, compressed))) {
      static std::atomic<int> reported{0};
      if (reported.fetch_add(1) < 4)
        log::info("vulkan", "Texture format {} {}x{} usage={:#x} flags={:#x} stays decoded: {}", int(info->format),
                  info->extent.width, info->extent.height, info->usage, info->flags,
                  shaped ? "host cannot store it as BC7" : "no BC7 plane for this format");
    }
    if (shaped && compressible(physical, compressed) &&
        (mode == 1 || memory_pressure(physical, decoded))) {
      static std::atomic<bool> reported{false};
      if (!reported.exchange(true))
        log::info("vulkan", "Storing decoded textures as BC7 from a {}x{} texture on ({})",
                  info->extent.width, info->extent.height, mode == 1 ? "always" : "GPU memory threshold");
      img->storage = std::move(storage);
      host = compressed;
    }
  }
  img->device = d;
  img->format = texture_format(info->format);
  img->info = *info;
  img->info.pNext = nullptr;
  for (std::uint32_t mip = 0; mip < info->mipLevels; ++mip) {
    img->mip_offsets.push_back(img->layer_stride);
    img->layer_stride += slice_bytes(*img, mip) * mip_depth(*img, mip);
  }
  const auto size = img->layer_stride * info->arrayLayers;
  VkPhysicalDeviceProperties props{};
  phys<PFN_vkGetPhysicalDeviceProperties>(physical, "vkGetPhysicalDeviceProperties")(physical, &props);
  for (const auto& plane : img->storage.planes)
    if (VkDeviceSize(info->extent.width) * info->extent.height * plane.texel_bytes >
        props.limits.maxStorageBufferRange)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  if (!size || size > props.limits.maxStorageBufferRange) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  auto families = device_queue_families(d);
  auto private_info = *info;
  {
    std::lock_guard lock(texture_lock);
    if (lazy_devices.contains(d) && families.size() > 1) {
      private_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
      private_info.queueFamilyIndexCount = std::uint32_t(families.size());
      private_info.pQueueFamilyIndices = families.data();
      img->shadow_concurrent = true;
    }
  }
  img->source_size = size;
  if (private_info.sharingMode == VK_SHARING_MODE_CONCURRENT)
    img->source_families.assign(private_info.pQueueFamilyIndices,
        private_info.pQueueFamilyIndices + private_info.queueFamilyIndexCount);
  // Recreated exclusive buffers cannot inherit an old queue-family ownership
  // transfer. Multi-family devices use concurrent private shadows; otherwise
  // leave that source on its original backing.
  {
    std::lock_guard lock(texture_lock);
    // A private queue guarantees device-local restore when every mapped pool
    // is exhausted, without submitting behind unresolved guest semaphore waits.
    img->pageable_source = pageable_sources_enabled() && lazy_devices.contains(d) &&
        (private_info.sharingMode == VK_SHARING_MODE_CONCURRENT || families.size() == 1);
  }
  if (img->pageable_source) {
    auto r = make_buffer(d, size, &private_info, img->shadow, true, true, true);
    if (r == VK_ERROR_DEVICE_LOST) return r;
    if (r != VK_SUCCESS) {
      // Absence/exhaustion of compatible cached staging must not reject an
      // image the original device-local path can back. Never retire this one.
      img->shadow.reset();
      img->pageable_source = false;
    }
  }
  if (!img->pageable_source)
    if (auto r = make_buffer(d, size, &private_info, img->shadow); r != VK_SUCCESS) return r;
  // Optimal compressed images have opaque layouts. Their guest allocation
  // accounts for the compressed blocks; decoded planes are private resources,
  // just like the additional mutable-format planes below. In particular, a
  // mobile texture pool must not reserve RGBA storage for every ASTC image.
  vk<PFN_vkGetBufferMemoryRequirements>(d, "vkGetBufferMemoryRequirements")(
      d, img->shadow->buffer, &img->guest_requirements);
  // Nothing reads or writes an optimal image's guest memory: its blocks live in
  // the private source and the decoded image. The guest reserves one alignment
  // unit instead of a second, unused copy of every compressed texture in VRAM.
  // A LINEAR image's memory is mapped by the guest and keeps its full size.
  if (info->tiling == VK_IMAGE_TILING_OPTIMAL)
    img->guest_requirements.size = std::max<VkDeviceSize>(img->guest_requirements.alignment, 1);
  if (img->pageable_source) {
    img->source_used = std::chrono::steady_clock::now();
    have_pageable_sources.store(true, std::memory_order_relaxed);
    register_pipeline_exit();
  }
  for (std::size_t i = 1; i < img->storage.planes.size(); ++i) {
    auto companion = std::make_unique<Companion>();
    companion->device = d;
    companion->destroy_image = vk<PFN_vkDestroyImage>(d, "vkDestroyImage");
    companion->free_memory = vk<PFN_vkFreeMemory>(d, "vkFreeMemory");
    auto ci = host;
    if (img->shadow_concurrent) {
      ci.sharingMode = private_info.sharingMode;
      ci.queueFamilyIndexCount = private_info.queueFamilyIndexCount;
      ci.pQueueFamilyIndices = private_info.pQueueFamilyIndices;
    }
    ci.format = img->storage.planes[i].format;
    ci.flags &= ~VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    bool lazy = false;
    if (auto r = try_lazy_companion(d, ci, *companion, lazy); r != VK_SUCCESS) return r;
    if (lazy) { img->companions.push_back(std::move(companion)); continue; }
    if (auto r = create(d, &ci, nullptr, &companion->image); r != VK_SUCCESS) return r;
    if (auto r = bind_decoded_image(d, companion->image, companion->decoded_allocation); r != VK_SUCCESS)
      return r;
    img->companions.push_back(std::move(companion));
  }
  auto result = create(d, &host, alloc, out);
  if (result == VK_SUCCESS) {
    img->image = *out;
    if (census::enabled()) census::created(*out, census::decoded);
    std::lock_guard lock(texture_lock);
    images[*out] = std::move(img);
  }
  return result;
}
// An emulated image's layout is its raw blocks, layer-major then mip, which
// its memory requirements cover (a LINEAR image's mapped reads stay in range).
void VKAPI_CALL subresource_layout(VkDevice d, VkImage handle, const VkImageSubresource* sub,
                                   VkSubresourceLayout* out) {
  if (auto img = image(handle); img && sub && out && sub->mipLevel < img->info.mipLevels &&
                                sub->arrayLayer < img->info.arrayLayers) {
    const auto slice = slice_bytes(*img, sub->mipLevel);
    *out = {raw_offset(*img, sub->mipLevel, sub->arrayLayer), slice * mip_depth(*img, sub->mipLevel),
            VkDeviceSize(ceil_div(mip_extent(*img, sub->mipLevel).width, img->format.width)) *
                img->format.block_bytes,
            img->layer_stride, slice};
    return;
  }
  vk<PFN_vkGetImageSubresourceLayout>(d, "vkGetImageSubresourceLayout")(d, handle, sub, out);
}
void VKAPI_CALL destroy_image(VkDevice d, VkImage handle, const VkAllocationCallbacks* alloc) {
  // Forget the record before the host frees the handle: once it is freed, a
  // concurrent vkCreateImage may receive the same value and register its own
  // record. The shadow buffer outlives the host image.
  std::shared_ptr<Image> removed;
  {
    std::lock_guard lock(texture_lock);
    auto it = images.find(handle);
    if (it != images.end()) {
      removed = std::move(it->second);
      if (removed->retained) removed->retained->cancelled.store(true, std::memory_order_release);
      images.erase(it);
    }
    depth_aliases.erase(handle);
  }
  if (census::enabled()) census::destroyed(handle);
  vk<PFN_vkDestroyImage>(d, "vkDestroyImage")(d, handle, alloc);
}
void VKAPI_CALL image_requirements(VkDevice d, VkImage handle, VkMemoryRequirements* out) {
  if (auto img = image(handle)) { *out = img->guest_requirements; return; }
  vk<PFN_vkGetImageMemoryRequirements>(d, "vkGetImageMemoryRequirements")(d, handle, out);
}
void private_dedicated_requirements(void* chain) {
  for (auto* p = static_cast<VkBaseOutStructure*>(chain); p; p = p->pNext)
    if (p->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
      auto& dedicated = *reinterpret_cast<VkMemoryDedicatedRequirements*>(p);
      dedicated.prefersDedicatedAllocation = dedicated.requiresDedicatedAllocation = VK_FALSE;
    }
}
void VKAPI_CALL image_requirements2(VkDevice d, const VkImageMemoryRequirementsInfo2* info,
                                    VkMemoryRequirements2* out) {
  if (auto img = image(info->image)) {
    out->memoryRequirements = img->guest_requirements;
    private_dedicated_requirements(out->pNext);
    return;
  }
  vk<PFN_vkGetImageMemoryRequirements2>(d, "vkGetImageMemoryRequirements2")(d, info, out);
}
VkResult bind_private_image(VkDevice d, const std::shared_ptr<Image>& img) {
  // Serialize only competing binds of this image, never unrelated texture
  // creation or command recording while the driver allocates pooled memory.
  std::lock_guard lock(img->binding_mutex);
  if (img->memory_bound) return VK_ERROR_VALIDATION_FAILED;
  const auto r = bind_decoded_image(d, img->image, img->decoded_allocation);
  if (r != VK_SUCCESS) return r;
  img->memory_bound = true;
  if (census::enabled()) census::bound(d, img->image);
  return VK_SUCCESS;
}
VkResult VKAPI_CALL bind_image(VkDevice d, VkImage handle, VkDeviceMemory memory, VkDeviceSize offset) {
  if (auto img = image(handle)) return bind_private_image(d, img);
  const auto result = vk<PFN_vkBindImageMemory>(d, "vkBindImageMemory")(d, handle, memory, offset);
  if (result == VK_SUCCESS && census::enabled()) census::bound(d, handle);
  return result;
}
VkResult VKAPI_CALL bind_images(VkDevice d, std::uint32_t count, const VkBindImageMemoryInfo* infos) {
  bool translated = false;
  for (std::uint32_t i = 0; i < count; ++i) translated |= bool(image(infos[i].image));
  if (!translated) {
    const auto result = vk<PFN_vkBindImageMemory2>(d, "vkBindImageMemory2")(d, count, infos);
    if (result == VK_SUCCESS && census::enabled())
      for (std::uint32_t i = 0; i < count; ++i) census::bound(d, infos[i].image);
    return result;
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto& info = infos[i];
    VkResult result;
    if (auto img = image(info.image)) {
      // Disjoint, external, and sparse images are not advertised by this
      // compressed-image translator and cannot reach this binding path.
      if (info.pNext) return VK_ERROR_FEATURE_NOT_PRESENT;
      result = bind_private_image(d, img);
    } else {
      result = vk<PFN_vkBindImageMemory2>(d, "vkBindImageMemory2")(d, 1, &info);
      if (result == VK_SUCCESS && census::enabled()) census::bound(d, info.image);
    }
    if (result != VK_SUCCESS) return result;
  }
  return VK_SUCCESS;
}
VkResult VKAPI_CALL allocate_guest_memory(VkDevice d, const VkMemoryAllocateInfo* info,
                                          const VkAllocationCallbacks* alloc, VkDeviceMemory* out) {
  auto copy = *info;
  auto* link = reinterpret_cast<VkBaseOutStructure*>(&copy);
  while (link->pNext) {
    auto* next = link->pNext;
    if (next->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO &&
        image(reinterpret_cast<const VkMemoryDedicatedAllocateInfo*>(next)->image)) {
      // The host image is backed separately. Keep the guest's dedicated
      // reservation, without dedicating that smaller allocation to an RGBA image.
      link->pNext = next->pNext;
    } else link = next;
  }
  const auto result = vk<PFN_vkAllocateMemory>(d, "vkAllocateMemory")(d, &copy, alloc, out);
  if (result == VK_SUCCESS && census::enabled()) {
    VkPhysicalDeviceMemoryProperties props{};
    const auto physical = device_physical(d);
    phys<PFN_vkGetPhysicalDeviceMemoryProperties>(physical, "vkGetPhysicalDeviceMemoryProperties")(physical, &props);
    const bool local = props.memoryTypes[info->memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    auto& s = census::state();
    std::lock_guard lock(s.mutex);
    s.memories[*out] = {info->allocationSize, local};
    (local ? s.local : s.host) += info->allocationSize;
  }
  return result;
}
void VKAPI_CALL free_guest_memory(VkDevice d, VkDeviceMemory memory, const VkAllocationCallbacks* alloc) {
  if (census::enabled() && memory) {
    auto& s = census::state();
    std::lock_guard lock(s.mutex);
    if (auto it = s.memories.find(memory); it != s.memories.end()) {
      (it->second.second ? s.local : s.host) -= it->second.first;
      s.memories.erase(it);
    }
  }
  vk<PFN_vkFreeMemory>(d, "vkFreeMemory")(d, memory, alloc);
}
VkResult VKAPI_CALL create_view(VkDevice d, const VkImageViewCreateInfo* info,
                                const VkAllocationCallbacks* alloc, VkImageView* out) {
  auto copy = *info;
  if (auto img = image(info->image)) {
    // The chain is the call's translated copy. Usage and min-LOD apply to the
    // decoded view unchanged. Consume ASTC decode precision before forwarding
    // to the non-ASTC host view, selecting its matching decoded plane below.
    int unsupported = -1;
    bool has_usage = false;
    VkFormat decode_mode = VK_FORMAT_UNDEFINED;
    auto* link = reinterpret_cast<VkBaseOutStructure*>(&copy);
    while (link->pNext) {
      auto* next = link->pNext;
      if (next->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_ASTC_DECODE_MODE_EXT) {
        decode_mode = reinterpret_cast<VkImageViewASTCDecodeModeEXT*>(next)->decodeMode;
        link->pNext = next->pNext;
        continue;
      }
      has_usage |= next->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
      if (next->sType != VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO &&
          next->sType != VK_STRUCTURE_TYPE_IMAGE_VIEW_MIN_LOD_CREATE_INFO_EXT && unsupported < 0)
        unsupported = int(next->sType);
      link = next;
    }
    auto view = unsupported >= 0 ? std::nullopt : texture_view(img->storage, copy.format, decode_mode);
    if (!view) {
      log::error("vulkan", "unsupported view of emulated compressed image: image format={} view format={} chain={}",
                 int(img->format.compressed), int(copy.format), unsupported);
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    copy.image = img->plane(view->plane);
    copy.format = view->format;
    if (view->plane && img->companions[view->plane - 1]->lazy) {
      // Cancel/reclaim retirement before the activation path takes texture_lock.
      std::shared_ptr<Buffer> source;
      try { source = source_buffer(img); }
      catch (const TextureFailure& failure) { return failure.result; }
      std::lock_guard lock(texture_lock);
      if (auto r = materialize_companion(img, view->plane); r != VK_SUCCESS) return r;
      // The private UNORM storage view needs STORAGE usage; a guest SRGB
      // sampling view must not inherit that unsupported usage bit.
      VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
      if (!has_usage) {
        usage.usage = img->info.usage;
        usage.pNext = copy.pNext;
        copy.pNext = &usage;
      }
      auto r = vk<PFN_vkCreateImageView>(d, "vkCreateImageView")(d, &copy, alloc, out);
      if (r == VK_SUCCESS) companion_views[{d, *out}] = img;
      return r;
    }
  }
  return vk<PFN_vkCreateImageView>(d, "vkCreateImageView")(d, &copy, alloc, out);
}
void VKAPI_CALL destroy_view(VkDevice d, VkImageView view, const VkAllocationCallbacks* alloc) {
  { std::lock_guard lock(texture_lock); companion_views.erase({d, view}); }
  vk<PFN_vkDestroyImageView>(d, "vkDestroyImageView")(d, view, alloc);
}
void VKAPI_CALL device_image_requirements(VkDevice d, const VkDeviceImageMemoryRequirements* info,
                                         VkMemoryRequirements2* out) {
  if (emulated(device_physical(d), info->pCreateInfo->format)) {
    TextureStorage storage;
    VkImageCreateInfo image_info{};
    if (!host_image_info(*info->pCreateInfo, storage, image_info)) {
      // vkCreateImage refuses this descriptor too; report nothing to bind.
      log::error("vulkan", "memory requirements for unsupported emulated compressed image format={}",
                 int(info->pCreateInfo->format));
      out->memoryRequirements = {};
      return;
    }
    Image layout;
    layout.info = *info->pCreateInfo;
    layout.format = texture_format(layout.info.format);
    VkDeviceSize size = 0;
    for (std::uint32_t mip = 0; mip < layout.info.mipLevels; ++mip)
      size += slice_bytes(layout, mip) * mip_depth(layout, mip);
    size *= layout.info.arrayLayers;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = layout.info.sharingMode;
    ci.queueFamilyIndexCount = layout.info.queueFamilyIndexCount;
    ci.pQueueFamilyIndices = layout.info.pQueueFamilyIndices;
    VkBuffer buffer{};
    const auto result = vk<PFN_vkCreateBuffer>(d, "vkCreateBuffer")(d, &ci, nullptr, &buffer);
    if (result != VK_SUCCESS) {
      out->memoryRequirements = {};
      log::error("vulkan", "compressed image memory requirements buffer failed: {}", int(result));
      return;
    }
    vk<PFN_vkGetBufferMemoryRequirements>(d, "vkGetBufferMemoryRequirements")(
        d, buffer, &out->memoryRequirements);
    vk<PFN_vkDestroyBuffer>(d, "vkDestroyBuffer")(d, buffer, nullptr);
    private_dedicated_requirements(out->pNext);
    return;
  }
  vk<PFN_vkGetDeviceImageMemoryRequirements>(d, "vkGetDeviceImageMemoryRequirements")(d, info, out);
}
void VKAPI_CALL device_sparse_requirements(VkDevice d, const VkDeviceImageMemoryRequirements* info,
                                          std::uint32_t* count, VkSparseImageMemoryRequirements2* out) {
  if (emulated(device_physical(d), info->pCreateInfo->format)) {
    *count = 0;  // No sparse residency properties for an emulated format.
    return;
  }
  vk<PFN_vkGetDeviceImageSparseMemoryRequirements>(d, "vkGetDeviceImageSparseMemoryRequirements")(d, info, count,
                                                                                              out);
}
bool depth_alias(VkImage handle) {
  std::lock_guard lock(texture_lock);
  return depth_aliases.contains(handle);
}
struct DepthConversion {
  std::shared_ptr<Pipeline> pipeline;
  std::shared_ptr<Buffer> words;
  VkDescriptorPool pool{};
  PFN_vkDestroyDescriptorPool destroy_pool{};
  ~DepthConversion() {
    if (pool) destroy_pool(pipeline->device, pool, nullptr);
  }
};
// Tightly packed words of a depth buffer-image copy region in private scratch,
// and the copies between them and the guest buffer's rows and slices.
struct DepthRegion {
  std::uint32_t words;
  std::vector<VkBufferCopy> rows;  // guest offset in srcOffset, scratch offset in dstOffset
};
DepthRegion depth_region(const VkBufferImageCopy& r) {
  const auto width = r.imageExtent.width, height = r.imageExtent.height;
  const auto row = r.bufferRowLength ? r.bufferRowLength : width;
  const auto rows = r.bufferImageHeight ? r.bufferImageHeight : height;
  const auto slices = r.imageSubresource.layerCount * r.imageExtent.depth;
  DepthRegion region{width * height * slices, {}};
  if (row == width && rows == height) {
    region.rows.push_back({r.bufferOffset, 0, VkDeviceSize(region.words) * 4});
    return region;
  }
  for (std::uint32_t k = 0; k < slices; ++k)
    for (std::uint32_t y = 0; y < height; ++y)
      region.rows.push_back({r.bufferOffset + (VkDeviceSize(k) * rows + y) * row * 4,
                             (VkDeviceSize(k) * height + y) * width * 4, VkDeviceSize(width) * 4});
  return region;
}
// Converts scratch words between the guest's D24 buffer layout and the host's
// D32 float layout, ordered after the transfer that filled them and before the
// transfer that consumes them.
void convert_depth(VkCommandBuffer cmd, const std::shared_ptr<Buffer>& words, std::uint32_t count, bool to_unorm) {
  const auto d = command_device(cmd);
  if (!command_supports_compute(cmd))
    throw std::runtime_error("24-bit depth buffer copies on this host require a compute-capable queue family");
  auto conversion = std::make_shared<DepthConversion>();
  conversion->pipeline = image_pipeline(d, depth_mode);
  conversion->words = words;
  conversion->destroy_pool = vk<PFN_vkDestroyDescriptorPool>(d, "vkDestroyDescriptorPool");
  auto& p = *conversion->pipeline;
  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
  VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pi.maxSets = 1;
  pi.poolSizeCount = 1;
  pi.pPoolSizes = &size;
  checked(vk<PFN_vkCreateDescriptorPool>(d, "vkCreateDescriptorPool")(d, &pi, nullptr, &conversion->pool));
  {
    std::lock_guard lock(texture_lock);
    command_state[cmd].resources.push_back(conversion);
  }
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = conversion->pool;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &p.descriptors;
  VkDescriptorSet set{};
  checked(vk<PFN_vkAllocateDescriptorSets>(d, "vkAllocateDescriptorSets")(d, &ai, &set));
  VkDescriptorBufferInfo buffer{words->buffer, 0, VkDeviceSize(count) * 4};
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = set;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write.pBufferInfo = &buffer;
  vk<PFN_vkUpdateDescriptorSets>(d, "vkUpdateDescriptorSets")(d, 1, &write, 0, nullptr);
  VkPhysicalDeviceProperties props{};
  const auto physical = device_physical(d);
  phys<PFN_vkGetPhysicalDeviceProperties>(physical, "vkGetPhysicalDeviceProperties")(physical, &props);
  const std::uint32_t step = props.limits.maxComputeWorkGroupCount[0] * 64u;
  memory_barrier(d, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  vk<PFN_vkCmdBindPipeline>(d, "vkCmdBindPipeline")(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
  vk<PFN_vkCmdBindDescriptorSets>(d, "vkCmdBindDescriptorSets")(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout,
                                                                0, 1, &set, 0, nullptr);
  for (std::uint32_t base = 0; base < count;) {
    const auto n = std::min(step, count - base);
    const std::uint32_t push[3] = {base, count, to_unorm ? 1u : 0u};
    vk<PFN_vkCmdPushConstants>(d, "vkCmdPushConstants")(cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                                        sizeof(push), push);
    vk<PFN_vkCmdDispatch>(d, "vkCmdDispatch")(cmd, ceil_div(n, 64), 1, 1);
    base += n;
  }
  memory_barrier(d, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  restore_compute(cmd);
}
// Buffer copies of an aliased image's depth aspect: the guest's words are
// packed 24-bit UNORM, the host image's 32-bit float. Stencil is identical.
void copy_buffer_depth(VkCommandBuffer cmd, VkBuffer source, VkImage dest, VkImageLayout layout,
                       std::uint32_t count, const VkBufferImageCopy* regions) {
  const auto d = command_device(cmd);
  auto copy = vk<PFN_vkCmdCopyBufferToImage>(d, "vkCmdCopyBufferToImage");
  for (std::uint32_t i = 0; i < count; ++i) {
    auto r = regions[i];
    if (!(r.imageSubresource.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)) {
      copy(cmd, source, dest, layout, 1, &r);
      continue;
    }
    const auto region = depth_region(r);
    if (!region.words) continue;
    auto words = buffer(d, VkDeviceSize(region.words) * 4);
    vk<PFN_vkCmdCopyBuffer>(d, "vkCmdCopyBuffer")(cmd, source, words->buffer, std::uint32_t(region.rows.size()),
                                                  region.rows.data());
    convert_depth(cmd, words, region.words, false);
    r.bufferOffset = 0;
    r.bufferRowLength = r.bufferImageHeight = 0;
    copy(cmd, words->buffer, dest, layout, 1, &r);
  }
}
void copy_depth_buffer(VkCommandBuffer cmd, VkImage source, VkImageLayout layout, VkBuffer dest,
                       std::uint32_t count, const VkBufferImageCopy* regions) {
  const auto d = command_device(cmd);
  auto copy = vk<PFN_vkCmdCopyImageToBuffer>(d, "vkCmdCopyImageToBuffer");
  for (std::uint32_t i = 0; i < count; ++i) {
    auto r = regions[i];
    if (!(r.imageSubresource.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)) {
      copy(cmd, source, layout, dest, 1, &r);
      continue;
    }
    auto region = depth_region(r);
    if (!region.words) continue;
    auto words = buffer(d, VkDeviceSize(region.words) * 4);
    r.bufferOffset = 0;
    r.bufferRowLength = r.bufferImageHeight = 0;
    copy(cmd, source, layout, words->buffer, 1, &r);
    convert_depth(cmd, words, region.words, true);
    for (auto& row : region.rows) std::swap(row.srcOffset, row.dstOffset);
    vk<PFN_vkCmdCopyBuffer>(d, "vkCmdCopyBuffer")(cmd, words->buffer, dest, std::uint32_t(region.rows.size()),
                                                  region.rows.data());
  }
}
template <class Region, class Info>
std::vector<VkBufferImageCopy> plain_regions(const Info& info) {
  std::vector<VkBufferImageCopy> regions;
  for (std::uint32_t i = 0; i < info.regionCount; ++i) {
    const Region& r = info.pRegions[i];
    regions.push_back({r.bufferOffset, r.bufferRowLength, r.bufferImageHeight, r.imageSubresource,
                       r.imageOffset, r.imageExtent});
  }
  return regions;
}
void VKAPI_CALL copy_buffer_image(VkCommandBuffer cmd, VkBuffer source, VkImage dest, VkImageLayout layout,
                                  std::uint32_t count, const VkBufferImageCopy* regions) {
  if (depth_alias(dest)) return copy_buffer_depth(cmd, source, dest, layout, count, regions);
  auto d = command_device(cmd);
  auto img = image(dest);
  if (!img)
    return vk<PFN_vkCmdCopyBufferToImage>(d, "vkCmdCopyBufferToImage")(cmd, source, dest, layout, count,
                                                                       regions);
  auto target = source_buffer(img, cmd);
  if (!target) return;
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto& r = regions[i];
    auto e = mip_extent(*img, r.imageSubresource.mipLevel);
    const auto bw = img->format.width, bh = img->format.height, bytes = img->format.block_bytes;
    const auto row = ceil_div(r.bufferRowLength ? r.bufferRowLength : r.imageExtent.width, bw);
    const auto rows = ceil_div(r.bufferImageHeight ? r.bufferImageHeight : r.imageExtent.height, bh);
    const auto target_row = ceil_div(e.width, bw);
    const auto slices = copy_slices(*img, r.imageSubresource.layerCount, r.imageExtent.depth);
    for (std::uint32_t k = 0; k < slices; ++k) {
      const auto [layer, z] = copy_slice(*img, r.imageSubresource.baseArrayLayer, r.imageOffset.z, k);
      std::vector<VkBufferCopy> copies;
      for (std::uint32_t y = 0; y < ceil_div(r.imageExtent.height, bh); ++y) {
        copies.push_back(
            {r.bufferOffset + VkDeviceSize(k * rows + y) * row * bytes,
             raw_offset(*img, r.imageSubresource.mipLevel, layer, z) +
                 (VkDeviceSize(r.imageOffset.y / bh + y) * target_row + r.imageOffset.x / bw) * bytes,
             VkDeviceSize(ceil_div(r.imageExtent.width, bw)) * bytes});
      }
      vk<PFN_vkCmdCopyBuffer>(d, "vkCmdCopyBuffer")(cmd, source, target->buffer,
                                                    static_cast<std::uint32_t>(copies.size()), copies.data());
      decode_mip(cmd, img, r.imageSubresource.mipLevel, layer, z, layout);
    }
  }
}
void VKAPI_CALL copy_image_buffer(VkCommandBuffer cmd, VkImage source, VkImageLayout layout, VkBuffer dest,
                                  std::uint32_t count, const VkBufferImageCopy* regions) {
  if (depth_alias(source)) return copy_depth_buffer(cmd, source, layout, dest, count, regions);
  auto d = command_device(cmd);
  auto img = image(source);
  if (!img)
    return vk<PFN_vkCmdCopyImageToBuffer>(d, "vkCmdCopyImageToBuffer")(cmd, source, layout, dest, count,
                                                                       regions);
  memory_barrier(d, cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  auto retained = source_buffer(img, cmd);
  if (!retained) return;
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto& r = regions[i];
    auto e = mip_extent(*img, r.imageSubresource.mipLevel);
    auto bw = img->format.width, bh = img->format.height, bytes = img->format.block_bytes;
    auto row = ceil_div(r.bufferRowLength ? r.bufferRowLength : r.imageExtent.width, bw);
    auto rows = ceil_div(r.bufferImageHeight ? r.bufferImageHeight : r.imageExtent.height, bh);
    auto source_row = ceil_div(e.width, bw);
    std::vector<VkBufferCopy> copies;
    const auto slices = copy_slices(*img, r.imageSubresource.layerCount, r.imageExtent.depth);
    for (std::uint32_t k = 0; k < slices; ++k) {
      const auto [layer, z] = copy_slice(*img, r.imageSubresource.baseArrayLayer, r.imageOffset.z, k);
      for (std::uint32_t y = 0; y < ceil_div(r.imageExtent.height, bh); ++y) {
        copies.push_back(
            {raw_offset(*img, r.imageSubresource.mipLevel, layer, z) +
                 (VkDeviceSize(r.imageOffset.y / bh + y) * source_row + r.imageOffset.x / bw) * bytes,
             r.bufferOffset + VkDeviceSize(k * rows + y) * row * bytes,
             VkDeviceSize(ceil_div(r.imageExtent.width, bw)) * bytes});
      }
    }
    vk<PFN_vkCmdCopyBuffer>(d, "vkCmdCopyBuffer")(cmd, retained->buffer, dest,
                                                  static_cast<std::uint32_t>(copies.size()), copies.data());
  }
}
void VKAPI_CALL copy_image(VkCommandBuffer cmd, VkImage source, VkImageLayout source_layout, VkImage dest,
                           VkImageLayout dest_layout, std::uint32_t count, const VkImageCopy* regions) {
  auto d = command_device(cmd);
  auto src = image(source), dst = image(dest);
  if (!src && !dst)
    return vk<PFN_vkCmdCopyImage>(d, "vkCmdCopyImage")(cmd, source, source_layout, dest, dest_layout, count,
                                                       regions);
  auto src_buffer = src ? source_buffer(src, cmd) : nullptr;
  auto dst_buffer = dst ? source_buffer(dst, cmd) : nullptr;
  if ((src && !src_buffer) || (dst && !dst_buffer)) return;
  // Size-compatible copies (VUID-vkCmdCopyImage-srcImage-01548) only need the
  // same block geometry; the destination decodes with its own storage.
  if (src && dst &&
      (src->format.width != dst->format.width || src->format.height != dst->format.height ||
       src->format.block_bytes != dst->format.block_bytes))
    throw std::runtime_error("compressed copies between different block sizes are unsupported");
  memory_barrier(d, cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
  if (!src || !dst) {
    // Uncompressed <-> compressed: each texel of the uncompressed image is one
    // block, and the extent counts source texels. The emulated side's blocks
    // live in its shadow buffer, laid out as an image of block-sized texels.
    const auto& img = src ? *src : *dst;
    const auto bw = img.format.width, bh = img.format.height, bytes = img.format.block_bytes;
    for (std::uint32_t i = 0; i < count; ++i) {
      const auto& r = regions[i];
      const auto& blocks_sub = src ? r.srcSubresource : r.dstSubresource;
      const auto& texels_sub = src ? r.dstSubresource : r.srcSubresource;
      const auto blocks_at = src ? r.srcOffset : r.dstOffset;
      const auto row = ceil_div(mip_extent(img, blocks_sub.mipLevel).width, bw);
      VkBufferImageCopy c{};
      c.bufferRowLength = row;
      c.imageOffset = src ? r.dstOffset : r.srcOffset;
      c.imageExtent = src ? VkExtent3D{ceil_div(r.extent.width, bw), ceil_div(r.extent.height, bh), 1}
                          : VkExtent3D{r.extent.width, r.extent.height, 1};
      // Slices of a 3D image are its depth slices. The uncompressed image is
      // 3D when its subresource has one layer: copies between a 3D image and a
      // 2D array map depth slices to layers.
      const auto slices = copy_slices(img, blocks_sub.layerCount, r.extent.depth);
      const auto texels_at = c.imageOffset;
      const bool texels_3d = texels_sub.layerCount == 1;
      for (std::uint32_t k = 0; k < slices; ++k) {
        const auto [layer, z] = copy_slice(img, blocks_sub.baseArrayLayer, blocks_at.z, k);
        c.bufferOffset = raw_offset(img, blocks_sub.mipLevel, layer, z) +
                         (VkDeviceSize(blocks_at.y / bh) * row + blocks_at.x / bw) * bytes;
        c.imageOffset.z = texels_3d ? texels_at.z + std::int32_t(k) : texels_at.z;
        c.imageSubresource = {texels_sub.aspectMask, texels_sub.mipLevel,
                              texels_sub.baseArrayLayer + (texels_3d ? 0 : k), 1};
        if (src) {
          vk<PFN_vkCmdCopyBufferToImage>(d, "vkCmdCopyBufferToImage")(cmd, src_buffer->buffer, dest, dest_layout,
                                                                      1, &c);
        } else {
          vk<PFN_vkCmdCopyImageToBuffer>(d, "vkCmdCopyImageToBuffer")(cmd, source, source_layout,
                                                                      dst_buffer->buffer, 1, &c);
          decode_mip(cmd, dst, blocks_sub.mipLevel, layer, z, dest_layout);
        }
      }
    }
    return;
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    auto& r = regions[i];
    auto se = mip_extent(*src, r.srcSubresource.mipLevel), de = mip_extent(*dst, r.dstSubresource.mipLevel);
    auto bw = src->format.width, bh = src->format.height, bytes = src->format.block_bytes;
    // Depth slices of a 3D image correspond to array layers of a 2D one.
    const auto slices = copy_slices(*src, r.srcSubresource.layerCount, r.extent.depth);
    for (std::uint32_t k = 0; k < slices; ++k) {
      const auto [src_layer, src_z] = copy_slice(*src, r.srcSubresource.baseArrayLayer, r.srcOffset.z, k);
      const auto [dst_layer, dst_z] = copy_slice(*dst, r.dstSubresource.baseArrayLayer, r.dstOffset.z, k);
      std::vector<VkBufferCopy> copies;
      for (std::uint32_t y = 0; y < ceil_div(r.extent.height, bh); ++y) {
        copies.push_back(
            {raw_offset(*src, r.srcSubresource.mipLevel, src_layer, src_z) +
                 (VkDeviceSize(r.srcOffset.y / bh + y) * ceil_div(se.width, bw) + r.srcOffset.x / bw) * bytes,
             raw_offset(*dst, r.dstSubresource.mipLevel, dst_layer, dst_z) +
                 (VkDeviceSize(r.dstOffset.y / bh + y) * ceil_div(de.width, bw) + r.dstOffset.x / bw) * bytes,
             VkDeviceSize(ceil_div(r.extent.width, bw)) * bytes});
      }
      vk<PFN_vkCmdCopyBuffer>(d, "vkCmdCopyBuffer")(cmd, src_buffer->buffer, dst_buffer->buffer,
                                                    static_cast<std::uint32_t>(copies.size()), copies.data());
      decode_mip(cmd, dst, r.dstSubresource.mipLevel, dst_layer, dst_z, dest_layout);
    }
  }
}
void VKAPI_CALL copy_buffer_image2(VkCommandBuffer cmd, const VkCopyBufferToImageInfo2* info) {
  if (depth_alias(info->dstImage)) {
    auto regions = plain_regions<VkBufferImageCopy2>(*info);
    return copy_buffer_depth(cmd, info->srcBuffer, info->dstImage, info->dstImageLayout, info->regionCount,
                             regions.data());
  }
  if (!image(info->dstImage))
    return vk<PFN_vkCmdCopyBufferToImage2>(command_device(cmd), "vkCmdCopyBufferToImage2")(cmd, info);
  if (info->pNext) throw std::runtime_error("compressed buffer-image copy chain unsupported");
  std::vector<VkBufferImageCopy> regions;
  for (std::uint32_t i = 0; i < info->regionCount; ++i) {
    auto& r = info->pRegions[i];
    if (r.pNext) throw std::runtime_error("compressed buffer-image region chain unsupported");
    regions.push_back({r.bufferOffset, r.bufferRowLength, r.bufferImageHeight, r.imageSubresource,
                       r.imageOffset, r.imageExtent});
  }
  copy_buffer_image(cmd, info->srcBuffer, info->dstImage, info->dstImageLayout, info->regionCount,
                    regions.data());
}
void VKAPI_CALL copy_image_buffer2(VkCommandBuffer cmd, const VkCopyImageToBufferInfo2* info) {
  if (depth_alias(info->srcImage)) {
    auto regions = plain_regions<VkBufferImageCopy2>(*info);
    return copy_depth_buffer(cmd, info->srcImage, info->srcImageLayout, info->dstBuffer, info->regionCount,
                             regions.data());
  }
  if (!image(info->srcImage))
    return vk<PFN_vkCmdCopyImageToBuffer2>(command_device(cmd), "vkCmdCopyImageToBuffer2")(cmd, info);
  if (info->pNext) throw std::runtime_error("compressed image-buffer copy chain unsupported");
  std::vector<VkBufferImageCopy> regions;
  for (std::uint32_t i = 0; i < info->regionCount; ++i) {
    auto& r = info->pRegions[i];
    if (r.pNext) throw std::runtime_error("compressed image-buffer region chain unsupported");
    regions.push_back({r.bufferOffset, r.bufferRowLength, r.bufferImageHeight, r.imageSubresource,
                       r.imageOffset, r.imageExtent});
  }
  copy_image_buffer(cmd, info->srcImage, info->srcImageLayout, info->dstBuffer, info->regionCount,
                    regions.data());
}
void VKAPI_CALL copy_image2(VkCommandBuffer cmd, const VkCopyImageInfo2* info) {
  if (!image(info->srcImage) && !image(info->dstImage))
    return vk<PFN_vkCmdCopyImage2>(command_device(cmd), "vkCmdCopyImage2")(cmd, info);
  if (info->pNext) throw std::runtime_error("compressed image copy chain unsupported");
  std::vector<VkImageCopy> regions;
  for (std::uint32_t i = 0; i < info->regionCount; ++i) {
    auto& r = info->pRegions[i];
    if (r.pNext) throw std::runtime_error("compressed image copy region chain unsupported");
    regions.push_back({r.srcSubresource, r.srcOffset, r.dstSubresource, r.dstOffset, r.extent});
  }
  copy_image(cmd, info->srcImage, info->srcImageLayout, info->dstImage, info->dstImageLayout,
             info->regionCount, regions.data());
}
std::vector<VkBufferMemoryBarrier> shadow_ownership(VkCommandBuffer cmd, std::uint32_t count,
                                                    const VkImageMemoryBarrier* barriers) {
  std::vector<VkBufferMemoryBarrier> out;
  for (std::uint32_t i = 0; i < count; ++i) {
    auto& barrier = barriers[i];
    auto img = image(barrier.image);
    if (!img || barrier.srcQueueFamilyIndex == barrier.dstQueueFamilyIndex ||
        img->shadow_concurrent || img->info.sharingMode == VK_SHARING_MODE_CONCURRENT)
      continue;
    auto source = source_buffer(img, cmd);
    if (!source) continue;
    const auto& range = barrier.subresourceRange;
    auto levels = range.levelCount == VK_REMAINING_MIP_LEVELS ? img->info.mipLevels - range.baseMipLevel
                                                              : range.levelCount;
    auto layers = range.layerCount == VK_REMAINING_ARRAY_LAYERS ? img->info.arrayLayers - range.baseArrayLayer
                                                                : range.layerCount;
    for (std::uint32_t layer = 0; layer < layers; ++layer)
      for (std::uint32_t level = 0; level < levels; ++level) {
        auto mip = range.baseMipLevel + level;
        VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.srcQueueFamilyIndex = barrier.srcQueueFamilyIndex;
        b.dstQueueFamilyIndex = barrier.dstQueueFamilyIndex;
        b.buffer = source->buffer;
        b.offset = raw_offset(*img, mip, range.baseArrayLayer + layer);
        b.size = slice_bytes(*img, mip) * mip_depth(*img, mip);
        out.push_back(b);
      }
  }
  return out;
}
// Eager companions follow guest layouts and ownership. Sparse companions use
// GENERAL and concurrent sharing so private first-use decoding is independent
// of the guest's current layout and queue family.
template <class Barrier>
std::vector<Barrier> with_companions(std::uint32_t count, const Barrier* barriers) {
  std::vector<Barrier> out;
  for (std::uint32_t i = 0; i < count; ++i) {
    auto img = image(barriers[i].image);
    if (!img || img->companions.empty()) continue;
    if (out.empty()) out.assign(barriers, barriers + count);
    for (auto& companion : img->companions) {
      out.push_back(barriers[i]);
      out.back().image = companion->image;
      if (companion->lazy) {
        out.back().oldLayout = barriers[i].oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ?
            VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
        out.back().newLayout = VK_IMAGE_LAYOUT_GENERAL;
        out.back().srcQueueFamilyIndex = out.back().dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      }
    }
  }
  return out;
}
VkDependencyInfo with_companions(const VkDependencyInfo& info, std::vector<VkImageMemoryBarrier2>& storage) {
  storage = with_companions(info.imageMemoryBarrierCount, info.pImageMemoryBarriers);
  auto copy = info;
  if (!storage.empty()) {
    copy.imageMemoryBarrierCount = static_cast<std::uint32_t>(storage.size());
    copy.pImageMemoryBarriers = storage.data();
  }
  return copy;
}
void VKAPI_CALL barrier(VkCommandBuffer cmd, VkPipelineStageFlags source, VkPipelineStageFlags dest,
                        VkDependencyFlags flags, std::uint32_t memory_count, const VkMemoryBarrier* memory,
                        std::uint32_t buffer_count, const VkBufferMemoryBarrier* buffers,
                        std::uint32_t image_count, const VkImageMemoryBarrier* image_barriers) {
  auto additional = shadow_ownership(cmd, image_count, image_barriers);
  if (!additional.empty()) {
    if (buffer_count) additional.insert(additional.end(), buffers, buffers + buffer_count);
    buffers = additional.data();
    buffer_count = static_cast<std::uint32_t>(additional.size());
    source |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dest |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  }
  auto images = with_companions(image_count, image_barriers);
  if (!images.empty()) {
    image_barriers = images.data();
    image_count = static_cast<std::uint32_t>(images.size());
  }
  vk<PFN_vkCmdPipelineBarrier>(command_device(cmd), "vkCmdPipelineBarrier")(
      cmd, source, dest, flags, memory_count, memory, buffer_count, buffers, image_count, image_barriers);
}
void VKAPI_CALL wait_events(VkCommandBuffer cmd, std::uint32_t event_count, const VkEvent* events,
                            VkPipelineStageFlags source, VkPipelineStageFlags dest,
                            std::uint32_t memory_count, const VkMemoryBarrier* memory,
                            std::uint32_t buffer_count, const VkBufferMemoryBarrier* buffers,
                            std::uint32_t image_count, const VkImageMemoryBarrier* image_barriers) {
  { std::lock_guard lock(texture_lock);
    if (lazy_devices.contains(command_device(cmd))) command_state[cmd].may_wait = true; }
  // Event source stages must match vkCmdSetEvent; don't change those stages.
  // Ownership transfers are prohibited with vkCmdWaitEvents by Vulkan.
  auto additional = shadow_ownership(cmd, image_count, image_barriers);
  if (!additional.empty()) throw std::runtime_error("queue ownership transfer in vkCmdWaitEvents");
  auto images = with_companions(image_count, image_barriers);
  if (!images.empty()) {
    image_barriers = images.data();
    image_count = static_cast<std::uint32_t>(images.size());
  }
  vk<PFN_vkCmdWaitEvents>(command_device(cmd), "vkCmdWaitEvents")(cmd, event_count, events, source, dest,
                                                                  memory_count, memory, buffer_count, buffers,
                                                                  image_count, image_barriers);
}
void VKAPI_CALL barrier2(VkCommandBuffer cmd, const VkDependencyInfo* info) {
  std::vector<VkBufferMemoryBarrier2> buffers;
  if (info->bufferMemoryBarrierCount)
    buffers.assign(info->pBufferMemoryBarriers, info->pBufferMemoryBarriers + info->bufferMemoryBarrierCount);
  for (std::uint32_t i = 0; i < info->imageMemoryBarrierCount; ++i) {
    auto& b = info->pImageMemoryBarriers[i];
    VkImageMemoryBarrier legacy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    legacy.image = b.image;
    legacy.subresourceRange = b.subresourceRange;
    legacy.srcQueueFamilyIndex = b.srcQueueFamilyIndex;
    legacy.dstQueueFamilyIndex = b.dstQueueFamilyIndex;
    for (auto& raw : shadow_ownership(cmd, 1, &legacy)) {
      VkBufferMemoryBarrier2 copy{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
      copy.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      copy.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      copy.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
      copy.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
      copy.srcQueueFamilyIndex = raw.srcQueueFamilyIndex;
      copy.dstQueueFamilyIndex = raw.dstQueueFamilyIndex;
      copy.buffer = raw.buffer;
      copy.offset = raw.offset;
      copy.size = raw.size;
      buffers.push_back(copy);
    }
  }
  std::vector<VkImageMemoryBarrier2> images;
  auto copy = with_companions(*info, images);
  copy.bufferMemoryBarrierCount = static_cast<std::uint32_t>(buffers.size());
  copy.pBufferMemoryBarriers = buffers.data();
  vk<PFN_vkCmdPipelineBarrier2>(command_device(cmd), "vkCmdPipelineBarrier2")(cmd, &copy);
}
// vkCmdWaitEvents2 must name the dependencies its vkCmdSetEvent2 did; both
// gain the same companion barriers.
void VKAPI_CALL set_event2(VkCommandBuffer cmd, VkEvent event, const VkDependencyInfo* info) {
  std::vector<VkImageMemoryBarrier2> images;
  auto copy = with_companions(*info, images);
  vk<PFN_vkCmdSetEvent2>(command_device(cmd), "vkCmdSetEvent2")(cmd, event, &copy);
}
void VKAPI_CALL wait_events2(VkCommandBuffer cmd, std::uint32_t count, const VkEvent* events,
                             const VkDependencyInfo* infos) {
  { std::lock_guard lock(texture_lock);
    if (lazy_devices.contains(command_device(cmd))) command_state[cmd].may_wait = true; }
  std::vector<std::vector<VkImageMemoryBarrier2>> images(count);
  std::vector<VkDependencyInfo> copies;
  for (std::uint32_t i = 0; i < count; ++i) copies.push_back(with_companions(infos[i], images[i]));
  vk<PFN_vkCmdWaitEvents2>(command_device(cmd), "vkCmdWaitEvents2")(cmd, count, events, copies.data());
}
void VKAPI_CALL execute_commands(VkCommandBuffer cmd, std::uint32_t count, const VkCommandBuffer* secondary) {
  vk<PFN_vkCmdExecuteCommands>(command_device(cmd), "vkCmdExecuteCommands")(cmd, count, secondary);
  std::lock_guard lock(texture_lock);
  if (count && lazy_devices.contains(command_device(cmd)))
    command_state[cmd].secondary.insert(command_state[cmd].secondary.end(), secondary, secondary + count);
}
VkResult VKAPI_CALL begin(VkCommandBuffer cmd, const VkCommandBufferBeginInfo* info) {
  auto r = vk<PFN_vkBeginCommandBuffer>(command_device(cmd), "vkBeginCommandBuffer")(cmd, info);
  if (r == VK_SUCCESS) {
    release_texture_commands(cmd);
    release_render_commands(cmd);
    // Without SIMULTANEOUS_USE only one execution can be pending. Reusing
    // outputs across distinct decodes must not introduce cross-execution
    // aliasing when the same command buffer runs on multiple queues.
    // Ordinary graphics-only recordings need no texture bookkeeping.
    if (info->flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT) {
      std::lock_guard lock(texture_lock);
      command_state[cmd].reuse_decoded = false;
    }
  }
  return r;
}
VkResult VKAPI_CALL end(VkCommandBuffer cmd) {
  auto result = vk<PFN_vkEndCommandBuffer>(command_device(cmd), "vkEndCommandBuffer")(cmd);
  std::lock_guard lock(texture_lock);
  auto it = command_state.find(cmd);
  if (it != command_state.end() && it->second.recording_error != VK_SUCCESS)
    return it->second.recording_error;
  return result;
}
VkResult VKAPI_CALL reset(VkCommandBuffer cmd, VkCommandBufferResetFlags flags) {
  auto r = vk<PFN_vkResetCommandBuffer>(command_device(cmd), "vkResetCommandBuffer")(cmd, flags);
  if (r == VK_SUCCESS) {
    release_texture_commands(cmd);
    release_render_commands(cmd);
  }
  return r;
}
void VKAPI_CALL bind_pipeline(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipeline p) {
  auto f = vk<PFN_vkCmdBindPipeline>(command_device(cmd), "vkCmdBindPipeline");
  f(cmd, point, p);
  if (point == VK_PIPELINE_BIND_POINT_COMPUTE) {
    std::lock_guard lock(texture_lock);
    command_state[cmd].compute_state.push_back([=] { f(cmd, point, p); });
  }
}
void VKAPI_CALL bind_sets(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipelineLayout layout,
                          std::uint32_t first, std::uint32_t count, const VkDescriptorSet* sets,
                          std::uint32_t dynamic_count, const std::uint32_t* dynamic) {
  auto f = vk<PFN_vkCmdBindDescriptorSets>(command_device(cmd), "vkCmdBindDescriptorSets");
  f(cmd, point, layout, first, count, sets, dynamic_count, dynamic);
  if (point == VK_PIPELINE_BIND_POINT_COMPUTE) {
    std::vector<VkDescriptorSet> s;
    if (count) s.assign(sets, sets + count);
    std::vector<std::uint32_t> offsets;
    if (dynamic_count) offsets.assign(dynamic, dynamic + dynamic_count);
    std::lock_guard lock(texture_lock);
    command_state[cmd].compute_state.push_back(
        [=] { f(cmd, point, layout, first, count, s.data(), dynamic_count, offsets.data()); });
  }
}
void VKAPI_CALL push_constants(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stages,
                               std::uint32_t offset, std::uint32_t size, const void* values) {
  auto f = vk<PFN_vkCmdPushConstants>(command_device(cmd), "vkCmdPushConstants");
  f(cmd, layout, stages, offset, size, values);
  {
    std::vector<std::byte> data(size);
    std::memcpy(data.data(), values, size);
    std::lock_guard lock(texture_lock);
    command_state[cmd].compute_state.push_back([=] { f(cmd, layout, stages, offset, size, data.data()); });
  }
}
void VKAPI_CALL bind_sets2(VkCommandBuffer cmd, const VkBindDescriptorSetsInfo* info) {
  if (info->pNext) throw std::runtime_error("descriptor set chain unsupported during texture emulation");
  auto f = vk<PFN_vkCmdBindDescriptorSets2>(command_device(cmd), "vkCmdBindDescriptorSets2");
  f(cmd, info);
  if (info->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) {
    auto copy = *info;
    std::vector<VkDescriptorSet> sets;
    if (info->descriptorSetCount)
      sets.assign(info->pDescriptorSets, info->pDescriptorSets + info->descriptorSetCount);
    std::vector<std::uint32_t> offsets;
    if (info->dynamicOffsetCount)
      offsets.assign(info->pDynamicOffsets, info->pDynamicOffsets + info->dynamicOffsetCount);
    std::lock_guard lock(texture_lock);
    command_state[cmd].compute_state.push_back([=]() mutable {
      copy.pDescriptorSets = sets.data();
      copy.pDynamicOffsets = offsets.data();
      f(cmd, &copy);
    });
  }
}
void VKAPI_CALL push_constants2(VkCommandBuffer cmd, const VkPushConstantsInfo* info) {
  push_constants(cmd, info->layout, info->stageFlags, info->offset, info->size, info->pValues);
}
struct PushWrites {
  std::vector<VkWriteDescriptorSet> writes;
  std::vector<std::vector<std::byte>> data;
  Chain chain;
};
bool image_descriptor(VkDescriptorType type) {
  return type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
         type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
         type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
}
bool translate_descriptor(VkDevice d, VkDescriptorImageInfo& info) {
  std::lock_guard lock(texture_lock);
  auto it = companion_views.find({d, info.imageView});
  if (it == companion_views.end() || it->second.expired() || info.imageLayout == VK_IMAGE_LAYOUT_GENERAL)
    return false;
  info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  return true;
}
void translate_write(VkDevice d, VkWriteDescriptorSet& write) {
  if (!image_descriptor(write.descriptorType)) return;
  auto* infos = const_cast<VkDescriptorImageInfo*>(write.pImageInfo);
  for (std::uint32_t i = 0; i < write.descriptorCount; ++i) translate_descriptor(d, infos[i]);
}
void VKAPI_CALL update_descriptors(VkDevice d, std::uint32_t count, const VkWriteDescriptorSet* writes,
                                    std::uint32_t copy_count, const VkCopyDescriptorSet* copies) {
  {
    std::lock_guard lock(texture_lock);
    if (companion_views.empty()) {
      vk<PFN_vkUpdateDescriptorSets>(d, "vkUpdateDescriptorSets")(d, count, writes, copy_count, copies);
      return;
    }
  }
  std::vector<VkWriteDescriptorSet> translated;
  std::vector<std::vector<VkDescriptorImageInfo>> images(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto& write = writes[i];
    if (!image_descriptor(write.descriptorType) || !write.descriptorCount) continue;
    bool changed = false;
    for (std::uint32_t j = 0; j < write.descriptorCount; ++j) {
      auto image = write.pImageInfo[j];
      changed |= translate_descriptor(d, image);
    }
    if (!changed) continue;
    if (translated.empty()) translated.assign(writes, writes + count);
    images[i].assign(write.pImageInfo, write.pImageInfo + write.descriptorCount);
    translated[i].pImageInfo = images[i].data();
    translate_write(d, translated[i]);
  }
  vk<PFN_vkUpdateDescriptorSets>(d, "vkUpdateDescriptorSets")(
      d, count, translated.empty() ? writes : translated.data(), copy_count, copies);
}
std::size_t descriptor_size(VkDescriptorType type) {
  switch (type) {
    case VK_DESCRIPTOR_TYPE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
      return sizeof(VkDescriptorImageInfo);
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
      return sizeof(VkDescriptorBufferInfo);
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
      return sizeof(VkBufferView);
    default:
      throw std::runtime_error("unsupported push descriptor type");
  }
}
void assign_descriptor_data(VkWriteDescriptorSet& write, const void* data) {
  auto size = descriptor_size(write.descriptorType);
  if (size == sizeof(VkBufferView))
    write.pTexelBufferView = static_cast<const VkBufferView*>(data);
  else if (write.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
           write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
           write.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
           write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
    write.pBufferInfo = static_cast<const VkDescriptorBufferInfo*>(data);
  else
    write.pImageInfo = static_cast<const VkDescriptorImageInfo*>(data);
}
std::shared_ptr<PushWrites> copy_writes(VkDevice d, std::uint32_t count, const VkWriteDescriptorSet* writes) {
  auto copy = std::make_shared<PushWrites>();
  if (!count) return copy;
  copy->writes.assign(writes, writes + count);
  copy->data.resize(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    auto& w = copy->writes[i];
    if (w.pNext) throw std::runtime_error("push descriptor extension data is unsupported");
    auto bytes = descriptor_size(w.descriptorType) * w.descriptorCount;
    const void* source = w.pImageInfo;
    if (w.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
        w.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
      source = w.pTexelBufferView;
    else if (w.descriptorType >= VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
             w.descriptorType <= VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
      source = w.pBufferInfo;
    copy->data[i].resize(bytes);
    if (bytes) std::memcpy(copy->data[i].data(), source, bytes);
    assign_descriptor_data(w, copy->data[i].data());
    translate_write(d, w);
  }
  return copy;
}
void record_push(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipelineLayout layout, std::uint32_t set,
                 const std::shared_ptr<PushWrites>& copy) {
  auto f = vk<PFN_vkCmdPushDescriptorSet>(command_device(cmd), "vkCmdPushDescriptorSet");
  auto record = [=] {
    f(cmd, point, layout, set, static_cast<std::uint32_t>(copy->writes.size()), copy->writes.data());
  };
  record();
  if (point == VK_PIPELINE_BIND_POINT_COMPUTE) {
    std::lock_guard lock(texture_lock);
    command_state[cmd].compute_state.push_back(std::move(record));
  }
}
void VKAPI_CALL push_descriptors(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipelineLayout layout,
                                 std::uint32_t set, std::uint32_t count, const VkWriteDescriptorSet* writes) {
  record_push(cmd, point, layout, set, copy_writes(command_device(cmd), count, writes));
}
void VKAPI_CALL push_descriptors2(VkCommandBuffer cmd, const VkPushDescriptorSetInfo* info) {
  auto f = vk<PFN_vkCmdPushDescriptorSet2>(command_device(cmd), "vkCmdPushDescriptorSet2");
  if (info->pNext) throw std::runtime_error("push descriptor info chain unsupported");
  auto writes = copy_writes(command_device(cmd), info->descriptorWriteCount, info->pDescriptorWrites);
  auto copy = *info;
  copy.pDescriptorWrites = writes->writes.data();
  auto record = [=] { f(cmd, &copy); };
  record();
  if (info->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) {
    std::lock_guard lock(texture_lock);
    command_state[cmd].resources.push_back(writes);
    command_state[cmd].compute_state.push_back(std::move(record));
  }
}
VkResult VKAPI_CALL create_template(VkDevice d, const VkDescriptorUpdateTemplateCreateInfo* info,
                                    const VkAllocationCallbacks* alloc, VkDescriptorUpdateTemplate* out) {
  auto r =
      vk<PFN_vkCreateDescriptorUpdateTemplate>(d, "vkCreateDescriptorUpdateTemplate")(d, info, alloc, out);
  if (r == VK_SUCCESS) {
    DescriptorTemplate record;
    if (info->descriptorUpdateEntryCount)
      record.entries.assign(info->pDescriptorUpdateEntries,
                            info->pDescriptorUpdateEntries + info->descriptorUpdateEntryCount);
    record.point = info->pipelineBindPoint;
    std::lock_guard lock(texture_lock);
    descriptor_templates[*out] = std::move(record);
  }
  return r;
}
void VKAPI_CALL destroy_template(VkDevice d, VkDescriptorUpdateTemplate t,
                                 const VkAllocationCallbacks* alloc) {
  vk<PFN_vkDestroyDescriptorUpdateTemplate>(d, "vkDestroyDescriptorUpdateTemplate")(d, t, alloc);
  std::lock_guard lock(texture_lock);
  descriptor_templates.erase(t);
}
void VKAPI_CALL push_template(VkCommandBuffer cmd, VkDescriptorUpdateTemplate t, VkPipelineLayout layout,
                              std::uint32_t set, const void* data) {
  const auto d = command_device(cmd);
  DescriptorTemplate info;
  {
    std::lock_guard lock(texture_lock);
    info = descriptor_templates.at(t);
  }
  auto writes = std::make_shared<PushWrites>();
  writes->data.resize(info.entries.size());
  for (std::size_t i = 0; i < info.entries.size(); ++i) {
    auto& e = info.entries[i];
    auto size = descriptor_size(e.descriptorType);
    auto& payload = writes->data[i];
    payload.resize(size * e.descriptorCount);
    for (std::uint32_t j = 0; j < e.descriptorCount; ++j)
      std::memcpy(payload.data() + j * size, static_cast<const std::byte*>(data) + e.offset + j * e.stride,
                  size);
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstBinding = e.dstBinding;
    w.dstArrayElement = e.dstArrayElement;
    w.descriptorCount = e.descriptorCount;
    w.descriptorType = e.descriptorType;
    assign_descriptor_data(w, payload.data());
    translate_write(d, w);
    writes->writes.push_back(w);
  }
  // Expand now: Vulkan permits destroying a descriptor template immediately
  // after recording its push. Replaying a saved template handle would use freed
  // driver state when another texture upload is recorded later.
  record_push(cmd, info.point, layout, set, writes);
}
void VKAPI_CALL push_template2(VkCommandBuffer cmd, const VkPushDescriptorSetWithTemplateInfo* info) {
  if (info->pNext) throw std::runtime_error("push descriptor template chain unsupported");
  push_template(cmd, info->descriptorUpdateTemplate, info->layout, info->set, info->pData);
}
void VKAPI_CALL update_template(VkDevice d, VkDescriptorSet set, VkDescriptorUpdateTemplate t,
                                const void* data) {
  DescriptorTemplate info;
  {
    std::lock_guard lock(texture_lock);
    if (companion_views.empty()) {
      vk<PFN_vkUpdateDescriptorSetWithTemplate>(d, "vkUpdateDescriptorSetWithTemplate")(d, set, t, data);
      return;
    }
    info = descriptor_templates.at(t);
  }
  bool changed = false;
  for (const auto& entry : info.entries) {
    if (!image_descriptor(entry.descriptorType)) continue;
    for (std::uint32_t i = 0; i < entry.descriptorCount; ++i) {
      VkDescriptorImageInfo image;
      std::memcpy(&image, static_cast<const std::byte*>(data) + entry.offset + i * entry.stride, sizeof(image));
      changed |= translate_descriptor(d, image);
    }
  }
  if (!changed) {
    vk<PFN_vkUpdateDescriptorSetWithTemplate>(d, "vkUpdateDescriptorSetWithTemplate")(d, set, t, data);
    return;
  }
  std::size_t bytes = 0;
  for (const auto& entry : info.entries) {
    if (!entry.descriptorCount) continue;
    auto size = entry.descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK ? std::size_t(1) :
        entry.descriptorType == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR ? sizeof(VkAccelerationStructureKHR) :
        entry.descriptorType == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV ? sizeof(VkAccelerationStructureNV) :
        descriptor_size(entry.descriptorType);
    auto stride = entry.descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK ? std::size_t(1) : entry.stride;
    bytes = std::max(bytes, entry.offset + (entry.descriptorCount - 1) * stride + size);
  }
  std::vector<std::byte> payload(bytes);
  if (bytes) std::memcpy(payload.data(), data, bytes);
  for (const auto& entry : info.entries) {
    if (!image_descriptor(entry.descriptorType)) continue;
    for (std::uint32_t i = 0; i < entry.descriptorCount; ++i) {
      auto* slot = payload.data() + entry.offset + i * entry.stride;
      VkDescriptorImageInfo image;
      std::memcpy(&image, slot, sizeof(image));
      translate_descriptor(d, image);
      std::memcpy(slot, &image, sizeof(image));
    }
  }
  vk<PFN_vkUpdateDescriptorSetWithTemplate>(d, "vkUpdateDescriptorSetWithTemplate")(d, set, t, payload.data());
}
void VKAPI_CALL format_properties(VkPhysicalDevice p, VkFormat f, VkFormatProperties* out) {
  auto query = phys<PFN_vkGetPhysicalDeviceFormatProperties>(p, "vkGetPhysicalDeviceFormatProperties");
  query(p, f, out);
  if (emulated(p, f)) {
    VkFormatProperties backing{};
    query(p, texture_format(f).decoded, &backing);
    out->linearTilingFeatures = 0;
    out->bufferFeatures = 0;
    out->optimalTilingFeatures =
        backing.optimalTilingFeatures &
        (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
         VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
  }
  if (f == VK_FORMAT_R8G8_UNORM && emulated_density_maps(p))
    out->optimalTilingFeatures |= VK_FORMAT_FEATURE_FRAGMENT_DENSITY_MAP_BIT_EXT;
}
void VKAPI_CALL format_properties2(VkPhysicalDevice p, VkFormat f, VkFormatProperties2* out) {
  phys<PFN_vkGetPhysicalDeviceFormatProperties2>(p, "vkGetPhysicalDeviceFormatProperties2")(p, f, out);
  format_properties(p, f, &out->formatProperties);
  if (f == VK_FORMAT_R8G8_UNORM && emulated_density_maps(p))
    for (auto* next = static_cast<VkBaseOutStructure*>(out->pNext); next; next = next->pNext)
      if (next->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3)
        reinterpret_cast<VkFormatProperties3*>(next)->optimalTilingFeatures |=
            VK_FORMAT_FEATURE_2_FRAGMENT_DENSITY_MAP_BIT_EXT;
  if (emulated(p, f))
    for (auto* next = static_cast<VkBaseOutStructure*>(out->pNext); next; next = next->pNext) {
      if (next->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {
        auto& p3 = *reinterpret_cast<VkFormatProperties3*>(next);
        p3.linearTilingFeatures = out->formatProperties.linearTilingFeatures;
        p3.optimalTilingFeatures = out->formatProperties.optimalTilingFeatures;
        p3.bufferFeatures = 0;
      }
    }
}
// `chain` is the guest VkPhysicalDeviceImageFormatInfo2 chain, or nullptr.
VkResult image_properties_chained(VkPhysicalDevice p, VkFormat f, VkImageType type, VkImageTiling tiling,
                                  VkImageUsageFlags usage, VkImageCreateFlags flags, const void* chain,
                                  VkImageFormatProperties* out) {
  if (emulated_density_maps(p)) {
    // A density-only image is created as a sampled image (chain.cpp).
    usage &= ~VkImageUsageFlags(VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT);
    if (!usage) usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    flags &= ~VkImageCreateFlags(VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT);
  }
  const bool needs_decode = emulated(p, f);
  if (needs_decode && tiling != VK_IMAGE_TILING_OPTIMAL) return VK_ERROR_FORMAT_NOT_SUPPORTED;
  if (needs_decode) {
    VkImageCreateInfo guest{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, chain, flags, type, f, {1, 1, 1}, 1, 1,
                            VK_SAMPLE_COUNT_1_BIT, tiling, usage};
    VkImageCreateInfo host{};
    TextureStorage storage;
    if (!host_image_info(guest, storage, host)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    f = host.format;
    usage = host.usage;
    flags = host.flags;
  }
  auto r = phys<PFN_vkGetPhysicalDeviceImageFormatProperties>(p, "vkGetPhysicalDeviceImageFormatProperties")(
      p, f, type, tiling, usage, flags, out);
  if (needs_decode && r == VK_SUCCESS) {
    VkPhysicalDeviceProperties props{};
    phys<PFN_vkGetPhysicalDeviceProperties>(p, "vkGetPhysicalDeviceProperties")(p, &props);
    out->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
    out->maxResourceSize = std::min(out->maxResourceSize, VkDeviceSize(props.limits.maxStorageBufferRange));
  }
  return r;
}
VkResult VKAPI_CALL image_properties(VkPhysicalDevice p, VkFormat f, VkImageType type, VkImageTiling tiling,
                                     VkImageUsageFlags usage, VkImageCreateFlags flags,
                                     VkImageFormatProperties* out) {
  return image_properties_chained(p, f, type, tiling, usage, flags, nullptr, out);
}
VkResult VKAPI_CALL image_properties2(VkPhysicalDevice p, const VkPhysicalDeviceImageFormatInfo2* info,
                                      VkImageFormatProperties2* out) {
  if (!emulated(p, info->format)) {
    auto host = *info;
    if (emulated_density_maps(p)) {
      host.usage &= ~VkImageUsageFlags(VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT);
      if (!host.usage) host.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
      host.flags &= ~VkImageCreateFlags(VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT);
    }
    auto result = phys<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
        p, "vkGetPhysicalDeviceImageFormatProperties2")(p, &host, out);
    if (result == VK_SUCCESS) external_image_properties(out);
    return result;
  }
  // Only a format list may extend the query; it narrows the views as at creation.
  if (out->pNext) return VK_ERROR_FORMAT_NOT_SUPPORTED;
  return image_properties_chained(p, info->format, info->type, info->tiling, info->usage, info->flags,
                                  info->pNext, &out->imageFormatProperties);
}
void VKAPI_CALL features(VkPhysicalDevice p, VkPhysicalDeviceFeatures* out) {
  phys<PFN_vkGetPhysicalDeviceFeatures>(p, "vkGetPhysicalDeviceFeatures")(p, out);
  guest_texture_features(p, *out);
}
void VKAPI_CALL features2(VkPhysicalDevice p, VkPhysicalDeviceFeatures2* out) {
  phys<PFN_vkGetPhysicalDeviceFeatures2>(p, "vkGetPhysicalDeviceFeatures2")(p, out);
  guest_texture_features(p, out->features);
  for (auto* next = static_cast<VkBaseOutStructure*>(out->pNext); next; next = next->pNext) {
    if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES)
      reinterpret_cast<VkPhysicalDeviceVulkan14Features*>(next)->hostImageCopy = VK_FALSE;
    if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES)
      reinterpret_cast<VkPhysicalDeviceHostImageCopyFeatures*>(next)->hostImageCopy = VK_FALSE;
    if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT && emulated_density_maps(p)) {
      auto& density = *reinterpret_cast<VkPhysicalDeviceFragmentDensityMapFeaturesEXT*>(next);
      density.fragmentDensityMap = VK_TRUE;
      density.fragmentDensityMapDynamic = VK_FALSE;
      density.fragmentDensityMapNonSubsampledImages = VK_TRUE;
    }
  }
}
}  // namespace

struct DecodeResources {
  std::shared_ptr<Pipeline> pipeline;
  VkDescriptorPool pool{};
  PFN_vkDestroyDescriptorPool destroy_pool{};
  ~DecodeResources() {
    if (pool) destroy_pool(pipeline->device, pool, nullptr);
  }
};
void configure_lazy_texture_device(VkDevice d, VkQueue queue, std::uint32_t family,
                                    const VkPhysicalDeviceFeatures& features) {
  std::lock_guard lock(texture_lock);
  auto& config = lazy_devices[d];
  config.queue = queue; config.family = family; config.features = features;
  config.wait_idle = reinterpret_cast<PFN_vkQueueWaitIdle>(cleanup_device_proc(d, "vkQueueWaitIdle"));
  if (!config.wait_idle) throw std::runtime_error("missing Vulkan cleanup command vkQueueWaitIdle");
}
void ensure_texture_source_test_queue(VkDevice d, VkQueue queue, std::uint32_t family) {
  std::lock_guard lock(texture_lock);
  // Preserve real private-queue features, including sparse companion coverage.
  if (lazy_devices.contains(d)) return;
  auto& config = lazy_devices[d];
  config.queue = queue; config.family = family;
  config.wait_idle = reinterpret_cast<PFN_vkQueueWaitIdle>(cleanup_device_proc(d, "vkQueueWaitIdle"));
  if (!config.wait_idle) throw std::runtime_error("missing Vulkan cleanup command vkQueueWaitIdle");
}
namespace {
void gather_uploads(VkCommandBuffer cmd, std::vector<Commands::Upload>& uploads, bool& may_wait,
                    std::set<VkCommandBuffer>& visited) {
  if (!visited.insert(cmd).second) return;
  auto it = command_state.find(cmd);
  if (it == command_state.end()) return;
  uploads.insert(uploads.end(), it->second.uploads.begin(), it->second.uploads.end());
  may_wait |= it->second.may_wait;
  for (auto secondary : it->second.secondary) gather_uploads(secondary, uploads, may_wait, visited);
}
bool inactive_companion(const Image& img) {
  return std::ranges::any_of(img.companions, [](const auto& c) { return c->lazy && !c->active; });
}
VkResult retire_producer(LazyTextureDevice& config, const std::shared_ptr<TextureProducer>& producer) {
  if (producer->fence_submitted) return VK_SUCCESS;
  VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  info.waitSemaphoreCount = 1; info.pWaitSemaphores = &producer->semaphore; info.pWaitDstStageMask = &stage;
  auto mutex = gfx::queue_mutex(config.queue);
  std::lock_guard queue_lock(*mutex);
  auto r = vk<PFN_vkQueueSubmit>(producer->device, "vkQueueSubmit")(config.queue, 1, &info, producer->fence);
  if (r == VK_SUCCESS) { producer->fence_submitted = true; producer->consumed = true; }
  return r;
}

VkResult materialize_companion_impl(const std::shared_ptr<Image>& img, std::size_t plane) {
  auto& companion = *img->companions.at(plane - 1);
  if (!companion.lazy || companion.active) return VK_SUCCESS;
  const auto d = img->device;
  auto& config = lazy_devices.at(d);
  // Only submissions without unresolved guest waits are deferred. Waiting
  // for their private fences cannot depend on a later guest API call.
  for (const auto& [queue, producer] : img->producers) {
    if (auto r = retire_producer(config, producer); r != VK_SUCCESS) return r;
    if (auto r = vk<PFN_vkWaitForFences>(d, "vkWaitForFences")(
            d, 1, &producer->fence, VK_TRUE, UINT64_MAX); r != VK_SUCCESS) return r;
  }
  auto bindings = sparse_image_bindings(img->info, companion.requirements, companion.sparse);
  if (!companion.memory) {
    auto requirements = companion.requirements;
    requirements.size = bindings.bytes;
    if (auto r = allocate(d, requirements, &companion.memory); r != VK_SUCCESS) return r;
  }
  for (auto& tile : bindings.tiles) tile.memory = companion.memory;
  for (auto& tail : bindings.tails) tail.memory = companion.memory;
  VkSparseImageMemoryBindInfo tiles{companion.image,
      static_cast<std::uint32_t>(bindings.tiles.size()), bindings.tiles.data()};
  VkSparseImageOpaqueMemoryBindInfo tails{companion.image,
      static_cast<std::uint32_t>(bindings.tails.size()), bindings.tails.data()};
  VkBindSparseInfo bind{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
  bind.imageBindCount = bindings.tiles.empty() ? 0 : 1;
  bind.pImageBinds = &tiles;
  bind.imageOpaqueBindCount = bindings.tails.empty() ? 0 : 1;
  bind.pImageOpaqueBinds = &tails;
  auto source = img->uploaded.empty() ? nullptr : source_buffer(img);
  struct Initialization {
    VkDevice d;
    VkQueue queue;
    std::shared_ptr<Buffer> activation;
    VkFence fence{};
    VkCommandPool pool{};
    bool submitted = false;
    bool complete = false;
    bool activated = false;
    std::vector<std::shared_ptr<DecodeResources>> decodes;
    ~Initialization() {
      if (submitted && !complete) {
        auto mutex = gfx::queue_mutex(queue);
        std::lock_guard lock(*mutex);
        vk<PFN_vkQueueWaitIdle>(d, "vkQueueWaitIdle")(queue);
      }
      if (!activated) write_activation(activation, 0);
      if (pool) vk<PFN_vkDestroyCommandPool>(d, "vkDestroyCommandPool")(d, pool, nullptr);
      if (fence) vk<PFN_vkDestroyFence>(d, "vkDestroyFence")(d, fence, nullptr);
    }
  } init{d, config.queue, companion.activation};
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (auto r = vk<PFN_vkCreateFence>(d, "vkCreateFence")(d, &fi, nullptr, &init.fence);
      r != VK_SUCCESS) return r;
  {
    auto mutex = gfx::queue_mutex(config.queue);
    std::lock_guard queue_lock(*mutex);
    if (auto r = vk<PFN_vkQueueBindSparse>(d, "vkQueueBindSparse")(
            config.queue, 1, &bind, init.fence); r != VK_SUCCESS) return r;
    init.submitted = true;
  }
  // Sparse binding is not ordered against command-buffer execution without
  // synchronization. Finish it before submitting the initialization dispatch.
  if (auto r = vk<PFN_vkWaitForFences>(d, "vkWaitForFences")(
          d, 1, &init.fence, VK_TRUE, UINT64_MAX); r != VK_SUCCESS) return r;
  init.complete = true;
  if (auto r = vk<PFN_vkResetFences>(d, "vkResetFences")(d, 1, &init.fence);
      r != VK_SUCCESS) return r;
  VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pi.queueFamilyIndex = config.family;
  if (auto r = vk<PFN_vkCreateCommandPool>(d, "vkCreateCommandPool")(d, &pi, nullptr, &init.pool);
      r != VK_SUCCESS) return r;
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = init.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
  VkCommandBuffer cmd{};
  if (auto r = vk<PFN_vkAllocateCommandBuffers>(d, "vkAllocateCommandBuffers")(d, &ai, &cmd);
      r != VK_SUCCESS) return r;
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (auto r = vk<PFN_vkBeginCommandBuffer>(d, "vkBeginCommandBuffer")(cmd, &bi);
      r != VK_SUCCESS) return r;
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = companion.image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, img->info.mipLevels, 0, img->info.arrayLayers};
  barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  vk<PFN_vkCmdPipelineBarrier>(d, "vkCmdPipelineBarrier")(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  if (auto r = write_activation(companion.activation, 1); r != VK_SUCCESS) return r;
  for (const auto& [mip, layer, z] : img->uploaded) {
    auto extent = mip_extent(*img, mip);
    DecodeRegion region{texture_format(img->storage.planes[plane].decode), extent.width, extent.height,
        ceil_div(extent.width, img->format.width),
        static_cast<std::uint32_t>(raw_offset(*img, mip, layer, z) / 4)};
    region.format.decoded = img->storage.planes[plane].format;
    region.format.texel_bytes = img->storage.planes[plane].texel_bytes;
    init.decodes.push_back(record_decode_image(d, cmd, source->buffer, source->size,
        companion.storage_views.at(mip), companion.activation->buffer, region,
        img->info.imageType == VK_IMAGE_TYPE_3D, img->info.imageType == VK_IMAGE_TYPE_3D ? z : layer));
  }
  memory_barrier(d, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
  auto r = vk<PFN_vkEndCommandBuffer>(d, "vkEndCommandBuffer")(cmd);
  if (r == VK_SUCCESS) {
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
    auto mutex = gfx::queue_mutex(config.queue);
    std::lock_guard queue_lock(*mutex);
    r = vk<PFN_vkQueueSubmit>(d, "vkQueueSubmit")(config.queue, 1, &submit, init.fence);
    if (r == VK_SUCCESS) { init.submitted = true; init.complete = false; }
  }
  if (r == VK_SUCCESS)
    r = vk<PFN_vkWaitForFences>(d, "vkWaitForFences")(d, 1, &init.fence, VK_TRUE, UINT64_MAX);
  if (r != VK_SUCCESS) return r;
  init.complete = init.activated = true;
  companion.active = true;
  if (!inactive_companion(*img)) img->producers.clear();
  return VK_SUCCESS;
}

VkResult materialize_companion(const std::shared_ptr<Image>& img, std::size_t plane) {
  try { return materialize_companion_impl(img, plane); }
  catch (const TextureFailure& failure) { return failure.result; }
}
template<class Submit, class Native>
VkResult submit_textures(VkQueue queue, std::uint32_t count, const Submit* submits,
                         VkFence fence, Native native) {
  auto binding = queue_binding(queue);
  const auto d = binding ? binding->device : VK_NULL_HANDLE;
  std::unique_lock lock(texture_lock);
  auto device = lazy_devices.find(d);
  auto submit = [&](const Submit* infos) {
    if constexpr (std::is_same_v<Submit, VkSubmitInfo>)
      return timed_submit(queue, count, infos, fence, native);
    else return timed_submit2(queue, count, infos, fence, native);
  };
  if (device == lazy_devices.end()) { lock.unlock(); return submit(submits); }
  auto& config = device->second;
  auto [finite_queue, inserted] = config.finite_queues.try_emplace(queue, true);
  bool finite = finite_queue->second;
  std::vector<Commands::Upload> uploads;
  std::set<VkCommandBuffer> visited;
  for (std::uint32_t i = 0; i < count; ++i) {
    bool may_wait = false;
    if constexpr (std::is_same_v<Submit, VkSubmitInfo>) {
      may_wait = submits[i].waitSemaphoreCount != 0;
      for (std::uint32_t j = 0; j < submits[i].commandBufferCount; ++j)
        gather_uploads(submits[i].pCommandBuffers[j], uploads, may_wait, visited);
    } else {
      may_wait = submits[i].waitSemaphoreInfoCount != 0;
      for (std::uint32_t j = 0; j < submits[i].commandBufferInfoCount; ++j)
        gather_uploads(submits[i].pCommandBufferInfos[j].commandBuffer, uploads, may_wait, visited);
    }
    finite &= !may_wait;
  }
  // Before a potentially blocked producer enters a guest queue, back its
  // companions. Existing deferred producers have no unresolved guest waits.
  if (!finite) for (const auto& upload : uploads) {
    for (std::size_t plane = 1; plane < upload.image->storage.planes.size(); ++plane)
      if (auto r = materialize_companion(upload.image, plane); r != VK_SUCCESS) return r;
  }
  std::shared_ptr<TextureProducer> producer;
  std::vector<Submit> copies;
  std::vector<VkSemaphore> signals;
  std::vector<VkSemaphoreSubmitInfo> signals2;
  std::vector<std::uint64_t> signal_values;
  std::vector<std::uint32_t> signal_devices;
  Chain chain;
  if (finite && std::ranges::any_of(uploads, [](const auto& u) { return inactive_companion(*u.image); })) {
    producer = std::make_shared<TextureProducer>();
    producer->device = d;
    producer->destroy_fence = vk<PFN_vkDestroyFence>(d, "vkDestroyFence");
    producer->destroy_semaphore = vk<PFN_vkDestroySemaphore>(d, "vkDestroySemaphore");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (auto r = vk<PFN_vkCreateFence>(d, "vkCreateFence")(d, &fi, nullptr, &producer->fence);
        r != VK_SUCCESS) return r;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (auto r = vk<PFN_vkCreateSemaphore>(d, "vkCreateSemaphore")(d, &si, nullptr, &producer->semaphore);
        r != VK_SUCCESS) return r;
    copies.assign(submits, submits + count);
    auto& last = copies.back();
    if constexpr (std::is_same_v<Submit, VkSubmitInfo>) {
      if (last.signalSemaphoreCount) signals.assign(last.pSignalSemaphores,
          last.pSignalSemaphores + last.signalSemaphoreCount);
      signals.push_back(producer->semaphore);
      last.pSignalSemaphores = signals.data(); last.signalSemaphoreCount++;
      last.pNext = chain.translate(last.pNext);
      if (chain.result() != VK_SUCCESS) return chain.result();
      for (auto* node = const_cast<VkBaseOutStructure*>(static_cast<const VkBaseOutStructure*>(last.pNext));
           node; node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
          auto& timeline = *reinterpret_cast<VkTimelineSemaphoreSubmitInfo*>(node);
          if (timeline.signalSemaphoreValueCount) signal_values.assign(timeline.pSignalSemaphoreValues,
              timeline.pSignalSemaphoreValues + timeline.signalSemaphoreValueCount);
          signal_values.push_back(0);
          timeline.signalSemaphoreValueCount++; timeline.pSignalSemaphoreValues = signal_values.data();
        } else if (node->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO) {
          auto& group = *reinterpret_cast<VkDeviceGroupSubmitInfo*>(node);
          if (group.signalSemaphoreCount) signal_devices.assign(group.pSignalSemaphoreDeviceIndices,
              group.pSignalSemaphoreDeviceIndices + group.signalSemaphoreCount);
          signal_devices.push_back(0);
          group.signalSemaphoreCount++; group.pSignalSemaphoreDeviceIndices = signal_devices.data();
        }
      }
    } else {
      if (last.signalSemaphoreInfoCount) signals2.assign(last.pSignalSemaphoreInfos,
          last.pSignalSemaphoreInfos + last.signalSemaphoreInfoCount);
      VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
      signal.semaphore = producer->semaphore; signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      signals2.push_back(signal);
      last.pSignalSemaphoreInfos = signals2.data(); last.signalSemaphoreInfoCount++;
    }
  }
  auto r = submit(copies.empty() ? submits : copies.data());
  if (r != VK_SUCCESS) return r;
  finite_queue->second = finite;
  for (const auto& upload : uploads) {
    upload.image->uploaded.emplace(upload.mip, upload.layer, upload.z);
    if (producer && inactive_companion(*upload.image)) upload.image->producers[queue] = producer;
  }
  if (producer) {
    config.pending.push_back(producer);
    // Failure here does not invalidate a successful guest submission. Keep
    // its signal alive and retry the private wait at first use instead.
    retire_producer(config, producer);
  }
  std::erase_if(config.pending, [&](const auto& p) {
    return p->fence_submitted && vk<PFN_vkGetFenceStatus>(d, "vkGetFenceStatus")(d, p->fence) == VK_SUCCESS;
  });
  return r;
}
} // namespace
VkResult texture_submit(VkQueue queue, std::uint32_t count, const VkSubmitInfo* submits,
                        VkFence fence, PFN_vkQueueSubmit native) {
  auto result = submit_textures(queue, count, submits, fence, native);
  if (have_pageable_sources.load(std::memory_order_relaxed) && result == VK_SUCCESS) {
    auto binding = queue_binding(queue);
    if (!binding) return result;
    try { retire_sources(binding->device, false); }
    catch (...) { /* Retirement must not fail an already successful submission. */ }
  }
  return result;
}
VkResult texture_submit2(VkQueue queue, std::uint32_t count, const VkSubmitInfo2* submits,
                         VkFence fence, PFN_vkQueueSubmit2 native) {
  auto result = submit_textures(queue, count, submits, fence, native);
  if (have_pageable_sources.load(std::memory_order_relaxed) && result == VK_SUCCESS) {
    auto binding = queue_binding(queue);
    if (!binding) return result;
    try { retire_sources(binding->device, false); }
    catch (...) { /* Retirement must not fail an already successful submission. */ }
  }
  return result;
}
void force_pageable_texture_sources(bool forced) { pageable_sources_forced.store(forced); }
void trim_texture_sources(VkDevice device) { retire_sources(device, true); }
void wait_texture_sources(VkDevice device) {
  std::shared_future<void> pending;
  {
    std::lock_guard lock(texture_lock);
    if (auto it = source_workers.find(device); it != source_workers.end()) pending = it->second.pending;
  }
  if (pending.valid()) pending.wait();
}
bool texture_source_gpu_resident(VkImage handle) {
  std::lock_guard lock(texture_lock);
  auto img = image(handle);
  return img && bool(img->shadow);
}
void set_texture_source_copy_hook(std::function<void()> hook) {
  std::lock_guard lock(texture_lock);
  source_copy_hook = std::move(hook);
}
void set_texture_source_worker_hook(std::function<void()> hook) {
  std::lock_guard lock(texture_lock);
  source_worker_hook = std::move(hook);
}
std::size_t texture_source_scan_count(VkDevice device) {
  std::lock_guard lock(texture_lock);
  return source_workers.at(device).scanned;
}
void texture_queue_idle(VkQueue queue) {
  std::lock_guard lock(texture_lock);
  auto binding = queue_binding(queue);
  if (binding && lazy_devices.contains(binding->device)) lazy_devices.at(binding->device).finite_queues[queue] = true;
}
void texture_queue_uncertain(VkQueue queue) {
  std::lock_guard lock(texture_lock);
  auto binding = queue_binding(queue);
  if (binding && lazy_devices.contains(binding->device)) lazy_devices.at(binding->device).finite_queues[queue] = false;
}
std::shared_ptr<DecodeResources> record_decode(VkDevice d, VkCommandBuffer cmd, VkBuffer source,
                                               VkDeviceSize source_size, VkBuffer dest,
                                               VkDeviceSize dest_size, const DecodeRegion& region) {
  auto resources = std::make_shared<DecodeResources>();
  resources->pipeline = pipeline(d);
  resources->destroy_pool = vk<PFN_vkDestroyDescriptorPool>(d, "vkDestroyDescriptorPool");
  auto& p = *resources->pipeline;
  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
  VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pi.maxSets = 1;
  pi.poolSizeCount = 1;
  pi.pPoolSizes = &size;
  checked(vk<PFN_vkCreateDescriptorPool>(d, "vkCreateDescriptorPool")(d, &pi, nullptr, &resources->pool));
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = resources->pool;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &p.descriptors;
  VkDescriptorSet set{};
  checked(vk<PFN_vkAllocateDescriptorSets>(d, "vkAllocateDescriptorSets")(d, &ai, &set));
  VkDescriptorBufferInfo buffers[2] = {{source, 0, source_size}, {dest, 0, dest_size}};
  VkWriteDescriptorSet writes[2]{};
  for (std::uint32_t i = 0; i < 2; ++i) {
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &buffers[i];
  }
  vk<PFN_vkUpdateDescriptorSets>(d, "vkUpdateDescriptorSets")(d, 2, writes, 0, nullptr);
  vk<PFN_vkCmdBindPipeline>(d, "vkCmdBindPipeline")(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
  vk<PFN_vkCmdBindDescriptorSets>(d, "vkCmdBindDescriptorSets")(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout,
                                                                0, 1, &set, 0, nullptr);
  std::uint32_t push[8] = {static_cast<std::uint32_t>(region.format.compressed),
                           region.format.width,
                           region.format.height,
                           region.width,
                           region.height,
                           region.row_blocks,
                           region.source_word,
                           region.format.texel_bytes == 8 ? 0x80000000u : 0u};
  vk<PFN_vkCmdPushConstants>(d, "vkCmdPushConstants")(cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                                      sizeof(push), push);
  vk<PFN_vkCmdDispatch>(d, "vkCmdDispatch")(cmd, ceil_div(region.width, 8), ceil_div(region.height, 8), 1);
  return resources;
}
std::shared_ptr<DecodeResources> record_encode_bc7(VkDevice d, VkCommandBuffer cmd, VkBuffer source,
    VkDeviceSize source_size, VkBuffer dest, VkDeviceSize dest_size, std::uint32_t width, std::uint32_t height) {
  auto resources = std::make_shared<DecodeResources>();
  resources->pipeline = image_pipeline(d, bc7_mode);
  resources->destroy_pool = vk<PFN_vkDestroyDescriptorPool>(d, "vkDestroyDescriptorPool");
  auto& p = *resources->pipeline;
  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
  VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pi.maxSets = 1; pi.poolSizeCount = 1; pi.pPoolSizes = &size;
  checked(vk<PFN_vkCreateDescriptorPool>(d, "vkCreateDescriptorPool")(d, &pi, nullptr, &resources->pool));
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = resources->pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &p.descriptors;
  VkDescriptorSet set{};
  checked(vk<PFN_vkAllocateDescriptorSets>(d, "vkAllocateDescriptorSets")(d, &ai, &set));
  VkDescriptorBufferInfo buffers[2] = {{source, 0, source_size}, {dest, 0, dest_size}};
  VkWriteDescriptorSet writes[2]{};
  for (std::uint32_t i = 0; i < 2; ++i) {
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[i].dstSet = set; writes[i].dstBinding = i; writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &buffers[i];
  }
  vk<PFN_vkUpdateDescriptorSets>(d, "vkUpdateDescriptorSets")(d, 2, writes, 0, nullptr);
  vk<PFN_vkCmdBindPipeline>(d, "vkCmdBindPipeline")(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
  vk<PFN_vkCmdBindDescriptorSets>(d, "vkCmdBindDescriptorSets")(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout,
                                                                0, 1, &set, 0, nullptr);
  std::uint32_t push[8] = {width, height};
  vk<PFN_vkCmdPushConstants>(d, "vkCmdPushConstants")(cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                                      sizeof(push), push);
  vk<PFN_vkCmdDispatch>(d, "vkCmdDispatch")(cmd, ceil_div(ceil_div(width, 4), 8), ceil_div(ceil_div(height, 4), 8), 1);
  return resources;
}
std::shared_ptr<DecodeResources> record_decode_image(VkDevice d, VkCommandBuffer cmd, VkBuffer source,
    VkDeviceSize source_size, VkImageView destination, VkBuffer activation, const DecodeRegion& region,
    bool volume, std::uint32_t slice) {
  if (region.format.compressed < VK_FORMAT_ASTC_4x4_UNORM_BLOCK ||
      region.format.compressed > VK_FORMAT_ASTC_12x12_SRGB_BLOCK)
    throw std::runtime_error("direct image decode requires ASTC");
  auto resources = std::make_shared<DecodeResources>();
  const bool srgb = region.format.decoded == VK_FORMAT_R8G8B8A8_SRGB;
  resources->pipeline = image_pipeline(d, region.format.decoded == VK_FORMAT_R8G8B8A8_UNORM
      ? (volume ? 6u : 5u) : (volume ? 3u : 1u) + unsigned(srgb));
  resources->destroy_pool = vk<PFN_vkDestroyDescriptorPool>(d, "vkDestroyDescriptorPool");
  auto& p = *resources->pipeline;
  VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
  VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pi.maxSets = 1; pi.poolSizeCount = 2; pi.pPoolSizes = sizes;
  checked(vk<PFN_vkCreateDescriptorPool>(d, "vkCreateDescriptorPool")(d, &pi, nullptr, &resources->pool));
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = resources->pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &p.descriptors;
  VkDescriptorSet set{};
  checked(vk<PFN_vkAllocateDescriptorSets>(d, "vkAllocateDescriptorSets")(d, &ai, &set));
  VkDescriptorBufferInfo buffers[] = {{source, 0, source_size}, {activation, 0, sizeof(std::uint32_t)}};
  VkDescriptorImageInfo image{VK_NULL_HANDLE, destination, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[3]{};
  for (unsigned i = 0; i < 3; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set; writes[i].dstBinding = i; writes[i].descriptorCount = 1;
    writes[i].descriptorType = i == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if (i == 1) writes[i].pImageInfo = &image;
    else writes[i].pBufferInfo = &buffers[i == 0 ? 0 : 1];
  }
  vk<PFN_vkUpdateDescriptorSets>(d, "vkUpdateDescriptorSets")(d, 3, writes, 0, nullptr);
  vk<PFN_vkCmdBindPipeline>(d, "vkCmdBindPipeline")(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
  vk<PFN_vkCmdBindDescriptorSets>(d, "vkCmdBindDescriptorSets")(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
      p.layout, 0, 1, &set, 0, nullptr);
  std::uint32_t push[] = {unsigned(region.format.compressed), region.format.width, region.format.height,
      region.width, region.height, region.row_blocks, region.source_word, slice};
  vk<PFN_vkCmdPushConstants>(d, "vkCmdPushConstants")(cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT,
      0, sizeof(push), push);
  vk<PFN_vkCmdDispatch>(d, "vkCmdDispatch")(cmd, ceil_div(region.width, 8), ceil_div(region.height, 8), 1);
  return resources;
}
SparseImageBindings sparse_image_bindings(const VkImageCreateInfo& image,
    const VkMemoryRequirements& memory, const VkSparseImageMemoryRequirements& sparse) {
  SparseImageBindings plan;
  const auto granularity = sparse.formatProperties.imageGranularity;
  if (!memory.alignment || !granularity.width || !granularity.height || !granularity.depth ||
      sparse.formatProperties.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT)
    throw std::runtime_error("unsupported sparse image geometry");
  auto align = [&](VkDeviceSize size) { return (size + memory.alignment - 1) / memory.alignment * memory.alignment; };
  for (std::uint32_t layer = 0; layer < image.arrayLayers; ++layer) {
    for (std::uint32_t mip = 0; mip < std::min(image.mipLevels, sparse.imageMipTailFirstLod); ++mip) {
      VkExtent3D extent{std::max(1u, image.extent.width >> mip),
                        std::max(1u, image.extent.height >> mip),
                        std::max(1u, image.extent.depth >> mip)};
      for (std::uint32_t z = 0; z < extent.depth; z += granularity.depth)
        for (std::uint32_t y = 0; y < extent.height; y += granularity.height)
          for (std::uint32_t x = 0; x < extent.width; x += granularity.width) {
            VkSparseImageMemoryBind bind{};
            bind.subresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer};
            bind.offset = {std::int32_t(x), std::int32_t(y), std::int32_t(z)};
            bind.extent = {std::min(granularity.width, extent.width - x),
                           std::min(granularity.height, extent.height - y),
                           std::min(granularity.depth, extent.depth - z)};
            bind.memoryOffset = plan.bytes;
            plan.tiles.push_back(bind);
            plan.bytes += memory.alignment;
          }
    }
    if (sparse.imageMipTailFirstLod < image.mipLevels && sparse.imageMipTailSize &&
        (!(sparse.formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT) || layer == 0)) {
      VkSparseMemoryBind tail{};
      tail.resourceOffset = sparse.imageMipTailOffset;
      if (!(sparse.formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT))
        tail.resourceOffset += layer * sparse.imageMipTailStride;
      tail.size = sparse.imageMipTailSize;
      plan.bytes = align(plan.bytes);
      tail.memoryOffset = plan.bytes;
      plan.tails.push_back(tail);
      plan.bytes += tail.size;
    }
  }
  plan.bytes = align(plan.bytes);
  return plan;
}

TextureFormat texture_format(VkFormat format) {
  if (format >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) {
    constexpr std::uint32_t sizes[][2] = {{4, 4}, {5, 4},  {5, 5},  {6, 5},  {6, 6},   {8, 5},   {8, 6},
                                          {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    auto index = static_cast<unsigned>(format - VK_FORMAT_ASTC_4x4_UNORM_BLOCK);
    bool srgb = index & 1;
    return {format,
            srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
            sizes[index / 2][0],
            sizes[index / 2][1],
            16,
            4u};
  }
  if (format >= VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK && format <= VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK) {
    bool srgb = (format - VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK) & 1;
    return {format, srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, 4,
            4,      format >= VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK ? 16u : 8u,  4};
  }
  if (format >= VK_FORMAT_EAC_R11_UNORM_BLOCK && format <= VK_FORMAT_EAC_R11G11_SNORM_BLOCK) {
    bool rg = format >= VK_FORMAT_EAC_R11G11_UNORM_BLOCK;
    return {format, rg ? VK_FORMAT_R32G32_SFLOAT : VK_FORMAT_R32_SFLOAT, 4, 4, rg ? 16u : 8u, rg ? 8u : 4u};
  }
  return {};
}
TextureStorage texture_storage(VkFormat format, bool mutable_format, std::span<const VkFormat> view_formats,
                               bool compressed) {
  const auto base = texture_format(format);
  if (!base) return {};
  // Each decoded compatibility class is a pair of adjacent enums from
  // VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK (147, odd) to VK_FORMAT_ASTC_12x12_SRGB_BLOCK
  // (184): the odd UNORM member, then its SRGB (ETC2, ASTC) or SNORM (EAC) partner.
  const auto first = static_cast<VkFormat>((format - 1) | 1);
  const auto second = static_cast<VkFormat>(first + 1);
  bool with_first = false, with_second = false;
  if (!mutable_format) {
    (format == first ? with_first : with_second) = true;
  } else if (view_formats.empty()) {
    with_first = with_second = true;
  } else {
    for (auto view : view_formats) {
      if (view == first) with_first = true;
      else if (view == second) with_second = true;
      else return {};
    }
  }
  auto plane = [](VkFormat f) {
    auto t = texture_format(f);
    return TextureStorage::Plane{t.decoded, {}, {f}, f, t.texel_bytes};
  };
  TextureStorage storage;
  if (with_first != with_second) {
    storage.planes = {plane(with_first ? first : second)};
  } else if (first <= VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK) {
    // ETC2 decodes identically for UNORM and SRGB; only the view's transfer
    // function differs, so one mutable RGBA8 image serves both views.
    storage.planes = {{VK_FORMAT_R8G8B8A8_UNORM, {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB},
                       {first, second}, second, 4}};
  } else {
    // ASTC UNORM defaults to the owner-approved decode_unorm8 precision
    // while SRGB views use the sRGB decode to 8 bits; EAC UNORM and SNORM read
    // the same bits as different values. Each format gets its own plane, the
    // image format's first.
    storage.planes = {plane(format), plane(format == first ? second : first)};
  }
  if (compressed) {
    auto bc7 = [](VkFormat& f) {
      if (f == VK_FORMAT_R8G8B8A8_UNORM) f = VK_FORMAT_BC7_UNORM_BLOCK;
      else if (f == VK_FORMAT_R8G8B8A8_SRGB) f = VK_FORMAT_BC7_SRGB_BLOCK;
    };
    for (auto& p : storage.planes) {
      bc7(p.format);
      for (auto& f : p.view_formats) bc7(f);
    }
  }
  return storage;
}
std::optional<TextureView> texture_view(const TextureStorage& storage, VkFormat format, VkFormat decode_mode) {
  const bool linear_astc = (format >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK &&
      format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK && (format & 1)) ||
      (format >= VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK && format <= VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK);
  for (std::size_t i = 0; i < storage.planes.size(); ++i) {
    const auto& p = storage.planes[i];
    if (std::ranges::find(p.guest_views, format) == p.guest_views.end()) continue;
    // A BC7 plane holds the UNORM8 decode, re-encoded.
    const auto stored = p.format == VK_FORMAT_BC7_UNORM_BLOCK ? VK_FORMAT_R8G8B8A8_UNORM : p.format;
    if (linear_astc && decode_mode != VK_FORMAT_UNDEFINED && stored != decode_mode) continue;
    if (p.view_formats.empty()) return TextureView{i, p.format};
    const auto view = std::ranges::find(p.guest_views, format) - p.guest_views.begin();
    return TextureView{i, p.view_formats.at(view)};
  }
  return std::nullopt;
}
void force_texture_emulation(bool forced) { emulation_forced.store(forced, std::memory_order_relaxed); }
void force_texture_compression(int mode) { compression_forced.store(mode, std::memory_order_relaxed); }
std::vector<VkImage> emulated_texture_planes(VkImage handle) {
  std::vector<VkImage> out;
  if (auto img = image(handle))
    for (std::size_t i = 0; i < img->storage.planes.size(); ++i) out.push_back(img->plane(i));
  return out;
}
void texture_extensions(VkPhysicalDevice physical, std::vector<VkExtensionProperties>& extensions) {
  // Only native ASTC forwards explicit FP16 view precision. Emulated LDR
  // images have UNORM8 storage and must not advertise the broader contract.
  if (emulated(physical, VK_FORMAT_ASTC_4x4_UNORM_BLOCK))
    std::erase_if(extensions, [](const auto& extension) {
      return std::string_view(extension.extensionName) == VK_EXT_ASTC_DECODE_MODE_EXTENSION_NAME;
    });
}
void guest_texture_features(VkPhysicalDevice physical, VkPhysicalDeviceFeatures& out) {
  auto supported = [&](int first, int last) {
    for (int f = first; f <= last; ++f) {
      VkFormatProperties props{};
      format_properties(physical, static_cast<VkFormat>(f), &props);
      if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return false;
    }
    return true;
  };
  out.textureCompressionASTC_LDR = supported(VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
  out.textureCompressionETC2 = supported(VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, VK_FORMAT_EAC_R11G11_SNORM_BLOCK);
}
VkResult host_texture_features(VkPhysicalDevice physical, VkPhysicalDeviceFeatures& requested) {
  VkPhysicalDeviceFeatures native{};
  phys<PFN_vkGetPhysicalDeviceFeatures>(physical, "vkGetPhysicalDeviceFeatures")(physical, &native);
  auto available = native;
  guest_texture_features(physical, available);
  if ((requested.textureCompressionASTC_LDR && !available.textureCompressionASTC_LDR) ||
      (requested.textureCompressionETC2 && !available.textureCompressionETC2))
    return VK_ERROR_FEATURE_NOT_PRESENT;
  if (!native.textureCompressionASTC_LDR) requested.textureCompressionASTC_LDR = VK_FALSE;
  if (!native.textureCompressionETC2) requested.textureCompressionETC2 = VK_FALSE;
  // BC7 storage for decoded textures under GPU memory pressure (create_image).
  requested.textureCompressionBC |= native.textureCompressionBC;
  return VK_SUCCESS;
}
void release_texture_commands(VkCommandBuffer command) {
  Commands removed;
  {
    std::lock_guard lock(texture_lock);
    auto it = command_state.find(command);
    if (it != command_state.end()) {
      removed = std::move(it->second);
      command_state.erase(it);
      if (!removed.decoded.empty()) {
        auto& budget = scratch_budgets[removed.decode_pool.first];
        auto& idle = scratch_pools[removed.decode_pool];
        for (auto& b : removed.decoded) {
          if (budget.count == scratch_count_limit || b->allocation_size > scratch_byte_limit - budget.bytes)
            continue;
          budget.bytes += b->allocation_size;
          ++budget.count;
          idle.push_back(std::move(b));
        }
      }
    }
  }
}
void trim_texture_pool(VkDevice device, VkCommandPool pool) {
  std::vector<std::shared_ptr<Buffer>> removed;
  {
    std::lock_guard lock(texture_lock);
    auto it = scratch_pools.find({device, pool});
    if (it == scratch_pools.end()) return;
    removed = std::move(it->second);
    scratch_pools.erase(it);
    auto& budget = scratch_budgets[device];
    for (const auto& b : removed) {
      budget.bytes -= b->allocation_size;
      --budget.count;
    }
  }
}
void release_texture_device(VkDevice device) {
  {
    std::lock_guard lock(texture_lock);
    for (const auto& [handle, img] : images)
      if (img->device == device && img->retained)
        img->retained->cancelled.store(true, std::memory_order_release);
  }
  // Also on device loss: no mapped staging can be freed while a CPU reader runs.
  wait_texture_sources(device);
  std::shared_future<std::shared_ptr<Pipeline>> pending;
  std::vector<std::shared_future<std::shared_ptr<Pipeline>>> pending_images;
  {
    std::lock_guard lock(texture_lock);
    if (auto it = lazy_devices.find(device); it != lazy_devices.end()) {
      auto mutex = gfx::queue_mutex(it->second.queue);
      std::lock_guard queue_lock(*mutex);
      it->second.wait_idle(it->second.queue);
    }
    if (auto it = pipelines.find(device); it != pipelines.end()) {
      pending = std::move(it->second);
      pipelines.erase(it);
    }
    std::erase_if(image_pipelines, [&](auto& entry) {
      if (entry.first.first != device) return false;
      pending_images.push_back(std::move(entry.second));
      return true;
    });
  }
  // Finish creation before the host device dies; destroy future-owned objects
  // outside texture_lock, even if creation failed halfway through.
  if (pending.valid()) pending.wait();
  for (auto& future : pending_images) if (future.valid()) future.wait();
  std::vector<Commands> removed_commands;
  std::vector<std::shared_ptr<Image>> removed_images;
  std::vector<std::vector<std::shared_ptr<Buffer>>> removed_scratch;
  {
    std::lock_guard lock(texture_lock);
    std::erase_if(command_state, [&](auto& p) {
      if (command_device(p.first) != device) return false;
      removed_commands.push_back(std::move(p.second));
      return true;
    });
    std::erase_if(images, [&](auto& p) {
      if (p.second->device != device) return false;
      removed_images.push_back(std::move(p.second));
      return true;
    });
    std::erase_if(depth_aliases, [&](const auto& p) { return p.second == device; });
    std::erase_if(scratch_pools, [&](auto& p) {
      if (p.first.first != device) return false;
      removed_scratch.push_back(std::move(p.second));
      return true;
    });
    scratch_budgets.erase(device);
    std::erase_if(companion_views, [&](const auto& entry) {
      auto image = entry.second.lock();
      return !image || image->device == device;
    });
    lazy_devices.erase(device);
    source_workers.erase(device);
  }
  // Release image/command owners before the device's allocator registry entry.
  removed_commands.clear();
  removed_images.clear();
  release_texture_allocator(device);
}
void prepare_texture_device(VkDevice device) {
  register_pipeline_exit();
  std::lock_guard lock(texture_lock);
  auto [it, inserted] = pipelines.try_emplace(device);
  if (!inserted) return;
  try { it->second = std::async(std::launch::async, create_pipeline, device, 0u).share(); }
  catch (...) { pipelines.erase(it); throw; }
}
VkPipeline wait_texture_pipeline(VkDevice device) { return pipeline(device)->pipeline; }
PFN_vkVoidFunction texture_proc(std::string_view name) {
#define FN(command, function) \
  if (name == #command) return reinterpret_cast<PFN_vkVoidFunction>(function)
  FN(vkCreateImage, create_image);
  FN(vkDestroyImage, destroy_image);
  FN(vkGetImageMemoryRequirements, image_requirements);
  FN(vkGetImageSubresourceLayout, subresource_layout);
  FN(vkGetImageMemoryRequirements2, image_requirements2);
  FN(vkGetImageMemoryRequirements2KHR, image_requirements2);
  FN(vkBindImageMemory, bind_image);
  FN(vkBindImageMemory2, bind_images);
  FN(vkBindImageMemory2KHR, bind_images);
  FN(vkAllocateMemory, allocate_guest_memory);
  // Only the census needs frees; without it the chain stays as it was.
  if (census::enabled()) FN(vkFreeMemory, free_guest_memory);
  FN(vkCreateImageView, create_view);
  FN(vkDestroyImageView, destroy_view);
  FN(vkUpdateDescriptorSets, update_descriptors);
  FN(vkUpdateDescriptorSetWithTemplate, update_template);
  FN(vkCmdExecuteCommands, execute_commands);
  FN(vkGetDeviceImageMemoryRequirements, device_image_requirements);
  FN(vkGetDeviceImageMemoryRequirementsKHR, device_image_requirements);
  FN(vkGetDeviceImageSparseMemoryRequirements, device_sparse_requirements);
  FN(vkCmdCopyBufferToImage, copy_buffer_image);
  FN(vkCmdCopyImageToBuffer, copy_image_buffer);
  FN(vkCmdCopyImage, copy_image);
  FN(vkCmdCopyBufferToImage2, copy_buffer_image2);
  FN(vkCmdCopyImageToBuffer2, copy_image_buffer2);
  FN(vkCmdCopyImage2, copy_image2);
  FN(vkBeginCommandBuffer, begin);
  FN(vkEndCommandBuffer, end);
  FN(vkResetCommandBuffer, reset);
  FN(vkCmdBindPipeline, bind_pipeline);
  FN(vkCmdPipelineBarrier, barrier);
  FN(vkCmdPipelineBarrier2, barrier2);
  FN(vkCmdWaitEvents, wait_events);
  FN(vkCmdSetEvent2, set_event2);
  FN(vkCmdWaitEvents2, wait_events2);
  FN(vkCmdBindDescriptorSets, bind_sets);
  FN(vkCmdPushConstants, push_constants);
  FN(vkCmdBindDescriptorSets2, bind_sets2);
  FN(vkCmdPushConstants2, push_constants2);
  FN(vkCmdPushDescriptorSet, push_descriptors);
  FN(vkCmdPushDescriptorSet2, push_descriptors2);
  FN(vkCreateDescriptorUpdateTemplate, create_template);
  FN(vkDestroyDescriptorUpdateTemplate, destroy_template);
  FN(vkCmdPushDescriptorSetWithTemplate, push_template);
  FN(vkCmdPushDescriptorSetWithTemplate2, push_template2);
  FN(vkGetPhysicalDeviceFormatProperties, format_properties);
  FN(vkGetPhysicalDeviceFormatProperties2, format_properties2);
  FN(vkGetPhysicalDeviceImageFormatProperties, image_properties);
  FN(vkGetPhysicalDeviceImageFormatProperties2, image_properties2);
  FN(vkGetPhysicalDeviceFeatures, features);
  FN(vkGetPhysicalDeviceFeatures2, features2);
#undef FN
  return nullptr;
}
void log_gpu_memory() {
  // GLES titles create no guest images; their frames come from the shared device.
  auto physical = last_physical.load(std::memory_order_relaxed);
  if (!physical) physical = gfx::gpu_device().physical_device;
  if (!physical) return;
  if (const auto [usage, limit] = gpu_memory(physical); limit)
    log::info("vulkan", "GPU memory in use now: {} of {} MiB", usage >> 20, limit >> 20);
}
}  // namespace venture::vulkan
