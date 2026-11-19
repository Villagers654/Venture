// The system keyboard's host side (xr/system_keyboard.h): its panel placement,
// aim-ray hits and pixel conversion, and over the mock runtime the panel on
// top of a game's frame, uploads only on change, the controllers working its
// keys and the game seeing no controller input while it is open.
#include "../mock_openxr_runtime/control.h"
#include "hle/openxr/openxr.h"
#include "hle/vrapi/state.h"
#include "hle/vrapi/vrapi.h"
#include "xr/math.h"
#include "xr/session.h"
#include "xr/system_keyboard.h"
#include "xr_images_fixture.h"
#include <doctest/doctest.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <thread>
#include <vector>

namespace kb = venture::xr::keyboard;
namespace vr = venture::hle::vrapi;
namespace vg = vr::guest;
namespace xg = venture::hle::openxr::guest;

namespace {
constexpr XrPosef kIdentity{{0, 0, 0, 1}, {0, 0, 0}};
XrQuaternionf about(XrVector3f axis, float radians) {
  const float s = std::sin(radians / 2);
  return {axis.x * s, axis.y * s, axis.z * s, std::cos(radians / 2)};
}
void check_near(XrVector3f a, XrVector3f b) {
  CHECK(a.x == doctest::Approx(b.x).epsilon(0.001));
  CHECK(a.y == doctest::Approx(b.y).epsilon(0.001));
  CHECK(a.z == doctest::Approx(b.z).epsilon(0.001));
}
// A 64x32 picture with one key over all of it.
void show_picture(std::uint32_t color, std::int32_t layout = 7) {
  kb::show(std::vector<std::uint32_t>(64 * 32, color), 64, 32, {{0, 0, 64, 32}}, layout);
}
struct Closed {
  ~Closed() {
    kb::hide();
    kb::take_events();
  }
};
bool has_event(const std::vector<std::int32_t> &events, kb::Event kind, int a, int b) {
  for (std::size_t i = 0; i + 3 < events.size(); i += 4)
    if (events[i] == kind && events[i + 1] == a && events[i + 2] == b) return true;
  return false;
}
std::optional<XrCompositionLayerQuad> panel_quad() {
  std::array<XrCompositionLayerQuad, 8> quads{};
  const auto count = std::min<std::uint32_t>(ventureMockXrLastQuads(quads.data(), 8), 8);
  for (std::uint32_t i = 0; i < count; ++i)
    if (std::abs(quads[i].size.width - 0.6f) < 1e-4f) return quads[i];
  return std::nullopt;
}
struct VrApiGame {
  venture::test::XrImageFixture images;
  vr::abi::Mobile *mobile{};
  vr::abi::Submit submit{0, 1, 0, 0, 0, 0, nullptr};
  std::uint64_t index = 0;
  VrApiGame() {
    venture::xr::install_xr_images(&images);
    vr::abi::Init init{1, 1, 1, 42, 0, 0x10300, {1, 2, 3}};
    REQUIRE(vg::vrapi_Initialize(&init) == 0);
    vr::abi::Mode mode{2, 0, {1, 2, 3}, 4, 5, 6};
    mobile = vg::vrapi_EnterVrMode(&mode);
    REQUIRE(mobile);
    vr::abi::Event event{};
    while (vg::vrapi_PollEvent(&event) == vr::abi::success) {}
  }
  ~VrApiGame() {
    if (vr::state().mobile) vg::vrapi_LeaveVrMode(mobile);
    vg::vrapi_Shutdown();
    venture::xr::install_xr_images(nullptr);
    MockXrScript restored{};
    ventureMockXrSetScript(&restored);
  }
  void frame() {
    submit.frame_index = ++index;
    submit.display_time = vg::vrapi_GetPredictedDisplayTime(mobile, std::int64_t(index));
    REQUIRE(vg::vrapi_SubmitFrame2(mobile, &submit) == 0);
  }
  // Frames until the host has `released` more images (uploads are never waited for).
  bool frames_until_released(std::uint32_t released) {
    for (int i = 0; i < 200; ++i) {
      frame();
      if (ventureMockXrStats().released >= released) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
  }
  vr::abi::RemoteState input(int hand) {
    REQUIRE(vr::sync_input() == XR_SUCCESS);
    vr::abi::RemoteState state{};
    state.header.type = 4;
    REQUIRE(vg::vrapi_GetCurrentInputState(mobile, std::uint32_t(hand + 1), &state.header) == 0);
    return state;
  }
};
} // namespace

TEST_CASE("The keyboard panel opens in front of and below the head, turned with its heading only") {
  const XrPosef ahead{{0, 0, 0, 1}, {0, 1.6f, 0}};
  check_near(kb::panel_pose(ahead).position, {0, 1.3f, -0.55f});
  // Looking left (+90 degrees about up) puts it to the left.
  const XrPosef left{about({0, 1, 0}, 1.5707963f), {0, 1.6f, 0}};
  check_near(kb::panel_pose(left).position, {-0.55f, 1.3f, 0});
  // Looking down does not tip it: only the heading counts.
  const XrPosef down{about({1, 0, 0}, -0.7853982f), {0, 1.6f, 0}};
  check_near(kb::panel_pose(down).position, {0, 1.3f, -0.55f});
  // Tilted back to face the eyes: its front (+Z) points up toward the head.
  const auto front = venture::xr::rotate(kb::panel_pose(ahead).orientation, {0, 0, 1});
  CHECK(front.y > 0.2f);
  CHECK(front.z > 0.8f);
}

TEST_CASE("An aim ray finds the pixel it points at on the keyboard's front only") {
  const auto panel = kb::panel_pose({{0, 0, 0, 1}, {0, 1.6f, 0}});
  const XrExtent2Df size{0.6f, 0.3f};
  auto aim = [&](XrVector3f at, XrQuaternionf turn = {0, 0, 0, 1}) {
    return venture::xr::compose(panel, XrPosef{turn, at});
  };
  auto centre = kb::hit(panel, size, aim({0, 0, 0.5f}), 64, 32);
  REQUIRE(centre);
  CHECK(centre->x == doctest::Approx(32));
  CHECK(centre->y == doctest::Approx(16));
  CHECK(centre->distance == doctest::Approx(0.5f));
  auto corner = kb::hit(panel, size, aim({0.15f, 0.075f, 0.3f}), 64, 32);
  REQUIRE(corner);
  CHECK(corner->x == doctest::Approx(48));  // right of centre
  CHECK(corner->y == doctest::Approx(8));   // above centre: row 0 is the top
  CHECK_FALSE(kb::hit(panel, size, aim({0.31f, 0, 0.5f}), 64, 32));               // beside it
  CHECK_FALSE(kb::hit(panel, size, aim({0, 0, -0.5f}), 64, 32));                  // behind it
  CHECK_FALSE(kb::hit(panel, size, aim({0, 0, 0.5f}, about({0, 1, 0}, 3.14159f)), 64, 32));  // facing away
  const std::vector<kb::Rect> keys{{0, 0, 10, 10}, {10, 0, 10, 10}};
  CHECK(kb::key_at(keys, 5, 5) == 0);
  CHECK(kb::key_at(keys, 10, 9.5f) == 1);
  CHECK(kb::key_at(keys, 25, 5) == -1);
}

TEST_CASE("Keyboard pixels go into RGBA and BGRA swapchains, linearized for UNORM") {
  std::uint8_t out[4];
  const std::uint32_t opaque = 0xFF112233;
  kb::convert(&opaque, 1, VK_FORMAT_R8G8B8A8_SRGB, out);
  CHECK((out[0] == 0x11 && out[1] == 0x22 && out[2] == 0x33 && out[3] == 0xFF));
  kb::convert(&opaque, 1, VK_FORMAT_B8G8R8A8_SRGB, out);
  CHECK((out[0] == 0x33 && out[1] == 0x22 && out[2] == 0x11 && out[3] == 0xFF));
  // UNORM: sRGB 0x80 is about 0.216 linear; premultiplied alpha stays premultiplied.
  const std::uint32_t grey = 0xFF808080, half_white = 0x80808080, clear = 0;
  kb::convert(&grey, 1, VK_FORMAT_R8G8B8A8_UNORM, out);
  CHECK(int(out[0]) == doctest::Approx(55).epsilon(0.03));
  CHECK(out[3] == 0xFF);
  kb::convert(&half_white, 1, VK_FORMAT_R8G8B8A8_UNORM, out);
  CHECK((out[0] == 0x80 && out[3] == 0x80));
  kb::convert(&clear, 1, VK_FORMAT_B8G8R8A8_UNORM, out);
  CHECK((out[0] == 0 && out[3] == 0));
}

TEST_CASE("The keyboard's layers fit under the host's layer limit without dropping a projection") {
  XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerQuad first{XR_TYPE_COMPOSITION_LAYER_QUAD}, second{XR_TYPE_COMPOSITION_LAYER_QUAD};
  auto *p = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection);
  auto *a = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&first);
  auto *b = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&second);
  std::vector<const XrCompositionLayerBaseHeader *> layers{p, a, b};
  CHECK(kb::fit_layers(layers, 0, 3) == 3);  // no known limit
  CHECK(layers.size() == 3);
  CHECK(kb::fit_layers(layers, 3, 1) == 1);  // the game's last quad makes room
  CHECK(layers == std::vector{p, a});
  layers = {p, a, p};
  CHECK(kb::fit_layers(layers, 3, 2) == 1);  // the quad goes; projections stay
  CHECK(layers == std::vector{p, p});
  layers = {p, p};
  CHECK(kb::fit_layers(layers, 2, 1) == 0);  // nothing to drop: no panel
  CHECK(layers.size() == 2);
}

