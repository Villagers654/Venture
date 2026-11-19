#include "common/controls.h"
#include "common/controls_file.h"
#include <doctest/doctest.h>
#include <set>

using namespace venture::controls;
namespace {
const Profile& profile(std::string_view path) {
  const auto* p = find_profile(path);
  REQUIRE(p);
  return *p;
}
// The one host path a guest path reaches, or none; a row with several inputs fails here.
std::optional<std::string> one_binding(std::string_view guest, const Profile& p, const Resolved& r, bool dpad) {
  auto paths = host_bindings(guest, p, r, dpad);
  CHECK(paths.size() <= 1);
  if (paths.empty()) return std::nullopt;
  return paths.front();
}
std::optional<std::string> bind(std::string_view guest, std::string_view host, const Choice* choice = nullptr,
                                bool dpad = true) {
  const auto& p = profile(host);
  return one_binding(guest, p, resolve(p, choice), dpad);
}
constexpr std::string_view kIndex = "/interaction_profiles/valve/index_controller";
constexpr std::string_view kTouch = "/interaction_profiles/oculus/touch_controller";
constexpr std::string_view kVive = "/interaction_profiles/htc/vive_controller";
constexpr std::string_view kGeneric = "/interaction_profiles/khr/generic_controller";
// Every Touch path a Quest game binds.
constexpr std::string_view kGuestPaths[]{
    "/input/x/click", "/input/x/touch", "/input/y/click", "/input/y/touch", "/input/a/click", "/input/a/touch",
    "/input/b/click", "/input/b/touch", "/input/menu/click", "/input/thumbstick", "/input/thumbstick/x",
    "/input/thumbstick/y", "/input/thumbstick/click", "/input/thumbstick/touch", "/input/trigger/value",
    "/input/trigger/touch", "/input/squeeze/value", "/input/thumbrest/touch", "/input/grip/pose",
    "/input/aim/pose", "/output/haptic"};
}  // namespace

TEST_CASE("Controls defaults use real, free inputs of each controller") {
  for (const auto& p : kProfiles) {
    CAPTURE(p.path);
    const auto r = resolve(p, nullptr);
    for (int hand = 0; hand < 2; ++hand)
      for (int q = 0; q < kQuestCount; ++q) {
        const auto path = p.defaults[size_t(hand)][size_t(q)];
        CHECK(r.rows[size_t(hand)][size_t(q)] == (path.empty() ? Inputs{} : Inputs{std::string(path)}));
        if (path.empty()) continue;
        const auto* input = find_input(p, path);
        REQUIRE(input);
        CHECK_FALSE(input->reserved);
        CHECK(on_hand(*input, hand));
        CHECK(is_axes(path) == (q == int(Quest::stick)));
      }
    // The menu: the left input it names (if any) and both stick clicks, always.
    CHECK(r.menu.automatic);
    CHECK(r.menu.sources == std::vector<Inputs>{{std::string(hand_root(0)) + std::string(p.defaults[0][size_t(Quest::stick_click)])},
                                                {std::string(hand_root(1)) + std::string(p.defaults[1][size_t(Quest::stick_click)])}});
    CHECK(r.menu.hold_ns == 0);  // pressed together, no hold
    if (p.menu_path.empty()) {
      CHECK(r.menu.kind == Menu::Kind::synthesized);
    } else {
      CHECK(r.menu.kind == Menu::Kind::direct);
      CHECK(r.menu.direct == p.menu_path);
      const auto* input = find_input(p, p.menu_path);
      REQUIRE(input);
      CHECK(on_hand(*input, 0));
      CHECK_FALSE(input->reserved);
    }
    // No two Quest inputs share a host input, except the menu's stick clicks.
    CHECK(conflicts(r, MenuMode::automatic) ==
          std::vector<Conflict>{{0, Quest::menu, Quest::stick_click}, {1, Quest::menu, Quest::stick_click}});
    // Every translated path is one of the controller's inputs (or a pose, haptics or a sensor it has).
    for (const bool dpad : {true, false})
      for (int hand = 0; hand < 2; ++hand)
        for (auto guest : kGuestPaths) {
          const auto host = one_binding(std::string(hand_root(hand)) + std::string(guest), p, r, dpad);
          if (!host) continue;
          const std::string_view component = std::string_view(*host).substr(hand_root(hand).size());
          CHECK(host->starts_with(hand_root(hand)));
          if (component.ends_with("/pose") || component == "/output/haptic") continue;
          if (component.ends_with("/touch") && !find_input(p, component)) {
            CHECK(guest.ends_with("/touch"));
            continue;
          }
          auto base = component;
          if (base.ends_with("/x") || base.ends_with("/y")) base.remove_suffix(2);
          CAPTURE(component);
          const auto* input = find_input(p, base);
          REQUIRE(input);
          CHECK(on_hand(*input, hand));
          if (!dpad) CHECK(component.find("/dpad_", 7) == std::string_view::npos);
        }
  }
}

