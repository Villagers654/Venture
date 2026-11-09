#include "gfx/shared_texture.h"

#include <mutex>
#include <stdexcept>

#include "common/log.h"
#include "gfx/gpu_device.h"
#include "gfx/shared_image_import.h"
#include "gfx/xr_images.h"

namespace venture::gfx {
namespace {
using ExportImage = EGLBoolean (*)(EGLDisplay, EGLImageKHR, void*, void*);
using TransferTextures = void (*)(GLuint, const GLuint*, GLenum*);
using AcquireTextures = void (*)(GLuint, const GLuint*, const GLenum*);

PFN_vkVoidFunction device_proc(const char* name) {
  auto& gpu = gpu_device();
  static const auto get = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
      gpu.get_instance_proc_addr(gpu.instance, "vkGetDeviceProcAddr"));
  return get ? get(gpu.device, name) : nullptr;
}
template <class T> T vk(const char* name) { return reinterpret_cast<T>(device_proc(name)); }

// One copy at a time on the host queue; the CPU waits for it, so the picture
// may return to its producer once the texture is handed to GL.
struct Copier {
  std::mutex lock;
  SharedImageImports imports;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool ready = false, failed = false;
  bool initialize() {
    if (ready || failed) return ready;
    failed = true;
    auto& gpu = gpu_device();
    imports.bind(gpu.device, device_proc);
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = gpu.queue_family;
    if (vk<PFN_vkCreateCommandPool>("vkCreateCommandPool")(gpu.device, &pool_info, nullptr, &pool) != VK_SUCCESS)
      return false;
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
                                         VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (vk<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(gpu.device, &allocate, &command) != VK_SUCCESS)
      return false;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vk<PFN_vkCreateFence>("vkCreateFence")(gpu.device, &fence_info, nullptr, &fence) != VK_SUCCESS) return false;
    failed = false;
    return ready = true;
  }
  // Copies `source` (released to EXTERNAL in GENERAL) into `target`, whose
  // current layout is `from`, leaving it in `to`.
  bool copy(VkImage source, VkImage target, VkImageLayout from, VkImageLayout to, std::uint32_t width,
            std::uint32_t height, std::string* error) {
    auto& gpu = gpu_device();
    auto fail = [&](const char* what, VkResult result) {
      if (error) *error = std::string(what) + " failed: VkResult=" + std::to_string(int(result));
      return false;
    };
    VkResult result = vk<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(command, 0);
    if (result != VK_SUCCESS) return fail("vkResetCommandBuffer", result);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vk<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command, &begin);
    if (result != VK_SUCCESS) return fail("vkBeginCommandBuffer", result);
    VkImageMemoryBarrier barriers[2]{{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}, {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}};
    // Acquire the producer's release.
    barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].oldLayout = barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    barriers[0].dstQueueFamilyIndex = gpu.queue_family;
    barriers[0].image = source;
    barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // The texture is new; ANGLE's earlier work on this queue (its storage
    // initialization, if any) precedes it in submission order.
    barriers[1].srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].oldLayout = from;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].srcQueueFamilyIndex = barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].image = target;
    barriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
    VkImageCopy region{};
    region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {width, height, 1};
    vk<PFN_vkCmdCopyImage>("vkCmdCopyImage")(command, source, VK_IMAGE_LAYOUT_GENERAL, target,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    auto& done = barriers[1];
    done.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    done.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    done.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    done.newLayout = to;
    vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &done);
    result = vk<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command);
    if (result != VK_SUCCESS) return fail("vkEndCommandBuffer", result);
    result = vk<PFN_vkResetFences>("vkResetFences")(gpu.device, 1, &fence);
    if (result != VK_SUCCESS) return fail("vkResetFences", result);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    {
      // ANGLE submits to this queue too (gfx/gpu_device.h).
      std::lock_guard queue(gpu.queue_lock);
      if (!lock_angle_queue()) {
        if (error) *error = "ANGLE queue lock unavailable";
        return false;
      }
      result = vk<PFN_vkQueueSubmit>("vkQueueSubmit")(gpu.queue, 1, &submit, fence);
      unlock_angle_queue();
    }
    if (result != VK_SUCCESS) return fail("vkQueueSubmit", result);
    result = vk<PFN_vkWaitForFences>("vkWaitForFences")(gpu.device, 1, &fence, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) return fail("vkWaitForFences", result);
    return true;
  }
};
Copier& copier() {
  static Copier* instance = new Copier;  // lives with the process-wide device
  return *instance;
}
GLenum gl_layout(VkImageLayout layout) {
  return layout == VK_IMAGE_LAYOUT_GENERAL ? GL_LAYOUT_GENERAL_EXT : GL_LAYOUT_SHADER_READ_ONLY_EXT;
}
}  // namespace

