#include <doctest/doctest.h>
#include "audio/audio.h"
#include "media/media.h"
#include <fstream>
#include <cstring>
#include "java/jvm.h"
#include "java/natives/framework_natives.h"
#include "java/natives/runtime_bridge.h"
#include "java/natives/media_bridge.h"
#include "android_test_support.h"
#include "android/native_window.h"
#include "gfx/angle.h"
#include <cstdlib>
#include <array>

// Opt-in hardware check; invoke through gpu_policy.py. CPU codec tests do not
// initialize a graphics device. Sample the actual external texture, including
// replacement of a previously consumed EGLImage sibling.
static void check_surface_texture_gl(JNIEnv* env) {
  using namespace venture;
  using namespace venture::gfx;
  if(!std::getenv("VENTURE_MEDIA_GL_TEST"))return;
  REQUIRE(initialize_angle());auto display=angle_display();
#define EGL_CALL(type,name,...) angle_function<type>(name)(__VA_ARGS__)
#define GL_CALL(type,name,...) angle_function<type>(name)(__VA_ARGS__)
  EGLConfig config{};EGLint count=0;
  const EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_NONE};
  REQUIRE(EGL_CALL(PFNEGLCHOOSECONFIGPROC,"eglChooseConfig",display,attrs,&config,1,&count));REQUIRE(count>0);
  const EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE},pa[]={EGL_WIDTH,2,EGL_HEIGHT,2,EGL_NONE};
  auto context=EGL_CALL(PFNEGLCREATECONTEXTPROC,"eglCreateContext",display,config,EGL_NO_CONTEXT,ca);
  REQUIRE(context!=EGL_NO_CONTEXT);
  auto surface=EGL_CALL(PFNEGLCREATEPBUFFERSURFACEPROC,"eglCreatePbufferSurface",display,config,pa);
  REQUIRE(surface!=EGL_NO_SURFACE);
  REQUIRE(EGL_CALL(PFNEGLMAKECURRENTPROC,"eglMakeCurrent",display,surface,surface,context));
  auto consumer=EGL_CALL(PFNEGLCREATECONTEXTPROC,"eglCreateContext",display,config,context,ca);REQUIRE(consumer!=EGL_NO_CONTEXT);
  GLuint texture=0;GL_CALL(PFNGLGENTEXTURESPROC,"glGenTextures",1,&texture);
  GL_CALL(PFNGLBINDTEXTUREPROC,"glBindTexture",GL_TEXTURE_EXTERNAL_OES,texture);
  GL_CALL(PFNGLTEXPARAMETERIPROC,"glTexParameteri",GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
  auto cls=env->FindClass("android/graphics/SurfaceTexture");REQUIRE(cls);
  auto object=env->NewObject(cls,env->GetMethodID(cls,"<init>","(I)V"),jint(texture));REQUIRE(object);
  REQUIRE_FALSE(env->ExceptionCheck());
  auto handle=env->CallLongMethod(object,env->GetMethodID(cls,"ventureAcquireWindow","()J"));
  REQUIRE_FALSE(env->ExceptionCheck());
  auto window=android::pin_native_window(handle);REQUIRE(window);
  auto shader=[&](GLenum type,const char* source) {
    auto id=GL_CALL(PFNGLCREATESHADERPROC,"glCreateShader",type);
    GL_CALL(PFNGLSHADERSOURCEPROC,"glShaderSource",id,1,&source,nullptr);GL_CALL(PFNGLCOMPILESHADERPROC,"glCompileShader",id);
    GLint ok=0;GL_CALL(PFNGLGETSHADERIVPROC,"glGetShaderiv",id,GL_COMPILE_STATUS,&ok);REQUIRE(ok);return id;
  };
  auto vertex=shader(GL_VERTEX_SHADER,"#version 300 es\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.-1.,0,1);}");
  auto fragment=shader(GL_FRAGMENT_SHADER,"#version 300 es\n#extension GL_OES_EGL_image_external_essl3 : require\nprecision highp float;uniform samplerExternalOES movie;out vec4 color;void main(){color=texture(movie,vec2(.5));}");
  auto program=GL_CALL(PFNGLCREATEPROGRAMPROC,"glCreateProgram");
  GL_CALL(PFNGLATTACHSHADERPROC,"glAttachShader",program,vertex);GL_CALL(PFNGLATTACHSHADERPROC,"glAttachShader",program,fragment);
  GL_CALL(PFNGLLINKPROGRAMPROC,"glLinkProgram",program);GLint linked=0;GL_CALL(PFNGLGETPROGRAMIVPROC,"glGetProgramiv",program,GL_LINK_STATUS,&linked);REQUIRE(linked);
  GL_CALL(PFNGLUSEPROGRAMPROC,"glUseProgram",program);GL_CALL(PFNGLVIEWPORTPROC,"glViewport",0,0,2,2);
  GLuint unpack=0;GL_CALL(PFNGLGENBUFFERSPROC,"glGenBuffers",1,&unpack);
  GL_CALL(PFNGLBINDBUFFERPROC,"glBindBuffer",GL_PIXEL_UNPACK_BUFFER,unpack);
  GL_CALL(PFNGLPIXELSTOREIPROC,"glPixelStorei",GL_UNPACK_ROW_LENGTH,7);
  for(int channel=0;channel<2;++channel) {
    std::vector<std::uint8_t> rgba(16,0);for(int pixel=0;pixel<4;++pixel){rgba[pixel*4+channel]=255;rgba[pixel*4+3]=255;}
    REQUIRE(android::publish_window_rgba(window.get(),rgba,2,2,false,channel*1000)==0);
    GL_CALL(PFNGLBINDTEXTUREPROC,"glBindTexture",GL_TEXTURE_EXTERNAL_OES,0);
    env->CallVoidMethod(object,env->GetMethodID(cls,"updateTexImage","()V"));
    if(env->ExceptionCheck())env->ExceptionDescribe();REQUIRE_FALSE(env->ExceptionCheck());
    auto timestamp=env->CallLongMethod(object,env->GetMethodID(cls,"getTimestamp","()J"));REQUIRE_FALSE(env->ExceptionCheck());
    CHECK(timestamp==channel*1000);
    GLint external=0;GL_CALL(PFNGLGETINTEGERVPROC,"glGetIntegerv",GL_TEXTURE_BINDING_EXTERNAL_OES,&external);CHECK(external==texture);
    GL_CALL(PFNGLDRAWARRAYSPROC,"glDrawArrays",GL_TRIANGLES,0,3);
    std::array<std::uint8_t,4> pixel{};GL_CALL(PFNGLREADPIXELSPROC,"glReadPixels",0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel.data());
    CHECK(pixel[channel]==255);CHECK(pixel[1-channel]==0);CHECK(pixel[3]==255);
    GLint row=0,bound=0;GL_CALL(PFNGLGETINTEGERVPROC,"glGetIntegerv",GL_UNPACK_ROW_LENGTH,&row);CHECK(row==7);
    GL_CALL(PFNGLGETINTEGERVPROC,"glGetIntegerv",GL_PIXEL_UNPACK_BUFFER_BINDING,&bound);CHECK(bound==unpack);
    CHECK(GL_CALL(PFNGLGETERRORPROC,"glGetError")==GL_NO_ERROR);
    REQUIRE(EGL_CALL(PFNEGLMAKECURRENTPROC,"eglMakeCurrent",display,surface,surface,consumer));
    GL_CALL(PFNGLBINDTEXTUREPROC,"glBindTexture",GL_TEXTURE_EXTERNAL_OES,texture);
    GL_CALL(PFNGLUSEPROGRAMPROC,"glUseProgram",program);GL_CALL(PFNGLVIEWPORTPROC,"glViewport",0,0,2,2);
    GL_CALL(PFNGLDRAWARRAYSPROC,"glDrawArrays",GL_TRIANGLES,0,3);
    GL_CALL(PFNGLREADPIXELSPROC,"glReadPixels",0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel.data());
    CHECK(pixel[channel]==255);CHECK(pixel[1-channel]==0);
    REQUIRE(EGL_CALL(PFNEGLMAKECURRENTPROC,"eglMakeCurrent",display,surface,surface,context));
  }
  env->CallVoidMethod(object,env->GetMethodID(cls,"release","()V"));REQUIRE_FALSE(env->ExceptionCheck());android::release(window.get());
  env->DeleteLocalRef(object);env->DeleteLocalRef(cls);
  GL_CALL(PFNGLDELETEPROGRAMPROC,"glDeleteProgram",program);GL_CALL(PFNGLDELETESHADERPROC,"glDeleteShader",vertex);GL_CALL(PFNGLDELETESHADERPROC,"glDeleteShader",fragment);
  GL_CALL(PFNGLDELETETEXTURESPROC,"glDeleteTextures",1,&texture);
  GL_CALL(PFNGLDELETEBUFFERSPROC,"glDeleteBuffers",1,&unpack);
  EGL_CALL(PFNEGLMAKECURRENTPROC,"eglMakeCurrent",display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
  EGL_CALL(PFNEGLDESTROYSURFACEPROC,"eglDestroySurface",display,surface);EGL_CALL(PFNEGLDESTROYCONTEXTPROC,"eglDestroyContext",display,context);
  EGL_CALL(PFNEGLDESTROYCONTEXTPROC,"eglDestroyContext",display,consumer);
#undef EGL_CALL
#undef GL_CALL
}

TEST_CASE("MediaPlayer JNI and Looper prepare start seek loop release callbacks") {
  using namespace venture;
  REQUIRE(test::android_has_symbol("libmediandk.so","AMediaExtractor_getSampleFlags"));
  REQUIRE(audio::Backend::get().use_null_device());
  java::register_framework_natives();java::install_runtime_framework_bridges();
  std::string error;
  // Allows replaying an earlier framework class against the same native test
  // binary for a controlled Java concurrency regression.
  const char* framework=std::getenv("VENTURE_MEDIA_TEST_FRAMEWORK_JAR");
  if(!framework)framework=VENTURE_TEST_FRAMEWORK_JAR;
  REQUIRE(java::start_jvm({VENTURE_TEST_JAVA_HOME,
    {framework,VENTURE_MEDIA_JAVA_CLASSES},{"-Xcheck:jni","-Xverify:all"}},&error));
  auto env=java::host_env();REQUIRE(env);
  auto cls=env->FindClass("MediaFlow");REQUIRE(cls);
  auto method=env->GetStaticMethodID(cls,"run","(Ljava/lang/String;)Ljava/lang/String;");REQUIRE(method);
  auto path=env->NewStringUTF(VENTURE_MEDIA_FIXTURE);REQUIRE(path);
  auto failure=static_cast<jstring>(env->CallStaticObjectMethod(cls,method,path));
  if(env->ExceptionCheck())env->ExceptionDescribe();
  REQUIRE_FALSE(env->ExceptionCheck());
  if(failure){auto text=env->GetStringUTFChars(failure,nullptr);INFO(std::string(text));CHECK(failure==nullptr);env->ReleaseStringUTFChars(failure,text);}
  else CHECK(failure==nullptr);

  std::ifstream input(VENTURE_VIDEO_FIXTURE,std::ios::binary);
  auto bytes=std::make_shared<std::vector<char>>(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
  media::Extractor extractor;
  REQUIRE(extractor.open({static_cast<std::int64_t>(bytes->size()),[bytes](void* out,std::uint64_t size,std::int64_t offset)->std::int64_t {
    if(offset<0 || std::uint64_t(offset)>bytes->size())return -1;
    size=std::min<std::uint64_t>(size,bytes->size()-offset);std::memcpy(out,bytes->data()+offset,size);return size;
  }})==0);
  REQUIRE(extractor.select(0)==0);std::vector<media::Sample> samples;
  do{samples.push_back(extractor.sample());}while(extractor.advance());
  auto byte_array=[&](const std::vector<std::byte>& bytes) {auto array=env->NewByteArray(bytes.size());env->SetByteArrayRegion(array,0,bytes.size(),reinterpret_cast<const jbyte*>(bytes.data()));return array;};
  auto config=byte_array(extractor.tracks()[0].config);auto array_type=env->FindClass("[B");REQUIRE(array_type);
  auto packets=env->NewObjectArray(samples.size(),array_type,nullptr);auto times=env->NewLongArray(samples.size());
  std::vector<jlong> timestamps;
  for(std::size_t i=0;i<samples.size();++i){auto packet=byte_array(samples[i].bytes);env->SetObjectArrayElement(packets,i,packet);env->DeleteLocalRef(packet);timestamps.push_back(samples[i].time_us);}
  env->SetLongArrayRegion(times,0,timestamps.size(),timestamps.data());
  method=env->GetStaticMethodID(cls,"codec","([B[[B[J)Ljava/lang/String;");REQUIRE(method);
  failure=static_cast<jstring>(env->CallStaticObjectMethod(cls,method,config,packets,times));
  if(env->ExceptionCheck())env->ExceptionDescribe();REQUIRE_FALSE(env->ExceptionCheck());
  if(failure){auto text=env->GetStringUTFChars(failure,nullptr);INFO(std::string(text));CHECK(failure==nullptr);env->ReleaseStringUTFChars(failure,text);}
  env->DeleteLocalRef(config);env->DeleteLocalRef(packets);env->DeleteLocalRef(times);env->DeleteLocalRef(array_type);
  env->DeleteLocalRef(path);env->DeleteLocalRef(cls);
  check_surface_texture_gl(env);
  java::shutdown_media_players();CHECK(java::stop_jvm(&error));
}
