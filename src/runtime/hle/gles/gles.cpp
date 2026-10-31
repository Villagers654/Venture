#include "common/environment.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <sstream>

#include <vulkan/vulkan.h>

#include "common/log.h"
#include "core/signals.h"
#include "core/thread.h"
#include "core/guest_storage.h"
#include "core/process.h"
#include "hle/egl/egl.h"
#include "hle/gles/gles_state.h"
#include "gfx/a32_handles.h"
#include "gfx/gpu_device.h"

namespace venture::gfx {
namespace {
std::mutex context_lock;
std::map<EGLContext, std::shared_ptr<GlContextState>> states;
// Callback user pointers can still be held by an ANGLE worker at context
// destruction. Keep these small records until process exit; never a dangling
// PC.
std::vector<std::shared_ptr<GlCallback>> retired_callbacks;
thread_local std::shared_ptr<GlContextState> current;
thread_local std::set<GLenum> no_context_errors;
// Whether the shared GPU samples ASTC itself rather than through ANGLE's
// decode.
bool native_astc() {
  auto& gpu = gpu_device();
  auto features = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(
      gpu.get_instance_proc_addr
          ? gpu.get_instance_proc_addr(gpu.instance, "vkGetPhysicalDeviceFeatures")
          : nullptr);
  if (!features || !gpu.physical_device) return true;
  VkPhysicalDeviceFeatures supported{};
  features(gpu.physical_device, &supported);
  return supported.textureCompressionASTC_LDR;
}
void build_extensions(GlContextState& state) {
  const auto* raw =
      angle_function<PFNGLGETSTRINGPROC, "glGetString">()(GL_EXTENSIONS);
  if (!raw) return;
  const auto* text = reinterpret_cast<const char*>(raw);
  const auto* version = reinterpret_cast<const char*>(
      angle_function<PFNGLGETSTRINGPROC, "glGetString">()(GL_VERSION));
  if (version && std::strlen(version) > 10 && version[10] >= '2' &&
      version[10] <= '3')
    state.major_version = version[10] - '0';
  const auto* renderer = reinterpret_cast<const char*>(
      angle_function<PFNGLGETSTRINGPROC, "glGetString">()(GL_RENDERER));
  if (version && renderer) state.version_string = host_gl_version(version, renderer);
  // ANGLE offers ASTC on every Vulkan GPU and, without native support (NVIDIA,
  // AMD), decodes it to RGBA8 itself. Those textures overran 8 GB GPUs
  // (Hellsweeper VR: 11.6 GB in use, GL_OUT_OF_MEMORY, black screen), so the
  // HLE fallback takes ASTC over and stores BC7 where ANGLE has BPTC.
  // VENTURE_TEXTURE_COMPRESSION=never (Settings > Runtime) keeps ANGLE's
  // RGBA8; auto has no budget feed here, so it compresses like always.
  const char* compression = std::getenv("VENTURE_TEXTURE_COMPRESSION");
  const bool compress =
      state.major_version >= 3 &&
      has_extension(text, "GL_EXT_texture_compression_bptc") &&
      !(compression && (!std::strcmp(compression, "never") ||
                        !std::strcmp(compression, "0")));
  state.astc_fallback =
      !has_extension(text, "GL_KHR_texture_compression_astc_ldr") ||
      (compress && !native_astc());
  state.bc7_fallback = state.astc_fallback && compress;
  GLint minor = 0;
  if (state.major_version >= 3)
    angle_function<PFNGLGETINTEGERVPROC, "glGetIntegerv">()(GL_MINOR_VERSION, &minor);
  std::istringstream input(text);
  for (std::string name; input >> name;)
    // ANGLE_multiview_multisample requires ES 3.1, or the ES 3.0-only
    // ANGLE_texture_multisample entry points which we do not expose. The
    // fallback offers ASTC LDR 2D only, not ANGLE's HDR/3D/decode-mode forms.
    if (gl_extension_supported(name) &&
        (name != "GL_ANGLE_multiview_multisample" ||
         (state.major_version >= 3 && minor >= 1)) &&
        !(state.astc_fallback && name.find("_astc") != std::string::npos))
      state.extensions.push_back(std::move(name));
  if (state.astc_fallback)
    state.extensions.emplace_back("GL_KHR_texture_compression_astc_ldr");
  for (const auto& name : state.extensions) {
    if (!state.extension_string.empty()) state.extension_string += ' ';
    state.extension_string += name;
  }
  state.extensions_ready = true;
}
const GLubyte* get_string(GLenum name) {
  if (name == GL_EXTENSIONS)
    if (auto state = current_gl_state())
      return reinterpret_cast<const GLubyte*>(state->extension_string.c_str());
  const auto* value = angle_function<PFNGLGETSTRINGPROC, "glGetString">()(name);
  if (value && name == GL_VERSION)
    if (auto state = current_gl_state(); state && !state->version_string.empty())
      return reinterpret_cast<const GLubyte*>(state->version_string.c_str());
  // Both native glGetString and Java GLES queries use this path. UE4 chooses
  // its Android device profile using GL_RENDERER, separately from GL_VERSION
  // and extension/feature support. Present the same headset as Build/VrApi.
  // Query ANGLE first to retain its no-current-context/error behavior.
  if (value && name == GL_RENDERER) {
    const auto renderer = process_config().device_identity->gpu_renderer;
    if (!renderer.empty()) return reinterpret_cast<const GLubyte*>(renderer.data());
  }
  return value;
}
}  // namespace
std::string host_gl_version(std::string_view version, std::string_view renderer) {
  std::uint64_t hash = 0xcbf29ce484222325ull;  // FNV-1a
  for (unsigned char c : renderer) hash = (hash ^ c) * 0x100000001b3ull;
  return std::format("{} host {:016x}", version, hash);
}
namespace {
const GLubyte* get_string_i(GLenum name, GLuint index) {
  if (name == GL_EXTENSIONS)
    if (auto state = current_gl_state()) {
      if (index >= state->extensions.size()) {
        gl_error(GL_INVALID_VALUE);
        return nullptr;
      }
      return reinterpret_cast<const GLubyte*>(state->extensions[index].c_str());
    }
  return angle_function<PFNGLGETSTRINGIPROC, "glGetStringi">()(name, index);
}
const GLubyte* get_string32(GLenum name) {
  return reinterpret_cast<const GLubyte*>(guest_string(reinterpret_cast<const char*>(get_string(name))));
}
const GLubyte* get_string_i32(GLenum name, GLuint index) {
  return reinterpret_cast<const GLubyte*>(
      guest_string(reinterpret_cast<const char*>(get_string_i(name, index))));
}
template <class T>
bool query(GLenum name, T* value) {
  if (!value) return false;
  if (auto state = current_gl_state()) {
    if (name == GL_CONTEXT_FLAGS && state->no_error) {
      GLint flags = 0;
      angle_function<PFNGLGETINTEGERVPROC, "glGetIntegerv">()(name, &flags);
      *value = static_cast<T>(flags | GL_CONTEXT_FLAG_NO_ERROR_BIT_KHR);
      return true;
    }
    if (name == GL_NUM_EXTENSIONS) {
      *value = static_cast<T>(state->extensions.size());
      return true;
    }
    if (state->astc_fallback && (name == GL_NUM_COMPRESSED_TEXTURE_FORMATS ||
                                 name == GL_COMPRESSED_TEXTURE_FORMATS)) {
      auto formats = guest_compressed_formats();
      if (name == GL_NUM_COMPRESSED_TEXTURE_FORMATS)
        *value = static_cast<T>(formats.size());
      else
        std::ranges::transform(formats, value,
                               [](GLint f) { return static_cast<T>(f); });
      return true;
    }
  }
  return false;
}
void get_integer(GLenum name, GLint* value) {
  if (!query(name, value))
    angle_function<PFNGLGETINTEGERVPROC, "glGetIntegerv">()(name, value);
}
void get_integer64(GLenum name, std::int64_t* value) {
  if (!query(name, value))
    angle_function<void (*)(GLenum, std::int64_t*), "glGetInteger64v">()(name,
                                                                       value);
}
void get_float(GLenum name, GLfloat* value) {
  if (!query(name, value))
    angle_function<PFNGLGETFLOATVPROC, "glGetFloatv">()(name, value);
}
void get_boolean(GLenum name, GLboolean* value) {
  GLint i{};
  if (name != GL_COMPRESSED_TEXTURE_FORMATS && query(name, &i) && value) {
    *value = i != 0;
    return;
  }
  if (name == GL_COMPRESSED_TEXTURE_FORMATS && current_gl_state() &&
      current_gl_state()->astc_fallback && value) {
    auto formats = guest_compressed_formats();
    std::ranges::fill(std::span(value, formats.size()), GL_TRUE);
    return;
  }
  angle_function<PFNGLGETBOOLEANVPROC, "glGetBooleanv">()(name, value);
}
GLenum get_error() {
  auto& errors = current ? current->errors : no_context_errors;
  if (current && current->no_error) {
    // KHR_no_error: "All calls to GetError will return NO_ERROR or
    // OUT_OF_MEMORY." ANGLE still validates; drop what it reports.
    preserve_gl_errors();
    const bool out_of_memory = errors.contains(GL_OUT_OF_MEMORY);
    errors.clear();
    return out_of_memory ? GL_OUT_OF_MEMORY : GL_NO_ERROR;
  }
  if (!errors.empty()) {
    auto it = errors.begin();
    auto error = *it;
    errors.erase(it);
    return error;
  }
  return angle_function<PFNGLGETERRORPROC, "glGetError">()();
}
bool valid_buffer_label(GLenum type, GLuint name) {
  // EXT_debug_label, Errors: labeling a name which is not an object of the
  // specified type generates INVALID_OPERATION. A generated, never-bound
  // buffer is only a reserved name. ANGLE's no-error path dereferences null
  // for it, so retain this boundary check even in no-error guest contexts.
  if (type != GL_BUFFER_OBJECT_EXT) return true;
  if (!current_gl_state() || !angle_function<PFNGLISBUFFERPROC, "glIsBuffer">()(name)) {
    gl_error(GL_INVALID_OPERATION);
    static std::atomic<unsigned> reports{0};
    if (reports.fetch_add(1) < 8)
      log::warn("gles", "Debug label rejected for nonexistent object type={:#x} name={}", type, name);
    return false;
  }
  return true;
}
void label_object(GLenum type, GLuint name, GLsizei length, const GLchar* label) {
  if (length < 0) { gl_error(GL_INVALID_VALUE); return; }
  if (valid_buffer_label(type, name))
    angle_function<PFNGLLABELOBJECTEXTPROC, "glLabelObjectEXT">()(type, name, length, label);
}
void get_object_label(GLenum type, GLuint name, GLsizei size, GLsizei* length, GLchar* label) {
  if (size < 0) { gl_error(GL_INVALID_VALUE); return; }
  if (valid_buffer_label(type, name))
    angle_function<PFNGLGETOBJECTLABELEXTPROC, "glGetObjectLabelEXT">()(type, name, size, length, label);
}
void GL_APIENTRY debug_callback(GLenum source, GLenum type, GLuint id,
                                GLenum severity, GLsizei length,
                                const GLchar* message,
                                const void* user) noexcept {
  auto* state = static_cast<GlCallback*>(const_cast<void*>(user));
  GuestAddr callback{}, data{};
  {
    std::lock_guard lock(state->lock);
    callback = state->function;
    data = state->user;
  }
  if (!callback) return;
  try {
    auto* thread = GuestThread::current();
    if (!thread) thread = &GuestThread::adopt_current();
    if (thread->cpu().isa() == GuestIsa::a32) {
      GuestStorage text;
      auto count = length > 0 ? static_cast<std::size_t>(length) : std::strlen(message);
      auto address = text.string(std::string_view(message, count));
      hle::a32::CallBuilder32 args;
      args.value(source).value(type).value(id).value(severity).value(length).pointer(h2g(address)).pointer(data);
      thread->call32(callback, args);
    } else thread->call(callback,
                 {source, type, id, severity,
                  static_cast<std::uint64_t>(length), h2g(message), data});
  } catch (const std::exception& e) {
    log::error("gles", "Guest GL debug callback failed: {}", e.what());
  } catch (...) {
    log::error("gles", "Guest GL debug callback failed");
  }
}
void set_debug_callback(GuestAddr callback, GuestAddr user) {
  auto state = current_gl_state();
  if (!state) {
    gl_error(GL_INVALID_OPERATION);
    return;
  }
  {
    std::lock_guard lock(state->callback->lock);
    state->callback->function = callback;
    state->callback->user = user;
    state->callback->guest_calls.store(callback != 0, std::memory_order_release);
  }
  auto fn = reinterpret_cast<PFNGLDEBUGMESSAGECALLBACKPROC>(
      angle_proc("glDebugMessageCallbackKHR"));
  if (!fn)
    fn =
        angle_function<PFNGLDEBUGMESSAGECALLBACKPROC, "glDebugMessageCallback">();
  fn(callback ? debug_callback : nullptr,
     callback ? state->callback.get() : nullptr);
}
void get_pointer(GLenum name, void** value) {
  if (auto state = current_gl_state(); state && value &&
                                       (name == GL_DEBUG_CALLBACK_FUNCTION ||
                                        name == GL_DEBUG_CALLBACK_USER_PARAM)) {
    std::lock_guard lock(state->callback->lock);
    *value = g2h(name == GL_DEBUG_CALLBACK_FUNCTION ? state->callback->function
                                                    : state->callback->user);
    return;
  }
  auto fn =
      reinterpret_cast<PFNGLGETPOINTERVPROC>(angle_proc("glGetPointervKHR"));
  if (!fn) fn = angle_function<PFNGLGETPOINTERVPROC, "glGetPointerv">();
  fn(name, value);
}
GLenum client_wait(GLsync sync, GLbitfield flags, std::uint64_t timeout) {
  const auto start = std::chrono::steady_clock::now();
  auto remaining = timeout;
  for (;;) {
    auto result = angle_function<PFNGLCLIENTWAITSYNCPROC, "glClientWaitSync">()(
        sync, flags, std::min<std::uint64_t>(remaining, 1000000));
    if (result != GL_TIMEOUT_EXPIRED || remaining == 0) return result;
    flags = 0;
    if (auto* thread = GuestThread::current())
      signals::deliver_pending(*thread);
    const auto elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
    if (elapsed >= timeout) return GL_TIMEOUT_EXPIRED;
    remaining = timeout - elapsed;
  }
}
void finish() {
  auto state = current_gl_state();
  if (!state || state->major_version < 3) {
    angle_function<PFNGLFINISHPROC, "glFinish">()();
    return;
  }
  auto fence = angle_function<PFNGLFENCESYNCPROC, "glFenceSync">()(
      GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  if (!fence) return;
  client_wait(fence, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
  angle_function<PFNGLDELETESYNCPROC, "glDeleteSync">()(fence);
}
// VENTURE_GL_PROGRAM_TIMING=1 (off by default): wall time guest threads spend in
// ANGLE's shader compile/link entry points. ANGLE links asynchronously and
// resolves at status queries, so those are timed too. Logged once a second.
const bool program_timing = venture::environment::getenv("VENTURE_GL_PROGRAM_TIMING") != nullptr;
constexpr const char* timed_names[]{"compile", "link", "binary", "getprogramiv", "getshaderiv", "getbinary"};
std::atomic<std::uint64_t> timed_calls[6], timed_ns[6], timed_max_ns[6];
std::atomic<std::int64_t> timing_logged{0};
struct ProgramTimer {
  int slot;
  std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
  ~ProgramTimer() {
    const auto now = std::chrono::steady_clock::now();
    const auto ns = static_cast<std::uint64_t>((now - begin).count());
    timed_calls[slot] += 1;
    timed_ns[slot] += ns;
    for (auto max = timed_max_ns[slot].load(); ns > max && !timed_max_ns[slot].compare_exchange_weak(max, ns);) {}
    const auto second = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    if (auto last = timing_logged.load(); second > last && timing_logged.compare_exchange_strong(last, second)) {
      std::string line;
      for (int i = 0; i < 6; ++i)
        line += std::format(" {}={}/{:.1f}ms/max{:.1f}ms", timed_names[i], timed_calls[i].load(),
                            timed_ns[i].load() / 1e6, timed_max_ns[i].load() / 1e6);
      log::info("gles", "program timing:{}", line);
    }
  }
};
void compile_shader(GLuint shader) {
  static const auto fn = angle_function<PFNGLCOMPILESHADERPROC, "glCompileShader">();
  ProgramTimer timer{0};
  fn(shader);
}
void link_program(GLuint program) {
  static const auto fn = angle_function<PFNGLLINKPROGRAMPROC, "glLinkProgram">();
  ProgramTimer timer{1};
  fn(program);
}
void program_binary(GLuint program, GLenum format, const void* binary, GLsizei length) {
  static const auto fn = angle_function<PFNGLPROGRAMBINARYPROC, "glProgramBinary">();
  ProgramTimer timer{2};
  fn(program, format, binary, length);
}
void get_programiv(GLuint program, GLenum name, GLint* value) {
  static const auto fn = angle_function<PFNGLGETPROGRAMIVPROC, "glGetProgramiv">();
  ProgramTimer timer{3};
  fn(program, name, value);
}
void get_shaderiv(GLuint shader, GLenum name, GLint* value) {
  static const auto fn = angle_function<PFNGLGETSHADERIVPROC, "glGetShaderiv">();
  ProgramTimer timer{4};
  fn(shader, name, value);
}
void get_program_binary(GLuint program, GLsizei size, GLsizei* length, GLenum* format, void* binary) {
  static const auto fn = angle_function<PFNGLGETPROGRAMBINARYPROC, "glGetProgramBinary">();
  ProgramTimer timer{5};
  fn(program, size, length, format, binary);
}
}  // namespace
const GLubyte* gles_host_string(GLenum name) {
  return get_string(name);
}
GLenum gles_host_error() { return get_error(); }
void gles_context_created(EGLContext context, EGLContext share, bool no_error) {
  std::lock_guard lock(context_lock);
  auto state = std::make_shared<GlContextState>();
  state->no_error = no_error;
  if (auto it = states.find(share); it != states.end())
    state->textures = it->second->textures;
  auto it = states.find(share);
  state->buffers32 = it == states.end() ? std::make_shared<BufferGroup32>() : it->second->buffers32;
  states[context] = std::move(state);
}
void gles_context_destroyed(EGLContext context) {
  std::lock_guard lock(context_lock);
  auto it = states.find(context);
  if (it != states.end()) {
    retired_callbacks.push_back(it->second->callback);
    states.erase(it);
  }
}
void gles_context_bound(EGLContext context) {
  std::lock_guard lock(context_lock);
  auto it = states.find(context);
  current = it == states.end() ? nullptr : it->second;
}
std::shared_ptr<GlContextState> current_gl_state() {
  if (current && !current->extensions_ready) build_extensions(*current);
  return current;
}
GlContextState* current_gl_dispatch_state() noexcept { return current.get(); }
void begin_gl_render_timing() {
  static const bool enabled = [] {
    const auto* path = venture::environment::getenv("VENTURE_VULKAN_GPU_TIMING");
    return path && *path;
  }();
  if (!enabled || !current || current->active_render_timer >= 0) return;
  auto state = current_gl_state();
  const auto* extensions = angle_function<PFNGLGETSTRINGPROC, "glGetString">()(GL_EXTENSIONS);
  if (!extensions || !has_extension(reinterpret_cast<const char*>(extensions),
                                    "GL_EXT_disjoint_timer_query")) return;
  for (std::size_t i = 0; i < state->render_timers.size(); ++i) {
    auto& timer = state->render_timers[i];
    if (timer.pending) continue;
    if (!timer.queries[0])
      angle_function<PFNGLGENQUERIESEXTPROC, "glGenQueriesEXT">()(2, timer.queries);
    timer.invalid = false;
    angle_function<PFNGLQUERYCOUNTEREXTPROC, "glQueryCounterEXT">()(
        timer.queries[0], GL_TIMESTAMP_EXT);
    state->active_render_timer = int(i);
    return;
  }
  // A saturated ring is missing coverage, never a reason to stall the game.
}
std::vector<GlRenderTiming> end_gl_render_timing(std::uint64_t frame) {
  std::vector<GlRenderTiming> samples;
  if (!current) return samples;
  auto& state = *current;
  const bool allocated = std::any_of(state.render_timers.begin(), state.render_timers.end(),
                                    [](const auto& timer) { return timer.queries[0] != 0; });
  if (!allocated) return samples;
  GLint disjoint = 0;
  angle_function<PFNGLGETINTEGERVPROC, "glGetIntegerv">()(GL_GPU_DISJOINT_EXT, &disjoint);
  if (disjoint)
    for (auto& timer : state.render_timers) timer.invalid = true;
  auto get = angle_function<PFNGLGETQUERYOBJECTUI64VEXTPROC, "glGetQueryObjectui64vEXT">();
  for (auto& timer : state.render_timers) {
    if (!timer.pending) continue;
    GLuint64 begin_ready = 0, end_ready = 0;
    get(timer.queries[0], GL_QUERY_RESULT_AVAILABLE_EXT, &begin_ready);
    get(timer.queries[1], GL_QUERY_RESULT_AVAILABLE_EXT, &end_ready);
    if (!begin_ready || !end_ready) continue;
    GLuint64 begin = 0, end = 0;
    get(timer.queries[0], GL_QUERY_RESULT_EXT, &begin);
    get(timer.queries[1], GL_QUERY_RESULT_EXT, &end);
    samples.push_back({timer.frame, begin, end, !timer.invalid && end >= begin});
    timer.pending = false;
  }
  if (state.active_render_timer >= 0) {
    auto& timer = state.render_timers[std::size_t(state.active_render_timer)];
    angle_function<PFNGLQUERYCOUNTEREXTPROC, "glQueryCounterEXT">()(
        timer.queries[1], GL_TIMESTAMP_EXT);
    timer.frame = frame;
    timer.pending = true;
    state.active_render_timer = -1;
  }
  return samples;
}
bool gl_dispatch_needs_run_loop() {
  return current && (!current->extensions_ready ||
      current->callback->guest_calls.load(std::memory_order_acquire));
}
void gl_error(GLenum error) {
  (current ? current->errors : no_context_errors).insert(error);
  log::debug("gles", "GL error 0x{:x}", error);
}
void preserve_gl_errors() {
  auto get = angle_function<PFNGLGETERRORPROC, "glGetError">();
  for (GLenum error; (error = get()) != GL_NO_ERROR;) gl_error(error);
}
bool capture_gl_errors() {
  bool ok = true;
  auto get = angle_function<PFNGLGETERRORPROC, "glGetError">();
  for (GLenum error; (error = get()) != GL_NO_ERROR;) {
    gl_error(error);
    ok = false;
  }
  return ok;
}
hle::Handler special_gl_handler(std::string_view name) {
  if (name == "glLabelObjectEXT") return wrap<&label_object>();
  if (name == "glGetObjectLabelEXT") return wrap<&get_object_label>();
  if (name == "glGetString")
    return process_config().isa() == GuestIsa::a32 ? wrap<&get_string32>() : wrap<&get_string>();
  if (name == "glGetStringi")
    return process_config().isa() == GuestIsa::a32 ? wrap<&get_string_i32>() : wrap<&get_string_i>();
  if (name == "glGetIntegerv") return wrap<&get_integer>();
  if (name == "glGetInteger64v") return wrap<&get_integer64>();
  if (name == "glGetFloatv") return wrap<&get_float>();
  if (name == "glGetBooleanv") return wrap<&get_boolean>();
  if (name == "glGetError") return wrap<&get_error>();
  if (auto handler = buffer_gl_handler(name)) return handler;
  if (name == "glDebugMessageCallback" || name == "glDebugMessageCallbackKHR")
    return wrap<&set_debug_callback>();
  if (name == "glGetPointerv" || name == "glGetPointervKHR")
    return wrap<&get_pointer>();
  if (name == "glClientWaitSync") return wrap<&client_wait>();
  if (name == "glFinish") return wrap<&finish>();
  if (program_timing) {
    if (name == "glCompileShader") return wrap<&compile_shader>();
    if (name == "glLinkProgram") return wrap<&link_program>();
    if (name == "glProgramBinary" || name == "glProgramBinaryOES") return wrap<&program_binary>();
    if (name == "glGetProgramiv") return wrap<&get_programiv>();
    if (name == "glGetShaderiv") return wrap<&get_shaderiv>();
    if (name == "glGetProgramBinary" || name == "glGetProgramBinaryOES") return wrap<&get_program_binary>();
  }
  return texture_gl_handler(name);
}
namespace {
void get_pointer32(GLenum name, std::uint32_t* value) {
  if (!value) return;
  void* host = nullptr;
  get_pointer(name, &host);
  if (h2g(host) > UINT32_MAX) throw hle::a32::AbiError{err::kEOVERFLOW};
  auto word = h2g32(host);
  hle::a32::write_bytes(h2g(value), &word, 4);
}
}
void register_gles32_semantics() {
  for (auto library : {"libGLESv2.so", "libGLESv3.so"}) {
    hle::a32::register_adapter(library, "glDebugMessageCallback", a32::wrap<&set_debug_callback>());
    hle::a32::register_adapter(library, "glDebugMessageCallbackKHR", a32::wrap<&set_debug_callback>());
    hle::a32::register_adapter(library, "glGetPointerv", a32::wrap<&get_pointer32>());
    hle::a32::register_adapter(library, "glGetPointervKHR", a32::wrap<&get_pointer32>());
    hle::a32::register_adapter(library, "glClientWaitSync", a32::wrap<&client_wait>());
  }
}
}  // namespace venture::gfx
