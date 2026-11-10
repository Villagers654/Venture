#include "xr/venture_nr.h"
#include "xr/ngx_bridge.h"
#include "common/environment.h"
#include "common/log.h"
#include "hle/vulkan/internal.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace venture::hle::openxr {
double render_scale();
}

namespace venture::xr::venture_nr {
#ifndef _WIN32
struct NeuralRenderer::Impl {};
std::unique_ptr<NeuralRenderer> NeuralRenderer::create(const XrGraphicsBindingVulkanKHR&, PFN_vkGetInstanceProcAddr,
                                                       const XrSwapchainCreateInfo&) {
  return nullptr;
}
NeuralRenderer::NeuralRenderer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
NeuralRenderer::~NeuralRenderer() = default;
bool NeuralRenderer::record(VkCommandBuffer, VkImage, VkImage, VkFormat, VkExtent2D, bool, VkImageLayout) {
  return false;
}
#else
namespace {

std::wstring widen(const char* text) {
  if (!text || !*text) return {};
  const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
  std::wstring out(std::size_t(std::max(length, 1)), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), length);
  out.resize(std::wcslen(out.c_str()));
  return out;
}

// NGX state is per process and bound to one device.
std::mutex init_mutex;
VkDevice ngx_device{};
bool ngx_failed = false;
void release_device(VkDevice device) {
  std::lock_guard lock(init_mutex);
  if (device != ngx_device) return;
  if (auto* b = ngx_bridge::get()) b->shutdown(device);
  ngx_device = VK_NULL_HANDLE;
  ngx_failed = false;
}
bool initialize(const XrGraphicsBindingVulkanKHR& binding, PFN_vkGetInstanceProcAddr get) {
  std::lock_guard lock(init_mutex);
  if (ngx_device) return ngx_device == binding.device;
  if (ngx_failed) return false;
  auto* b = ngx_bridge::get();
  if (!b) return ngx_failed = true, false;
  std::wstring data = widen(venture::environment::getenv("LOCALAPPDATA"));
  data = data.empty() ? ngx_bridge::runtime_directory() : data + L"\\Venture\\ngx";
  CreateDirectoryW((data.substr(0, data.find_last_of(L'\\'))).c_str(), nullptr);
  CreateDirectoryW(data.c_str(), nullptr);
  // The folder with nvngx_dlssnr.dll (the launcher's NVIDIA DLLs folder), else the runtime's.
  std::wstring dlss = widen(venture::environment::getenv("VENTURE_DLSS_PATH"));
  if (dlss.empty()) dlss = ngx_bridge::runtime_directory();
  VentureNgxInit info{VENTURE_NGX_ABI_VERSION,
                      binding.instance,
                      binding.physicalDevice,
                      binding.device,
                      get,
                      reinterpret_cast<PFN_vkGetDeviceProcAddr>(get(binding.instance, "vkGetDeviceProcAddr")),
                      data.c_str(),
                      dlss.c_str()};
  const auto result = b->init(&info);
  if (!ngx_bridge::succeeded(result)) {
    log::warn("venture-nr", "NGX Vulkan initialization failed (0x{:08X}); Venture NR is off", result);
    return ngx_failed = true, false;
  }
  ngx_device = binding.device;
  vulkan::add_device_release_hook(release_device);
  log::info("venture-nr", "NGX Vulkan initialized");
  return true;
}
}  // namespace