bool gl_imports_shared_images() {
  // Never initializes graphics: a GLES consumer exists only once ANGLE does.
  if (!angle_initialized()) return false;
  static const bool supported = [] {
    if (!angle_shared_device()) return false;
    // gpu_device() publishes the host GPU identity (ANGLE may have created
    // the device through Venture's callbacks without it).
    if (!shared_image_device(gpu_device().device) || !host_gpu()) return false;
    return angle_proc("eglExportVkImageANGLE") && angle_proc("glReleaseTexturesANGLE") &&
           angle_proc("glAcquireTexturesANGLE") && angle_proc("glTexStorage2D");
  }();
  return supported;
}

bool shared_image_texture(const SharedImage& picture, SharedTexture* out, std::string* error) {
  *out = {};
  auto host = host_gpu();
  if (!gl_imports_shared_images() || !host || host->device_uuid != picture.device_uuid ||
      picture.format != VK_FORMAT_R8G8B8A8_UNORM || !picture.width || !picture.height) {
    if (error) *error = "shared image is not importable here";
    return false;
  }
  const auto display = angle_display();
  const auto context = angle_function<PFNEGLGETCURRENTCONTEXTPROC, "eglGetCurrentContext">()();
  if (context == EGL_NO_CONTEXT) {
    if (error) *error = "no current GL context";
    return false;
  }
  auto get = angle_function<PFNGLGETINTEGERVPROC, "glGetIntegerv">();
  auto bind = angle_function<PFNGLBINDTEXTUREPROC, "glBindTexture">();
  auto remove = angle_function<PFNGLDELETETEXTURESPROC, "glDeleteTextures">();
  GLint previous = 0;
  get(GL_TEXTURE_BINDING_2D, &previous);
  GLuint texture = 0;
  angle_function<PFNGLGENTEXTURESPROC, "glGenTextures">()(1, &texture);
  bind(GL_TEXTURE_2D, texture);
  auto parameter = angle_function<PFNGLTEXPARAMETERIPROC, "glTexParameteri">();
  parameter(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  parameter(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  angle_function<PFNGLTEXSTORAGE2DPROC, "glTexStorage2D">()(GL_TEXTURE_2D, 1, GL_RGBA8, GLsizei(picture.width),
                                                            GLsizei(picture.height));
  bind(GL_TEXTURE_2D, GLuint(previous));
  const EGLint attributes[]{EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
  auto image = angle_function<PFNEGLCREATEIMAGEKHRPROC, "eglCreateImageKHR">()(
      display, context, EGL_GL_TEXTURE_2D_KHR, reinterpret_cast<EGLClientBuffer>(std::uintptr_t(texture)), attributes);
  auto discard = [&](const std::string& why) {
    if (image != EGL_NO_IMAGE_KHR) angle_function<PFNEGLDESTROYIMAGEKHRPROC, "eglDestroyImageKHR">()(display, image);
    remove(1, &texture);
    if (error) *error = why;
    return false;
  };
  if (image == EGL_NO_IMAGE_KHR) return discard("texture EGLImage failed");
  VkImage target = VK_NULL_HANDLE;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  if (!angle_function<ExportImage, "eglExportVkImageANGLE">()(display, image, &target, &info) || !target ||
      info.format != VK_FORMAT_R8G8B8A8_UNORM || info.extent.width != picture.width ||
      info.extent.height != picture.height || info.mipLevels != 1 || info.arrayLayers != 1 ||
      !(info.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
    return discard("ANGLE texture export is incompatible");
  // Vulkan owns the texture between release and acquire (ANGLE_vulkan_image).
  GLenum released = GL_NONE;
  angle_function<TransferTextures, "glReleaseTexturesANGLE">()(1, &texture, &released);
  VkImageLayout from;
  try {
    from = xr_released_layout(released);
  } catch (const std::exception& e) {
    return discard(e.what());
  }
  const auto to = angle_uses_general_layout() ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  bool copied = false;
  std::string failure;
  {
    auto& state = copier();
    std::lock_guard lock(state.lock);
    if (!state.initialize()) {
      failure = "host copy setup failed";
    } else {
      VkResult import_result = VK_SUCCESS;
      const VkImage source = state.imports.import(picture, &import_result);
      if (!source) failure = "import failed: VkResult=" + std::to_string(int(import_result));
      else copied = state.copy(source, target, from, to, picture.width, picture.height, &failure);
    }
  }
  // Hand the texture back either way; uncopied contents are undefined but the
  // texture is deleted below.
  const GLenum layout = copied ? gl_layout(to) : released;
  angle_function<AcquireTextures, "glAcquireTexturesANGLE">()(1, &texture, &layout);
  if (!copied) return discard(failure);
  out->texture = texture;
  out->image = image;
  return true;
}

}  // namespace venture::gfx