TEST_CASE("The open keyboard is drawn over a VrApi game, uploaded only when its picture changes") {
  Closed closed;
  VrApiGame game;
  game.frame();
  CHECK_FALSE(panel_quad());
  const auto before = ventureMockXrStats();
  show_picture(0xFF202020);
  // The panel and the cursor picture go up together, once.
  REQUIRE(game.frames_until_released(before.released + 2));
  game.frame();
  const auto quad = panel_quad();
  REQUIRE(quad);
  CHECK(quad->size.height == doctest::Approx(0.3f));
  CHECK(quad->layerFlags == XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT);
  CHECK(quad->subImage.imageRect.extent.width == 64);
  const auto uploaded = ventureMockXrStats();
  CHECK(uploaded.released == before.released + 2);
  for (int i = 0; i < 4; ++i) game.frame();
  CHECK(ventureMockXrStats().released == uploaded.released);  // nothing new to show
  CHECK(ventureMockXrStats().acquired == uploaded.acquired);
  show_picture(0xFF404040);
  REQUIRE(game.frames_until_released(uploaded.released + 1));
  for (int i = 0; i < 4; ++i) game.frame();
  CHECK(ventureMockXrStats().released == uploaded.released + 1);  // the panel only
  // The End the host got owns the layers: the game's owner stays alive with it.
  {
    std::lock_guard lock(venture::xr::session().mutex);
    auto owner = std::make_shared<int>(1);
    const std::weak_ptr<int> watched = owner;
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
    info.displayTime = vg::vrapi_GetPredictedDisplayTime(game.mobile, std::int64_t(game.index + 1));
    info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    auto extended = kb::extend(venture::xr::session().handle, info, owner);
    REQUIRE(extended);
    owner.reset();
    CHECK_FALSE(watched.expired());
    CHECK(extended->info.layerCount >= 1);
    CHECK(extended->info.layers == extended->layers.data());
    CHECK(extended->layers[0]->type == XR_TYPE_COMPOSITION_LAYER_QUAD);
    extended.reset();
    CHECK(watched.expired());
  }
  kb::hide();
  game.frame();
  CHECK_FALSE(panel_quad());
}

