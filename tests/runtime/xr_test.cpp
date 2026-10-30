#define XR_USE_GRAPHICS_API_OPENGL_ES
#include "../async_wait.h"

using EGLenum = unsigned int;
#include "common/environment.h"
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include "xr/menu_chord.h"
#include "xr_menu_binding_fixture.h"
#include "../mock_openxr_runtime/control.h"
#include "core/thread_runtime.h"
#include "core/wait.h"
#include "core/process.h"
#include "gfx/gpu_device.h"
#include "hle/openxr/openxr.h"
#include "hle/openxr/state.h"
#include "xr/bindings.h"
#include "xr/clock.h"
#include "xr/session.h"
#include "xr/hands.h"
#include "xr/host_xr.h"
#include "xr/math.h"
#include "xr/haptic_sequence.h"
#include "platform/thread_priority.h"
#include "xr_images_fixture.h"
#include "xr_refresh_host.h"
#include "vulkan_test_support.h"
#include "hle/vulkan/internal.h"
#include "hle/vulkan/chain.h"
#include <cstring>
#include <future>
#include <limits>
#include <numbers>
#include <cstdio>
#include <doctest/doctest.h>

static_assert(sizeof(venture::xr::SamplerOpenGLES) == sizeof(XrSwapchainStateSamplerOpenGLESFB));
static_assert(offsetof(venture::xr::SamplerOpenGLES, borderColor) == offsetof(XrSwapchainStateSamplerOpenGLESFB, borderColor));

namespace {
struct XrForwardingFixture {
  venture::xr::HostXr& host = venture::xr::host_xr();
  void* original = host.get_instance_proc_addr;
  inline static XrForwardingFixture* active = nullptr;
  std::atomic<unsigned> boolean_resolves{0}, sync_resolves{0}, missing_resolves{0}, boolean_calls{0};
  bool second = false, missing = false, transient = false;
  XrInstance instance{};
  XrSession session = reinterpret_cast<XrSession>(0x3330);
  XrForwardingFixture() {
    active = this;
    host.get_instance_proc_addr = reinterpret_cast<void*>(resolve);
    venture::xr::invalidate_host_procedures();
  }
  ~XrForwardingFixture() {
    if (instance) venture::hle::openxr::guest::xrDestroyInstance(instance);
    host.get_instance_proc_addr = original;
    venture::xr::invalidate_host_procedures();
    active = nullptr;
  }
  XrResult create() {
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    auto result = venture::hle::openxr::guest::xrCreateInstance(&info, &instance);
    if (XR_SUCCEEDED(result)) venture::hle::openxr::track_handle(session, "XrSession", instance);
    return result;
  }
  static XrResult XRAPI_CALL first(XrSession, const XrActionStateGetInfo*, XrActionStateBoolean* out) {
    ++active->boolean_calls;
    out->currentState = XR_TRUE; out->changedSinceLastSync = XR_TRUE;
    out->isActive = XR_TRUE; out->lastChangeTime = 1234; return XR_SUCCESS;
  }
  static XrResult XRAPI_CALL next(XrSession, const XrActionStateGetInfo*, XrActionStateBoolean* out) {
    out->currentState = XR_FALSE; out->changedSinceLastSync = XR_FALSE;
    out->isActive = XR_TRUE; out->lastChangeTime = 4321; return XR_SUCCESS;
  }
  static XrResult XRAPI_CALL sync(XrSession, const XrActionsSyncInfo*) { return XR_SUCCESS; }
  static XrResult XRAPI_CALL resolve(XrInstance, const char* name, PFN_xrVoidFunction* out) {
    *out = nullptr;
    if (std::strcmp(name, "xrGetActionStateBoolean") == 0) {
      ++active->boolean_resolves;
      if (active->transient) { active->transient = false; return XR_ERROR_OUT_OF_MEMORY; }
      if (active->missing) return XR_ERROR_FUNCTION_UNSUPPORTED;
      *out = reinterpret_cast<PFN_xrVoidFunction>(active->second ? next : first);
    } else if (std::strcmp(name, "xrSyncActions") == 0) {
      ++active->sync_resolves;
      *out = reinterpret_cast<PFN_xrVoidFunction>(sync);
    } else { ++active->missing_resolves; return XR_ERROR_FUNCTION_UNSUPPORTED; }
    return XR_SUCCESS;
  }
};
struct HapticRootFallbackFixture {
  venture::xr::HostXr& host = venture::xr::host_xr();
  void* original = host.get_instance_proc_addr;
  PFN_xrCreateAction native_create{};
  inline static HapticRootFallbackFixture* active = nullptr;
  XrActionSet set{};
  std::vector<std::uint32_t> counts;
  HapticRootFallbackFixture() {
    PFN_xrVoidFunction function{};
    const auto resolve = reinterpret_cast<PFN_xrGetInstanceProcAddr>(original);
    if (XR_FAILED(resolve(host.instance, "xrCreateAction", &function)))
      throw std::runtime_error("Native action creation unavailable");
    native_create = reinterpret_cast<PFN_xrCreateAction>(function);
    active = this;
    host.get_instance_proc_addr = reinterpret_cast<void*>(lookup);
    venture::xr::invalidate_host_procedures();
  }
  ~HapticRootFallbackFixture() {
    host.get_instance_proc_addr = original;
    venture::xr::invalidate_host_procedures();
    if (set) ::xrDestroyActionSet(set);
    active = nullptr;
  }
  static XrResult XRAPI_CALL create(XrActionSet set, const XrActionCreateInfo* info, XrAction* out) {
    active->counts.push_back(info->countSubactionPaths);
    if (info->countSubactionPaths) { *out = XR_NULL_HANDLE; return XR_ERROR_PATH_UNSUPPORTED; }
    return active->native_create(set, info, out);
  }
  static XrResult XRAPI_CALL lookup(XrInstance instance, const char* name, PFN_xrVoidFunction* out) {
    if (std::strcmp(name, "xrCreateAction") == 0) {
      *out = reinterpret_cast<PFN_xrVoidFunction>(create);return XR_SUCCESS;
    }
    return reinterpret_cast<PFN_xrGetInstanceProcAddr>(active->original)(instance, name, out);
  }
};
}
TEST_CASE("Aggregate vibration falls back when native subaction roots are unsupported") {
  HapticRootFallbackFixture fixture;
  XrActionSetCreateInfo set{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strcpy(set.actionSetName, "root_fallback");std::strcpy(set.localizedActionSetName, "Root fallback");
  REQUIRE(::xrCreateActionSet(fixture.host.instance, &set, &fixture.set) == XR_SUCCESS);
  XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
  std::strcpy(info.actionName, "vibration");std::strcpy(info.localizedActionName, "Vibration");
  info.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
  XrAction action{};
  REQUIRE(venture::hle::openxr::guest::create_action_for_host(fixture.set, &info, &action) == XR_SUCCESS);
  REQUIRE(action != XR_NULL_HANDLE);
  CHECK(fixture.counts == std::vector<std::uint32_t>{4, 0});
  CHECK(info.countSubactionPaths == 0);CHECK(info.subactionPaths == nullptr);
  CHECK_FALSE(venture::hle::openxr::state().actions.contains(action));
  REQUIRE(::xrDestroyAction(action) == XR_SUCCESS);
  fixture.counts.clear();
  XrPath hand{};
  REQUIRE(::xrStringToPath(fixture.host.instance, "/user/hand/left", &hand) == XR_SUCCESS);
  info.countSubactionPaths = 1;info.subactionPaths = &hand;
  CHECK(venture::hle::openxr::guest::create_action_for_host(fixture.set, &info, &action) == XR_ERROR_PATH_UNSUPPORTED);
  CHECK(action == XR_NULL_HANDLE);
  CHECK(fixture.counts == std::vector<std::uint32_t>{1});
  fixture.counts.clear();info.countSubactionPaths = 0;info.subactionPaths = nullptr;
  info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
  REQUIRE(venture::hle::openxr::guest::create_action_for_host(fixture.set, &info, &action) == XR_SUCCESS);
  CHECK(fixture.counts == std::vector<std::uint32_t>{0});
  REQUIRE(::xrDestroyAction(action) == XR_SUCCESS);
}
TEST_CASE("XR forwarding slots retain results and refresh after instance recreation") {
  XrForwardingFixture fixture;
  REQUIRE(fixture.create() == XR_SUCCESS);
  XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
  XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
  for (unsigned i = 0; i < 100; ++i) {
    REQUIRE(venture::hle::openxr::guest::xrGetActionStateBoolean(fixture.session, &info, &value) == XR_SUCCESS);
    CHECK(value.currentState == XR_TRUE); CHECK(value.changedSinceLastSync == XR_TRUE);
    CHECK(value.isActive == XR_TRUE); CHECK(value.lastChangeTime == 1234);
  }
  CHECK(fixture.boolean_resolves == 1);
  value.type = XR_TYPE_ACTION_STATE_FLOAT;
  CHECK(venture::hle::openxr::guest::xrGetActionStateBoolean(fixture.session, &info, &value) == XR_ERROR_VALIDATION_FAILURE);
  CHECK(fixture.boolean_resolves == 1);
  value.type = XR_TYPE_ACTION_STATE_BOOLEAN;
  REQUIRE(venture::hle::openxr::guest::xrDestroyInstance(fixture.instance) == XR_SUCCESS);
  fixture.instance = {};
  CHECK(venture::hle::openxr::guest::xrGetActionStateBoolean(fixture.session, &info, &value) == XR_ERROR_HANDLE_INVALID);
  CHECK(fixture.boolean_resolves == 1);
  fixture.second = true;
  fixture.session = reinterpret_cast<XrSession>(0x4440);
  REQUIRE(fixture.create() == XR_SUCCESS);
  REQUIRE(venture::hle::openxr::guest::xrGetActionStateBoolean(fixture.session, &info, &value) == XR_SUCCESS);
  CHECK(value.currentState == XR_FALSE); CHECK(value.changedSinceLastSync == XR_FALSE);
  CHECK(value.isActive == XR_TRUE); CHECK(value.lastChangeTime == 4321);
  CHECK(fixture.boolean_resolves == 2);
}
TEST_CASE("XR action reads reuse the host answer until the next sync") {
  namespace xg = venture::hle::openxr::guest;
  XrForwardingFixture fixture;
  REQUIRE(fixture.create() == XR_SUCCESS);
  XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
  XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
  for (unsigned i = 0; i < 10; ++i) {
    value.lastChangeTime = 0;
    REQUIRE(xg::xrGetActionStateBoolean(fixture.session, &info, &value) == XR_SUCCESS);
    CHECK(value.currentState == XR_TRUE); CHECK(value.lastChangeTime == 1234);
  }
  CHECK(fixture.boolean_calls == 1);
  XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
  REQUIRE(xg::xrSyncActions(fixture.session, &sync) == XR_SUCCESS);
  REQUIRE(xg::xrGetActionStateBoolean(fixture.session, &info, &value) == XR_SUCCESS);
  CHECK(fixture.boolean_calls == 2);
  info.subactionPath = 7; // each subaction path is its own read
  REQUIRE(xg::xrGetActionStateBoolean(fixture.session, &info, &value) == XR_SUCCESS);
  REQUIRE(xg::xrGetActionStateBoolean(fixture.session, &info, &value) == XR_SUCCESS);
  CHECK(fixture.boolean_calls == 3);
}
TEST_CASE("XR forwarding caches missing procedures but retries transient lookup errors") {
  XrForwardingFixture fixture;
  fixture.missing = true;
  REQUIRE(fixture.create() == XR_SUCCESS);
  XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
  XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
  value.lastChangeTime = 99;
  for (unsigned i = 0; i < 100; ++i)
    CHECK(venture::hle::openxr::guest::xrGetActionStateBoolean(fixture.session, &info, &value) == XR_ERROR_FUNCTION_UNSUPPORTED);
  CHECK(value.lastChangeTime == 99); CHECK(fixture.boolean_resolves == 1);
  REQUIRE(venture::hle::openxr::guest::xrDestroyInstance(fixture.instance) == XR_SUCCESS);
  fixture.instance = {};
  fixture.missing = false; fixture.transient = true;
  REQUIRE(fixture.create() == XR_SUCCESS);
  CHECK(venture::hle::openxr::guest::xrGetActionStateBoolean(fixture.session, &info, &value) == XR_ERROR_FUNCTION_UNSUPPORTED);
  REQUIRE(venture::hle::openxr::guest::xrGetActionStateBoolean(fixture.session, &info, &value) == XR_SUCCESS);
  CHECK(value.lastChangeTime == 1234); CHECK(fixture.boolean_resolves == 3);
}
TEST_CASE("XR forwarding resolves one shared procedure across threads and distinguishes names") {
  XrForwardingFixture fixture;
  REQUIRE(fixture.create() == XR_SUCCESS);
  XrActionsSyncInfo info{XR_TYPE_ACTIONS_SYNC_INFO};
  std::vector<std::future<XrResult>> results;
  for (unsigned i = 0; i < 16; ++i)
    results.push_back(std::async(std::launch::async, [&] {
      return venture::xr::host_call<PFN_xrSyncActions, "xrSyncActions">(fixture.session, &info);
    }));
  for (auto& result : results) CHECK(result.get() == XR_SUCCESS);
  CHECK(fixture.sync_resolves == 1);
  for (unsigned i = 0; i < 100; ++i)
    CHECK(venture::xr::host_call<PFN_xrSyncActions, "xrMissing">(fixture.session, &info) == XR_ERROR_FUNCTION_UNSUPPORTED);
  CHECK(fixture.missing_resolves == 1); CHECK(fixture.sync_resolves == 1);
}
TEST_CASE("XR forwarding slots follow host instance and loader replacement") {
  XrForwardingFixture fixture;
  const auto original_instance = fixture.host.instance;
  struct RestoreInstance {
    venture::xr::HostXr& host;
    XrInstance instance;
    ~RestoreInstance() { host.instance = instance; }
  } restore{fixture.host, original_instance};
  XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
  XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
  REQUIRE((venture::xr::host_call<PFN_xrGetActionStateBoolean, "xrGetActionStateBoolean">(
      fixture.session, &info, &value)) == XR_SUCCESS);
  CHECK(value.currentState == XR_TRUE);
  fixture.second = true;
  fixture.host.instance = reinterpret_cast<XrInstance>(0x7770);
  REQUIRE((venture::xr::host_call<PFN_xrGetActionStateBoolean, "xrGetActionStateBoolean">(
      fixture.session, &info, &value)) == XR_SUCCESS);
  CHECK(value.currentState == XR_FALSE);
  CHECK(fixture.boolean_resolves == 2);
  fixture.host.get_instance_proc_addr = nullptr;
  CHECK((venture::xr::host_call<PFN_xrGetActionStateBoolean, "xrGetActionStateBoolean">(
      fixture.session, &info, &value)) == XR_ERROR_FUNCTION_UNSUPPORTED);
  fixture.host.get_instance_proc_addr = reinterpret_cast<void*>(XrForwardingFixture::resolve);
  REQUIRE((venture::xr::host_call<PFN_xrGetActionStateBoolean, "xrGetActionStateBoolean">(
      fixture.session, &info, &value)) == XR_SUCCESS);
  CHECK(fixture.boolean_resolves == 3);
}

TEST_CASE("Display timing distinguishes startup, measured modes and prediction jitter") {
  venture::xr::DisplayTiming timing;
  CHECK(timing.source == venture::xr::DisplayTiming::Source::unknown);
  CHECK(timing.rate() == doctest::Approx(90));
  CHECK_FALSE(timing.observe(6'944'444, venture::xr::DisplayTiming::Source::predicted));
  CHECK(timing.rate() == doctest::Approx(144));
  for (int i = 0; i < 100; ++i) {
    CHECK_FALSE(timing.observe(i % 2 ? 8'333'333 : 6'944'444,
                              venture::xr::DisplayTiming::Source::predicted));
  }
  CHECK_FALSE(timing.observe(6'944'444, venture::xr::DisplayTiming::Source::predicted));
  for (int i = 0; i < 29; ++i)
    CHECK_FALSE(timing.observe(8'333'333, venture::xr::DisplayTiming::Source::predicted));
  CHECK(timing.observe(8'333'333, venture::xr::DisplayTiming::Source::predicted));
  CHECK(timing.rate() == doctest::Approx(120));
  CHECK(timing.observe(6'944'444, venture::xr::DisplayTiming::Source::host));
  for (int i = 0; i < 40; ++i)
    CHECK_FALSE(timing.observe(11'111'111, venture::xr::DisplayTiming::Source::predicted));
  CHECK(timing.rate() == doctest::Approx(144));
  timing.restart();
  CHECK(timing.source == venture::xr::DisplayTiming::Source::unknown);
  CHECK(timing.rate() == doctest::Approx(144));
  CHECK_FALSE(timing.observe(8'333'333, venture::xr::DisplayTiming::Source::predicted));
  CHECK(timing.rate() == doctest::Approx(120));
}

TEST_CASE("A remembered host period stands in until the host reports one") {
  using Source = venture::xr::DisplayTiming::Source;
  venture::xr::DisplayTiming timing;
  CHECK(timing.remember(6'944'444));
  CHECK(timing.source == Source::remembered);
  CHECK(timing.rate() == doctest::Approx(144));
  CHECK_FALSE(timing.remember(8'333'333));  // only ever the first knowledge
  timing.restart();  // a session created before the first frame keeps it
  CHECK(timing.source == Source::remembered);
  // The host's first predicted period replaces it at once, without the 30-frame mode filter.
  CHECK_FALSE(timing.observe(8'333'333, Source::predicted));
  CHECK(timing.source == Source::predicted);
  CHECK(timing.rate() == doctest::Approx(120));
  CHECK_FALSE(timing.remember(6'944'444));
}

TEST_CASE("Host mode events update shared Android timing before guest filtering") {
  venture::xr::Session session;
  session.display_timing.reset();
  XrEventDataDisplayRefreshRateChangedFB event{
      XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB, nullptr, 90, 144};
  XrEventDataBuffer buffer{};
  std::memcpy(&buffer, &event, sizeof(event));
  session.observe_host_event(buffer);
  CHECK(session.display_timing.source == venture::xr::DisplayTiming::Source::host);
  CHECK(session.display_timing.rate() == doctest::Approx(144));
  // Prediction cadence cannot override the user's actual host mode.
  session.observe_display_period(11'111'111, venture::xr::DisplayTiming::Source::predicted);
  CHECK(session.display_timing.rate() == doctest::Approx(144));
  session.display_timing.reset();
}

namespace ox = venture::hle::openxr;
namespace xg = ox::guest;

TEST_CASE("PCM haptics retain signed carrier energy and report consumed input samples") {
  std::array<float, 8> carrier{1, -1, 1, -1, 0, 0, 0, 0};
  uint32_t consumed = 99;
  auto pulses = venture::xr::pcm_envelope(carrier, 1000, 1, consumed);
  REQUIRE(pulses.size() == 1);
  CHECK(consumed == 4);
  CHECK(pulses[0].amplitude == doctest::Approx(1));
  CHECK(pulses[0].duration == 4'000'000);
  pulses = venture::xr::pcm_envelope(carrier, 1000, 4000, consumed);
  REQUIRE(pulses.size() == 2);
  CHECK(consumed == 8);
  CHECK(pulses[1].amplitude == 0);
  pulses = venture::xr::pcm_envelope(carrier, 1000, 0, consumed);
  CHECK(pulses.empty());
  CHECK(consumed == 0);
  std::array<float, 4> overshoot{1.031019f, -2.f, 2.f, -1.5f};
  pulses = venture::xr::pcm_envelope(overshoot, 2000, 4000, consumed);
  REQUIRE(pulses.size() == 1);
  CHECK(consumed == 4);
  CHECK(pulses[0].amplitude == 1);
}

TEST_CASE("Registry controller mappings preserve haptic outputs on either hand") {
  for (auto profile : {"/interaction_profiles/oculus/touch_controller",
                       "/interaction_profiles/valve/index_controller",
                       "/interaction_profiles/htc/vive_controller",
                       "/interaction_profiles/khr/simple_controller"})
    for (auto source : {"/user/hand/left/output/haptic", "/user/hand/right/output/haptic"})
      CHECK(venture::xr::touch_binding(source, profile) == std::string(source));
  CHECK(venture::xr::touch_binding("/user/hand/left/input/thumbstick/x",
      "/interaction_profiles/htc/vive_controller") == "/user/hand/left/input/trackpad/x");
  CHECK_FALSE(venture::xr::touch_binding("/user/hand/left/input/x/click",
      "/interaction_profiles/htc/vive_controller"));
}

TEST_CASE("XR PCM and envelope haptics bind both hands, append, replace and stop") {
  venture::test::XrImageFixture interop;
  venture::xr::install_xr_images(&interop);
  const char *extensions[]{"XR_KHR_opengl_es_enable", "XR_FB_haptic_pcm",
                           "XR_FB_haptic_amplitude_envelope"};
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  create.enabledExtensionCount = 3;
  create.enabledExtensionNames = extensions;
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&create, &instance) == XR_SUCCESS);
  xg::GlesBinding graphics{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR, nullptr, 1, 2, 3};
  XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO, &graphics, 0, 1};
  XrSession session{};
  REQUIRE(xg::xrCreateSession(instance, &ci, &session) == XR_SUCCESS);
  XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strcpy(set_info.actionSetName, "haptic_test");
  std::strcpy(set_info.localizedActionSetName, "Haptic test");
  XrActionSet set{};
  REQUIRE(xg::xrCreateActionSet(instance, &set_info, &set) == XR_SUCCESS);
  XrPath hands[2]{}, outputs[2]{}, profile{};
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/left", &hands[0]) == XR_SUCCESS);
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/right", &hands[1]) == XR_SUCCESS);
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/left/output/haptic", &outputs[0]) == XR_SUCCESS);
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/right/output/haptic", &outputs[1]) == XR_SUCCESS);
  REQUIRE(xg::xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &profile) == XR_SUCCESS);
  XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
  ai.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
  ai.countSubactionPaths = 2;
  ai.subactionPaths = hands;
  std::strcpy(ai.actionName, "vibration");
  std::strcpy(ai.localizedActionName, "Vibration");
  SUBCASE("Explicit vibration subactions retain their contract") {}
  SUBCASE("Aggregate mobile vibration supports later per-hand targeting") {
    ai.countSubactionPaths = 0;
    ai.subactionPaths = nullptr;
  }
  XrAction action{}, unbound{};
  REQUIRE(xg::xrCreateAction(set, &ai, &action) == XR_SUCCESS);
  const auto& declaration = venture::hle::openxr::state().actions.at(action);
  CHECK(declaration.subactions.size() == ai.countSubactionPaths);
  CHECK(declaration.haptic_subactions().size() == (ai.countSubactionPaths ? 2 : 4));
  if (!ai.countSubactionPaths) {
    CHECK(ai.subactionPaths == nullptr);
    for (const char* name : {"/user/head", "/user/gamepad"}) {
      XrPath root{};
      REQUIRE(xg::xrStringToPath(instance, name, &root) == XR_SUCCESS);
      const auto paths = declaration.haptic_subactions();
      CHECK(std::find(paths.begin(), paths.end(), root) != paths.end());
    }
  }
  std::strcpy(ai.actionName, "unbound");
  REQUIRE(xg::xrCreateAction(set, &ai, &unbound) == XR_SUCCESS);
  XrActionSuggestedBinding bindings[]{{action, outputs[0]}, {action, outputs[1]}};
  XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
      nullptr, profile, 2, bindings};
  REQUIRE(xg::xrSuggestInteractionProfileBindings(instance, &suggestion) == XR_SUCCESS);
  XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &set};
  REQUIRE(xg::xrAttachSessionActionSets(session, &attach) == XR_SUCCESS);
  auto poll = [&] {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xg::xrPollEvent(instance, &event) == XR_SUCCESS) event = {XR_TYPE_EVENT_DATA_BUFFER};
  };
  poll();
  XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
  REQUIRE(xg::xrBeginSession(session, &begin) == XR_SUCCESS);
  poll();
  MockXrScript script{};
  ventureMockXrSetScript(&script);
  XrHapticActionInfo request{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, action, hands[0]};
  XrDevicePcmSampleRateGetInfoFB rate{XR_TYPE_DEVICE_PCM_SAMPLE_RATE_STATE_FB};
  CHECK(xg::xrGetDeviceSampleRateFB(session, &request, &rate) == XR_SUCCESS);
  CHECK(rate.sampleRate == 250);
  auto before = ventureMockXrStats().haptic_pulses;
  XrHapticVibration zero{XR_TYPE_HAPTIC_VIBRATION, nullptr, 10'000'000, 0, 0};
  CHECK(xg::xrApplyHapticFeedback(session, nullptr, reinterpret_cast<XrHapticBaseHeader *>(&zero)) == XR_ERROR_VALIDATION_FAILURE);
  CHECK(xg::xrApplyHapticFeedback(session, &request, nullptr) == XR_ERROR_VALIDATION_FAILURE);
  CHECK(xg::xrStopHapticFeedback(session, nullptr) == XR_ERROR_VALIDATION_FAILURE);
  CHECK(xg::xrGetDeviceSampleRateFB(session, &request, nullptr) == XR_ERROR_VALIDATION_FAILURE);
  CHECK(xg::xrGetDeviceSampleRateFB(session, nullptr, &rate) == XR_ERROR_VALIDATION_FAILURE);
  CHECK(::xrApplyHapticFeedback(session, nullptr, reinterpret_cast<XrHapticBaseHeader *>(&zero)) == XR_ERROR_VALIDATION_FAILURE);
  auto unsupported = request;
  unsupported.subactionPath = outputs[0]; // A valid path not declared as a subaction.
  CHECK(xg::xrApplyHapticFeedback(session, &unsupported, reinterpret_cast<XrHapticBaseHeader *>(&zero)) == XR_ERROR_PATH_UNSUPPORTED);
  CHECK(xg::xrStopHapticFeedback(session, &unsupported) == XR_ERROR_PATH_UNSUPPORTED);
  zero.amplitude = std::numeric_limits<float>::quiet_NaN();
  CHECK(xg::xrApplyHapticFeedback(session, &request, reinterpret_cast<XrHapticBaseHeader *>(&zero)) == XR_ERROR_VALIDATION_FAILURE);
  CHECK(ventureMockXrStats().haptic_pulses == before);
  zero.amplitude = 0;
  CHECK(xg::xrApplyHapticFeedback(session, &request, reinterpret_cast<XrHapticBaseHeader *>(&zero)) == XR_SUCCESS);
  CHECK(ventureMockXrStats().haptic_pulses == before);
  XrBaseInStructure unknown{static_cast<XrStructureType>(0x7ffffffe), nullptr};
  request.next = &unknown;
  zero.next = &unknown;
  CHECK(xg::xrApplyHapticFeedback(session, &request, reinterpret_cast<XrHapticBaseHeader *>(&zero)) == XR_SUCCESS);
  CHECK(xg::xrStopHapticFeedback(session, &request) == XR_SUCCESS);
  rate.next = &unknown;
  CHECK(xg::xrGetDeviceSampleRateFB(session, &request, &rate) == XR_SUCCESS);
  CHECK(rate.sampleRate == 250);
  CHECK(request.next == &unknown);
  CHECK(zero.next == &unknown);
  unknown.next = &unknown;
  CHECK(xg::xrApplyHapticFeedback(session, &request, reinterpret_cast<XrHapticBaseHeader *>(&zero)) == XR_ERROR_VALIDATION_FAILURE);
  unknown.next = nullptr;
  request.next = zero.next = rate.next = nullptr;
  zero.amplitude = 1;
  zero.duration = 0; // A valid no-op must not count as controller output.
  CHECK(xg::xrApplyHapticFeedback(session, &request, reinterpret_cast<XrHapticBaseHeader *>(&zero)) == XR_SUCCESS);
  CHECK(ventureMockXrStats().haptic_pulses == before);
  zero.duration = 10'000'000;
  zero.amplitude = 2.5f; // Captured finite overdrive from a grip interaction.
  CHECK(xg::xrApplyHapticFeedback(session, &request, reinterpret_cast<XrHapticBaseHeader *>(&zero)) == XR_SUCCESS);
  CHECK(ventureMockXrStats().haptic_pulses == before + 1);
  CHECK(zero.amplitude == 2.5f);
  ++before;
  auto wildcard = request;
  wildcard.subactionPath = XR_NULL_PATH;
  const auto stops = ventureMockXrStats().haptic_stops;
  REQUIRE(xg::xrApplyHapticFeedback(session, &wildcard, reinterpret_cast<XrHapticBaseHeader *>(&zero)) == XR_SUCCESS);
  CHECK(ventureMockXrStats().haptic_pulses == before + 2);
  REQUIRE(xg::xrStopHapticFeedback(session, &wildcard) == XR_SUCCESS);
  CHECK(ventureMockXrStats().haptic_stops == stops + 1);
  before += 2;
  request.action = unbound;
  CHECK(xg::xrGetDeviceSampleRateFB(session, &request, &rate) == XR_SUCCESS);
  CHECK(rate.sampleRate == 0);
  zero.amplitude = 1;
  CHECK(xg::xrApplyHapticFeedback(session, &request, reinterpret_cast<XrHapticBaseHeader *>(&zero)) == XR_SUCCESS);
  CHECK(ventureMockXrStats().haptic_pulses == before);
  request.action = action;
  float first[]{-0.5f};
  uint32_t consumed = 99;
  XrHapticPcmVibrationFB pcm{XR_TYPE_HAPTIC_PCM_VIBRATION_FB, nullptr, 1, first, 2, XR_FALSE, &consumed};
  pcm.next = &unknown;
  CHECK(xg::xrApplyHapticFeedback(session, &request, reinterpret_cast<XrHapticBaseHeader *>(&pcm)) == XR_SUCCESS);
  CHECK(pcm.next == &unknown);
  pcm.next = nullptr;
  CHECK(consumed == 1);
  REQUIRE(ventureMockXrAwaitHaptics(before + 1, 1000));
  pcm.append = XR_TRUE;
  CHECK(xg::xrApplyHapticFeedback(session, &request, reinterpret_cast<XrHapticBaseHeader *>(&pcm)) == XR_SUCCESS);
  CHECK(consumed == 1);
  CHECK_FALSE(ventureMockXrAwaitHaptics(before + 2, 30));
  pcm.append = XR_FALSE;
  CHECK(xg::xrApplyHapticFeedback(session, &request, reinterpret_cast<XrHapticBaseHeader *>(&pcm)) == XR_SUCCESS);
  REQUIRE(ventureMockXrAwaitHaptics(before + 2, 1000));
  REQUIRE(xg::xrStopHapticFeedback(session, &request) == XR_SUCCESS);
  CHECK_FALSE(ventureMockXrAwaitHaptics(before + 3, 550));
  request.subactionPath = hands[1];
  float amplitudes[]{0.2f, 0.8f};
  XrHapticAmplitudeEnvelopeVibrationFB env{XR_TYPE_HAPTIC_AMPLITUDE_ENVELOPE_VIBRATION_FB,
      nullptr, 16'000'000, 2, amplitudes};
  CHECK(xg::xrApplyHapticFeedback(session, &request, reinterpret_cast<XrHapticBaseHeader *>(&env)) == XR_SUCCESS);
  REQUIRE(ventureMockXrAwaitHaptics(before + 4, 1000));
  REQUIRE(xg::xrStopHapticFeedback(session, &request) == XR_SUCCESS);
  // Appending PCM must replace an overlapping non-PCM all-hands effect.
  auto overlap = ventureMockXrStats().haptic_pulses;
  request.subactionPath = XR_NULL_PATH;
  env.duration = 1'000'000'000;
  REQUIRE(xg::xrApplyHapticFeedback(session, &request, reinterpret_cast<XrHapticBaseHeader *>(&env)) == XR_SUCCESS);
  REQUIRE(ventureMockXrAwaitHaptics(overlap + 2, 1000));
  request.subactionPath = hands[1];
  pcm.append = XR_TRUE;
  REQUIRE(xg::xrApplyHapticFeedback(session, &request, reinterpret_cast<XrHapticBaseHeader *>(&pcm)) == XR_SUCCESS);
  CHECK(consumed == 1);
  REQUIRE(ventureMockXrAwaitHaptics(overlap + 3, 1000));
  REQUIRE(xg::xrStopHapticFeedback(session, &request) == XR_SUCCESS);
  CHECK_FALSE(ventureMockXrAwaitHaptics(overlap + 4, 550));
  auto dense = ventureMockXrStats().haptic_pulses;
  env.duration = 1; // Dense envelopes are resampled, not rejected for rounding.
  REQUIRE(xg::xrApplyHapticFeedback(session, &request, reinterpret_cast<XrHapticBaseHeader *>(&env)) == XR_SUCCESS);
  REQUIRE(ventureMockXrAwaitHaptics(dense + 1, 1000));
  std::array<float, 180> mixed_clip;
  mixed_clip.fill(1.031019f);
  pcm.buffer = mixed_clip.data(); pcm.bufferSize = mixed_clip.size();
  pcm.sampleRate = 2000; pcm.append = XR_FALSE;
  REQUIRE(xg::xrApplyHapticFeedback(session, &request, reinterpret_cast<XrHapticBaseHeader *>(&pcm)) == XR_SUCCESS);
  CHECK(consumed == 180);
  REQUIRE(ventureMockXrAwaitHaptics(dense + 2, 1000));
  CHECK(xg::xrStopHapticFeedback(session, &request) == XR_SUCCESS);
  pcm.sampleRate = 0;
  consumed = 99;
  CHECK(xg::xrApplyHapticFeedback(session, &request, reinterpret_cast<XrHapticBaseHeader *>(&pcm)) == XR_ERROR_VALIDATION_FAILURE);
  CHECK(consumed == 0);
  SUBCASE("Guest request retains STOPPING, IDLE, EXITING") {
    REQUIRE(xg::xrRequestExitSession(session) == XR_SUCCESS);
    poll();
    CHECK(venture::xr::session().state == XR_SESSION_STATE_STOPPING);
    CHECK(xg::xrEndSession(session) == XR_SUCCESS);
    poll();
    CHECK(venture::xr::session().state == XR_SESSION_STATE_EXITING);
  }
  SUBCASE("Host Activity quit stops at IDLE without guest EXITING") {
    REQUIRE(venture::xr::session().request_host_stop() == XR_SUCCESS);
    REQUIRE(venture::xr::session().request_host_stop() == XR_SUCCESS);
    poll();
    CHECK(venture::xr::session().state == XR_SESSION_STATE_STOPPING);
    CHECK(xg::xrEndSession(session) == XR_SUCCESS);
    poll();
    CHECK(venture::xr::session().state == XR_SESSION_STATE_IDLE);
  }
  SUBCASE("Guest exit after host STOPPING still delivers EXITING") {
    REQUIRE(venture::xr::session().request_host_stop() == XR_SUCCESS);
    poll();
    REQUIRE(xg::xrRequestExitSession(session) == XR_SUCCESS);
    CHECK(xg::xrEndSession(session) == XR_SUCCESS);
    poll();
    CHECK(venture::xr::session().state == XR_SESSION_STATE_EXITING);
  }
  CHECK(xg::xrDestroySession(session) == XR_SUCCESS);
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  venture::xr::install_xr_images(nullptr);
}

