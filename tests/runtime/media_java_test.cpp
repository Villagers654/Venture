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
#include "gfx/shared_texture.h"
#include "shared_image_test_support.h"
#include <cstdlib>
#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#ifndef _WIN32
#include <sys/resource.h>
#endif

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
  const EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,
    EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
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
  // A decoder's GPU picture, exported by another device on this GPU, reaches
  // the external texture by a GPU copy.
  CHECK(android::window_shared_image_consumer(window.get())==gl_imports_shared_images());
  if(gl_imports_shared_images()) {
    test::ExportedImage exported(gpu_device(),2,2,{{0.2f,0.4f,0.6f,1.0f}});
    auto picture=std::make_shared<SharedImage>(exported.shared);
    REQUIRE(android::publish_window_image(window.get(),picture,2,2,5000)==0);
    GL_CALL(PFNGLBINDTEXTUREPROC,"glBindTexture",GL_TEXTURE_EXTERNAL_OES,0);
    env->CallVoidMethod(object,env->GetMethodID(cls,"updateTexImage","()V"));
    if(env->ExceptionCheck())env->ExceptionDescribe();REQUIRE_FALSE(env->ExceptionCheck());
    CHECK(env->CallLongMethod(object,env->GetMethodID(cls,"getTimestamp","()J"))==5000);
    GL_CALL(PFNGLDRAWARRAYSPROC,"glDrawArrays",GL_TRIANGLES,0,3);
    std::array<std::uint8_t,4> pixel{};GL_CALL(PFNGLREADPIXELSPROC,"glReadPixels",0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel.data());
    CHECK(pixel==std::array<std::uint8_t,4>{51,102,153,255});
    CHECK(GL_CALL(PFNGLGETERRORPROC,"glGetError")==GL_NO_ERROR);
    CHECK(android::window_shared_image_consumer(window.get()));
    // The window lets go of the picture with its next frame, and the import
    // cache keeps no reference of its own.
    std::vector<std::uint8_t> rgba(16,0);
    REQUIRE(android::publish_window_rgba(window.get(),rgba,2,2,false,6000)==0);
    CHECK(picture.use_count()==1);
  } else MESSAGE("GL shared-image import unavailable; GPU picture path not exercised");
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

// Opt-in measurement (VENTURE_MEDIA_GL_MOVIE=<movie>, report appended to
// VENTURE_MEDIA_GL_REPORT): a Surface codec feeding a real SurfaceTexture,
// released on presentation deadlines as ExoPlayer does (more than 30 ms late
// is dropped), each new frame latched with updateTexImage and sampled.
static void measure_surface_texture_rate(JNIEnv* env) {
  using namespace venture;
  using namespace venture::gfx;
  const char* movie=std::getenv("VENTURE_MEDIA_GL_MOVIE");const char* report_path=std::getenv("VENTURE_MEDIA_GL_REPORT");
  if(!movie || !report_path)return;
  using clock_type=std::chrono::steady_clock;
  auto now_ns=[]{return std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now().time_since_epoch()).count();};
  auto cpu_seconds=[] {
#ifdef _WIN32
    FILETIME created,exited,kernel,user;
    if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))return 0.0;
    auto value=[](FILETIME t){return (std::uint64_t(t.dwHighDateTime)<<32|t.dwLowDateTime)/1e7;};
    return value(kernel)+value(user);
#else
    rusage usage{};getrusage(RUSAGE_SELF,&usage);
    return usage.ru_utime.tv_sec+usage.ru_utime.tv_usec/1e6+usage.ru_stime.tv_sec+usage.ru_stime.tv_usec/1e6;
#endif
  };
  REQUIRE(initialize_angle());auto display=angle_display();