TEST_CASE("Controllers work the keyboard's keys; a VrApi game sees them at rest while it is open") {
  Closed closed;
  VrApiGame game;
  MockXrScript script{};
  script.left.trigger = 0;
  script.right.trigger = 0.9f;
  ventureMockXrSetScript(&script);
  CHECK(game.input(1).trigger == doctest::Approx(0.9f));
  show_picture(0xFF202020);
  const auto before = ventureMockXrStats();
  REQUIRE(game.frames_until_released(before.released + 2));
  kb::take_events();
  // Both mock controllers aim straight ahead from below the head: at the panel.
  const auto held = game.input(1);
  CHECK(held.trigger == 0);
  CHECK(held.buttons == 0);
  auto events = kb::take_events();
  CHECK(has_event(events, kb::hover, 0, 0));
  CHECK(has_event(events, kb::hover, 1, 0));
  CHECK_FALSE(has_event(events, kb::press, 1, 0));  // held as it opened: no press
  script.right.trigger = 0;
  ventureMockXrSetScript(&script);
  game.input(1);
  const auto pulses = ventureMockXrStats().haptic_pulses;
  script.right.trigger = 0.9f;
  ventureMockXrSetScript(&script);
  game.input(1);
  script.right.trigger = 0;
  ventureMockXrSetScript(&script);
  game.input(1);
  events = kb::take_events();
  CHECK(has_event(events, kb::press, 1, 0));
  CHECK(has_event(events, kb::release, 1, 0));
  CHECK(ventureMockXrAwaitHaptics(pulses + 1, 2000));  // a tick on the pressing hand
  // The desktop mirror's typing arrives as code points and Android key codes.
  kb::type_text("a\xC3\xA9");
  kb::type_key(67);
  events = kb::take_events();
  CHECK(has_event(events, kb::text, 'a', 0));
  CHECK(has_event(events, kb::text, 0xE9, 0));
  CHECK(has_event(events, kb::key, 67, 0));
  kb::hide();
  script.right.trigger = 0.9f;
  ventureMockXrSetScript(&script);
  CHECK(game.input(1).trigger == doctest::Approx(0.9f));
  kb::type_text("b");
  CHECK(kb::take_events().empty());  // closed: nothing queues
}

