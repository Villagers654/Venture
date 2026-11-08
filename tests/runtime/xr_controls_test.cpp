// The player's controls end to end over the mock runtime: each controller's
// inputs pressed on the host, and what the game sees.
#include "common/controls.h"
#include "common/environment.h"
#include "common/log.h"
#include "../mock_openxr_runtime/control.h"
#include "hle/openxr/openxr.h"
#include "hle/openxr/state.h"
#include "hle/vrapi/state.h"
#include "hle/vrapi/vrapi.h"
#include "xr/bindings.h"
#include "xr/host_xr.h"
#include "xr/menu_chord.h"
#include "xr_images_fixture.h"
#include "xr_menu_binding_fixture.h"
#include <doctest/doctest.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <set>
#include <thread>
#include <vector>

namespace vr = venture::hle::vrapi;
namespace vg = vr::guest;
namespace xg = venture::hle::openxr::guest;
using namespace venture::controls;

namespace {
struct ScopedControls {
  bool present = std::getenv("VENTURE_CONTROLS") != nullptr;
  std::string previous = present ? std::getenv("VENTURE_CONTROLS") : "";
  explicit ScopedControls(const std::string& value) { set(value.c_str()); }
  ~ScopedControls() { set(present ? previous.c_str() : nullptr); }
  static void set(const char* value) {
#ifdef _WIN32
    _putenv_s("VENTURE_CONTROLS", value ? value : "");
#else
    if (value) setenv("VENTURE_CONTROLS", value, 1); else unsetenv("VENTURE_CONTROLS");
#endif
    venture::environment::refresh();
    venture::xr::invalidate_host_procedures();
  }
};
void press(MockXrController& c, std::string_view path, float value = 1) {
  for (auto& raw : c.raw)
    if (!raw.path[0] || path == raw.path) {
      std::snprintf(raw.path, sizeof raw.path, "%.*s", int(path.size()), path.data());
      raw.value = value;
      return;
    }
  FAIL("no free mock input slot");
}
// A VrApi game on the mock runtime reporting `profile` for both hands.
struct VrApiGame {
  venture::test::XrImageFixture images;
  decltype(vg::vrapi_EnterVrMode(nullptr)) mobile{};
  explicit VrApiGame(const char* profile) {
    ventureMockXrSetProfile(profile);
    venture::xr::install_xr_images(&images);
    vr::abi::Init init{1, 1, 1, 42, 0, 0x10300, {1, 2, 3}};
    REQUIRE(vg::vrapi_Initialize(&init) == 0);
    vr::abi::Mode mode{2, 0, {1, 2, 3}, 4, 5, 6};
    mobile = vg::vrapi_EnterVrMode(&mode);
    REQUIRE(mobile);
    venture::xr::note_menu_profiles_changed(vr::session().handle);
    MockXrScript rest{};
    ventureMockXrSetScript(&rest);
    REQUIRE(vr::sync_input() == XR_SUCCESS);
    REQUIRE(vg::vrapi_WaitFrame(mobile, 1) == 0);
  }
  ~VrApiGame() {
    vg::vrapi_LeaveVrMode(mobile);
    vg::vrapi_Shutdown();
    venture::xr::install_xr_images(nullptr);
    MockXrScript restored{};
    ventureMockXrSetScript(&restored);
    ventureMockXrSetProfile(nullptr);
  }
  // The game's controller state for one hand after the host reports `script`.
  vr::abi::RemoteState read(const MockXrScript& script, int hand) {
    ventureMockXrSetScript(&script);
    REQUIRE(vr::sync_input() == XR_SUCCESS);
    vr::abi::RemoteState state{};
    state.header.type = 4;
    REQUIRE(vg::vrapi_GetCurrentInputState(mobile, std::uint32_t(hand + 1), &state.header) == 0);
    return state;
  }
};
constexpr std::uint32_t kMenu = 0x100000u, kStickClick = 0x80000000u;
std::uint32_t face_bit(Quest q, int hand) {
  return q == Quest::primary ? (hand ? 1u : 0x100u) : (hand ? 2u : 0x200u);
}
// Whether the game sees Quest input `q` of `hand` in `state`.
bool sees(const vr::abi::RemoteState& state, Quest q, int hand) {
  switch (q) {
    case Quest::primary:
    case Quest::secondary: return (state.buttons & face_bit(q, hand)) != 0;
    case Quest::stick: return state.raw_joystick.x > 0.4f;
    case Quest::stick_click: return (state.buttons & kStickClick) != 0;
    case Quest::trigger: return state.trigger > 0.9f;
    case Quest::grip: return state.grip > 0.9f;
    case Quest::thumbrest: return (state.touches & 0x1000u) != 0;
    case Quest::menu: return (state.buttons & kMenu) != 0;
    default: return false;
  }
}
// The host input of a row, pressed.
void press_row(MockXrScript& script, int hand, std::string_view path, Quest q) {
  auto& c = hand ? script.right : script.left;
  if (q == Quest::stick) c.thumbstick = {0.5f, 0};
  else press(c, path, 1);
}
void hold(std::chrono::milliseconds time) { std::this_thread::sleep_for(time); }
}  // namespace

