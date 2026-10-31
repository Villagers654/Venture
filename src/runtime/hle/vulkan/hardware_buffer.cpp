// VK_ANDROID_external_memory_android_hardware_buffer over host external memory.
//
// A GPU-usage AHardwareBuffer is a dedicated image allocation exported as a
// platform payload handle (Windows: OPAQUE_WIN32). Guest images bound to AHB
// memory are created with one canonical create info per shape, because opaque
// imports require the exporter's image parameters and an AHB may be bound to
// any image the guest creates for it. RGB external formats need no sampler
// conversion; their YCbCr conversion objects only carry the component mapping.
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_android.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>

#include "android/hardware_buffer.h"
#include "common/log.h"
#include "external.h"
#include "gfx/gpu_device.h"
#include "internal.h"

namespace venture::vulkan {
namespace {
namespace ahb = android::ahb;
constexpr auto ahb_handle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
constexpr std::string_view ahb_extension = "VK_ANDROID_external_memory_android_hardware_buffer";
constexpr std::string_view foreign_extension = "VK_EXT_queue_family_foreign";

// Vulkan spec, "AHardwareBuffer Formats": AHB format and equivalent VkFormat.
// The AHB format value doubles as the (nonzero) external format identifier.
struct Format {
  std::uint32_t ahb;
  VkFormat vk;
};
constexpr Format formats[] = {
    {1, VK_FORMAT_R8G8B8A8_UNORM},           {2, VK_FORMAT_R8G8B8A8_UNORM},
    {3, VK_FORMAT_R8G8B8_UNORM},             {4, VK_FORMAT_R5G6B5_UNORM_PACK16},
    {0x16, VK_FORMAT_R16G16B16A16_SFLOAT},   {0x2b, VK_FORMAT_A2B10G10R10_UNORM_PACK32},
    {0x30, VK_FORMAT_D16_UNORM},             {0x31, VK_FORMAT_X8_D24_UNORM_PACK32},
    {0x32, VK_FORMAT_D24_UNORM_S8_UINT},     {0x33, VK_FORMAT_D32_SFLOAT},
    {0x34, VK_FORMAT_D32_SFLOAT_S8_UINT},    {0x35, VK_FORMAT_S8_UINT}};
constexpr std::uint32_t format_rgbx = 2;
VkFormat vk_format(std::uint64_t ahb_format) {
  for (auto f : formats)
    if (f.ahb == ahb_format) return f.vk;
  return VK_FORMAT_UNDEFINED;
}
std::uint32_t ahb_format(VkFormat format) {
  for (auto f : formats)
    if (f.vk == format) return f.ahb;
  return 0;
}
bool depth_stencil(VkFormat f) {
  return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D24_UNORM_S8_UINT ||
         f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_S8_UINT;
}
std::uint32_t full_mips(std::uint32_t width, std::uint32_t height) {
  return static_cast<std::uint32_t>(std::bit_width(std::max(width, height)));
}

template <class F>
F physical_fn(VkPhysicalDevice p, const char* name) {
  return reinterpret_cast<F>(raw_physical_proc(p, name));
}
template <class F>
F device_fn(VkDevice d, const char* name) {
  return reinterpret_cast<F>(raw_device_proc(d, name));
}
// The next layer below this one (internal.h Layer), else the host, for
// commands this file also intercepts.
template <class F, ProcedureName Name, class H>
F next_fn(H handle) {
  if (auto f = cached_layer_proc<Layer::hardware_buffer, Name>()) return reinterpret_cast<F>(f);
  if constexpr (std::is_same_v<H, VkPhysicalDevice>)
    return physical_fn<F>(handle, Name.value);
  else
    return device_fn<F>(handle, Name.value);
}
template <class T>
T* find(const void* chain, VkStructureType type) {
  for (auto* p = static_cast<const VkBaseInStructure*>(chain); p; p = p->pNext)
    if (p->sType == type) return reinterpret_cast<T*>(const_cast<VkBaseInStructure*>(p));
  return nullptr;
}
// Inputs reaching these overrides are the call's translated copies.
void unlink(void* owner, VkStructureType type) {
  for (auto* p = static_cast<VkBaseOutStructure*>(owner); p->pNext;) {
    if (p->pNext->sType == type)
      p->pNext = p->pNext->pNext;
    else
      p = p->pNext;
  }
}

bool host_extension(VkPhysicalDevice physical, std::string_view name) {
  auto f = physical_fn<PFN_vkEnumerateDeviceExtensionProperties>(physical, "vkEnumerateDeviceExtensionProperties");
  std::uint32_t count = 0;
  if (!f || f(physical, nullptr, &count, nullptr) != VK_SUCCESS) return false;
  std::vector<VkExtensionProperties> values(count);
  if (f(physical, nullptr, &count, values.data()) < VK_SUCCESS) return false;
  values.resize(count);
  return std::ranges::any_of(values, [&](auto& e) { return name == e.extensionName; });
}
bool host_foreign_queue() {
  static const bool value = [] {
    try {
      return host_extension(gfx::gpu_device().physical_device, foreign_extension);
    } catch (const std::exception&) {
      return false;
    }
  }();
  return value;
}

// The host supports the format as an exportable and importable payload.
bool host_supports(VkPhysicalDevice p, VkFormat format, VkImageUsageFlags usage, VkImageCreateFlags flags,
                   VkImageFormatProperties* limits = nullptr) {
  VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
                                                   nullptr, memory_handle_type()};
  VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &external, format,
                                        VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, flags};
  VkExternalImageFormatProperties properties{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
  VkImageFormatProperties2 out{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &properties};
  auto query = physical_fn<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
      p, "vkGetPhysicalDeviceImageFormatProperties2");
  if (!query || query(p, &info, &out) != VK_SUCCESS) return false;
  constexpr VkExternalMemoryFeatureFlags needed =
      VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
  if ((properties.externalMemoryProperties.externalMemoryFeatures & needed) != needed) return false;
  if (limits) *limits = out.imageFormatProperties;
  return true;
}
// Every usage the AHB usage equivalence allows for the format.
std::optional<VkImageUsageFlags> canonical_usage(VkPhysicalDevice p, VkFormat format) {
  if (!ahb_format(format)) return std::nullopt;
  VkFormatProperties properties{};
  physical_fn<PFN_vkGetPhysicalDeviceFormatProperties>(p, "vkGetPhysicalDeviceFormatProperties")(p, format,
                                                                                               &properties);
  const auto f = properties.optimalTilingFeatures;
  if (!(f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return std::nullopt;
  VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  if (f & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (f & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (depth_stencil(format)) {
    if (f & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
      usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
  } else {
    if (f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
      usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    if (f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
  }
  if (host_supports(p, format, usage, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)) return usage;
  usage &= ~VK_IMAGE_USAGE_STORAGE_BIT;
  if (host_supports(p, format, usage, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)) return usage;
  return std::nullopt;
}
std::optional<VkImageCreateInfo> canonical_image(VkPhysicalDevice p, VkFormat format, std::uint32_t width,
                                                 std::uint32_t height, std::uint32_t mips, std::uint32_t layers) {
  auto usage = canonical_usage(p, format);
  if (!usage || !width || !height || !layers || !mips || mips > full_mips(width, height)) return std::nullopt;
  VkImageCreateFlags flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  if (width == height && layers % 6 == 0 && host_supports(p, format, *usage, flags | VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT))
    flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  VkImageFormatProperties limits{};
  if (!host_supports(p, format, *usage, flags, &limits) || width > limits.maxExtent.width ||
      height > limits.maxExtent.height || mips > limits.maxMipLevels || layers > limits.maxArrayLayers)
    return std::nullopt;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.flags = flags;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = mips;
  info.arrayLayers = layers;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = *usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  return info;
}
bool same_image(const VkImageCreateInfo& a, const VkImageCreateInfo& b) {
  return a.flags == b.flags && a.format == b.format && a.extent.width == b.extent.width &&
         a.extent.height == b.extent.height && a.mipLevels == b.mipLevels && a.arrayLayers == b.arrayLayers &&
         a.usage == b.usage;
}
// AHB usage equivalent to an image's usage and flags.
std::uint64_t ahb_usage(VkImageUsageFlags usage, VkImageCreateFlags flags, bool mipmap_complete) {
  std::uint64_t result = 0;
  if (usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) result |= ahb::gpu_sampled_image;
  if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
    result |= ahb::gpu_framebuffer;
  if (!result) result = ahb::gpu_sampled_image;  // AHB GPU buffers carry at least one GPU usage
  if (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) result |= ahb::gpu_cube_map;
  if (flags & VK_IMAGE_CREATE_PROTECTED_BIT) result |= ahb::protected_content;
  if (mipmap_complete) result |= ahb::gpu_mipmap_complete;
  return result;
}

struct Payload final : android::GpuPayload {
  std::shared_ptr<MemoryHandle> handle;
  VkDeviceSize size = 0;
  std::uint32_t memory_type = 0;
  VkImageCreateInfo image{};  // canonical; pNext is null
};
struct Image {
  VkDevice device{};
  VkImageCreateInfo canonical{};
  std::uint32_t format = 0;  // AHB format
  bool external_format = false;
  VkImageUsageFlags usage = 0;
  VkImageCreateFlags flags = 0;
};
struct Memory {
  VkDevice device{};
  GuestAddr buffer = 0;  // one AHB reference
};
struct Conversion {
  VkDevice device{};
  VkComponentMapping components{};
  std::unique_ptr<char> token;  // the handle's unique address
};
std::mutex state_lock;
std::map<VkImage, Image> images;
std::map<VkDeviceMemory, Memory> memories;
std::map<VkSamplerYcbcrConversion, Conversion> conversions;

std::optional<Image> image_record(VkImage image) {
  std::lock_guard lock(state_lock);
  auto it = images.find(image);
  return it == images.end() ? std::nullopt : std::optional(it->second);
}
std::optional<VkComponentMapping> conversion_components(VkSamplerYcbcrConversion conversion) {
  std::lock_guard lock(state_lock);
  auto it = conversions.find(conversion);
  return it == conversions.end() ? std::nullopt : std::optional(it->second.components);
}
std::shared_ptr<const Payload> payload(GuestAddr buffer, android::HardwareBufferDesc* desc = nullptr) {
  auto b = android::hardware_buffer(buffer);
  if (!b) return nullptr;
  if (desc) *desc = b->desc;
  return std::dynamic_pointer_cast<const Payload>(b->gpu);
}

// AHardwareBuffer_allocate GPU path: a private device on the shared GPU
// allocates and exports, then frees its own references to the payload.
struct AllocatorDevice {
  VkDevice device{};
  PFN_vkGetDeviceProcAddr gdpa{};
};
const AllocatorDevice* allocator_device() {
  static AllocatorDevice state;
  static std::once_flag once;
  std::call_once(once, [] {
    try {
      if (!hardware_buffers_supported()) return;
      auto& gpu = gfx::gpu_device();
      auto create = reinterpret_cast<PFN_vkCreateDevice>(gpu.get_instance_proc_addr(gpu.instance, "vkCreateDevice"));
      float priority = 1;
      VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      queue.queueFamilyIndex = gpu.queue_family;
      queue.queueCount = 1;
      queue.pQueuePriorities = &priority;
      const char* extensions[]{memory_handle_extension()};
      VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
      info.queueCreateInfoCount = 1;
      info.pQueueCreateInfos = &queue;
      info.enabledExtensionCount = 1;
      info.ppEnabledExtensionNames = extensions;
      VkDevice device{};
      if (auto r = create(gpu.physical_device, &info, nullptr, &device); r != VK_SUCCESS) {
        log::warn("vulkan", "AHardwareBuffer allocator device unavailable ({})", int(r));
        return;
      }
      state.gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
          gpu.get_instance_proc_addr(gpu.instance, "vkGetDeviceProcAddr"));
      state.device = device;  // process lifetime
    } catch (const std::exception& e) {
      log::warn("vulkan", "AHardwareBuffer allocator device unavailable: {}", e.what());
    }
  });
  return state.device ? &state : nullptr;
}
std::optional<VkImageCreateInfo> buffer_image(const android::HardwareBufferDesc& d) {
  constexpr auto allowed = ahb::gpu_sampled_image | ahb::gpu_framebuffer | ahb::gpu_cube_map;
  if ((d.usage & ~allowed) || !(d.usage & (ahb::gpu_sampled_image | ahb::gpu_framebuffer))) return std::nullopt;
  if ((d.usage & ahb::gpu_cube_map) && (d.width != d.height || d.layers % 6)) return std::nullopt;
  if (!hardware_buffers_supported()) return std::nullopt;
  auto image = canonical_image(gfx::gpu_device().physical_device, vk_format(d.format), d.width, d.height, 1,
                               d.layers);
  if (!image) return std::nullopt;
  if ((d.usage & ahb::gpu_framebuffer) &&
      !(image->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)))
    return std::nullopt;
  if ((d.usage & ahb::gpu_cube_map) && !(image->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)) return std::nullopt;
  return image;
}
std::shared_ptr<const android::GpuPayload> allocate_buffer(const android::HardwareBufferDesc& d) {
  auto image_info = buffer_image(d);
  auto* a = image_info ? allocator_device() : nullptr;
  if (!a) return nullptr;
  auto fn = [a](const char* name) { return a->gdpa(a->device, name); };
  auto& gpu = gfx::gpu_device();
  VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr,
                                           VkExternalMemoryHandleTypeFlags(memory_handle_type())};
  auto info = *image_info;
  info.pNext = &external;
  VkImage image{};
  if (reinterpret_cast<PFN_vkCreateImage>(fn("vkCreateImage"))(a->device, &info, nullptr, &image) != VK_SUCCESS)
    return nullptr;
  VkMemoryRequirements requirements{};
  reinterpret_cast<PFN_vkGetImageMemoryRequirements>(fn("vkGetImageMemoryRequirements"))(a->device, image,
                                                                                          &requirements);
  VkPhysicalDeviceMemoryProperties memory_properties{};
  physical_fn<PFN_vkGetPhysicalDeviceMemoryProperties>(gpu.physical_device, "vkGetPhysicalDeviceMemoryProperties")(
      gpu.physical_device, &memory_properties);
  std::optional<std::uint32_t> type;
  for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount && !type; ++i)
    if ((requirements.memoryTypeBits & (1u << i)) &&
        (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
      type = i;
  if (!type && requirements.memoryTypeBits) type = std::countr_zero(requirements.memoryTypeBits);
  std::shared_ptr<MemoryHandle> handle;
  if (type) {
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, image};
    VkExportMemoryAllocateInfo exported{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dedicated,
                                        VkExternalMemoryHandleTypeFlags(memory_handle_type())};
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exported, requirements.size, *type};
    VkDeviceMemory memory{};
    if (reinterpret_cast<PFN_vkAllocateMemory>(fn("vkAllocateMemory"))(a->device, &allocation, nullptr, &memory) ==
        VK_SUCCESS) {
      handle = export_memory_handle(a->gdpa, a->device, memory);
      reinterpret_cast<PFN_vkFreeMemory>(fn("vkFreeMemory"))(a->device, memory, nullptr);
    }
  }
  reinterpret_cast<PFN_vkDestroyImage>(fn("vkDestroyImage"))(a->device, image, nullptr);
  if (!handle) return nullptr;
  auto result = std::make_shared<Payload>();
  result->handle = std::move(handle);
  result->size = requirements.size;
  result->memory_type = *type;
  result->image = *image_info;
  return result;
}

// Producer writes (AImageReader frames) go through the allocator device's
// own queue: an image bound to an import of the payload receives a staged
// copy, then is released to external queues in GENERAL layout for the guest.
bool write_buffer(const android::GpuPayload& target, std::span<const std::uint8_t> rgba) {
  auto* p = dynamic_cast<const Payload*>(&target);
  auto* a = allocator_device();
  if (!p || !a || p->image.format != VK_FORMAT_R8G8B8A8_UNORM || p->image.arrayLayers != 1 ||
      !(p->image.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
      rgba.size() != std::size_t(p->image.extent.width) * p->image.extent.height * 4)
    return false;
  static std::mutex queue_mutex;  // the allocator device's single queue
  std::lock_guard lock(queue_mutex);
  auto fn = [a](const char* name) { return a->gdpa(a->device, name); };
  auto& gpu = gfx::gpu_device();
  const auto device = a->device;
  VkImage image{};
  VkDeviceMemory memory{}, staging_memory{};
  VkBuffer staging{};
  VkCommandPool pool{};
  VkFence fence{};
  bool done = false;
  [&] {
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr,
                                             VkExternalMemoryHandleTypeFlags(memory_handle_type())};
    auto info = p->image;
    info.pNext = &external;
    if (reinterpret_cast<PFN_vkCreateImage>(fn("vkCreateImage"))(device, &info, nullptr, &image) != VK_SUCCESS) return;
    auto import = import_memory_handle(*p->handle);
    if (!import) return;
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, image};
    import->pNext = reinterpret_cast<const VkBaseInStructure*>(&dedicated);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, import.get(), p->size, p->memory_type};
    auto allocate = reinterpret_cast<PFN_vkAllocateMemory>(fn("vkAllocateMemory"));
    if (allocate(device, &allocation, nullptr, &memory) != VK_SUCCESS) return;
    memory_handle_imported(*import);
    if (reinterpret_cast<PFN_vkBindImageMemory>(fn("vkBindImageMemory"))(device, image, memory, 0) != VK_SUCCESS)
      return;
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = rgba.size();
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (reinterpret_cast<PFN_vkCreateBuffer>(fn("vkCreateBuffer"))(device, &buffer_info, nullptr, &staging) !=
        VK_SUCCESS)
      return;
    VkMemoryRequirements requirements{};
    reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(fn("vkGetBufferMemoryRequirements"))(device, staging,
                                                                                              &requirements);
    VkPhysicalDeviceMemoryProperties properties{};
    physical_fn<PFN_vkGetPhysicalDeviceMemoryProperties>(gpu.physical_device, "vkGetPhysicalDeviceMemoryProperties")(
        gpu.physical_device, &properties);
    constexpr VkMemoryPropertyFlags host =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    std::optional<std::uint32_t> type;
    for (std::uint32_t i = 0; i < properties.memoryTypeCount && !type; ++i)
      if ((requirements.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & host) == host)
        type = i;
    if (!type) return;
    VkMemoryAllocateInfo staging_allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, *type};
    if (allocate(device, &staging_allocation, nullptr, &staging_memory) != VK_SUCCESS) return;
    if (reinterpret_cast<PFN_vkBindBufferMemory>(fn("vkBindBufferMemory"))(device, staging, staging_memory, 0) !=
        VK_SUCCESS)
      return;
    void* mapped = nullptr;
    if (reinterpret_cast<PFN_vkMapMemory>(fn("vkMapMemory"))(device, staging_memory, 0, VK_WHOLE_SIZE, 0, &mapped) !=
        VK_SUCCESS)
      return;
    std::memcpy(mapped, rgba.data(), rgba.size());
    reinterpret_cast<PFN_vkUnmapMemory>(fn("vkUnmapMemory"))(device, staging_memory);
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                      VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, gpu.queue_family};
    if (reinterpret_cast<PFN_vkCreateCommandPool>(fn("vkCreateCommandPool"))(device, &pool_info, nullptr, &pool) !=
        VK_SUCCESS)
      return;
    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
                                             VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer commands{};
    if (reinterpret_cast<PFN_vkAllocateCommandBuffers>(fn("vkAllocateCommandBuffers"))(device, &command_info,
                                                                                        &commands) != VK_SUCCESS)
      return;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    reinterpret_cast<PFN_vkBeginCommandBuffer>(fn("vkBeginCommandBuffer"))(commands, &begin);
    auto barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(fn("vkCmdPipelineBarrier"));
    VkImageMemoryBarrier to_copy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image,
                                 {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    barrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
            &to_copy);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = p->image.extent;
    reinterpret_cast<PFN_vkCmdCopyBufferToImage>(fn("vkCmdCopyBufferToImage"))(
        commands, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkImageMemoryBarrier release{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, gpu.queue_family,
                                 VK_QUEUE_FAMILY_EXTERNAL, image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
            1, &release);
    if (reinterpret_cast<PFN_vkEndCommandBuffer>(fn("vkEndCommandBuffer"))(commands) != VK_SUCCESS) return;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (reinterpret_cast<PFN_vkCreateFence>(fn("vkCreateFence"))(device, &fence_info, nullptr, &fence) != VK_SUCCESS)
      return;
    VkQueue queue{};
    reinterpret_cast<PFN_vkGetDeviceQueue>(fn("vkGetDeviceQueue"))(device, gpu.queue_family, 0, &queue);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    if (reinterpret_cast<PFN_vkQueueSubmit>(fn("vkQueueSubmit"))(queue, 1, &submit, fence) != VK_SUCCESS) return;
    done = reinterpret_cast<PFN_vkWaitForFences>(fn("vkWaitForFences"))(device, 1, &fence, VK_TRUE, UINT64_MAX) ==
           VK_SUCCESS;
  }();
  if (fence) reinterpret_cast<PFN_vkDestroyFence>(fn("vkDestroyFence"))(device, fence, nullptr);
  if (pool) reinterpret_cast<PFN_vkDestroyCommandPool>(fn("vkDestroyCommandPool"))(device, pool, nullptr);
  if (staging) reinterpret_cast<PFN_vkDestroyBuffer>(fn("vkDestroyBuffer"))(device, staging, nullptr);
  auto free = reinterpret_cast<PFN_vkFreeMemory>(fn("vkFreeMemory"));
  if (staging_memory) free(device, staging_memory, nullptr);
  if (image) reinterpret_cast<PFN_vkDestroyImage>(fn("vkDestroyImage"))(device, image, nullptr);
  if (memory) free(device, memory, nullptr);
  return done;
}

