#include "java/natives/media_bridge.h"
#include "java/natives/path_bridge.h"
#include "java/internal.h"
#include "media/media.h"
#include "hle/libc/vfs.h"
#include "android/native_window.h"
#include "java/natives/surface_bridge.h"
#include "common/paths.h"
#include "common/log.h"
#include <map>
#include <mutex>

namespace venture::java {
namespace {
std::mutex mutex;
std::map<jlong,std::shared_ptr<media::Player>> players;
std::map<jlong,std::shared_ptr<media::Codec>> codecs;
jlong next=1;
bool closing=false;
std::shared_ptr<media::Player> player(jlong id) {
  std::lock_guard lock(mutex);auto it=players.find(id);
  return it==players.end()?nullptr:it->second;
}
template<class F> auto guard(JNIEnv* env,F&& f)->decltype(f()) {
  try{return f();}
  catch(...) {throw_java(env,"java/io/IOException","Host audio media operation failed");return {};}
}

std::shared_ptr<media::Codec> codec(jlong id) {
  std::lock_guard lock(mutex);auto it=codecs.find(id);return it==codecs.end()?nullptr:it->second;
}
media::Codec::Sink window_sink(JNIEnv* env,jobject object) {
  auto* window=acquire_surface_window(env,object);if(!window)return {};
  auto owner=std::shared_ptr<android::NativeWindow>(window,android::release);
  return [owner](const media::VideoFrame& f) {
    return android::publish_window_rgba(owner.get(),f.rgba,f.width,f.height,false,f.time_us*1000);
  };
}
jlong JNICALL codec_create(JNIEnv* env,jclass,jstring mime) {
  return guard(env,[&]()->jlong {
    if(!mime || !media::Library::get()->available())return 0;
    auto text=env->GetStringUTFChars(mime,nullptr);if(!text)return 0;
    std::string name(text);env->ReleaseStringUTFChars(mime,text);
    if(name!="audio/mp4a-latm" && !media::video_mime(name))return 0;
    auto value=std::make_shared<media::Codec>();std::lock_guard lock(mutex);
    if(closing)return 0;auto id=next++;codecs.emplace(id,std::move(value));return id;
  });
}
jint JNICALL codec_configure(JNIEnv* env,jclass,jlong id,jstring mime,jint width,jint height,jint rate,jint channels,jbyteArray bytes,jobject surface) {
  return guard(env,[&]()->jint {
    auto c=codec(id);if(!c || !mime)return media::bad_state;
    auto text=env->GetStringUTFChars(mime,nullptr);if(!text)return media::invalid;
    media::Track track;track.mime=text;env->ReleaseStringUTFChars(mime,text);
    track.width=width;track.height=height;track.rate=rate;track.channels=channels;
    if(bytes){auto size=env->GetArrayLength(bytes);if(size>65536)return media::invalid;
      track.config.resize(size);env->GetByteArrayRegion(bytes,0,size,reinterpret_cast<jbyte*>(track.config.data()));}
    auto sink=window_sink(env,surface);if(surface && !sink)return media::invalid;
    return c->configure(track,std::move(sink));
  });
}
jlong JNICALL codec_control(JNIEnv* env,jclass,jlong id,jint operation,jlong value) {
  return guard(env,[&]()->jlong {
    auto c=codec(id);if(!c)return media::bad_state;
    switch(operation){case 0:return c->start();case 1:return c->stop();case 2:return c->flush();
      case 3:return c->input(value);case 4:return c->format().width;case 5:return c->format().height;case 6:return c->format().color_format;default:return media::invalid;}
  });
}
jobject JNICALL codec_buffer(JNIEnv* env,jclass,jlong id,jboolean input,jint index) {
  return guard(env,[&]()->jobject {
    auto c=codec(id);if(!c)return nullptr;
    if(input){auto b=c->input_buffer(index,false);return b.empty()?nullptr:env->NewDirectByteBuffer(b.data(),b.size());}
    auto b=c->output_buffer(index);return b.empty()?nullptr:env->NewDirectByteBuffer(const_cast<std::byte*>(b.data()),b.size());
  });
}
jint JNICALL codec_queue(JNIEnv* env,jclass,jlong id,jint index,jint offset,jint size,jlong pts,jint flags) {
  return guard(env,[&]()->jint {auto c=codec(id);return c?c->queue(index,offset,size,pts,flags):media::bad_state;});
}
jint JNICALL codec_output(JNIEnv* env,jclass,jlong id,jlong timeout,jlongArray values) {
  return guard(env,[&]()->jint {
    auto c=codec(id);if(!c)return media::bad_state;if(!values || env->GetArrayLength(values)<4)return media::invalid;
    media::Codec::Info info;auto result=c->output(&info,timeout);
    if(result>=0){jlong data[]={info.offset,info.size,info.time,info.flags};env->SetLongArrayRegion(values,0,4,data);}return static_cast<jint>(result);
  });
}
jint JNICALL codec_render(JNIEnv* env,jclass,jlong id,jint index,jboolean render,jlong at) {
  return guard(env,[&]()->jint {auto c=codec(id);return c?c->release(index,render,at):media::bad_state;});
}
jint JNICALL codec_surface(JNIEnv* env,jclass,jlong id,jobject surface) {
  return guard(env,[&]()->jint {auto c=codec(id);if(!c)return media::bad_state;auto sink=window_sink(env,surface);return sink?c->surface(std::move(sink)):media::invalid;});
}
void JNICALL codec_release(JNIEnv* env,jclass,jlong id) {
  guard(env,[&]()->int {std::shared_ptr<media::Codec> c;{std::lock_guard lock(mutex);auto it=codecs.find(id);if(it!=codecs.end()){c=it->second;codecs.erase(it);}}if(c)c->release();return 0;});
}
jlong JNICALL source(JNIEnv* env,jclass,jstring path,jobject fd,jlong offset,jlong length) {
  return guard(env,[&]()->jlong {
    media::Input input;
    if(path) {
      auto count=env->GetStringLength(path);
      if(count>32768)return 0;
      std::u16string text(count,0);env->GetStringRegion(path,0,count,reinterpret_cast<jchar*>(text.data()));
      if(env->ExceptionCheck() || text.find(u'\0')!=text.npos)return 0;
      auto name=paths::utf8(std::filesystem::path(text));
      if(auto guest=framework_path_to_guest(name))name=*guest;
      input=media::path_input(name);
    } else if(fd && offset>=0 && length>=0) {
      auto cls=env->GetObjectClass(fd);
#ifdef _WIN32
      auto field=env->GetFieldID(cls,"handle","J");
      auto handle=field?env->GetLongField(fd,field):-1;
#else
      auto field=env->GetFieldID(cls,"fd","I");
      auto handle=field?env->GetIntField(fd,field):-1;
#endif
      env->DeleteLocalRef(cls);if(env->ExceptionCheck())return 0;
      input=media::file_input(hle::libc::host::import_readonly_handle(static_cast<std::intptr_t>(handle)),offset,length);
    }
    auto value=std::make_shared<media::Player>();if(value->source(std::move(input)))return 0;
    std::lock_guard lock(mutex);if(closing)return 0;
    auto id=next++;players.emplace(id,std::move(value));return id;
  });
}
jint JNICALL control(JNIEnv* env,jclass,jlong id,jint operation,jlong value) {
  return guard(env,[&]()->jint {
    auto p=player(id);if(!p)return media::bad_state;
    switch(operation) {
      case 0:return p->prepare();case 1:return p->start();case 2:return p->pause();
      case 3:return p->stop();case 4:return p->seek(value);case 5:p->loop(value!=0);return 0;
      default:return media::invalid;
    }
  });
}
jlong JNICALL query(JNIEnv* env,jclass,jlong id,jint operation) {
  return guard(env,[&]()->jlong {
    auto p=player(id);if(!p)return -1;
    switch(operation){case 0:return p->duration_us();case 1:return p->position_us();
      case 2:return p->playing();case 3:return p->completed();case 4:return p->error();
      case 5:return p->video_width();case 6:return p->video_height();default:return -1;}
  });
}
void JNICALL volume(JNIEnv* env,jclass,jlong id,jfloat left,jfloat right) {
  guard(env,[&]()->int {if(auto p=player(id))p->volume(left,right);return 0;});
}
void JNICALL surface(JNIEnv* env,jclass,jlong id,jobject object) {
  guard(env,[&]()->int {
    auto p=player(id);if(!p)return 0;
    auto* window=acquire_surface_window(env,object);
    if(!window){p->surface({});return 0;}
    auto owner=std::shared_ptr<android::NativeWindow>(window,android::release);
    {std::lock_guard lock(window->lock);
      log::info("media","MediaPlayer surface {}x{} image_consumer={}",window->width,window->height,window->image_consumer);}
    p->surface([owner](const media::VideoFrame& f) {
      return android::publish_window_rgba(owner.get(),f.rgba,f.width,f.height,false,f.time_us*1000);
    },[owner] {
      std::lock_guard lock(owner->lock);
      return owner->image_consumer && !owner->fixed_consumer_extent?
        std::pair{0,0}:std::pair{owner->width,owner->height};
    });return 0;
  });
}
void JNICALL release(JNIEnv* env,jclass,jlong id) {
  guard(env,[&]()->int {
    std::shared_ptr<media::Player> p;
    {std::lock_guard lock(mutex);auto it=players.find(id);if(it!=players.end()){p=it->second;players.erase(it);}}
    if(p)p->release();return 0;
  });
}
}
void shutdown_media_players() {
  std::map<jlong,std::shared_ptr<media::Player>> ending;
  std::map<jlong,std::shared_ptr<media::Codec>> ending_codecs;
  {std::lock_guard lock(mutex);closing=true;ending.swap(players);ending_codecs.swap(codecs);}
  for(auto& [id,p]:ending)p->release();
  media::Codec::shutdown_all();
}
void register_media_players(JNIEnv* env) {
  register_surface_textures(env);if(env->ExceptionCheck())return;
  auto cls=env->FindClass("android/media/MediaPlayer");if(!cls)return;
  const JNINativeMethod methods[]{
    {const_cast<char*>("nativeSource"),const_cast<char*>("(Ljava/lang/String;Ljava/io/FileDescriptor;JJ)J"),reinterpret_cast<void*>(source)},
    {const_cast<char*>("nativeControl"),const_cast<char*>("(JIJ)I"),reinterpret_cast<void*>(control)},
    {const_cast<char*>("nativeQuery"),const_cast<char*>("(JI)J"),reinterpret_cast<void*>(query)},
    {const_cast<char*>("nativeVolume"),const_cast<char*>("(JFF)V"),reinterpret_cast<void*>(volume)},
    {const_cast<char*>("nativeSurface"),const_cast<char*>("(JLandroid/view/Surface;)V"),reinterpret_cast<void*>(surface)},
    {const_cast<char*>("nativeRelease"),const_cast<char*>("(J)V"),reinterpret_cast<void*>(release)},
  };
  env->RegisterNatives(cls,methods,static_cast<jint>(std::size(methods)));env->DeleteLocalRef(cls);
  if(env->ExceptionCheck())return;
  cls=env->FindClass("android/media/MediaCodec");if(!cls)return;
#define METHOD(n,s,f) {const_cast<char*>(n),const_cast<char*>(s),reinterpret_cast<void*>(f)}
  const JNINativeMethod codec_methods[]{
    METHOD("nativeCreate","(Ljava/lang/String;)J",codec_create),
    METHOD("nativeConfigure","(JLjava/lang/String;IIII[BLandroid/view/Surface;)I",codec_configure),
    METHOD("nativeControl","(JIJ)J",codec_control),METHOD("nativeBuffer","(JZI)Ljava/nio/ByteBuffer;",codec_buffer),
    METHOD("nativeQueue","(JIIIJI)I",codec_queue),METHOD("nativeOutput","(JJ[J)I",codec_output),
    METHOD("nativeRender","(JIZJ)I",codec_render),METHOD("nativeSurface","(JLandroid/view/Surface;)I",codec_surface),
    METHOD("nativeRelease","(J)V",codec_release)
  };
#undef METHOD
  env->RegisterNatives(cls,codec_methods,std::size(codec_methods));env->DeleteLocalRef(cls);

}
}
