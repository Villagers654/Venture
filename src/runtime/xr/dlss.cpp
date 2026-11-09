#include "xr/dlss.h"
#include "common/environment.h"
#include "common/log.h"
#include "hle/vulkan/internal.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include "xr/dlss_bridge.h"
#endif

namespace venture::xr::dlss {
#ifndef _WIN32
struct Upscaler::Impl {};
std::unique_ptr<Upscaler> Upscaler::create(const XrGraphicsBindingVulkanKHR&, PFN_vkGetInstanceProcAddr,
                                           const XrSwapchainCreateInfo&, std::span<const VkImage>) {
  return nullptr;
}
Upscaler::Upscaler(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Upscaler::~Upscaler() = default;
bool Upscaler::record(VkCommandBuffer, std::uint32_t, VkImage, VkFormat, VkExtent2D, bool, VkImageLayout,
                      std::span<const ViewSample>, bool) {
  return false;
}
void note_render_size(std::uint32_t, std::uint32_t) {}
void jitter_views(XrTime, std::uint32_t, XrView*) {}
bool unjitter(XrFovf&, float&, float&) { return false; }
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
  if (auto* b = bridge()) b->shutdown(device);
  ngx_device = VK_NULL_HANDLE;
  ngx_failed = false;
}
bool initialize(const XrGraphicsBindingVulkanKHR& binding, PFN_vkGetInstanceProcAddr get) {
  std::lock_guard lock(init_mutex);
  if (ngx_device) return ngx_device == binding.device;
  if (ngx_failed) return false;
  auto* b = bridge();
  if (!b) return ngx_failed = true, false;
  std::wstring data = widen(venture::environment::getenv("LOCALAPPDATA"));
  data = data.empty() ? runtime_directory() : data + L"\\Venture\\ngx";
  CreateDirectoryW((data.substr(0, data.find_last_of(L'\\'))).c_str(), nullptr);
  CreateDirectoryW(data.c_str(), nullptr);
  std::wstring dlss = widen(venture::environment::getenv("VENTURE_DLSS_PATH"));
  if (dlss.empty()) dlss = runtime_directory();
  VentureNgxInit info{VENTURE_NGX_ABI_VERSION,
                      binding.instance,
                      binding.physicalDevice,
                      binding.device,
                      get,
                      reinterpret_cast<PFN_vkGetDeviceProcAddr>(get(binding.instance, "vkGetDeviceProcAddr")),
                      data.c_str(),
                      dlss.c_str()};
  const auto result = b->init(&info);
  if (!succeeded(result)) {
    log::warn("dlss", "NGX Vulkan initialization failed (0x{:08X}); DLSS is off", result);
    return ngx_failed = true, false;
  }
  if (!b->available()) {
    log::warn("dlss", "NGX initialized but DLSS Super Resolution is unavailable (driver or nvngx_dlss.dll); DLSS is off");
    b->shutdown(binding.device);
    return ngx_failed = true, false;
  }
  ngx_device = binding.device;
  vulkan::add_device_release_hook(release_device);
  log::info("dlss", "NGX Vulkan initialized; DLSS Super Resolution available");
  return true;
}
}  // namespace

namespace {
namespace motion_shader {
#include "dlss_motion.inc"
}
std::mutex jitter_mutex;
std::uint32_t render_width = 0, render_height = 0;
XrTime jitter_time = 0;
std::uint32_t jitter_index = 0;
float frame_jitter_x = 0, frame_jitter_y = 0;
struct Issued {
  XrFovf jittered{}, original{};
  float x = 0, y = 0;
};
std::array<Issued, 32> issued{};
std::size_t issued_next = 0;

float halton(std::uint32_t index, std::uint32_t base) {
  float fraction = 1, result = 0;
  for (; index; index /= base) {
    fraction /= float(base);
    result += fraction * float(index % base);
  }
  return result;
}
bool jitter_enabled() {
  static const bool value = [] {
    const char* text = venture::environment::getenv("VENTURE_DLSS_JITTER");
    return requested() && !(text && *text == '0');
  }();
  return value;
}
bool same(const XrFovf& a, const XrFovf& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }
}  // namespace

