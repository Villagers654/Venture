#include "java/natives/audio_bridge.h"
#include "java/natives/media_bridge.h"
#include "audio/audio.h"
#include "av_telemetry.h"
#include "common/log.h"
#include "core/thread.h"
#include "java/internal.h"
#include <map>
#include <limits>
#include <chrono>
#include <format>

namespace venture::java {
namespace {
struct Track {
  std::shared_ptr<audio::Stream> stream;
  std::atomic<std::int64_t> origin{0};
  std::atomic<bool> stopped{true};
};
std::mutex track_mutex;
std::map<jlong, std::shared_ptr<Track>> tracks;
jlong next_track = 1;
bool closing = false;
std::shared_ptr<Track> track(jlong id) {
  std::lock_guard lock(track_mutex);
  auto it = tracks.find(id);
  return it == tracks.end() ? nullptr : it->second;
}
int result_code(int result) {
  if (result >= 0) return result;
  if (result == audio::disconnected) return -6;
  if (result == audio::invalid_handle || result == audio::invalid_state) return -3;
  return -1;
}
template<class F> auto guard(JNIEnv* env, F&& fn) -> decltype(fn()) {
  try { return fn(); }
  catch (const std::exception& e) { throw_java(env, "java/lang/IllegalStateException", e.what()); }
  return {};
}
jlong JNICALL open(JNIEnv* env, jclass, jint rate, jint channels, jint encoding, jint frames) {
  return guard(env, [&]() -> jlong {
    audio::Config c;
    c.api = "AudioTrack";
    c.follow_default_output = true;
    c.stop_at_written_position = true;
    c.rate = rate; c.channels = channels; c.capacity = frames;
    c.callback_frames = std::min(frames, audio::refill_quantum(rate));
    switch (encoding) {
      case 2: c.format = audio::Format::i16; break;
      case 3: c.format = audio::Format::u8; break;
      case 4: c.format = audio::Format::f32; break;
      case 21: c.format = audio::Format::i24; break;
      case 22: c.format = audio::Format::i32; break;
      default: return 0;
    }
    int result = 0;
    auto stream = audio::Stream::create(c, &result);
    if (!stream) { log::warn("AudioTrack", "PCM output open failed: {}", result); return 0; }
    stream->set_buffer_size(frames);
    // Wake blocking Java writers outside the real-time device callback.
    stream->launch_worker([](audio::Stream&) {});
    log::info("AudioTrack", "PCM output opened: {} Hz, {} channels, encoding {}", rate, channels, encoding);
    auto value = std::make_shared<Track>(); value->stream = std::move(stream);
    std::lock_guard lock(track_mutex);
    if (closing) { value->stream->close(); return 0; }
    const auto id = next_track++;
    tracks.emplace(id, std::move(value));
    return id;
  });
}
jint JNICALL control(JNIEnv* env, jclass, jlong id, jint operation) {
  return guard(env, [&]() -> jint {
    auto value = track(id);
    if (!value) return -3;
    auto& stream = *value->stream;
    int result;
    switch (operation) {
      case 0: {
        { std::lock_guard lock(track_mutex); tracks.erase(id); }
        return result_code(stream.close());
      }
      case 1:
        result = stream.start_resuming_drain();
        if (!result && value->stopped.load()) {
          value->origin.store(stream.frames_read.load());
          value->stopped.store(false);
        }
        return result_code(result);
      case 2: return result_code(stream.pause());
      case 3: {
        const auto begin = std::chrono::steady_clock::now();
        // Publish STOPPED to Java immediately; the mixer retains queued PCM.
        // A paused/not-yet-started track must not start playing merely to stop.
        const auto state = stream.state.load();
        result = state == audio::State::started || state == audio::State::starting ||
            state == audio::State::stopping ? stream.request_stop() : stream.stop();
        if (!result) value->stopped.store(true);
        if (stream.timing_enabled) av::emit("audiotrack_stop", std::format(
            "\"source_id\":{},\"duration_ns\":{},\"written\":{},\"consumed\":{},\"result\":{}",
            stream.telemetry_id,
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count(),
            stream.frames_written.load(), stream.frames_read.load(), result));
        return result_code(result);
      }
      case 4: result = stream.flush_cancelling_drain(); break;
      case 5: return value->stopped.load() ? 0 : static_cast<jint>(stream.frames_read.load() - value->origin.load());
      default: return -2;
    }
    if (!result) value->origin.store(stream.frames_read.load());
    return result_code(result);
  });
}
jint JNICALL write(JNIEnv* env, jclass, jlong id, jbyteArray bytes, jint offset, jint size, jboolean blocking) {
  return guard(env, [&]() -> jint {
    auto value = track(id);
    if (!value) return -3;
    if (!bytes || offset < 0 || size < 0 || offset > env->GetArrayLength(bytes) - size) return -2;
    auto& stream = *value->stream;
    const auto stride = audio::sample_bytes(stream.config.format) * stream.config.channels;
    const auto count = size / stride;
    if (!count) return 0;
    std::vector<jbyte> pcm(count * stride);
    env->GetByteArrayRegion(bytes, offset, static_cast<jsize>(pcm.size()), pcm.data());
    if (env->ExceptionCheck()) return -2;
    auto* caller = blocking ? &GuestThread::adopt_current() : nullptr;
    const auto result = stream.transfer(caller, pcm.data(), static_cast<int>(count),
        blocking ? std::numeric_limits<std::int64_t>::max() : 0, true);
    return result < 0 ? result_code(result) : static_cast<jint>(result * stride);
  });
}
jint JNICALL volume(JNIEnv*, jclass, jlong id, jfloat left, jfloat right) {
  auto value = track(id);
  if (!value) return -3;
  value->stream->gain_left.store(left); value->stream->gain_right.store(right);
  return 0;
}
void JNICALL shutdown(JNIEnv*, jclass) { shutdown_audio_tracks(); }
}
void shutdown_audio_tracks() {
  shutdown_media_players();
  std::map<jlong, std::shared_ptr<Track>> ending;
  { std::lock_guard lock(track_mutex); closing = true; ending.swap(tracks); }
  for (auto& [id, value] : ending) value->stream->close();
}
void register_audio_tracks(JNIEnv* env) {
  auto cls = env->FindClass("android/media/AudioTrack");
  if (!cls) return;
  const JNINativeMethod methods[]{
    {const_cast<char*>("nativeOpen"), const_cast<char*>("(IIII)J"), reinterpret_cast<void*>(open)},
    {const_cast<char*>("nativeControl"), const_cast<char*>("(JI)I"), reinterpret_cast<void*>(control)},
    {const_cast<char*>("nativeWrite"), const_cast<char*>("(J[BIIZ)I"), reinterpret_cast<void*>(write)},
    {const_cast<char*>("nativeVolume"), const_cast<char*>("(JFF)I"), reinterpret_cast<void*>(volume)},
    {const_cast<char*>("nativeShutdown"), const_cast<char*>("()V"), reinterpret_cast<void*>(shutdown)},
  };
  env->RegisterNatives(cls, methods, static_cast<jint>(std::size(methods)));
  env->DeleteLocalRef(cls);
}
}
