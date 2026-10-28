#include "../mock_openxr_runtime/control.h"
#include "audio/audio.h"
#include "core/hle_runtime.h"
#include "core/linker.h"
#include "core/thread.h"
#include "fake_cpu32.h"
#include "hle/opensles/abi.h"
#include "hle/opensles/opensles.h"
#include "hle/aaudio/aaudio.h"
#include "hle/vrapi/abi32.h"
#include "hle/vrapi/integration.h"
#include "hle/vrapi/vrapi.h"
#include "ovrplatform_support.h"
#include "xr_images_fixture.h"
#include <doctest/doctest.h>

#include "../async_wait.h"

using namespace venture;
namespace vr = hle::vrapi;
namespace sl = hle::opensles;
using hle::a32::CallBuilder32;
namespace {
struct Calls {
  test::LowMemory32 memory;
  GuestAddr put(unsigned offset, const auto &value) {
    std::memcpy(memory.at<void>(offset), &value, sizeof(value));
    return memory.address + offset;
  }
  std::uint64_t call(std::string_view lib, std::string_view name,
                     const CallBuilder32 &args) {
    for (auto &fn : hle::registered_functions())
      if (fn.library == lib && fn.name == name) {
        REQUIRE(fn.a32);
        REQUIRE(fn.handler != hle::a32::diagnostic);
        test::FakeCpu32 cpu;
        cpu.set_sp(memory.address + 0xf000);
        args.apply(cpu, memory.address);
        hle::CallContext ctx(GuestThread::adopt_current(), cpu, fn);
        fn.handler(ctx);
        return cpu.x(0) | (cpu.x(1) << 32);
      }
    FAIL("missing registered function " << name);
    return 0;
  }
  std::uint32_t words(std::string_view lib, std::string_view name,
                      std::initializer_list<std::uint64_t> words = {}) {
    CallBuilder32 b;
    b.untyped_words({words.begin(), words.size()});
    return std::uint32_t(call(lib, name, b));
  }
  std::uint32_t v(std::string_view name,
                  std::initializer_list<std::uint64_t> w = {}) {
    return words("libvrapi.so", name, w);
  }
};
} // namespace
TEST_CASE("A32 VrApi 1.21 wire layouts, sret, Token32 and frame submission") {
  vr::register_libraries();
  test::XrImageFixture images;
  xr::install_xr_images(&images);
  Calls c;
  vr::abi32::IntegrationInit integration{1, 1, 21, 0, {1, 2, 3}};
  auto p = c.put(0, integration);
  c.put(28, 0xfeedfaceu);
  CHECK(c.words("libvrintegrationloader.so", "vrintegration_Initialize", {p}) ==
        0);
  CHECK(vr::integration::has_initialized());
  CHECK(*c.memory.at<std::uint32_t>(28) == 0xfeedface);
  integration.java.env = 0;
  c.put(0, integration);
  CHECK(std::int32_t(c.words("libvrintegrationloader.so",
                             "vrintegration_Initialize", {p})) ==
        vr::abi::invalid);
  integration = {1, 1, 16, 0, {1, 2, 3}};
  c.put(0, integration);
  CHECK(c.words("libvrintegrationloader.so", "vrintegration_Initialize", {p}) ==
        0);
  vr::abi32::Init init{1, 1, 1, 21, 0, 0x10300, {1, 2, 3}};
  p = c.put(64, init);
  c.put(100, 0xfeedfaceu);
  REQUIRE(c.v("vrapi_Initialize", {p}) == 0);
  CHECK(*c.memory.at<std::uint32_t>(100) == 0xfeedface);
  auto ring = c.v("vrapi_CreateTextureSwapChain", {0, 4, 64, 64, 1, 1});
  REQUIRE(ring);
  CHECK(c.v("vrapi_GetTextureSwapChainLength", {ring}) == 3);
  auto texture = c.v("vrapi_GetTextureSwapChainHandle", {ring, 1});
  REQUIRE(texture);
  vr::abi32::Mode mode{2, 0, {1, 2, 3}, 0x123456789abcdef0, 5, 6};
  auto mobile = c.v("vrapi_EnterVrMode", {c.put(128, mode)});
  REQUIRE(mobile);
  CHECK(mobile != ring);
  double time = std::bit_cast<double>(
      c.call("libvrapi.so", "vrapi_GetPredictedDisplayTime",
             CallBuilder32{}.value(mobile).value(std::int64_t(1))));
  REQUIRE(time > 0);
  auto tracking = c.memory.address + 256;
  c.put(256 + 360, 0xfeedfaceu);
  c.call("libvrapi.so", "vrapi_GetPredictedTracking2",
         CallBuilder32{}.pointer(tracking).value(mobile).value(time));
  auto t = *g2h<vr::abi::Tracking>(tracking);
  CHECK(t.status == 143);
  CHECK(t.head.time == doctest::Approx(time));
  CHECK(*c.memory.at<std::uint32_t>(256 + 360) == 0xfeedface);
  auto old_tracking = c.memory.address + 768;
  c.put(768 + 104, 0xfeedfaceu);
  c.call("libvrapi.so", "vrapi_GetPredictedTracking",
         CallBuilder32{}.pointer(old_tracking).value(mobile).value(time));
  CHECK(g2h<vr::abi32::Tracking>(old_tracking)->status == t.status);
  CHECK(*c.memory.at<std::uint32_t>(768 + 104) == 0xfeedface);
  auto input = c.memory.address + 1024;
  c.put(1024 + 104, 0xfeedfaceu);
  CHECK(std::int32_t(
            c.call("libvrapi.so", "vrapi_GetInputTrackingState",
                   CallBuilder32{}.value(mobile).value(1u).value(time).pointer(
                       input))) == 0);
  CHECK(*c.memory.at<std::uint32_t>(1024 + 104) == 0xfeedface);
  CHECK(g2h<vr::abi32::Tracking>(input)->head.time == doctest::Approx(time));
  vr::abi::RemoteCapabilities caps{};
  caps.header = {4, 1};
  c.put(1280, caps);
  c.put(1328, 0xfeedfaceu);
  CHECK(c.v("vrapi_GetInputDeviceCapabilities",
            {mobile, c.memory.address + 1280}) == 0);
  CHECK(*c.memory.at<std::uint32_t>(1328) == 0xfeedface);
  CHECK(c.v("vrapi_EnumerateInputDevices",
            {mobile, 0, c.memory.address + 1280}) == 0);
  CHECK(c.memory.at<vr::abi::InputHeader>(1280)->type == 4);
  CHECK(std::int32_t(c.v("vrapi_EnumerateInputDevices",
                         {mobile, 2, c.memory.address + 1280})) ==
        vr::abi::no_device);
  vr::abi::RemoteState remote{};
  remote.header.type = 4;
  c.put(1408, remote);
  c.put(1480, 0xfeedfaceu);
  c.v("vrapi_GetCurrentInputState", {mobile, 1, c.memory.address + 1408});
  CHECK(*c.memory.at<std::uint32_t>(1480) == 0xfeedface);
  CHECK(c.v("vrapi_SetClockLevels", {mobile, 2, 3}) == 0);
  CHECK(c.v("vrapi_SetPerfThread", {mobile, 0, 1}) == 0);
  CHECK(c.v("vrapi_SetExtraLatencyMode", {mobile, 0}) == 0);
  CHECK(c.v("vrapi_SetRemoteEmulation", {mobile, 0}) == 0);
  CHECK(std::int32_t(c.call("libvrapi.so", "vrapi_SetDisplayRefreshRate",
                            CallBuilder32{}.value(mobile).value(0.0f))) == 0);
  CHECK(std::int32_t(
            c.call("libvrapi.so", "vrapi_SetHapticVibrationSimple",
                   CallBuilder32{}.value(mobile).value(1u).value(0.0f))) == 0);
  vr::abi32::HapticBuffer h{time, 0, 1, {0, 0, 0}, 0};
  CHECK(std::int32_t(c.v("vrapi_SetHapticVibrationBuffer", {mobile, 1, 0})) == vr::abi::invalid);
  h.count = 257;
  CHECK(std::int32_t(c.v("vrapi_SetHapticVibrationBuffer", {mobile, 1, c.put(1536, h)})) == vr::abi::invalid);
  CHECK(std::int32_t(c.call("libvrapi.so", "vrapi_SetHapticVibrationSimple",
      CallBuilder32{}.value(0xdeadbeefu).value(1u).value(0.5f))) == vr::abi::invalid);
  h.count = 0;
  CHECK(c.v("vrapi_SetHapticVibrationBuffer", {mobile, 1, c.put(1536, h)}) ==
        0);
  auto pulses = ventureMockXrStats().haptic_pulses;
  CHECK(std::int32_t(c.call("libvrapi.so", "vrapi_SetHapticVibrationSimple",
      CallBuilder32{}.value(mobile).value(2u).value(96.3344497680664f))) == 0);
  CHECK(ventureMockXrStats().haptic_pulses == pulses + 1);
  CHECK(std::int32_t(c.call("libvrapi.so", "vrapi_SetHapticVibrationSimple",
      CallBuilder32{}.value(mobile).value(2u).value(-0.00035392120480537415f))) == 0);
  CHECK(ventureMockXrStats().haptic_pulses == pulses + 1);
  CHECK(std::int32_t(c.call("libvrapi.so", "vrapi_SetHapticVibrationSimple",
      CallBuilder32{}.value(mobile).value(2u).value(0.0f))) == 0);
  pulses = ventureMockXrStats().haptic_pulses;
  std::array<std::uint8_t, 128> samples;
  samples.fill(255);
  h.samples = std::uint32_t(c.put(1600, samples));
  h.count = samples.size();
  h.time = 0;
  CHECK(c.v("vrapi_SetHapticVibrationBuffer", {mobile, 1, c.put(1536, h)}) ==
        0);
  // A long envelope tolerates host scheduling delays; expired samples may
  // legitimately be skipped by the existing haptic scheduler.
  std::memset(c.memory.at<void>(1600), 0, samples.size());
  CHECK(ventureMockXrAwaitHaptics(pulses + 1, 2000));
  vr::abi32::Projection layer{};
  layer.header = {1, 2, {1, 1, 1, 1}, 1, 0, 0};
  layer.head = t.head;
  for (auto &e : layer.textures) {
    e.chain = ring;
    e.index = 1;
    e.rect = {0, 0, 1, 1};
    e.tan_angles.m[0][0] = e.tan_angles.m[1][1] = 0.5f;
    e.tan_angles.m[0][2] = e.tan_angles.m[1][2] = -0.5f;
    e.tan_angles.m[2][2] = -1;
  }
  auto lp = c.put(2048, layer);
  auto list = c.put(2400, std::uint32_t(lp));
  vr::abi32::Submit frame{
      0, 1, 1, time, 0x123456789abcdef0, 1, std::uint32_t(list)};
  REQUIRE(c.v("vrapi_SubmitFrame2", {mobile, c.put(2432, frame)}) == 0);
  CHECK(images.copies == 1);
  CHECK(images.last_texture == texture);
  CHECK(images.last_fence == frame.completion_fence);
  // Two pointers test the ILP32 list stride independently of layer decoding.
  auto projections = ventureMockXrStats().projection;
  c.put(2400,
        std::array<std::uint32_t, 2>{std::uint32_t(lp), std::uint32_t(lp)});
  frame.layer_count = 2;
  ++frame.frame_index;
  REQUIRE(c.v("vrapi_SubmitFrame2", {mobile, c.put(2432, frame)}) == 0);
  CHECK(ventureMockXrStats().projection == projections + 2);
  vr::abi32::LegacyFrame legacy{};
  legacy.type = 3;
  legacy.layer_count = 1;
  legacy.frame_index = 3;
  legacy.swap_interval = 1;
  legacy.layers[0].color_scale = 1;
  legacy.layers[0].src_blend = 1;
  for (int e = 0; e < 2; ++e) {
    auto &l = legacy.layers[0].textures[e];
    l.color = ring;
    l.index = 1;
    l.tan_angles = layer.textures[e].tan_angles;
    l.rect = {0, 0, 1, 1};
    l.head = t.head;
  }
  auto frames = ventureMockXrStats().ends;
  c.v("vrapi_SubmitFrame", {mobile, c.put(4096, legacy)});
  CHECK(ventureMockXrStats().ends == frames + 1);
  c.v("vrapi_DestroyTextureSwapChain", {ring});
  CHECK(std::int32_t(c.v("vrapi_GetTextureSwapChainLength", {ring})) ==
        vr::abi::invalid);
  CHECK(std::int32_t(c.v("vrapi_GetTextureSwapChainLength", {mobile})) ==
        vr::abi::invalid); // distinct Token32 types
  auto length = c.put(6400, 0xfeedfaceu);
  CHECK(std::int32_t(c.v("vrapi_GetInstanceExtensionsVulkan", {0, length})) ==
        vr::abi::unsupported);
  CHECK(*g2h<std::uint32_t>(length) == 0);
  CHECK(std::int32_t(c.v("vrapi_CreateSystemVulkan", {0})) ==
        vr::abi::unsupported);
  CHECK(c.v("vrapi_CreateAndroidSurfaceSwapChain", {64, 64}) == 0);
  c.v("vrapi_LeaveVrMode", {mobile});
  c.v("vrapi_Shutdown");
  c.words("libvrintegrationloader.so", "vrintegration_Shutdown");
  xr::install_xr_images(nullptr);
}