TEST_CASE("Controls open the menu with the menu button and both stick clicks on every runtime") {
  // Some runtimes keep the left menu button of Touch-shaped controllers (SteamVR's
  // dashboard), others give it to the game (Meta, Virtual Desktop, Pico, Monado):
  // the default binds the button where the host lets the game have it and adds both
  // stick clicks pressed together beside it, so one default works everywhere.
  const std::set<std::string_view> menu_button{kTouch, "/interaction_profiles/meta/touch_controller_plus",
                                               "/interaction_profiles/facebook/touch_controller_pro",
                                               "/interaction_profiles/bytedance/pico4_controller",
                                               "/interaction_profiles/bytedance/pico_neo3_controller",
                                               "/interaction_profiles/htc/vive_focus3_controller",
                                               "/interaction_profiles/htc/vive_cosmos_controller",
                                               "/interaction_profiles/hp/mixed_reality_controller",
                                               "/interaction_profiles/microsoft/motion_controller", kVive};
  for (const auto& p : kProfiles) {
    CAPTURE(p.path);
    const auto r = resolve(p, nullptr);
    CHECK((p.menu_path == "/input/menu/click") == menu_button.contains(p.path));
    if (!menu_button.contains(p.path)) continue;
    CHECK(r.menu.kind == Menu::Kind::direct);
    CHECK(p.menu_label == "Left menu button");
    CHECK(one_binding("/user/hand/left/input/menu/click", p, r, true) == "/user/hand/left/input/menu/click");
    CHECK_FALSE(one_binding("/user/hand/right/input/menu/click", p, r, true));
  }
  const auto touch = resolve(profile(kTouch), nullptr);
  CHECK(touch.menu.sources == std::vector<Inputs>{{"/user/hand/left/input/thumbstick/click"},
                                                  {"/user/hand/right/input/thumbstick/click"}});
  // The generic controller has no menu button: both stick clicks alone.
  const auto generic = resolve(profile(kGeneric), nullptr);
  CHECK(generic.menu.kind == Menu::Kind::synthesized);
  CHECK(generic.menu.sources.size() == 2);
  CHECK_FALSE(one_binding("/user/hand/left/input/menu/click", profile(kGeneric), generic, true));
  // The stick clicks follow the Stick click rows: the Vive's are the trackpad centers.
  CHECK(resolve(profile(kVive), nullptr).menu.sources ==
        std::vector<Inputs>{{"/user/hand/left/input/trackpad/dpad_center"}, {"/user/hand/right/input/trackpad/dpad_center"}});
  CHECK(profile(kIndex).menu_path == "/input/trackpad/force");
  CHECK(profile("/interaction_profiles/valve/frame_controller_valve").menu_path == "/input/view/click");
  CHECK(profile("/interaction_profiles/mndx/pssense_controller_mndx").menu_path == "/input/share_mndx/click");
  // A player's own choice replaces both: the left menu button alone, or both stick clicks alone.
  Choice c;
  c.menu = {MenuMode::button, "/user/hand/left/input/menu/click", {}, kHoldMs};
  const auto chosen = resolve(profile(kTouch), &c);
  CHECK(chosen.menu.kind == Menu::Kind::direct);
  CHECK_FALSE(chosen.menu.automatic);
  CHECK(chosen.menu.sources.empty());
  CHECK(one_binding("/user/hand/left/input/menu/click", profile(kTouch), chosen, true) ==
        "/user/hand/left/input/menu/click");
  c.menu = {MenuMode::chord, "/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click",
            kChordHoldMs};
  const auto chord = resolve(profile(kTouch), &c);
  CHECK(chord.menu.kind == Menu::Kind::synthesized);
  CHECK_FALSE(chord.menu.automatic);
  CHECK_FALSE(one_binding("/user/hand/left/input/menu/click", profile(kTouch), chord, true));
}

TEST_CASE("Controls defaults follow each controller's precedent") {
  CHECK(bind("/user/hand/left/input/x/click", kIndex) == "/user/hand/left/input/a/click");
  CHECK(bind("/user/hand/left/input/x/touch", kIndex) == "/user/hand/left/input/a/touch");
  CHECK(bind("/user/hand/right/input/b/click", kIndex) == "/user/hand/right/input/b/click");
  CHECK(bind("/user/hand/left/input/thumbrest/touch", kIndex) == "/user/hand/left/input/trackpad/touch");
  CHECK(bind("/user/hand/left/input/menu/click", kIndex) == "/user/hand/left/input/trackpad/force");
  CHECK_FALSE(bind("/user/hand/right/input/system/click", kIndex));
  CHECK(bind("/user/hand/left/input/thumbstick/x", kIndex) == "/user/hand/left/input/thumbstick/x");
  CHECK(bind("/user/hand/right/output/haptic", kIndex) == "/user/hand/right/output/haptic");
  // Touch is identity (the menu on the left only); Pro's extra sensors are not
  // ordinary Touch controls.
  for (auto guest : kGuestPaths)
    if (!std::string_view(guest).starts_with("/input/a") && !std::string_view(guest).starts_with("/input/b")) {
      const std::string left = "/user/hand/left" + std::string(guest);
      CHECK(bind(left, kTouch) == left);
    }
  CHECK_FALSE(bind("/user/hand/right/input/menu/click", kTouch));
  CHECK_FALSE(bind("/user/hand/left/input/trigger/proximity_fb", kTouch));
  // Vive: trackpad halves for the face buttons, the center for the stick click.
  CHECK(bind("/user/hand/left/input/x/click", kVive) == "/user/hand/left/input/trackpad/dpad_down");
  CHECK(bind("/user/hand/right/input/b/click", kVive) == "/user/hand/right/input/trackpad/dpad_up");
  CHECK(bind("/user/hand/left/input/thumbstick/click", kVive) == "/user/hand/left/input/trackpad/dpad_center");
  CHECK(bind("/user/hand/left/input/thumbstick/y", kVive) == "/user/hand/left/input/trackpad/y");
  CHECK(bind("/user/hand/left/input/thumbstick/touch", kVive) == "/user/hand/left/input/trackpad/touch");
  CHECK(bind("/user/hand/left/input/squeeze/value", kVive) == "/user/hand/left/input/squeeze/click");
  CHECK(bind("/user/hand/left/input/menu/click", kVive) == "/user/hand/left/input/menu/click");
  CHECK_FALSE(bind("/user/hand/left/input/x/touch", kVive));
  CHECK_FALSE(bind("/user/hand/left/input/thumbrest/touch", kVive));
  // Generic: primary and secondary; the menu is synthesized, never bound.
  CHECK(bind("/user/hand/left/input/x/click", kGeneric) == "/user/hand/left/input/primary/click");
  CHECK(bind("/user/hand/right/input/b/click", kGeneric) == "/user/hand/right/input/secondary/click");
  CHECK_FALSE(bind("/user/hand/left/input/menu/click", kGeneric));
  CHECK_FALSE(bind("/user/hand/left/input/x/touch", kGeneric));
  // Windows Mixed Reality: SteamVR's own Touch remap (trackpad south X/A, north Y/B),
  // the real stick for the stick and its click, the left menu button.
  constexpr std::string_view kWmr = "/interaction_profiles/microsoft/motion_controller";
  CHECK(bind("/user/hand/left/input/x/click", kWmr) == "/user/hand/left/input/trackpad/dpad_down");
  CHECK(bind("/user/hand/right/input/b/click", kWmr) == "/user/hand/right/input/trackpad/dpad_up");
  CHECK(bind("/user/hand/left/input/thumbstick/click", kWmr) == "/user/hand/left/input/thumbstick/click");
  CHECK(bind("/user/hand/left/input/menu/click", kWmr) == "/user/hand/left/input/menu/click");
  // Steam Frame: SteamVR's Touch remap (X d-pad down, Y d-pad up, Menu on the left View button).
  constexpr std::string_view kFrame = "/interaction_profiles/valve/frame_controller_valve";
  CHECK(bind("/user/hand/left/input/x/click", kFrame) == "/user/hand/left/input/dpad_down/click");
  CHECK(bind("/user/hand/left/input/y/click", kFrame) == "/user/hand/left/input/dpad_up/click");
  CHECK(bind("/user/hand/right/input/a/click", kFrame) == "/user/hand/right/input/a/click");
  CHECK(bind("/user/hand/left/input/menu/click", kFrame) == "/user/hand/left/input/view/click");
  // PlayStation VR2 Sense: face buttons by position, Menu on Create (the left button where Quest's is).
  constexpr std::string_view kSense = "/interaction_profiles/mndx/pssense_controller_mndx";
  CHECK(bind("/user/hand/left/input/x/click", kSense) == "/user/hand/left/input/square_mndx/click");
  CHECK(bind("/user/hand/right/input/b/click", kSense) == "/user/hand/right/input/circle_mndx/click");
  CHECK(bind("/user/hand/left/input/menu/click", kSense) == "/user/hand/left/input/share_mndx/click");
  // Touch-shaped controllers are identity, the left menu button included.
  for (auto host : {"/interaction_profiles/htc/vive_cosmos_controller", "/interaction_profiles/htc/vive_focus3_controller",
                    "/interaction_profiles/hp/mixed_reality_controller", "/interaction_profiles/bytedance/pico4_controller",
                    "/interaction_profiles/bytedance/pico_neo3_controller"}) {
    CAPTURE(host);
    for (auto guest : {"/user/hand/left/input/x/click", "/user/hand/left/input/y/click", "/user/hand/right/input/a/click",
                       "/user/hand/right/input/b/click"})
      CHECK(bind(guest, host) == guest);
    CHECK(bind("/user/hand/left/input/menu/click", host) == "/user/hand/left/input/menu/click");
  }
}