void note_render_size(std::uint32_t width, std::uint32_t height) {
  std::lock_guard lock(jitter_mutex);
  render_width = width;
  render_height = height;
}

void jitter_views(XrTime display_time, std::uint32_t count, XrView* views) {
  if (!views || count != 2 || !jitter_enabled()) return;
  std::lock_guard lock(jitter_mutex);
  if (!render_width || !render_height) return;
  if (display_time != jitter_time) {
    jitter_time = display_time;
    // DLSS suggests 8 * (output / render)^2 phases: 32 at half resolution.
    const std::uint32_t phase = jitter_index++ % 32 + 1;
    frame_jitter_x = halton(phase, 2) - 0.5f;
    frame_jitter_y = halton(phase, 3) - 0.5f;
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    auto& fov = views[i].fov;
    Issued entry;
    entry.original = fov;
    entry.x = frame_jitter_x;
    entry.y = frame_jitter_y;
    // A shift of the whole window by a fraction of a pixel: x right, y down.
    const float left = std::tan(fov.angleLeft), right = std::tan(fov.angleRight);
    const float up = std::tan(fov.angleUp), down = std::tan(fov.angleDown);
    const float dx = entry.x * (right - left) / float(render_width);
    const float dy = entry.y * (up - down) / float(render_height);
    fov.angleLeft = std::atan(left + dx);
    fov.angleRight = std::atan(right + dx);
    fov.angleUp = std::atan(up - dy);
    fov.angleDown = std::atan(down - dy);
    entry.jittered = fov;
    issued[issued_next++ % issued.size()] = entry;
  }
}

bool unjitter(XrFovf& fov, float& jitter_x, float& jitter_y) {
  if (!jitter_enabled()) return false;
  std::lock_guard lock(jitter_mutex);
  // Both xrEndFrame passes look each view up; report matches now and then.
  static std::uint64_t matched = 0, unmatched = 0;
  const auto report = [&] {
    if ((matched + unmatched) % 2400 == 1)
      log::info("dlss", "jitter: {} submitted views matched a jittered field of view, {} did not", matched,
                unmatched);
  };
  for (std::size_t n = 0; n < issued.size(); ++n) {
    const auto& entry = issued[(issued_next + issued.size() - 1 - n) % issued.size()];
    if (!same(entry.jittered, fov)) continue;
    fov = entry.original;
    jitter_x = entry.x;
    jitter_y = entry.y;
    ++matched;
    report();
    return true;
  }
  ++unmatched;
  report();
  return false;
}

