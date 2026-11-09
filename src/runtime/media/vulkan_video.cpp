#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "media/vulkan_video.h"
#include "media/ffmpeg_api.h"
#include "gfx/shared_image.h"
#include "common/environment.h"
#include "common/log.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
extern "C" {
#include <libavutil/hwcontext_vulkan.h>
}
#ifdef _WIN32
#include <vulkan/vulkan_win32.h>
#endif

// The queue and frame locks are FFmpeg's documented synchronization for
// submissions to its device's queues and frames.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace venture::media::vulkan_video {
namespace {
#include "yuv_rgba_comp.inc"

#ifdef _WIN32
constexpr auto handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
constexpr std::string_view memory_extension = "VK_KHR_external_memory_win32";
#else
constexpr auto handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
constexpr std::string_view memory_extension = "VK_KHR_external_memory_fd";
#endif

using Uuid = std::array<std::uint8_t, 16>;
void identity(AVVulkanDeviceContext* hw, Uuid* device, Uuid* driver) {
  auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
      hw->get_proc_addr(hw->inst, "vkGetPhysicalDeviceProperties2"));
  VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
  VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
  if (properties) properties(hw->phys_dev, &p);
  if (device) std::memcpy(device->data(), id.deviceUUID, 16);
  if (driver) std::memcpy(driver->data(), id.driverUUID, 16);
}
AVVulkanDeviceContext* vulkan_context(AVBufferRef* device) {
  return static_cast<AVVulkanDeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(device->data)->hwctx);
}

struct DeviceState {
  std::mutex mutex;
  bool attempted = false;
  AVBufferRef* device = nullptr;  // process lifetime, like the host device
};
DeviceState& device_state() {
  static auto* state = new DeviceState;
  return *state;
}

#define VULKAN_DEVICE_FUNCTIONS(X) \
  X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
  X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) X(vkWaitForFences) \
  X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkQueueSubmit2) \
  X(vkCmdPipelineBarrier2) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) \
  X(vkCmdDispatch) X(vkCmdCopyImageToBuffer) \
  X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) \
  X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) \
  X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) \
  X(vkDestroyPipelineLayout) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateComputePipelines) \
  X(vkDestroyPipeline) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) \
  X(vkUpdateDescriptorSets) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
  X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkInvalidateMappedMemoryRanges)

// FFmpeg's device, its functions and identity; shared by a converter and
// every image it has handed out.
struct Context {
  std::shared_ptr<Library> library;
  AVBufferRef* device_ref = nullptr;
  AVHWDeviceContext* hwdev = nullptr;
  AVVulkanDeviceContext* hw = nullptr;
  VkDevice device = VK_NULL_HANDLE;
#define FIELD(n) PFN_##n n = nullptr;
  VULKAN_DEVICE_FUNCTIONS(FIELD)
#undef FIELD
#ifdef _WIN32
  PFN_vkGetMemoryWin32HandleKHR export_handle = nullptr;
#else
  PFN_vkGetMemoryFdKHR export_handle = nullptr;
#endif
  VkPhysicalDeviceMemoryProperties memory{};
  Uuid device_uuid{}, driver_uuid{};
  bool exportable = false;
  ~Context() { if (device_ref) library->impl->av_buffer_unref(&device_ref); }
  bool load(AVBufferRef* reference) {
    device_ref = library->impl->av_buffer_ref(reference);
    if (!device_ref) return false;
    hwdev = reinterpret_cast<AVHWDeviceContext*>(device_ref->data);
    hw = static_cast<AVVulkanDeviceContext*>(hwdev->hwctx);
    device = hw->act_dev;
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(hw->get_proc_addr(hw->inst, "vkGetDeviceProcAddr"));
    auto memory_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        hw->get_proc_addr(hw->inst, "vkGetPhysicalDeviceMemoryProperties"));
    if (!gdpa || !memory_properties) return false;
#define LOAD(n) n = reinterpret_cast<PFN_##n>(gdpa(device, #n)); if (!n) return false;
    VULKAN_DEVICE_FUNCTIONS(LOAD)
#undef LOAD
    memory_properties(hw->phys_dev, &memory);
    identity(hw, &device_uuid, &driver_uuid);
    for (int i = 0; i < hw->nb_enabled_dev_extensions; ++i)
      exportable |= memory_extension == hw->enabled_dev_extensions[i];
#ifdef _WIN32
    if (exportable) export_handle = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(gdpa(device, "vkGetMemoryWin32HandleKHR"));
#else
    if (exportable) export_handle = reinterpret_cast<PFN_vkGetMemoryFdKHR>(gdpa(device, "vkGetMemoryFdKHR"));
#endif
    exportable = export_handle != nullptr;
    return true;
  }
  std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags wanted, VkMemoryPropertyFlags preferred = 0) const {
    std::uint32_t found = UINT32_MAX;
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
      auto flags = memory.memoryTypes[i].propertyFlags;
      if (!(bits & (1u << i)) || (flags & wanted) != wanted) continue;
      if ((flags & preferred) == preferred) return i;
      if (found == UINT32_MAX) found = i;
    }
    return found;
  }
};