TEST_CASE("Controls offer only inputs the controller has on that hand") {
  // Pico 4's menu button is on the left only: a right menu button is not a menu choice.
  const auto& pico = profile("/interaction_profiles/bytedance/pico4_controller");
  const auto* menu = find_input(pico, "/input/menu/click");
  REQUIRE(menu);
  CHECK(on_hand(*menu, 0));
  CHECK_FALSE(on_hand(*menu, 1));
  Choice choice;
  choice.menu = {MenuMode::button, "/user/hand/right/input/menu/click", {}, kHoldMs};
  CHECK(resolve(pico, &choice).menu.automatic);
  // Pico Neo 3 has one on each controller: the right one can be the menu button.
  const auto& neo3 = profile("/interaction_profiles/bytedance/pico_neo3_controller");
  const auto* neo3_menu = find_input(neo3, "/input/menu/click");
  REQUIRE(neo3_menu);
  CHECK(on_hand(*neo3_menu, 0));
  CHECK(on_hand(*neo3_menu, 1));
  const auto right = resolve(neo3, &choice);
  CHECK_FALSE(right.menu.automatic);
  CHECK(right.menu.sources == std::vector<Inputs>{{"/user/hand/right/input/menu/click"}});
}

TEST_CASE("Controls trackpad halves without XR_EXT_dpad_binding are split by Venture") {
  // The host binds no half; Venture reads the pad and its press (#159: WMR on its own runtime).
  CHECK_FALSE(bind("/user/hand/left/input/x/click", kVive, nullptr, false));
  CHECK_FALSE(bind("/user/hand/left/input/y/click", kVive, nullptr, false));
  CHECK_FALSE(bind("/user/hand/left/input/thumbstick/click", kVive, nullptr, false));
  const auto& wmr = profile("/interaction_profiles/microsoft/motion_controller");
  const auto r = resolve(wmr, nullptr);
  for (int hand = 0; hand < 2; ++hand)
    CHECK(pads(wmr, r, hand) == std::vector<Pad>{{"/input/trackpad", "/input/trackpad/click"}});
  // Index's pad has no click: its press is the force sensor.
  Choice choice;
  choice.rows[1][int(Quest::secondary)] = Inputs{"/input/trackpad/dpad_up"};
  REQUIRE(find_input(profile(kIndex), "/input/trackpad/force"));
  CHECK_FALSE(bind("/user/hand/right/input/b/click", kIndex, &choice, false));
  CHECK(bind("/user/hand/right/input/b/click", kIndex, &choice, true) == "/user/hand/right/input/trackpad/dpad_up");
  CHECK(pads(profile(kIndex), resolve(profile(kIndex), &choice), 1) ==
        std::vector<Pad>{{"/input/trackpad", "/input/trackpad/force"}});
  CHECK(pads(profile(kIndex), resolve(profile(kIndex), &choice), 0).empty());
  // Not a listed input: the choice falls back to the default B.
  choice.rows[1][int(Quest::secondary)] = Inputs{"/input/thumbstick/dpad_up"};
  CHECK(bind("/user/hand/right/input/b/click", kIndex, &choice, false) == "/user/hand/right/input/b/click");
  // Controllers without halves read no pad.
  CHECK(pads(profile(kTouch), resolve(profile(kTouch), nullptr), 0).empty());
  // A menu source on a half becomes the pad's press.
  CHECK(bindable("/user/hand/left/input/trackpad/dpad_up", profile(kVive), false) == "/user/hand/left/input/trackpad/click");
  CHECK(bindable("/user/hand/left/input/trackpad/dpad_up", profile(kVive), true) == "/user/hand/left/input/trackpad/dpad_up");
}

TEST_CASE("Controls trackpad halves split the pad by where it is pressed") {
  CHECK(half_pressed("/input/trackpad/dpad_center", 0.1f, -0.2f));
  CHECK_FALSE(half_pressed("/input/trackpad/dpad_up", 0.1f, 0.2f));  // the center is not a half
  CHECK(half_pressed("/input/trackpad/dpad_up", 0.2f, 0.9f));
  CHECK(half_pressed("/input/trackpad/dpad_down", -0.3f, -0.8f));
  CHECK_FALSE(half_pressed("/input/trackpad/dpad_down", 0.f, 0.8f));
  CHECK(half_pressed("/input/trackpad/dpad_left", -0.9f, 0.1f));
  CHECK(half_pressed("/input/trackpad/dpad_right", 0.9f, -0.1f));
  CHECK_FALSE(half_pressed("/input/trackpad/dpad_center", 0.f, 0.7f));
}

