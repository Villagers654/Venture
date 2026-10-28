#include "hle/gles/gles_state.h"
#include "common/log.h"
#include "core/process.h"
#include "gfx/a32_handles.h"
#include <atomic>
#include <cstring>
#include <algorithm>
#include <limits>

namespace venture::gfx {
using hle::a32::write_bytes;
GLuint bound_buffer(GLenum target) {
  GLenum binding;
  switch (target) {
    case GL_ARRAY_BUFFER: binding = GL_ARRAY_BUFFER_BINDING; break;
    case GL_ELEMENT_ARRAY_BUFFER: binding = GL_ELEMENT_ARRAY_BUFFER_BINDING; break;
    case GL_COPY_READ_BUFFER: binding = GL_COPY_READ_BUFFER_BINDING; break;
    case GL_COPY_WRITE_BUFFER: binding = GL_COPY_WRITE_BUFFER_BINDING; break;
    case GL_PIXEL_PACK_BUFFER: binding = GL_PIXEL_PACK_BUFFER_BINDING; break;
    case GL_PIXEL_UNPACK_BUFFER: binding = GL_PIXEL_UNPACK_BUFFER_BINDING; break;
    case GL_UNIFORM_BUFFER: binding = GL_UNIFORM_BUFFER_BINDING; break;
    case GL_TRANSFORM_FEEDBACK_BUFFER: binding = GL_TRANSFORM_FEEDBACK_BUFFER_BINDING; break;
    case GL_SHADER_STORAGE_BUFFER: binding = GL_SHADER_STORAGE_BUFFER_BINDING; break;
    case GL_ATOMIC_COUNTER_BUFFER: binding = GL_ATOMIC_COUNTER_BUFFER_BINDING; break;
    case GL_DRAW_INDIRECT_BUFFER: binding = GL_DRAW_INDIRECT_BUFFER_BINDING; break;
    case GL_DISPATCH_INDIRECT_BUFFER: binding = GL_DISPATCH_INDIRECT_BUFFER_BINDING; break;
    case GL_TEXTURE_BUFFER: binding = GL_TEXTURE_BUFFER_BINDING; break;
    default: return 0;
  }
  GLint name = 0;
  angle_function<PFNGLGETINTEGERVPROC, "glGetIntegerv">()(binding, &name);
  return static_cast<GLuint>(name);
}
constexpr std::uint64_t spare_shadow_limit = 32ull << 20;
// The smallest spare shadow holding `length` bytes without wasting more than
// half of it, or a new one. Returns whether it was reused (old contents remain).
bool take_shadow(BufferGroup32& buffers, BufferMapping32& mapping, std::uint64_t length) {
  auto it = buffers.spare_shadows.lower_bound(length);
  if (length && it != buffers.spare_shadows.end() && it->first <= 2 * align_up(length, 4096)) {
    mapping.capacity = it->first;
    mapping.shadow = std::move(it->second);
    buffers.spare_bytes -= it->first;
    buffers.spare_shadows.erase(it);
    return true;
  }
  mapping.shadow = std::make_unique<GuestStorage>();
  mapping.shadow->resize(length);
  mapping.capacity = align_up(length, 4096);
  return false;
}
void keep_shadow(BufferGroup32& buffers, BufferMapping32& mapping) {
  if (!mapping.capacity || buffers.spare_bytes + mapping.capacity > spare_shadow_limit) return;
  try {
    buffers.spare_shadows.emplace(mapping.capacity, std::move(mapping.shadow));
    buffers.spare_bytes += mapping.capacity;
  } catch (const std::bad_alloc&) {} // Reuse is optional; the map has already ended.
}
std::shared_ptr<BufferGroup32> buffer_group() {
  auto state = current_gl_state();
  if (!state) return {};
  return state->buffers32;
}
namespace {
// All callers hold the share group's lock. Account allocated capacity, and
// release invalid mirrors instead of retaining their vector allocations.
void forget_contents(BufferGroup32 &buffers, BufferDataState &data) {
  buffers.mirrored_bytes -= data.contents.capacity();
  std::vector<std::byte>().swap(data.contents);
  data.contents_known = false;
}
bool resize_contents(BufferGroup32 &buffers, BufferDataState &data, std::uint64_t size) {
  if (size == data.contents.size()) return true;
  auto available = buffers.mirror_limit - buffers.mirrored_bytes + data.contents.capacity();
  forget_contents(buffers, data);
  if (!size || size > available) return false;
  try {
    std::vector<std::byte> contents(size);
    if (contents.capacity() > available) return false;
    buffers.mirrored_bytes += contents.capacity();
    data.contents = std::move(contents);
    return true;
  } catch (const std::exception&) { return false; }
}
constexpr GLbitfield shadow_access = GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT |
    GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_UNSYNCHRONIZED_BIT;
constexpr GLbitfield invalidate_access = GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT;
bool buffer_state(GLenum target, BufferDataState &data) {
  auto state = current_gl_state();
  if (!state || state->major_version < 3) return false;
  if (data.size < 0) {
    GLint64 size = -1;
    angle_function<PFNGLGETBUFFERPARAMETERI64VPROC, "glGetBufferParameteri64v">()(
        target, GL_BUFFER_SIZE, &size);
    if (size < 0) return false;
    GLint usage = 0;
    angle_function<PFNGLGETBUFFERPARAMETERIVPROC, "glGetBufferParameteriv">()(
        target, GL_BUFFER_USAGE, &usage);
    data.size = size; data.usage = usage;
  }
  // Immutable buffers may prohibit BufferSubData even though mapping is legal.
  if (has_extension(state->extension_string.c_str(), "GL_EXT_buffer_storage")) {
    GLint immutable = 0;
    angle_function<PFNGLGETBUFFERPARAMETERIVPROC, "glGetBufferParameteriv">()(
        target, GL_BUFFER_IMMUTABLE_STORAGE_EXT, &immutable);
    if (immutable) return false;
  }
  return true;
}
}
static void* map_buffer_impl(GLenum target, std::int64_t offset, std::int64_t length,
                             GLbitfield access, bool oes) {
  auto native_map = [oes](GLenum target, std::int64_t offset, std::int64_t length, GLbitfield access) {
    if (oes) return angle_function<PFNGLMAPBUFFEROESPROC, "glMapBufferOES">()(target, GL_WRITE_ONLY_OES);
    return angle_function<PFNGLMAPBUFFERRANGEPROC, "glMapBufferRange">()(target, offset, length, access);
  };
  auto buffers = buffer_group();
  auto name = buffers ? bound_buffer(target) : 0;
  if (!name) return native_map(target, offset, length, access);
  std::lock_guard lock(buffers->lock);
  if (buffers->mappings.contains(name)) { gl_error(GL_INVALID_OPERATION); return nullptr; }
  auto &data = buffers->data[name];
  if (buffers->retired_buffer_names.contains(name)) {
    data.gpu_writable = true;
    forget_contents(*buffers, data);
    data.size = -1;
  }
  bool emulate = (access & GL_MAP_WRITE_BIT) && !(access & ~shadow_access) &&
      offset >= 0 && length > 0 && buffer_state(target, data) &&
      offset <= data.size && length <= data.size - offset;
  GLint mapped = GL_FALSE;
  if (emulate) {
    angle_function<PFNGLGETBUFFERPARAMETERIVPROC, "glGetBufferParameteriv">()(
        target, GL_BUFFER_MAPPED, &mapped);
    emulate = !mapped && !data.gpu_writable && ((access & invalidate_access) ||
        (data.contents_known && data.contents.size() == std::uint64_t(data.size)));
  }
  void *host = nullptr;
  if (!emulate) {
    preserve_gl_errors();
    host = native_map(target, offset, length, access);
    if (!capture_gl_errors() || !host) return nullptr;
    data.contents_known = false;
  }
  std::shared_ptr<BufferMapping32> mapping;
  try { mapping = std::make_shared<BufferMapping32>(); }
  catch (const std::bad_alloc&) {
    if (host && process_config().isa() != GuestIsa::a32) return host;
    if (host) angle_function<PFNGLUNMAPBUFFERPROC, "glUnmapBuffer">()(target);
    gl_error(GL_OUT_OF_MEMORY); return nullptr;
  }
  mapping->host = host; mapping->offset = offset; mapping->length = length;
  mapping->access = access; mapping->emulated = emulate;
  mapping->size = data.size; mapping->usage = data.usage;
  if (!emulate && process_config().isa() != GuestIsa::a32) {
    buffers->mappings.emplace(name, std::move(mapping));
    return host;
  }
  bool reused = false;
  try {
    reused = take_shadow(*buffers, *mapping, static_cast<std::uint64_t>(length));
  } catch (const std::exception&) {
    if (host) angle_function<PFNGLUNMAPBUFFERPROC, "glUnmapBuffer">()(target);
    gl_error(GL_OUT_OF_MEMORY); return nullptr;
  }
  auto* guest = g2h(mapping->shadow->address());
  // OES_mapbuffer explicitly preserves unwritten bytes. For ordinary write
  // maps, only a known CPU copy can avoid obtaining the original contents.
  if (emulate && !(access & invalidate_access))
    std::memcpy(guest, data.contents.data() + offset, static_cast<std::size_t>(length));
  else if (host && ((access & GL_MAP_READ_BIT) || !(access & invalidate_access)))
    std::memcpy(guest, host, static_cast<std::size_t>(length));
  else if (reused) std::memset(guest, 0, static_cast<std::size_t>(length));
  buffers->mappings.emplace(name, std::move(mapping));
  if (emulate) buffers->emulated_maps.fetch_add(1, std::memory_order_relaxed);
  return guest;
}
namespace {
// KHR_no_error: in a guest no-error context errors are undefined behaviour,
// and engines built for no-error drivers write through the map pointer
// unconditionally (Unity clears a pooled buffer that was never allocated,
// with 0 bound). ANGLE keeps validating, so a map it rejects for a reason
// other than OUT_OF_MEMORY returns guest storage that unmap discards.
void* no_error_map(GLenum target, std::int64_t length, void* result) {
  auto state = current_gl_state();
  if (!state || !state->no_error) return result;
  auto& discarded = state->discarded_maps;
  if (result) {
    if (!discarded.empty()) discarded.erase(target);
    return result;
  }
  preserve_gl_errors();
  if (state->errors.contains(GL_OUT_OF_MEMORY) || length <= 0) return nullptr;
  try {
    auto& storage = discarded[target];
    if (!storage) storage = std::make_unique<GuestStorage>();
    auto* data = g2h(storage->resize(static_cast<std::uint64_t>(length)));
    std::memset(data, 0, static_cast<std::size_t>(length));
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true))
      log::warn("gles", "No-error context: host rejected glMapBufferRange target={:#x} length={}; writes are discarded",
                target, length);
    return data;
  } catch (const std::exception&) {
    discarded.erase(target);
    return nullptr;
  }
}
bool unmap_discarded(GLenum target) {
  auto state = current_gl_state();
  return state && !state->discarded_maps.empty() && state->discarded_maps.erase(target) != 0;
}
}
void* map_buffer(GLenum target, std::int64_t offset, std::int64_t length, GLbitfield access) {
  return no_error_map(target, length, map_buffer_impl(target, offset, length, access, false));
}
void* map_buffer_oes(GLenum target, GLenum access) {
  if (access != GL_WRITE_ONLY_OES)
    return angle_function<PFNGLMAPBUFFEROESPROC, "glMapBufferOES">()(target, access);
  if (auto buffers = buffer_group()) {
    auto name = bound_buffer(target);
    std::lock_guard lock(buffers->lock);
    auto it = buffers->data.find(name);
    if (!buffers->retired_buffer_names.contains(name) &&
        it != buffers->data.end() && it->second.size >= 0)
      return map_buffer_impl(target, 0, it->second.size, GL_MAP_WRITE_BIT, true);
  }
  GLint64 length = -1;
  if (auto state = current_gl_state(); state && state->major_version >= 3) {
    preserve_gl_errors();
    angle_function<PFNGLGETBUFFERPARAMETERI64VPROC, "glGetBufferParameteri64v">()(
        target, GL_BUFFER_SIZE, &length);
    if (!capture_gl_errors()) return nullptr;
  } else {
    GLint size = 0;
    preserve_gl_errors();
    angle_function<PFNGLGETBUFFERPARAMETERIVPROC, "glGetBufferParameteriv">()(
        target, GL_BUFFER_SIZE, &size);
    if (!capture_gl_errors()) return nullptr;
    length = size;
  }
  return map_buffer_impl(target, 0, length, GL_MAP_WRITE_BIT, true);
}
void flush_buffer(GLenum target, std::int64_t offset, std::int64_t length) {
  auto buffers = buffer_group();
  auto name = buffers ? bound_buffer(target) : 0;
  if (!name) {
    angle_function<PFNGLFLUSHMAPPEDBUFFERRANGEPROC, "glFlushMappedBufferRange">()(target, offset, length);
    return;
  }
  std::lock_guard lock(buffers->lock);
  auto it = buffers->mappings.find(name);
  if (it == buffers->mappings.end()) {
    angle_function<PFNGLFLUSHMAPPEDBUFFERRANGEPROC, "glFlushMappedBufferRange">()(target, offset, length);
    return;
  }
  if (!(it->second->access & GL_MAP_FLUSH_EXPLICIT_BIT)) {
    gl_error(GL_INVALID_OPERATION); return;
  }
  auto& m = *it->second;
  if (offset < 0 || length < 0 || offset > m.length || length > m.length - offset) {
    gl_error(GL_INVALID_VALUE); return;
  }
  if (!m.shadow) {
    angle_function<PFNGLFLUSHMAPPEDBUFFERRANGEPROC, "glFlushMappedBufferRange">()(target, offset, length);
    return;
  }
  // EXT_map_buffer_range: flush offsets are relative to the mapped range;
  // only explicitly flushed writes propagate when FLUSH_EXPLICIT is set.
  if (length) std::memcpy(static_cast<std::byte*>(m.host) + offset, g2h(m.shadow->address() + offset), static_cast<std::size_t>(length));
  angle_function<PFNGLFLUSHMAPPEDBUFFERRANGEPROC, "glFlushMappedBufferRange">()(target, offset, length);
}
GLboolean unmap_buffer(GLenum target) {
  if (unmap_discarded(target)) return GL_TRUE;
  auto native_unmap = angle_function<PFNGLUNMAPBUFFERPROC, "glUnmapBuffer">();
  auto buffers = buffer_group();
  if (!buffers) return native_unmap(target);
  auto name = bound_buffer(target);
  if (!name) return native_unmap(target);
  std::lock_guard lock(buffers->lock);
  auto it = buffers->mappings.find(name);
  if (it == buffers->mappings.end()) return native_unmap(target);
  auto& m = *it->second;
  GLboolean result = GL_TRUE;
  if (m.emulated) {
    preserve_gl_errors();
    if ((m.access & GL_MAP_INVALIDATE_BUFFER_BIT) && !m.offset && m.length == m.size)
      angle_function<PFNGLBUFFERDATAPROC, "glBufferData">()(
          target, m.size, nullptr, m.usage);
    angle_function<PFNGLBUFFERSUBDATAPROC, "glBufferSubData">()(
        target, m.offset, m.length, g2h(m.shadow->address()));
    result = capture_gl_errors() ? GL_TRUE : GL_FALSE;
    auto &data = buffers->data[name];
    if (result && !data.gpu_writable) {
      try {
        if (!m.offset && m.length == m.size) {
          data.contents_known = resize_contents(*buffers, data, m.size);
        } else if (m.access & GL_MAP_INVALIDATE_BUFFER_BIT) {
          forget_contents(*buffers, data);
        }
        if (data.contents_known)
          std::memcpy(data.contents.data() + m.offset, g2h(m.shadow->address()), m.length);
      } catch (const std::bad_alloc&) { forget_contents(*buffers, data); }
    } else { forget_contents(*buffers, data); }
  } else {
    if (m.shadow && (m.access & GL_MAP_WRITE_BIT) && !(m.access & GL_MAP_FLUSH_EXPLICIT_BIT))
      std::memcpy(m.host, g2h(m.shadow->address()), static_cast<std::size_t>(m.length));
    auto &data = buffers->data[name];
    if (m.size < 0) {
      buffer_state(target, data);
      m.size = data.size;
    }
    // The first full native map seeds the mirror, including OES write-only
    // maps whose native staging storage preserves unwritten bytes. Explicit
    // flush maps cannot seed it: unflushed writes do not belong to the store.
    if (!data.gpu_writable && !m.offset && m.length == m.size &&
        !(m.access & GL_MAP_FLUSH_EXPLICIT_BIT) && resize_contents(*buffers, data, m.size)) {
      std::memcpy(data.contents.data(), m.host, m.length);
      data.contents_known = true;
    } else forget_contents(*buffers, data);
    result = native_unmap(target);
    if (!result) { forget_contents(*buffers, data); }
  }
  if (m.emulated) buffers->emulated_maps.fetch_sub(1, std::memory_order_relaxed);
  keep_shadow(*buffers, m);
  buffers->mappings.erase(it);
  return result;
}
void buffer_pointer32(GLenum target, GLenum pname, std::uint32_t* out) {
  if (pname != GL_BUFFER_MAP_POINTER) { gl_error(GL_INVALID_ENUM); return; }
  if (!out) return;
  auto buffers = buffer_group();
  if (!buffers) return;
  auto name = bound_buffer(target);
  if (!name) {
    void *unused = nullptr;
    angle_function<PFNGLGETBUFFERPOINTERVPROC, "glGetBufferPointerv">()(target, pname, &unused);
    return;
  }
  std::lock_guard lock(buffers->lock);
  auto it = buffers->mappings.find(name);
  auto word = it == buffers->mappings.end() ? 0u : static_cast<std::uint32_t>(it->second->shadow->address());
  write_bytes(h2g(out), &word, 4);
}
void delete_buffers(GLsizei count, const GLuint* names) {
  auto buffers = buffer_group();
  if (!buffers || count < 0 || !names) {
    angle_function<PFNGLDELETEBUFFERSPROC, "glDeleteBuffers">()(count, names);
    return;
  }
  std::lock_guard lock(buffers->lock);
  for (GLsizei i = 0; i < count; ++i)
    if (angle_function<PFNGLISBUFFERPROC, "glIsBuffer">()(names[i]))
      buffers->retired_buffer_names.insert(names[i]);
  angle_function<PFNGLDELETEBUFFERSPROC, "glDeleteBuffers">()(count, names);
  for (GLsizei i = 0; i < count; ++i) {
    auto it = buffers->mappings.find(names[i]);
    if (it != buffers->mappings.end()) {
      if (it->second->emulated) buffers->emulated_maps.fetch_sub(1, std::memory_order_relaxed);
      buffers->mappings.erase(it);
    }
    if (auto data = buffers->data.find(names[i]); data != buffers->data.end()) {
      forget_contents(*buffers, data->second);
      buffers->data.erase(data);
    }
  }
}