VkResult VKAPI_CALL image_properties2(VkPhysicalDevice p, const VkPhysicalDeviceImageFormatInfo2* info,
                                      VkImageFormatProperties2* out) {
  auto* external = find<const VkPhysicalDeviceExternalImageFormatInfo>(
      info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
  if (!external || external->handleType != ahb_handle)
    return next_fn<PFN_vkGetPhysicalDeviceImageFormatProperties2, "vkGetPhysicalDeviceImageFormatProperties2">(p)(
        p, info, out);
  out->imageFormatProperties = {};
  auto* properties = find<VkExternalImageFormatProperties>(out->pNext,
                                                           VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES);
  if (properties) properties->externalMemoryProperties = {};
  constexpr VkImageCreateFlags allowed = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  auto usage = hardware_buffers_supported() && info->type == VK_IMAGE_TYPE_2D &&
                       info->tiling == VK_IMAGE_TILING_OPTIMAL && !(info->flags & ~allowed)
                   ? canonical_usage(p, info->format)
                   : std::nullopt;
  const auto flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | (info->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
  VkImageFormatProperties limits{};
  if (!usage || (info->usage & ~*usage) || !host_supports(p, info->format, *usage, flags, &limits))
    return VK_ERROR_FORMAT_NOT_SUPPORTED;
  limits.sampleCounts = VK_SAMPLE_COUNT_1_BIT;
  out->imageFormatProperties = limits;
  if (properties)
    properties->externalMemoryProperties = {VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT |
                                                VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
                                                VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,
                                            ahb_handle, ahb_handle};
  if (auto* u = find<VkAndroidHardwareBufferUsageANDROID>(out->pNext,
                                                          VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID))
    u->androidHardwareBufferUsage = ahb_usage(info->usage, info->flags, false);
  if (auto* y = find<VkSamplerYcbcrConversionImageFormatProperties>(
          out->pNext, VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES))
    y->combinedImageSamplerDescriptorCount = 1;
  return VK_SUCCESS;
}

// The host create info for a guest image bound to AHB memory, or nullopt for
// ordinary images. Fails with `result` set when the shape has no AHB form.
std::optional<Image> ahb_image(VkDevice d, const VkImageCreateInfo& info, VkResult& result) {
  result = VK_SUCCESS;
  auto* external = find<const VkExternalMemoryImageCreateInfo>(info.pNext,
                                                               VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
  auto* format_node = find<const VkExternalFormatANDROID>(info.pNext, VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID);
  const auto external_format = format_node ? format_node->externalFormat : 0;
  if ((!external || !(external->handleTypes & ahb_handle)) && !external_format) return std::nullopt;
  auto format = external_format ? vk_format(external_format) : info.format;
  std::optional<VkImageCreateInfo> canonical;
  if (format && (!external_format || info.format == VK_FORMAT_UNDEFINED) && info.imageType == VK_IMAGE_TYPE_2D &&
      info.tiling == VK_IMAGE_TILING_OPTIMAL && info.samples == VK_SAMPLE_COUNT_1_BIT && info.extent.depth == 1)
    canonical = canonical_image(device_physical(d), format, info.extent.width, info.extent.height, info.mipLevels,
                                info.arrayLayers);
  if (!canonical || (info.usage & ~canonical->usage) || (info.flags & ~canonical->flags)) {
    log::error("vulkan", "AHardwareBuffer image unsupported: format={} external={} usage={:#x} flags={:#x}",
               int(info.format), external_format, info.usage, info.flags);
    result = VK_ERROR_FORMAT_NOT_SUPPORTED;
    return std::nullopt;
  }
  return Image{d, *canonical, external_format ? std::uint32_t(external_format) : ahb_format(format),
               external_format != 0, info.usage, info.flags};
}
VkImageCreateInfo host_image(const Image& image, const VkImageCreateInfo& guest,
                             VkExternalMemoryImageCreateInfo& external) {
  external = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr,
              VkExternalMemoryHandleTypeFlags(memory_handle_type())};
  auto host = image.canonical;
  host.pNext = &external;
  host.sharingMode = guest.sharingMode;
  host.queueFamilyIndexCount = guest.queueFamilyIndexCount;
  host.pQueueFamilyIndices = guest.pQueueFamilyIndices;
  return host;
}
VkResult VKAPI_CALL create_image(VkDevice d, const VkImageCreateInfo* info, const VkAllocationCallbacks* alloc,
                                 VkImage* out) {
  VkResult result;
  auto image = ahb_image(d, *info, result);
  if (result != VK_SUCCESS) return result;
  if (!image) return next_fn<PFN_vkCreateImage, "vkCreateImage">(d)(d, info, alloc, out);
  VkExternalMemoryImageCreateInfo external;
  auto host = host_image(*image, *info, external);
  result = device_fn<PFN_vkCreateImage>(d, "vkCreateImage")(d, &host, alloc, out);
  if (result == VK_SUCCESS) {
    std::lock_guard lock(state_lock);
    images[*out] = *image;
  }
  return result;
}
void VKAPI_CALL destroy_image(VkDevice d, VkImage image, const VkAllocationCallbacks* alloc) {
  {
    std::lock_guard lock(state_lock);
    images.erase(image);
  }
  next_fn<PFN_vkDestroyImage, "vkDestroyImage">(d)(d, image, alloc);
}
void VKAPI_CALL device_image_requirements(VkDevice d, const VkDeviceImageMemoryRequirements* info,
                                         VkMemoryRequirements2* out) {
  VkResult result;
  auto image = ahb_image(d, *info->pCreateInfo, result);
  if (result != VK_SUCCESS) {
    out->memoryRequirements = {};
    return;
  }
  if (!image)
    return next_fn<PFN_vkGetDeviceImageMemoryRequirements, "vkGetDeviceImageMemoryRequirements">(d)(d, info, out);
  VkExternalMemoryImageCreateInfo external;
  auto host = host_image(*image, *info->pCreateInfo, external);
  auto copy = *info;
  copy.pCreateInfo = &host;
  device_fn<PFN_vkGetDeviceImageMemoryRequirements>(d, "vkGetDeviceImageMemoryRequirements")(d, &copy, out);
}
VkResult VKAPI_CALL create_view(VkDevice d, const VkImageViewCreateInfo* info, const VkAllocationCallbacks* alloc,
                                VkImageView* out) {
  auto copy = *info;
  if (auto* node = find<const VkSamplerYcbcrConversionInfo>(info->pNext,
                                                            VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO)) {
    if (auto components = conversion_components(node->conversion)) {
      unlink(&copy, VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO);
      copy.components = *components;  // the conversion's mapping; the view's own must be identity
    }
  }
  if (copy.format == VK_FORMAT_UNDEFINED)
    if (auto image = image_record(info->image); image && image->external_format) copy.format = image->canonical.format;
  return next_fn<PFN_vkCreateImageView, "vkCreateImageView">(d)(d, &copy, alloc, out);
}
VkResult VKAPI_CALL create_sampler(VkDevice d, const VkSamplerCreateInfo* info, const VkAllocationCallbacks* alloc,
                                   VkSampler* out) {
  auto copy = *info;
  if (auto* node = find<const VkSamplerYcbcrConversionInfo>(info->pNext,
                                                            VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO);
      node && conversion_components(node->conversion))
    unlink(&copy, VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO);
  return device_fn<PFN_vkCreateSampler>(d, "vkCreateSampler")(d, &copy, alloc, out);
}
VkResult VKAPI_CALL create_conversion(VkDevice d, const VkSamplerYcbcrConversionCreateInfo* info,
                                      const VkAllocationCallbacks* alloc, VkSamplerYcbcrConversion* out) {
  auto* node = find<const VkExternalFormatANDROID>(info->pNext, VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID);
  if (!node || !node->externalFormat)
    return device_fn<PFN_vkCreateSamplerYcbcrConversion>(d, "vkCreateSamplerYcbcrConversion")(d, info, alloc, out);
  if (!vk_format(node->externalFormat)) {
    log::error("vulkan", "unknown Android external format {}", node->externalFormat);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  // Every external format here is RGB: the suggested model is RGB_IDENTITY
  // with full range, which leaves texels unchanged apart from the mapping.
  if (info->ycbcrModel != VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY)
    log::warn("vulkan", "YCbCr model {} on an RGB Android external format is sampled as RGB", int(info->ycbcrModel));
  Conversion conversion{d, info->components, std::make_unique<char>()};
  auto handle = reinterpret_cast<VkSamplerYcbcrConversion>(conversion.token.get());
  std::lock_guard lock(state_lock);
  conversions[handle] = std::move(conversion);
  *out = handle;
  return VK_SUCCESS;
}
void VKAPI_CALL destroy_conversion(VkDevice d, VkSamplerYcbcrConversion conversion,
                                   const VkAllocationCallbacks* alloc) {
  {
    std::lock_guard lock(state_lock);
    if (conversions.erase(conversion)) return;
  }
  device_fn<PFN_vkDestroySamplerYcbcrConversion>(d, "vkDestroySamplerYcbcrConversion")(d, conversion, alloc);
}

VkResult VKAPI_CALL allocate_memory(VkDevice d, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks* alloc,
                                    VkDeviceMemory* out) {
  auto* import = find<const VkImportAndroidHardwareBufferInfoANDROID>(
      info->pNext, VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID);
  auto* exported = find<const VkExportMemoryAllocateInfo>(info->pNext, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
  const bool export_ahb = exported && (exported->handleTypes & ahb_handle);
  if (!import && !export_ahb) return next_fn<PFN_vkAllocateMemory, "vkAllocateMemory">(d)(d, info, alloc, out);
  auto* dedicated = find<const VkMemoryDedicatedAllocateInfo>(info->pNext,
                                                              VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
  auto image = dedicated && dedicated->image ? image_record(dedicated->image) : std::nullopt;
  if (!image || image->device != d) {
    log::error("vulkan", "AHardwareBuffer memory needs a dedicated AHB image; AHB buffers are not provided");
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  }
  // Host chain: the platform import/export node, then the guest's remaining nodes.
  auto copy = *info;
  const auto other_exports = exported ? exported->handleTypes & ~VkExternalMemoryHandleTypeFlags(ahb_handle) : 0;
  unlink(&copy, VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID);
  unlink(&copy, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
  // An import that is also exported as an AHB returns the imported buffer.
  VkExportMemoryAllocateInfo host_export{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, copy.pNext,
                                         other_exports | (export_ahb && !import ? memory_handle_type() : 0)};
  if (host_export.handleTypes) copy.pNext = &host_export;
  std::shared_ptr<const Payload> source;
  std::shared_ptr<VkBaseInStructure> import_node;
  GuestAddr buffer = 0;
  if (import) {
    buffer = h2g(import->buffer);
    source = payload(buffer);
    if (!source || !same_image(source->image, image->canonical) || info->memoryTypeIndex != source->memory_type) {
      log::error("vulkan", "AHardwareBuffer import does not match the dedicated image or memory type");
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    import_node = import_memory_handle(*source->handle);
    if (!import_node) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    import_node->pNext = static_cast<const VkBaseInStructure*>(copy.pNext);
    copy.pNext = import_node.get();
    copy.allocationSize = source->size;
  } else {
    VkMemoryRequirements requirements{};
    device_fn<PFN_vkGetImageMemoryRequirements>(d, "vkGetImageMemoryRequirements")(d, dedicated->image,
                                                                                   &requirements);
    copy.allocationSize = requirements.size;  // the guest passes zero for AHB exports
  }
  auto result = device_fn<PFN_vkAllocateMemory>(d, "vkAllocateMemory")(d, &copy, alloc, out);
  if (result != VK_SUCCESS) return result;
  if (import) {
    memory_handle_imported(*import_node);
    android::acquire_hardware_buffer(buffer);
  } else {
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(raw_device_proc(d, "vkGetDeviceProcAddr"));
    auto handle = gdpa ? export_memory_handle(gdpa, d, *out) : nullptr;
    if (!handle) {
      device_fn<PFN_vkFreeMemory>(d, "vkFreeMemory")(d, *out, alloc);
      *out = VK_NULL_HANDLE;
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    auto exported_payload = std::make_shared<Payload>();
    exported_payload->handle = std::move(handle);
    exported_payload->size = copy.allocationSize;
    exported_payload->memory_type = info->memoryTypeIndex;
    exported_payload->image = image->canonical;
    const auto& c = image->canonical;
    android::HardwareBufferDesc desc{c.extent.width, c.extent.height, c.arrayLayers, image->format,
                                     ahb_usage(image->usage, image->flags,
                                               c.mipLevels > 1 && c.mipLevels == full_mips(c.extent.width, c.extent.height)),
                                     c.extent.width, 0, 0};
    buffer = android::publish_hardware_buffer(desc, std::move(exported_payload));
  }
  std::lock_guard lock(state_lock);
  memories[*out] = {d, buffer};
  return VK_SUCCESS;
}
void VKAPI_CALL free_memory(VkDevice d, VkDeviceMemory memory, const VkAllocationCallbacks* alloc) {
  GuestAddr buffer = 0;
  {
    std::lock_guard lock(state_lock);
    if (auto it = memories.find(memory); it != memories.end()) {
      buffer = it->second.buffer;
      memories.erase(it);
    }
  }
  next_fn<PFN_vkFreeMemory, "vkFreeMemory">(d)(d, memory, alloc);
  if (buffer) android::release_hardware_buffer(buffer);
}
VkResult VKAPI_CALL buffer_properties(VkDevice d, const AHardwareBuffer* buffer,
                                      VkAndroidHardwareBufferPropertiesANDROID* out) {
  android::HardwareBufferDesc desc{};
  auto source = payload(h2g(buffer), &desc);
  if (!source) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  out->allocationSize = source->size;
  out->memoryTypeBits = 1u << source->memory_type;
  auto physical = device_physical(d);
  VkFormatProperties format_properties{};
  physical_fn<PFN_vkGetPhysicalDeviceFormatProperties>(physical, "vkGetPhysicalDeviceFormatProperties")(
      physical, source->image.format, &format_properties);
  auto features = format_properties.optimalTilingFeatures | VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT;
  if (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
    features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT;
  VkComponentMapping components{};  // identity
  if (desc.format == format_rgbx) components.a = VK_COMPONENT_SWIZZLE_ONE;
  for (auto* p = static_cast<VkBaseOutStructure*>(out->pNext); p; p = p->pNext) {
    if (p->sType == VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID) {
      auto& f = *reinterpret_cast<VkAndroidHardwareBufferFormatPropertiesANDROID*>(p);
      f.format = source->image.format;
      f.externalFormat = desc.format;
      f.formatFeatures = features;
      f.samplerYcbcrConversionComponents = components;
      f.suggestedYcbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
      f.suggestedYcbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
      f.suggestedXChromaOffset = f.suggestedYChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
    } else if (p->sType == VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_2_ANDROID) {
      auto& f = *reinterpret_cast<VkAndroidHardwareBufferFormatProperties2ANDROID*>(p);
      f.format = source->image.format;
      f.externalFormat = desc.format;
      f.formatFeatures = features;
      f.samplerYcbcrConversionComponents = components;
      f.suggestedYcbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
      f.suggestedYcbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
      f.suggestedXChromaOffset = f.suggestedYChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
    }
  }
  return VK_SUCCESS;
}
VkResult VKAPI_CALL memory_buffer(VkDevice, const VkMemoryGetAndroidHardwareBufferInfoANDROID* info,
                                  AHardwareBuffer** out) {
  GuestAddr buffer = 0;
  {
    std::lock_guard lock(state_lock);
    if (auto it = memories.find(info->memory); it != memories.end()) buffer = it->second.buffer;
  }
  if (!buffer) return VK_ERROR_OUT_OF_HOST_MEMORY;
  android::acquire_hardware_buffer(buffer);  // each call returns a new reference
  *out = g2h<AHardwareBuffer>(buffer);
  return VK_SUCCESS;
}
}  // namespace

bool hardware_buffers_supported() {
  static const bool value = [] {
    try {
      if (!memory_handle_type()) return false;
      auto& gpu = gfx::gpu_device();
      if (gpu.api_version < VK_API_VERSION_1_1) return false;
      auto physical = gpu.physical_device;
      VkPhysicalDeviceProperties properties{};
      physical_fn<PFN_vkGetPhysicalDeviceProperties>(physical, "vkGetPhysicalDeviceProperties")(physical,
                                                                                                 &properties);
      if (properties.apiVersion < VK_API_VERSION_1_1 || !host_extension(physical, memory_handle_extension()))
        return false;
      VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
      VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &ycbcr};
      physical_fn<PFN_vkGetPhysicalDeviceFeatures2>(physical, "vkGetPhysicalDeviceFeatures2")(physical, &features);
      return ycbcr.samplerYcbcrConversion == VK_TRUE;
    } catch (const std::exception&) {
      return false;
    }
  }();
  return value;
}
bool hardware_buffer_extension(std::string_view name) {
  return name == ahb_extension || name == foreign_extension;
}
void hardware_buffer_extensions(std::vector<VkExtensionProperties>& values) {
  if (!hardware_buffers_supported()) return;
  values.push_back({"VK_ANDROID_external_memory_android_hardware_buffer",
                    VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_SPEC_VERSION});
  if (!std::ranges::any_of(values, [](auto& e) { return foreign_extension == e.extensionName; }))
    values.push_back({"VK_EXT_queue_family_foreign", VK_EXT_QUEUE_FAMILY_FOREIGN_SPEC_VERSION});
}
const char* hardware_buffer_host_extension(const char* name) {
  if (name == ahb_extension) return memory_handle_extension();
  if (name == foreign_extension && !host_foreign_queue()) return nullptr;  // translated to EXTERNAL
  return name;
}
void hardware_buffer_chain(VkBaseOutStructure* p) {
  // Foreign agents here are host Vulkan users of the same payload, which is
  // what VK_QUEUE_FAMILY_EXTERNAL denotes.
  auto translate = [](std::uint32_t& family) {
    if (family == VK_QUEUE_FAMILY_FOREIGN_EXT && !host_foreign_queue()) family = VK_QUEUE_FAMILY_EXTERNAL;
  };
  switch (p->sType) {
    case VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER: {
      auto& b = *reinterpret_cast<VkImageMemoryBarrier*>(p);
      translate(b.srcQueueFamilyIndex);
      translate(b.dstQueueFamilyIndex);
      break;
    }
    case VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER: {
      auto& b = *reinterpret_cast<VkBufferMemoryBarrier*>(p);
      translate(b.srcQueueFamilyIndex);
      translate(b.dstQueueFamilyIndex);
      break;
    }
    case VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2: {
      auto& b = *reinterpret_cast<VkImageMemoryBarrier2*>(p);
      translate(b.srcQueueFamilyIndex);
      translate(b.dstQueueFamilyIndex);
      break;
    }
    case VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2: {
      auto& b = *reinterpret_cast<VkBufferMemoryBarrier2*>(p);
      translate(b.srcQueueFamilyIndex);
      translate(b.dstQueueFamilyIndex);
      break;
    }
    default:
      break;
  }
}
void release_hardware_buffer_device(VkDevice device) {
  std::vector<GuestAddr> released;
  {
    std::lock_guard lock(state_lock);
    std::erase_if(images, [&](auto& e) { return e.second.device == device; });
    std::erase_if(conversions, [&](auto& e) { return e.second.device == device; });
    std::erase_if(memories, [&](auto& e) {
      if (e.second.device != device) return false;
      released.push_back(e.second.buffer);
      return true;
    });
  }
  for (auto buffer : released) android::release_hardware_buffer(buffer);
}
void register_hardware_buffer_allocator() {
  android::set_gpu_allocator({[](const android::HardwareBufferDesc& d) { return buffer_image(d).has_value(); },
                              allocate_buffer, write_buffer});
}
PFN_vkVoidFunction hardware_buffer_proc(std::string_view name) {
#define FN(vk, fn) \
  if (name == #vk) return reinterpret_cast<PFN_vkVoidFunction>(fn)
  FN(vkGetPhysicalDeviceImageFormatProperties2, image_properties2);
  FN(vkCreateImage, create_image);
  FN(vkDestroyImage, destroy_image);
  FN(vkGetDeviceImageMemoryRequirements, device_image_requirements);
  FN(vkCreateImageView, create_view);
  FN(vkCreateSampler, create_sampler);
  FN(vkCreateSamplerYcbcrConversion, create_conversion);
  FN(vkDestroySamplerYcbcrConversion, destroy_conversion);
  FN(vkAllocateMemory, allocate_memory);
  FN(vkFreeMemory, free_memory);
  FN(vkGetAndroidHardwareBufferPropertiesANDROID, buffer_properties);
  FN(vkGetMemoryAndroidHardwareBufferANDROID, memory_buffer);
#undef FN
  return nullptr;
}
}  // namespace venture::vulkan
