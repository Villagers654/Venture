#include "common/controls_file.h"
#include "common/json.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

namespace venture::controls {
namespace {
constexpr std::string_view kSet = "/actions/venture_controls";

// SteamVR's controller_type -> interaction profile.
std::string profile_for(std::string_view type, std::string_view fallback) {
  struct Known {
    std::string_view type, profile;
  };
  static constexpr Known kKnown[]{
      {"knuckles", "/interaction_profiles/valve/index_controller"},
      {"vive_controller", "/interaction_profiles/htc/vive_controller"},
      {"vive_cosmos_controller", "/interaction_profiles/htc/vive_cosmos_controller"},
      {"vive_focus3_controller", "/interaction_profiles/htc/vive_focus3_controller"},
      {"holographic_controller", "/interaction_profiles/microsoft/motion_controller"},
      {"hpmotioncontroller", "/interaction_profiles/hp/mixed_reality_controller"},
      {"pico_controller", "/interaction_profiles/bytedance/pico4_controller"},
      {"frame_controller", "/interaction_profiles/valve/frame_controller_valve"},
  };
  for (const auto& k : kKnown)
    if (k.type == type) return std::string(k.profile);
  // Every Touch generation reports oculus_touch: the one the page shows, if it is one.
  if (type == "oculus_touch")
    return fallback.find("touch_controller") != std::string_view::npos ? std::string(fallback)
                                                                        : "/interaction_profiles/oculus/touch_controller";
  return std::string(fallback);
}

// Venture's fixed action -> the Quest row it reads. Touch sensors follow their
// button and poses are not rows: those are known, not skipped.
std::optional<Quest> row_of(std::string_view action, bool& known) {
  struct Row {
    std::string_view action;
    Quest q;
  };
  static constexpr Row kRows[]{
      {"button_x_a", Quest::primary},  {"button_y_b", Quest::secondary},     {"menu", Quest::menu},
      {"thumbstick", Quest::stick},    {"thumbstick_click", Quest::stick_click}, {"trigger", Quest::trigger},
      {"trigger_press", Quest::trigger}, {"grip", Quest::grip},              {"grip_press", Quest::grip},
      {"thumbrest_touch", Quest::thumbrest}, {"thumbstick_touch", Quest::stick_touch},
      {"thumbstick_up", Quest::stick_up},    {"thumbstick_down", Quest::stick_down},
      {"thumbstick_left", Quest::stick_left}, {"thumbstick_right", Quest::stick_right},
      {"button_x_a_touch", Quest::primary_touch}, {"button_y_b_touch", Quest::secondary_touch},
      {"trigger_touch", Quest::trigger_touch},
      {"thumb_proximity", Quest::thumb_proximity}, {"plus_thumb_proximity", Quest::thumb_proximity},
      {"trigger_proximity", Quest::trigger_proximity}, {"plus_trigger_proximity", Quest::trigger_proximity},
      {"pro_trigger_curl", Quest::trigger_curl}, {"plus_trigger_curl", Quest::trigger_curl},
      {"pro_trigger_slide", Quest::trigger_slide}, {"plus_trigger_slide", Quest::trigger_slide},
      {"plus_trigger_force", Quest::trigger_force}, {"pro_thumbrest_force", Quest::thumbrest_force},
      {"pro_stylus_force", Quest::stylus_force},
  };
  known = true;
  for (const auto& r : kRows)
    if (r.action == action) return r.q;
  known = action.ends_with("_touch") || action.ends_with("_pose") || action.ends_with("proximity") ||
          action == "haptic" || action.find("vibration") != std::string_view::npos;
  return std::nullopt;
}

// A SteamVR source component ("joystick", "grip") as the OpenXR identifier.
std::string identifier_of(std::string_view name) {
  if (name == "joystick") return "/input/thumbstick";
  if (name == "grip") return "/input/squeeze";
  if (name == "application_menu") return "/input/menu";
  return "/input/" + std::string(name);
}

// The profile's input for one SteamVR mode input, or "".
std::string host_input(const Profile& p, std::string_view id, std::string_view mode, std::string_view sub) {
  std::vector<std::string> tries;
  const std::string base(id);
  if (mode == "dpad") {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 5> kDirs{
        {{"north", "/dpad_up"}, {"south", "/dpad_down"}, {"east", "/dpad_right"}, {"west", "/dpad_left"}, {"center", "/dpad_center"}}};
    for (const auto& [dir, half] : kDirs)
      if (sub == dir) tries.push_back(base + std::string(half));
  } else if (sub == "position") {
    tries.push_back(base);
  } else if (sub == "touch") {
    tries.push_back(base + "/touch");
  } else if (sub == "click") {
    tries = {base + "/click", base + "/force", base + "/value"};
  } else if (sub == "pull" || sub == "grab" || sub == "force" || sub == "value") {
    tries = {base + "/value", base + "/force", base + "/click"};
  }
  for (const auto& t : tries)
    if (find_input(p, t)) return t;
  return {};
}
}  // namespace

std::string export_file(std::string_view profile, const Choice& choice) {
  Config config;
  config[std::string(profile)] = choice;
  Json j{{"ventureControls", 1}, {"controllers", Json::parse(encode(config))}};
  return j.dump(2) + "\n";
}

Imported import_file(std::string_view text, std::string_view fallback) {
  Imported out;
  const Json j = Json::parse(text, nullptr, false);
  if (!j.is_object()) {
    out.error = "not a controls file";
    return out;
  }
  if (const auto controllers = j.find("controllers"); j.contains("ventureControls") && controllers != j.end()) {
    out.config = decode(controllers->dump());
    std::erase_if(out.config, [](const auto& entry) { return !find_profile(entry.first); });
    if (out.config.empty()) out.error = "no controller Venture knows";
    return out;
  }
  const auto bindings = j.find("bindings");
  if (bindings == j.end() || !bindings->is_object()) {
    out.error = "not a Venture or SteamVR controls file";
    return out;
  }
  out.steamvr = true;
  const auto set = bindings->find(std::string(kSet));
  if (set == bindings->end() || !set->is_object()) {
    out.error = "a SteamVR binding file for another game";
    return out;
  }
  std::string type;
  if (const auto t = j.find("controller_type"); t != j.end() && t->is_string()) type = t->get<std::string>();
  const std::string profile_path = profile_for(type, fallback);
  const Profile* p = find_profile(profile_path);
  if (!p) {
    out.error = "no controller Venture knows";
    return out;
  }
  Choice choice;
  // Rows the file binds start empty: the file decides them, in its order.
  std::array<std::array<Inputs, kQuestCount>, 2> rows{};
  std::array<std::array<bool, kQuestCount>, 2> seen{};
  std::string menu_input;
  const auto sources = set->find("sources");
  if (sources != set->end() && sources->is_array())
    for (const auto& source : *sources) {
      if (!source.is_object()) continue;
      const auto path = source.value("path", std::string());
      const auto mode = source.value("mode", std::string());
      const auto inputs = source.find("inputs");
      int hand = -1;
      if (path.starts_with("/user/hand/left/input/")) hand = 0;
      else if (path.starts_with("/user/hand/right/input/")) hand = 1;
      if (hand < 0 || inputs == source.end() || !inputs->is_object()) {
        ++out.skipped;
        continue;
      }
      const auto id = identifier_of(std::string_view(path).substr(hand ? 23 : 22));
      // The rows this source's buttons (not its touch sensor) go to.
      std::vector<Quest> source_rows;
      for (const auto& [sub, binding] : inputs->items()) {
        if (sub == "touch" || !binding.is_object()) continue;
        const auto output = binding.value("output", std::string());
        bool known = false;
        if (const auto slash = output.rfind('/'); slash != std::string::npos)
          if (const auto q = row_of(std::string_view(output).substr(slash + 1), known)) source_rows.push_back(*q);
      }
      for (const auto& [sub, binding] : inputs->items()) {
        if (!binding.is_object()) continue;
        const auto output = binding.value("output", std::string());
        const auto slash = output.rfind('/');
        if (!output.starts_with(std::string(kSet) + "/in/") || slash == std::string::npos) {
          ++out.skipped;
          continue;
        }
        bool known = false;
        const auto q = row_of(std::string_view(output).substr(slash + 1), known);
        if (!q) {
          if (!known) ++out.skipped;
          continue;
        }
        const auto host = host_input(*p, id, mode, sub);
        // An input's own touch sensor already touches it in the game; a touch row holds other buttons.
        if (*q == Quest::stick_touch && (sub == "touch" || host == "/input/thumbstick/touch")) continue;
        // A button's touch sensor bound to a touch action follows that button's row
        // already (the sensor of that very input, the source's button going to the
        // touch's own row, or a sensor that is no input of its own); any other sensor
        // (a trackpad's touch) is a button there.
        if ((*q == Quest::primary_touch || *q == Quest::secondary_touch || *q == Quest::trigger_touch) && sub == "touch") {
          const Quest base = *q == Quest::primary_touch ? Quest::primary
                             : *q == Quest::secondary_touch ? Quest::secondary
                                                            : Quest::trigger;
          const auto& own = kQuestInputs[size_t(*q)];
          if (!find_input(*p, host) || std::ranges::contains(source_rows, base) ||
              host == (hand ? own.right_path : own.left_path))
            continue;
        }
        // A Pro or Plus sensor bound to its own action reaches the game as the host's
        // own sensor; the row holds other inputs.
        if (is_pro_plus(*q) && !host.empty() && !find_input(*p, host)) continue;
        const auto* input = host.empty() ? nullptr : find_input(*p, host);
        if (!input || input->reserved || !on_hand(*input, hand) || is_axes(host) != (*q == Quest::stick)) {
          ++out.skipped;
          continue;
        }
        // SteamVR's mode settings on this source: a toggle button, a click point, a dead zone.
        if (mode == "toggle_button") {
          if (*q == Quest::grip) choice.grip_toggle = true;
          else if (can_toggle(*q)) choice.toggles[size_t(*q)] = true;
        }
        if (const auto parameters = source.find("parameters"); parameters != source.end() && parameters->is_object()) {
          auto number = [&](const char* key) -> std::optional<double> {
            const auto v = parameters->find(key);
            if (v == parameters->end()) return std::nullopt;
            if (v->is_number()) return v->get<double>();
            if (!v->is_string()) return std::nullopt;
            char* end = nullptr;
            const auto text = v->get<std::string>();
            const double d = std::strtod(text.c_str(), &end);
            return end && *end == 0 && !text.empty() ? std::optional<double>(d) : std::nullopt;
          };
          auto& tuning = choice.tuning[size_t(hand)];
          if (const auto click = number("click_activate_threshold"); click && (*q == Quest::trigger || *q == Quest::grip))
            (*q == Quest::trigger ? tuning.trigger_press : tuning.grip_press) = int(std::lround(*click * 100));
          if (const auto dead = number("deadzone_pct"); dead && *q == Quest::stick) tuning.deadzone = int(std::lround(*dead));
          tuning = clamped(tuning);
        }
        if (*q == Quest::menu) {
          if (menu_input.empty()) menu_input = std::string(hand_root(hand)) + host;
        } else {
          auto& row = rows[size_t(hand)][size_t(*q)];
          seen[size_t(hand)][size_t(*q)] = true;
          if (std::ranges::find(row, host) == row.end()) row.push_back(host);
        }
        ++out.mapped;
      }
    }
  for (int hand = 0; hand < 2; ++hand)
    for (int q = 0; q < kQuestCount; ++q)
      if (seen[size_t(hand)][size_t(q)]) {
        // SteamVR binds a trigger's pull and click to an action and its boolean
        // twin; Venture's row reads both from the value.
        auto row = rows[size_t(hand)][size_t(q)];
        std::erase_if(row, [&](const std::string& h) {
          return h.ends_with("/click") && std::ranges::find(row, h.substr(0, h.size() - 6) + "/value") != row.end();
        });
        choice.rows[size_t(hand)][size_t(q)] = std::move(row);
      }
  if (!menu_input.empty()) choice.menu = {MenuMode::button, menu_input, {}, kHoldMs};
  // A file that binds vibration: a hand it leaves out vibrates no more, as in SteamVR.
  if (const auto haptics = set->find("haptics"); haptics != set->end() && haptics->is_array())
    for (int hand = 0; hand < 2; ++hand) {
      bool bound = false;
      for (const auto& h : *haptics)
        if (h.is_object() && h.value("output", std::string()) == std::string(kSet) + "/out/haptic" &&
            h.value("path", std::string()).starts_with(std::string(hand_root(hand)) + "/"))
          bound = true;
      choice.tuning[size_t(hand)].vibration = bound;
    }
  if (out.mapped == 0) {
    out.error = "no Quest controls in this SteamVR binding file";
    return out;
  }
  out.config[profile_path] = std::move(choice);
  return out;
}

}  // namespace venture::controls
