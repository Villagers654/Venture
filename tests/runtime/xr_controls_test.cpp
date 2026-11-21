// The player's controls end to end over the mock runtime: each controller's
// inputs pressed on the host, and what the game sees.
#include "common/controls.h"
#include "common/environment.h"
#include "common/log.h"
#include "core/wait.h"
#include "../mock_openxr_runtime/control.h"
#include "hle/openxr/openxr.h"
#include "hle/openxr/state.h"
#include "hle/vrapi/state.h"
#include "hle/vrapi/vrapi.h"
#include "xr/bindings.h"
#include "xr/host_xr.h"
#include "xr/haptic_sequence.h"
#include "xr/grip_toggle.h"
#include "xr/menu_chord.h"
#include "xr_images_fixture.h"
#include "xr_menu_binding_fixture.h"
#include <doctest/doctest.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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
  touch.rows[0][int(Quest::primary)] = Inputs{"/input/y/click"};
  touch.rows[0][int(Quest::secondary)] = Inputs{"/input/x/click"};
  touch.rows[1][int(Quest::stick_click)] = Inputs{};
  touch.rows[1][int(Quest::grip)] = Inputs{"/input/trigger/value"};
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

TEST_CASE("Without XR_EXT_dpad_binding Venture splits a Vive wand's trackpad into its halves") {
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
  // The mock's trackpad is its thumbstick vector and its press the stick click.
  // Bottom is X / A, top Y / B, the center Stick click (#159: WMR's own runtime).
  struct Press {
    XrVector2f at;
    Quest expected;
  };
  for (const auto& [at, expected] : {Press{{0.f, -0.9f}, Quest::primary}, Press{{0.1f, 0.8f}, Quest::secondary},
                                     Press{{0.f, 0.1f}, Quest::stick_click}})
    for (int hand = 0; hand < 2; ++hand) {
      CAPTURE(hand);
      CAPTURE(int(expected));
      MockXrScript script{};
      auto& c = hand ? script.right : script.left;
      c.thumbstick = at;
      c.thumbstick_click = true;
      const auto state = game.read(script, hand);
      for (const auto q : {Quest::primary, Quest::secondary, Quest::stick_click})
        CHECK(sees(state, q, hand) == (q == expected));
    }
  // Touching the pad without pressing it presses nothing.
  MockXrScript touched{};
  touched.left.thumbstick = {0.f, -0.9f};
  const auto state = game.read(touched, 0);
  CHECK_FALSE(sees(state, Quest::primary, 0));
}

TEST_CASE("An OpenXR game's Touch bindings follow the player's controls on another controller") {
  venture::test::ScopedMenuMode automatic("");
  Config config;
  config["/interaction_profiles/htc/vive_controller"].rows[0][int(Quest::primary)] = Inputs{"/input/trackpad/dpad_up"};
  config["/interaction_profiles/htc/vive_controller"].rows[0][int(Quest::secondary)] = Inputs{"/input/trackpad/dpad_down"};
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
  // The game names its own source as a Quest would, whatever the host controller is.
  XrInputSourceLocalizedNameGetInfo name_info{XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO, nullptr, source,
      XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT | XR_INPUT_SOURCE_LOCALIZED_NAME_INTERACTION_PROFILE_BIT |
      XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT};
  std::uint32_t name_size = 0;
  REQUIRE(xg::xrGetInputSourceLocalizedName(session, &name_info, 0, &name_size, nullptr) == XR_SUCCESS);
  std::string name(name_size, '\0');
  CHECK(xg::xrGetInputSourceLocalizedName(session, &name_info, 1, &name_size, name.data()) ==
        XR_ERROR_SIZE_INSUFFICIENT);
  REQUIRE(xg::xrGetInputSourceLocalizedName(session, &name_info, name_size, &name_size, name.data()) == XR_SUCCESS);
  CHECK(std::string(name.c_str()) == "Left Hand Oculus Touch Controller X Button");
  name_info.whichComponents = XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT;
  REQUIRE(xg::xrGetInputSourceLocalizedName(session, &name_info, name_size, &name_size, name.data()) == XR_SUCCESS);
  CHECK(std::string(name.c_str()) == "X Button");
  name_info.whichComponents = 0;
  CHECK(xg::xrGetInputSourceLocalizedName(session, &name_info, 0, &name_size, nullptr) == XR_ERROR_VALIDATION_FAILURE);
  name_info.whichComponents = XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT;
  name_info.sourcePath = left;
  CHECK(xg::xrGetInputSourceLocalizedName(session, &name_info, 0, &name_size, nullptr) == XR_ERROR_PATH_UNSUPPORTED);
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

TEST_CASE("An OpenXR game's action reaches every host input the player gave its Quest input") {
  // A Steam Frame player puts Stick click on the stick and on the bumper: the game's
  // one stick-click action is suggested on both host paths, and either drives it.
  venture::test::ScopedMenuMode automatic("");
  const char* frame = "/interaction_profiles/valve/frame_controller_valve";
  Config config;
  config[frame].rows[0][int(Quest::stick_click)] = Inputs{"/input/thumbstick/click", "/input/bumper/click"};
  ScopedControls controls(encode(config));
  ventureMockXrSetProfile(frame);
  CHECK(venture::xr::touch_bindings("/user/hand/left/input/thumbstick/click", frame) ==
        std::vector<std::string>{"/user/hand/left/input/thumbstick/click", "/user/hand/left/input/bumper/click"});
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
  std::strcpy(set_info.actionSetName, "multi_test");
  std::strcpy(set_info.localizedActionSetName, "Multi test");
  XrActionSet set{};
  REQUIRE(xg::xrCreateActionSet(instance, &set_info, &set) == XR_SUCCESS);
  XrPath left{}, touch{}, click_path{};
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/left", &left) == XR_SUCCESS);
  REQUIRE(xg::xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &touch) == XR_SUCCESS);
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/left/input/thumbstick/click", &click_path) == XR_SUCCESS);
  XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
  ai.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
  ai.countSubactionPaths = 1;
  ai.subactionPaths = &left;
  std::strcpy(ai.actionName, "click");
  std::strcpy(ai.localizedActionName, "click");
  XrAction click{};
  REQUIRE(xg::xrCreateAction(set, &ai, &click) == XR_SUCCESS);
  XrActionSuggestedBinding binding{click, click_path};
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
  // The game sees one bound source, its own path.
  XrBoundSourcesForActionEnumerateInfo bound{XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO, nullptr, click};
  std::array<XrPath, 4> sources{};
  std::uint32_t count = 0;
  REQUIRE(xg::xrEnumerateBoundSourcesForAction(session, &bound, std::uint32_t(sources.size()), &count, sources.data()) ==
          XR_SUCCESS);
  REQUIRE(count >= 1);
  for (std::uint32_t i = 0; i < count; ++i) CHECK(sources[i] == click_path);
  XrActiveActionSet active{set, XR_NULL_PATH};
  XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
  auto pressed = [&](const MockXrScript& script) {
    ventureMockXrSetScript(&script);
    REQUIRE(xg::xrSyncActions(session, &sync) == XR_SUCCESS);
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, click, XR_NULL_PATH};
    XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
    REQUIRE(xg::xrGetActionStateBoolean(session, &get, &value) == XR_SUCCESS);
    return value.isActive && value.currentState;
  };
  CHECK_FALSE(pressed(MockXrScript{}));
  MockXrScript script{};
  press(script.left, "/input/thumbstick/click");
  CHECK(pressed(script));
  script = {};
  press(script.left, "/input/bumper/click");
  CHECK(pressed(script));
  CHECK_FALSE(pressed(MockXrScript{}));
  // The right bumper is not the left Stick click.
  script = {};
  press(script.right, "/input/bumper/click");
  CHECK_FALSE(pressed(script));
  CHECK(xg::xrDestroySession(session) == XR_SUCCESS);
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  venture::xr::install_xr_images(nullptr);
  MockXrScript restored{};
  ventureMockXrSetScript(&restored);
  ventureMockXrSetProfile(nullptr);
}