std::atomic<std::uint64_t> next_image_id{1};

// One RGBA8 conversion target, exportable when the device allows it.
struct Slot {
  std::shared_ptr<Context> context;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  gfx::SharedImage shared;
  ~Slot() {
    auto& c = *context;
    if (view) c.vkDestroyImageView(c.device, view, nullptr);
    if (image) c.vkDestroyImage(c.device, image, nullptr);
    if (memory) c.vkFreeMemory(c.device, memory, nullptr);
#ifdef _WIN32
    if (shared.handle != -1 && shared.handle) CloseHandle(reinterpret_cast<HANDLE>(shared.handle));
#else
    if (shared.handle >= 0) ::close(int(shared.handle));
#endif
  }
  bool create(std::uint32_t width, std::uint32_t height, bool exported) {
    auto& c = *context;
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr,
                                             VkExternalMemoryHandleTypeFlags(handle_type)};
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, exported ? &external : nullptr};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {width, height, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (c.vkCreateImage(c.device, &info, nullptr, &image) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    c.vkGetImageMemoryRequirements(c.device, image, &requirements);
    auto type = c.memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) return false;
    VkExportMemoryAllocateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, nullptr,
                                           VkExternalMemoryHandleTypeFlags(handle_type)};
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                                            exported ? &export_info : nullptr, image, VK_NULL_HANDLE};
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicated, requirements.size, type};
    if (c.vkAllocateMemory(c.device, &allocate, nullptr, &memory) != VK_SUCCESS) return false;
    if (c.vkBindImageMemory(c.device, image, memory, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = info.format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (c.vkCreateImageView(c.device, &view_info, nullptr, &view) != VK_SUCCESS) return false;
    shared.id = next_image_id.fetch_add(1);
    shared.width = width;
    shared.height = height;
    shared.format = info.format;
    shared.usage = info.usage;
    shared.handle_type = handle_type;
    shared.allocation_size = requirements.size;
    shared.memory_type = type;
    shared.device_uuid = c.device_uuid;
    shared.driver_uuid = c.driver_uuid;
    if (exported) {
#ifdef _WIN32
      VkMemoryGetWin32HandleInfoKHR get{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR, nullptr, memory, handle_type};
      HANDLE handle{};
      if (c.export_handle(c.device, &get, &handle) != VK_SUCCESS || !handle) return false;
      shared.handle = reinterpret_cast<std::intptr_t>(handle);
#else
      VkMemoryGetFdInfoKHR get{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, nullptr, memory, handle_type};
      int fd = -1;
      if (c.export_handle(c.device, &get, &fd) != VK_SUCCESS || fd < 0) return false;
      shared.handle = fd;
#endif
    }
    return true;
  }
};

// Targets handed to consumers return here when their last reference goes.
struct Ring {
  std::mutex mutex;
  std::condition_variable released;
  std::vector<std::shared_ptr<Slot>> slots;
  std::vector<bool> busy;
};
constexpr std::size_t ring_size = 4;  // window frame, present snapshot, pending render, conversion

