#include "common/environment.h"
#include "common/log.h"
#include "xr/xr_images.h"

#if defined(VENTURE_XR_HAS_ANGLE)
#include "core/wait.h"
#include "gfx/angle.h"
#include "gfx/gpu_device.h"
#include "gfx/xr_images.h"
#include "hle/gles/gles_state.h"
#include "hle/vulkan/timing.h"
#include "platform/profile.h"
#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <bit>
#include <map>
#include <tuple>

namespace venture::xr {
namespace {
class AngleImages final : public XrImages {
  using Key = std::pair<const gfx::TextureGroup *, std::uint32_t>;
  // VENTURE_VULKAN_GPU_TIMING: the last copy into a destination is reported once
  // its bridge fence has been waited, at the destination's next acquire.
  struct Pending {
    std::uint64_t frame = 0;
    std::int64_t cpu_ns = 0, release_cpu_ns = 0, wait_ns = 0, thread_cpu_ns = -1;
    GLuint queries[2]{};  // GL timestamps before/after the GLES copy
    bool valid = false;
  };
  struct Destination {
    const gfx::TextureGroup *group;
    std::unique_ptr<gfx::XrImageBridge> bridge;
    Pending pending;
    bool direct_source = false;
    std::uint32_t source_texture = 0;
  };
  static bool timer_queries() {
    static const bool available = [] {
      auto *text = gfx::angle_function<PFNGLGETSTRINGPROC>("glGetString")(GL_EXTENSIONS);
      return text && gfx::has_extension(reinterpret_cast<const char *>(text),
                                        "GL_EXT_disjoint_timer_query");
    }();
    return available;
  }
  static void report(Destination &d) {
    auto &p = d.pending;
    if (!p.valid) return;
    p.valid = false;
    vulkan::XrCopySample s{"gles", p.frame, p.cpu_ns, p.release_cpu_ns, p.wait_ns, 0, 0, 0, 0};
    s.thread_cpu_ns = p.thread_cpu_ns;
    s.gpu_ownership = d.bridge->gpu_ownership();
    if (p.queries[0]) {
      auto get = gfx::angle_function<PFNGLGETQUERYOBJECTUI64VEXTPROC>("glGetQueryObjectui64vEXT");
      GLuint64 ready[2]{};
      get(p.queries[0], GL_QUERY_RESULT_AVAILABLE_EXT, &ready[0]);
      get(p.queries[1], GL_QUERY_RESULT_AVAILABLE_EXT, &ready[1]);
      if (ready[0] && ready[1]) {
        get(p.queries[0], GL_QUERY_RESULT_EXT, &s.gl_begin_ns);
        get(p.queries[1], GL_QUERY_RESULT_EXT, &s.gl_after_ns);
      }
    }
    d.bridge->take_gpu_span(0, s.vk_begin_ns, s.vk_end_ns);
    vulkan::timing_xr_copy(s);
  }
  std::map<Key, ImageDesc> owned;
  std::map<VkImage, Destination> targets;
  // Exported per-layer staging of OpenXR GLES slots (create_staging).
  struct Staging {
    const gfx::TextureGroup *group = nullptr;
    std::vector<GLuint> textures;
    std::vector<EGLImageKHR> siblings;
  };
  std::map<const StagedImage *, Staging> staging;
  using ExportImage = EGLBoolean (*)(EGLDisplay, EGLImageKHR, void *, void *);
  using TransferTextures = void (*)(GLuint, const GLuint *, GLenum *);
  using AcquireTextures = void (*)(GLuint, const GLuint *, const GLenum *);
  // Drains GL errors raised by Venture's own calls; returns the first.
  static GLenum own_gl_error() {
    auto get = gfx::angle_function<PFNGLGETERRORPROC>("glGetError");
    GLenum first = GL_NO_ERROR;
    for (GLenum e; (e = get()) != GL_NO_ERROR;) {
      gfx::note_angle_gl_error(e, "XR image handoff");
      if (first == GL_NO_ERROR) first = e;
    }
    return first;
  }
  static PFNGLCOPYIMAGESUBDATAPROC copy_image() {
    auto copy = reinterpret_cast<PFNGLCOPYIMAGESUBDATAPROC>(gfx::angle_proc("glCopyImageSubDataEXT"));
    return copy ? copy : reinterpret_cast<PFNGLCOPYIMAGESUBDATAPROC>(gfx::angle_proc("glCopyImageSubData"));
  }
  // EXT_external_objects layout of a Vulkan layout, for glAcquireTexturesANGLE.
  static GLenum gl_layout(VkImageLayout layout) {
    switch (layout) {
      case VK_IMAGE_LAYOUT_GENERAL: return GL_LAYOUT_GENERAL_EXT;
      case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return GL_LAYOUT_COLOR_ATTACHMENT_EXT;
      case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return GL_LAYOUT_SHADER_READ_ONLY_EXT;
      case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return GL_LAYOUT_TRANSFER_SRC_EXT;
      case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return GL_LAYOUT_TRANSFER_DST_EXT;
      default: return GL_NONE;
    }
  }
  static auto group() {
    auto context = gfx::current_gl_state();
    return context ? context->textures : std::shared_ptr<gfx::TextureGroup>{};
  }
  static GLenum target(const ImageDesc &d) {
    return d.layers > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
  }
  static XrResult error(const char *operation) {
    log::warn("xr-angle", "{}", operation);
    return XR_ERROR_RUNTIME_FAILURE;
  }

public:
  XrResult update_sampler(std::span<const std::uint32_t> names, const ImageDesc& d,
                          const SamplerOpenGLES& s) override {
    auto current = group();
    if (!current) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    for (auto name : names)
      if (!owned.contains({current.get(), name})) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    const auto* text = gfx::angle_function<PFNGLGETSTRINGPROC>("glGetString")(GL_EXTENSIONS);
    const std::string_view extensions = text ? reinterpret_cast<const char*>(text) : "";
    const bool border = gfx::has_extension(extensions, "GL_EXT_texture_border_clamp") ||
                        gfx::has_extension(extensions, "GL_OES_texture_border_clamp");
    const bool anisotropy = gfx::has_extension(extensions, "GL_EXT_texture_filter_anisotropic");
    if (((s.minFilter >= 0x9139 && s.minFilter <= 0x913b) || s.magFilter == 0x9139) &&
        !gfx::has_extension(extensions, "GL_IMG_texture_filter_cubic")) return XR_ERROR_FEATURE_UNSUPPORTED;
    if (!border && (s.wrapModeS == 0x812d || s.wrapModeT == 0x812d)) return XR_ERROR_FEATURE_UNSUPPORTED;
    if (!anisotropy && s.maxAnisotropy > 1) return XR_ERROR_FEATURE_UNSUPPORTED;
    if (anisotropy) {
      GLfloat maximum = 1;
      gfx::angle_function<PFNGLGETFLOATVPROC>("glGetFloatv")(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maximum);
      if (s.maxAnisotropy > maximum) return XR_ERROR_FEATURE_UNSUPPORTED;
    }
    gfx::preserve_gl_errors();
    const auto type = target(d);
    auto bind = gfx::angle_function<PFNGLBINDTEXTUREPROC>("glBindTexture");
    auto param = gfx::angle_function<PFNGLTEXPARAMETERIPROC>("glTexParameteri");
    GLint previous = 0;
    gfx::angle_function<PFNGLGETINTEGERVPROC>("glGetIntegerv")(
        type == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D : GL_TEXTURE_BINDING_2D_ARRAY, &previous);
    for (auto name : names) {
      bind(type, name);
      param(type, GL_TEXTURE_MIN_FILTER, s.minFilter);
      param(type, GL_TEXTURE_MAG_FILTER, s.magFilter);
      param(type, GL_TEXTURE_WRAP_S, s.wrapModeS);
      param(type, GL_TEXTURE_WRAP_T, s.wrapModeT);
      param(type, GL_TEXTURE_SWIZZLE_R, s.swizzleRed);
      param(type, GL_TEXTURE_SWIZZLE_G, s.swizzleGreen);
      param(type, GL_TEXTURE_SWIZZLE_B, s.swizzleBlue);
      param(type, GL_TEXTURE_SWIZZLE_A, s.swizzleAlpha);
      if (anisotropy) gfx::angle_function<PFNGLTEXPARAMETERFPROC>("glTexParameterf")(
          type, GL_TEXTURE_MAX_ANISOTROPY_EXT, s.maxAnisotropy);
      if (border) gfx::angle_function<PFNGLTEXPARAMETERFVPROC>("glTexParameterfv")(
          type, GL_TEXTURE_BORDER_COLOR_EXT, &s.borderColor.r);
    }
    bind(type, previous);
    return gfx::capture_gl_errors() ? XR_SUCCESS : XR_ERROR_RUNTIME_FAILURE;
  }
  bool application_uses_gles() const override { return true; }
  bool begin_handoff_batch() override {
    const char* enabled = venture::environment::getenv("VENTURE_XR_BATCH_HANDOFF");
    return enabled && std::string_view(enabled) == "1" && gfx::XrImageBridge::begin_batch();
  }
  XrResult end_handoff_batch() override {
    const bool timed = vulkan::timing_enabled();
    const auto start = timed ? monotonic_ns() : 0;
    const auto* native = timed ? [] {
      thread_local const auto value = platform::profile_current_thread();
      return &value;
    }() : nullptr;
    std::uint64_t cpu_begin = 0, cpu_end = 0;
    const bool cpu_timed = native && platform::profile_cpu_time(*native, cpu_begin);
    std::string diagnostic;
    std::int64_t wait_ns = 0;
    const bool success = gfx::XrImageBridge::end_batch(&diagnostic, timed ? &wait_ns : nullptr);
    if (timed) {
      vulkan::XrCopySample sample{"batch_submit", vulkan::timing_frame(),
          monotonic_ns() - start, 0, wait_ns, 0, 0, 0, 0};
      sample.gpu_transfer = false;
      if (cpu_timed && platform::profile_cpu_time(*native, cpu_end) && cpu_end >= cpu_begin)
        sample.thread_cpu_ns = std::int64_t(cpu_end - cpu_begin);
      vulkan::timing_xr_copy(sample);
    }
    return success ? XR_SUCCESS : error(diagnostic.c_str());
  }
  bool external_queue_active() const override { return gfx::angle_shared_device(); }
  bool lock_external_queue() override { return gfx::lock_angle_queue(); }
  void unlock_external_queue() override { gfx::unlock_angle_queue(); }
  XrResult wait_producer_sync(std::uint64_t fence) override {
    if (!fence) return XR_SUCCESS;
    std::lock_guard queue(gfx::gpu_device().queue_lock);
    auto wait = gfx::angle_function<PFNEGLWAITSYNCKHRPROC>("eglWaitSyncKHR");
    return wait && wait(gfx::angle_display(),
        reinterpret_cast<EGLSyncKHR>(std::uintptr_t(fence)), 0)
        ? XR_SUCCESS : error("VrApi completion fence wait failed");
  }
  XrResult create_ring(const ImageDesc &d, std::span<std::uint32_t> names) override {
    auto current = group();
    if (!current || !gfx::angle_shared_device())
      return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    if (!d.width || !d.height || !d.layers || !d.mip_count ||
        d.mip_count > std::uint32_t(std::bit_width(std::max(d.width, d.height))))
      return XR_ERROR_VALIDATION_FAILURE;
    if (d.samples != 1 || d.face_count != 1 ||
        (d.gl_format != GL_RGBA8 && d.gl_format != GL_SRGB8_ALPHA8 && d.gl_format != GL_RGBA16F &&
         d.gl_format != GL_RGB10_A2))
      return XR_ERROR_FEATURE_UNSUPPORTED;
    gfx::preserve_gl_errors();
    const auto type = target(d);
    GLint previous = 0;
    gfx::angle_function<PFNGLGETINTEGERVPROC>("glGetIntegerv")(
        type == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D : GL_TEXTURE_BINDING_2D_ARRAY, &previous);
    auto bind = gfx::angle_function<PFNGLBINDTEXTUREPROC>("glBindTexture");
    gfx::angle_function<PFNGLGENTEXTURESPROC>("glGenTextures")(GLsizei(names.size()), names.data());
    for (auto name : names) {
      bind(type, name);
      if (type == GL_TEXTURE_2D)
        gfx::angle_function<PFNGLTEXSTORAGE2DPROC>("glTexStorage2D")(type, d.mip_count, GLenum(d.gl_format),
                                                                     d.width, d.height);
      else
        gfx::angle_function<PFNGLTEXSTORAGE3DPROC>("glTexStorage3D")(type, d.mip_count, GLenum(d.gl_format),
                                                                     d.width, d.height, d.layers);
      owned[{current.get(), name}] = d;
    }
    bind(type, GLuint(previous));
    // Venture's own allocation errors stay out of the guest's GL error state.
    if (const auto failed = own_gl_error()) {
      log::warn("xr-angle", "Allocating the {}x{}x{} XR texture ring failed: GL error {:#x}", d.width,
                d.height, d.layers, failed);
      return failed == GL_OUT_OF_MEMORY ? XR_ERROR_OUT_OF_MEMORY : XR_ERROR_RUNTIME_FAILURE;
    }
    return XR_SUCCESS;
  }
  XrResult create_staging(const ImageDesc &d, std::span<const std::uint32_t> names,
                          std::span<StagedImage> out) override {
    auto current = group();
    if (!current || !gfx::angle_shared_device())
      return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    if (names.size() != out.size()) return XR_ERROR_VALIDATION_FAILURE;
    for (auto name : names)
      if (!owns_texture(name, d)) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    const auto *text = gfx::angle_function<PFNGLGETSTRINGPROC>("glGetString")(GL_EXTENSIONS);
    if (!text || !gfx::has_extension(reinterpret_cast<const char *>(text), "GL_ANGLE_vulkan_image") ||
        !gfx::angle_proc("eglExportVkImageANGLE") || !copy_image())
      return XR_ERROR_FEATURE_UNSUPPORTED;
    gfx::preserve_gl_errors();
    GLint previous = 0;
    gfx::angle_function<PFNGLGETINTEGERVPROC>("glGetIntegerv")(GL_TEXTURE_BINDING_2D, &previous);
    auto bind = gfx::angle_function<PFNGLBINDTEXTUREPROC>("glBindTexture");
    const auto context = gfx::angle_function<PFNEGLGETCURRENTCONTEXTPROC>("eglGetCurrentContext")();
    XrResult result = XR_SUCCESS;
    for (auto &slot : out) {
      if (XR_FAILED(result)) break;
      auto &record = staging[&slot];
      record.group = current.get();
      record.textures.resize(d.layers);
      gfx::angle_function<PFNGLGENTEXTURESPROC>("glGenTextures")(GLsizei(d.layers), record.textures.data());
      for (auto texture : record.textures) {
        bind(GL_TEXTURE_2D, texture);
        gfx::angle_function<PFNGLTEXSTORAGE2DPROC>("glTexStorage2D")(GL_TEXTURE_2D, 1, GLenum(d.gl_format),
                                                                     d.width, d.height);
        if (const auto failed = own_gl_error()) {
          log::warn("xr-angle", "XR staging {}x{} failed: GL error {:#x}", d.width, d.height, failed);
          result = failed == GL_OUT_OF_MEMORY ? XR_ERROR_OUT_OF_MEMORY : XR_ERROR_RUNTIME_FAILURE;
          break;
        }
        const EGLint attributes[]{EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        auto sibling = gfx::angle_function<PFNEGLCREATEIMAGEKHRPROC>("eglCreateImageKHR")(
            gfx::angle_display(), context, EGL_GL_TEXTURE_2D_KHR,
            reinterpret_cast<EGLClientBuffer>(std::uintptr_t(texture)), attributes);
        if (!sibling) {
          // ANGLE allocates the image here; EGL_BAD_ALLOC is a full device.
          const auto egl = gfx::angle_function<PFNEGLGETERRORPROC>("eglGetError")();
          own_gl_error();
          log::warn("xr-angle", "XR staging {}x{} EGLImage failed: EGL error {:#x}", d.width, d.height, egl);
          result = egl == EGL_BAD_ALLOC ? XR_ERROR_OUT_OF_MEMORY : XR_ERROR_RUNTIME_FAILURE;
          break;
        }
        record.siblings.push_back(sibling);
        VkImage image{};
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        if (!gfx::angle_function<ExportImage>("eglExportVkImageANGLE")(gfx::angle_display(), sibling, &image, &info) ||
            !image || info.format != d.vk_format || info.arrayLayers != 1 || info.mipLevels != 1 ||
            info.samples != VK_SAMPLE_COUNT_1_BIT || info.extent.width != d.width ||
            info.extent.height != d.height || !(info.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
          log::warn("xr-angle", "XR staging export is incompatible; using private images");
          result = XR_ERROR_FEATURE_UNSUPPORTED;
          break;
        }
        slot.layers.push_back(image);
      }
      slot.layouts.assign(slot.layers.size(), VK_IMAGE_LAYOUT_UNDEFINED);
      slot.idle_layout = gfx::angle_uses_general_layout() ? VK_IMAGE_LAYOUT_GENERAL
                                                          : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      slot.staged = false;
    }
    bind(GL_TEXTURE_2D, GLuint(previous));
    own_gl_error();
    if (XR_FAILED(result)) destroy_staging(out);
    return result;
  }
  XrResult stage(std::uint32_t name, StagedImage &slot, const ImageDesc &d) override {
    if (vulkan::timing_enabled())
      for (const auto& sample : gfx::end_gl_render_timing(vulkan::timing_frame()))
        vulkan::timing_gles_render(sample.frame, sample.begin_ns, sample.end_ns, sample.valid);
    const auto start = vulkan::timing_enabled() ? monotonic_ns() : 0;
    std::unique_lock queue(gfx::gpu_device().queue_lock);
    auto current = group();
    auto it = staging.find(&slot);
    if (!current || it == staging.end() || it->second.group != current.get() || !owns_texture(name, d) ||
        it->second.textures.size() != d.layers || slot.layers.size() != d.layers)
      return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    auto &textures = it->second.textures;
    gfx::preserve_gl_errors();
    try {
      std::vector<GLenum> layouts(textures.size());
      if (slot.staged) {
        for (std::size_t layer = 0; layer < layouts.size(); ++layer) layouts[layer] = gl_layout(slot.layouts[layer]);
        gfx::angle_function<AcquireTextures>("glAcquireTexturesANGLE")(GLuint(textures.size()), textures.data(),
                                                                       layouts.data());
        slot.staged = false;
      }
      // The base level, all layers; host presentation has one level.
      for (std::uint32_t layer = 0; layer < d.layers; ++layer)
        copy_image()(name, target(d), 0, 0, 0, d.layers > 1 ? GLint(layer) : 0, textures[layer], GL_TEXTURE_2D,
                     0, 0, 0, 0, GLsizei(d.width), GLsizei(d.height), 1);
      gfx::angle_function<TransferTextures>("glReleaseTexturesANGLE")(GLuint(textures.size()), textures.data(),
                                                                      layouts.data());
      for (std::size_t layer = 0; layer < layouts.size(); ++layer)
        slot.layouts[layer] = gfx::xr_released_layout(layouts[layer]);
      slot.staged = true;
    } catch (const std::exception &e) {
      own_gl_error();
      return error(e.what());
    }
    if (const auto failed = own_gl_error()) {
      log::warn("xr-angle", "Staging the guest XR texture failed: GL error {:#x}", failed);
      return XR_ERROR_RUNTIME_FAILURE;
    }
    if (start) {
      vulkan::XrCopySample sample{"gles-stage", vulkan::timing_frame(), monotonic_ns() - start, 0, 0, 0, 0, 0, 0};
      vulkan::timing_xr_copy(sample);
    }
    return XR_SUCCESS;
  }
  void destroy_staging(std::span<StagedImage> slots) override {
    auto current = group();
    for (auto &slot : slots) {
      auto it = staging.find(&slot);
      if (it == staging.end()) continue;
      auto &record = it->second;
      if (!current || record.group != current.get()) {
        error("Destroying XR staging requires its GLES share group current");
        continue;
      }
      // Deleting a texture Vulkan still owns needs it back first, as a bridge does.
      if (slot.staged && record.textures.size() == slot.layouts.size()) {
        std::vector<GLenum> layouts(slot.layouts.size());
        for (std::size_t layer = 0; layer < layouts.size(); ++layer)
          layouts[layer] = gl_layout(slot.layouts[layer]);
        try {
          gfx::angle_function<AcquireTextures>("glAcquireTexturesANGLE")(GLuint(record.textures.size()),
                                                                         record.textures.data(), layouts.data());
        } catch (const std::exception &e) {
          error(e.what());
        }
      }
      for (auto sibling : record.siblings)
        gfx::angle_function<PFNEGLDESTROYIMAGEKHRPROC>("eglDestroyImageKHR")(gfx::angle_display(), sibling);
      gfx::angle_function<PFNGLDELETETEXTURESPROC>("glDeleteTextures")(GLsizei(record.textures.size()),
                                                                       record.textures.data());
      staging.erase(it);
      slot = {};
    }
    own_gl_error();
  }
  void destroy_ring(std::span<const std::uint32_t> names) override {
    auto current = group();
    if (!current) {
      error("Destroying XR ring requires its GLES share group current");
      return;
    }
    std::vector<GLuint> textures;
    for (auto name : names)
      if (owned.erase({current.get(), name}))
        textures.push_back(name);
    gfx::angle_function<PFNGLDELETETEXTURESPROC>("glDeleteTextures")(GLsizei(textures.size()),
                                                                     textures.data());
  }
  bool owns_texture(std::uint32_t name, const ImageDesc &d) const override {
    auto current = group();
    if (!current)
      return false;
    auto it = owned.find({current.get(), name});
    return it != owned.end() && it->second.width == d.width && it->second.height == d.height &&
           it->second.layers == d.layers && it->second.gl_format == d.gl_format &&
           it->second.mip_count == d.mip_count && it->second.samples == d.samples &&
           it->second.face_count == d.face_count && it->second.vk_format == d.vk_format;
  }
  XrResult copy_to_image(std::uint32_t name, VkImage image, const ImageDesc &d,
                         std::uint64_t fence, bool bottom_up) override {
    if (vulkan::timing_enabled())
      for (const auto& sample : gfx::end_gl_render_timing(vulkan::timing_frame()))
        vulkan::timing_gles_render(sample.frame, sample.begin_ns, sample.end_ns, sample.valid);
    std::unique_lock queue(gfx::gpu_device().queue_lock);
    auto current = group();
    if (!current || !owns_texture(name, d))
      return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    if (d.host_mip_count > d.mip_count) return XR_ERROR_VALIDATION_FAILURE;
    gfx::preserve_gl_errors();
    // EGL HLE retains native sync identities (hle/egl/egl.cpp); wait on the
    // producer's sync in the current context before the GPU-side image copy.
    auto sync_result = wait_producer_sync(fence);
    if (XR_FAILED(sync_result)) return sync_result;
    auto copy =
        reinterpret_cast<PFNGLCOPYIMAGESUBDATAPROC>(gfx::angle_proc("glCopyImageSubDataEXT"));
    if (!copy)
      copy = gfx::angle_function<PFNGLCOPYIMAGESUBDATAPROC>("glCopyImageSubData");
    if (!copy)
      return XR_ERROR_FEATURE_UNSUPPORTED;
    std::string diagnostic;
    auto it = targets.find(image);
    if (it != targets.end() && it->second.direct_source && it->second.source_texture != name) {
      queue.unlock();
      if (!it->second.bridge->acquire(0, &diagnostic)) return error(diagnostic.c_str());
      queue.lock();
      auto result = release_images({&image, 1});
      if (XR_FAILED(result)) return result;
      it = targets.end();
    }
    if (it == targets.end()) {
      gfx::XrImageDescription desc;
      desc.width = d.width;
      desc.height = d.height;
      desc.layers = d.layers;
      desc.mip_count = d.host_mip_count ? d.host_mip_count : d.mip_count;
      desc.format = d.vk_format;
      desc.images = std::span(&image, 1);
      desc.external_subresources = d.layers > 1 || d.mip_count > 1;
      const char* direct = venture::environment::getenv("VENTURE_XR_DIRECT_TEXTURES");
      const bool eligible = direct && std::string_view(direct) == "1" &&
          d.layers == 1 && d.mip_count == 1 && d.samples == 1 && d.face_count == 1 &&
          owned.contains(Key{current.get(), name});
      if (eligible) {
        desc.source_texture = name;
        desc.source_group = reinterpret_cast<std::uintptr_t>(current.get());
      }
      auto bridge = gfx::XrImageBridge::create(desc, &diagnostic);
      bool exported = bridge && eligible;
      if (!bridge && eligible) {
        static std::once_flag reported;
        std::call_once(reported, [&] { log::warn("xr-angle", "Direct XR export unavailable: {}; using bridge copy", diagnostic); });
        desc.source_texture = 0;
        bridge = gfx::XrImageBridge::create(desc, &diagnostic);
      }
      if (!bridge) return error(diagnostic.c_str());
      it = targets.emplace(image, Destination{current.get(), std::move(bridge), {}, exported, exported ? name : 0}).first;
    }
    if (it->second.group != current.get())
      return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    auto &bridge = *it->second.bridge;
    const bool timed = vulkan::timing_enabled();
    const auto start = timed ? monotonic_ns() : 0;
    // Reuse one handle per submitting thread; acquire's fence wait is part of
    // wall time but must not be counted as render-thread CPU work.
    const auto *native = timed ? [] {
      thread_local const auto value = platform::profile_current_thread();
      return &value;
    }() : nullptr;
    std::uint64_t cpu_begin = 0;
    const bool cpu_timed = native && platform::profile_cpu_time(*native, cpu_begin);
    std::int64_t wait_ns = 0;
    queue.unlock();
    if (!bridge.acquire(0, &diagnostic, timed ? &wait_ns : nullptr))
      return error(diagnostic.c_str());
    queue.lock();
    auto &pending = it->second.pending;
    const bool queries = timed && timer_queries();
    if (timed) {
      report(it->second);
      if (queries && !pending.queries[0])
        gfx::angle_function<PFNGLGENQUERIESEXTPROC>("glGenQueriesEXT")(2, pending.queries);
      if (queries && !bridge.gpu_ownership())
        gfx::angle_function<PFNGLQUERYCOUNTEREXTPROC>("glQueryCounterEXT")(pending.queries[0],
                                                                           GL_TIMESTAMP_EXT);
    }
    // Persistent VrApi slots are unrelated to the host acquisition index.
    // Preserve authored mip contents, including levels rendered independently
    // by the application; generating them from level zero would lose data.
    const bool direct_subresources = d.layers > 1 || d.mip_count > 1;
    // The bridge extracts array layers and mip levels into exported 2D
    // images. Read the guest texture directly instead of first duplicating
    // its entire array/mip chain into the bridge's intermediate texture.
    if (!direct_subresources && !it->second.direct_source)
      copy(name, target(d), 0, 0, 0, 0, bridge.textures()[0], target(d), 0, 0, 0, 0,
           d.width, d.height, 1);
    bool copy_failed = !gfx::capture_gl_errors();
    const auto copied = timed ? monotonic_ns() : 0;
    std::int64_t ownership_wait_ns = 0;
    queue.unlock();
    if (!bridge.release(0, &diagnostic, bottom_up, direct_subresources ? name : 0,
                         queries ? pending.queries[1] : 0, timed ? &ownership_wait_ns : nullptr,
                         queries && bridge.gpu_ownership() ? pending.queries[0] : 0))
      return error(diagnostic.c_str());
    queue.lock();
    if (timed) {
      const auto end = monotonic_ns();
      pending.frame = vulkan::timing_frame();
      std::uint64_t cpu_end = 0;
      pending.thread_cpu_ns = cpu_timed && platform::profile_cpu_time(*native, cpu_end) &&
          cpu_end >= cpu_begin ? std::int64_t(cpu_end - cpu_begin) : -1;
      pending.wait_ns = wait_ns + ownership_wait_ns;
      pending.cpu_ns = end - start;
      pending.release_cpu_ns = end - copied;
      pending.valid = true;
    }
    return copy_failed ? error("Copying the guest XR texture failed") : XR_SUCCESS;
  }
  XrResult release_images(std::span<const VkImage> images) override {
    auto current = group();
    for (auto image : images) {
      auto it = targets.find(image);
      if (it != targets.end() && (!current || it->second.group != current.get()))
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    for (auto image : images) {
      auto it = targets.find(image);
      if (it == targets.end()) continue;
      if (it->second.pending.queries[0])
        gfx::angle_function<PFNGLDELETEQUERIESEXTPROC>("glDeleteQueriesEXT")(
            2, it->second.pending.queries);
      targets.erase(it);
    }
    return XR_SUCCESS;
  }
};
} // namespace
bool install_angle_xr_images() {
  // Explicit swapchain teardown clears all GL objects while a context is
  // current. The process-owned provider itself outlives EGL static teardown.
  static auto *provider = new AngleImages;
  if (!xr_images())
    install_xr_images(provider);
  return true;
}
} // namespace venture::xr
#else
namespace venture::xr {
bool install_angle_xr_images() { return false; }
} // namespace venture::xr
#endif