#define EGL_CALL(type,name,...) angle_function<type>(name)(__VA_ARGS__)
#define GL_CALL(type,name,...) angle_function<type>(name)(__VA_ARGS__)
  EGLConfig config{};EGLint count=0;
  const EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_NONE};
  REQUIRE(EGL_CALL(PFNEGLCHOOSECONFIGPROC,"eglChooseConfig",display,attrs,&config,1,&count));REQUIRE(count>0);
  const EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE},pa[]={EGL_WIDTH,2,EGL_HEIGHT,2,EGL_NONE};
  auto context=EGL_CALL(PFNEGLCREATECONTEXTPROC,"eglCreateContext",display,config,EGL_NO_CONTEXT,ca);REQUIRE(context!=EGL_NO_CONTEXT);
  auto surface=EGL_CALL(PFNEGLCREATEPBUFFERSURFACEPROC,"eglCreatePbufferSurface",display,config,pa);REQUIRE(surface!=EGL_NO_SURFACE);
  REQUIRE(EGL_CALL(PFNEGLMAKECURRENTPROC,"eglMakeCurrent",display,surface,surface,context));
  GLuint texture=0;GL_CALL(PFNGLGENTEXTURESPROC,"glGenTextures",1,&texture);
  GL_CALL(PFNGLBINDTEXTUREPROC,"glBindTexture",GL_TEXTURE_EXTERNAL_OES,texture);
  GL_CALL(PFNGLTEXPARAMETERIPROC,"glTexParameteri",GL_TEXTURE_EXTERNAL_OES,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
  auto cls=env->FindClass("android/graphics/SurfaceTexture");REQUIRE(cls);
  auto object=env->NewObject(cls,env->GetMethodID(cls,"<init>","(I)V"),jint(texture));REQUIRE(object);
  auto handle=env->CallLongMethod(object,env->GetMethodID(cls,"ventureAcquireWindow","()J"));REQUIRE_FALSE(env->ExceptionCheck());
  auto window=android::pin_native_window(handle);REQUIRE(window);
  auto update=env->GetMethodID(cls,"updateTexImage","()V");
  auto shader=[&](GLenum type,const char* source) {
    auto id=GL_CALL(PFNGLCREATESHADERPROC,"glCreateShader",type);
    GL_CALL(PFNGLSHADERSOURCEPROC,"glShaderSource",id,1,&source,nullptr);GL_CALL(PFNGLCOMPILESHADERPROC,"glCompileShader",id);return id;
  };
  auto program=GL_CALL(PFNGLCREATEPROGRAMPROC,"glCreateProgram");
  GL_CALL(PFNGLATTACHSHADERPROC,"glAttachShader",program,shader(GL_VERTEX_SHADER,"#version 300 es\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.-1.,0,1);}"));
  GL_CALL(PFNGLATTACHSHADERPROC,"glAttachShader",program,shader(GL_FRAGMENT_SHADER,"#version 300 es\n#extension GL_OES_EGL_image_external_essl3 : require\nprecision highp float;uniform samplerExternalOES movie;out vec4 color;void main(){color=texture(movie,vec2(.5));}"));
  GL_CALL(PFNGLLINKPROGRAMPROC,"glLinkProgram",program);GL_CALL(PFNGLUSEPROGRAMPROC,"glUseProgram",program);
  GL_CALL(PFNGLVIEWPORTPROC,"glViewport",0,0,2,2);
  std::ifstream input(movie,std::ios::binary);
  auto bytes=std::make_shared<std::vector<char>>(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
  media::Extractor extractor;
  REQUIRE(extractor.open({static_cast<std::int64_t>(bytes->size()),[bytes](void* out,std::uint64_t size,std::int64_t offset)->std::int64_t {
    if(offset<0 || std::uint64_t(offset)>bytes->size())return -1;
    size=std::min<std::uint64_t>(size,bytes->size()-offset);std::memcpy(out,bytes->data()+offset,size);return size;
  }})==0);
  int track=-1;
  for(std::size_t i=0;i<extractor.tracks().size();++i)if(media::video_mime(extractor.tracks()[i].mime)){track=int(i);break;}
  REQUIRE(track>=0);REQUIRE(extractor.select(track)==0);
  std::atomic<std::int64_t> start{0};
  media::Codec codec;
  auto owner=window;
  REQUIRE(codec.configure(extractor.tracks()[track],[owner](media::VideoFrame& f) {
    if(f.image)return android::publish_window_image(owner.get(),std::move(f.image),f.width,f.height,f.time_us*1000);
    return android::publish_window_rgba(owner.get(),std::move(f.rgba),f.width,f.height,f.time_us*1000);
  },[owner]{return media::Codec::Target(android::window_consumer_extent(owner.get()),
                                        android::window_shared_image_consumer(owner.get()));})==0);
  REQUIRE(codec.start()==0);
  bool sent=false,eos=false;std::uint64_t rendered=0,dropped=0,outputs=0,updates=0,gpu_frames=0,last_serial=0;
  double update_ms=0,update_max_ms=0;std::int64_t late_ns=0,late_max_ns=0;
  auto latch=[&] {
    std::shared_ptr<const android::SurfaceFrame> frame;
    {std::lock_guard lock(window->lock);frame=window->frame;}
    if(!frame || frame->serial==last_serial)return;
    last_serial=frame->serial;if(frame->image)++gpu_frames;
    const std::int64_t late=now_ns()-(start.load()+frame->timestamp_ns);late_ns+=late;late_max_ns=std::max(late_max_ns,late);
    auto begin=clock_type::now();
    env->CallVoidMethod(object,update);REQUIRE_FALSE(env->ExceptionCheck());
    GL_CALL(PFNGLDRAWARRAYSPROC,"glDrawArrays",GL_TRIANGLES,0,3);
    auto ms=std::chrono::duration<double,std::milli>(clock_type::now()-begin).count();
    update_ms+=ms;update_max_ms=std::max(update_max_ms,ms);++updates;
  };
  auto deadline=clock_type::now()+std::chrono::seconds(180);
  auto begin=clock_type::now();auto cpu_begin=cpu_seconds();
  while(!eos && clock_type::now()<deadline) {
    latch();
    if(!sent && codec.input(0)==0) {
      auto& packet=extractor.sample();
      if(packet.track>=0) {
        auto buffer=codec.input_buffer(0);REQUIRE(packet.bytes.size()<=buffer.size());
        std::copy(packet.bytes.begin(),packet.bytes.end(),buffer.begin());
        REQUIRE(codec.queue(0,0,packet.bytes.size(),packet.time_us,0)==0);extractor.advance();
      } else {REQUIRE(codec.queue(0,0,0,0,4)==0);sent=true;}
    }
    media::Codec::Info info;auto index=codec.output(&info,1000);
    if(index==-1 || index==-2)continue;REQUIRE(index>=0);
    eos=info.flags&4;if(eos){codec.release(index,false);break;}
    ++outputs;
    if(!start)start=now_ns()-info.time*1000;
    auto due=start+info.time*1000;
    if(now_ns()>due+30000000){++dropped;REQUIRE(codec.release(index,false)==0);continue;}
    while(now_ns()<due-50000000){latch();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    ++rendered;REQUIRE(codec.release(index,true,due)==0);
  }
  for(auto until=clock_type::now()+std::chrono::milliseconds(200);clock_type::now()<until;){latch();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
  GL_CALL(PFNGLFINISHPROC,"glFinish");
  auto elapsed=std::chrono::duration<double>(clock_type::now()-begin).count();auto cpu=cpu_seconds()-cpu_begin;
  codec.release();
  std::ofstream report(report_path,std::ios::app);
  report<<"{\"phase\":\"surface_texture\",\"outputs\":"<<outputs<<",\"rendered\":"<<rendered<<",\"dropped\":"<<dropped
    <<",\"updates\":"<<updates<<",\"gpu_frames\":"<<gpu_frames<<",\"seconds\":"<<elapsed<<",\"cpu_seconds\":"<<cpu
    <<",\"update_avg_ms\":"<<(updates?update_ms/updates:0)<<",\"update_max_ms\":"<<update_max_ms
    <<",\"latch_late_avg_ms\":"<<(updates?late_ns/1e6/updates:0)<<",\"latch_late_max_ms\":"<<late_max_ns/1e6
    <<",\"eos\":"<<eos<<"}\n";
  CHECK(eos);
  env->CallVoidMethod(object,env->GetMethodID(cls,"release","()V"));android::release(window.get());
  env->DeleteLocalRef(object);env->DeleteLocalRef(cls);
  GL_CALL(PFNGLDELETEPROGRAMPROC,"glDeleteProgram",program);GL_CALL(PFNGLDELETETEXTURESPROC,"glDeleteTextures",1,&texture);
  EGL_CALL(PFNEGLMAKECURRENTPROC,"eglMakeCurrent",display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
  EGL_CALL(PFNEGLDESTROYSURFACEPROC,"eglDestroySurface",display,surface);EGL_CALL(PFNEGLDESTROYCONTEXTPROC,"eglDestroyContext",display,context);
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
  method=env->GetStaticMethodID(cls,"image","([B[[B[J)Ljava/lang/String;");REQUIRE(method);
  failure=static_cast<jstring>(env->CallStaticObjectMethod(cls,method,config,packets,times));
  if(env->ExceptionCheck())env->ExceptionDescribe();REQUIRE_FALSE(env->ExceptionCheck());
  if(failure){auto text=env->GetStringUTFChars(failure,nullptr);INFO(std::string(text));CHECK(failure==nullptr);env->ReleaseStringUTFChars(failure,text);}
  env->DeleteLocalRef(config);env->DeleteLocalRef(packets);env->DeleteLocalRef(times);env->DeleteLocalRef(array_type);
  method=env->GetStaticMethodID(cls,"video","(Ljava/lang/String;)Ljava/lang/String;");REQUIRE(method);
  auto video=env->NewStringUTF(VENTURE_VIDEO_FIXTURE);REQUIRE(video);
  failure=static_cast<jstring>(env->CallStaticObjectMethod(cls,method,video));
  if(env->ExceptionCheck())env->ExceptionDescribe();REQUIRE_FALSE(env->ExceptionCheck());
  if(failure){auto text=env->GetStringUTFChars(failure,nullptr);INFO(std::string(text));CHECK(failure==nullptr);env->ReleaseStringUTFChars(failure,text);}
  env->DeleteLocalRef(video);
  env->DeleteLocalRef(path);env->DeleteLocalRef(cls);
  check_surface_texture_gl(env);
  measure_surface_texture_rate(env);
  java::shutdown_media_players();CHECK(java::stop_jvm(&error));
}