// ---------------------------------------------------------------------------
// Venture NR: DLSS-NR on a foveated region of each eye at the host copy.
namespace {
struct NrSettings {
  bool on = true;
  float width = 0.35f, height = 0.35f;
  VentureNgxNrTuning tuning{2, 0, 1.0f, 0.0f, 0.7f, 0.5f};
  bool debug = false;
  float working_scale = 1.f;   // NR at this fraction of the region; its correction is applied full size
  bool cache = false;          // NR every `interval` frames, its last correction on the frames between
  std::uint32_t interval = 2;
  float transition = 0.f;      // the region's edge fades over this fraction of it
  float roundness = 0.f;       // the region's corners: 0 square, 1 an ellipse
  bool split = false;          // NR on the region's left half only (A/B comparison)
};
// The launcher's values (SOFTWARE\Venture\NR), read again a few times a second.
NrSettings read_nr_settings(NrSettings current) {
  HKEY key{};
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Venture\\NR", 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
    return current;
  const auto dword = [&](const wchar_t* name) -> std::optional<DWORD> {
    DWORD value{}, size = sizeof(value), type{};
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
        type == REG_DWORD)
      return value;
    return std::nullopt;
  };
  if (const auto v = dword(L"On")) current.on = *v != 0;
  if (const auto v = dword(L"Width")) current.width = std::clamp(float(*v) / 100.f, 0.1f, 1.f);
  if (const auto v = dword(L"Height")) current.height = std::clamp(float(*v) / 100.f, 0.1f, 1.f);
  if (const auto v = dword(L"Preset")) current.tuning.preset = std::min<DWORD>(*v, 2);
  if (const auto v = dword(L"Style")) current.tuning.style = std::min<DWORD>(*v, 2);
  if (const auto v = dword(L"Intensity")) current.tuning.intensity = std::min<DWORD>(*v, 100) / 100.f;
  if (const auto v = dword(L"Tone")) current.tuning.local_tone = std::min<DWORD>(*v, 200) / 100.f;
  if (const auto v = dword(L"Structure")) current.tuning.local_structure = std::min<DWORD>(*v, 200) / 100.f;
  if (const auto v = dword(L"Skin")) current.tuning.skin_structure = std::min<DWORD>(*v, 200) / 100.f;
  if (const auto v = dword(L"Debug")) current.debug = *v != 0;
  if (const auto v = dword(L"WorkingScale")) current.working_scale = std::clamp(float(*v) / 100.f, 0.25f, 1.f);
  if (const auto v = dword(L"Cache")) current.cache = *v != 0;
  if (const auto v = dword(L"Interval")) current.interval = std::clamp<std::uint32_t>(*v, 2, 8);
  if (const auto v = dword(L"Transition")) current.transition = std::min<DWORD>(*v, 50) / 100.f;
  if (const auto v = dword(L"Roundness")) current.roundness = std::min<DWORD>(*v, 100) / 100.f;
  if (const auto v = dword(L"Split")) current.split = *v != 0;
  RegCloseKey(key);
  return current;
}
// The same texels, read and written without an sRGB transfer (NR works on the encoded values, as VDXR's).
namespace nr_apply_shader {
#include "nr_apply.inc"
}
VkFormat raw_format(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    default: return format;
  }
}
}  // namespace

struct NeuralRenderer::Impl {
  XrGraphicsBindingVulkanKHR binding{};
  PFN_vkGetInstanceProcAddr get{};
  XrSwapchainCreateInfo guest{};
  bool failed = false;
  NrSettings settings;
  std::chrono::steady_clock::time_point next_read{};
  struct Image {
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    VkFormat format{};
    VkExtent2D extent{};
  };
  struct Eye {
    // The region as stored and in float (full size), NR's input and output at its working size, the region as shown.
    Image in_raw, proxy, low_in, neural, motion, depth, result, out_raw;
    VkDescriptorSet set{};         // nr_apply.comp: proxy, low_in, neural -> result
    void* feature{};
    VkExtent2D size{};             // the region, guest pixels
    VkExtent2D work{};             // NR's working size
    VkExtent2D host{};             // the region, host pixels
    std::uint32_t age = 0;         // frames since NR last ran (cache)
    bool correction = false;       // low_in / neural hold a correction
    VentureNgxNrTuning created{};  // the preset the feature was made with
    VkOffset2D offset{-1, -1};     // the region's top left last frame
    bool reset = true;
  };
  std::array<Eye, 2> eyes;
  Image green, red;  // 1 x 1, the NR and VRS outlines' colors
  // nr_apply.comp.
  VkDescriptorSetLayout set_layout{};
  VkPipelineLayout pipeline_layout{};
  VkPipeline pipeline{};
  VkSampler sampler{};
  VkDescriptorPool pool{};
  bool green_ready = false;
  std::uint32_t failures = 0;

  template <class T> T fn(const char* name) const { return reinterpret_cast<T>(get(binding.instance, name)); }