TEST_CASE("Several host inputs on one Quest input reach a VrApi game, and both stick-click sets open the automatic menu") {
  venture::test::ScopedMenuMode automatic("");
  const char* frame = "/interaction_profiles/valve/frame_controller_valve";
  Config config;
  for (int hand = 0; hand < 2; ++hand)
    config[frame].rows[size_t(hand)][int(Quest::stick_click)] = Inputs{"/input/thumbstick/click", "/input/bumper/click"};
  ScopedControls controls(encode(config));
  VrApiGame game(frame);
  for (int hand = 0; hand < 2; ++hand)
    for (auto input : {"/input/thumbstick/click", "/input/bumper/click"}) {
      CAPTURE(hand);
      CAPTURE(input);
      MockXrScript script{};
      press(hand ? script.right : script.left, input);
      const auto state = game.read(script, hand);
      CHECK(sees(state, Quest::stick_click, hand));
      CHECK_FALSE(sees(game.read(script, 0), Quest::menu, 0));
      CHECK_FALSE(sees(game.read(MockXrScript{}, hand), Quest::stick_click, hand));
    }
  // The automatic menu's both stick clicks: any of each hand's Stick click inputs.
  MockXrScript script{};
  press(script.left, "/input/bumper/click");
  press(script.right, "/input/thumbstick/click");
  CHECK(sees(game.read(script, 0), Quest::menu, 0));
  CHECK_FALSE(sees(game.read(MockXrScript{}, 0), Quest::menu, 0));
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
    const XrAction trigger = action("trigger_click", 2), grip = action("grip_click", 2);
    XrPath menu_path{}, left_click{}, right_click{}, left_trigger{}, right_trigger{}, left_grip{};
    REQUIRE(xg::xrStringToPath(instance, "/user/hand/left/input/menu/click", &menu_path) == XR_SUCCESS);
    REQUIRE(xg::xrStringToPath(instance, "/user/hand/left/input/thumbstick/click", &left_click) == XR_SUCCESS);
    REQUIRE(xg::xrStringToPath(instance, "/user/hand/right/input/thumbstick/click", &right_click) == XR_SUCCESS);
    // Unreal omits the component suffix for these boolean Touch actions.
    REQUIRE(xg::xrStringToPath(instance, "/user/hand/left/input/trigger", &left_trigger) == XR_SUCCESS);
    REQUIRE(xg::xrStringToPath(instance, "/user/hand/right/input/trigger", &right_trigger) == XR_SUCCESS);
    REQUIRE(xg::xrStringToPath(instance, "/user/hand/left/input/squeeze", &left_grip) == XR_SUCCESS);
    XrActionSuggestedBinding bindings[]{{menu, menu_path}, {stick, left_click}, {stick, right_click},
                                        {trigger, left_trigger}, {trigger, right_trigger}, {grip, left_grip}};
    XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr, touch,
                                                    std::size(bindings), bindings};
    REQUIRE(xg::xrSuggestInteractionProfileBindings(instance, &suggestion) == XR_SUCCESS);
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &set};
    REQUIRE(xg::xrAttachSessionActionSets(session, &attach) == XR_SUCCESS);
    // The game makes and destroys spaces. Where the host numbers handles per type a
    // space may share the value of a host action or set; the game's own actions and
    // sets are Venture's handles, which no host space shares.
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
    CHECK_FALSE(collided);
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
    script = {};
    press(script.right, p.defaults[1][size_t(Quest::trigger)]);
    CHECK(state(trigger, script, hands[1]));
    CHECK_FALSE(state(trigger, script, hands[0]));
    script = {};
    press(script.left, p.defaults[0][size_t(Quest::trigger)]);
    press(script.left, p.defaults[0][size_t(Quest::grip)]);
    CHECK(state(trigger, script, hands[0]));
    CHECK_FALSE(state(trigger, script, hands[1]));
    CHECK(state(grip, script, hands[0]));
    CHECK_FALSE(state(trigger, MockXrScript{}, hands[0]));
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

TEST_CASE("Without XR_EXT_dpad_binding an OpenXR game gets A/B and X/Y from a WMR controller's trackpad halves") {
  // #159: H3VR2 (OpenXR) on Windows Mixed Reality's own runtime, by default
  // controls: pad bottom pressed is X / A, top Y / B, on both hands.
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
  OpenXrGame game("/interaction_profiles/microsoft/motion_controller");
  const XrAction x = game.action("x"), y = game.action("y"), a = game.action("a"), b = game.action("b");
  REQUIRE(game.suggest("/interaction_profiles/oculus/touch_controller",
                       {{x, game.path("/user/hand/left/input/x/click")},
                        {y, game.path("/user/hand/left/input/y/click")},
                        {a, game.path("/user/hand/right/input/a/click")},
                        {b, game.path("/user/hand/right/input/b/click")}}) == XR_SUCCESS);
  game.start();
  // The mock's trackpad is its thumbstick vector and its press the stick click.
  auto at = [](int hand, XrVector2f where, bool click) {
    MockXrScript script{};
    auto& c = hand ? script.right : script.left;
    c.thumbstick = where;
    c.thumbstick_click = click;
    return script;
  };
  for (int hand = 0; hand < 2; ++hand) {
    CAPTURE(hand);
    const XrAction lower = hand ? a : x, upper = hand ? b : y;
    CHECK_FALSE(game.pressed(lower, MockXrScript{}));
    CHECK_FALSE(game.pressed(upper, MockXrScript{}));
    CHECK(game.pressed(lower, at(hand, {0.1f, -0.85f}, true)));
    CHECK_FALSE(game.pressed(upper, at(hand, {0.1f, -0.85f}, true)));
    CHECK(game.pressed(upper, at(hand, {-0.1f, 0.85f}, true)));
    CHECK_FALSE(game.pressed(lower, at(hand, {-0.1f, 0.85f}, true)));
    // Resting a thumb on the pad presses nothing; the center is Stick click's.
    CHECK_FALSE(game.pressed(upper, at(hand, {0.f, 0.85f}, false)));
    CHECK_FALSE(game.pressed(lower, at(hand, {0.f, 0.05f}, true)));
    CHECK_FALSE(game.pressed(upper, at(hand, {0.f, 0.05f}, true)));
  }
  // The other hand's pad never presses this hand's buttons.
  CHECK_FALSE(game.pressed(x, at(1, {0.f, -0.85f}, true)));
  CHECK_FALSE(game.pressed(b, at(0, {0.f, 0.85f}, true)));
}

TEST_CASE("Touch Pro and Touch Plus rows give an OpenXR game those sensors from another controller's inputs") {
  venture::test::ScopedMenuMode automatic("");
  constexpr const char* kIndex = "/interaction_profiles/valve/index_controller";
  Config config;
  config[kIndex].rows[1][int(Quest::thumb_proximity)] = Inputs{"/input/thumbstick/touch"};
  config[kIndex].rows[1][int(Quest::trigger_curl)] = Inputs{"/input/trigger/value"};
  config[kIndex].rows[1][int(Quest::trigger_proximity)] = Inputs{"/input/b/click"};
  config[kIndex].rows[1][int(Quest::stylus_force)] = Inputs{"/input/a/click"};
  ScopedControls controls(encode(config));
  OpenXrGame game(kIndex);
  auto float_action = [&](const char* name) {
    XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
    ai.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    ai.countSubactionPaths = 2;
    ai.subactionPaths = game.hands;
    std::strcpy(ai.actionName, name);
    std::strcpy(ai.localizedActionName, name);
    XrAction out{};
    REQUIRE(xg::xrCreateAction(game.set, &ai, &out) == XR_SUCCESS);
    return out;
  };
  const XrAction near_thumb = game.action("near_thumb"), near_trigger = game.action("near_trigger");
  const XrAction curl = float_action("curl"), stylus = float_action("stylus");
  REQUIRE(game.suggest("/interaction_profiles/meta/touch_controller_plus",
                       {{near_thumb, game.path("/user/hand/right/input/thumb_meta/proximity_meta")},
                        {curl, game.path("/user/hand/right/input/trigger/curl_meta")}}) == XR_SUCCESS);
  REQUIRE(game.suggest("/interaction_profiles/facebook/touch_controller_pro",
                       {{near_trigger, game.path("/user/hand/right/input/trigger/proximity_fb")},
                        {stylus, game.path("/user/hand/right/input/stylus_fb/force")}}) == XR_SUCCESS);
  game.start();
  auto value = [&](XrAction a) {
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, a, XR_NULL_PATH};
    XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
    REQUIRE(xg::xrGetActionStateFloat(game.session, &get, &state) == XR_SUCCESS);
    CHECK(state.isActive);
    return state.currentState;
  };
  MockXrScript script{};
  script.trigger = 0;
  CHECK_FALSE(game.pressed(near_thumb, script));
  CHECK_FALSE(game.pressed(near_trigger, script));
  CHECK(value(curl) == doctest::Approx(0));
  CHECK(value(stylus) == doctest::Approx(0));
  press(script.right, "/input/thumbstick/touch");
  CHECK(game.pressed(near_thumb, script));
  CHECK_FALSE(game.pressed(near_trigger, script));
  script = {};
  script.right.trigger = 0.6f;
  press(script.right, "/input/b/click");
  press(script.right, "/input/a/click");
  CHECK(game.pressed(near_trigger, script));
  CHECK(value(curl) == doctest::Approx(0.6f));  // the analog trigger's own value
  CHECK(value(stylus) == doctest::Approx(1));   // a button, all the way
}

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