// A host offering XR_FB_haptic_pcm that answers a device sample rate of 0 (Monado, for
// controllers whose driver reports no output limits) cannot play PCM, but core vibration
// still reaches the controller: the title sees the emulation rate and its PCM is played as
// core vibration instead of being forwarded to a device that drops it.
TEST_CASE("XR PCM haptics use core vibration when the host's PCM sample rate is 0") {
  struct PcmHost {
    bool enabled = venture::xr::host_xr().enabled_extensions.contains("XR_FB_haptic_pcm");
    PcmHost() { set("0"); venture::xr::host_xr().enabled_extensions.insert("XR_FB_haptic_pcm"); }
    ~PcmHost() { set(""); if (!enabled) venture::xr::host_xr().enabled_extensions.erase("XR_FB_haptic_pcm"); }
    static void set(const char* value) {
#ifdef _WIN32
      _putenv_s("VENTURE_MOCK_PCM_RATE", value);
#else
      if (*value) setenv("VENTURE_MOCK_PCM_RATE", value, 1); else unsetenv("VENTURE_MOCK_PCM_RATE");
#endif
      venture::environment::refresh();
      ventureMockXrRefreshEnvironment();
    }
  } host;
  venture::test::XrImageFixture interop;
  venture::xr::install_xr_images(&interop);
  const char *extensions[]{"XR_KHR_opengl_es_enable", "XR_FB_haptic_pcm"};
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  create.enabledExtensionCount = 2;
  create.enabledExtensionNames = extensions;
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&create, &instance) == XR_SUCCESS);
  xg::GlesBinding graphics{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR, nullptr, 1, 2, 3};
  XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO, &graphics, 0, 1};
  XrSession session{};
  REQUIRE(xg::xrCreateSession(instance, &ci, &session) == XR_SUCCESS);
  XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strcpy(set_info.actionSetName, "pcm_host_test");
  std::strcpy(set_info.localizedActionSetName, "PCM host test");
  XrActionSet set{};
  REQUIRE(xg::xrCreateActionSet(instance, &set_info, &set) == XR_SUCCESS);
  XrPath hands[2]{}, outputs[2]{}, profile{};
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/left", &hands[0]) == XR_SUCCESS);
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/right", &hands[1]) == XR_SUCCESS);
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/left/output/haptic", &outputs[0]) == XR_SUCCESS);
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/right/output/haptic", &outputs[1]) == XR_SUCCESS);
  REQUIRE(xg::xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &profile) == XR_SUCCESS);
  XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
  ai.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
  ai.countSubactionPaths = 2;
  ai.subactionPaths = hands;
  std::strcpy(ai.actionName, "vibration");
  std::strcpy(ai.localizedActionName, "Vibration");
  XrAction action{}, unbound{};
  REQUIRE(xg::xrCreateAction(set, &ai, &action) == XR_SUCCESS);
  std::strcpy(ai.actionName, "unbound");
  REQUIRE(xg::xrCreateAction(set, &ai, &unbound) == XR_SUCCESS);
  XrActionSuggestedBinding bindings[]{{action, outputs[0]}, {action, outputs[1]}};
  XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
      nullptr, profile, 2, bindings};
  REQUIRE(xg::xrSuggestInteractionProfileBindings(instance, &suggestion) == XR_SUCCESS);
  XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &set};
  REQUIRE(xg::xrAttachSessionActionSets(session, &attach) == XR_SUCCESS);
  auto poll = [&] {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xg::xrPollEvent(instance, &event) == XR_SUCCESS) event = {XR_TYPE_EVENT_DATA_BUFFER};
  };
  poll();
  XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
  REQUIRE(xg::xrBeginSession(session, &begin) == XR_SUCCESS);
  poll();
  MockXrScript script{};
  ventureMockXrSetScript(&script);
  XrHapticActionInfo request{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, action, hands[1]};
  XrDevicePcmSampleRateGetInfoFB rate{XR_TYPE_DEVICE_PCM_SAMPLE_RATE_STATE_FB};
  REQUIRE(xg::xrGetDeviceSampleRateFB(session, &request, &rate) == XR_SUCCESS);
  CHECK(rate.sampleRate == venture::xr::haptic_emulation_rate);
  auto before = ventureMockXrStats().haptic_pulses;
  float samples[]{0.8f, -0.8f, 0.8f, -0.8f};
  uint32_t consumed = 99;
  XrHapticPcmVibrationFB pcm{XR_TYPE_HAPTIC_PCM_VIBRATION_FB, nullptr, 4, samples, rate.sampleRate, XR_FALSE, &consumed};
  CHECK(xg::xrApplyHapticFeedback(session, &request, reinterpret_cast<XrHapticBaseHeader *>(&pcm)) == XR_SUCCESS);
  CHECK(consumed == 4);
  REQUIRE(ventureMockXrAwaitHaptics(before + 1, 1000));
  CHECK(xg::xrStopHapticFeedback(session, &request) == XR_SUCCESS);
  // A title that sends PCM without asking first is routed the same way.
  request.subactionPath = hands[0];
  before = ventureMockXrStats().haptic_pulses;
  CHECK(xg::xrApplyHapticFeedback(session, &request, reinterpret_cast<XrHapticBaseHeader *>(&pcm)) == XR_SUCCESS);
  REQUIRE(ventureMockXrAwaitHaptics(before + 1, 1000));
  CHECK(xg::xrStopHapticFeedback(session, &request) == XR_SUCCESS);
  // An action bound to nothing still reports no rate.
  request.action = unbound;
  REQUIRE(xg::xrGetDeviceSampleRateFB(session, &request, &rate) == XR_SUCCESS);
  CHECK(rate.sampleRate == 0);
  CHECK(xg::xrDestroySession(session) == XR_SUCCESS);
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  venture::xr::install_xr_images(nullptr);
}

TEST_CASE("XR layer sizing uses runtime pixels, honors allocation limits and rejects stale time") {
  venture::test::XrImageFixture interop;
  venture::xr::install_xr_images(&interop);
  const char* extensions[]{"XR_KHR_opengl_es_enable", "XR_META_recommended_layer_resolution",
                           "XR_KHR_composition_layer_color_scale_bias"};
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  create.enabledExtensionNames = extensions;
  create.enabledExtensionCount = 3;
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&create, &instance) == XR_SUCCESS);
  PFN_xrVoidFunction proc{};
  REQUIRE(xg::xrGetInstanceProcAddr(instance, "xrGetRecommendedLayerResolutionMETA", &proc) == XR_SUCCESS);
  REQUIRE(proc != nullptr);
  xg::GlesBinding binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR, nullptr, 1, 2, 3};
  XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO, &binding, 0, 1};
  XrSession session{};
  REQUIRE(xg::xrCreateSession(instance, &session_info, &session) == XR_SUCCESS);
  XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr,
      XR_REFERENCE_SPACE_TYPE_LOCAL, {{0, 0, 0, 1}, {0, 0, 0}}};
  XrSpace space{};
  REQUIRE(xg::xrCreateReferenceSpace(session, &space_info, &space) == XR_SUCCESS);
  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  quad.space = space;
  quad.pose.orientation.w = 1;
  quad.pose.position.z = -2;
  quad.size = {2, 1};
  XrRecommendedLayerResolutionGetInfoMETA info{XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_GET_INFO_META,
      nullptr, reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad), 1};
  XrRecommendedLayerResolutionMETA out{XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_META};
  // OVRPlugin chains XrCompositionLayerImageLayoutFB without enabling its
  // extension; runtimes ignore unenabled next nodes (OpenXR 2.9.7).
  XrCompositionLayerImageLayoutFB layout{XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB};
  quad.next = &layout;
  REQUIRE(xg::xrGetRecommendedLayerResolutionMETA(session, &info, &out) == XR_SUCCESS);
  quad.next = nullptr;
  CHECK(out.isValid == XR_TRUE);
  CHECK(out.recommendedImageDimensions.width > 32);
  CHECK(out.recommendedImageDimensions.height > 16);
  CHECK(std::abs(out.recommendedImageDimensions.width - 2 * out.recommendedImageDimensions.height) <= 1);
  const auto allocation = out.recommendedImageDimensions;
  ox::state().enabled.erase("XR_META_recommended_layer_resolution");
  CHECK(xg::xrGetInstanceProcAddr(instance, "xrGetRecommendedLayerResolutionMETA", &proc) == XR_ERROR_FUNCTION_UNSUPPORTED);
  CHECK(xg::xrGetRecommendedLayerResolutionMETA(session, &info, &out) == XR_ERROR_FUNCTION_UNSUPPORTED);
  ox::state().enabled.insert("XR_META_recommended_layer_resolution");
  XrSwapchainCreateInfo image_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  image_info.format = 0x8058;
  image_info.width = 32; image_info.height = 16;
  image_info.arraySize = image_info.sampleCount = image_info.mipCount = image_info.faceCount = 1;
  image_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  XrSwapchain chain{};
  REQUIRE(xg::xrCreateSwapchain(session, &image_info, &chain) == XR_SUCCESS);
  quad.subImage = {chain, {{0, 0}, {32, 16}}, 0};
  REQUIRE(xg::xrGetRecommendedLayerResolutionMETA(session, &info, &out) == XR_SUCCESS);
  CHECK(out.recommendedImageDimensions.width <= 32);
  CHECK(out.recommendedImageDimensions.height <= 16);
  XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                           {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  views[0].subImage.swapchain = chain;
  XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION, nullptr, 0, space, 2, views};
  info.layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
  // Any null required swapchain removes all allocation limits, even if another
  // eye already has a smaller allocation (extension's explicit exception).
  // OVRPlugin's projection carries color scale/bias, whose registry
  // structextends is XrCompositionLayerBaseHeader, then the unenabled layout.
  XrCompositionLayerColorScaleBiasKHR color{XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR, &layout};
  projection.next = &color;
  REQUIRE(xg::xrGetRecommendedLayerResolutionMETA(session, &info, &out) == XR_SUCCESS);
  projection.next = nullptr;
  CHECK(out.recommendedImageDimensions.width > 32);
  views[1].subImage.swapchain = chain;
  REQUIRE(xg::xrGetRecommendedLayerResolutionMETA(session, &info, &out) == XR_SUCCESS);
  CHECK(out.recommendedImageDimensions.width <= 32);
  CHECK(out.recommendedImageDimensions.height <= 16);
  XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  while (xg::xrPollEvent(instance, &event) == XR_SUCCESS) event = {XR_TYPE_EVENT_DATA_BUFFER};
  REQUIRE(xg::xrBeginSession(session, &begin) == XR_SUCCESS);
  XrFrameState frame{XR_TYPE_FRAME_STATE};
  REQUIRE(xg::xrWaitFrame(session, nullptr, &frame) == XR_SUCCESS);
  CHECK(xg::xrGetRecommendedLayerResolutionMETA(session, &info, &out) == XR_ERROR_TIME_INVALID);
  info.predictedDisplayTime = frame.predictedDisplayTime;
  info.layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
  quad.subImage.swapchain = XR_NULL_HANDLE;
  REQUIRE(xg::xrGetRecommendedLayerResolutionMETA(session, &info, &out) == XR_SUCCESS);
  CHECK(out.recommendedImageDimensions.width == allocation.width);
  // The allocation-sizing exception permits null swapchains, not null spaces.
  // Reject bad handles without overwriting the caller's existing recommendation.
  quad.space = XR_NULL_HANDLE;
  CHECK(xg::xrGetRecommendedLayerResolutionMETA(session, &info, &out) == XR_ERROR_HANDLE_INVALID);
  CHECK(xg::xrGetRecommendedLayerResolutionMETA(session, &info, &out) == XR_ERROR_HANDLE_INVALID);
  CHECK(xg::xrGetRecommendedLayerResolutionMETA(XR_NULL_HANDLE, &info, &out) == XR_ERROR_HANDLE_INVALID);
  CHECK(out.recommendedImageDimensions.width == allocation.width);
  quad.space = space;
  REQUIRE(xg::xrDestroySwapchain(chain) == XR_SUCCESS);
  quad.subImage.swapchain = chain;
  CHECK(xg::xrGetRecommendedLayerResolutionMETA(session, &info, &out) == XR_ERROR_HANDLE_INVALID);
  CHECK(out.recommendedImageDimensions.width == allocation.width);
  quad.subImage.swapchain = XR_NULL_HANDLE;
  quad.size.height = 0;
  CHECK(xg::xrGetRecommendedLayerResolutionMETA(session, &info, &out) == XR_ERROR_VALIDATION_FAILURE);
  REQUIRE(xg::xrDestroySpace(space) == XR_SUCCESS);
  REQUIRE(xg::xrDestroySession(session) == XR_SUCCESS);
  REQUIRE(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  venture::xr::install_xr_images(nullptr);
}

TEST_CASE("XR legacy OVRPlugin reference spaces preserve floor tracking") {
  venture::test::XrImageFixture interop;
  venture::xr::install_xr_images(&interop);
  const char* extension = "XR_KHR_opengl_es_enable";
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 27);
  create.enabledExtensionNames = &extension;
  create.enabledExtensionCount = 1;
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&create, &instance) == XR_SUCCESS);
  CHECK_FALSE(ox::enabled("XR_EXT_local_floor"));
  CHECK_FALSE(ox::enabled("XR_OCULUS_common_reference_spaces"));
  xg::GlesBinding binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR, nullptr, 1, 2, 3};
  XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO, &binding, 0, 1};
  XrSession session{};
  REQUIRE(xg::xrCreateSession(instance, &sessionInfo, &session) == XR_SUCCESS);
  MockXrScript input;
  ventureMockXrSetScript(&input);
  XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr,
      XR_REFERENCE_SPACE_TYPE_VIEW, {{0, 0, 0, 1}, {0, 0, 0}}};
  XrSpace view{};
  REQUIRE(xg::xrCreateReferenceSpace(session, &info, &view) == XR_SUCCESS);
  const XrTime time = 1;
  // Job Simulator's OVRPlugin 1.88 creates these with next=null even when
  // XR_OCULUS_common_reference_spaces was not advertised/enabled.
  for (const auto [legacy, native] : {
           std::pair{1000043000, XR_REFERENCE_SPACE_TYPE_STAGE},
           std::pair{1000043001, XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR}}) {
    info.referenceSpaceType = static_cast<XrReferenceSpaceType>(legacy);
    XrSpace space{};
    const auto result = xg::xrCreateReferenceSpace(session, &info, &space);
    CHECK(result == XR_SUCCESS);
    if (XR_FAILED(result)) continue;
    CHECK(ox::valid_handle(space, "XrSpace"));
    CHECK(int(info.referenceSpaceType) == legacy);
    CHECK(info.next == nullptr);
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    REQUIRE(xg::xrLocateSpace(view, space, time, &head) == XR_SUCCESS);
    CHECK(head.locationFlags == 15);
    CHECK(head.pose.position.y == doctest::Approx(1.6f));
    REQUIRE(xg::xrDestroySpace(space) == XR_SUCCESS);

    // A translated space must retain the caller's position and orientation.
    info.poseInReferenceSpace = {{0, .38268343f, 0, .92387953f}, {.3f, .4f, -.5f}};
    REQUIRE(xg::xrCreateReferenceSpace(session, &info, &space) == XR_SUCCESS);
    auto nativeInfo = info;
    nativeInfo.referenceSpaceType = native;
    XrSpace reference{};
    // The host enables local-floor independently of the guest's 1.0 instance.
    REQUIRE(::xrCreateReferenceSpace(session, &nativeInfo, &reference) == XR_SUCCESS);
    XrSpaceLocation relative{XR_TYPE_SPACE_LOCATION};
    REQUIRE(::xrLocateSpace(space, reference, time, &relative) == XR_SUCCESS);
    CHECK(relative.locationFlags == 15);
    CHECK(relative.pose.position.x == doctest::Approx(0).epsilon(.00001));
    CHECK(relative.pose.position.y == doctest::Approx(0).epsilon(.00001));
    CHECK(relative.pose.position.z == doctest::Approx(0).epsilon(.00001));
    CHECK(relative.pose.orientation.w == doctest::Approx(1));
    CHECK(info.poseInReferenceSpace.position.y == .4f);
    CHECK(info.poseInReferenceSpace.orientation.y == .38268343f);
    REQUIRE(::xrDestroySpace(reference) == XR_SUCCESS);
    REQUIRE(xg::xrDestroySpace(space) == XR_SUCCESS);
    info.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
  }
  info.referenceSpaceType = static_cast<XrReferenceSpaceType>(1000043002);
  XrSpace unsupported{};
  CHECK(xg::xrCreateReferenceSpace(session, &info, &unsupported) == XR_ERROR_REFERENCE_SPACE_UNSUPPORTED);
  CHECK(unsupported == XR_NULL_HANDLE);
  // A host without XR_EXT_local_floor (Virtual Desktop) gets that extension's
  // floor: LOCAL lowered to the STAGE floor, where the host's own would be.
  info.referenceSpaceType = static_cast<XrReferenceSpaceType>(1000043001);
  auto floor_extension = venture::xr::host_xr().enabled_extensions.extract("XR_EXT_local_floor");
  XrSpace emulated{};
  REQUIRE(xg::xrCreateReferenceSpace(session, &info, &emulated) == XR_SUCCESS);
  venture::xr::host_xr().enabled_extensions.insert(std::move(floor_extension));
  XrReferenceSpaceCreateInfo floor_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr,
                                        XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR, {{0, 0, 0, 1}, {0, 0, 0}}};
  XrSpace floor{};
  REQUIRE(::xrCreateReferenceSpace(session, &floor_info, &floor) == XR_SUCCESS);
  XrSpaceLocation offset{XR_TYPE_SPACE_LOCATION};
  REQUIRE(::xrLocateSpace(emulated, floor, time, &offset) == XR_SUCCESS);
  CHECK(offset.pose.position.x == doctest::Approx(0).epsilon(.00001));
  CHECK(offset.pose.position.y == doctest::Approx(0).epsilon(.00001));
  CHECK(offset.pose.position.z == doctest::Approx(0).epsilon(.00001));
  CHECK(offset.pose.orientation.w == doctest::Approx(1));
  REQUIRE(::xrDestroySpace(floor) == XR_SUCCESS);
  REQUIRE(xg::xrDestroySpace(emulated) == XR_SUCCESS);
  REQUIRE(xg::xrDestroySpace(view) == XR_SUCCESS);
  REQUIRE(xg::xrDestroySession(session) == XR_SUCCESS);
  REQUIRE(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  venture::xr::install_xr_images(nullptr);
}

TEST_CASE("XR guest capabilities advertise implemented sampler states") {
  auto& host = venture::xr::host_xr();
  struct Restore {
    venture::xr::HostXr& host;
    decltype(host.available_extensions) available = host.available_extensions;
    decltype(host.enabled_extensions) enabled = host.enabled_extensions;
    ~Restore() {
      host.available_extensions = std::move(available);
      host.enabled_extensions = std::move(enabled);
    }
  } restore{host};
  for (const char* name : {"XR_FB_swapchain_update_state_vulkan", "XR_FB_swapchain_update_state_opengl_es"}) {
    host.available_extensions[name] = 1;
    host.enabled_extensions.insert(name);
  }
  const auto capabilities = venture::hle::openxr::capabilities();
  CHECK(capabilities.contains("XR_FB_swapchain_update_state_vulkan"));
  CHECK(capabilities.contains("XR_FB_swapchain_update_state_opengl_es"));
  CHECK(capabilities.contains("XR_FB_swapchain_update_state"));
}

