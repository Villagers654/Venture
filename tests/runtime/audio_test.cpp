#include "audio/audio.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_test_support.h"
#include "core/thread.h"
#include "core/wait.h"
#include "hle/opensles/abi.h"
#include "platform/thread_priority.h"

#include "../async_wait.h"

using namespace venture;
using namespace venture::test::audio_core;
namespace sl = venture::hle::opensles::abi;
namespace {
void setup() {
  static bool ready = [] {
    auto result = audio::Backend::get().use_null_device();
    if (!result) throw std::runtime_error("audio backend already initialized");
    audio::register_libraries();
    return true;
  }();
  (void)ready;
}
std::int64_t aa(std::string_view name,
                std::initializer_list<std::uint64_t> args) {
  return call(symbol("libaaudio.so", name), args);
}
bool until(const std::function<bool()>& predicate, int ms = venture::test::hang_timeout_ms) {
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!predicate() && std::chrono::steady_clock::now() < end)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const bool ready = predicate();
  if (!ready) FAIL_CHECK("audio condition timed out: output active=", audio::Backend::get().output_active(),
                         " non-silent samples=", audio::Backend::get().mixer_stats().non_silent_samples);
  return ready;
}
GuestAddr builder(bool capture = false, int format = 2, int rate = 48000) {
  GuestAddr b = 0;
  CHECK(aa("AAudio_createStreamBuilder", {h2g(&b)}) == 0);
  aa("AAudioStreamBuilder_setDirection", {b, capture ? 1u : 0u});
  aa("AAudioStreamBuilder_setFormat", {b, std::uint64_t(format)});
  aa("AAudioStreamBuilder_setSampleRate", {b, std::uint64_t(rate)});
  aa("AAudioStreamBuilder_setChannelCount", {b, capture ? 1u : 2u});
  return b;
}
GuestAddr open(GuestAddr b) {
  GuestAddr s = 0;
  CHECK(aa("AAudioStreamBuilder_openStream", {b, h2g(&s)}) == 0);
  CHECK(s != 0);
  aa("AAudioStreamBuilder_delete", {b});
  return s;
}
template <class T>
const T& table(GuestAddr itf) {
  return *g2h<T>(*g2h<GuestAddr>(itf));
}
GuestAddr iid(const char* name) {
  return *g2h<GuestAddr>(symbol("libOpenSLES.so", name));
}
GuestAddr get(GuestAddr obj, const char* name) {
  GuestAddr itf = 0;
  CHECK(call(table<sl::ObjectTable>(obj).GetInterface,
             {obj, iid(name), h2g(&itf)}) == 0);
  return itf;
}
void realize(GuestAddr obj) {
  CHECK(call(table<sl::ObjectTable>(obj).Realize, {obj, 0}) == 0);
}
struct SlSession {
  GuestAddr engine = 0, engine_itf = 0, mix = 0, player = 0;
  explicit SlSession(GuestAddr object_callback = 0, GuestAddr context = 0) {
    CHECK(call(symbol("libOpenSLES.so", "slCreateEngine"),
               {h2g(&engine), 0, 0, 0, 0, 0}) == 0);
    realize(engine);
    engine_itf = get(engine, "SL_IID_ENGINE");
    if (object_callback)
      REQUIRE(call(table<sl::ObjectTable>(engine).RegisterCallback,
                   {engine, object_callback, context}) == 0);
    CHECK(call(table<sl::EngineTable>(engine_itf).CreateOutputMix,
               {engine_itf, h2g(&mix), 0, 0, 0}) == 0);
    realize(mix);
  }
  GuestAddr create_player(sl::PcmEx& pcm) {
    sl::QueueLocator queue{0x800007bd, 4};
    sl::OutputMixLocator loc{4, 0, mix};
    sl::Data source{h2g(&queue), h2g(&pcm)}, sink{h2g(&loc), 0};
    CHECK(call(table<sl::EngineTable>(engine_itf).CreateAudioPlayer,
               {engine_itf, h2g(&player), h2g(&source), h2g(&sink), 0, 0, 0}) ==
          0);
    realize(player);
    return player;
  }
  ~SlSession() {
    if (engine) call(table<sl::ObjectTable>(engine).Destroy, {engine});
  }
};
}  // namespace
TEST_CASE(
    "audio: sample conversions preserve signs, endpoints and packed I24") {
  for (auto format :
       {audio::Format::i16, audio::Format::f32, audio::Format::i24,
        audio::Format::i32, audio::Format::u8}) {
    for (auto value : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f}) {
      std::uint8_t bytes[4]{};
      audio::encode_sample(bytes, format, value);
      CHECK(audio::decode_sample(bytes, format) ==
            doctest::Approx(value).epsilon(0.01));
    }
  }
  std::uint8_t min24[] = {0, 0, 0x80}, max24[] = {0xff, 0xff, 0x7f};
  CHECK(audio::decode_sample(min24, audio::Format::i24) == -1);
  CHECK(audio::decode_sample(max24, audio::Format::i24) > 0.9999);
  float nan = std::nanf("");
  CHECK(audio::decode_sample(&nan, audio::Format::f32) == 0);
}
TEST_CASE(
    "audio: AAudio blocking calls, state transitions, counters, timeout and "
    "invalid handles") {
  setup();
  auto s = open(builder());
  CHECK(aa("AAudioStream_getState", {s}) == 2);
  CHECK(aa("AAudioStream_getSharingMode", {s}) == 1);
  std::array<float, 480 * 2> samples;
  samples.fill(0.25f);
  CHECK(aa("AAudioStream_write", {s, h2g(samples.data()), 480, 0}) == 480);
  CHECK(aa("AAudioStream_getFramesWritten", {s}) == 480);
  CHECK(aa("AAudioStream_requestStart", {s}) == 0);
  auto before = monotonic_ns();
  std::vector<float> big(4800 * 2, 0.125f);
  CHECK(aa("AAudioStream_write", {s, h2g(big.data()), 4800, 1000000000}) ==
        4800);
  CHECK(monotonic_ns() - before >= 10'000'000);
  CHECK(aa("AAudioStream_getFramesRead", {s}) > 0);
  std::int64_t pos = 0, stamp = 0;
  CHECK(aa("AAudioStream_getTimestamp", {s, 1, h2g(&pos), h2g(&stamp)}) == 0);
  CHECK(pos > 0);
  CHECK(stamp > 0);
  CHECK(aa("AAudioStream_requestPause", {s}) == 0);
  CHECK(aa("AAudioStream_requestFlush", {s}) == 0);
  std::int32_t state = -1;
  CHECK(aa("AAudioStream_waitForStateChange", {s, 4, h2g(&state), 0}) == 0);
  CHECK(state == 8);
  CHECK(aa("AAudioStream_waitForStateChange", {s, 8, h2g(&state), 2000000}) ==
        audio::timeout);
  CHECK(aa("AAudioStream_close", {s}) == 0);
  CHECK(aa("AAudioStream_requestStart", {s}) == audio::invalid_handle);
  CHECK(aa("AAudioStream_close", {s}) == audio::invalid_handle);
}
TEST_CASE(
    "audio: AAudio guest callback delivery on adopted workers, stop and close "
    "quiescence") {
  setup();
  std::atomic<int> count{0};
  std::atomic<bool> adopted{true};
  auto cb = add_callback([&](std::span<const std::uint64_t> args) {
    adopted.store(adopted.load() && GuestThread::current() != nullptr);
    std::fill_n(g2h<float>(args[2]), std::size_t(args[3]) * 2, 0.375f);
    count.fetch_add(1);
    return 0;
  });
  auto b = builder();
  aa("AAudioStreamBuilder_setDataCallback", {b, cb, 42});
  aa("AAudioStreamBuilder_setFramesPerDataCallback", {b, 240});
  auto s = open(b);
  CHECK(count.load() == 0); // opening/preparing must not execute the callback
  CHECK(aa("AAudioStream_requestStart", {s}) == 0);
  CHECK(until([&] { return count.load() >= 10; }));
  // A delayed controller can observe arbitrarily many valid callbacks. Refill
  // bounds are checked with manual device pulls in the audio render fixture.
  INFO("callbacks=", count.load(), " adopted=", adopted.load());
  CHECK(adopted.load());
  CHECK(aa("AAudioStream_getFramesWritten", {s}) >= 2400);
  CHECK(aa("AAudioStream_getFramesRead", {s}) > 0);
  CHECK(aa("AAudioStream_close", {s}) == 0);
  auto last = count.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  CHECK(count.load() == last);
}
TEST_CASE(
    "audio: OpenSL pointer-cell IDs, UUID value equality and PCM_EX queue "
    "callback") {
  setup();
  SlSession sls;
  CHECK(g2h<sl::InterfaceId>(iid("SL_IID_ENGINE"))->time_low == 0x8d97c260);
  auto copy = *g2h<sl::InterfaceId>(iid("SL_IID_ENGINE"));
  GuestAddr engine_copy = 0;
  CHECK(call(table<sl::ObjectTable>(sls.engine).GetInterface,
             {sls.engine, h2g(&copy), h2g(&engine_copy)}) == 0);
  CHECK(engine_copy == sls.engine_itf);
  sl::PcmEx pcm{{4, 2, 48000000, 32, 32, 3, 2}, 3};
  auto obj = sls.create_player(pcm);
  auto play = get(obj, "SL_IID_PLAY"),
       queue = get(obj, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE"),
       volume = get(obj, "SL_IID_VOLUME");
  CHECK(call(table<sl::VolumeTable>(volume).SetVolumeLevel,
             {volume, std::uint64_t(std::int64_t(-600))}) == 0);
  std::int16_t level = 0;
  CHECK(call(table<sl::VolumeTable>(volume).GetVolumeLevel,
             {volume, h2g(&level)}) == 0);
  CHECK(level == -600);
  std::atomic<int> completed{0};
  std::atomic<bool> arguments{false};
  auto cb = add_callback([&](std::span<const std::uint64_t> args) {
    arguments.store(args[0] == queue && args[1] == 567 &&
                    GuestThread::current());
    completed.fetch_add(1);
    return 0;
  });
  CHECK(call(table<sl::BufferQueueTable>(queue).RegisterCallback,
             {queue, cb, 567}) == 0);
  std::array<float, 480 * 2> samples;
  samples.fill(0.5f);
  CHECK(call(table<sl::BufferQueueTable>(queue).Enqueue,
             {queue, h2g(samples.data()), sizeof(samples)}) == 0);
  sl::QueueState state{};
  CHECK(call(table<sl::BufferQueueTable>(queue).GetState,
             {queue, h2g(&state)}) == 0);
  CHECK(state.count == 1);
  CHECK(call(table<sl::PlayTable>(play).SetPlayState, {play, 3}) == 0);
  CHECK(until([&] { return completed.load() == 1; }));
  CHECK(arguments.load());
  CHECK(call(table<sl::BufferQueueTable>(queue).GetState,
             {queue, h2g(&state)}) == 0);
  CHECK(state.count == 0);
  CHECK(state.index == 1);
  std::uint32_t pos = 0;
  // The queue callback releases copied guest storage, not played frames.
  // Wait on the playback position itself; a fixed delay or copy-completion
  // event cannot establish that the null device has consumed this packet.
  const auto position_deadline = std::chrono::steady_clock::now() + venture::test::hang_timeout;
  do {
    REQUIRE(call(table<sl::PlayTable>(play).GetPosition, {play, h2g(&pos)}) == 0);
    if (pos >= 10) break;
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < position_deadline);
  INFO("playback position ms=", pos, " completed buffers=", completed.load());
  CHECK(pos >= 10);
  CHECK(call(table<sl::BufferQueueTable>(queue).Clear, {queue}) == 0);
  CHECK(call(table<sl::BufferQueueTable>(queue).GetState,
             {queue, h2g(&state)}) == 0);
  CHECK(state.index == 0);
}
TEST_CASE("audio: OpenSL object callback registration permits Wwise engine initialization and PCM playback") {
  setup();
  std::atomic<int> object_events{0}, completed{0};
  auto object_cb = add_callback([&](auto) {
    object_events.fetch_add(1);
    return 0;
  });
  SlSession sls(object_cb, 789);
  sl::QueueLocator locator{0x800007bd, 2};
  sl::Pcm pcm{2, 2, 48000000, 16, 16, 3, 2};
  sl::OutputMixLocator mix{4, 0, sls.mix};
  sl::Data source{h2g(&locator), h2g(&pcm)}, sink{h2g(&mix), 0};
  REQUIRE(call(table<sl::EngineTable>(sls.engine_itf).CreateAudioPlayer,
               {sls.engine_itf, h2g(&sls.player), h2g(&source), h2g(&sink), 0, 0, 0}) == 0);
  auto registration = table<sl::ObjectTable>(sls.player).RegisterCallback;
  // Object callback registration is also valid before player realization.
  REQUIRE(call(registration, {sls.player, object_cb, 789}) == 0);
  realize(sls.player);
  CHECK(call(registration, {sls.player, object_cb, 456}) == 0);
  CHECK(call(registration, {0, object_cb, 456}) == 2);
  auto play = get(sls.player, "SL_IID_PLAY");
  auto queue = get(sls.player, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE");
  auto queue_cb = add_callback([&](auto) {
    completed.fetch_add(1);
    return 0;
  });
  REQUIRE(call(table<sl::BufferQueueTable>(queue).RegisterCallback,
               {queue, queue_cb, 0}) == 0);
  std::array<std::int16_t, 960 * 2> samples;
  samples.fill(8192);
  const auto before = audio::Backend::get().mixer_stats();
  REQUIRE(call(table<sl::BufferQueueTable>(queue).Enqueue,
               {queue, h2g(samples.data()), sizeof(samples)}) == 0);
  REQUIRE(call(table<sl::PlayTable>(play).SetPlayState, {play, 3}) == 0);
  REQUIRE(until([&] { return completed.load() == 1; }));
  // With 10 ms device periods (Linux) the whole buffer fits the stream's two
  // bursts, so it completes before the device has mixed it.
  REQUIRE(until([&] { return audio::Backend::get().mixer_stats().non_silent_samples > before.non_silent_samples; }));
  const auto after = audio::Backend::get().mixer_stats();
  CHECK(after.peak >= 0.25f);
  CHECK(object_events.load() == 0); // synchronous Realize has no async event
  CHECK(call(registration, {sls.player, 0, 0}) == 0);
}
TEST_CASE("audio: OpenSL buffer queues keep calling back with odd device-period buffers") {
  // Oboe's OpenSL output enqueues two device-burst buffers and refills from the
  // completion callback; a 561-frame burst (an 8-channel virtual endpoint) is
  // larger than half the stream's ring.
  setup();
  SlSession sls(0, 0);
  sl::QueueLocator locator{0x800007bd, 2};
  sl::Pcm pcm{2, 2, 48000000, 16, 16, 3, 2};
  sl::OutputMixLocator mix{4, 0, sls.mix};
  sl::Data source{h2g(&locator), h2g(&pcm)}, sink{h2g(&mix), 0};
  REQUIRE(call(table<sl::EngineTable>(sls.engine_itf).CreateAudioPlayer,
               {sls.engine_itf, h2g(&sls.player), h2g(&source), h2g(&sink), 0, 0, 0}) == 0);
  realize(sls.player);
  auto play = get(sls.player, "SL_IID_PLAY");
  auto queue = get(sls.player, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE");
  static std::array<std::array<std::int16_t, 561 * 2>, 2> buffers;
  std::atomic<int> completed{0};
  auto queue_cb = add_callback([&](auto) {
    auto& buffer = buffers[std::size_t(completed.fetch_add(1)) % 2];
    call(table<sl::BufferQueueTable>(queue).Enqueue, {queue, h2g(buffer.data()), sizeof(buffer)});
    return 0;
  });
  REQUIRE(call(table<sl::BufferQueueTable>(queue).RegisterCallback, {queue, queue_cb, 0}) == 0);
  // OpenSL moves between any two play states; Oboe pauses a stream it never started.
  CHECK(call(table<sl::PlayTable>(play).SetPlayState, {play, 2}) == 0);
  for (auto& buffer : buffers)
    REQUIRE(call(table<sl::BufferQueueTable>(queue).Enqueue, {queue, h2g(buffer.data()), sizeof(buffer)}) == 0);
  REQUIRE(call(table<sl::PlayTable>(play).SetPlayState, {play, 3}) == 0);
  // 100 buffers are 1.17 s of audio: completions must follow the device clock.
  CHECK(until([&] { return completed.load() >= 100; }));
  CHECK(call(table<sl::PlayTable>(play).SetPlayState, {play, 2}) == 0);
  CHECK(call(table<sl::PlayTable>(play).SetPlayState, {play, 1}) == 0);
  CHECK(call(table<sl::PlayTable>(play).SetPlayState, {play, 2}) == 0);
}
TEST_CASE("audio: silence a game writes between sounds counts as a dropout, a fade or a pause does not") {
  // A game mixer that misses its deadline (Batman's Meta XR Audio sink under
  // CPU contention) hands over zeros: the host device never underruns, the
  // output still crackles.
  setup();
  audio::Config config;
  config.format = audio::Format::f32;
  config.capacity = 65536;
  config.burst_frames = 32768;
  std::int32_t result = 0;
  auto stream = audio::Stream::create(config, &result);
  REQUIRE(stream);
  std::vector<float> pcm;
  auto put = [&](std::size_t frames, float value) { pcm.insert(pcm.end(), frames * 2, value); };
  put(480, 0);      // leading silence before any sound
  put(100, 0.5f);
  put(480, 0);      // dropout: sound cut to zeros and back
  put(100, -0.5f);
  put(100, 1e-5f);  // a fade that reaches zero
  put(480, 0);
  put(100, 1e-5f);
  put(100, 0.5f);
  put(9600, 0);     // a 200 ms pause
  put(100, 0.5f);
  // Two writes split the dropout: the detector follows the stream, not a call.
  const auto half = std::uint32_t(pcm.size() / 4);
  REQUIRE(stream->write(pcm.data(), half) == half);
  REQUIRE(stream->write(pcm.data() + half * 2, std::uint32_t(pcm.size() / 2) - half) == pcm.size() / 2 - half);
  CHECK(stream->guest_gaps.load() == 1);
  CHECK(stream->guest_gap_frames.load() == 480);
  stream->close();
}
TEST_CASE("audio: a refused real-time request still gets host audio scheduling until SCHED_OTHER") {
  // Wwise, Meta XR Audio and UE4 ask SCHED_FIFO/SCHED_RR for their audio
  // renderers. Android refuses an app (the guest sees EPERM and SCHED_OTHER);
  // the host runs the thread at audio priority.
  std::thread([] {
    constexpr std::int32_t tid = 0x7ffff00;
    platform::bind_thread_priority(tid);
    CHECK(platform::request_realtime_thread(tid) == 0);
    int policy = -1, priority = -1;
    CHECK(platform::get_audio_thread_scheduler(tid, &policy, &priority) == 0);
    CHECK(policy == 0);
    CHECK(priority == 0);
#ifdef _WIN32
    CHECK(platform::current_audio_priority_class() != platform::AudioPriorityClass::normal);
#endif
    // A nice change keeps the request; SCHED_OTHER withdraws it.
    CHECK(platform::set_thread_priority(tid, -4) == 0);
#ifdef _WIN32
    CHECK(platform::current_audio_priority_class() != platform::AudioPriorityClass::normal);
#endif
    CHECK(platform::set_audio_thread_priority(tid, 0) == 0);
    platform::apply_pending_thread_priority();
    CHECK(platform::current_audio_priority_class() == platform::AudioPriorityClass::normal);
    platform::unbind_thread_priority(tid);
  }).join();
  CHECK(platform::request_realtime_thread(0x7ffff01) != 0);  // no such thread
}
TEST_CASE(
    "audio: AAudio error callback follows actual miniaudio stop notification "
    "and allows reopen") {
  setup();
  std::atomic<int> errors{0};
  std::atomic<std::int32_t> result{0};
  auto b = builder();
  auto cb = add_callback([&](std::span<const std::uint64_t> args) {
    result.store(std::int32_t(args[2]));
    errors.fetch_add(1);
    return 0;
  });
  aa("AAudioStreamBuilder_setErrorCallback", {b, cb, 0});
  auto s = open(b);
  CHECK(aa("AAudioStream_requestStart", {s}) == 0);
  CHECK(stop_null_output());
  CHECK(until([&] { return errors.load() == 1; }));
  CHECK(result.load() == audio::disconnected);
  CHECK(aa("AAudioStream_getState", {s}) == 13);
  std::array<float, 2> samples{};
  CHECK(aa("AAudioStream_write", {s, h2g(samples.data()), 1, 0}) ==
        audio::disconnected);
  CHECK(aa("AAudioStream_close", {s}) == 0);
  // The game reopens at once, with the endpoint's last properties; the monitor
  // brings the endpoint back without the game waiting on it (an H3VR2 player's
  // FMOD reopen waited on a WASAPI open until the game froze).
  auto reopened = open(builder());
  CHECK(aa("AAudioStream_getFramesPerBurst", {reopened}) > 0);
  CHECK(aa("AAudioStream_getHardwareSampleRate", {reopened}) == 48000);
  CHECK(aa("AAudioStream_requestStart", {reopened}) == 0);
  CHECK(until([] { return audio::Backend::get().route(audio::Direction::output).open; }));
  CHECK(aa("AAudioStream_close", {reopened}) == 0);
}
TEST_CASE(
    "audio: callback STOP and self-close reject prevent guest worker lifetime "
    "races") {
  setup();
  std::atomic<int> callbacks{0};
  std::atomic<std::int32_t> close_result{0};
  auto b = builder();
  auto cb = add_callback([&](std::span<const std::uint64_t> args) {
    close_result.store(std::int32_t(aa("AAudioStream_close", {args[0]})));
    callbacks.fetch_add(1);
    return 1;  // AAUDIO_CALLBACK_RESULT_STOP
  });
  aa("AAudioStreamBuilder_setDataCallback", {b, cb, 0});
  auto s = open(b);
  CHECK(aa("AAudioStream_requestStart", {s}) == 0);
  CHECK(until([&] { return aa("AAudioStream_getState", {s}) == 10; }));
  CHECK(close_result.load() == audio::invalid_state);
  CHECK(callbacks.load() == 1);
  CHECK(aa("AAudioStream_close", {s}) == 0);
}
TEST_CASE("audio: AAudio hardware getters report Venture's 48 kHz float stereo mixer, not the host endpoint") {
  setup();
  auto b = builder(false, 1, 44100);  // mono I16 at 44.1 kHz
  aa("AAudioStreamBuilder_setChannelCount", {b, 1});
  auto s = open(b);
  CHECK(aa("AAudioStream_getSampleRate", {s}) == 44100);
  CHECK(aa("AAudioStream_getHardwareSampleRate", {s}) == 48000);
  CHECK(aa("AAudioStream_getHardwareChannelCount", {s}) == 2);
  CHECK(aa("AAudioStream_getHardwareFormat", {s}) == 2);  // AAUDIO_FORMAT_PCM_FLOAT
  CHECK(aa("AAudioStream_close", {s}) == 0);
}
TEST_CASE("audio: AAudio channel masks and attribution follow AOSP builder rules") {
  setup();
  auto b = builder();
  aa("AAudioStreamBuilder_setPackageName", {b, 0});
  aa("AAudioStreamBuilder_setAttributionTag", {b, 0});
  aa("AAudioStreamBuilder_setChannelMask", {b, 1});  // AAUDIO_CHANNEL_MONO
  auto s = open(b);
  CHECK(aa("AAudioStream_getChannelCount", {s}) == 1);
  CHECK(aa("AAudioStream_getChannelMask", {s}) == 1);
  CHECK(aa("AAudioStream_close", {s}) == 0);
  b = builder();  // a count clears an earlier mask; the stream reports STEREO
  aa("AAudioStreamBuilder_setChannelMask", {b, 1});
  aa("AAudioStreamBuilder_setChannelCount", {b, 2});
  s = open(b);
  CHECK(aa("AAudioStream_getChannelCount", {s}) == 2);
  CHECK(aa("AAudioStream_getChannelMask", {s}) == 3);
  CHECK(aa("AAudioStream_close", {s}) == 0);
  b = builder();
  aa("AAudioStreamBuilder_setChannelMask", {b, std::uint64_t(std::int64_t(-1))});  // INVALID
  GuestAddr rejected = 0;
  CHECK(aa("AAudioStreamBuilder_openStream", {b, h2g(&rejected)}) == audio::illegal_argument);
  CHECK(rejected == 0);
  CHECK(aa("AAudioStreamBuilder_delete", {b}) == 0);
}

TEST_CASE("audio: a lost microphone fails capture with DISCONNECTED, not a hang (issue 89)") {
  setup();
  auto s = open(builder(true, 2, 48000));
  REQUIRE(aa("AAudioStream_requestStart", {s}) == 0);
  // A blocking read waiting on the microphone wakes when the device stops.
  auto stopper = std::thread([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(stop_null_input());
  });
  std::array<float, 48000> samples{};
  const auto begin = std::chrono::steady_clock::now();
  const auto read = aa("AAudioStream_read", {s, h2g(samples.data()), samples.size(), 30'000'000'000ull});
  stopper.join();
  CHECK(std::chrono::steady_clock::now() - begin < std::chrono::seconds(10));
  // Frames read before the loss are returned first; the next read reports it.
  if (read >= 0) CHECK(aa("AAudioStream_read", {s, h2g(samples.data()), 1, 0}) == audio::disconnected);
  else CHECK(read == audio::disconnected);
  CHECK(until([&] { return aa("AAudioStream_getState", {s}) == 13; }));
  CHECK(aa("AAudioStream_requestStart", {s}) == audio::disconnected);
  CHECK(aa("AAudioStream_close", {s}) == 0);
  // A new stream reopens the microphone.
  auto reopened = open(builder(true, 2, 48000));
  CHECK(aa("AAudioStream_requestStart", {reopened}) == 0);
  CHECK(aa("AAudioStream_close", {reopened}) == 0);
}
TEST_CASE("audio: an OpenSL recorder keeps recording across a lost microphone (issue 89)") {
  setup();
  SlSession session;
  GuestAddr recorder = 0;
  sl::IoLocator device{3, 1, 0xffffffffu, 0, 0};
  sl::QueueLocator locator{0x800007bd, 2};
  sl::Pcm pcm{2, 1, 48000 * 1000, 16, 16, 4, 2};
  sl::Data source{h2g(&device), 0}, sink{h2g(&locator), h2g(&pcm)};
  REQUIRE(call(table<sl::EngineTable>(session.engine_itf).CreateAudioRecorder,
               {session.engine_itf, h2g(&recorder), h2g(&source), h2g(&sink), 0, 0, 0}) == 0);
  struct Destroy { GuestAddr object; ~Destroy() { call(table<sl::ObjectTable>(object).Destroy, {object}); } } destroy{recorder};
  realize(recorder);
  auto queue = get(recorder, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE"), record = get(recorder, "SL_IID_RECORD");
  static std::array<std::array<std::int16_t, 480>, 2> buffers{};
  std::atomic<int> filled{0};
  auto callback = add_callback([&](auto args) {
    filled.fetch_add(1);
    call(table<sl::BufferQueueTable>(args[0]).Enqueue, {args[0], h2g(buffers[0].data()), sizeof(buffers[0])});
    return 0;
  });
  REQUIRE(call(table<sl::BufferQueueTable>(queue).RegisterCallback, {queue, callback, 0}) == 0);
  for (auto& buffer : buffers)
    REQUIRE(call(table<sl::BufferQueueTable>(queue).Enqueue, {queue, h2g(buffer.data()), sizeof(buffer)}) == 0);
  REQUIRE(call(table<sl::RecordTable>(record).SetRecordState, {record, 3}) == 0);
  CHECK(until([&] { return filled.load() >= 3; }));
  // Android restores a legacy recorder's route; the recorder never errors.
  CHECK(stop_null_input());
  const auto before = filled.load();
  CHECK(until([&] { return filled.load() >= before + 5; }));
  std::uint32_t state = 0;
  CHECK(call(table<sl::RecordTable>(record).GetRecordState, {record, h2g(&state)}) == 0);
  CHECK(state == 3);
  CHECK(call(table<sl::RecordTable>(record).SetRecordState, {record, 1}) == 0);
}
TEST_CASE("audio: WASAPI endpoint IDs match the endpoint an XR runtime names") {
  const std::string_view render = "{0.0.0.00000000}.{8D2C4C1E-6B1F-4F39-9C1A-2E4A6D7B9F01}";
  const std::string_view capture = "{0.0.1.00000000}.{8d2c4c1e-6b1f-4f39-9c1a-2e4a6d7b9f01}";
  // The full endpoint ID, as the Meta runtime answers, in any case.
  CHECK(audio::wasapi_endpoint_matches(render, "{0.0.0.00000000}.{8d2c4c1e-6b1f-4f39-9c1a-2e4a6d7b9f01}"));
  CHECK(audio::wasapi_endpoint_matches(render, " {0.0.0.00000000}.{8D2C4C1E-6B1F-4F39-9C1A-2E4A6D7B9F01}\n"));
  // The endpoint GUID alone, braced or bare.
  CHECK(audio::wasapi_endpoint_matches(render, "{8d2c4c1e-6b1f-4f39-9c1a-2e4a6d7b9f01}"));
  CHECK(audio::wasapi_endpoint_matches(capture, "8D2C4C1E-6B1F-4F39-9C1A-2E4A6D7B9F01"));
  // Another endpoint, the other flow's full ID, fragments and nothing.
  CHECK_FALSE(audio::wasapi_endpoint_matches(render, "{0.0.0.00000000}.{8d2c4c1e-6b1f-4f39-9c1a-2e4a6d7b9f02}"));
  CHECK_FALSE(audio::wasapi_endpoint_matches(render, "{8d2c4c1e-6b1f-4f39-9c1a-2e4a6d7b9f02}"));
  CHECK_FALSE(audio::wasapi_endpoint_matches(capture, render));
  CHECK_FALSE(audio::wasapi_endpoint_matches(render, "9f01}"));
  CHECK_FALSE(audio::wasapi_endpoint_matches(render, "{0.0.0.00000000}"));
  CHECK_FALSE(audio::wasapi_endpoint_matches(render, ""));
  CHECK_FALSE(audio::wasapi_endpoint_matches("", "{8d2c4c1e-6b1f-4f39-9c1a-2e4a6d7b9f01}"));
}
TEST_CASE("audio: playback and recording use the headset endpoint the XR runtime names and follow its answer") {
  setup();
  auto& backend = audio::Backend::get();
  // The runtime's answers. Static: the monitor may still hold the source.
  static std::mutex answers;
  static std::string output, input;
  const auto answer = [](std::string out, std::string in) {
    {
      std::lock_guard lock(answers);
      output = std::move(out);
      input = std::move(in);
    }
    audio::Backend::get().request_route_check();
  };
  answer("NULL Playback Device", "NULL Capture Device");
  backend.set_headset_endpoints("Mock runtime (XR_OCULUS_audio_device_guid)", [](audio::Direction direction) {
    std::lock_guard lock(answers);
    return direction == audio::Direction::input ? input : output;
  });
  struct Reset {
    ~Reset() { audio::Backend::get().set_headset_endpoints({}, nullptr); }
  } reset;
  std::atomic<int> count{0};
  auto cb = add_callback([&](std::span<const std::uint64_t> args) {
    std::fill_n(g2h<float>(args[2]), std::size_t(args[3]) * 2, 0.25f);
    count.fetch_add(1);
    return 0;
  });
  auto b = builder();
  aa("AAudioStreamBuilder_setDataCallback", {b, cb, 0});
  auto s = open(b);
  REQUIRE(aa("AAudioStream_requestStart", {s}) == 0);
  auto mic = open(builder(true, 2, 48000));
  REQUIRE(aa("AAudioStream_requestStart", {mic}) == 0);
  const auto on = [&](audio::Direction direction, bool headset) {
    return until([&] {
      const auto route = backend.route(direction);
      return route.open && route.headset == headset;
    });
  };
  CHECK(on(audio::Direction::output, true));
  CHECK(on(audio::Direction::input, true));
  CHECK(backend.route(audio::Direction::output).device == "NULL Playback Device");
  CHECK(backend.route(audio::Direction::input).device == "NULL Capture Device");
  CHECK(backend.route(audio::Direction::output).named == "NULL Playback Device");
  // An endpoint that is not active: the system default, and the stream plays on.
  answer("{0.0.0.00000000}.{00000000-0000-0000-0000-000000000000}", "NULL Capture Device");
  CHECK(on(audio::Direction::output, false));
  CHECK(backend.route(audio::Direction::output).device.empty());
  CHECK(backend.route(audio::Direction::input).headset);
  auto before = count.load();
  CHECK(until([&] { return count.load() >= before + 5; }));
  CHECK(aa("AAudioStream_getState", {s}) == 4);
  // The runtime names it again (a headset reconnecting), then nothing.
  answer("NULL Playback Device", "");
  CHECK(on(audio::Direction::output, true));
  CHECK(on(audio::Direction::input, false));
  before = count.load();
  CHECK(until([&] { return count.load() >= before + 5; }));
  CHECK(aa("AAudioStream_getState", {s}) == 4);
  CHECK(aa("AAudioStream_getState", {mic}) == 4);
  // Without a runtime source, the system default.
  audio::Backend::get().set_headset_endpoints("Mock runtime names no headset endpoint", nullptr);
  CHECK(on(audio::Direction::output, false));
  CHECK(aa("AAudioStream_close", {s}) == 0);
  CHECK(aa("AAudioStream_close", {mic}) == 0);
}
TEST_CASE("audio: the PC output plays once while the headset endpoint is the system default") {
  setup();
  auto& backend = audio::Backend::get();
  // The null backend's one output is both the headset endpoint and the system default.
  backend.set_headset_endpoints("Mock runtime (XR_OCULUS_audio_device_guid)", [](audio::Direction direction) {
    return std::string(direction == audio::Direction::output ? "NULL Playback Device" : "");
  });
  struct Reset {
    ~Reset() {
      audio::Backend::get().set_pc_output(false);
      audio::Backend::get().set_headset_endpoints({}, nullptr);
    }
  } reset;
  auto s = open(builder());
  REQUIRE(aa("AAudioStream_requestStart", {s}) == 0);
  CHECK(until([&] { return backend.route(audio::Direction::output).headset; }));
  CHECK(backend.pc_output().plan == audio::PcOutputPlan::off);
  backend.set_pc_output(true);
  CHECK(backend.pc_output().plan == audio::PcOutputPlan::same_endpoint);
  CHECK_FALSE(backend.pc_output().open);
  CHECK_FALSE(backend.output_mirror().active());
  // No headset endpoint: the game plays on the system default, once.
  backend.set_headset_endpoints("Mock runtime names no headset endpoint", nullptr);
  CHECK(until([&] { return backend.pc_output().plan == audio::PcOutputPlan::not_on_headset; }));
  CHECK_FALSE(backend.pc_output().open);
  backend.set_pc_output(false);
  CHECK(backend.pc_output().plan == audio::PcOutputPlan::off);
  CHECK(aa("AAudioStream_close", {s}) == 0);
}