TEST_CASE("A game whose Touch Pro suggestion binds all its Touch actions is told Touch Pro on a Pro controller") {
  // H3VR2 since 20261115C binds every action on Touch Pro as well as Touch Plus and Touch.
  venture::test::ScopedMenuMode automatic("");
  ScopedControls defaults("");
  for (const char* host : {"/interaction_profiles/facebook/touch_controller_pro",
                           "/interaction_profiles/meta/touch_controller_plus"}) {
    CAPTURE(host);
    OpenXrGame game(host);
    const XrAction a_action = game.action("a"), b_action = game.action("b");
    const XrPath a = game.path("/user/hand/right/input/a/click"), b = game.path("/user/hand/right/input/b/click");
    REQUIRE(game.suggest("/interaction_profiles/meta/touch_controller_plus", {{a_action, a}, {b_action, b}}) == XR_SUCCESS);
    REQUIRE(game.suggest("/interaction_profiles/facebook/touch_controller_pro", {{a_action, a}, {b_action, b}}) == XR_SUCCESS);
    REQUIRE(game.suggest("/interaction_profiles/oculus/touch_controller", {{a_action, a}}) == XR_SUCCESS);
    game.start();
    // A Pro controller is reported as one; a Quest 3's stays Touch Plus.
    CHECK(game.current(1) == game.path(host));
    MockXrScript script{};
    script.right.primary = true;
    CHECK(game.pressed(a_action, script));
    script = {};
    script.right.secondary = true;
    CHECK(game.pressed(b_action, script));
  }
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
        for (const auto& mapped :
             venture::xr::touch_bindings(std::string(hand ? "/user/hand/right" : "/user/hand/left") + suffix, profile)) {
          XrPath path{};
          if (XR_SUCCEEDED(::xrStringToPath(instance, mapped.c_str(), &path))) bindings.push_back({spec.action, path});
        }
      }
    XrPath profile_path{};
    REQUIRE(::xrStringToPath(instance, profile.c_str(), &profile_path) == XR_SUCCESS);
    const auto result = venture::xr::suggest_translated(instance, profile_path, std::move(bindings));
    MESSAGE(profile, ": ", int(result));
    CHECK(result == XR_SUCCESS);
  }
  ::xrDestroyActionSet(set);
}

TEST_CASE("A grip latch flips once per press, whatever the game reads per sync") {
  venture::xr::GripLatch latch;
  CHECK_FALSE(latch.update(1, 0.f, true, 10));
  CHECK(latch.update(2, 1.f, true, 20));
  CHECK(latch.changed);
  CHECK(latch.time == 20);
  // Read again in the same sync: nothing changes, still the flip of this sync.
  CHECK(latch.update(2, 0.f, true, 25));
  CHECK(latch.changed);
  CHECK(latch.time == 20);
  CHECK(latch.update(3, 1.f, true, 30));  // still pressed
  CHECK_FALSE(latch.changed);
  CHECK(latch.update(4, 0.45f, true, 40));  // between release and press: still pressed
  CHECK(latch.update(5, 0.45f, true, 50));
  CHECK(latch.update(6, 0.f, true, 60));  // released: still held
  CHECK_FALSE(latch.changed);
  CHECK_FALSE(latch.update(7, 0.6f, true, 70));  // the next press lets go
  CHECK(latch.changed);
  CHECK(latch.time == 70);
  CHECK_FALSE(latch.update(8, 0.f, true, 80));
  CHECK(latch.update(9, 1.f, true, 90));
  // No controller: let go, and the press still down when it returns counts as a new one.
  CHECK_FALSE(latch.update(10, 1.f, false, 100));
  CHECK(latch.changed);
  CHECK(latch.update(11, 1.f, true, 110));
  CHECK(latch.update(12, 0.f, true, 120));
  CHECK_FALSE(latch.update(13, 1.f, true, 130));
}

namespace {
// An OpenXR game with a float grip action per hand and a boolean one without
// subaction paths, both bound to Touch's squeeze, on the host controller `profile`.
struct GripGame {
  venture::test::XrImageFixture interop;
  XrInstance instance{};
  XrSession session{};
  XrActionSet set{};
  XrAction grab{}, any{};
  std::array<XrPath, 2> hands{};
  explicit GripGame(const char* profile) {
    ventureMockXrSetProfile(profile);
    venture::xr::install_xr_images(&interop);
    const char* extensions[]{"XR_KHR_opengl_es_enable"};
    XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
    create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    create.enabledExtensionCount = 1;
    create.enabledExtensionNames = extensions;
    REQUIRE(xg::xrCreateInstance(&create, &instance) == XR_SUCCESS);
    xg::GlesBinding graphics{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR, nullptr, 1, 2, 3};
    XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO, &graphics, 0, 1};
    REQUIRE(xg::xrCreateSession(instance, &ci, &session) == XR_SUCCESS);
    XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(set_info.actionSetName, "grip_test");
    std::strcpy(set_info.localizedActionSetName, "Grip test");
    REQUIRE(xg::xrCreateActionSet(instance, &set_info, &set) == XR_SUCCESS);
    REQUIRE(xg::xrStringToPath(instance, "/user/hand/left", &hands[0]) == XR_SUCCESS);
    REQUIRE(xg::xrStringToPath(instance, "/user/hand/right", &hands[1]) == XR_SUCCESS);
    XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
    ai.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    ai.countSubactionPaths = 2;
    ai.subactionPaths = hands.data();
    std::strcpy(ai.actionName, "grab");
    std::strcpy(ai.localizedActionName, "grab");
    REQUIRE(xg::xrCreateAction(set, &ai, &grab) == XR_SUCCESS);
    ai.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    ai.countSubactionPaths = 0;
    ai.subactionPaths = nullptr;
    std::strcpy(ai.actionName, "any");
    std::strcpy(ai.localizedActionName, "any");
    REQUIRE(xg::xrCreateAction(set, &ai, &any) == XR_SUCCESS);
    XrPath touch{}, left{}, right{};
    REQUIRE(xg::xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &touch) == XR_SUCCESS);
    REQUIRE(xg::xrStringToPath(instance, "/user/hand/left/input/squeeze/value", &left) == XR_SUCCESS);
    REQUIRE(xg::xrStringToPath(instance, "/user/hand/right/input/squeeze/value", &right) == XR_SUCCESS);
    XrActionSuggestedBinding bindings[]{{grab, left}, {grab, right}, {any, left}, {any, right}};
    XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr, touch, 4,
                                                    bindings};
    REQUIRE(xg::xrSuggestInteractionProfileBindings(instance, &suggestion) == XR_SUCCESS);
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &set};
    REQUIRE(xg::xrAttachSessionActionSets(session, &attach) == XR_SUCCESS);
    poll();
    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    REQUIRE(xg::xrBeginSession(session, &begin) == XR_SUCCESS);
    poll();
  }
  ~GripGame() {
    CHECK(xg::xrDestroySession(session) == XR_SUCCESS);
    CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
    venture::xr::install_xr_images(nullptr);
    MockXrScript restored{};
    ventureMockXrSetScript(&restored);
    ventureMockXrSetProfile(nullptr);
  }
  void poll() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xg::xrPollEvent(instance, &event) == XR_SUCCESS) event = {XR_TYPE_EVENT_DATA_BUFFER};
  }
  // Both grips as the host reports them, synced once.
  void sync(float left, float right) {
    MockXrScript script{};
    script.left.squeeze = left;
    script.right.squeeze = right;
    ventureMockXrSetScript(&script);
    XrActiveActionSet active{set, XR_NULL_PATH};
    XrActionsSyncInfo info{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
    REQUIRE(xg::xrSyncActions(session, &info) == XR_SUCCESS);
  }
  XrActionStateFloat hand(int h) {
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, grab, hands[size_t(h)]};
    XrActionStateFloat value{XR_TYPE_ACTION_STATE_FLOAT};
    REQUIRE(xg::xrGetActionStateFloat(session, &get, &value) == XR_SUCCESS);
    CHECK(value.isActive);
    return value;
  }
  XrActionStateBoolean either() {
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, any, XR_NULL_PATH};
    XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
    REQUIRE(xg::xrGetActionStateBoolean(session, &get, &value) == XR_SUCCESS);
    CHECK(value.isActive);
    return value;
  }
};
std::string grip_toggle_on(const char* profile) {
  Config config;
  config[profile].grip_toggle = true;
  return encode(config);
}
}  // namespace