TEST_CASE("XR foveation hints preserve full-rate GLES stereo submission") {
  venture::test::XrImageFixture interop;
  venture::xr::install_xr_images(&interop);
  const char* extensions[]{"XR_FB_swapchain_update_state", "XR_FB_foveation",
                           "XR_KHR_opengl_es_enable", "XR_FB_foveation_configuration"};
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  create.enabledExtensionNames = extensions;
  create.enabledExtensionCount = 4;
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&create, &instance) == XR_SUCCESS);
  PFN_xrVoidFunction proc{};
  REQUIRE(xg::xrGetInstanceProcAddr(instance, "xrCreateFoveationProfileFB", &proc) == XR_SUCCESS);
  CHECK(proc != nullptr);
  REQUIRE(xg::xrGetInstanceProcAddr(instance, "xrUpdateSwapchainFB", &proc) == XR_SUCCESS);
  CHECK(proc != nullptr);
  std::uint32_t count{};
  REQUIRE(xg::xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr) == XR_SUCCESS);
  std::vector<XrExtensionProperties> advertised(count, {XR_TYPE_EXTENSION_PROPERTIES});
  REQUIRE(xg::xrEnumerateInstanceExtensionProperties(nullptr, count, &count, advertised.data()) == XR_SUCCESS);
  std::set<std::string> names;
  for (const auto& extension : advertised) names.insert(extension.extensionName);
  CHECK(names.contains("XR_FB_foveation"));
  CHECK(names.contains("XR_FB_foveation_configuration"));
  CHECK(names.contains("XR_FB_swapchain_update_state"));
  CHECK(names.contains("XR_FB_foveation_vulkan") == ox::full_rate_composition());
  CHECK(names.contains("XR_FB_space_warp") == ox::full_rate_composition());
  xg::GlesBinding binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR, nullptr, 1, 2, 3};
  XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO, &binding, 0, 1};
  XrSession session{};
  REQUIRE(xg::xrCreateSession(instance, &sessionInfo, &session) == XR_SUCCESS);

  XrFoveationLevelProfileCreateInfoFB level{XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB};
  level.level = XR_FOVEATION_LEVEL_HIGH_FB;
  level.dynamic = XR_FOVEATION_DYNAMIC_LEVEL_ENABLED_FB;
  level.verticalOffset = 12;
  XrFoveationProfileCreateInfoFB profileInfo{XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB, &level};
  XrFoveationProfileFB profile{};
  level.next = &level;
  CHECK(xg::xrCreateFoveationProfileFB(session, &profileInfo, &profile) == XR_ERROR_VALIDATION_FAILURE);
  CHECK(profile == XR_NULL_HANDLE);
  level.next = nullptr;
  level.verticalOffset = std::numeric_limits<float>::quiet_NaN();
  CHECK(xg::xrCreateFoveationProfileFB(session, &profileInfo, &profile) == XR_ERROR_VALIDATION_FAILURE);
  level.verticalOffset = 12;
  for (int value = XR_FOVEATION_LEVEL_NONE_FB; value <= XR_FOVEATION_LEVEL_HIGH_FB; ++value) {
    level.level = static_cast<XrFoveationLevelFB>(value);
    REQUIRE(xg::xrCreateFoveationProfileFB(session, &profileInfo, &profile) == XR_SUCCESS);
    CHECK(ox::state().foveation.at(profile)->level == level.level);
    CHECK(ox::state().foveation.at(profile)->dynamic == level.dynamic);
    CHECK(ox::state().foveation.at(profile)->vertical == level.verticalOffset);
    if (level.level != XR_FOVEATION_LEVEL_HIGH_FB)
      REQUIRE(xg::xrDestroyFoveationProfileFB(profile) == XR_SUCCESS);
  }
  CHECK(level.level == XR_FOVEATION_LEVEL_HIGH_FB);
  CHECK(level.dynamic == XR_FOVEATION_DYNAMIC_LEVEL_ENABLED_FB);
  CHECK(level.verticalOffset == 12);
  CHECK(level.next == nullptr);

  XrSwapchainCreateInfoFoveationFB foveation{XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB};
  XrSwapchainCreateInfo imageInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO, &foveation};
  imageInfo.format = 0x8058;
  imageInfo.width = imageInfo.height = 32;
  imageInfo.arraySize = 2;
  imageInfo.sampleCount = imageInfo.mipCount = imageInfo.faceCount = 1;
  imageInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  XrSwapchain chain{};
  foveation.flags = XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB;
  // Negotiating the Vulkan resource contract does not make it usable with
  // a GLES graphics binding. Cover the defensive guard independently here.
  ox::state().enabled.insert("XR_FB_foveation_vulkan");
  CHECK(xg::xrCreateSwapchain(session, &imageInfo, &chain) == XR_ERROR_FEATURE_UNSUPPORTED);
  ox::state().enabled.erase("XR_FB_foveation_vulkan");
  CHECK(chain == XR_NULL_HANDLE);
  foveation.flags = 4;
  CHECK(xg::xrCreateSwapchain(session, &imageInfo, &chain) == XR_ERROR_VALIDATION_FAILURE);
  foveation.flags = XR_SWAPCHAIN_CREATE_FOVEATION_SCALED_BIN_BIT_FB;
  REQUIRE(xg::xrCreateSwapchain(session, &imageInfo, &chain) == XR_SUCCESS);
  REQUIRE(xg::xrDestroySwapchain(chain) == XR_SUCCESS);
  // OVRPlugin also sets the density-map bit without negotiating Vulkan
  // foveation. Preserve full-rate color images and successful stereo frames.
  foveation.flags = XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB;
  REQUIRE(xg::xrCreateSwapchain(session, &imageInfo, &chain) == XR_SUCCESS);
  CHECK(foveation.next == nullptr);
  CHECK(foveation.flags == XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB);
  XrSwapchainStateFoveationFB selected{XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB, nullptr, 0, profile};
  REQUIRE(xg::xrUpdateSwapchainFB(chain, reinterpret_cast<XrSwapchainStateBaseHeaderFB*>(&selected)) == XR_SUCCESS);
  selected.profile = XR_NULL_HANDLE;
  REQUIRE(xg::xrGetSwapchainStateFB(chain, reinterpret_cast<XrSwapchainStateBaseHeaderFB*>(&selected)) == XR_SUCCESS);
  CHECK(selected.profile == profile);
  REQUIRE(xg::xrDestroyFoveationProfileFB(profile) == XR_SUCCESS);

  XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr,
      XR_REFERENCE_SPACE_TYPE_LOCAL, {{0, 0, 0, 1}, {0, 0, 0}}};
  XrSpace local{};
  REQUIRE(xg::xrCreateReferenceSpace(session, &spaceInfo, &local) == XR_SUCCESS);
  auto poll = [&] {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xg::xrPollEvent(instance, &event) == XR_SUCCESS) event = {XR_TYPE_EVENT_DATA_BUFFER};
  };
  poll();
  XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
  REQUIRE(xg::xrBeginSession(session, &begin) == XR_SUCCESS);
  poll();
  XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                           {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  for (std::uint32_t eye = 0; eye < 2; ++eye) {
    views[eye].pose.orientation.w = 1;
    views[eye].fov = {-0.5f, 0.5f, 0.5f, -0.5f};
    views[eye].subImage = {chain, {{0, 0}, {32, 32}}, eye};
  }
  XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION, nullptr, 0, local, 2, views};
  const auto* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
  const auto previous = ventureMockXrStats().ends;
  for (int frame = 0; frame < 2; ++frame) {
    XrFrameState timing{XR_TYPE_FRAME_STATE};
    REQUIRE(xg::xrWaitFrame(session, nullptr, &timing) == XR_SUCCESS);
    REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_SUCCESS);
    std::uint32_t index{};
    REQUIRE(xg::xrAcquireSwapchainImage(chain, nullptr, &index) == XR_SUCCESS);
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    REQUIRE(xg::xrWaitSwapchainImage(chain, &wait) == XR_SUCCESS);
    REQUIRE(xg::xrReleaseSwapchainImage(chain, nullptr) == XR_SUCCESS);
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, timing.predictedDisplayTime,
                       XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1, &layer};
    REQUIRE(xg::xrEndFrame(session, &end) == XR_SUCCESS);
    const auto pixel = interop.read_pixel();
    CHECK(pixel[0] == 64);
    CHECK((pixel[1] == 127 || pixel[1] == 128));
    CHECK(pixel[2] == 191);
    CHECK(pixel[3] == 255);
  }
  CHECK(ventureMockXrStats().ends == previous + 2);
  REQUIRE(xg::xrRequestExitSession(session) == XR_SUCCESS);
  poll();
  CHECK(xg::xrEndSession(session) == XR_SUCCESS);
  CHECK(xg::xrDestroySpace(local) == XR_SUCCESS);
  CHECK(xg::xrDestroySwapchain(chain) == XR_SUCCESS);
  CHECK(xg::xrDestroySession(session) == XR_SUCCESS);
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  venture::xr::install_xr_images(nullptr);
}

TEST_CASE("XR GLES subimage rects reach the host in the bridged image's row order") {
  venture::test::XrImageFixture interop;
  venture::xr::install_xr_images(&interop);
  const char* extensions[]{"XR_KHR_opengl_es_enable", "XR_KHR_composition_layer_cylinder",
                           "XR_FB_composition_layer_image_layout"};
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  create.enabledExtensionNames = extensions;
  create.enabledExtensionCount = 3;
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&create, &instance) == XR_SUCCESS);
  xg::GlesBinding binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR, nullptr, 1, 2, 3};
  XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO, &binding, 0, 1};
  XrSession session{};
  REQUIRE(xg::xrCreateSession(instance, &session_info, &session) == XR_SUCCESS);
  XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr,
      XR_REFERENCE_SPACE_TYPE_LOCAL, {{0, 0, 0, 1}, {0, 0, 0}}};
  XrSpace space{};
  REQUIRE(xg::xrCreateReferenceSpace(session, &space_info, &space) == XR_SUCCESS);
  auto make_chain = [&](std::uint32_t width, std::uint32_t height, std::uint32_t layers) {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.format = 0x8058;  // GL_RGBA8
    info.width = width;
    info.height = height;
    info.arraySize = layers;
    info.sampleCount = info.mipCount = info.faceCount = 1;
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    XrSwapchain chain{};
    REQUIRE(xg::xrCreateSwapchain(session, &info, &chain) == XR_SUCCESS);
    return chain;
  };
  const auto eyes = make_chain(32, 24, 2);
  const auto overlay = make_chain(16, 8, 1);
  auto poll = [&] {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xg::xrPollEvent(instance, &event) == XR_SUCCESS) event = {XR_TYPE_EVENT_DATA_BUFFER};
  };
  poll();
  XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
  REQUIRE(xg::xrBeginSession(session, &begin) == XR_SUCCESS);
  poll();
  // OVRPlugin dynamic resolution renders part of a GLES image. Its rects are
  // in XR_KHR_opengl_es_enable coordinates, with the origin at the lower left.
  XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                           {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  for (auto& view : views) {
    view.pose.orientation.w = 1;
    view.fov = {-0.5f, 0.5f, 0.5f, -0.5f};
  }
  views[0].subImage = {eyes, {{0, 0}, {32, 16}}, 0};
  views[1].subImage = {eyes, {{4, 1}, {24, 20}}, 1};
  XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION, nullptr, 0, space, 2, views};
  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD, nullptr, 0, space, XR_EYE_VISIBILITY_BOTH,
      {overlay, {{0, 0}, {16, 8}}, 0}, {{0, 0, 0, 1}, {0, 0, -2}}, {2, 1}};
  XrCompositionLayerCylinderKHR cylinder{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR, nullptr, 0, space,
      XR_EYE_VISIBILITY_BOTH, {overlay, {{2, 1}, {12, 3}}, 0}, {{0, 0, 0, 1}, {0, 0, 0}}, 2, 1, 4};
  const XrCompositionLayerBaseHeader* layers[]{
      reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),
      reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad),
      reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cylinder)};
  for (int frame = 0; frame < 2; ++frame) {
    XrFrameState timing{XR_TYPE_FRAME_STATE};
    REQUIRE(xg::xrWaitFrame(session, nullptr, &timing) == XR_SUCCESS);
    REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_SUCCESS);
    for (auto chain : {eyes, overlay}) {
      std::uint32_t index{};
      REQUIRE(xg::xrAcquireSwapchainImage(chain, nullptr, &index) == XR_SUCCESS);
      XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
      REQUIRE(xg::xrWaitSwapchainImage(chain, &wait) == XR_SUCCESS);
      REQUIRE(xg::xrReleaseSwapchainImage(chain, nullptr) == XR_SUCCESS);
    }
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, timing.predictedDisplayTime,
                       XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 3, layers};
    REQUIRE(xg::xrEndFrame(session, &end) == XR_SUCCESS);
    // The bridge copies GLES rows bottom-up into the host image, so the host
    // samples y' = height - y - h; x, extent and the array index are unchanged.
    XrSwapchainSubImage host[5]{};
    REQUIRE(ventureMockXrLastSubImages(host, 5) == 4);
    CHECK(host[0].swapchain == venture::hle::openxr::state().swapchains.at(eyes).host_handle);
    CHECK(host[0].imageArrayIndex == 0);
    CHECK(host[0].imageRect.offset.x == 0);
    CHECK(host[0].imageRect.offset.y == 8);
    CHECK(host[0].imageRect.extent.width == 32);
    CHECK(host[0].imageRect.extent.height == 16);
    CHECK(host[1].imageArrayIndex == 1);
    CHECK(host[1].imageRect.offset.x == 4);
    CHECK(host[1].imageRect.offset.y == 3);
    CHECK(host[1].imageRect.extent.height == 20);
    CHECK(host[2].swapchain == venture::hle::openxr::state().swapchains.at(overlay).host_handle);
    CHECK(host[2].imageRect.offset.y == 0);
    CHECK(host[2].imageRect.extent.height == 8);
    CHECK(host[3].imageRect.offset.x == 2);
    CHECK(host[3].imageRect.offset.y == 4);
    CHECK(host[3].imageRect.extent.height == 3);
    // Only host copies change; the application's layers keep GLES coordinates.
    CHECK(projection.views == views);
    CHECK(views[0].subImage.imageRect.offset.y == 0);
    CHECK(views[1].subImage.imageRect.offset.y == 1);
    CHECK(cylinder.subImage.imageRect.offset.y == 1);
  }
  // XR_FB_composition_layer_image_layout: Venture reverses the flipped layer's
  // rows once more in the host copy, so its rect keeps the application's y.
  XrCompositionLayerImageLayoutFB flip{XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB, nullptr,
                                       XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB};
  cylinder.next = &flip;
  for (int frame = 0; frame < 2; ++frame) {
    XrFrameState timing{XR_TYPE_FRAME_STATE};
    REQUIRE(xg::xrWaitFrame(session, nullptr, &timing) == XR_SUCCESS);
    REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_SUCCESS);
    for (auto chain : {eyes, overlay}) {
      std::uint32_t index{};
      REQUIRE(xg::xrAcquireSwapchainImage(chain, nullptr, &index) == XR_SUCCESS);
      XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
      REQUIRE(xg::xrWaitSwapchainImage(chain, &wait) == XR_SUCCESS);
      REQUIRE(xg::xrReleaseSwapchainImage(chain, nullptr) == XR_SUCCESS);
    }
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, timing.predictedDisplayTime,
                       XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 3, layers};
    REQUIRE(xg::xrEndFrame(session, &end) == XR_SUCCESS);
    XrSwapchainSubImage host[5]{};
    REQUIRE(ventureMockXrLastSubImages(host, 5) == 4);
    CHECK(host[0].imageRect.offset.y == 8);
    CHECK(host[3].imageRect.offset.y == 1);
    CHECK(venture::hle::openxr::state().swapchains.at(overlay).host_flip);
    CHECK_FALSE(venture::hle::openxr::state().swapchains.at(eyes).host_flip);
  }
  // Unreal can round a crop a pixel or two past the right and bottom edges;
  // the host copy is clipped instead of the host rejecting the frame.
  views[1].subImage.imageRect = {{4, 1}, {30, 25}};
  {
    XrFrameState timing{XR_TYPE_FRAME_STATE};
    REQUIRE(xg::xrWaitFrame(session, nullptr, &timing) == XR_SUCCESS);
    REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_SUCCESS);
    for (auto chain : {eyes, overlay}) {
      std::uint32_t index{};
      REQUIRE(xg::xrAcquireSwapchainImage(chain, nullptr, &index) == XR_SUCCESS);
      XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
      REQUIRE(xg::xrWaitSwapchainImage(chain, &wait) == XR_SUCCESS);
      REQUIRE(xg::xrReleaseSwapchainImage(chain, nullptr) == XR_SUCCESS);
    }
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, timing.predictedDisplayTime,
                       XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 3, layers};
    REQUIRE(xg::xrEndFrame(session, &end) == XR_SUCCESS);
    XrSwapchainSubImage host[5]{};
    REQUIRE(ventureMockXrLastSubImages(host, 5) == 4);
    CHECK(host[1].imageRect.offset.x == 4);
    CHECK(host[1].imageRect.offset.y == 0);
    CHECK(host[1].imageRect.extent.width == 28);
    CHECK(host[1].imageRect.extent.height == 23);
    CHECK(views[1].subImage.imageRect.extent.width == 30);
  }
  REQUIRE(xg::xrRequestExitSession(session) == XR_SUCCESS);
  poll();
  CHECK(xg::xrEndSession(session) == XR_SUCCESS);
  CHECK(xg::xrDestroySpace(space) == XR_SUCCESS);
  CHECK(xg::xrDestroySwapchain(overlay) == XR_SUCCESS);
  CHECK(xg::xrDestroySwapchain(eyes) == XR_SUCCESS);
  CHECK(xg::xrDestroySession(session) == XR_SUCCESS);
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  venture::xr::install_xr_images(nullptr);
}

TEST_CASE("XR leaves out the smallest layers when the host takes fewer than the game submits") {
  // SteamVR on Linux refused every frame (XR_ERROR_LAYER_LIMIT_EXCEEDED) once
  // AC Nexus showed more panels than it takes; the game froze.
  struct LayerLimit {
    LayerLimit() { set("2"); }
    ~LayerLimit() { set(""); }
    static void set(const char* value) {
#ifdef _WIN32
      _putenv_s("VENTURE_MOCK_MAX_LAYERS", value);
#else
      if (*value) setenv("VENTURE_MOCK_MAX_LAYERS", value, 1); else unsetenv("VENTURE_MOCK_MAX_LAYERS");
#endif
      venture::environment::refresh();
      ventureMockXrRefreshEnvironment();
    }
  } limit;
  venture::test::XrImageFixture interop;
  venture::xr::install_xr_images(&interop);
  const char* extensions[]{"XR_KHR_opengl_es_enable", "XR_KHR_composition_layer_cylinder",
                           "XR_FB_composition_layer_image_layout"};
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  create.enabledExtensionNames = extensions;
  create.enabledExtensionCount = 3;
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&create, &instance) == XR_SUCCESS);
  xg::GlesBinding binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR, nullptr, 1, 2, 3};
  XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO, &binding, 0, 1};
  XrSession session{};
  REQUIRE(xg::xrCreateSession(instance, &session_info, &session) == XR_SUCCESS);
  XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr,
      XR_REFERENCE_SPACE_TYPE_LOCAL, {{0, 0, 0, 1}, {0, 0, 0}}};
  XrSpace space{};
  REQUIRE(xg::xrCreateReferenceSpace(session, &space_info, &space) == XR_SUCCESS);
  auto make_chain = [&](std::uint32_t width, std::uint32_t height, std::uint32_t layers) {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.format = 0x8058;  // GL_RGBA8
    info.width = width;
    info.height = height;
    info.arraySize = layers;
    info.sampleCount = info.mipCount = info.faceCount = 1;
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    XrSwapchain chain{};
    REQUIRE(xg::xrCreateSwapchain(session, &info, &chain) == XR_SUCCESS);
    return chain;
  };
  const auto eyes = make_chain(32, 24, 2);
  const auto overlay = make_chain(16, 8, 1);
  auto poll = [&] {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xg::xrPollEvent(instance, &event) == XR_SUCCESS) event = {XR_TYPE_EVENT_DATA_BUFFER};
  };
  poll();
  XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
  REQUIRE(xg::xrBeginSession(session, &begin) == XR_SUCCESS);
  poll();
  // OVRPlugin dynamic resolution renders part of a GLES image. Its rects are
  // in XR_KHR_opengl_es_enable coordinates, with the origin at the lower left.
  XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                           {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  for (auto& view : views) {
    view.pose.orientation.w = 1;
    view.fov = {-0.5f, 0.5f, 0.5f, -0.5f};
  }
  views[0].subImage = {eyes, {{0, 0}, {32, 16}}, 0};
  views[1].subImage = {eyes, {{4, 1}, {24, 20}}, 1};
  XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION, nullptr, 0, space, 2, views};
  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD, nullptr, 0, space, XR_EYE_VISIBILITY_BOTH,
      {overlay, {{0, 0}, {16, 8}}, 0}, {{0, 0, 0, 1}, {0, 0, -2}}, {2, 1}};
  XrCompositionLayerCylinderKHR cylinder{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR, nullptr, 0, space,
      XR_EYE_VISIBILITY_BOTH, {overlay, {{2, 1}, {12, 3}}, 0}, {{0, 0, 0, 1}, {0, 0, 0}}, 2, 1, 4};
  const XrCompositionLayerBaseHeader* layers[]{
      reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),
      reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad),
      reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cylinder)};
  for (int frame = 0; frame < 2; ++frame) {
    XrFrameState timing{XR_TYPE_FRAME_STATE};
    REQUIRE(xg::xrWaitFrame(session, nullptr, &timing) == XR_SUCCESS);
    REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_SUCCESS);
    for (auto chain : {eyes, overlay}) {
      std::uint32_t index{};
      REQUIRE(xg::xrAcquireSwapchainImage(chain, nullptr, &index) == XR_SUCCESS);
      XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
      REQUIRE(xg::xrWaitSwapchainImage(chain, &wait) == XR_SUCCESS);
      REQUIRE(xg::xrReleaseSwapchainImage(chain, nullptr) == XR_SUCCESS);
    }
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, timing.predictedDisplayTime,
                       XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 3, layers};
    CHECK(xg::xrEndFrame(session, &end) == XR_SUCCESS);
    // Both eye views and the quad; the cylinder, the smallest, is left out.
    XrSwapchainSubImage host[5]{};
    CHECK(ventureMockXrLastSubImages(host, 5) == 3);
    CHECK(host[2].imageRect.extent.width == 16);
  }
  REQUIRE(xg::xrRequestExitSession(session) == XR_SUCCESS);
  poll();
  CHECK(xg::xrEndSession(session) == XR_SUCCESS);
  CHECK(xg::xrDestroySpace(space) == XR_SUCCESS);
  CHECK(xg::xrDestroySwapchain(overlay) == XR_SUCCESS);
  CHECK(xg::xrDestroySwapchain(eyes) == XR_SUCCESS);
  CHECK(xg::xrDestroySession(session) == XR_SUCCESS);
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  venture::xr::install_xr_images(nullptr);
}

TEST_CASE("XR paced waits survive session restart, guest interruption and session loss") {
  using namespace std::chrono_literals;
  venture::test::XrImageFixture interop;
  venture::xr::install_xr_images(&interop);
  const char *extensions[]{"XR_KHR_opengl_es_enable"};
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  create.enabledExtensionNames = extensions;
  create.enabledExtensionCount = 1;
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&create, &instance) == XR_SUCCESS);
  xg::GlesBinding binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR, nullptr, 1, 2, 3};
  XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO, &binding, 0, 1};
  XrSession session{};
  REQUIRE(xg::xrCreateSession(instance, &sessionInfo, &session) == XR_SUCCESS);
  // Drains events and reports whether `wanted` was among them.
  auto poll = [&](XrSessionState wanted) {
    bool seen = false;
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xg::xrPollEvent(instance, &event) == XR_SUCCESS) {
      if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED &&
          reinterpret_cast<XrEventDataSessionStateChanged &>(event).state == wanted)
        seen = true;
      event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    return seen;
  };
  REQUIRE(poll(XR_SESSION_STATE_READY));
  XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
  REQUIRE(xg::xrBeginSession(session, &begin) == XR_SUCCESS);
  REQUIRE(poll(XR_SESSION_STATE_FOCUSED));
  XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, 0, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 0, nullptr};
  // Release a broken implementation's blocked waiter before an assertion
  // unwinds its future; the session cannot be reused afterwards.
  auto settle = [&](auto &future) {
    const bool ready = future.wait_for(30s) == std::future_status::ready;
    if (!ready) FAIL_CHECK("frame wait did not complete after the session transition; destroying session to release it");
    if (!ready) xg::xrDestroySession(session);
    return ready;
  };

  // A frame waited before a runtime-initiated stop (STOPPING, then READY)
  // must not gate the first wait after xrBeginSession: the host's frame state
  // starts reset. A pipelined render thread never began it.
  XrFrameState stale{XR_TYPE_FRAME_STATE}, fresh{XR_TYPE_FRAME_STATE};
  REQUIRE(xg::xrWaitFrame(session, nullptr, &stale) == XR_SUCCESS);
  ventureMockXrRestartSession();
  REQUIRE(poll(XR_SESSION_STATE_STOPPING));
  REQUIRE(xg::xrEndSession(session) == XR_SUCCESS);
  CHECK(xg::xrBeginFrame(session, nullptr) == XR_ERROR_SESSION_NOT_RUNNING);
  CHECK(xg::xrWaitFrame(session, nullptr, &fresh) == XR_ERROR_SESSION_NOT_RUNNING);
  REQUIRE(poll(XR_SESSION_STATE_READY));
  REQUIRE(xg::xrBeginSession(session, &begin) == XR_SUCCESS);
  auto restarted = std::async(std::launch::async, [&] {
    return xg::xrWaitFrame(session, nullptr, &fresh);
  });
  REQUIRE(settle(restarted));
  REQUIRE(restarted.get() == XR_SUCCESS);
  CHECK(fresh.predictedDisplayTime > stale.predictedDisplayTime);
  REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_SUCCESS);
  end.displayTime = fresh.predictedDisplayTime;
  REQUIRE(xg::xrEndFrame(session, &end) == XR_SUCCESS);
  poll(XR_SESSION_STATE_FOCUSED);

  // A guest thread parked in the paced wait blocks through futex_wait
  // (core/wait.h): a signal interrupt re-blocks it, process or thread exit
  // unwinds it, and the frame it was waiting behind is unaffected.
  XrFrameState first{XR_TYPE_FRAME_STATE}, following{XR_TYPE_FRAME_STATE};
  REQUIRE(xg::xrWaitFrame(session, nullptr, &first) == XR_SUCCESS);
  std::promise<venture::GuestThread *> adopted;
  auto parked = std::async(std::launch::async, [&] {
    adopted.set_value(&venture::GuestThread::adopt_current());
    return xg::xrWaitFrame(session, nullptr, &following);
  });
  auto *thread = adopted.get_future().get();
  CHECK(parked.wait_for(50ms) == std::future_status::timeout);
  thread->interrupt();
  CHECK(parked.wait_for(50ms) == std::future_status::timeout);
  thread->exit_requested = true;
  thread->interrupt();
  REQUIRE(settle(parked));
  CHECK_THROWS_AS(parked.get(), venture::ThreadExit);
  REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_SUCCESS);
  end.displayTime = first.predictedDisplayTime;
  REQUIRE(xg::xrEndFrame(session, &end) == XR_SUCCESS);

  // A host session loss reported by the render thread's xrBeginFrame
  // releases the pipelined waiter with the host's error, and later waits on
  // the lost handle return it immediately instead of blocking.
  XrFrameState waited{XR_TYPE_FRAME_STATE}, next{XR_TYPE_FRAME_STATE};
  REQUIRE(xg::xrWaitFrame(session, nullptr, &waited) == XR_SUCCESS);
  auto pipelined = std::async(std::launch::async, [&] {
    venture::GuestThread::adopt_current();
    return xg::xrWaitFrame(session, nullptr, &next);
  });
  CHECK(pipelined.wait_for(50ms) == std::future_status::timeout);
  ventureMockXrLoseSession();
  CHECK(xg::xrBeginFrame(session, nullptr) == XR_ERROR_SESSION_LOST);
  REQUIRE(settle(pipelined));
  CHECK(pipelined.get() == XR_ERROR_SESSION_LOST);
  CHECK(xg::xrWaitFrame(session, nullptr, &next) == XR_ERROR_SESSION_LOST);
  CHECK(poll(XR_SESSION_STATE_LOSS_PENDING));
  REQUIRE(xg::xrDestroySession(session) == XR_SUCCESS);
  REQUIRE(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  venture::xr::install_xr_images(nullptr);
}

static void frame_lead_case(bool async) {
  using namespace std::chrono_literals;
  venture::test::XrImageFixture interop;
  venture::xr::install_xr_images(&interop);
  const char *extensions[]{"XR_KHR_opengl_es_enable"};
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  create.enabledExtensionNames = extensions;
  create.enabledExtensionCount = 1;
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&create, &instance) == XR_SUCCESS);
  xg::GlesBinding binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR, nullptr, 1, 2, 3};
  XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO, &binding, 0, 1};
  XrSession session{};
  REQUIRE(xg::xrCreateSession(instance, &sessionInfo, &session) == XR_SUCCESS);
  auto poll = [&](XrSessionState wanted) {
    bool seen = false;
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xg::xrPollEvent(instance, &event) == XR_SUCCESS) {
      if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED &&
          reinterpret_cast<XrEventDataSessionStateChanged &>(event).state == wanted)
        seen = true;
      event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    return seen;
  };
  REQUIRE(poll(XR_SESSION_STATE_READY));
  XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
  REQUIRE(xg::xrBeginSession(session, &begin) == XR_SUCCESS);
  REQUIRE(poll(XR_SESSION_STATE_FOCUSED));
  auto &s = venture::xr::session();
  {
    std::lock_guard lock(s.mutex);
    s.lead.enabled = true;
    s.lead.lead_ns = 1'000'000;
    s.lead.async = async;
  }
  // Frames run on an adopted guest thread, as led waits require.
  auto frames = [&](int count, bool finish_last) {
    return std::async(std::launch::async, [&, count, finish_last] {
      venture::GuestThread::adopt_current();
      for (int i = 0; i < count; ++i) {
        XrFrameState state{XR_TYPE_FRAME_STATE};
        if (auto r = xg::xrWaitFrame(session, nullptr, &state); r != XR_SUCCESS) return r;
        if (auto r = xg::xrBeginFrame(session, nullptr); r != XR_SUCCESS) return r;
        if (i + 1 == count && !finish_last) return XR_SUCCESS;
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, state.predictedDisplayTime,
                           XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 0, nullptr};
        if (auto r = xg::xrEndFrame(session, &end); r != XR_SUCCESS) return r;
      }
      return XR_SUCCESS;
    });
  };
  auto settle = [&](auto &future) {
    const bool ready = future.wait_for(30s) == std::future_status::ready;
    if (!ready) FAIL_CHECK("led frame loop did not complete; destroying session to release it");
    if (!ready) xg::xrDestroySession(session);
    return ready;
  };
  // After one host-waited frame, waits are led and End consumes the host wait
  // the waiter issued right after the previous host end.
  auto led = frames(6, true);
  REQUIRE(settle(led));
  CHECK(led.get() == XR_SUCCESS);
  CHECK(s.lead.led_frames >= 4);
  // Asynchronous submission: the host waiter made those frames' host ends.
  CHECK((s.lead.async_ends >= 4) == async);

  // A runtime stop with a host wait in flight: xrEndSession drains it first.
  ventureMockXrRestartSession();
  REQUIRE(poll(XR_SESSION_STATE_STOPPING));
  auto ended = std::async(std::launch::async, [&] { return xg::xrEndSession(session); });
  REQUIRE(settle(ended));
  CHECK(ended.get() == XR_SUCCESS);
  REQUIRE(poll(XR_SESSION_STATE_READY));
  REQUIRE(xg::xrBeginSession(session, &begin) == XR_SUCCESS);
  poll(XR_SESSION_STATE_FOCUSED);
  auto resumed = frames(4, true);
  REQUIRE(settle(resumed));
  CHECK(resumed.get() == XR_SUCCESS);

  // Loss while a led frame is begun: its End returns the loss, never hangs. An asynchronous End that was handed
  // to the host before the loss was seen succeeds, and the next Wait returns the loss instead.
  auto begun = frames(2, false);
  REQUIRE(settle(begun));
  REQUIRE(begun.get() == XR_SUCCESS);
  ventureMockXrLoseSession();
  auto lost = std::async(std::launch::async, [&] {
    venture::GuestThread::adopt_current();
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, 0, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 0, nullptr};
    const auto ended = xg::xrEndFrame(session, &end);
    if (ended != XR_SUCCESS || !async) return ended;
    XrFrameState state{XR_TYPE_FRAME_STATE};
    return xg::xrWaitFrame(session, nullptr, &state);
  });
  REQUIRE(settle(lost));
  CHECK(lost.get() == XR_ERROR_SESSION_LOST);
  REQUIRE(xg::xrDestroySession(session) == XR_SUCCESS);
  REQUIRE(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  venture::xr::install_xr_images(nullptr);
}

