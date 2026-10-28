#include "java/natives/graphics_bridge.h"

#include <jni.h>
#include <mutex>
#include <vector>
#include "common/log.h"
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
    METHOD("terminate", "(J)Z", terminate), METHOD("eglGetError", "()I", get_error)};
  env->RegisterNatives(cls, egl, std::size(egl));
  env->DeleteLocalRef(cls);
  if (env->ExceptionCheck()) return;
  cls = env->FindClass("android/opengl/GLES20");
  if (!cls) return;
  const JNINativeMethod gl[]{METHOD("glGetString", "(I)Ljava/lang/String;", get_string),
                           METHOD("glGetError", "()I", get_gl_error)};
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