TEST_CASE("Grip toggle latches an OpenXR game's grip per hand on a Vive wand") {
  venture::test::ScopedMenuMode automatic("");
  ScopedControls controls(grip_toggle_on("/interaction_profiles/htc/vive_controller"));
  GripGame game("/interaction_profiles/htc/vive_controller");
  game.sync(0, 0);
  CHECK(game.hand(0).currentState == 0.f);
  CHECK_FALSE(game.either().currentState);
  // A press holds: the game sees the change once, however often it reads.
  game.sync(1, 0);
  auto left = game.hand(0);
  CHECK(left.currentState == 1.f);
  CHECK(left.changedSinceLastSync);
  const XrTime pressed_at = left.lastChangeTime;
  left = game.hand(0);
  CHECK(left.currentState == 1.f);
  CHECK(left.changedSinceLastSync);
  CHECK(game.hand(1).currentState == 0.f);
  CHECK(game.either().currentState);
  game.sync(1, 0);
  left = game.hand(0);
  CHECK(left.currentState == 1.f);
  CHECK_FALSE(left.changedSinceLastSync);
  CHECK(left.lastChangeTime == pressed_at);
  // Let go of the button: still held.
  game.sync(0, 0);
  CHECK(game.hand(0).currentState == 1.f);
  CHECK(game.either().currentState);
  // The other hand latches on its own; the action without a hand holds while either hand does.
  game.sync(0, 1);
  CHECK(game.hand(1).currentState == 1.f);
  CHECK(game.hand(0).currentState == 1.f);
  CHECK(game.either().currentState);
  game.sync(0, 0);
  // The next press lets go.
  game.sync(1, 0);
  left = game.hand(0);
  CHECK(left.currentState == 0.f);
  CHECK(left.changedSinceLastSync);
  CHECK(game.either().currentState);
  game.sync(0, 0);
  CHECK(game.hand(0).currentState == 0.f);
  CHECK(game.hand(1).currentState == 1.f);
}

TEST_CASE("Without grip toggle an OpenXR game's grip follows the button") {
  venture::test::ScopedMenuMode automatic("");
  // Turned off for the Vive wand (on by default for its click grip): its grip follows the button.
  Config config;
  config["/interaction_profiles/htc/vive_controller"].grip_toggle = false;
  ScopedControls controls(encode(config));
  GripGame game("/interaction_profiles/htc/vive_controller");
  game.sync(1, 0);
  CHECK(game.hand(0).currentState == 1.f);
  CHECK(game.either().currentState);
  game.sync(0, 0);
  CHECK(game.hand(0).currentState == 0.f);
  CHECK_FALSE(game.either().currentState);
  game.sync(1, 0);
  CHECK(game.hand(0).currentState == 1.f);
}

TEST_CASE("Grip toggle latches a VrApi game's grip") {
  venture::test::ScopedMenuMode automatic("");
  ScopedControls controls(grip_toggle_on("/interaction_profiles/htc/vive_controller"));
  VrApiGame game("/interaction_profiles/htc/vive_controller");
  MockXrScript pressed{};
  pressed.left.squeeze = 1;
  CHECK(sees(game.read(pressed, 0), Quest::grip, 0));
  CHECK(sees(game.read(MockXrScript{}, 0), Quest::grip, 0));
  CHECK_FALSE(sees(game.read(MockXrScript{}, 1), Quest::grip, 1));
  CHECK_FALSE(sees(game.read(pressed, 0), Quest::grip, 0));
  CHECK_FALSE(sees(game.read(MockXrScript{}, 0), Quest::grip, 0));
}

namespace {
// The host action sets and actions Venture creates, by name.
struct HostNames {
  venture::xr::HostXr& host = venture::xr::host_xr();
  void* original = host.get_instance_proc_addr;
  inline static HostNames* active = nullptr;
  PFN_xrCreateActionSet native_set{};
  PFN_xrCreateAction native_action{};
  std::set<std::string> sets, actions;
  HostNames() {
    const auto resolve = reinterpret_cast<PFN_xrGetInstanceProcAddr>(original);
    PFN_xrVoidFunction function{};
    REQUIRE(resolve(host.instance, "xrCreateActionSet", &function) == XR_SUCCESS);
    native_set = reinterpret_cast<PFN_xrCreateActionSet>(function);
    REQUIRE(resolve(host.instance, "xrCreateAction", &function) == XR_SUCCESS);
    native_action = reinterpret_cast<PFN_xrCreateAction>(function);
    active = this;
    host.get_instance_proc_addr = reinterpret_cast<void*>(lookup);
    venture::xr::invalidate_host_procedures();
  }
  ~HostNames() {
    host.get_instance_proc_addr = original;
    venture::xr::invalidate_host_procedures();
    active = nullptr;
  }
  static XrResult XRAPI_CALL create_set(XrInstance instance, const XrActionSetCreateInfo* info, XrActionSet* out) {
    active->sets.insert(info->actionSetName);
    return active->native_set(instance, info, out);
  }
  static XrResult XRAPI_CALL create_action(XrActionSet set, const XrActionCreateInfo* info, XrAction* out) {
    active->actions.insert(info->actionName);
    return active->native_action(set, info, out);
  }
  static XrResult XRAPI_CALL lookup(XrInstance instance, const char* name, PFN_xrVoidFunction* out) {
    if (std::strcmp(name, "xrCreateActionSet") == 0) {
      *out = reinterpret_cast<PFN_xrVoidFunction>(create_set);
      return XR_SUCCESS;
    }
    if (std::strcmp(name, "xrCreateAction") == 0) {
      *out = reinterpret_cast<PFN_xrVoidFunction>(create_action);
      return XR_SUCCESS;
    }
    return reinterpret_cast<PFN_xrGetInstanceProcAddr>(active->original)(instance, name, out);
  }
};
// One build of a game: its action set and actions under its own names, Touch bindings
// for the trigger, played on an Index controller. What the host is asked to create.
struct Names {
  std::set<std::string> sets, actions;
};
Names play_build(const char* set_name, const std::vector<std::string>& action_names) {
  ventureMockXrSetProfile("/interaction_profiles/valve/index_controller");
  venture::test::XrImageFixture interop;
  venture::xr::install_xr_images(&interop);
  const char* extensions[]{"XR_KHR_opengl_es_enable"};
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  create.enabledExtensionCount = 1;
  create.enabledExtensionNames = extensions;
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&create, &instance) == XR_SUCCESS);
  HostNames names;
  xg::GlesBinding graphics{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR, nullptr, 1, 2, 3};
  XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO, &graphics, 0, 1};
  XrSession session{};
  REQUIRE(xg::xrCreateSession(instance, &ci, &session) == XR_SUCCESS);
  XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strcpy(set_info.actionSetName, set_name);
  std::strcpy(set_info.localizedActionSetName, set_name);
  XrActionSet set{};
  REQUIRE(xg::xrCreateActionSet(instance, &set_info, &set) == XR_SUCCESS);
  XrPath touch{}, trigger{};
  REQUIRE(xg::xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &touch) == XR_SUCCESS);
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/right/input/trigger/value", &trigger) == XR_SUCCESS);
  std::vector<XrAction> actions;
  std::vector<XrActionSuggestedBinding> bindings;
  for (const auto& name : action_names) {
    XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
    ai.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    std::strcpy(ai.actionName, name.c_str());
    std::strcpy(ai.localizedActionName, name.c_str());
    XrAction action{};
    REQUIRE(xg::xrCreateAction(set, &ai, &action) == XR_SUCCESS);
    actions.push_back(action);
    bindings.push_back({action, trigger});
  }
  XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr, touch,
                                                  std::uint32_t(bindings.size()), bindings.data()};
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
  // Every build reads the trigger, whatever it calls its actions.
  MockXrScript script{};
  script.right.trigger = 0.75f;
  ventureMockXrSetScript(&script);
  XrActiveActionSet active{set, XR_NULL_PATH};
  XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
  REQUIRE(xg::xrSyncActions(session, &sync) == XR_SUCCESS);
  for (const auto action : actions) {
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, action, XR_NULL_PATH};
    XrActionStateFloat value{XR_TYPE_ACTION_STATE_FLOAT};
    REQUIRE(xg::xrGetActionStateFloat(session, &get, &value) == XR_SUCCESS);
    CHECK(value.isActive);
    CHECK(value.currentState == doctest::Approx(0.75f));
  }
  CHECK(xg::xrDestroySession(session) == XR_SUCCESS);
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  venture::xr::install_xr_images(nullptr);
  MockXrScript restored{};
  ventureMockXrSetScript(&restored);
  ventureMockXrSetProfile(nullptr);
  return {names.sets, names.actions};
}
}  // namespace