TEST_CASE(
    "A32 OpenSL four-byte tables, deep IID arrays and guest queue callback") {
  REQUIRE(audio::Backend::get().use_null_device());
  sl::register_library();
  auto *lib = Linker::get().open("libOpenSLES.so", kRtldNow, nullptr);
  REQUIRE(lib);
  Calls c;
  auto iid = [&](const char *name) {
    auto p = lib->find_symbol(name);
    REQUIRE(p);
    return *g2h<std::uint32_t>(p);
  };
  CHECK(g2h<sl::abi::InterfaceId>(iid("SL_IID_ENGINE"))->time_low ==
        0x8d97c260);
  auto call = [&](std::string_view name,
                  std::initializer_list<std::uint64_t> args) {
    return c.words("libOpenSLES.so", name, args);
  };
  auto output = c.memory.address;
  c.put(4, 0xfeedfaceu);
  auto ids = c.put(64, std::array<std::uint32_t, 2>{iid("SL_IID_ENGINE"),
                                                    iid("SL_IID_PLAY")});
  auto req = c.put(80, std::array<std::uint32_t, 2>{1, 0});
  REQUIRE(call("slCreateEngine", {output, 0, 0, 2, ids, req}) == 0);
  auto engine = *g2h<std::uint32_t>(output);
  REQUIRE(engine);
  CHECK(*c.memory.at<std::uint32_t>(4) == 0xfeedface);
  auto vt = *g2h<std::uint32_t>(engine);
  REQUIRE(vt);
  CHECK(AddressSpace::get().is_guest_executable(g2h<std::uint32_t>(vt)[6]));
  REQUIRE(call("Object.Realize", {engine, 0}) == 0);
  auto get = [&](std::uint32_t obj, const char *name) {
    REQUIRE(call("Object.GetInterface", {obj, iid(name), output}) == 0);
    CHECK(*c.memory.at<std::uint32_t>(4) == 0xfeedface);
    return *g2h<std::uint32_t>(output);
  };
  auto ei = get(engine, "SL_IID_ENGINE");
  REQUIRE(call("Engine.CreateOutputMix", {ei, output, 0, 0, 0}) == 0);
  auto mix = *g2h<std::uint32_t>(output);
  REQUIRE(call("Object.Realize", {mix, 0}) == 0);
  auto qloc = c.put(128, sl::abi::QueueLocator{0x800007bd, 2});
  auto pcm = c.put(160, sl::abi::Pcm{2, 2, 48000000, 16, 16, 3, 2});
  auto mloc = c.put(200, std::array<std::uint32_t, 2>{4, mix});
  auto src = c.put(224, std::array<std::uint32_t, 2>{std::uint32_t(qloc),
                                                     std::uint32_t(pcm)});
  auto sink = c.put(240, std::array<std::uint32_t, 2>{std::uint32_t(mloc), 0});
  c.put(64, std::array<std::uint32_t, 2>{iid("SL_IID_ANDROIDSIMPLEBUFFERQUEUE"),
                                         iid("SL_IID_VOLUME")});
  c.put(80, std::array<std::uint32_t, 2>{1, 1});
  REQUIRE(call("Engine.CreateAudioPlayer",
               {ei, output, src, sink, 2, ids, req}) == 0);
  auto player = *g2h<std::uint32_t>(output);
  REQUIRE(call("Object.Realize", {player, 0}) == 0);
  auto queue = get(player, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE"),
       play = get(player, "SL_IID_PLAY");
  test::LowMemory32 code(false, true);
  // ARM: ldr r2,[r1]; add r2,r2,#1; str r2,[r1]; bx lr.
  const std::uint32_t callback[]{0xe5912000, 0xe2822001, 0xe5812000,
                                 0xe12fff1e};
  std::memcpy(code.at<void>(), callback, sizeof(callback));
  auto counter = c.put(512, 0u);
  REQUIRE(call("BufferQueue.RegisterCallback",
               {queue, code.address, counter}) == 0);
  std::fill_n(c.memory.at<std::int16_t>(4096), 960, 8192);
  REQUIRE(call("BufferQueue.Enqueue", {queue, c.memory.address + 4096, 1920}) ==
          0);
  REQUIRE(call("Play.SetPlayState", {play, 3}) == 0);
  auto end = std::chrono::steady_clock::now() + venture::test::hang_timeout;
  // GetState holds the queue mutex; completion callback may run just after it.
  for (;;) {
    REQUIRE(call("BufferQueue.GetState", {queue, c.memory.address + 544}) == 0);
    if (c.memory.at<sl::abi::QueueState>(544)->index ||
        std::chrono::steady_clock::now() > end)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  call("Object.Destroy",
       {engine}); // joins worker before reading callback memory
  INFO("A32 queue completion index=", c.memory.at<sl::abi::QueueState>(544)->index,
       " callbacks=", *g2h<std::uint32_t>(counter));
  CHECK(*g2h<std::uint32_t>(counter) == 1);
  CHECK(*c.memory.at<std::uint32_t>(4) == 0xfeedface);
  sl::shutdown_library();
}

TEST_CASE(
    "A32 Platform retains 64-bit IDs, low handles and entitlement semantics") {
  namespace op = ovrplatform;
  auto provider = std::make_shared<op::test::FakeProvider>();
  op::Memory memory{[](std::size_t n) {
                      return AddressSpace::get().allocate(
                          n, kProtRead | kProtWrite, "[platform32-test]");
                    },
                    [](GuestAddr p) { AddressSpace::get().free(p); }};
  op::Session session(op::test::app(), provider, memory);
  auto lib = op::make_library(session);
  Calls c;
  auto app = op::test::app().app_id;
  std::memcpy(c.memory.at<void>(64), app.c_str(), app.size() + 1);
  CHECK(c.words("libovrplatformloader.so", "ovr_PlatformInitializeAndroid",
                {c.memory.address + 64, 1, 2}) == 0);
  auto entitlement =
      c.call("libovrplatformloader.so", "ovr_Entitlement_GetIsViewerEntitled",
             CallBuilder32{});
  REQUIRE(entitlement);
  auto entitled_message = op::test::wait_pop(session);
  CHECK(c.words("libovrplatformloader.so", "ovr_Message_IsError",
                {entitled_message}) == 0);
  CHECK(c.call("libovrplatformloader.so", "ovr_Message_GetRequestID",
               CallBuilder32{}.pointer(entitled_message)) == entitlement);
  c.words("libovrplatformloader.so", "ovr_FreeMessage", {entitled_message});
  auto request = c.call("libovrplatformloader.so", "ovr_User_GetLoggedInUser",
                        CallBuilder32{});
  REQUIRE(request);
  auto message = op::test::wait_pop(session);
  REQUIRE(message <= UINT32_MAX);
  CHECK(c.call("libovrplatformloader.so", "ovr_Message_GetRequestID",
               CallBuilder32{}.pointer(message)) == request);
  auto user =
      c.words("libovrplatformloader.so", "ovr_Message_GetUser", {message});
  REQUIRE(user);
  CHECK(c.call("libovrplatformloader.so", "ovr_User_GetID",
               CallBuilder32{}.value(user)) == 0xfedcba9876543210ull);
  auto alias =
      c.words("libovrplatformloader.so", "ovr_User_GetOculusID", {user});
  CHECK(std::string(g2h<char>(alias)) == "fixture-alias");
  c.words("libovrplatformloader.so", "ovr_FreeMessage", {message});
  CHECK(c.call("libovrplatformloader.so", "ovr_User_GetID",
               CallBuilder32{}.value(user)) == 0);
  c.words("libovrplatformloader.so", "ovr_Log_NewEvent", {0, 0, 0});
  const char id_text[] = "18364758544493064720"; // fedcba9876543210
  auto string = c.put(256, id_text);
  auto output = c.put(320, std::uint64_t(0));
  c.put(328, 0xfeedfaceu);
  CHECK(c.words("libovrplatformloader.so", "ovrID_FromString",
                {output, string}) == 1);
  CHECK(*g2h<std::uint64_t>(output) == 0xfedcba9876543210ull);
  CHECK(*c.memory.at<std::uint32_t>(328) == 0xfeedface);
  auto pair = c.memory.address + 384;
  c.put(408, 0xfeedfaceu);
  c.words("libovrplatformloader.so", "ovrKeyValuePair_makeString",
          {pair, string, string});
  CHECK(*g2h<std::uint32_t>(pair) == string);
  CHECK(*c.memory.at<std::uint32_t>(388) == 0);
  CHECK(*c.memory.at<std::uint32_t>(392) == string);
  CHECK(*c.memory.at<std::uint32_t>(408) == 0xfeedface);
  session.register_handler("ovr_Leaderboard_WriteEntry",
                           {[](op::Provider &, const op::RequestArgs &args,
                               std::stop_token) {
                              REQUIRE(args.values.size() == 5);
                              CHECK(std::get<std::string>(args.values[0]) ==
                                    "fixture-board");
                              CHECK(std::get<std::int64_t>(args.values[1]) == -7);
                              const auto &blob = std::get<op::Blob>(args.values[2]);
                              CHECK(blob == op::Blob{std::byte{1}, std::byte{2}});
                              CHECK(std::get<std::uint32_t>(args.values[3]) == 2);
                              CHECK(std::get<bool>(args.values[4]));
                              return op::Result{};
                            },
                            {}});
  const char board_name[] = "fixture-board";
  auto board = c.put(512, board_name);
  auto blob = c.put(544, std::array{std::byte{1}, std::byte{2}});
  auto write = c.call("libovrplatformloader.so", "ovr_Leaderboard_WriteEntry",
                      CallBuilder32{}.pointer(board).value(std::int64_t{-7})
                          .pointer(blob).value(std::uint32_t{2}).value(true));
  REQUIRE(write);
  auto write_message = op::test::wait_pop(session);
  CHECK(session.get(write_message, "ovr_Message_GetRequestID") == write);
  CHECK(session.get(write_message, "ovr_Message_IsError") == 0);
  session.free_message(write_message);
  session.shutdown();
}