namespace {
struct Matrix3 {
  float m[3][3];
};
Matrix3 rotation(const XrQuaternionf& q) {
  const float x = q.x, y = q.y, z = q.z, w = q.w;
  return {{{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
           {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
           {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}}};
}
struct MotionParameters {
  float current_tan[4];
  float previous_tan[4];
  float row0[4], row1[4], row2[4];
  float translation[4];
  float size[2];
  float origin[2];
};
void tangents(const XrFovf& fov, float out[4]) {
  out[0] = std::tan(fov.angleLeft);
  out[1] = std::tan(fov.angleRight);
  out[2] = std::tan(fov.angleUp);
  out[3] = std::tan(fov.angleDown);
}
// Previous view from current view: R = Rp^T Rc, t = Rp^T (pc - pp).
MotionParameters motion_parameters(const ViewSample& current, const ViewSample& previous, float inverse_near,
                                   VkRect2D rect) {
  const auto size = rect.extent;
  MotionParameters p{};
  tangents(current.fov, p.current_tan);
  tangents(previous.fov, p.previous_tan);
  const auto rc = rotation(current.pose.orientation), rp = rotation(previous.pose.orientation);
  float* rows[3] = {p.row0, p.row1, p.row2};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      float sum = 0;
      for (int k = 0; k < 3; ++k) sum += rp.m[k][i] * rc.m[k][j];
      rows[i][j] = sum;
    }
  p.row0[3] = inverse_near;
  const float delta[3] = {current.pose.position.x - previous.pose.position.x,
                          current.pose.position.y - previous.pose.position.y,
                          current.pose.position.z - previous.pose.position.z};
  for (int i = 0; i < 3; ++i)
    p.translation[i] = rp.m[0][i] * delta[0] + rp.m[1][i] * delta[1] + rp.m[2][i] * delta[2];
  p.size[0] = float(size.width);
  p.size[1] = float(size.height);
  p.origin[0] = float(rect.offset.x);
  p.origin[1] = float(rect.offset.y);
  return p;
}
}  // namespace

struct Upscaler::Impl {
  XrGraphicsBindingVulkanKHR binding{};
  PFN_vkGetInstanceProcAddr get{};
  XrSwapchainCreateInfo guest{};
  std::uint32_t layers = 1;
  bool failed = false;
  // Source views: [slot][layer].
  std::vector<std::vector<VkImageView>> source_views;
  std::vector<VkImage> slots;
  struct Image {
    VkImage image{};
    VkDeviceMemory memory{};
    std::vector<VkImageView> views;
  };
  // One output image per eye: Cheeky follows a single-layer copy from it to
  // the submitted image (its gaze mapping), which a layered output defeats.
  std::array<Image, 2> outputs;
  Image motion, depth;
  VkExtent2D output_extent{};
  bool partial_reported = false;  // DLSS input: a view rect, logged on change
  bool inputs_cleared = false;
  bool reset = true;
  bool real_depth = false;
  // Reversed infinite Z: distance = near / depth (VENTURE_DLSS_NEAR, 10 cm).
  float inverse_near = 10.0f;
  // Camera motion vectors (shaders/dlss_motion.comp).
  VkDescriptorSetLayout motion_set_layout{};
  VkPipelineLayout motion_layout{};
  VkPipeline motion_pipeline{};
  VkSampler motion_sampler{};
  VkDescriptorPool motion_pool{};
  std::map<std::pair<VkImageView, std::uint32_t>, VkDescriptorSet> motion_sets;  // by depth view and eye
  std::array<ViewSample, 2> previous{};
  bool motion_ready = false;  // motion layers hold defined values
  float jitter_sign = 1.0f;
  int motion_reported = -1;  // zero 0, rotation 1, depth 2; logged on change
  std::vector<void*> features;  // per layer
  VkFormat output_format = VK_FORMAT_R16G16B16A16_SFLOAT;

  template <class T> T fn(const char* name) const { return reinterpret_cast<T>(get(binding.instance, name)); }