TEST_CASE("Controls offsets are saved, limited, mirrored and turned") {
  Config config;
  auto& index = config[std::string(kIndex)];
  index.offsets[0] = {{10, -5, 20}, {-30, 0, 0}};
  index.offsets[1] = mirrored(index.offsets[0]);
  CHECK(index.offsets[1] == Offset{{-10, -5, 20}, {-30, 0, 0}});
  const auto back = decode(encode(config));
  REQUIRE(back.contains(std::string(kIndex)));
  CHECK(back.at(std::string(kIndex)).offsets == index.offsets);
  // A hand without an offset is not written.
  Config one;
  one["x"].offsets[1].rotation = {0, 15, 0};
  CHECK(encode(one).find("\"left\"") == std::string::npos);
  // Out of range values are held at the limits.
  const auto wild = decode(R"({"/interaction_profiles/valve/index_controller":{"offsets":{"left":{"position":[900,0,0],"rotation":[0,0,-400]}}}})");
  const auto& o = wild.at(std::string(kIndex)).offsets[0];
  CHECK(o.position[0] == kMaxShiftMm);
  CHECK(o.rotation[2] == -kMaxTurnDeg);
  CHECK(resolve(profile(kIndex), &wild.at(std::string(kIndex))).offsets[0] == o);
  // Tilt alone turns about x; turn alone about y.
  const auto tilt = orientation({{}, {90, 0, 0}});
  CHECK(tilt[0] == doctest::Approx(0.70710678f));
  CHECK(tilt[3] == doctest::Approx(0.70710678f));
  const auto turn = orientation({{}, {0, -60, 0}});
  CHECK(turn[1] == doctest::Approx(-0.5f));
  CHECK(turn[3] == doctest::Approx(0.8660254f));
  CHECK(orientation({}) == std::array<float, 4>{0, 0, 0, 1});
}

TEST_CASE("Controls Steam Frame touches follow every button with a sensor") {
  const auto& frame = profile("/interaction_profiles/valve/frame_controller_valve");
  const auto r = resolve(frame, nullptr);
  CHECK(host_bindings("/user/hand/right/input/a/touch", frame, r, true) ==
        std::vector<std::string>{"/user/hand/right/input/a/touch"});
  CHECK(host_bindings("/user/hand/left/input/x/touch", frame, r, true) ==
        std::vector<std::string>{"/user/hand/left/input/dpad_down/touch"});
  CHECK(host_bindings("/user/hand/left/input/y/touch", frame, r, true) ==
        std::vector<std::string>{"/user/hand/left/input/dpad_up/touch"});
}

TEST_CASE("Controls choices move, clear and refuse inputs") {
  Choice choice;
  // Swap X and Y on Touch, and take the right stick click away.
  choice.rows[0][int(Quest::primary)] = Inputs{"/input/y/click"};
  choice.rows[0][int(Quest::secondary)] = Inputs{"/input/x/click"};
  choice.rows[1][int(Quest::stick_click)] = Inputs{};
  CHECK(bind("/user/hand/left/input/x/click", kTouch, &choice) == "/user/hand/left/input/y/click");
  CHECK(bind("/user/hand/left/input/x/touch", kTouch, &choice) == "/user/hand/left/input/y/touch");
  CHECK(bind("/user/hand/left/input/y/click", kTouch, &choice) == "/user/hand/left/input/x/click");
  CHECK_FALSE(bind("/user/hand/right/input/thumbstick/click", kTouch, &choice));
  CHECK(bind("/user/hand/left/input/thumbstick/click", kTouch, &choice) == "/user/hand/left/input/thumbstick/click");
  // Choices the controller cannot take keep the default: the other hand's
  // button, a reserved input, a button on the stick, a stick on a button.
  Choice bad;
  bad.rows[0][int(Quest::primary)] = Inputs{"/input/a/click"};
  bad.rows[1][int(Quest::secondary)] = Inputs{"/input/system/click"};
  bad.rows[1][int(Quest::stick)] = Inputs{"/input/a/click"};
  bad.rows[1][int(Quest::trigger)] = Inputs{"/input/thumbstick"};
  bad.rows[0][int(Quest::grip)] = Inputs{"/input/nonexistent/click"};
  const auto r = resolve(profile(kTouch), &bad);
  CHECK(r.rows == resolve(profile(kTouch), nullptr).rows);
  // A grip on the Index's trigger: grip and trigger now share it.
  Choice shared;
  shared.rows[0][int(Quest::grip)] = Inputs{"/input/trigger/value"};
  const auto s = resolve(profile(kIndex), &shared);
  CHECK(bind("/user/hand/left/input/squeeze/value", kIndex, &shared) == "/user/hand/left/input/trigger/value");
  CHECK(conflicts(s, MenuMode::automatic) == std::vector<Conflict>{{0, Quest::menu, Quest::stick_click},
                                                                   {0, Quest::trigger, Quest::grip},
                                                                   {1, Quest::menu, Quest::stick_click}});
}

