#include "hle/ovrplatform/microphone.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "audio/audio.h"
#include "common/log.h"
#include "core/hle.h"
#include "core/a32_types.h"
#include "core/process.h"
#include "core/thread.h"

namespace venture::ovrplatform {
namespace {
// OVR_Microphone.h: 48 kHz mono, about 480 samples per 10 ms; the GetPCM
// calls copy what is available and never block, from any thread.
constexpr std::int32_t rate = 48000, capacity = 24000;  // 500 ms ring
struct Microphone {
  std::mutex lock;
  std::shared_ptr<audio::Stream> stream;
  // MicrophoneDataAvailableCallback(void*), run on the stream's worker thread.
  std::atomic<GuestAddr> callback{0}, user{0};
  std::int64_t notified = 0;  // worker-owned: frames recorded at the last call
};
std::mutex registry_lock;
std::map<std::uint64_t, std::shared_ptr<Microphone>> microphones;
// ovrMicrophoneHandle is opaque to the caller; small nonzero values fit A32 too.
std::uint64_t next_handle = 1;

std::shared_ptr<Microphone> find(std::uint64_t handle) {
  std::lock_guard lock(registry_lock);
  auto it = microphones.find(handle);
  return it == microphones.end() ? nullptr : it->second;
}
std::shared_ptr<audio::Stream> open_stream(const std::shared_ptr<Microphone>& mic) {
  audio::Config c;
  c.api = "ovr_Microphone";
  c.direction = audio::Direction::input;
  c.format = audio::Format::f32;
  c.rate = rate;
  c.channels = 1;
  c.capacity = capacity;
  // A Quest's microphone does not go away; follow the default input instead.
  c.follow_default_input = true;
  std::int32_t result = 0;
  auto stream = audio::Stream::create(c, &result);
  if (!stream) {
    log::warn("ovrplatform", "Platform microphone open failed: {}", result);
    return stream;
  }
  stream->launch_worker([weak = std::weak_ptr<Microphone>(mic)](audio::Stream& s) {
    auto mic = weak.lock();
    if (!mic) return;
    const auto cb = mic->callback.load();
    const auto recorded = s.frames_written.load();
    if (!cb || s.state.load() != audio::State::started || recorded == mic->notified || !s.ring.readable()) return;
    mic->notified = recorded;
    auto& thread = GuestThread::adopt_current();
    if (process_config().isa() == GuestIsa::a32)
      thread.call32(cb, hle::a32::CallBuilder32{}.pointer(mic->user.load()));
    else
      thread.call(cb, {mic->user.load()});
  });
  return stream;
}
void create(hle::CallContext& ctx) {
  auto mic = std::make_shared<Microphone>();
  mic->stream = open_stream(mic);
  std::lock_guard lock(registry_lock);
  const auto handle = next_handle++;
  microphones.emplace(handle, std::move(mic));
  log::info("ovrplatform", "Platform microphone created");
  ctx.return_x(handle);
}
void destroy(hle::CallContext& ctx) {
  std::shared_ptr<Microphone> mic;
  {
    std::lock_guard lock(registry_lock);
    auto it = microphones.find(ctx.x(0));
    if (it == microphones.end()) return;
    mic = std::move(it->second);
    microphones.erase(it);
  }
  // Close outside the lock: it joins the worker, whose callback may be reading.
  std::shared_ptr<audio::Stream> stream;
  { std::lock_guard lock(mic->lock); stream.swap(mic->stream); }
  if (stream) stream->close();
}
void start(hle::CallContext& ctx) {
  auto mic = find(ctx.x(0));
  if (!mic) return;
  std::lock_guard lock(mic->lock);
  if (!mic->stream) mic->stream = open_stream(mic);
  if (mic->stream && mic->stream->start())
    log::warn("ovrplatform", "Platform microphone start failed");
}
void stop(hle::CallContext& ctx) {
  auto mic = find(ctx.x(0));
  if (!mic) return;
  std::lock_guard lock(mic->lock);
  if (mic->stream) mic->stream->stop();
}
void available(hle::CallContext& ctx) {
  auto mic = find(ctx.x(0));
  if (!mic) return ctx.return_x(0);
  std::lock_guard lock(mic->lock);
  ctx.return_x(mic->stream ? mic->stream->ring.readable() : 0);
}
void max_size(hle::CallContext& ctx) {
  ctx.return_x(find(ctx.x(0)) ? capacity : 0);
}
// Copies up to count samples of the running recording into out.
std::size_t read(std::uint64_t handle, float* out, std::size_t count) {
  auto mic = find(handle);
  if (!mic || !out || !count) return 0;
  std::lock_guard lock(mic->lock);
  if (!mic->stream || mic->stream->state.load() != audio::State::started) return 0;
  return mic->stream->read(out, static_cast<std::uint32_t>(std::min<std::size_t>(count, capacity)));
}
void pcm_float(hle::CallContext& ctx) {
  ctx.return_x(read(ctx.x(0), g2h<float>(ctx.x(1)), ctx.x(2)));
}
void pcm16(hle::CallContext& ctx) {
  auto* out = g2h<std::int16_t>(ctx.x(1));
  std::vector<float> samples(std::min<std::size_t>(ctx.x(2), capacity));
  const auto count = out ? read(ctx.x(0), samples.data(), samples.size()) : 0;
  for (std::size_t i = 0; i < count; ++i)
    out[i] = static_cast<std::int16_t>(std::lround(std::clamp(samples[i], -1.0f, 1.0f) * 32767.0f));
  ctx.return_x(count);
}
void delay_hint(hle::CallContext&) {}  // A hint; the host mixer period decides.
void data_callback(hle::CallContext& ctx) {
  // Called from the recording's worker thread when new samples arrive.
  auto mic = find(ctx.x(0));
  if (!mic) return;
  mic->user.store(ctx.x(2));
  mic->callback.store(ctx.x(1));
}
#define VENTURE_EXPORT(name, handler) Export{name, handler}
const Export table[]{
    VENTURE_EXPORT("ovr_Microphone_Create", create),
    VENTURE_EXPORT("ovr_Microphone_Destroy", destroy),
    VENTURE_EXPORT("ovr_Microphone_Start", start),
    VENTURE_EXPORT("ovr_Microphone_Stop", stop),
    VENTURE_EXPORT("ovr_Microphone_GetNumSamplesAvailable", available),
    VENTURE_EXPORT("ovr_Microphone_GetOutputBufferMaxSize", max_size),
    VENTURE_EXPORT("ovr_Microphone_GetPCM", pcm16),
    VENTURE_EXPORT("ovr_Microphone_GetPCMFloat", pcm_float),
    VENTURE_EXPORT("ovr_Microphone_ReadData", pcm_float),
    VENTURE_EXPORT("ovr_Microphone_SetAcceptableRecordingDelayHint", delay_hint),
    VENTURE_EXPORT("ovr_Microphone_SetAudioDataAvailableCallback", data_callback),
};
#undef VENTURE_EXPORT
}  // namespace
std::span<const Export> microphone_exports() { return table; }
void close_microphones() {
  std::map<std::uint64_t, std::shared_ptr<Microphone>> closing;
  { std::lock_guard lock(registry_lock); closing.swap(microphones); }
  for (auto& [handle, mic] : closing) {
    std::shared_ptr<audio::Stream> stream;
    { std::lock_guard lock(mic->lock); stream.swap(mic->stream); }
    if (stream) stream->close();
  }
}
}  // namespace venture::ovrplatform