TEST_CASE("A game update that renames its actions leaves the host's actions as they were") {
  // SteamVR keeps a player's custom binding by action name: the host sees the same
  // names from every build of a game, none of them the game's.
  venture::test::ScopedMenuMode automatic("");
  ScopedControls defaults("");
  const auto first = play_build("gameplay", {"fire"});
  const auto second = play_build("gameplay_v2", {"shoot_v2", "trigger_axis"});
  CHECK(first.sets == second.sets);
  CHECK(first.actions == second.actions);
  CHECK(first.sets == std::set<std::string>{"venture_controls"});
  for (const char* name : {"fire", "shoot_v2", "trigger_axis"}) CHECK_FALSE(second.actions.contains(name));
}

TEST_CASE("An OpenXR hand space made before the game suggests its bindings follows the hand") {
  // H3VR2 makes its grip pose space first and binds it later; the space must follow
  // the binding the game gives it, as on a Quest.
  venture::test::ScopedMenuMode automatic("");
  ScopedControls defaults("");
  ventureMockXrSetProfile("/interaction_profiles/valve/index_controller");
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
  std::strcpy(set_info.actionSetName, "hands");
  std::strcpy(set_info.localizedActionSetName, "Hands");
  XrActionSet set{};
  REQUIRE(xg::xrCreateActionSet(instance, &set_info, &set) == XR_SUCCESS);
  XrPath hands[2]{}, touch{}, grip{};
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/left", &hands[0]) == XR_SUCCESS);
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/right", &hands[1]) == XR_SUCCESS);
  REQUIRE(xg::xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &touch) == XR_SUCCESS);
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/left/input/grip/pose", &grip) == XR_SUCCESS);
  XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
  ai.actionType = XR_ACTION_TYPE_POSE_INPUT;
  ai.countSubactionPaths = 2;
  ai.subactionPaths = hands;
  std::strcpy(ai.actionName, "grip");
  std::strcpy(ai.localizedActionName, "Grip");
  XrAction pose{};
  REQUIRE(xg::xrCreateAction(set, &ai, &pose) == XR_SUCCESS);
  // The space first, the bindings after.
  XrActionSpaceCreateInfo space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO, nullptr, pose, hands[0], {{0, 0, 0, 1}, {0, 0, 0}}};
  XrSpace hand{};
  REQUIRE(xg::xrCreateActionSpace(session, &space_info, &hand) == XR_SUCCESS);
  XrReferenceSpaceCreateInfo local_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, XR_REFERENCE_SPACE_TYPE_LOCAL,
                                        {{0, 0, 0, 1}, {0, 0, 0}}};
  XrSpace local{};
  REQUIRE(xg::xrCreateReferenceSpace(session, &local_info, &local) == XR_SUCCESS);
  XrActionSuggestedBinding binding{pose, grip};
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
  XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
  // Not synced yet: not located.
  REQUIRE(xg::xrLocateSpace(hand, local, venture::monotonic_ns(), &location) == XR_SUCCESS);
  CHECK(location.locationFlags == 0);
  XrActiveActionSet active{set, XR_NULL_PATH};
  XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
  REQUIRE(xg::xrSyncActions(session, &sync) == XR_SUCCESS);
  REQUIRE(xg::xrLocateSpace(hand, local, venture::monotonic_ns(), &location) == XR_SUCCESS);
  CHECK((location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0);
  CHECK((location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0);
  CHECK(xg::xrDestroySpace(hand) == XR_SUCCESS);
  CHECK(xg::xrDestroySpace(local) == XR_SUCCESS);
  CHECK(xg::xrDestroySession(session) == XR_SUCCESS);
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  venture::xr::install_xr_images(nullptr);
  MockXrScript restored{};
  ventureMockXrSetScript(&restored);
  ventureMockXrSetProfile(nullptr);
}

TEST_CASE("A player's offset moves where a VrApi game sees that controller") {
  venture::test::ScopedMenuMode automatic("");
  constexpr const char* kIndexProfile = "/interaction_profiles/valve/index_controller";
  // Where the game sees the left controller under `controls`.
  auto locate = [&](const std::string& controls) {
    ScopedControls scoped(controls);
    VrApiGame game(kIndexProfile);
    MockXrScript script{};
    script.left.grip = {{0, 0, 0, 1}, {0.1f, 1.2f, -0.3f}};
    ventureMockXrSetScript(&script);
    REQUIRE(vr::sync_input() == XR_SUCCESS);
    vr::abi::TrackingHead tracking{};
    REQUIRE(vg::vrapi_GetInputTrackingState(game.mobile, 1, vg::vrapi_GetPredictedDisplayTime(game.mobile, 1),
                                            &tracking) == 0);
    return tracking.head.pose;
  };
  const auto plain = locate("");
  Config shifted;
  shifted[kIndexProfile].offsets[0].position = {0, 20, -100};  // 2 cm up, 10 cm forward
  const auto moved = locate(encode(shifted));
  CHECK(moved.position.x == doctest::Approx(plain.position.x).epsilon(0.001));
  CHECK(moved.position.y - plain.position.y == doctest::Approx(0.02f).epsilon(0.01));
  CHECK(moved.position.z - plain.position.z == doctest::Approx(-0.1f).epsilon(0.01));
  // Another controller's offset leaves this one alone.
  Config other;
  other["/interaction_profiles/htc/vive_controller"].offsets[0].position = {0, 0, -100};
  CHECK(locate(encode(other)).position.z == doctest::Approx(plain.position.z));
  // A turn turns it.
  Config turned;
  turned[kIndexProfile].offsets[0].rotation = {0, 90, 0};
  const auto t = locate(encode(turned));
  CHECK(std::abs(t.orientation.y - plain.orientation.y) > 0.3f);
}

namespace {
// Where an OpenXR Touch game sees the left grip and aim on host profile `profile`
// under `controls`, the host's grip and aim both unturned at the same point.
std::array<XrPosef, 2> openxr_grip_and_aim(const char* profile, const std::string& controls) {
  ScopedControls scoped(controls);
  ventureMockXrSetProfile(profile);
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
  std::strcpy(set_info.actionSetName, "poses");
  std::strcpy(set_info.localizedActionSetName, "Poses");
  XrActionSet set{};
  REQUIRE(xg::xrCreateActionSet(instance, &set_info, &set) == XR_SUCCESS);
  XrPath left{}, touch{}, paths[2]{};
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/left", &left) == XR_SUCCESS);
  REQUIRE(xg::xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &touch) == XR_SUCCESS);
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/left/input/grip/pose", &paths[0]) == XR_SUCCESS);
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/left/input/aim/pose", &paths[1]) == XR_SUCCESS);
  XrAction actions[2]{};
  XrActionSuggestedBinding bindings[2]{};
  for (int i = 0; i < 2; ++i) {
    XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
    ai.actionType = XR_ACTION_TYPE_POSE_INPUT;
    ai.countSubactionPaths = 1;
    ai.subactionPaths = &left;
    std::strcpy(ai.actionName, i ? "aim" : "grip");
    std::strcpy(ai.localizedActionName, i ? "Aim" : "Grip");
    REQUIRE(xg::xrCreateAction(set, &ai, &actions[i]) == XR_SUCCESS);
    bindings[i] = {actions[i], paths[i]};
  }
  XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr, touch, 2,
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
  MockXrScript script{};
  script.left.grip = script.left.aim = {{0, 0, 0, 1}, {-0.2f, 1.1f, -0.3f}};
  ventureMockXrSetScript(&script);
  XrActiveActionSet active{set, XR_NULL_PATH};
  XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
  REQUIRE(xg::xrSyncActions(session, &sync) == XR_SUCCESS);
  XrReferenceSpaceCreateInfo local_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, XR_REFERENCE_SPACE_TYPE_LOCAL,
                                        {{0, 0, 0, 1}, {0, 0, 0}}};
  XrSpace local{};
  REQUIRE(xg::xrCreateReferenceSpace(session, &local_info, &local) == XR_SUCCESS);
  std::array<XrPosef, 2> out{};
  for (int i = 0; i < 2; ++i) {
    XrActionSpaceCreateInfo space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO, nullptr, actions[i], left,
                                       {{0, 0, 0, 1}, {0, 0, 0}}};
    XrSpace space{};
    REQUIRE(xg::xrCreateActionSpace(session, &space_info, &space) == XR_SUCCESS);
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    REQUIRE(xg::xrLocateSpace(space, local, venture::monotonic_ns(), &location) == XR_SUCCESS);
    CHECK((location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0);
    out[std::size_t(i)] = location.pose;
    CHECK(xg::xrDestroySpace(space) == XR_SUCCESS);
  }
  CHECK(xg::xrDestroySpace(local) == XR_SUCCESS);
  CHECK(xg::xrDestroySession(session) == XR_SUCCESS);
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  venture::xr::install_xr_images(nullptr);
  MockXrScript restored{};
  ventureMockXrSetScript(&restored);
  ventureMockXrSetProfile(nullptr);
  return out;
}
}  // namespace