TEST_CASE("Controls menu choices resolve to a binding or a synthesized press") {
  const auto& index = profile(kIndex);
  Choice c;
  c.menu = {MenuMode::button, "/user/hand/left/input/b/click", {}, 500};
  auto r = resolve(index, &c);
  CHECK(r.menu.kind == Menu::Kind::direct);
  CHECK(r.menu.direct == "/input/b/click");
  CHECK_FALSE(r.menu.automatic);
  CHECK(one_binding("/user/hand/left/input/menu/click", index, r, true) == "/user/hand/left/input/b/click");
  // The left B is now the menu and Y: they share.
  CHECK(conflicts(r, MenuMode::button) == std::vector<Conflict>{{0, Quest::secondary, Quest::menu}});
  // A right-hand button cannot carry the game's left menu binding: synthesized.
  c.menu = {MenuMode::button, "/user/hand/right/input/b/click", {}, 500};
  r = resolve(index, &c);
  CHECK(r.menu.kind == Menu::Kind::synthesized);
  CHECK(r.menu.sources == std::vector<Inputs>{{"/user/hand/right/input/b/click"}});
  CHECK(r.menu.hold_ns == 0);
  CHECK_FALSE(one_binding("/user/hand/left/input/menu/click", index, r, true));
  // Hold: the button reaches the game too, so no conflict is reported.
  c.menu = {MenuMode::hold, "/user/hand/right/input/b/click", {}, 1000};
  r = resolve(index, &c);
  CHECK(r.menu.kind == Menu::Kind::synthesized);
  CHECK(r.menu.hold_ns == 1'000'000'000);
  CHECK(conflicts(r, MenuMode::hold).empty());
  // Two buttons.
  c.menu = {MenuMode::chord, "/user/hand/left/input/thumbstick/click", "/user/hand/right/input/b/click", 2000};
  r = resolve(index, &c);
  CHECK(r.menu.sources.size() == 2);
  CHECK(r.menu.hold_ns == 2'000'000'000);
  CHECK(conflicts(r, MenuMode::chord) ==
        std::vector<Conflict>{{0, Quest::menu, Quest::stick_click}, {1, Quest::secondary, Quest::menu}});
  // Off: no binding, nothing synthesized.
  c.menu = {MenuMode::off, {}, {}, 500};
  r = resolve(index, &c);
  CHECK(r.menu.kind == Menu::Kind::off);
  CHECK_FALSE(one_binding("/user/hand/left/input/menu/click", index, r, true));
  // Unusable menu choices fall back to automatic: the same button twice, a reserved
  // or wrong-hand input, a missing one.
  for (const auto& menu : {MenuChoice{MenuMode::chord, "/user/hand/left/input/a/click", "/user/hand/left/input/a/click", 500},
                           MenuChoice{MenuMode::button, "/user/hand/left/input/system/click", {}, 500},
                           MenuChoice{MenuMode::hold, "", {}, 500}}) {
    c.menu = menu;
    r = resolve(index, &c);
    CHECK(r.menu.automatic);
    CHECK(r.menu.direct == "/input/trackpad/force");
  }
  c.menu = {MenuMode::button, "/user/hand/left/input/a/click", {}, 500};
  CHECK(resolve(profile(kTouch), &c).menu.automatic);
}

TEST_CASE("Controls encode and decode the player's choices") {
  Config config;
  Choice& index = config[std::string(kIndex)];
  index.rows[0][int(Quest::primary)] = Inputs{"/input/b/click"};
  index.rows[1][int(Quest::thumbrest)] = Inputs{};
  index.menu = {MenuMode::chord, "/user/hand/left/input/thumbstick/click", "/user/hand/right/input/b/click", 1000};
  config[std::string(kVive)].menu.mode = MenuMode::off;
  const auto text = encode(config);
  CHECK(decode(text) == config);
  CHECK(text.find("\"stickClick\"") == std::string::npos);  // unset rows are left out
  CHECK(text.find("\"thumbrest\":\"\"") != std::string::npos);
  CHECK(decode("").empty());
  CHECK(decode("not json").empty());
  CHECK(decode("[1,2]").empty());
  // Wrong types are skipped, the rest kept.
  const auto mixed = decode(R"({"a":5,"b":{"left":{"primary":7,"secondary":"/input/x/click"},"menu":{"mode":"bogus","holdMs":"x"}}})");
  REQUIRE(mixed.size() == 1);
  CHECK_FALSE(mixed.at("b").rows[0][int(Quest::primary)]);
  CHECK(mixed.at("b").rows[0][int(Quest::secondary)] == Inputs{"/input/x/click"});
  CHECK(mixed.at("b").menu == MenuChoice{});
  // A saved time stays as saved; a choice saved without one gets its mode's default.
  const auto times = decode(R"({"c":{"menu":{"mode":"chord","holdMs":500}},"d":{"menu":{"mode":"chord"}},)"
                            R"("e":{"menu":{"mode":"hold"}},"f":{"menu":{"mode":"chord","holdMs":0}}})");
  CHECK(times.at("c").menu.hold_ms == 500);
  CHECK(times.at("d").menu.hold_ms == kChordHoldMs);
  CHECK(times.at("e").menu.hold_ms == kHoldMs);
  CHECK(times.at("f").menu.hold_ms == 0);
  CHECK(decode(encode(times)) == times);
}

TEST_CASE("The former stick-click menu setting migrates into the menu choices") {
  CHECK(migrate_menu_button(0).empty());
  CHECK(migrate_menu_button(7).empty());
  // Always: both stick clicks pressed together on every controller with sticks.
  const auto always = migrate_menu_button(1);
  CHECK_FALSE(always.contains(std::string(kVive)));
  for (const auto& [path, choice] : always) {
    CHECK(choice.menu.mode == MenuMode::chord);
    const auto r = resolve(profile(path), &choice);
    CHECK(r.menu.kind == Menu::Kind::synthesized);
    CHECK(r.menu.sources.size() == 2);
    CHECK(r.menu.hold_ns == 0);
  }
  // Off: each controller's own menu input with no fallback; none on a generic controller.
  const auto off = migrate_menu_button(2);
  CHECK(off.size() == std::size(kProfiles));
  for (const auto& [path, choice] : off) {
    const auto r = resolve(profile(path), &choice);
    CHECK_FALSE(r.menu.automatic);
    const auto* own = find_input(profile(path), "/input/menu/click");
    const bool has_button = !profile(path).menu_path.empty() || (own && on_hand(*own, 0));
    CHECK(r.menu.kind == (has_button ? Menu::Kind::direct : Menu::Kind::off));
  }
}

TEST_CASE("Controls rows take several host inputs") {
  constexpr std::string_view kFrame = "/interaction_profiles/valve/frame_controller_valve";
  const auto& frame = profile(kFrame);
  Choice choice;
  // Stick click on the stick and the bumper; repeats, another hand's button, a
  // reserved input and a stick on a button are dropped, the order kept.
  choice.rows[0][int(Quest::stick_click)] =
      Inputs{"/input/thumbstick/click", "/input/bumper/click", "/input/thumbstick/click", "/input/a/click",
             "/input/system/click", "/input/thumbstick"};
  auto r = resolve(frame, &choice);
  CHECK(r.rows[0][int(Quest::stick_click)] == Inputs{"/input/thumbstick/click", "/input/bumper/click"});
  CHECK(host_bindings("/user/hand/left/input/thumbstick/click", frame, r, true) ==
        std::vector<std::string>{"/user/hand/left/input/thumbstick/click", "/user/hand/left/input/bumper/click"});
  // The automatic menu's left stick click is either of them.
  CHECK(r.menu.sources == std::vector<Inputs>{{"/user/hand/left/input/thumbstick/click", "/user/hand/left/input/bumper/click"},
                                              {"/user/hand/right/input/thumbstick/click"}});
  // A row of only unusable inputs keeps the default.
  choice.rows[0][int(Quest::stick_click)] = Inputs{"/input/a/click", "/input/system/click"};
  CHECK(resolve(frame, &choice).rows[0][int(Quest::stick_click)] == Inputs{"/input/thumbstick/click"});
  // The stick row takes only two-axis inputs.
  choice = {};
  choice.rows[1][int(Quest::stick)] = Inputs{"/input/thumbstick", "/input/bumper/click"};
  CHECK(resolve(frame, &choice).rows[1][int(Quest::stick)] == Inputs{"/input/thumbstick"});
  // Touch sensors follow every input of the row that has one.
  const auto& index = profile(kIndex);
  choice = {};
  choice.rows[0][int(Quest::primary)] = Inputs{"/input/a/click", "/input/b/click", "/input/trigger/click"};
  r = resolve(index, &choice);
  CHECK(host_bindings("/user/hand/left/input/x/touch", index, r, true) ==
        std::vector<std::string>{"/user/hand/left/input/a/touch", "/user/hand/left/input/b/touch",
                                 "/user/hand/left/input/trigger/touch"});
  CHECK(host_bindings("/user/hand/left/input/x/click", index, r, true).size() == 3);
}

TEST_CASE("Controls conflicts count every input of a row") {
  Choice choice;
  // The bumper on Stick click and on the grip.
  choice.rows[0][int(Quest::stick_click)] = Inputs{"/input/thumbstick/click", "/input/bumper/click"};
  choice.rows[0][int(Quest::grip)] = Inputs{"/input/squeeze/value", "/input/bumper/click"};
  const auto r = resolve(profile("/interaction_profiles/valve/frame_controller_valve"), &choice);
  CHECK(conflicts(r, MenuMode::automatic) == std::vector<Conflict>{{0, Quest::menu, Quest::stick_click},
                                                                   {0, Quest::menu, Quest::grip},
                                                                   {0, Quest::stick_click, Quest::grip},
                                                                   {1, Quest::menu, Quest::stick_click}});
}

TEST_CASE("Controls trackpad halves in a row with several inputs are left to Venture without dpad binding") {
  const auto& vive = profile(kVive);
  Choice choice;
  // Left X on both trackpad halves and the trigger click.
  choice.rows[0][int(Quest::primary)] = Inputs{"/input/trackpad/dpad_down", "/input/trackpad/dpad_up", "/input/trigger/click"};
  const auto r = resolve(vive, &choice);
  CHECK(host_bindings("/user/hand/left/input/x/click", vive, r, true) ==
        std::vector<std::string>{"/user/hand/left/input/trackpad/dpad_down", "/user/hand/left/input/trackpad/dpad_up",
                                 "/user/hand/left/input/trigger/click"});
  // Without XR_EXT_dpad_binding only the plain button is bound; one pad is read for the halves.
  CHECK(host_bindings("/user/hand/left/input/x/click", vive, r, false) ==
        std::vector<std::string>{"/user/hand/left/input/trigger/click"});
  CHECK(host_bindings("/user/hand/left/input/y/click", vive, r, false).empty());
  CHECK(pads(vive, r, 0) == std::vector<Pad>{{"/input/trackpad", "/input/trackpad/click"}});
}

TEST_CASE("Controls rows with several inputs are saved as lists; single ones stay strings") {
  Config config;
  Choice& frame = config["/interaction_profiles/valve/frame_controller_valve"];
  frame.rows[0][int(Quest::stick_click)] = Inputs{"/input/thumbstick/click", "/input/bumper/click"};
  frame.rows[1][int(Quest::primary)] = Inputs{"/input/b/click"};
  frame.rows[1][int(Quest::thumbrest)] = Inputs{};
  const auto text = encode(config);
  CHECK(text.find(R"("stickClick":["/input/thumbstick/click","/input/bumper/click"])") != std::string::npos);
  CHECK(text.find(R"("primary":"/input/b/click")") != std::string::npos);
  CHECK(text.find(R"("thumbrest":"")") != std::string::npos);
  CHECK(decode(text) == config);
  // Old settings (one string per row) load and save unchanged.
  constexpr std::string_view kOld = R"({"p":{"left":{"primary":"/input/b/click","stickClick":""}}})";
  const auto old = decode(kOld);
  CHECK(old.at("p").rows[0][int(Quest::primary)] == Inputs{"/input/b/click"});
  CHECK(old.at("p").rows[0][int(Quest::stick_click)] == Inputs{});
  CHECK(encode(old) == kOld);
  // Lists: repeats and empty entries dropped, an empty list is none, a one-entry list
  // is that input; a list with a non-string is skipped like any wrong type.
  const auto lists = decode(R"({"p":{"left":{"primary":["/input/a/click","","/input/a/click","/input/b/click"],)"
                            R"("secondary":[],"trigger":["/input/trigger/value"],"grip":["/input/x/click",5]}}})");
  const auto& rows = lists.at("p").rows[0];
  CHECK(rows[int(Quest::primary)] == Inputs{"/input/a/click", "/input/b/click"});
  CHECK(rows[int(Quest::secondary)] == Inputs{});
  CHECK(rows[int(Quest::trigger)] == Inputs{"/input/trigger/value"});
  CHECK_FALSE(rows[int(Quest::grip)]);
}

TEST_CASE("Controls grip toggle is saved only when set, defaults on for wand side grips, and older settings decode without it") {
  Config config;
  config["/interaction_profiles/htc/vive_controller"].grip_toggle = true;
  config["/interaction_profiles/valve/index_controller"].rows[0][int(Quest::primary)] = Inputs{"/input/b/click"};
  const auto text = encode(config);
  CHECK(text.find("\"gripToggle\":true") != std::string::npos);
  CHECK(text.find("gripToggle") == text.rfind("gripToggle"));  // the Index entry has none
  CHECK(decode(text) == config);
  // Settings saved before the option: unset (the controller's default).
  const auto old = decode(R"({"/interaction_profiles/htc/vive_controller":{"menu":{"mode":"off","holdMs":500}}})");
  CHECK_FALSE(old.at("/interaction_profiles/htc/vive_controller").grip_toggle);
  // Not a boolean: ignored.
  CHECK_FALSE(decode(R"({"p":{"gripToggle":1}})").at("p").grip_toggle);
  CHECK_FALSE(decode(R"({"p":{"gripToggle":"yes"}})").at("p").grip_toggle);
  // A choice of only the toggle is still a choice (not the defaults).
  Choice only;
  only.grip_toggle = true;
  CHECK_FALSE(only == Choice{});
  // resolve() carries it; unset follows the controller: on where the grip only clicks.
  const auto& vive = profile("/interaction_profiles/htc/vive_controller");
  const auto& index = profile("/interaction_profiles/valve/index_controller");
  const auto& touch = profile("/interaction_profiles/oculus/touch_controller");
  CHECK(needs_grip_toggle(vive));
  CHECK_FALSE(needs_grip_toggle(index));
  CHECK_FALSE(needs_grip_toggle(touch));
  CHECK(needs_grip_toggle(profile("/interaction_profiles/microsoft/motion_controller")));
  CHECK_FALSE(needs_grip_toggle(profile("/interaction_profiles/htc/vive_cosmos_controller")));
  CHECK(resolve(vive, &only).grip_toggle);
  CHECK(resolve(vive, nullptr).grip_toggle);
  CHECK(resolve(vive, &old.at("/interaction_profiles/htc/vive_controller")).grip_toggle);
  CHECK_FALSE(resolve(index, nullptr).grip_toggle);
  CHECK_FALSE(resolve(touch, nullptr).grip_toggle);
  // Turned off on a click grip: saved as off and stays off.
  Config off;
  off["/interaction_profiles/htc/vive_controller"].grip_toggle = false;
  const auto off_text = encode(off);
  CHECK(off_text.find("\"gripToggle\":false") != std::string::npos);
  CHECK_FALSE(resolve(vive, &decode(off_text).at("/interaction_profiles/htc/vive_controller")).grip_toggle);
}

TEST_CASE("Steam Frame: the bumper uses Valve's path, saved shoulder paths move to it, and Thumbrest can take the stick's touch") {
  constexpr std::string_view kFrame = "/interaction_profiles/valve/frame_controller_valve";
  const auto& frame = profile(kFrame);
  CHECK(find_input(frame, "/input/bumper/click"));
  CHECK_FALSE(find_input(frame, "/input/shoulder/click"));
  const auto config = decode(
      R"({"/interaction_profiles/valve/frame_controller_valve":{"left":{"stickClick":["/input/thumbstick/click","/input/shoulder/click"]},)"
      R"("menu":{"mode":"button","first":"/user/hand/left/input/shoulder/click"}}})");
  const auto& choice = config.at(std::string(kFrame));
  CHECK(choice.rows[0][int(Quest::stick_click)] == Inputs{"/input/thumbstick/click", "/input/bumper/click"});
  CHECK(choice.menu.first == "/user/hand/left/input/bumper/click");
  // Another profile's shoulder (the Cosmos bumper) is left alone.
  const auto cosmos = decode(R"({"/interaction_profiles/htc/vive_cosmos_controller":{"left":{"grip":"/input/shoulder/click"}}})");
  CHECK(cosmos.begin()->second.rows[0][int(Quest::grip)] == Inputs{"/input/shoulder/click"});
  Choice rest;
  for (int hand = 0; hand < 2; ++hand) rest.rows[size_t(hand)][int(Quest::thumbrest)] = Inputs{"/input/thumbstick/touch"};
  const auto r = resolve(frame, &rest);
  CHECK(host_bindings("/user/hand/right/input/thumbrest/touch", frame, r, true) ==
        std::vector<std::string>{"/user/hand/right/input/thumbstick/touch"});
}

TEST_CASE("Controls files round trip one controller") {
  Choice choice;
  choice.rows[1][int(Quest::primary)] = Inputs{"/input/b/click"};
  choice.offsets[0] = {{0, 10, 0}, {-20, 0, 5}};
  choice.grip_toggle = true;
  const auto text = export_file(kIndex, choice);
  CHECK(text.find("ventureControls") != std::string::npos);
  const auto back = import_file(text, kTouch);
  CHECK(back.error.empty());
  CHECK_FALSE(back.steamvr);
  REQUIRE(back.config.size() == 1);
  CHECK(back.config.at(std::string(kIndex)) == choice);
  CHECK_FALSE(import_file("not json", kTouch).error.empty());
  CHECK_FALSE(import_file(R"({"ventureControls":1,"controllers":{"/interaction_profiles/none":{}}})", kTouch).error.empty());
}

TEST_CASE("Controls read a SteamVR binding file made for Venture") {
  // As SteamVR's binding UI saves one for an Index player.
  const auto text = R"({
    "action_manifest_version": 0, "controller_type": "knuckles", "name": "Venture shared",
    "bindings": {"/actions/venture_controls": {
      "sources": [
        {"path": "/user/hand/right/input/a", "mode": "button",
         "inputs": {"click": {"output": "/actions/venture_controls/in/button_y_b"},
                    "touch": {"output": "/actions/venture_controls/in/button_y_b_touch"}}},
        {"path": "/user/hand/right/input/trackpad", "mode": "dpad", "parameters": {"sub_mode": "click"},
         "inputs": {"north": {"output": "/actions/venture_controls/in/button_x_a"}}},
        {"path": "/user/hand/left/input/thumbstick", "mode": "joystick",
         "inputs": {"position": {"output": "/actions/venture_controls/in/thumbstick"},
                    "click": {"output": "/actions/venture_controls/in/menu"}}},
        {"path": "/user/hand/left/input/trigger", "mode": "trigger",
         "inputs": {"pull": {"output": "/actions/venture_controls/in/trigger"},
                    "click": {"output": "/actions/venture_controls/in/trigger_press"}}},
        {"path": "/user/hand/left/input/system", "mode": "button",
         "inputs": {"click": {"output": "/actions/venture_controls/in/button_x_a"}}},
        {"path": "/user/hand/left/input/grip", "mode": "force_sensor",
         "inputs": {"force": {"output": "/actions/other/in/fly"}}}
      ],
      "poses": [{"output": "/actions/venture_controls/in/grip_pose", "path": "/user/hand/left/pose/raw"}]
    }}})";
  const auto imported = import_file(text, kTouch);
  CHECK(imported.error.empty());
  CHECK(imported.steamvr);
  REQUIRE(imported.config.contains(std::string(kIndex)));
  const auto& c = imported.config.at(std::string(kIndex));
  CHECK(c.rows[1][int(Quest::secondary)] == Inputs{"/input/a/click"});
  CHECK(c.rows[1][int(Quest::primary)] == Inputs{"/input/trackpad/dpad_up"});
  CHECK(c.rows[0][int(Quest::stick)] == Inputs{"/input/thumbstick"});
  CHECK(c.rows[0][int(Quest::trigger)] == Inputs{"/input/trigger/value"});  // the click twin folds in
  CHECK(c.menu == MenuChoice{MenuMode::button, "/user/hand/left/input/thumbstick/click", {}, kHoldMs});
  CHECK_FALSE(c.rows[0][int(Quest::primary)]);  // the system button is the runtime's: left as it was
  CHECK(imported.mapped == 6);
  CHECK(imported.skipped == 2);  // the system button and the other action set's output
  // A file for another game, and an unknown controller read for the one shown.
  CHECK_FALSE(import_file(R"({"bindings":{"/actions/main":{"sources":[]}}})", kTouch).error.empty());
  const auto unknown = import_file(R"({"controller_type":"mystery","bindings":{"/actions/venture_controls":{"sources":[
      {"path":"/user/hand/right/input/b","mode":"button","inputs":{"click":{"output":"/actions/venture_controls/in/button_x_a"}}}]}}})",
                                   kTouch);
  REQUIRE(unknown.config.contains(std::string(kTouch)));
  CHECK(unknown.config.at(std::string(kTouch)).rows[1][int(Quest::primary)] == Inputs{"/input/b/click"});
}

