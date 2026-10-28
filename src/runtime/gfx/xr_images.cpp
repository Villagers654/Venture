#include "common/environment.h"
#include "gfx/xr_images.h"

#include <mutex>
#include <functional>
#include <map>
#include "core/wait.h"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <stdexcept>

#include "common/log.h"
#include "core/signals.h"
#include "core/thread.h"
#include "gfx/angle.h"
#include "gfx/gpu_device.h"

namespace venture::gfx {
namespace {
using ExportImage = EGLBoolean (*)(EGLDisplay, EGLImageKHR, void*, void*);
using TransferTextures = void (*)(GLuint, const GLuint*, GLenum*);
using AcquireTextures = void (*)(GLuint, const GLuint*, const GLenum*);
template <class T>
T device_proc(const char* name) {
  auto& gpu = gpu_device();
  auto get = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
      gpu.get_instance_proc_addr(gpu.instance, "vkGetDeviceProcAddr"));
  auto result = reinterpret_cast<T>(get(gpu.device, name));
  if (!result)
    throw std::runtime_error(std::string("Missing Vulkan procedure: ") + name);
  return result;
}
void check(VkResult r, const char* name) {
  if (r != VK_SUCCESS)
    throw std::runtime_error(std::string(name) + ": " + std::to_string(r));
}
bool failure(std::string* error, std::string text) {
  log::error("gfx-xr", "{}", text);
  if (error) *error = std::move(text);
  return false;
}
GLenum gl_format(VkFormat f) {
  switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM:
      return GL_RGBA8;
    case VK_FORMAT_R8G8B8A8_SRGB:
      return GL_SRGB8_ALPHA8;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
      return GL_RGBA16F;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      return GL_RGB10_A2;
    default:
      return 0;
  }
}
VkImageLayout vk_layout(GLenum layout) {
  if (angle_uses_general_layout()) return VK_IMAGE_LAYOUT_GENERAL;
  // EXT_external_objects layout mapping (ANGLE_vulkan_image uses these enums).
  switch (layout) {
    case GL_LAYOUT_GENERAL_EXT:
      return VK_IMAGE_LAYOUT_GENERAL;
    case GL_LAYOUT_COLOR_ATTACHMENT_EXT:
      return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case GL_LAYOUT_SHADER_READ_ONLY_EXT:
      return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case GL_LAYOUT_TRANSFER_SRC_EXT:
      return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case GL_LAYOUT_TRANSFER_DST_EXT:
      return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case GL_NONE:
      return VK_IMAGE_LAYOUT_UNDEFINED;
    default:
      throw std::runtime_error("Unsupported ANGLE released image layout");
  }
}
struct DirectSource {
  GLuint texture = 0;
  GLuint ownership_texture = 0;
  EGLImageKHR sibling{};
  VkImage image{};
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  GLenum layout = GL_LAYOUT_TRANSFER_SRC_EXT;
  bool released = false;
  ~DirectSource() {
    if (ownership_texture) angle_function<PFNGLDELETETEXTURESPROC>("glDeleteTextures")(1, &ownership_texture);
    if (sibling) angle_function<PFNEGLDESTROYIMAGEKHRPROC>("eglDestroyImageKHR")(angle_display(), sibling);
  }
};
bool gpu_ownership_enabled() {
  const auto* value = venture::environment::getenv("VENTURE_XR_GPU_OWNERSHIP");
  return value && std::string_view(value) == "1";
}
void gpu_acquire_marker(DirectSource& source, VkFormat format) {
  // Exported EGL images lock their share group to ANGLE's Medium queue, which
  // is also the queue supplied by our Vulkan device callback. After reacquire,
  // an ANGLE resource use must follow the external submit: a query/fence alone
  // does not update the image's lifetime serial before an immediate CPU upload.
  // Read one texel into private storage, then server-wait on that resource use.
  if (!source.ownership_texture) {
    GLint binding{};
    angle_function<PFNGLGETINTEGERVPROC>("glGetIntegerv")(GL_TEXTURE_BINDING_2D, &binding);
    angle_function<PFNGLGENTEXTURESPROC>("glGenTextures")(1, &source.ownership_texture);
    angle_function<PFNGLBINDTEXTUREPROC>("glBindTexture")(GL_TEXTURE_2D, source.ownership_texture);
    angle_function<PFNGLTEXSTORAGE2DPROC>("glTexStorage2D")(GL_TEXTURE_2D, 1, gl_format(format), 1, 1);
    angle_function<PFNGLBINDTEXTUREPROC>("glBindTexture")(GL_TEXTURE_2D, binding);
  }
  auto copy = reinterpret_cast<PFNGLCOPYIMAGESUBDATAPROC>(angle_proc("glCopyImageSubDataEXT"));
  if (!copy) copy = angle_function<PFNGLCOPYIMAGESUBDATAPROC>("glCopyImageSubData");
  copy(source.texture, GL_TEXTURE_2D, 0, 0, 0, 0,
       source.ownership_texture, GL_TEXTURE_2D, 0, 0, 0, 0, 1, 1, 1);
  auto fence = angle_function<PFNGLFENCESYNCPROC>("glFenceSync")(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  if (!fence) throw std::runtime_error("ANGLE GPU ownership fence failed");
  angle_function<PFNGLWAITSYNCPROC>("glWaitSync")(fence, 0, GL_TIMEOUT_IGNORED);
  angle_function<PFNGLDELETESYNCPROC>("glDeleteSync")(fence);
}
std::shared_ptr<DirectSource> direct_source(const XrImageDescription& desc) {
  // EGL_KHR_image forbids creating another image from an existing sibling.
  // One guest texture can feed all three acquired host images and both row
  // orders. Share its export, not its destination command/fence state.
  static std::map<std::pair<std::uintptr_t, GLuint>, std::weak_ptr<DirectSource>> cache;
  const auto context = angle_function<PFNEGLGETCURRENTCONTEXTPROC>("eglGetCurrentContext")();
  const auto key = std::make_pair(desc.source_group ? desc.source_group : reinterpret_cast<std::uintptr_t>(context), desc.source_texture);
  auto source = cache[key].lock();
  if (!source) {
    std::erase_if(cache, [](const auto& entry) { return entry.second.expired(); });
    source = std::make_shared<DirectSource>();
    source->texture = desc.source_texture;
    const EGLint attributes[]{EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    source->sibling = angle_function<PFNEGLCREATEIMAGEKHRPROC>("eglCreateImageKHR")(
        angle_display(), context, EGL_GL_TEXTURE_2D_KHR,
        reinterpret_cast<EGLClientBuffer>(std::uintptr_t(desc.source_texture)), attributes);
    if (!source->sibling || !angle_function<ExportImage>("eglExportVkImageANGLE")(
            angle_display(), source->sibling, &source->image, &source->info))
      throw std::runtime_error("ANGLE direct source export failed");
    cache[key] = source;
  }
  const auto& info = source->info;
  if (!source->image || info.format != desc.format || info.arrayLayers != 1 || info.mipLevels != 1 ||
      info.samples != VK_SAMPLE_COUNT_1_BIT || info.extent.width != desc.width || info.extent.height != desc.height ||
      !(info.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
    throw std::runtime_error("ANGLE direct source export metadata incompatible");
  return source;
}
using SharedFence = std::shared_ptr<VkFence>;
SharedFence make_fence() {
  VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence{};
  check(device_proc<PFN_vkCreateFence>("vkCreateFence")(
      gpu_device().device, &info, nullptr, &fence), "vkCreateFence");
  return SharedFence(new VkFence(fence), [](VkFence* value) {
    device_proc<PFN_vkDestroyFence>("vkDestroyFence")(gpu_device().device, *value, nullptr);
    delete value;
  });
}
struct BridgeBatch {
  bool active = false, ownership_wait = false;
  SharedFence* preferred = nullptr;
  struct Image { SharedFence* fence; bool* submitted; bool* failed; };
  std::vector<Image> images;
  std::vector<VkCommandBuffer> commands;
  std::vector<std::function<void(bool)>> finish;
};
thread_local BridgeBatch batch;
void wait_fence(VkFence fence, bool interruptible = true) {
  auto wait = device_proc<PFN_vkWaitForFences>("vkWaitForFences");
  for (;;) {
    auto r = wait(gpu_device().device, 1, &fence, VK_TRUE, 1000000);
    if (r == VK_SUCCESS) return;
    if (r != VK_TIMEOUT) check(r, "vkWaitForFences");
    if (interruptible)
      if (auto* thread = GuestThread::current()) signals::deliver_pending(*thread);
  }
}
struct QueueGuard {
  std::lock_guard<std::recursive_mutex> lock{gpu_device().queue_lock};
  QueueGuard() {
    if (!lock_angle_queue())
      throw std::runtime_error("ANGLE queue lock unavailable");
  }
  ~QueueGuard() { unlock_angle_queue(); }
};
void barrier(VkCommandBuffer command, VkImage image, VkImageLayout from,
             VkImageLayout to, std::uint32_t layers, std::uint32_t levels = 1) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask =
      from == VK_IMAGE_LAYOUT_UNDEFINED
          ? 0
          : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
  device_proc<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
      command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}
}  // namespace
struct XrImageBridge::Impl {
  std::mutex lock;
  XrImageDescription description{};
  bool gpu_ownership = false;
  std::vector<VkImage> targets;
  StorageCensus storage;
  std::vector<GLuint> textures;
  struct Image {
    std::vector<GLuint> bridge_textures;
    std::vector<EGLImageKHR> siblings;
    std::vector<VkImage> sources;
    std::shared_ptr<DirectSource> direct;
    VkCommandBuffer command{};
    SharedFence fence, submitted_fence;
    bool submitted = false, acquired = false, initialized = false,
         released_to_vulkan = false, failed = false, timed = false;
  };
  std::vector<Image> images;
  VkCommandPool pool{};
  // VENTURE_VULKAN_GPU_TIMING: two timestamps per image around its release copy.
  VkQueryPool queries{};
  double timestamp_period = 1;
  ~Impl() {
    if (!pool && images.empty()) return;
    try {
      for (auto& image : images) {
        if (image.submitted) wait_fence(*image.submitted_fence, false);
        if (image.released_to_vulkan && !image.failed) {
          std::vector<GLenum> layouts(image.bridge_textures.size(),
                                      GL_LAYOUT_TRANSFER_SRC_EXT);
          angle_function<AcquireTextures>("glAcquireTexturesANGLE")(
              static_cast<GLuint>(layouts.size()), image.bridge_textures.data(),
              layouts.data());
        }
        image.fence.reset();
        for (auto sibling : image.siblings)
          angle_function<PFNEGLDESTROYIMAGEKHRPROC>("eglDestroyImageKHR")(
              angle_display(), sibling);
        if ((description.layers > 1 || description.mip_count > 1 || description.external_subresources) && !image.bridge_textures.empty())
          angle_function<PFNGLDELETETEXTURESPROC>("glDeleteTextures")(
              static_cast<GLsizei>(image.bridge_textures.size()),
              image.bridge_textures.data());
      }
      if (!description.source_texture && !textures.empty())
        angle_function<PFNGLDELETETEXTURESPROC>("glDeleteTextures")(
            static_cast<GLsizei>(textures.size()), textures.data());
      if (pool)
        device_proc<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(
            gpu_device().device, pool, nullptr);
      if (queries)
        device_proc<PFN_vkDestroyQueryPool>("vkDestroyQueryPool")(
            gpu_device().device, queries, nullptr);
    } catch (const std::exception& e) {
      log::error("gfx-xr", "Bridge cleanup failed: {}", e.what());
    }
  }
};
XrImageBridge::XrImageBridge() : impl_(std::make_unique<Impl>()) {}
XrImageBridge::~XrImageBridge() = default;
std::span<const std::uint32_t> XrImageBridge::textures() const {
  return impl_->textures;
}
XrImageBridge::StorageCensus XrImageBridge::storage_census() const {
  return impl_->storage;
}
bool XrImageBridge::gpu_ownership() const { return impl_->gpu_ownership; }
std::unique_ptr<XrImageBridge> XrImageBridge::create(
    const XrImageDescription& desc, std::string* error) {
  if (!desc.width || !desc.height || !desc.layers || !desc.mip_count ||
      desc.mip_count > std::bit_width(std::max(desc.width, desc.height)) || desc.images.empty() ||
      !gl_format(desc.format) || desc.samples != VK_SAMPLE_COUNT_1_BIT ||
      !(desc.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
      (desc.external_subresources && desc.source_texture)) {
    failure(error,
            "Unsupported XR image shape, format, samples, or missing "
            "transfer-destination usage");
    return nullptr;
  }
  if (!angle_shared_device()) {
    failure(error, "ANGLE is not using the shared graphics device");
    return nullptr;
  }
  auto* ext = reinterpret_cast<const char*>(
      angle_function<PFNGLGETSTRINGPROC>("glGetString")(GL_EXTENSIONS));
  if (!ext || !has_extension(ext, "GL_ANGLE_vulkan_image") ||
      !angle_proc("eglExportVkImageANGLE")) {
    failure(error,
            "ANGLE Vulkan image export/acquire/release is unavailable in "
            "current context");
    return nullptr;
  }
  VkFormatProperties properties{};
  auto& gpu = gpu_device();
  reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
      gpu.get_instance_proc_addr(gpu.instance, "vkGetPhysicalDeviceFormatProperties"))(
          gpu.physical_device, desc.format, &properties);
  constexpr auto blit_features = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
  if ((properties.optimalTilingFeatures & blit_features) != blit_features) {
    failure(error, "XR color format cannot blit between GLES and Vulkan orientations");
    return nullptr;
  }
  auto result = std::unique_ptr<XrImageBridge>(new XrImageBridge);
  auto& state = *result->impl_;
  state.description = desc;
  if (desc.source_texture && gpu_ownership_enabled()) {
    EGLint version = 0;
    const auto context = angle_function<PFNEGLGETCURRENTCONTEXTPROC>("eglGetCurrentContext")();
    const bool es3 = angle_function<PFNEGLQUERYCONTEXTPROC>("eglQueryContext")(
        angle_display(), context, EGL_CONTEXT_CLIENT_VERSION, &version) && version >= 3;
    GLint minor = 0;
    if (es3) angle_function<PFNGLGETINTEGERVPROC>("glGetIntegerv")(GL_MINOR_VERSION, &minor);
    state.gpu_ownership = es3 &&
        (minor >= 2 || has_extension(ext, "GL_EXT_copy_image")) &&
        (angle_proc("glCopyImageSubDataEXT") || angle_proc("glCopyImageSubData")) &&
        angle_proc("glFenceSync") && angle_proc("glWaitSync") && angle_proc("glDeleteSync");
  }
  state.description.images = {};
  state.targets.assign(desc.images.begin(), desc.images.end());
  state.images.resize(desc.images.size());
  if (!desc.external_subresources) state.textures.resize(desc.images.size());
  try {
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = gpu_device().queue_family;
    check(device_proc<PFN_vkCreateCommandPool>("vkCreateCommandPool")(
              gpu_device().device, &pool, nullptr, &state.pool),
          "vkCreateCommandPool");
    if (const char* timing = venture::environment::getenv("VENTURE_VULKAN_GPU_TIMING"); timing && *timing) {
      std::uint32_t count = 0;
      auto families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
          gpu.get_instance_proc_addr(gpu.instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
      families(gpu.physical_device, &count, nullptr);
      std::vector<VkQueueFamilyProperties> values(count);
      families(gpu.physical_device, &count, values.data());
      VkPhysicalDeviceProperties limits{};
      reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
          gpu.get_instance_proc_addr(gpu.instance, "vkGetPhysicalDeviceProperties"))(
              gpu.physical_device, &limits);
      if (gpu.queue_family < count && values[gpu.queue_family].timestampValidBits == 64) {
        VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        query.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query.queryCount = static_cast<std::uint32_t>(2 * desc.images.size());
        check(device_proc<PFN_vkCreateQueryPool>("vkCreateQueryPool")(
                  gpu_device().device, &query, nullptr, &state.queries),
              "vkCreateQueryPool");
        state.timestamp_period = limits.limits.timestampPeriod;
      }
    }
    const GLenum target =
        desc.layers == 1 ? GL_TEXTURE_2D : GL_TEXTURE_2D_ARRAY;
    GLint old_texture{}, old_2d{};
    auto get = angle_function<PFNGLGETINTEGERVPROC>("glGetIntegerv");
    get(target == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D
                                : GL_TEXTURE_BINDING_2D_ARRAY,
        &old_texture);
    get(GL_TEXTURE_BINDING_2D, &old_2d);
    struct Restore {
      GLenum target;
      GLint old_texture, old_2d;
      ~Restore() {
        auto bind = angle_function<PFNGLBINDTEXTUREPROC>("glBindTexture");
        bind(target, old_texture);
        bind(GL_TEXTURE_2D, old_2d);
      }
    } restore{target, old_texture, old_2d};
    auto gen = angle_function<PFNGLGENTEXTURESPROC>("glGenTextures");
    auto bind = angle_function<PFNGLBINDTEXTUREPROC>("glBindTexture");
    const std::uint64_t texel_bytes = desc.format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
    std::uint64_t aggregate_bytes = 0;
    for (std::uint32_t level = 0; level < desc.mip_count; ++level)
      aggregate_bytes += std::uint64_t(std::max(1u, desc.width >> level)) *
                         std::max(1u, desc.height >> level) * desc.layers * texel_bytes;
    if (desc.source_texture) {
      if (desc.layers != 1 || desc.mip_count != 1 || desc.images.size() != 1)
        throw std::runtime_error("Direct XR source must be one single-level 2D texture");
      state.textures[0] = desc.source_texture;
    } else if (!state.textures.empty())
      gen(static_cast<GLsizei>(state.textures.size()), state.textures.data());
    for (std::size_t index = 0; index < state.images.size(); ++index) {
      auto& image = state.images[index];
      if (!state.textures.empty()) bind(target, state.textures[index]);
      if (desc.source_texture) {
        GLint immutable = 0;
        angle_function<PFNGLGETTEXPARAMETERIVPROC>("glGetTexParameteriv")(
            GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_FORMAT, &immutable);
        if (!immutable) throw std::runtime_error("Direct XR source requires immutable texture storage");
      }
      if (!desc.source_texture && !desc.external_subresources && desc.layers == 1)
        angle_function<PFNGLTEXSTORAGE2DPROC>("glTexStorage2D")(
            target, desc.mip_count, gl_format(desc.format), desc.width, desc.height);
      else if (!desc.source_texture && !desc.external_subresources)
        angle_function<PFNGLTEXSTORAGE3DPROC>("glTexStorage3D")(
            target, desc.mip_count, gl_format(desc.format), desc.width, desc.height,
            desc.layers);
      if (!desc.source_texture && !desc.external_subresources) {
        ++state.storage.aggregate_textures;
        state.storage.aggregate_bytes += aggregate_bytes;
      }
      image.bridge_textures.resize(desc.layers * desc.mip_count);
      if (image.bridge_textures.size() == 1 && !state.textures.empty())
        image.bridge_textures[0] = state.textures[index];
      else
        gen(static_cast<GLsizei>(image.bridge_textures.size()), image.bridge_textures.data());
      for (std::size_t subresource = 0; subresource < image.bridge_textures.size(); ++subresource) {
        if (desc.source_texture) {
          image.direct = direct_source(desc);
          image.sources.push_back(image.direct->image);
          continue;
        }
        const auto texture = image.bridge_textures[subresource];
        const auto level = subresource % desc.mip_count;
        const auto width = std::max(1u, desc.width >> level);
        const auto height = std::max(1u, desc.height >> level);
        // EGL_KHR_gl_texture_2D_image selects a mip level, but exporting that
        // sibling does not describe its offset within the backing VkImage.
        // As for array layers, export a separate single-level 2D texture.
        if (image.bridge_textures.size() > 1 || desc.external_subresources) {
          bind(GL_TEXTURE_2D, texture);
          angle_function<PFNGLTEXSTORAGE2DPROC>("glTexStorage2D")(
              GL_TEXTURE_2D, 1, gl_format(desc.format), width, height);
          ++state.storage.subresource_textures;
          state.storage.subresource_bytes += std::uint64_t(width) * height * texel_bytes;
        }
        const EGLint attributes[] = {EGL_GL_TEXTURE_LEVEL_KHR, 0,
                                     EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
                                     EGL_NONE};
        auto context = angle_function<PFNEGLGETCURRENTCONTEXTPROC>(
            "eglGetCurrentContext")();
        auto sibling =
            angle_function<PFNEGLCREATEIMAGEKHRPROC>("eglCreateImageKHR")(
                angle_display(), context, EGL_GL_TEXTURE_2D_KHR,
                reinterpret_cast<EGLClientBuffer>(
                    static_cast<std::uintptr_t>(texture)),
                attributes);
        if (!sibling)
          throw std::runtime_error(
              "ANGLE failed to create 2D EGLImage sibling");
        image.siblings.push_back(sibling);
        VkImage source{};
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        if (!angle_function<ExportImage>("eglExportVkImageANGLE")(
                angle_display(), sibling, &source, &info) ||
            !source || info.format != desc.format || info.arrayLayers != 1 ||
            info.mipLevels != 1 || info.samples != VK_SAMPLE_COUNT_1_BIT ||
            info.extent.width != width ||
            info.extent.height != height ||
            !(info.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
          throw std::runtime_error(
              "ANGLE export metadata incompatible with XR copy");
        image.sources.push_back(source);
      }
      VkCommandBufferAllocateInfo alloc{
          VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      alloc.commandPool = state.pool;
      alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      alloc.commandBufferCount = 1;
      check(
          device_proc<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(
              gpu_device().device, &alloc, &image.command),
          "vkAllocateCommandBuffers");
      image.fence = make_fence();
    }
    if (angle_function<PFNGLGETERRORPROC>("glGetError")() != GL_NO_ERROR)
      throw std::runtime_error("GL error creating XR bridge");
  } catch (const std::exception& e) {
    failure(error, e.what());
    return nullptr;
  }
  return result;
}
bool XrImageBridge::acquire(std::uint32_t index, std::string* error, std::int64_t* wait_ns) {
  if (wait_ns) *wait_ns = 0;
  std::lock_guard lock(impl_->lock);
  if (index >= impl_->images.size() || impl_->images[index].acquired ||
      impl_->images[index].failed)
    return failure(error,
                   "Invalid, failed, or already-acquired XR bridge index");
  auto& image = impl_->images[index];
  try {
    if (image.submitted) {
      const auto start = wait_ns ? monotonic_ns() : 0;
      wait_fence(*image.submitted_fence);
      if (wait_ns) *wait_ns = monotonic_ns() - start;
      image.submitted = false;
      image.submitted_fence.reset();
    }
    if (image.released_to_vulkan) {
      std::lock_guard queue(gpu_device().queue_lock);
      std::vector<GLenum> layouts(image.bridge_textures.size(),
                                  GL_LAYOUT_TRANSFER_SRC_EXT);
      angle_function<AcquireTextures>("glAcquireTexturesANGLE")(
          static_cast<GLuint>(layouts.size()), image.bridge_textures.data(),
          layouts.data());
      image.released_to_vulkan = false;
    }
    image.acquired = true;
    return true;
  } catch (const std::exception& e) {
    return failure(error, e.what());
  }
}
bool XrImageBridge::release(std::uint32_t index, std::string* error, bool bottom_up,
                             std::uint32_t source_texture, std::uint32_t end_query, std::int64_t* ownership_wait_ns,
                             std::uint32_t begin_query) {
  if (ownership_wait_ns) *ownership_wait_ns = 0;
  std::lock_guard lock(impl_->lock);
  if (index >= impl_->images.size() || !impl_->images[index].acquired ||
      impl_->images[index].failed)
    return failure(error, "XR bridge image was not acquired or has failed");
  auto& image = impl_->images[index];
  auto& desc = impl_->description;
  if (desc.external_subresources && !source_texture)
    return failure(error, "External subresource bridge requires its guest source texture");
  std::unique_lock queue_lock(gpu_device().queue_lock);
  if (source_texture && !desc.external_subresources && desc.layers == 1 && desc.mip_count == 1)
    return failure(error, "Direct bridge sources require layers or mip levels");
  try {
    if (desc.external_subresources || desc.layers > 1 || desc.mip_count > 1) {
      auto copy = reinterpret_cast<PFNGLCOPYIMAGESUBDATAPROC>(
          angle_proc("glCopyImageSubDataEXT"));
      if (!copy)
        copy = angle_function<PFNGLCOPYIMAGESUBDATAPROC>("glCopyImageSubData");
      if (!copy)
        throw std::runtime_error("Array/mip bridge requires GL copy_image");
      for (GLuint layer = 0; layer < desc.layers; ++layer)
        for (GLuint level = 0; level < desc.mip_count; ++level)
          copy(source_texture ? source_texture : impl_->textures[index],
               desc.layers > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D,
               level, 0, 0, layer, image.bridge_textures[layer * desc.mip_count + level],
               GL_TEXTURE_2D, 0, 0, 0, 0,
               std::max(1u, desc.width >> level), std::max(1u, desc.height >> level), 1);
    }
    // The timestamp must precede ReleaseTextures' submission. Issuing it
    // afterwards leaves it pending until another GL submission, measuring
    // the next CPU frame's delay as part of this GPU handoff.
    if (end_query && !impl_->gpu_ownership)
      angle_function<PFNGLQUERYCOUNTEREXTPROC>("glQueryCounterEXT")(
          end_query, GL_TIMESTAMP_EXT);
    std::vector<GLenum> layouts(image.sources.size());
    if (image.direct && image.direct->released) {
      layouts[0] = image.direct->layout;
    } else {
      angle_function<TransferTextures>("glReleaseTexturesANGLE")(
          static_cast<GLuint>(layouts.size()), image.bridge_textures.data(), layouts.data());
      if (image.direct) {
        image.direct->released = true;
        image.direct->layout = layouts[0];
      } else image.released_to_vulkan = true;
    }
    if (angle_function<PFNGLGETERRORPROC>("glGetError")() != GL_NO_ERROR)
      throw std::runtime_error("GL error releasing XR bridge textures");
    check(device_proc<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(
              image.command, 0),
          "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(device_proc<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(
              image.command, &begin),
          "vkBeginCommandBuffer");
    auto timestamp = [&](std::uint32_t which) {
      // BOTTOM_OF_PIPE: written once all earlier queue work has completed.
      device_proc<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(
          image.command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, impl_->queries, 2 * index + which);
    };
    if (impl_->queries) {
      device_proc<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool")(
          image.command, impl_->queries, 2 * index, 2);
      timestamp(0);
    }
    barrier(image.command, impl_->targets[index],
            image.initialized ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                              : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, desc.layers, desc.mip_count);
    for (std::size_t subresource = 0; subresource < image.sources.size(); ++subresource) {
      const auto layer = static_cast<std::uint32_t>(subresource / desc.mip_count);
      const auto level = static_cast<std::uint32_t>(subresource % desc.mip_count);
      const auto width = static_cast<std::int32_t>(std::max(1u, desc.width >> level));
      const auto height = static_cast<std::int32_t>(std::max(1u, desc.height >> level));
      barrier(image.command, image.sources[subresource], vk_layout(layouts[subresource]),
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1);
      // GLES framebuffer rows increase from bottom to top; Vulkan/OpenXR
      // subimages increase from top to bottom. Convert the image coordinate
      // convention here, before XR ownership transfer (never in capture code).
      // VrApi layers whose texture row 0 is their top are copied unchanged.
      VkImageBlit blit{};
      blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1};
      blit.srcOffsets[0] = {0, bottom_up ? height : 0, 0};
      blit.srcOffsets[1] = {width, bottom_up ? 0 : height, 1};
      blit.dstOffsets[1] = {width, height, 1};
      device_proc<PFN_vkCmdBlitImage>("vkCmdBlitImage")(
          image.command, image.sources[subresource], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          impl_->targets[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
      if (image.direct && vk_layout(layouts[subresource]) != VK_IMAGE_LAYOUT_UNDEFINED)
        barrier(image.command, image.sources[subresource],
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk_layout(layouts[subresource]), 1);
      else if (angle_uses_general_layout())
        barrier(image.command, image.sources[subresource],
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, 1);
    }
    barrier(image.command, impl_->targets[index],
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, desc.layers, desc.mip_count);
    if (impl_->queries) timestamp(1);
    check(device_proc<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(
              image.command),
          "vkEndCommandBuffer");
    auto finish = [&image, direct = bool(desc.source_texture), format = desc.format,
                   gpu_ownership = impl_->gpu_ownership, begin_query, end_query](bool submitted) {
      image.submitted = submitted;
      if (!submitted) { image.failed = true; return; }
      // Restore guest ownership only after the batched Vulkan submit. Queue
      // ordering makes subsequent ANGLE rendering wait for the orientation blit.
      if (gpu_ownership && begin_query)
        angle_function<PFNGLQUERYCOUNTEREXTPROC>("glQueryCounterEXT")(begin_query, GL_TIMESTAMP_EXT);
      if (direct && image.direct->released) {
        auto& source = *image.direct;
        const GLenum layout = vk_layout(source.layout) == VK_IMAGE_LAYOUT_UNDEFINED
            ? GL_LAYOUT_TRANSFER_SRC_EXT : source.layout;
        angle_function<AcquireTextures>("glAcquireTexturesANGLE")(1, &source.texture, &layout);
        source.released = false;
        if (gpu_ownership) gpu_acquire_marker(source, format);
        if (angle_function<PFNGLGETERRORPROC>("glGetError")() != GL_NO_ERROR)
          throw std::runtime_error("GL error reacquiring direct XR source");
      }
      // Measure the GPU ownership read/wait separately from the Vulkan blit.
      // The old GLES-copy interval precedes the blit and would miss this work.
      if (gpu_ownership) {
        if (end_query)
          angle_function<PFNGLQUERYCOUNTEREXTPROC>("glQueryCounterEXT")(end_query, GL_TIMESTAMP_EXT);
        angle_function<PFNGLFLUSHPROC>("glFlush")();
      }
    };
    if (batch.active) {
      batch.ownership_wait |= desc.source_texture != 0 && !impl_->gpu_ownership;
      if (!batch.preferred) batch.preferred = &image.fence;
      batch.images.push_back({&image.submitted_fence, &image.submitted, &image.failed});
      batch.commands.push_back(image.command);
      batch.finish.push_back(std::move(finish));
    } else {
      // A former stereo batch may still own this fence through another eye.
      if (image.fence.use_count() > 1) image.fence = make_fence();
      check(device_proc<PFN_vkResetFences>("vkResetFences")(
          gpu_device().device, 1, image.fence.get()), "vkResetFences");
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &image.command;
      {
        QueueGuard queue;
        check(device_proc<PFN_vkQueueSubmit>("vkQueueSubmit")(
            gpu_device().queue, 1, &submit, *image.fence), "vkQueueSubmit");
      }
      image.submitted_fence = image.fence;
      image.submitted = true;
      if (desc.source_texture && !impl_->gpu_ownership) {
        const auto start = ownership_wait_ns ? monotonic_ns() : 0;
        queue_lock.unlock();
        wait_fence(*image.submitted_fence);
        queue_lock.lock();
        if (ownership_wait_ns) *ownership_wait_ns = monotonic_ns() - start;
      }
      finish(true);
    }
    image.initialized = true;
    image.acquired = false;
    image.timed = impl_->queries != VK_NULL_HANDLE;
    return true;
  } catch (const std::exception& e) {
    image.failed = true;
    return failure(error, e.what());
  }
}
bool XrImageBridge::begin_batch() {
  if (batch.active) return false;
  batch.active = true;
  return true;
}
bool XrImageBridge::end_batch(std::string* error, std::int64_t* ownership_wait_ns) {
  if (ownership_wait_ns) *ownership_wait_ns = 0;
  if (!batch.active) return true;
  auto& current = batch;
  struct Clear {
    ~Clear() {
      batch.active = batch.ownership_wait = false;
      batch.preferred = nullptr;
      batch.commands.clear();
      batch.images.clear();
      batch.finish.clear();
    }
  } clear;
  bool submitted = false;
  try {
    if (!current.commands.empty()) {
      if (current.preferred->use_count() > 1) *current.preferred = make_fence();
      auto fence = *current.preferred;
      check(device_proc<PFN_vkResetFences>("vkResetFences")(
          gpu_device().device, 1, fence.get()), "vkResetFences batch");
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.commandBufferCount = static_cast<std::uint32_t>(current.commands.size());
      submit.pCommandBuffers = current.commands.data();
      {
        QueueGuard queue;
        check(device_proc<PFN_vkQueueSubmit>("vkQueueSubmit")(
            gpu_device().queue, 1, &submit, *fence), "vkQueueSubmit batch");
      }
      submitted = true;
      for (auto& image : current.images) {
        *image.fence = fence;
        *image.submitted = true;
      }
      // ANGLE acquire does not track external Vulkan queue serials. Complete
      // the batch before returning exported storage to any guest producer.
      if (current.ownership_wait) {
        const auto start = ownership_wait_ns ? monotonic_ns() : 0;
        wait_fence(*fence);
        if (ownership_wait_ns) *ownership_wait_ns = monotonic_ns() - start;
      }
      std::lock_guard queue(gpu_device().queue_lock);
      std::string reacquire_error;
      for (auto& finish : current.finish) {
        try { finish(true); }
        catch (const std::exception& e) { if (reacquire_error.empty()) reacquire_error = e.what(); }
      }
      if (!reacquire_error.empty()) throw std::runtime_error(reacquire_error);
    }
    return true;
  } catch (const std::exception& e) {
    // A rejected submit leaves an unsignalled fence. Never wait for it during
    // cleanup. After a successful submit, preserve the fence even if GL failed.
    for (auto& image : current.images) *image.failed = true;
    if (!submitted) for (auto& finish : current.finish) finish(false);
    return failure(error, e.what());
  }
}
bool XrImageBridge::take_gpu_span(std::uint32_t index, std::uint64_t& begin_ns,
                                  std::uint64_t& end_ns) {
  std::lock_guard lock(impl_->lock);
  if (index >= impl_->images.size()) return false;
  auto& image = impl_->images[index];
  if (!image.timed || image.submitted) return false;
  image.timed = false;
  std::uint64_t ticks[2]{};
  if (device_proc<PFN_vkGetQueryPoolResults>("vkGetQueryPoolResults")(
          gpu_device().device, impl_->queries, 2 * index, 2, sizeof(ticks), ticks,
          sizeof(ticks[0]), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
    return false;
  begin_ns = static_cast<std::uint64_t>(double(ticks[0]) * impl_->timestamp_period);
  end_ns = static_cast<std::uint64_t>(double(ticks[1]) * impl_->timestamp_period);
  return true;
}
}  // namespace venture::gfx
