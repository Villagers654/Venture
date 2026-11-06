#define XR_USE_GRAPHICS_API_OPENGL_ES
using EGLenum = unsigned int;
#include "common/environment.h"
#include "../mock_openxr_runtime/control.h"
#include "core/process.h"
#include "hle/openxr/openxr.h"
#include "hle/openxr/passthrough.h"
#include "xr/host_xr.h"
#include "xr/xr_images.h"
#include "xr_images_fixture.h"
#include "common/paths.h"
#include <array>
#include <chrono>
#include <optional>
#include <cstdlib>
#include <string>
#include <thread>
#include <doctest/doctest.h>

namespace ox = venture::hle::openxr;
namespace xg = ox::guest;

namespace {
// One host profile of the mock runtime: its passthrough extensions and system
// properties, its blend modes, and the user's passthrough setting.
struct HostProfile {
  bool saved_setting = venture::process_config().passthrough;
  std::optional<std::array<std::uint8_t, 3>> saved_key = venture::process_config().passthrough_key;
  HostProfile(const char* capabilities, bool setting, std::optional<std::array<std::uint8_t, 3>> key = {}) {
    set(capabilities);
    auto& host = venture::xr::host_xr();
    REQUIRE(host.instance != XR_NULL_HANDLE);
    // The host instance outlives profiles; enable what this one advertises.
    const std::string list = capabilities;
    const auto has = [&](const char* name) { return (',' + list + ',').find(std::string(",") + name + ',') != std::string::npos; };
    for (const char* extension : {"XR_FB_passthrough", "XR_HTC_passthrough"}) host.enabled_extensions.erase(extension);
    if (setting && (has("fb") || has("fb-unsupported"))) host.enabled_extensions.insert("XR_FB_passthrough");
    if (setting && has("htc")) host.enabled_extensions.insert("XR_HTC_passthrough");
    venture::process_config().passthrough = setting;
    venture::process_config().passthrough_key = key;
    ox::reset_passthrough_probe();
  }
  ~HostProfile() {
    set("");
    auto& host = venture::xr::host_xr();
    host.enabled_extensions.erase("XR_FB_passthrough");
    host.enabled_extensions.erase("XR_HTC_passthrough");
    venture::process_config().passthrough = saved_setting;
    venture::process_config().passthrough_key = saved_key;
    ox::reset_passthrough_probe();
  }
  static void set(const char* value) {
#ifdef _WIN32
    _putenv_s("VENTURE_MOCK_PASSTHROUGH", value);
#else
    if (*value) setenv("VENTURE_MOCK_PASSTHROUGH", value, 1); else unsetenv("VENTURE_MOCK_PASSTHROUGH");
#endif
    venture::environment::refresh();
    ventureMockXrRefreshEnvironment();
  }
};

struct Expect {
  const char* capabilities;
  bool setting;
  ox::PassthroughBackend backend;
  XrEnvironmentBlendMode host_mode; // the blend mode the host receives
  std::uint32_t fb_layers, htc_layers;
  std::optional<std::array<std::uint8_t, 3>> key = {}; // the user's color key (Settings)
};

void run_profile(const Expect& expect) {
  venture::test::XrImageFixture interop;
  venture::xr::install_xr_images(&interop);
  // The host instance exists before a profile adjusts it.
  {
    const char* extensions[]{"XR_KHR_opengl_es_enable"};
    XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
    create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    create.enabledExtensionNames = extensions;
    create.enabledExtensionCount = 1;
    XrInstance instance{};
    REQUIRE(xg::xrCreateInstance(&create, &instance) == XR_SUCCESS);
    REQUIRE(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  }
  HostProfile profile(expect.capabilities, expect.setting, expect.key);
  const char* extensions[]{"XR_KHR_opengl_es_enable", "XR_FB_passthrough"};
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  create.enabledExtensionNames = extensions;
  create.enabledExtensionCount = 2;
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&create, &instance) == XR_SUCCESS);
  CHECK(ox::passthrough_backend() == expect.backend);