  VkImageView view(VkImage image, VkFormat format, std::uint32_t layer) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
    VkImageView out{};
    if (fn<PFN_vkCreateImageView>("vkCreateImageView")(binding.device, &vi, nullptr, &out) != VK_SUCCESS)
      return VK_NULL_HANDLE;
    return out;
  }
  bool make(Image& out, VkFormat format, VkExtent2D extent, std::uint32_t count, VkImageUsageFlags usage) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {extent.width, extent.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = count;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    if (fn<PFN_vkCreateImage>("vkCreateImage")(binding.device, &ci, nullptr, &out.image) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    fn<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(binding.device, out.image, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    fn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(binding.physicalDevice,
                                                                                         &memory);
    std::uint32_t type = 0;
    while (type < memory.memoryTypeCount && (!(requirements.memoryTypeBits & (1u << type)) ||
                                              !(memory.memoryTypes[type].propertyFlags &
                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
      ++type;
    if (type == memory.memoryTypeCount) return false;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, type};
    if (fn<PFN_vkAllocateMemory>("vkAllocateMemory")(binding.device, &ai, nullptr, &out.memory) != VK_SUCCESS)
      return false;
    if (fn<PFN_vkBindImageMemory>("vkBindImageMemory")(binding.device, out.image, out.memory, 0) != VK_SUCCESS)
      return false;
    for (std::uint32_t layer = 0; layer < count; ++layer) {
      out.views.push_back(view(out.image, format, layer));
      if (!out.views.back()) return false;
    }
    return true;
  }
  void destroy(Image& image) {
    for (auto v : image.views)
      if (v) fn<PFN_vkDestroyImageView>("vkDestroyImageView")(binding.device, v, nullptr);
    image.views.clear();
    if (image.image) fn<PFN_vkDestroyImage>("vkDestroyImage")(binding.device, image.image, nullptr);
    if (image.memory) fn<PFN_vkFreeMemory>("vkFreeMemory")(binding.device, image.memory, nullptr);
    image = {};
  }
  void release_features() {
    if (auto* b = bridge())
      for (auto* f : features)
        if (f) b->release(f);
    features.clear();
  }
  void barrier(VkCommandBuffer command, VkImage image, VkImageLayout before, VkImageLayout after,
               std::uint32_t count) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.image = image;
    b.oldLayout = before;
    b.newLayout = after;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.srcAccessMask = before == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, count};
    fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                                                         nullptr, 1, &b);
  }
  // Output resources follow the target size, which is fixed per host chain.
  bool prepare_output(VkCommandBuffer command, VkExtent2D extent) {
    if (outputs[0].image && output_extent.width == extent.width && output_extent.height == extent.height &&
        features.size() == layers)
      return true;
    // A new host chain size: the history restarts.
    release_features();
    for (auto& output : outputs) destroy(output);
    for (std::uint32_t layer = 0; layer < layers; ++layer)
      if (!make(outputs[layer], output_format, extent, 1,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        log::warn("dlss", "DLSS output image ({}x{}) could not be created", extent.width, extent.height);
        return false;
      }
    output_extent = extent;
    const auto* b = bridge();
    const double ratio = double(guest.width) / extent.width;
    // NVSDK_NGX_PerfQuality_Value: MaxPerf 0, Balanced 1, MaxQuality 2, UltraPerformance 3, DLAA 5.
    const std::int32_t quality = ratio >= 0.99 ? 5 : ratio >= 0.66 ? 2 : ratio >= 0.58 ? 1 : ratio >= 0.5 ? 0 : 3;
    // MVLowRes: vectors at render size, without jitter (jitter_views).
    // DepthInverted: Unreal renders reversed Z; VENTURE_DLSS_DEPTH_INVERTED=0 for
    // a title with forward Z.
    const char* inverted = venture::environment::getenv("VENTURE_DLSS_DEPTH_INVERTED");
    const std::int32_t flags = (1 << 1) | (inverted && *inverted == '0' ? 0 : 1 << 3);
    const VentureNgxDlssCreate info{guest.width, guest.height, extent.width, extent.height, quality, flags};
    for (std::uint32_t layer = 0; layer < layers; ++layer) {
      void* feature{};
      const auto result = b->create(command, &info, &feature);
      if (!succeeded(result) || !feature) {
        log::warn("dlss", "DLSS feature creation failed (0x{:08X}) for {}x{} -> {}x{}", result, guest.width,
                  guest.height, extent.width, extent.height);
        return false;
      }
      features.push_back(feature);
    }
    log::info("dlss", "DLSS Super Resolution {}x{} -> {}x{}, {} view(s), quality {}", guest.width, guest.height,
              extent.width, extent.height, layers, quality);
    reset = true;
    return true;
  }
  bool create_motion_pipeline() {
    const VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                                               sizeof(motion_shader::decode_shader), motion_shader::decode_shader};
    VkShaderModule shader{};
    if (fn<PFN_vkCreateShaderModule>("vkCreateShaderModule")(binding.device, &shader_info, nullptr, &shader) !=
        VK_SUCCESS)
      return false;
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    const VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 2,
                                                   bindings};
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(MotionParameters)};
    bool ok = fn<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(binding.device, &set_info, nullptr,
                                                                                &motion_set_layout) == VK_SUCCESS;
    if (ok) {
      const VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1,
                                                   &motion_set_layout, 1, &range};
      ok = fn<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(binding.device, &layout_info, nullptr,
                                                                    &motion_layout) == VK_SUCCESS;
    }
    if (ok) {
      VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
      pipeline_info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                             VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", nullptr};
      pipeline_info.layout = motion_layout;
      pipeline_info.basePipelineIndex = -1;
      ok = fn<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(binding.device, VK_NULL_HANDLE, 1,
                                                                        &pipeline_info, nullptr,
                                                                        &motion_pipeline) == VK_SUCCESS;
    }
    fn<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(binding.device, shader, nullptr);
    if (ok) {
      VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
      ok = fn<PFN_vkCreateSampler>("vkCreateSampler")(binding.device, &sampler, nullptr, &motion_sampler) ==
           VK_SUCCESS;
    }
    if (ok) {
      // Three swapchain images x two eyes; a few spare for recreated framebuffers.
      const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 32}};
      const VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 32, 2, sizes};
      ok = fn<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(binding.device, &pool, nullptr, &motion_pool) ==
           VK_SUCCESS;
    }
    return ok;
  }
  VkDescriptorSet motion_set(VkImageView depth_view, std::uint32_t layer) {
    if (auto it = motion_sets.find({depth_view, layer}); it != motion_sets.end()) return it->second;
    const VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, motion_pool,
                                                 1, &motion_set_layout};
    VkDescriptorSet set{};
    if (fn<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(binding.device, &allocation, &set) != VK_SUCCESS)
      return VK_NULL_HANDLE;
    const VkDescriptorImageInfo images[] = {
        {motion_sampler, depth_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, motion.views[layer], VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[2]{};
    for (std::uint32_t i = 0; i < 2; ++i)
      writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, i, 0, 1,
                   i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[i],
                   nullptr, nullptr};
    fn<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(binding.device, 2, writes, 0, nullptr);
    motion_sets.emplace(std::pair{depth_view, layer}, set);
    return set;
  }
  ~Impl() {
    release_features();
    if (motion_pool) fn<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(binding.device, motion_pool, nullptr);
    if (motion_sampler) fn<PFN_vkDestroySampler>("vkDestroySampler")(binding.device, motion_sampler, nullptr);
    if (motion_pipeline) fn<PFN_vkDestroyPipeline>("vkDestroyPipeline")(binding.device, motion_pipeline, nullptr);
    if (motion_layout)
      fn<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(binding.device, motion_layout, nullptr);
    if (motion_set_layout)
      fn<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(binding.device, motion_set_layout,
                                                                          nullptr);
    for (auto& views : source_views)
      for (auto v : views)
        if (v) fn<PFN_vkDestroyImageView>("vkDestroyImageView")(binding.device, v, nullptr);
    for (auto& output : outputs) destroy(output);
    destroy(motion);
    destroy(depth);
  }
};