TEST_CASE("XR frame lead keeps the host wait on schedule through restart and loss") {
  frame_lead_case(false);
}

TEST_CASE("XR asynchronous submission keeps the frame lead's order through restart and loss") {
  frame_lead_case(true);
}

TEST_CASE("XR asynchronous submission finishes its host End before host objects are destroyed") {
  using namespace std::chrono_literals;
  venture::test::XrImageFixture interop;
  venture::xr::install_xr_images(&interop);
  const char *extensions[]{"XR_KHR_opengl_es_enable"};
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  create.enabledExtensionNames = extensions;
  create.enabledExtensionCount = 1;
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&create, &instance) == XR_SUCCESS);
  xg::GlesBinding binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR, nullptr, 1, 2, 3};
  XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO, &binding, 0, 1};
  XrSession session{};
  REQUIRE(xg::xrCreateSession(instance, &sessionInfo, &session) == XR_SUCCESS);
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  auto drain_events = [&] { while (xg::xrPollEvent(instance, &event) == XR_SUCCESS) event = {XR_TYPE_EVENT_DATA_BUFFER}; };
  drain_events();
  XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
  REQUIRE(xg::xrBeginSession(session, &begin) == XR_SUCCESS);
  drain_events();
  XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr,
      XR_REFERENCE_SPACE_TYPE_LOCAL, {{0, 0, 0, 1}, {0, 0, 0}}};
  XrSpace space{};
  REQUIRE(xg::xrCreateReferenceSpace(session, &space_info, &space) == XR_SUCCESS);
  auto &s = venture::xr::session();
  {
    std::lock_guard lock(s.mutex);
    s.lead.enabled = s.lead.async = true;
    s.lead.lead_ns = 1'000'000;
  }
  struct ReleaseEnd { ~ReleaseEnd() { ventureMockXrGateEnd(false); } } release;
  // A failed wait releases the gate first: the futures' destructors wait for their calls.
  auto settled = [](auto &future, auto timeout) {
    if (future.wait_for(timeout) == std::future_status::ready) return true;
    ventureMockXrGateEnd(false);
    future.wait_for(30s);
    return false;
  };
  // Arm the gate only once the previous asynchronous End has returned, so it
  // holds the next frame's End and not an earlier one.
  auto gate = [&] {
    s.finish_host_end();
    ventureMockXrGateEnd(true);
  };
  auto frames = [&](int count) {
    return std::async(std::launch::async, [&, count] {
      venture::GuestThread::adopt_current();
      for (int i = 0; i < count; ++i) {
        XrFrameState state{XR_TYPE_FRAME_STATE};
        if (auto r = xg::xrWaitFrame(session, nullptr, &state); r != XR_SUCCESS) return r;
        if (auto r = xg::xrBeginFrame(session, nullptr); r != XR_SUCCESS) return r;
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, state.predictedDisplayTime,
                           XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 0, nullptr};
        if (auto r = xg::xrEndFrame(session, &end); r != XR_SUCCESS) return r;
      }
      return XR_SUCCESS;
    });
  };
  auto warm = frames(4);
  REQUIRE(settled(warm, 30s));
  REQUIRE(warm.get() == XR_SUCCESS);
  REQUIRE(s.lead.async_ends >= 2);
  // The guest's End returns while the host End is still held.
  gate();
  auto held = frames(1);
  CHECK(ventureMockXrAwaitGatedEnd(5000));
  REQUIRE(settled(held, 5s));
  CHECK(held.get() == XR_SUCCESS);
  // Destroying a space or the session waits for that End, and the session's
  // drain runs before its queue lock, which the held End owns.
  auto space_gone = std::async(std::launch::async, [&] { return xg::xrDestroySpace(space); });
  CHECK(space_gone.wait_for(100ms) == std::future_status::timeout);
  ventureMockXrGateEnd(false);
  REQUIRE(settled(space_gone, 30s));
  CHECK(space_gone.get() == XR_SUCCESS);
  gate();
  auto last = frames(1);
  CHECK(ventureMockXrAwaitGatedEnd(5000));
  REQUIRE(settled(last, 5s));
  CHECK(last.get() == XR_SUCCESS);
  auto gone = std::async(std::launch::async, [&] { return xg::xrDestroySession(session); });
  CHECK(gone.wait_for(100ms) == std::future_status::timeout);
  ventureMockXrGateEnd(false);
  REQUIRE(settled(gone, 30s));
  CHECK(gone.get() == XR_SUCCESS);
  CHECK(ventureMockXrDestroysDuringEnd() == 0);
  REQUIRE(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  venture::xr::install_xr_images(nullptr);
}

TEST_CASE("OpenXR reports the launch-selected Horizon compatibility identity") {
  auto& config = venture::process_config();
  struct Restore {
    const venture::DeviceIdentity* saved;
    ~Restore() { venture::process_config().device_identity = saved; }
  } restore{config.device_identity};
  config.device_identity = &venture::select_device_identity("quest2|cambria");
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&create, &instance) == XR_SUCCESS);
  XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
  CHECK(xg::xrGetSystemProperties(instance, venture::xr::host_xr().system, &properties) == XR_SUCCESS);
  CHECK(std::string(properties.systemName) == "Meta Quest Pro");
  CHECK(properties.vendorId == 0x2833);
  // Not enabled: the headset id node is left untouched.
  XrSystemHeadsetIdPropertiesMETA headset{XR_TYPE_SYSTEM_HEADSET_ID_PROPERTIES_META};
  properties.next = &headset;
  CHECK(xg::xrGetSystemProperties(instance, venture::xr::host_xr().system, &properties) == XR_SUCCESS);
  CHECK(std::all_of(std::begin(headset.id.data), std::end(headset.id.data), [](auto b) { return b == 0; }));
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);

  // Enabled: the selected identity's id, which OVRPlugin maps to that headset.
  const char *extension = "XR_META_headset_id";
  create.enabledExtensionCount = 1;
  create.enabledExtensionNames = &extension;
  REQUIRE(xg::xrCreateInstance(&create, &instance) == XR_SUCCESS);
  CHECK(xg::xrGetSystemProperties(instance, venture::xr::host_xr().system, &properties) == XR_SUCCESS);
  CHECK(headset.type == XR_TYPE_SYSTEM_HEADSET_ID_PROPERTIES_META);
  CHECK(headset.next == nullptr);
  const std::uint8_t pro[16]{0xb5, 0x84, 0x43, 0xc9, 0xdf, 0x86, 0x40, 0xcd,
                             0x89, 0x18, 0x2a, 0x27, 0x8e, 0x18, 0xab, 0x71};
  CHECK(std::equal(std::begin(pro), std::end(pro), std::begin(headset.id.data)));
  CHECK(std::string(properties.systemName) == "Meta Quest Pro");
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
}

TEST_CASE("XR fallback hand model points every joint's -Z along its bone") {
  // XR_EXT_hand_tracking: a joint's -Z runs along its bone toward the tip. Runtimes follow it, so
  // a model joint off its bone turns that bone when their rotations drive the bind skeleton.
  for (bool left : {true, false}) {
    const auto &model = venture::xr::hand_model(left);
    for (int joint = 2; joint < 26; ++joint) {
      if (joint == XR_HAND_JOINT_THUMB_TIP_EXT || joint == XR_HAND_JOINT_INDEX_TIP_EXT ||
          joint == XR_HAND_JOINT_MIDDLE_TIP_EXT || joint == XR_HAND_JOINT_RING_TIP_EXT ||
          joint == XR_HAND_JOINT_LITTLE_TIP_EXT)
        continue;
      CAPTURE(left);
      CAPTURE(joint);
      const auto a = model.joints[joint].position, b = model.joints[joint + 1].position;
      const XrVector3f bone{b.x - a.x, b.y - a.y, b.z - a.z};
      const float length = venture::xr::length(bone);
      const auto axis = venture::xr::rotate(model.joints[joint].orientation, {0, 0, -1});
      CHECK(axis.x == doctest::Approx(bone.x / length).epsilon(0.00001));
      CHECK(axis.y == doctest::Approx(bone.y / length).epsilon(0.00001));
      CHECK(axis.z == doctest::Approx(bone.z / length).epsilon(0.00001));
    }
  }
}

TEST_CASE("XR fallback hand scale retains wrist and matches bind size") {
  const auto &model = venture::xr::hand_model(true);
  XrHandJointLocationEXT values[26]{};
  XrHandJointVelocityEXT speeds[26]{};
  for (int i = 0; i < 26; ++i) {
    values[i].pose = model.joints[i];
    auto &p = values[i].pose.position;
    p = {3 + 2 * p.x, 4 + 2 * p.y, 5 + 2 * p.z};
    values[i].radius = 2 * model.radii[i];
    values[i].locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    speeds[i].velocityFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
    speeds[i].linearVelocity = {2, 4, 6};
  }
  speeds[1].linearVelocity = {1, 2, 3};
  XrHandJointLocationsEXT joints{XR_TYPE_HAND_JOINT_LOCATIONS_EXT, nullptr, XR_TRUE, 26, values};
  XrHandJointVelocitiesEXT velocities{XR_TYPE_HAND_JOINT_VELOCITIES_EXT, nullptr, 26, speeds};
  XrHandTrackingScaleFB scale{XR_TYPE_HAND_TRACKING_SCALE_FB};
  CHECK(venture::xr::hand_scale(true, joints, &velocities, scale) == XR_SUCCESS);
  CHECK(scale.sensorOutput == doctest::Approx(2));
  CHECK(scale.currentOutput == doctest::Approx(2));
  scale.overrideHandScale = XR_TRUE;
  scale.overrideValueInput = 1;
  REQUIRE(venture::xr::hand_scale(true, joints, &velocities, scale) == XR_SUCCESS);
  CHECK(scale.sensorOutput == doctest::Approx(2));
  CHECK(scale.currentOutput == doctest::Approx(1));
  for (int i = 0; i < 26; ++i) {
    CHECK(values[i].pose.position.x == doctest::Approx(3 + model.joints[i].position.x));
    CHECK(values[i].pose.position.y == doctest::Approx(4 + model.joints[i].position.y));
    CHECK(values[i].pose.position.z == doctest::Approx(5 + model.joints[i].position.z));
    CHECK(values[i].radius == doctest::Approx(model.radii[i]));
  }
  CHECK(speeds[0].linearVelocity.x == doctest::Approx(1.5f));
  CHECK(speeds[1].linearVelocity.x == doctest::Approx(1));
  scale.overrideValueInput = -1;
  CHECK(venture::xr::hand_scale(true, joints, nullptr, scale) == XR_ERROR_VALIDATION_FAILURE);
}

TEST_CASE("XR generated metadata retains A64 member layouts") {
  CHECK(ox::command_metadata().size() >= 80);
  bool found = false;
  for (auto &m : ox::member_metadata())
    if (std::strcmp(m.structure, "XrInstanceCreateInfo") == 0 &&
        std::strcmp(m.name, "enabledExtensionNames") == 0) {
      found = true;
      CHECK(m.pointer);
      CHECK(m.offset == offsetof(XrInstanceCreateInfo, enabledExtensionNames));
    }
  CHECK(found);
  CHECK(venture::xr::touch_binding("/user/hand/left/input/x/click",
                              "/interaction_profiles/valve/index_controller") ==
        "/user/hand/left/input/a/click");
  CHECK_FALSE(venture::xr::touch_binding("/user/hand/right/input/system/click",
                                    "/interaction_profiles/valve/index_controller"));
}
TEST_CASE("XR input-chain filtering preserves an enabled tail and guest memory") {
  const char* extension = "XR_KHR_composition_layer_color_scale_bias";
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  create.enabledExtensionCount = 1;
  create.enabledExtensionNames = &extension;
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&create, &instance) == XR_SUCCESS);
  XrCompositionLayerColorScaleBiasKHR color{XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR};
  color.colorScale = {0.25f, 0.5f, 1, 1};
  XrBaseInStructure ignored{static_cast<XrStructureType>(1000294000),
      reinterpret_cast<const XrBaseInStructure*>(&color)};
  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  quad.next = &ignored;
  ox::InputChain copy;
  REQUIRE(copy.copy(&quad, false) == XR_SUCCESS);
  const auto* copied_quad = static_cast<const XrCompositionLayerQuad*>(copy.head());
  REQUIRE(copied_quad != &quad);
  const auto* copied_color = static_cast<const XrCompositionLayerColorScaleBiasKHR*>(copied_quad->next);
  REQUIRE(copied_color != nullptr);
  CHECK(copied_color->type == color.type);
  CHECK(copied_color->colorScale.r == color.colorScale.r);
  CHECK(copied_color->next == nullptr);
  CHECK(quad.next == &ignored);
  CHECK(ignored.next == reinterpret_cast<const XrBaseInStructure*>(&color));
  {
    auto& host = venture::xr::host_xr().enabled_extensions;
    auto node = host.extract(extension);
    REQUIRE_FALSE(node.empty());
    CHECK(copy.copy(&quad, false) == XR_ERROR_FEATURE_UNSUPPORTED);
    host.insert(std::move(node));
  }
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
}

TEST_CASE("XR input-chain filtering preserves Equirect2 subimages") {
  // Filtering unknown/unenabled next nodes must preserve the complete base
  // layer, including the opaque handle used by OVRPlugin's loading panorama.
  XrBaseInStructure unknown{static_cast<XrStructureType>(0x7ffffff0), nullptr};
  XrCompositionLayerEquirect2KHR sphere{XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR};
  sphere.next = &unknown;
  sphere.eyeVisibility = XR_EYE_VISIBILITY_LEFT;
  sphere.subImage = {reinterpret_cast<XrSwapchain>(std::uintptr_t(0x5678)), {{0, 0}, {256, 512}}, 0};
  sphere.centralHorizontalAngle = std::numbers::pi_v<float>;
  ox::InputChain copy;
  REQUIRE(copy.copy(&sphere, false) == XR_SUCCESS);
  const auto* copied = static_cast<const XrCompositionLayerEquirect2KHR*>(copy.head());
  REQUIRE(copied != &sphere);
  CHECK(copied->next == nullptr);
  CHECK(copied->subImage.swapchain == sphere.subImage.swapchain);
  CHECK(copied->subImage.imageRect.extent.width == 256);
  CHECK(copied->subImage.imageRect.extent.height == 512);
  CHECK(copied->subImage.imageArrayIndex == 0);
  CHECK(copied->eyeVisibility == XR_EYE_VISIBILITY_LEFT);
  CHECK(copied->centralHorizontalAngle == sphere.centralHorizontalAngle);
  CHECK(sphere.next == &unknown);
}

TEST_CASE("XR legacy Vulkan games get the shared GPU from their own instance") {
  // XR_KHR_vulkan_enable games create their VkInstance themselves. The mock,
  // like the Oculus runtime, rejects instances it did not create.
  const char *extension = "XR_KHR_vulkan_enable";
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  create.enabledExtensionCount = 1;
  create.enabledExtensionNames = &extension;
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&create, &instance) == XR_SUCCESS);
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo vci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  vci.pApplicationInfo = &app;
  VkInstance vkInstance{};
  REQUIRE(venture::test::vk_call("vkCreateInstance", &vci, nullptr, &vkInstance) == VK_SUCCESS);
  VkPhysicalDevice physical{};
  REQUIRE(xg::xrGetVulkanGraphicsDeviceKHR(instance, venture::xr::host_xr().system, vkInstance, &physical) == XR_SUCCESS);
  std::uint32_t count = 0;
  REQUIRE(venture::test::vk_call("vkEnumeratePhysicalDevices", vkInstance, &count, nullptr) == VK_SUCCESS);
  std::vector<VkPhysicalDevice> devices(count);
  REQUIRE(venture::test::vk_call("vkEnumeratePhysicalDevices", vkInstance, &count, devices.data()) == VK_SUCCESS);
  CHECK(std::find(devices.begin(), devices.end(), physical) != devices.end());
  venture::test::vk_call("vkDestroyInstance", vkInstance, nullptr);
  REQUIRE(xg::xrDestroyInstance(instance) == XR_SUCCESS);
}

TEST_CASE("XR enable2 creates tracked guest Vulkan objects with feature chains") {
  const char *extension = "XR_KHR_vulkan_enable2";
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  create.enabledExtensionCount = 1;
  create.enabledExtensionNames = &extension;
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&create, &instance) == XR_SUCCESS);
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo vci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  vci.pApplicationInfo = &app;
  XrVulkanInstanceCreateInfoKHR ici{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
  ici.systemId = venture::xr::host_xr().system;
  // The resolver is guest ARM64 code, entered through its CPU trampoline.
  ici.pfnGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      venture::vulkan::instance_proc(nullptr, "vkGetInstanceProcAddr"));
  ici.vulkanCreateInfo = &vci;
  VkInstance vkInstance{};
  VkResult result = VK_ERROR_UNKNOWN;
  REQUIRE(xg::xrCreateVulkanInstanceKHR(instance, &ici, &vkInstance, &result) == XR_SUCCESS);
  REQUIRE(result == VK_SUCCESS);
  XrVulkanGraphicsDeviceGetInfoKHR get{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
  get.systemId = ici.systemId; get.vulkanInstance = vkInstance;
  VkPhysicalDevice physical{};
  REQUIRE(xg::xrGetVulkanGraphicsDevice2KHR(instance, &get, &physical) == XR_SUCCESS);
  std::uint32_t count = 0;
  venture::test::vk_call("vkGetPhysicalDeviceQueueFamilyProperties", physical, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  venture::test::vk_call("vkGetPhysicalDeviceQueueFamilyProperties", physical, &count, families.data());
  std::uint32_t family = 0;
  while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
  REQUIRE(family < count);
  float priority = 1;
  VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue.queueFamilyIndex = family; queue.queueCount = 1; queue.pQueuePriorities = &priority;
  VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  features.features.textureCompressionASTC_LDR = VK_TRUE;
  features.features.textureCompressionETC2 = VK_TRUE;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &features};
  dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &queue;
  XrVulkanDeviceCreateInfoKHR deviceInfo{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
  deviceInfo.systemId = ici.systemId; deviceInfo.pfnGetInstanceProcAddr = ici.pfnGetInstanceProcAddr;
  deviceInfo.vulkanPhysicalDevice = physical; deviceInfo.vulkanCreateInfo = &dci;
  VkDevice device{};
  REQUIRE(xg::xrCreateVulkanDeviceKHR(instance, &deviceInfo, &device, &result) == XR_SUCCESS);
  REQUIRE(result == VK_SUCCESS);
  CHECK(device != venture::gfx::gpu_device().device);
  CHECK(venture::vulkan::device_instance(device) == vkInstance);
  CHECK(venture::vulkan::device_physical(device) == physical);
  CHECK(dci.pNext == &features);
  CHECK(features.features.textureCompressionASTC_LDR == VK_TRUE);
  CHECK(features.features.textureCompressionETC2 == VK_TRUE);
  struct DeviceHook { VkDevice device; venture::GuestAddr thunk{}; unsigned resolves{}, creates{}; } hook{device};
  hook.thunk = venture::hle::make_thunk("xr-test", "existing device", [](venture::hle::CallContext& ctx) {
    auto& hook = *static_cast<DeviceHook*>(ctx.user());
    *venture::g2h<VkDevice>(ctx.x(3)) = hook.device;
    ++hook.creates;
    ctx.return_x(VK_SUCCESS);
  }, &hook);
  auto resolve = venture::hle::make_thunk("xr-test", "Vulkan resolver", [](venture::hle::CallContext& ctx) {
    auto& hook = *static_cast<DeviceHook*>(ctx.user());
    CHECK(std::strcmp(venture::g2h<const char>(ctx.x(1)), "vkCreateDevice") == 0);
    ++hook.resolves;
    ctx.return_x(hook.thunk);
  }, &hook);
  deviceInfo.pfnGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(resolve);
  VkDevice reused{};
  REQUIRE(xg::xrCreateVulkanDeviceKHR(instance, &deviceInfo, &reused, &result) == XR_SUCCESS);
  REQUIRE(result == VK_SUCCESS);
  CHECK(reused == device);
  CHECK(hook.resolves == 1);
  CHECK(hook.creates == 1);
  CHECK(venture::vulkan::device_proc(device, "vkQueueSubmit") != 0);
  VkQueue actualQueue{};
  venture::test::vk_call("vkGetDeviceQueue", device, family, 0u, &actualQueue);
  CHECK(actualQueue != VK_NULL_HANDLE);
  venture::test::vk_call("vkDestroyDevice", device, nullptr);
  venture::test::vk_call("vkDestroyInstance", vkInstance, nullptr);
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
}
TEST_CASE("XR negotiates Vulkan foveation only with implemented full-rate resources") {
  std::uint32_t count{};
  REQUIRE(xg::xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr) == XR_SUCCESS);
  std::vector<XrExtensionProperties> properties(count, {XR_TYPE_EXTENSION_PROPERTIES});
  REQUIRE(xg::xrEnumerateInstanceExtensionProperties(nullptr, count, &count, properties.data()) ==
          XR_SUCCESS);
  bool offered = false;
  for (const auto& property : properties)
    offered |= std::strcmp(property.extensionName, "XR_FB_foveation_vulkan") == 0;
  CHECK(offered == ox::full_rate_composition());
  const char* extensions[]{"XR_KHR_vulkan_enable2", "XR_FB_swapchain_update_state",
                           "XR_FB_foveation", "XR_FB_foveation_vulkan"};
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  create.enabledExtensionCount = std::size(extensions);
  create.enabledExtensionNames = extensions;
  XrInstance instance{};
  CHECK(xg::xrCreateInstance(&create, &instance) ==
        (offered ? XR_SUCCESS : XR_ERROR_EXTENSION_NOT_PRESENT));
  if (offered) REQUIRE(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  else CHECK(instance == XR_NULL_HANDLE);
}

TEST_CASE("XR full-resolution startup resources enumerate real Vulkan images") {
  if (!venture::hle::openxr::full_rate_composition()) {
    CHECK_FALSE(venture::hle::openxr::capabilities().contains("XR_FB_foveation_vulkan"));
    return;
  }
  const char* extensions[]{"XR_KHR_vulkan_enable2", "XR_FB_swapchain_update_state",
                           "XR_FB_foveation", "XR_FB_foveation_vulkan"};
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  ci.enabledExtensionCount = std::size(extensions);
  ci.enabledExtensionNames = extensions;
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&ci, &instance) == XR_SUCCESS);
  venture::test::VulkanDevice gpu;
  XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR, nullptr,
      gpu.instance, gpu.physical, gpu.device, gpu.family, 0};
  XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO, &binding, 0, venture::xr::host_xr().system};
  XrSession session{};
  REQUIRE(xg::xrCreateSession(instance, &si, &session) == XR_SUCCESS);
  XrSwapchainCreateInfoFoveationFB hint{XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB};
  hint.flags = XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB;
  XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO, &hint};
  create.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  create.format = VK_FORMAT_R8G8B8A8_UNORM;
  create.width = create.height = 64;
  create.sampleCount = create.faceCount = create.mipCount = 1;
  create.arraySize = 2;
  XrSwapchain chain{};
  REQUIRE(xg::xrCreateSwapchain(session, &create, &chain) == XR_SUCCESS);
  std::uint32_t count{};
  REQUIRE(xg::xrEnumerateSwapchainImages(chain, 0, &count, nullptr) == XR_SUCCESS);
  REQUIRE(count > 0);
  std::vector<XrSwapchainImageVulkanKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
  std::vector<XrSwapchainImageFoveationVulkanFB> resources(count,
      {XR_TYPE_SWAPCHAIN_IMAGE_FOVEATION_VULKAN_FB});
  for (std::uint32_t i = 0; i < count; ++i) images[i].next = &resources[i];
  REQUIRE(xg::xrEnumerateSwapchainImages(chain, count, &count,
      reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())) == XR_SUCCESS);
  for (const auto& resource : resources) {
    REQUIRE(resource.image != VK_NULL_HANDLE);
    // One texel per density texel of the 64x64 eye: 1x1 without density maps,
    // 2x2 at the 32x32 texel size offered as on Quest (or a native host's).
    CHECK((resource.width == 1 || resource.width == 2));
    CHECK(resource.height == resource.width);
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = resource.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view.format = VK_FORMAT_R8G8_UNORM;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
    VkImageView handle{};
    REQUIRE(venture::test::vk_call("vkCreateImageView", gpu.device, &view, nullptr, &handle) == VK_SUCCESS);
    venture::test::vk_call("vkDestroyImageView", gpu.device, handle, nullptr);
  }
  REQUIRE(xg::xrDestroySwapchain(chain) == XR_SUCCESS);
  // Clients can reuse the output chain even when this particular swapchain
  // did not request density-map resources. Preserve the inapplicable payload
  // and return every color image, without allocating or fabricating a map.
  for (bool with_hint : {false, true}) {
    hint.flags = 0;
    create.next = with_hint ? &hint : nullptr;
    REQUIRE(xg::xrCreateSwapchain(session, &create, &chain) == XR_SUCCESS);
    for (std::uint32_t i = 0; i < count; ++i) {
      images[i].image = VK_NULL_HANDLE;
      resources[i].image = VK_NULL_HANDLE;
      resources[i].width = 17;
      resources[i].height = 19;
    }
    REQUIRE(xg::xrEnumerateSwapchainImages(chain, count, &count,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())) == XR_SUCCESS);
    for (std::uint32_t i = 0; i < count; ++i) {
      CHECK(images[i].image != VK_NULL_HANDLE);
      CHECK(images[i].next == &resources[i]);
      CHECK(resources[i].image == VK_NULL_HANDLE);
      CHECK(resources[i].width == 17);
      CHECK(resources[i].height == 19);
    }
    REQUIRE(xg::xrDestroySwapchain(chain) == XR_SUCCESS);
  }
  REQUIRE(xg::xrDestroySession(session) == XR_SUCCESS);
  REQUIRE(xg::xrDestroyInstance(instance) == XR_SUCCESS);
}

