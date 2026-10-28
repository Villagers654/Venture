#include "hle/opensles/opensles.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "audio/audio.h"
#include "media/media.h"
#include "audio/guest_abi.h"
#include "av_telemetry.h"
#include "common/log.h"
#include "core/address_space.h"
#include "core/hle.h"
#include "core/linker.h"
#include "core/thread.h"
#include "core/process.h"
#include "core/profile.h"
#include "hle/opensles/abi.h"
#include "platform/thread_priority.h"

namespace venture::hle::opensles {
namespace {
using Result = std::uint32_t;
constexpr Result ok = 0, precondition = 1, invalid = 2, memory_failure = 3,
                 resource_error = 4, resource_lost = 5, insufficient = 7,
                 unsupported = 12;
#include "hle/opensles/interface_ids.inc"
enum class Kind { engine, mix, player, recorder };
enum class Itf {
  object,
  engine,
  play,
  record,
  queue,
  android_queue,
  volume,
  config,
  seek
};
struct Entry {
  GuestAddr buffer;
  std::uint32_t frames, offset = 0;
};
struct Object {
  Kind kind;
  GuestAddr address = 0, parent = 0;
  std::uint32_t realized = 1, play_state = 1, queue_limit = 2, queue_index = 0;
  std::shared_ptr<audio::Stream> stream;
  std::mutex lock;
  std::deque<Entry> queue;
  GuestAddr queue_cb = 0, queue_user = 0, queue_itf = 0;
  GuestAddr object_cb = 0, object_user = 0;
  std::int16_t volume = 0, pan = 0;
  std::uint32_t mute = 0, pan_enabled = 0;
  std::int32_t priority = 0;
  std::uint32_t preemptable = 0;
  std::uint32_t stream_type = 3, performance = 1, recording_preset = 1;
  std::int64_t position_origin = 0;
  std::shared_ptr<media::Player> media_player;
  bool loop=false;
  std::vector<GuestAddr> allocations;
  std::unordered_map<Itf, GuestAddr> interfaces;
  ~Object() {
    if (stream) stream->close();
    for (auto p : allocations) AddressSpace::get().free(p);
  }
};
std::mutex registry_lock;
struct Interface {
  std::shared_ptr<Object> object;
  Itf kind;
};
std::unordered_map<GuestAddr, Interface> registry;
std::unordered_map<Itf, GuestAddr> tables;
std::unordered_map<std::string, GuestAddr> id_addresses;
bool abi32 = false; // selected once before tables or objects are published

std::shared_ptr<Object> find(GuestAddr id) {
  std::lock_guard lock(registry_lock);
  auto it = registry.find(id);
  return it == registry.end() ? nullptr : it->second.object;
}
const char* id_name(const abi::InterfaceId* id) {
  if (!id) return nullptr;
  for (auto& entry : ids)
    if (!std::memcmp(id, &entry.value, sizeof(*id))) return entry.name;
  return nullptr;
}
bool supported(Kind kind, std::string_view name) {
  if (name == "SL_IID_OBJECT") return true;
  if (kind == Kind::engine) return name == "SL_IID_ENGINE";
  if (kind == Kind::mix) return false;
  if (name == "SL_IID_BUFFERQUEUE" ||
      name == "SL_IID_ANDROIDSIMPLEBUFFERQUEUE" ||
      name == "SL_IID_ANDROIDCONFIGURATION")
    return true;
  return kind == Kind::player
             ? (name == "SL_IID_PLAY" || name == "SL_IID_VOLUME" || name == "SL_IID_SEEK")
             : name == "SL_IID_RECORD";
}
Itf interface_kind(std::string_view name) {
  if (name == "SL_IID_ENGINE") return Itf::engine;
  if (name == "SL_IID_PLAY") return Itf::play;
  if (name == "SL_IID_RECORD") return Itf::record;
  if (name == "SL_IID_BUFFERQUEUE") return Itf::queue;
  if (name == "SL_IID_ANDROIDSIMPLEBUFFERQUEUE") return Itf::android_queue;
  if (name == "SL_IID_VOLUME") return Itf::volume;
  if (name == "SL_IID_ANDROIDCONFIGURATION") return Itf::config;
  if (name == "SL_IID_SEEK") return Itf::seek;
  return Itf::object;
}
Result check_interfaces(Kind kind, std::uint32_t count,
                        const GuestAddr* requested,
                        const std::uint32_t* required) {
  if (count > 64 || (count && (!requested || !required))) return invalid;
  for (std::uint32_t i = 0; i < count; ++i) {
    auto name = id_name(g2h<abi::InterfaceId>(requested[i]));
    if (!name || !supported(kind, name)) {
      if (required[i]) {
        log::warn("OpenSL", "Required interface {} is unsupported",
                  name ? name : "unknown UUID");
        return unsupported;
      }
    }
  }
  return ok;
}
GuestAddr allocate(Object& obj, std::uint64_t size, std::string_view name) {
  auto p = AddressSpace::get().allocate(size, kProtRead | kProtWrite, name);
  if (p) obj.allocations.push_back(p);
  return p;
}
std::shared_ptr<Object> make_object(Kind kind, GuestAddr parent) {
  auto obj = std::make_shared<Object>();
  obj->kind = kind;
  obj->parent = parent;
  auto add = [&](Itf itf) {
    auto p = allocate(*obj, abi32 ? 4 : 8, "[opensles-interface]");
    if (!p) throw std::bad_alloc();
    if (abi32) *g2h<std::uint32_t>(p) = h2g32(g2h(tables.at(itf)));
    else *g2h<GuestAddr>(p) = tables.at(itf);
    obj->interfaces[itf] = p;
  };
  add(Itf::object);
  if (kind == Kind::engine) add(Itf::engine);
  if (kind == Kind::player || kind == Kind::recorder) {
    add(kind == Kind::player ? Itf::play : Itf::record);
    add(Itf::queue);
    add(Itf::android_queue);
    add(Itf::config);
    if (kind == Kind::player) {add(Itf::volume);add(Itf::seek);}
  }
  obj->address = obj->interfaces.at(Itf::object);
  return obj;
}
void publish(const std::shared_ptr<Object>& obj) {
  std::lock_guard lock(registry_lock);
  for (auto [kind, p] : obj->interfaces)
    registry.emplace(p, Interface{obj, kind});
}
void update_gain(Object& o) {
  float gain = o.mute ? 0 : std::pow(10.0f, float(o.volume) / 2000);
  float pan = o.pan_enabled ? float(o.pan) / 1000 : 0;
  o.stream->gain_left.store(gain * (pan > 0 ? 1 - pan : 1));
  o.stream->gain_right.store(gain * (pan < 0 ? 1 + pan : 1));
}
Result realize(GuestAddr self, std::uint32_t async) {
  auto o = find(self);
  if (!o) return invalid;
  if (async) {
    log::warn("OpenSL", "Async Realize is unsupported");
    return unsupported;
  }
  std::lock_guard lock(o->lock);
  o->realized = 2;
  return ok;
}
Result object_state(GuestAddr self, std::uint32_t* state) {
  auto o = find(self);
  if (!o || !state) return invalid;
  std::lock_guard lock(o->lock);
  *state = o->realized;
  return ok;
}
Result register_object_callback(GuestAddr self, GuestAddr cb, GuestAddr user) {
  auto o = find(self);
  if (!o) return invalid;
  // AOSP wilhelm/src/itf/IObject.c, IObject_RegisterCallback: stores
  // callback/context under the object lock, including before Realize and
  // when callback is null. Synchronous Realize does not emit an event;
  // asynchronous Realize and resource preemption remain unsupported.
  std::lock_guard lock(o->lock);
  o->object_cb = cb;
  o->object_user = user;
  return ok;
}
Result get_interface(GuestAddr self, const abi::InterfaceId* id,
                     GuestAddr* out) {
  if (!out) return invalid;
  *out = 0;
  auto o = find(self);
  if (!o) return invalid;
  std::lock_guard lock(o->lock);
  auto name = id_name(id);
  if (!name || !supported(o->kind, name)) {
    log::warn("OpenSL", "GetInterface: {} unsupported",
              name ? name : "unknown UUID");
    return unsupported;
  }
  auto itf = interface_kind(name);
  // Android configuration is deliberately obtainable before Realize.
  if (o->realized != 2 && itf != Itf::config) return precondition;
  *out = o->interfaces.at(itf);
  return ok;
}
void destroy(CallContext& ctx, GuestAddr self) {
  auto o = find(self);
  if (!o) return;
  std::vector<std::shared_ptr<Object>> victims;
  {
    std::lock_guard lock(registry_lock);
    victims.push_back(o);
    // Engine destruction invalidates its child objects. Hold their ownership
    // until all callback threads have joined, never while holding the registry.
    if (o->kind == Kind::engine)
      for (auto& [p, entry] : registry)
        if (entry.kind == Itf::object && entry.object->parent == o->address)
          victims.push_back(entry.object);
    for (auto& v : victims)
      for (auto [kind, p] : v->interfaces) registry.erase(p);
  }
  for (auto& v : victims)
    if (v->stream) v->stream->close(&ctx.thread());
}
void abort_async(GuestAddr) { /* Realize is synchronous; there is no async
                                 operation to cancel. */
}
Result set_priority(GuestAddr self, std::int32_t value,
                    std::uint32_t preemptable) {
  auto o = find(self);
  if (!o || preemptable > 1) return invalid;
  // A shared software mixer has no scarce voice allocation to preempt.
  std::lock_guard lock(o->lock);
  o->priority = value;
  o->preemptable = preemptable;
  return ok;
}
Result get_priority(GuestAddr self, std::int32_t* value,
                    std::uint32_t* preemptable) {
  auto o = find(self);
  if (!o || !value || !preemptable) return invalid;
  std::lock_guard lock(o->lock);
  *value = o->priority;
  *preemptable = o->preemptable;
  return ok;
}
void fail_method(CallContext& ctx) {
  log::warn("OpenSL", "{} is unsupported", ctx.info().name);
  ctx.return_x(unsupported);
}
Result create_engine(GuestAddr* out, std::uint32_t option_count,
                     const abi::EngineOption* options, std::uint32_t count,
                     const GuestAddr* requested,
                     const std::uint32_t* required) {
  if (!out) return invalid;
  *out = 0;
  if (option_count > 16 || (option_count && !options)) return invalid;
  for (std::uint32_t i = 0; i < option_count; ++i) {
    // OpenSLES.h: THREADSAFE=1, LOSSOFCONTROL=2. We are always thread-safe.
    if (options[i].feature != 1 && options[i].feature != 2) return unsupported;
    if (options[i].data > 1) return invalid;
    if (options[i].feature == 2 && options[i].data) return unsupported;
  }
  auto result = check_interfaces(Kind::engine, count, requested, required);
  if (result) return result;
  try {
    auto o = make_object(Kind::engine, 0);
    publish(o);
    *out = o->address;
    return ok;
  } catch (const std::bad_alloc&) {
    return memory_failure;
  }
}
Result create_mix(GuestAddr self, GuestAddr* out, std::uint32_t count,
                  const GuestAddr* requested, const std::uint32_t* required) {
  if (!out) return invalid;
  *out = 0;
  auto engine = find(self);
  if (!engine || engine->kind != Kind::engine) return invalid;
  {
    std::lock_guard lock(engine->lock);
    if (engine->realized != 2) return precondition;
  }
  auto result = check_interfaces(Kind::mix, count, requested, required);
  if (result) return result;
  try {
    auto o = make_object(Kind::mix, engine->address);
    publish(o);
    *out = o->address;
    return ok;
  } catch (const std::bad_alloc&) {
    return memory_failure;
  }
}
Result parse_queue(const abi::Data* data, audio::Config& c,
                   std::uint32_t* queue_limit) {
  if (!data || !data->locator || !data->format) return invalid;
  auto loc = g2h<abi::QueueLocator>(data->locator);
  if (loc->type != 6 && loc->type != 0x800007bd) {
    log::warn("OpenSL",
              "Data locator {:#x} unsupported (URI/file playback unavailable)",
              loc->type);
    return unsupported;
  }
  if (!loc->buffers || loc->buffers > 64) return invalid;
  *queue_limit = loc->buffers;
  auto pcm = g2h<abi::Pcm>(data->format);
  if (pcm->type != 2 && pcm->type != 4) return unsupported;
  if (pcm->endian != 2 || pcm->bits != pcm->container || !pcm->rate_millihz ||
      pcm->rate_millihz % 1000)
    return unsupported;
  if (pcm->channels < 1 || pcm->channels > 2) return unsupported;
  // Stereo front-left/right, mono front-center, or Android default/indexed.
  auto mask = pcm->mask;
  if (mask && mask != (pcm->channels == 1 ? 4u : 3u) &&
      mask != (0x80000000u | ((1u << pcm->channels) - 1)))
    return unsupported;
  c.channels = std::int32_t(pcm->channels);
  c.rate = std::int32_t(pcm->rate_millihz / 1000);
  std::uint32_t representation =
      pcm->type == 4 ? g2h<abi::PcmEx>(data->format)->representation
                     : (pcm->bits == 8 ? 2u : 1u);
  if (representation == 3 && pcm->bits == 32)
    c.format = audio::Format::f32;
  else if (representation == 2 && pcm->bits == 8)
    c.format = audio::Format::u8;
  else if (representation == 1) {
    if (pcm->bits == 16)
      c.format = audio::Format::i16;
    else if (pcm->bits == 24)
      c.format = audio::Format::i24;
    else if (pcm->bits == 32)
      c.format = audio::Format::i32;
    else
      return unsupported;
  } else
    return unsupported;
  c.api = "OpenSL ES";
  c.prefill_output = c.direction == audio::Direction::output;
  c.follow_default_output = true;
  // Round the five-millisecond refill quantum up. Two truncated quanta at
  // 44.1 kHz hold 440 frames, less than a ten-millisecond 441-frame packet.
  c.callback_frames = audio::refill_quantum(c.rate);
  c.capacity = std::max(c.callback_frames * 2, c.rate / 50);
  return ok;
}
void queue_work(std::weak_ptr<Object> weak) {
  auto o = weak.lock();
  if (!o) return;
  GuestAddr cb = 0, user = 0, caller = 0;
  std::shared_ptr<audio::Stream> stream;
  {
    std::lock_guard lock(o->lock);
    auto& s = *o->stream;
    if (o->play_state != 3 || (s.state != audio::State::started && s.state != audio::State::starting) ||
        o->queue.empty())
      return;
    auto bytes = audio::sample_bytes(s.config.format) * s.config.channels;
    // Prefill subsequent buffers before the first one completes, so a queue
    // with two buffers stays continuous across completion callback scheduling.
    for (auto& entry : o->queue) {
      if (entry.offset < entry.frames) {
        auto p =
            g2h<std::uint8_t>(entry.buffer) + std::size_t(entry.offset) * bytes;
        auto n = o->kind == Kind::player
                     ? s.write(p, entry.frames - entry.offset)
                     : s.read(p, entry.frames - entry.offset);
        entry.offset += n;
        if (entry.offset != entry.frames) break;
      }
    }
    auto& entry = o->queue.front();
    // Android's AudioTrack MORE_DATA handler returns an OpenSL buffer once
    // copied into AudioTrack-owned storage, before hardware playback. Our ring
    // owns the PCM too; waiting for host consumption delays the next refill.
    if (entry.offset != entry.frames) return;
    o->queue.pop_front();
    ++o->queue_index;
    cb = o->queue_cb;
    user = o->queue_user;
    caller = o->queue_itf;
    stream = o->stream;
  }
  if (cb) {
    const bool measure = av::Telemetry::get().enabled();
    const auto adopt_begin = measure ? av::now_ns() : 0;
    if (measure) stream->callback_adopt_begin_ns.store(adopt_begin, std::memory_order_release);
    auto& thread = GuestThread::adopt_current();
    if (measure) stream->callback_guest_tid.store(thread.tid(), std::memory_order_relaxed);
    const auto callback_begin = measure ? av::now_ns() : 0;
    if (measure) stream->callback_begin_ns.store(callback_begin, std::memory_order_release);
    const auto cpu_begin = measure ? platform::thread_cpu_time_ns() : -1;
    const auto cycles_begin = measure ? platform::thread_cpu_cycles() : -1;
    const auto blocks_begin = measure ? profile::thread_emitted_blocks() : -1;
    if (abi32) thread.call32(cb, a32::CallBuilder32{}.pointer(caller).pointer(user));
    else thread.call(cb, {caller, user});
    if (measure) {
      stream->callback_end_ns.store(av::now_ns(), std::memory_order_release);
      const auto cpu_end = platform::thread_cpu_time_ns();
      const auto cycles_end = platform::thread_cpu_cycles();
      const auto blocks_end = profile::thread_emitted_blocks();
      stream->record_callback(callback_begin - adopt_begin,
                              av::now_ns() - callback_begin,
                              cpu_begin >= 0 && cpu_end >= cpu_begin ? cpu_end - cpu_begin : -1,
                              cycles_begin >= 0 && cycles_end >= cycles_begin ? cycles_end - cycles_begin : -1,
                              callback_begin,
                              blocks_begin >= 0 && blocks_end >= blocks_begin ? blocks_end - blocks_begin : -1);
    }
  }
}
Result create_audio(GuestAddr self, GuestAddr* out, const abi::Data* source,
                    const abi::Data* sink, std::uint32_t count,
                    const GuestAddr* requested, const std::uint32_t* required,
                    bool capture) {
  if (!out) return invalid;
  *out = 0;
  auto engine = find(self);
  if (!engine || engine->kind != Kind::engine) return invalid;
  {
    std::lock_guard lock(engine->lock);
    if (engine->realized != 2) return precondition;
  }
  Kind kind = capture ? Kind::recorder : Kind::player;
  auto result = check_interfaces(kind, count, requested, required);
  if (result) return result;
  if (!source || !sink || !source->locator || !sink->locator) return invalid;
  if (capture) {
    auto io = g2h<abi::IoLocator>(source->locator);
    if (io->type != 3 || io->device_type != 1 || io->device_id != 0xffffffffu ||
        io->object) {
      log::warn("OpenSL", "Recorder requires the default audio input device");
      return unsupported;
    }
  } else {
    auto mix = g2h<abi::OutputMixLocator>(sink->locator);
    if (mix->type != 4) {
      log::warn("OpenSL", "Player sink locator {} is unsupported", mix->type);
      return unsupported;
    }
    auto obj = find(mix->object);
    if (!obj || obj->kind != Kind::mix) return invalid;
    std::lock_guard lock(obj->lock);
    if (obj->realized != 2) return precondition;
  }
  try {
    auto o = make_object(kind, engine->address);
    audio::Config c;
    c.direction = capture ? audio::Direction::input : audio::Direction::output;
    auto locator_type=*g2h<std::uint32_t>(source->locator);
    if(!capture && (locator_type==1 || locator_type==0x800007bc)) {
      if(!source->format || *g2h<std::uint32_t>(source->format)!=1)return unsupported;
      media::Input input;
      if(locator_type==1) {
        auto locator=g2h<abi::UriLocator>(source->locator);
        if(!locator->uri)return invalid;
        input=media::path_input(g2h<const char>(locator->uri));
      } else {
        auto locator=g2h<abi::FdLocator>(source->locator);
        input=media::fd_input(locator->fd,locator->offset,locator->length<0?INT64_MAX:locator->length);
      }
      o->media_player=std::make_shared<media::Player>(media::PlayerApi::open_sl);
      if(o->media_player->source(std::move(input)) || o->media_player->prepare())return unsupported;
      o->stream=o->media_player->stream();
      publish(o);*out=o->address;return ok;
    }
    result = parse_queue(capture ? sink : source, c, &o->queue_limit);
    if (result) return result;
    std::int32_t status;
    o->stream = audio::Stream::create(c, &status);
    if (!o->stream) {
      log::error("OpenSL", "Audio stream open failed with AAudio result {}",
                 status);
      return resource_error;
    }
    o->stream->launch_worker([weak = std::weak_ptr<Object>(o)](audio::Stream&) {
      queue_work(weak);
    });
    publish(o);
    *out = o->address;
    return ok;
  } catch (const std::bad_alloc&) {
    return memory_failure;
  }
}
Result create_player(GuestAddr self, GuestAddr* out, const abi::Data* src,
                     const abi::Data* sink, std::uint32_t count,
                     const GuestAddr* ids, const std::uint32_t* req) {
  return create_audio(self, out, src, sink, count, ids, req, false);
}
Result create_recorder(GuestAddr self, GuestAddr* out, const abi::Data* src,
                       const abi::Data* sink, std::uint32_t count,
                       const GuestAddr* ids, const std::uint32_t* req) {
  return create_audio(self, out, src, sink, count, ids, req, true);
}
Result set_play(GuestAddr self, std::uint32_t state) {
  auto o = find(self);
  if (!o || !o->stream || state < 1 || state > 3) return invalid;
  std::lock_guard lock(o->lock);
  if (o->realized != 2) return precondition;
  std::int32_t status;
  if(o->media_player)
    status=state==3?o->media_player->start():state==2?o->media_player->pause():o->media_player->stop();
  else if (state == 3)
    status = o->stream->start();
  else if (state == 2 && o->kind == Kind::player)
    status = o->stream->pause();
  else
    status = o->stream->stop();
  if (status)
    return status == audio::disconnected ? resource_lost : precondition;
  o->play_state = state;
  if (state == 1)
    o->position_origin = o->kind == Kind::player
                             ? o->stream->frames_read.load()
                             : o->stream->frames_written.load();
  return ok;
}
Result get_play(GuestAddr self, std::uint32_t* state) {
  auto o = find(self);
  if (!o || !state || !o->stream) return invalid;
  std::lock_guard lock(o->lock);
  *state = o->media_player && o->media_player->completed()?1:o->play_state;
  return ok;
}
Result duration(GuestAddr self, std::uint32_t* value) {
  auto o=find(self);if (!o || !value) return invalid;
  *value = o->media_player?static_cast<std::uint32_t>(o->media_player->duration_us()/1000):0xffffffffu;
  return ok;
}
Result position(GuestAddr self, std::uint32_t* value) {
  auto o = find(self);
  if (!o || !value || !o->stream) return invalid;
  std::lock_guard lock(o->lock);
  if(o->media_player){*value=static_cast<std::uint32_t>(o->media_player->position_us()/1000);return ok;}
  auto frames = o->kind == Kind::player ? o->stream->frames_read.load()
                                        : o->stream->frames_written.load();
  *value =
      std::uint32_t(std::max(std::int64_t(0), frames - o->position_origin) *
                    1000 / o->stream->config.rate);
  return ok;
}
Result enqueue(GuestAddr self, GuestAddr buffer, std::uint32_t size) {
  auto o = find(self);
  if (!o || !o->stream || !buffer || !size) return invalid;
  if(o->media_player)return unsupported;
  auto bytes = audio::sample_bytes(o->stream->config.format) *
               o->stream->config.channels;
  if (size % bytes) return invalid;
  std::lock_guard lock(o->lock);
  if (o->realized != 2) return precondition;
  if (o->queue.size() == o->queue_limit) return insufficient;
  o->queue.push_back(Entry{buffer, size / bytes});
  return ok;
}
Result seek_position(GuestAddr self,std::uint32_t milliseconds,std::uint32_t mode) {
  auto o=find(self);if(!o || !o->media_player)return unsupported;
  if(mode!=1 && mode!=2)return invalid;
  std::lock_guard lock(o->lock);
  return o->media_player->seek(std::int64_t(milliseconds)*1000)?precondition:ok;
}
Result seek_loop(GuestAddr self,std::uint32_t enabled,std::uint32_t start,std::uint32_t end) {
  auto o=find(self);if(!o || !o->media_player)return unsupported;
  std::lock_guard lock(o->lock);
  if(enabled>1 || start || (end!=0xffffffffu && end!=o->media_player->duration_us()/1000))return unsupported;
  o->loop=enabled!=0;o->media_player->loop(o->loop);return ok;
}
Result get_loop(GuestAddr self,std::uint32_t* enabled,std::uint32_t* start,std::uint32_t* end) {
  auto o=find(self);if(!o || !o->media_player)return unsupported;
  if(!enabled || !start || !end)return invalid;
  std::lock_guard lock(o->lock);*enabled=o->loop;*start=0;*end=0xffffffffu;return ok;
}
Result clear_queue(GuestAddr self) {
  auto o = find(self);
  if (!o || !o->stream) return invalid;
  std::lock_guard lock(o->lock);
  auto& s = *o->stream;
  auto old = s.state.load();
  if (old == audio::State::disconnected) return resource_lost;
  if (old == audio::State::closed || old == audio::State::closing)
    return precondition;
  std::int32_t result = 0;
  if (o->kind == Kind::player) {
    if (old == audio::State::started || old == audio::State::starting) result = s.pause();
    if (result)
      return result == audio::disconnected ? resource_lost : precondition;
    auto before = s.frames_read.load();
    result = s.flush();
    o->position_origin += s.frames_read.load() - before;
  } else
    result = s.stop();
  if (result)
    return result == audio::disconnected ? resource_lost : precondition;
  o->queue.clear();
  o->queue_index = 0;
  if ((old == audio::State::started || old == audio::State::starting) && s.start()) return resource_error;
  return ok;
}
Result queue_state(GuestAddr self, abi::QueueState* state) {
  auto o = find(self);
  if (!o || !state || !o->stream) return invalid;
  std::lock_guard lock(o->lock);
  *state = {std::uint32_t(o->queue.size()), o->queue_index};
  return ok;
}
Result register_queue_callback(GuestAddr self, GuestAddr cb, GuestAddr user) {
  auto o = find(self);
  if (!o || !o->stream) return invalid;
  {
    std::lock_guard lock(o->lock);
    if (o->play_state == 3 || !o->realized) return precondition;
  }
  // Registration happens before PLAYING. Prepare without holding Object::lock;
  // the worker may inspect the queue while waiting for the preparation request.
  if (cb && o->stream->prepare_guest_worker() != 0) return resource_error;
  std::lock_guard lock(o->lock);
  if (o->play_state == 3 || !o->realized) return precondition;
  o->queue_cb = cb;
  o->queue_user = user;
  o->queue_itf = self;
  return ok;
}
Result set_volume(GuestAddr self, std::int16_t value) {
  auto o = find(self);
  if (!o || !o->stream || value > 0) return invalid;
  std::lock_guard lock(o->lock);
  o->volume = value;
  update_gain(*o);
  return ok;
}
Result get_volume(GuestAddr self, std::int16_t* value) {
  auto o = find(self);
  if (!o || !value) return invalid;
  std::lock_guard lock(o->lock);
  *value = o->volume;
  return ok;
}
Result max_volume(GuestAddr self, std::int16_t* value) {
  if (!find(self) || !value) return invalid;
  *value = 0;
  return ok;
}
Result set_mute(GuestAddr self, std::uint32_t value) {
  auto o = find(self);
  if (!o || !o->stream || value > 1) return invalid;
  std::lock_guard lock(o->lock);
  o->mute = value;
  update_gain(*o);
  return ok;
}
Result get_mute(GuestAddr self, std::uint32_t* value) {
  auto o = find(self);
  if (!o || !value) return invalid;
  std::lock_guard lock(o->lock);
  *value = o->mute;
  return ok;
}
Result enable_pan(GuestAddr self, std::uint32_t value) {
  auto o = find(self);
  if (!o || !o->stream || value > 1) return invalid;
  std::lock_guard lock(o->lock);
  o->pan_enabled = value;
  update_gain(*o);
  return ok;
}
Result get_pan_enabled(GuestAddr self, std::uint32_t* value) {
  auto o = find(self);
  if (!o || !value) return invalid;
  std::lock_guard lock(o->lock);
  *value = o->pan_enabled;
  return ok;
}
Result set_pan(GuestAddr self, std::int16_t value) {
  auto o = find(self);
  if (!o || !o->stream || value < -1000 || value > 1000) return invalid;
  std::lock_guard lock(o->lock);
  o->pan = value;
  update_gain(*o);
  return ok;
}
Result get_pan(GuestAddr self, std::int16_t* value) {
  auto o = find(self);
  if (!o || !value) return invalid;
  std::lock_guard lock(o->lock);
  *value = o->pan;
  return ok;
}
std::uint32_t* config_value(Object& o, std::string_view key) {
  if (key == "androidPlaybackStreamType" && o.kind == Kind::player)
    return &o.stream_type;
  if (key == "androidPerformanceMode") return &o.performance;
  if (key == "androidRecordingPreset" && o.kind == Kind::recorder)
    return &o.recording_preset;
  return nullptr;
}
Result set_config(GuestAddr self, const char* key, const void* data,
                  std::uint32_t size) {
  auto o = find(self);
  if (!o || !key || !data || size != 4) return invalid;
  std::lock_guard lock(o->lock);
  if (o->realized != 1) return precondition;
  auto target = config_value(*o, key);
  if (!target) {
    log::warn("OpenSL", "Unknown Android audio configuration key");
    return unsupported;
  }
  std::uint32_t value;
  std::memcpy(&value, data, 4);
  if ((target == &o->performance && value > 3) ||
      (target == &o->stream_type && value > 5))
    return invalid;
  // Host capture supplies unprocessed PCM. Voice processing presets require
  // a real DSP implementation; do not silently promise echo cancellation.
  if (target == &o->recording_preset && value != 1 && value != 5)
    return unsupported;
  *target = value;
  return ok;
}
Result get_config(GuestAddr self, const char* key, std::uint32_t* size,
                  void* data) {
  auto o = find(self);
  if (!o || !key || !size) return invalid;
  std::lock_guard lock(o->lock);
  auto target = config_value(*o, key);
  if (!target) return unsupported;
  auto available = *size;
  *size = 4;
  if (!data) return ok;
  if (available < 4) return insufficient;
  std::memcpy(data, target, 4);
  return ok;
}
std::vector<const IdDefinition*> supported_ids(std::uint32_t object_id) {
  Kind kind;
  switch (object_id) {
    case 0x1001:
      kind = Kind::engine;
      break;
    case 0x1004:
      kind = Kind::player;
      break;
    case 0x1005:
      kind = Kind::recorder;
      break;
    case 0x1009:
      kind = Kind::mix;
      break;
    default:
      return {};
  }
  std::vector<const IdDefinition*> list;
  for (auto& entry : ids)
    if (supported(kind, entry.name)) list.push_back(&entry);
  return list;
}
Result query_num(GuestAddr self, std::uint32_t object_id,
                 std::uint32_t* count) {
  if (!find(self) || !count) return invalid;
  auto list = supported_ids(object_id);
  if (list.empty()) return unsupported;
  *count = std::uint32_t(list.size());
  return ok;
}
Result query_id(GuestAddr self, std::uint32_t object_id, std::uint32_t index,
                GuestAddr* result) {
  if (!find(self) || !result) return invalid;
  auto list = supported_ids(object_id);
  if (index >= list.size()) return invalid;
  *result = id_addresses.at(list[index]->name);
  return ok;
}
Result extension_num(GuestAddr self, std::uint32_t* count) {
  if (!find(self) || !count) return invalid;
  *count = 0;
  return ok;
}
Result extension_supported(GuestAddr self, const char* name,
                           std::uint32_t* supported) {
  if (!find(self) || !name || !supported) return invalid;
  *supported = 0;
  return ok;
}
template <class T>
void install_table(Itf kind, T table) {
  auto addr = AddressSpace::get().allocate(
      sizeof(table), kProtRead | kProtWrite, "[opensles-vtable]");
  if (!addr) throw std::bad_alloc();
  if (abi32) {
    // NDK SLES/OpenSLES*.h: each interface is a pointer to a vtable whose
    // slots are function pointers, hence four-byte strides under ARM ILP32.
    std::array<GuestAddr, sizeof(T) / sizeof(GuestAddr)> entries;
    std::memcpy(entries.data(), &table, sizeof(table));
    for (std::size_t i = 0; i < entries.size(); ++i)
      g2h<std::uint32_t>(addr)[i] = h2g32(g2h(entries[i]));
  } else
    std::memcpy(g2h(addr), &table, sizeof(table));
  AddressSpace::get().protect(addr, kGuestPageSize, kProtRead);
  tables[kind] = addr;
}

template <class T> T read32(GuestAddr p) {
  T v;
  a32::read_bytes(p, &v, sizeof(v));
  return v;
}
struct Ids32 {
  std::array<GuestAddr, 64> values{};
  Ids32(std::uint32_t count, GuestAddr pointer) {
    if (count > values.size() || (count && !pointer))
      return;
    for (std::uint32_t i = 0; i < count; ++i)
      values[i] = read32<std::uint32_t>(pointer + 4ull * i);
  }
};
// Android NDK OpenSLES.h: DataSource/Sink are two pointers, OutputMix is
// {uint32, pointer}, IODevice is {uint32, uint32, uint32, pointer}.
struct Data32 {
  abi::Data value{};
  abi::OutputMixLocator mix{};
  abi::IoLocator io{};
  abi::UriLocator uri{};
  explicit Data32(GuestAddr pointer) {
    if (!pointer)
      return;
    auto words = read32<std::array<std::uint32_t, 2>>(pointer);
    value = {words[0], words[1]};
    if (!value.locator)
      return;
    auto type = read32<std::uint32_t>(value.locator);
    if(type==1) {
      auto w=read32<std::array<std::uint32_t,2>>(value.locator);
      uri={w[0],0,w[1]};value.locator=h2g(&uri);
    } else if (type == 4) {
      auto w = read32<std::array<std::uint32_t, 2>>(value.locator);
      mix = {w[0], 0, w[1]};
      value.locator = h2g(&mix);
    } else if (type == 3) {
      auto w = read32<std::array<std::uint32_t, 4>>(value.locator);
      io = {w[0], w[1], w[2], 0, w[3]};
      value.locator = h2g(&io);
    }
  }
};
void output32(GuestAddr address, GuestAddr value) {
  const auto word = h2g32(g2h(value));
  a32::write_bytes(address, &word, 4);
}
template <auto A, auto B>
constexpr bool same_function = [] {
  if constexpr (std::is_same_v<decltype(A), decltype(B)>)
    return A == B;
  else
    return false;
}();
template <auto F> void invoke32(CallContext &ctx) {
  try {
    a32::ArgCursor32 c(ctx.cpu());
    if constexpr (same_function<F, &create_engine>) {
      auto out = c.word();
      auto option_count = c.word();
      auto options = c.word();
      auto count = c.word();
      auto ids = c.word();
      auto required = c.word();
      if (!out) {
        ctx.return_x(invalid);
        return;
      }
      a32::check_out(out);
      Ids32 requested(count, ids);
      GuestAddr value = 0;
      auto r =
          create_engine(&value, option_count, g2h<abi::EngineOption>(options),
                        count, ids ? requested.values.data() : nullptr,
                        g2h<std::uint32_t>(required));
      output32(out, value);
      ctx.return_x(r);
    } else if constexpr (same_function<F, &create_mix>) {
      auto self = c.word();
      auto out = c.word();
      auto count = c.word();
      auto ids = c.word();
      auto required = c.word();
      if (!out) {
        ctx.return_x(invalid);
        return;
      }
      a32::check_out(out);
      Ids32 requested(count, ids);
      GuestAddr value = 0;
      auto r = create_mix(self, &value, count,
                          ids ? requested.values.data() : nullptr,
                          g2h<std::uint32_t>(required));
      output32(out, value);
      ctx.return_x(r);
    } else if constexpr (same_function<F, &create_player> ||
                         same_function<F, &create_recorder>) {
      auto self = c.word();
      auto out = c.word();
      auto source = c.word();
      auto sink = c.word();
      auto count = c.word();
      auto ids = c.word();
      auto required = c.word();
      if (!out) {
        ctx.return_x(invalid);
        return;
      }
      a32::check_out(out);
      Ids32 requested(count, ids);
      Data32 src(source), dst(sink);
      GuestAddr value = 0;
      auto r = F(self, &value, source ? &src.value : nullptr,
                 sink ? &dst.value : nullptr, count,
                 ids ? requested.values.data() : nullptr,
                 g2h<std::uint32_t>(required));
      output32(out, value);
      ctx.return_x(r);
    } else if constexpr (same_function<F, &get_interface> ||
                         same_function<F, &query_id>) {
      auto self = c.word();
      auto a = c.word();
      auto b = c.word();
      auto out = same_function<F, &get_interface> ? b : c.word();
      if (!out) {
        ctx.return_x(invalid);
        return;
      }
      a32::check_out(out);
      GuestAddr value = 0;
      Result r;
      if constexpr (same_function<F, &get_interface>)
        r = get_interface(self, g2h<abi::InterfaceId>(a), &value);
      else
        r = query_id(self, a, b, &value);
      output32(out, value);
      ctx.return_x(r);
    } else {
      // Remaining registered methods contain only scalar words, low interface
      // addresses/callbacks, UUIDs, QueueState (two words) or scalar buffers.
      a32::invoke(F, ctx);
    }
  } catch (const a32::AbiError &e) {
    a32::fail(ctx, e.error);
  } catch (const std::bad_alloc &) {
    ctx.return_x(memory_failure);
  } catch (const std::system_error &e) {
    log::warn("OpenSL", "{} failed: {}", ctx.info().name, e.what());
    ctx.return_x(resource_error);
  }
}
template <auto F> GuestAddr method(std::string_view name) {
  if (abi32)
    a32::register_adapter("libOpenSLES.so", name, invoke32<F>);
  return make_thunk("libOpenSLES.so", name, audio::wrap_abi<F>());
}
} // namespace
void register_library() {
  static std::once_flag once;
  std::call_once(once, [] {
    abi32 = process_config().isa() == GuestIsa::a32;
    auto lib = std::make_shared<HleLibrary>("libOpenSLES.so");
    auto thunk = [](std::string_view name, Handler fn) {
      return make_thunk("libOpenSLES.so", name, fn);
    };
    auto fail = [&](std::string_view name) {
      if (abi32)
        a32::register_adapter("libOpenSLES.so", name, fail_method);
      return thunk(name, fail_method);
    };
#define FN(name, function) method<&function>(name)
    install_table(
        Itf::object,
        abi::ObjectTable{FN("Object.Realize", realize), fail("Object.Resume"),
                         FN("Object.GetState", object_state),
                         FN("Object.GetInterface", get_interface),
                         FN("Object.RegisterCallback", register_object_callback),
                         FN("Object.AbortAsyncOperation", abort_async),
                         FN("Object.Destroy", destroy),
                         FN("Object.SetPriority", set_priority),
                         FN("Object.GetPriority", get_priority),
                         fail("Object.SetLossOfControlInterfaces")});
    install_table(
        Itf::engine,
        abi::EngineTable{
            fail("Engine.CreateLEDDevice"), fail("Engine.CreateVibraDevice"),
            FN("Engine.CreateAudioPlayer", create_player),
            FN("Engine.CreateAudioRecorder", create_recorder),
            fail("Engine.CreateMidiPlayer"), fail("Engine.CreateListener"),
            fail("Engine.Create3DGroup"),
            FN("Engine.CreateOutputMix", create_mix),
            fail("Engine.CreateMetadataExtractor"),
            fail("Engine.CreateExtensionObject"),
            FN("Engine.QueryNumSupportedInterfaces", query_num),
            FN("Engine.QuerySupportedInterfaces", query_id),
            FN("Engine.QueryNumSupportedExtensions", extension_num),
            fail("Engine.QuerySupportedExtension"),
            FN("Engine.IsExtensionSupported", extension_supported)});
    install_table(
        Itf::play,
        abi::PlayTable{
            FN("Play.SetPlayState", set_play),
            FN("Play.GetPlayState", get_play), FN("Play.GetDuration", duration),
            FN("Play.GetPosition", position), fail("Play.RegisterCallback"),
            fail("Play.SetCallbackEventsMask"),
            fail("Play.GetCallbackEventsMask"), fail("Play.SetMarkerPosition"),
            fail("Play.ClearMarkerPosition"), fail("Play.GetMarkerPosition"),
            fail("Play.SetPositionUpdatePeriod"),
            fail("Play.GetPositionUpdatePeriod")});
    install_table(Itf::seek,abi::SeekTable{FN("Seek.SetPosition",seek_position),
      FN("Seek.SetLoop",seek_loop),FN("Seek.GetLoop",get_loop)});
    install_table(Itf::record,
                  abi::RecordTable{FN("Record.SetRecordState", set_play),
                                   FN("Record.GetRecordState", get_play),
                                   fail("Record.SetDurationLimit"),
                                   FN("Record.GetPosition", position),
                                   fail("Record.RegisterCallback"),
                                   fail("Record.SetCallbackEventsMask"),
                                   fail("Record.GetCallbackEventsMask"),
                                   fail("Record.SetMarkerPosition"),
                                   fail("Record.ClearMarkerPosition"),
                                   fail("Record.GetMarkerPosition"),
                                   fail("Record.SetPositionUpdatePeriod"),
                                   fail("Record.GetPositionUpdatePeriod")});
    abi::BufferQueueTable queue{
        FN("BufferQueue.Enqueue", enqueue),
        FN("BufferQueue.Clear", clear_queue),
        FN("BufferQueue.GetState", queue_state),
        FN("BufferQueue.RegisterCallback", register_queue_callback)};
    install_table(Itf::queue, queue);
    install_table(Itf::android_queue, queue);
    install_table(
        Itf::volume,
        abi::VolumeTable{FN("Volume.SetVolumeLevel", set_volume),
                         FN("Volume.GetVolumeLevel", get_volume),
                         FN("Volume.GetMaxVolumeLevel", max_volume),
                         FN("Volume.SetMute", set_mute),
                         FN("Volume.GetMute", get_mute),
                         FN("Volume.EnableStereoPosition", enable_pan),
                         FN("Volume.IsEnabledStereoPosition", get_pan_enabled),
                         FN("Volume.SetStereoPosition", set_pan),
                         FN("Volume.GetStereoPosition", get_pan)});
    install_table(Itf::config,
                  abi::AndroidConfigurationTable{
                      FN("AndroidConfiguration.SetConfiguration", set_config),
                      FN("AndroidConfiguration.GetConfiguration", get_config),
                      fail("AndroidConfiguration.AcquireJavaProxy"),
                      fail("AndroidConfiguration.ReleaseJavaProxy")});
    lib->add_function("slCreateEngine", FN("slCreateEngine", create_engine));
#undef FN
    // r6-ndk/symbols.csv measured these data imports in UE4, Wwise and Vivox.
    // UUID existence is not an advertisement of support (same as AOSP's IID
    // array); optional effects fail GetInterface and required effects fail
    // object creation. Each exported symbol is a pointer cell followed by UUID.
    for (auto& entry : ids) {
      auto addr = AddressSpace::get().allocate(24, kProtRead | kProtWrite,
                                               "[opensles-iid]");
      if (!addr)
        throw std::bad_alloc();
      auto width = abi32 ? 4u : 8u;
      if (abi32)
        *g2h<std::uint32_t>(addr) = h2g32(g2h(addr + width));
      else
        *g2h<GuestAddr>(addr) = addr + width;
      std::memcpy(g2h(addr + width), &entry.value, 16);
      AddressSpace::get().protect(addr, kGuestPageSize, kProtRead);
      id_addresses[entry.name] = addr + width;
      lib->add_data(entry.name, addr);
    }
    Linker::get().register_hle_library(std::move(lib));
  });
}
void shutdown_library() {
  std::unordered_map<GuestAddr, Interface> active;
  {
    std::lock_guard lock(registry_lock);
    active.swap(registry);
  }
  for (auto& [addr, entry] : active)
    if (entry.kind == Itf::object && entry.object->stream)
      entry.object->stream->close();
}
}  // namespace venture::hle::opensles
