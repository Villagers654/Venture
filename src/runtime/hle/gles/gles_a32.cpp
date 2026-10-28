#include "hle/gles/gles_state.h"
#include "gfx/a32_handles.h"
#include <cstring>

namespace venture::gfx {
namespace {
using hle::a32::read_bytes;
using hle::a32::write_bytes;
// gl.xml: string/varying arrays contain four-byte guest pointers. The
// strings themselves retain identity; only the pointer array is expanded.
std::vector<const GLchar*> strings32(GLsizei count, const std::uint32_t* input) {
  std::vector<const GLchar*> result(static_cast<std::size_t>(count));
  for (GLsizei i = 0; i < count; ++i) {
    std::uint32_t address;
    read_bytes(h2g(input) + 4ull * i, &address, 4);
    result[i] = g2h<const GLchar>(address);
  }
  return result;
}
void shader_source(GLuint shader, GLsizei count, const std::uint32_t* input, const GLint* lengths) {
  if (count < 0) { gl_error(GL_INVALID_VALUE); return; }
  if (count && !input) { gl_error(GL_INVALID_VALUE); return; }
  auto strings = strings32(count, input);
  angle_function<PFNGLSHADERSOURCEPROC, "glShaderSource">()(shader, count, strings.data(), lengths);
}
void varyings(GLuint program, GLsizei count, const std::uint32_t* input, GLenum mode) {
  if (count < 0) { gl_error(GL_INVALID_VALUE); return; }
  if (count && !input) { gl_error(GL_INVALID_VALUE); return; }
  auto strings = strings32(count, input);
  angle_function<PFNGLTRANSFORMFEEDBACKVARYINGSPROC, "glTransformFeedbackVaryings">()(program, count, strings.data(), mode);
}
void vertex_pointer(GLuint index, GLenum name, std::uint32_t* out) {
  if (!out) return;
  void* pointer = nullptr;
  angle_function<PFNGLGETVERTEXATTRIBPOINTERVPROC, "glGetVertexAttribPointerv">()(index, name, &pointer);
  if (h2g(pointer) > UINT32_MAX) throw hle::a32::AbiError{err::kEOVERFLOW};
  auto word = h2g32(pointer);
  write_bytes(h2g(out), &word, 4);
}
GLsync fence_sync(GLenum condition, GLbitfield flags) {
  auto sync = angle_function<PFNGLFENCESYNCPROC, "glFenceSync">()(condition, flags);
  if (sync) if (auto buffers = buffer_group()) {
    std::lock_guard lock(buffers->lock);
    buffers->syncs.insert(sync);
  }
  return sync;
}
void delete_sync(GLsync sync) {
  angle_function<PFNGLDELETESYNCPROC, "glDeleteSync">()(sync);
  a32::retire(a32::Handle::gl_sync, sync);
  if (auto buffers = buffer_group()) {
    std::lock_guard lock(buffers->lock);
    buffers->syncs.erase(sync);
  }
}
GLboolean is_sync(GLsync sync) { return angle_function<PFNGLISSYNCPROC, "glIsSync">()(sync); }
void wait_sync(GLsync sync, GLbitfield flags, std::uint64_t timeout) {
  angle_function<PFNGLWAITSYNCPROC, "glWaitSync">()(sync, flags, timeout);
}
void get_sync(GLsync sync, GLenum pname, GLsizei size, GLsizei* length, GLint* values) {
  angle_function<PFNGLGETSYNCIVPROC, "glGetSynciv">()(sync, pname, size, length, values);
}
void image_texture(GLenum target, GLeglImageOES image) {
  angle_function<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC, "glEGLImageTargetTexture2DOES">()(target, image);
}
void image_renderbuffer(GLenum target, GLeglImageOES image) {
  angle_function<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC, "glEGLImageTargetRenderbufferStorageOES">()(target, image);
}
void object_ptr_label(GuestAddr token, GLsizei length, const GLchar* label) {
  auto* sync = a32::decode(a32::Handle::gl_sync, static_cast<std::uint32_t>(token));
  angle_function<PFNGLOBJECTPTRLABELPROC, "glObjectPtrLabel">()(sync, length, label);
}
void get_object_ptr_label(GuestAddr token, GLsizei size, GLsizei* length, GLchar* label) {
  auto* sync = a32::decode(a32::Handle::gl_sync, static_cast<std::uint32_t>(token));
  angle_function<PFNGLGETOBJECTPTRLABELPROC, "glGetObjectPtrLabel">()(sync, size, length, label);
}
}
BufferGroup32::~BufferGroup32() {
  // GL_ES 3.0 shared-object lifetime: the last context in the share group
  // releases its sync objects, including a context with deferred destruction.
  for (auto sync : syncs) a32::retire(a32::Handle::gl_sync, sync);
}
void register_gles32_adapters() {
  register_gles32_semantics();
  register_buffer32_adapters();
  for (auto library : {"libGLESv2.so", "libGLESv3.so"}) {
#define ADAPT(name, fn) hle::a32::register_adapter(library, name, a32::wrap<&fn>())
    ADAPT("glShaderSource", shader_source);
    ADAPT("glTransformFeedbackVaryings", varyings);
    ADAPT("glGetVertexAttribPointerv", vertex_pointer);
    ADAPT("glMapBufferRange", map_buffer);
    ADAPT("glMapBufferRangeEXT", map_buffer);
    ADAPT("glMapBufferOES", map_buffer_oes);
    ADAPT("glFlushMappedBufferRange", flush_buffer);
    ADAPT("glFlushMappedBufferRangeEXT", flush_buffer);
    ADAPT("glUnmapBuffer", unmap_buffer);
    ADAPT("glUnmapBufferOES", unmap_buffer);
    ADAPT("glGetBufferPointerv", buffer_pointer32);
    ADAPT("glGetBufferPointervOES", buffer_pointer32);
    ADAPT("glDeleteBuffers", delete_buffers);
    ADAPT("glFenceSync", fence_sync);
    ADAPT("glDeleteSync", delete_sync);
    ADAPT("glIsSync", is_sync);
    ADAPT("glWaitSync", wait_sync);
    ADAPT("glGetSynciv", get_sync);
    ADAPT("glEGLImageTargetTexture2DOES", image_texture);
    ADAPT("glEGLImageTargetRenderbufferStorageOES", image_renderbuffer);
    ADAPT("glObjectPtrLabel", object_ptr_label);
    ADAPT("glObjectPtrLabelKHR", object_ptr_label);
    ADAPT("glGetObjectPtrLabel", get_object_ptr_label);
    ADAPT("glGetObjectPtrLabelKHR", get_object_ptr_label);
#undef ADAPT
  }
}
} // namespace venture::gfx
