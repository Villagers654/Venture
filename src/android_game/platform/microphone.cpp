// SPDX-License-Identifier: GPL-3.0-or-later
// OVR_Microphone.h inside a game Venture installed on a headset
// (docs/design/native-headsets.md 5.3): the headset's own microphone through
// AAudio, 48 kHz mono float, the same contract as the PC runtime's
// hle/ovrplatform/microphone.cpp (whose streams are the runtime's audio
// backend and guest threads). GetPCM calls copy what is available and never
// block; the data callback runs on a thread of ours, never AAudio's real-time
// one. The game needs Android's RECORD_AUDIO permission, which it asks for as
// it would on a Quest.
#include "android_game/platform/microphone.h"

#include <aaudio/AAudio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "common/log.h"
#include "core/hle.h"

namespace venture::ovrplatform {
namespace {
constexpr std::int32_t kRate = 48000, kCapacity = 24000;  // 500 ms ring

struct Microphone {
  std::mutex lock;  // ring, stream
  std::vector<float> ring = std::vector<float>(kCapacity);
  std::size_t head = 0, count = 0;  // oldest sample, samples held
  std::uint64_t written = 0;        // samples recorded since start
  AAudioStream* stream = nullptr;
  bool started = false;
  // MicrophoneDataAvailableCallback(void*).
  std::atomic<std::uintptr_t> callback{0}, user{0};
  std::condition_variable wake;
  std::thread notifier;
  bool closing = false;

