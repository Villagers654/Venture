#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>
#include <zlib.h>

#include "android/native_window.h"
#include "core/thread.h"
#include "core/hle_runtime.h"
#include "core/process.h"
#include "core/pipeline_stats.h"
#include "gfx/angle.h"
#include "gfx/blob_cache.h"
#include "gfx/dispatch.h"
#include "gfx/gpu_device.h"
#include "hle/gles/gles_state.h"
#include "gfx/gpu_device_internal.h"
#include "gfx/texture_decode.h"
#include "gfx/xr_images.h"
#include "hle/vulkan/timing.h"
#include "platform/profile.h"
#include "common/json.h"
#include "common/paths.h"
#include "xr/xr_images.h"
#include "gfx_test_support.h"
#include "hle/egl/egl.h"

using namespace venture;
namespace {
// Drive registered HLE handlers using AAPCS64 X/V registers plus stack
// overflow.
template <class T>
std::uint64_t word(T v) {
  if constexpr (std::is_pointer_v<T>)
    return h2g(v);
  else
    return static_cast<std::uint64_t>(v);
}
template <class... A>
std::uint64_t call(const char* name, A... args) {
  auto* info = test::graphics_function(name);
  REQUIRE(info != nullptr);
  std::array<std::uint64_t, 8> gp{};
  std::array<double, 8> fp{};
  std::array<std::uint64_t, 32> stack{};
  unsigned x = 0, v = 0, s = 0;
  auto put = [&]<class T>(T value) {
    if constexpr (std::is_floating_point_v<T>) {
      auto bits = [&]() -> std::uint64_t {
        if constexpr (sizeof(T) == 4)
          return std::bit_cast<std::uint32_t>(value);
        else
          return std::bit_cast<std::uint64_t>(value);
      }();
      if (v < 8)
        fp[v++] = std::bit_cast<double>(bits);
      else
        stack[s++] = bits;
    } else {
      if (x < 8)
        gp[x++] = word(value);
      else
        stack[s++] = word(value);
    }
  };
  (put(args), ...);
  auto& thread = GuestThread::adopt_current();
  return thread.call(info->thunk, {{gp.data(), x}, {fp.data(), v}, {stack.data(), s}}).x0;
}
struct EglSession {
  EGLDisplay display{};
  EGLConfig config{};
  EGLContext context{};
  EGLSurface surface{};
  explicit EglSession(int version = 3, bool no_error = false, int context_minor = 0,
                      bool exact_version = false) {
    gfx::register_graphics_libraries();
    display = g2h(call("eglGetDisplay", 0));
    REQUIRE(display != nullptr);
    EGLint major{}, minor{};
    REQUIRE(call("eglInitialize", display, &major, &minor) == EGL_TRUE);
    CHECK(major == 1);
    const EGLint attributes[] = {EGL_SURFACE_TYPE,
                                 EGL_WINDOW_BIT,
                                 EGL_RENDERABLE_TYPE,
                                 EGL_OPENGL_ES3_BIT,
                                 EGL_RED_SIZE,
                                 8,
                                 EGL_GREEN_SIZE,
                                 8,
                                 EGL_BLUE_SIZE,
                                 8,
                                 EGL_ALPHA_SIZE,
                                 8,
                                 EGL_NONE};
    EGLint count{};
    REQUIRE(call("eglChooseConfig", display, attributes, &config, 1, &count) ==
            EGL_TRUE);
    REQUIRE(count == 1);
    std::vector<EGLint> ca = {EGL_CONTEXT_CLIENT_VERSION, version,
                             EGL_CONTEXT_PRIORITY_LEVEL_IMG,
                             EGL_CONTEXT_PRIORITY_HIGH_IMG,
                             EGL_CONTEXT_OPENGL_NO_ERROR_KHR,
                             no_error ? EGL_TRUE : EGL_FALSE};
    if (context_minor)
      ca.insert(ca.end(), {EGL_CONTEXT_MINOR_VERSION_KHR, context_minor});
    // EGL may satisfy a version request with a newer compatible context.
    // Only a negative-version fixture asks ANGLE for the exact version; this
    // host-only test query does not advertise an extra guest extension.
    if (exact_version) {
      const auto* extensions = gfx::angle_function<PFNEGLQUERYSTRINGPROC>(
          "eglQueryString")(display, EGL_EXTENSIONS);
      if (extensions && gfx::has_extension(extensions,
              "EGL_ANGLE_create_context_backwards_compatible"))
        ca.insert(ca.end(), {0x3483 /* EGL_CONTEXT_OPENGL_BACKWARDS_COMPATIBLE_ANGLE */, EGL_FALSE});
    }
    ca.push_back(EGL_NONE);
    context = g2h(call("eglCreateContext", display, config, 0, ca.data()));
    REQUIRE(context != nullptr);
    const EGLint pa[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    surface = g2h(call("eglCreatePbufferSurface", display, config, pa));
    REQUIRE(surface != nullptr);
    REQUIRE(call("eglMakeCurrent", display, surface, surface, context) ==
            EGL_TRUE);
  }
  ~EglSession() {
    call("eglMakeCurrent", display, 0, 0, 0);
    call("eglDestroySurface", display, surface);
    call("eglDestroyContext", display, context);
  }
};
bool have_angle() {
  if (!std::getenv("VENTURE_ANGLE_DIR")) {
    MESSAGE("Set VENTURE_ANGLE_DIR to run ANGLE hardware smoke tests");
    return false;
  }
  return true;
}
// ASTC specification C.2.24: LDR void-extent constant-color block, all extents
// ones, RGBA unsigned normalized 16-bit. Valid for every 2D LDR block
// footprint.
std::array<std::uint8_t, 16> constant_block(std::uint8_t r, std::uint8_t g,
                                            std::uint8_t b,
                                            std::uint8_t a = 255) {
  return {0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
          r,    r,    g,    g,    b,    b,    a,    a};
}
std::array<std::uint8_t, 4> sample_texture(GLuint texture) {
  const char* vs =
      "#version 300 es\nvoid main(){ vec2 "
      "p=vec2((gl_VertexID<<1)&2,gl_VertexID&2); "
      "gl_Position=vec4(p*2.0-1.0,0,1); }";
  const char* fs =
      "#version 300 es\nprecision highp float; uniform sampler2D source; out "
      "vec4 color; void main(){color=texture(source,vec2(0.5));}";
  GLuint shaders[2]{};
  for (int i = 0; i < 2; ++i) {
    shaders[i] = static_cast<GLuint>(
        call("glCreateShader", i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER));
    auto source = i ? fs : vs;
    call("glShaderSource", shaders[i], 1, &source, 0);
    call("glCompileShader", shaders[i]);
    GLint ok{};
    call("glGetShaderiv", shaders[i], GL_COMPILE_STATUS, &ok);
    REQUIRE(ok);
  }
  auto program = static_cast<GLuint>(call("glCreateProgram"));
  for (auto shader : shaders) call("glAttachShader", program, shader);
  call("glLinkProgram", program);
  GLint linked{};
  call("glGetProgramiv", program, GL_LINK_STATUS, &linked);
  REQUIRE(linked);
  call("glUseProgram", program);
  GLuint output{}, fbo{};
  call("glGenTextures", 1, &output);
  call("glBindTexture", GL_TEXTURE_2D, output);
  call("glTexStorage2D", GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1);
  call("glGenFramebuffers", 1, &fbo);
  call("glBindFramebuffer", GL_FRAMEBUFFER, fbo);
  call("glFramebufferTexture2D", GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
       GL_TEXTURE_2D, output, 0);
  REQUIRE(call("glCheckFramebufferStatus", GL_FRAMEBUFFER) ==
          GL_FRAMEBUFFER_COMPLETE);
  call("glBindTexture", GL_TEXTURE_2D, texture);
  call("glTexParameteri", GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  call("glTexParameteri", GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  call("glViewport", 0, 0, 1, 1);
  call("glDrawArrays", GL_TRIANGLES, 0, 3);
  std::array<std::uint8_t, 4> pixel{};
  call("glReadPixels", 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
  CHECK(call("glGetError") == GL_NO_ERROR);
  call("glDeleteFramebuffers", 1, &fbo);
  call("glDeleteTextures", 1, &output);
  call("glUseProgram", 0);
  call("glDeleteProgram", program);
  for (auto shader : shaders) call("glDeleteShader", shader);
  return pixel;
}
}  // namespace
TEST_CASE("GLES shadow maps preserve data and report mapping state with native validity fallback") {
  if (!have_angle()) return;
  EglSession egl;
  GLuint buffer{};
  call("glGenBuffers", 1, &buffer);
  call("glBindBuffer", GL_ARRAY_BUFFER, buffer);
  const std::array<GLuint, 4> initial{11, 22, 33, 44};
  call("glBufferData", GL_ARRAY_BUFFER, sizeof(initial), initial.data(), GL_DYNAMIC_DRAW);
  auto &buffers = *gfx::current_gl_state()->buffers32;
  auto *seed = g2h<GLuint>(call("glMapBufferOES", GL_ARRAY_BUFFER, GL_WRITE_ONLY_OES));
  REQUIRE(seed); REQUIRE_FALSE(buffers.mappings.at(buffer)->emulated);
  REQUIRE(call("glUnmapBufferOES", GL_ARRAY_BUFFER) == GL_TRUE);
  auto *pointer = g2h<GLuint>(call("glMapBufferRange", GL_ARRAY_BUFFER, 4, 8, GL_MAP_WRITE_BIT));
  REQUIRE(pointer);
  REQUIRE(buffers.mappings.at(buffer)->emulated);
  GLint host_mapped = GL_TRUE;
  gfx::angle_function<PFNGLGETBUFFERPARAMETERIVPROC>("glGetBufferParameteriv")(
      GL_ARRAY_BUFFER, GL_BUFFER_MAPPED, &host_mapped);
  CHECK(host_mapped == GL_FALSE); // No ANGLE map or submit-and-wait.
  pointer[0] = 77; // Leave the second word unwritten.
  for (auto [pname, expected] : {std::pair{GL_BUFFER_MAPPED, 1},
       {GL_BUFFER_ACCESS_FLAGS, int(GL_MAP_WRITE_BIT)}, {GL_BUFFER_MAP_OFFSET, 4}, {GL_BUFFER_MAP_LENGTH, 8}}) {
    GLint value = -1;
    call("glGetBufferParameteriv", GL_ARRAY_BUFFER, pname, &value);
    CHECK(value == expected);
    GLint64 wide = -1;
    call("glGetBufferParameteri64v", GL_ARRAY_BUFFER, pname, &wide);
    CHECK(wide == expected);
  }
  void *reported = nullptr;
  call("glGetBufferPointerv", GL_ARRAY_BUFFER, GL_BUFFER_MAP_POINTER, &reported);
  CHECK(reported == pointer);
  CHECK(call("glMapBufferRange", GL_ARRAY_BUFFER, 0, 4, GL_MAP_WRITE_BIT) == 0);
  CHECK(call("glGetError") == GL_INVALID_OPERATION);
  CHECK(call("glUnmapBuffer", GL_ARRAY_BUFFER) == GL_TRUE);
  pointer = g2h<GLuint>(call("glMapBufferRange", GL_ARRAY_BUFFER, 0, sizeof(initial), GL_MAP_READ_BIT));
  REQUIRE(pointer);
  CHECK(pointer[0] == 11); CHECK(pointer[1] == 77); CHECK(pointer[2] == 33); CHECK(pointer[3] == 44);
  CHECK(call("glUnmapBuffer", GL_ARRAY_BUFFER) == GL_TRUE);
  for (auto pname : {GL_BUFFER_MAPPED, GL_BUFFER_ACCESS_FLAGS, GL_BUFFER_MAP_OFFSET, GL_BUFFER_MAP_LENGTH}) {
    GLint value = -1; call("glGetBufferParameteriv", GL_ARRAY_BUFFER, pname, &value); CHECK(value == 0);
  }
  reported = pointer;
  call("glGetBufferPointerv", GL_ARRAY_BUFFER, GL_BUFFER_MAP_POINTER, &reported);
  CHECK(reported == nullptr);
  for (auto [offset, length, access, error] : {
       std::tuple{std::int64_t(-1), std::int64_t(4), GLbitfield(GL_MAP_WRITE_BIT), GLenum(GL_INVALID_VALUE)},
       {12, 8, GL_MAP_WRITE_BIT, GL_INVALID_VALUE}, {0, 0, GL_MAP_WRITE_BIT, GL_INVALID_OPERATION},
       {0, 4, 0, GL_INVALID_OPERATION}, {0, 4, 0x10000, GL_INVALID_VALUE},
       {0, 4, GL_MAP_READ_BIT | GL_MAP_INVALIDATE_RANGE_BIT, GL_INVALID_OPERATION}}) {
    CHECK(call("glMapBufferRange", GL_ARRAY_BUFFER, offset, length, access) == 0);
    CHECK(call("glGetError") == error);
    CHECK(call("glGetError") == GL_NO_ERROR);
  }
  call("glBindBuffer", GL_ARRAY_BUFFER, 0);
  CHECK(call("glMapBufferRange", GL_ARRAY_BUFFER, 0, 4, GL_MAP_WRITE_BIT) == 0);
  CHECK(call("glGetError") == GL_INVALID_OPERATION);
  CHECK(call("glGetError") == GL_NO_ERROR);
  call("glBindBuffer", GL_ARRAY_BUFFER, buffer);
  // NULL stores receive a real initial value, retaining bytes that an OES
  // write-only map leaves untouched after a partial SubData update.
  call("glBufferData", GL_ARRAY_BUFFER, 8, 0, GL_DYNAMIC_DRAW);
  GLuint update = 71;
  call("glBufferSubData", GL_ARRAY_BUFFER, 4, 4, &update);
  pointer = g2h<GLuint>(call("glMapBufferOES", GL_ARRAY_BUFFER, GL_WRITE_ONLY_OES));
  REQUIRE(pointer); REQUIRE(buffers.mappings.at(buffer)->emulated);
  CHECK(pointer[0] == 0); CHECK(pointer[1] == 71);
  pointer[0] = 17;
  CHECK(call("glUnmapBufferOES", GL_ARRAY_BUFFER) == GL_TRUE);
  pointer = g2h<GLuint>(call("glMapBufferRange", GL_ARRAY_BUFFER, 0, 8, GL_MAP_READ_BIT));
  REQUIRE(pointer); CHECK(pointer[0] == 17); CHECK(pointer[1] == 71);
  CHECK(call("glUnmapBuffer", GL_ARRAY_BUFFER) == GL_TRUE);
  // BufferData invalidates the cached size, and an invalidated whole map
  // uploads after orphaning while retaining the original usage hint.
  call("glBufferData", GL_ARRAY_BUFFER, 8, 0, GL_STREAM_DRAW);
  CHECK(buffers.data.at(buffer).size == -1);
  pointer = g2h<GLuint>(call("glMapBufferRange", GL_ARRAY_BUFFER, 0, 8,
      GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT));
  REQUIRE(pointer); REQUIRE(buffers.mappings.at(buffer)->emulated);
  pointer[0] = 55; pointer[1] = 66;
  CHECK(call("glUnmapBuffer", GL_ARRAY_BUFFER) == GL_TRUE);
  pointer = g2h<GLuint>(call("glMapBufferOES", GL_ARRAY_BUFFER, GL_WRITE_ONLY_OES));
  REQUIRE(pointer); REQUIRE(buffers.mappings.at(buffer)->emulated);
  GLint oes_access = 0;
  call("glGetBufferParameteriv", GL_ARRAY_BUFFER, GL_BUFFER_ACCESS_OES, &oes_access);
  CHECK(oes_access == GL_WRITE_ONLY_OES);
  pointer[0] = 99;
  CHECK(call("glUnmapBufferOES", GL_ARRAY_BUFFER) == GL_TRUE);
  pointer = g2h<GLuint>(call("glMapBufferRange", GL_ARRAY_BUFFER, 0, 8, GL_MAP_READ_BIT));
  REQUIRE(pointer); CHECK(pointer[0] == 99); CHECK(pointer[1] == 66);
  CHECK(call("glUnmapBuffer", GL_ARRAY_BUFFER) == GL_TRUE);
  CHECK(call("glGetError") == GL_NO_ERROR);
  // A GL command issued during an emulated map must retain native validation.
  pointer = g2h<GLuint>(call("glMapBufferRange", GL_ARRAY_BUFFER, 0, 8, GL_MAP_WRITE_BIT));
  REQUIRE(pointer); REQUIRE(buffers.mappings.at(buffer)->emulated);
  call("glBindBuffer", GL_PIXEL_PACK_BUFFER, buffer);
  call("glReadPixels", 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, 0);
  CHECK(call("glGetError") == GL_INVALID_OPERATION);
  REQUIRE_FALSE(buffers.mappings.at(buffer)->emulated);
  gfx::angle_function<PFNGLGETBUFFERPARAMETERIVPROC>("glGetBufferParameteriv")(
      GL_PIXEL_PACK_BUFFER, GL_BUFFER_MAPPED, &host_mapped);
  CHECK(host_mapped == GL_TRUE);
  CHECK(call("glUnmapBuffer", GL_PIXEL_PACK_BUFFER) == GL_TRUE);
  CHECK(buffers.emulated_maps.load() == 0);
  call("glBindBuffer", GL_PIXEL_PACK_BUFFER, 0);
  // A GPU-writable binding makes the CPU copy unprovable, even if the
  // buffer is later rebound to ARRAY_BUFFER: the native map remains in use.
  call("glBindBuffer", GL_PIXEL_PACK_BUFFER, buffer);
  call("glBindBuffer", GL_ARRAY_BUFFER, buffer);
  call("glBufferData", GL_ARRAY_BUFFER, sizeof(initial), initial.data(), GL_DYNAMIC_DRAW);
  pointer = g2h<GLuint>(call("glMapBufferRange", GL_ARRAY_BUFFER, 0, sizeof(initial), GL_MAP_WRITE_BIT));
  REQUIRE(pointer); REQUIRE_FALSE(buffers.mappings.at(buffer)->emulated);
  CHECK(call("glUnmapBuffer", GL_ARRAY_BUFFER) == GL_TRUE);
  pointer = g2h<GLuint>(call("glMapBufferRange", GL_ARRAY_BUFFER, 0, sizeof(initial),
      GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT));
  REQUIRE(pointer); REQUIRE_FALSE(buffers.mappings.at(buffer)->emulated);
  CHECK(call("glUnmapBuffer", GL_ARRAY_BUFFER) == GL_TRUE);
  CHECK(call("glGetError") == GL_NO_ERROR);
  call("glBindBuffer", GL_PIXEL_PACK_BUFFER, 0);
  call("glDeleteBuffers", 1, &buffer);
}
TEST_CASE("gfx ANGLE binary cache identity and UE4 stereo framebuffer replay") {
  if (!have_angle()) return;
  EglSession egl;
  const char* sources[] = {
      "#version 300 es\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,"
      "gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0.5,1);}",
      "#version 300 es\nprecision highp float;uniform vec4 eyeColor;"
      "out vec4 color;void main(){color=eyeColor;}"};
  GLuint shaders[2]{};
  const auto original = static_cast<GLuint>(call("glCreateProgram"));
  call("glProgramParameteri", original, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
  for (unsigned i = 0; i < 2; ++i) {
    shaders[i] = static_cast<GLuint>(
        call("glCreateShader", i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER));
    call("glShaderSource", shaders[i], 1, &sources[i], 0);
    call("glCompileShader", shaders[i]);
    GLint compiled{};
    call("glGetShaderiv", shaders[i], GL_COMPILE_STATUS, &compiled);
    REQUIRE(compiled);
    call("glAttachShader", original, shaders[i]);
  }
  call("glLinkProgram", original);
  GLint linked{}, length{};
  call("glGetProgramiv", original, GL_LINK_STATUS, &linked);
  REQUIRE(linked);
  call("glGetProgramiv", original, GL_PROGRAM_BINARY_LENGTH, &length);
  REQUIRE(length > 16);
  std::vector<unsigned char> binary(length);
  GLenum format{};
  GLsizei written{};
  call("glGetProgramBinary", original, length, &written, &format, binary.data());
  REQUIRE(written > 16);
  binary.resize(written);

  // ANGLE Program.cpp serializes a 16-byte source compatibility ID first.
  // With compressProgramBinaryBlob it wraps the payload in Program.h's
  // 16-byte ANGL header and gzip. Downstream Vulkan patches change this ID
  // even when the upstream git revision (formerly all GL_VERSION identified)
  // stays fixed. A driver's public cache key must change with that ID.
  std::array<char, 16> compatibility{};
  std::uint32_t magic{};
  std::memcpy(&magic, binary.data(), sizeof(magic));
  if (magic == 0x414e474c) {
    z_stream stream{};
    REQUIRE(inflateInit2(&stream, MAX_WBITS + 16) == Z_OK);
    stream.next_in = binary.data() + 16;
    stream.avail_in = static_cast<uInt>(binary.size() - 16);
    stream.next_out = reinterpret_cast<Bytef*>(compatibility.data());
    stream.avail_out = static_cast<uInt>(compatibility.size());
    const auto result = inflate(&stream, Z_NO_FLUSH);
    CHECK((result == Z_OK || result == Z_STREAM_END));
    CHECK(stream.total_out == compatibility.size());
    CHECK(inflateEnd(&stream) == Z_OK);
  } else {
    std::memcpy(compatibility.data(), binary.data(), compatibility.size());
  }
  const std::string_view version(
      g2h<const char>(call("glGetString", GL_VERSION)));
  INFO(version);
  INFO(std::string_view(compatibility.data(), compatibility.size()));
  CHECK(version.find(std::string_view(compatibility.data(), compatibility.size())) !=
        std::string_view::npos);

  // Replay the observed Vader path: cached program, ordinary packed 2D eye
  // texture, reversed depth (clear 0 / GEQUAL), and depth/stencil discard.
  // It does not use a multiview framebuffer or implicit MSAA resolve.
  const auto program = static_cast<GLuint>(call("glCreateProgram"));
  call("glProgramBinary", program, format, binary.data(), written);
  call("glGetProgramiv", program, GL_LINK_STATUS, &linked);
  REQUIRE(linked);
  call("glDeleteProgram", original);
  for (auto shader : shaders) call("glDeleteShader", shader);
  call("glUseProgram", program);
  const auto color = static_cast<GLint>(call("glGetUniformLocation", program, "eyeColor"));
  REQUIRE(color >= 0);
  GLuint textures[2]{}, fbo{}, vao{}, indices{};
  call("glGenTextures", 2, textures);
  for (unsigned i = 0; i < 2; ++i) {
    call("glBindTexture", GL_TEXTURE_2D, textures[i]);
    call("glTexStorage2D", GL_TEXTURE_2D, 1, i ? GL_DEPTH24_STENCIL8 : GL_RGBA8, 16, 8);
  }
  call("glGenFramebuffers", 1, &fbo);
  call("glBindFramebuffer", GL_FRAMEBUFFER, fbo);
  call("glFramebufferTexture2D", GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
       GL_TEXTURE_2D, textures[0], 0);
  call("glFramebufferTexture2D", GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
       GL_TEXTURE_2D, textures[1], 0);
  REQUIRE(call("glCheckFramebufferStatus", GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
  const GLfloat black[] = {0, 0, 0, 1};
  call("glClearBufferfv", GL_COLOR, 0, black);
  call("glDepthMask", GL_TRUE);
  call("glClearBufferfi", GL_DEPTH_STENCIL, 0, 0.0f, 0);
  call("glEnable", GL_DEPTH_TEST);
  call("glDepthFunc", GL_GEQUAL);
  call("glGenVertexArrays", 1, &vao);
  call("glBindVertexArray", vao);
  call("glGenBuffers", 1, &indices);
  call("glBindBuffer", GL_ELEMENT_ARRAY_BUFFER, indices);
  const GLushort triangle[] = {0, 1, 2};
  call("glBufferData", GL_ELEMENT_ARRAY_BUFFER, sizeof(triangle), triangle, GL_STATIC_DRAW);
  for (unsigned eye = 0; eye < 2; ++eye) {
    call("glViewport", eye * 8, 0, 8, 8);
    call("glUniform4f", color, eye ? 0.0f : 1.0f, eye ? 1.0f : 0.0f, 0.0f, 1.0f);
    call("glDrawElements", GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, 0);
  }
  const GLenum discarded[] = {GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT};
  call("glDiscardFramebufferEXT", GL_FRAMEBUFFER, 2, discarded);
  for (unsigned eye = 0; eye < 2; ++eye) {
    std::array<GLubyte, 4> pixel{};
    call("glReadPixels", eye * 8 + 4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    CHECK(pixel[0] == (eye ? 0 : 255));
    CHECK(pixel[1] == (eye ? 255 : 0));
    CHECK(pixel[2] == 0);
    CHECK(pixel[3] == 255);
  }
  CHECK(call("glGetError") == GL_NO_ERROR);
  call("glDeleteBuffers", 1, &indices);
  call("glDeleteVertexArrays", 1, &vao);
  call("glDeleteFramebuffers", 1, &fbo);
  call("glDeleteTextures", 2, textures);
  call("glUseProgram", 0);
  call("glDeleteProgram", program);
}
TEST_CASE("gfx ASTC every LDR footprint and sRGB decode plus cache integrity") {
  const auto block = constant_block(37, 83, 171, 209);
  for (std::uint32_t base : {0x93B0u, 0x93D0u})
    for (std::uint32_t i = 0; i < 14; ++i) {
      gfx::AstcFormat format{};
      REQUIRE(gfx::astc_format(base + i, format));
      auto decoded = gfx::decode_astc(base + i, format.block_width,
                                      format.block_height, 1, block);
      REQUIRE(decoded.size() == format.block_width * format.block_height * 4);
      for (std::size_t p = 0; p < decoded.size(); p += 4) {
        CHECK(decoded[p] == 37);
        CHECK(decoded[p + 1] == 83);
        CHECK(decoded[p + 2] == 171);
        CHECK(decoded[p + 3] == 209);
      }
    }
  CHECK_THROWS(gfx::decode_astc(0x93B0, 4, 4, 1, std::span(block).first(15)));
  std::vector<std::uint8_t> packed(16 * 16 * 16 * 2);
  for (std::size_t i = 0; i < packed.size(); i += 16)
    std::copy(block.begin(), block.end(), packed.begin() + i);
  auto large = gfx::decode_astc(0x93B0, 64, 64, 2, packed);
  std::vector<std::uint8_t> expected(64 * 64 * 2 * 4);
  for (std::size_t i = 0; i < expected.size(); i += 4) {
    expected[i] = 37;
    expected[i + 1] = 83;
    expected[i + 2] = 171;
    expected[i + 3] = 209;
  }
  CHECK(large == expected);
  auto cache =
      std::filesystem::temp_directory_path() /
      ("Venture-gfx-test-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(cache);
  auto first = gfx::decode_astc(0x93B0, 4, 4, 1, block, cache);
  CHECK(gfx::decode_astc(0x93B0, 4, 4, 1, block, cache) == first);
  for (const auto& entry : std::filesystem::directory_iterator(cache))
    if (entry.path().extension() == ".rgba") {
      std::ofstream corrupt(entry.path(), std::ios::binary | std::ios::trunc);
      corrupt << "corrupt";
    }
  CHECK(gfx::decode_astc(0x93B0, 4, 4, 1, block, cache) == first);
  // Real copied-data roots plus the content key/temp suffix can exceed MAX_PATH.
  const auto long_cache = cache / std::string(80, 'a') / std::string(80, 'b');
  CHECK(gfx::decode_astc(0x93B0, 4, 4, 1, block, long_cache) == first);
  std::filesystem::path published;
  unsigned entries = 0;
  for (const auto& entry : std::filesystem::directory_iterator(paths::io_path(long_cache))) {
    CHECK(entry.path().extension() == ".rgba");
    published = entry.path();
    ++entries;
  }
  REQUIRE(entries == 1);
  REQUIRE(published.native().size() > 260);
  CHECK(std::filesystem::file_size(published) == first.size() + 64);
  const auto stamp = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
  std::filesystem::last_write_time(published, stamp);
  const auto saved_stamp = std::filesystem::last_write_time(published);
  CHECK(gfx::decode_astc(0x93B0, 4, 4, 1, block, long_cache) == first);
  // A cache hit reads the old file; decoding and republishing changes its stamp.
  CHECK(std::filesystem::last_write_time(published) == saved_stamp);
  // A cache directory that is a file must not prevent decoding.
  auto unavailable = cache / "unavailable";
  { std::ofstream out(unavailable); out << "file"; }
  CHECK(gfx::decode_astc(0x93B0, 4, 4, 1, block, unavailable) == first);
  // Publication over a directory fails, but decoded pixels remain usable.
  for (const auto& entry : std::filesystem::directory_iterator(cache)) {
    if (entry.path().extension() != ".rgba") continue;
    std::filesystem::remove(entry.path());
    std::filesystem::create_directory(entry.path());
  }
  CHECK(gfx::decode_astc(0x93B0, 4, 4, 1, block, cache) == first);
  for (const auto& entry : std::filesystem::directory_iterator(cache))
    CHECK(entry.path().extension() != ".tmp");
  // This directory is exclusively created by the test, below
  // temp_directory_path.
  std::filesystem::remove_all(cache);
}
TEST_SUITE_BEGIN("gpu");
TEST_CASE("gfx desktop multisample-to-texture resolves antialiased guest eye buffers") {
  if (!have_angle()) return;
  EglSession egl;
  const auto* extensions = g2h<const char>(call("glGetString", GL_EXTENSIONS));
  REQUIRE(extensions);
  REQUIRE(gfx::has_extension(extensions, "GL_EXT_multisampled_render_to_texture"));
  REQUIRE(call("eglGetProcAddress", "glFramebufferTexture2DMultisampleEXT") != 0);
  GLuint texture{}, fbo{};
  call("glGenTextures", 1, &texture);
  call("glBindTexture", GL_TEXTURE_2D, texture);
  call("glTexStorage2D", GL_TEXTURE_2D, 1, GL_RGBA8, 16, 16);
  call("glGenFramebuffers", 1, &fbo);
  call("glBindFramebuffer", GL_FRAMEBUFFER, fbo);
  call("glFramebufferTexture2DMultisampleEXT", GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0, 4);
  REQUIRE(call("glCheckFramebufferStatus", GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
  const char* sources[]{
      "#version 300 es\nvoid main(){vec2 p[3]=vec2[](vec2(-1,-1),vec2(1,-1),vec2(-1,1));gl_Position=vec4(p[gl_VertexID],0,1);}",
      "#version 300 es\nprecision highp float;out vec4 color;void main(){color=vec4(1,1,1,1);}"};
  GLuint shaders[2];
  auto program = call("glCreateProgram");
  for (int i = 0; i < 2; ++i) {
    shaders[i] = static_cast<GLuint>(call("glCreateShader", i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER));
    call("glShaderSource", shaders[i], 1, &sources[i], 0);
    call("glCompileShader", shaders[i]);
    GLint compiled{}; call("glGetShaderiv", shaders[i], GL_COMPILE_STATUS, &compiled);
    REQUIRE(compiled);
    call("glAttachShader", program, shaders[i]);
  }
  call("glLinkProgram", program);
  GLint linked{}; call("glGetProgramiv", program, GL_LINK_STATUS, &linked); REQUIRE(linked);
  call("glUseProgram", program);
  call("glViewport", 0, 0, 16, 16);
  call("glClearColor", 0.f, 0.f, 0.f, 1.f);
  call("glClear", GL_COLOR_BUFFER_BIT);
  call("glDrawArrays", GL_TRIANGLES, 0, 3);
  // Binding a different framebuffer triggers the EXT implicit texture resolve.
  call("glBindFramebuffer", GL_FRAMEBUFFER, 0);
  call("glBindFramebuffer", GL_FRAMEBUFFER, fbo);
  call("glFramebufferTexture2D", GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
  std::array<unsigned char, 16*16*4> pixels{};
  call("glReadPixels", 0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  CHECK(call("glGetError") == GL_NO_ERROR);
  CHECK(pixels[0] == 255);
  CHECK(pixels[(16*16-1)*4] == 0);
  unsigned partial = 0;
  for (std::size_t i = 0; i < pixels.size(); i += 4) partial += pixels[i] > 0 && pixels[i] < 255;
  CHECK(partial >= 12);
  call("glBindFramebuffer", GL_FRAMEBUFFER, 0);
  call("glDeleteFramebuffers", 1, &fbo);
  call("glDeleteTextures", 1, &texture);
  call("glUseProgram", 0);
  call("glDeleteProgram", program);
  for (auto shader : shaders) call("glDeleteShader", shader);
}
TEST_CASE(
    "gfx EGL GLES HLE renders reads back validates and reverses debug "
    "callback") {
  if (!have_angle()) return;
  EglSession egl;
  if (const auto* dump = std::getenv("VENTURE_GFX_PROCS_OUT"))
    test::dump_graphics_functions(dump);
  call("glClearColor", 0.25f, 0.5f, 0.75f, 1.0f);
  call("glClear", GL_COLOR_BUFFER_BIT);
  std::array<std::uint8_t, 4> pixel{};
  call("glReadPixels", 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
  CHECK(pixel[0] == 64);
  CHECK(pixel[1] >= 127);
  CHECK(pixel[1] <= 128);
  CHECK(pixel[2] == 191);
  CHECK(pixel[3] == 255);
  CHECK(call("glGetError") == GL_NO_ERROR);
  auto thunk = call("eglGetProcAddress", "glClearColor");
  CHECK(thunk != 0);
  CHECK(thunk != h2g(gfx::angle_proc("glClearColor")));
  // Host entry points resolve once by name, from any copy of the name.
  const std::string clear_color = "glClearColor";
  CHECK(gfx::angle_proc(clear_color.c_str()) == gfx::angle_proc("glClearColor"));
  CHECK(gfx::angle_proc("eglGetError") != nullptr);
  CHECK(gfx::angle_proc("glDoesNotExistVenture") == nullptr);
  CHECK(gfx::angle_proc("glDoesNotExistVenture") == nullptr);
  CHECK(call("eglGetProcAddress", "glDoesNotExistVenture") == 0);
  CHECK(call("eglGetProcAddress", "glImportMemoryFdEXT") == 0);
  GLint n{};
  call("glGetIntegerv", GL_NUM_EXTENSIONS, &n);
  CHECK(n > 0);
  const auto* all = g2h<const char>(call("glGetString", GL_EXTENSIONS));
  REQUIRE(all);
  for (GLint i = 0; i < n; ++i) {
    const auto* ext = g2h<const char>(call("glGetStringi", GL_EXTENSIONS, i));
    CHECK(gfx::has_extension(all, ext));
    CHECK((!std::string_view(ext).starts_with("GL_ANGLE_") ||
           std::string_view(ext) == "GL_ANGLE_multiview_multisample"));
  }
  CHECK(call("glGetStringi", GL_EXTENSIONS, n) == 0);
  CHECK(call("glGetError") == GL_INVALID_VALUE);
  call("glEnable", GL_DEBUG_OUTPUT);
  call("glEnable", GL_DEBUG_OUTPUT_SYNCHRONOUS);
  call("glDebugMessageCallback", test::graphics_callback(), 0xabcdefu);
  call("glDebugMessageControlKHR", GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0,
       0, GL_TRUE);
  call("glDebugMessageInsertKHR", GL_DEBUG_SOURCE_APPLICATION,
       GL_DEBUG_TYPE_MARKER, 17, GL_DEBUG_SEVERITY_NOTIFICATION, -1,
       "Venture callback test");
  auto callback = test::last_graphics_callback();
  CHECK(callback.function == test::graphics_callback());
  REQUIRE(callback.arguments.size() == 7);
  CHECK(callback.arguments[2] == 17);
  CHECK(callback.arguments[6] == 0xabcdefu);
  void* ptr{};
  call("glGetPointerv", GL_DEBUG_CALLBACK_FUNCTION, &ptr);
  CHECK(h2g(ptr) == test::graphics_callback());
  void* host_callback{};
  void* host_user{};
  auto host_query =
      gfx::angle_function<PFNGLGETPOINTERVKHRPROC>("glGetPointervKHR");
  host_query(GL_DEBUG_CALLBACK_FUNCTION, &host_callback);
  host_query(GL_DEBUG_CALLBACK_USER_PARAM, &host_user);
  test::GraphicsCallback from_worker;
  std::thread worker([&] {
    CHECK(GuestThread::current() == nullptr);
    reinterpret_cast<GLDEBUGPROCKHR>(host_callback)(
        GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_MARKER, 29,
        GL_DEBUG_SEVERITY_NOTIFICATION, 6, "worker", host_user);
    CHECK(GuestThread::current() != nullptr);
    from_worker = test::last_graphics_callback();
  });
  worker.join();
  CHECK(from_worker.function == test::graphics_callback());
  CHECK(from_worker.arguments[2] == 29);
  call("glDebugMessageCallback", 0, 0);
  CHECK(call("eglSwapBuffers", egl.display, egl.surface) == EGL_TRUE);
  CHECK(gfx::egl_frame_count() > 0);
  CHECK(call("eglDestroyContext", egl.display, 0x1234) == EGL_FALSE);
  CHECK(call("eglGetError") == EGL_BAD_CONTEXT);
  CHECK(call("eglGetError") == EGL_SUCCESS);
  CHECK(call("eglCreateImageKHR", egl.display, egl.context,
             EGL_NATIVE_BUFFER_ANDROID, 0, 0) == 0);
  CHECK(call("eglGetError") == EGL_BAD_PARAMETER);
  auto* window = android::create_native_window(32, 24);
  auto ws = g2h(
      call("eglCreateWindowSurface", egl.display, egl.config, h2g(window), 0));
  REQUIRE(ws);
  GLint width{};
  CHECK(call("eglQuerySurface", egl.display, ws, EGL_WIDTH, &width) ==
        EGL_TRUE);
  CHECK(width == 32);
  CHECK(window->refs == 2);
  CHECK(call("eglDestroySurface", egl.display, ws) == EGL_TRUE);
  CHECK(window->refs == 1);
  android::release(window);
  auto sync = g2h(call("eglCreateSyncKHR", egl.display, EGL_SYNC_FENCE_KHR, 0));
  REQUIRE(sync);
  CHECK(call("eglClientWaitSyncKHR", egl.display, sync,
             EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,
             EGL_FOREVER_KHR) == EGL_CONDITION_SATISFIED_KHR);
  CHECK(call("eglDestroySyncKHR", egl.display, sync) == EGL_TRUE);
  std::thread render_thread([&] {
    CHECK(call("eglGetCurrentContext") == 0);
    const EGLint attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    auto context = g2h(
        call("eglCreateContext", egl.display, egl.config, egl.context, attrs));
    const EGLint surface_attrs[] = {EGL_WIDTH, 2, EGL_HEIGHT, 2, EGL_NONE};
    auto surface = g2h(call("eglCreatePbufferSurface", egl.display, egl.config,
                            surface_attrs));
    CHECK(call("eglMakeCurrent", egl.display, surface, surface, context) ==
          EGL_TRUE);
    call("glClearColor", 1.0f, 0.0f, 0.0f, 1.0f);
    call("glClear", GL_COLOR_BUFFER_BIT);
    std::array<std::uint8_t, 4> color{};
    call("glReadPixels", 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, color.data());
    CHECK(color[0] == 255);
    call("eglReleaseThread");
    call("eglDestroySurface", egl.display, surface);
    call("eglDestroyContext", egl.display, context);
  });
  render_thread.join();
  CHECK(call("eglGetCurrentContext") == h2g(egl.context));
}
namespace {
// Reference BC7 mode 6 decode of one block (texel order row-major).
std::array<std::uint8_t, 64> decode_bc7_mode6(const std::uint8_t* block) {
  unsigned position = 0;
  auto bits = [&](unsigned count) {
    unsigned value = 0;
    for (unsigned i = 0; i < count; ++i, ++position)
      value |= unsigned(block[position / 8] >> (position % 8) & 1) << i;
    return value;
  };
  REQUIRE(bits(7) == 64);
  unsigned e[2][4];
  for (int c = 0; c < 4; ++c) {
    e[0][c] = bits(7);
    e[1][c] = bits(7);
  }
  const unsigned p0 = bits(1), p1 = bits(1);
  constexpr unsigned weights[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
  std::array<std::uint8_t, 64> out{};
  for (unsigned i = 0; i < 16; ++i) {
    const unsigned w = weights[bits(i ? 4 : 3)];
    for (int c = 0; c < 4; ++c) {
      const unsigned a = e[0][c] * 2 + p0, b = e[1][c] * 2 + p1;
      out[i * 4 + c] = std::uint8_t((a * (64 - w) + b * w + 32) >> 6);
    }
  }
  return out;
}
}  // namespace
TEST_CASE("gfx BC7 encode keeps gradients close and cutout alpha exact") {
  // 6x6: partial blocks repeat the edge texels.
  const unsigned w = 6, h = 6;
  std::vector<std::uint8_t> rgba(w * h * 4 * 2);
  for (unsigned layer = 0; layer < 2; ++layer)
    for (unsigned y = 0; y < h; ++y)
      for (unsigned x = 0; x < w; ++x) {
        auto* t = &rgba[((layer * h + y) * w + x) * 4];
        // Slice 0: an opaque gradient along one color line; slice 1: one color
        // under a binary cutout. Mode 6 fits one line per block.
        t[0] = std::uint8_t(layer ? 180 : (x + y) * 20);
        t[1] = std::uint8_t(layer ? 40 : 200 - (x + y) * 15);
        t[2] = 90;
        t[3] = !layer || (x + y) % 3 ? 255 : 0;
      }
  auto blocks = gfx::encode_bc7(w, h, 2, rgba);
  REQUIRE(blocks.size() == 4 * 2 * 16);
  for (unsigned layer = 0; layer < 2; ++layer)
    for (unsigned b = 0; b < 4; ++b) {
      auto texels = decode_bc7_mode6(&blocks[(layer * 4 + b) * 16]);
      for (unsigned i = 0; i < 16; ++i) {
        const unsigned x = (b % 2) * 4 + i % 4, y = (b / 2) * 4 + i / 4;
        if (x >= w || y >= h) continue;
        const auto* t = &rgba[((layer * h + y) * w + x) * 4];
        CHECK(texels[i * 4 + 3] == t[3]);
        // Colors under alpha 0 are free; visible ones stay close.
        if (t[3])
          for (int c = 0; c < 3; ++c) CHECK(std::abs(int(texels[i * 4 + c]) - int(t[c])) <= 6);
      }
    }
  CHECK_THROWS(gfx::encode_bc7(4, 4, 1, std::span(rgba).first(10)));
  // The transcode matches decode-then-encode, and its cache returns the same.
  const auto block = constant_block(37, 83, 171, 209);
  const auto expected = gfx::encode_bc7(4, 4, 1, gfx::decode_astc(0x93B0, 4, 4, 1, block));
  CHECK(gfx::transcode_astc_bc7(0x93B0, 4, 4, 1, block) == expected);
  auto cache = std::filesystem::temp_directory_path() /
               ("Venture-bc7-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  CHECK(gfx::transcode_astc_bc7(0x93B0, 4, 4, 1, block, cache) == expected);
  CHECK(gfx::transcode_astc_bc7(0x93B0, 4, 4, 1, block, cache) == expected);
  unsigned files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(cache)) files += entry.path().extension() == ".bc7";
  CHECK(files == 1);
  std::error_code ec;
  std::filesystem::remove_all(cache, ec);
}
TEST_CASE("gfx ASTC fallback HLE uploads subuploads storage array and PBO") {
  if (!have_angle()) return;
  EglSession egl;
  GLuint texture{}, fbo{};
  call("glGenTextures", 1, &texture);
  call("glBindTexture", GL_TEXTURE_2D, texture);
  auto red = constant_block(255, 0, 0);
  call("glCompressedTexImage2D", GL_TEXTURE_2D, 0, 0x93B0, 4, 4, 0, 16,
       red.data());
  CHECK(call("glGetError") == GL_NO_ERROR);
  GLint format{}, compressed{};
  call("glGetTexLevelParameteriv", GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT,
       &format);
  call("glGetTexLevelParameteriv", GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED,
       &compressed);
  CHECK(format == 0x93B0);
  CHECK(compressed == GL_TRUE);
  // The guest reports the compressed format, but ANGLE must retain main's
  // eight-bit LDR backing rather than the held HDR fallback's FP16 storage.
  if (gfx::current_gl_state()->astc_fallback) {
    GLint native_format{};
    gfx::angle_function<PFNGLGETTEXLEVELPARAMETERIVPROC, "glGetTexLevelParameteriv">()(
        GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &native_format);
    const std::string storage = gfx::current_gl_state()->bc7_fallback ? "BC7" : "RGBA8";
    MESSAGE("ASTC fallback storage: " << storage);
    CHECK(native_format == (gfx::current_gl_state()->bc7_fallback
                                ? GL_COMPRESSED_RGBA_BPTC_UNORM_EXT
                                : GL_RGBA8));
  }
  // BC7 mode 6 endpoints share one p-bit across channels: opaque red is 255,1,1.
  auto near = [](std::array<std::uint8_t, 4> a, std::array<std::uint8_t, 4> b) {
    for (int c = 0; c < 4; ++c)
      if (std::abs(int(a[c]) - int(b[c])) > (c == 3 ? 0 : 1)) return false;
    return true;
  };
  auto p = sample_texture(texture);
  CHECK(near(p, {255, 0, 0, 255}));
  call("glGenFramebuffers", 1, &fbo);
  call("glBindFramebuffer", GL_FRAMEBUFFER, fbo);
  call("glFramebufferTexture2D", GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
       GL_TEXTURE_2D, texture, 0);
  CHECK(call("glCheckFramebufferStatus", GL_FRAMEBUFFER) ==
        GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT);
  call("glClear", GL_COLOR_BUFFER_BIT);
  CHECK(call("glGetError") == GL_INVALID_FRAMEBUFFER_OPERATION);
  // The admitted draw must observe storage changes made through a shared
  // context, including cached ineligible -> eligible -> ineligible transitions.
  const EGLint shared_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  auto shared = g2h(call("eglCreateContext", egl.display, egl.config, egl.context,
                        shared_attributes));
  REQUIRE(shared);
  REQUIRE(call("eglMakeCurrent", egl.display, egl.surface, egl.surface, shared) == EGL_TRUE);
  call("glBindTexture", GL_TEXTURE_2D, texture);
  call("glTexImage2D", GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA,
       GL_UNSIGNED_BYTE, 0);
  CHECK(call("glGetError") == GL_NO_ERROR);
  REQUIRE(call("eglMakeCurrent", egl.display, egl.surface, egl.surface, egl.context) == EGL_TRUE);
  call("glClear", GL_COLOR_BUFFER_BIT);
  CHECK(call("glGetError") == GL_NO_ERROR);
  REQUIRE(call("eglMakeCurrent", egl.display, egl.surface, egl.surface, shared) == EGL_TRUE);
  call("glCompressedTexImage2D", GL_TEXTURE_2D, 0, 0x93B0, 4, 4, 0, 16, red.data());
  CHECK(call("glGetError") == GL_NO_ERROR);
  REQUIRE(call("eglMakeCurrent", egl.display, egl.surface, egl.surface, egl.context) == EGL_TRUE);
  call("glClear", GL_COLOR_BUFFER_BIT);
  CHECK(call("glGetError") == GL_INVALID_FRAMEBUFFER_OPERATION);
  CHECK(call("eglDestroyContext", egl.display, shared) == EGL_TRUE);
  call("glDeleteFramebuffers", 1, &fbo);
  fbo = 0;
  auto green = constant_block(0, 255, 0);
  call("glCompressedTexSubImage2D", GL_TEXTURE_2D, 0, 0, 0, 4, 4, 0x93B0, 16,
       green.data());
  p = sample_texture(texture);
  CHECK(near(p, {0, 255, 0, 255}));
  call("glCompressedTexSubImage2D", GL_TEXTURE_2D, 0, 1, 0, 3, 4, 0x93B0, 16,
       green.data());
  CHECK(call("glGetError") == GL_INVALID_OPERATION);
  call("glDeleteTextures", 1, &texture);
  call("glDeleteFramebuffers", 1, &fbo);
  call("glGenTextures", 1, &texture);
  call("glBindTexture", GL_TEXTURE_2D_ARRAY, texture);
  call("glTexStorage3D", GL_TEXTURE_2D_ARRAY, 1, 0x93D0, 4, 4, 2);
  GLuint buffer{};
  call("glGenBuffers", 1, &buffer);
  call("glBindBuffer", GL_PIXEL_UNPACK_BUFFER, buffer);
  call("glBufferData", GL_PIXEL_UNPACK_BUFFER, 16, red.data(), GL_STATIC_DRAW);
  call("glCompressedTexSubImage3D", GL_TEXTURE_2D_ARRAY, 0, 0, 0, 1, 4, 4, 1,
       0x93D0, 16, 0);
  CHECK(call("glGetError") == GL_NO_ERROR);
  call("glBindBuffer", GL_PIXEL_UNPACK_BUFFER, 0);
  call("glGetTexLevelParameteriv", GL_TEXTURE_2D_ARRAY, 0,
       GL_TEXTURE_INTERNAL_FORMAT, &format);
  CHECK(format == 0x93D0);
  call("glDeleteBuffers", 1, &buffer);
  call("glDeleteTextures", 1, &texture);
  // 6x6 footprints: a region at texel 6 is off the 4x4 grid BC7 storage uses.
  call("glGenTextures", 1, &texture);
  call("glBindTexture", GL_TEXTURE_2D, texture);
  call("glTexStorage2D", GL_TEXTURE_2D, 1, 0x93B4, 12, 12);
  for (GLint y : {0, 6})
    for (GLint x : {0, 6})
      call("glCompressedTexSubImage2D", GL_TEXTURE_2D, 0, x, y, 6, 6, 0x93B4, 16,
           (x || y ? green : red).data());
  CHECK(call("glGetError") == GL_NO_ERROR);
  call("glDeleteTextures", 1, &texture);
}
TEST_CASE("gfx ANGLE ETC2 EAC fallback sampled readback") {
  if (!have_angle()) return;
  EglSession egl;
  // ETC2 individual RGB mode: zero base, modifier +2 in every texel. EAC
  // unsigned maximum block saturates every R11 component to 1.0.
  for (GLenum format : {GL_COMPRESSED_RGB8_ETC2, GL_COMPRESSED_R11_EAC,
                        GL_COMPRESSED_RG11_EAC}) {
    std::array<std::uint8_t, 16> blocks{};
    if (format != GL_COMPRESSED_RGB8_ETC2) blocks.fill(255);
    GLuint texture{};
    call("glGenTextures", 1, &texture);
    call("glBindTexture", GL_TEXTURE_2D, texture);
    call("glCompressedTexImage2D", GL_TEXTURE_2D, 0, format, 4, 4, 0,
         format == GL_COMPRESSED_RG11_EAC ? 16 : 8, blocks.data());
    CHECK(call("glGetError") == GL_NO_ERROR);
    auto pixel = sample_texture(texture);
    CHECK(pixel[0] == (format == GL_COMPRESSED_RGB8_ETC2 ? 2 : 255));
    CHECK(pixel[1] == (format == GL_COMPRESSED_RGB8_ETC2  ? 2
                       : format == GL_COMPRESSED_RG11_EAC ? 255
                                                          : 0));
    CHECK(pixel[2] == (format == GL_COMPRESSED_RGB8_ETC2 ? 2 : 0));
    CHECK(pixel[3] == 255);
    call("glDeleteTextures", 1, &texture);
  }
}

namespace {
template <class T>
T vkfn(const char* name) {
  auto& gpu = gfx::gpu_device();
  return reinterpret_cast<T>(gpu.get_instance_proc_addr(gpu.instance, name));
}
std::uint32_t memory_type(std::uint32_t mask, VkMemoryPropertyFlags flags) {
  VkPhysicalDeviceMemoryProperties properties{};
  vkfn<PFN_vkGetPhysicalDeviceMemoryProperties>(
      "vkGetPhysicalDeviceMemoryProperties")(gfx::gpu_device().physical_device,
                                             &properties);
  for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i)
    if ((mask & (1u << i)) &&
        (properties.memoryTypes[i].propertyFlags & flags) == flags)
      return i;
  FAIL("No compatible Vulkan memory type");
  return 0;
}
struct CopyTarget {
  VkImage image{};
  VkDeviceMemory image_memory{}, buffer_memory{};
  VkBuffer buffer{};
  VkCommandPool pool{};
  VkCommandBuffer command{};
  VkFence fence{};
  std::uint32_t layers, width, height, levels;
  VkDeviceSize bytes = 0;
  explicit CopyTarget(std::uint32_t layer_count, std::uint32_t w = 4, std::uint32_t h = 4,
                      std::uint32_t mips = 1, VkFormat format = VK_FORMAT_R8G8B8A8_UNORM)
      : layers(layer_count), width(w), height(h), levels(mips) {
    for (std::uint32_t level = 0; level < levels; ++level)
      bytes += VkDeviceSize(std::max(1u, width >> level)) * std::max(1u, height >> level) * 4 * layers;
    auto d = gfx::gpu_device().device;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = levels;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    REQUIRE(vkfn<PFN_vkCreateImage>("vkCreateImage")(d, &info, nullptr,
                                                     &image) == VK_SUCCESS);
    VkMemoryRequirements req{};
    vkfn<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(
        d, image, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, 0);
    REQUIRE(vkfn<PFN_vkAllocateMemory>("vkAllocateMemory")(
                d, &alloc, nullptr, &image_memory) == VK_SUCCESS);
    REQUIRE(vkfn<PFN_vkBindImageMemory>("vkBindImageMemory")(
                d, image, image_memory, 0) == VK_SUCCESS);
    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size = bytes;
    bc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    REQUIRE(vkfn<PFN_vkCreateBuffer>("vkCreateBuffer")(d, &bc, nullptr,
                                                       &buffer) == VK_SUCCESS);
    vkfn<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(
        d, buffer, &req);
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memory_type(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    REQUIRE(vkfn<PFN_vkAllocateMemory>("vkAllocateMemory")(
                d, &alloc, nullptr, &buffer_memory) == VK_SUCCESS);
    REQUIRE(vkfn<PFN_vkBindBufferMemory>("vkBindBufferMemory")(
                d, buffer, buffer_memory, 0) == VK_SUCCESS);
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pc.queueFamilyIndex = gfx::gpu_device().queue_family;
    REQUIRE(vkfn<PFN_vkCreateCommandPool>("vkCreateCommandPool")(
                d, &pc, nullptr, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo cb{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb.commandPool = pool;
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb.commandBufferCount = 1;
    REQUIRE(vkfn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(
                d, &cb, &command) == VK_SUCCESS);
    VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    REQUIRE(vkfn<PFN_vkCreateFence>("vkCreateFence")(d, &fc, nullptr, &fence) ==
            VK_SUCCESS);
  }
  std::vector<std::uint8_t> read(std::uint64_t* before_read_ns = nullptr) {
    auto& gpu = gfx::gpu_device();
    VkQueryPool query{};
    if (before_read_ns) {
      VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      info.queryType = VK_QUERY_TYPE_TIMESTAMP;
      info.queryCount = 1;
      REQUIRE(vkfn<PFN_vkCreateQueryPool>("vkCreateQueryPool")(
          gpu.device, &info, nullptr, &query) == VK_SUCCESS);
    }
    vkfn<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(command, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    REQUIRE(vkfn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(
                command, &begin) == VK_SUCCESS);
    if (query) {
      vkfn<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool")(command, query, 0, 1);
      vkfn<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp")(
          command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query, 0);
    }
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
    auto barrier = vkfn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    barrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    std::vector<VkBufferImageCopy> copies(levels);
    VkDeviceSize offset = 0;
    for (std::uint32_t level = 0; level < levels; ++level) {
      auto& copy = copies[level];
      copy.bufferOffset = offset;
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, layers};
      copy.imageExtent = {std::max(1u, width >> level), std::max(1u, height >> level), 1};
      offset += VkDeviceSize(copy.imageExtent.width) * copy.imageExtent.height * 4 * layers;
    }
    vkfn<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
        command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, levels, copies.data());
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    std::swap(b.oldLayout, b.newLayout);
    barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
            &b);
    REQUIRE(vkfn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command) ==
            VK_SUCCESS);
    vkfn<PFN_vkResetFences>("vkResetFences")(gpu.device, 1, &fence);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    {
      std::lock_guard lock(gpu.queue_lock);
      REQUIRE(gfx::lock_angle_queue());
      auto r = vkfn<PFN_vkQueueSubmit>("vkQueueSubmit")(gpu.queue, 1, &submit,
                                                        fence);
      gfx::unlock_angle_queue();
      REQUIRE(r == VK_SUCCESS);
    }
    REQUIRE(vkfn<PFN_vkWaitForFences>("vkWaitForFences")(
                gpu.device, 1, &fence, VK_TRUE, 10000000000) == VK_SUCCESS);
    if (query) {
      std::uint64_t ticks{};
      REQUIRE(vkfn<PFN_vkGetQueryPoolResults>("vkGetQueryPoolResults")(
          gpu.device, query, 0, 1, sizeof(ticks), &ticks, sizeof(ticks),
          VK_QUERY_RESULT_64_BIT) == VK_SUCCESS);
      VkPhysicalDeviceProperties properties{};
      vkfn<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(
          gpu.physical_device, &properties);
      *before_read_ns = std::uint64_t(double(ticks) * properties.limits.timestampPeriod);
      vkfn<PFN_vkDestroyQueryPool>("vkDestroyQueryPool")(gpu.device, query, nullptr);
    }
    void* data{};
    REQUIRE(vkfn<PFN_vkMapMemory>("vkMapMemory")(gpu.device, buffer_memory, 0,
                                                 bytes, 0,
                                                 &data) == VK_SUCCESS);
    auto* p = static_cast<std::uint8_t*>(data);
    std::vector<std::uint8_t> result(p, p + bytes);
    vkfn<PFN_vkUnmapMemory>("vkUnmapMemory")(gpu.device, buffer_memory);
    return result;
  }
  ~CopyTarget() {
    auto d = gfx::gpu_device().device;
    vkfn<PFN_vkDestroyFence>("vkDestroyFence")(d, fence, nullptr);
    vkfn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(d, pool, nullptr);
    vkfn<PFN_vkDestroyBuffer>("vkDestroyBuffer")(d, buffer, nullptr);
    vkfn<PFN_vkDestroyImage>("vkDestroyImage")(d, image, nullptr);
    vkfn<PFN_vkFreeMemory>("vkFreeMemory")(d, buffer_memory, nullptr);
    vkfn<PFN_vkFreeMemory>("vkFreeMemory")(d, image_memory, nullptr);
  }
};
}  // namespace
TEST_CASE(
    "gfx XR bridge copies and reacquires real Vulkan images including stereo "
    "arrays") {
  if (!have_angle()) return;
  EglSession egl;
  for (std::uint32_t layers : {1u, 2u}) {
    CopyTarget target(layers);
    gfx::XrImageDescription description;
    description.width = 4;
    description.height = 4;
    description.layers = layers;
    description.images = {&target.image, 1};
    std::string error;
    auto bridge = gfx::XrImageBridge::create(description, &error);
    INFO(error);
    REQUIRE(bridge);
    CHECK_FALSE(bridge->release(0, &error));
    GLuint external{};
    if (layers > 1) {
      call("glGenTextures", 1, &external);
      call("glBindTexture", GL_TEXTURE_2D_ARRAY, external);
      call("glTexStorage3D", GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 4, 4, layers);
    }
    GLuint fbo{};
    GLuint end_query{};
    gfx::angle_function<PFNGLGENQUERIESEXTPROC>("glGenQueriesEXT")(1, &end_query);
    call("glGenFramebuffers", 1, &fbo);
    call("glBindFramebuffer", GL_FRAMEBUFFER, fbo);
    for (int frame = 0; frame < 2; ++frame) {
      REQUIRE(bridge->acquire(0, &error));
      const GLuint source = frame == 1 && external ? external : bridge->textures()[0];
      if (layers == 1 && frame == 0)
        CHECK_FALSE(bridge->release(0, &error, true, source));
      for (GLuint layer = 0; layer < layers; ++layer) {
        if (layers == 1)
          call("glFramebufferTexture2D", GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
               GL_TEXTURE_2D, source, 0);
        else
          call("glFramebufferTextureLayer", GL_FRAMEBUFFER,
               GL_COLOR_ATTACHMENT0, source, 0, layer);
        REQUIRE(call("glCheckFramebufferStatus", GL_FRAMEBUFFER) ==
                GL_FRAMEBUFFER_COMPLETE);
        call("glClearColor", layer == 0 ? 1.0f : 0.0f, layer == 1 ? 1.0f : 0.0f,
             static_cast<float>(frame), 1.0f);
        call("glClear", GL_COLOR_BUFFER_BIT);
        // Distinct GL top row verifies the Vulkan output orientation, not just color.
        call("glEnable", GL_SCISSOR_TEST);
        call("glScissor", 0, 3, 4, 1);
        call("glClearColor", 0.0f, 0.0f, 1.0f, 1.0f);
        call("glClear", GL_COLOR_BUFFER_BIT);
        call("glDisable", GL_SCISSOR_TEST);
      }
      REQUIRE(bridge->release(0, &error, true, source == external ? external : 0, end_query));
      std::uint64_t before_read_ns{};
      auto bytes = target.read(&before_read_ns);
      // The host's fence has completed, but no subsequent GL frame or flush
      // has submitted a deferred timestamp on our behalf.
      GLuint ready{};
      gfx::angle_function<PFNGLGETQUERYOBJECTUIVEXTPROC>("glGetQueryObjectuivEXT")(
          end_query, GL_QUERY_RESULT_AVAILABLE_EXT, &ready);
      CHECK(ready == GL_TRUE);
      GLuint64 end_ns{};
      gfx::angle_function<PFNGLGETQUERYOBJECTUI64VEXTPROC>("glGetQueryObjectui64vEXT")(
          end_query, GL_QUERY_RESULT_EXT, &end_ns);
      CHECK(end_ns <= before_read_ns);
      for (std::uint32_t layer = 0; layer < layers; ++layer)
        for (std::size_t p = 64 * layer; p < 64 * (layer + 1); p += 4) {
          const bool top = p % 64 < 16;
          CHECK(bytes[p] == (!top && layer == 0 ? 255 : 0));
          CHECK(bytes[p + 1] == (!top && layer == 1 ? 255 : 0));
          CHECK(bytes[p + 2] == (top ? 255 : frame * 255));
          CHECK(bytes[p + 3] == 255);
        }
    }
    call("glDeleteFramebuffers", 1, &fbo);
    gfx::angle_function<PFNGLDELETEQUERIESEXTPROC>("glDeleteQueriesEXT")(1, &end_query);
    bridge.reset();
    if (external) call("glDeleteTextures", 1, &external);
  }
}

TEST_CASE("gfx external XR subresources omit aggregate storage and preserve every eye and mip") {
  if (!have_angle()) return;
  EglSession egl;
  for (std::uint32_t levels : {1u, 12u}) {
    CopyTarget target(2, 2800, 2856, levels);
    gfx::XrImageDescription desc;
    desc.width = 2800; desc.height = 2856; desc.layers = 2; desc.mip_count = levels;
    desc.images = {&target.image, 1};
    std::string error;
    auto ordinary = gfx::XrImageBridge::create(desc, &error);
    REQUIRE_MESSAGE(ordinary, error);
    desc.external_subresources = true;
    auto external = gfx::XrImageBridge::create(desc, &error);
    REQUIRE_MESSAGE(external, error);
    const auto before = ordinary->storage_census(), after = external->storage_census();
    CHECK(before.aggregate_textures == 1);
    CHECK(after.aggregate_textures == 0);
    CHECK(before.subresource_textures == 2 * levels);
    CHECK(after.subresource_textures == before.subresource_textures);
    CHECK(after.subresource_bytes == before.subresource_bytes);
    CHECK(before.aggregate_bytes == before.subresource_bytes);
    CHECK(after.aggregate_bytes == 0);
    CHECK(external->textures().empty());
    MESSAGE("PCVR omitted aggregate payload: ", before.aggregate_bytes, " bytes; levels=", levels);
  }
  GLuint source{};
  call("glGenTextures", 1, &source);
  call("glBindTexture", GL_TEXTURE_2D_ARRAY, source);
  call("glTexStorage3D", GL_TEXTURE_2D_ARRAY, 4, GL_RGBA8, 8, 4, 2);
  for (unsigned level = 0; level < 4; ++level) {
    const auto w = std::max(1u, 8u >> level), h = std::max(1u, 4u >> level);
    std::vector<std::uint8_t> pixels(w * h * 2 * 4);
    for (unsigned layer = 0; layer < 2; ++layer)
      for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
        const std::array<std::uint8_t, 4> color{std::uint8_t(20 * level),
            std::uint8_t(80 * layer), std::uint8_t(30 * y + x), 255};
        std::copy(color.begin(), color.end(), pixels.begin() + ((layer * h + y) * w + x) * 4);
      }
    call("glTexSubImage3D", GL_TEXTURE_2D_ARRAY, level, 0, 0, 0, w, h, 2,
         GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  }
  for (bool bottom_up : {false, true}) {
    std::vector<std::uint8_t> expected;
    for (bool external : {false, true}) {
      CopyTarget target(2, 8, 4, 4);
      gfx::XrImageDescription desc;
      desc.width = 8; desc.height = 4; desc.layers = 2; desc.mip_count = 4;
      desc.images = {&target.image, 1}; desc.external_subresources = external;
      std::string error;
      auto bridge = gfx::XrImageBridge::create(desc, &error);
      REQUIRE_MESSAGE(bridge, error);
      REQUIRE(bridge->acquire(0, &error));
      if (external) CHECK_FALSE(bridge->release(0, &error, bottom_up));
      REQUIRE_MESSAGE(bridge->release(0, &error, bottom_up, source), error);
      auto pixels = target.read();
      if (!external) expected = pixels;
      else CHECK(pixels == expected);
    }
  }
  call("glDeleteTextures", 1, &source);
  CHECK(call("glGetError") == GL_NO_ERROR);
}
TEST_CASE("gfx multiview eye layers survive MSAA resolve and XR orientation conversion") {
  if (!have_angle()) return;
  EglSession egl;
  const auto* extensions = g2h<const char>(call("glGetString", GL_EXTENSIONS));
  REQUIRE(gfx::has_extension(extensions, "GL_OVR_multiview"));
  REQUIRE(gfx::has_extension(extensions, "GL_OVR_multiview2"));
  const bool msaa = gfx::has_extension(extensions, "GL_OVR_multiview_multisampled_render_to_texture");
  REQUIRE(gfx::has_extension(extensions, "GL_ANGLE_multiview_multisample"));
  MESSAGE("Multiview MSAA exposed: ", msaa);
  const char* sources[]{
      "#version 300 es\n#extension GL_OVR_multiview2 : require\nlayout(num_views=2) in; flat out uint eye; void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.-1.,0,1);eye=gl_ViewID_OVR;}",
      "#version 300 es\n#extension GL_OVR_multiview2 : require\nprecision highp float;flat in highp uint eye;out vec4 color;void main(){color=vec4(eye==0u?1.:0.,eye==1u?1.:0.,gl_FragCoord.y>2.?1.:0.,1.);}"};
  GLuint shaders[2];
  auto program = call("glCreateProgram");
  for (int i=0; i<2; ++i) {
    shaders[i] = static_cast<GLuint>(call("glCreateShader", i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER));
    call("glShaderSource", shaders[i], 1, &sources[i], 0); call("glCompileShader", shaders[i]);
    GLint compiled{}; call("glGetShaderiv", shaders[i], GL_COMPILE_STATUS, &compiled); REQUIRE(compiled);
    call("glAttachShader", program, shaders[i]);
  }
  call("glLinkProgram", program);
  GLint linked{}; call("glGetProgramiv", program, GL_LINK_STATUS, &linked); REQUIRE(linked);
  call("glUseProgram", program);
  for (int mode : {0, 1, 2}) {
    if (mode == 1 && !msaa) continue;
    CAPTURE(mode);
    CopyTarget target(2);
    REQUIRE(xr::install_angle_xr_images());
    auto* provider = xr::xr_images();
    xr::ImageDesc description{GL_RGBA8, VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 2, 1, 1, 1};
    GLuint texture{};
    REQUIRE(provider->create_ring(description, {&texture, 1}) == XR_SUCCESS);
    GLuint multisample{};
    if (mode == 2) {
      call("glGenTextures", 1, &multisample);
      call("glBindTexture", GL_TEXTURE_2D_MULTISAMPLE_ARRAY, multisample);
      call("glTexStorage3DMultisampleOES", GL_TEXTURE_2D_MULTISAMPLE_ARRAY, 4,
           GL_RGBA8, 4, 4, 2, GL_TRUE);
      REQUIRE(call("glGetError") == GL_NO_ERROR);
    }
    GLuint fbo{}; call("glGenFramebuffers", 1, &fbo); call("glBindFramebuffer", GL_FRAMEBUFFER, fbo);
    if (mode != 1)
      call("glFramebufferTextureMultiviewOVR", GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, mode == 2 ? multisample : texture, 0, 0, 2);
    else
      call("glFramebufferTextureMultisampleMultiviewOVR", GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 0, 4, 0, 2);
    REQUIRE(call("glGetError") == GL_NO_ERROR);
    REQUIRE(call("glCheckFramebufferStatus", GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    GLuint depth{};
    if (mode == 1) {
      call("glGenTextures", 1, &depth);
      call("glBindTexture", GL_TEXTURE_2D_ARRAY, depth);
      call("glTexStorage3D", GL_TEXTURE_2D_ARRAY, 1, GL_DEPTH24_STENCIL8, 4, 4, 2);
      call("glFramebufferTextureMultisampleMultiviewOVR", GL_FRAMEBUFFER,
           GL_DEPTH_STENCIL_ATTACHMENT, depth, 0, 4, 0, 2);
      REQUIRE(call("glCheckFramebufferStatus", GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
      call("glEnable", GL_DEPTH_TEST);
      call("glClearDepthf", 1.f);
      call("glClear", GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
    call("glViewport", 0, 0, 4, 4); call("glClearColor", 0.f, 0.f, 0.f, 1.f);
    call("glClear", GL_COLOR_BUFFER_BIT); call("glDrawArrays", GL_TRIANGLES, 0, 3);
    REQUIRE(call("glGetError") == GL_NO_ERROR);
    if (mode == 2) {
      // OVR_multiview forbids blitting a multiview FBO. Resolve its array
      // layers separately, as required for explicit multisample textures.
      GLuint resolve[2]{};
      call("glGenFramebuffers", 2, resolve);
      for (int layer = 0; layer < 2; ++layer) {
        call("glBindFramebuffer", GL_READ_FRAMEBUFFER, resolve[0]);
        call("glFramebufferTextureLayer", GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, multisample, 0, layer);
        call("glBindFramebuffer", GL_DRAW_FRAMEBUFFER, resolve[1]);
        call("glFramebufferTextureLayer", GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 0, layer);
        REQUIRE(call("glCheckFramebufferStatus", GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
        REQUIRE(call("glCheckFramebufferStatus", GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
        call("glBlitFramebuffer", 0, 0, 4, 4, 0, 0, 4, 4, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        REQUIRE(call("glGetError") == GL_NO_ERROR);
      }
      call("glDeleteFramebuffers", 2, resolve);
    }
    REQUIRE(provider->copy_to_image(texture, target.image, description, 0, true) == XR_SUCCESS);
    auto bytes = target.read();
    for (unsigned layer=0; layer<2; ++layer)
      for (unsigned pixel=0; pixel<16; ++pixel) {
        const auto p = layer*64 + pixel*4;
        CHECK(bytes[p] == (layer == 0 ? 255 : 0));
        CHECK(bytes[p+1] == (layer == 1 ? 255 : 0));
        CHECK(bytes[p+2] == (pixel < 8 ? 255 : 0));
      }
    if (mode == 1) {
      // Sampling/copying resolves the implicit MSAA image. A later partial
      // update must preserve the resolved contents of both views.
      call("glBindFramebuffer", GL_FRAMEBUFFER, fbo);
      call("glEnable", GL_SCISSOR_TEST);
      call("glScissor", 0, 0, 1, 4);
      call("glClearColor", 0.f, 0.f, 1.f, 1.f);
      call("glClear", GL_COLOR_BUFFER_BIT);
      call("glDisable", GL_SCISSOR_TEST);
      call("glDisable", GL_DEPTH_TEST);
      REQUIRE(call("glGetError") == GL_NO_ERROR);
      REQUIRE(provider->copy_to_image(texture, target.image, description, 0, true) == XR_SUCCESS);
      bytes = target.read();
      for (unsigned layer = 0; layer < 2; ++layer)
        for (unsigned pixel = 0; pixel < 16; ++pixel) {
          const auto p = layer * 64 + pixel * 4;
          const bool cleared = pixel % 4 == 0;
          CHECK(bytes[p] == (!cleared && layer == 0 ? 255 : 0));
          CHECK(bytes[p+1] == (!cleared && layer == 1 ? 255 : 0));
          CHECK(bytes[p+2] == (cleared || pixel < 8 ? 255 : 0));
        }
    }
    call("glBindFramebuffer", GL_FRAMEBUFFER, 0); call("glDeleteFramebuffers", 1, &fbo);
    if (depth) call("glDeleteTextures", 1, &depth);
    if (multisample) call("glDeleteTextures", 1, &multisample);
    REQUIRE(provider->release_images({&target.image, 1}) == XR_SUCCESS);
    provider->destroy_ring({&texture, 1});
  }
  call("glUseProgram", 0); call("glDeleteProgram", program);
  for (auto shader : shaders) call("glDeleteShader", shader);
}

TEST_CASE("gfx guest no-error contexts follow KHR_no_error over validated ANGLE") {
  if (!have_angle()) return;
  EglSession egl(3, true);
  const auto* extensions = g2h<const char>(call("eglQueryString", egl.display, EGL_EXTENSIONS));
  REQUIRE(extensions);
  CHECK(gfx::has_extension(extensions, "EGL_KHR_create_context_no_error"));
  GLint flags{};
  call("glGetIntegerv", GL_CONTEXT_FLAGS, &flags);
  CHECK((flags & GL_CONTEXT_FLAG_NO_ERROR_BIT_KHR) != 0);
  // Unity's ClearBuffer on a pooled, never-allocated buffer: binds 0, maps,
  // and writes unconditionally. Errors are undefined behaviour here; the
  // write must land somewhere harmless, as on Quest's no-error driver.
  call("glBindBuffer", GL_COPY_WRITE_BUFFER, 0);
  auto* scratch = g2h<std::uint8_t>(call("glMapBufferRange", GL_COPY_WRITE_BUFFER, 0, 4096,
                                         GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT));
  REQUIRE(scratch);
  std::memset(scratch, 0, 4096);
  CHECK(call("glUnmapBuffer", GL_COPY_WRITE_BUFFER) == GL_TRUE);
  CHECK(call("glGetError") == GL_NO_ERROR);
  // Real maps are untouched, also after a discarded map left on the target.
  GLuint buffer{};
  call("glGenBuffers", 1, &buffer);
  call("glBindBuffer", GL_ARRAY_BUFFER, 0);
  REQUIRE(call("glMapBufferRange", GL_ARRAY_BUFFER, 0, 16, GL_MAP_WRITE_BIT) != 0);
  call("glBindBuffer", GL_ARRAY_BUFFER, buffer);
  call("glBufferData", GL_ARRAY_BUFFER, 16, 0, GL_DYNAMIC_DRAW);
  auto* data = g2h<std::uint8_t>(call("glMapBufferRange", GL_ARRAY_BUFFER, 0, 16, GL_MAP_WRITE_BIT));
  REQUIRE(data);
  std::memset(data, 0x5a, 16);
  CHECK(call("glUnmapBuffer", GL_ARRAY_BUFFER) == GL_TRUE);
  GLint mapped = -1;
  call("glGetBufferParameteriv", GL_ARRAY_BUFFER, GL_BUFFER_MAPPED, &mapped);
  CHECK(mapped == GL_FALSE);
  data = g2h<std::uint8_t>(call("glMapBufferRange", GL_ARRAY_BUFFER, 0, 16, GL_MAP_READ_BIT));
  REQUIRE(data);
  CHECK(std::all_of(data, data + 16, [](std::uint8_t b) { return b == 0x5a; }));
  CHECK(call("glUnmapBuffer", GL_ARRAY_BUFFER) == GL_TRUE);
  CHECK(call("glGetError") == GL_NO_ERROR);
  // EGL_KHR_create_context_no_error: BAD_MATCH for a share group mismatch and
  // for no-error debug or robustness contexts; only TRUE/FALSE are values.
  auto refused = [&](EGLContext share, std::initializer_list<EGLint> list) {
    std::vector<EGLint> attrs{EGL_CONTEXT_CLIENT_VERSION, 3};
    attrs.insert(attrs.end(), list);
    attrs.push_back(EGL_NONE);
    return call("eglCreateContext", egl.display, egl.config, share, attrs.data()) == 0 ? call("eglGetError") : 0;
  };
  CHECK(refused(egl.context, {}) == EGL_BAD_MATCH);
  CHECK(refused(egl.context, {EGL_CONTEXT_OPENGL_NO_ERROR_KHR, EGL_FALSE}) == EGL_BAD_MATCH);
  CHECK(refused(0, {EGL_CONTEXT_OPENGL_NO_ERROR_KHR, EGL_TRUE, EGL_CONTEXT_FLAGS_KHR,
                    EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR}) == EGL_BAD_MATCH);
  CHECK(refused(0, {EGL_CONTEXT_OPENGL_NO_ERROR_KHR, EGL_TRUE,
                    EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT, EGL_TRUE}) == EGL_BAD_MATCH);
  CHECK(refused(0, {EGL_CONTEXT_OPENGL_NO_ERROR_KHR, 2}) == EGL_BAD_ATTRIBUTE);
  std::thread worker([&] {
    const EGLint no_error[] = {EGL_CONTEXT_CLIENT_VERSION, 3,
                               EGL_CONTEXT_OPENGL_NO_ERROR_KHR, EGL_TRUE, EGL_NONE};
    auto shared = g2h(call("eglCreateContext", egl.display, egl.config, egl.context, no_error));
    REQUIRE(shared);
    REQUIRE(call("eglMakeCurrent", egl.display, 0, 0, shared) == EGL_TRUE);
    CHECK(call("glIsBuffer", buffer) == GL_TRUE);
    CHECK(call("glMapBufferRange", GL_ARRAY_BUFFER, 0, 16, GL_MAP_WRITE_BIT) != 0);
    CHECK(call("glUnmapBuffer", GL_ARRAY_BUFFER) == GL_TRUE);
    CHECK(call("glGetError") == GL_NO_ERROR);
    // A validated context keeps the defined errors.
    const EGLint validated[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    auto plain = g2h(call("eglCreateContext", egl.display, egl.config, 0, validated));
    REQUIRE(plain);
    REQUIRE(call("eglMakeCurrent", egl.display, 0, 0, plain) == EGL_TRUE);
    CHECK(call("glMapBufferRange", GL_ARRAY_BUFFER, 0, 16, GL_MAP_WRITE_BIT) == 0);
    CHECK(call("glGetError") == GL_INVALID_OPERATION);
    CHECK(call("glUnmapBuffer", GL_ARRAY_BUFFER) == GL_FALSE);
    CHECK(call("glGetError") == GL_INVALID_OPERATION);
    call("eglReleaseThread");
    call("eglDestroyContext", egl.display, shared);
    call("eglDestroyContext", egl.display, plain);
  });
  worker.join();
  GLint binding{};
  call("glGetIntegerv", GL_ARRAY_BUFFER_BINDING, &binding);
  CHECK(binding == static_cast<GLint>(buffer));
  call("glDeleteBuffers", 1, &buffer);
}

TEST_SUITE_END();

namespace {
std::atomic<unsigned> stable_proc_resolutions{0};

}