TEST_CASE("XR guest lifecycle and real Vulkan swapchain through mock loader") {
  REQUIRE(venture::xr::host_xr().available);
  CHECK(venture::xr::host_xr().runtime_name == "Venture Mock OpenXR");
  const char *extensions[]{
      "XR_KHR_vulkan_enable2",     "XR_FB_display_refresh_rate",    "XR_FB_color_space",
      "XR_FB_foveation",           "XR_FB_foveation_configuration", "XR_FB_swapchain_update_state",
      "XR_EXT_hand_tracking", "XR_FB_touch_controller_pro", "XR_KHR_composition_layer_equirect2", "XR_KHR_composition_layer_cylinder"};
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 2, 0);
  ci.enabledExtensionCount = std::size(extensions);
  ci.enabledExtensionNames = extensions;
  XrInstance instance{};
  CHECK(xg::xrCreateInstance(&ci, &instance) == XR_ERROR_API_VERSION_UNSUPPORTED);
  ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 1, 0);
  REQUIRE(xg::xrCreateInstance(&ci, &instance) == XR_SUCCESS);
  PFN_xrVoidFunction thunk{}, again{};
  CHECK(xg::xrGetInstanceProcAddr(instance, "xrLocateSpaces", &again) == XR_SUCCESS);
  CHECK(again != nullptr);
  CHECK(xg::xrGetInstanceProcAddr(instance, "xrWaitFrame", &thunk) == XR_SUCCESS);
  CHECK(xg::xrGetInstanceProcAddr(instance, "xrWaitFrame", &again) == XR_SUCCESS);
  CHECK(thunk == again);
  REQUIRE(thunk);
  // Guest action dispatch below exercises thunk execution and return; the
  // internal instruction count can change to support native instrumentation.
  CHECK(xg::xrGetInstanceProcAddr(instance, "xrCreatePassthroughFB", &again) ==
        XR_ERROR_FUNCTION_UNSUPPORTED);
  CHECK(again == nullptr);
  CHECK(xg::xrGetInstanceProcAddr(instance, "xrCreateFoveationProfileFB", &again) ==
        XR_SUCCESS);
  CHECK(again != nullptr);
  XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
  CHECK(xg::xrGetInstanceProperties(instance, &ip) == XR_SUCCESS);
  CHECK(std::string(ip.runtimeName) == "Oculus");
  XrSystemHandTrackingPropertiesEXT duplicate{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
  duplicate.supportsHandTracking = 0x12345678;
  XrSystemHandTrackingPropertiesEXT handProperties{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT, &duplicate};
  struct UnknownProperty {
    XrStructureType type;
    void* next;
    std::uint64_t sentinel;
  } unknown{static_cast<XrStructureType>(0x6fff1234), &handProperties, 0xabcdef1234567890ULL};
  XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES, &unknown};
  REQUIRE(xg::xrGetSystemProperties(instance, venture::xr::host_xr().system, &systemProperties) == XR_SUCCESS);
  CHECK(handProperties.supportsHandTracking == XR_TRUE);
  CHECK(duplicate.supportsHandTracking == 0x12345678);
  CHECK(unknown.sentinel == 0xabcdef1234567890ULL);
  CHECK(unknown.next == &handProperties);
  CHECK(handProperties.next == &duplicate);
  auto &gpu = venture::gfx::gpu_device();
  XrGraphicsBindingVulkanKHR b{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR,
                               nullptr,
                               gpu.instance,
                               gpu.physical_device,
                               gpu.device,
                               gpu.queue_family,
                               gpu.queue_index};
  XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO, &b, 0, venture::xr::host_xr().system};
  XrSession session{};
  REQUIRE(xg::xrCreateSession(instance, &si, &session) == XR_SUCCESS);
  XrActionSetCreateInfo asi{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strcpy(asi.actionSetName, "test");
  std::strcpy(asi.localizedActionSetName, "Test");
  XrActionSet action_set{};
  REQUIRE(xg::xrCreateActionSet(instance, &asi, &action_set) == XR_SUCCESS);
  XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
  ai.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
  std::strcpy(ai.actionName, "left_x");
  std::strcpy(ai.localizedActionName, "Left X");
  XrAction action{};
  REQUIRE(xg::xrCreateAction(action_set, &ai, &action) == XR_SUCCESS);
  XrPath touch_profile{}, source{}, hand{};
  REQUIRE(xg::xrStringToPath(instance, "/interaction_profiles/facebook/touch_controller_pro",
                             &touch_profile) == XR_SUCCESS);
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/left/input/x/click", &source) == XR_SUCCESS);
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/left", &hand) == XR_SUCCESS);
  XrActionSuggestedBinding binding{action, source};
  XrPath right{};
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/right", &right) == XR_SUCCESS);
  XrPath hands[]{hand, right};
  auto create_action = [&](const char* name, XrActionType type) {
    XrActionCreateInfo create{XR_TYPE_ACTION_CREATE_INFO};
    std::strcpy(create.actionName, name);
    std::strcpy(create.localizedActionName, name);
    create.actionType = type;
    create.countSubactionPaths = 2;
    create.subactionPaths = hands;
    XrAction result{};
    REQUIRE(xg::xrCreateAction(action_set, &create, &result) == XR_SUCCESS);
    return result;
  };
  auto trigger = create_action("trigger", XR_ACTION_TYPE_FLOAT_INPUT);
  auto stick = create_action("stick", XR_ACTION_TYPE_VECTOR2F_INPUT);
  auto aim = create_action("aim", XR_ACTION_TYPE_POSE_INPUT);
  std::vector<XrActionSuggestedBinding> input_bindings{binding};
  for (const auto* side : {"left", "right"})
    for (const auto& [action, suffix] : std::initializer_list<std::pair<XrAction, const char*>>{
             {trigger, "trigger/value"}, {stick, "thumbstick"}, {aim, "aim/pose"}}) {
      XrPath path{};
      REQUIRE(xg::xrStringToPath(instance, (std::string("/user/hand/") + side + "/input/" + suffix).c_str(), &path) == XR_SUCCESS);
      input_bindings.push_back({action, path});
    }
  XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
                                                  nullptr, touch_profile, std::uint32_t(input_bindings.size()), input_bindings.data()};
  REQUIRE(xg::xrSuggestInteractionProfileBindings(instance, &suggestion) == XR_SUCCESS);
  XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1,
                                       &action_set};
  REQUIRE(xg::xrAttachSessionActionSets(session, &attach) == XR_SUCCESS);
  XrInteractionProfileState current{XR_TYPE_INTERACTION_PROFILE_STATE};
  REQUIRE(xg::xrGetCurrentInteractionProfile(session, hand, &current) == XR_SUCCESS);
  CHECK(current.interactionProfile == touch_profile);
  XrBoundSourcesForActionEnumerateInfo bound{XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO,
                                             nullptr, action};
  XrPath actual{};
  std::uint32_t source_count = 0;
  REQUIRE(xg::xrEnumerateBoundSourcesForAction(session, &bound, 1, &source_count, &actual) ==
          XR_SUCCESS);
  CHECK(actual == source);
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  while (xg::xrPollEvent(instance, &event) == XR_SUCCESS)
    event = {XR_TYPE_EVENT_DATA_BUFFER};
  XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO, nullptr,
                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
  REQUIRE(xg::xrBeginSession(session, &bi) == XR_SUCCESS);
  event = {XR_TYPE_EVENT_DATA_BUFFER};
  while (xg::xrPollEvent(instance, &event) == XR_SUCCESS)
    event = {XR_TYPE_EVENT_DATA_BUFFER};
  XrActiveActionSet active_set{action_set, XR_NULL_PATH};
  XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active_set};
  REQUIRE(xg::xrSyncActions(session, &sync) == XR_SUCCESS);
  XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, trigger, right};
  XrActionStateFloat trigger_state{XR_TYPE_ACTION_STATE_FLOAT};
  REQUIRE(xg::xrGetActionStateFloat(session, &get, &trigger_state) == XR_SUCCESS);
  CHECK(trigger_state.isActive);
  CHECK(trigger_state.currentState == doctest::Approx(0.25));
  MockXrScript input;
  input.left.trigger = 0.1f;
  input.left.primary = true;
  input.right.trigger = 0.9f;
  input.right.thumbstick = {-0.4f, 0.8f};
  input.right.aim = {{0, 0, 0, 1}, {0.4f, 1.2f, -0.6f}};
  ventureMockXrSetScript(&input);
  REQUIRE(xg::xrGetActionStateFloat(session, &get, &trigger_state) == XR_SUCCESS);
  CHECK(trigger_state.currentState == doctest::Approx(0.25)); // Frozen until sync.
  REQUIRE(xg::xrSyncActions(session, &sync) == XR_SUCCESS);
  REQUIRE(xg::xrGetActionStateFloat(session, &get, &trigger_state) == XR_SUCCESS);
  CHECK(trigger_state.currentState == doctest::Approx(0.9));
  CHECK(trigger_state.changedSinceLastSync);
  CHECK(trigger_state.lastChangeTime > 0);
  const auto changed_at = trigger_state.lastChangeTime;
  get.subactionPath = hand;
  REQUIRE(xg::xrGetActionStateFloat(session, &get, &trigger_state) == XR_SUCCESS);
  CHECK(trigger_state.currentState == doctest::Approx(0.1));
  get = {XR_TYPE_ACTION_STATE_GET_INFO, nullptr, stick, right};
  XrActionStateVector2f vector{XR_TYPE_ACTION_STATE_VECTOR2F};
  REQUIRE(xg::xrGetActionStateVector2f(session, &get, &vector) == XR_SUCCESS);
  CHECK(vector.currentState.x == doctest::Approx(-0.4));
  CHECK(vector.currentState.y == doctest::Approx(0.8));
  XrActionStateBoolean button{XR_TYPE_ACTION_STATE_BOOLEAN};
  get = {XR_TYPE_ACTION_STATE_GET_INFO, nullptr, action, XR_NULL_PATH};
  REQUIRE(xg::xrGetActionStateBoolean(session, &get, &button) == XR_SUCCESS);
  CHECK(button.currentState);
  CHECK(button.changedSinceLastSync);
  auto& guest_thread = venture::GuestThread::adopt_current();
  auto& cpu = guest_thread.cpu();
  auto fast_action = [&](const char* name, void* out) {
    auto thunk = ox::command_thunk(name);
    REQUIRE(thunk != 0);
    cpu.set_x(0, venture::h2g(session));
    cpu.set_x(1, venture::h2g(&get));
    cpu.set_x(2, venture::h2g(out));
    cpu.set_lr(venture::hle::return_to_host_trampoline());
    cpu.set_pc(thunk);
    CHECK(cpu.run() == venture::CpuExit::Svc);
    CHECK(cpu.svc_immediate() == venture::kSvcReturnToHost);
    return static_cast<XrResult>(std::int32_t(cpu.x(0)));
  };
  button.currentState = XR_FALSE;
  REQUIRE(fast_action("xrGetActionStateBoolean", &button) == XR_SUCCESS);
  CHECK(button.currentState);
  CHECK(button.changedSinceLastSync);
  get = {XR_TYPE_ACTION_STATE_GET_INFO, nullptr, trigger, right};
  CHECK(fast_action("xrGetActionStateBoolean", &button) == XR_ERROR_ACTION_TYPE_MISMATCH);
  trigger_state.currentState = 0;
  REQUIRE(fast_action("xrGetActionStateFloat", &trigger_state) == XR_SUCCESS);
  CHECK(trigger_state.currentState == doctest::Approx(0.9));
  CHECK(trigger_state.changedSinceLastSync);
  CHECK(trigger_state.lastChangeTime == changed_at);
  CHECK(fast_action("xrGetActionStateFloat", nullptr) == XR_ERROR_VALIDATION_FAILURE);
  // A lifecycle owner must never be waited on inside the callback/gate.
  std::atomic<bool> locked = false, released = false;
  std::thread owner([&] {
    std::lock_guard lock(ox::state().mutex);
    locked.store(true, std::memory_order_release);
    while (!released.load(std::memory_order_acquire)) std::this_thread::yield();
  });
  while (!locked.load(std::memory_order_acquire)) std::this_thread::yield();
  for (const auto* name : {"xrGetActionStateBoolean", "xrGetActionStateFloat"}) {
    cpu.set_x(0, venture::h2g(session));
    cpu.set_x(1, venture::h2g(&get));
    cpu.set_x(2, 0);
    cpu.set_pc(ox::command_thunk(name));
    CHECK(cpu.run() == venture::CpuExit::Svc);
    CHECK(cpu.svc_immediate() == venture::kSvcHleCallFastFp);
    CHECK(cpu.x(0) == venture::h2g(session));
    CHECK(cpu.x(1) == venture::h2g(&get));
    CHECK(cpu.x(2) == 0);
  }
  released.store(true, std::memory_order_release);
  owner.join();
  REQUIRE(xg::xrSyncActions(session, &sync) == XR_SUCCESS);
  REQUIRE(xg::xrGetActionStateFloat(session, &get, &trigger_state) == XR_SUCCESS);
  CHECK_FALSE(trigger_state.changedSinceLastSync);
  CHECK(trigger_state.lastChangeTime == changed_at);
  XrActionSpaceCreateInfo pose_info{XR_TYPE_ACTION_SPACE_CREATE_INFO, nullptr, aim, right,
                                   {{0, 0, 0, 1}, {0, 0, -0.1f}}};
  XrSpace aim_space{}, local_space{};
  REQUIRE(xg::xrCreateActionSpace(session, &pose_info, &aim_space) == XR_SUCCESS);
  XrReferenceSpaceCreateInfo local_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr,
                                      XR_REFERENCE_SPACE_TYPE_LOCAL, {{0, 0, 0, 1}, {0, 0, 0}}};
  REQUIRE(xg::xrCreateReferenceSpace(session, &local_info, &local_space) == XR_SUCCESS);
  XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
  REQUIRE(xg::xrLocateSpace(aim_space, local_space, venture::monotonic_ns(), &location) == XR_SUCCESS);
  CHECK(location.locationFlags == 15);
  CHECK(location.pose.position.x == doctest::Approx(0.4));
  CHECK(location.pose.position.y == doctest::Approx(-0.4));
  CHECK(location.pose.position.z == doctest::Approx(-0.7));
  XrSpace stage_space{}, view_space{}, floor_space{};
  local_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR;
  REQUIRE(xg::xrCreateReferenceSpace(session, &local_info, &floor_space) == XR_SUCCESS);
  REQUIRE(xg::xrLocateSpace(floor_space, local_space, venture::monotonic_ns(), &location) == XR_SUCCESS);
  CHECK(location.pose.position.y == doctest::Approx(-1.6));
  local_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
  REQUIRE(xg::xrCreateReferenceSpace(session, &local_info, &stage_space) == XR_SUCCESS);
  local_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  REQUIRE(xg::xrCreateReferenceSpace(session, &local_info, &view_space) == XR_SUCCESS);
  REQUIRE(xg::xrLocateSpace(view_space, local_space, venture::monotonic_ns(), &location) == XR_SUCCESS);
  CHECK(location.pose.position.y == doctest::Approx(0));
  REQUIRE(xg::xrLocateSpace(view_space, stage_space, venture::monotonic_ns(), &location) == XR_SUCCESS);
  CHECK(location.pose.position.y == doctest::Approx(1.6));
  REQUIRE(xg::xrLocateSpace(aim_space, stage_space, venture::monotonic_ns(), &location) == XR_SUCCESS);
  CHECK(location.pose.position.y == doctest::Approx(1.2));
  input.head.position.y += 0.1f;
  ventureMockXrSetScript(&input);
  REQUIRE(xg::xrLocateSpace(view_space, local_space, venture::monotonic_ns(), &location) == XR_SUCCESS);
  CHECK(location.pose.position.y == doctest::Approx(0.1)); // LOCAL stays world-locked.
  // Velocities the runtime does not report reach the guest as zero, never NaN (xr_no_velocity).
  XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
  location.next = &velocity;
  REQUIRE(xg::xrLocateSpace(view_space, local_space, venture::monotonic_ns(), &location) == XR_SUCCESS);
  location.next = nullptr;
  for (auto v : {velocity.linearVelocity, velocity.angularVelocity})
    CHECK((std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z)));
  if (std::getenv("VENTURE_MOCK_NO_VELOCITY")) {
    CHECK(velocity.velocityFlags == 0);
    CHECK(velocity.linearVelocity.x == 0);
    CHECK(velocity.angularVelocity.y == 0);
  }
  REQUIRE(xg::xrLocateSpace(view_space, floor_space, venture::monotonic_ns(), &location) == XR_SUCCESS);
  CHECK(location.pose.position.y == doctest::Approx(1.7));

  // App recentering creates an offset LOCAL space; it must neither move the
  // floor nor fold a STAGE offset into the calibrated eye-level origin.
  const auto before_recenter = input;
  input.head.position.x += 0.35f;
  input.head.position.z -= 0.25f;
  input.head.orientation = {0, std::sin(0.35f), 0, std::cos(0.35f)};
  ventureMockXrSetScript(&input);
  REQUIRE(xg::xrLocateSpace(view_space, local_space, venture::monotonic_ns(), &location) == XR_SUCCESS);
  XrReferenceSpaceCreateInfo recenter_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr,
                                         XR_REFERENCE_SPACE_TYPE_LOCAL, location.pose};
  XrSpace recentered{}, offset_stage{};
  REQUIRE(xg::xrCreateReferenceSpace(session, &recenter_info, &recentered) == XR_SUCCESS);
  REQUIRE(xg::xrLocateSpace(view_space, recentered, venture::monotonic_ns(), &location) == XR_SUCCESS);
  CHECK(location.pose.position.x == doctest::Approx(0));
  CHECK(location.pose.position.y == doctest::Approx(0));
  CHECK(location.pose.position.z == doctest::Approx(0));
  CHECK(location.pose.orientation.y == doctest::Approx(0));
  CHECK(location.pose.orientation.w == doctest::Approx(1));
  REQUIRE(xg::xrLocateSpace(view_space, stage_space, venture::monotonic_ns(), &location) == XR_SUCCESS);
  CHECK(location.pose.position.y == doctest::Approx(1.7));
  recenter_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
  recenter_info.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0.25f, 0}};
  REQUIRE(xg::xrCreateReferenceSpace(session, &recenter_info, &offset_stage) == XR_SUCCESS);
  REQUIRE(xg::xrLocateSpace(view_space, offset_stage, venture::monotonic_ns(), &location) == XR_SUCCESS);
  CHECK(location.pose.position.y == doctest::Approx(1.45));
  REQUIRE(xg::xrLocateSpace(stage_space, local_space, venture::monotonic_ns(), &location) == XR_SUCCESS);
  CHECK(location.pose.position.y == doctest::Approx(-1.6));
  CHECK(xg::xrDestroySpace(recentered) == XR_SUCCESS);
  CHECK(xg::xrDestroySpace(offset_stage) == XR_SUCCESS);
  ventureMockXrSetScript(&before_recenter);
  CHECK(xg::xrDestroySpace(floor_space) == XR_SUCCESS);
  CHECK(xg::xrDestroySpace(stage_space) == XR_SUCCESS);
  CHECK(xg::xrDestroySpace(view_space) == XR_SUCCESS);
  sync.countActiveActionSets = 0;
  REQUIRE(xg::xrSyncActions(session, &sync) == XR_SUCCESS);
  REQUIRE(xg::xrGetActionStateFloat(session, &get, &trigger_state) == XR_SUCCESS);
  CHECK_FALSE(trigger_state.isActive);
  CHECK(trigger_state.currentState == 0);
  REQUIRE(xg::xrLocateSpace(aim_space, local_space, venture::monotonic_ns(), &location) == XR_SUCCESS);
  CHECK(location.locationFlags == 0);
  CHECK(xg::xrDestroySpace(aim_space) == XR_SUCCESS);
  CHECK(xg::xrDestroySpace(local_space) == XR_SUCCESS);
  {
    RefreshHost host;
    float current = 0, listed = 0;
    std::uint32_t count = 0;
    CHECK(xg::xrGetDisplayRefreshRateFB(session, &current) == XR_SUCCESS);
    CHECK(current == doctest::Approx(144));
    RefreshHost::query_result = XR_ERROR_SESSION_NOT_RUNNING;
    current = 0;
    CHECK(xg::xrGetDisplayRefreshRateFB(session, &current) == XR_SUCCESS);
    CHECK(current == doctest::Approx(144));
    RefreshHost::query_result = XR_ERROR_SESSION_LOST;
    CHECK(xg::xrGetDisplayRefreshRateFB(session, &current) == XR_ERROR_SESSION_LOST);
    RefreshHost::query_result = XR_SUCCESS;
    CHECK(xg::xrEnumerateDisplayRefreshRatesFB(session, 1, &count, &listed) == XR_SUCCESS);
    CHECK(count == 1);
    CHECK(listed == current);
    CHECK(xg::xrRequestDisplayRefreshRateFB(session, 0) == XR_SUCCESS);
    CHECK(xg::xrRequestDisplayRefreshRateFB(session, 144) == XR_SUCCESS);
    CHECK(xg::xrRequestDisplayRefreshRateFB(session, 72) == XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB);
    CHECK(RefreshHost::requests == 0);
    CHECK(RefreshHost::enumerations == 0);
  }
  venture::xr::session().display_timing.reset();
  MockXrScript original;
  ventureMockXrSetScript(&original);
  XrFrameState frame{XR_TYPE_FRAME_STATE};
  REQUIRE(xg::xrWaitFrame(session, nullptr, &frame) == XR_SUCCESS);
  float rate = 0;
  CHECK(xg::xrGetDisplayRefreshRateFB(session, &rate) == XR_SUCCESS);
  CHECK(rate == doctest::Approx(90));
  CHECK(xg::xrRequestDisplayRefreshRateFB(session, 72) ==
        XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB);
  std::uint32_t color_count = 0;
  REQUIRE(xg::xrEnumerateColorSpacesFB(session, 0, &color_count, nullptr) == XR_SUCCESS);
  REQUIRE(color_count == venture::xr::color_spaces.size());
  std::array<XrColorSpaceFB, 8> colors{};
  colors.fill(XR_COLOR_SPACE_MAX_ENUM_FB);
  CHECK(xg::xrEnumerateColorSpacesFB(session, 1, &color_count, colors.data()) == XR_ERROR_SIZE_INSUFFICIENT);
  CHECK(colors[0] == XR_COLOR_SPACE_MAX_ENUM_FB);
  REQUIRE(xg::xrEnumerateColorSpacesFB(session, colors.size(), &color_count, colors.data()) == XR_SUCCESS);
  CHECK(colors == venture::xr::color_spaces);
  for (auto color : colors) CHECK(xg::xrSetColorSpaceFB(session, color) == XR_SUCCESS);
  CHECK(xg::xrSetColorSpaceFB(session, XR_COLOR_SPACE_MAX_ENUM_FB) == XR_ERROR_COLOR_SPACE_UNSUPPORTED_FB);
  CHECK(xg::xrSetColorSpaceFB(session, XR_COLOR_SPACE_REC2020_FB) == XR_SUCCESS);
  XrSystemColorSpacePropertiesFB display_color{XR_TYPE_SYSTEM_COLOR_SPACE_PROPERTIES_FB};
  XrSystemProperties display_properties{XR_TYPE_SYSTEM_PROPERTIES, &display_color};
  REQUIRE(xg::xrGetSystemProperties(instance, venture::xr::host_xr().system, &display_properties) == XR_SUCCESS);
  CHECK(display_color.colorSpace == XR_COLOR_SPACE_REC709_FB);

  CHECK(xg::xrSetColorSpaceFB(session, XR_COLOR_SPACE_REC709_FB) == XR_SUCCESS);
  REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_SUCCESS);
  XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  sci.format = VK_FORMAT_R8G8B8A8_UNORM;
  sci.width = 64;
  sci.height = 64;
  sci.sampleCount = sci.faceCount = sci.arraySize = sci.mipCount = 1;
  XrSwapchain chain{};
  XrBaseInStructure unknownImageChain{XR_STRUCTURE_TYPE_MAX_ENUM};
  unknownImageChain.next = &unknownImageChain;
  sci.next = &unknownImageChain;
  CHECK(xg::xrCreateSwapchain(session, &sci, &chain) == XR_ERROR_VALIDATION_FAILURE);
  CHECK(chain == XR_NULL_HANDLE);
  unknownImageChain.next = nullptr;
  {
    // The host lists no BGRA order (SteamVR on AMD lists no RGBA UNORM); a
    // Quest takes it, and the host chain converts.
    auto bgra = sci;
    bgra.next = nullptr;
    bgra.format = VK_FORMAT_B8G8R8A8_UNORM;
    XrSwapchain other{};
    REQUIRE(xg::xrCreateSwapchain(session, &bgra, &other) == XR_SUCCESS);
    CHECK(xg::xrDestroySwapchain(other) == XR_SUCCESS);
    bgra.format = VK_FORMAT_R16G16_UNORM;
    CHECK(xg::xrCreateSwapchain(session, &bgra, &other) == XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED);
  }
  XrSwapchainCreateInfoFoveationFB hint{XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB};
  hint.flags = XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB;
  hint.next = &unknownImageChain;
  sci.next = &hint;
  REQUIRE(xg::xrCreateSwapchain(session, &sci, &chain) == XR_SUCCESS);
  CHECK(sci.next == &hint);
  CHECK(hint.next == &unknownImageChain);
  CHECK(hint.flags == XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB);
  CHECK(ventureMockXrStats().live_images == 0); // guest-only backing has no host XR images
  XrSwapchainImageVulkanKHR images[3]{{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR},
                                      {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR},
                                      {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR}};
  std::uint32_t count{};
  // OVRPlugin appends these even without negotiating XR_FB_foveation_vulkan.
  // Return the real color images and leave unnegotiated output fields alone.
  XrSwapchainImageFoveationVulkanFB density[3]{};
  for (auto& image : density) {
    image.type = XR_TYPE_SWAPCHAIN_IMAGE_FOVEATION_VULKAN_FB;
    image.width = 17;
    image.height = 19;
  }
  for (std::size_t i = 0; i < 3; ++i) images[i].next = &density[i];
  density[0].next = &density[0];
  CHECK(xg::xrEnumerateSwapchainImages(chain, 3, &count,
          reinterpret_cast<XrSwapchainImageBaseHeader*>(images)) == XR_ERROR_VALIDATION_FAILURE);
  density[0].next = &unknownImageChain;
  CHECK(xg::xrEnumerateSwapchainImages(chain, 3, &count,
                                       reinterpret_cast<XrSwapchainImageBaseHeader *>(images)) ==
        XR_SUCCESS);
  CHECK(images[0].image != VK_NULL_HANDLE);
  // The guest resolves into the runtime's eye images without creating them;
  // the Vulkan layer's attachment clamp needs their extents.
  for (const auto& image : images) {
    CHECK(venture::vulkan::attachment_extent(image.image).width == 64);
    CHECK(venture::vulkan::attachment_extent(image.image).height == 64);
  }
  for (std::size_t i = 0; i < 3; ++i) {
    CHECK(images[i].next == &density[i]);
    CHECK(density[i].type == XR_TYPE_SWAPCHAIN_IMAGE_FOVEATION_VULKAN_FB);
    CHECK(density[i].width == 17);
    CHECK(density[i].height == 19);
    CHECK(density[i].image == VK_NULL_HANDLE);
  }
  CHECK(density[0].next == &unknownImageChain);
  std::uint32_t index{};
  REQUIRE(xg::xrAcquireSwapchainImage(chain, nullptr, &index) == XR_SUCCESS);
  CHECK(index < count);
  CHECK(xg::xrReleaseSwapchainImage(chain, nullptr) == XR_ERROR_CALL_ORDER_INVALID);
  XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
  REQUIRE(xg::xrWaitSwapchainImage(chain, &wi) == XR_SUCCESS);
  REQUIRE(xg::xrReleaseSwapchainImage(chain, nullptr) == XR_SUCCESS);
  XrFoveationProfileCreateInfoFB fi{XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB};
  XrFoveationProfileFB profile{};
  CHECK(xg::xrCreateFoveationProfileFB(session, &fi, &profile) == XR_SUCCESS);
  XrSwapchainStateFoveationFB f{XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB};
  f.profile = profile;
  CHECK(xg::xrUpdateSwapchainFB(chain, reinterpret_cast<XrSwapchainStateBaseHeaderFB *>(&f)) ==
        XR_SUCCESS);
  f.profile = {};
  CHECK(xg::xrGetSwapchainStateFB(chain, reinterpret_cast<XrSwapchainStateBaseHeaderFB *>(&f)) ==
        XR_SUCCESS);
  CHECK(f.profile == profile);
  CHECK(xg::xrDestroyFoveationProfileFB(profile) == XR_SUCCESS);
  XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
  end.displayTime = frame.predictedDisplayTime;
  end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR;
  spaceInfo.poseInReferenceSpace.orientation.w = 1;
  XrSpace space{};
  REQUIRE(xg::xrCreateReferenceSpace(session, &spaceInfo, &space) == XR_SUCCESS);
  XrSpaceLocationData located{};
  XrSpaceLocations locations{XR_TYPE_SPACE_LOCATIONS, nullptr, 1, &located};
  XrSpacesLocateInfo locate{XR_TYPE_SPACES_LOCATE_INFO, nullptr, space,
                           frame.predictedDisplayTime, 1, &space};
  REQUIRE(xg::xrLocateSpaces(session, &locate, &locations) == XR_SUCCESS);
  CHECK(located.pose.position.y == 0);
  XrCompositionLayerEquirect2KHR sphere{XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR};
  sphere.space = space; sphere.pose.orientation.w = 1;
  sphere.subImage = {chain, {{0, 0}, {64, 64}}, 0};
  sphere.centralHorizontalAngle = 6.28f;
  sphere.lowerVerticalAngle = -1.57f; sphere.upperVerticalAngle = 1.57f;
  const auto* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&sphere);
  end.layerCount = 1; end.layers = &layer;
  sphere.radius = -1;
  CHECK(xg::xrEndFrame(session, &end) == XR_ERROR_LAYER_INVALID);
  sphere.radius = 0; // Infinite-distance panorama, as submitted by OVRPlugin.
  // Native errors retire the frame. Do not retry EndFrame with edited layers:
  // begin a fresh frame, allowing runtimes that discard the rejected one here.
  CHECK(xg::xrEndFrame(session, &end) == XR_ERROR_CALL_ORDER_INVALID);
  REQUIRE(xg::xrWaitFrame(session, nullptr, &frame) == XR_SUCCESS);
  REQUIRE(XR_SUCCEEDED(xg::xrBeginFrame(session, nullptr)));
  end.displayTime = frame.predictedDisplayTime;
  const auto previousSpheres = ventureMockXrStats().equirect;
  CHECK(xg::xrEndFrame(session, &end) == XR_SUCCESS);
  CHECK(ventureMockXrStats().equirect == previousSpheres + 1);
  CHECK(xg::xrEndFrame(session, &end) == XR_ERROR_CALL_ORDER_INVALID);
  // OpenXR Frame Submission allows synchronized Begin/End on separate threads.
  REQUIRE(xg::xrWaitFrame(session, nullptr, &frame) == XR_SUCCESS);
  REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_SUCCESS);
  auto threaded_empty = end;
  threaded_empty.displayTime = frame.predictedDisplayTime;
  threaded_empty.layerCount = 0;
  threaded_empty.layers = nullptr;
  XrResult threaded_end = XR_ERROR_RUNTIME_FAILURE;
  std::thread renderer([&] { threaded_end = xg::xrEndFrame(session, &threaded_empty); });
  renderer.join();
  REQUIRE(threaded_end == XR_SUCCESS);
  XrCompositionLayerCylinderKHR cylinder{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
  cylinder.space = space; cylinder.pose.orientation.w = 1;
  cylinder.subImage = sphere.subImage; cylinder.aspectRatio = 2; cylinder.centralAngle = 1;
  const auto* cylinder_layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cylinder);
  end.layers = &cylinder_layer;
  for (auto radius : {0.f, std::numeric_limits<float>::infinity()}) {
    REQUIRE(xg::xrWaitFrame(session, nullptr, &frame) == XR_SUCCESS);
    REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_SUCCESS);
    end.displayTime = frame.predictedDisplayTime;
    cylinder.radius = -1;
    CHECK(xg::xrEndFrame(session, &end) == XR_ERROR_LAYER_INVALID);
    REQUIRE(xg::xrWaitFrame(session, nullptr, &frame) == XR_SUCCESS);
    REQUIRE(XR_SUCCEEDED(xg::xrBeginFrame(session, nullptr)));
    end.displayTime = frame.predictedDisplayTime;
    cylinder.radius = radius;
    cylinder.centralAngle = 2*std::numbers::pi_v<float>;
    CHECK(xg::xrEndFrame(session, &end) == XR_ERROR_LAYER_INVALID);
    REQUIRE(xg::xrWaitFrame(session, nullptr, &frame) == XR_SUCCESS);
    REQUIRE(XR_SUCCEEDED(xg::xrBeginFrame(session, nullptr)));
    end.displayTime = frame.predictedDisplayTime;
    cylinder.centralAngle = 0;
    CHECK(xg::xrEndFrame(session, &end) == XR_SUCCESS);
  }
  end.layers = &layer;

  // A host without Equirect2 can display a full panorama through Equirect1.
  // Unsupported cropped layers must not discard the valid projection eyes.
  {
    auto& extensions = venture::xr::host_xr().enabled_extensions;
    struct Restore {
      std::set<std::string>& extensions;
      std::set<std::string>::node_type node;
      ~Restore() { extensions.insert(std::move(node)); }
    } restore{extensions, extensions.extract("XR_KHR_composition_layer_equirect2")};
    sphere.centralHorizontalAngle = 2*std::numbers::pi_v<float>;
    sphere.upperVerticalAngle = std::numbers::pi_v<float>/2;
    sphere.lowerVerticalAngle = -sphere.upperVerticalAngle;
    REQUIRE(xg::xrWaitFrame(session, nullptr, &frame) == XR_SUCCESS);
    REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_SUCCESS);
    end.displayTime = frame.predictedDisplayTime;
    CHECK(xg::xrEndFrame(session, &end) == XR_SUCCESS);
    CHECK(ventureMockXrStats().equirect == previousSpheres + 2);
    XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                             {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    for (auto& view : views) { view.subImage = sphere.subImage; view.pose.orientation.w = 1; }
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.space = space; projection.viewCount = 2; projection.views = views;
    const XrCompositionLayerBaseHeader* both[]{
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection), layer};
    sphere.centralHorizontalAngle = 1; // Needs clipping; explicitly omitted.
    end.layerCount = 2; end.layers = both;
    const auto beforeProjection = ventureMockXrStats().projection;
    REQUIRE(xg::xrWaitFrame(session, nullptr, &frame) == XR_SUCCESS);
    REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_SUCCESS);
    end.displayTime = frame.predictedDisplayTime;
    CHECK(xg::xrEndFrame(session, &end) == XR_SUCCESS);
    CHECK(ventureMockXrStats().projection == beforeProjection + 1);
    CHECK(ventureMockXrStats().equirect == previousSpheres + 2);
    {
      // A host without cylinders (SteamVR) draws one as strips of its arc.
      Restore cylinderRestore{extensions, extensions.extract("XR_KHR_composition_layer_cylinder")};
      CHECK(venture::hle::openxr::capabilities().contains("XR_KHR_composition_layer_cylinder"));
      cylinder.radius = 2; cylinder.centralAngle = 1;
      end.layerCount = 1; end.layers = &cylinder_layer;
      const auto quads = ventureMockXrStats().quad, cylinders = ventureMockXrStats().cylinder;
      REQUIRE(xg::xrWaitFrame(session, nullptr, &frame) == XR_SUCCESS);
      REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_SUCCESS);
      end.displayTime = frame.predictedDisplayTime;
      CHECK(xg::xrEndFrame(session, &end) == XR_SUCCESS);
      // One radian takes eight strips that tile the image side by side.
      CHECK(ventureMockXrStats().quad == quads + 8);
      CHECK(ventureMockXrStats().cylinder == cylinders);
      std::array<XrSwapchainSubImage, 9> slices{};
      REQUIRE(ventureMockXrLastSubImages(slices.data(), 9) == 8);
      auto x = cylinder.subImage.imageRect.offset.x;
      for (std::size_t s = 0; s < 8; ++s) {
        CHECK(slices[s].imageRect.offset.x == x);
        CHECK(slices[s].imageRect.extent.height == cylinder.subImage.imageRect.extent.height);
        x += slices[s].imageRect.extent.width;
      }
      CHECK(x == cylinder.subImage.imageRect.offset.x + cylinder.subImage.imageRect.extent.width);
      end.layerCount = 2; end.layers = both;
    }
    Restore colorRestore{extensions, extensions.extract("XR_KHR_composition_layer_color_scale_bias")};
    Restore layoutRestore{extensions, extensions.extract("XR_FB_composition_layer_image_layout")};
    auto submit = [&] {
      REQUIRE(xg::xrWaitFrame(session, nullptr, &frame) == XR_SUCCESS);
      REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_SUCCESS);
      end.displayTime = frame.predictedDisplayTime;
      return xg::xrEndFrame(session, &end);
    };
    // Unenabled extension nodes are ignored regardless of their payload.
    XrCompositionLayerColorScaleBiasKHR color{XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR};
    color.colorScale = {0.5f, 1, 1, 1};
    projection.next = &color;
    CHECK(submit() == XR_SUCCESS);
    CHECK(projection.next == &color);
    CHECK(color.colorScale.r == 0.5f);
    CHECK(ventureMockXrStats().projection == beforeProjection + 2);
    XrCompositionLayerImageLayoutFB layout{XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB};
    layout.flags = XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;
    projection.next = &layout;
    CHECK(submit() == XR_SUCCESS);
    CHECK(projection.next == &layout);
    CHECK(ventureMockXrStats().projection == beforeProjection + 3);
    color.next = &layout;
    projection.next = &color;
    CHECK(submit() == XR_SUCCESS);
    CHECK(projection.next == &color);
    CHECK(color.next == &layout);
    CHECK(ventureMockXrStats().projection == beforeProjection + 4);
    const XrCompositionLayerBaseHeader* twinProjections[]{
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
    end.layers = twinProjections;
    CHECK(submit() == XR_SUCCESS);
    CHECK(ventureMockXrStats().projection == beforeProjection + 6);
    // Job Simulator's OVRPlugin 1.88 also attaches this to its loading quads.
    // Multiple copied layers must remain valid and preserve the guest chain.
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.next = &layout;
    quad.space = space;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage = sphere.subImage;
    quad.pose.orientation.w = 1;
    quad.size = {1, 1};
    const XrCompositionLayerBaseHeader* mixed[]{
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad),
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
    end.layerCount = 3; end.layers = mixed;
    const auto beforeQuads = ventureMockXrStats().quad;
    CHECK(submit() == XR_SUCCESS);
    CHECK(quad.next == &layout);
    CHECK(layout.next == nullptr);
    CHECK(layout.flags == XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB);
    CHECK(ventureMockXrStats().quad == beforeQuads + 2);
    CHECK(ventureMockXrStats().projection == beforeProjection + 7);
    // Even ignored nodes must form a finite, acyclic chain.
    layout.next = &layout;
    CHECK(submit() == XR_ERROR_VALIDATION_FAILURE);
    layout.next = nullptr;
    CHECK(xg::xrEndFrame(session, &end) == XR_SUCCESS);
    end.layerCount = 2;
    end.layers = both;
  }
  end.layerCount = 0; end.layers = nullptr;
  CHECK(xg::xrDestroySpace(space) == XR_SUCCESS);
  // Wait N+1 before ending N. The state retains N's display time and allows
  // the render thread to finish while the simulation thread is pacing ahead.
  REQUIRE(xg::xrWaitFrame(session, nullptr, &frame) == XR_SUCCESS);
  REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_SUCCESS);
  MockXrScript changed{};
  changed.display_period = 10'000'000;
  ventureMockXrSetScript(&changed);
  XrFrameState next{XR_TYPE_FRAME_STATE};
  REQUIRE(xg::xrWaitFrame(session, nullptr, &next) == XR_SUCCESS);
  CHECK(next.predictedDisplayTime > frame.predictedDisplayTime);
  end.displayTime = frame.predictedDisplayTime;
  REQUIRE(xg::xrEndFrame(session, &end) == XR_SUCCESS);
  REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_SUCCESS);
  end.displayTime = next.predictedDisplayTime;
  REQUIRE(xg::xrEndFrame(session, &end) == XR_SUCCESS);
  event = {XR_TYPE_EVENT_DATA_BUFFER};
  CHECK(xg::xrPollEvent(instance, &event) == XR_EVENT_UNAVAILABLE);
  for (int i = 0; i < 29; ++i) {
    REQUIRE(xg::xrWaitFrame(session, nullptr, &next) == XR_SUCCESS);
    REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_SUCCESS);
    end.displayTime = next.predictedDisplayTime;
    REQUIRE(xg::xrEndFrame(session, &end) == XR_SUCCESS);
  }
  event = {XR_TYPE_EVENT_DATA_BUFFER};
  REQUIRE(xg::xrPollEvent(instance, &event) == XR_SUCCESS);
  CHECK(event.type == XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB);
  auto &change = reinterpret_cast<XrEventDataDisplayRefreshRateChangedFB &>(event);
  CHECK(change.fromDisplayRefreshRate == doctest::Approx(90));
  CHECK(change.toDisplayRefreshRate == doctest::Approx(100));
  // Both the guest bridge and the standalone mock must pace the next wait at
  // BeginFrame. Waiting for EndFrame would deadlock a pipelined engine.
  struct FrameApi {
    PFN_xrWaitFrame wait;
    PFN_xrBeginFrame begin;
    PFN_xrEndFrame end;
  };
  for (auto api : {FrameApi{xg::xrWaitFrame, xg::xrBeginFrame, xg::xrEndFrame},
                   FrameApi{::xrWaitFrame, ::xrBeginFrame, ::xrEndFrame}}) {
    XrFrameState first{XR_TYPE_FRAME_STATE}, following{XR_TYPE_FRAME_STATE};
    REQUIRE(api.wait(session, nullptr, &first) == XR_SUCCESS);
    std::promise<void> entered;
    auto started = entered.get_future();
    auto waiting = std::async(std::launch::async, [&] {
      entered.set_value();
      return api.wait(session, nullptr, &following);
    });
    started.wait();
    CHECK(waiting.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    const auto begun = api.begin(session, nullptr);
    const auto ready = waiting.wait_for(venture::test::hang_timeout);
    // Release a broken implementation before an assertion unwinds the future.
    if (XR_FAILED(begun) || ready != std::future_status::ready)
      xg::xrDestroySession(session);
    INFO("begin result=", begun, " pipelined waiter ready=", ready == std::future_status::ready);
    REQUIRE(begun == XR_SUCCESS);
    REQUIRE(ready == std::future_status::ready);
    REQUIRE(waiting.get() == XR_SUCCESS);
    CHECK(following.predictedDisplayTime > first.predictedDisplayTime);
    end.displayTime = first.predictedDisplayTime;
    REQUIRE(api.end(session, &end) == XR_SUCCESS);
    REQUIRE(api.begin(session, nullptr) == XR_SUCCESS);
    end.displayTime = following.predictedDisplayTime;
    REQUIRE(api.end(session, &end) == XR_SUCCESS);
  }
  MockXrScript restored{};
  ventureMockXrSetScript(&restored);
  auto static_info = sci;
  static_info.next = nullptr;
  static_info.createFlags = XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT;
  XrSwapchain static_chain{};
  CHECK(xg::xrCreateSwapchain(session, &static_info, &static_chain) == XR_ERROR_FEATURE_UNSUPPORTED);
  static_info.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
  REQUIRE(xg::xrCreateSwapchain(session, &static_info, &static_chain) == XR_SUCCESS);
  std::uint32_t static_count{};
  REQUIRE(xg::xrEnumerateSwapchainImages(static_chain, 0, &static_count, nullptr) == XR_SUCCESS);
  CHECK(static_count == 1);
  XrSwapchainImageVulkanKHR static_image{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR};
  REQUIRE(xg::xrEnumerateSwapchainImages(static_chain, 1, &static_count,
      reinterpret_cast<XrSwapchainImageBaseHeader*>(&static_image)) == XR_SUCCESS);
  CHECK(static_image.image != VK_NULL_HANDLE);
  std::uint32_t static_index{};
  REQUIRE(xg::xrAcquireSwapchainImage(static_chain, nullptr, &static_index) == XR_SUCCESS);
  CHECK(static_index == 0);
  XrSwapchainImageWaitInfo static_wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
  REQUIRE(xg::xrWaitSwapchainImage(static_chain, &static_wait) == XR_SUCCESS);
  REQUIRE(xg::xrReleaseSwapchainImage(static_chain, nullptr) == XR_SUCCESS);
  CHECK(xg::xrAcquireSwapchainImage(static_chain, nullptr, &static_index) == XR_ERROR_CALL_ORDER_INVALID);
  XrReferenceSpaceCreateInfo static_space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  static_space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
  static_space_info.poseInReferenceSpace.orientation.w = 1;
  XrSpace static_space{};
  REQUIRE(xg::xrCreateReferenceSpace(session, &static_space_info, &static_space) == XR_SUCCESS);
  XrCompositionLayerQuad static_quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  static_quad.space = static_space;
  static_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  static_quad.subImage.swapchain = static_chain;
  static_quad.subImage.imageRect.extent = {std::int32_t(static_info.width), std::int32_t(static_info.height)};
  static_quad.pose.orientation.w = 1;
  static_quad.size = {1, 1};
  const XrCompositionLayerBaseHeader* static_layers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&static_quad)};
  const auto static_quads_before = ventureMockXrStats().quad;
  for (int repeat = 0; repeat < 3; ++repeat) {
    REQUIRE(xg::xrWaitFrame(session, nullptr, &frame) == XR_SUCCESS);
    REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_SUCCESS);
    end.displayTime = frame.predictedDisplayTime;
    end.layerCount = 1; end.layers = static_layers;
    REQUIRE(xg::xrEndFrame(session, &end) == XR_SUCCESS);
  }
  CHECK(ventureMockXrStats().quad == static_quads_before + 3);
  CHECK(xg::xrDestroySpace(static_space) == XR_SUCCESS);
  CHECK(xg::xrDestroySwapchain(static_chain) == XR_SUCCESS);
  CHECK(xg::xrDestroySwapchain(chain) == XR_SUCCESS);
  CHECK(ventureMockXrStats().live_images == 0);
  for (const auto& image : images) CHECK(venture::vulkan::attachment_extent(image.image).width == 0);
  CHECK(xg::xrDestroySession(session) == XR_SUCCESS);
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
}

