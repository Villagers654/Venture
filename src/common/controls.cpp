#include "common/controls.h"
#include "common/json.h"
#include <algorithm>

namespace venture::controls {
namespace {
constexpr std::string_view kKeys[kQuestCount]{"primary", "secondary", "menu",  "stick",
                                              "stickClick", "trigger", "grip", "thumbrest"};
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
// The whole pad's press, for a half without XR_EXT_dpad_binding.
std::string pad_press(const Profile& p, std::string_view component) {
  const std::string id(identifier(component));
  for (auto suffix : {"/click", "/force"})
    if (find_input(p, id + suffix)) return id + suffix;
  return {};
}
}  // namespace

std::string_view hand_root(int hand) { return hand ? "/user/hand/right" : "/user/hand/left"; }
std::string_view quest_key(Quest q) { return kKeys[int(q)]; }

const Profile* find_profile(std::string_view path) {
  for (const auto& p : kProfiles)
    if (p.path == path) return &p;
  return nullptr;
}
const HostInput* find_input(const Profile& p, std::string_view path) {
  for (const auto& input : p.inputs)
    if (input.path == path) return &input;
  return nullptr;
}

Resolved resolve(const Profile& p, const Choice* choice) {
  Resolved r;
  for (int hand = 0; hand < 2; ++hand)
    for (int q = 0; q < kQuestCount; ++q) {
      std::string path(p.defaults[size_t(hand)][size_t(q)]);
      if (q == int(Quest::menu)) {
        r.rows[size_t(hand)][size_t(q)] = path;
        continue;
      }
      if (choice && choice->rows[size_t(hand)][size_t(q)]) {
        const auto& chosen = *choice->rows[size_t(hand)][size_t(q)];
        if (chosen.empty() || usable(p, hand, chosen, q == int(Quest::stick))) path = chosen;
      }
      r.rows[size_t(hand)][size_t(q)] = path;
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
      m.automatic = true;
      if (p.menu_path.empty()) {
        m.kind = Menu::Kind::synthesized;
        m.sources = {std::string(hand_root(0)) + "/input/thumbstick/click",
                     std::string(hand_root(1)) + "/input/thumbstick/click"};
        m.hold_ns = std::int64_t(kChordHoldMs) * 1'000'000;
      } else {
        m.kind = Menu::Kind::direct;
        m.direct = p.menu_path;
      }
      break;
    case MenuMode::button:
      if (const auto parts = split(menu.first); parts && parts->first == 0) {
        // A left button is the game's own menu binding moved there.
        m.kind = Menu::Kind::direct;
        m.direct = parts->second;
      } else {
        m.kind = Menu::Kind::synthesized;
        m.sources = {menu.first};
      }
      break;
    case MenuMode::hold:
      m.kind = Menu::Kind::synthesized;
      m.sources = {menu.first};
      m.hold_ns = std::int64_t(hold_ms) * 1'000'000;
      break;
    case MenuMode::chord:
      m.kind = Menu::Kind::synthesized;
      m.sources = {menu.first, menu.second};
      m.hold_ns = std::int64_t(hold_ms) * 1'000'000;
      break;
    case MenuMode::off: m.kind = Menu::Kind::off; break;
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

std::optional<std::string> host_binding(std::string_view guest, const Profile& p, const Resolved& r, bool dpad) {
  const auto parts = split(guest);
  if (!parts) return std::nullopt;
  const auto [hand, c] = *parts;
  const std::string root(hand_root(hand));
  if (c == "/input/grip/pose" || c == "/input/aim/pose" || c == "/output/haptic") return std::string(guest);
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
  else if (c == "/input/trigger/value") q = Quest::trigger;
  else if (c == "/input/trigger/touch") q = Quest::trigger, part = Part::touch;
  else if (c == "/input/squeeze/value") q = Quest::grip;
  else if (c == "/input/thumbrest/touch") q = Quest::thumbrest;
  else return std::nullopt;  // Pro's pinch, force and stylus are not ordinary Touch controls
  if (q == Quest::menu) {
    if (hand != 0 || r.menu.kind != Menu::Kind::direct) return std::nullopt;
    auto path = bindable(root + r.menu.direct, p, dpad);
    return path.empty() ? std::nullopt : std::optional(path);
  }
  const auto& host = r.rows[size_t(hand)][size_t(q)];
  if (host.empty()) return std::nullopt;
  if (is_dpad(host) && !dpad) {
    // The first half on this pad, in row order, takes the pad's press.
    for (int other = 0; other < kQuestCount; ++other) {
      const auto& o = r.rows[size_t(hand)][size_t(other)];
      if (other == int(Quest::menu) || !is_dpad(o) || identifier(o) != identifier(host)) continue;
      if (other != int(q)) return std::nullopt;
      break;
    }
  }
  switch (part) {
    case Part::main: {
      if (is_dpad(host) && !dpad) {
        const auto press = pad_press(p, host);
        return press.empty() ? std::nullopt : std::optional(root + press);
      }
      return root + host;
    }
    case Part::x:
    case Part::y:
      if (!is_axes(host)) return std::nullopt;
      return root + host + (part == Part::x ? "/x" : "/y");
    case Part::touch: {
      if (is_dpad(host)) return std::nullopt;
      const auto id = identifier(host);
      const auto name = id.substr(7);
      for (std::string_view t = p.touches; !t.empty();) {
        const auto space = t.find(' ');
        if (t.substr(0, space) == name) return root + std::string(id) + "/touch";
        if (space == std::string_view::npos) break;
        t.remove_prefix(space + 1);
      }
      return std::nullopt;
    }
  }
  return std::nullopt;
}

std::vector<Conflict> conflicts(const Resolved& r, MenuMode mode) {
  std::vector<Conflict> out;
  for (int hand = 0; hand < 2; ++hand) {
    // Each row's host inputs on this hand; the menu's come from its sources.
    std::array<std::vector<std::string>, kQuestCount> at;
    for (int q = 0; q < kQuestCount; ++q)
      if (q != int(Quest::menu) && !r.rows[size_t(hand)][size_t(q)].empty())
        at[size_t(q)].push_back(r.rows[size_t(hand)][size_t(q)]);
    if (mode != MenuMode::hold) {
      if (r.menu.kind == Menu::Kind::direct && hand == 0) at[size_t(Quest::menu)].push_back(r.menu.direct);
      for (const auto& s : r.menu.sources)
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
      for (int q = 0; q < kQuestCount; ++q)
        if (const auto& row = choice.rows[size_t(hand)][size_t(q)]) rows[std::string(kKeys[q])] = *row;
      if (!rows.empty()) entry[hand ? "right" : "left"] = std::move(rows);
    }
    if (choice.menu != MenuChoice{}) {
      Json menu{{"mode", std::string(kModes[int(choice.menu.mode)])}, {"holdMs", choice.menu.hold_ms}};
      if (!choice.menu.first.empty()) menu["first"] = choice.menu.first;
      if (!choice.menu.second.empty()) menu["second"] = choice.menu.second;
      entry["menu"] = std::move(menu);
    }
    j[profile] = std::move(entry);
  }
  return j.dump();
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
        if (const auto row = rows->find(std::string(kKeys[q])); row != rows->end() && row->is_string() && row->get_ref<const std::string&>().size() < 128)
          choice.rows[size_t(hand)][size_t(q)] = row->get<std::string>();
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
      // The controller's own left menu button (Touch's too, though its default is
      // the stick clicks), else its automatic input, else none.
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