struct Conversion {
  float r[4], g[4], b[4];
  float scale[2], offset[2];
  std::int32_t size[2];
};
static_assert(sizeof(Conversion) == 72);

// Y'CbCr (normalized samples of an 8-bit or MSB-packed 10-bit container) to
// R'G'B': the tagged matrix (BT.709, BT.2020, else BT.601) and range (full when
// tagged, else limited), as the CPU conversion signals them.
void coefficients(const AVFrame& f, bool ten_bit, Conversion* out) {
  double kr = 0.299, kb = 0.114;
  if (f.colorspace == AVCOL_SPC_BT709) kr = 0.2126, kb = 0.0722;
  else if (f.colorspace == AVCOL_SPC_BT2020_NCL || f.colorspace == AVCOL_SPC_BT2020_CL) kr = 0.2627, kb = 0.0593;
  const double kg = 1 - kr - kb;
  const bool full = f.color_range == AVCOL_RANGE_JPEG;
  // Code values n of depth d: an MSB-packed 10-bit sample reads as
  // n * 64 / 65535, an 8-bit one as n / 255. Normalize to n / (2^d - 1).
  const double peak = ten_bit ? 1023.0 : 255.0, unpack = ten_bit ? 65535.0 / (1023.0 * 64.0) : 1.0;
  const double step = peak / 255.0;  // limited-range levels scale with depth
  const double y_offset = full ? 0.0 : 16.0 * step / peak, y_range = full ? 1.0 : 219.0 * step / peak;
  const double c_offset = 128.0 * step / peak, c_range = full ? 1.0 : 224.0 * step / peak;
  auto row = [&](float* v, double cb, double cr) {
    v[0] = float(unpack / y_range);
    v[1] = float(unpack / c_range * cb);
    v[2] = float(unpack / c_range * cr);
    v[3] = float(-y_offset / y_range - (cb + cr) * c_offset / c_range);
  };
  row(out->r, 0, 2 * (1 - kr));
  row(out->g, -2 * kb * (1 - kb) / kg, -2 * kr * (1 - kr) / kg);
  row(out->b, 2 * (1 - kb), 0);
}
}  // namespace

AVBufferRef* device_reference(Library& library) {
  if (!library.available()) return nullptr;
  auto& a = *library.impl;
  auto& state = device_state();
  std::lock_guard lock(state.mutex);
  if (!state.attempted) {
    state.attempted = true;
    const auto host = gfx::host_gpu();
    // Diagnostic: a device index or name, or "off" for the other backends.
    const char* forced = venture::environment::getenv("VENTURE_MEDIA_VULKAN_DEVICE");
    if (forced && std::string_view(forced) == "off") {
      log::info("media", "Vulkan video disabled by VENTURE_MEDIA_VULKAN_DEVICE=off");
      return nullptr;
    }
    // FFmpeg selects by index or name. Physical device order follows the
    // loader's, so the host GPU is normally the first index tried.
    for (int index = 0; index < 8 && !state.device; ++index) {
      AVBufferRef* device = nullptr;
      const std::string selection = forced && *forced ? std::string(forced) : std::to_string(index);
      if (a.av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VULKAN, selection.c_str(), nullptr, 0) < 0) break;
      Uuid uuid{};
      identity(vulkan_context(device), &uuid, nullptr);
      if ((forced && *forced) || !host || uuid == host->device_uuid) state.device = device;
      else a.av_buffer_unref(&device);
      if (forced && *forced) break;
    }
    if (state.device) {
      VkPhysicalDeviceProperties properties{};
      auto* hw = vulkan_context(state.device);
      if (auto get = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
              hw->get_proc_addr(hw->inst, "vkGetPhysicalDeviceProperties")))
        get(hw->phys_dev, &properties);
      log::info("media", "Vulkan video device: {} (host GPU {})", properties.deviceName,
                host ? "matched" : "unknown");
    } else {
      log::info("media", "Vulkan video device unavailable");
    }
  }
  return state.device ? a.av_buffer_ref(state.device) : nullptr;
}