  bool make(Image& out, VkFormat format, VkExtent2D extent, VkImageUsageFlags usage, bool view) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {extent.width, extent.height, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    if (fn<PFN_vkCreateImage>("vkCreateImage")(binding.device, &ci, nullptr, &out.image) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    fn<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(binding.device, out.image, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    fn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(binding.physicalDevice, &memory);
    std::uint32_t type = 0;
    while (type < memory.memoryTypeCount &&
           (!(requirements.memoryTypeBits & (1u << type)) ||
            !(memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
      ++type;
    if (type == memory.memoryTypeCount) return false;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, type};
    if (fn<PFN_vkAllocateMemory>("vkAllocateMemory")(binding.device, &ai, nullptr, &out.memory) != VK_SUCCESS ||
        fn<PFN_vkBindImageMemory>("vkBindImageMemory")(binding.device, out.image, out.memory, 0) != VK_SUCCESS)
      return false;
    out.format = format;
    out.extent = extent;
    if (!view) return true;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return fn<PFN_vkCreateImageView>("vkCreateImageView")(binding.device, &vi, nullptr, &out.view) == VK_SUCCESS;
  }
  void destroy(Image& image) {
    if (image.view) fn<PFN_vkDestroyImageView>("vkDestroyImageView")(binding.device, image.view, nullptr);
    if (image.image) fn<PFN_vkDestroyImage>("vkDestroyImage")(binding.device, image.image, nullptr);
    if (image.memory) fn<PFN_vkFreeMemory>("vkFreeMemory")(binding.device, image.memory, nullptr);
    image = {};
  }
  void destroy(Eye& eye) {
    if (eye.feature)
      if (auto* b = ngx_bridge::get()) b->release(eye.feature);
    if (eye.set) fn<PFN_vkFreeDescriptorSets>("vkFreeDescriptorSets")(binding.device, pool, 1, &eye.set);
    for (auto* image : {&eye.in_raw, &eye.proxy, &eye.low_in, &eye.neural, &eye.motion, &eye.depth, &eye.result,
                        &eye.out_raw})
      destroy(*image);
    eye = {};
  }
  bool create_pipeline() {
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    const VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 4,
                                                   bindings};
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};  // nr_apply.comp's parameters
    const VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                                               sizeof(nr_apply_shader::decode_shader), nr_apply_shader::decode_shader};
    VkShaderModule shader{};
    bool ok = fn<PFN_vkCreateShaderModule>("vkCreateShaderModule")(binding.device, &shader_info, nullptr, &shader) ==
                  VK_SUCCESS &&
              fn<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(binding.device, &set_info, nullptr,
                                                                                 &set_layout) == VK_SUCCESS;
    if (ok) {
      const VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1,
                                                   &set_layout, 1, &range};
      ok = fn<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(binding.device, &layout_info, nullptr,
                                                                    &pipeline_layout) == VK_SUCCESS;
    }
    if (ok) {
      VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
      info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, shader,
                    "main", nullptr};
      info.layout = pipeline_layout;
      info.basePipelineIndex = -1;
      ok = fn<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(binding.device, VK_NULL_HANDLE, 1, &info,
                                                                        nullptr, &pipeline) == VK_SUCCESS;
    }
    if (shader) fn<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(binding.device, shader, nullptr);
    if (ok) {
      VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
      si.magFilter = si.minFilter = VK_FILTER_LINEAR;
      si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      ok = fn<PFN_vkCreateSampler>("vkCreateSampler")(binding.device, &si, nullptr, &sampler) == VK_SUCCESS;
    }
    if (ok) {
      const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4}};
      const VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr,
                                                 VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, 4, 2, sizes};
      ok = fn<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(binding.device, &pool_info, nullptr, &pool) ==
           VK_SUCCESS;
    }
    return ok;
  }
  void barrier(VkCommandBuffer command, VkImage image, VkImageLayout before, VkImageLayout after,
               std::uint32_t first_layer = 0, std::uint32_t layers = 1) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.image = image;
    b.oldLayout = before;
    b.newLayout = after;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.srcAccessMask = before == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, first_layer, layers};
    fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                                                         1, &b);
  }
  bool blittable(VkFormat format, VkFormatFeatureFlags needed) const {
    VkFormatProperties properties{};
    fn<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(binding.physicalDevice, format,
                                                                                       &properties);
    return (properties.optimalTilingFeatures & needed) == needed;
  }
  // The eye's working images and NR feature for a region of `size` (guest) shown at `host` (host pixels).
  bool prepare(VkCommandBuffer command, Eye& eye, VkExtent2D size, VkExtent2D host, VkFormat target_format) {
    const auto& t = settings.tuning;
    const auto scaled = [&](std::uint32_t v) {
      return std::min(v, std::max(64u, (std::uint32_t(float(v) * settings.working_scale) + 7) / 8 * 8));
    };
    const VkExtent2D work_size{scaled(size.width), scaled(size.height)};
    const bool same_size = eye.feature && eye.size.width == size.width && eye.size.height == size.height &&
                           eye.work.width == work_size.width && eye.work.height == work_size.height &&
                           eye.host.width == host.width && eye.host.height == host.height &&
                           eye.out_raw.format == raw_format(target_format);
    if (same_size && eye.created.preset == t.preset) return true;
    destroy(eye);
    constexpr VkImageUsageFlags copy = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    constexpr VkImageUsageFlags work = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!make(eye.in_raw, raw_format(VkFormat(guest.format)), size, copy, false) ||
        !make(eye.proxy, VK_FORMAT_R16G16B16A16_SFLOAT, size, copy | work, true) ||
        !make(eye.low_in, VK_FORMAT_R16G16B16A16_SFLOAT, work_size, copy | work, true) ||
        !make(eye.neural, VK_FORMAT_R16G16B16A16_SFLOAT, work_size, copy | work, true) ||
        !make(eye.motion, VK_FORMAT_R16G16_SFLOAT, work_size, copy | work, true) ||
        !make(eye.depth, VK_FORMAT_R32_SFLOAT, work_size, copy | work, true) ||
        !make(eye.result, VK_FORMAT_R16G16B16A16_SFLOAT, size, copy | work, true) ||
        !make(eye.out_raw, raw_format(target_format), host, copy, false)) {
      log::warn("venture-nr", "Venture NR: working images ({}x{}) could not be created", size.width, size.height);
      return false;
    }
    {
      const VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, pool, 1,
                                                   &set_layout};
      if (fn<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(binding.device, &allocation, &eye.set) !=
          VK_SUCCESS)
        return false;
      const VkDescriptorImageInfo images[] = {{sampler, eye.proxy.view, VK_IMAGE_LAYOUT_GENERAL},
                                              {sampler, eye.low_in.view, VK_IMAGE_LAYOUT_GENERAL},
                                              {sampler, eye.neural.view, VK_IMAGE_LAYOUT_GENERAL},
                                              {VK_NULL_HANDLE, eye.result.view, VK_IMAGE_LAYOUT_GENERAL}};
      VkWriteDescriptorSet writes[4]{};
      for (std::uint32_t i = 0; i < 4; ++i)
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, eye.set, i, 0, 1,
                     i == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[i],
                     nullptr, nullptr};
      fn<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(binding.device, 4, writes, 0, nullptr);
    }
    // Zero motion and depth (no vectors yet: the region follows the gaze, NR's history restarts on jumps), and a
    // zero correction until NR runs.
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (auto* image : {&eye.motion, &eye.depth, &eye.low_in, &eye.neural}) {
      barrier(command, image->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      fn<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(command, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                           &zero, 1, &range);
      barrier(command, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    }
    const VentureNgxNrCreate info{work_size.width, work_size.height, t};
    const auto result = ngx_bridge::get()->nr_create(command, &info, &eye.feature);
    if (!ngx_bridge::succeeded(result) || !eye.feature) {
      log::warn("venture-nr", "Venture NR: DLSS-NR feature creation failed (0x{:08X}) for {}x{}", result, size.width,
                size.height);
      eye.feature = nullptr;
      return false;
    }
    eye.size = size;
    eye.work = work_size;
    eye.host = host;
    eye.created = t;
    eye.reset = true;
    log::info("venture-nr", "Venture NR: DLSS-NR {}x{} region at {}x{} (shown at {}x{}), preset {}", size.width, size.height,
              work_size.width, work_size.height, host.width, host.height, t.preset);
    return true;
  }
  ~Impl() {
    for (auto& eye : eyes) destroy(eye);
    destroy(green);
    destroy(red);
    if (pool) fn<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(binding.device, pool, nullptr);
    if (sampler) fn<PFN_vkDestroySampler>("vkDestroySampler")(binding.device, sampler, nullptr);
    if (pipeline) fn<PFN_vkDestroyPipeline>("vkDestroyPipeline")(binding.device, pipeline, nullptr);
    if (pipeline_layout) fn<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(binding.device, pipeline_layout, nullptr);
    if (set_layout) fn<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(binding.device, set_layout, nullptr);
  }
  // A frame of `t` pixels around `rect` (clipped to `bounds`) in `target`, from a 1 x 1 `color` image.
  void outline(VkCommandBuffer command, VkImage color, VkImage target, std::uint32_t layer, VkOffset2D at,
               VkExtent2D size, VkRect2D bounds, std::int32_t t) {
    const std::int32_t x0 = std::max(at.x, bounds.offset.x), y0 = std::max(at.y, bounds.offset.y);
    const std::int32_t x1 = std::min(at.x + std::int32_t(size.width), bounds.offset.x + std::int32_t(bounds.extent.width));
    const std::int32_t y1 = std::min(at.y + std::int32_t(size.height), bounds.offset.y + std::int32_t(bounds.extent.height));
    if (x1 - x0 <= 2 * t || y1 - y0 <= 2 * t) return;
    const VkOffset3D edges[4][2] = {{{x0, y0, 0}, {x1, y0 + t, 1}},
                                    {{x0, y1 - t, 0}, {x1, y1, 1}},
                                    {{x0, y0, 0}, {x0 + t, y1, 1}},
                                    {{x1 - t, y0, 0}, {x1, y1, 1}}};
    for (const auto& edge : edges) {
      VkImageBlit line{};
      line.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      line.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
      line.srcOffsets[1] = {1, 1, 1};
      line.dstOffsets[0] = edge[0];
      line.dstOffsets[1] = edge[1];
      fn<PFN_vkCmdBlitImage>("vkCmdBlitImage")(command, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &line, VK_FILTER_NEAREST);
    }
  }
  // VRS Debug View: the red frame around VRS's full-rate area of each eye, in the host image (VDXR draws it in the
  // VDXR DLSS NR mode; here Venture NR does).
  void vrs_outline(VkCommandBuffer command, VkImage target, VkExtent2D extent, bool flip_rows, bool side_by_side,
                   float width, float height) {
    for (std::uint32_t eye = 0; eye < 2; ++eye) {
      float u = 0.5f, v = 0.5f;
      vulkan::fovea_center(eye, u, v);
      const VkExtent2D size{std::uint32_t(width * extent.width), std::uint32_t(height * extent.height)};
      std::int32_t x = std::int32_t(u * extent.width) - std::int32_t(size.width / 2);
      std::int32_t y = std::int32_t(v * extent.height) - std::int32_t(size.height / 2);
      if (flip_rows) y = std::int32_t(extent.height) - y - std::int32_t(size.height);
      const std::int32_t base = side_by_side ? std::int32_t(eye * extent.width) : 0;
      outline(command, red.image, target, side_by_side ? 0 : eye, {base + x, y}, size,
              {{base, 0}, extent}, 4);
    }
  }
  // The outlines' 1 x 1 color images, once.
  bool prepare_colors(VkCommandBuffer command) {
    if (green_ready) return true;
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (auto [image, rgba] : {std::pair{&green, VkClearColorValue{{0.16f, 0.85f, 0.25f, 1.f}}},
                               std::pair{&red, VkClearColorValue{{0.95f, 0.26f, 0.37f, 1.f}}}}) {
      if (!make(*image, VK_FORMAT_R8G8B8A8_UNORM, {1, 1},
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false))
        return false;
      barrier(command, image->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      fn<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(command, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                           &rgba, 1, &range);
      barrier(command, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    green_ready = true;
    return true;
  }
};

std::unique_ptr<NeuralRenderer> NeuralRenderer::create(const XrGraphicsBindingVulkanKHR& binding,
                                                       PFN_vkGetInstanceProcAddr get, const XrSwapchainCreateInfo& guest) {
  if (!requested() || guest.sampleCount != 1 || guest.faceCount != 1 || guest.arraySize != 2 ||
      guest.mipCount != 1)
    return nullptr;
  auto* b = ngx_bridge::get();
  if (!b || !b->nr_create || !b->nr_evaluate) {
    log::warn("venture-nr", "Venture NR: venture_ngx.dll has no DLSS-NR entry points; NR is off");
    return nullptr;
  }
  if (!initialize(binding, get)) return nullptr;
  auto impl = std::make_unique<Impl>();
  impl->binding = binding;
  impl->get = get;
  impl->guest = guest;
  impl->guest.next = nullptr;
  impl->settings = read_nr_settings(impl->settings);
  if (!impl->create_pipeline()) {
    log::warn("venture-nr", "Venture NR: its compute pipeline could not be created; NR is off");
    return nullptr;
  }
  log::info("venture-nr", "Venture NR ready for a {}x{} stereo chain (format {})", guest.width, guest.height, guest.format);
  return std::unique_ptr<NeuralRenderer>(new NeuralRenderer(std::move(impl)));
}
NeuralRenderer::NeuralRenderer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
NeuralRenderer::~NeuralRenderer() = default;

bool NeuralRenderer::record(VkCommandBuffer command, VkImage source, VkImage target, VkFormat target_format,
                            VkExtent2D extent, bool flip_rows, VkImageLayout layout) {
  constexpr bool side_by_side = false;  // each eye is its own layer of the host chain
  auto& s = *impl_;
  if (s.failed || !extent.width || !extent.height) return false;
  if (const auto now = std::chrono::steady_clock::now(); now >= s.next_read) {
    s.next_read = now + std::chrono::milliseconds(250);
    s.settings = read_nr_settings(s.settings);
  }
  float vrs_width = 0, vrs_height = 0;
  const bool vrs_outline = vulkan::vrs_outline(vrs_width, vrs_height);
  if (!s.settings.on) {
    // Toggle NR off: VRS goes on with its own area, and its outline still shows.
    vulkan::note_local_nr_area(0, 0);
    if (!vrs_outline || !s.prepare_colors(command)) return false;
    s.barrier(command, target, layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, side_by_side ? 1 : 2);
    s.vrs_outline(command, target, extent, flip_rows, side_by_side, vrs_width, vrs_height);
    s.barrier(command, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layout, 0, side_by_side ? 1 : 2);
    return true;
  }
  const VkFormat in_format = raw_format(VkFormat(s.guest.format)), out_format = raw_format(target_format);
  constexpr VkFormatFeatureFlags src = VK_FORMAT_FEATURE_BLIT_SRC_BIT, dst = VK_FORMAT_FEATURE_BLIT_DST_BIT;
  if (!s.blittable(in_format, src) || !s.blittable(out_format, dst) ||
      !s.blittable(VK_FORMAT_R16G16B16A16_SFLOAT, src | dst)) {
    log::warn("venture-nr", "Venture NR: formats {} / {} cannot be converted; NR is off", int(in_format), int(out_format));
    s.failed = true;
    return false;
  }
  const std::uint32_t gw = s.guest.width, gh = s.guest.height;
  // The region: its size from the settings (multiples of 8), centered on the fovea, inside the image.
  const VkExtent2D size{std::min(gw, std::max(64u, (std::uint32_t(s.settings.width * gw) + 7) / 8 * 8)),
                        std::min(gh, std::max(64u, (std::uint32_t(s.settings.height * gh) + 7) / 8 * 8))};
  vulkan::note_local_nr_area(float(size.width) / gw, float(size.height) / gh);
  const auto to_host_x = [&](std::int64_t v) { return std::int32_t((v * extent.width + gw / 2) / gw); };
  const auto to_host_y = [&](std::int64_t v) { return std::int32_t((v * extent.height + gh / 2) / gh); };
  const VkExtent2D host{std::uint32_t(std::max(1, to_host_x(size.width))),
                        std::uint32_t(std::max(1, to_host_y(size.height)))};
  // The outlines' colors, once.
  if (!s.prepare_colors(command)) {
    s.failed = true;
    return false;
  }
  auto* b = ngx_bridge::get();
  s.barrier(command, source, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 2);
  s.barrier(command, target, layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, side_by_side ? 1 : 2);
  bool any = false;
  std::uint32_t failed_result = 0;
  for (std::uint32_t index = 0; index < 2; ++index) {
    auto& eye = s.eyes[index];
    if (!s.prepare(command, eye, size, host, target_format)) {
      s.failed = true;
      break;
    }
    float u = 0.5f, v = 0.5f;
    vulkan::fovea_center(index, u, v);
    // Moves in steps of 8 pixels; a jump of more than a third of the region restarts NR's history.
    const std::int32_t x = std::clamp(std::int32_t(u * gw) - std::int32_t(size.width / 2), 0,
                                      std::int32_t(gw - size.width)) / 8 * 8;
    const std::int32_t y = std::clamp(std::int32_t(v * gh) - std::int32_t(size.height / 2), 0,
                                      std::int32_t(gh - size.height)) / 8 * 8;
    if (eye.offset.x >= 0 && (std::abs(x - eye.offset.x) > std::int32_t(size.width / 3) ||
                              std::abs(y - eye.offset.y) > std::int32_t(size.height / 3)))
      eye.reset = true;
    eye.offset = {x, y};
    // 1. The region, as stored (encoded), then in float for NR.
    s.barrier(command, eye.in_raw.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, index, 1};
    copy.srcOffset = {x, y, 0};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {size.width, size.height, 1};
    s.fn<PFN_vkCmdCopyImage>("vkCmdCopyImage")(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, eye.in_raw.image,
                                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    s.barrier(command, eye.in_raw.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    s.barrier(command, eye.proxy.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit{};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = blit.dstOffsets[1] = {std::int32_t(size.width), std::int32_t(size.height), 1};
    s.fn<PFN_vkCmdBlitImage>("vkCmdBlitImage")(command, eye.in_raw.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                               eye.proxy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                                               VK_FILTER_NEAREST);
    // 2. NR on its working size: every frame, or every `interval` frames with the cache (the correction it made is
    // applied to the frames between). A reset or a moved region runs it again.
    const bool run = !s.settings.cache || !eye.correction || eye.reset || ++eye.age >= s.settings.interval;
    if (run) {
      eye.age = 0;
      s.barrier(command, eye.proxy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      s.barrier(command, eye.low_in.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      VkImageBlit down{};
      down.srcSubresource = down.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      down.srcOffsets[1] = {std::int32_t(size.width), std::int32_t(size.height), 1};
      down.dstOffsets[1] = {std::int32_t(eye.work.width), std::int32_t(eye.work.height), 1};
      s.fn<PFN_vkCmdBlitImage>("vkCmdBlitImage")(command, eye.proxy.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                 eye.low_in.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &down,
                                                 eye.work.width == size.width ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
      s.barrier(command, eye.low_in.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
      s.barrier(command, eye.proxy.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
      s.barrier(command, eye.neural.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
      VentureNgxNrEvaluate e{};
      const VkExtent2D w = eye.work;
      e.color = {eye.low_in.image, eye.low_in.view, eye.low_in.format, w.width, w.height, 0};
      e.output = {eye.neural.image, eye.neural.view, eye.neural.format, w.width, w.height, 0};
      e.motion = {eye.motion.image, eye.motion.view, eye.motion.format, w.width, w.height, 0};
      e.depth = {eye.depth.image, eye.depth.view, eye.depth.format, w.width, w.height, 0};
      e.width = w.width;
      e.height = w.height;
      e.reset = eye.reset ? 1 : 0;
      e.depth_inverted = 1;
      e.tuning = s.settings.tuning;
      const auto result = b->nr_evaluate(command, eye.feature, &e);
      const bool evaluated = ngx_bridge::succeeded(result);
      if (!evaluated) failed_result = result;
      eye.reset = !evaluated;
      eye.correction = evaluated;
      s.barrier(command, eye.neural.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    } else {
      s.barrier(command, eye.proxy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    }
    // 3. The region plus NR's correction, faded at its edge (nr_apply.comp); none after a failed evaluation.
    s.barrier(command, eye.result.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    const struct {
      std::int32_t width, height;
      float transition, strength, roundness;
      std::int32_t split;
    } push{std::int32_t(size.width), std::int32_t(size.height), s.settings.transition, eye.correction ? 1.f : 0.f,
           s.settings.roundness, s.settings.split ? 1 : 0};
    s.fn<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(command, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipeline);
    s.fn<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                                                 s.pipeline_layout, 0, 1, &eye.set, 0, nullptr);
    s.fn<PFN_vkCmdPushConstants>("vkCmdPushConstants")(command, s.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                                       sizeof(push), &push);
    s.fn<PFN_vkCmdDispatch>("vkCmdDispatch")(command, (size.width + 7) / 8, (size.height + 7) / 8, 1);
    // 4. Back to the host chain's encoding and size, rows as the host shows them, with the debug outline.
    const VkImage result_image = eye.result.image;
    s.barrier(command, result_image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    s.barrier(command, eye.out_raw.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit back{};
    back.srcSubresource = back.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    back.srcOffsets[1] = {std::int32_t(size.width), std::int32_t(size.height), 1};
    back.dstOffsets[1] = {std::int32_t(host.width), std::int32_t(host.height), 1};
    if (flip_rows) std::swap(back.srcOffsets[0].y, back.srcOffsets[1].y);
    s.fn<PFN_vkCmdBlitImage>("vkCmdBlitImage")(command, result_image,
                                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, eye.out_raw.image,
                                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &back,
                                               host.width == size.width ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    if (s.settings.debug) {
      const std::int32_t w = std::int32_t(host.width), h = std::int32_t(host.height), t = 4;
      const VkOffset3D edges[4][2] = {{{0, 0, 0}, {w, t, 1}},
                                      {{0, h - t, 0}, {w, h, 1}},
                                      {{0, 0, 0}, {t, h, 1}},
                                      {{w - t, 0, 0}, {w, h, 1}}};
      for (const auto& edge : edges) {
        VkImageBlit line{};
        line.srcSubresource = line.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        line.srcOffsets[1] = {1, 1, 1};
        line.dstOffsets[0] = edge[0];
        line.dstOffsets[1] = edge[1];
        s.fn<PFN_vkCmdBlitImage>("vkCmdBlitImage")(command, s.green.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                   eye.out_raw.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &line,
                                                   VK_FILTER_NEAREST);
      }
    }
    s.barrier(command, eye.out_raw.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageCopy put{};
    put.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    put.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, side_by_side ? 0 : index, 1};
    const std::int32_t hx = to_host_x(x) + (side_by_side ? std::int32_t(index * extent.width) : 0);
    const std::int32_t hy = flip_rows ? to_host_y(std::int64_t(gh) - y - size.height) : to_host_y(y);
    put.dstOffset = {std::clamp(hx, 0, std::int32_t((side_by_side ? 2 : 1) * extent.width - host.width)),
                     std::clamp(hy, 0, std::int32_t(extent.height - host.height)), 0};
    put.extent = {host.width, host.height, 1};
    s.fn<PFN_vkCmdCopyImage>("vkCmdCopyImage")(command, eye.out_raw.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &put);
    any = true;
  }
  if (vrs_outline) s.vrs_outline(command, target, extent, flip_rows, side_by_side, vrs_width, vrs_height);
  s.barrier(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout, 0, 2);
  s.barrier(command, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layout, 0, side_by_side ? 1 : 2);
  if (failed_result) {
    if (++s.failures >= 30) {
      log::warn("venture-nr", "Venture NR: DLSS-NR evaluation keeps failing (0x{:08X}); NR is off", failed_result);
      s.failed = true;
    } else if (s.failures == 1) {
      log::warn("venture-nr", "Venture NR: DLSS-NR evaluation failed (0x{:08X})", failed_result);
    }
  } else if (any && !failed_result) {
    s.failures = 0;
    static bool reported = false;
    if (!std::exchange(reported, true)) log::info("venture-nr", "Venture NR: first frame with DLSS-NR recorded");
  }
  return any;
}
#endif
}  // namespace venture::xr::venture_nr
