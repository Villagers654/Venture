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
        d.mip_count > std::bit_width(std::max(d.width, d.height)))
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
    if (!gfx::capture_gl_errors())
      return error("Allocating XR texture ring failed");
    return XR_SUCCESS;
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