  // What a game that only asks for support sees.
  XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO, nullptr, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
  XrSystemId system{};
  REQUIRE(xg::xrGetSystem(instance, &system_info, &system) == XR_SUCCESS);
  XrSystemPassthroughProperties2FB newer{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB};
  XrSystemPassthroughPropertiesFB older{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB, &newer};
  XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES, &older};
  REQUIRE(xg::xrGetSystemProperties(instance, system, &properties) == XR_SUCCESS);
  const bool supported = expect.backend != ox::PassthroughBackend::none;
  CHECK(bool(older.supportsPassthrough) == supported);
  CHECK(bool(newer.capabilities & XR_PASSTHROUGH_CAPABILITY_BIT_FB) == supported);

  xg::GlesBinding binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR, nullptr, 1, 2, 3};
  XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO, &binding, 0, system};
  XrSession session{};
  REQUIRE(xg::xrCreateSession(instance, &session_info, &session) == XR_SUCCESS);
  XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr,
      XR_REFERENCE_SPACE_TYPE_LOCAL, {{0, 0, 0, 1}, {0, 0, 0}}};
  XrSpace space{};
  REQUIRE(xg::xrCreateReferenceSpace(session, &space_info, &space) == XR_SUCCESS);
  XrSwapchainCreateInfo chain_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  chain_info.format = 0x8058; // GL_RGBA8
  chain_info.width = 32;
  chain_info.height = 24;
  chain_info.arraySize = 2;
  chain_info.sampleCount = chain_info.mipCount = chain_info.faceCount = 1;
  chain_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  XrSwapchain eyes{};
  REQUIRE(xg::xrCreateSwapchain(session, &chain_info, &eyes) == XR_SUCCESS);

  XrPassthroughCreateInfoFB feature_info{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB, nullptr,
      XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB};
  XrPassthroughFB feature{};
  const auto created = xg::xrCreatePassthroughFB(session, &feature_info, &feature);
  if (!supported) {
    // Prey VR relies on this answer to keep its VR path.
    CHECK(created == XR_ERROR_FEATURE_UNSUPPORTED);
  } else {
    REQUIRE(created == XR_SUCCESS);
  }
  XrPassthroughLayerFB layer{};
  if (supported) {
    XrPassthroughLayerCreateInfoFB layer_info{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB, nullptr, feature,
        XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB, XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB};
    REQUIRE(xg::xrCreatePassthroughLayerFB(session, &layer_info, &layer) == XR_SUCCESS);
    if (expect.backend == ox::PassthroughBackend::htc) CHECK(ventureMockXrStats().live_passthrough == 1);
    XrPassthroughStyleFB style{XR_TYPE_PASSTHROUGH_STYLE_FB, nullptr, 0.5f, {0, 0, 0, 0}};
    CHECK(xg::xrPassthroughLayerSetStyleFB(layer, &style) == XR_SUCCESS);
  }

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
    views[eye].subImage = {eyes, {{0, 0}, {32, 24}}, eye};
  }
  // The usual mixed reality arrangement: the camera underneath, the game on top
  // blended by its alpha.
  XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION, nullptr,
      XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT,
      space, 2, views};
  XrCompositionLayerPassthroughFB underlay{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB, nullptr,
      XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, XR_NULL_HANDLE, layer};
  const XrCompositionLayerBaseHeader* with_camera[]{
      reinterpret_cast<const XrCompositionLayerBaseHeader*>(&underlay),
      reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
  const XrCompositionLayerBaseHeader* game_only[]{
      reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
  auto frame = [&](bool camera) {
    XrFrameState timing{XR_TYPE_FRAME_STATE};
    REQUIRE(xg::xrWaitFrame(session, nullptr, &timing) == XR_SUCCESS);
    REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_SUCCESS);
    std::uint32_t index{};
    REQUIRE(xg::xrAcquireSwapchainImage(eyes, nullptr, &index) == XR_SUCCESS);
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    REQUIRE(xg::xrWaitSwapchainImage(eyes, &wait) == XR_SUCCESS);
    REQUIRE(xg::xrReleaseSwapchainImage(eyes, nullptr) == XR_SUCCESS);
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, timing.predictedDisplayTime,
        XR_ENVIRONMENT_BLEND_MODE_OPAQUE, camera ? 2u : 1u, camera ? with_camera : game_only};
    CHECK(xg::xrEndFrame(session, &end) == XR_SUCCESS);
  };
  for (int i = 0; i < 2; ++i) {
    frame(supported);
    const auto stats = ventureMockXrStats();
    CHECK(stats.passthrough_fb == expect.fb_layers);
    CHECK(stats.passthrough_htc == expect.htc_layers);
    CHECK(stats.blend_mode == std::uint32_t(expect.host_mode));
    if (expect.htc_layers) CHECK(stats.passthrough_htc_alpha == doctest::Approx(0.5f));
  }
  if (supported) {
    // A paused layer shows no world, on every backend.
    REQUIRE(xg::xrPassthroughLayerPauseFB(layer) == XR_SUCCESS);
    frame(true);
    const auto stats = ventureMockXrStats();
    CHECK(stats.passthrough_htc == 0);
    CHECK(stats.blend_mode == std::uint32_t(XR_ENVIRONMENT_BLEND_MODE_OPAQUE));
    REQUIRE(xg::xrPassthroughLayerResumeFB(layer) == XR_SUCCESS);
  }
  REQUIRE(xg::xrRequestExitSession(session) == XR_SUCCESS);
  poll();
  CHECK(xg::xrEndSession(session) == XR_SUCCESS);
  if (supported) {
    CHECK(xg::xrDestroyPassthroughLayerFB(layer) == XR_SUCCESS);
    CHECK(xg::xrDestroyPassthroughFB(feature) == XR_SUCCESS);
    if (expect.backend != ox::PassthroughBackend::native) CHECK(ventureMockXrStats().live_passthrough == 0);
  }
  CHECK(xg::xrDestroySpace(space) == XR_SUCCESS);
  CHECK(xg::xrDestroySwapchain(eyes) == XR_SUCCESS);
  CHECK(xg::xrDestroySession(session) == XR_SUCCESS);
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  venture::xr::install_xr_images(nullptr);
}
} // namespace

