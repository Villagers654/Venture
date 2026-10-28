#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include <future>
#include <iostream>
#include <chrono>
#include <thread>

#include "audio/audio.h"
#include "hle/opensles/abi.h"
#include "core/thread.h"
#include "core/signals.h"
#include "audio_test_support.h"
#include "platform/audio_timer.h"
#include "platform/thread_priority.h"
#ifdef __linux__
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

#include "../async_wait.h"

using namespace venture;
using namespace venture::test::audio_core;
namespace venture::audio {
struct StreamTestAccess {
  static void refill(Stream& stream, const std::function<void(Stream&)>& work) {
    stream.refill_worker(work);
  }
  static unsigned waiters(Stream& stream) { return stream.event_waiters_.load(); }
};
}
namespace {
void setup() {
  static bool ready = [] {
    auto result = audio::Backend::get().use_null_device(false);
    if (!result) throw std::runtime_error("backend initialized");
    audio::register_libraries();
    return true;
  }();
  (void)ready;
}
std::int64_t aa(std::string_view name,
                std::initializer_list<std::uint64_t> args) {
  return call(symbol("libaaudio.so", name), args);
}
GuestAddr open(bool input, int format, int rate, int channels) {
  GuestAddr builder = 0, stream = 0;
  CHECK(aa("AAudio_createStreamBuilder", {h2g(&builder)}) == 0);
  aa("AAudioStreamBuilder_setDirection", {builder, input ? 1u : 0u});
  aa("AAudioStreamBuilder_setFormat", {builder, std::uint64_t(format)});
  aa("AAudioStreamBuilder_setSampleRate", {builder, std::uint64_t(rate)});
  aa("AAudioStreamBuilder_setChannelCount", {builder, std::uint64_t(channels)});
  CHECK(aa("AAudioStreamBuilder_openStream", {builder, h2g(&stream)}) == 0);
  aa("AAudioStreamBuilder_delete", {builder});
  return stream;
}
}  // namespace
TEST_CASE("audio render: AAudio stop plays queued PCM before STOPPED and restarts monotonically") {
  setup();
  const auto stream = open(false, 2, 48000, 2);
  struct Close { GuestAddr stream; ~Close() { aa("AAudioStream_close", {stream}); } } close{stream};
  std::array<float, 480> pcm;
  std::array<float, 960> output;
  pcm.fill(.375f);
  REQUIRE(aa("AAudioStream_requestStart", {stream}) == 0);
  REQUIRE(aa("AAudioStream_write", {stream, h2g(pcm.data()), 240, 0}) == 240);
  const auto xruns = aa("AAudioStream_getXRunCount", {stream});
  REQUIRE(aa("AAudioStream_requestStop", {stream}) == 0);
  CHECK(aa("AAudioStream_getState", {stream}) == int(audio::State::stopping));
  CHECK(aa("AAudioStream_getFramesRead", {stream}) == 0);
  CHECK(aa("AAudioStream_requestStart", {stream}) == audio::invalid_state);
  CHECK(aa("AAudioStream_write", {stream, h2g(pcm.data()), 240, 0}) == audio::invalid_state);
  // No automatic null-device consumption: the real mixer must play the PCM.
  audio::Backend::get().process(audio::Direction::output, output.data(), nullptr, 480);
  CHECK(std::all_of(output.begin(), output.begin() + 480, [](float sample) { return std::abs(sample - .375f) < .0001f; }));
  CHECK(std::all_of(output.begin() + 480, output.end(), [](float sample) { return sample == 0; }));
  audio::State next{};
  CHECK(aa("AAudioStream_waitForStateChange", {stream, int(audio::State::stopping), h2g(&next), 5'000'000'000}) == 0);
  CHECK(next == audio::State::stopped);
  CHECK(aa("AAudioStream_getFramesRead", {stream}) == 240);
  CHECK(aa("AAudioStream_getFramesWritten", {stream}) == 240);
  CHECK(aa("AAudioStream_getXRunCount", {stream}) == xruns);
  pcm.fill(.125f);
  REQUIRE(aa("AAudioStream_write", {stream, h2g(pcm.data()), 240, 0}) == 240);
  REQUIRE(aa("AAudioStream_requestStart", {stream}) == 0);
  audio::Backend::get().process(audio::Direction::output, output.data(), nullptr, 240);
  CHECK(std::all_of(output.begin(), output.begin() + 480, [](float sample) { return std::abs(sample - .125f) < .0001f; }));
  CHECK(aa("AAudioStream_getFramesRead", {stream}) == 480);
  CHECK(aa("AAudioStream_getFramesWritten", {stream}) == 480);
}
TEST_CASE("audio render: AAudio close retains callback getters until the worker joins") {
  setup();
  std::promise<void> entered, release;
  auto began = entered.get_future();
  auto finish = release.get_future().share();
  std::atomic<std::int64_t> getter{audio::invalid_handle};
  std::atomic<bool> closed{false};
  GuestAddr builder{}, stream{};
  REQUIRE(aa("AAudio_createStreamBuilder", {h2g(&builder)}) == 0);
  const auto callback = add_callback([&](auto args) {
    entered.set_value();
    finish.wait();
    getter = aa("AAudioStream_getXRunCount", {args[0]});
    return 0;
  });
  aa("AAudioStreamBuilder_setDataCallback", {builder, callback, 0});
  REQUIRE(aa("AAudioStreamBuilder_openStream", {builder, h2g(&stream)}) == 0);
  aa("AAudioStreamBuilder_delete", {builder});
  bool released = false;
  std::thread closer;
  struct Cleanup {
    std::promise<void>& release; bool& released; std::thread& closer; GuestAddr stream;
    ~Cleanup() {
      if (!released) release.set_value();
      if (closer.joinable()) closer.join();
      else aa("AAudioStream_close", {stream});
    }
  } cleanup{release, released, closer, stream};
  REQUIRE(aa("AAudioStream_requestStart", {stream}) == 0);
  INFO("callback stream state=", aa("AAudioStream_getState", {stream}));
  REQUIRE(began.wait_for(venture::test::hang_timeout) == std::future_status::ready);
  closer = std::thread([&] {
    CHECK(aa("AAudioStream_close", {stream}) == 0);
    closed = true;
  });
  const auto until = std::chrono::steady_clock::now() + venture::test::hang_timeout;
  while (std::chrono::steady_clock::now() < until &&
         aa("AAudioStream_getState", {stream}) != int(audio::State::closed))
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(aa("AAudioStream_getState", {stream}) == int(audio::State::closed));
  CHECK_FALSE(closed.load());
  CHECK(aa("AAudioStream_close", {stream}) == audio::invalid_state);
  release.set_value(); released = true;
  closer.join();
  CHECK(closed.load());
  CHECK(getter.load() >= 0);
  CHECK(aa("AAudioStream_getXRunCount", {stream}) == audio::invalid_handle);
}
TEST_CASE("audio render: fixed AAudio callback blocks adapt to a smaller writable device buffer") {
  setup();
  std::atomic<unsigned> callbacks{0}, callback_frames{0};
  const auto cb = add_callback([&](std::span<const std::uint64_t> args) {
    callback_frames.store(unsigned(args[3]));
    std::fill_n(g2h<float>(args[2]), std::size_t(args[3]) * 2, 0.25f);
    callbacks.fetch_add(1);
    return 0;
  });
  GuestAddr builder = 0, handle = 0;
  REQUIRE(aa("AAudio_createStreamBuilder", {h2g(&builder)}) == 0);
  aa("AAudioStreamBuilder_setFormat", {builder, 2});
  aa("AAudioStreamBuilder_setFramesPerDataCallback", {builder, 960});
  aa("AAudioStreamBuilder_setBufferCapacityInFrames", {builder, 1920});
  aa("AAudioStreamBuilder_setDataCallback", {builder, cb, 0});
  REQUIRE(aa("AAudioStreamBuilder_openStream", {builder, h2g(&handle)}) == 0);
  REQUIRE(aa("AAudioStreamBuilder_delete", {builder}) == 0);
  struct Close { GuestAddr handle; ~Close() { aa("AAudioStream_close", {handle}); } } close{handle};
  CHECK(callbacks == 0);
  CHECK(aa("AAudioStream_getFramesPerDataCallback", {handle}) == 960);
  REQUIRE(aa("AAudioStream_getFramesPerBurst", {handle}) == 240);
  CHECK(aa("AAudioStream_getBufferCapacityInFrames", {handle}) == 1920);
  REQUIRE(aa("AAudioStream_setBufferSizeInFrames", {handle, 0}) == 240);
  REQUIRE(aa("AAudioStream_requestStart", {handle}) == 0);
  auto await = [&](std::int64_t written) {
    const auto end = std::chrono::steady_clock::now() + venture::test::hang_timeout;
    while (aa("AAudioStream_getFramesWritten", {handle}) < written && std::chrono::steady_clock::now() < end)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto actual = aa("AAudioStream_getFramesWritten", {handle});
    if (actual < written) FAIL_CHECK("audio refill stalled: expected frames=", written, " actual=", actual,
                                   " state=", aa("AAudioStream_getState", {handle}));
    return actual >= written;
  };
  std::array<float, 480> output;
  for (unsigned chunk = 1; chunk <= 4; ++chunk) {
    REQUIRE(await(chunk * 240));
    CHECK(callbacks == 1); // retain the remainder, do not call the guest again
    audio::Backend::get().process(audio::Direction::output, output.data(), nullptr, 240);
    CHECK(std::all_of(output.begin(), output.end(), [](float v) { return v == 0.25f; }));
  }
  CHECK(callback_frames == 960);
  CHECK(aa("AAudioStream_getXRunCount", {handle}) == 0);
}
#ifdef __linux__
#endif
TEST_CASE(
    "audio render: AAudio samples reach the shared mixer and clip only after "
    "summing") {
  setup();
  auto a = open(false, 2, 48000, 2), b = open(false, 2, 48000, 2);
  std::array<float, 240 * 2> data;
  data.fill(0.75f);
  CHECK(aa("AAudioStream_write", {a, h2g(data.data()), 240, 0}) == 240);
  CHECK(aa("AAudioStream_write", {b, h2g(data.data()), 240, 0}) == 240);
  aa("AAudioStream_requestStart", {a});
  aa("AAudioStream_requestStart", {b});
  std::array<float, 240 * 2> output;
  const auto before = audio::Backend::get().mixer_stats();
  audio::Backend::get().process(audio::Direction::output, output.data(),
                                nullptr, 240);
  const auto after = audio::Backend::get().mixer_stats();
  CHECK(after.output_frames - before.output_frames == 240);
  CHECK(after.non_silent_samples - before.non_silent_samples == 480);
  CHECK(after.peak == 1);
  CHECK(std::all_of(output.begin(), output.end(),
                    [](float v) { return v == 1; }));
  CHECK(aa("AAudioStream_getFramesRead", {a}) == 240);
  CHECK(aa("AAudioStream_getFramesRead", {b}) == 240);
  aa("AAudioStream_close", {a});
  aa("AAudioStream_close", {b});
}
TEST_CASE(
    "audio render: guest 44.1 kHz mono converts into 48 kHz stereo with "
    "bounded frame accounting") {
  setup();
  auto s = open(false, 1, 44100, 1);
  CHECK(aa("AAudioStream_setBufferSizeInFrames", {s, 882}) == 882);
  std::vector<std::int16_t> pcm(882, 8192);
  CHECK(aa("AAudioStream_write", {s, h2g(pcm.data()), 882, 0}) == 882);
  aa("AAudioStream_requestStart", {s});
  std::vector<float> output(960 * 2);
  audio::Backend::get().process(audio::Direction::output, output.data(),
                                nullptr, 960);
  // Ignore the converter's filter startup; channel routing is still identical.
  for (unsigned i = 20; i < 950; i += 47) {
    CHECK(output[2 * i] == doctest::Approx(0.25).epsilon(0.002));
    CHECK(output[2 * i + 1] == output[2 * i]);
  }
  auto read = aa("AAudioStream_getFramesRead", {s});
  CHECK(read >= 880);
  CHECK(read <= 882);
  aa("AAudioStream_close", {s});
}
TEST_CASE(
    "audio render: packed I24 and I32 travel through HLE without byte-stride "
    "errors") {
  setup();
  for (int format : {3, 4}) {
    auto s = open(false, format, 48000, 2);
    auto f = audio::Format(format);
    auto bytes = audio::sample_bytes(f);
    std::vector<std::uint8_t> pcm(240 * 2 * bytes);
    for (unsigned i = 0; i < 240; ++i) {
      audio::encode_sample(pcm.data() + 2 * i * bytes, f, -0.5f);
      audio::encode_sample(pcm.data() + (2 * i + 1) * bytes, f, 0.25f);
    }
    CHECK(aa("AAudioStream_write", {s, h2g(pcm.data()), 240, 0}) == 240);
    aa("AAudioStream_requestStart", {s});
    std::array<float, 480> output;
    audio::Backend::get().process(audio::Direction::output, output.data(),
                                  nullptr, 240);
    CHECK(output[100] == -0.5f);
    CHECK(output[101] == 0.25f);
    aa("AAudioStream_close", {s});
  }
}
TEST_CASE("audio render: nonzero stereo capture converts to 16 kHz mono I16") {
  setup();
  auto s = open(true, 1, 16000, 1);
  aa("AAudioStream_requestStart", {s});
  std::array<float, 480 * 2> input;
  input.fill(0.5f);
  audio::Backend::get().process(audio::Direction::input, nullptr, input.data(),
                                480);
  std::array<std::int16_t, 160> captured{};
  auto frames = aa("AAudioStream_read", {s, h2g(captured.data()), 160, 0});
  CHECK(frames >= 155);
  CHECK(frames <= 160);
  CHECK(captured[100] == doctest::Approx(16384).epsilon(0.002));
  aa("AAudioStream_close", {s});
}

TEST_CASE("audio render: OpenSL releases copied buffers before host playback") {
  setup();
  unsigned rate = 48000, packet_frames = 480;
  SUBCASE("48 kHz ten-millisecond packet") {}
  SUBCASE("44.1 kHz ten-millisecond packet") { rate = 44100; packet_frames = 441; }
  namespace sl = venture::hle::opensles::abi;
  auto table = []<class T>(GuestAddr interface) -> const T& { return *g2h<T>(*g2h<GuestAddr>(interface)); };
  auto realize = [&](GuestAddr object) { REQUIRE(call(table.operator()<sl::ObjectTable>(object).Realize, {object, 0}) == 0); };
  auto get = [&](GuestAddr object, const char* name) {
    GuestAddr result = 0;
    REQUIRE(call(table.operator()<sl::ObjectTable>(object).GetInterface,
      {object, *g2h<GuestAddr>(symbol("libOpenSLES.so", name)), h2g(&result)}) == 0);
    return result;
  };
  GuestAddr engine = 0, mix = 0, player = 0;
  REQUIRE(call(symbol("libOpenSLES.so", "slCreateEngine"), {h2g(&engine), 0, 0, 0, 0, 0}) == 0);
  struct Destroy { GuestAddr object; ~Destroy() { call(g2h<sl::ObjectTable>(*g2h<GuestAddr>(object))->Destroy, {object}); } } destroy{engine};
  realize(engine);
  auto engine_itf = get(engine, "SL_IID_ENGINE");
  REQUIRE(call(table.operator()<sl::EngineTable>(engine_itf).CreateOutputMix, {engine_itf, h2g(&mix), 0, 0, 0}) == 0);
  realize(mix);
  sl::QueueLocator locator{0x800007bd, 1};
  sl::Pcm pcm{2, 2, rate * 1000, 16, 16, 3, 2};
  sl::OutputMixLocator output_mix{4, 0, mix};
  sl::Data source{h2g(&locator), h2g(&pcm)}, sink{h2g(&output_mix), 0};
  REQUIRE(call(table.operator()<sl::EngineTable>(engine_itf).CreateAudioPlayer,
    {engine_itf, h2g(&player), h2g(&source), h2g(&sink), 0, 0, 0}) == 0);
  realize(player);
  auto queue = get(player, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE"), play = get(player, "SL_IID_PLAY");
  auto completed = std::make_shared<std::atomic<unsigned>>(0);
  auto copied = std::make_shared<std::promise<void>>();
  auto copy_complete = copied->get_future();
  auto callback = add_callback([completed, copied](auto) {
    if (completed->fetch_add(1) == 0) copied->set_value();
    return 0;
  });
  REQUIRE(call(table.operator()<sl::BufferQueueTable>(queue).RegisterCallback, {queue, callback, 0}) == 0);
  std::vector<std::int16_t> samples(packet_frames * 2, 8192);
  REQUIRE(call(table.operator()<sl::BufferQueueTable>(queue).Enqueue, {queue, h2g(samples.data()), samples.size() * sizeof(samples[0])}) == 0);
  REQUIRE(call(table.operator()<sl::PlayTable>(play).SetPlayState, {play, 3}) == 0);
  const auto copied_status = copy_complete.wait_for(venture::test::hang_timeout);
  INFO("packet frames=", packet_frames, " rate=", rate, " callbacks=", completed->load(),
       " output active=", audio::Backend::get().output_active());
  REQUIRE(copied_status == std::future_status::ready);
  CHECK(completed->load() == 1);
  // No background device pulls: copying the entire packet and returning its
  // guest storage must leave the actual playback position at zero.
  std::uint32_t position = 123;
  REQUIRE(call(table.operator()<sl::PlayTable>(play).GetPosition, {play, h2g(&position)}) == 0);
  CHECK(position == 0);
  // Async STARTING may finish after the callback returns. Await renderability,
  // rather than making a manual pull race that worker state transition.
  const auto ready_deadline = std::chrono::steady_clock::now() + venture::test::hang_timeout;
  while (!audio::Backend::get().output_active() && std::chrono::steady_clock::now() < ready_deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  REQUIRE(audio::Backend::get().output_active());
  // The guest may reuse the completed buffer immediately. Host PCM is owned.
  std::fill(samples.begin(), samples.end(), 24576);
  std::array<float, 480> output;
  audio::Backend::get().process(audio::Direction::output, output.data(), nullptr, 240);
  // The fractional-rate converter has an initial filter transient.
  CHECK(output[200] == doctest::Approx(0.25));
  CHECK(output.back() == doctest::Approx(0.25));
  if (rate == 48000) {
    REQUIRE(call(table.operator()<sl::PlayTable>(play).GetPosition, {play, h2g(&position)}) == 0);
    CHECK(position == 5);
    audio::Backend::get().process(audio::Direction::output, output.data(), nullptr, 240);
    REQUIRE(call(table.operator()<sl::PlayTable>(play).GetPosition, {play, h2g(&position)}) == 0);
    CHECK(position == 10);
    CHECK(std::all_of(output.begin(), output.end(), [](float value) { return value == doctest::Approx(0.25); }));
  }
}
