#include "java/natives/media_bridge.h"
#include "java/internal.h"
#include "android/native_window.h"
#include "gfx/angle.h"
#include "hle/gles/gles_state.h"
#include "common/log.h"
#include "av_telemetry.h"
#include <format>

namespace venture::java {
namespace {
JNIEnv* daemon_env() {JNIEnv* env=nullptr;if(attach_host(true,nullptr,&env)!=JNI_OK)throw std::runtime_error("Surface callback attach failed");return env;}
template<class F> auto guard(JNIEnv* env,F&& fn)->decltype(fn()) {
  try{return fn();}catch(const std::exception& e){throw_java(env,"java/lang/IllegalStateException",e.what());return {};}
}
struct Listener {
  jweak object=nullptr;jmethodID method=nullptr;
  ~Listener(){if(object && host_vm())try{daemon_env()->DeleteWeakGlobalRef(object);}catch(...) {}}
  void notify() {
    if(!host_vm())return;auto env=daemon_env();auto local=env->NewLocalRef(object);if(!local)return;
    env->CallVoidMethod(local,method);
    if(env->ExceptionCheck()){env->ExceptionDescribe();env->ExceptionClear();}
    env->DeleteLocalRef(local);
  }
};
jlong JNICALL create(JNIEnv* env,jobject self) {
  return guard(env,[&]()->jlong {
    auto listener=std::make_shared<Listener>();auto cls=env->GetObjectClass(self);
    listener->method=env->GetMethodID(cls,"postFrameAvailable","()V");env->DeleteLocalRef(cls);
    if(!listener->method)return 0;listener->object=env->NewWeakGlobalRef(self);if(!listener->object)return 0;
    auto window=android::create_native_window(1,1);if(!window)return 0;
    window->image_consumer=true;window->frame_available=[listener]{listener->notify();};
    log::info("media","SurfaceTexture created native-window consumer");return h2g(window);
  });
}
jlong JNICALL acquire(JNIEnv* env,jclass,jlong handle) {
  return guard(env,[&]()->jlong {auto w=android::pin_native_window(handle);if(!w)return 0;android::acquire(w.get());return h2g(w.get());});
}
void JNICALL release(JNIEnv* env,jclass,jlong handle) {
  guard(env,[&]()->int {
    auto w=android::pin_native_window(handle);if(!w)return 0;
    std::function<void()> listener;
    {std::lock_guard lock(w->lock);w->image_consumer=false;w->frame.reset();listener=std::move(w->frame_available);}
    android::release(w.get());return 0;
  });
}
void JNICALL size(JNIEnv* env,jclass,jlong handle,jint width,jint height) {
  guard(env,[&]()->int {
    auto w=android::pin_native_window(handle);if(!w || width<=0 || height<=0 || width>16384 || height>16384)throw std::runtime_error("Invalid SurfaceTexture dimensions");
    std::lock_guard lock(w->lock);w->width=w->default_width=width;w->height=w->default_height=height;return 0;
  });
}
jlongArray JNICALL update(JNIEnv* env,jclass,jlong handle,jint texture,jlong serial) {
  return guard(env,[&]()->jlongArray {
    auto w=android::pin_native_window(handle);if(!w)throw std::runtime_error("SurfaceTexture released");
    std::shared_ptr<const android::SurfaceFrame> frame;
    {std::lock_guard lock(w->lock);frame=w->frame;}
    if(!frame || frame->serial==std::uint64_t(serial))return nullptr;
    using namespace gfx;
    auto display=angle_function<PFNEGLGETCURRENTDISPLAYPROC,"eglGetCurrentDisplay">()();
    auto context=angle_function<PFNEGLGETCURRENTCONTEXTPROC,"eglGetCurrentContext">()();
    if(context==EGL_NO_CONTEXT)throw std::runtime_error("updateTexImage requires the attached GL context");
    auto get=angle_function<PFNGLGETINTEGERVPROC,"glGetIntegerv">();
    auto bind=angle_function<PFNGLBINDTEXTUREPROC,"glBindTexture">();
    auto pixel=angle_function<PFNGLPIXELSTOREIPROC,"glPixelStorei">();
    GLint previous2d=0,alignment=0,pbo=0,row=0,skip_rows=0,skip_pixels=0;
    get(GL_TEXTURE_BINDING_2D,&previous2d);get(GL_UNPACK_ALIGNMENT,&alignment);
    EGLint version=2;
    angle_function<PFNEGLQUERYCONTEXTPROC,"eglQueryContext">()(display,context,EGL_CONTEXT_CLIENT_VERSION,&version);
    bool es3=version>=3;
    auto bind_buffer=angle_function<PFNGLBINDBUFFERPROC,"glBindBuffer">();
    if(es3) {
      get(GL_PIXEL_UNPACK_BUFFER_BINDING,&pbo);bind_buffer(GL_PIXEL_UNPACK_BUFFER,0);
      get(GL_UNPACK_ROW_LENGTH,&row);get(GL_UNPACK_SKIP_ROWS,&skip_rows);get(GL_UNPACK_SKIP_PIXELS,&skip_pixels);
      pixel(GL_UNPACK_ROW_LENGTH,0);pixel(GL_UNPACK_SKIP_ROWS,0);pixel(GL_UNPACK_SKIP_PIXELS,0);
    }
    GLuint source=0;angle_function<PFNGLGENTEXTURESPROC,"glGenTextures">()(1,&source);
    bind(GL_TEXTURE_2D,source);pixel(GL_UNPACK_ALIGNMENT,1);
    auto parameter=angle_function<PFNGLTEXPARAMETERIPROC,"glTexParameteri">();
    parameter(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);parameter(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    angle_function<PFNGLTEXIMAGE2DPROC,"glTexImage2D">()(GL_TEXTURE_2D,0,GL_RGBA,frame->width,frame->height,0,GL_RGBA,GL_UNSIGNED_BYTE,frame->rgba.data());
    const EGLint attrs[]={EGL_GL_TEXTURE_LEVEL_KHR,0,EGL_IMAGE_PRESERVED_KHR,EGL_TRUE,EGL_NONE};
    auto image=angle_function<PFNEGLCREATEIMAGEKHRPROC,"eglCreateImageKHR">()(display,context,EGL_GL_TEXTURE_2D_KHR,
      reinterpret_cast<EGLClientBuffer>(std::uintptr_t(source)),attrs);
    if(image!=EGL_NO_IMAGE_KHR) {
      bind(GL_TEXTURE_EXTERNAL_OES,texture);
      angle_function<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC,"glEGLImageTargetTexture2DOES">()(GL_TEXTURE_EXTERNAL_OES,image);
      angle_function<PFNEGLDESTROYIMAGEKHRPROC,"eglDestroyImageKHR">()(display,image);
    }
    // updateTexImage implicitly binds the attached external texture. Publish
    // writes before a shared render context samples its EGLImage sibling.
    bind(GL_TEXTURE_2D,previous2d);pixel(GL_UNPACK_ALIGNMENT,alignment);
    if(es3){pixel(GL_UNPACK_ROW_LENGTH,row);pixel(GL_UNPACK_SKIP_ROWS,skip_rows);pixel(GL_UNPACK_SKIP_PIXELS,skip_pixels);bind_buffer(GL_PIXEL_UNPACK_BUFFER,pbo);}
    angle_function<PFNGLDELETETEXTURESPROC,"glDeleteTextures">()(1,&source);
    if(image==EGL_NO_IMAGE_KHR)throw std::runtime_error("SurfaceTexture EGLImage upload failed");
    angle_function<PFNGLFLUSHPROC,"glFlush">()();
    av::emit("surface_texture_update",std::format("\"texture\":{},\"serial\":{},\"pts_ns\":{},\"width\":{},\"height\":{}",
      texture,frame->serial,frame->timestamp_ns,frame->width,frame->height));
    jlong values[]={static_cast<jlong>(frame->serial),frame->timestamp_ns};auto result=env->NewLongArray(2);
    if(result)env->SetLongArrayRegion(result,0,2,values);return result;
  });
}
}
void register_surface_textures(JNIEnv* env) {
  auto cls=env->FindClass("android/graphics/SurfaceTexture");if(!cls)return;
#define METHOD(n,s,f) {const_cast<char*>(n),const_cast<char*>(s),reinterpret_cast<void*>(f)}
  const JNINativeMethod methods[]={METHOD("nativeCreate","()J",create),METHOD("nativeAcquire","(J)J",acquire),
    METHOD("nativeRelease","(J)V",release),METHOD("nativeSize","(JII)V",size),METHOD("nativeUpdate","(JIJ)[J",update)};
#undef METHOD
  env->RegisterNatives(cls,methods,std::size(methods));env->DeleteLocalRef(cls);
}
}