TEST_CASE("Every controller's default inputs reach a VrApi game as the Quest inputs they stand for") {
  venture::test::ScopedMenuMode automatic("");
  ScopedControls defaults("");
  REQUIRE(venture::xr::host_dpad());
  for (const auto& p : kProfiles) {
    CAPTURE(p.path);
    VrApiGame game(std::string(p.path).c_str());
    for (int hand = 0; hand < 2; ++hand)
      for (int q = 0; q < kQuestCount; ++q) {
        const auto path = p.defaults[size_t(hand)][size_t(q)];
        if (path.empty() || q == int(Quest::menu)) continue;
        CAPTURE(hand);
        CAPTURE(path);
        CHECK_FALSE(sees(game.read(MockXrScript{}, hand), Quest(q), hand));
        MockXrScript script{};
        press_row(script, hand, path, Quest(q));
        const auto state = game.read(script, hand);
        CHECK(sees(state, Quest(q), hand));
        // Nothing else lights up (the trigger rests at the mock's 0.25).
        for (int other = 0; other < kQuestCount; ++other)
          if (other != q && other != int(Quest::menu)) CHECK_FALSE(sees(state, Quest(other), hand));
        CHECK_FALSE(sees(game.read(MockXrScript{}, 0), Quest::menu, 0));
      }
    // The automatic menu: the controller's own left input (if any), and both stick
    // clicks pressed together (at once); one stick click alone stays the game's.
    MockXrScript script{};
    if (!p.menu_path.empty()) {
      press(script.left, p.menu_path);
      CHECK(sees(game.read(script, 0), Quest::menu, 0));
      CHECK_FALSE(sees(game.read(MockXrScript{}, 0), Quest::menu, 0));
      script = {};
    }
    const auto left_click = p.defaults[0][size_t(Quest::stick_click)], right_click = p.defaults[1][size_t(Quest::stick_click)];
    press(script.left, left_click);
    CHECK_FALSE(sees(game.read(script, 0), Quest::menu, 0));
    CHECK(sees(game.read(script, 0), Quest::stick_click, 0));
    press(script.right, right_click);
    const auto both = game.read(script, 0);
    CHECK(sees(both, Quest::menu, 0));
    CHECK(sees(both, Quest::stick_click, 0));  // the game still sees its click
    CHECK(sees(game.read(script, 0), Quest::menu, 0));  // held while both are
    // Releasing one ends the press; it rearms only once both are up.
    MockXrScript one{};
    press(one.left, left_click);
    CHECK_FALSE(sees(game.read(one, 0), Quest::menu, 0));
    CHECK_FALSE(sees(game.read(script, 0), Quest::menu, 0));
    CHECK_FALSE(sees(game.read(MockXrScript{}, 0), Quest::menu, 0));
    CHECK(sees(game.read(script, 0), Quest::menu, 0));
    CHECK_FALSE(sees(game.read(MockXrScript{}, 0), Quest::menu, 0));
  }
}

TEST_CASE("Moved, cleared and swapped inputs reach a VrApi game where the player put them") {
  venture::test::ScopedMenuMode automatic("");
  Config config;
  auto& touch = config["/interaction_profiles/oculus/touch_controller"];
  touch.rows[0][int(Quest::primary)] = "/input/y/click";
  touch.rows[0][int(Quest::secondary)] = "/input/x/click";
  touch.rows[1][int(Quest::stick_click)] = "";
  touch.rows[1][int(Quest::grip)] = "/input/trigger/value";
  ScopedControls controls(encode(config));
  VrApiGame game("/interaction_profiles/oculus/touch_controller");
  MockXrScript script{};
  press(script.left, "/input/x/click");
  auto left = game.read(script, 0);
  CHECK(sees(left, Quest::secondary, 0));
  CHECK_FALSE(sees(left, Quest::primary, 0));
  script = {};
  script.right.thumbstick_click = true;
  CHECK_FALSE(sees(game.read(script, 1), Quest::stick_click, 1));
  // The right trigger now drives both the trigger and the grip.
  script = {};
  press(script.right, "/input/trigger/value");
  const auto right = game.read(script, 1);
  CHECK(sees(right, Quest::trigger, 1));
  CHECK(sees(right, Quest::grip, 1));
  // The left side keeps its defaults.
  script = {};
  press(script.left, "/input/squeeze/value");
  CHECK(sees(game.read(script, 0), Quest::grip, 0));
}

