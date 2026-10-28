#include "gfx/dispatch.h"

#include <algorithm>
#include <map>
#include <mutex>

#include "core/linker.h"
#include "gfx/angle.h"
#include "core/process.h"
#include "hle/gles/gles_state.h"
#include "hle/egl/egl.h"

namespace venture::gfx {
namespace {
std::recursive_mutex registry_lock;
std::map<std::string, GuestAddr, std::less<>> addresses;
std::set<std::string, std::less<>> unavailable32;
bool try_graphics(hle::CallContext& ctx) {
  // GL_KHR_debug may synchronously enter guest code from any ANGLE command.
  // First-use extension discovery also takes registration locks; initialize it
  // at the original outer boundary before admitting ordinary forwards.
  if (gl_dispatch_needs_run_loop()) return false;
  ctx.info().handler(ctx);  // Preserve error capture, current lookup and A32 adapters.
  return true;
}
bool try_query(hle::CallContext& ctx) {
  // QUERY_RESULT waits for GPU completion; availability queries do not.
  return ctx.x(1) != GL_QUERY_RESULT && try_graphics(ctx);
}
bool try_draw(hle::CallContext& ctx) {
  return !gl_dispatch_needs_run_loop() && try_gl_draw_dispatch(ctx);
}
bool try_egl(hle::CallContext& ctx) {
  return !gl_dispatch_needs_run_loop() && try_egl_dispatch(ctx);
}
}  // namespace
hle::Execution graphics_execution(std::string_view name, GuestIsa isa, GlCheck check) {
  if (name.starts_with("egl")) {
    // Every ILP32 EGL adapter takes the lifecycle lock. Its slow owner may
    // allocate guest handle/string storage and wait for this JIT to stop.
    if (isa == GuestIsa::a32) return hle::Execution::RunLoop;
    // Lifecycle/ownership changes and CPU/GPU waits retain the outer boundary.
    if (name.starts_with("eglGet") || name.starts_with("eglQuery") ||
        name == "eglChooseConfig" || name == "eglInitialize" ||
        name == "eglSwapInterval" || name == "eglSurfaceAttrib" || name == "eglBindAPI" ||
        name.starts_with("eglCreateSync") || name.starts_with("eglDestroySync") ||
        name.starts_with("eglCreateImage") || name.starts_with("eglDestroyImage")) {
      return hle::Execution::InJitFp;
    }
    return hle::Execution::RunLoop;
  }
  // These helpers may materialize maps or take a shared map lock whose owner
  // allocates guest storage. Keep their existing implementation and gate boundary.
  if (buffer_gl_handler(name)) return hle::Execution::RunLoop;
  // Draw validation can use its existing framebuffer cache while holding only
  // uncontended share-group locks. Mapping/materialization still falls back.
  if (check == GlCheck::draw) return hle::Execution::InJitFp;
  if (check != GlCheck::none) return hle::Execution::RunLoop;
  if (name == "glFinish" || name.starts_with("glClientWaitSync") ||
      name.starts_with("glWaitSync") || name.starts_with("glReadPixels") ||
      name.starts_with("glReadnPixels") || name.starts_with("glMapBuffer") ||
      name.starts_with("glUnmapBuffer") || name.starts_with("glFlushMappedBufferRange") ||
      name == "glDeleteBuffers" || name == "glDeleteTextures" ||
      name == "glBufferData" || name == "glBufferStorageEXT" ||
      name.starts_with("glDebugMessageCallback") ||
      (isa == GuestIsa::a32 && (name == "glGetString" || name == "glGetStringi" ||
                              name == "glFenceSync" || name == "glDeleteSync")))
    return hle::Execution::RunLoop;
  return hle::Execution::InJitFp;
}
void add_graphics_function(HleLibrary& library, std::string_view name,
                           hle::Handler handler, void* host, GlCheck check) {
  std::lock_guard lock(registry_lock);
  auto [it, inserted] = addresses.try_emplace(std::string(name), 0);
  if (inserted) {
    if (process_config().isa() == GuestIsa::a32) {
      hle::FunctionInfo probe{handler, host, library.soname(), std::string(name)};
      hle::a32::prepare(probe);
      if (probe.handler == hle::a32::diagnostic) unavailable32.emplace(name);
    }
    auto execution = unavailable32.contains(name) ? hle::Execution::RunLoop :
        graphics_execution(name, process_config().isa(), check);
    const auto conditional = check == GlCheck::draw ? try_draw :
        name.starts_with("egl") ? try_egl :
        name.starts_with("glGetQueryObject") ? try_query : try_graphics;
    it->second = hle::make_thunk(library.soname(), name, handler, host, execution,
        execution == hle::Execution::RunLoop ? nullptr : conditional);
  }
  library.add_function(name, it->second);
}
void register_gles(HleLibrary& library) {
  if (process_config().isa() == GuestIsa::a32) register_gles32_adapters();
  for (const auto& cmd : gl_commands()) {
    if (blocked_gl_command(cmd.name)) continue;
    if (auto host = angle_proc(cmd.name)) {
      auto special = special_gl_handler(cmd.name);
      add_graphics_function(library, cmd.name, special ? special : cmd.handler,
                            host, cmd.check);
    }
  }
}
void register_graphics_libraries() {
  static std::once_flag once;
  std::call_once(once, [] {
    auto egl = std::make_shared<HleLibrary>("libEGL.so");
    auto gles = std::make_shared<HleLibrary>(
        "libGLESv2.so", std::vector<std::string>{"libGLESv3.so"});
    register_egl(*egl);
    register_gles(*gles);
    Linker::get().register_hle_library(egl);
    Linker::get().register_hle_library(gles);
  });
}
GuestAddr guest_proc_address(std::string_view name) {
  register_graphics_libraries();
  std::lock_guard lock(registry_lock);
  if (unavailable32.contains(name)) return 0;
  auto it = addresses.find(name);
  return it == addresses.end() ? 0 : it->second;
}
bool gl_extension_supported(std::string_view name) {
  // ANGLE_multiview_multisample adds no entry points or host handles: it
  // permits multisample array attachments in FramebufferTextureMultiviewOVR.
  if (name == "GL_ANGLE_multiview_multisample") return true;
  // External OS handles belong to Android, and ANGLE-private entry points must
  // not expose its Vulkan handles or host function pointers to the guest.
  if (name.starts_with("GL_ANGLE_") || name.starts_with("GL_CHROMIUM_") ||
      name.starts_with("GL_QCOM_") || name.starts_with("GL_NV_") ||
      name.starts_with("GL_EXT_memory_object") ||
      name.starts_with("GL_EXT_semaphore") ||
      name == "GL_EXT_external_buffer" ||
      name == "GL_EXT_EGL_image_storage_compression")
    return false;
  const auto extensions = gl_extensions();
  auto ext = std::ranges::find_if(
      extensions, [&](const auto& e) { return name == e.name; });
  if (ext == extensions.end()) return false;
  return std::ranges::all_of(ext->commands, [](const char* cmd) {
    std::lock_guard lock(registry_lock);
    return !unavailable32.contains(cmd) && !blocked_gl_command(cmd) && angle_proc(cmd);
  });
}
bool blocked_gl_command(std::string_view name) {
  return name.ends_with("ANGLE") || name.ends_with("CHROMIUM") ||
         name.ends_with("QCOM") || name.ends_with("NV") ||
         name.find("MemoryObject") != name.npos ||
         name.find("Semaphore") != name.npos ||
         name.find("StorageMem") != name.npos ||
         name.find("ImportMemory") != name.npos ||
         name.find("Win32") != name.npos || name.find("FdEXT") != name.npos ||
         name == "glBufferStorageExternalEXT" ||
         name == "glNamedBufferStorageExternalEXT";
}
}  // namespace venture::gfx