void delete_buffer_textures(GLsizei count, const GLuint *names) {
  auto buffers = buffer_group();
  std::unique_lock<std::recursive_mutex> lock;
  if (buffers) lock = std::unique_lock(buffers->lock);
  angle_function<PFNGLDELETETEXTURESPROC, "glDeleteTextures">()(count, names);
  if (buffers && count >= 0 && names) {
    for (GLsizei i = 0; i < count; ++i)
      if (buffers->buffer_textures.erase(names[i]))
        buffers->retired_buffer_texture_names.insert(names[i]);
  }
}
bool materialize_buffer_maps() {
  auto buffers = buffer_group();
  if (!buffers || !buffers->emulated_maps.load(std::memory_order_relaxed)) return true;
  std::lock_guard lock(buffers->lock);
  if (!buffers->emulated_maps.load(std::memory_order_relaxed)) return true;
  // GPU commands must still see ANGLE's mapped-buffer validation. Normal
  // map/write/unmap sequences never take this path. A command issued while
  // a shadow is mapped lazily installs its native map, keeping the guest
  // pointer and its writes until unmap, and restoring the context's binding.
  GLint previous = 0;
  angle_function<PFNGLGETINTEGERVPROC, "glGetIntegerv">()(GL_COPY_WRITE_BUFFER_BINDING, &previous);
  auto bind = angle_function<PFNGLBINDBUFFERPROC, "glBindBuffer">();
  auto map = angle_function<PFNGLMAPBUFFERRANGEPROC, "glMapBufferRange">();
  preserve_gl_errors();
  bool ok = true;
  for (auto &[name, mapping] : buffers->mappings) {
    auto &m = *mapping;
    if (!m.emulated) continue;
    bind(GL_COPY_WRITE_BUFFER, name);
    auto *host = map(GL_COPY_WRITE_BUFFER, m.offset, m.length, m.access);
    if (!capture_gl_errors() || !host) { ok = false; break; }
    m.host = host; m.emulated = false;
    buffers->emulated_maps.fetch_sub(1, std::memory_order_relaxed);
    auto &data = buffers->data[name]; forget_contents(*buffers, data);
  }
  bind(GL_COPY_WRITE_BUFFER, previous);
  return ok;
}
namespace {
void buffer_data(GLenum target, std::int64_t size, const void *input, GLenum usage) {
  auto buffers = buffer_group();
  auto name = buffers ? bound_buffer(target) : 0;
  std::unique_lock<std::recursive_mutex> lock;
  if (buffers) lock = std::unique_lock(buffers->lock);
  // NULL data has undefined contents. Choose an explicit initial value for
  // previously mirrored small stores, so OES maps preserve unwritten bytes without
  // asking ANGLE to copy the device-local allocation back to a staging map.
  if (name && !input && size > 0 && size <= spare_shadow_limit &&
      !buffers->data[name].gpu_writable && !buffers->data[name].contents.empty() &&
      std::uint64_t(size) <= buffers->mirror_limit - buffers->mirrored_bytes +
          buffers->data[name].contents.capacity()) {
    auto &data = buffers->data[name];
    // Reuse the mirror instead of allocating a second buffer on each orphan.
    // Invalidate it before changing bytes: if ANGLE rejects BufferData, the
    // original store remains intact and the next map must obtain its contents.
    data.contents_known = false;
    if (resize_contents(*buffers, data, size)) {
      std::fill(data.contents.begin(), data.contents.end(), std::byte{});
      input = data.contents.data();
    }
  }
  preserve_gl_errors();
  angle_function<PFNGLBUFFERDATAPROC, "glBufferData">()(target, size, input, usage);
  bool ok = capture_gl_errors();
  if (!buffers || !name) return;
  auto &data = buffers->data[name];
  data.size = -1; // Query the actual store size on the next eligible map.
  if (!ok) return;
  if (auto it = buffers->mappings.find(name); it != buffers->mappings.end()) {
    if (it->second->emulated) buffers->emulated_maps.fetch_sub(1, std::memory_order_relaxed);
    keep_shadow(*buffers, *it->second); buffers->mappings.erase(it);
  }
  // Static uploads never allocate a mirror. Only buffers whose successful
  // maps already seeded one pay for keeping it in sync across reallocation.
  bool mirrored = !data.contents.empty();
  data.contents_known = false;
  if (mirrored && !data.gpu_writable && size > 0 && input &&
      resize_contents(*buffers, data, size)) {
    if (input != data.contents.data()) std::memcpy(data.contents.data(), input, size);
    data.contents_known = true;
  } else forget_contents(*buffers, data);
}
void buffer_sub_data(GLenum target, std::int64_t offset, std::int64_t length, const void *input) {
  auto buffers = buffer_group();
  auto name = buffers ? bound_buffer(target) : 0;
  std::unique_lock<std::recursive_mutex> lock;
  if (buffers) lock = std::unique_lock(buffers->lock);
  if (buffers && name) {
    auto it = buffers->mappings.find(name);
    if (it != buffers->mappings.end() && it->second->emulated) {
      if (offset < 0 || length < 0 || offset > it->second->size || length > it->second->size - offset)
        gl_error(GL_INVALID_VALUE);
      else gl_error(GL_INVALID_OPERATION);
      return;
    }
  }
  preserve_gl_errors();
  angle_function<PFNGLBUFFERSUBDATAPROC, "glBufferSubData">()(target, offset, length, input);
  bool ok = capture_gl_errors();
  if (buffers && name && ok) {
    auto &data = buffers->data[name];
    if (data.contents_known && input && offset >= 0 && length >= 0 &&
        std::uint64_t(offset) <= data.contents.size() &&
        std::uint64_t(length) <= data.contents.size() - offset)
      std::memcpy(data.contents.data() + offset, input, length);
    else if (length) { forget_contents(*buffers, data); }
  }
}
void gpu_writable(GLenum target, GLuint name) {
  if (target != GL_PIXEL_PACK_BUFFER && target != GL_TRANSFORM_FEEDBACK_BUFFER &&
      target != GL_SHADER_STORAGE_BUFFER && target != GL_ATOMIC_COUNTER_BUFFER) return;
  if (!name) return;
  if (auto buffers = buffer_group()) {
    std::lock_guard lock(buffers->lock);
    auto &data = buffers->data[name];
    data.gpu_writable = true; forget_contents(*buffers, data);
  }
}
void texture_buffer_binding(BufferGroup32 &buffers, GLuint buffer) {
  GLint name = 0;
  angle_function<PFNGLGETINTEGERVPROC, "glGetIntegerv">()(GL_TEXTURE_BINDING_BUFFER, &name);
  if (!name) return;
  auto &texture = buffers.buffer_textures[static_cast<GLuint>(name)];
  texture.buffer = buffer;
  if (buffers.retired_buffer_texture_names.contains(static_cast<GLuint>(name)))
    texture.writable_image = true;
  if (buffer && texture.writable_image) {
    auto &data = buffers.data[buffer];
    data.gpu_writable = true; forget_contents(buffers, data);
  }
}
enum class TextureBufferApi { core, ext, oes };
template<TextureBufferApi Api>
void tex_buffer(GLenum target, GLenum format, GLuint buffer) {
  auto buffers = buffer_group();
  std::unique_lock<std::recursive_mutex> lock;
  if (buffers) lock = std::unique_lock(buffers->lock);
  preserve_gl_errors();
  constexpr auto entry = Api == TextureBufferApi::ext ? "glTexBufferEXT" :
      Api == TextureBufferApi::oes ? "glTexBufferOES" : "glTexBuffer";
  angle_function<PFNGLTEXBUFFERPROC>(entry)(target, format, buffer);
  if (capture_gl_errors() && buffers) texture_buffer_binding(*buffers, buffer);
}
template<TextureBufferApi Api>
void tex_buffer_range(GLenum target, GLenum format, GLuint buffer, std::int64_t offset, std::int64_t size) {
  auto buffers = buffer_group();
  std::unique_lock<std::recursive_mutex> lock;
  if (buffers) lock = std::unique_lock(buffers->lock);
  preserve_gl_errors();
  constexpr auto entry = Api == TextureBufferApi::ext ? "glTexBufferRangeEXT" :
      Api == TextureBufferApi::oes ? "glTexBufferRangeOES" : "glTexBufferRange";
  angle_function<PFNGLTEXBUFFERRANGEPROC>(entry)(target, format, buffer, offset, size);
  if (capture_gl_errors() && buffers) texture_buffer_binding(*buffers, buffer);
}
void bind_image_texture(GLuint unit, GLuint texture, GLint level, GLboolean layered,
                        GLint layer, GLenum access, GLenum format) {
  auto buffers = buffer_group();
  std::unique_lock<std::recursive_mutex> lock;
  if (buffers) lock = std::unique_lock(buffers->lock);
  preserve_gl_errors();
  angle_function<PFNGLBINDIMAGETEXTUREPROC, "glBindImageTexture">()(
      unit, texture, level, layered, layer, access, format);
  if (!capture_gl_errors() || !buffers || !texture || access == GL_READ_ONLY) return;
  // Sampling a buffer texture cannot change its storage. Only write-capable
  // image bindings opt it out of the CPU mirror; remember them before an
  // attachment as well, since TexBuffer can replace an image unit's storage.
  auto &image = buffers->buffer_textures[texture];
  image.writable_image = true;
  if (image.buffer) {
    auto &data = buffers->data[image.buffer];
    data.gpu_writable = true; forget_contents(*buffers, data);
  }
}
void bind_buffer(GLenum target, GLuint name) {
  angle_function<PFNGLBINDBUFFERPROC, "glBindBuffer">()(target, name);
  gpu_writable(target, name);
}
void bind_buffer_base(GLenum target, GLuint index, GLuint name) {
  angle_function<PFNGLBINDBUFFERBASEPROC, "glBindBufferBase">()(target, index, name);
  gpu_writable(target, name);
}
void bind_buffer_range(GLenum target, GLuint index, GLuint name, std::int64_t offset, std::int64_t size) {
  angle_function<PFNGLBINDBUFFERRANGEPROC, "glBindBufferRange">()(target, index, name, offset, size);
  gpu_writable(target, name);
}
void copy_buffer(GLenum source, GLenum target, std::int64_t from, std::int64_t to, std::int64_t length) {
  if (auto buffers = buffer_group()) {
    auto name = bound_buffer(target);
    std::lock_guard lock(buffers->lock);
    if (name) { auto &data = buffers->data[name]; forget_contents(*buffers, data); }
    auto read = bound_buffer(source);
    for (auto buffer : {name, read}) {
      auto it = buffers->mappings.find(buffer);
      if (it != buffers->mappings.end() && it->second->emulated) {
        gl_error(GL_INVALID_OPERATION); return;
      }
    }
  }
  angle_function<PFNGLCOPYBUFFERSUBDATAPROC, "glCopyBufferSubData">()(source, target, from, to, length);
}
void buffer_storage(GLenum target, std::int64_t size, const void *input, GLbitfield flags) {
  preserve_gl_errors();
  angle_function<PFNGLBUFFERSTORAGEEXTPROC, "glBufferStorageEXT">()(target, size, input, flags);
  bool ok = capture_gl_errors();
  if (auto buffers = buffer_group()) {
    auto name = bound_buffer(target);
    std::lock_guard lock(buffers->lock);
    if (name) {
      auto &data = buffers->data[name]; data.size = -1;
      if (ok) {
        forget_contents(*buffers, data);
        if (auto it = buffers->mappings.find(name); it != buffers->mappings.end()) {
          if (it->second->emulated) buffers->emulated_maps.fetch_sub(1, std::memory_order_relaxed);
          keep_shadow(*buffers, *it->second); buffers->mappings.erase(it);
        }
      }
    }
  }
}
template<class T> bool mapped_parameter(GLenum target, GLenum pname, T *out) {
  if (!out || (pname != GL_BUFFER_ACCESS_OES && pname != GL_BUFFER_MAPPED && pname != GL_BUFFER_ACCESS_FLAGS &&
      pname != GL_BUFFER_MAP_OFFSET && pname != GL_BUFFER_MAP_LENGTH)) return false;
  auto buffers = buffer_group();
  if (!buffers) return false;
  auto name = bound_buffer(target);
  if (!name) return false;
  std::lock_guard lock(buffers->lock);
  auto it = buffers->mappings.find(name);
  if (it == buffers->mappings.end()) return false;
  const auto &m = *it->second;
  if (pname == GL_BUFFER_ACCESS_OES && !m.emulated) return false;
  switch (pname) {
    case GL_BUFFER_ACCESS_OES: *out = GL_WRITE_ONLY_OES; break;
    case GL_BUFFER_MAPPED: *out = GL_TRUE; break;
    case GL_BUFFER_ACCESS_FLAGS: *out = m.access; break;
    case GL_BUFFER_MAP_OFFSET: *out = static_cast<T>(std::clamp<std::int64_t>(
        m.offset, std::numeric_limits<T>::min(), std::numeric_limits<T>::max())); break;
    case GL_BUFFER_MAP_LENGTH: *out = static_cast<T>(std::clamp<std::int64_t>(
        m.length, std::numeric_limits<T>::min(), std::numeric_limits<T>::max())); break;
  }
  return true;
}
void buffer_parameter(GLenum target, GLenum pname, GLint *out) {
  if (!mapped_parameter(target, pname, out))
    angle_function<PFNGLGETBUFFERPARAMETERIVPROC, "glGetBufferParameteriv">()(target, pname, out);
}
void buffer_parameter64(GLenum target, GLenum pname, GLint64 *out) {
  if (!mapped_parameter(target, pname, out))
    angle_function<PFNGLGETBUFFERPARAMETERI64VPROC, "glGetBufferParameteri64v">()(target, pname, out);
}
void buffer_pointer(GLenum target, GLenum pname, void **out) {
  if (pname == GL_BUFFER_MAP_POINTER && out) {
    if (auto buffers = buffer_group()) {
      auto name = bound_buffer(target);
      std::lock_guard lock(buffers->lock);
      auto it = buffers->mappings.find(name);
      if (it != buffers->mappings.end()) {
        *out = it->second->shadow ? g2h(it->second->shadow->address()) : it->second->host;
        return;
      }
    }
  }
  angle_function<PFNGLGETBUFFERPOINTERVPROC, "glGetBufferPointerv">()(target, pname, out);
}
}
hle::Handler buffer_gl_handler(std::string_view name) {
  if (name == "glMapBufferRange" || name == "glMapBufferRangeEXT") return wrap<&map_buffer>();
  if (name == "glMapBufferOES") return wrap<&map_buffer_oes>();
  if (name == "glUnmapBuffer" || name == "glUnmapBufferOES") return wrap<&unmap_buffer>();
  if (name == "glFlushMappedBufferRange" || name == "glFlushMappedBufferRangeEXT") return wrap<&flush_buffer>();
  if (name == "glGetBufferPointerv" || name == "glGetBufferPointervOES") return wrap<&buffer_pointer>();
  if (name == "glGetBufferParameteriv") return wrap<&buffer_parameter>();
  if (name == "glGetBufferParameteri64v") return wrap<&buffer_parameter64>();
  if (name == "glBufferData") return wrap<&buffer_data>();
  if (name == "glBufferSubData") return wrap<&buffer_sub_data>();
  if (name == "glBufferStorageEXT") return wrap<&buffer_storage>();
  if (name == "glCopyBufferSubData" || name == "glCopyBufferSubDataNV") return wrap<&copy_buffer>();
  if (name == "glTexBuffer") return wrap<&tex_buffer<TextureBufferApi::core>>();
  if (name == "glTexBufferEXT") return wrap<&tex_buffer<TextureBufferApi::ext>>();
  if (name == "glTexBufferOES") return wrap<&tex_buffer<TextureBufferApi::oes>>();
  if (name == "glTexBufferRange") return wrap<&tex_buffer_range<TextureBufferApi::core>>();
  if (name == "glTexBufferRangeEXT") return wrap<&tex_buffer_range<TextureBufferApi::ext>>();
  if (name == "glTexBufferRangeOES") return wrap<&tex_buffer_range<TextureBufferApi::oes>>();
  if (name == "glBindImageTexture") return wrap<&bind_image_texture>();
  if (name == "glBindBuffer") return wrap<&bind_buffer>();
  if (name == "glBindBufferBase") return wrap<&bind_buffer_base>();
  if (name == "glBindBufferRange") return wrap<&bind_buffer_range>();
  if (name == "glDeleteBuffers") return wrap<&delete_buffers>();
  return nullptr;
}
void register_buffer32_adapters() {
  for (auto library : {"libGLESv2.so", "libGLESv3.so"}) {
#define ADAPT(name, fn) hle::a32::register_adapter(library, name, a32::wrap<&fn>())
    ADAPT("glGetBufferParameteriv", buffer_parameter);
    ADAPT("glGetBufferParameteri64v", buffer_parameter64);
    ADAPT("glBufferData", buffer_data);
    ADAPT("glBufferSubData", buffer_sub_data);
    ADAPT("glBufferStorageEXT", buffer_storage);
    ADAPT("glCopyBufferSubData", copy_buffer);
    ADAPT("glCopyBufferSubDataNV", copy_buffer);
    ADAPT("glTexBuffer", tex_buffer<TextureBufferApi::core>);
    ADAPT("glTexBufferEXT", tex_buffer<TextureBufferApi::ext>);
    ADAPT("glTexBufferOES", tex_buffer<TextureBufferApi::oes>);
    ADAPT("glTexBufferRange", tex_buffer_range<TextureBufferApi::core>);
    ADAPT("glTexBufferRangeEXT", tex_buffer_range<TextureBufferApi::ext>);
    ADAPT("glTexBufferRangeOES", tex_buffer_range<TextureBufferApi::oes>);
    ADAPT("glBindImageTexture", bind_image_texture);
    ADAPT("glBindBuffer", bind_buffer);
    ADAPT("glBindBufferBase", bind_buffer_base);
    ADAPT("glBindBufferRange", bind_buffer_range);
#undef ADAPT
  }
}
}
