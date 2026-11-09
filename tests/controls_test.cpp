#include "common/controls.h"
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
                    "/interaction_profiles/hp/mixed_reality_controller", "/interaction_profiles/bytedance/pico4_controller"}) {
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
}

TEST_CASE("Controls trackpad halves fall back to the whole pad without XR_EXT_dpad_binding") {
  // The first half on a pad, in row order, takes the pad's press; the others stay unbound.
  CHECK(bind("/user/hand/left/input/x/click", kVive, nullptr, false) == "/user/hand/left/input/trackpad/click");
  CHECK_FALSE(bind("/user/hand/left/input/y/click", kVive, nullptr, false));
  CHECK_FALSE(bind("/user/hand/left/input/thumbstick/click", kVive, nullptr, false));
  // Index's pad has no click: its press is the force sensor.
  Choice choice;
  choice.rows[1][int(Quest::secondary)] = Inputs{"/input/trackpad/dpad_up"};
  REQUIRE(find_input(profile(kIndex), "/input/trackpad/force"));
  // Not a listed Index input: the choice falls back to the default B.
  CHECK(bind("/user/hand/right/input/b/click", kIndex, &choice, false) == "/user/hand/right/input/b/click");
  // A menu source on a half becomes the pad's press too.
  CHECK(bindable("/user/hand/left/input/trackpad/dpad_up", profile(kVive), false) == "/user/hand/left/input/trackpad/click");
  CHECK(bindable("/user/hand/left/input/trackpad/dpad_up", profile(kVive), true) == "/user/hand/left/input/trackpad/dpad_up");
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
      Inputs{"/input/thumbstick/click", "/input/shoulder/click", "/input/thumbstick/click", "/input/a/click",
             "/input/system/click", "/input/thumbstick"};
  auto r = resolve(frame, &choice);
  CHECK(r.rows[0][int(Quest::stick_click)] == Inputs{"/input/thumbstick/click", "/input/shoulder/click"});
  CHECK(host_bindings("/user/hand/left/input/thumbstick/click", frame, r, true) ==
        std::vector<std::string>{"/user/hand/left/input/thumbstick/click", "/user/hand/left/input/shoulder/click"});
  // The automatic menu's left stick click is either of them.
  CHECK(r.menu.sources == std::vector<Inputs>{{"/user/hand/left/input/thumbstick/click", "/user/hand/left/input/shoulder/click"},
                                              {"/user/hand/right/input/thumbstick/click"}});
  // A row of only unusable inputs keeps the default.
  choice.rows[0][int(Quest::stick_click)] = Inputs{"/input/a/click", "/input/system/click"};
  CHECK(resolve(frame, &choice).rows[0][int(Quest::stick_click)] == Inputs{"/input/thumbstick/click"});
  // The stick row takes only two-axis inputs.
  choice = {};
  choice.rows[1][int(Quest::stick)] = Inputs{"/input/thumbstick", "/input/shoulder/click"};
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
  choice.rows[0][int(Quest::stick_click)] = Inputs{"/input/thumbstick/click", "/input/shoulder/click"};
  choice.rows[0][int(Quest::grip)] = Inputs{"/input/squeeze/value", "/input/shoulder/click"};
  const auto r = resolve(profile("/interaction_profiles/valve/frame_controller_valve"), &choice);
  CHECK(conflicts(r, MenuMode::automatic) == std::vector<Conflict>{{0, Quest::menu, Quest::stick_click},
                                                                   {0, Quest::menu, Quest::grip},
                                                                   {0, Quest::stick_click, Quest::grip},
                                                                   {1, Quest::menu, Quest::stick_click}});
}

TEST_CASE("Controls trackpad halves in a row with several inputs follow the whole-pad fallback") {
  const auto& vive = profile(kVive);
  Choice choice;
  // Left X on both trackpad halves and the trigger click.
  choice.rows[0][int(Quest::primary)] = Inputs{"/input/trackpad/dpad_down", "/input/trackpad/dpad_up", "/input/trigger/click"};
  const auto r = resolve(vive, &choice);
  CHECK(host_bindings("/user/hand/left/input/x/click", vive, r, true) ==
        std::vector<std::string>{"/user/hand/left/input/trackpad/dpad_down", "/user/hand/left/input/trackpad/dpad_up",
                                 "/user/hand/left/input/trigger/click"});
  // Without XR_EXT_dpad_binding the row's first half takes the pad's press, its
  // second half adds nothing and its plain button stays.
  CHECK(host_bindings("/user/hand/left/input/x/click", vive, r, false) ==
        std::vector<std::string>{"/user/hand/left/input/trackpad/click", "/user/hand/left/input/trigger/click"});
  // Y (the top half by default) and Stick click (the center) come later on the same pad: unbound.
  CHECK(host_bindings("/user/hand/left/input/y/click", vive, r, false).empty());
  CHECK(host_bindings("/user/hand/left/input/thumbstick/click", vive, r, false).empty());
  // A half that is not its row's first input still loses to an earlier row's half.
  Choice later;
  later.rows[0][int(Quest::secondary)] = Inputs{"/input/menu/click", "/input/trackpad/dpad_up"};
  const auto l = resolve(vive, &later);
  CHECK(host_bindings("/user/hand/left/input/y/click", vive, l, false) ==
        std::vector<std::string>{"/user/hand/left/input/menu/click"});
  CHECK(host_bindings("/user/hand/left/input/x/click", vive, l, false) ==
        std::vector<std::string>{"/user/hand/left/input/trackpad/click"});
}

TEST_CASE("Controls rows with several inputs are saved as lists; single ones stay strings") {
  Config config;
  Choice& frame = config["/interaction_profiles/valve/frame_controller_valve"];
  frame.rows[0][int(Quest::stick_click)] = Inputs{"/input/thumbstick/click", "/input/shoulder/click"};
  frame.rows[1][int(Quest::primary)] = Inputs{"/input/b/click"};
  frame.rows[1][int(Quest::thumbrest)] = Inputs{};
  const auto text = encode(config);
  CHECK(text.find(R"("stickClick":["/input/thumbstick/click","/input/shoulder/click"])") != std::string::npos);
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
