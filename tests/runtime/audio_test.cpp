#include "audio/audio.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#include "audio_test_support.h"
#include "core/thread.h"
#include "core/wait.h"
#include "hle/opensles/abi.h"

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
  const auto after = audio::Backend::get().mixer_stats();
  CHECK(after.non_silent_samples > before.non_silent_samples);
  CHECK(after.peak >= 0.25f);
  CHECK(object_events.load() == 0); // synchronous Realize has no async event
  CHECK(call(registration, {sls.player, 0, 0}) == 0);
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
  auto reopened = open(builder());
  CHECK(aa("AAudioStream_requestStart", {reopened}) == 0);
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

#ifdef _WIN32
#include <memory>
#include <vector>

#include "platform/change_cache.h"
#include "platform/win32/audio_endpoints.h"

namespace {
using Devices = std::vector<venture::audio::DeviceInfo>;
using venture::audio::Direction;
IMMNotificationClient* endpoint_client = nullptr;
bool capture_endpoint_client(IMMNotificationClient* client) {
  endpoint_client = client;
  return true;
}
bool notify_while_registering(IMMNotificationClient* client) {
  endpoint_client = client;
  client->OnDefaultDeviceChanged(eRender, eConsole, L"{registering}");
  return true;
}
bool refuse_endpoint_client(IMMNotificationClient*) { return false; }
// Host endpoint state behind the query seam; events arrive through the real
// IMMNotificationClient that watch_audio_endpoints registers.
struct HostEndpoints {
  Devices devices;
  int queries = 0;
  std::unique_ptr<venture::platform::WatchedQuery<Devices>> watch(venture::platform::RegisterEndpointClient register_client) {
    return std::make_unique<venture::platform::WatchedQuery<Devices>>(
        [register_client](auto changed, void* context) {
          return venture::platform::watch_audio_endpoints(changed, context, register_client);
        },
        [this] { ++queries; return devices; });
  }
};
void check_devices(const Devices& actual, const Devices& expected) {
  REQUIRE(actual.size() == expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    CHECK(actual[i].direction == expected[i].direction);
    CHECK(actual[i].name == expected[i].name);
    CHECK(venture::audio::device_id(actual[i]) == venture::audio::device_id(expected[i]));
  }
}
}  // namespace

#endif

#ifndef _WIN32
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <unistd.h>

#include "platform/audio_endpoints.h"

namespace {
std::optional<std::string> run(const std::string& command) {
  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) return std::nullopt;
  std::string out;
  char buffer[256];
  while (fgets(buffer, sizeof buffer, pipe)) out += buffer;
  if (pclose(pipe) != 0) return std::nullopt;
  return out;
}
void count_change(void* counter) { static_cast<std::atomic<int>*>(counter)->fetch_add(1); }
}  // namespace

#endif