TEST_CASE("On a Steam Frame an OpenXR game's grip sits where a Quest's does, its aim stays the host's") {
  venture::test::ScopedMenuMode automatic("");
  constexpr const char* kFrame = "/interaction_profiles/valve/frame_controller_valve";
  const auto& q = venture::controls::find_profile(kFrame)->quest_grip;
  const auto [grip, aim] = openxr_grip_and_aim(kFrame, "");
  // The grip tilts up about its own x by the profile's turn and shifts by its offset, mirrored for the left.
  const float half = q.rotation[0] * 3.14159265f / 360;
  CHECK(grip.orientation.x == doctest::Approx(std::sin(half)).epsilon(0.001));
  CHECK(grip.orientation.w == doctest::Approx(std::cos(half)).epsilon(0.001));
  CHECK(grip.position.x == doctest::Approx(-0.2f - q.position[0] / 1000).epsilon(0.001));
  CHECK(grip.position.y - aim.position.y == doctest::Approx(q.position[1] / 1000).epsilon(0.01));
  CHECK(grip.position.z == doctest::Approx(-0.3f + q.position[2] / 1000).epsilon(0.001));
  CHECK(aim.orientation.x == doctest::Approx(0));
  CHECK(aim.orientation.w == doctest::Approx(1));
  CHECK(aim.position.x == doctest::Approx(-0.2f));
  CHECK(aim.position.z == doctest::Approx(-0.3f));
  // The player's tilt comes on top of it.
  Config tilted;
  tilted[kFrame].offsets[0].rotation = {-q.rotation[0], 0, 0};
  const auto back = openxr_grip_and_aim(kFrame, encode(tilted));
  CHECK(back[0].orientation.x == doctest::Approx(0).epsilon(0.001));
  CHECK(back[1].orientation.x == doctest::Approx(std::sin(-half)).epsilon(0.001));
  // An Index grip is already a Quest's.
  const auto index = openxr_grip_and_aim("/interaction_profiles/valve/index_controller", "");
  CHECK(index[0].orientation.x == doctest::Approx(0));
  CHECK(index[0].position.x == doctest::Approx(-0.2f));
}

TEST_CASE("On a Steam Frame a VrApi game's controller sits where a Quest's does") {
  venture::test::ScopedMenuMode automatic("");
  ScopedControls defaults("");
  auto locate = [&](const char* profile) {
    VrApiGame game(profile);
    MockXrScript script{};
    script.left.grip = {{0, 0, 0, 1}, {0.1f, 1.2f, -0.3f}};
    ventureMockXrSetScript(&script);
    REQUIRE(vr::sync_input() == XR_SUCCESS);
    vr::abi::TrackingHead tracking{};
    REQUIRE(vg::vrapi_GetInputTrackingState(game.mobile, 1, vg::vrapi_GetPredictedDisplayTime(game.mobile, 1),
                                            &tracking) == 0);
    return tracking.head.pose;
  };
  // The legacy pose is 60 degrees below the grip; on the Frame the grip itself tilts up first.
  const auto index = locate("/interaction_profiles/valve/index_controller");
  CHECK(index.orientation.x == doctest::Approx(-0.5f).epsilon(0.001));
  const auto& q = venture::controls::find_profile("/interaction_profiles/valve/frame_controller_valve")->quest_grip;
  const auto frame = locate("/interaction_profiles/valve/frame_controller_valve");
  CHECK(frame.orientation.x == doctest::Approx(std::sin((q.rotation[0] - 60) * 3.14159265f / 360)).epsilon(0.001));
}

TEST_CASE("Tuning reaches a VrApi game: stick dead zone and inversion, trigger press point, toggles, vibration off") {
  venture::test::ScopedMenuMode automatic("");
  constexpr const char* kIndex = "/interaction_profiles/valve/index_controller";
  Config config;
  auto& c = config[kIndex];
  c.tuning[0] = {20, false, true, 30, 0, false, 100};
  c.toggles[int(Quest::primary)] = true;
  ScopedControls controls(encode(config));
  VrApiGame game(kIndex);
  MockXrScript s{};
  s.left.thumbstick = {0, -0.5f};
  auto state = game.read(s, 0);
  CHECK(state.raw_joystick.y == doctest::Approx(0.5f));
  CHECK(state.joystick.y == doctest::Approx(0.375f));
  s = {};
  s.left.trigger = 0.35f;
  CHECK((game.read(s, 0).buttons & 0x20000000u) != 0);  // pressed at 30 %, not the usual 55 %
  s.right.trigger = 0.35f;
  CHECK((game.read(s, 1).buttons & 0x20000000u) == 0);  // the right hand keeps the default
  // A toggle: one press holds, the next lets go.
  MockXrScript pressed{};
  pressed.left.primary = true;
  CHECK(sees(game.read(pressed, 0), Quest::primary, 0));
  CHECK(sees(game.read(MockXrScript{}, 0), Quest::primary, 0));
  CHECK_FALSE(sees(game.read(pressed, 0), Quest::primary, 0));
  CHECK_FALSE(sees(game.read(MockXrScript{}, 0), Quest::primary, 0));
  // Vibration off on the left only.
  const auto pulses = ventureMockXrStats().haptic_pulses;
  CHECK(vg::vrapi_SetHapticVibrationSimple(game.mobile, 1, 1.f) == 0);
  REQUIRE(venture::xr::haptics_settled(std::chrono::seconds(2)));
  CHECK(ventureMockXrStats().haptic_pulses == pulses);
  CHECK(vg::vrapi_SetHapticVibrationSimple(game.mobile, 2, 1.f) == 0);
  REQUIRE(venture::xr::haptics_settled(std::chrono::seconds(2)));
  CHECK(ventureMockXrStats().haptic_pulses == pulses + 1);
}

TEST_CASE("Tuning reaches an OpenXR game: the grip's press point and a dead zone on its hand") {
  venture::test::ScopedMenuMode automatic("");
  constexpr const char* kIndex = "/interaction_profiles/valve/index_controller";
  Config config;
  config[kIndex].tuning[0].grip_press = 80;
  ScopedControls controls(encode(config));
  GripGame game(kIndex);
  game.sync(0.6f, 0);
  CHECK_FALSE(game.either().currentState);  // the left grip presses at 80 %
  game.sync(0.85f, 0);
  CHECK(game.either().currentState);
  game.sync(0.75f, 0);
  CHECK(game.either().currentState);  // held until a little lower
  game.sync(0.65f, 0);
  CHECK_FALSE(game.either().currentState);
  CHECK(game.hand(0).currentState == doctest::Approx(0.65f));  // the analog grip is untouched
}