TEST_CASE("XR system-property output chains ignore unenabled nodes and retain bytes") {
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&create, &instance) == XR_SUCCESS);
  struct Unknown {
    XrStructureType type;
    void* next;
    std::uint64_t payload;
  } unknown{static_cast<XrStructureType>(123456), nullptr, 0xabcdef};
  XrSystemColorSpacePropertiesFB color{XR_TYPE_SYSTEM_COLOR_SPACE_PROPERTIES_FB,
                                      &unknown, XR_COLOR_SPACE_REC2020_FB};
  XrSystemHandTrackingPropertiesEXT hands{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT,
                                        &color, 1234};
  XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES, &hands};
  REQUIRE(xg::xrGetSystemProperties(instance, venture::xr::host_xr().system,
                                    &properties) == XR_SUCCESS);
  CHECK(properties.systemId == venture::xr::host_xr().system);
  CHECK(properties.next == &hands);
  CHECK(hands.next == &color);
  CHECK(hands.supportsHandTracking == 1234);
  CHECK(color.next == &unknown);
  CHECK(color.colorSpace == XR_COLOR_SPACE_REC2020_FB);
  CHECK(unknown.type == static_cast<XrStructureType>(123456));
  CHECK(unknown.next == nullptr);
  CHECK(unknown.payload == 0xabcdef);
  unknown.next = &hands;
  CHECK(xg::xrGetSystemProperties(instance, venture::xr::host_xr().system,
                                  &properties) == XR_ERROR_VALIDATION_FAILURE);
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
}

