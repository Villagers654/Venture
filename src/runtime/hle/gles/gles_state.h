#pragma once
#include <atomic>
#include <array>
#include <deque>
#include <unordered_map>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "gfx/angle.h"
#include "gfx/dispatch.h"
#include "core/guest_storage.h"
namespace venture::gfx {
struct TextureLevel {
  GLenum format{};
  GLint width{}, height{}, depth{};
};
struct TextureJob;
// An ASTC upload whose decode runs in the background. Its GL upload waits until
// a draw may sample the texture, the texture is otherwise used, or the decoded
// data is ready and the render thread has time (textures.cpp).
struct PendingUpload {
  GLuint texture{};
  GLenum target{}, upload_format{};
  GLint level{}, x{}, y{}, z{};
  GLsizei width{}, height{}, depth{};
  bool three = false, blocks = false;
  std::shared_ptr<TextureJob> job;
};
struct TextureGroup {
  std::recursive_mutex lock;
  std::map<std::tuple<GLuint, GLenum, GLint>, TextureLevel> levels;
  std::uint64_t generation = 0;
  // In call order; uploads of one texture are applied in this order.
  std::deque<PendingUpload> pending;
  std::unordered_map<GLuint, unsigned> pending_textures;
  std::size_t pending_bytes = 0;
  std::atomic<std::size_t> pending_count{0};
};
struct GlCallback {
  std::mutex lock;
  GuestAddr function = 0, user = 0;
  // Dispatch admission only; the callback payload retains its existing lock.
  std::atomic<bool> guest_calls{false};
};
struct BufferMapping32 {
  void* host = nullptr;
  std::int64_t offset = 0, length = 0, size = 0;
  GLenum usage = GL_STATIC_DRAW;
  bool emulated = false;
  GLbitfield access = 0;
  std::unique_ptr<GuestStorage> shadow;
  std::uint64_t capacity = 0;
};
struct BufferDataState {
  std::int64_t size = -1;
  GLenum usage = GL_STATIC_DRAW;
  // OES write maps must preserve unwritten bytes. Only CPU-known contents
  // can seed a shadow; any binding through which the GPU can write opts out.
  bool gpu_writable = false, contents_known = false;
  std::vector<std::byte> contents;
};
struct BufferTextureState {
  GLuint buffer = 0;
  // Sticky until texture deletion, including bindings in another shared context.
  bool writable_image = false;
};
struct BufferGroup32 {
  std::recursive_mutex lock;
  std::map<GLuint, std::shared_ptr<BufferMapping32>> mappings;
  std::map<GLuint, BufferDataState> data;
  std::map<GLuint, BufferTextureState> buffer_textures;
  // A deleted name can denote distinct stores retained by shared contexts.
  // Name-indexed CPU mirrors cannot distinguish those stores safely.
  std::set<GLuint> retired_buffer_names;
  // Retained textures can still have writable image bindings after deletion.
  std::set<GLuint> retired_buffer_texture_names;
  std::atomic<unsigned> emulated_maps{0};
  static constexpr std::uint64_t mirror_limit = 256ull << 20;
  std::uint64_t mirrored_bytes = 0;
  // Shadows of unmapped buffers by capacity, reused by later mappings: each
  // new low mapping costs an address-space allocation and release.
  std::multimap<std::uint64_t, std::unique_ptr<GuestStorage>> spare_shadows;
  std::uint64_t spare_bytes = 0;
  std::set<GLsync> syncs;
  ~BufferGroup32();
};
struct GlContextState {
  struct RenderTimer {
    GLuint queries[2]{};
    std::uint64_t frame = 0;
    bool pending = false, invalid = false;
  };
  std::array<RenderTimer, 32> render_timers{};
  // Textures bound per unit to the targets ASTC fallback textures can have
  // (2D, cube map, 2D array, cube map array), for pending uploads.
  std::vector<std::array<GLuint, 4>> texture_units;
  GLuint active_texture_unit = 0;
  GLint texture_unit_count = -1;
  int active_render_timer = -1;
  std::shared_ptr<TextureGroup> textures = std::make_shared<TextureGroup>();
  std::vector<std::string> extensions;
  std::string extension_string;
  std::string version_string;  // GL_VERSION with the host token
  bool extensions_ready = false, astc_fallback = false;
  // ASTC fallback levels stored as BC7 (EXT_texture_compression_bptc) rather
  // than RGBA8: a quarter of the GPU memory.
  bool bc7_fallback = false;
  int major_version = 2;
  std::map<GLuint, bool> framebuffer_cache;
  std::uint64_t framebuffer_generation = 0;
  std::set<GLenum> errors;
  std::shared_ptr<GlCallback> callback = std::make_shared<GlCallback>();
  std::shared_ptr<BufferGroup32> buffers32;
  // EGL_KHR_create_context_no_error. ANGLE keeps validating; the guest sees
  // the KHR_no_error contract (glGetError in gles.cpp, maps in buffers.cpp).
  bool no_error = false;
  // Storage returned by maps that failed host validation in a no-error
  // context, by target, until the guest unmaps that target.
  std::map<GLenum, std::unique_ptr<GuestStorage>> discarded_maps;
};
std::shared_ptr<GlContextState> current_gl_state();
// Borrowed owner-thread state, retained by the bound context. Does not initialize
// extensions; dispatch admission must first check gl_dispatch_needs_run_loop().
GlContextState* current_gl_dispatch_state() noexcept;
struct GlRenderTiming {
  std::uint64_t frame, begin_ns, end_ns;
  bool valid;
};
// Diagnostic only: first guest draw/clear/blit through the XR handoff boundary.
// Includes guest offscreen passes and GPU queue starvation between commands;
// excludes Venture's handoff copies. Collection never waits for a result.
void begin_gl_render_timing();
std::vector<GlRenderTiming> end_gl_render_timing(std::uint64_t frame);
// Dispatch-only check; does not initialize extensions or acquire shared ownership.
bool gl_dispatch_needs_run_loop();
// Declines without waiting if validation could enter a guest-storage owner.
bool try_gl_draw_dispatch(hle::CallContext& ctx);
// ANGLE's GL_VERSION with a token hashed from ANGLE's real GL_RENDERER appended
// to its vendor-specific information. GL_RENDERER reports the emulated headset,
// so engines that key program-binary caches on GL_VERSION and GL_RENDERER (UE4)
// would otherwise reuse binaries ANGLE rejects after a host GPU or driver change.
std::string host_gl_version(std::string_view version, std::string_view renderer);
void gl_error(GLenum error);
// glShaderSource and glCreateShaderProgramv with integer varyings made flat.
void guest_shader_source(GLuint shader, GLsizei count, const GLchar* const* strings,
                         const GLint* lengths);
GLuint guest_shader_program(GLenum type, GLsizei count, const GLchar* const* strings);
void preserve_gl_errors();
bool capture_gl_errors();
hle::Handler texture_gl_handler(std::string_view name);
// Applies the texture's pending ASTC uploads (textures.cpp) in the current context.
void settle_texture_uploads(GLuint texture);
std::vector<GLint> guest_compressed_formats();
// Quest 3's Adreno 740 reports 4 for GL_MAX_SAMPLES and the other sample
// limits. Engines pick their MSAA as min(setting, limit); desktop ANGLE's 8 (or
// more) doubled eye-buffer memory past what the driver allocates at PCVR sizes
// (Bait!: 8x multiview depth at 4016x4016, out of memory, device lost).
// Diagnostic: VENTURE_GLES_MAX_SAMPLES raises the limit (device-lost tests).
GLint guest_max_samples();
std::shared_ptr<BufferGroup32> buffer_group();
void* map_buffer(GLenum target, std::int64_t offset, std::int64_t length, GLbitfield access);
void* map_buffer_oes(GLenum target, GLenum access);
void flush_buffer(GLenum target, std::int64_t offset, std::int64_t length);
GLboolean unmap_buffer(GLenum target);
void buffer_pointer32(GLenum target, GLenum pname, std::uint32_t* out);
void delete_buffers(GLsizei count, const GLuint* names);
void delete_buffer_textures(GLsizei count, const GLuint* names);
bool materialize_buffer_maps();
hle::Handler buffer_gl_handler(std::string_view name);
void register_buffer32_adapters();
void register_gles32_adapters();
void register_gles32_semantics();
}  // namespace venture::gfx