namespace {
struct ScopedReport {
  std::filesystem::path file =
      std::filesystem::temp_directory_path() /
      ("venture_controllers_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
  ScopedReport() { set(file.string().c_str()); }
  ~ScopedReport() {
    set(nullptr);
    std::error_code ec;
    std::filesystem::remove(file, ec);
  }
  static void set(const char* value) {
#ifdef _WIN32
    _putenv_s("VENTURE_CONTROLLER_REPORT", value ? value : "");
#else
    if (value) setenv("VENTURE_CONTROLLER_REPORT", value, 1); else unsetenv("VENTURE_CONTROLLER_REPORT");
#endif
    venture::environment::refresh();
  }
  std::string text() const {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }
};
struct ScopedModels {
  explicit ScopedModels(std::array<std::string, 2> models) { venture::xr::override_controller_models(std::move(models)); }
  ~ScopedModels() { venture::xr::override_controller_models(std::nullopt); }
};
std::set<std::string> suggested(const char* profile) {
  std::string text(ventureMockXrSuggestedBindings(profile, nullptr, 0), '\0');
  ventureMockXrSuggestedBindings(profile, text.data(), std::uint32_t(text.size()));
  std::set<std::string> out;
  for (std::size_t at = 0, end; (end = text.find('\n', at)) != std::string::npos; at = end + 1)
    out.insert(text.substr(at, end - at));
  return out;
}
}  // namespace

TEST_CASE("Touch Plus is suggested with every input Venture binds on Touch, so a runtime that knows it picks it") {
  // A runtime picks the most specific profile it has bindings for; one given only
  // the Plus extras, or fewer inputs than Touch, could keep the controller on Touch.
  venture::test::ScopedMenuMode automatic("");
  ScopedControls defaults("");
  OpenXrGame game("/interaction_profiles/meta/touch_controller_plus");
  const XrAction a = game.action("a");
  REQUIRE(game.suggest("/interaction_profiles/oculus/touch_controller", {{a, game.path("/user/hand/right/input/a/click")}}) ==
          XR_SUCCESS);
  game.start();
  const auto touch = suggested("/interaction_profiles/oculus/touch_controller");
  const auto plus = suggested("/interaction_profiles/meta/touch_controller_plus");
  REQUIRE_FALSE(touch.empty());
  for (const auto& path : touch) {
    CAPTURE(path);
    CHECK(plus.contains(path));
  }
  for (const char* hand : {"/user/hand/left", "/user/hand/right"})
    for (const char* component : {"/input/trigger/value", "/input/trigger/touch", "/input/squeeze/value", "/input/thumbstick",
                                  "/input/thumbstick/click", "/input/thumbstick/touch", "/input/thumbrest/touch",
                                  "/input/grip/pose", "/input/aim/pose", "/output/haptic"}) {
      CAPTURE(hand);
      CAPTURE(component);
      CHECK(plus.contains(std::string(hand) + component));
    }
  for (const char* path : {"/user/hand/left/input/x/click", "/user/hand/left/input/x/touch", "/user/hand/left/input/y/click",
                           "/user/hand/left/input/y/touch", "/user/hand/left/input/menu/click", "/user/hand/right/input/a/click",
                           "/user/hand/right/input/a/touch", "/user/hand/right/input/b/click", "/user/hand/right/input/b/touch"}) {
    CAPTURE(path);
    CHECK(plus.contains(path));
  }
  // The game bound only Touch, so it is still told Touch on the Plus host.
  CHECK(game.current(1) == game.path("/interaction_profiles/oculus/touch_controller"));
}

TEST_CASE("A runtime that reports Touch for Touch Plus controllers: the report and the player's controls follow its render model") {
  // SteamVR has no Touch Plus profile and reports Touch for a Quest 3's
  // controllers; its render model for them is oculus_quest_plus_controller_*.
  venture::test::ScopedMenuMode automatic("");
  ScopedModels models({"oculus_quest_plus_controller_left", "oculus_quest_plus_controller_right"});
  ScopedReport report;
  Config config;
  // The player swapped X and Y on the controller the Controls page showed them.
  auto& plus = config["/interaction_profiles/meta/touch_controller_plus"];
  plus.rows[0][int(Quest::primary)] = Inputs{"/input/y/click"};
  plus.rows[0][int(Quest::secondary)] = Inputs{"/input/x/click"};
  ScopedControls controls(encode(config));
  VrApiGame game("/interaction_profiles/oculus/touch_controller");
  MockXrScript script{};
  press(script.left, "/input/x/click");
  const auto left = game.read(script, 0);
  CHECK(sees(left, Quest::secondary, 0));
  CHECK_FALSE(sees(left, Quest::primary, 0));
  const auto json = report.text();
  CAPTURE(json);
  CHECK(json.find("\"left\":\"/interaction_profiles/meta/touch_controller_plus\"") != std::string::npos);
  CHECK(json.find("\"right\":\"/interaction_profiles/meta/touch_controller_plus\"") != std::string::npos);
}

TEST_CASE("Without a render model naming another controller, the host's profile is the controller") {
  venture::test::ScopedMenuMode automatic("");
  // A Quest 2's Touch: the runtime's own profile is the controller.
  ScopedModels models({"oculus_quest2_controller_left", "oculus_quest2_controller_right"});
  Config config;
  config["/interaction_profiles/meta/touch_controller_plus"].rows[0][int(Quest::primary)] = Inputs{"/input/y/click"};
  ScopedControls controls(encode(config));
  VrApiGame game("/interaction_profiles/oculus/touch_controller");
  MockXrScript script{};
  press(script.left, "/input/x/click");
  CHECK(sees(game.read(script, 0), Quest::primary, 0));  // Touch's defaults, not Touch Plus's controls
}

TEST_CASE("Stick buttons push a VrApi game's stick and touch it") {
  // An arm-swinging player walks with a button instead of the stick.
  venture::test::ScopedMenuMode automatic("");
  constexpr const char* kTouch = "/interaction_profiles/oculus/touch_controller";
  Config config;
  config[kTouch].rows[1][int(Quest::stick_up)] = Inputs{"/input/b/click"};
  config[kTouch].rows[1][int(Quest::stick_left)] = Inputs{"/input/a/click"};
  config[kTouch].rows[0][int(Quest::stick_touch)] = Inputs{"/input/x/click"};
  config[kTouch].tuning[1].deadzone = 20;
  ScopedControls controls(encode(config));
  VrApiGame game(kTouch);
  auto rest = game.read(MockXrScript{}, 1);
  CHECK(rest.raw_joystick.x == 0);
  CHECK(rest.raw_joystick.y == 0);
  CHECK((rest.touches & 0x20u) == 0);
  MockXrScript script{};
  press(script.right, "/input/b/click");
  auto state = game.read(script, 1);
  CHECK(state.raw_joystick.y == doctest::Approx(1));
  CHECK(state.joystick.y == doctest::Approx(1));
  CHECK(state.raw_joystick.x == doctest::Approx(0));
  CHECK((state.touches & 0x20u) != 0);
  // With the stick held right, the button walks forward and the stick still steers.
  script.right.thumbstick = {0.6f, 0};
  state = game.read(script, 1);
  CHECK(state.raw_joystick.x == doctest::Approx(0.6f / std::sqrt(1.36f)));
  CHECK(state.raw_joystick.y == doctest::Approx(1 / std::sqrt(1.36f)));
  CHECK(state.joystick.x == doctest::Approx(0.5f / std::sqrt(1.25f)));  // past the 20 % dead zone
  // Two directions together: a diagonal, all the way.
  script = {};
  press(script.right, "/input/b/click");
  press(script.right, "/input/a/click");
  state = game.read(script, 1);
  CHECK(state.raw_joystick.x == doctest::Approx(-std::sqrt(0.5f)));
  CHECK(state.raw_joystick.y == doctest::Approx(std::sqrt(0.5f)));
  // Let go: the stick is the stick again.
  CHECK(game.read(MockXrScript{}, 1).raw_joystick.y == 0);
  // Stick touch on X touches the left stick and leaves it where it is.
  script = {};
  press(script.left, "/input/x/click");
  state = game.read(script, 0);
  CHECK((state.touches & 0x20u) != 0);
  CHECK(state.raw_joystick.y == 0);
  CHECK((game.read(script, 1).touches & 0x20u) == 0);
}

TEST_CASE("Stick buttons push an OpenXR game's stick, its halves and its touch") {
  venture::test::ScopedMenuMode automatic("");
  constexpr const char* kIndex = "/interaction_profiles/valve/index_controller";
  Config config;
  config[kIndex].rows[1][int(Quest::stick_up)] = Inputs{"/input/b/click"};
  config[kIndex].rows[1][int(Quest::stick_right)] = Inputs{"/input/trackpad/dpad_right"};
  ScopedControls controls(encode(config));
  ventureMockXrSetProfile(kIndex);
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
  std::strcpy(set_info.actionSetName, "stick_test");
  std::strcpy(set_info.localizedActionSetName, "Stick test");
  XrActionSet set{};
  REQUIRE(xg::xrCreateActionSet(instance, &set_info, &set) == XR_SUCCESS);
  XrPath right{}, touch{};
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/right", &right) == XR_SUCCESS);
  REQUIRE(xg::xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &touch) == XR_SUCCESS);
  auto action = [&](const char* name, XrActionType type) {
    XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
    ai.actionType = type;
    ai.countSubactionPaths = 1;
    ai.subactionPaths = &right;
    std::strcpy(ai.actionName, name);
    std::strcpy(ai.localizedActionName, name);
    XrAction out{};
    REQUIRE(xg::xrCreateAction(set, &ai, &out) == XR_SUCCESS);
    return out;
  };
  const XrAction move = action("move", XR_ACTION_TYPE_VECTOR2F_INPUT), north = action("north", XR_ACTION_TYPE_BOOLEAN_INPUT),
                 touched = action("touched", XR_ACTION_TYPE_BOOLEAN_INPUT);
  auto path = [&](const char* text) {
    XrPath out{};
    REQUIRE(xg::xrStringToPath(instance, text, &out) == XR_SUCCESS);
    return out;
  };
  XrActionSuggestedBinding bindings[]{{move, path("/user/hand/right/input/thumbstick")},
                                      {north, path("/user/hand/right/input/thumbstick/dpad_up")},
                                      {touched, path("/user/hand/right/input/thumbstick/touch")}};
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
  XrActiveActionSet active{set, XR_NULL_PATH};
  XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
  auto sync_with = [&](const MockXrScript& script) {
    ventureMockXrSetScript(&script);
    REQUIRE(xg::xrSyncActions(session, &sync) == XR_SUCCESS);
  };
  auto stick = [&] {
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, move, XR_NULL_PATH};
    XrActionStateVector2f value{XR_TYPE_ACTION_STATE_VECTOR2F};
    REQUIRE(xg::xrGetActionStateVector2f(session, &get, &value) == XR_SUCCESS);
    CHECK(value.isActive);
    return value.currentState;
  };
  auto pressed = [&](XrAction a) {
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, a, XR_NULL_PATH};
    XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
    REQUIRE(xg::xrGetActionStateBoolean(session, &get, &value) == XR_SUCCESS);
    return value.isActive && value.currentState;
  };
  sync_with(MockXrScript{});
  CHECK(stick().y == 0);
  CHECK_FALSE(pressed(north));
  CHECK_FALSE(pressed(touched));
  MockXrScript script{};
  press(script.right, "/input/b/click");
  sync_with(script);
  CHECK(stick().x == doctest::Approx(0));
  CHECK(stick().y == doctest::Approx(1));
  CHECK(pressed(north));  // the stick's upper half, from the pushed stick
  CHECK(pressed(touched));
  // A trackpad half as Stick right, with the real stick held down a little.
  script = {};
  press(script.right, "/input/trackpad/dpad_right");
  script.right.thumbstick = {0, -0.4f};
  sync_with(script);
  const auto v = stick();
  CHECK(v.x == doctest::Approx(1 / std::sqrt(1.16f)));
  CHECK(v.y == doctest::Approx(-0.4f / std::sqrt(1.16f)));
  CHECK_FALSE(pressed(north));
  // Released: the stick alone.
  script = {};
  script.right.thumbstick = {0, -0.4f};
  sync_with(script);
  CHECK(stick().x == 0);
  CHECK(stick().y == doctest::Approx(-0.4f));
  CHECK(xg::xrDestroySession(session) == XR_SUCCESS);
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  venture::xr::install_xr_images(nullptr);
  MockXrScript restored{};
  ventureMockXrSetScript(&restored);
  ventureMockXrSetProfile(nullptr);
}