TEST_CASE("The player's menu choice opens a VrApi game's menu") {
  venture::test::ScopedMenuMode automatic("");
  const char* index = "/interaction_profiles/valve/index_controller";
  const char* touch = "/interaction_profiles/oculus/touch_controller";
  Config config;
  SUBCASE("Hold the right B for 1 s; a short press still reaches the game") {
    config[index].menu = {MenuMode::hold, "/user/hand/right/input/b/click", {}, 1000};
    ScopedControls controls(encode(config));
    VrApiGame game(index);
    MockXrScript script{};
    press(script.right, "/input/b/click");
    CHECK(sees(game.read(script, 1), Quest::secondary, 1));
    CHECK_FALSE(sees(game.read(script, 0), Quest::menu, 0));
    hold(std::chrono::milliseconds(600));
    CHECK_FALSE(sees(game.read(script, 0), Quest::menu, 0));
    hold(std::chrono::milliseconds(450));
    CHECK(sees(game.read(script, 0), Quest::menu, 0));
    CHECK(sees(game.read(script, 0), Quest::menu, 0));  // held while the button is
    CHECK_FALSE(sees(game.read(MockXrScript{}, 0), Quest::menu, 0));
    // The trackpad press no longer opens it.
    script = {};
    press(script.left, "/input/trackpad/force");
    CHECK_FALSE(sees(game.read(script, 0), Quest::menu, 0));
  }
  SUBCASE("Two buttons together: left stick click and right B for 0.5 s") {
    config[touch].menu = {MenuMode::chord, "/user/hand/left/input/thumbstick/click", "/user/hand/right/input/b/click", 500};
    ScopedControls controls(encode(config));
    VrApiGame game(touch);
    MockXrScript script{};
    script.left.thumbstick_click = true;
    CHECK_FALSE(sees(game.read(script, 0), Quest::menu, 0));
    hold(std::chrono::milliseconds(520));
    CHECK_FALSE(sees(game.read(script, 0), Quest::menu, 0));
    press(script.right, "/input/b/click");
    CHECK_FALSE(sees(game.read(script, 0), Quest::menu, 0));
    hold(std::chrono::milliseconds(520));
    CHECK(sees(game.read(script, 0), Quest::menu, 0));
    // The left menu button is no longer the menu.
    script = {};
    script.left.menu = true;
    CHECK_FALSE(sees(game.read(script, 0), Quest::menu, 0));
  }
  SUBCASE("One right-hand button presses the menu at once") {
    config[touch].menu = {MenuMode::button, "/user/hand/right/input/a/click", {}, 500};
    ScopedControls controls(encode(config));
    VrApiGame game(touch);
    MockXrScript script{};
    press(script.right, "/input/a/click");
    CHECK(sees(game.read(script, 0), Quest::menu, 0));
    CHECK(sees(game.read(script, 1), Quest::primary, 1));
  }
  SUBCASE("One left-hand button takes the game's own menu binding") {
    config[index].menu = {MenuMode::button, "/user/hand/left/input/a/click", {}, 500};
    ScopedControls controls(encode(config));
    VrApiGame game(index);
    MockXrScript script{};
    press(script.left, "/input/a/click");
    const auto state = game.read(script, 0);
    CHECK(sees(state, Quest::menu, 0));
    CHECK(sees(state, Quest::primary, 0));  // X shares it, as the Controls page warns
  }
  SUBCASE("Off: the game gets no menu, not even the stick-click fallback") {
    config[touch].menu.mode = MenuMode::off;
    ScopedControls controls(encode(config));
    VrApiGame game(touch);
    MockXrScript script{};
    script.left.menu = true;
    CHECK_FALSE(sees(game.read(script, 0), Quest::menu, 0));
    script.left.thumbstick_click = script.right.thumbstick_click = true;
    hold(std::chrono::milliseconds(520));
    CHECK_FALSE(sees(game.read(script, 0), Quest::menu, 0));
  }
}

TEST_CASE("A Touch-shaped controller's menu is its menu button and both stick clicks pressed together") {
  venture::test::ScopedMenuMode automatic("");
  ScopedControls controls("");
  for (const char* profile : {"/interaction_profiles/oculus/touch_controller", "/interaction_profiles/meta/touch_controller_plus",
                              "/interaction_profiles/facebook/touch_controller_pro", "/interaction_profiles/bytedance/pico4_controller",
                              "/interaction_profiles/htc/vive_focus3_controller", "/interaction_profiles/htc/vive_cosmos_controller"}) {
    CAPTURE(profile);
    VrApiGame game(profile);
    MockXrScript script{};
    // The left menu button where the runtime gives it to the game (Meta, Virtual
    // Desktop, Pico); both stick clicks where it keeps it (SteamVR's dashboard).
    script.left.menu = true;
    CHECK(sees(game.read(script, 0), Quest::menu, 0));
    CHECK_FALSE(sees(game.read(MockXrScript{}, 0), Quest::menu, 0));
    script = {};
    script.left.thumbstick_click = true;
    CHECK_FALSE(sees(game.read(script, 0), Quest::menu, 0));
    CHECK(sees(game.read(script, 0), Quest::stick_click, 0));
    script.right.thumbstick_click = true;
    CHECK(sees(game.read(script, 0), Quest::menu, 0));  // at once, no hold
    CHECK_FALSE(sees(game.read(MockXrScript{}, 0), Quest::menu, 0));
  }
  {
    // A player who picks the left menu button gets it alone.
    Config config;
    config["/interaction_profiles/oculus/touch_controller"].menu = {MenuMode::button, "/user/hand/left/input/menu/click", {}, kHoldMs};
    ScopedControls chosen(encode(config));
    VrApiGame game("/interaction_profiles/oculus/touch_controller");
    MockXrScript script{};
    script.left.menu = true;
    CHECK(sees(game.read(script, 0), Quest::menu, 0));
    script = {};
    script.left.thumbstick_click = script.right.thumbstick_click = true;
    CHECK_FALSE(sees(game.read(script, 0), Quest::menu, 0));
  }
  {
    // A player who picks both stick clicks gets them alone.
    Config config;
    config["/interaction_profiles/oculus/touch_controller"].menu = {
        MenuMode::chord, "/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click", kChordHoldMs};
    ScopedControls chosen(encode(config));
    VrApiGame game("/interaction_profiles/oculus/touch_controller");
    MockXrScript script{};
    script.left.menu = true;
    CHECK_FALSE(sees(game.read(script, 0), Quest::menu, 0));
    script = {};
    script.left.thumbstick_click = script.right.thumbstick_click = true;
    CHECK(sees(game.read(script, 0), Quest::menu, 0));
  }
}