TEST_CASE("XR passthrough picks a backend by host capability and reaches the real world through it") {
  using B = ox::PassthroughBackend;
  constexpr auto opaque = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  SUBCASE("host XR_FB_passthrough is forwarded") {
    run_profile({"fb,htc,alpha_blend", true, B::native, opaque, 1, 0});
  }
  SUBCASE("XR_HTC_passthrough stands in when host XR_FB_passthrough reports no support") {
    run_profile({"fb-unsupported,htc,alpha_blend", true, B::htc, opaque, 0, 1});
  }
  SUBCASE("an ALPHA_BLEND host shows the world under the game") {
    run_profile({"fb-unsupported,alpha_blend,additive", true, B::blend, XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND, 0, 0});
  }
  SUBCASE("an ADDITIVE host shows the world through the game") {
    run_profile({"additive", true, B::blend, XR_ENVIRONMENT_BLEND_MODE_ADDITIVE, 0, 0});
  }
  SUBCASE("a chosen color key replaces every host backend and keeps the host opaque") {
    run_profile({"fb,htc,alpha_blend", true, B::color_key, opaque, 0, 0, std::array<std::uint8_t, 3>{255, 0, 255}});
  }
  SUBCASE("with mixed reality off a color key is not used either") {
    run_profile({"fb,htc,alpha_blend", false, B::none, opaque, 0, 0, std::array<std::uint8_t, 3>{255, 0, 255}});
  }
  SUBCASE("turning mixed reality off keeps every game in VR") {
    run_profile({"fb,htc,alpha_blend", false, B::none, opaque, 0, 0});
  }
}