std::unique_ptr<Upscaler> Upscaler::create(const XrGraphicsBindingVulkanKHR& binding, PFN_vkGetInstanceProcAddr get,
                                           const XrSwapchainCreateInfo& guest, std::span<const VkImage> slots) {
  if (!requested() || guest.sampleCount != 1 || guest.faceCount != 1 || guest.arraySize > 2) return nullptr;
  if (!initialize(binding, get)) return nullptr;
  auto impl = std::make_unique<Impl>();
  impl->binding = binding;
  impl->get = get;
  impl->guest = guest;
  impl->guest.next = nullptr;
  impl->layers = guest.arraySize;
  impl->slots.assign(slots.begin(), slots.end());
  for (auto image : slots) {
    auto& views = impl->source_views.emplace_back();
    for (std::uint32_t layer = 0; layer < impl->layers; ++layer) {
      views.push_back(impl->view(image, VkFormat(guest.format), layer));
      if (!views.back()) {
        log::warn("dlss", "DLSS source view (format {}) could not be created", guest.format);
        return nullptr;
      }
    }
  }
  const VkExtent2D render{guest.width, guest.height};
  if (!impl->make(impl->motion, VK_FORMAT_R16G16_SFLOAT, render, impl->layers,
                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
      !impl->make(impl->depth, VK_FORMAT_R32_SFLOAT, render, 1,
                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
    log::warn("dlss", "DLSS motion/depth inputs could not be created");
    return nullptr;
  }
  if (!impl->create_motion_pipeline()) {
    log::warn("dlss", "DLSS motion vector pipeline could not be created");
    return nullptr;
  }
  if (const char* plane = venture::environment::getenv("VENTURE_DLSS_NEAR"); plane && std::atof(plane) > 0)
    impl->inverse_near = float(1.0 / std::atof(plane));
  if (const char* inverted = venture::environment::getenv("VENTURE_DLSS_DEPTH_INVERTED"); inverted && *inverted == '0')
    impl->inverse_near = 0;  // forward Z is not modeled: rotation only
  if (const char* sign = venture::environment::getenv("VENTURE_DLSS_JITTER_SIGN"); sign && std::atof(sign) < 0)
    impl->jitter_sign = -1.0f;
  return std::unique_ptr<Upscaler>(new Upscaler(std::move(impl)));
}
Upscaler::Upscaler(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Upscaler::~Upscaler() = default;

bool Upscaler::record(VkCommandBuffer command, std::uint32_t index, VkImage target, VkFormat target_format,
                      VkExtent2D extent, bool flip_rows, VkImageLayout layout, std::span<const ViewSample> views,
                      bool side_by_side) {
  auto& s = *impl_;
  if (s.failed || index >= s.slots.size() || !extent.width || !extent.height) return false;
  VkFormatProperties properties{};
  s.fn<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(
      s.binding.physicalDevice, target_format, &properties);
  if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) return false;
  // The area each eye rendered, in image rows: a game with dynamic resolution
  // (Iron Man) draws into a rect smaller than its image. DLSS upscales that
  // rect to its host size, which is what the layer shows and what Cheeky's
  // gaze mapping compares with. Both eyes must share a size (one output size).
  const VkRect2D whole{{0, 0}, {s.guest.width, s.guest.height}};
  std::array<VkRect2D, 2> area{whole, whole};
  bool partial = false;
  for (std::uint32_t layer = 0; layer < s.layers && layer < 2; ++layer) {
    if (layer >= views.size() || !views[layer].valid) continue;
    const auto& r = views[layer].rect;
    if (r.extent.width <= 0 || r.extent.height <= 0 || r.offset.x < 0 || r.offset.y < 0 ||
        std::uint32_t(r.offset.x + r.extent.width) > s.guest.width ||
        std::uint32_t(r.offset.y + r.extent.height) > s.guest.height)
      continue;
    area[layer] = {{r.offset.x, r.offset.y}, {std::uint32_t(r.extent.width), std::uint32_t(r.extent.height)}};
    partial |= area[layer].extent.width != s.guest.width || area[layer].extent.height != s.guest.height;
  }
  if (partial && s.layers == 2 && (area[0].extent.width != area[1].extent.width ||
                                   area[0].extent.height != area[1].extent.height)) {
    partial = false;
    area = {whole, whole};
  }
  const auto to_host_x = [&](std::int64_t v) { return std::int32_t((v * extent.width + s.guest.width / 2) / s.guest.width); };
  const auto to_host_y = [&](std::int64_t v) { return std::int32_t((v * extent.height + s.guest.height / 2) / s.guest.height); };
  const VkExtent2D output_extent =
      partial ? VkExtent2D{std::uint32_t(to_host_x(area[0].extent.width)), std::uint32_t(to_host_y(area[0].extent.height))}
              : extent;
  if (partial != s.partial_reported) {
    s.partial_reported = partial;
    log::info("dlss", "DLSS input: {}", partial ? std::format("view rect {}x{} at ({},{}) / ({},{}) of {}x{}",
                                                               area[0].extent.width, area[0].extent.height,
                                                               area[0].offset.x, area[0].offset.y, area[1].offset.x,
                                                               area[1].offset.y, s.guest.width, s.guest.height)
                                                 : std::string("whole image"));
  }
  if (!s.prepare_output(command, output_extent)) {
    s.failed = true;
    return false;
  }
  if (!s.inputs_cleared) {
    // Constant depth, until the game's depth is found: every pixel distant.
    s.barrier(command, s.depth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    s.fn<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(command, s.depth.image,
                                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    s.barrier(command, s.depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1);
    s.inputs_cleared = true;
  }
  const auto source = s.slots[index];
  s.barrier(command, source, layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, s.layers);
  for (std::uint32_t layer = 0; layer < s.layers; ++layer)
    s.barrier(command, s.outputs[layer].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 1);
  auto* b = bridge();
  // The pass that resolved into this guest image left its per-eye depth
  // (hle/vulkan/render_area.cpp: Batman's multiview MSAA depth).
  vulkan::StereoDepthViews depth{};
  const bool real_depth = s.layers == 2 && vulkan::stereo_depth_for_color(source, depth) &&
                          depth.extent.width == s.guest.width && depth.extent.height == s.guest.height;
  if (real_depth != s.real_depth) {
    s.real_depth = real_depth;
    log::info("dlss", "DLSS depth input: {}", real_depth ? "resolved stereo depth" : "constant");
  }
  // Motion: when this and the previous frame have poses, the game's depth is
  // reprojected; without it every pixel counts as distant and the vectors
  // follow head rotation, which is what trails without them. Zero otherwise.
  bool reprojected = views.size() >= s.layers;
  for (std::uint32_t layer = 0; reprojected && layer < s.layers; ++layer)
    reprojected = views[layer].valid && s.previous[layer].valid;
  std::array<VkDescriptorSet, 2> sets{};
  for (std::uint32_t layer = 0; reprojected && layer < s.layers; ++layer)
    reprojected = (sets[layer] = s.motion_set(real_depth ? depth.views[layer] : s.depth.views[0], layer)) !=
                  VK_NULL_HANDLE;
  const auto before = s.motion_ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
  if (reprojected) {
    s.barrier(command, s.motion.image, before, VK_IMAGE_LAYOUT_GENERAL, s.layers);
    s.fn<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(command, VK_PIPELINE_BIND_POINT_COMPUTE, s.motion_pipeline);
    for (std::uint32_t layer = 0; layer < s.layers; ++layer) {
      const auto& rendered = area[layer < 2 ? layer : 0];
      const auto parameters =
          motion_parameters(views[layer], s.previous[layer], real_depth ? s.inverse_near : 0.0f, rendered);
      s.fn<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                                                   s.motion_layout, 0, 1, &sets[layer], 0, nullptr);
      s.fn<PFN_vkCmdPushConstants>("vkCmdPushConstants")(command, s.motion_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                                         sizeof(parameters), &parameters);
      s.fn<PFN_vkCmdDispatch>("vkCmdDispatch")(command, (rendered.extent.width + 7) / 8,
                                               (rendered.extent.height + 7) / 8, 1);
    }
    s.barrier(command, s.motion.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, s.layers);
  } else {
    s.barrier(command, s.motion.image, before, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, s.layers);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, s.layers};
    s.fn<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(command, s.motion.image,
                                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    s.barrier(command, s.motion.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              s.layers);
  }
  s.motion_ready = true;
  if (const int kind = !reprojected ? 0 : real_depth ? 2 : 1; kind != s.motion_reported) {
    s.motion_reported = kind;
    log::info("dlss", "DLSS motion vectors: {}",
              !reprojected ? "zero" : real_depth ? "depth reprojection" : "head rotation");
  }
  for (std::uint32_t layer = 0; layer < s.layers; ++layer) {
    VentureNgxDlssEvaluate e{};
    e.color = {source, s.source_views[index][layer], VkFormat(s.guest.format), s.guest.width, s.guest.height, layer};
    e.output = {s.outputs[layer].image, s.outputs[layer].views[0], s.output_format, output_extent.width,
                output_extent.height, 0};
    e.depth = real_depth
                  ? VentureNgxImage{depth.images[layer], depth.views[layer], VK_FORMAT_R32_SFLOAT, s.guest.width,
                                    s.guest.height, 0}
                  : VentureNgxImage{s.depth.image, s.depth.views[0], VK_FORMAT_R32_SFLOAT, s.guest.width,
                                    s.guest.height, 0};
    e.motion = {s.motion.image, s.motion.views[layer], VK_FORMAT_R16G16_SFLOAT, s.guest.width, s.guest.height,
                layer};
    const auto& rendered = area[layer < 2 ? layer : 0];
    if (layer < views.size() && views[layer].valid) {
      // jitter_views offsets in recommended-size pixels; the view spans its rect.
      const float sx = partial && render_width ? float(rendered.extent.width) / float(render_width) : 1.0f;
      const float sy = partial && render_height ? float(rendered.extent.height) / float(render_height) : 1.0f;
      e.jitter_x = s.jitter_sign * views[layer].jitter_x * sx;
      e.jitter_y = s.jitter_sign * views[layer].jitter_y * sy;
    }
    e.motion_scale_x = e.motion_scale_y = 1.0f;
    e.render_width = rendered.extent.width;
    e.render_height = rendered.extent.height;
    e.subrect_x = std::uint32_t(rendered.offset.x);
    e.subrect_y = std::uint32_t(rendered.offset.y);
    e.reset = s.reset ? 1 : 0;
    const auto result = b->evaluate(command, s.features[layer], &e);
    if (!succeeded(result)) {
      // The layouts below still balance; the blit then shows this frame's
      // undefined output once, and later frames take the plain blit.
      log::warn("dlss", "DLSS evaluation failed (0x{:08X}); falling back to the plain upscale", result);
      s.failed = true;
    }
  }
  s.reset = false;
  for (std::uint32_t layer = 0; layer < s.layers && layer < s.previous.size(); ++layer)
    s.previous[layer] = layer < views.size() ? views[layer] : ViewSample{};
  const std::uint32_t target_layers = side_by_side ? 1 : s.layers;
  for (std::uint32_t layer = 0; layer < s.layers; ++layer)
    s.barrier(command, s.outputs[layer].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1);
  s.barrier(command, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, target_layers);
  for (std::uint32_t layer = 0; layer < s.layers; ++layer) {
    // One single-layer region per eye, 1:1: the shape Cheeky's copy graph follows.
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, side_by_side ? 0 : layer, 1};
    // The upscaled view rect lands where the presented rect maps in the host image.
    const auto& rendered = area[layer < 2 ? layer : 0];
    const int x = (side_by_side ? int(layer * extent.width) : 0) + (partial ? to_host_x(rendered.offset.x) : 0);
    // A flipped copy puts the rect's rows top first in the host image.
    const int y = !partial ? 0
                  : flip_rows ? to_host_y(std::int64_t(s.guest.height) - rendered.offset.y - rendered.extent.height)
                              : to_host_y(rendered.offset.y);
    blit.srcOffsets[1] = {int(output_extent.width), int(output_extent.height), 1};
    blit.dstOffsets[0] = {x, y, 0};
    blit.dstOffsets[1] = {x + int(output_extent.width), y + int(output_extent.height), 1};
    if (flip_rows) std::swap(blit.srcOffsets[0].y, blit.srcOffsets[1].y);
    s.fn<PFN_vkCmdBlitImage>("vkCmdBlitImage")(command, s.outputs[layer].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                               target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                                               VK_FILTER_NEAREST);
  }
  s.barrier(command, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, layout, s.layers);
  s.barrier(command, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layout, target_layers);
  static bool reported = false;
  if (!std::exchange(reported, true)) log::info("dlss", "first DLSS-upscaled frame recorded");
  return true;
}
#endif
}  // namespace venture::xr::dlss