TEST_CASE("Without XR_EXT_dpad_binding a Vive wand's trackpad press is X and A") {
  venture::test::ScopedMenuMode automatic("");
  ScopedControls defaults("");
  auto& host = venture::xr::host_xr();
  struct Restore {
    venture::xr::HostXr& host;
    std::set<std::string> saved;
    ~Restore() {
      host.enabled_extensions = saved;
      venture::xr::invalidate_host_procedures();
    }
  } restore{host, host.enabled_extensions};
  host.enabled_extensions.erase("XR_EXT_dpad_binding");
  venture::xr::invalidate_host_procedures();
  REQUIRE_FALSE(venture::xr::host_dpad());
  VrApiGame game("/interaction_profiles/htc/vive_controller");
  for (int hand = 0; hand < 2; ++hand) {
    MockXrScript script{};
    (hand ? script.right : script.left).thumbstick_click = true;  // the mock's trackpad/click
    const auto state = game.read(script, hand);
    CHECK(sees(state, Quest::primary, hand));
    CHECK_FALSE(sees(state, Quest::secondary, hand));
    CHECK_FALSE(sees(state, Quest::stick_click, hand));
  }
}

TEST_CASE("An OpenXR game's Touch bindings follow the player's controls on another controller") {
  venture::test::ScopedMenuMode automatic("");
  Config config;
  config["/interaction_profiles/htc/vive_controller"].rows[0][int(Quest::primary)] = "/input/trackpad/dpad_up";
  config["/interaction_profiles/htc/vive_controller"].rows[0][int(Quest::secondary)] = "/input/trackpad/dpad_down";
  config["/interaction_profiles/htc/vive_controller"].menu = {MenuMode::hold, "/user/hand/left/input/menu/click", {}, 500};
  ScopedControls controls(encode(config));
  ventureMockXrSetProfile("/interaction_profiles/htc/vive_controller");
  venture::test::XrImageFixture interop;
  venture::xr::install_xr_images(&interop);
  const char* extensions[]{"XR_KHR_opengl_es_enable"};
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
  std::strcpy(set_info.actionSetName, "controls_test");
  std::strcpy(set_info.localizedActionSetName, "Controls test");
  XrActionSet set{};
  REQUIRE(xg::xrCreateActionSet(instance, &set_info, &set) == XR_SUCCESS);
  XrPath left{}, touch{};
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/left", &left) == XR_SUCCESS);
  REQUIRE(xg::xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &touch) == XR_SUCCESS);
  auto action = [&](const char* name) {
    XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
    ai.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    ai.countSubactionPaths = 1;
    ai.subactionPaths = &left;
    std::strcpy(ai.actionName, name);
    std::strcpy(ai.localizedActionName, name);
    XrAction out{};
    REQUIRE(xg::xrCreateAction(set, &ai, &out) == XR_SUCCESS);
    return out;
  };
  const XrAction x = action("x"), y = action("y"), menu = action("menu");
  XrPath x_path{}, y_path{}, menu_path{};
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/left/input/x/click", &x_path) == XR_SUCCESS);
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/left/input/y/click", &y_path) == XR_SUCCESS);
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/left/input/menu/click", &menu_path) == XR_SUCCESS);
  XrActionSuggestedBinding bindings[]{{x, x_path}, {y, y_path}, {menu, menu_path}};
  XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr, touch, 3,
                                                  bindings};
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
  // The game sees its own profile and its own paths.
  XrInteractionProfileState current{XR_TYPE_INTERACTION_PROFILE_STATE};
  REQUIRE(xg::xrGetCurrentInteractionProfile(session, left, &current) == XR_SUCCESS);
  CHECK(current.interactionProfile == touch);
  XrBoundSourcesForActionEnumerateInfo bound{XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO, nullptr, x};
  XrPath source{};
  std::uint32_t count = 0;
  REQUIRE(xg::xrEnumerateBoundSourcesForAction(session, &bound, 1, &count, &source) == XR_SUCCESS);
  CHECK(count == 1);
  CHECK(source == x_path);
  XrActiveActionSet active{set, XR_NULL_PATH};
  XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
  auto state = [&](XrAction a, const MockXrScript& script) {
    ventureMockXrSetScript(&script);
    REQUIRE(xg::xrSyncActions(session, &sync) == XR_SUCCESS);
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, a, XR_NULL_PATH};
    XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
    REQUIRE(xg::xrGetActionStateBoolean(session, &get, &value) == XR_SUCCESS);
    return value.isActive && value.currentState;
  };
  MockXrScript script{};
  press(script.left, "/input/trackpad/dpad_up");
  CHECK(state(x, script));
  CHECK_FALSE(state(y, script));
  script = {};
  press(script.left, "/input/trackpad/dpad_down");
  CHECK(state(y, script));
  CHECK_FALSE(state(x, script));
  // The menu button held 0.5 s.
  script = {};
  script.left.menu = true;
  CHECK_FALSE(state(menu, script));
  std::this_thread::sleep_for(std::chrono::milliseconds(520));
  CHECK(state(menu, script));
  CHECK_FALSE(state(menu, MockXrScript{}));
  CHECK(xg::xrDestroySession(session) == XR_SUCCESS);
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  venture::xr::install_xr_images(nullptr);
  MockXrScript restored{};
  ventureMockXrSetScript(&restored);
  ventureMockXrSetProfile(nullptr);
}

