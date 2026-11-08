#include "android/errors.h"
#include "hle/libandroid/libandroid.h"
#include "hle/libandroid/parts.h"
#include "android/handles.h"
#include "core/a32_types.h"
#include "core/address_space.h"
#include "core/guest_storage.h"
#include "core/process.h"
#include "media/media.h"
#include "android/native_window.h"
#include <array>
#include <algorithm>
#include <bit>
#include <climits>
#include <deque>
#include <cstring>
#include <map>
#include <variant>
#include <vector>

namespace venture::hle {
void register_image_reader(HleLibrary& lib);  // image_reader.cpp
namespace {
using namespace android_hle;
// AOSP frameworks/av/media/ndk/include/media/NdkMediaError.h.
constexpr int unsupported = -10002, invalid_object = -10003, invalid_parameter = -10004;
struct Format {
  using Value = std::variant<std::int32_t, std::int64_t, float, double, std::string, std::vector<std::byte>>;
  std::mutex mutex;
  std::map<std::string, Value, std::less<>> values;
  std::string description;
};
android::Handles<Format> formats("AMediaFormat");
bool a32_guest() { return process_config().isa() == GuestIsa::a32; }
Format* format_new() { return formats.create(); }
int format_delete(Format* p) {
  if (!formats.get(p)) return invalid_object;
  formats.erase(p);
  return 0;
}
template <class T>
void format_set(Format* p, const char* key, T value) {
  auto f = formats.get(p);
  if (!f || !key) return;
  std::lock_guard lock(f->mutex);
  f->values.insert_or_assign(key, value);
}
template <class T>
bool format_get(Format* p, const char* key, T* value) {
  auto f = formats.get(p);
  if (!f || !key || !value) return false;
  std::lock_guard lock(f->mutex);
  auto it = f->values.find(key);
  if (it == f->values.end()) return false;
  auto* v = std::get_if<T>(&it->second);
  if (!v) return false;
  *value = *v;
  return true;
}
void set_string(Format* p, const char* key, const char* value) {
  if (value) format_set(p, key, std::string(value));
}
bool get_string(Format* p, const char* key, const char** out) {
  auto f = formats.get(p);
  if (!f || !key || !out) return false;
  std::lock_guard lock(f->mutex);
  auto it = f->values.find(key);
  if (it == f->values.end()) return false;
  auto* value = std::get_if<std::string>(&it->second);
  if (!value) return false;
  *out = guest_string(value->c_str());  // A32: an interned low copy
  return true;
}
void set_buffer(Format* p, const char* key, const void* data, std::uint64_t size) {
  if ((!data && size) || size > 4*1024*1024) return;
  std::vector<std::byte> copy(static_cast<std::size_t>(size));
  if (size) std::memcpy(copy.data(), data, copy.size());
  format_set(p, key, std::move(copy));
}
bool get_buffer(Format* p, const char* key, GuestAddr* data, std::uint64_t* size) {
  auto f = formats.get(p);
  if (!f || !key || !data || !size) return false;
  std::lock_guard lock(f->mutex);
  auto it = f->values.find(key);
  if (it == f->values.end()) return false;
  auto* value = std::get_if<std::vector<std::byte>>(&it->second);
  if (!value) return false;
  // A32 guests cannot address the host vector: hand out an interned low copy.
  *data = a32_guest() ? low_data(*value) : h2g(value->data());
  *size = value->size();
  return true;
}
const char* format_string(Format* p) {
  auto f = formats.get(p);
  if (!f) return nullptr;
  std::lock_guard lock(f->mutex);
  f->description = "{";
  for (auto& [key, value] : f->values) {
    if (f->description.size() > 1) f->description += ", ";
    f->description += key + ": ";
    std::visit(
        [&](const auto& v) {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, std::string>)
            f->description += v;
          else if constexpr (std::is_same_v<T, std::vector<std::byte>>)
            f->description += "buffer(" + std::to_string(v.size()) + ")";
          else
            f->description += std::to_string(v);
        },
        value);
  }
  f->description += '}';
  return f->description.c_str();
}
// AMediaDataSource callbacks run on the guest thread calling the extractor.
// A32 guests cannot address the extractor's host buffers: reads land in guest
// storage, at most one 64 KiB stage per call, and are copied out.
struct Custom {
  GuestAddr user = 0, read = 0, close = 0;
  std::mutex mutex;
  GuestStorage stage;
};
GuestThread& caller() { auto t=GuestThread::current();return t?*t:GuestThread::adopt_current(); }
std::int64_t custom_read(Custom& c, void* out, std::uint64_t size, std::int64_t pos) {
  auto& t=caller();
  if(!a32_guest())return static_cast<std::int64_t>(t.call(c.read,{c.user,static_cast<std::uint64_t>(pos),h2g(out),size}));
  size=std::min<std::uint64_t>(size,65536);
  std::lock_guard lock(c.mutex);
  auto stage=c.stage.address()?c.stage.address():c.stage.resize(65536);
  auto got=static_cast<std::int32_t>(t.call32(c.read,a32::CallBuilder32{}.pointer(c.user).value(pos)
                                               .pointer(stage).value(static_cast<std::uint32_t>(size))).x0);
  if(got>0 && static_cast<std::uint64_t>(got)<=size)std::memcpy(out,g2h(stage),static_cast<std::size_t>(got));
  return got;
}
void custom_close(const std::shared_ptr<Custom>& c) {
  if(!c || !c->close || !c->user)return;  // as AOSP NdkDataSource::close
  if(a32_guest())caller().call32(c->close,a32::CallBuilder32{}.pointer(c->user));
  else caller().call(c->close,{c->user});
}
struct Extractor { std::mutex mutex; media::Extractor backend; std::shared_ptr<Custom> custom; };
android::Handles<Extractor> extractors("AMediaExtractor");
Extractor* extractor_new() { return extractors.create(); }
int extractor_delete(Extractor* p) {
  auto e=extractors.get(p);if(!e)return invalid_object;
  extractors.erase(p);
  std::shared_ptr<Custom> custom;
  {std::lock_guard lock(e->mutex);custom=std::move(e->custom);}
  custom_close(custom);  // the extractor releases its data source, as on Android
  return 0;
}
int extractor_open(Extractor* p, media::Input input, std::shared_ptr<Custom> custom = {}) {
  auto e=extractors.get(p);if(!e)return invalid_object;
  std::unique_lock lock(e->mutex);
  auto r=e->backend.open(std::move(input));if(r)custom.reset();
  std::swap(custom,e->custom);lock.unlock();
  custom_close(custom);  // a replaced source is released; a rejected one stays the caller's
  return r;
}
int extractor_source(Extractor* p, const char* path) {
  return path?extractor_open(p,media::path_input(path)):extractors.get(p)?invalid_parameter:invalid_object;
}
int extractor_fd(Extractor* p, int fd, std::int64_t off, std::int64_t len) {
  return extractor_open(p,media::fd_input(fd,off,len));
}
struct DataSource {
  std::mutex mutex;
  GuestAddr user = 0, read = 0, size = 0, close = 0;
};
android::Handles<DataSource> sources("AMediaDataSource");
// A source of unknown size (getSize absent or negative) is read to its end
// once, bounded like every extractor source, and served from host memory.
int extractor_custom(Extractor* p, DataSource* source) {
  auto d=sources.get(source);if(!extractors.get(p))return invalid_object;
  if(!d)return invalid_parameter;
  auto c=std::make_shared<Custom>();GuestAddr get_size;
  {std::lock_guard lock(d->mutex);c->user=d->user;c->read=d->read;c->close=d->close;get_size=d->size;}
  if(!c->read)return invalid_parameter;
  std::int64_t length=-1;
  if(get_size)length=a32_guest()?static_cast<std::int32_t>(caller().call32(get_size,a32::CallBuilder32{}.pointer(c->user)).x0)
                                 :static_cast<std::int64_t>(caller().call(get_size,{c->user}));
  media::Input input{length,[c](void* out,std::uint64_t size,std::int64_t pos){return custom_read(*c,out,size,pos);}};
  if(length<0) {
    auto bytes=std::make_shared<std::vector<std::byte>>();
    for(std::int64_t got=1;got>0;) {
      if(bytes->size()>=512*1024*1024){log::warn("media","AMediaDataSource of unknown size exceeds bounded region limit");return media::unsupported;}
      bytes->resize(bytes->size()+65536);
      got=custom_read(*c,bytes->data()+bytes->size()-65536,65536,static_cast<std::int64_t>(bytes->size()-65536));
      bytes->resize(bytes->size()-65536+static_cast<std::size_t>(std::clamp<std::int64_t>(got,0,65536)));
      if(got<0){log::warn("media","AMediaDataSource read failed: {}",got);return media::unsupported;}
    }
    input={static_cast<std::int64_t>(bytes->size()),[bytes](void* out,std::uint64_t size,std::int64_t pos) {
      if(pos<0 || static_cast<std::uint64_t>(pos)>bytes->size())return std::int64_t(-1);
      auto n=std::min<std::uint64_t>(size,bytes->size()-pos);std::memcpy(out,bytes->data()+pos,n);return static_cast<std::int64_t>(n);
    }};
  }
  log::info("media","AMediaDataSource source of {} bytes{}",input.length,length<0?", size unknown: buffered":"");
  return extractor_open(p,std::move(input),std::move(c));
}
std::uint64_t track_count(Extractor* p) {
  auto e=extractors.get(p);if(!e)return 0;
  std::lock_guard lock(e->mutex);return e->backend.tracks().size();
}
Format* make_track_format(const media::Track& track) {
  auto f=format_new();set_string(f,"mime",track.mime.c_str());
  format_set(f,"sample-rate",std::int32_t(track.rate));
  format_set(f,"channel-count",std::int32_t(track.channels));
  format_set(f,"durationUs",track.duration_us);
  if(track.width)format_set(f,"width",std::int32_t(track.width));
  if(track.height)format_set(f,"height",std::int32_t(track.height));
  if(track.color_format)format_set(f,"color-format",std::int32_t(track.color_format));
  if(!track.config.empty())set_buffer(f,"csd-0",track.config.data(),track.config.size());
  return f;
}
Format* track_format(Extractor* p, std::uint64_t index) {
  auto e=extractors.get(p);if(!e)return nullptr;
  std::lock_guard lock(e->mutex);
  return index<e->backend.tracks().size()?make_track_format(e->backend.tracks()[index]):nullptr;
}
int change_track(Extractor* p,std::uint64_t index,bool enabled) {
  auto e=extractors.get(p);if(!e)return invalid_object;
  std::lock_guard lock(e->mutex);
  return index>INT_MAX?invalid_parameter:e->backend.select(static_cast<int>(index),enabled);
}
int select_audio_track(Extractor* p,std::uint64_t index) {return change_track(p,index,true);}
int unselect_audio_track(Extractor* p,std::uint64_t index) {return change_track(p,index,false);}
bool advance(Extractor* p) {
  auto e=extractors.get(p);if(!e)return false;
  std::lock_guard lock(e->mutex);return e->backend.advance();
}
std::int64_t sample(Extractor* p) {
  auto e=extractors.get(p);if(!e)return -1;
  std::lock_guard lock(e->mutex);return e->backend.sample().time_us;
}
std::int32_t sample_track(Extractor* p) {
  auto e=extractors.get(p);if(!e)return -1;
  std::lock_guard lock(e->mutex);return e->backend.sample().track;
}
std::uint32_t sample_flags(Extractor* p) {
  auto e=extractors.get(p);if(!e)return 0;
  std::lock_guard lock(e->mutex);return e->backend.sample().flags;
}
std::int64_t read_sample(Extractor* p, void* out, std::uint64_t size) {
  auto e=extractors.get(p);if(!e || !out)return -1;
  std::lock_guard lock(e->mutex);const auto& sample=e->backend.sample();
  if(sample.track<0)return -1;
  if(sample.bytes.size()>size)return -1;
  std::memcpy(out,sample.bytes.data(),sample.bytes.size());return sample.bytes.size();
}
int seek_sample(Extractor* p, std::int64_t time, int mode) {
  auto e=extractors.get(p);if(!e)return invalid_object;
  std::lock_guard lock(e->mutex);return e->backend.seek(time,mode);
}
DataSource* source_new() { return sources.create(); }
void source_delete(DataSource* p) { sources.erase(p); }
template <GuestAddr DataSource::* Field>
void source_set(DataSource* p, GuestAddr value) {
  if (auto s = sources.get(p)) {
    std::lock_guard lock(s->mutex);
    s.get()->*Field = value;
  }
}

using CodecInfo=media::Codec::Info;
static_assert(sizeof(CodecInfo)==24 && offsetof(CodecInfo,time)==8);
// A32 guests reach codec buffers through guest-memory copies, since the
// decoder's buffers live in host memory above 4 GiB: getInputBuffer hands out
// guest storage that queueInputBuffer copies in, getOutputBuffer a copy out.
struct Codec : media::Codec {
  std::mutex a32_mutex;
  GuestStorage input32, output32;
};
android::Handles<Codec> codecs("AMediaCodec");
// An ANativeWindow's frame sink (taking each frame's pixels) and consumer size.
std::pair<media::Codec::Sink,media::Codec::Extent> surface_sink(void* surface) {
  auto window=android::pin_native_window(h2g(surface));
  if(!window)return {};
  auto* raw=window.get();android::acquire(raw);
  auto owner=std::shared_ptr<android::NativeWindow>(raw,[pin=std::move(window)](android::NativeWindow* p){(void)pin;android::release(p);});
  return {[owner](media::VideoFrame& f) {
    return android::publish_window_rgba(owner.get(),std::move(f.rgba),f.width,f.height,f.time_us*1000);
  },[owner]{return android::window_consumer_extent(owner.get());}};
}
Codec* codec_new(const char* mime) {
  log::info("media","AMediaCodec_createDecoderByType mime={}",mime?mime:"<null>");
  if(!mime || (std::strcmp(mime,"audio/mp4a-latm") && !media::video_mime(mime)) || !media::Library::get()->available())return nullptr;
  return codecs.create();
}
int codec_delete(Codec* p) {
  auto c=codecs.get(p);if(!c)return invalid_object;c->release();codecs.erase(p);return 0;
}
int codec_configure(Codec* p,Format* format,void* surface,void* crypto,std::uint32_t flags) {
  log::info("media","AMediaCodec_configure surface={} crypto={} flags={}",surface!=nullptr,crypto!=nullptr,flags);
  auto c=codecs.get(p);auto f=formats.get(format);
  if(!c)return invalid_object;if(!f)return invalid_parameter;if(crypto || flags)return unsupported;
  media::Track track;
  {
    std::lock_guard lock(f->mutex);
    auto text=[&](const char* key) {auto it=f->values.find(key);return it==f->values.end()?nullptr:std::get_if<std::string>(&it->second);};
    auto number=[&](const char* key) {auto it=f->values.find(key);auto v=it==f->values.end()?nullptr:std::get_if<std::int32_t>(&it->second);return v?*v:0;};
    auto mime=text("mime");if(!mime)return invalid_parameter;track.mime=*mime;
    track.rate=number("sample-rate");track.channels=number("channel-count");
    track.width=number("width");track.height=number("height");
    for(auto key:{"csd-0","csd-1","csd-2"}) {
      auto it=f->values.find(key);
      if(it!=f->values.end())if(auto bytes=std::get_if<std::vector<std::byte>>(&it->second))
        track.config.insert(track.config.end(),bytes->begin(),bytes->end());
    }
  }
  log::info("media","Codec configure {} {}x{} rate={} channels={} csd={} bytes",track.mime,track.width,track.height,track.rate,track.channels,track.config.size());
  auto [sink,extent]=surface_sink(surface);if(surface && !sink)return invalid_parameter;
  return c->configure(track,std::move(sink),std::move(extent));
}
int codec_start(Codec* p){auto c=codecs.get(p);return c?c->start():invalid_object;}
int codec_stop(Codec* p){auto c=codecs.get(p);return c?c->stop():invalid_object;}
int codec_flush(Codec* p){auto c=codecs.get(p);return c?c->flush():invalid_object;}
std::int64_t codec_input(Codec* p,std::int64_t timeout){auto c=codecs.get(p);return c?c->input(timeout):invalid_object;}
std::byte* codec_input_buffer(Codec* p,std::uint64_t index,std::uint64_t* size) {
  if(size)*size=0;auto c=codecs.get(p);if(!c || !size)return nullptr;
  auto b=c->input_buffer(index);*size=b.size();
  if(!b.data() || !a32_guest())return b.data();
  std::lock_guard lock(c->a32_mutex);
  return g2h<std::byte>(c->input32.resize(std::max<std::uint64_t>(b.size(),1)));
}
int codec_queue(Codec* p,std::uint64_t index,std::uint64_t offset,std::uint64_t size,std::uint64_t time,std::uint32_t flags) {
  auto c=codecs.get(p);if(!c)return invalid_object;
  if(a32_guest()) {
    std::lock_guard lock(c->a32_mutex);
    auto b=c->input_buffer(index);
    if(c->input32.address() && offset<=b.size() && size<=b.size()-offset)
      std::memcpy(b.data()+offset,g2h(c->input32.address()+offset),size);
  }
  return c->queue(index,offset,size,std::bit_cast<std::int64_t>(time),flags);
}
std::int64_t codec_output(Codec* p,CodecInfo* info,std::int64_t timeout){auto c=codecs.get(p);return c?c->output(info,timeout):invalid_object;}
const std::byte* codec_output_buffer(Codec* p,std::uint64_t index,std::uint64_t* size) {
  if(size)*size=0;auto c=codecs.get(p);if(!c || !size)return nullptr;
  auto b=c->output_buffer(index);*size=b.size();
  if(!b.data() || !a32_guest())return b.data();
  std::lock_guard lock(c->a32_mutex);
  auto copy=c->output32.resize(std::max<std::uint64_t>(b.size(),1));
  std::memcpy(g2h(copy),b.data(),b.size());
  return g2h<const std::byte>(copy);
}
int codec_release(Codec* p,std::uint64_t index,bool render){auto c=codecs.get(p);return c?c->release(index,render):invalid_object;}
int codec_release_at(Codec* p,std::uint64_t index,std::int64_t time){auto c=codecs.get(p);return c?c->release(index,true,time):invalid_object;}
int codec_surface(Codec* p,void* surface) {
  auto c=codecs.get(p);if(!c)return invalid_object;auto [sink,extent]=surface_sink(surface);
  return sink?c->surface(std::move(sink),std::move(extent)):invalid_parameter;
}
Format* codec_format(Codec* p) {
  auto c=codecs.get(p);if(!c)return nullptr;auto track=c->format();if(track.mime.empty())return nullptr;
  bool video=media::video_mime(track.mime);track.mime=video?"video/raw":"audio/raw";track.config.clear();
  auto f=make_track_format(track);
  if(!video)format_set(f,"pcm-encoding",std::int32_t(2));
  else {
    format_set(f,"stride",std::int32_t(media::nv12_extent(track.width)));
    format_set(f,"slice-height",std::int32_t(media::nv12_extent(track.height)));
    format_set(f,"crop-left",std::int32_t(0));format_set(f,"crop-top",std::int32_t(0));
    format_set(f,"crop-right",std::int32_t(track.width-1));format_set(f,"crop-bottom",std::int32_t(track.height-1));
  }
  return f;
}
void unavailable(CallContext& ctx) {
  log::warn("media", "{}: no host media backend", ctx.info().name);
  ctx.return_x(0);
}
void bad_object(CallContext& ctx) {
  log::debug("media", "{}: invalid media object", ctx.info().name);
  ctx.return_x(static_cast<std::uint64_t>(static_cast<std::int64_t>(invalid_object)));
}
void null_result(CallContext& ctx) { ctx.return_x(0); }
void dispose_absent(CallContext& ctx) {
  if (ctx.x(0)) log::warn("media", "{}: invalid media object", ctx.info().name);
}
int no_object_out(void*, GuestAddr* out) {
  if (out) *out = 0;
  return invalid_object;
}
void add_handler(HleLibrary& lib, const char* name, Handler handler) {
  // These register-level handlers only read argument registers and return a
  // 32-bit status or null pointer, so they serve A32 guests unchanged. The
  // descriptor (tools/codegen/a32_media.py) still pins each signature.
  if (a32_guest()) a32::register_adapter(lib.soname(), name, handler);
  lib.add_function(name, make_thunk(lib.soname(), name, handler));
}
void key(HleLibrary& lib, const char* symbol, const char* value) {
  auto address =
      AddressSpace::get().allocate(8 + std::strlen(value) + 1, kProtRead | kProtWrite, "[media-format-key]");
  if (!address) throw std::bad_alloc();
  *g2h<GuestAddr>(address) = address + 8;
  std::memcpy(g2h(address + 8), value, std::strlen(value) + 1);
  // These imports are DATA of type const char*, not the address of text itself.
  lib.add_data(symbol, address);
}
}  // namespace
void register_libmediandk() {
  auto library = std::make_shared<HleLibrary>("libmediandk.so");
  auto& lib = *library;
#define API(name, fn) add<&fn>(lib, name)
  API("AMediaFormat_new", format_new);
  API("AMediaFormat_delete", format_delete);
  API("AMediaFormat_toString", format_string);
  API("AMediaFormat_getInt32", format_get<std::int32_t>);
  API("AMediaFormat_getInt64", format_get<std::int64_t>);
  API("AMediaFormat_getFloat", format_get<float>);
  API("AMediaFormat_getDouble", format_get<double>);
  API("AMediaFormat_setInt32", format_set<std::int32_t>);
  API("AMediaFormat_setInt64", format_set<std::int64_t>);
  API("AMediaFormat_setFloat", format_set<float>);
  API("AMediaFormat_setDouble", format_set<double>);
  API("AMediaFormat_getString", get_string);
  API("AMediaFormat_setString", set_string);
  API("AMediaFormat_getBuffer", get_buffer);
  API("AMediaFormat_setBuffer", set_buffer);
  API("AMediaExtractor_new", extractor_new);
  API("AMediaExtractor_delete", extractor_delete);
  API("AMediaExtractor_setDataSource", extractor_source);
  API("AMediaExtractor_setDataSourceFd", extractor_fd);
  API("AMediaExtractor_setDataSourceCustom", extractor_custom);
  API("AMediaExtractor_getTrackCount", track_count);
  API("AMediaExtractor_getTrackFormat", track_format);
  API("AMediaExtractor_selectTrack", select_audio_track);
  API("AMediaExtractor_unselectTrack", unselect_audio_track);
  API("AMediaExtractor_advance", advance);
  API("AMediaExtractor_getSampleTime", sample);
  API("AMediaExtractor_getSampleTrackIndex", sample_track);
  API("AMediaExtractor_getSampleFlags", sample_flags);
  API("AMediaExtractor_readSampleData", read_sample);
  API("AMediaExtractor_seekTo", seek_sample);
  API("AMediaCodec_createDecoderByType", codec_new);
  API("AMediaCodec_configure", codec_configure);
  API("AMediaCodec_delete", codec_delete);
  API("AMediaCodec_start", codec_start);
  API("AMediaCodec_stop", codec_stop);
  API("AMediaCodec_flush", codec_flush);
  API("AMediaCodec_dequeueInputBuffer", codec_input);
  API("AMediaCodec_getInputBuffer", codec_input_buffer);
  API("AMediaCodec_queueInputBuffer", codec_queue);
  API("AMediaCodec_dequeueOutputBuffer", codec_output);
  API("AMediaCodec_getOutputBuffer", codec_output_buffer);
  API("AMediaCodec_releaseOutputBuffer", codec_release);
  API("AMediaCodec_getOutputFormat", codec_format);
  API("AMediaCodec_setOutputSurface", codec_surface);
  API("AMediaCodec_releaseOutputBufferAtTime", codec_release_at);
  API("AMediaDataSource_new", source_new);
  API("AMediaDataSource_delete", source_delete);
  API("AMediaDataSource_setUserdata", source_set<&DataSource::user>);
  API("AMediaDataSource_setReadAt", source_set<&DataSource::read>);
  API("AMediaDataSource_setGetSize", source_set<&DataSource::size>);
  API("AMediaDataSource_setClose", source_set<&DataSource::close>);
  API("AMediaCodec_createInputSurface", no_object_out);
#undef API
  register_image_reader(lib);
  for (auto name : {"AMediaCodec_createEncoderByType",
                    "AMediaCodec_createCodecByName", "AMediaMuxer_new", "AMediaCrypto_new"})
    add_handler(lib, name, unavailable);
  for (auto name : {"AMediaCodec_signalEndOfInputStream",
                    "AMediaMuxer_addTrack",
                    "AMediaMuxer_delete",
                    "AMediaMuxer_start",
                    "AMediaMuxer_stop",
                    "AMediaMuxer_writeSampleData"})
    add_handler(lib, name, bad_object);
  for (auto name : {"AMediaCrypto_isCryptoSchemeSupported",
                    "AMediaCrypto_requiresSecureDecoderComponent"})
    add_handler(lib, name, null_result);
  for (auto name : {"AMediaCrypto_delete"})
    add_handler(lib, name, dispose_absent);

  // Android API 32 exported MediaFormat key ABI (including pointer indirection).
  // https://android.googlesource.com/platform/frameworks/av/+/android-12.1.0_r1/media/ndk/NdkMediaFormat.cpp
  const std::pair<const char*, const char*> keys[] = {
    {"AAC_DRC_ATTENUATION_FACTOR", "aac-drc-cut-level"},
    {"AAC_DRC_BOOST_FACTOR", "aac-drc-boost-level"},
    {"AAC_DRC_HEAVY_COMPRESSION", "aac-drc-heavy-compression"},
    {"AAC_DRC_TARGET_REFERENCE_LEVEL", "aac-target-ref-level"},
    {"AAC_ENCODED_TARGET_LEVEL", "aac-encoded-target-level"},
    {"AAC_MAX_OUTPUT_CHANNEL_COUNT", "aac-max-output-channel_count"},
    {"AAC_PROFILE", "aac-profile"},
    {"AAC_SBR_MODE", "aac-sbr-mode"},
    {"ALBUM", "album"},
    {"ALBUMART", "albumart"},
    {"ALBUMARTIST", "albumartist"},
    {"ARTIST", "artist"},
    {"AUDIO_PRESENTATION_INFO", "audio-presentation-info"},
    {"AUDIO_PRESENTATION_PRESENTATION_ID", "audio-presentation-presentation-id"},
    {"AUDIO_PRESENTATION_PROGRAM_ID", "audio-presentation-program-id"},
    {"AUDIO_SESSION_ID", "audio-session-id"},
    {"AUTHOR", "author"},
    {"BITRATE_MODE", "bitrate-mode"},
    {"BIT_RATE", "bitrate"},
    {"BITS_PER_SAMPLE", "bits-per-sample"},
    {"CAPTURE_RATE", "capture-rate"},
    {"CDTRACKNUMBER", "cdtracknum"},
    {"CHANNEL_COUNT", "channel-count"},
    {"CHANNEL_MASK", "channel-mask"},
    {"COLOR_FORMAT", "color-format"},
    {"COLOR_RANGE", "color-range"},
    {"COLOR_STANDARD", "color-standard"},
    {"COLOR_TRANSFER", "color-transfer"},
    {"COMPILATION", "compilation"},
    {"COMPLEXITY", "complexity"},
    {"COMPOSER", "composer"},
    {"CREATE_INPUT_SURFACE_SUSPENDED", "create-input-buffers-suspended"},
    {"CRYPTO_DEFAULT_IV_SIZE", "crypto-default-iv-size"},
    {"CRYPTO_ENCRYPTED_BYTE_BLOCK", "crypto-encrypted-byte-block"},
    {"CRYPTO_ENCRYPTED_SIZES", "crypto-encrypted-sizes"},
    {"CRYPTO_IV", "crypto-iv"},
    {"CRYPTO_KEY", "crypto-key"},
    {"CRYPTO_MODE", "crypto-mode"},
    {"CRYPTO_PLAIN_SIZES", "crypto-plain-sizes"},
    {"CRYPTO_SKIP_BYTE_BLOCK", "crypto-skip-byte-block"},
    {"CSD", "csd"},
    {"CSD_0", "csd-0"},
    {"CSD_1", "csd-1"},
    {"CSD_2", "csd-2"},
    {"CSD_AVC", "csd-avc"},
    {"CSD_HEVC", "csd-hevc"},
    {"D263", "d263"},
    {"DATE", "date"},
    {"DISCNUMBER", "discnum"},
    {"DISPLAY_CROP", "crop"},
    {"DISPLAY_HEIGHT", "display-height"},
    {"DISPLAY_WIDTH", "display-width"},
    {"DURATION", "durationUs"},
    {"ENCODER_DELAY", "encoder-delay"},
    {"ENCODER_PADDING", "encoder-padding"},
    {"ESDS", "esds"},
    {"EXIF_OFFSET", "exif-offset"},
    {"EXIF_SIZE", "exif-size"},
    {"FLAC_COMPRESSION_LEVEL", "flac-compression-level"},
    {"FRAME_COUNT", "frame-count"},
    {"FRAME_RATE", "frame-rate"},
    {"GENRE", "genre"},
    {"GRID_COLUMNS", "grid-cols"},
    {"GRID_ROWS", "grid-rows"},
    {"HAPTIC_CHANNEL_COUNT", "haptic-channel-count"},
    {"HDR_STATIC_INFO", "hdr-static-info"},
    {"HDR10_PLUS_INFO", "hdr10-plus-info"},
    {"HEIGHT", "height"},
    {"ICC_PROFILE", "icc-profile"},
    {"INTRA_REFRESH_PERIOD", "intra-refresh-period"},
    {"IS_ADTS", "is-adts"},
    {"IS_AUTOSELECT", "is-autoselect"},
    {"IS_DEFAULT", "is-default"},
    {"IS_FORCED_SUBTITLE", "is-forced-subtitle"},
    {"IS_SYNC_FRAME", "is-sync-frame"},
    {"I_FRAME_INTERVAL", "i-frame-interval"},
    {"LANGUAGE", "language"},
    {"LAST_SAMPLE_INDEX_IN_CHUNK", "last-sample-index-in-chunk"},
    {"LATENCY", "latency"},
    {"LEVEL", "level"},
    {"LOCATION", "location"},
    {"LOOP", "loop"},
    {"LOW_LATENCY", "low-latency"},
    {"LYRICIST", "lyricist"},
    {"MANUFACTURER", "manufacturer"},
    {"MAX_BIT_RATE", "max-bitrate"},
    {"MAX_FPS_TO_ENCODER", "max-fps-to-encoder"},
    {"MAX_HEIGHT", "max-height"},
    {"MAX_INPUT_SIZE", "max-input-size"},
    {"MAX_PTS_GAP_TO_ENCODER", "max-pts-gap-to-encoder"},
    {"MAX_WIDTH", "max-width"},
    {"MIME", "mime"},
    {"MPEG_USER_DATA", "mpeg-user-data"},
    {"MPEG2_STREAM_HEADER", "mpeg2-stream-header"},
    {"MPEGH_COMPATIBLE_SETS", "mpegh-compatible-sets"},
    {"MPEGH_PROFILE_LEVEL_INDICATION", "mpegh-profile-level-indication"},
    {"MPEGH_REFERENCE_CHANNEL_LAYOUT", "mpegh-reference-channel-layout"},
    {"OPERATING_RATE", "operating-rate"},
    {"PCM_ENCODING", "pcm-encoding"},
    {"PRIORITY", "priority"},
    {"PROFILE", "profile"},
    {"PCM_BIG_ENDIAN", "pcm-big-endian"},
    {"PSSH", "pssh"},
    {"PUSH_BLANK_BUFFERS_ON_STOP", "push-blank-buffers-on-shutdown"},
    {"REPEAT_PREVIOUS_FRAME_AFTER", "repeat-previous-frame-after"},
    {"ROTATION", "rotation-degrees"},
    {"SAMPLE_FILE_OFFSET", "sample-file-offset"},
    {"SAMPLE_RATE", "sample-rate"},
    {"SAMPLE_TIME_BEFORE_APPEND", "sample-time-before-append"},
    {"SAR_HEIGHT", "sar-height"},
    {"SAR_WIDTH", "sar-width"},
    {"SEI", "sei"},
    {"SLICE_HEIGHT", "slice-height"},
    {"SLOW_MOTION_MARKERS", "slow-motion-markers"},
    {"STRIDE", "stride"},
    {"TARGET_TIME", "target-time"},
    {"TEMPORAL_LAYER_COUNT", "temporal-layer-count"},
    {"TEMPORAL_LAYER_ID", "temporal-layer-id"},
    {"TEMPORAL_LAYERING", "ts-schema"},
    {"TEXT_FORMAT_DATA", "text-format-data"},
    {"THUMBNAIL_CSD_AV1C", "thumbnail-csd-av1c"},
    {"THUMBNAIL_CSD_HEVC", "thumbnail-csd-hevc"},
    {"THUMBNAIL_HEIGHT", "thumbnail-height"},
    {"THUMBNAIL_TIME", "thumbnail-time"},
    {"THUMBNAIL_WIDTH", "thumbnail-width"},
    {"TILE_HEIGHT", "tile-height"},
    {"TILE_WIDTH", "tile-width"},
    {"TIME_US", "timeUs"},
    {"TITLE", "title"},
    {"TRACK_ID", "track-id"},
    {"TRACK_INDEX", "track-index"},
    {"VALID_SAMPLES", "valid-samples"},
    {"WIDTH", "width"},
    {"XMP_OFFSET", "xmp-offset"},
    {"XMP_SIZE", "xmp-size"},
    {"YEAR", "year"},
  };
  for (auto [symbol, value] : keys) key(lib, (std::string("AMEDIAFORMAT_KEY_") + symbol).c_str(), value);
  Linker::get().register_hle_library(std::move(library));
}
}  // namespace venture::hle