TEST_CASE("Controls tuning: sticks, press points, vibration and toggles are saved, limited and read back") {
  Choice c;
  c.tuning[0] = {20, true, false, 30, 0, true, 60};
  c.tuning[1] = {0, false, true, 0, 70, false, 100};
  c.toggles[int(Quest::primary)] = c.toggles[int(Quest::trigger)] = true;
  Config config{{std::string(kIndex), c}};
  const auto back = decode(encode(config));
  REQUIRE(back.contains(std::string(kIndex)));
  CHECK(back.at(std::string(kIndex)) == c);
  // Defaults write nothing; out-of-range values are limited; unknown toggles are ignored.
  CHECK(encode({{std::string(kIndex), Choice{}}}).find("tuning") == std::string::npos);
  const auto odd = decode(R"({"/interaction_profiles/valve/index_controller":{"tuning":{"left":{"deadzone":99,"triggerPress":2,"strength":300}},"toggles":["menu","grip","secondary"]}})");
  const auto& o = odd.at(std::string(kIndex));
  CHECK(o.tuning[0].deadzone == kMaxDeadzone);
  CHECK(o.tuning[0].trigger_press == kMinPress);
  CHECK(o.tuning[0].strength == 100);
  CHECK_FALSE(o.toggles[int(Quest::menu)]);
  CHECK_FALSE(o.toggles[int(Quest::grip)]);  // the grip's toggle is grip_toggle
  CHECK(o.toggles[int(Quest::secondary)]);
  const auto r = resolve(profile(kIndex), &c);
  CHECK(r.tuning[0].trigger_press == 30);
  CHECK(r.toggles[int(Quest::trigger)]);
}