TEST_CASE("A VrApi game gets a button as a full analog value and an analog input as a press") {
  venture::test::ScopedMenuMode automatic("");
  constexpr const char* kIndex = "/interaction_profiles/valve/index_controller";
  Config config;
  config[kIndex].rows[1][int(Quest::trigger)] = Inputs{"/input/b/click"};
  config[kIndex].rows[1][int(Quest::primary)] = Inputs{"/input/squeeze/force"};
  config[kIndex].rows[1][int(Quest::secondary)] = Inputs{"/input/a/touch"};
  ScopedControls controls(encode(config));
  VrApiGame game(kIndex);
  MockXrScript script{};
  press(script.right, "/input/b/click");
  auto state = game.read(script, 1);
  CHECK(state.trigger == doctest::Approx(1));
  CHECK((state.buttons & 0x1u) == 0);
  press(script.right, "/input/squeeze/force", 0.3f);  // under the runtime's press point
  CHECK((game.read(script, 1).buttons & 0x1u) == 0);
  press(script.right, "/input/squeeze/force", 0.8f);
  state = game.read(script, 1);
  CHECK((state.buttons & 0x1u) != 0);  // A
  // A touch sensor as a button: resting on A presses B.
  script = {};
  press(script.right, "/input/a/touch");
  CHECK((game.read(script, 1).buttons & 0x2u) != 0);
}

TEST_CASE("Touch rows touch a VrApi game's A, B and trigger") {
  // A controller without the sensors (a Vive wand) still lets a game see a resting finger.
  venture::test::ScopedMenuMode automatic("");
  constexpr const char* kVive = "/interaction_profiles/htc/vive_controller";
  Config config;
  config[kVive].rows[1][int(Quest::trigger_touch)] = Inputs{"/input/trackpad/touch"};
  config[kVive].rows[1][int(Quest::primary_touch)] = Inputs{"/input/trackpad/dpad_center"};
  config[kVive].rows[0][int(Quest::secondary_touch)] = Inputs{"/input/menu/click"};
  ScopedControls controls(encode(config));
  VrApiGame game(kVive);
  auto rest = game.read(MockXrScript{}, 1);
  CHECK((rest.touches & 0x43u) == 0);
  MockXrScript script{};
  press(script.right, "/input/trackpad/touch");
  auto state = game.read(script, 1);
  CHECK((state.touches & 0x40u) != 0);  // trigger
  CHECK((state.touches & 0x1u) == 0);
  press(script.right, "/input/trackpad/dpad_center");
  state = game.read(script, 1);
  CHECK((state.touches & 0x1u) != 0);  // A
  CHECK((state.buttons & 0x1u) == 0);  // touched, not pressed
  script = {};
  press(script.left, "/input/menu/click");
  state = game.read(script, 0);
  CHECK((state.touches & 0x8u) != 0);  // Y
  CHECK((game.read(script, 1).touches & 0x2u) == 0);
}

TEST_CASE("Touch rows touch an OpenXR game's A and trigger") {
  venture::test::ScopedMenuMode automatic("");
  constexpr const char* kIndex = "/interaction_profiles/valve/index_controller";
  Config config;
  config[kIndex].rows[1][int(Quest::primary_touch)] = Inputs{"/input/trackpad/touch"};
  config[kIndex].rows[1][int(Quest::trigger_touch)] = Inputs{"/input/trackpad/dpad_left"};
  ScopedControls controls(encode(config));
  ventureMockXrSetProfile(kIndex);
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
  std::strcpy(set_info.actionSetName, "touch_test");
  std::strcpy(set_info.localizedActionSetName, "Touch test");
  XrActionSet set{};
  REQUIRE(xg::xrCreateActionSet(instance, &set_info, &set) == XR_SUCCESS);
  XrPath right{}, touch{};
  REQUIRE(xg::xrStringToPath(instance, "/user/hand/right", &right) == XR_SUCCESS);
  REQUIRE(xg::xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &touch) == XR_SUCCESS);
  auto action = [&](const char* name) {
    XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
    ai.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    ai.countSubactionPaths = 1;
    ai.subactionPaths = &right;
    std::strcpy(ai.actionName, name);
    std::strcpy(ai.localizedActionName, name);
    XrAction out{};
    REQUIRE(xg::xrCreateAction(set, &ai, &out) == XR_SUCCESS);
    return out;
  };
  const XrAction a_touch = action("a_touch"), trigger_touch = action("trigger_touch"), a_click = action("a_click");
  auto path = [&](const char* text) {
    XrPath out{};
    REQUIRE(xg::xrStringToPath(instance, text, &out) == XR_SUCCESS);
    return out;
  };
  XrActionSuggestedBinding bindings[]{{a_touch, path("/user/hand/right/input/a/touch")},
                                      {trigger_touch, path("/user/hand/right/input/trigger/touch")},
                                      {a_click, path("/user/hand/right/input/a/click")}};
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
  XrActiveActionSet active{set, XR_NULL_PATH};
  XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
  auto sync_with = [&](const MockXrScript& script) {
    ventureMockXrSetScript(&script);
    REQUIRE(xg::xrSyncActions(session, &sync) == XR_SUCCESS);
  };
  auto pressed = [&](XrAction a) {
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, a, XR_NULL_PATH};
    XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
    REQUIRE(xg::xrGetActionStateBoolean(session, &get, &value) == XR_SUCCESS);
    return value.isActive && value.currentState;
  };
  // The mock's default trigger rests a finger on it; these scripts lift it.
  MockXrScript script{};
  script.trigger = 0;
  sync_with(script);
  CHECK_FALSE(pressed(a_touch));
  CHECK_FALSE(pressed(trigger_touch));
  press(script.right, "/input/trackpad/touch");
  sync_with(script);
  CHECK(pressed(a_touch));
  CHECK_FALSE(pressed(a_click));
  CHECK_FALSE(pressed(trigger_touch));
  script = {};
  script.trigger = 0;
  press(script.right, "/input/trackpad/dpad_left");
  sync_with(script);
  CHECK(pressed(trigger_touch));
  // A's own sensor still touches A.
  script = {};
  script.trigger = 0;
  press(script.right, "/input/a/touch");
  sync_with(script);
  CHECK(pressed(a_touch));
  CHECK(xg::xrDestroySession(session) == XR_SUCCESS);
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  venture::xr::install_xr_images(nullptr);
  MockXrScript restored{};
  ventureMockXrSetScript(&restored);
  ventureMockXrSetProfile(nullptr);
}