TEST_CASE("An OpenXR game's actions read inactive while the keyboard is open") {
  Closed closed;
  venture::test::XrImageFixture interop;
  venture::xr::install_xr_images(&interop);
  const char *extensions[]{"XR_KHR_opengl_es_enable"};
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  create.enabledExtensionCount = 1;
  create.enabledExtensionNames = extensions;
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&create, &instance) == XR_SUCCESS);
  xg::GlesBinding graphics{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR, nullptr, 1, 2, 3};
  XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO, &graphics, 0, 1};
  XrSession session{};
  REQUIRE(xg::xrCreateSession(instance, &ci, &session) == XR_SUCCESS);
  XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strcpy(set_info.actionSetName, "keyboard_test");
  std::strcpy(set_info.localizedActionSetName, "Keyboard test");
  XrActionSet set{};
  REQUIRE(xg::xrCreateActionSet(instance, &set_info, &set) == XR_SUCCESS);
  XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
  ai.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
  std::strcpy(ai.actionName, "pull");
  std::strcpy(ai.localizedActionName, "pull");
  XrAction pull{};
  REQUIRE(xg::xrCreateAction(set, &ai, &pull) == XR_SUCCESS);
  XrPath touch{}, trigger{};
  REQUIRE(xg::xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &touch) == XR_SUCCESS);
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/right/input/trigger/value", &trigger) == XR_SUCCESS);
  XrActionSuggestedBinding binding{pull, trigger};
  XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr, touch, 1,
                                                  &binding};
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
  script.right.trigger = 0.8f;
  ventureMockXrSetScript(&script);
  auto read = [&] {
    XrActiveActionSet active{set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
    REQUIRE(xg::xrSyncActions(session, &sync) == XR_SUCCESS);
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, pull, XR_NULL_PATH};
    XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
    REQUIRE(xg::xrGetActionStateFloat(session, &gi, &state) == XR_SUCCESS);
    return state;
  };
  auto state = read();
  CHECK(state.isActive);
  CHECK(state.currentState == doctest::Approx(0.8f));
  show_picture(0xFF202020);
  state = read();
  CHECK_FALSE(state.isActive);
  CHECK(state.currentState == 0);
  kb::hide();
  state = read();
  CHECK(state.isActive);
  CHECK(state.currentState == doctest::Approx(0.8f));
  CHECK(xg::xrDestroySession(session) == XR_SUCCESS);
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  venture::xr::install_xr_images(nullptr);
  MockXrScript restored{};
  ventureMockXrSetScript(&restored);
}