TEST_CASE("An OpenXR game that also binds both stick clicks gets its Quest menu from the menu button and both stick clicks") {
  // OVRPlugin's OpenXR backend (Unreal games such as Asgard's Wrath 2, Unity games
  // such as Attack on Titan VR) binds the left menu and both stick clicks on Touch.
  // The default reaches the game's menu from the controller's menu button where the
  // runtime leaves it to games, and from both stick clicks where it keeps it
  // (SteamVR's dashboard). A space the game destroys may share its handle value with
  // the game's actions on a runtime that numbers each handle type from 1 (VDXR, the
  // xr_typed_handles_menu test); the menu must survive it.
  venture::test::ScopedMenuMode automatic("");
  ScopedControls defaults("");
  const bool typed = std::getenv("VENTURE_MOCK_TYPED_HANDLES") != nullptr;
  for (const auto& p : kProfiles) {
    CAPTURE(p.path);
    ventureMockXrSetProfile(std::string(p.path).c_str());
    venture::test::XrImageFixture interop;
    venture::xr::install_xr_images(&interop);
    const char* extensions[]{"XR_KHR_opengl_es_enable"};
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
    std::strcpy(set_info.actionSetName, "default");
    std::strcpy(set_info.localizedActionSetName, "OVRPlugin-like");
    XrActionSet set{};
    REQUIRE(xg::xrCreateActionSet(instance, &set_info, &set) == XR_SUCCESS);
    XrPath hands[2]{}, touch{};
    REQUIRE(xg::xrStringToPath(instance, "/user/hand/left", &hands[0]) == XR_SUCCESS);
    REQUIRE(xg::xrStringToPath(instance, "/user/hand/right", &hands[1]) == XR_SUCCESS);
    REQUIRE(xg::xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &touch) == XR_SUCCESS);
    auto action = [&](const char* name, std::uint32_t count) {
      XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
      ai.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
      ai.countSubactionPaths = count;
      ai.subactionPaths = hands;
      std::strcpy(ai.actionName, name);
      std::strcpy(ai.localizedActionName, name);
      XrAction out{};
      REQUIRE(xg::xrCreateAction(set, &ai, &out) == XR_SUCCESS);
      return out;
    };
    // Enough other actions that the menu's handle value is one a space also gets.
    for (int i = 0; i < 24; ++i) action(("other_" + std::to_string(i)).c_str(), 2);
    const XrAction menu = action("menu", 1), stick = action("thumbstick_click", 2);
    XrPath menu_path{}, left_click{}, right_click{};
    REQUIRE(xg::xrStringToPath(instance, "/user/hand/left/input/menu/click", &menu_path) == XR_SUCCESS);
    REQUIRE(xg::xrStringToPath(instance, "/user/hand/left/input/thumbstick/click", &left_click) == XR_SUCCESS);
    REQUIRE(xg::xrStringToPath(instance, "/user/hand/right/input/thumbstick/click", &right_click) == XR_SUCCESS);
    XrActionSuggestedBinding bindings[]{{menu, menu_path}, {stick, left_click}, {stick, right_click}};
    XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr, touch, 3,
                                                    bindings};
    REQUIRE(xg::xrSuggestInteractionProfileBindings(instance, &suggestion) == XR_SUCCESS);
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &set};
    REQUIRE(xg::xrAttachSessionActionSets(session, &attach) == XR_SUCCESS);
    // The game makes and destroys spaces; one shares the menu action's value where
    // handles are numbered per type.
    bool collided = false;
    for (int i = 0; i < 96 && !collided; ++i) {
      XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, XR_REFERENCE_SPACE_TYPE_LOCAL,
                                            {{0, 0, 0, 1}, {0, 0, 0}}};
      XrSpace space{};
      REQUIRE(xg::xrCreateReferenceSpace(session, &space_info, &space) == XR_SUCCESS);
      collided = reinterpret_cast<std::uintptr_t>(space) == reinterpret_cast<std::uintptr_t>(menu) ||
                 reinterpret_cast<std::uintptr_t>(space) == reinterpret_cast<std::uintptr_t>(set);
      REQUIRE(xg::xrDestroySpace(space) == XR_SUCCESS);
    }
    CHECK(collided == typed);
    auto poll = [&] {
      XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
      while (xg::xrPollEvent(instance, &event) == XR_SUCCESS) event = {XR_TYPE_EVENT_DATA_BUFFER};
    };
    poll();
    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    REQUIRE(xg::xrBeginSession(session, &begin) == XR_SUCCESS);
    poll();
    XrActiveActionSet active{set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
    auto state = [&](XrAction a, const MockXrScript& script, XrPath hand = XR_NULL_PATH) {
      ventureMockXrSetScript(&script);
      REQUIRE(xg::xrSyncActions(session, &sync) == XR_SUCCESS);
      XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, a, hand};
      XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
      REQUIRE(xg::xrGetActionStateBoolean(session, &get, &value) == XR_SUCCESS);
      return value.isActive && value.currentState;
    };
    MockXrScript script{};
    CHECK_FALSE(state(menu, script));
    // The controller's menu button, where it has one.
    if (!p.menu_path.empty()) {
      press(script.left, p.menu_path);
      CHECK(state(menu, script));
      CHECK_FALSE(state(menu, MockXrScript{}));
      script = {};
    }
    // Both stick clicks (the Stick click rows) pressed together.
    press(script.left, p.defaults[0][size_t(Quest::stick_click)]);
    CHECK_FALSE(state(menu, script));
    CHECK(state(stick, script, hands[0]));  // one click stays the game's stick click
    press(script.right, p.defaults[1][size_t(Quest::stick_click)]);
    CHECK(state(menu, script));
    CHECK_FALSE(state(menu, MockXrScript{}));
    CHECK(xg::xrDestroySession(session) == XR_SUCCESS);
    CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
    venture::xr::install_xr_images(nullptr);
    MockXrScript restored{};
    ventureMockXrSetScript(&restored);
    ventureMockXrSetProfile(nullptr);
  }
}