TEST_CASE("XR Android chains, GLES image copies, host-clock conversion and hands") {
  venture::test::XrImageFixture interop;
  venture::xr::install_xr_images(&interop);
  xg::AndroidInit init{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR, nullptr, 0x1234, 0x5678};
  CHECK(xg::xrInitializeLoaderKHR(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR *>(&init)) ==
        XR_SUCCESS);
  const char *extensions[]{
      "XR_KHR_android_create_instance", "XR_KHR_opengl_es_enable", "XR_KHR_convert_timespec_time",
      "XR_EXT_hand_tracking",           "XR_FB_hand_tracking_aim", "XR_FB_hand_tracking_capsules",
      "XR_FB_hand_tracking_mesh",       "XR_FB_color_space",       "XR_KHR_locate_spaces"};
  xg::AndroidInstance android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR, nullptr, 0x1234, 0x5678};
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO, &android};
  ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  ci.enabledExtensionCount = std::size(extensions);
  ci.enabledExtensionNames = extensions;
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&ci, &instance) == XR_SUCCESS);
  XrSystemColorSpacePropertiesFB color{XR_TYPE_SYSTEM_COLOR_SPACE_PROPERTIES_FB};
  XrSystemHandTrackingPropertiesEXT hands{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT, &color};
  XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES, &hands};
  REQUIRE(xg::xrGetSystemProperties(instance, venture::xr::host_xr().system, &props) == XR_SUCCESS);
  CHECK(hands.supportsHandTracking);
  CHECK(hands.next == &color);
  CHECK(color.colorSpace == XR_COLOR_SPACE_REC709_FB);
  XrBaseOutStructure unknown{static_cast<XrStructureType>(123456),
                             reinterpret_cast<XrBaseOutStructure*>(&hands)};
  props.next = &unknown;
  hands.supportsHandTracking = XR_FALSE;
  CHECK(xg::xrGetSystemProperties(instance, 1, &props) == XR_SUCCESS);
  CHECK(props.next == &unknown);
  CHECK(unknown.type == static_cast<XrStructureType>(123456));
  CHECK(unknown.next == reinterpret_cast<XrBaseOutStructure*>(&hands));
  CHECK(hands.supportsHandTracking == XR_TRUE);
  CHECK(hands.next == &color);
  xg::Timespec ts{venture::monotonic_ns() / 1'000'000'000, 123456789}, back{};
  XrTime time{};
  CHECK(xg::xrConvertTimespecTimeToTimeKHR(instance, &ts, &time) == XR_SUCCESS);
  CHECK(xg::xrConvertTimeToTimespecTimeKHR(instance, time, &back) == XR_SUCCESS);
  CHECK(back.sec == ts.sec);
  CHECK(back.nsec == ts.nsec);
  ts.nsec = 1'000'000'000;
  CHECK(xg::xrConvertTimespecTimeToTimeKHR(instance, &ts, &time) == XR_ERROR_TIME_INVALID);
  xg::GlesBinding binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR, nullptr, 1, 2, 3};
  XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO, &binding, 0, 1};
  XrSession session{};
  REQUIRE(xg::xrCreateSession(instance, &si, &session) == XR_SUCCESS);
  XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  sc.format = 0x8058;
  sc.width = sc.height = 32;
  sc.arraySize = sc.sampleCount = sc.mipCount = sc.faceCount = 1;
  sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  XrSwapchain chain{};
  XrSwapchainCreateInfoFoveationFB duplicateFoveation{XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB};
  duplicateFoveation.flags = UINT64_MAX;
  XrSwapchainCreateInfoFoveationFB disabledFoveation{XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB, &duplicateFoveation};
  disabledFoveation.flags = UINT64_MAX; // A disabled payload must never be read.
  struct UnknownSwapchainInfo {
    XrStructureType type;
    const void* next;
    std::uint64_t sentinel;
  } unknownSwapchain{static_cast<XrStructureType>(0x6fff1234), &disabledFoveation, UINT64_MAX};
  sc.next = &unknownSwapchain;
  duplicateFoveation.next = &unknownSwapchain;
  CHECK(xg::xrCreateSwapchain(session, &sc, &chain) == XR_ERROR_VALIDATION_FAILURE);
  CHECK(chain == XR_NULL_HANDLE);
  duplicateFoveation.next = &hands; // Enabled hand properties have the wrong parent.
  CHECK(xg::xrCreateSwapchain(session, &sc, &chain) == XR_ERROR_VALIDATION_FAILURE);
  duplicateFoveation.next = nullptr;
  sc.width = sc.height = 0;  // OVRPlugin's unnegotiated space-warp swapchains: rejected, never sent to the host
  CHECK(xg::xrCreateSwapchain(session, &sc, &chain) == XR_ERROR_VALIDATION_FAILURE);
  CHECK(chain == XR_NULL_HANDLE);
  sc.width = sc.height = 32;
  REQUIRE(xg::xrCreateSwapchain(session, &sc, &chain) == XR_SUCCESS);
  CHECK(sc.next == &unknownSwapchain);
  CHECK(unknownSwapchain.next == &disabledFoveation);
  CHECK(unknownSwapchain.sentinel == UINT64_MAX);
  CHECK(disabledFoveation.next == &duplicateFoveation);
  CHECK(disabledFoveation.flags == UINT64_MAX);
  CHECK(duplicateFoveation.next == nullptr);
  CHECK(duplicateFoveation.flags == UINT64_MAX);
  xg::GlesImage gl[3]{{XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR},
                      {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR},
                      {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR}};
  std::uint32_t count{};
  REQUIRE(xg::xrEnumerateSwapchainImages(
              chain, 3, &count, reinterpret_cast<XrSwapchainImageBaseHeader *>(gl)) == XR_SUCCESS);
  std::uint32_t acquired{};
  REQUIRE(xg::xrAcquireSwapchainImage(chain, nullptr, &acquired) == XR_SUCCESS);
  XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
  REQUIRE(xg::xrWaitSwapchainImage(chain, &wait) == XR_SUCCESS);
  REQUIRE(xg::xrReleaseSwapchainImage(chain, nullptr) == XR_SUCCESS);
  CHECK(interop.last_texture == gl[acquired].image);
  CHECK(interop.copies == 1);
  // A blocked destination fence must not serialize unrelated OpenXR calls.
  REQUIRE(xg::xrAcquireSwapchainImage(chain, nullptr, &acquired) == XR_SUCCESS);
  REQUIRE(xg::xrWaitSwapchainImage(chain, &wait) == XR_SUCCESS);
  std::promise<void> copying, resume;
  auto copying_started = copying.get_future();
  auto resumed = resume.get_future().share();
  interop.before_copy = [&] { copying.set_value(); resumed.wait(); };
  auto release = std::async(std::launch::async, [&] {
    return xg::xrReleaseSwapchainImage(chain, nullptr);
  });
  copying_started.wait();
  auto query = std::async(std::launch::async, [&] {
    std::uint32_t images = 0;
    return xg::xrEnumerateSwapchainImages(chain, 0, &images, nullptr);
  });
  const auto query_ready = query.wait_for(venture::test::hang_timeout);
  // Never leave a test worker blocked if a lock regression occurs.
  if (query_ready != std::future_status::ready) resume.set_value();
  INFO("enumeration ready while destination copy is blocked=", query_ready == std::future_status::ready);
  CHECK(query_ready == std::future_status::ready);
  CHECK(query.get() == XR_SUCCESS);
  if (query_ready == std::future_status::ready) {
    CHECK(xg::xrDestroySwapchain(chain) == XR_ERROR_CALL_ORDER_INVALID);
    CHECK(xg::xrDestroySession(session) == XR_ERROR_CALL_ORDER_INVALID);
    resume.set_value();
  }
  CHECK(release.get() == XR_SUCCESS);
  interop.before_copy = {};

  auto pixel = interop.read_pixel();
  CHECK(pixel[0] == 64);
  CHECK((pixel[1] == 127 || pixel[1] == 128));
  CHECK(pixel[2] == 191);
  CHECK(pixel[3] == 255); // 0.5 maps to the UNORM midpoint 127.5.
  std::uint32_t indices[3]{};
  for (auto &index : indices)
    REQUIRE(xg::xrAcquireSwapchainImage(chain, nullptr, &index) == XR_SUCCESS);
  CHECK(xg::xrAcquireSwapchainImage(chain, nullptr, &acquired) == XR_ERROR_CALL_ORDER_INVALID);
  for (auto index : indices) {
    REQUIRE(xg::xrWaitSwapchainImage(chain, &wait) == XR_SUCCESS);
    CHECK(xg::xrWaitSwapchainImage(chain, &wait) == XR_ERROR_CALL_ORDER_INVALID);
    REQUIRE(xg::xrReleaseSwapchainImage(chain, nullptr) == XR_SUCCESS);
    CHECK(interop.last_texture == gl[index].image);
  }
  XrReferenceSpaceCreateInfo ri{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  ri.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  ri.poseInReferenceSpace.orientation.w = 1;
  XrSpace local{};
  REQUIRE(xg::xrCreateReferenceSpace(session, &ri, &local) == XR_SUCCESS);
  XrSpacesLocateInfo batch{XR_TYPE_SPACES_LOCATE_INFO, nullptr, local, time, 1, &local};
  XrSpaceLocationData location{};
  XrSpaceVelocityData speed{};
  XrSpaceVelocities speeds{XR_TYPE_SPACE_VELOCITIES, nullptr, 1, &speed};
  XrSpaceLocations batch_out{XR_TYPE_SPACE_LOCATIONS, &speeds, 1, &location};
  REQUIRE(xg::xrLocateSpacesKHR(session, &batch, &batch_out) == XR_SUCCESS);
  CHECK(location.locationFlags == 15);
  CHECK(speed.velocityFlags == 3);
  // OVRPlugin supplies this unrecognized optional structure. Its
  // presence must not disable the otherwise supported hand-tracking path.
  struct OptionalHandInfo {
    XrStructureType type;
    const void* next;
    std::uint64_t sentinel;
  } optionalHand{static_cast<XrStructureType>(1000539000), nullptr, 0xabcdef1234567890ULL};
  XrHandTrackerCreateInfoEXT hi{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT, &optionalHand, XR_HAND_LEFT_EXT,
                                XR_HAND_JOINT_SET_DEFAULT_EXT};
  XrHandTrackerEXT tracker{};
  // OVRPlugin 1.88 supplies this private, unrecognized next-chain node.
  XrBaseInStructure hand_hint{static_cast<XrStructureType>(1000294000), nullptr};
  hi.next = &hand_hint;
  hand_hint.next = &hand_hint;
  CHECK(xg::xrCreateHandTrackerEXT(session, &hi, &tracker) == XR_ERROR_VALIDATION_FAILURE);
  hand_hint.next = nullptr;
  REQUIRE(xg::xrCreateHandTrackerEXT(session, &hi, &tracker) == XR_SUCCESS);
  CHECK(hi.next == &hand_hint);
  CHECK(hand_hint.next == nullptr);
  REQUIRE(xg::xrDestroyHandTrackerEXT(tracker) == XR_SUCCESS);
  tracker = XR_NULL_HANDLE;
  hi.next = &optionalHand;
  optionalHand.next = &optionalHand;
  CHECK(xg::xrCreateHandTrackerEXT(session, &hi, &tracker) == XR_ERROR_VALIDATION_FAILURE);
  optionalHand.next = &hands; // Recognized, enabled, but wrong parent remains invalid.
  CHECK(xg::xrCreateHandTrackerEXT(session, &hi, &tracker) == XR_ERROR_VALIDATION_FAILURE);
  XrHandTrackingDataSourceInfoEXT disabledSource{XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT};
  disabledSource.requestedDataSourceCount = UINT32_MAX; // Disabled payload must never be read.
  optionalHand.next = &disabledSource;
  disabledSource.next = &disabledSource;
  CHECK(xg::xrCreateHandTrackerEXT(session, &hi, &tracker) == XR_ERROR_VALIDATION_FAILURE);
  XrHandTrackingDataSourceInfoEXT duplicateSource{XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT};
  duplicateSource.requestedDataSourceCount = UINT32_MAX;
  disabledSource.next = &duplicateSource;
  REQUIRE(xg::xrCreateHandTrackerEXT(session, &hi, &tracker) == XR_SUCCESS);
  CHECK(optionalHand.sentinel == 0xabcdef1234567890ULL);
  CHECK(hi.next == &optionalHand);
  CHECK(optionalHand.next == &disabledSource);
  CHECK(disabledSource.requestedDataSourceCount == UINT32_MAX);
  CHECK(disabledSource.next == &duplicateSource);
  CHECK(duplicateSource.requestedDataSourceCount == UINT32_MAX);
  CHECK(duplicateSource.next == nullptr);
  XrHandJointLocationEXT joints[26]{};
  XrHandTrackingCapsulesStateFB capsules{XR_TYPE_HAND_TRACKING_CAPSULES_STATE_FB};
  XrHandTrackingAimStateFB aim{XR_TYPE_HAND_TRACKING_AIM_STATE_FB, &capsules};
  XrHandJointLocationsEXT locations{XR_TYPE_HAND_JOINT_LOCATIONS_EXT, &aim, XR_FALSE, 26, joints};
  XrHandJointsLocateInfoEXT li{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT, nullptr, local, time};
  XrHandTrackingDataSourceStateEXT disabledState{XR_TYPE_HAND_TRACKING_DATA_SOURCE_STATE_EXT};
  disabledState.isActive = XR_TRUE;
  disabledState.dataSource = XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT;
  unknown.next = reinterpret_cast<XrBaseOutStructure*>(&disabledState);
  locations.next = &unknown;
  disabledState.next = &disabledState;
  CHECK(xg::xrLocateHandJointsEXT(tracker, &li, &locations) == XR_ERROR_VALIDATION_FAILURE);
  disabledState.next = &hands; // An enabled structure with the wrong parent is still invalid.
  CHECK(xg::xrLocateHandJointsEXT(tracker, &li, &locations) == XR_ERROR_VALIDATION_FAILURE);
  XrHandTrackingAimStateFB duplicateAim{XR_TYPE_HAND_TRACKING_AIM_STATE_FB};
  duplicateAim.status = UINT64_MAX;
  capsules.next = &duplicateAim;
  disabledState.next = &aim;
  REQUIRE(xg::xrLocateHandJointsEXT(tracker, &li, &locations) == XR_SUCCESS);
  CHECK(locations.next == &unknown);
  CHECK(unknown.next == reinterpret_cast<XrBaseOutStructure*>(&disabledState));
  CHECK(disabledState.next == &aim);
  CHECK(disabledState.isActive == XR_TRUE);
  CHECK(disabledState.dataSource == XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT);
  CHECK(duplicateAim.status == UINT64_MAX);
  CHECK(duplicateAim.next == nullptr);
  locations.next = &aim;
  capsules.next = nullptr;
  REQUIRE(xg::xrLocateHandJointsEXT(tracker, &li, &locations) == XR_SUCCESS);
  CHECK(locations.isActive);
  CHECK((aim.status & XR_HAND_TRACKING_AIM_VALID_BIT_FB) != 0);
  CHECK(capsules.capsules[0].radius > 0);
  // Hand joints follow the scripted STAGE wrist and the caller's base space.
  MockXrScript positioned{};
  positioned.left_hand = {{0, 0, 1, 0}, {-0.2f, 1.3f, -0.4f}};
  ventureMockXrSetScript(&positioned);
  XrSpace stage{};
  ri.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
  REQUIRE(xg::xrCreateReferenceSpace(session, &ri, &stage) == XR_SUCCESS);
  li.baseSpace = stage;
  REQUIRE(xg::xrLocateHandJointsEXT(tracker, &li, &locations) == XR_SUCCESS);
  CHECK(joints[XR_HAND_JOINT_WRIST_EXT].pose.position.x == doctest::Approx(-0.2));
  CHECK(joints[XR_HAND_JOINT_WRIST_EXT].pose.position.y == doctest::Approx(1.3));
  CHECK(joints[XR_HAND_JOINT_INDEX_TIP_EXT].pose.position.x == doctest::Approx(-0.227));
  positioned.left_hand_joints = venture::xr::hand_model(true).joints;
  (*positioned.left_hand_joints)[XR_HAND_JOINT_INDEX_TIP_EXT].position.z = -0.07f;
  ventureMockXrSetScript(&positioned);
  REQUIRE(xg::xrLocateHandJointsEXT(tracker, &li, &locations) == XR_SUCCESS);
  CHECK(joints[XR_HAND_JOINT_INDEX_TIP_EXT].pose.position.z == doctest::Approx(-0.47));
  li.baseSpace = local;
  REQUIRE(xg::xrLocateHandJointsEXT(tracker, &li, &locations) == XR_SUCCESS);
  CHECK(joints[XR_HAND_JOINT_WRIST_EXT].pose.position.y == doctest::Approx(-0.3));
  positioned.hand_tracking_active = false;
  ventureMockXrSetScript(&positioned);
  REQUIRE(xg::xrLocateHandJointsEXT(tracker, &li, &locations) == XR_SUCCESS);
  CHECK_FALSE(locations.isActive);
  CHECK(joints[XR_HAND_JOINT_WRIST_EXT].locationFlags == 0);
  // The mock leaves an inactive hand's joints NaN; the guest sees neutral values.
  for (const auto& joint : joints) {
    CHECK(joint.pose.orientation.w == 1);
    CHECK(joint.pose.position.x == 0);
    CHECK(joint.radius == 0);
  }
  CHECK(xg::xrDestroySpace(stage) == XR_SUCCESS);
  XrHandTrackingMeshFB mesh{XR_TYPE_HAND_TRACKING_MESH_FB};
  CHECK(xg::xrGetHandMeshFB(tracker, &mesh) == XR_SUCCESS);
  CHECK(mesh.jointCountOutput == 26);
  CHECK(mesh.vertexCountOutput > 0);
  mesh.jointCapacityInput = 1;
  CHECK(xg::xrGetHandMeshFB(tracker, &mesh) == XR_SUCCESS); // another capacity is zero
  mesh.vertexCapacityInput = 1;
  mesh.indexCapacityInput = 1;
  CHECK(xg::xrGetHandMeshFB(tracker, &mesh) == XR_ERROR_SIZE_INSUFFICIENT);
  mesh.jointCapacityInput = mesh.jointCountOutput;
  mesh.vertexCapacityInput = mesh.vertexCountOutput;
  mesh.indexCapacityInput = 0;
  // All arrays may remain null in a size query, including ones with nonzero
  // capacities. A real Quest must accept the same enumeration contract.
  CHECK(xg::xrGetHandMeshFB(tracker, &mesh) == XR_SUCCESS);
  struct FutureMeshOutput {
    XrStructureType type;
    void* next;
    std::uint64_t untouched;
  } future{static_cast<XrStructureType>(0x7ffe1234), nullptr, 0x123456789abcdef0ull};
  mesh.next = &future;
  CHECK(xg::xrGetHandMeshFB(tracker, &mesh) == XR_SUCCESS);
  CHECK(mesh.next == &future);
  CHECK(future.type == static_cast<XrStructureType>(0x7ffe1234));
  CHECK(future.next == nullptr);
  CHECK(future.untouched == 0x123456789abcdef0ull);
  std::vector<XrPosef> bind(mesh.jointCountOutput);
  std::vector<float> radii(mesh.jointCountOutput);
  std::vector<XrHandJointEXT> parents(mesh.jointCountOutput);
  std::vector<XrVector3f> vertices(mesh.vertexCountOutput), normals(mesh.vertexCountOutput);
  std::vector<XrVector2f> uv(mesh.vertexCountOutput);
  std::vector<XrVector4sFB> blends(mesh.vertexCountOutput);
  std::vector<XrVector4f> weights(mesh.vertexCountOutput);
  std::vector<std::int16_t> mesh_indices(mesh.indexCountOutput);
  mesh.indexCapacityInput = mesh.indexCountOutput;
  mesh.jointBindPoses = bind.data(); mesh.jointRadii = radii.data(); mesh.jointParents = parents.data();
  mesh.vertexPositions = vertices.data(); mesh.vertexNormals = normals.data(); mesh.vertexUVs = uv.data();
  mesh.vertexBlendIndices = blends.data(); mesh.vertexBlendWeights = weights.data(); mesh.indices = mesh_indices.data();
  CHECK(xg::xrGetHandMeshFB(tracker, &mesh) == XR_SUCCESS);
  CHECK(bind[XR_HAND_JOINT_WRIST_EXT].orientation.w == doctest::Approx(1));
  CHECK(future.untouched == 0x123456789abcdef0ull);
  MockXrScript lost{};
  lost.active = false;
  ventureMockXrSetScript(&lost);
  REQUIRE(xg::xrLocateHandJointsEXT(tracker, &li, &locations) == XR_SUCCESS);
  CHECK_FALSE(locations.isActive);
  CHECK((aim.status & XR_HAND_TRACKING_AIM_VALID_BIT_FB) == 0);
  MockXrScript active{};
  ventureMockXrSetScript(&active);
  CHECK(xg::xrDestroyHandTrackerEXT(tracker) == XR_SUCCESS);
  CHECK(xg::xrDestroySpace(local) == XR_SUCCESS);
  CHECK(xg::xrDestroySwapchain(chain) == XR_SUCCESS);
  CHECK(xg::xrDestroySession(session) == XR_SUCCESS);
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  venture::xr::install_xr_images(nullptr);
}

namespace {
struct UnfocusedSync {
  void* previous = venture::xr::host_xr().get_instance_proc_addr;
  inline static PFN_xrGetInstanceProcAddr fallback{};
  static XrResult XRAPI_CALL sync(XrSession, const XrActionsSyncInfo*) { return XR_SESSION_NOT_FOCUSED; }
  static XrResult XRAPI_CALL get(XrInstance instance, const char* name, PFN_xrVoidFunction* out) {
    if (std::strcmp(name, "xrSyncActions") != 0) return fallback(instance, name, out);
    *out = reinterpret_cast<PFN_xrVoidFunction>(sync); return XR_SUCCESS;
  }
  UnfocusedSync() { fallback = reinterpret_cast<PFN_xrGetInstanceProcAddr>(previous); venture::xr::host_xr().get_instance_proc_addr = reinterpret_cast<void*>(get); }
  ~UnfocusedSync() { venture::xr::host_xr().get_instance_proc_addr = previous; }
};
struct EnableMenuChord {
  std::string previous;
  bool present;
  EnableMenuChord() : present(std::getenv("VENTURE_MENU_CHORD") != nullptr) {
    if (present) previous = std::getenv("VENTURE_MENU_CHORD");
#ifdef _WIN32
    _putenv_s("VENTURE_MENU_CHORD", "stick-clicks");
#else
    setenv("VENTURE_MENU_CHORD", "stick-clicks", 1);
#endif
    venture::environment::refresh();
    venture::xr::invalidate_host_procedures();
  }
  ~EnableMenuChord() {
#ifdef _WIN32
    _putenv_s("VENTURE_MENU_CHORD", present ? previous.c_str() : "");
#else
    if (present) setenv("VENTURE_MENU_CHORD", previous.c_str(), 1); else unsetenv("VENTURE_MENU_CHORD");
#endif
    venture::environment::refresh();
    venture::xr::invalidate_host_procedures();
  }
};
}
TEST_CASE("XR sampler extension dependencies and unsupported requests are rejected") {
  const char* names[]{"XR_FB_swapchain_update_state_vulkan"};
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  ci.enabledExtensionCount = 1;
  ci.enabledExtensionNames = names;
  XrInstance instance{};
  CHECK(xg::xrCreateInstance(&ci, &instance) == XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED);
  names[0] = "XR_VENTURE_nonexistent";
  CHECK(xg::xrCreateInstance(&ci, &instance) == XR_ERROR_EXTENSION_NOT_PRESENT);
  CHECK(instance == XR_NULL_HANDLE);
}
TEST_CASE("XR Vulkan instance accepts both sampler declarations without GLES graphics") {
  const char* names[]{"XR_FB_swapchain_update_state", "XR_KHR_vulkan_enable",
    "XR_FB_swapchain_update_state_vulkan", "XR_FB_swapchain_update_state_opengl_es"};
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  ci.enabledExtensionCount = 4; ci.enabledExtensionNames = names;
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&ci, &instance) == XR_SUCCESS);
  CHECK_FALSE(venture::hle::openxr::enabled("XR_KHR_opengl_es_enable"));
  CHECK(venture::hle::openxr::enabled("XR_FB_swapchain_update_state_opengl_es"));
  REQUIRE(xg::xrDestroyInstance(instance) == XR_SUCCESS);
}
TEST_CASE("XR sampler states match session bindings with base-only acceptance") {
  venture::test::XrImageFixture interop;
  venture::xr::install_xr_images(&interop);
  auto& host = venture::xr::host_xr();
  struct Restore {
    venture::xr::HostXr& host;
    std::set<std::string> saved;
    ~Restore() { host.enabled_extensions = saved; }
  } restore{host, host.enabled_extensions};
  bool gles = false, sampler_extensions = true;
  SUBCASE("Vulkan session, both sampler extensions") { gles = false; }
  SUBCASE("Vulkan session, base extension only") { gles = false; sampler_extensions = false; }
  SUBCASE("GLES session, both sampler extensions") { gles = true; }
  SUBCASE("GLES session, base extension only") { gles = true; sampler_extensions = false; }
  const char* extensions[]{"XR_FB_swapchain_update_state", "XR_KHR_opengl_es_enable",
    "XR_KHR_vulkan_enable", "XR_FB_swapchain_update_state_opengl_es", "XR_FB_swapchain_update_state_vulkan"};
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  ci.enabledExtensionCount = sampler_extensions ? 5 : 3; ci.enabledExtensionNames = extensions;
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&ci, &instance) == XR_SUCCESS);
  xg::GlesBinding gl_binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR, nullptr, 1, 2, 3};
  auto& gpu = venture::gfx::gpu_device();
  XrGraphicsBindingVulkanKHR vk_binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR, nullptr,
    gpu.instance, gpu.physical_device, gpu.device, gpu.queue_family, gpu.queue_index};
  XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO, gles ? static_cast<const void*>(&gl_binding) : &vk_binding,
    0, host.system};
  XrSession session{};
  REQUIRE(xg::xrCreateSession(instance, &si, &session) == XR_SUCCESS);
  CHECK(venture::hle::openxr::state().gles == gles);
  XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  sci.format = gles ? 0x8058 : VK_FORMAT_R8G8B8A8_UNORM;
  sci.width = sci.height = 32;
  sci.sampleCount = sci.faceCount = sci.arraySize = sci.mipCount = 1;
  XrSwapchain chain{}, second{};
  REQUIRE(xg::xrCreateSwapchain(session, &sci, &chain) == XR_SUCCESS);
  REQUIRE(xg::xrCreateSwapchain(session, &sci, &second) == XR_SUCCESS);
  // Creation must initialize application texture defaults for base-only guests.
  CHECK(interop.sampler_updates == (gles ? 2u : 0u));
  if (gles) {
    CHECK(interop.last_sampler.minFilter == 0x2601);
    CHECK(interop.last_sampler.wrapModeS == 0x812f);
    CHECK(interop.last_sampler.swizzleAlpha == 0x1906);
    CHECK(interop.last_sampler.maxAnisotropy == 1);
  }
  // Materialize the lazily allocated host resources for the forwarding part
  // of this test. Before presentation, sampler state belongs to the guest.
  for (auto value : {chain, second}) {
    std::uint32_t index{};
    REQUIRE(xg::xrAcquireSwapchainImage(value, nullptr, &index) == XR_SUCCESS);
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    REQUIRE(xg::xrWaitSwapchainImage(value, &wait) == XR_SUCCESS);
    REQUIRE(xg::xrReleaseSwapchainImage(value, nullptr) == XR_SUCCESS);
  }
  REQUIRE(venture::hle::openxr::prepare_swapchains({chain, second}) == XR_SUCCESS);
  auto header = [](auto& s) { return reinterpret_cast<XrSwapchainStateBaseHeaderFB*>(&s); };
  XrSwapchainStateSamplerVulkanFB vk = venture::xr::default_sampler();
  XrSwapchainStateSamplerOpenGLESFB gl{XR_TYPE_SWAPCHAIN_STATE_SAMPLER_OPENGL_ES_FB};
  REQUIRE(xg::xrGetSwapchainStateFB(chain, gles ? header(gl) : header(vk)) == XR_SUCCESS);
  // Mismatched structs are rejected with both sampler extensions or only the base.
  CHECK(xg::xrGetSwapchainStateFB(chain, gles ? header(vk) : header(gl)) == XR_ERROR_VALIDATION_FAILURE);
  CHECK(xg::xrUpdateSwapchainFB(chain, gles ? header(vk) : header(gl)) == XR_ERROR_VALIDATION_FAILURE);
  for (auto name : {"XR_FB_swapchain_update_state_vulkan", "XR_FB_swapchain_update_state_opengl_es"})
    venture::hle::openxr::state().enabled.erase(name);
  CHECK(xg::xrGetSwapchainStateFB(chain, gles ? header(vk) : header(gl)) == XR_ERROR_VALIDATION_FAILURE);
  CHECK(xg::xrUpdateSwapchainFB(chain, gles ? header(vk) : header(gl)) == XR_ERROR_VALIDATION_FAILURE);
  auto exercise = [&]<class T>(T sampler) {
    constexpr bool is_gl = std::is_same_v<T, XrSwapchainStateSamplerOpenGLESFB>;
    REQUIRE(xg::xrGetSwapchainStateFB(chain, header(sampler)) == XR_SUCCESS);
    CHECK(sampler.minFilter == (is_gl ? 0x2601 : VK_FILTER_LINEAR));
    CHECK(sampler.maxAnisotropy == 1);
    CHECK(sampler.swizzleAlpha == (is_gl ? 0x1906 : VK_COMPONENT_SWIZZLE_IDENTITY));
    CHECK(sampler.wrapModeS == (is_gl ? 0x812f : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE));
    auto fresh = sampler;
    REQUIRE(xg::xrGetSwapchainStateFB(second, header(fresh)) == XR_SUCCESS);
    CHECK(fresh.maxAnisotropy == 1);
    auto fields = [](const T& s) {
      auto common = std::tuple{s.type, s.next, s.minFilter, s.magFilter, s.wrapModeS, s.wrapModeT,
        s.swizzleRed, s.swizzleGreen, s.swizzleBlue, s.swizzleAlpha, s.maxAnisotropy,
        s.borderColor.r, s.borderColor.g, s.borderColor.b, s.borderColor.a};
      if constexpr (is_gl) return common;
      else return std::tuple_cat(common, std::tuple{s.mipmapMode});
    };
    for (bool forward : {false, true}) {
      if (forward) {
        host.enabled_extensions.insert("XR_FB_swapchain_update_state");
        host.enabled_extensions.insert("XR_FB_swapchain_update_state_vulkan");
      } else host.enabled_extensions.erase("XR_FB_swapchain_update_state_vulkan");
      if constexpr (is_gl) {
        sampler.minFilter = 0x2703; sampler.magFilter = 0x2600;
        sampler.wrapModeS = 0x2901; sampler.wrapModeT = 0x8370;
        sampler.swizzleRed = 1; sampler.swizzleGreen = 0;
        sampler.swizzleBlue = 0x1906; sampler.swizzleAlpha = 0x1903;
      } else {
        sampler.minFilter = VK_FILTER_NEAREST; sampler.magFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler.wrapModeS = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.wrapModeT = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        sampler.swizzleRed = VK_COMPONENT_SWIZZLE_ONE; sampler.swizzleGreen = VK_COMPONENT_SWIZZLE_ZERO;
        sampler.swizzleBlue = VK_COMPONENT_SWIZZLE_A; sampler.swizzleAlpha = VK_COMPONENT_SWIZZLE_R;
      }
      sampler.maxAnisotropy = 8; sampler.borderColor = {.5f, .6f, .7f, .8f};
      REQUIRE(xg::xrUpdateSwapchainFB(chain, header(sampler)) == XR_SUCCESS);
      auto got = fresh;
      REQUIRE(xg::xrGetSwapchainStateFB(chain, header(got)) == XR_SUCCESS);
      CHECK(fields(got) == fields(sampler));
      XrSwapchainStateSamplerVulkanFB native{XR_TYPE_SWAPCHAIN_STATE_SAMPLER_VULKAN_FB};
      REQUIRE(venture::xr::host_call<PFN_xrGetSwapchainStateFB, "xrGetSwapchainStateFB">(venture::hle::openxr::state().swapchains.at(chain).host_handle, header(native)) == XR_SUCCESS);
      CHECK(native.maxAnisotropy == (forward ? 8 : 1));
      if (forward) {
        CHECK(native.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR);
        CHECK(native.wrapModeT == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT);
        CHECK(native.swizzleRed == VK_COMPONENT_SWIZZLE_ONE);
        CHECK(native.swizzleAlpha == VK_COMPONENT_SWIZZLE_R);
        CHECK(native.borderColor.a == sampler.borderColor.a);
        REQUIRE(venture::xr::host_call<PFN_xrGetSwapchainStateFB, "xrGetSwapchainStateFB">(venture::hle::openxr::state().swapchains.at(second).host_handle, header(native)) == XR_SUCCESS);
        CHECK(native.maxAnisotropy == 1); // unrelated swapchain retains its own sampler
        MockXrScript script{}; script.sampler_update_result = XR_ERROR_RUNTIME_FAILURE;
        ventureMockXrSetScript(&script);
        auto failed = sampler; failed.maxAnisotropy = 16;
        CHECK(xg::xrUpdateSwapchainFB(chain, header(failed)) == XR_ERROR_RUNTIME_FAILURE);
        REQUIRE(xg::xrGetSwapchainStateFB(chain, header(got)) == XR_SUCCESS);
        CHECK(fields(got) == fields(sampler));
        if constexpr (is_gl) CHECK(interop.last_sampler.maxAnisotropy == 8);
        script.sampler_update_result = XR_SUCCESS; ventureMockXrSetScript(&script);
      }
      if constexpr (is_gl) {
        CHECK(interop.last_sampler.swizzleAlpha == sampler.swizzleAlpha);
        CHECK_FALSE(interop.sampler_textures.empty());
        interop.sampler_result = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        CHECK(xg::xrUpdateSwapchainFB(chain, header(sampler)) == XR_ERROR_GRAPHICS_DEVICE_INVALID);
        interop.sampler_result = XR_SUCCESS;
        auto bad = sampler; bad.magFilter = 0x2703;
        CHECK(xg::xrUpdateSwapchainFB(chain, header(bad)) == XR_ERROR_VALIDATION_FAILURE);
      }
      auto invalid = sampler;
      if constexpr (is_gl) invalid.swizzleBlue = 999;
      else invalid.swizzleBlue = VkComponentSwizzle(-1);
      CHECK(xg::xrUpdateSwapchainFB(chain, header(invalid)) == XR_ERROR_VALIDATION_FAILURE);
      invalid = sampler;
      invalid.maxAnisotropy = std::numeric_limits<float>::quiet_NaN();
      CHECK(xg::xrUpdateSwapchainFB(chain, header(invalid)) == XR_ERROR_VALIDATION_FAILURE);
      invalid = sampler; invalid.next = &vk;
      CHECK(xg::xrGetSwapchainStateFB(chain, header(invalid)) == XR_ERROR_VALIDATION_FAILURE);
      CHECK(xg::xrUpdateSwapchainFB(chain, header(invalid)) == XR_ERROR_VALIDATION_FAILURE);
      // Sampler sub-extensions remain absent throughout these round trips.
      CHECK_FALSE(venture::hle::openxr::enabled("XR_FB_swapchain_update_state_vulkan"));
      CHECK_FALSE(venture::hle::openxr::enabled("XR_FB_swapchain_update_state_opengl_es"));
    }
    venture::hle::openxr::state().enabled.erase("XR_FB_swapchain_update_state");
    CHECK(xg::xrGetSwapchainStateFB(chain, header(sampler)) == XR_ERROR_FUNCTION_UNSUPPORTED);
    CHECK(xg::xrUpdateSwapchainFB(chain, header(sampler)) == XR_ERROR_FUNCTION_UNSUPPORTED);
  };
  if (gles) exercise(gl); else exercise(vk);
  CHECK(xg::xrGetSwapchainStateFB(chain, gles ? header(vk) : header(gl)) == XR_ERROR_FUNCTION_UNSUPPORTED);
  REQUIRE(xg::xrDestroySwapchain(second) == XR_SUCCESS);
  REQUIRE(xg::xrDestroySwapchain(chain) == XR_SUCCESS);
  REQUIRE(xg::xrDestroySession(session) == XR_SUCCESS);
  REQUIRE(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  venture::xr::install_xr_images(nullptr);
}

TEST_CASE("XR GLES sampler conversion covers every filter wrap and swizzle") {
  venture::xr::SamplerOpenGLES gl;
  XrSwapchainStateSamplerVulkanFB vk{};
  for (auto filter : {0x2600u, 0x2601u, 0x2700u, 0x2701u, 0x2702u, 0x2703u}) {
    gl.minFilter = filter;
    REQUIRE(venture::xr::gles_sampler(gl, vk));
    CHECK(vk.minFilter == ((filter == 0x2600 || filter == 0x2700 || filter == 0x2702) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR));
    CHECK(vk.mipmapMode == ((filter == 0x2702 || filter == 0x2703) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST));
  }
  for (const auto [gl_wrap, vk_wrap] : {
      std::pair{0x2901u, VK_SAMPLER_ADDRESS_MODE_REPEAT},
      std::pair{0x8370u, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT},
      std::pair{0x812fu, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE},
      std::pair{0x812du, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER}}) {
    gl.wrapModeS = gl.wrapModeT = gl_wrap;
    REQUIRE(venture::xr::gles_sampler(gl, vk));
    CHECK(vk.wrapModeS == vk_wrap); CHECK(vk.wrapModeT == vk_wrap);
  }
  for (const auto [gl_swizzle, vk_swizzle] : {
      std::pair{0u, VK_COMPONENT_SWIZZLE_ZERO}, std::pair{1u, VK_COMPONENT_SWIZZLE_ONE},
      std::pair{0x1903u, VK_COMPONENT_SWIZZLE_R}, std::pair{0x1904u, VK_COMPONENT_SWIZZLE_G},
      std::pair{0x1905u, VK_COMPONENT_SWIZZLE_B}, std::pair{0x1906u, VK_COMPONENT_SWIZZLE_A}}) {
    gl.swizzleRed = gl.swizzleGreen = gl.swizzleBlue = gl.swizzleAlpha = gl_swizzle;
    REQUIRE(venture::xr::gles_sampler(gl, vk));
    CHECK(vk.swizzleRed == vk_swizzle); CHECK(vk.swizzleGreen == vk_swizzle);
    CHECK(vk.swizzleBlue == vk_swizzle); CHECK(vk.swizzleAlpha == vk_swizzle);
  }
}