TEST_CASE("Controls tuning: the stick's dead zone stretches the rest, inversion flips, presses hold until a little lower") {
  Tuning t;
  CHECK(shape_stick(t, 0.05f, -0.5f) == std::array<float, 2>{0.05f, -0.5f});
  t.invert_y = true;
  CHECK(shape_stick(t, 0.05f, -0.5f) == std::array<float, 2>{0.05f, 0.5f});
  t = {};
  t.deadzone = 20;
  CHECK(shape_stick(t, 0.1f, 0.1f) == std::array<float, 2>{0, 0});
  const auto full = shape_stick(t, 1, 0);
  CHECK(full[0] == doctest::Approx(1));
  const auto half = shape_stick(t, 0, 0.6f);
  CHECK(half[1] == doctest::Approx(0.5f));
  CHECK_FALSE(analog_press(0.25f, false, 30));
  CHECK(analog_press(0.3f, false, 30));
  CHECK(analog_press(0.22f, true, 30));   // held until below 20 %
  CHECK_FALSE(analog_press(0.19f, true, 30));
  CHECK(analog_press(0.5f, false, 0));    // 0: half way
  CHECK(vibration_scale({0, false, false, 0, 0, true, 40}) == doctest::Approx(0.4f));
  CHECK(vibration_scale({0, false, false, 0, 0, false, 40}) == 0);
}