namespace {
// An OpenXR game on the mock runtime reporting `profile` for both hands, with
// one action set whose boolean actions the test binds.
struct OpenXrGame {
  venture::test::XrImageFixture interop;
  XrInstance instance{};
  XrSession session{};
  XrActionSet set{};
  XrPath hands[2]{};
  explicit OpenXrGame(const char* profile) {
    ventureMockXrSetProfile(profile);
    venture::xr::install_xr_images(&interop);
    const char* extensions[]{"XR_KHR_opengl_es_enable", "XR_FB_touch_controller_pro", "XR_META_touch_controller_plus"};
    XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
    create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    create.enabledExtensionCount = 3;
    create.enabledExtensionNames = extensions;
    REQUIRE(xg::xrCreateInstance(&create, &instance) == XR_SUCCESS);
    xg::GlesBinding graphics{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR, nullptr, 1, 2, 3};
    XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO, &graphics, 0, 1};
    REQUIRE(xg::xrCreateSession(instance, &ci, &session) == XR_SUCCESS);
    XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(set_info.actionSetName, "profiles_test");
    std::strcpy(set_info.localizedActionSetName, "Profiles test");
    REQUIRE(xg::xrCreateActionSet(instance, &set_info, &set) == XR_SUCCESS);
    hands[0] = path("/user/hand/left");
    hands[1] = path("/user/hand/right");
  }
  ~OpenXrGame() {
    CHECK(xg::xrDestroySession(session) == XR_SUCCESS);
    CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
    venture::xr::install_xr_images(nullptr);
    MockXrScript restored{};
    ventureMockXrSetScript(&restored);
    ventureMockXrSetProfile(nullptr);
  }
  XrPath path(const char* text) {
    XrPath out{};
    REQUIRE(xg::xrStringToPath(instance, text, &out) == XR_SUCCESS);
    return out;
  }
  XrAction action(const char* name) {
    XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
    ai.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    ai.countSubactionPaths = 2;
    ai.subactionPaths = hands;
    std::strcpy(ai.actionName, name);
    std::strcpy(ai.localizedActionName, name);
    XrAction out{};
    REQUIRE(xg::xrCreateAction(set, &ai, &out) == XR_SUCCESS);
    return out;
  }
  XrResult suggest(const char* profile, std::vector<XrActionSuggestedBinding> bindings) {
    XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr, path(profile),
                                                    std::uint32_t(bindings.size()), bindings.data()};
    return xg::xrSuggestInteractionProfileBindings(instance, &suggestion);
  }
  void start() {
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &set};
    REQUIRE(xg::xrAttachSessionActionSets(session, &attach) == XR_SUCCESS);
    poll();
    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    REQUIRE(xg::xrBeginSession(session, &begin) == XR_SUCCESS);
    poll();
  }
  void poll() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xg::xrPollEvent(instance, &event) == XR_SUCCESS) event = {XR_TYPE_EVENT_DATA_BUFFER};
  }
  XrPath current(int hand) {
    XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
    REQUIRE(xg::xrGetCurrentInteractionProfile(session, hands[hand], &state) == XR_SUCCESS);
    return state.interactionProfile;
  }
  // Whether `a` is bound and pressed after the host reports `script`.
  bool pressed(XrAction a, const MockXrScript& script) {
    ventureMockXrSetScript(&script);
    XrActiveActionSet active{set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
    REQUIRE(xg::xrSyncActions(session, &sync) == XR_SUCCESS);
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, a, XR_NULL_PATH};
    XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
    REQUIRE(xg::xrGetActionStateBoolean(session, &get, &value) == XR_SUCCESS);
    return value.isActive && value.currentState;
  }
};
}  // namespace