  void push(const float* samples, std::int32_t frames) {
    std::lock_guard guard(lock);
    for (std::int32_t i = 0; i < frames; ++i) {
      const std::size_t tail = (head + count) % ring.size();
      ring[tail] = samples[i];
      if (count < ring.size()) ++count;
      else head = (head + 1) % ring.size();  // full: the oldest sample goes
    }
    written += std::uint64_t(frames);
    wake.notify_one();
  }
  std::size_t read(float* out, std::size_t max) {
    std::lock_guard guard(lock);
    if (!started) return 0;
    const std::size_t n = std::min(max, count);
    for (std::size_t i = 0; i < n; ++i) out[i] = ring[(head + i) % ring.size()];
    head = (head + n) % ring.size();
    count -= n;
    return n;
  }
  std::size_t available() {
    std::lock_guard guard(lock);
    return started ? count : 0;
  }
};

aaudio_data_callback_result_t on_data(AAudioStream*, void* self, void* data, int32_t frames) {
  static_cast<Microphone*>(self)->push(static_cast<const float*>(data), frames);
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

std::mutex registry_lock;
std::map<std::uint64_t, std::shared_ptr<Microphone>> microphones;
std::uint64_t next_handle = 1;

std::shared_ptr<Microphone> find(std::uint64_t handle) {
  std::lock_guard guard(registry_lock);
  auto it = microphones.find(handle);
  return it == microphones.end() ? nullptr : it->second;
}

// Called with mic->lock held.
bool open(Microphone& mic) {
  AAudioStreamBuilder* builder = nullptr;
  if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return false;
  AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
  AAudioStreamBuilder_setSampleRate(builder, kRate);
  AAudioStreamBuilder_setChannelCount(builder, 1);
  AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
  AAudioStreamBuilder_setInputPreset(builder, AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION);
  AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
  AAudioStreamBuilder_setDataCallback(builder, on_data, &mic);
  const auto result = AAudioStreamBuilder_openStream(builder, &mic.stream);
  AAudioStreamBuilder_delete(builder);
  if (result != AAUDIO_OK) {
    log::warn("ovrplatform", "the microphone did not open: {} (does the game have RECORD_AUDIO?)",
              AAudio_convertResultToText(result));
    mic.stream = nullptr;
    return false;
  }
  return true;
}

void notifier(std::shared_ptr<Microphone> mic) {
  std::uint64_t notified = 0;
  std::unique_lock guard(mic->lock);
  while (!mic->closing) {
    mic->wake.wait_for(guard, std::chrono::milliseconds(50));
    const auto cb = mic->callback.load();
    if (mic->closing || !cb || !mic->started || mic->written == notified || !mic->count) continue;
    notified = mic->written;
    const auto user = mic->user.load();
    guard.unlock();
    reinterpret_cast<void (*)(void*)>(cb)(reinterpret_cast<void*>(user));
    guard.lock();
  }
}

void create(hle::CallContext& ctx) {
  auto mic = std::make_shared<Microphone>();
  {
    std::lock_guard guard(mic->lock);
    open(*mic);
  }
  mic->notifier = std::thread(notifier, mic);
  std::lock_guard guard(registry_lock);
  const auto handle = next_handle++;
  microphones.emplace(handle, std::move(mic));
  log::info("ovrplatform", "Platform microphone created");
  ctx.return_x(handle);
}
void close(const std::shared_ptr<Microphone>& mic) {
  AAudioStream* stream = nullptr;
  {
    std::lock_guard guard(mic->lock);
    mic->closing = true;
    mic->started = false;
    std::swap(stream, mic->stream);
    mic->wake.notify_one();
  }
  if (stream) {
    AAudioStream_requestStop(stream);
    AAudioStream_close(stream);  // waits for the data callback to return
  }
  if (mic->notifier.joinable()) {
    if (mic->notifier.get_id() == std::this_thread::get_id()) mic->notifier.detach();
    else mic->notifier.join();
  }
}
void destroy(hle::CallContext& ctx) {
  std::shared_ptr<Microphone> mic;
  {
    std::lock_guard guard(registry_lock);
    auto it = microphones.find(ctx.x(0));
    if (it == microphones.end()) return;
    mic = std::move(it->second);
    microphones.erase(it);
  }
  close(mic);
}
void start(hle::CallContext& ctx) {
  auto mic = find(ctx.x(0));
  if (!mic) return;
  std::lock_guard guard(mic->lock);
  if (!mic->stream && !open(*mic)) return;
  if (mic->started) return;
  mic->head = mic->count = 0;
  if (const auto result = AAudioStream_requestStart(mic->stream); result != AAUDIO_OK) {
    log::warn("ovrplatform", "the microphone did not start: {}", AAudio_convertResultToText(result));
    return;
  }
  mic->started = true;
}
void stop(hle::CallContext& ctx) {
  auto mic = find(ctx.x(0));
  if (!mic) return;
  std::lock_guard guard(mic->lock);
  if (mic->stream && mic->started) AAudioStream_requestStop(mic->stream);
  mic->started = false;
}
void available(hle::CallContext& ctx) {
  auto mic = find(ctx.x(0));
  ctx.return_x(mic ? mic->available() : 0);
}
void max_size(hle::CallContext& ctx) { ctx.return_x(find(ctx.x(0)) ? kCapacity : 0); }
std::size_t read(std::uint64_t handle, float* out, std::size_t count) {
  auto mic = find(handle);
  if (!mic || !out || !count) return 0;
  return mic->read(out, std::min<std::size_t>(count, kCapacity));
}
void pcm_float(hle::CallContext& ctx) {
  ctx.return_x(read(ctx.x(0), reinterpret_cast<float*>(static_cast<std::uintptr_t>(ctx.x(1))), ctx.x(2)));
}
void pcm16(hle::CallContext& ctx) {
  auto* out = reinterpret_cast<std::int16_t*>(static_cast<std::uintptr_t>(ctx.x(1)));
  std::vector<float> samples(std::min<std::size_t>(ctx.x(2), kCapacity));
  const auto count = out ? read(ctx.x(0), samples.data(), samples.size()) : 0;
  for (std::size_t i = 0; i < count; ++i)
    out[i] = static_cast<std::int16_t>(std::lround(std::clamp(samples[i], -1.0f, 1.0f) * 32767.0f));
  ctx.return_x(count);
}
void delay_hint(hle::CallContext&) {}  // a hint; AAudio's low-latency mode decides
void data_callback(hle::CallContext& ctx) {
  auto mic = find(ctx.x(0));
  if (!mic) return;
  mic->user.store(static_cast<std::uintptr_t>(ctx.x(2)));
  mic->callback.store(static_cast<std::uintptr_t>(ctx.x(1)));
}

const Export table[]{
    {"ovr_Microphone_Create", create},
    {"ovr_Microphone_Destroy", destroy},
    {"ovr_Microphone_Start", start},
    {"ovr_Microphone_Stop", stop},
    {"ovr_Microphone_GetNumSamplesAvailable", available},
    {"ovr_Microphone_GetOutputBufferMaxSize", max_size},
    {"ovr_Microphone_GetPCM", pcm16},
    {"ovr_Microphone_GetPCMFloat", pcm_float},
    {"ovr_Microphone_ReadData", pcm_float},
    {"ovr_Microphone_SetAcceptableRecordingDelayHint", delay_hint},
    {"ovr_Microphone_SetAudioDataAvailableCallback", data_callback},
};
}  // namespace

std::span<const Export> native_microphone_exports() { return table; }
}  // namespace venture::ovrplatform