// Opt-in, for a real runtime chosen with XR_RUNTIME_JSON (Monado, WiVRn, SteamVR,
// Meta): VENTURE_TEST_HOST_PASSTHROUGH=1. A game's whole passthrough path on
// whatever backend the host offers: properties, creation, a running layer over
// real frames, pause, resume and teardown, with no xrEndFrame error.
TEST_CASE("XR passthrough on the host runtime") {
  if (!venture::environment::getenv("VENTURE_TEST_HOST_PASSTHROUGH")) return;
  venture::test::XrImageFixture interop;
  venture::xr::install_xr_images(&interop);
  venture::process_config().passthrough = true;
  ox::reset_passthrough_probe();
  const char* extensions[]{"XR_KHR_opengl_es_enable", "XR_FB_passthrough"};
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  create.enabledExtensionNames = extensions;
  create.enabledExtensionCount = 2;
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&create, &instance) == XR_SUCCESS);
  XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO, nullptr, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
  XrSystemId system{};
  REQUIRE(xg::xrGetSystem(instance, &system_info, &system) == XR_SUCCESS);
  XrSystemPassthroughProperties2FB newer{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB};
  XrSystemPassthroughPropertiesFB older{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB, &newer};
  XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES, &older};
  REQUIRE(xg::xrGetSystemProperties(instance, system, &properties) == XR_SUCCESS);
  const auto offered = ox::passthrough_backend();
  MESSAGE("host ", venture::xr::host_xr().runtime_name, " (", properties.systemName, "): backend ",
          std::string(ox::passthrough_backend_name(offered)), ", supportsPassthrough ", int(older.supportsPassthrough),
          ", capabilities ", newer.capabilities);
  CHECK(bool(older.supportsPassthrough) == (offered != ox::PassthroughBackend::none));

  xg::GlesBinding binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR, nullptr, 1, 2, 3};
  XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO, &binding, 0, system};
  XrSession session{};
  REQUIRE(xg::xrCreateSession(instance, &session_info, &session) == XR_SUCCESS);
  XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr,
      XR_REFERENCE_SPACE_TYPE_LOCAL, {{0, 0, 0, 1}, {0, 0, 0}}};
  XrSpace space{};
  REQUIRE(xg::xrCreateReferenceSpace(session, &space_info, &space) == XR_SUCCESS);
  XrSwapchainCreateInfo chain_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  chain_info.format = 0x8058; // GL_RGBA8
  chain_info.width = 256;
  chain_info.height = 256;
  chain_info.arraySize = 2;
  chain_info.sampleCount = chain_info.mipCount = chain_info.faceCount = 1;
  chain_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  XrSwapchain eyes{};
  REQUIRE(xg::xrCreateSwapchain(session, &chain_info, &eyes) == XR_SUCCESS);

  XrPassthroughCreateInfoFB feature_info{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB, nullptr,
      XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB};
  XrPassthroughFB feature{};
  const auto created = xg::xrCreatePassthroughFB(session, &feature_info, &feature);
  const auto chosen = ox::passthrough_backend();
  MESSAGE("xrCreatePassthroughFB ", int(created), ", backend ", std::string(ox::passthrough_backend_name(chosen)));
  if (chosen == ox::PassthroughBackend::none) CHECK(created == XR_ERROR_FEATURE_UNSUPPORTED);
  else REQUIRE(created == XR_SUCCESS);
  const bool supported = created == XR_SUCCESS;
  XrPassthroughLayerFB layer{};
  if (supported) {
    XrPassthroughLayerCreateInfoFB layer_info{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB, nullptr, feature,
        XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB, XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB};
    REQUIRE(xg::xrCreatePassthroughLayerFB(session, &layer_info, &layer) == XR_SUCCESS);
  }

  XrSessionState state = XR_SESSION_STATE_UNKNOWN;
  auto poll = [&] {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xg::xrPollEvent(instance, &event) == XR_SUCCESS) {
      if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        state = reinterpret_cast<XrEventDataSessionStateChanged*>(&event)->state;
      event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
  };
  auto await = [&](XrSessionState wanted) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (state != wanted && std::chrono::steady_clock::now() < end) {
      poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return state == wanted;
  };
  REQUIRE(await(XR_SESSION_STATE_READY));
  XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
  REQUIRE(xg::xrBeginSession(session, &begin) == XR_SUCCESS);
  XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                           {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  for (std::uint32_t eye = 0; eye < 2; ++eye) {
    views[eye].pose.orientation.w = 1;
    views[eye].fov = {-0.8f, 0.8f, 0.8f, -0.8f};
    views[eye].subImage = {eyes, {{0, 0}, {256, 256}}, eye};
  }
  XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION, nullptr,
      XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT,
      space, 2, views};
  XrCompositionLayerPassthroughFB underlay{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB, nullptr,
      XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, XR_NULL_HANDLE, layer};
  const XrCompositionLayerBaseHeader* with_camera[]{
      reinterpret_cast<const XrCompositionLayerBaseHeader*>(&underlay),
      reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
  int errors = 0, frames = 0;
  auto run = [&](int count) {
    for (int i = 0; i < count; ++i) {
      poll();
      XrFrameState timing{XR_TYPE_FRAME_STATE};
      REQUIRE(xg::xrWaitFrame(session, nullptr, &timing) == XR_SUCCESS);
      REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_SUCCESS);
      std::uint32_t index{};
      REQUIRE(xg::xrAcquireSwapchainImage(eyes, nullptr, &index) == XR_SUCCESS);
      XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
      REQUIRE(xg::xrWaitSwapchainImage(eyes, &wait) == XR_SUCCESS);
      REQUIRE(xg::xrReleaseSwapchainImage(eyes, nullptr) == XR_SUCCESS);
      XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, timing.predictedDisplayTime,
          XR_ENVIRONMENT_BLEND_MODE_OPAQUE, supported ? 2u : 1u, supported ? with_camera : with_camera + 1};
      if (XR_FAILED(xg::xrEndFrame(session, &end))) ++errors;
      ++frames;
    }
  };
  run(180);
  if (supported) {
    XrPassthroughStyleFB style{XR_TYPE_PASSTHROUGH_STYLE_FB, nullptr, 0.5f, {0, 0, 0, 0}};
    CHECK(xg::xrPassthroughLayerSetStyleFB(layer, &style) == XR_SUCCESS);
    run(60);
    REQUIRE(xg::xrPassthroughLayerPauseFB(layer) == XR_SUCCESS);
    run(60);
    REQUIRE(xg::xrPassthroughLayerResumeFB(layer) == XR_SUCCESS);
    run(60);
  }
  MESSAGE(frames, " frames, ", errors, " xrEndFrame errors, backend at the end ",
          std::string(ox::passthrough_backend_name(ox::passthrough_backend())));
  CHECK(errors == 0);
  try {
    MESSAGE("passthrough.json: ", venture::paths::read(venture::paths::data_dir() / "passthrough.json", 1 << 20));
  } catch (...) {}
  REQUIRE(xg::xrRequestExitSession(session) == XR_SUCCESS);
  const auto stop_end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (state != XR_SESSION_STATE_STOPPING && std::chrono::steady_clock::now() < stop_end) run(1);
  CHECK(xg::xrEndSession(session) == XR_SUCCESS);
  if (supported) {
    CHECK(xg::xrDestroyPassthroughLayerFB(layer) == XR_SUCCESS);
    CHECK(xg::xrDestroyPassthroughFB(feature) == XR_SUCCESS);
  }
  CHECK(xg::xrDestroySpace(space) == XR_SUCCESS);
  CHECK(xg::xrDestroySwapchain(eyes) == XR_SUCCESS);
  CHECK(xg::xrDestroySession(session) == XR_SUCCESS);
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  venture::xr::install_xr_images(nullptr);
}