TEST_CASE("A game's Touch, Touch Plus and Touch Pro suggestions all stay bound, reported as a Quest 3's controller") {
  // Unity's OpenXR input binds only Touch; OVRPlugin then suggests Touch Plus and
  // Touch Pro (H3VR2). The last suggestion replaced the Touch one on every host
  // profile, and the game was told Touch Pro, which OVRPlugin passes to Unity
  // unchanged (it answers Touch only for Touch Plus) and for which Unity has no
  // device: no controllers.
  venture::test::ScopedMenuMode automatic("");
  ScopedControls defaults("");
  for (const char* host : {"/interaction_profiles/facebook/touch_controller_pro",
                           "/interaction_profiles/meta/touch_controller_plus",
                           "/interaction_profiles/oculus/touch_controller",
                           "/interaction_profiles/valve/index_controller"}) {
    CAPTURE(host);
    OpenXrGame game(host);
    const XrAction unity = game.action("unity_a"), plugin = game.action("plugin_b");
    const XrPath a = game.path("/user/hand/right/input/a/click"), b = game.path("/user/hand/right/input/b/click");
    REQUIRE(game.suggest("/interaction_profiles/oculus/touch_controller", {{unity, a}, {plugin, b}}) == XR_SUCCESS);
    REQUIRE(game.suggest("/interaction_profiles/meta/touch_controller_plus", {{plugin, b}}) == XR_SUCCESS);
    REQUIRE(game.suggest("/interaction_profiles/facebook/touch_controller_pro", {{plugin, b}}) == XR_SUCCESS);
    game.start();
    // A Quest 3 answers Touch Plus where the game binds it, never Touch Pro.
    const XrPath plus = game.path("/interaction_profiles/meta/touch_controller_plus");
    CHECK(game.current(0) == plus);
    CHECK(game.current(1) == plus);
    MockXrScript script{};
    script.right.primary = true;
    CHECK(game.pressed(unity, script));
    CHECK_FALSE(game.pressed(plugin, script));
    script = {};
    script.right.secondary = true;
    CHECK(game.pressed(plugin, script));
    CHECK_FALSE(game.pressed(unity, script));
    XrBoundSourcesForActionEnumerateInfo bound{XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO, nullptr, unity};
    XrPath source{};
    std::uint32_t count = 0;
    REQUIRE(xg::xrEnumerateBoundSourcesForAction(game.session, &bound, 1, &count, &source) == XR_SUCCESS);
    CHECK(count == 1);
    CHECK(source == a);
  }
}

TEST_CASE("A game that suggests only Touch Pro is told Touch Pro on any controller") {
  venture::test::ScopedMenuMode automatic("");
  ScopedControls defaults("");
  OpenXrGame game("/interaction_profiles/oculus/touch_controller");
  const XrAction pro_a = game.action("pro_a");
  const XrPath pro = game.path("/interaction_profiles/facebook/touch_controller_pro");
  REQUIRE(game.suggest("/interaction_profiles/facebook/touch_controller_pro",
                       {{pro_a, game.path("/user/hand/right/input/a/click")}}) == XR_SUCCESS);
  game.start();
  CHECK(game.current(1) == pro);
  MockXrScript script{};
  script.right.primary = true;
  CHECK(game.pressed(pro_a, script));
}

TEST_CASE("A game's own suggestion for the host's controller keeps its bindings and profile after its Touch one") {
  venture::test::ScopedMenuMode automatic("");
  ScopedControls defaults("");
  const char* index = "/interaction_profiles/valve/index_controller";
  OpenXrGame game(index);
  const XrAction own = game.action("index_b"), touch_only = game.action("touch_x");
  // The game's Index binding: its action on B. Its later Touch suggestion puts the
  // same action on A and adds one the Index suggestion leaves out.
  REQUIRE(game.suggest(index, {{own, game.path("/user/hand/right/input/b/click")}}) == XR_SUCCESS);
  REQUIRE(game.suggest("/interaction_profiles/oculus/touch_controller",
                       {{own, game.path("/user/hand/right/input/a/click")},
                        {touch_only, game.path("/user/hand/left/input/x/click")}}) == XR_SUCCESS);
  game.start();
  CHECK(game.current(1) == game.path(index));
  MockXrScript script{};
  script.right.secondary = true;
  CHECK(game.pressed(own, script));
  script = {};
  script.right.primary = true;
  CHECK_FALSE(game.pressed(own, script));
  script = {};
  script.left.primary = true;
  CHECK(game.pressed(touch_only, script));
}

