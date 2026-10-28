// NDK r30 armeabi-v7a oracle: real clang-generated EGL/GLES calls, including
// ILP32 pointer arrays, pointer-sized offsets and aligned GLuint64 arguments.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <stdint.h>
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
static int callback_seen;
static void GL_APIENTRY callback(GLenum source, GLenum type, GLuint id, GLenum severity,
                                 GLsizei length, const GLchar* text, const void* user) {
  if (source == GL_DEBUG_SOURCE_APPLICATION_KHR && type == GL_DEBUG_TYPE_MARKER_KHR &&
      id == 91 && severity == GL_DEBUG_SEVERITY_NOTIFICATION_KHR && length == 3 &&
      text[0] == 'a' && text[2] == 'c' && user == &callback_seen) callback_seen = 1;
}
int vtest_main(int argc, char** argv) {
  (void)argc; (void)argv;
  _Static_assert(sizeof(EGLConfig) == 4 && sizeof(EGLAttrib) == 4 && sizeof(GLsync) == 4, "ILP32");
  _Static_assert(sizeof(GLintptr) == 4 && sizeof(GLsizeiptr) == 4 && sizeof(GLuint64) == 8, "GL widths");
  EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  CHECK(d); CHECK(eglInitialize(d, 0, 0));
  CHECK(eglQueryString(d, EGL_VERSION) == eglQueryString(d, EGL_VERSION));
  struct { EGLConfig values[2]; uint32_t canary; } allconfigs = {{0, 0}, 0x10203040};
  EGLint allcount = 0;
  CHECK(eglGetConfigs(d, allconfigs.values, 2, &allcount)); CHECK(allcount > 0 && allcount <= 2 && allconfigs.canary == 0x10203040);
  const EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                         EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
  struct { EGLConfig c; uint32_t canary; } configs = {0, 0xfeedface};
  EGLint count = 0;
  CHECK(eglChooseConfig(d, attrs, &configs.c, 1, &count)); CHECK(count == 1 && configs.c && configs.canary == 0xfeedface);
  const EGLint ctxattrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  EGLContext c = eglCreateContext(d, configs.c, EGL_NO_CONTEXT, ctxattrs); CHECK(c);
  EGLint query = 0;
  CHECK(!eglQueryContext(d, (EGLContext)configs.c, EGL_CONTEXT_CLIENT_VERSION, &query)); CHECK(eglGetError() == EGL_BAD_CONTEXT);
  const EGLint surfaceattrs[] = {EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE};
  EGLSurface s = eglCreatePbufferSurface(d, configs.c, surfaceattrs); CHECK(s);
  CHECK(eglMakeCurrent(d, s, s, c)); CHECK(eglGetCurrentContext() == c && eglGetCurrentDisplay() == d && eglGetCurrentSurface(EGL_DRAW) == s);
  CHECK(glGetString(GL_VERSION) == glGetString(GL_VERSION));
  GLint extensions = 0; glGetIntegerv(GL_NUM_EXTENSIONS, &extensions);
  if (extensions) CHECK(glGetStringi(GL_EXTENSIONS, 0) == glGetStringi(GL_EXTENSIONS, 0));
  // Proc pointers must be even ARM-mode guest thunks and cache their address.
  PFNGLMAPBUFFERRANGEPROC map = (PFNGLMAPBUFFERRANGEPROC)eglGetProcAddress("glMapBufferRange");
  CHECK(map && !((uintptr_t)map & 1) && (void*)map == (void*)eglGetProcAddress("glMapBufferRange"));
  CHECK(!map(GL_ARRAY_BUFFER, 0, 4, GL_MAP_WRITE_BIT)); CHECK(glGetError() == GL_INVALID_OPERATION);
  CHECK(glGetError() == GL_NO_ERROR);
  GLuint b; glGenBuffers(1, &b); glBindBuffer(GL_ARRAY_BUFFER, b);
  const uint32_t initial[] = {11, 22, 33, 44};
  glBufferData(GL_ARRAY_BUFFER, sizeof(initial), initial, GL_DYNAMIC_DRAW);
  CHECK(!map(GL_ARRAY_BUFFER, -4, 4, GL_MAP_READ_BIT)); CHECK(glGetError() == GL_INVALID_VALUE);
  uint32_t* p = map(GL_ARRAY_BUFFER, 4, 8, GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);
  CHECK(p && p[0] == 22 && p[1] == 33); p[0] = 77; p[1] = 88;
  struct { void* ptr; uint32_t canary; } pointer = {0, 0x12345678};
  glGetBufferPointerv(GL_ARRAY_BUFFER, GL_BUFFER_MAP_POINTER, &pointer.ptr); CHECK(pointer.ptr == p && pointer.canary == 0x12345678);
  CHECK(glUnmapBuffer(GL_ARRAY_BUFFER));
  p = map(GL_ARRAY_BUFFER, 0, sizeof(initial), GL_MAP_READ_BIT); CHECK(p && p[0] == 11 && p[1] == 77 && p[2] == 88 && p[3] == 44);
  CHECK(glUnmapBuffer(GL_ARRAY_BUFFER));
  p = map(GL_ARRAY_BUFFER, 4, 8, GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT); CHECK(p);
  p[0] = 99; p[1] = 100; glFlushMappedBufferRange(GL_ARRAY_BUFFER, 0, 4); CHECK(glUnmapBuffer(GL_ARRAY_BUFFER));
  p = map(GL_ARRAY_BUFFER, 0, sizeof(initial), GL_MAP_READ_BIT); CHECK(p && p[1] == 99 && p[2] == 88); CHECK(glUnmapBuffer(GL_ARRAY_BUFFER));
  p = map(GL_ARRAY_BUFFER, 4, 8, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT); CHECK(p);
  GLint value = 0;
  glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_MAPPED, &value); CHECK(value == GL_TRUE);
  glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_ACCESS_FLAGS, &value); CHECK(value == (GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT));
  glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_MAP_OFFSET, &value); CHECK(value == 4);
  glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_MAP_LENGTH, &value); CHECK(value == 8);
  glGetBufferPointerv(GL_ARRAY_BUFFER, GL_BUFFER_MAP_POINTER, &pointer.ptr); CHECK(pointer.ptr == p && pointer.canary == 0x12345678);
  CHECK(!map(GL_ARRAY_BUFFER, 0, 4, GL_MAP_WRITE_BIT)); CHECK(glGetError() == GL_INVALID_OPERATION);
  p[0] = 99; p[1] = 88; CHECK(glUnmapBuffer(GL_ARRAY_BUFFER));
  glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_MAPPED, &value); CHECK(value == GL_FALSE);
  glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_MAP_OFFSET, &value); CHECK(value == 0);
  glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_MAP_LENGTH, &value); CHECK(value == 0);
  PFNGLMAPBUFFEROESPROC map_oes = (PFNGLMAPBUFFEROESPROC)eglGetProcAddress("glMapBufferOES"); CHECK(map_oes);
  p = map_oes(GL_ARRAY_BUFFER, GL_WRITE_ONLY_OES); CHECK(p); p[0] = 7; p[1] = 8; p[2] = 9; p[3] = 10; CHECK(glUnmapBuffer(GL_ARRAY_BUFFER));
  p = map(GL_ARRAY_BUFFER, 0, sizeof(initial), GL_MAP_READ_BIT); CHECK(p && p[0] == 7 && p[3] == 10); CHECK(glUnmapBuffer(GL_ARRAY_BUFFER));
  // An unmapped shadow is reused while unwritten OES bytes retain their contents.
  uint32_t* q = map_oes(GL_ARRAY_BUFFER, GL_WRITE_ONLY_OES); CHECK(q == p && q[0] == 7 && q[3] == 10); q[0] = 5; CHECK(glUnmapBuffer(GL_ARRAY_BUFFER));
  p = map(GL_ARRAY_BUFFER, 0, sizeof(initial), GL_MAP_READ_BIT); CHECK(p == q && p[0] == 5 && p[3] == 10); CHECK(glUnmapBuffer(GL_ARRAY_BUFFER));
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8, (void*)4);
  glGetVertexAttribPointerv(1, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer.ptr); CHECK(pointer.ptr == (void*)4 && pointer.canary == 0x12345678);
  GLuint shader = glCreateShader(GL_VERTEX_SHADER);
  const char* source[] = {"#version 300 es\n", "out float value; void main(){value=1.0; gl_Position=vec4(0);}"};
  glShaderSource(shader, 2, source, 0); glCompileShader(shader);
  GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok); CHECK(ok);
  GLuint program = glCreateProgram(); glAttachShader(program, shader);
  GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
  const char* fragment_source = "#version 300 es\nprecision mediump float; out vec4 color; void main(){color=vec4(1);}";
  glShaderSource(fragment, 1, &fragment_source, 0); glCompileShader(fragment); glAttachShader(program, fragment);
  const char* varying[] = {"value"}; glTransformFeedbackVaryings(program, 1, varying, GL_INTERLEAVED_ATTRIBS);
  glLinkProgram(program); glGetProgramiv(program, GL_LINK_STATUS, &ok); CHECK(ok);
  GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0); CHECK(sync && glIsSync(sync));
  GLenum waited = glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, UINT64_MAX); CHECK(waited == GL_ALREADY_SIGNALED || waited == GL_CONDITION_SATISFIED);
  glWaitSync(sync, 0, GL_TIMEOUT_IGNORED); glDeleteSync(sync); CHECK(!glIsSync(sync));
  CHECK(glClientWaitSync(sync, 0, 0) == GL_WAIT_FAILED); CHECK(glGetError() == GL_INVALID_VALUE);
  EGLAttrib syncattrs[] = {EGL_NONE}; EGLSync es = eglCreateSync(d, EGL_SYNC_FENCE, syncattrs); CHECK(es);
  struct { EGLAttrib value; uint32_t canary; } attr = {0, 0xabcdef01};
  CHECK(eglGetSyncAttrib(d, es, EGL_SYNC_TYPE, &attr.value)); CHECK(attr.value == EGL_SYNC_FENCE && attr.canary == 0xabcdef01);
  CHECK(eglClientWaitSync(d, es, EGL_SYNC_FLUSH_COMMANDS_BIT, EGL_FOREVER) != EGL_FALSE); CHECK(eglDestroySync(d, es));
  // An EGLClientBuffer texture name stays a GLuint, while the resulting
  // EGLImage and GLeglImageOES alias share a typed opaque token.
  GLuint textures[2]; glGenTextures(2, textures); glBindTexture(GL_TEXTURE_2D, textures[0]);
  const unsigned char image_pixel[] = {13, 27, 41, 255};
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, image_pixel);
  EGLAttrib imageattrs[] = {EGL_IMAGE_PRESERVED, EGL_TRUE, EGL_NONE};
  EGLImage image = eglCreateImage(d, c, EGL_GL_TEXTURE_2D_KHR, (EGLClientBuffer)(uintptr_t)textures[0], imageattrs); CHECK(image);
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES"); CHECK(image_target);
  glBindTexture(GL_TEXTURE_2D, textures[1]); image_target(GL_TEXTURE_2D, image); CHECK(glGetError() == GL_NO_ERROR);
  GLuint fbo; glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[1], 0); CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
  unsigned char copied[4]; glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, copied);
  CHECK(copied[0] == 13 && copied[1] == 27 && copied[2] == 41 && copied[3] == 255);
  glBindFramebuffer(GL_FRAMEBUFFER, 0); glDeleteFramebuffers(1, &fbo); CHECK(eglDestroyImage(d, image)); glDeleteTextures(2, textures);
  PFNGLDEBUGMESSAGECALLBACKKHRPROC debug = (PFNGLDEBUGMESSAGECALLBACKKHRPROC)eglGetProcAddress("glDebugMessageCallbackKHR");
  PFNGLDEBUGMESSAGEINSERTKHRPROC insert = (PFNGLDEBUGMESSAGEINSERTKHRPROC)eglGetProcAddress("glDebugMessageInsertKHR");
  PFNGLGETPOINTERVKHRPROC getpointer = (PFNGLGETPOINTERVKHRPROC)eglGetProcAddress("glGetPointervKHR");
  if (debug && insert && getpointer) {
    glEnable(GL_DEBUG_OUTPUT_KHR); glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR); debug(callback, &callback_seen);
    getpointer(GL_DEBUG_CALLBACK_USER_PARAM_KHR, &pointer.ptr); CHECK(pointer.ptr == &callback_seen && pointer.canary == 0x12345678);
    insert(GL_DEBUG_SOURCE_APPLICATION_KHR, GL_DEBUG_TYPE_MARKER_KHR, 91, GL_DEBUG_SEVERITY_NOTIFICATION_KHR, 3, "abc"); CHECK(callback_seen);
    debug(0, 0);
  }
  glClearColor(0.25f, 0.5f, 0.75f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
  unsigned char pixel[4]; glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
  CHECK(pixel[0] >= 63 && pixel[0] <= 64 && pixel[1] >= 127 && pixel[1] <= 128 && pixel[2] >= 191 && pixel[2] <= 192);
  CHECK(glGetError() == GL_NO_ERROR);
  glDeleteBuffers(1, &b); glDeleteProgram(program); glDeleteShader(shader); glDeleteShader(fragment);
  CHECK(eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
  CHECK(eglDestroySurface(d, s)); CHECK(eglDestroyContext(d, c)); CHECK(eglTerminate(d));
  return 0;
}
