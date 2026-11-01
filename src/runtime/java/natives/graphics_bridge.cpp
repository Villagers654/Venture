#include "java/natives/graphics_bridge.h"

#include <jni.h>
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "common/log.h"
#include "gfx/angle.h"
#include "hle/egl/egl.h"
#include "java/jvm.h"

namespace venture::java {
namespace {
template<class F> auto guarded(JNIEnv* env, F&& fn) -> decltype(fn()) {
  try { return fn(); }
  catch (const std::exception& e) {
    log::error("framework", "Java graphics: {}", e.what());
    auto cls = env->FindClass("java/lang/IllegalStateException");
    if (cls) { env->ThrowNew(cls, e.what()); env->DeleteLocalRef(cls); }
  }
  return {};
}
template<class T> T pointer(jlong value) { return reinterpret_cast<T>(static_cast<std::uintptr_t>(value)); }
std::vector<EGLint> attributes(JNIEnv* env, jintArray array) {
  if (!array) return {EGL_NONE};
  std::vector<EGLint> result(env->GetArrayLength(array));
  env->GetIntArrayRegion(array, 0, static_cast<jsize>(result.size()), result.data());
  return result;
}
jlong JNICALL get_display(JNIEnv* env, jclass, jint id) {
  return guarded(env, [&]() -> jlong {
    return reinterpret_cast<std::uintptr_t>(gfx::egl_host_api().get_display(
        reinterpret_cast<EGLNativeDisplayType>(static_cast<std::uintptr_t>(id))));
  });
}
jboolean JNICALL initialize(JNIEnv* env, jclass, jlong display, jintArray version) {
  return guarded(env, [&]() -> jboolean {
    EGLint values[2]{};
    auto result = gfx::egl_host_api().initialize(pointer<EGLDisplay>(display), &values[0], &values[1]);
    if (result) env->SetIntArrayRegion(version, 0, 2, values);
    return result;
  });
}
jboolean JNICALL choose_config(JNIEnv* env, jclass, jlong display, jintArray attrs,
                               jlongArray configs, jintArray count) {
  return guarded(env, [&]() -> jboolean {
    auto a = attributes(env, attrs);
    const jsize capacity = configs ? env->GetArrayLength(configs) : 0;
    std::vector<EGLConfig> result(capacity);
    EGLint n = 0;
    auto ok = gfx::egl_host_api().choose_config(pointer<EGLDisplay>(display), a.data(),
        configs ? result.data() : nullptr, capacity, &n);
    if (ok) {
      env->SetIntArrayRegion(count, 0, 1, &n);
      if (configs) {
        std::vector<jlong> handles;
        for (int i = 0; i < std::min(n, capacity); ++i)
          handles.push_back(reinterpret_cast<std::uintptr_t>(result[i]));
        if (!env->ExceptionCheck()) env->SetLongArrayRegion(configs, 0, handles.size(), handles.data());
      }
    }
    return ok;
  });
}
jlong JNICALL create_pbuffer(JNIEnv* env, jclass, jlong display, jlong config, jintArray attrs) {
  return guarded(env, [&]() -> jlong {
    auto a = attributes(env, attrs);
    return reinterpret_cast<std::uintptr_t>(gfx::egl_host_api().create_pbuffer(
        pointer<EGLDisplay>(display), pointer<EGLConfig>(config), a.data()));
  });
}
jlong JNICALL create_context(JNIEnv* env, jclass, jlong display, jlong config, jlong share, jintArray attrs) {
  return guarded(env, [&]() -> jlong {
    auto a = attributes(env, attrs);
    return reinterpret_cast<std::uintptr_t>(gfx::egl_host_api().create_context(
        pointer<EGLDisplay>(display), pointer<EGLConfig>(config), pointer<EGLContext>(share), a.data()));
  });
}
jboolean JNICALL make_current(JNIEnv* env, jclass, jlong d, jlong draw, jlong read, jlong context) {
  return guarded(env, [&]() -> jboolean { return gfx::egl_host_api().make_current(
      pointer<EGLDisplay>(d), pointer<EGLSurface>(draw), pointer<EGLSurface>(read), pointer<EGLContext>(context)); });
}
jboolean JNICALL destroy_surface(JNIEnv* env, jclass, jlong d, jlong s) {
  return guarded(env, [&]() -> jboolean { return gfx::egl_host_api().destroy_surface(pointer<EGLDisplay>(d), pointer<EGLSurface>(s)); });
}
jboolean JNICALL destroy_context(JNIEnv* env, jclass, jlong d, jlong c) {
  return guarded(env, [&]() -> jboolean { return gfx::egl_host_api().destroy_context(pointer<EGLDisplay>(d), pointer<EGLContext>(c)); });
}
jboolean JNICALL terminate(JNIEnv* env, jclass, jlong d) {
  return guarded(env, [&]() -> jboolean { return gfx::egl_host_api().terminate(pointer<EGLDisplay>(d)); });
}
jint JNICALL get_error(JNIEnv*, jclass) { return gfx::egl_host_api().get_error(); }
jlong JNICALL current_display(JNIEnv*, jclass) {
  return reinterpret_cast<std::uintptr_t>(gfx::egl_host_api().get_current_display());
}
jlong JNICALL current_context(JNIEnv*, jclass) {
  return reinterpret_cast<std::uintptr_t>(gfx::egl_host_api().get_current_context());
}
jlong JNICALL current_surface(JNIEnv*, jclass, jint which) {
  return reinterpret_cast<std::uintptr_t>(gfx::egl_host_api().get_current_surface(which));
}
jboolean JNICALL query_context(JNIEnv* env, jclass, jlong d, jlong c, jint attribute, jintArray value) {
  return guarded(env, [&]() -> jboolean {
    EGLint result = 0;
    auto ok = gfx::egl_host_api().query_context(pointer<EGLDisplay>(d), pointer<EGLContext>(c), attribute, &result);
    if (ok) env->SetIntArrayRegion(value, 0, 1, &result);
    return ok;
  });
}
jstring JNICALL query_string(JNIEnv* env, jclass, jlong d, jint name) {
  return guarded(env, [&]() -> jstring {
    const auto* text = gfx::egl_host_api().query_string(pointer<EGLDisplay>(d), name);
    return text ? env->NewStringUTF(text) : nullptr;
  });
}

// android.opengl.GLES20 on the thread's current context. Java checked arrays
// and buffers; these only move data.
template <class T, gfx::detail::AngleName Name> T gl() { return gfx::angle_function<T, Name>(); }
#define GL(type, name) gl<type, #name>()
std::string utf8(JNIEnv* env, jstring text) {
  if (!text) return {};
  const char* chars = env->GetStringUTFChars(text, nullptr);
  std::string result = chars ? chars : "";
  if (chars) env->ReleaseStringUTFChars(text, chars);
  return result;
}
// A direct buffer's address, or a pinned primitive array released after the call.
class Storage {
 public:
  Storage(JNIEnv* env, jobject base, jint offset) : env_(env) {
    if (!base) return;
    auto* address = static_cast<char*>(env->GetDirectBufferAddress(base));
    if (!address) {
      array_ = static_cast<jarray>(base);
      address = pinned_ = static_cast<char*>(env->GetPrimitiveArrayCritical(array_, nullptr));
      if (!address) throw std::runtime_error("cannot pin Java array");
    }
    data_ = address + offset;
  }
  Storage(const Storage&) = delete;
  ~Storage() {
    if (pinned_) env_->ReleasePrimitiveArrayCritical(array_, pinned_, 0);
  }
  void* data() const { return data_; }
 private:
  JNIEnv* env_;
  jarray array_ = nullptr;
  char* pinned_ = nullptr;
  void* data_ = nullptr;
};
void JNICALL gl_names(JNIEnv* env, jclass, jint op, jint n, jintArray names, jint offset) {
  std::vector<GLuint> values(n);
  if (op >= 4) env->GetIntArrayRegion(names, offset, n, reinterpret_cast<jint*>(values.data()));
  switch (op) {
    case 0: GL(PFNGLGENTEXTURESPROC, glGenTextures)(n, values.data()); break;
    case 1: GL(PFNGLGENBUFFERSPROC, glGenBuffers)(n, values.data()); break;
    case 2: GL(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)(n, values.data()); break;
    case 3: GL(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers)(n, values.data()); break;
    case 4: GL(PFNGLDELETETEXTURESPROC, glDeleteTextures)(n, values.data()); break;
    case 5: GL(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)(n, values.data()); break;
    case 6: GL(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)(n, values.data()); break;
    case 7: GL(PFNGLDELETERENDERBUFFERSPROC, glDeleteRenderbuffers)(n, values.data()); break;
  }
  if (op < 4) env->SetIntArrayRegion(names, offset, n, reinterpret_cast<const jint*>(values.data()));
}
void JNICALL gl_integers(JNIEnv* env, jclass, jint op, jint a, jint b, jintArray params, jint offset) {
  // A query writes as many values as its name needs (GL_VIEWPORT 4); values it
  // leaves alone keep the caller's, and nothing goes past the caller's array.
  const jsize count = std::min<jsize>(env->GetArrayLength(params) - offset, 64);
  GLint values[64]{};
  env->GetIntArrayRegion(params, offset, count, values);
  switch (op) {
    case 0: GL(PFNGLGETINTEGERVPROC, glGetIntegerv)(a, values); break;
    case 1: GL(PFNGLGETSHADERIVPROC, glGetShaderiv)(a, b, values); break;
    case 2: GL(PFNGLGETPROGRAMIVPROC, glGetProgramiv)(a, b, values); break;
    case 3: GL(PFNGLGETTEXPARAMETERIVPROC, glGetTexParameteriv)(a, b, values); break;
  }
  env->SetIntArrayRegion(params, offset, count, values);
}
void JNICALL gl_floats(JNIEnv* env, jclass, jint op, jint location, jint count, jboolean transpose, jfloatArray value,
                       jint offset) {
  std::vector<GLfloat> values(static_cast<std::size_t>(count) * (op == 0 ? 16 : op == 1 ? 4 : 1));
  env->GetFloatArrayRegion(value, offset, static_cast<jsize>(values.size()), values.data());
  switch (op) {
    case 0: GL(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv)(location, count, transpose, values.data()); break;
    case 1: GL(PFNGLUNIFORM4FVPROC, glUniform4fv)(location, count, values.data()); break;
    case 2: GL(PFNGLUNIFORM1FVPROC, glUniform1fv)(location, count, values.data()); break;
  }
}
void JNICALL gl_memory(JNIEnv* env, jclass, jint op, jint a, jint b, jint c, jint d, jint e, jint f, jint g, jint h,
                       jobject base, jint offset) {
  guarded(env, [&]() -> int {
    Storage storage(env, base, offset);
    switch (op) {
      case 0: GL(PFNGLTEXIMAGE2DPROC, glTexImage2D)(a, b, c, d, e, f, g, h, storage.data()); break;
      case 1: GL(PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D)(a, b, c, d, e, f, g, h, storage.data()); break;
      case 2: GL(PFNGLREADPIXELSPROC, glReadPixels)(a, b, c, d, e, f, storage.data()); break;
      case 3: GL(PFNGLBUFFERDATAPROC, glBufferData)(a, b, storage.data(), c); break;
      case 4: GL(PFNGLBUFFERSUBDATAPROC, glBufferSubData)(a, b, c, storage.data()); break;
    }
    return 0;
  });
}
void JNICALL gl_attrib_pointer(JNIEnv* env, jclass, jint index, jint size, jint type, jboolean normalized, jint stride,
                               jobject pointer, jint offset) {
  auto* address = static_cast<char*>(env->GetDirectBufferAddress(pointer));
  GL(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)(index, size, type, normalized, stride, address + offset);
}
void JNICALL gl_shader_source(JNIEnv* env, jclass, jint shader, jstring text) {
  auto source = utf8(env, text);
  const GLchar* strings[]{source.c_str()};
  GL(PFNGLSHADERSOURCEPROC, glShaderSource)(shader, 1, strings, nullptr);
}
template <class Length, class Log> jstring info_log(JNIEnv* env, GLuint object, Length length, Log log) {
  GLint size = 0;
  length(object, GL_INFO_LOG_LENGTH, &size);
  std::string text(std::max(size, 1), '\0');
  GLsizei written = 0;
  log(object, static_cast<GLsizei>(text.size()), &written, text.data());
  text.resize(std::clamp<GLsizei>(written, 0, static_cast<GLsizei>(text.size())));
  return env->NewStringUTF(text.c_str());
}
jstring JNICALL gl_program_log(JNIEnv* env, jclass, jint program) {
  return info_log(env, program, GL(PFNGLGETPROGRAMIVPROC, glGetProgramiv), GL(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog));
}
jstring JNICALL gl_shader_log(JNIEnv* env, jclass, jint shader) {
  return info_log(env, shader, GL(PFNGLGETSHADERIVPROC, glGetShaderiv), GL(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog));
}
jint JNICALL gl_attrib_location(JNIEnv* env, jclass, jint program, jstring name) {
  return GL(PFNGLGETATTRIBLOCATIONPROC, glGetAttribLocation)(program, utf8(env, name).c_str());
}
jint JNICALL gl_uniform_location(JNIEnv* env, jclass, jint program, jstring name) {
  return GL(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)(program, utf8(env, name).c_str());
}
void JNICALL gl_bind_attrib_location(JNIEnv* env, jclass, jint program, jint index, jstring name) {
  GL(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation)(program, index, utf8(env, name).c_str());
}
void JNICALL gl_draw_elements(JNIEnv*, jclass, jint mode, jint count, jint type, jint offset) {
  GL(PFNGLDRAWELEMENTSPROC, glDrawElements)(mode, count, type, reinterpret_cast<const void*>(static_cast<std::intptr_t>(offset)));
}
void JNICALL gl_attrib_offset(JNIEnv*, jclass, jint index, jint size, jint type, jboolean normalized, jint stride,
                              jint offset) {
  GL(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)(index, size, type, normalized, stride,
      reinterpret_cast<const void*>(static_cast<std::intptr_t>(offset)));
}
// Calls whose Java and GL parameters match one to one.
template <class T, gfx::detail::AngleName Name, class R, class... A> R JNICALL forward(JNIEnv*, jclass, A... args) {
  if constexpr (std::is_void_v<R>) gl<T, Name>()(args...);
  else return static_cast<R>(gl<T, Name>()(args...));
}
jint JNICALL get_gl_error(JNIEnv* env, jclass) {
  return guarded(env, []() -> jint { return gfx::gles_host_error(); });
}
jstring JNICALL get_string(JNIEnv* env, jclass, jint name) {
  return guarded(env, [&]() -> jstring {
    const auto* value = gfx::gles_host_string(name);
    return value ? env->NewStringUTF(reinterpret_cast<const char*>(value)) : nullptr;
  });
}
void register_natives(JNIEnv* env) {
  auto cls = env->FindClass("android/opengl/EGL14");
  if (!cls) return;
#define METHOD(name, signature, function) {const_cast<char*>(name), const_cast<char*>(signature), reinterpret_cast<void*>(function)}
  const JNINativeMethod egl[]{
    METHOD("getDisplay", "(I)J", get_display), METHOD("initialize", "(J[I)Z", initialize),
    METHOD("chooseConfig", "(J[I[J[I)Z", choose_config), METHOD("createPbuffer", "(JJ[I)J", create_pbuffer),
    METHOD("createContext", "(JJJ[I)J", create_context), METHOD("makeCurrent", "(JJJJ)Z", make_current),
    METHOD("destroySurface", "(JJ)Z", destroy_surface), METHOD("destroyContext", "(JJ)Z", destroy_context),
    METHOD("terminate", "(J)Z", terminate), METHOD("eglGetError", "()I", get_error),
    METHOD("currentDisplay", "()J", current_display), METHOD("currentContext", "()J", current_context),
    METHOD("currentSurface", "(I)J", current_surface), METHOD("queryContext", "(JJI[I)Z", query_context),
    METHOD("queryString", "(JI)Ljava/lang/String;", query_string)};
  env->RegisterNatives(cls, egl, std::size(egl));
  env->DeleteLocalRef(cls);
  if (env->ExceptionCheck()) return;
  cls = env->FindClass("android/opengl/GLES20");
  if (!cls) return;
#define FORWARD(name, signature, type, ...) METHOD(#name, signature, (forward<type, #name, __VA_ARGS__>))
  const JNINativeMethod gl[]{METHOD("glGetString", "(I)Ljava/lang/String;", get_string),
    METHOD("glGetError", "()I", get_gl_error),
    METHOD("names", "(II[II)V", gl_names), METHOD("integers", "(III[II)V", gl_integers),
    METHOD("floats", "(IIIZ[FI)V", gl_floats), METHOD("memory", "(IIIIIIIIILjava/lang/Object;I)V", gl_memory),
    METHOD("attribPointer", "(IIIZILjava/nio/Buffer;I)V", gl_attrib_pointer),
    METHOD("shaderSource", "(ILjava/lang/String;)V", gl_shader_source),
    METHOD("glGetProgramInfoLog", "(I)Ljava/lang/String;", gl_program_log),
    METHOD("glGetShaderInfoLog", "(I)Ljava/lang/String;", gl_shader_log),
    METHOD("glGetAttribLocation", "(ILjava/lang/String;)I", gl_attrib_location),
    METHOD("glGetUniformLocation", "(ILjava/lang/String;)I", gl_uniform_location),
    METHOD("glBindAttribLocation", "(IILjava/lang/String;)V", gl_bind_attrib_location),
    METHOD("glDrawElements", "(IIII)V", gl_draw_elements),
    METHOD("glVertexAttribPointer", "(IIIZII)V", gl_attrib_offset),
    FORWARD(glActiveTexture, "(I)V", PFNGLACTIVETEXTUREPROC, void, jint),
    FORWARD(glAttachShader, "(II)V", PFNGLATTACHSHADERPROC, void, jint, jint),
    FORWARD(glBindBuffer, "(II)V", PFNGLBINDBUFFERPROC, void, jint, jint),
    FORWARD(glBindFramebuffer, "(II)V", PFNGLBINDFRAMEBUFFERPROC, void, jint, jint),
    FORWARD(glBindRenderbuffer, "(II)V", PFNGLBINDRENDERBUFFERPROC, void, jint, jint),
    FORWARD(glBindTexture, "(II)V", PFNGLBINDTEXTUREPROC, void, jint, jint),
    FORWARD(glBlendFunc, "(II)V", PFNGLBLENDFUNCPROC, void, jint, jint),
    FORWARD(glCheckFramebufferStatus, "(I)I", PFNGLCHECKFRAMEBUFFERSTATUSPROC, jint, jint),
    FORWARD(glClear, "(I)V", PFNGLCLEARPROC, void, jint),
    FORWARD(glClearColor, "(FFFF)V", PFNGLCLEARCOLORPROC, void, jfloat, jfloat, jfloat, jfloat),
    FORWARD(glColorMask, "(ZZZZ)V", PFNGLCOLORMASKPROC, void, jboolean, jboolean, jboolean, jboolean),
    FORWARD(glCompileShader, "(I)V", PFNGLCOMPILESHADERPROC, void, jint),
    FORWARD(glCreateProgram, "()I", PFNGLCREATEPROGRAMPROC, jint),
    FORWARD(glCreateShader, "(I)I", PFNGLCREATESHADERPROC, jint, jint),
    FORWARD(glCullFace, "(I)V", PFNGLCULLFACEPROC, void, jint),
    FORWARD(glDeleteProgram, "(I)V", PFNGLDELETEPROGRAMPROC, void, jint),
    FORWARD(glDeleteShader, "(I)V", PFNGLDELETESHADERPROC, void, jint),
    FORWARD(glDepthMask, "(Z)V", PFNGLDEPTHMASKPROC, void, jboolean),
    FORWARD(glDetachShader, "(II)V", PFNGLDETACHSHADERPROC, void, jint, jint),
    FORWARD(glDisable, "(I)V", PFNGLDISABLEPROC, void, jint),
    FORWARD(glDisableVertexAttribArray, "(I)V", PFNGLDISABLEVERTEXATTRIBARRAYPROC, void, jint),
    FORWARD(glDrawArrays, "(III)V", PFNGLDRAWARRAYSPROC, void, jint, jint, jint),
    FORWARD(glEnable, "(I)V", PFNGLENABLEPROC, void, jint),
    FORWARD(glEnableVertexAttribArray, "(I)V", PFNGLENABLEVERTEXATTRIBARRAYPROC, void, jint),
    FORWARD(glFinish, "()V", PFNGLFINISHPROC, void),
    FORWARD(glFlush, "()V", PFNGLFLUSHPROC, void),
    FORWARD(glFramebufferRenderbuffer, "(IIII)V", PFNGLFRAMEBUFFERRENDERBUFFERPROC, void, jint, jint, jint, jint),
    FORWARD(glFramebufferTexture2D, "(IIIII)V", PFNGLFRAMEBUFFERTEXTURE2DPROC, void, jint, jint, jint, jint, jint),
    FORWARD(glIsEnabled, "(I)Z", PFNGLISENABLEDPROC, jboolean, jint),
    FORWARD(glLinkProgram, "(I)V", PFNGLLINKPROGRAMPROC, void, jint),
    FORWARD(glPixelStorei, "(II)V", PFNGLPIXELSTOREIPROC, void, jint, jint),
    FORWARD(glRenderbufferStorage, "(IIII)V", PFNGLRENDERBUFFERSTORAGEPROC, void, jint, jint, jint, jint),
    FORWARD(glScissor, "(IIII)V", PFNGLSCISSORPROC, void, jint, jint, jint, jint),
    FORWARD(glTexParameterf, "(IIF)V", PFNGLTEXPARAMETERFPROC, void, jint, jint, jfloat),
    FORWARD(glTexParameteri, "(III)V", PFNGLTEXPARAMETERIPROC, void, jint, jint, jint),
    FORWARD(glUniform1f, "(IF)V", PFNGLUNIFORM1FPROC, void, jint, jfloat),
    FORWARD(glUniform1i, "(II)V", PFNGLUNIFORM1IPROC, void, jint, jint),
    FORWARD(glUniform2f, "(IFF)V", PFNGLUNIFORM2FPROC, void, jint, jfloat, jfloat),
    FORWARD(glUniform4f, "(IFFFF)V", PFNGLUNIFORM4FPROC, void, jint, jfloat, jfloat, jfloat, jfloat),
    FORWARD(glUseProgram, "(I)V", PFNGLUSEPROGRAMPROC, void, jint),
    FORWARD(glViewport, "(IIII)V", PFNGLVIEWPORTPROC, void, jint, jint, jint, jint)};
#undef FORWARD
  env->RegisterNatives(cls, gl, std::size(gl));
  env->DeleteLocalRef(cls);
#undef METHOD
}
}
void register_graphics_natives() {
  static std::once_flag once;
  std::call_once(once, [] { add_framework_natives(register_natives); });
}
}
