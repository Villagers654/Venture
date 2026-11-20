#include "common/controls.h"
#include "common/json.h"
#include <algorithm>
#include <cmath>

namespace venture::controls {
namespace {
constexpr std::string_view kKeys[kQuestCount]{"primary",    "secondary", "menu",      "stick",     "stickClick",
                                              "trigger",    "grip",      "thumbrest", "stickTouch", "stickUp",
                                              "stickDown",  "stickLeft", "stickRight", "primaryTouch", "secondaryTouch",
                                              "triggerTouch", "thumbProximity", "triggerProximity", "triggerCurl",
                                              "triggerSlide", "triggerForce", "thumbrestForce", "stylusForce"};
constexpr std::string_view kModes[]{"automatic", "button", "hold", "chord", "off"};

// "/input/trackpad/dpad_up" -> "/input/trackpad"; "/input/a/click" -> "/input/a".
std::string_view identifier(std::string_view component) {
  const auto second = component.find('/', 7);
  return second == std::string_view::npos ? component : component.substr(0, second);
}
// A half of a pad or stick ("/input/trackpad/dpad_up"), not a real d-pad button ("/input/dpad_up/click").
bool is_dpad(std::string_view component) { return component.find("/dpad_", 7) != std::string_view::npos; }
// Splits "/user/hand/left/input/x/click" into its hand and component.
std::optional<std::pair<int, std::string_view>> split(std::string_view path) {
  for (int hand = 0; hand < 2; ++hand) {
    const auto root = hand_root(hand);
    if (path.size() > root.size() && path.starts_with(root) && path[root.size()] == '/')
      return std::pair{hand, path.substr(root.size())};
  }
  return std::nullopt;
}
// A host input a row (or a menu source) may use on `hand`.
bool usable(const Profile& p, int hand, std::string_view component, bool axes) {
  const auto* input = find_input(p, component);
  return input && !input->reserved && on_hand(*input, hand) && is_axes(component) == axes;
}
// A saved row: a string ("" for none) or a list of strings, whose repeats and
// empty entries are dropped. Anything else is not a row.
std::optional<Inputs> decode_row(const Json& row) {
  constexpr std::size_t kMaxPath = 128, kMaxInputs = 16;
  if (row.is_string()) {
    const auto& path = row.get_ref<const std::string&>();
    if (path.size() >= kMaxPath) return std::nullopt;
    return path.empty() ? Inputs{} : Inputs{path};
  }
  if (!row.is_array() || row.size() > kMaxInputs) return std::nullopt;
  Inputs inputs;
  for (const auto& item : row) {
    if (!item.is_string() || item.get_ref<const std::string&>().size() >= kMaxPath) return std::nullopt;
    const auto& path = item.get_ref<const std::string&>();
    if (!path.empty() && std::ranges::find(inputs, path) == inputs.end()) inputs.push_back(path);
  }
  return inputs;
}
// The whole pad's press, for a half without XR_EXT_dpad_binding.
std::string pad_press(const Profile& p, std::string_view component) {
  const std::string id(identifier(component));
  for (auto suffix : {"/click", "/force"})
    if (find_input(p, id + suffix)) return id + suffix;
  return {};
}
Offset clamped(Offset o) {
  for (auto& v : o.position) v = std::isfinite(v) ? std::clamp(v, -kMaxShiftMm, kMaxShiftMm) : 0.f;
  for (auto& v : o.rotation) v = std::isfinite(v) ? std::clamp(v, -kMaxTurnDeg, kMaxTurnDeg) : 0.f;
  return o;
}
}  // namespace

Tuning clamped(Tuning t) {
  t.deadzone = std::clamp(t.deadzone, 0, kMaxDeadzone);
  auto press = [](int v) { return v ? std::clamp(v, kMinPress, kMaxPress) : 0; };
  t.trigger_press = press(t.trigger_press);
  t.grip_press = press(t.grip_press);
  t.strength = std::clamp(t.strength, 0, 100);
  return t;
}
std::array<float, 2> shape_stick(const Tuning& t, float x, float y) {
  if (t.invert_x) x = -x;
  if (t.invert_y) y = -y;
  if (t.deadzone <= 0) return {x, y};
  const float length = std::sqrt(x * x + y * y), dead = float(t.deadzone) / 100.f;
  if (!(length > dead)) return {0, 0};
  // Radial: the direction is kept and the travel past the dead zone fills the whole range.
  const float scale = std::min(1.f, (length - dead) / (1.f - dead)) / length;
  return {x * scale, y * scale};
}
std::array<float, 2> push_stick(float x, float y, bool up, bool down, bool left, bool right) {
  if (!up && !down && !left && !right) return {x, y};
  if (up || down) y = float(int(up) - int(down));
  if (left || right) x = float(int(right) - int(left));
  // A round stick reaches no corner: a diagonal is all the way along it.
  const float length = std::sqrt(x * x + y * y);
  if (length > 1) x /= length, y /= length;
  return {x, y};
}
bool analog_press(float value, bool was, int percent) {
  const float press = float(percent ? percent : 50) / 100.f;
  return was ? value > std::max(press - 0.1f, press * 0.5f) : value >= press;
}

std::string_view hand_root(int hand) { return hand ? "/user/hand/right" : "/user/hand/left"; }
std::string_view quest_key(Quest q) { return kKeys[int(q)]; }

const Profile* find_profile(std::string_view path) {
  for (const auto& p : kProfiles)
    if (p.path == path) return &p;
  return nullptr;
}
std::string_view evident_profile(std::string_view host_profile, std::string_view render_model) {
  const auto* host = find_profile(host_profile);
  if (!host || render_model.empty()) return host_profile;
  for (const auto& p : kProfiles) {
    if (p.render_model.empty() || p.inputs.data() != host->inputs.data() ||
        p.inputs.size() != host->inputs.size())
      continue;
    std::string_view model = render_model;
    if (model.ends_with("_left")) model.remove_suffix(5);
    else if (model.ends_with("_right")) model.remove_suffix(6);
    if (model == p.render_model || render_model == p.render_model) return p.path;
  }
  return host_profile;
}
const HostInput* find_input(const Profile& p, std::string_view path) {
  for (const auto& input : p.inputs)
    if (input.path == path) return &input;
  return nullptr;
}

bool needs_grip_toggle(const Profile& profile) {
  return profile.path == "/interaction_profiles/htc/vive_controller" ||
         profile.path == "/interaction_profiles/microsoft/motion_controller";
}

Resolved resolve(const Profile& p, const Choice* choice) {
  Resolved r;
  for (int hand = 0; hand < 2; ++hand)
    for (int q = 0; q < kQuestCount; ++q) {
      auto& row = r.rows[size_t(hand)][size_t(q)];
      if (const auto path = p.defaults[size_t(hand)][size_t(q)]; !path.empty()) row = {std::string(path)};
      if (q == int(Quest::menu) || !choice || !choice->rows[size_t(hand)][size_t(q)]) continue;
      const auto& chosen = *choice->rows[size_t(hand)][size_t(q)];
      Inputs kept;
      for (const auto& path : chosen)
        if (usable(p, hand, path, q == int(Quest::stick)) && std::ranges::find(kept, path) == kept.end())
          kept.push_back(path);
      if (chosen.empty() || !kept.empty()) row = std::move(kept);
    }
  // The menu.
  auto source = [&](const std::string& path) {
    const auto parts = split(path);
    return parts && usable(p, parts->first, parts->second, false);
  };
  MenuChoice menu = choice ? choice->menu : MenuChoice{};
  const int hold_ms = std::clamp(menu.hold_ms, 0, 10'000);
  switch (menu.mode) {
    case MenuMode::button:
    case MenuMode::hold:
      if (!source(menu.first)) menu.mode = MenuMode::automatic;
      break;
    case MenuMode::chord:
      if (!source(menu.first) || !source(menu.second) || menu.first == menu.second) menu.mode = MenuMode::automatic;
      break;
    default: break;
  }
  auto& m = r.menu;
  switch (menu.mode) {
    case MenuMode::automatic:
      // The controller's menu input where the runtime lets the game have it, and
      // both stick clicks (the Stick click rows, as the player put them) where it
      // does not: the two work side by side, so no runtime is left without a menu.
      m.automatic = true;
      m.kind = p.menu_path.empty() ? Menu::Kind::synthesized : Menu::Kind::direct;
      m.direct = p.menu_path;
      for (int hand = 0; hand < 2; ++hand)
        if (const auto& clicks = r.rows[size_t(hand)][size_t(Quest::stick_click)]; !clicks.empty()) {
          auto& source = m.sources.emplace_back();
          for (const auto& click : clicks) source.push_back(std::string(hand_root(hand)) + click);
        }
      if (m.sources.size() < 2) {
        m.sources.clear();
        if (m.kind == Menu::Kind::synthesized) m.kind = Menu::Kind::off;
      }
      m.hold_ns = std::int64_t(kChordHoldMs) * 1'000'000;
      break;
    case MenuMode::button:
      if (const auto parts = split(menu.first); parts && parts->first == 0) {
        // A left button is the game's own menu binding moved there.
        m.kind = Menu::Kind::direct;
        m.direct = parts->second;
      } else {
        m.kind = Menu::Kind::synthesized;
        m.sources = {{menu.first}};
      }
      break;
    case MenuMode::hold:
      m.kind = Menu::Kind::synthesized;
      m.sources = {{menu.first}};
      m.hold_ns = std::int64_t(hold_ms) * 1'000'000;
      break;
    case MenuMode::chord:
      m.kind = Menu::Kind::synthesized;
      m.sources = {{menu.first}, {menu.second}};
      m.hold_ns = std::int64_t(hold_ms) * 1'000'000;
      break;
    case MenuMode::off: m.kind = Menu::Kind::off; break;
  }
  r.grip_toggle = choice && choice->grip_toggle ? *choice->grip_toggle : needs_grip_toggle(p);
  r.quest_grip = {mirrored(p.quest_grip), p.quest_grip};
  if (choice) {
    for (int hand = 0; hand < 2; ++hand) {
      r.offsets[size_t(hand)] = clamped(choice->offsets[size_t(hand)]);
      r.tuning[size_t(hand)] = clamped(choice->tuning[size_t(hand)]);
    }
    for (int q = 0; q < kQuestCount; ++q)
      r.toggles[size_t(q)] = choice->toggles[size_t(q)] && can_toggle(Quest(q)) && Quest(q) != Quest::grip;
  }
  return r;
}

std::string bindable(std::string_view path, const Profile& p, bool dpad) {
  const auto parts = split(path);
  if (!parts) return {};
  const auto [hand, component] = *parts;
  if (!is_dpad(component) || dpad) return std::string(path);
  const auto press = pad_press(p, component);
  return press.empty() ? std::string() : std::string(hand_root(hand)) + press;
}

std::vector<Pad> pads(const Profile& p, const Resolved& r, int hand) {
  std::vector<Pad> out;
  for (int q = 0; q < kQuestCount; ++q)
    for (const auto& host : r.rows[size_t(hand)][size_t(q)]) {
      if (!is_dpad(host)) continue;
      Pad pad{std::string(identifier(host)), pad_press(p, host)};
      if (pad.press.empty() || std::ranges::find(out, pad) != out.end()) continue;
      out.push_back(std::move(pad));
    }
  return out;
}

bool half_pressed(std::string_view component, float x, float y) {
  const float length = std::sqrt(x * x + y * y);
  if (component.ends_with("/dpad_center")) return length < 0.5f;
  if (length < 0.5f) return false;
  if (component.ends_with("/dpad_up")) return y >= std::abs(x);
  if (component.ends_with("/dpad_down")) return -y >= std::abs(x);
  if (component.ends_with("/dpad_left")) return -x > std::abs(y);
  if (component.ends_with("/dpad_right")) return x > std::abs(y);
  return false;
}

Offset mirrored(const Offset& o) {
  // A mirror across the controller's y-z plane: x flips, and so do turns about y and z.
  return {{-o.position[0], o.position[1], o.position[2]}, {o.rotation[0], -o.rotation[1], -o.rotation[2]}};
}

std::array<float, 4> orientation(const Offset& o) {
  constexpr float kHalfRad = 3.14159265358979f / 360.f;
  auto axis = [&](int i) {
    const float a = o.rotation[size_t(i)] * kHalfRad;
    std::array<float, 4> q{0, 0, 0, std::cos(a)};
    q[size_t(i)] = std::sin(a);
    return q;
  };
  auto mul = [](const std::array<float, 4>& a, const std::array<float, 4>& b) {
    return std::array<float, 4>{a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                                a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                                a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                                a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
  };
  // Turn (y), then tilt (x), then roll (z), each about the controller's own axes.
  return mul(mul(axis(1), axis(0)), axis(2));
}

std::vector<std::string> host_bindings(std::string_view guest, const Profile& p, const Resolved& r, bool dpad) {
  const auto parts = split(guest);
  if (!parts) return {};
  const auto [hand, c] = *parts;
  const std::string root(hand_root(hand));
  if (c == "/input/grip/pose" || c == "/input/aim/pose" || c == "/output/haptic") return {std::string(guest)};
  // The Quest row and which part of it the game reads.
  enum class Part { main, x, y, touch };
  Quest q;
  Part part = Part::main;
  if (c == "/input/x/click" || c == "/input/a/click") q = Quest::primary;
  else if (c == "/input/x/touch" || c == "/input/a/touch") q = Quest::primary, part = Part::touch;
  else if (c == "/input/y/click" || c == "/input/b/click") q = Quest::secondary;
  else if (c == "/input/y/touch" || c == "/input/b/touch") q = Quest::secondary, part = Part::touch;
  else if (c == "/input/menu/click") q = Quest::menu;
  else if (c == "/input/thumbstick") q = Quest::stick;
  else if (c == "/input/thumbstick/x") q = Quest::stick, part = Part::x;
  else if (c == "/input/thumbstick/y") q = Quest::stick, part = Part::y;
  else if (c == "/input/thumbstick/touch") q = Quest::stick, part = Part::touch;
  else if (c == "/input/thumbstick/click") q = Quest::stick_click;
  else if (c == "/input/thumbstick/dpad_up") q = Quest::stick_up;
  else if (c == "/input/thumbstick/dpad_down") q = Quest::stick_down;
  else if (c == "/input/thumbstick/dpad_left") q = Quest::stick_left;
  else if (c == "/input/thumbstick/dpad_right") q = Quest::stick_right;
  // Boolean actions may bind to the parent path of an analog input.
  else if (c == "/input/trigger" || c == "/input/trigger/value") q = Quest::trigger;
  else if (c == "/input/trigger/touch") q = Quest::trigger, part = Part::touch;
  else if (c == "/input/squeeze" || c == "/input/squeeze/value") q = Quest::grip;
  else if (c == "/input/thumbrest/touch") q = Quest::thumbrest;
  // Touch Pro and Touch Plus sensors: their rows' inputs, beside the host's own sensor.
  else if (c == "/input/thumb_fb/proximity_fb" || c == "/input/thumb_meta/proximity_meta") q = Quest::thumb_proximity;
  else if (c == "/input/trigger/proximity_fb" || c == "/input/trigger/proximity_meta") q = Quest::trigger_proximity;
  else if (c == "/input/trigger/curl_fb" || c == "/input/trigger/curl_meta") q = Quest::trigger_curl;
  else if (c == "/input/trigger/slide_fb" || c == "/input/trigger/slide_meta") q = Quest::trigger_slide;
  else if (c == "/input/trigger/force") q = Quest::trigger_force;
  else if (c == "/input/thumbrest/force") q = Quest::thumbrest_force;
  else if (c == "/input/stylus_fb/force") q = Quest::stylus_force;
  else return {};
  if (q == Quest::menu) {
    if (hand != 0 || r.menu.kind != Menu::Kind::direct) return {};
    auto path = bindable(root + r.menu.direct, p, dpad);
    if (path.empty()) return {};
    return {std::move(path)};
  }
  std::vector<std::string> out;
  auto add = [&](std::string path) {
    if (std::ranges::find(out, path) == out.end()) out.push_back(std::move(path));
  };
  for (const auto& host : r.rows[size_t(hand)][size_t(q)]) {
    const bool half = is_dpad(host);
    if (half && !dpad) continue;  // pads(): Venture splits the pad itself
    switch (part) {
      case Part::main: add(root + host); break;
      case Part::x:
      case Part::y:
        if (is_axes(host)) add(root + host + (part == Part::x ? "/x" : "/y"));
        break;
      case Part::touch: {
        if (half) break;
        const auto id = identifier(host);
        const auto name = id.substr(7);
        for (std::string_view t = p.touches; !t.empty();) {
          const auto space = t.find(' ');
          if (t.substr(0, space) == name) {
            add(root + std::string(id) + "/touch");
            break;
          }
          if (space == std::string_view::npos) break;
          t.remove_prefix(space + 1);
        }
        break;
      }
    }
  }
  // A touch: also each button of the input's touch row, and for the stick each
  // button of the directions (a thumb pushing the stick touches it).
  if (part == Part::touch && touch_row(q) != Quest::count) {
    std::vector<Quest> extras{touch_row(q)};
    if (q == Quest::stick) extras.insert(extras.end(), {Quest::stick_up, Quest::stick_down, Quest::stick_left, Quest::stick_right});
    for (const auto extra : extras)
      for (const auto& host : r.rows[size_t(hand)][size_t(extra)])
        if (!is_dpad(host) || dpad) add(root + host);
  }
  return out;
}

std::vector<Conflict> conflicts(const Resolved& r, MenuMode mode) {
  std::vector<Conflict> out;
  for (int hand = 0; hand < 2; ++hand) {
    // Each row's host inputs on this hand; the menu's come from its sources.
    std::array<std::vector<std::string>, kQuestCount> at;
    for (int q = 0; q < kQuestCount; ++q)
      if (q != int(Quest::menu)) at[size_t(q)] = r.rows[size_t(hand)][size_t(q)];
    if (mode != MenuMode::hold) {
      if (r.menu.kind == Menu::Kind::direct && hand == 0) at[size_t(Quest::menu)].push_back(r.menu.direct);
      for (const auto& source : r.menu.sources)
        for (const auto& s : source)
          if (const auto parts = split(s); parts && parts->first == hand)
            at[size_t(Quest::menu)].emplace_back(parts->second);
    }
    for (int a = 0; a < kQuestCount; ++a)
      for (int b = a + 1; b < kQuestCount; ++b)
        if (std::ranges::any_of(at[size_t(a)], [&](const auto& x) { return std::ranges::find(at[size_t(b)], x) != at[size_t(b)].end(); }))
          out.push_back({hand, Quest(a), Quest(b)});
  }
  return out;
}

std::string encode(const Config& config) {
  Json j = Json::object();
  for (const auto& [profile, choice] : config) {
    Json entry = Json::object();
    for (int hand = 0; hand < 2; ++hand) {
      Json rows = Json::object();
      for (int q = 0; q < kQuestCount; ++q) {
        const auto& row = choice.rows[size_t(hand)][size_t(q)];
        if (!row) continue;
        // None or one input stays a string, as settings always had it; several are a list.
        if (row->size() > 1) rows[std::string(kKeys[q])] = *row;
        else rows[std::string(kKeys[q])] = row->empty() ? std::string() : row->front();
      }
      if (!rows.empty()) entry[hand ? "right" : "left"] = std::move(rows);
    }
    if (choice.menu != MenuChoice{}) {
      Json menu{{"mode", std::string(kModes[int(choice.menu.mode)])}, {"holdMs", choice.menu.hold_ms}};
      if (!choice.menu.first.empty()) menu["first"] = choice.menu.first;
      if (!choice.menu.second.empty()) menu["second"] = choice.menu.second;
      entry["menu"] = std::move(menu);
    }
    if (choice.grip_toggle) entry["gripToggle"] = *choice.grip_toggle;
    Json offsets = Json::object();
    for (int hand = 0; hand < 2; ++hand)
      if (const auto& o = choice.offsets[size_t(hand)]; !o.zero())
        offsets[hand ? "right" : "left"] = Json{{"position", o.position}, {"rotation", o.rotation}};
    if (!offsets.empty()) entry["offsets"] = std::move(offsets);
    Json tuning = Json::object();
    for (int hand = 0; hand < 2; ++hand) {
      const auto& t = choice.tuning[size_t(hand)];
      if (t == Tuning{}) continue;
      Json h = Json::object();
      if (t.deadzone) h["deadzone"] = t.deadzone;
      if (t.invert_x) h["invertX"] = true;
      if (t.invert_y) h["invertY"] = true;
      if (t.trigger_press) h["triggerPress"] = t.trigger_press;
      if (t.grip_press) h["gripPress"] = t.grip_press;
      if (!t.vibration) h["vibration"] = false;
      if (t.strength != 100) h["strength"] = t.strength;
      tuning[hand ? "right" : "left"] = std::move(h);
    }
    if (!tuning.empty()) entry["tuning"] = std::move(tuning);
    Json toggles = Json::array();
    for (int q = 0; q < kQuestCount; ++q)
      if (choice.toggles[size_t(q)]) toggles.push_back(std::string(kKeys[q]));
    if (!toggles.empty()) entry["toggles"] = std::move(toggles);
    j[profile] = std::move(entry);
  }
  return j.dump();
}

// Paths an earlier Venture saved that the profile spells differently: the Steam
// Frame's bumper was /input/shoulder/click, which Valve's profile does not have.
static void rename_paths(std::string_view profile, Choice& choice) {
  if (profile != "/interaction_profiles/valve/frame_controller_valve") return;
  const auto fix = [](std::string& path) {
    if (const auto at = path.find("/input/shoulder/"); at != std::string::npos)
      path.replace(at, std::string_view("/input/shoulder/").size(), "/input/bumper/");
  };
  for (auto& hand : choice.rows)
    for (auto& row : hand)
      if (row)
        for (auto& path : *row) fix(path);
  fix(choice.menu.first);
  fix(choice.menu.second);
}

Config decode(std::string_view text) {
  Config config;
  const Json j = Json::parse(text, nullptr, false);
  if (!j.is_object()) return config;
  auto string = [](const Json& o, const char* key) {
    const auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
  };
  for (const auto& [profile, entry] : j.items()) {
    if (!entry.is_object() || profile.size() > 128) continue;
    Choice choice;
    for (int hand = 0; hand < 2; ++hand) {
      const auto rows = entry.find(hand ? "right" : "left");
      if (rows == entry.end() || !rows->is_object()) continue;
      for (int q = 0; q < kQuestCount; ++q)
        if (const auto row = rows->find(std::string(kKeys[q])); row != rows->end())
          if (auto inputs = decode_row(*row)) choice.rows[size_t(hand)][size_t(q)] = std::move(*inputs);
    }
    if (const auto menu = entry.find("menu"); menu != entry.end() && menu->is_object()) {
      const auto mode = string(*menu, "mode");
      for (int m = 0; m < int(std::size(kModes)); ++m)
        if (mode == kModes[m]) choice.menu.mode = MenuMode(m);
      choice.menu.first = string(*menu, "first");
      choice.menu.second = string(*menu, "second");
      if (const auto hold = menu->find("holdMs"); hold != menu->end() && hold->is_number_integer())
        choice.menu.hold_ms = std::clamp(hold->get<int>(), 0, 10'000);
      else if (choice.menu.mode == MenuMode::chord)
        choice.menu.hold_ms = kChordHoldMs;
    }
    if (const auto toggle = entry.find("gripToggle"); toggle != entry.end() && toggle->is_boolean())
      choice.grip_toggle = toggle->get<bool>();
    if (const auto offsets = entry.find("offsets"); offsets != entry.end() && offsets->is_object())
      for (int hand = 0; hand < 2; ++hand) {
        const auto o = offsets->find(hand ? "right" : "left");
        if (o == offsets->end() || !o->is_object()) continue;
        auto triple = [&](const char* key, std::array<float, 3>& out) {
          const auto v = o->find(key);
          if (v == o->end() || !v->is_array() || v->size() != 3) return;
          for (size_t i = 0; i < 3; ++i)
            if ((*v)[i].is_number()) out[i] = (*v)[i].get<float>();
        };
        Offset offset;
        triple("position", offset.position);
        triple("rotation", offset.rotation);
        choice.offsets[size_t(hand)] = clamped(offset);
      }
    if (const auto tuning = entry.find("tuning"); tuning != entry.end() && tuning->is_object())
      for (int hand = 0; hand < 2; ++hand) {
        const auto h = tuning->find(hand ? "right" : "left");
        if (h == tuning->end() || !h->is_object()) continue;
        Tuning t;
        auto number = [&](const char* key, int& out) {
          if (const auto v = h->find(key); v != h->end() && v->is_number()) out = int(std::lround(v->get<double>()));
        };
        auto flag = [&](const char* key, bool& out) {
          if (const auto v = h->find(key); v != h->end() && v->is_boolean()) out = v->get<bool>();
        };
        number("deadzone", t.deadzone);
        flag("invertX", t.invert_x);
        flag("invertY", t.invert_y);
        number("triggerPress", t.trigger_press);
        number("gripPress", t.grip_press);
        flag("vibration", t.vibration);
        number("strength", t.strength);
        choice.tuning[size_t(hand)] = clamped(t);
      }
    if (const auto toggles = entry.find("toggles"); toggles != entry.end() && toggles->is_array())
      for (const auto& key : *toggles)
        for (int q = 0; q < kQuestCount; ++q)
          if (key.is_string() && key.get<std::string>() == kKeys[q] && can_toggle(Quest(q)) && Quest(q) != Quest::grip)
            choice.toggles[size_t(q)] = true;
    rename_paths(profile, choice);
    config[profile] = std::move(choice);
  }
  return config;
}

Config migrate_menu_button(int menu_button) {
  Config config;
  if (menu_button != 1 && menu_button != 2) return config;
  for (const auto& p : kProfiles) {
    Choice choice;
    if (menu_button == 1) {
      choice.menu = {MenuMode::chord, std::string(hand_root(0)) + "/input/thumbstick/click",
                     std::string(hand_root(1)) + "/input/thumbstick/click", kChordHoldMs};
    } else {
      // The controller's own left menu button (without the default's stick
      // clicks beside it), else its automatic input, else none.
      const auto* own = find_input(p, "/input/menu/click");
      const std::string_view button =
          own && !own->reserved && on_hand(*own, 0) ? own->path : p.menu_path;
      if (button.empty()) choice.menu.mode = MenuMode::off;
      else choice.menu = {MenuMode::button, std::string(hand_root(0)) + std::string(button), {}, kHoldMs};
    }
    // A controller whose menu cannot be moved there keeps its default.
    if (menu_button == 1 && !find_input(p, "/input/thumbstick/click")) continue;
    config[std::string(p.path)] = std::move(choice);
  }
  return config;
}

}  // namespace venture::controls