TEST_CASE("Controls read SteamVR's toggle buttons, click points, dead zone and vibration bindings") {
  const auto imported = import_file(R"({"controller_type":"knuckles","bindings":{"/actions/venture_controls":{
    "sources":[
      {"path":"/user/hand/right/input/a","mode":"toggle_button","inputs":{"click":{"output":"/actions/venture_controls/in/button_x_a"}}},
      {"path":"/user/hand/left/input/trigger","mode":"trigger","parameters":{"click_activate_threshold":"0.25"},
       "inputs":{"pull":{"output":"/actions/venture_controls/in/trigger"}}},
      {"path":"/user/hand/left/input/grip","mode":"toggle_button","inputs":{"click":{"output":"/actions/venture_controls/in/grip_press"}}},
      {"path":"/user/hand/left/input/thumbstick","mode":"joystick","parameters":{"deadzone_pct":"12"},
       "inputs":{"position":{"output":"/actions/venture_controls/in/thumbstick"}}}],
    "haptics":[{"output":"/actions/venture_controls/out/haptic","path":"/user/hand/left/output/haptic"}]}}})",
                                    kTouch);
  CHECK(imported.error.empty());
  REQUIRE(imported.config.contains(std::string(kIndex)));
  const auto& c = imported.config.at(std::string(kIndex));
  CHECK(c.toggles[int(Quest::primary)]);
  CHECK(c.grip_toggle == true);
  CHECK(c.tuning[0].trigger_press == 25);
  CHECK(c.tuning[0].deadzone == 12);
  CHECK(c.tuning[0].vibration);
  CHECK_FALSE(c.tuning[1].vibration);
}

TEST_CASE("Controls name the controller the host runtime's render model shows, among controllers with the same inputs") {
  constexpr std::string_view kPlus = "/interaction_profiles/meta/touch_controller_plus";
  constexpr std::string_view kPro = "/interaction_profiles/facebook/touch_controller_pro";
  // SteamVR reports Touch for every Touch-shaped Quest controller; its drivers
  // (oculus for Link, vrlink for Steam Link) name the model of each one.
  CHECK(evident_profile(kTouch, "oculus_quest_plus_controller_left") == kPlus);
  CHECK(evident_profile(kTouch, "oculus_quest_plus_controller_right") == kPlus);
  CHECK(evident_profile(kTouch, "oculus_quest_pro_controller_right") == kPro);
  // A Quest 2's Touch, or a model no listed controller has: the host's profile.
  CHECK(evident_profile(kTouch, "oculus_quest2_controller_left") == kTouch);
  CHECK(evident_profile(kTouch, "oculus_rifts_controller_left") == kTouch);
  CHECK(evident_profile(kTouch, "") == kTouch);
  // A host that has the controller's own profile keeps it.
  CHECK(evident_profile(kPlus, "oculus_quest_plus_controller_left") == kPlus);
  // Never another layout: a Touch model behind an Index profile (a remapped
  // controller) is reported as the Index profile its inputs are.
  CHECK(evident_profile(kIndex, "oculus_quest_plus_controller_left") == kIndex);
  CHECK(evident_profile(kTouch, "valve_controller_knu_1_0_left") == kTouch);
  // A profile Venture does not list stays what the host said.
  CHECK(evident_profile("/interaction_profiles/ext/hand_interaction_ext", "oculus_quest_plus_controller_left") ==
        "/interaction_profiles/ext/hand_interaction_ext");
  CHECK(evident_profile("", "oculus_quest_plus_controller_left").empty());
}

TEST_CASE("Controls Touch, Touch Plus and Touch Pro share one input list, so evidence can move between them") {
  // The same physical layout: player controls and suggested bindings carry over.
  const auto& touch = profile(kTouch);
  for (auto path : {"/interaction_profiles/meta/touch_controller_plus", "/interaction_profiles/facebook/touch_controller_pro"}) {
    CAPTURE(path);
    CHECK(profile(path).inputs.data() == touch.inputs.data());
    CHECK(profile(path).defaults == touch.defaults);
  }
}