TEST_CASE("XR GLES cubic sampler enums remain cubic on the Vulkan host") {
  venture::xr::SamplerOpenGLES gl;
  gl.magFilter = 0x9139;
  for (auto filter : {0x9139u, 0x913au, 0x913bu}) {
    gl.minFilter = filter;
    XrSwapchainStateSamplerVulkanFB vk{};
    REQUIRE(venture::xr::gles_sampler(gl, vk));
    CHECK(vk.minFilter == VK_FILTER_CUBIC_EXT); CHECK(vk.magFilter == VK_FILTER_CUBIC_EXT);
    CHECK(vk.mipmapMode == (filter == 0x913b ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST));
  }
}

namespace {
// A host runtime offering XR_FB_space_warp: the mock lists it and the host
// connection treats it as enabled for the duration.
struct SpaceWarpHost {
  bool available = venture::xr::host_xr().available_extensions.contains("XR_FB_space_warp");
  bool enabled = venture::xr::host_xr().enabled_extensions.contains("XR_FB_space_warp");
  SpaceWarpHost() {
    set("1");
    venture::xr::host_xr().available_extensions.emplace("XR_FB_space_warp", 2);
    venture::xr::host_xr().enabled_extensions.insert("XR_FB_space_warp");
  }
  ~SpaceWarpHost() {
    set("");
    if (!available) venture::xr::host_xr().available_extensions.erase("XR_FB_space_warp");
    if (!enabled) venture::xr::host_xr().enabled_extensions.erase("XR_FB_space_warp");
  }
  static void set(const char* value) {
#ifdef _WIN32
    _putenv_s("VENTURE_MOCK_SPACE_WARP", value);
#else
    if (*value) setenv("VENTURE_MOCK_SPACE_WARP", value, 1); else unsetenv("VENTURE_MOCK_SPACE_WARP");
#endif
    venture::environment::refresh();
    ventureMockXrRefreshEnvironment();
  }
};
}  // namespace

// One stereo frame whose projection views carry XrCompositionLayerSpaceWarpInfoFB;
// returns how many views reached the host with the node.
static std::uint32_t space_warp_frame(bool negotiated, bool validate_full_rate = false,
                                     std::uint32_t explicit_depth_order = 0, bool invalid_depth = false) {
  std::vector<const char*> extensions{"XR_KHR_vulkan_enable2"};
  if (negotiated) extensions.push_back("XR_FB_space_warp");
  if (explicit_depth_order) extensions.push_back("XR_KHR_composition_layer_depth");
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  create.enabledExtensionNames = extensions.data();
  create.enabledExtensionCount = std::uint32_t(extensions.size());
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&create, &instance) == XR_SUCCESS);
  XrSystemSpaceWarpPropertiesFB warp{XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB};
  XrSystemProperties system{XR_TYPE_SYSTEM_PROPERTIES, &warp};
  REQUIRE(xg::xrGetSystemProperties(instance, venture::xr::host_xr().system, &system) == XR_SUCCESS);
  CHECK(warp.next == nullptr);
  CHECK((warp.recommendedMotionVectorImageRectWidth > 0) == negotiated);
  CHECK((warp.recommendedMotionVectorImageRectHeight > 0) == negotiated);
  auto& gpu = venture::gfx::gpu_device();
  XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR, nullptr, gpu.instance,
      gpu.physical_device, gpu.device, gpu.queue_family, gpu.queue_index};
  XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO, &binding, 0, venture::xr::host_xr().system};
  XrSession session{};
  REQUIRE(xg::xrCreateSession(instance, &sessionInfo, &session) == XR_SUCCESS);
  std::uint32_t count{};
  REQUIRE(xg::xrEnumerateSwapchainFormats(session, 0, &count, nullptr) == XR_SUCCESS);
  std::vector<std::int64_t> formats(count);
  REQUIRE(xg::xrEnumerateSwapchainFormats(session, count, &count, formats.data()) == XR_SUCCESS);
  // Desktop runtimes list the motion-vector format with or without depth (HITMAN
  // ends with an invalid image when its motion chain is refused).
  CHECK(std::ranges::count(formats, VK_FORMAT_R16G16B16A16_SFLOAT) == 1);
  if (negotiated) {
    // Vulkan guests always see Quest's D24S8 (backed by D32S8 where the GPU attaches no D24).
    CHECK(std::ranges::count(formats, VK_FORMAT_D24_UNORM_S8_UINT) == 1);
  }
  auto swapchain = [&](std::int64_t format, XrSwapchainUsageFlags usage, std::uint32_t size) {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO, nullptr, 0, usage, format, 1, size, size, 1, 2, 1};
    XrSwapchain chain{};
    REQUIRE(xg::xrCreateSwapchain(session, &info, &chain) == XR_SUCCESS);
    return chain;
  };
  const auto mv_size = negotiated ? warp.recommendedMotionVectorImageRectWidth : 64;
  auto color = swapchain(VK_FORMAT_R8G8B8A8_SRGB, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT, 64);
  auto motion = swapchain(VK_FORMAT_R16G16B16A16_SFLOAT, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT, mv_size);
  // Quest's depth format; where the GPU attaches no D24S8 (RADV), Venture backs it with a D32S8 chain.
  // A host listing neither (SteamVR on AMD) still gets OVRPlugin's D24S8 request.
  const auto depth_format = std::ranges::count(formats, VK_FORMAT_D24_UNORM_S8_UINT) ||
                                    !std::ranges::count(formats, VK_FORMAT_D32_SFLOAT_S8_UINT)
      ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
  auto depth = swapchain(depth_format, XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, mv_size);
  XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr,
      XR_REFERENCE_SPACE_TYPE_LOCAL, {{0, 0, 0, 1}, {0, 0, 0}}};
  XrSpace local{};
  REQUIRE(xg::xrCreateReferenceSpace(session, &spaceInfo, &local) == XR_SUCCESS);
  auto poll = [&] {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xg::xrPollEvent(instance, &event) == XR_SUCCESS) event = {XR_TYPE_EVENT_DATA_BUFFER};
  };
  poll();
  XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
  REQUIRE(xg::xrBeginSession(session, &begin) == XR_SUCCESS);
  poll();
  XrCompositionLayerSpaceWarpInfoFB infos[2]{{XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB},
                                             {XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB}};
  XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                           {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  XrCompositionLayerDepthInfoKHR depths[2]{};
  const XrExtent2Di mv_extent{std::int32_t(mv_size), std::int32_t(mv_size)};
  for (std::uint32_t eye = 0; eye < 2; ++eye) {
    infos[eye].motionVectorSubImage = {motion, {{0, 0}, mv_extent}, eye};
    infos[eye].depthSubImage = {depth, {{0, 0}, mv_extent}, eye};
    infos[eye].appSpaceDeltaPose.orientation.w = 1;
    infos[eye].maxDepth = 1;
    infos[eye].nearZ = 0.1f;
    infos[eye].farZ = 100;
    views[eye].next = &infos[eye];
    views[eye].pose.orientation.w = 1;
    views[eye].fov = {-0.5f, 0.5f, 0.5f, -0.5f};
    views[eye].subImage = {color, {{0, 0}, {64, 64}}, eye};
    if (explicit_depth_order) {
      depths[eye] = {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR, nullptr,
          infos[eye].depthSubImage, 0, 1, 0.1f, 100};
      if (explicit_depth_order == 1) {
        depths[eye].next = &infos[eye];
        views[eye].next = &depths[eye];
      } else {
        infos[eye].next = &depths[eye];
      }
    }
  }
  XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION, nullptr, 0, local, 2, views};
  const auto* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
  const auto before = ventureMockXrStats().space_warp;
  XrBaseInStructure unknown{XR_STRUCTURE_TYPE_MAX_ENUM,
      reinterpret_cast<const XrBaseInStructure*>(views[0].next)};
  if (validate_full_rate) views[0].next = &unknown;
  const auto depth_before = ventureMockXrStats().projection_depth;
  XrFrameState timing{XR_TYPE_FRAME_STATE};
  REQUIRE(xg::xrWaitFrame(session, nullptr, &timing) == XR_SUCCESS);
  if (validate_full_rate) CHECK(timing.predictedDisplayPeriod == 8'333'333);
  REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_SUCCESS);
  for (auto chain : {color, motion, depth}) {
    std::uint32_t index{};
    REQUIRE(xg::xrAcquireSwapchainImage(chain, nullptr, &index) == XR_SUCCESS);
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    REQUIRE(xg::xrWaitSwapchainImage(chain, &wait) == XR_SUCCESS);
    REQUIRE(xg::xrReleaseSwapchainImage(chain, nullptr) == XR_SUCCESS);
  }
  XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, timing.predictedDisplayTime,
                     XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1, &layer};
  if (validate_full_rate) {
    // Motion payloads are unused on full-rate hosts; malformed orientation,
    // flags or motion subimage must not reject valid color/depth submission.
    for (auto& info : infos) {
      info.appSpaceDeltaPose.orientation = {};
      info.motionVectorSubImage.imageArrayIndex = 99;
      info.layerFlags = 2;
      if (invalid_depth) info.nearZ = info.farZ;
    }
  }
  CHECK(xg::xrEndFrame(session, &end) == XR_SUCCESS);
  if (validate_full_rate) {
    CHECK(ventureMockXrStats().projection_depth - depth_before == (invalid_depth ? 0 : 2));
    CHECK(infos[0].depthSubImage.swapchain == depth);
    CHECK(infos[0].motionVectorSubImage.imageArrayIndex == 99);
    CHECK(infos[0].layerFlags == 2);
    CHECK(infos[0].appSpaceDeltaPose.orientation.w == 0);
  }
  const auto forwarded = ventureMockXrStats().space_warp - before;
  CHECK(views[0].next == (validate_full_rate ? static_cast<const void*>(&unknown) : &infos[0]));
  for (auto chain : {color, motion, depth}) REQUIRE(xg::xrDestroySwapchain(chain) == XR_SUCCESS);
  REQUIRE(xg::xrDestroySpace(local) == XR_SUCCESS);
  REQUIRE(xg::xrDestroySession(session) == XR_SUCCESS);
  REQUIRE(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  return forwarded;
}

TEST_CASE("XR offers Vulkan guests D24 depth chains on hosts that attach only D32") {
  // Unity's space warp ends the session unless D24S8 is offered; RADV has none.
  struct Radv {
    std::string previous;
    bool present = false;
    Radv() {
      if (const auto* value = venture::environment::getenv("VENTURE_MOCK_DEPTH_FORMATS")) { previous = value; present = true; }
      set("radv");
      venture::vulkan::force_depth_alias(true);
    }
    ~Radv() {
      venture::vulkan::force_depth_alias(false);
      set(present ? previous.c_str() : "");
    }
    static void set(const char* value) {
#ifdef _WIN32
      _putenv_s("VENTURE_MOCK_DEPTH_FORMATS", value);
#else
      if (*value) setenv("VENTURE_MOCK_DEPTH_FORMATS", value, 1);
      else unsetenv("VENTURE_MOCK_DEPTH_FORMATS");
#endif
      venture::environment::refresh();
      ventureMockXrRefreshEnvironment();
    }
  } radv;
  const char* extensions[]{"XR_KHR_vulkan_enable2"};
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  ci.enabledExtensionCount = std::size(extensions);
  ci.enabledExtensionNames = extensions;
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&ci, &instance) == XR_SUCCESS);
  venture::test::VulkanDevice gpu;
  if (venture::vulkan::host_depth_format(gpu.physical, VK_FORMAT_D24_UNORM_S8_UINT) != VK_FORMAT_D32_SFLOAT_S8_UINT) {
    xg::xrDestroyInstance(instance);
    return;
  }
  XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR, nullptr,
      gpu.instance, gpu.physical, gpu.device, gpu.family, 0};
  XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO, &binding, 0, venture::xr::host_xr().system};
  XrSession session{};
  REQUIRE(xg::xrCreateSession(instance, &si, &session) == XR_SUCCESS);
  std::uint32_t count{};
  REQUIRE(xg::xrEnumerateSwapchainFormats(session, 0, &count, nullptr) == XR_SUCCESS);
  std::vector<std::int64_t> formats(count);
  REQUIRE(xg::xrEnumerateSwapchainFormats(session, count, &count, formats.data()) == XR_SUCCESS);
  const auto listed = [&](VkFormat f) { return std::find(formats.begin(), formats.end(), f) != formats.end(); };
  CHECK(listed(VK_FORMAT_D32_SFLOAT_S8_UINT));
  CHECK(listed(VK_FORMAT_D24_UNORM_S8_UINT));
  CHECK(listed(VK_FORMAT_X8_D24_UNORM_PACK32));
  XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  create.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  create.format = VK_FORMAT_D24_UNORM_S8_UINT;
  create.width = create.height = 64;
  create.sampleCount = create.faceCount = create.mipCount = 1;
  create.arraySize = 2;
  XrSwapchain chain{};
  REQUIRE(xg::xrCreateSwapchain(session, &create, &chain) == XR_SUCCESS);
  REQUIRE(xg::xrEnumerateSwapchainImages(chain, 0, &count, nullptr) == XR_SUCCESS);
  CHECK(count > 0);
  CHECK(xg::xrDestroySwapchain(chain) == XR_SUCCESS);
  CHECK(xg::xrDestroySession(session) == XR_SUCCESS);
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
}

TEST_CASE("XR keeps a depth chain the host has no format for, and leaves its depth out of host frames") {
  // Vampire: The Masquerade - Justice (OVRPlugin) asks for D24S8 depth chains
  // without checking the list; SteamVR on an RX 5700 XT offers no D24S8 or
  // D32S8, refused them, and every frame then failed: a black screen.
  struct NoStencil {
    std::string previous;
    bool present = false;
    NoStencil() {
      if (const auto* value = venture::environment::getenv("VENTURE_MOCK_DEPTH_FORMATS")) { previous = value; present = true; }
      set("nostencil");
    }
    ~NoStencil() { set(present ? previous.c_str() : ""); }
    static void set(const char* value) {
#ifdef _WIN32
      _putenv_s("VENTURE_MOCK_DEPTH_FORMATS", value);
#else
      if (*value) setenv("VENTURE_MOCK_DEPTH_FORMATS", value, 1);
      else unsetenv("VENTURE_MOCK_DEPTH_FORMATS");
#endif
      venture::environment::refresh();
      ventureMockXrRefreshEnvironment();
    }
  } host;
  const auto depth_before = ventureMockXrStats().projection_depth;
  CHECK(space_warp_frame(false, false, 2) == 0);
  CHECK(ventureMockXrStats().projection_depth == depth_before);
}

TEST_CASE("XR space warp forwards only when the host offers it and the guest enables it") {
  // Payload negotiation is independent of descriptor support. Advertise the
  // auxiliary formats even in the unnegotiated-payload case below.
  struct Formats {
    Formats() { MockXrScript script; script.depth_formats = true; ventureMockXrSetScript(&script); }
    ~Formats() { MockXrScript script; ventureMockXrSetScript(&script); }
  } formats;
  std::uint32_t count{};
  auto advertised = [&] {
    REQUIRE(xg::xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr) == XR_SUCCESS);
    std::vector<XrExtensionProperties> properties(count, {XR_TYPE_EXTENSION_PROPERTIES});
    REQUIRE(xg::xrEnumerateInstanceExtensionProperties(nullptr, count, &count, properties.data()) == XR_SUCCESS);
    for (const auto& p : properties)
      if (std::string_view(p.extensionName) == "XR_FB_space_warp") return true;
    return false;
  };
  // OVRPlugin attaches space-warp info to its projection views without
  // enabling the extension; the frame must succeed and the host never sees it.
  CHECK(advertised() == ox::full_rate_composition());
  CHECK(space_warp_frame(false) == 0);
  SpaceWarpHost host;
  REQUIRE(advertised());
  CHECK(space_warp_frame(false) == 0);
  CHECK(space_warp_frame(true) == 2);
}

TEST_CASE("XR native-rate motion inputs preserve cadence, depth and guest structures") {
  struct Experiment {
    std::string previous_formats;
    bool formats_present = false;
    Experiment() {
      if (const auto* value = venture::environment::getenv("VENTURE_MOCK_DEPTH_FORMATS")) {
        previous_formats = value; formats_present = true;
      }
      formats("1");
      MockXrScript script;
      script.display_period = 8'333'333;
      ventureMockXrSetScript(&script);
    }
    ~Experiment() {
      formats(formats_present ? previous_formats.c_str() : "");
      MockXrScript script;
      ventureMockXrSetScript(&script);
    }
    static void formats(const char* value) {
#ifdef _WIN32
      _putenv_s("VENTURE_MOCK_DEPTH_FORMATS", value);
#else
      if (*value) setenv("VENTURE_MOCK_DEPTH_FORMATS", value, 1);
      else unsetenv("VENTURE_MOCK_DEPTH_FORMATS");
#endif
      venture::environment::refresh();
      ventureMockXrRefreshEnvironment();
    }
  } experiment;
  REQUIRE_FALSE(venture::xr::host_xr().enabled_extensions.contains("XR_FB_space_warp"));
  REQUIRE(venture::xr::host_xr().enabled_extensions.contains("XR_KHR_composition_layer_depth"));
  REQUIRE(venture::hle::openxr::capabilities().contains("XR_FB_space_warp"));
  REQUIRE(venture::hle::openxr::capabilities().contains("XR_FB_foveation_vulkan") ==
          venture::hle::openxr::full_rate_composition());
  // The native host sees ordinary depth, no unsupported mobile structure.
  CHECK(space_warp_frame(true, true) == 0);
  CHECK(space_warp_frame(true, true, 1) == 0);
  CHECK(space_warp_frame(true, true, 2) == 0);
  CHECK(space_warp_frame(true, true, 0, true) == 0);
}

TEST_CASE("XR_META_vulkan_swapchain_create_info flags become core swapchain usage") {
  XrVulkanSwapchainCreateInfoMETA meta{XR_TYPE_VULKAN_SWAPCHAIN_CREATE_INFO_META};
  meta.additionalCreateFlags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  meta.additionalUsageFlags = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
  XrSwapchainUsageFlags usage = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  CHECK(ox::guest::vulkan_swapchain_flags(meta, usage) == XR_SUCCESS);
  CHECK(usage == (XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT |
                  XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT |
                  XR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_KHR));
  // Without negotiated density maps, a subsampled request keeps full-rate images.
  meta.additionalCreateFlags = VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  meta.additionalUsageFlags = 0;
  usage = 0;
  CHECK(ox::guest::vulkan_swapchain_flags(meta, usage) == XR_SUCCESS);
  CHECK(usage == XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT);
  // No core usage expresses other create flags, such as sparse binding.
  meta.additionalCreateFlags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT;
  CHECK(ox::guest::vulkan_swapchain_flags(meta, usage) == XR_ERROR_FEATURE_UNSUPPORTED);
  meta.additionalCreateFlags = 0;
  meta.additionalUsageFlags = VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT;
  CHECK(ox::guest::vulkan_swapchain_flags(meta, usage) == XR_ERROR_FEATURE_UNSUPPORTED);
}


TEST_CASE("XR host swapchains keep input-attachment usage through the host extension") {
  // Monado refuses the usage bit unless XR_KHR_swapchain_usage_input_attachment_bit is enabled
  // (HITMAN's eye chains), and so does the mock.
  const char* extensions[]{"XR_KHR_vulkan_enable2"};
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  ci.enabledExtensionCount = std::size(extensions);
  ci.enabledExtensionNames = extensions;
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&ci, &instance) == XR_SUCCESS);
  CHECK(venture::xr::host_xr().enabled_extensions.contains("XR_KHR_swapchain_usage_input_attachment_bit"));
  venture::test::VulkanDevice gpu;
  XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR, nullptr,
      gpu.instance, gpu.physical, gpu.device, gpu.family, 0};
  XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO, &binding, 0, venture::xr::host_xr().system};
  XrSession session{};
  REQUIRE(xg::xrCreateSession(instance, &si, &session) == XR_SUCCESS);
  XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  create.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                      XR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_KHR;
  create.format = VK_FORMAT_R8G8B8A8_SRGB;
  create.width = create.height = 64;
  create.sampleCount = create.faceCount = create.mipCount = 1;
  create.arraySize = 2;
  XrSwapchain host{};
  REQUIRE(venture::xr::create_host_swapchain(venture::xr::session().handle, create, &host) == XR_SUCCESS);
  CHECK((create.usageFlags & XR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_KHR));
  CHECK(::xrDestroySwapchain(host) == XR_SUCCESS);
  CHECK(xg::xrDestroySession(session) == XR_SUCCESS);
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
}

TEST_CASE("XR startup preserves host render requests and discarded-frame semantics") {
  struct Timing {
    std::string delay, pulse, completion;
    bool had_delay = false, had_pulse = false, had_completion = false;
    static void set(const char* key, const char* value) {
#ifdef _WIN32
      _putenv_s(key, value);
#else
      if (*value) setenv(key, value, 1); else unsetenv(key);
#endif
      venture::environment::refresh();
      ventureMockXrRefreshEnvironment();
    }
    Timing() {
      if (auto value = std::getenv("VENTURE_MOCK_SESSION_EVENT_STEP_MS")) { delay = value; had_delay = true; }
      if (auto value = std::getenv("VENTURE_MOCK_INITIAL_RENDER_PULSE")) { pulse = value; had_pulse = true; }
      if (auto value = std::getenv("VENTURE_MOCK_REQUIRE_INITIAL_END_FRAME")) { completion = value; had_completion = true; }
      set("VENTURE_MOCK_REQUIRE_INITIAL_END_FRAME", "1");
      set("VENTURE_MOCK_SESSION_EVENT_STEP_MS", "5");
      set("VENTURE_MOCK_INITIAL_RENDER_PULSE", "1");
    }
    ~Timing() {
      set("VENTURE_MOCK_REQUIRE_INITIAL_END_FRAME", had_completion ? completion.c_str() : "");
      set("VENTURE_MOCK_SESSION_EVENT_STEP_MS", had_delay ? delay.c_str() : "");
      set("VENTURE_MOCK_INITIAL_RENDER_PULSE", had_pulse ? pulse.c_str() : "");
      MockXrScript script;
      ventureMockXrSetScript(&script);
      venture::xr::install_xr_images(nullptr);
    }
  } timing;
  venture::test::XrImageFixture interop;
  venture::xr::install_xr_images(&interop);
  const char* extensions[]{"XR_KHR_opengl_es_enable"};
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  create.enabledExtensionNames = extensions;
  create.enabledExtensionCount = 1;
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&create, &instance) == XR_SUCCESS);
  MockXrScript script;
  script.display_period = 8'333'333;
  ventureMockXrSetScript(&script);
  xg::GlesBinding binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR, nullptr, 1, 2, 3};
  XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO, &binding, 0, 1};
  XrSession session{};
  REQUIRE(xg::xrCreateSession(instance, &info, &session) == XR_SUCCESS);
  auto poll = [&](XrSessionState wanted) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
      const auto result = xg::xrPollEvent(instance, &event);
      if (result == XR_SUCCESS && event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        const auto state = reinterpret_cast<const XrEventDataSessionStateChanged&>(event).state;
        if (state == wanted) return true;
        if (int(state) > int(wanted)) return false;
      } else if (result != XR_EVENT_UNAVAILABLE && result != XR_SUCCESS) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  };
  REQUIRE(poll(XR_SESSION_STATE_READY));
  XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
  REQUIRE(xg::xrBeginSession(session, &begin) == XR_SUCCESS);
  auto frame = [&](bool renderable) {
    XrFrameState next{XR_TYPE_FRAME_STATE};
    REQUIRE(xg::xrWaitFrame(session, nullptr, &next) == XR_SUCCESS);
    CHECK(bool(next.shouldRender) == renderable);
    CHECK(next.predictedDisplayPeriod == script.display_period);
    CHECK(next.predictedDisplayTime > 0);
    CHECK(venture::xr::session().pending_frame.shouldRender == next.shouldRender);
    REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_SUCCESS);
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, next.predictedDisplayTime,
                      XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 0, nullptr};
    REQUIRE(xg::xrEndFrame(session, &end) == XR_SUCCESS);
  };
  SUBCASE("delayed event delivery does not suppress the native render request") {
    Timing::set("VENTURE_MOCK_INITIAL_RENDER_PULSE", "");
    frame(true);
    CHECK(venture::xr::session().state == XR_SESSION_STATE_READY);
    REQUIRE(poll(XR_SESSION_STATE_SYNCHRONIZED));
    frame(true);
    REQUIRE(poll(XR_SESSION_STATE_VISIBLE));
    frame(true);
    REQUIRE(poll(XR_SESSION_STATE_FOCUSED));
    frame(true);
  }
  SUBCASE("normally completed startup frames") {
    // The host's first wait really emits a pulse while READY, before the
    // application consumes any SYNCHRONIZED/VISIBLE/FOCUSED events.
    const auto waits_before = ventureMockXrStats().waits;
    frame(true);
    CHECK(ventureMockXrStats().waits == waits_before + 1);
    CHECK(venture::xr::session().state == XR_SESSION_STATE_READY);
    REQUIRE(poll(XR_SESSION_STATE_SYNCHRONIZED));
    frame(false);
    REQUIRE(poll(XR_SESSION_STATE_VISIBLE));
    frame(true);
    REQUIRE(poll(XR_SESSION_STATE_FOCUSED));
    frame(true);
    script.should_render = false;
    ventureMockXrSetScript(&script);
    frame(false); // Visibility never turns a real host false into true.
  }
  SUBCASE("discarded startup frames still let the compositor synchronize") {
    const auto ends_before = ventureMockXrStats().ends;
    const auto frames_before = venture::xr::session().frame_index;
    XrFrameState first{XR_TYPE_FRAME_STATE}, second{XR_TYPE_FRAME_STATE};
    REQUIRE(xg::xrWaitFrame(session, nullptr, &first) == XR_SUCCESS);
    CHECK(first.shouldRender); // The real host request wins even while READY.
    REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_SUCCESS);
    // With no completed frame, the host really does stay READY.
    XrEventDataBuffer unavailable{XR_TYPE_EVENT_DATA_BUFFER};
    CHECK(xg::xrPollEvent(instance, &unavailable) == XR_EVENT_UNAVAILABLE);
    REQUIRE(xg::xrWaitFrame(session, nullptr, &second) == XR_SUCCESS);
    REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_FRAME_DISCARDED);
    CHECK(ventureMockXrStats().ends == ends_before + 1);
    CHECK(venture::xr::session().frame_index == frames_before); // No fake guest frame.
    REQUIRE(poll(XR_SESSION_STATE_SYNCHRONIZED));
    REQUIRE(poll(XR_SESSION_STATE_VISIBLE));
    REQUIRE(poll(XR_SESSION_STATE_FOCUSED));
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, second.predictedDisplayTime,
                      XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 0, nullptr};
    REQUIRE(xg::xrEndFrame(session, &end) == XR_SUCCESS);
    CHECK(ventureMockXrStats().ends == ends_before + 2);
    CHECK(venture::xr::session().frame_index == frames_before + 1);
    frame(true);
    // A visible compositor keeps its ordinary discard contract: no blank
    // host completion replaces the previously displayed image.
    const auto visible_ends = ventureMockXrStats().ends;
    XrFrameState visible_first{XR_TYPE_FRAME_STATE}, visible_second{XR_TYPE_FRAME_STATE};
    REQUIRE(xg::xrWaitFrame(session, nullptr, &visible_first) == XR_SUCCESS);
    REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_SUCCESS);
    REQUIRE(xg::xrWaitFrame(session, nullptr, &visible_second) == XR_SUCCESS);
    CHECK(xg::xrBeginFrame(session, nullptr) == XR_FRAME_DISCARDED);
    CHECK(ventureMockXrStats().ends == visible_ends);
    end.displayTime = visible_second.predictedDisplayTime;
    CHECK(xg::xrEndFrame(session, &end) == XR_SUCCESS);
  }
  REQUIRE(xg::xrRequestExitSession(session) == XR_SUCCESS);
  REQUIRE(poll(XR_SESSION_STATE_STOPPING));
  CHECK(xg::xrEndSession(session) == XR_SUCCESS);
  CHECK(xg::xrDestroySession(session) == XR_SUCCESS);
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
}
