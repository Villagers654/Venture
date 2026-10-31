#include "android/errors.h"
#include "hle/libandroid/libandroid.h"
#include "hle/libandroid/parts.h"
#include "android/handles.h"
#include "core/address_space.h"
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
constexpr int unsupported = -10002, invalid_object = -10003, invalid_parameter = -10004,
              invalid_operation = -10005;
struct Format {
  using Value = std::variant<std::int32_t, std::int64_t, float, double, std::string, std::vector<std::byte>>;
  std::mutex mutex;
  std::map<std::string, Value, std::less<>> values;
  std::string description;
};
android::Handles<Format> formats;
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
  *out = value->c_str();
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
  *data = h2g(value->data());
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
struct Extractor { std::mutex mutex; media::Extractor backend; };
android::Handles<Extractor> extractors;
Extractor* extractor_new() { return extractors.create(); }
int extractor_delete(Extractor* p) {
  if (!extractors.get(p)) return invalid_object;
  extractors.erase(p);
  return 0;
}
int extractor_source(Extractor* p, const char* path) {
  auto e=extractors.get(p);if(!e)return invalid_object;
  std::lock_guard lock(e->mutex);
  return path?e->backend.open(media::path_input(path)):invalid_parameter;
}
int extractor_fd(Extractor* p, int fd, std::int64_t off, std::int64_t len) {
  auto e=extractors.get(p);if(!e)return invalid_object;
  std::lock_guard lock(e->mutex);return e->backend.open(media::fd_input(fd,off,len));
}
int extractor_custom(Extractor* p, void*) { return extractor_source(p, nullptr); }
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
struct DataSource {
  std::mutex mutex;
  GuestAddr user = 0, read = 0, size = 0, close = 0;
};
android::Handles<DataSource> sources;
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
using Codec=media::Codec;
android::Handles<Codec> codecs;
media::Codec::Sink surface_sink(void* surface) {
  auto window=android::pin_native_window(h2g(surface));
  if(!window)return {};
  auto* raw=window.get();android::acquire(raw);
  auto owner=std::shared_ptr<android::NativeWindow>(raw,[pin=std::move(window)](android::NativeWindow* p){(void)pin;android::release(p);});
  return [owner](const media::VideoFrame& f) {
    return android::publish_window_rgba(owner.get(),f.rgba,f.width,f.height,false,f.time_us*1000);
  };
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
  auto sink=surface_sink(surface);if(surface && !sink)return invalid_parameter;
  return c->configure(track,std::move(sink));
}
int codec_start(Codec* p){auto c=codecs.get(p);return c?c->start():invalid_object;}
int codec_stop(Codec* p){auto c=codecs.get(p);return c?c->stop():invalid_object;}
int codec_flush(Codec* p){auto c=codecs.get(p);return c?c->flush():invalid_object;}
std::int64_t codec_input(Codec* p,std::int64_t timeout){auto c=codecs.get(p);return c?c->input(timeout):invalid_object;}
std::byte* codec_input_buffer(Codec* p,std::uint64_t index,std::uint64_t* size) {
  if(size)*size=0;auto c=codecs.get(p);if(!c || !size)return nullptr;
  auto b=c->input_buffer(index);*size=b.size();return b.data();
}
int codec_queue(Codec* p,std::uint64_t index,std::uint64_t offset,std::uint64_t size,std::uint64_t time,std::uint32_t flags) {
  auto c=codecs.get(p);return c?c->queue(index,offset,size,std::bit_cast<std::int64_t>(time),flags):invalid_object;
}
std::int64_t codec_output(Codec* p,CodecInfo* info,std::int64_t timeout){auto c=codecs.get(p);return c?c->output(info,timeout):invalid_object;}
const std::byte* codec_output_buffer(Codec* p,std::uint64_t index,std::uint64_t* size) {
  if(size)*size=0;auto c=codecs.get(p);if(!c || !size)return nullptr;
  auto b=c->output_buffer(index);*size=b.size();return b.data();
}
int codec_release(Codec* p,std::uint64_t index,bool render){auto c=codecs.get(p);return c?c->release(index,render):invalid_object;}
int codec_release_at(Codec* p,std::uint64_t index,std::int64_t time){auto c=codecs.get(p);return c?c->release(index,true,time):invalid_object;}
int codec_surface(Codec* p,void* surface) {
  auto c=codecs.get(p);if(!c)return invalid_object;auto sink=surface_sink(surface);
  return sink?c->surface(std::move(sink)):invalid_parameter;
}
Format* codec_format(Codec* p) {
  auto c=codecs.get(p);if(!c)return nullptr;auto track=c->format();if(track.mime.empty())return nullptr;
  bool video=media::video_mime(track.mime);track.mime=video?"video/raw":"audio/raw";track.config.clear();
  auto f=make_track_format(track);
  if(!video)format_set(f,"pcm-encoding",std::int32_t(2));
  else {
    format_set(f,"stride",std::int32_t(track.width));format_set(f,"slice-height",std::int32_t(track.height));
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
void null_buffer(CallContext& ctx) {
  if (ctx.x(2)) *g2h<std::uint64_t>(ctx.x(2)) = 0;
  ctx.return_x(0);
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
