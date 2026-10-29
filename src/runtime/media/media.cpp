#include "media/media.h"
#include "audio/audio.h"
#include "common/log.h"
#include "common/paths.h"
#include "common/environment.h"
#include "av_telemetry.h"
#include <format>
#include "hle/libc/vfs.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <climits>
#include <cstring>
#include <limits>
#include <mutex>
#include <future>
#include <condition_variable>
#include <deque>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/mem.h>
#include <libavutil/mathematics.h>
#include <libavutil/log.h>
#include <libavutil/sha.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

namespace venture::media {
namespace {
constexpr std::size_t max_packet = 4*1024*1024;
constexpr AVRational microseconds{1,1000000};
#ifdef _WIN32
bool hardware_video_allowed() {
  if(venture::environment::getenv("VENTURE_MEDIA_SOFTWARE_VIDEO")){log::info("media","Hardware video disabled by software diagnostic");return false;}
  // D3D11 does not honor Vulkan ICD overrides. Software/suite/CPU policy
  // admission must therefore opt out before any hardware device is created.
  if(auto background=venture::environment::getenv("VENTURE_GPU_POLICY_BACKGROUND");background && *background){log::info("media","Hardware video disabled for background admission {}",background);return false;}
  auto admitted=venture::environment::getenv("VENTURE_GPU_POLICY_ADMITTED");
  auto allowed=!admitted || std::string_view(admitted).find("hardware")!=std::string_view::npos;
  if(!allowed)log::info("media","Hardware video disabled for admission {}",admitted);
  return allowed;
}
#endif
#ifdef _WIN32
using Module = HMODULE;
Module load(const std::filesystem::path& p) { return LoadLibraryW(p.c_str()); }
void unload(Module m) { if(m) FreeLibrary(m); }
void* symbol(Module m,const char* n) { return reinterpret_cast<void*>(GetProcAddress(m,n)); }
#else
using Module = void*;
Module load(const std::filesystem::path& p) { return dlopen(p.c_str(),RTLD_NOW|RTLD_LOCAL); }
void unload(Module m) { if(m) dlclose(m); }
void* symbol(Module m,const char* n) { return dlsym(m,n); }
#endif
}
struct Library::Impl {
  std::vector<Module> modules;
  bool ready = false;
#define MEDIA_FUNCTIONS(X) \
  X(avutil_version) X(avcodec_version) X(avformat_version) \
  X(av_malloc) X(av_free) X(av_max_alloc) X(av_rescale_q) X(av_log_set_level) \
  X(avio_alloc_context) X(avio_context_free) X(avformat_alloc_context) \
  X(av_find_input_format) X(avformat_open_input) X(avformat_find_stream_info) \
  X(avformat_close_input) X(av_read_frame) X(av_seek_frame) \
  X(av_packet_alloc) X(av_packet_free) X(av_packet_unref) X(av_new_packet) \
  X(av_packet_get_side_data) X(av_packet_new_side_data) \
  X(avcodec_find_decoder) X(avcodec_alloc_context3) X(avcodec_parameters_to_context) \
  X(avcodec_get_name) X(avcodec_get_hw_config) X(av_hwdevice_ctx_create) X(av_hwframe_transfer_data) \
  X(avcodec_open2) X(avcodec_free_context) X(avcodec_send_packet) \
  X(avcodec_receive_frame) X(avcodec_flush_buffers) \
  X(av_frame_alloc) X(av_frame_free) X(av_frame_unref) X(av_frame_apply_cropping) X(av_frame_copy_props) \
  X(sws_getCachedContext) X(sws_scale) X(sws_freeContext) \
  X(sws_getCoefficients) X(sws_setColorspaceDetails) \
  X(av_sample_fmt_is_planar) X(av_get_bytes_per_sample)
  // The built-in SHA implementation identifies the bounded input region.
#define MEDIA_HASH_FUNCTIONS(X) X(av_sha_alloc) X(av_sha_init) X(av_sha_update) X(av_sha_final)
#define FIELD(n) decltype(&::n) n = nullptr;
  MEDIA_FUNCTIONS(FIELD)
  MEDIA_HASH_FUNCTIONS(FIELD)
#undef FIELD
  ~Impl() { for(auto i=modules.rbegin();i!=modules.rend();++i) unload(*i); }
};
Library::Library(const std::filesystem::path& dir):impl(std::make_unique<Impl>()) {
  if(!dir.is_absolute()) return;
#ifdef _WIN32
  const char* names[] = {"avutil-60.dll","avcodec-62.dll","avformat-62.dll","swscale-9.dll"};
#else
  const char* names[] = {"libavutil.so.60","libavcodec.so.62","libavformat.so.62","libswscale.so.9"};
#endif
  for(auto name:names) {
    auto module=load(dir/name);
    if(!module) { log::warn("media","LGPL audio libraries unavailable; unsupported media"); return; }
    impl->modules.push_back(module);
  }
#define RESOLVE(n) \
  for(auto module:impl->modules) if(auto p=symbol(module,#n)) {impl->n=reinterpret_cast<decltype(impl->n)>(p);break;} \
  if(!impl->n) {log::warn("media","Audio backend missing required export {}",#n);return;}
  MEDIA_FUNCTIONS(RESOLVE)
  MEDIA_HASH_FUNCTIONS(RESOLVE)
#undef RESOLVE
  if((impl->avutil_version()>>16)!=60 || (impl->avcodec_version()>>16)!=62 ||
     (impl->avformat_version()>>16)!=62) {
    log::warn("media","Audio backend ABI unsupported"); return;
  }
  // No guest paths/metadata are emitted by FFmpeg's arbitrary-data diagnostics.
  impl->av_log_set_level(venture::environment::getenv("VENTURE_MEDIA_FFMPEG_LOG")?AV_LOG_WARNING:AV_LOG_QUIET);
  impl->av_max_alloc(128*1024*1024);
  impl->ready=true;
}
Library::~Library()=default;
bool Library::available() const {return impl->ready;}
std::shared_ptr<Library> Library::get() {
  static auto value=std::make_shared<Library>(paths::executable().parent_path());
  return value;
}
Input file_input(std::shared_ptr<hle::libc::File> file,std::int64_t off,std::int64_t len) {
  hle::libc::Stat stat;
  if(!file || off<0 || len<0 || file->stat(&stat) || stat.size<off) return {};
  len=std::min(len,stat.size-off);
  return {len,[file=std::move(file),off,len](void* out,std::uint64_t size,std::int64_t pos) {
    if(pos<0 || pos>len) return std::int64_t(-1);
    return file->pread(out,std::min<std::uint64_t>(size,len-pos),off+pos);
  }};
}
Input fd_input(int fd,std::int64_t off,std::int64_t len) {
  return file_input(hle::libc::FdTable::get().file(fd),off,len);
}
Input path_input(std::string_view path) {
  if(path.starts_with("file://")) path.remove_prefix(7);
  if(path.find("://")!=path.npos) return {};
  std::string name(path);
  auto fd=hle::libc::fd_open(name.c_str(),0);
  if(fd<0) return {};
  auto input=fd_input(static_cast<int>(fd),0,INT64_MAX);
  hle::libc::FdTable::get().close(static_cast<int>(fd));
  return input;
}
struct Extractor::Impl {
  std::shared_ptr<Library> library;
  Input input;
  AVFormatContext* format=nullptr;
  AVIOContext* io=nullptr;
  AVPacket* packet=nullptr;
  std::int64_t position=0;
  std::vector<Track> tracks;
  std::vector<bool> selected;
  Sample current;
  std::string hash;
  int failure=0;
  std::atomic<bool> cancelled{false};
  std::chrono::steady_clock::time_point deadline;
  explicit Impl(std::shared_ptr<Library> l):library(std::move(l)) {}
  auto& api() {return *library->impl;}
  void close() {
    if(packet) api().av_packet_free(&packet);
    if(format) api().avformat_close_input(&format);
    if(io) {api().av_free(io->buffer);io->buffer=nullptr;api().avio_context_free(&io);}
    tracks.clear();selected.clear();current={};hash.clear();failure=0;
  }
  ~Impl() {close();}
  static int interrupted(void* p) {
    auto& s=*static_cast<Impl*>(p);
    return s.cancelled || std::chrono::steady_clock::now()>s.deadline;
  }
  static int read(void* p,std::uint8_t* buffer,int n) {
    auto& s=*static_cast<Impl*>(p);
    if(interrupted(p)) return AVERROR_EXIT;
    auto got=s.input.read_at(buffer,std::max(n,0),s.position);
    if(got<0 || got>n) return AVERROR(EIO);
    if(!got) return AVERROR_EOF;
    s.position+=got; return static_cast<int>(got);
  }
  static std::int64_t seek_io(void* p,std::int64_t off,int whence) {
    auto& s=*static_cast<Impl*>(p);
    if(whence&AVSEEK_SIZE) return s.input.length;
    whence &= ~AVSEEK_FORCE;
    std::int64_t base=whence==SEEK_SET?0:whence==SEEK_CUR?s.position:whence==SEEK_END?s.input.length:-1;
    if(base<0 || (off>0 && base>INT64_MAX-off) || (off<0 && off<-base)) return AVERROR(EINVAL);
    auto next=base+off;
    if(next<0 || next>s.input.length) return AVERROR(EINVAL);
    return s.position=next;
  }
  void begin() { deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5); }
};
Extractor::Extractor(std::shared_ptr<Library> l):impl(std::make_unique<Impl>(std::move(l))) {}
Extractor::~Extractor()=default;
int Extractor::open(Input input) {
  auto& s=*impl;
  s.close();s.cancelled=false;s.position=0;
  if(!s.library->available() || !input.read_at || input.length<=0) return unsupported;
  s.input=std::move(input);s.begin();
  auto& a=s.api();
  if(s.input.length>512*1024*1024) {log::warn("media","Audio source exceeds bounded region limit");return unsupported;}
  auto sha=a.av_sha_alloc();if(!sha)return unsupported;
  a.av_sha_init(sha,256);
  std::array<std::uint8_t,65536> bytes{};
  for(std::int64_t offset=0;offset<s.input.length;) {
    if(Impl::interrupted(&s)){a.av_free(sha);return unsupported;}
    auto count=std::min<std::int64_t>(bytes.size(),s.input.length-offset);
    auto got=s.input.read_at(bytes.data(),count,offset);
    if(got<=0 || got>count){a.av_free(sha);return unsupported;}
    a.av_sha_update(sha,bytes.data(),static_cast<std::size_t>(got));offset+=got;
  }
  std::array<std::uint8_t,32> digest{};a.av_sha_final(sha,digest.data());a.av_free(sha);
  const char* hex="0123456789abcdef";
  for(auto byte:digest){s.hash+=hex[byte>>4];s.hash+=hex[byte&15];}
  log::info("media","Bounded source SHA-256 {}: {} bytes",s.hash,s.input.length);
  s.begin();
  auto buffer=static_cast<std::uint8_t*>(a.av_malloc(32768));
  if(!buffer) return unsupported;
  s.io=a.avio_alloc_context(buffer,32768,0,&s,Impl::read,nullptr,Impl::seek_io);
  if(!s.io) {a.av_free(buffer);return unsupported;}
  s.format=a.avformat_alloc_context();
  if(!s.format) {s.close();return unsupported;}
  s.format->pb=s.io;s.format->flags|=AVFMT_FLAG_CUSTOM_IO;
  s.format->interrupt_callback={Impl::interrupted,&s};
  s.format->probesize=1024*1024;s.format->max_analyze_duration=1000000;
  s.format->max_streams=16;s.format->max_index_size=4*1024*1024;s.format->max_probe_packets=64;
  if(a.avformat_open_input(&s.format,nullptr,nullptr,nullptr)<0 ||
     a.avformat_find_stream_info(s.format,nullptr)<0 || s.format->nb_streams>16) {
    s.close();log::warn("media","MOV audio source unsupported or invalid");return unsupported;
  }
  for(unsigned i=0;i<s.format->nb_streams;++i) {
    auto st=s.format->streams[i];auto p=st->codecpar;
    Track track;
    if(p->codec_id==AV_CODEC_ID_AAC) track.mime="audio/mp4a-latm";
    else if(p->codec_id==AV_CODEC_ID_H264) track.mime="video/avc";
    else if(p->codec_id==AV_CODEC_ID_HEVC) track.mime="video/hevc";
    else if(p->codec_id==AV_CODEC_ID_VP8) track.mime="video/x-vnd.on2.vp8";
    else if(p->codec_id==AV_CODEC_ID_VP9) track.mime="video/x-vnd.on2.vp9";
    else track.mime="application/octet-stream";
    track.rate=p->sample_rate;track.channels=p->ch_layout.nb_channels;
    if(p->width<0 || p->height<0 || p->width>16384 || p->height>16384){s.close();return unsupported;}
    track.width=p->width;track.height=p->height;
    track.duration_us=st->duration==AV_NOPTS_VALUE?
      (s.format->duration==AV_NOPTS_VALUE?0:s.format->duration):a.av_rescale_q(st->duration,st->time_base,microseconds);
    if(track.duration_us<0 || track.duration_us>86400LL*1000000){s.close();return unsupported;}
    if(p->extradata_size<0 || p->extradata_size>65536) {s.close();return unsupported;}
    if(p->extradata_size) track.config.assign(reinterpret_cast<std::byte*>(p->extradata),reinterpret_cast<std::byte*>(p->extradata)+p->extradata_size);
    log::info("media","MOV track {}: {} ({}), {} Hz, {} channels, {}x{}, {} us",
      i,track.mime,a.avcodec_get_name(p->codec_id),track.rate,track.channels,track.width,track.height,track.duration_us);
    s.tracks.push_back(std::move(track));
  }
  s.selected.resize(s.tracks.size());s.packet=a.av_packet_alloc();
  if(!s.packet) {s.close();return unsupported;}
  return 0;
}
const std::vector<Track>& Extractor::tracks() const {return impl->tracks;}
int Extractor::select(int index,bool enabled) {
  if(index<0 || static_cast<std::size_t>(index)>=impl->tracks.size()) return invalid;
  impl->selected[index]=enabled;
  if(impl->current.track<0 || !impl->selected[impl->current.track]) advance();
  return 0;
}
bool Extractor::advance() {
  auto& s=*impl;s.current={};
  if(!s.format || std::none_of(s.selected.begin(),s.selected.end(),[](bool b){return b;})) return false;
  s.begin();auto& a=s.api();
  while(!s.cancelled) {
    a.av_packet_unref(s.packet);
    auto status=a.av_read_frame(s.format,s.packet);
    if(status<0){s.failure=status==AVERROR_EOF?0:unsupported;return false;}
    auto p=s.packet;
    if(p->stream_index<0 || static_cast<std::size_t>(p->stream_index)>=s.selected.size()) return false;
    if(!s.selected[p->stream_index]) continue;
    if(p->size<0 || static_cast<std::size_t>(p->size)>max_packet) return false;
    if(p->size)s.current.bytes.assign(reinterpret_cast<std::byte*>(p->data),reinterpret_cast<std::byte*>(p->data)+p->size);
    s.current.track=p->stream_index;s.current.flags=(p->flags&AV_PKT_FLAG_KEY)?1:0;
    std::size_t skip_size=0;
    if(auto data=a.av_packet_get_side_data(p,AV_PKT_DATA_SKIP_SAMPLES,&skip_size);data && skip_size==10) {
      std::memcpy(s.current.skip_samples.data(),data,10);s.current.has_skip_samples=true;
    }
    if(a.av_packet_get_side_data(p,AV_PKT_DATA_ENCRYPTION_INFO,nullptr))s.current.flags|=2;
    auto pts=p->pts==AV_NOPTS_VALUE?p->dts:p->pts;
    s.current.time_us=pts==AV_NOPTS_VALUE?0:a.av_rescale_q(pts,s.format->streams[p->stream_index]->time_base,microseconds);
    if(s.current.time_us<-60LL*1000000 || s.current.time_us>86400LL*1000000) {
      s.current={};s.failure=unsupported;return false;
    }
    return true;
  }
  return false;
}
const Sample& Extractor::sample() const {return impl->current;}
int Extractor::seek(std::int64_t time,int mode) {
  auto& s=*impl;
  if(!s.format || time<0 || mode<0 || mode>2) return invalid;
  av::emit("media_extractor_seek",std::format("\"source_sha256\":\"{}\",\"time_us\":{},\"mode\":{}",s.hash,time,mode));
  s.begin();
  if(s.api().av_seek_frame(s.format,-1,time,mode==1?0:AVSEEK_FLAG_BACKWARD)<0) return unsupported;
  return advance()?0:unsupported;
}
void Extractor::cancel() {impl->cancelled=true;}
int Extractor::error() const {return impl->failure;}
const std::string& Extractor::source_hash() const {return impl->hash;}
struct Decoder::Impl {
  std::shared_ptr<Library> library;
  AVCodecContext* codec=nullptr;AVFrame* frame=nullptr;AVFrame* downloaded=nullptr;
  bool video_backend_logged=false;
  std::int64_t conversion_us=0;
  SwsContext* scale=nullptr;
  explicit Impl(std::shared_ptr<Library> l):library(std::move(l)){}
  auto& api(){return *library->impl;}
  ~Impl(){if(codec)api().avcodec_free_context(&codec);if(frame)api().av_frame_free(&frame);if(downloaded)api().av_frame_free(&downloaded);if(scale)api().sws_freeContext(scale);}
};
namespace {
AVCodecID codec_id(std::string_view mime) {
  if(mime=="audio/mp4a-latm")return AV_CODEC_ID_AAC;
  if(mime=="video/avc")return AV_CODEC_ID_H264;
  if(mime=="video/hevc")return AV_CODEC_ID_HEVC;
  if(mime=="video/x-vnd.on2.vp8")return AV_CODEC_ID_VP8;
  if(mime=="video/x-vnd.on2.vp9")return AV_CODEC_ID_VP9;
  return AV_CODEC_ID_NONE;
}
}
bool video_mime(std::string_view mime) {return mime.starts_with("video/") && codec_id(mime)!=AV_CODEC_ID_NONE;}
Decoder::Decoder(std::shared_ptr<Library> l):impl(std::make_unique<Impl>(std::move(l))){}
Decoder::~Decoder()=default;
int Decoder::configure(const Track& t) {
  auto& s=*impl;
  const bool video=video_mime(t.mime);
  if(!s.library->available() || codec_id(t.mime)==AV_CODEC_ID_NONE || t.config.size()>65536) return unsupported;
  if(video ? (t.width<=0 || t.height<=0 || t.width>8192 || t.height>8192) :
     (t.channels<1 || t.channels>2 || t.rate<8000 || t.rate>192000))return unsupported;
  auto& a=s.api();if(s.codec)a.avcodec_free_context(&s.codec);
  auto codec=a.avcodec_find_decoder(codec_id(t.mime));
  if(!codec || !(s.codec=a.avcodec_alloc_context3(codec))) return unsupported;
  if(!video) {
    s.codec->sample_rate=t.rate;s.codec->ch_layout.order=AV_CHANNEL_ORDER_NATIVE;
    s.codec->ch_layout.nb_channels=t.channels;s.codec->ch_layout.u.mask=t.channels==1?AV_CH_LAYOUT_MONO:AV_CH_LAYOUT_STEREO;
  }
  s.codec->max_pixels=8192LL*8192;
  s.codec->pkt_timebase=microseconds;
  s.codec->width=t.width;s.codec->height=t.height;
  // A stream may contain one slice per picture. Slice-only threading then
  // serializes even very large pictures; bounded frame threading works for
  // those streams without depending on a hardware decoder.
  s.codec->thread_count=video?(std::int64_t(t.width)*t.height>4096LL*2160?8:4):1;
  s.codec->thread_type=video?(FF_THREAD_FRAME|FF_THREAD_SLICE):FF_THREAD_SLICE;
  s.video_backend_logged=false;s.conversion_us=0;
#ifdef _WIN32
  // Measured >4K software movies can miss presentation deadlines. D3D11VA
  // uses the Windows video API across GPU vendors; FFmpeg's default format
  // negotiation retains software fallback for unsupported stream profiles.
  if(video && std::int64_t(t.width)*t.height>4096LL*2160 && hardware_video_allowed()) {
    log::info("media","Considering D3D11VA for {} {}x{}",t.mime,t.width,t.height);
    for(int index=0;auto config=a.avcodec_get_hw_config(codec,index);++index) {
      if(config->device_type==AV_HWDEVICE_TYPE_D3D11VA &&
         (config->methods&AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
        if(a.av_hwdevice_ctx_create(&s.codec->hw_device_ctx,AV_HWDEVICE_TYPE_D3D11VA,nullptr,nullptr,0)<0)
          log::info("media","D3D11VA device unavailable; retaining software video decoder");
        else log::info("media","D3D11VA device created for {}",t.mime);
        break;
      }
    }
  }
#endif
  if(!t.config.empty()) {
    s.codec->extradata=static_cast<std::uint8_t*>(a.av_malloc(t.config.size()+AV_INPUT_BUFFER_PADDING_SIZE));
    if(!s.codec->extradata)return unsupported;
    std::memset(s.codec->extradata,0,t.config.size()+AV_INPUT_BUFFER_PADDING_SIZE);
    std::memcpy(s.codec->extradata,t.config.data(),t.config.size());s.codec->extradata_size=static_cast<int>(t.config.size());
  }
  if(a.avcodec_open2(s.codec,codec,nullptr)<0) return unsupported;
  if(!s.frame)s.frame=a.av_frame_alloc();
  return s.frame?0:unsupported;
}
int Decoder::send(const Sample* sample) {
  auto& s=*impl;if(!s.codec)return bad_state;auto& a=s.api();
  if(!sample) return a.avcodec_send_packet(s.codec,nullptr);
  if(sample->bytes.size()>max_packet)return invalid;
  if(sample->flags&2)return unsupported;
  auto p=a.av_packet_alloc();if(!p)return unsupported;
  int result=a.av_new_packet(p,static_cast<int>(sample->bytes.size()));
  if(result>=0) {
    std::memcpy(p->data,sample->bytes.data(),sample->bytes.size());p->pts=sample->time_us;
    p->dts=s.codec->codec_type==AVMEDIA_TYPE_VIDEO?AV_NOPTS_VALUE:p->pts;
    if(sample->has_skip_samples) {
      auto data=a.av_packet_new_side_data(p,AV_PKT_DATA_SKIP_SAMPLES,10);
      if(!data){a.av_packet_free(&p);return unsupported;}
      std::memcpy(data,sample->skip_samples.data(),10);
    }
    result=a.avcodec_send_packet(s.codec,p);
  }
  a.av_packet_free(&p);return result;
}
int Decoder::receive(Pcm* out) {
  auto& s=*impl;if(!s.codec || !out)return bad_state;
  auto& a=s.api();auto f=s.frame;a.av_frame_unref(f);
  auto result=a.avcodec_receive_frame(s.codec,f);
  if(result==AVERROR(EAGAIN))return 0;if(result==AVERROR_EOF)return -1;if(result<0)return unsupported;
  int channels=f->ch_layout.nb_channels;
  if(channels<1 || channels>2 || f->nb_samples<0 || f->nb_samples>8192 || f->sample_rate<8000 || f->sample_rate>192000)return unsupported;
  auto format=static_cast<AVSampleFormat>(f->format);
  bool planar=a.av_sample_fmt_is_planar(format);int bytes=a.av_get_bytes_per_sample(format);
  if(bytes<=0)return unsupported;
  out->rate=f->sample_rate;out->channels=channels;out->time_us=f->pts==AV_NOPTS_VALUE?0:f->pts;
  out->samples.resize(static_cast<std::size_t>(f->nb_samples)*channels);
  for(int i=0;i<f->nb_samples;++i)for(int c=0;c<channels;++c) {
    auto p=f->extended_data[planar?c:0]+(planar?i:i*channels+c)*bytes;float value;
    if(format==AV_SAMPLE_FMT_FLTP || format==AV_SAMPLE_FMT_FLT)std::memcpy(&value,p,4);
    else if(format==AV_SAMPLE_FMT_S16P || format==AV_SAMPLE_FMT_S16){std::int16_t v;std::memcpy(&v,p,2);value=v/32768.0f;}
    else if(format==AV_SAMPLE_FMT_S32P || format==AV_SAMPLE_FMT_S32){std::int32_t v;std::memcpy(&v,p,4);value=static_cast<float>(v/2147483648.0);}
    else return unsupported;
    // AAC float decoding can legitimately overshoot full scale. Preserve the
    // decoder's PCM; the existing mixer clips only its final device output.
    out->samples[static_cast<std::size_t>(i)*channels+c]=std::isfinite(value)?value:0.0f;
  }
  return 1;
}
void Decoder::flush(){if(impl->codec)impl->api().avcodec_flush_buffers(impl->codec);}
std::int64_t Decoder::video_conversion_us() const{return impl->conversion_us;}
int Decoder::receive(VideoFrame* out,std::int64_t discard_before_us,int output_width,int output_height) {
  auto& s=*impl;if(!s.codec || !out || s.codec->codec_type!=AVMEDIA_TYPE_VIDEO)return bad_state;
  auto& a=s.api();auto f=s.frame;a.av_frame_unref(f);
  auto result=a.avcodec_receive_frame(s.codec,f);
  if(result==AVERROR(EAGAIN))return 0;if(result==AVERROR_EOF)return -1;if(result<0)return unsupported;
  if(!s.video_backend_logged) {
    log::info("media","Video decode backend={} {}x{} hardware_device={}",f->format==AV_PIX_FMT_D3D11?"D3D11VA":"software",f->width,f->height,s.codec->hw_device_ctx!=nullptr);
    s.video_backend_logged=true;
  }
  out->time_us=f->best_effort_timestamp==AV_NOPTS_VALUE?0:f->best_effort_timestamp;
  if(out->time_us<discard_before_us)return 2;
  auto conversion_begin=std::chrono::steady_clock::now();
  if(f->format==AV_PIX_FMT_D3D11) {
    if(!s.downloaded)s.downloaded=a.av_frame_alloc();if(!s.downloaded)return unsupported;
    a.av_frame_unref(s.downloaded);
    if(a.av_hwframe_transfer_data(s.downloaded,f,0)<0 || a.av_frame_copy_props(s.downloaded,f)<0)return unsupported;
    f=s.downloaded;
  }
  if(a.av_frame_apply_cropping(f,AV_FRAME_CROP_UNALIGNED)<0 || f->width<=0 || f->height<=0 ||
     f->width>8192 || f->height>8192)return unsupported;
  auto width=output_width>1 && output_width<=8192?output_width:f->width;
  auto height=output_height>1 && output_height<=8192?output_height:f->height;
  s.scale=a.sws_getCachedContext(s.scale,f->width,f->height,static_cast<AVPixelFormat>(f->format),
    width,height,AV_PIX_FMT_RGBA,SWS_BILINEAR,nullptr,nullptr,nullptr);
  if(!s.scale)return unsupported;
  int space=f->colorspace==AVCOL_SPC_BT709?SWS_CS_ITU709:
    f->colorspace==AVCOL_SPC_BT2020_NCL?SWS_CS_BT2020:SWS_CS_ITU601;
  auto coefficients=a.sws_getCoefficients(space);
  a.sws_setColorspaceDetails(s.scale,coefficients,f->color_range==AVCOL_RANGE_JPEG,
    coefficients,1,0,1<<16,1<<16);
  out->width=width;out->height=height;
  out->time_us=f->best_effort_timestamp==AV_NOPTS_VALUE?0:f->best_effort_timestamp;
  out->rgba.resize(std::size_t(width)*height*4);
  std::uint8_t* data[]={out->rgba.data(),nullptr,nullptr,nullptr};
  int stride[]={width*4,0,0,0};
  auto rows=a.sws_scale(s.scale,f->data,f->linesize,0,f->height,data,stride);
  s.conversion_us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-conversion_begin).count();
  return rows==height?1:unsupported;
}

namespace {
std::mutex codec_registry_mutex;
std::vector<std::weak_ptr<void>> codec_registry;
bool codecs_closing=false;
std::atomic<std::uint64_t> next_codec_id{1};
}
struct Codec::Impl {
  const std::uint64_t id=next_codec_id.fetch_add(1);
  mutable std::mutex mutex;
  std::condition_variable cv;
  Decoder decoder;
  std::mutex decoder_mutex;
  std::atomic<std::uint64_t> generation{0};
  Track track;
  Sink sink;
  std::vector<std::byte> buffer=std::vector<std::byte>(max_packet);
  struct Packet { Sample sample;std::uint32_t flags; };
  struct Output { std::vector<std::byte> bytes;VideoFrame video;Info info; };
  struct Render { VideoFrame video;std::int64_t at; };
  std::deque<Packet> packets;
  std::deque<Output> outputs;
  std::deque<Render> renders;
  bool configured=false,started=false,input_owned=false,output_owned=false;
  bool eos=false,drain=false,eos_output=false,closing=false,format_changed=false;
  int failure=0;
  std::int64_t eos_time=0;
  bool config_packet=false;
  std::thread worker;
  std::mutex close_mutex;
  Impl():worker([this] {
    try{work();}catch(...) {std::lock_guard lock(mutex);failure=unsupported;started=false;cv.notify_all();}
  }){}
  void close() {
    std::lock_guard closing_lock(close_mutex);
    {std::lock_guard lock(mutex);closing=true;started=false;++generation;cv.notify_all();}
    if(worker.joinable())worker.join();
    Sink ending_sink;
    {std::lock_guard lock(mutex);ending_sink=std::move(sink);packets.clear();outputs.clear();renders.clear();}
  }
  ~Impl() {close();}
  void clear() {
    packets.clear();outputs.clear();renders.clear();
    input_owned=output_owned=eos=drain=eos_output=false;failure=0;config_packet=false;eos_time=0;
    ++generation;
    {std::lock_guard decoder_lock(decoder_mutex);decoder.flush();}
    cv.notify_all();
  }
  void work() {
    std::unique_lock lock(mutex);
    while(!closing) {
      const auto epoch=generation.load();
      if(!renders.empty()) {
        auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if(renders.front().at<=now) {
          auto render=std::move(renders.front());renders.pop_front();auto target=sink;
          lock.unlock();auto result=target && generation==epoch?target(render.video):0;
          if(target)av::emit("codec_video_present",std::format("\"codec_id\":{},\"pts_us\":{},\"width\":{},\"height\":{},\"result\":{}",
            id,render.video.time_us,render.video.width,render.video.height,result));
          lock.lock();
          if(generation==epoch && result)failure=unsupported;
          cv.notify_all();continue;
        }
      }
      if(started && !failure && outputs.size()<4 && !eos_output) {
        Output out;int status=0;const bool video=video_mime(track.mime);
        Pcm pcm;
        lock.unlock();
        {
          std::lock_guard decoder_lock(decoder_mutex);
          if(generation==epoch)status=video?decoder.receive(&out.video):decoder.receive(&pcm);
        }
        if(status==1) {
          if(video) {out.info.time=out.video.time_us;out.info.size=static_cast<int>(out.video.rgba.size());}
          else {
            out.info.time=pcm.time_us;out.bytes.resize(pcm.samples.size()*2);
            for(std::size_t i=0;i<pcm.samples.size();++i) {
              auto v=static_cast<std::int16_t>(std::clamp(pcm.samples[i]*32768.0f,-32768.0f,32767.0f));
              std::memcpy(out.bytes.data()+i*2,&v,2);
            }
            out.info.size=static_cast<int>(out.bytes.size());
          }
        }
        lock.lock();if(generation!=epoch)continue;
        if(status==1 || status==-1) {
          if(video && status==1) {
            if(track.width!=out.video.width || track.height!=out.video.height)format_changed=true;
            track.width=out.video.width;track.height=out.video.height;
          }
          if(status==-1){out.info.flags=4;out.info.time=eos_time;eos_output=true;}
          outputs.push_back(std::move(out));cv.notify_all();continue;
        }
        if(status<0){log::warn("media","Codec {} receive failed status={}",track.mime,status);failure=unsupported;cv.notify_all();continue;}
        if(!packets.empty()) {
          auto packet=std::move(packets.front());packets.pop_front();
          if(packet.flags&2) {
            if(!config_packet)track.config.clear();
            if(packet.sample.bytes.size()>65536-track.config.size()) {failure=invalid;cv.notify_all();continue;}
            track.config.insert(track.config.end(),packet.sample.bytes.begin(),packet.sample.bytes.end());
            config_packet=true;
          } else config_packet=false;
          auto config=track;cv.notify_all();lock.unlock();
          int result=0;
          {
            std::lock_guard decoder_lock(decoder_mutex);
            if(generation==epoch) {
              if(packet.flags&2)result=decoder.configure(config);
              else if(!packet.sample.bytes.empty())result=decoder.send(&packet.sample);
            }
          }
          lock.lock();if(generation!=epoch)continue;
          if(result<0){log::warn("media","Codec {} input failed status={} pts_us={} flags={} bytes={}",track.mime,result,packet.sample.time_us,packet.flags,packet.sample.bytes.size());failure=unsupported;cv.notify_all();}
          if(packet.flags&4)drain=true;
          continue;
        }
        if(drain) {
          drain=false;lock.unlock();int result=0;
          {std::lock_guard decoder_lock(decoder_mutex);if(generation==epoch)result=decoder.send(nullptr);}
          lock.lock();if(generation==epoch && result<0)failure=unsupported;
          continue;
        }
      }
      if(renders.empty())cv.wait(lock);
      else cv.wait_until(lock,std::chrono::steady_clock::time_point(std::chrono::nanoseconds(renders.front().at)));
    }
  }
};
Codec::Codec():impl(std::make_shared<Impl>()) {
  std::lock_guard lock(codec_registry_mutex);
  std::erase_if(codec_registry,[](const auto& entry){return entry.expired();});
  codec_registry.push_back(impl);if(codecs_closing)impl->close();
}
void Codec::shutdown_all() {
  std::vector<std::shared_ptr<Impl>> ending;
  {std::lock_guard lock(codec_registry_mutex);codecs_closing=true;
    for(auto& entry:codec_registry)if(auto state=entry.lock())ending.push_back(std::static_pointer_cast<Impl>(state));}
  for(auto& state:ending)state->close();
}
Codec::~Codec(){release();}
void Codec::release(){impl->close();}
int Codec::configure(const Track& track,Sink sink) {
  auto& s=*impl;std::lock_guard lock(s.mutex);
  if(s.started || s.closing)return bad_state;
  s.configured=false;s.clear();s.track=track;s.sink=std::move(sink);
  std::lock_guard decoder_lock(s.decoder_mutex);
  auto result=s.decoder.configure(track);s.configured=result==0;
  if(!result && video_mime(track.mime))av::emit("codec_video_configure",std::format("\"codec_id\":{},\"surface\":{},\"width\":{},\"height\":{}",s.id,bool(s.sink),track.width,track.height));
  return result;
}
int Codec::start() {
  auto& s=*impl;std::lock_guard lock(s.mutex);
  if(!s.configured || s.started || s.closing || s.failure)return bad_state;
  s.started=true;s.format_changed=true;s.cv.notify_all();return 0;
}
int Codec::stop() {
  auto& s=*impl;std::lock_guard lock(s.mutex);
  if(!s.started)return bad_state;
  s.started=false;s.clear();return 0;
}
int Codec::flush() {
  auto& s=*impl;std::lock_guard lock(s.mutex);
  if(!s.started)return bad_state;s.clear();return 0;
}
int Codec::surface(Sink sink) {
  auto& s=*impl;std::lock_guard lock(s.mutex);
  if(!s.configured || s.closing || !video_mime(s.track.mime) || !sink)return bad_state;
  s.renders.clear();s.sink=std::move(sink);s.cv.notify_all();return 0;
}
std::int64_t Codec::input(std::int64_t timeout) {
  auto& s=*impl;std::unique_lock lock(s.mutex);
  auto ready=[&]{return !s.started || s.failure || (!s.input_owned && !s.eos && s.packets.size()<4);};
  if(timeout<0)s.cv.wait(lock,ready);
  else if(timeout>0)s.cv.wait_for(lock,std::chrono::microseconds(timeout),ready);
  if(!s.started)return bad_state;if(s.failure)return s.failure;
  if(!ready())return -1;s.input_owned=true;return 0;
}
std::span<std::byte> Codec::input_buffer(std::uint64_t index,bool require_owned) {
  auto& s=*impl;std::lock_guard lock(s.mutex);
  return !index && s.started && (!require_owned || s.input_owned)?std::span(s.buffer):std::span<std::byte>{};
}
int Codec::queue(std::uint64_t index,std::uint64_t offset,std::uint64_t size,std::int64_t time,std::uint32_t flags) {
  auto& s=*impl;std::lock_guard lock(s.mutex);
  if(!s.started || !s.input_owned)return bad_state;
  if(index || offset>s.buffer.size() || size>s.buffer.size()-offset || (flags&~7u))return invalid;
  Impl::Packet p;p.flags=flags;p.sample.time_us=time;
  if(video_mime(s.track.mime))av::emit("codec_video_queue",std::format("\"codec_id\":{},\"surface\":{},\"pts_us\":{},\"flags\":{},\"bytes\":{}",s.id,bool(s.sink),time,flags,size));
  p.sample.bytes.assign(s.buffer.begin()+offset,s.buffer.begin()+offset+size);
  s.packets.push_back(std::move(p));s.input_owned=false;if(flags&4){s.eos=true;s.eos_time=time;}s.cv.notify_all();return 0;
}
std::int64_t Codec::output(Info* info,std::int64_t timeout) {
  auto& s=*impl;std::unique_lock lock(s.mutex);if(!info)return invalid;
  auto ready=[&]{return !s.started || s.failure || s.format_changed || (!s.outputs.empty() && !s.output_owned);};
  if(timeout<0)s.cv.wait(lock,ready);
  else if(timeout>0)s.cv.wait_for(lock,std::chrono::microseconds(timeout),ready);
  if(!s.started)return bad_state;if(s.failure)return s.failure;
  if(s.format_changed){s.format_changed=false;return -2;}
  if(s.outputs.empty() || s.output_owned)return -1;
  *info=s.outputs.front().info;s.output_owned=true;return 0;
}
std::span<const std::byte> Codec::output_buffer(std::uint64_t index) {
  auto& s=*impl;std::lock_guard lock(s.mutex);
  if(index || !s.output_owned || s.outputs.empty())return {};
  auto& out=s.outputs.front();
  if(video_mime(s.track.mime)) {if(s.sink)return {};return {reinterpret_cast<const std::byte*>(out.video.rgba.data()),out.video.rgba.size()};}
  return out.bytes;
}
int Codec::release(std::uint64_t index,bool render,std::int64_t at) {
  auto& s=*impl;std::lock_guard lock(s.mutex);
  if(index || !s.started || !s.output_owned || s.outputs.empty())return invalid;
  auto& out=s.outputs.front();
  if(video_mime(s.track.mime) && !out.video.rgba.empty())
    av::emit("codec_video_release",std::format("\"codec_id\":{},\"surface\":{},\"pts_us\":{},\"render\":{},\"at_ns\":{}",s.id,bool(s.sink),out.info.time,render,at));
  if(render && s.sink && !out.video.rgba.empty()) {
    // SurfaceTexture is a latest-frame consumer. Bound scheduled releases too.
    if(s.renders.size()==4)s.renders.pop_front();
    s.renders.push_back({std::move(out.video),at});
  }
  s.outputs.pop_front();s.output_owned=false;s.cv.notify_all();return 0;
}
Track Codec::format() const {auto& s=*impl;std::lock_guard lock(s.mutex);auto track=s.track;
  if(video_mime(track.mime))track.color_format=s.sink?color_surface:color_rgba;return track;}
void trim_pcm(Pcm& pcm,std::int64_t begin,std::int64_t end) {
  if(pcm.rate<=0 || pcm.channels<=0)return;
  if(pcm.time_us<begin) {
    auto skip=std::min<std::int64_t>(pcm.samples.size()/pcm.channels,
      ((begin-pcm.time_us)*pcm.rate+500000)/1000000);
    pcm.samples.erase(pcm.samples.begin(),pcm.samples.begin()+skip*pcm.channels);
    pcm.time_us+=skip*1000000/pcm.rate;
  }
  if(end>0) {
    auto keep=std::max<std::int64_t>(0,((end-pcm.time_us)*pcm.rate+500000)/1000000);
    if(static_cast<std::uint64_t>(keep)<pcm.samples.size()/pcm.channels)
      pcm.samples.resize(static_cast<std::size_t>(keep)*pcm.channels);
  }
}
struct Player::Impl {
  const char* api="MediaPlayer";
  mutable std::mutex mutex;
  Input input;
  std::unique_ptr<Extractor> extractor;
  std::unique_ptr<Decoder> decoder;
  std::unique_ptr<Extractor> video_extractor;
  std::unique_ptr<Decoder> video_decoder;
  Codec::Sink video_sink;
  std::function<std::pair<int,int>()> video_extent;
  VideoFrame primed_video;
  bool video_primed=false;
  std::thread video_worker;
  std::mutex close_mutex;
  std::atomic<std::int64_t> video_seek{-1};
  std::atomic<std::uint64_t> video_epoch{0};
  std::shared_ptr<audio::Stream> stream;
  Pcm pending;
  std::size_t pending_offset=0;
  bool looping=false, eof=false, drained=false, done=false, released=false;
  std::atomic<bool> cancelled{false};
  int failure=0;
  std::int64_t duration=0, audio_duration=0, audio_cycle_end=0, base_position=0, frame_origin=0, decode_floor=0;
  int width=0,height=0;
  float left=1,right=1;
  std::int64_t silence_written=0,audio_written=0;
  void video_failed() {
    std::lock_guard lock(mutex);failure=unsupported;done=true;if(stream)stream->stop();
  }
  void video_work() {
    VideoFrame frame=std::move(primed_video);bool pending=video_primed,video_eof=false,video_drained=false;
    std::int64_t previous_clock=0,video_floor=0;
    auto epoch=video_epoch.load();
    while(!cancelled) {
      std::shared_ptr<audio::Stream> output;Codec::Sink sink;
      std::function<std::pair<int,int>()> extent;
      std::int64_t clock=0;bool loop=false;
      {
        std::lock_guard lock(mutex);
        output=stream;sink=video_sink;extent=video_extent;loop=looping;
        if(output)clock=base_position+(output->frames_read.load()-frame_origin)*1000000/output->config.rate;
        if(loop && duration>0)clock%=duration;
      }
      auto target=video_seek.exchange(-1);
      if(target>=0 || (loop && clock<previous_clock)) {
        epoch=video_epoch.load();
        video_floor=target>=0?target:0;
        if(video_extractor->seek(target>=0?target:0,0)){video_failed();break;}
        video_decoder->flush();pending=video_eof=video_drained=false;
      }
      previous_clock=clock;
      if(!output || output->state!=audio::State::started || video_drained) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));continue;
      }
      if(pending) {
        // Decode reference pictures from the preceding keyframe without
        // flashing their obsolete images after a seek.
        if(frame.time_us<video_floor){pending=false;continue;}
        if(frame.time_us<clock-50000) {
          av::emit("media_video_drop",std::format("\"source_id\":{},\"pts_us\":{},\"clock_us\":{},\"reason\":\"late\"",output->telemetry_id,frame.time_us,clock));
          pending=false;continue;
        }
        if(frame.time_us<=clock) {
          if(sink && epoch==video_epoch.load() && !cancelled) {
            auto result=sink(frame);
            av::emit("media_video_present",std::format("\"source_id\":{},\"pts_us\":{},\"clock_us\":{},\"width\":{},\"height\":{},\"result\":{}",
              output->telemetry_id,frame.time_us,clock,frame.width,frame.height,result));
          }
          pending=false;
        } else std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      // Skip expensive download/color conversion for pictures which cannot
      // finish within the presentation tolerance. Decode still consumes all
      // reference pictures; audio remains the presentation clock.
      auto floor=std::max(video_floor,clock+video_decoder->video_conversion_us()-50000);
      auto size=extent?extent():std::pair{0,0};
      auto result=video_decoder->receive(&frame,floor,size.first,size.second);
      if(result==2) {
        av::emit("media_video_drop",std::format("\"source_id\":{},\"pts_us\":{},\"clock_us\":{},\"reason\":\"conversion_deadline\"",output->telemetry_id,frame.time_us,clock));
        continue;
      }
      if(result==1){pending=true;continue;}
      if(result==-1){video_drained=true;continue;}
      if(result<0){video_failed();break;}
      auto& sample=video_extractor->sample();
      if(sample.track>=0) {
        if(video_decoder->send(&sample)<0){video_failed();break;}
        video_extractor->advance();if(video_extractor->error()){video_failed();break;}
      } else if(!video_eof) {if(video_decoder->send(nullptr)<0){video_failed();break;}video_eof=true;}
    }
  }
  void work(audio::Stream& output) {
    std::lock_guard lock(mutex);
    if(released || done)return;
    if(!decoder) {
      // Video-only media uses the same device-consumption clock as AV media.
      std::array<float,2048> silence{};
      auto end=duration*output.config.rate/1000000;
      auto remaining=looping?1024LL:std::max<std::int64_t>(0,end-silence_written);
      auto count=static_cast<unsigned>(std::min<std::int64_t>(1024,remaining));
      silence_written+=output.write(silence.data(),count);
      if(!count){output.source_eos=true;if(!output.ring.readable()){done=true;output.stop();}}
      return;
    }
    // At most one AAC frame remains outside the bounded PCM ring.
    for(int budget=0;budget<16 && output.ring.readable()<static_cast<unsigned>(output.buffer_size.load());++budget) {
      if(pending_offset<pending.samples.size()) {
        auto frames=(pending.samples.size()-pending_offset)/pending.channels;
        auto wrote=output.write(pending.samples.data()+pending_offset,static_cast<unsigned>(frames));
        pending_offset+=static_cast<std::size_t>(wrote)*pending.channels;audio_written+=wrote;
        if(!wrote)return;
        continue;
      }
      pending.samples.clear();pending_offset=0;
      auto status=decoder->receive(&pending);
      if(status==1) {
        if(pending.rate!=output.config.rate || pending.channels!=output.config.channels) {
          done=true;failure=unsupported;output.stop();log::warn("media","Mid-stream audio format change unsupported");return;
        }
        trim_pcm(pending,decode_floor,audio_duration);
        continue;
      }
      if(status==-1) {drained=true;output.source_eos=!video_decoder;break;}
      if(status<0) {done=true;failure=unsupported;output.stop();log::warn("media","AAC decode failed");return;}
      auto& sample=extractor->sample();
      if(sample.track>=0) {
        if(decoder->send(&sample)<0) {done=true;failure=unsupported;output.stop();return;}
        if(!extractor->advance())eof=true;
        if(extractor->error()){done=true;failure=unsupported;output.stop();return;}
      } else if(!eof) eof=true;
      if(eof && extractor->sample().track<0) {
        if(looping && (!video_decoder || audio_duration>=duration)) {
          if(extractor->seek(0,0)) {done=true;failure=unsupported;output.stop();return;}
          decode_floor=0;
          // Continue the decoder without discarding queued samples at the loop.
          eof=false;
        } else if(!drained) decoder->send(nullptr);
      }
    }
    if(drained && video_decoder) {
      // Keep the consumption clock running when video outlasts its audio track.
      auto remaining=std::max<std::int64_t>(0,audio_cycle_end-audio_written);
      if(remaining) {
        std::array<float,2048> silence{};
        audio_written+=output.write(silence.data(),static_cast<unsigned>(std::min<std::int64_t>(1024,remaining)));
        output.source_eos=false;return;
      }
      if(looping) {
        if(extractor->seek(0,0)){done=true;failure=unsupported;output.stop();return;}
        decoder->flush();eof=drained=false;decode_floor=0;audio_written=0;
        audio_cycle_end=duration*output.config.rate/1000000;
        output.source_eos=false;return;
      }
      output.source_eos=true;
    }
    if(drained && output.ring.readable()==0) {
      done=true;output.stop();
      log::info("media","Audio playback completed after PCM consumption");
    }
  }
};
Player::Player(PlayerApi api):impl(std::make_shared<Impl>()) {
  impl->api=api==PlayerApi::open_sl?"OpenSL ES media":"MediaPlayer";
}
Player::~Player(){release();}
int Player::source(Input input) {
  auto& s=*impl;std::lock_guard lock(s.mutex);
  if(s.released || s.stream)return bad_state;
  if(!Library::get()->available() || !input.read_at || input.length<=0) {
    log::warn("media","Audio source unsupported: backend or bounded input unavailable");return unsupported;
  }
  auto read=std::move(input.read_at);
  input.read_at=[read=std::move(read),weak=std::weak_ptr<Impl>(impl)](void* data,std::uint64_t size,std::int64_t offset) {
    auto state=weak.lock();
    return !state || state->cancelled?std::int64_t(-1):read(data,size,offset);
  };
  s.input=std::move(input);return 0;
}
int Player::prepare() {
  // Synchronous prepare waits for the bounded host preparation worker. Guest
  // callbacks and the real-time device callback never demux or allocate here.
  return std::async(std::launch::async,[this]() -> int {
  auto& s=*impl;
  {
    std::unique_lock lock(s.mutex);
    if(s.released || !s.input.read_at)return bad_state;
    if(s.stream) {
      auto state=s.stream->state.load();
      lock.unlock();
      return state==audio::State::stopped || state==audio::State::flushed?seek(0):bad_state;
    }
  }
  std::lock_guard lock(s.mutex);
  if(s.released || s.stream)return bad_state;
  s.extractor=std::make_unique<Extractor>();
  if(s.extractor->open(s.input))return unsupported;
  int index=-1,video_index=-1;
  for(std::size_t i=0;i<s.extractor->tracks().size();++i) {
    auto& t=s.extractor->tracks()[i];
    if(video_mime(t.mime) && video_index<0){video_index=static_cast<int>(i);s.width=t.width;s.height=t.height;}
    if(t.mime=="audio/mp4a-latm" && index<0)index=static_cast<int>(i);
  }
  if(index<0 && video_index<0)return unsupported;
  Track track;
  if(index>=0) {
    track=s.extractor->tracks()[index];s.decoder=std::make_unique<Decoder>();
    if(s.decoder->configure(track) || s.extractor->select(index))return unsupported;
  } else {track.rate=48000;track.channels=2;}
  s.duration=s.audio_duration=track.duration_us;
  if(video_index>=0) {
    auto video=s.extractor->tracks()[video_index];s.duration=std::max(s.duration,video.duration_us);
    s.video_extractor=std::make_unique<Extractor>();s.video_decoder=std::make_unique<Decoder>();
    if(s.video_extractor->open(s.input) || s.video_extractor->select(video_index) || s.video_decoder->configure(video))return unsupported;
    // Pre-roll before audio starts so first-frame decode latency cannot put
    // the picture behind the device-consumption clock. One frame is retained.
    bool draining=false;
    for(int budget=0;budget<128 && !s.cancelled;++budget) {
      auto result=s.video_decoder->receive(&s.primed_video);
      if(result==1){s.video_primed=true;break;}
      if(result<0)return unsupported;
      const auto& packet=s.video_extractor->sample();
      if(packet.track>=0) {
        if(s.video_decoder->send(&packet)<0)return unsupported;
        s.video_extractor->advance();if(s.video_extractor->error())return unsupported;
      } else {
        if(draining || s.video_decoder->send(nullptr)<0)return unsupported;
        draining=true;
      }
    }
    if(!s.video_primed)return unsupported;
  }
  audio::Config config;config.api=s.api;config.format=audio::Format::f32;
  config.rate=track.rate;config.channels=track.channels;config.follow_default_output=true;
  s.audio_cycle_end=s.duration*track.rate/1000000;
  config.callback_frames=audio::refill_quantum(track.rate);config.capacity=8*config.callback_frames;
  int result;s.stream=audio::Stream::create(config,&result);
  if(!s.stream)return unsupported;
  s.stream->media_source_sha256=s.extractor->source_hash();
  s.stream->set_buffer_size(6*config.callback_frames);
  s.stream->gain_left=s.left;s.stream->gain_right=s.right;
  s.stream->launch_worker([weak=std::weak_ptr<Impl>(impl)](audio::Stream& stream) {
    if(auto state=weak.lock())state->work(stream);
  });
  if(s.video_decoder)s.video_worker=std::thread([state=impl.get()] {
    try{state->video_work();}catch(...) {std::lock_guard lock(state->mutex);state->failure=unsupported;state->done=true;if(state->stream)state->stream->stop();}
  });
  log::info("media","AV player prepared source {} -> stream {}: {} Hz, {} channels, {} us",
    s.extractor->source_hash(),s.stream->telemetry_id,track.rate,track.channels,s.duration);
  return 0;
  }).get();
}
int Player::start() {
  auto& s=*impl;
  {
    std::lock_guard lock(s.mutex);
    if(!s.stream || s.released)return bad_state;
  }
  if(completed() && seek(0))return unsupported;
  auto stream=this->stream();
  // Prime on the control/worker path before attaching the host consumer.
  std::async(std::launch::async,[&]{s.work(*stream);}).get();
  auto result=stream->start();
  if(!result) {
    std::lock_guard lock(s.mutex);
    av::emit("media_audio_start",std::format("\"source_sha256\":\"{}\",\"source_id\":{},\"api\":\"{}\"",
      s.extractor->source_hash(),stream->telemetry_id,s.api));
    log::info("media","Source {} -> stream {} playback started",s.extractor->source_hash(),stream->telemetry_id);
  }
  return result;
}
int Player::pause() {auto stream=this->stream();return stream?stream->pause():bad_state;}
int Player::stop() {auto stream=this->stream();if(!stream)return bad_state;stream->stop();return seek(0);}
int Player::seek(std::int64_t time) {
  auto& s=*impl;bool active;std::shared_ptr<audio::Stream> stream;
  {
    std::lock_guard lock(s.mutex);
    if(!s.stream || s.released || time<0)return bad_state;
    stream=s.stream;active=stream->state==audio::State::started;
    stream->stop();stream->flush();stream->source_eos=false;
    time=std::min(time,std::max<std::int64_t>(0,s.duration-1));
    if(s.decoder && s.extractor->seek(time,0))return unsupported;
    if(s.decoder)s.decoder->flush();s.pending={};s.pending_offset=0;s.eof=s.drained=s.done=false;s.failure=0;
    s.base_position=time;s.decode_floor=time;s.frame_origin=stream->frames_read.load();
    s.silence_written=time*stream->config.rate/1000000;s.audio_written=0;
    s.audio_cycle_end=(s.duration-time)*stream->config.rate/1000000;
    ++s.video_epoch;s.video_seek=time;
  }
  std::async(std::launch::async,[&]{s.work(*stream);}).get();
  if(active)stream->start();
  return 0;
}
void Player::surface(Codec::Sink sink,std::function<std::pair<int,int>()> extent) {
  std::lock_guard lock(impl->mutex);impl->video_sink=std::move(sink);impl->video_extent=std::move(extent);
}
void Player::loop(bool enabled) {
  auto& s=*impl;std::lock_guard lock(s.mutex);
  if(s.looping && !enabled && s.stream && s.duration>0) {
    auto read=s.stream->frames_read.load();
    s.base_position=(s.base_position+(read-s.frame_origin)*1000000/s.stream->config.rate)%s.duration;
    s.frame_origin=read;
    if(!s.decoder)s.silence_written=s.base_position*s.stream->config.rate/1000000+s.stream->ring.readable();
  }
  s.looping=enabled;
}
void Player::volume(float left,float right) {
  std::lock_guard lock(impl->mutex);
  impl->left=std::isfinite(left)?std::clamp(left,0.0f,1.0f):0;
  impl->right=std::isfinite(right)?std::clamp(right,0.0f,1.0f):0;
  if(impl->stream){impl->stream->gain_left=impl->left;impl->stream->gain_right=impl->right;}
}
void Player::release() {
  auto& s=*impl;std::lock_guard closing(s.close_mutex);
  s.cancelled=true;
  std::shared_ptr<audio::Stream> stream;
  {
    std::lock_guard lock(s.mutex);s.released=true;
    if(s.extractor)s.extractor->cancel();stream=s.stream;
  }
  if(stream)stream->close();
  if(s.video_worker.joinable())s.video_worker.join();
  std::lock_guard lock(s.mutex);s.video_sink={};s.video_extent={};
}
bool Player::playing() const {auto stream=this->stream();return stream && stream->state==audio::State::started;}
bool Player::completed() const {std::lock_guard lock(impl->mutex);return impl->done;}
int Player::error() const {std::lock_guard lock(impl->mutex);return impl->failure;}
std::int64_t Player::position_us() const {
  auto& s=*impl;std::lock_guard lock(s.mutex);
  if(!s.stream)return 0;
  auto position=s.base_position+(s.stream->frames_read.load()-s.frame_origin)*1000000/s.stream->config.rate;
  return s.duration>0?(s.looping?position%s.duration:std::min(position,s.duration)):position;
}
std::int64_t Player::duration_us() const {std::lock_guard lock(impl->mutex);return impl->duration;}
int Player::video_width() const {std::lock_guard lock(impl->mutex);return impl->width;}
int Player::video_height() const {std::lock_guard lock(impl->mutex);return impl->height;}
std::vector<Track> Player::tracks() const {
  std::lock_guard lock(impl->mutex);return impl->extractor?impl->extractor->tracks():std::vector<Track>{};
}
std::shared_ptr<audio::Stream> Player::stream() const {std::lock_guard lock(impl->mutex);return impl->stream;}
} // namespace venture::media