struct Converter::Impl {
  std::shared_ptr<Library> library;
  std::shared_ptr<Context> context;
  bool failed = false;
  std::uint32_t family = 0;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkDescriptorSet set = VK_NULL_HANDLE;
  std::shared_ptr<Ring> ring;
  std::uint32_t ring_width = 0, ring_height = 0;
  bool ring_exported = false;
  VkBuffer readback = VK_NULL_HANDLE;
  VkDeviceMemory readback_memory = VK_NULL_HANDLE;
  void* readback_mapped = nullptr;
  VkDeviceSize readback_size = 0;
  bool readback_coherent = false;

  ~Impl() {
    if (!context) return;
    auto& c = *context;
    if (fence) c.vkWaitForFences(c.device, 1, &fence, VK_TRUE, UINT64_MAX);
    release_readback();
    if (descriptor_pool) c.vkDestroyDescriptorPool(c.device, descriptor_pool, nullptr);
    if (pipeline) c.vkDestroyPipeline(c.device, pipeline, nullptr);
    if (pipeline_layout) c.vkDestroyPipelineLayout(c.device, pipeline_layout, nullptr);
    if (set_layout) c.vkDestroyDescriptorSetLayout(c.device, set_layout, nullptr);
    if (sampler) c.vkDestroySampler(c.device, sampler, nullptr);
    if (fence) c.vkDestroyFence(c.device, fence, nullptr);
    if (pool) c.vkDestroyCommandPool(c.device, pool, nullptr);
    // Handed-out slots keep the context alive until their consumers let go.
  }
  void release_readback() {
    auto& c = *context;
    if (readback_mapped) c.vkUnmapMemory(c.device, readback_memory);
    if (readback) c.vkDestroyBuffer(c.device, readback, nullptr);
    if (readback_memory) c.vkFreeMemory(c.device, readback_memory, nullptr);
    readback_mapped = nullptr;
    readback = VK_NULL_HANDLE;
    readback_memory = VK_NULL_HANDLE;
    readback_size = 0;
  }
  bool initialize(AVBufferRef* device) {
    context = std::make_shared<Context>();
    context->library = library;
    if (!context->load(device)) return false;
    auto& c = *context;
    auto* hw = c.hw;
    int chosen = -1;
    for (int i = 0; i < hw->nb_qf; ++i)
      if (hw->qf[i].flags & VK_QUEUE_COMPUTE_BIT) { chosen = i; break; }
    if (chosen < 0) return false;
    family = std::uint32_t(hw->qf[chosen].idx);
    c.vkGetDeviceQueue(c.device, family, 0, &queue);
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                      VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
    if (c.vkCreateCommandPool(c.device, &pool_info, nullptr, &pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
                                             VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (c.vkAllocateCommandBuffers(c.device, &command_info, &command) != VK_SUCCESS) return false;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (c.vkCreateFence(c.device, &fence_info, nullptr, &fence) != VK_SUCCESS) return false;
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (c.vkCreateSampler(c.device, &sampler_info, nullptr, &sampler) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding bindings[3]{
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 3, bindings};
    if (c.vkCreateDescriptorSetLayout(c.device, &set_info, nullptr, &set_layout) != VK_SUCCESS) return false;
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Conversion)};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &set_layout, 1, &push};
    if (c.vkCreatePipelineLayout(c.device, &layout_info, nullptr, &pipeline_layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                                         sizeof(decode_shader), decode_shader};
    VkShaderModule module{};
    if (c.vkCreateShaderModule(c.device, &module_info, nullptr, &module) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                           VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
    pipeline_info.layout = pipeline_layout;
    auto created = c.vkCreateComputePipelines(c.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
    c.vkDestroyShaderModule(c.device, module, nullptr);
    if (created != VK_SUCCESS) return false;
    VkDescriptorPoolSize sizes[2]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
    VkDescriptorPoolCreateInfo descriptor_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 1, 2, sizes};
    if (c.vkCreateDescriptorPool(c.device, &descriptor_info, nullptr, &descriptor_pool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo set_allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, descriptor_pool,
                                             1, &set_layout};
    return c.vkAllocateDescriptorSets(c.device, &set_allocate, &set) == VK_SUCCESS;
  }
  // A free target of this size, waiting briefly for a consumer to let one go.
  std::shared_ptr<Slot> acquire(std::uint32_t width, std::uint32_t height, bool exported, std::size_t* index) {
    if (!ring || ring_width != width || ring_height != height || ring_exported != exported) {
      // Images still held by consumers stay alive through their references.
      ring = std::make_shared<Ring>();
      ring->slots.resize(ring_size);
      ring->busy.assign(ring_size, false);
      ring_width = width;
      ring_height = height;
      ring_exported = exported;
    }
    std::unique_lock lock(ring->mutex);
    auto free_slot = [&] {
      return std::ranges::find(ring->busy, false) != ring->busy.end();
    };
    if (!ring->released.wait_for(lock, std::chrono::milliseconds(100), free_slot)) return nullptr;
    *index = std::size_t(std::ranges::find(ring->busy, false) - ring->busy.begin());
    auto& slot = ring->slots[*index];
    if (!slot) {
      auto created = std::make_shared<Slot>();
      created->context = context;
      if (!created->create(width, height, exported)) return nullptr;
      created->shared.allocation = created;
      slot = std::move(created);
    }
    ring->busy[*index] = true;
    return slot;
  }
  void release(std::size_t index) {
    std::lock_guard lock(ring->mutex);
    ring->busy[index] = false;
    ring->released.notify_all();
  }
  bool ensure_readback(VkDeviceSize size) {
    if (readback_size >= size) return true;
    release_readback();
    auto& c = *context;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (c.vkCreateBuffer(c.device, &info, nullptr, &readback) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    c.vkGetBufferMemoryRequirements(c.device, readback, &requirements);
    auto type = c.memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                              VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type == UINT32_MAX) return false;
    readback_coherent = c.memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, type};
    if (c.vkAllocateMemory(c.device, &allocate, nullptr, &readback_memory) != VK_SUCCESS) return false;
    if (c.vkBindBufferMemory(c.device, readback, readback_memory, 0) != VK_SUCCESS) return false;
    if (c.vkMapMemory(c.device, readback_memory, 0, VK_WHOLE_SIZE, 0, &readback_mapped) != VK_SUCCESS) return false;
    readback_size = size;
    return true;
  }
  bool convert(AVFrame* f, int width, int height, std::shared_ptr<const gfx::SharedImage>* image,
               std::vector<std::uint8_t>* rgba);
};

bool Converter::Impl::convert(AVFrame* f, int width, int height, std::shared_ptr<const gfx::SharedImage>* image,
                              std::vector<std::uint8_t>* rgba) {
  if (failed || f->format != AV_PIX_FMT_VULKAN || !f->hw_frames_ctx || !f->data[0] || width <= 0 || height <= 0 ||
      width > 8192 || height > 8192 || (!image == !rgba))
    return false;
  auto* frames = reinterpret_cast<AVHWFramesContext*>(f->hw_frames_ctx->data);
  auto* vk_frames = static_cast<AVVulkanFramesContext*>(frames->hwctx);
  const bool ten_bit = frames->sw_format == AV_PIX_FMT_P010;
  if ((frames->sw_format != AV_PIX_FMT_NV12 && !ten_bit) || !(vk_frames->usage & VK_IMAGE_USAGE_SAMPLED_BIT) ||
      !(vk_frames->img_flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT))
    return false;
  if (!context) {
    if (!initialize(frames->device_ref)) {
      failed = true;
      log::warn("media", "Vulkan video conversion unavailable; downloading pictures");
      return false;
    }
  } else if (context->hwdev != reinterpret_cast<AVHWDeviceContext*>(frames->device_ref->data)) {
    return false;
  }
  auto& c = *context;
  const bool exported = image != nullptr;
  if (exported && !c.exportable) return false;
  std::size_t index = 0;
  auto slot = acquire(std::uint32_t(width), std::uint32_t(height), exported, &index);
  if (!slot) return false;
  struct Held {
    Impl& impl;
    std::size_t index;
    bool keep = false;
    ~Held() { if (!keep) impl.release(index); }
  } held{*this, index};
  if (rgba && !ensure_readback(VkDeviceSize(width) * height * 4)) return false;

  auto* frame = reinterpret_cast<AVVkFrame*>(f->data[0]);
  const bool multiplane = frame->img[1] == VK_NULL_HANDLE;
  const VkFormat plane_formats[2]{ten_bit ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM,
                                  ten_bit ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R8G8_UNORM};
  VkImageView planes[2]{};
  struct Views {
    Context& c;
    VkImageView* views;
    ~Views() { for (int i = 0; i < 2; ++i) if (views[i]) c.vkDestroyImageView(c.device, views[i], nullptr); }
  } views{c, planes};
  for (int plane = 0; plane < 2; ++plane) {
    VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, nullptr, VK_IMAGE_USAGE_SAMPLED_BIT};
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &usage};
    info.image = multiplane ? frame->img[0] : frame->img[plane];
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = plane_formats[plane];
    info.subresourceRange = {multiplane ? VkImageAspectFlags(plane ? VK_IMAGE_ASPECT_PLANE_1_BIT : VK_IMAGE_ASPECT_PLANE_0_BIT)
                                        : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                             0, 1, 0, 1};
    if (c.vkCreateImageView(c.device, &info, nullptr, &planes[plane]) != VK_SUCCESS) return false;
  }
  VkDescriptorImageInfo sampled[2]{{sampler, planes[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                   {sampler, planes[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
  VkDescriptorImageInfo storage{VK_NULL_HANDLE, slot->view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[3]{};
  for (std::uint32_t i = 0; i < 3; ++i) {
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, i, 0, 1,
                 i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                 i < 2 ? &sampled[i] : &storage, nullptr, nullptr};
  }
  c.vkUpdateDescriptorSets(c.device, 3, writes, 0, nullptr);

  Conversion push{};
  coefficients(*f, ten_bit, &push);
  const double image_width = frames->width, image_height = frames->height;
  push.offset[0] = float(f->crop_left / image_width);
  push.offset[1] = float(f->crop_top / image_height);
  push.scale[0] = float((f->width - double(f->crop_left) - double(f->crop_right)) / image_width);
  push.scale[1] = float((f->height - double(f->crop_top) - double(f->crop_bottom)) / image_height);
  push.size[0] = width;
  push.size[1] = height;

  if (c.vkResetCommandBuffer(command, 0) != VK_SUCCESS) return false;
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                 VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  if (c.vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) return false;

  const int images = multiplane ? 1 : 2;
  vk_frames->lock_frame(frames, frame);
  struct FrameLock {
    AVVulkanFramesContext* vk_frames;
    AVHWFramesContext* frames;
    AVVkFrame* frame;
    ~FrameLock() { vk_frames->unlock_frame(frames, frame); }
  } frame_lock{vk_frames, frames, frame};
  VkImageMemoryBarrier2 barriers[3]{};
  for (int i = 0; i < images; ++i) {
    auto& b = barriers[i];
    b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = frame->layout[i];
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = frame->queue_family[i];
    b.dstQueueFamilyIndex = frame->queue_family[i] == VK_QUEUE_FAMILY_IGNORED ? VK_QUEUE_FAMILY_IGNORED : family;
    b.image = frame->img[i];
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
  }
  auto& target = barriers[images];
  target = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  target.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
  target.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  target.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  target.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // every pixel is rewritten
  target.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  target.srcQueueFamilyIndex = target.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  target.image = slot->image;
  target.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = std::uint32_t(images + 1);
  dependency.pImageMemoryBarriers = barriers;
  c.vkCmdPipelineBarrier2(command, &dependency);
  c.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  c.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &set, 0, nullptr);
  c.vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
  c.vkCmdDispatch(command, (std::uint32_t(width) + 15) / 16, (std::uint32_t(height) + 15) / 16, 1);

  VkImageMemoryBarrier2 done{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  done.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  done.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  done.image = slot->image;
  done.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (exported) {
    // Hand the image to the consumer's device, which acquires it from EXTERNAL.
    done.oldLayout = done.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    done.srcQueueFamilyIndex = family;
    done.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
  } else {
    done.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    done.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    done.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    done.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    done.srcQueueFamilyIndex = done.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  }
  VkDependencyInfo finish{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  finish.imageMemoryBarrierCount = 1;
  finish.pImageMemoryBarriers = &done;
  c.vkCmdPipelineBarrier2(command, &finish);
  if (!exported) {
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {std::uint32_t(width), std::uint32_t(height), 1};
    c.vkCmdCopyImageToBuffer(command, slot->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
    VkBufferMemoryBarrier2 host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = readback;
    host.size = VK_WHOLE_SIZE;
    VkDependencyInfo to_host{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    to_host.bufferMemoryBarrierCount = 1;
    to_host.pBufferMemoryBarriers = &host;
    c.vkCmdPipelineBarrier2(command, &to_host);
  }
  if (c.vkEndCommandBuffer(command) != VK_SUCCESS) return false;

  // Wait for the decode that produced the picture and signal the next value,
  // as every user of an AVVkFrame must.
  VkSemaphoreSubmitInfo waits[2]{}, signals[2]{};
  for (int i = 0; i < images; ++i) {
    waits[i] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, frame->sem[i], frame->sem_value[i],
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
    signals[i] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, frame->sem[i], frame->sem_value[i] + 1,
                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
  }
  VkCommandBufferSubmitInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, command, 0};
  VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  submit.waitSemaphoreInfoCount = std::uint32_t(images);
  submit.pWaitSemaphoreInfos = waits;
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &command_info;
  submit.signalSemaphoreInfoCount = std::uint32_t(images);
  submit.pSignalSemaphoreInfos = signals;
  if (c.vkResetFences(c.device, 1, &fence) != VK_SUCCESS) return false;
  c.hw->lock_queue(c.hwdev, family, 0);
  auto submitted = c.vkQueueSubmit2(queue, 1, &submit, fence);
  c.hw->unlock_queue(c.hwdev, family, 0);
  if (submitted != VK_SUCCESS) {
    failed = true;
    log::warn("media", "Vulkan video conversion submit failed ({})", int(submitted));
    return false;
  }
  for (int i = 0; i < images; ++i) {
    frame->layout[i] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    frame->access[i] = VkAccessFlagBits(VK_ACCESS_SHADER_READ_BIT);
    ++frame->sem_value[i];
    if (frame->queue_family[i] != VK_QUEUE_FAMILY_IGNORED) frame->queue_family[i] = family;
  }
  if (c.vkWaitForFences(c.device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
    failed = true;
    return false;
  }
  if (rgba) {
    if (!readback_coherent) {
      VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, readback_memory, 0, VK_WHOLE_SIZE};
      c.vkInvalidateMappedMemoryRanges(c.device, 1, &range);
    }
    rgba->resize(std::size_t(width) * height * 4);
    std::memcpy(rgba->data(), readback_mapped, rgba->size());
    return true;
  }
  // The consumer's reference keeps the target out of the ring until released.
  held.keep = true;
  auto owner = ring;
  *image = std::shared_ptr<const gfx::SharedImage>(&slot->shared, [owner, slot, index](const gfx::SharedImage*) {
    std::lock_guard lock(owner->mutex);
    owner->busy[index] = false;
    owner->released.notify_all();
  });
  return true;
}

Converter::Converter(std::shared_ptr<Library> library) : impl(std::make_unique<Impl>()) {
  impl->library = std::move(library);
}
Converter::~Converter() = default;
bool Converter::convert(AVFrame* picture, int width, int height, std::shared_ptr<const gfx::SharedImage>* image,
                        std::vector<std::uint8_t>* rgba) {
  return impl->convert(picture, width, height, image, rgba);
}
}  // namespace venture::media::vulkan_video
