#include "hle/aaudio/aaudio.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "audio/audio.h"
#include "audio/guest_abi.h"
#include "av_telemetry.h"
#include "common/log.h"
#include "core/address_space.h"
#include "core/hle.h"
#include "core/linker.h"
#include "core/process.h"
#include "core/profile.h"
#include "core/thread.h"
#include "platform/thread_priority.h"

namespace venture::hle::aaudio {
namespace {
using namespace audio;
struct Builder {
  std::mutex lock;
  Config config;
  std::int32_t direction = 0, format = 0, rate = 0, channels = 0, capacity = 0,
               callback_frames = 0;
  std::int32_t sharing = 1, performance = 10, usage = 1, content = 2,
               device = 0;
  std::int32_t input_preset = 6, session = -1, channel_mask = 0;
  GuestAddr data_callback = 0, data_user = 0, error_callback = 0,
            error_user = 0;
};
struct Handle {
  std::shared_ptr<Stream> stream;
  std::atomic<bool> closing{false};
  std::int32_t usage = 1, content = 2, performance = 10, callback_frames = 0,
               input_preset = 6, channel_mask = 0;
  std::int32_t session = -1;
  bool callback = false;
};
std::mutex registry_lock;
std::atomic<std::int32_t> next_session{1};
// Process override from AAudioTesting.h (also public in API 36). Zero leaves
// selection to the platform; this backend has no MMAP service.
std::atomic<std::int32_t> mmap_policy{0};
std::int32_t set_mmap_policy(std::int32_t policy) {
  if (policy < 0 || policy > 3) return illegal_argument;
  mmap_policy.store(policy);
  return 0;
}
std::int32_t get_mmap_policy() { return mmap_policy.load(); }
std::unordered_map<GuestAddr, std::shared_ptr<Builder>> builders;
std::unordered_map<GuestAddr, std::shared_ptr<Handle>> streams;
template <class T>
auto find(std::unordered_map<GuestAddr, std::shared_ptr<T>>& registry,
          GuestAddr id) {
  std::lock_guard lock(registry_lock);
  auto it = registry.find(id);
  return it == registry.end() ? std::shared_ptr<T>{} : it->second;
}
GuestAddr token(std::string_view name) {
  return AddressSpace::get().allocate(8, kProtRead | kProtWrite, name);
}
std::int32_t create(GuestAddr* result) {
  if (!result) return null_pointer;
  *result = 0;
  try {
    auto b = std::make_shared<Builder>();
    auto addr = token("[aaudio-builder]");
    if (!addr) return no_memory;
    std::lock_guard lock(registry_lock);
    builders.emplace(addr, std::move(b));
    *result = addr;
    return 0;
  } catch (const std::bad_alloc&) {
    return no_memory;
  }
}
std::int32_t delete_builder(GuestAddr id) {
  {
    std::lock_guard lock(registry_lock);
    if (!builders.erase(id)) return invalid_handle;
  }
  AddressSpace::get().free(id);
  return 0;
}
template <auto Member>
void set(GuestAddr id, std::int32_t value) {
  if (auto b = find(builders, id)) {
    std::lock_guard lock(b->lock);
    b.get()->*Member = value;
  } else
    log::warn("AAudio", "Setter used invalid builder handle");
}
// AOSP: a channel count clears the mask; a positional mask fixes the count.
void set_channel_count(GuestAddr id, std::int32_t count) {
  if (auto b = find(builders, id)) {
    std::lock_guard lock(b->lock);
    b->channels = count;
    b->channel_mask = 0;
  }
}
void set_channel_mask(GuestAddr id, std::int32_t mask) {
  if (auto b = find(builders, id)) {
    std::lock_guard lock(b->lock);
    b->channel_mask = mask;
    b->channels = mask > 0 ? std::popcount(std::uint32_t(mask)) : mask ? -1 : 0;
  }
}
// Attribution only feeds AppOps accounting on Android; nothing to route here.
void set_attribution(GuestAddr, GuestAddr) {}
void data_callback(GuestAddr id, GuestAddr fn, GuestAddr user) {
  if (auto b = find(builders, id)) {
    std::lock_guard lock(b->lock);
    b->data_callback = fn;
    b->data_user = user;
  }
}
void error_callback(GuestAddr id, GuestAddr fn, GuestAddr user) {
  if (auto b = find(builders, id)) {
    std::lock_guard lock(b->lock);
    b->error_callback = fn;
    b->error_user = user;
  }
}
struct CallbackBuffer {
  GuestAddr address = 0;
  ~CallbackBuffer() {
    if (address) AddressSpace::get().free(address);
  }
};
std::int32_t open(GuestAddr id, GuestAddr* result) {
  if (!result) return null_pointer;
  *result = 0;
  auto b = find(builders, id);
  if (!b) return invalid_handle;
  std::lock_guard builder_lock(b->lock);
  if (b->direction < 0 || b->direction > 1 || b->sharing < 0 ||
      b->sharing > 1 || b->performance < 10 || b->performance > 12 ||
      b->capacity < 0 || b->callback_frames < 0 || b->usage < 1 ||
      b->usage > 16 || b->content < 0 || b->content > 4)
    return illegal_argument;
  const auto default_id = b->direction == 0 ? default_output_id : default_input_id;
  if (b->session < -1) return illegal_argument;
  if (b->device != 0 && b->device != default_id) {
    log::warn("AAudio", "Unknown audio device {} (default {})", b->device, default_id);
    return unimplemented;
  }
  if (b->rate && (b->rate < 8000 || b->rate > 192000)) return invalid_rate;
  if (b->channels < 0 || b->channels > 2) return illegal_argument;
  if (b->callback_frames > 65536 || b->capacity > 65536) return out_of_range;
  // AAudio accepts EXCLUSIVE as a preference; the negotiated stream is
  // honestly reported as SHARED because all guests feed the shared mixer.
  Config c;
  c.api = "AAudio";
  c.prefill_output = b->data_callback != 0 && b->direction == 0;
  c.direction = Direction(b->direction);
  c.format = b->format ? Format(b->format) : Format::i16;
  if (c.format == Format::u8) return invalid_format;  // u8 is OpenSL only
  if (!sample_bytes(c.format)) return invalid_format;
  // ALWAYS forbids legacy fallback. Match AOSP's no-MMAP-service result;
  // NEVER, AUTO and UNSPECIFIED may use the existing legacy mixer.
  if (mmap_policy.load() == 3) return -881;  // AAUDIO_ERROR_NO_SERVICE
  c.rate = b->rate ? b->rate : 48000;
  c.channels = b->channels ? b->channels : (b->direction ? 1 : 2);
  c.burst_frames = Backend::get().frames_per_burst(c.direction, c.rate);
  if (c.burst_frames < 1) return unavailable;
  c.callback_frames =
      b->callback_frames ? b->callback_frames : c.burst_frames;
  c.capacity =
      b->capacity ? b->capacity : std::min(65536, std::max(c.callback_frames * 2, c.burst_frames * 4));
  c.capacity = std::max({c.capacity, c.callback_frames, c.burst_frames});
  std::int32_t status;
  auto stream = Stream::create(c, &status);
  if (!stream) return status;
  auto h = std::make_shared<Handle>();
  h->stream = stream;
  h->usage = b->usage;
  h->content = b->content;
  h->callback = b->data_callback != 0;
  h->performance = b->performance == 12 ? 12 : 10;
  h->callback_frames = b->callback_frames;
  h->input_preset = b->input_preset;
  h->channel_mask = b->channel_mask;
  // NDK aaudio/AAudio.h: NONE=-1; ALLOCATE=0 requests one stable positive
  // identity, which may be reused by another stream. Effects APIs remain
  // unregistered; creating a session alone does not request an effect.
  h->session = b->session;
  if (h->session == 0) {
    auto candidate = next_session.load();
    do {
      if (candidate == INT32_MAX) return no_memory;
    } while (!next_session.compare_exchange_weak(candidate, candidate + 1));
    h->session = candidate;
  }
  auto addr = token("[aaudio-stream]");
  if (!addr) return no_memory;
  auto storage = std::make_shared<CallbackBuffer>();
  if (b->data_callback) {
    storage->address = AddressSpace::get().allocate(
        std::uint64_t(c.callback_frames) * c.channels * sample_bytes(c.format),
        kProtRead | kProtWrite, "[aaudio-callback]");
    if (!storage->address) {
      AddressSpace::get().free(addr);
      return no_memory;
    }
  }
  {
    std::lock_guard lock(registry_lock);
    streams.emplace(addr, h);
  }
  auto cb = b->data_callback, user = b->data_user, err = b->error_callback,
       err_user = b->error_user;
  try {
    stream->launch_worker([addr, cb, user, err, err_user, storage,
                           pending = std::uint32_t(0),
                           generation = std::uint64_t(0),
                           error_sent = false](Stream& s) mutable {
      if (s.state == State::disconnected && !error_sent) {
        error_sent = true;
        if (err)
          GuestThread::adopt_current().call(
              err, {addr, err_user, std::uint64_t(std::int64_t(disconnected))});
      }
      if (!cb || (s.state != State::started && s.state != State::starting)) return;
      if (generation != s.buffer_generation.load()) {
        generation = s.buffer_generation.load();
        pending = 0;
      }
      auto frames = std::uint32_t(s.config.callback_frames);
      bool output = s.config.direction == Direction::output;
      // ~5 ms host periods plus 1-2 guest periods buffer JIT scheduling jitter.
      // Callback PCM is guest-visible memory and remains alive until close
      // joins.
      if (output) {
        if (pending) {
          auto sent = s.write(g2h<std::uint8_t>(storage->address) +
                                  (frames - pending) * s.config.channels *
                                      sample_bytes(s.config.format),
                              pending);
          pending -= sent;
          if (pending) return;
        }
        const auto minimum = std::min(frames, std::uint32_t(s.config.burst_frames));
        if (s.ring.readable() + minimum > std::uint32_t(s.buffer_size.load()))
          return;
        std::memset(g2h(storage->address), 0,
                    std::size_t(frames) * s.config.channels *
                        sample_bytes(s.config.format));
      } else {
        if (s.ring.readable() < frames) return;
        if (s.read(g2h(storage->address), frames) != frames) return;
      }
      const bool measure = av::Telemetry::get().enabled();
      const auto adopt_begin = measure ? av::now_ns() : 0;
      if (measure) s.callback_adopt_begin_ns.store(adopt_begin, std::memory_order_release);
      auto& thread = GuestThread::adopt_current();
      if (measure) s.callback_guest_tid.store(thread.tid(), std::memory_order_relaxed);
      const auto callback_begin = measure ? av::now_ns() : 0;
      if (measure) s.callback_begin_ns.store(callback_begin, std::memory_order_release);
      const auto cpu_begin = measure ? platform::thread_cpu_time_ns() : -1;
      const auto cycles_begin = measure ? platform::thread_cpu_cycles() : -1;
      const auto blocks_begin = measure ? profile::thread_emitted_blocks() : -1;
      auto value = thread.call(cb, {addr, user, storage->address, frames});
      if (measure) {
        s.callback_end_ns.store(av::now_ns(), std::memory_order_release);
        const auto cpu_end = platform::thread_cpu_time_ns();
        const auto cycles_end = platform::thread_cpu_cycles();
        const auto blocks_end = profile::thread_emitted_blocks();
        s.record_callback(callback_begin - adopt_begin,
                          av::now_ns() - callback_begin,
                          cpu_begin >= 0 && cpu_end >= cpu_begin ? cpu_end - cpu_begin : -1,
                          cycles_begin >= 0 && cycles_end >= cycles_begin ? cycles_end - cycles_begin : -1,
                          callback_begin,
                          blocks_begin >= 0 && blocks_end >= blocks_begin ? blocks_end - blocks_begin : -1);
      }
      if (std::int32_t(value) != 0) {
        s.request_stop();
        return;
      }  // AAUDIO_CALLBACK_RESULT_STOP
      if (output && (s.state == State::started || s.state == State::starting))
        pending = frames - s.write(g2h(storage->address), frames);
    });
    if ((cb || err) && stream->prepare_guest_worker() != 0)
      throw std::runtime_error("Cannot prepare the guest audio callback worker");
  } catch (const std::exception& e) {
    {
      std::lock_guard lock(registry_lock);
      streams.erase(addr);
    }
    stream->close();
    AddressSpace::get().free(addr);
    log::error("AAudio", "Cannot create callback worker: {}", e.what());
    return unavailable;
  }
  *result = addr;
  return 0;
}
std::int32_t close(CallContext& ctx, GuestAddr id) {
  auto h = find(streams, id);
  if (!h) return invalid_handle;
  // AAudio.h forbids close from its callbacks. It must not self-join or
  // destroy the callback buffer while the guest is still using it.
  if (h->stream->on_worker()) return invalid_state;
  if (h->closing.exchange(true)) return invalid_state;
  // A callback may still query the stream while close is joining it. Retain
  // both the public token and its lookup until that callback and TLS cleanup end.
  h->stream->close(&ctx.thread());
  {
    std::lock_guard lock(registry_lock);
    streams.erase(id);
  }
  AddressSpace::get().free(id);
  return 0;
}
template <auto Fn>
std::int32_t control(GuestAddr id) {
  auto h = find(streams, id);
  return h ? (h->stream.get()->*Fn)() : invalid_handle;
}
std::int32_t read(CallContext& ctx, GuestAddr id, void* data,
                  std::int32_t frames, std::int64_t duration) {
  auto h = find(streams, id);
  if (!h) return invalid_handle;
  if (h->callback) return invalid_state;
  return h->stream->transfer(&ctx.thread(), data, frames, duration, false);
}
std::int32_t write(CallContext& ctx, GuestAddr id, void* data,
                   std::int32_t frames, std::int64_t duration) {
  auto h = find(streams, id);
  if (!h) return invalid_handle;
  if (h->callback) return invalid_state;
  return h->stream->transfer(&ctx.thread(), data, frames, duration, true);
}
std::int32_t wait(CallContext& ctx, GuestAddr id, std::int32_t input,
                  State* next, std::int64_t duration) {
  auto h = find(streams, id);
  return h ? h->stream->wait_state(ctx.thread(), State(input), next, duration)
           : invalid_handle;
}
std::int32_t timestamp(GuestAddr id, std::int32_t clock, std::int64_t* position,
                       std::int64_t* time) {
  auto h = find(streams, id);
  if (!h) return invalid_handle;
  // Core's timestamp is CLOCK_MONOTONIC. Linux CLOCK_BOOTTIME includes
  // suspend; do not claim it unless core supplies that clock as well.
  if (clock != 1) return illegal_argument;
  return h->stream->timestamp(position, time);
}
std::int32_t buffer_size(GuestAddr id, std::int32_t frames) {
  auto h = find(streams, id);
  return h ? h->stream->set_buffer_size(frames) : invalid_handle;
}
enum class Getter {
  state,
  rate,
  channels,
  format,
  direction,
  sharing,
  performance,
  usage,
  content,
  capacity,
  size,
  burst,
  callback_frames,
  xruns,
  written,
  read,
  device,
  preset,
  session,
  hardware_rate,
  hardware_channels,
  hardware_format,
  channel_mask,
  mmap
};
template <Getter G>
void getter(CallContext& ctx) {
  auto h = find(streams, ctx.x(0));
  if (!h) {
    ctx.return_x(std::uint64_t(std::int64_t(invalid_handle)));
    return;
  }
  auto& s = *h->stream;
  std::int64_t value = 0;
  if constexpr (G == Getter::state) value = std::int32_t(s.state.load());
  if constexpr (G == Getter::rate) value = s.config.rate;
  if constexpr (G == Getter::channels) value = s.config.channels;
  if constexpr (G == Getter::format) value = std::int32_t(s.config.format);
  if constexpr (G == Getter::direction)
    value = std::int32_t(s.config.direction);
  if constexpr (G == Getter::sharing) value = 1;
  if constexpr (G == Getter::performance) value = h->performance;
  if constexpr (G == Getter::usage) value = h->usage;
  if constexpr (G == Getter::content) value = h->content;
  if constexpr (G == Getter::capacity) value = s.config.capacity;
  if constexpr (G == Getter::size) value = s.buffer_size.load();
  if constexpr (G == Getter::burst) value = s.config.burst_frames;
  if constexpr (G == Getter::callback_frames) value = h->callback_frames;
  if constexpr (G == Getter::xruns) value = s.xruns.load();
  if constexpr (G == Getter::written) value = s.frames_written.load();
  if constexpr (G == Getter::read) value = s.frames_read.load();
  if constexpr (G == Getter::device)
    value = s.config.direction == Direction::output ? default_output_id : default_input_id;
  if constexpr (G == Getter::preset) value = h->input_preset;
  if constexpr (G == Getter::session) value = h->session;
  // Positional MONO/STEREO, as AOSP reports for streams opened by count.
  if constexpr (G == Getter::channel_mask)
    value = h->channel_mask ? h->channel_mask : s.config.channels == 1 ? 1 : 3;
  if constexpr (G == Getter::hardware_rate)
    value = Backend::get().hardware(s.config.direction).rate;
  if constexpr (G == Getter::hardware_channels)
    value = Backend::get().hardware(s.config.direction).channels;
  if constexpr (G == Getter::hardware_format)
    value = Backend::get().hardware(s.config.direction).format;
  ctx.return_x(std::uint64_t(value));
}
const char* result_text(std::int32_t code) {
  switch (code) {
    case 0:
      return "AAUDIO_OK";
    case disconnected:
      return "AAUDIO_ERROR_DISCONNECTED";
    case illegal_argument:
      return "AAUDIO_ERROR_ILLEGAL_ARGUMENT";
    case internal_error:
      return "AAUDIO_ERROR_INTERNAL";
    case invalid_state:
      return "AAUDIO_ERROR_INVALID_STATE";
    case invalid_handle:
      return "AAUDIO_ERROR_INVALID_HANDLE";
    case unimplemented:
      return "AAUDIO_ERROR_UNIMPLEMENTED";
    case unavailable:
      return "AAUDIO_ERROR_UNAVAILABLE";
    case -888:
      return "AAUDIO_ERROR_NO_FREE_HANDLES";
    case no_memory:
      return "AAUDIO_ERROR_NO_MEMORY";
    case null_pointer:
      return "AAUDIO_ERROR_NULL";
    case timeout:
      return "AAUDIO_ERROR_TIMEOUT";
    case -884:
      return "AAUDIO_ERROR_WOULD_BLOCK";
    case invalid_format:
      return "AAUDIO_ERROR_INVALID_FORMAT";
    case out_of_range:
      return "AAUDIO_ERROR_OUT_OF_RANGE";
    case -881:
      return "AAUDIO_ERROR_NO_SERVICE";
    case invalid_rate:
      return "AAUDIO_ERROR_INVALID_RATE";
    default:
      return "AAUDIO_ERROR_UNKNOWN";
  }
}
std::unordered_map<std::string, GuestAddr> text_addresses;
GuestAddr guest_text(const char* text) {
  std::lock_guard lock(registry_lock);
  auto it = text_addresses.find(text);
  if (it != text_addresses.end()) return it->second;
  auto addr = AddressSpace::get().allocate(
      std::strlen(text) + 1, kProtRead | kProtWrite, "[aaudio-text]");
  if (addr) {
    std::memcpy(g2h(addr), text, std::strlen(text) + 1);
    AddressSpace::get().protect(addr, kGuestPageSize, kProtRead);
    text_addresses.emplace(text, addr);
  }
  return addr;
}
GuestAddr convert_result(std::int32_t code) {
  return guest_text(result_text(code));
}
GuestAddr convert_state(std::int32_t code) {
  constexpr const char* text[] = {
      "AAUDIO_STREAM_STATE_UNINITIALIZED", "AAUDIO_STREAM_STATE_UNKNOWN",
      "AAUDIO_STREAM_STATE_OPEN",          "AAUDIO_STREAM_STATE_STARTING",
      "AAUDIO_STREAM_STATE_STARTED",       "AAUDIO_STREAM_STATE_PAUSING",
      "AAUDIO_STREAM_STATE_PAUSED",        "AAUDIO_STREAM_STATE_FLUSHING",
      "AAUDIO_STREAM_STATE_FLUSHED",       "AAUDIO_STREAM_STATE_STOPPING",
      "AAUDIO_STREAM_STATE_STOPPED",       "AAUDIO_STREAM_STATE_CLOSING",
      "AAUDIO_STREAM_STATE_CLOSED",        "AAUDIO_STREAM_STATE_DISCONNECTED"};
  return guest_text(code >= 0 && code < 14 ? text[code]
                                           : "AAUDIO_STREAM_STATE_UNKNOWN");
}
}  // namespace
void register_library() {
  static std::once_flag once;
  std::call_once(once, [] {
    auto lib = std::make_shared<HleLibrary>("libaaudio.so");
    auto add = [&](std::string_view name, Handler fn) {
      lib->add_function(name, make_thunk("libaaudio.so", name, fn));
    };
#define ADD(name, fn) add(name, audio::wrap_abi<&fn>())
    ADD("AAudio_createStreamBuilder", create);
    ADD("AAudio_setMMapPolicy", set_mmap_policy);
    ADD("AAudio_getMMapPolicy", get_mmap_policy);
    ADD("AAudioStreamBuilder_delete", delete_builder);
#define SET(name, member) \
  add("AAudioStreamBuilder_set" name, audio::wrap_abi<&set<&Builder::member>>())
    SET("Direction", direction);
    SET("SampleRate", rate);
    ADD("AAudioStreamBuilder_setChannelCount", set_channel_count);
    ADD("AAudioStreamBuilder_setSamplesPerFrame", set_channel_count);
    ADD("AAudioStreamBuilder_setChannelMask", set_channel_mask);
    ADD("AAudioStreamBuilder_setPackageName", set_attribution);
    ADD("AAudioStreamBuilder_setAttributionTag", set_attribution);
    SET("Format", format);
    SET("SharingMode", sharing);
    SET("PerformanceMode", performance);
    SET("BufferCapacityInFrames", capacity);
    SET("FramesPerDataCallback", callback_frames);
    SET("Usage", usage);
    SET("ContentType", content);
    SET("DeviceId", device);
    SET("SessionId", session);
    SET("InputPreset", input_preset);
    ADD("AAudioStreamBuilder_setDataCallback", data_callback);
    ADD("AAudioStreamBuilder_setErrorCallback", error_callback);
    ADD("AAudioStreamBuilder_openStream", open);
    ADD("AAudioStream_close", close);
    add("AAudioStream_requestStart",
        audio::wrap_abi<&control<&Stream::start>>());
    add("AAudioStream_requestPause",
        audio::wrap_abi<&control<&Stream::pause>>());
    add("AAudioStream_requestStop", audio::wrap_abi<&control<&Stream::request_stop>>());
    add("AAudioStream_requestFlush",
        audio::wrap_abi<&control<&Stream::flush>>());
    ADD("AAudioStream_read", read);
    ADD("AAudioStream_write", write);
    ADD("AAudioStream_waitForStateChange", wait);
    ADD("AAudioStream_getTimestamp", timestamp);
    ADD("AAudioStream_setBufferSizeInFrames", buffer_size);
    ADD("AAudio_convertResultToText", convert_result);
    ADD("AAudio_convertStreamStateToText", convert_state);
#define GET(name, kind) add("AAudioStream_get" name, &getter<Getter::kind>)
    GET("State", state);
    GET("SampleRate", rate);
    GET("ChannelCount", channels);
    GET("SamplesPerFrame", channels);
    GET("Format", format);
    GET("Direction", direction);
    GET("SharingMode", sharing);
    GET("PerformanceMode", performance);
    GET("Usage", usage);
    GET("ContentType", content);
    GET("BufferCapacityInFrames", capacity);
    GET("BufferSizeInFrames", size);
    GET("FramesPerBurst", burst);
    GET("FramesPerDataCallback", callback_frames);
    GET("XRunCount", xruns);
    GET("FramesWritten", written);
    GET("FramesRead", read);
    GET("DeviceId", device);
    GET("InputPreset", preset);
    GET("SessionId", session);
    GET("HardwareSampleRate", hardware_rate);
    GET("HardwareChannelCount", hardware_channels);
    GET("HardwareFormat", hardware_format);
    GET("ChannelMask", channel_mask);
    if (process_config().isa() == GuestIsa::a32)
      hle::a32::register_adapter("libaaudio.so", "AAudioStream_isMMapUsed", &getter<Getter::mmap>);
    add("AAudioStream_isMMapUsed", &getter<Getter::mmap>);
#undef GET
#undef SET
#undef ADD
    Linker::get().register_hle_library(std::move(lib));
  });
}
void shutdown_library() {
  std::unordered_map<GuestAddr, std::shared_ptr<Handle>> active;
  {
    std::lock_guard lock(registry_lock);
    active.swap(streams);
  }
  for (auto& [addr, h] : active) {
    h->stream->close();
    AddressSpace::get().free(addr);
  }
}
}  // namespace venture::hle::aaudio