// Run by hand against a real runtime (not in ctest):
//   XR_RUNTIME_JSON=<runtime manifest> VENTURE_REAL_XR_PROBE=1 venture_xr_tests -tc="Probe*"
// The runtime log shows each profile's suggestion and the host's result.
TEST_CASE("Probe: the host runtime accepts the bindings of every controller profile it defines" *
          doctest::skip(std::getenv("VENTURE_REAL_XR_PROBE") == nullptr)) {
  if (const char* file = std::getenv("VENTURE_PROBE_LOG")) {
    venture::log::set_level(venture::log::Level::info);
    venture::log::set_file(file);
  }
  auto& host = venture::xr::host_xr();
  REQUIRE(host.instance);
  MESSAGE("runtime ", host.runtime_name, " system result ", host.system_result, " dpad ",
          std::string(venture::xr::host_dpad() ? "yes" : "no"));
  // VrApi's Touch actions (hle/vrapi/input.cpp) on a set of their own: no session needed.
  const XrInstance instance = host.instance;
  XrActionSetCreateInfo sci{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strcpy(sci.actionSetName, "probe");
  std::strcpy(sci.localizedActionSetName, "Probe");
  XrActionSet set{};
  REQUIRE(::xrCreateActionSet(instance, &sci, &set) == XR_SUCCESS);
  XrPath hands[2]{};
  ::xrStringToPath(instance, "/user/hand/left", &hands[0]);
  ::xrStringToPath(instance, "/user/hand/right", &hands[1]);
  struct Spec {
    const char* name;
    XrActionType type;
    const char* left;
    const char* right;
    XrAction action = XR_NULL_HANDLE;
  };
  Spec specs[]{
      {"grip", XR_ACTION_TYPE_POSE_INPUT, "/input/grip/pose", "/input/grip/pose"},
      {"aim", XR_ACTION_TYPE_POSE_INPUT, "/input/aim/pose", "/input/aim/pose"},
      {"trigger", XR_ACTION_TYPE_FLOAT_INPUT, "/input/trigger/value", "/input/trigger/value"},
      {"squeeze", XR_ACTION_TYPE_FLOAT_INPUT, "/input/squeeze/value", "/input/squeeze/value"},
      {"stick", XR_ACTION_TYPE_VECTOR2F_INPUT, "/input/thumbstick", "/input/thumbstick"},
      {"a", XR_ACTION_TYPE_BOOLEAN_INPUT, "/input/x/click", "/input/a/click"},
      {"b", XR_ACTION_TYPE_BOOLEAN_INPUT, "/input/y/click", "/input/b/click"},
      {"click", XR_ACTION_TYPE_BOOLEAN_INPUT, "/input/thumbstick/click", "/input/thumbstick/click"},
      {"menu", XR_ACTION_TYPE_BOOLEAN_INPUT, "/input/menu/click", nullptr},
      {"haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, "/output/haptic", "/output/haptic"},
      {"touch_trigger", XR_ACTION_TYPE_BOOLEAN_INPUT, "/input/trigger/touch", "/input/trigger/touch"},
      {"touch_stick", XR_ACTION_TYPE_BOOLEAN_INPUT, "/input/thumbstick/touch", "/input/thumbstick/touch"},
      {"touch_a", XR_ACTION_TYPE_BOOLEAN_INPUT, "/input/x/touch", "/input/a/touch"},
      {"touch_b", XR_ACTION_TYPE_BOOLEAN_INPUT, "/input/y/touch", "/input/b/touch"},
      {"touch_rest", XR_ACTION_TYPE_BOOLEAN_INPUT, "/input/thumbrest/touch", "/input/thumbrest/touch"},
  };
  for (auto& spec : specs) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    std::strcpy(ci.actionName, spec.name);
    std::strcpy(ci.localizedActionName, spec.name);
    ci.actionType = spec.type;
    ci.countSubactionPaths = 2;
    ci.subactionPaths = hands;
    REQUIRE(::xrCreateAction(set, &ci, &spec.action) == XR_SUCCESS);
    if (std::string_view(spec.name) == "menu") venture::xr::register_menu_action(set, spec.action);
  }
  for (const auto& profile : venture::xr::touch_targets()) {
    std::vector<XrActionSuggestedBinding> bindings;
    for (auto& spec : specs)
      for (int hand = 0; hand < 2; ++hand) {
        const char* suffix = hand ? spec.right : spec.left;
        if (!suffix) continue;
        auto mapped = venture::xr::touch_binding(std::string(hand ? "/user/hand/right" : "/user/hand/left") + suffix,
                                                profile);
        XrPath path{};
        if (mapped && XR_SUCCEEDED(::xrStringToPath(instance, mapped->c_str(), &path)))
          bindings.push_back({spec.action, path});
      }
    XrPath profile_path{};
    REQUIRE(::xrStringToPath(instance, profile.c_str(), &profile_path) == XR_SUCCESS);
    const auto result = venture::xr::suggest_translated(instance, profile_path, std::move(bindings));
    MESSAGE(profile, ": ", int(result));
    CHECK(result == XR_SUCCESS);
  }
  ::xrDestroyActionSet(set);
}
