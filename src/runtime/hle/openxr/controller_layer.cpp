#include "hle/openxr/controller_layer.h"
#include "common/log.h"
#include "core/wait.h"
#include "core/xr_call_trace.h"
#include "xr/bindings.h"
#include "xr/clock.h"
#include "xr/grip_toggle.h"
#include "xr/haptic_output.h"
#include "xr/haptic_sequence.h"
#include "xr/menu_chord.h"
#include "xr/system_keyboard.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

namespace venture::hle::openxr::controls {
namespace {
// ---- The fixed inputs ------------------------------------------------------
// One host action per Touch input, with both hands as subaction paths. Trigger
// and grip have a boolean twin so the host thresholds a press as it does for
// any boolean bound to an analog input.
enum Input : std::uint8_t {
  grip_pose, aim_pose, palm_pose, trigger, trigger_press, trigger_touch, squeeze, squeeze_press,
  stick, stick_click, stick_touch, primary, primary_touch, secondary, secondary_touch, menu,
  thumbrest_touch, haptic, unbound_pose,
  // The player's buttons for the stick's directions, merged into the stick (push_stick).
  stick_up, stick_down, stick_left, stick_right,
  // A pad and its press, read where the host has no XR_EXT_dpad_binding: Venture
  // splits it into the halves the player's controls use (split_pads).
  pad, pad_press,
  // Touch Pro and Touch Plus sensors: bound on the host only where the host
  // controller has them, and only for a game that binds them.
  pro_thumbrest_force, pro_stylus_force, pro_trigger_curl, pro_trigger_slide, pro_thumb_proximity,
  pro_trigger_proximity, pro_haptic_trigger, pro_haptic_thumb, plus_thumb_proximity,
  plus_trigger_proximity, plus_trigger_curl, plus_trigger_slide, plus_trigger_force, input_count
};
// How an input reaches the host. translated: touch_bindings (the player's
// controls); profile: the same path, only on that host profile; palm: the
// host's palm pose (XR_KHR_maintenance1 grip_surface, else XR_EXT_palm_pose).
// pad: the pads controls::pads lists, without XR_EXT_dpad_binding.
enum class Host : std::uint8_t { translated, pro, plus, proximity, palm, pad, none };
struct Fixed {
  const char* name;
  const char* label;
  XrActionType type;
  const char* left;
  const char* right;
  Host host = Host::translated;
};
constexpr XrActionType kBool = XR_ACTION_TYPE_BOOLEAN_INPUT, kFloat = XR_ACTION_TYPE_FLOAT_INPUT,
                       kVector = XR_ACTION_TYPE_VECTOR2F_INPUT, kPose = XR_ACTION_TYPE_POSE_INPUT,
                       kVibration = XR_ACTION_TYPE_VIBRATION_OUTPUT;
constexpr Fixed kInputs[input_count]{
    {"grip_pose", "Grip pose", kPose, "/input/grip/pose", "/input/grip/pose"},
    {"aim_pose", "Aim pose", kPose, "/input/aim/pose", "/input/aim/pose"},
    {"palm_pose", "Palm pose", kPose, "/input/grip_surface/pose", "/input/grip_surface/pose", Host::palm},
    {"trigger", "Trigger", kFloat, "/input/trigger/value", "/input/trigger/value"},
    {"trigger_press", "Trigger press", kBool, "/input/trigger/value", "/input/trigger/value"},
    {"trigger_touch", "Trigger touch", kBool, "/input/trigger/touch", "/input/trigger/touch"},
    {"grip", "Grip", kFloat, "/input/squeeze/value", "/input/squeeze/value"},
    {"grip_press", "Grip press", kBool, "/input/squeeze/value", "/input/squeeze/value"},
    {"thumbstick", "Thumbstick", kVector, "/input/thumbstick", "/input/thumbstick"},
    {"thumbstick_click", "Thumbstick click", kBool, "/input/thumbstick/click", "/input/thumbstick/click"},
    {"thumbstick_touch", "Thumbstick touch", kBool, "/input/thumbstick/touch", "/input/thumbstick/touch"},
    {"button_x_a", "X / A button", kBool, "/input/x/click", "/input/a/click"},
    {"button_x_a_touch", "X / A touch", kBool, "/input/x/touch", "/input/a/touch"},
    {"button_y_b", "Y / B button", kBool, "/input/y/click", "/input/b/click"},
    {"button_y_b_touch", "Y / B touch", kBool, "/input/y/touch", "/input/b/touch"},
    {"menu", "Menu button", kBool, "/input/menu/click", ""},
    {"thumbrest_touch", "Thumb rest touch", kBool, "/input/thumbrest/touch", "/input/thumbrest/touch"},
    {"haptic", "Vibration", kVibration, "/output/haptic", "/output/haptic"},
    {"unbound_pose", "Unbound pose", kPose, "", "", Host::none},
    {"thumbstick_up", "Thumbstick up", kBool, "/input/thumbstick/dpad_up", "/input/thumbstick/dpad_up"},
    {"thumbstick_down", "Thumbstick down", kBool, "/input/thumbstick/dpad_down", "/input/thumbstick/dpad_down"},
    {"thumbstick_left", "Thumbstick left", kBool, "/input/thumbstick/dpad_left", "/input/thumbstick/dpad_left"},
    {"thumbstick_right", "Thumbstick right", kBool, "/input/thumbstick/dpad_right", "/input/thumbstick/dpad_right"},
    {"trackpad", "Trackpad", kVector, "/input/trackpad", "/input/trackpad", Host::pad},
    {"trackpad_press", "Trackpad press", kBool, "/input/trackpad/click", "/input/trackpad/click", Host::pad},
    {"pro_thumbrest_force", "Thumb rest force (Touch Pro)", kFloat, "/input/thumbrest/force", "/input/thumbrest/force", Host::pro},
    {"pro_stylus_force", "Stylus force (Touch Pro)", kFloat, "/input/stylus_fb/force", "/input/stylus_fb/force", Host::pro},
    {"pro_trigger_curl", "Trigger curl (Touch Pro)", kFloat, "/input/trigger/curl_fb", "/input/trigger/curl_fb", Host::pro},
    {"pro_trigger_slide", "Trigger slide (Touch Pro)", kFloat, "/input/trigger/slide_fb", "/input/trigger/slide_fb", Host::pro},
    {"thumb_proximity", "Thumb proximity", kBool, "/input/thumb_fb/proximity_fb", "/input/thumb_fb/proximity_fb", Host::proximity},
    {"trigger_proximity", "Trigger proximity", kBool, "/input/trigger/proximity_fb", "/input/trigger/proximity_fb", Host::proximity},
    {"pro_trigger_vibration", "Trigger vibration (Touch Pro)", kVibration, "/output/haptic_trigger_fb", "/output/haptic_trigger_fb", Host::pro},
    {"pro_thumb_vibration", "Thumb vibration (Touch Pro)", kVibration, "/output/haptic_thumb_fb", "/output/haptic_thumb_fb", Host::pro},
    {"plus_thumb_proximity", "Thumb proximity (Touch Plus)", kBool, "/input/thumb_meta/proximity_meta", "/input/thumb_meta/proximity_meta", Host::plus},
    {"plus_trigger_proximity", "Trigger proximity (Touch Plus)", kBool, "/input/trigger/proximity_meta", "/input/trigger/proximity_meta", Host::plus},
    {"plus_trigger_curl", "Trigger curl (Touch Plus)", kFloat, "/input/trigger/curl_meta", "/input/trigger/curl_meta", Host::plus},
    {"plus_trigger_slide", "Trigger slide (Touch Plus)", kFloat, "/input/trigger/slide_meta", "/input/trigger/slide_meta", Host::plus},
    {"plus_trigger_force", "Trigger force (Touch Plus)", kFloat, "/input/trigger/force", "/input/trigger/force", Host::plus},
};
constexpr std::string_view kTouch = "/interaction_profiles/oculus/touch_controller";
constexpr std::string_view kPlus = "/interaction_profiles/meta/touch_controller_plus";
constexpr std::string_view kPro = "/interaction_profiles/facebook/touch_controller_pro";
constexpr std::string_view kSimple = "/interaction_profiles/khr/simple_controller";
constexpr const char* kHandRoots[2]{"/user/hand/left", "/user/hand/right"};

// The order a Quest 3 answers in for its Touch Plus controllers: Touch Plus where
// the game binds it, else Touch, which every Touch controller also answers to;
// Pro, which a Quest 3's controllers never report, and the simple controller
// last. -1: not a Quest controller profile.
int quest_rank(std::string_view profile) {
  if (profile == kPlus) return 0;
  if (profile == kTouch) return 1;
  if (profile == kPro) return 2;
  if (profile == kSimple) return 3;
  return -1;
}

// ---- Guest objects ------------------------------------------------------------
enum class Part : std::uint8_t { whole, x, y, up, down, left, right };
// One game binding on a fixed input.
struct Source {
  std::uint8_t hand;
  Input input;
  Part part;
  XrPath guest;  // the game's own binding path
  bool operator==(const Source& o) const { return hand == o.hand && input == o.input && part == o.part; }
};
struct GuestSet {
  std::string name, localized;
  std::uint32_t priority = 0;
  XrActionSet host{};  // the game's own set on the host, where one of its actions needs it
  std::vector<XrAction> actions;
};
struct GuestAction {
  XrActionSet set{};
  XrActionType type{};
  std::string name, localized;
  std::vector<XrPath> subactions;
  XrAction host{};  // the game's own action on the host (a profile Venture does not translate)
};
// The game's bindings on one host controller profile.
struct Plan {
  XrPath reported{};          // the profile the game is told is current there
  std::set<XrAction> covered;  // read from the game's own host action (its own bindings for this profile)
  std::map<XrAction, std::vector<Source>> sources;
};
struct InputState {
  bool active = false;
  float value = 0;
  XrVector2f vector{};
  XrTime changed = 0;
};
struct Value {
  bool active = false, changed = false;
  float value = 0;
  XrVector2f vector{};
  XrTime time = 0;
};
// A guest action space: located only while its action is active, as on a Quest,
// at the input the game's bindings give it at that time. The guest handle is a
// host space on the unbound pose; the host spaces it follows are made on first use.
struct ActionSpace {
  XrSession session{};
  XrAction action{};
  XrPath subaction{};
  XrPosef pose{};
  // By hand, fixed pose input and host profile (the player's offset for that controller).
  std::map<std::tuple<std::uint8_t, Input, XrPath>, XrSpace> fixed;
  XrSpace own{};  // on the game's own host action
};

struct Layer {
  XrActionSet set{};
  std::array<XrAction, input_count> host{};
  std::array<XrPath, 2> hands{};
  std::map<XrActionSet, std::unique_ptr<GuestSet>> sets;
  std::map<XrAction, std::unique_ptr<GuestAction>> actions;
  // Each guest suggestion as the game made it, by profile (OpenXR replaces per profile).
  std::map<XrPath, std::vector<XrActionSuggestedBinding>> suggestions;
  std::map<XrPath, std::string> names;  // path strings of the profiles and bindings above
  // Host profiles suggested, and the fixed inputs and hands each binds there.
  std::map<XrPath, std::array<std::uint8_t, input_count>> bound;
  std::set<Input> optional;  // optional inputs the host bindings carry
  bool ever_attached = false;
  bool plans_valid = false;
  std::map<XrPath, Plan> plans;  // by host profile
  Plan quest;                    // a host profile Venture does not target (hand tracking, none)
  std::array<bool, input_count> used{};
  // The session.
  XrSession session{};
  std::array<XrPath, 2> profiles{};
  bool profiles_stale = true;
  std::uint64_t syncs = 0;
  std::array<std::array<InputState, input_count>, 2> inputs{};
  // The player's tuning (shape_inputs): toggles by hand and Quest row, and the
  // trigger and grip pressed at the player's point.
  std::array<std::array<venture::xr::GripLatch, venture::controls::kQuestCount>, 2> toggles{};
  std::array<bool, 2> trigger_down{}, grip_down{};
  std::map<std::pair<XrAction, XrPath>, Value> values;
  std::map<XrSpace, ActionSpace> spaces;
  // The system keyboard's pointers: the host aim pose of each hand.
  std::array<XrSpace, 2> keyboard_aim{};
};
Layer& layer() {
  static Layer value;
  return value;
}
XrInstance instance() { return host_xr().instance; }
const std::string& name_of(XrPath path) {
  auto& names = layer().names;
  if (auto it = names.find(path); it != names.end()) return it->second;
  char text[XR_MAX_PATH_LENGTH]{};
  std::uint32_t count = 0;
  std::string value;
  if (path && XR_SUCCEEDED(::xrPathToString(instance(), path, sizeof(text), &count, text))) value = text;
  return names.emplace(path, std::move(value)).first->second;
}
XrPath to_path(std::string_view text) {
  XrPath path{};
  return XR_SUCCEEDED(::xrStringToPath(instance(), std::string(text).c_str(), &path)) ? path : XR_NULL_PATH;
}
int hand_of(XrPath path) {
  const auto& L = layer();
  if (!path) return -1;
  return path == L.hands[0] ? 0 : path == L.hands[1] ? 1 : -2;
}

// ---- Game bindings on fixed inputs ---------------------------------------------
bool compatible(Input input, Part part, XrActionType type) {
  const auto have = kInputs[input].type;
  if (have == kPose || have == kVibration) return type == have && part == Part::whole;
  if (have == kVector) {
    if (part == Part::whole) return type == kVector;
    if (part == Part::x || part == Part::y) return type == kFloat;
    return type == kBool;
  }
  return part == Part::whole && (type == kBool || type == kFloat);
}
// The fixed input a game binding reaches, by the guest profile it is for.
std::optional<Source> parse(std::string_view profile, std::string_view path, XrActionType type, XrPath guest) {
  int hand = -1;
  for (int h = 0; h < 2; ++h)
    if (path.starts_with(kHandRoots[h]) && path.size() > std::strlen(kHandRoots[h]) &&
        path[std::strlen(kHandRoots[h])] == '/')
      hand = h;
  if (hand < 0) return std::nullopt;
  const auto c = path.substr(std::strlen(kHandRoots[hand]));
  const bool left = hand == 0;
  std::optional<Input> input;
  Part part = Part::whole;
  const bool boolean = type == kBool;
  if (profile == kSimple) {
    if (c == "/input/select/click" || c == "/input/select") input = boolean ? trigger_press : trigger;
    else if ((c == "/input/menu/click" || c == "/input/menu") && left) input = menu;
  } else if (c == "/input/x/click" || c == "/input/x") { if (left) input = primary; }
  else if (c == "/input/a/click" || c == "/input/a") { if (!left) input = primary; }
  else if (c == "/input/y/click" || c == "/input/y") { if (left) input = secondary; }
  else if (c == "/input/b/click" || c == "/input/b") { if (!left) input = secondary; }
  else if (c == "/input/x/touch") { if (left) input = primary_touch; }
  else if (c == "/input/a/touch") { if (!left) input = primary_touch; }
  else if (c == "/input/y/touch") { if (left) input = secondary_touch; }
  else if (c == "/input/b/touch") { if (!left) input = secondary_touch; }
  else if (c == "/input/menu/click" || c == "/input/menu") { if (left) input = menu; }
  else if (c == "/input/trigger/value" || c == "/input/trigger") input = boolean ? trigger_press : trigger;
  else if (c == "/input/trigger/touch") input = trigger_touch;
  else if (c == "/input/squeeze/value" || c == "/input/squeeze") input = boolean ? squeeze_press : squeeze;
  else if (c == "/input/thumbstick") input = type == kVector ? stick : stick_click;
  else if (c == "/input/thumbstick/x") input = stick, part = Part::x;
  else if (c == "/input/thumbstick/y") input = stick, part = Part::y;
  else if (c == "/input/thumbstick/dpad_up") input = stick, part = Part::up;
  else if (c == "/input/thumbstick/dpad_down") input = stick, part = Part::down;
  else if (c == "/input/thumbstick/dpad_left") input = stick, part = Part::left;
  else if (c == "/input/thumbstick/dpad_right") input = stick, part = Part::right;
  else if (c == "/input/thumbstick/click") input = stick_click;
  else if (c == "/input/thumbstick/touch") input = stick_touch;
  else if (c == "/input/thumbrest/touch" || c == "/input/thumbrest") input = thumbrest_touch;
  if (!input) {
    if (c == "/input/grip/pose" || c == "/input/grip") input = grip_pose;
    else if (c == "/input/aim/pose" || c == "/input/aim") input = aim_pose;
    else if (c == "/input/palm_ext/pose" || c == "/input/grip_surface/pose") input = palm_pose;
    else if (c == "/output/haptic") input = haptic;
    else if (profile != kSimple)
      for (int i = pro_thumbrest_force; i < input_count; ++i)
        if (c == kInputs[i].left) input = Input(i);
  }
  if (!input || !compatible(*input, part, type)) return std::nullopt;
  return Source{std::uint8_t(hand), *input, part, guest};
}
// Whether a Touch Pro suggestion binds every action the game's other Touch
// suggestions bind. Then the game handles that controller itself and is told it
// (H3VR2 since 20261115C binds all of its actions on Touch Pro); a partial one, as
// OVRPlugin's alone, is told the Quest 3 profile its bindings are merged under.
bool complete_pro(const std::vector<XrActionSuggestedBinding>& own) {
  std::set<XrAction> mine;
  for (const auto& binding : own) mine.insert(binding.action);
  for (const auto& [profile, bindings] : layer().suggestions) {
    const auto& name = name_of(profile);
    if (name == kPro || !is_touch_profile(name)) continue;
    for (const auto& binding : bindings)
      if (!mine.contains(binding.action)) return false;
  }
  return true;
}
Plan make_plan(std::string_view host, XrPath host_path) {
  auto& L = layer();
  Plan plan;
  std::vector<std::tuple<int, XrPath, const std::vector<XrActionSuggestedBinding>*>> quest;
  for (const auto& [profile, bindings] : L.suggestions)
    if (const int rank = quest_rank(name_of(profile)); rank >= 0) quest.emplace_back(rank, profile, &bindings);
  std::sort(quest.begin(), quest.end(), [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
  std::set<XrAction> covered;
  const auto native = host_path ? L.suggestions.find(host_path) : L.suggestions.end();
  if (native != L.suggestions.end() && quest_rank(host) < 0) {
    // The game's own bindings for the host's controller decide each action they bind.
    plan.reported = host_path;
    for (const auto& binding : native->second) plan.covered.insert(binding.action);
    covered = plan.covered;
  } else if (native != L.suggestions.end() && host == kPro && complete_pro(native->second)) {
    plan.reported = host_path;
    std::stable_partition(quest.begin(), quest.end(), [&](const auto& q) { return std::get<1>(q) == host_path; });
  } else if (!quest.empty()) {
    plan.reported = std::get<1>(quest.front());
  }
  // The first Quest profile decides each action it binds; the others only add the
  // actions it leaves out, so every action stays bound whichever controller the host has.
  for (const auto& [rank, profile, bindings] : quest) {
    std::set<XrAction> mine;
    const auto& profile_name = name_of(profile);
    for (const auto& binding : *bindings) {
      const auto action = L.actions.find(binding.action);
      if (covered.contains(binding.action) || action == L.actions.end()) continue;
      mine.insert(binding.action);
      auto source = parse(profile_name, name_of(binding.binding), action->second->type, binding.binding);
      if (!source) continue;
      auto& list = plan.sources[binding.action];
      if (std::ranges::find(list, *source) == list.end()) list.push_back(*source);
    }
    covered.insert(mine.begin(), mine.end());
  }
  return plan;
}
void build_plans() {
  auto& L = layer();
  if (L.plans_valid) return;
  L.plans.clear();
  L.used = {};
  L.quest = make_plan("", XR_NULL_PATH);
  for (const auto& target : touch_targets()) {
    const auto path = to_path(target);
    if (path) L.plans[path] = make_plan(target, path);
  }
  auto use = [&](const Plan& plan) {
    for (const auto& [action, sources] : plan.sources)
      for (const auto& source : sources) L.used[source.input] = true;
  };
  use(L.quest);
  for (const auto& [path, plan] : L.plans) use(plan);
  L.used[pad] = L.used[pad_press] = !venture::xr::host_dpad();
  // The direction buttons push a stick the game reads.
  for (const auto direction : {stick_up, stick_down, stick_left, stick_right})
    L.used[direction] = L.used[stick];
  // The player's press point reads the analog twin, a toggle the boolean one.
  L.used[trigger] = L.used[trigger_press] = L.used[trigger] || L.used[trigger_press];
  L.used[squeeze] = L.used[squeeze_press] = L.used[squeeze] || L.used[squeeze_press];
  L.plans_valid = true;
}
const Plan& plan_for(int hand) {
  build_plans();
  auto& L = layer();
  const auto it = L.plans.find(L.profiles[std::size_t(hand)]);
  return it == L.plans.end() ? L.quest : it->second;
}
// The optional inputs (palm pose, Pro and Plus sensors) the game's Quest bindings use.
std::set<Input> optional_inputs() {
  std::set<Input> result;
  auto& L = layer();
  for (const auto& [profile, bindings] : L.suggestions) {
    const auto& profile_name = name_of(profile);
    if (quest_rank(profile_name) < 0) continue;
    for (const auto& binding : bindings) {
      const auto action = L.actions.find(binding.action);
      if (action == L.actions.end()) continue;
      if (auto source = parse(profile_name, name_of(binding.binding), action->second->type, binding.binding);
          source && kInputs[source->input].host != Host::translated)
        result.insert(source->input);
    }
  }
  return result;
}

// ---- Host objects -----------------------------------------------------------------
XrResult ensure_fixed() {
  auto& L = layer();
  if (L.set) return XR_SUCCESS;
  for (int hand = 0; hand < 2; ++hand) {
    L.hands[std::size_t(hand)] = to_path(kHandRoots[hand]);
    if (!L.hands[std::size_t(hand)]) return XR_ERROR_PATH_INVALID;
  }
  XrActionSetCreateInfo ci{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strcpy(ci.actionSetName, "venture_controls");
  std::strcpy(ci.localizedActionSetName, "Quest controls");
  ci.priority = 0;
  auto result = venture::xr::host_call<PFN_xrCreateActionSet, "xrCreateActionSet">(instance(), &ci, &L.set);
  if (XR_FAILED(result)) {
    L.set = {};
    log::error("xr", "Controller action set not created: {}", int(result));
    return result;
  }
  for (int i = 0; i < input_count; ++i) {
    XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
    std::strcpy(ai.actionName, kInputs[i].name);
    std::strcpy(ai.localizedActionName, kInputs[i].label);
    ai.actionType = kInputs[i].type;
    ai.countSubactionPaths = 2;
    ai.subactionPaths = L.hands.data();
    result = venture::xr::host_call<PFN_xrCreateAction, "xrCreateAction">(L.set, &ai, &L.host[std::size_t(i)]);
    if (XR_FAILED(result)) {
      log::error("xr", "Controller action {} not created: {}", kInputs[i].name, int(result));
      ::xrDestroyActionSet(L.set);
      L.set = {};
      L.host = {};
      return result;
    }
  }
  venture::xr::register_menu_action(L.set, L.host[menu]);
  venture::xr::register_grip_action(L.host[squeeze], 3);
  venture::xr::register_grip_action(L.host[squeeze_press], 3);
  return XR_SUCCESS;
}
// The game's own host action set and action, made when a profile Venture does not
// translate binds the action.
XrAction own_action(XrAction guest) {
  auto& L = layer();
  const auto it = L.actions.find(guest);
  if (it == L.actions.end()) return XR_NULL_HANDLE;
  auto& action = *it->second;
  if (action.host) return action.host;
  auto& set = *L.sets.at(action.set);
  if (!set.host) {
    XrActionSetCreateInfo ci{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::snprintf(ci.actionSetName, sizeof(ci.actionSetName), "%s", set.name.c_str());
    std::snprintf(ci.localizedActionSetName, sizeof(ci.localizedActionSetName), "%s", set.localized.c_str());
    ci.priority = set.priority;
    if (const auto result = venture::xr::host_call<PFN_xrCreateActionSet, "xrCreateActionSet">(instance(), &ci, &set.host); XR_FAILED(result)) {
      log::warn("xr", "Host refused the game's action set {}: {}", set.name, int(result));
      set.host = {};
      return XR_NULL_HANDLE;
    }
  }
  XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
  std::snprintf(ci.actionName, sizeof(ci.actionName), "%s", action.name.c_str());
  std::snprintf(ci.localizedActionName, sizeof(ci.localizedActionName), "%s", action.localized.c_str());
  ci.actionType = action.type;
  ci.countSubactionPaths = static_cast<std::uint32_t>(action.subactions.size());
  ci.subactionPaths = action.subactions.data();
  if (const auto result = guest::create_action_for_host(set.host, &ci, &action.host); XR_FAILED(result)) {
    log::warn("xr", "Host refused the game's action {}/{}: {}", set.name, action.name, int(result));
    action.host = {};
    return XR_NULL_HANDLE;
  }
  // create_action_for_host records the host action's vibration paths under its handle.
  if (auto host = state().actions.find(action.host); host != state().actions.end()) state().actions.erase(host);
  venture::xr::register_menu_action(set.host, action.host);
  return action.host;
}
std::vector<XrActionSuggestedBinding> fixed_bindings(const std::string& target, bool with_optional) {
  auto& L = layer();
  std::vector<XrActionSuggestedBinding> bindings;
  const auto& extensions = host_xr().enabled_extensions;
  const char* palm = extensions.contains("XR_KHR_maintenance1") ? "/input/grip_surface/pose"
                     : extensions.contains("XR_EXT_palm_pose") ? "/input/palm_ext/pose" : nullptr;
  for (int i = 0; i < input_count; ++i) {
    const auto& fixed = kInputs[i];
    if (fixed.host != Host::translated && fixed.host != Host::pad && !(with_optional && L.optional.contains(Input(i)))) continue;
    for (int hand = 0; hand < 2; ++hand) {
      const char* suffix = hand ? fixed.right : fixed.left;
      if (!*suffix) continue;
      const std::string root = kHandRoots[hand];
      std::vector<std::string> paths;
      switch (fixed.host) {
        case Host::translated: paths = touch_bindings(root + suffix, target); break;
        case Host::pro: if (target == kPro) paths.push_back(root + suffix); break;
        case Host::plus: if (target == kPlus) paths.push_back(root + suffix); break;
        case Host::proximity:
          if (target == kPro || (target == kTouch && extensions.contains("XR_FB_touch_controller_proximity")))
            paths.push_back(root + suffix);
          break;
        case Host::palm: if (palm) paths.push_back(root + palm); break;
        case Host::pad: paths = venture::xr::pad_bindings(target, hand, i == pad_press); break;
        case Host::none: break;
      }
      // A Pro or Plus sensor also takes the inputs of the player's row for it, on any host.
      if (fixed.host == Host::pro || fixed.host == Host::plus || fixed.host == Host::proximity)
        for (auto& path : touch_bindings(root + suffix, target))
          if (std::ranges::find(paths, path) == paths.end()) paths.push_back(std::move(path));
      for (const auto& path : paths)
        if (const auto host = to_path(path)) bindings.push_back({L.host[std::size_t(i)], host});
    }
  }
  return bindings;
}
// What the menu chord and grip toggle know of a host profile: the game's menu and
// stick clicks on the fixed inputs, and the game's own bindings there.
void remember_controls(XrPath target, const std::vector<XrActionSuggestedBinding>& own) {
  auto& L = layer();
  std::vector<XrActionSuggestedBinding> declared = own;
  bool binds_menu = false, binds_stick = false;
  for (const auto& [profile, bindings] : L.suggestions) {
    const auto& profile_name = name_of(profile);
    if (quest_rank(profile_name) < 0) continue;
    for (const auto& binding : bindings) {
      const auto& path = name_of(binding.binding);
      binds_menu |= path == "/user/hand/left/input/menu/click" || path == "/user/hand/left/input/menu";
      binds_stick |= path.ends_with("/input/thumbstick/click");
    }
  }
  if (binds_menu) declared.push_back({L.host[menu], to_path("/user/hand/left/input/menu/click")});
  if (binds_stick) declared.push_back({L.host[stick_click], to_path("/user/hand/left/input/thumbstick/click")});
  const XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr, target,
                                                  static_cast<std::uint32_t>(declared.size()), declared.data()};
  venture::xr::remember_menu_bindings(instance(), target, &info);
  const XrInteractionProfileSuggestedBinding grips{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr, target,
                                                   static_cast<std::uint32_t>(own.size()), own.data()};
  venture::xr::remember_grip_bindings(instance(), target, &grips);
}
// The game's own bindings for host profile `target`, on its own host actions.
std::vector<XrActionSuggestedBinding> own_bindings(XrPath target, const std::string& name) {
  std::vector<XrActionSuggestedBinding> own;
  if (quest_rank(name) >= 0) return own;
  const auto it = layer().suggestions.find(target);
  if (it == layer().suggestions.end()) return own;
  for (const auto& binding : it->second)
    if (const auto host = own_action(binding.action)) own.push_back({host, binding.binding});
  return own;
}
// Suggests one host controller profile: the fixed inputs, and the game's own
// bindings where it binds that profile itself. The result is the host's answer
// for the game's own bindings (success where it has none there).
XrResult suggest_target(XrPath target, const std::string& name) {
  auto& L = layer();
  const auto own = own_bindings(target, name);
  XrResult result = XR_ERROR_PATH_UNSUPPORTED;
  bool own_accepted = false;
  for (int step = 0; step < 3 && XR_FAILED(result); ++step) {
    // A host that refuses an optional input keeps the core ones; one that refuses
    // the game's own bindings keeps the fixed ones (and the game hears why).
    auto bindings = fixed_bindings(name, step == 0);
    if (step < 2) bindings.insert(bindings.end(), own.begin(), own.end());
    else if (own.empty()) break;
    result = venture::xr::suggest_translated(instance(), target, bindings);
    if (XR_SUCCEEDED(result)) {
      own_accepted = step < 2;
      auto& bound = L.bound[target];
      bound = {};
      for (const auto& binding : bindings)
        for (int i = 0; i < input_count; ++i)
          if (binding.action == L.host[std::size_t(i)])
            bound[std::size_t(i)] |= name_of(binding.binding).starts_with("/user/hand/right/") ? 2 : 1;
    }
  }
  if (XR_FAILED(result)) {
    L.bound.erase(target);
    log::warn("xr", "Host refused every binding for {}: {}", name, int(result));
    return result;
  }
  remember_controls(target, own_accepted ? own : std::vector<XrActionSuggestedBinding>{});
  return own.empty() || own_accepted ? XR_SUCCESS : XR_ERROR_PATH_UNSUPPORTED;
}
// Every host controller profile carries the fixed inputs before the first attach.
// The result: success where any target took them.
XrResult suggest_targets(bool all) {
  auto& L = layer();
  if (auto result = ensure_fixed(); XR_FAILED(result)) return result;
  XrResult first = XR_SUCCESS;
  bool accepted = false, any = false;
  for (const auto& target : touch_targets()) {
    const auto path = to_path(target);
    if (!path || (!all && L.bound.contains(path))) continue;
    any = true;
    const auto result = suggest_target(path, target);
    if (target == touch_targets().front()) first = result;
    accepted |= L.bound.contains(path);
  }
  return !any || accepted ? XR_SUCCESS : first;
}
void refresh_menus() {
  for (const auto& [target, bound] : layer().bound) remember_controls(target, own_bindings(target, name_of(target)));
}

// ---- Reading the host -----------------------------------------------------------------
float dpad(XrVector2f v, Part part) {
  const float length = std::sqrt(v.x * v.x + v.y * v.y);
  if (length < 0.5f) return 0;
  switch (part) {
    case Part::up: return v.y >= std::abs(v.x) ? 1.f : 0.f;
    case Part::down: return -v.y >= std::abs(v.x) ? 1.f : 0.f;
    case Part::left: return -v.x > std::abs(v.y) ? 1.f : 0.f;
    case Part::right: return v.x > std::abs(v.y) ? 1.f : 0.f;
    default: return 0;
  }
}
void read_inputs(XrSession session) {
  auto& L = layer();
  for (int hand = 0; hand < 2; ++hand)
    for (int i = 0; i < input_count; ++i) {
      auto& out = L.inputs[std::size_t(hand)][std::size_t(i)];
      out = {};
      if (!L.used[std::size_t(i)]) continue;
      XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, L.host[std::size_t(i)], L.hands[std::size_t(hand)]};
      switch (kInputs[i].type) {
        case kBool: {
          XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
          if (XR_FAILED(::xrGetActionStateBoolean(session, &gi, &s))) break;
          if (i == menu) venture::xr::menu_action_state(session, &gi, &s);
          if (i == squeeze_press) venture::xr::grip_action_state(session, &gi, &s);
          out = {s.isActive == XR_TRUE, s.currentState ? 1.f : 0.f, {}, s.lastChangeTime};
          break;
        }
        case kFloat: {
          XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
          if (XR_FAILED(::xrGetActionStateFloat(session, &gi, &s))) break;
          if (i == squeeze) venture::xr::grip_action_state(session, &gi, &s);
          out = {s.isActive == XR_TRUE, s.currentState, {}, s.lastChangeTime};
          break;
        }
        case kVector: {
          XrActionStateVector2f s{XR_TYPE_ACTION_STATE_VECTOR2F};
          if (XR_FAILED(::xrGetActionStateVector2f(session, &gi, &s))) break;
          out = {s.isActive == XR_TRUE, 0, s.currentState, s.lastChangeTime};
          break;
        }
        case kPose: {
          XrActionStatePose s{XR_TYPE_ACTION_STATE_POSE};
          if (XR_SUCCEEDED(::xrGetActionStatePose(session, &gi, &s))) out.active = s.isActive == XR_TRUE;
          break;
        }
        default: break;
      }
    }
}
// Without XR_EXT_dpad_binding: each row's pad halves pressed where the pad is
// pressed, merged into the row's fixed inputs beside its other host inputs.
void split_pads() {
  if (venture::xr::host_dpad()) return;
  auto& L = layer();
  using venture::controls::Quest;
  for (int hand = 0; hand < 2; ++hand) {
    const auto& in = L.inputs[std::size_t(hand)];
    if (!in[pad].active || !in[pad_press].active) continue;
    const auto& name = name_of(L.profiles[std::size_t(hand)]);
    const auto r = venture::xr::resolved_controls(name);
    if (!r) continue;
    const auto pressed = venture::xr::split_pad(name, hand, in[pad].vector, in[pad_press].value > 0.5f);
    for (int q = 0; q < venture::controls::kQuestCount; ++q) {
      std::vector<Input> targets;
      switch (Quest(q)) {
        case Quest::primary: targets = {primary}; break;
        case Quest::secondary: targets = {secondary}; break;
        case Quest::stick_click: targets = {stick_click}; break;
        case Quest::trigger: targets = {trigger, trigger_press}; break;
        case Quest::grip: targets = {squeeze, squeeze_press}; break;
        case Quest::thumbrest: targets = {thumbrest_touch}; break;
        case Quest::stick_touch: targets = {stick_touch}; break;
        case Quest::stick_up: targets = {stick_up, stick_touch}; break;
        case Quest::stick_down: targets = {stick_down, stick_touch}; break;
        case Quest::stick_left: targets = {stick_left, stick_touch}; break;
        case Quest::stick_right: targets = {stick_right, stick_touch}; break;
        case Quest::primary_touch: targets = {primary_touch}; break;
        case Quest::secondary_touch: targets = {secondary_touch}; break;
        case Quest::trigger_touch: targets = {trigger_touch}; break;
        case Quest::thumb_proximity: targets = {pro_thumb_proximity, plus_thumb_proximity}; break;
        case Quest::trigger_proximity: targets = {pro_trigger_proximity, plus_trigger_proximity}; break;
        case Quest::trigger_curl: targets = {pro_trigger_curl, plus_trigger_curl}; break;
        case Quest::trigger_slide: targets = {pro_trigger_slide, plus_trigger_slide}; break;
        case Quest::trigger_force: targets = {plus_trigger_force}; break;
        case Quest::thumbrest_force: targets = {pro_thumbrest_force}; break;
        case Quest::stylus_force: targets = {pro_stylus_force}; break;
        default: continue;  // the stick takes no half; the menu has its own fallback
      }
      const auto& row = r->rows[std::size_t(hand)][std::size_t(q)];
      if (std::ranges::none_of(row, [](const std::string& h) { return h.find("/dpad_", 7) != std::string::npos; }))
        continue;
      for (const auto target : targets) {
        auto& out = L.inputs[std::size_t(hand)][target];
        out.active = true;
        if (pressed[std::size_t(q)]) out.value = 1.f;
        out.changed = std::max(out.changed, in[pad_press].changed);
      }
    }
  }
}
// The player's tuning of each hand's controller (controls::Tuning) and toggles,
// once per sync, after the host's values and the pad halves are in.
void shape_inputs(XrTime now) {
  auto& L = layer();
  using venture::controls::Quest;
  for (int hand = 0; hand < 2; ++hand) {
    auto& in = L.inputs[std::size_t(hand)];
    const auto r = venture::xr::resolved_controls(name_of(L.profiles[std::size_t(hand)]));
    if (!r) continue;
    const auto& t = r->tuning[std::size_t(hand)];
    if (in[stick].active) {
      const auto v = venture::controls::shape_stick(t, in[stick].vector.x, in[stick].vector.y);
      in[stick].vector = {v[0], v[1]};
    }
    // The direction rows' buttons push the stick all the way (after the dead zone:
    // a button is already all the way). A stick the host leaves unbound becomes
    // active through them.
    auto held = [&](Input direction) { return in[direction].active && in[direction].value > 0.5f; };
    if (std::ranges::any_of(std::array{stick_up, stick_down, stick_left, stick_right}, [&](Input d) { return in[d].active; })) {
      const auto v = venture::controls::push_stick(in[stick].vector.x, in[stick].vector.y, held(stick_up), held(stick_down),
                                                   held(stick_left), held(stick_right));
      in[stick].active = true;
      in[stick].vector = {v[0], v[1]};
      for (const auto d : {stick_up, stick_down, stick_left, stick_right})
        if (in[d].active) in[stick].changed = std::max(in[stick].changed, in[d].changed);
    }
    // A press point of the player's: the boolean twin follows the analog value.
    auto press_at = [&](Input analog, Input boolean, int percent, bool& down) {
      if (!percent || !in[analog].active) return void(down = false);
      const bool pressed = venture::controls::analog_press(in[analog].value, down, percent);
      if (pressed != down) in[boolean].changed = now;
      down = pressed;
      in[boolean].active = true;
      in[boolean].value = pressed ? 1.f : 0.f;
    };
    press_at(trigger, trigger_press, t.trigger_press, L.trigger_down[std::size_t(hand)]);
    // With grip toggle the grip latches on the host's press (grip_toggle.h).
    if (!r->grip_toggle) press_at(squeeze, squeeze_press, t.grip_press, L.grip_down[std::size_t(hand)]);
    for (const auto q : {Quest::primary, Quest::secondary, Quest::stick_click, Quest::trigger}) {
      auto& latch = L.toggles[std::size_t(hand)][std::size_t(q)];
      if (!r->toggles[std::size_t(q)]) {
        latch = {};
        continue;
      }
      const Input button = q == Quest::primary ? primary : q == Quest::secondary ? secondary
                           : q == Quest::stick_click ? stick_click : trigger_press;
      const bool held = latch.update(L.syncs, in[button].value, in[button].active, now);
      for (const auto target : {button, q == Quest::trigger ? trigger : button}) {
        in[target].value = held ? 1.f : 0.f;
        if (latch.time) in[target].changed = latch.time;
      }
    }
  }
}
// OpenXR's combination of several bound inputs: a boolean is pressed by any, a
// float takes the largest magnitude, a vector the longest, a pose any live one.
struct Combined {
  bool active = false;
  float value = 0;
  XrVector2f vector{};
  XrTime changed = 0;
  void add(XrActionType type, float f, XrVector2f v, XrTime time) {
    if (!active || (type == kVector ? v.x * v.x + v.y * v.y > vector.x * vector.x + vector.y * vector.y
                                    : std::abs(f) > std::abs(value))) {
      value = f;
      vector = v;
    }
    active = true;
    changed = std::max(changed, time);
  }
};
// The guest sets the game made active this sync: hands (bit per hand) and priority.
struct ActiveSet {
  unsigned hands = 0;
  std::uint32_t priority = 0;
};
void compute_values(XrSession session, const std::map<XrActionSet, ActiveSet>& active, XrTime now) {
  auto& L = layer();
  // Each fixed input goes to the actions of the highest-priority active sets binding it.
  std::array<std::array<std::int64_t, input_count>, 2> top;
  for (auto& row : top) row.fill(-1);
  for (int hand = 0; hand < 2; ++hand) {
    const auto& plan = plan_for(hand);
    for (const auto& [action, sources] : plan.sources) {
      const auto a = L.actions.find(action);
      if (a == L.actions.end() || plan.covered.contains(action)) continue;
      const auto set = active.find(a->second->set);
      if (set == active.end() || !(set->second.hands & (1u << hand))) continue;
      for (const auto& source : sources)
        if (source.hand == hand)
          top[std::size_t(hand)][source.input] = std::max<std::int64_t>(top[std::size_t(hand)][source.input], set->second.priority);
    }
  }
  for (const auto& [handle, action] : L.actions) {
    const auto set = active.find(action->set);
    std::vector<XrPath> queries{XR_NULL_PATH};
    queries.insert(queries.end(), action->subactions.begin(), action->subactions.end());
    for (const auto query : queries) {
      auto& value = L.values[{handle, query}];
      Combined out;
      if (set != active.end() && action->type != kVibration) {
        const int only = hand_of(query);
        for (int hand = 0; hand < 2; ++hand) {
          if ((only >= 0 && only != hand) || only == -2 || !(set->second.hands & (1u << hand))) continue;
          const auto& plan = plan_for(hand);
          if (plan.covered.contains(handle)) continue;
          const auto sources = plan.sources.find(handle);
          if (sources == plan.sources.end()) continue;
          for (const auto& source : sources->second) {
            if (source.hand != hand) continue;
            if (top[std::size_t(hand)][source.input] > std::int64_t(set->second.priority)) continue;
            const auto& in = L.inputs[std::size_t(hand)][source.input];
            if (!in.active) continue;
            float f = in.value;
            XrVector2f v = in.vector;
            switch (source.part) {
              case Part::x: f = in.vector.x; break;
              case Part::y: f = in.vector.y; break;
              case Part::whole: break;
              default: f = dpad(in.vector, source.part); break;
            }
            if (action->type == kBool) f = f > 0.5f ? 1.f : 0.f;
            out.add(action->type, f, v, in.changed);
          }
        }
        if (action->host) {
          XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, action->host, query};
          switch (action->type) {
            case kBool: {
              XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
              if (XR_SUCCEEDED(::xrGetActionStateBoolean(session, &gi, &s))) {
                venture::xr::menu_action_state(session, &gi, &s);
                venture::xr::grip_action_state(session, &gi, &s);
                if (s.isActive) out.add(kBool, s.currentState ? 1.f : 0.f, {}, s.lastChangeTime);
              }
              break;
            }
            case kFloat: {
              XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
              if (XR_SUCCEEDED(::xrGetActionStateFloat(session, &gi, &s))) {
                venture::xr::grip_action_state(session, &gi, &s);
                if (s.isActive) out.add(kFloat, s.currentState, {}, s.lastChangeTime);
              }
              break;
            }
            case kVector: {
              XrActionStateVector2f s{XR_TYPE_ACTION_STATE_VECTOR2F};
              if (XR_SUCCEEDED(::xrGetActionStateVector2f(session, &gi, &s)) && s.isActive)
                out.add(kVector, 0, s.currentState, s.lastChangeTime);
              break;
            }
            case kPose: {
              XrActionStatePose s{XR_TYPE_ACTION_STATE_POSE};
              if (XR_SUCCEEDED(::xrGetActionStatePose(session, &gi, &s)) && s.isActive) out.add(kPose, 0, {}, 0);
              break;
            }
            default: break;
          }
        }
      }
      Value next;
      next.active = out.active;
      next.value = out.value;
      next.vector = out.vector;
      // OpenXR: becoming active is not a change.
      next.changed = next.active && value.active && action->type != kPose &&
                     (next.value != value.value || next.vector.x != value.vector.x || next.vector.y != value.vector.y);
      if (!next.active) next.time = 0;
      else if (next.changed) next.time = out.changed > value.time ? out.changed : now;
      else next.time = value.active ? value.time : (out.changed ? out.changed : now);
      value = next;
    }
  }
}
void refresh_profiles(XrSession session) {
  auto& L = layer();
  for (int hand = 0; hand < 2; ++hand) {
    XrInteractionProfileState profile{XR_TYPE_INTERACTION_PROFILE_STATE};
    L.profiles[std::size_t(hand)] =
        XR_SUCCEEDED(::xrGetCurrentInteractionProfile(session, L.hands[std::size_t(hand)], &profile))
            ? profile.interactionProfile : XR_NULL_PATH;
  }
}
const Value* stored(XrSession session, const XrActionStateGetInfo* info, XrActionType type, XrResult& result) {
  auto& L = layer();
  result = XR_SUCCESS;
  if (!valid_handle(session, "XrSession")) return result = XR_ERROR_HANDLE_INVALID, nullptr;
  if (!info || XR_FAILED(validate_struct(info, XR_TYPE_ACTION_STATE_GET_INFO))) return result = XR_ERROR_VALIDATION_FAILURE, nullptr;
  const auto action = L.actions.find(info->action);
  if (!valid_handle(info->action, "XrAction") || action == L.actions.end()) return result = XR_ERROR_HANDLE_INVALID, nullptr;
  if (action->second->type != type) return result = XR_ERROR_ACTION_TYPE_MISMATCH, nullptr;
  if (!state().attached_sets[session].contains(key(action->second->set))) return result = XR_ERROR_ACTIONSET_NOT_ATTACHED, nullptr;
  // An action declared without subaction paths, read with one: the core
  // specification answers XR_ERROR_PATH_UNSUPPORTED, but Quest's runtime gives
  // the action's state, and games written for it (CitraVR's per-hand triggers)
  // stop on the error. Such a read gets the action's state, as there.
  auto subaction = info->subactionPath;
  if (subaction && action->second->subactions.empty()) {
    static bool reported = false;
    if (!std::exchange(reported, true))
      log::info("xr", "Action {} has no subaction paths and is read with {}; answering its state, as Quest's runtime does",
                action->second->name, name_of(subaction));
    subaction = XR_NULL_PATH;
  }
  if (subaction && std::ranges::find(action->second->subactions, subaction) == action->second->subactions.end())
    return result = XR_ERROR_PATH_UNSUPPORTED, nullptr;
  static const Value inactive{};
  const auto it = L.values.find({info->action, subaction});
  return it == L.values.end() ? &inactive : &it->second;
}
bool valid_name(std::string_view name) {
  if (name.empty()) return false;
  for (char c : name)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return false;
  return true;
}
std::string bounded(const char* text, std::size_t size) {
  return std::string(text, std::find(text, text + size, '\0'));
}
void forget_action(XrAction handle) {
  auto& L = layer();
  const auto it = L.actions.find(handle);
  if (it == L.actions.end()) return;
  auto& action = *it->second;
  if (action.type == kVibration) {
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, handle, XR_NULL_PATH};
    for (const auto& target : haptic_targets(info)) venture::xr::cancel_sequence(session().handle, &target);
  }
  if (action.host) {
    venture::xr::forget_menu_action(action.host);
    venture::xr::forget_grip_action(action.host);
    ::xrDestroyAction(action.host);
  }
  if (auto set = L.sets.find(action.set); set != L.sets.end()) std::erase(set->second->actions, handle);
  std::erase_if(L.values, [&](const auto& entry) { return entry.first.first == handle; });
  for (auto& [profile, bindings] : L.suggestions)
    std::erase_if(bindings, [&](const XrActionSuggestedBinding& b) { return b.action == handle; });
  L.actions.erase(it);
  L.plans_valid = false;
}
}  // namespace

// ---- Helpers for haptics, spaces and events ---------------------------------------------
std::vector<XrHapticActionInfo> haptic_targets(const XrHapticActionInfo& info) {
  auto& L = layer();
  std::vector<XrHapticActionInfo> targets;
  const auto action = L.actions.find(info.action);
  if (action == L.actions.end() || !L.set) return targets;
  const int only = hand_of(info.subactionPath);
  std::array<unsigned, input_count> hands{};
  for (int hand = 0; hand < 2; ++hand) {
    if ((only >= 0 && only != hand) || only == -2) continue;
    const auto& plan = plan_for(hand);
    if (plan.covered.contains(info.action)) continue;
    if (auto sources = plan.sources.find(info.action); sources != plan.sources.end())
      for (const auto& source : sources->second)
        if (source.hand == hand && kInputs[source.input].type == kVibration) hands[source.input] |= 1u << hand;
  }
  for (int i = 0; i < input_count; ++i) {
    if (!hands[std::size_t(i)]) continue;
    XrHapticActionInfo target{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, L.host[std::size_t(i)], XR_NULL_PATH};
    if (hands[std::size_t(i)] == 3 && only < 0) {
      targets.push_back(target);
      continue;
    }
    for (int hand = 0; hand < 2; ++hand)
      if (hands[std::size_t(i)] & (1u << hand)) {
        target.subactionPath = L.hands[std::size_t(hand)];
        targets.push_back(target);
      }
  }
  if (action->second->host) targets.push_back({XR_TYPE_HAPTIC_ACTION_INFO, nullptr, action->second->host, info.subactionPath});
  return targets;
}
bool haptic_suggested(const XrHapticActionInfo& info) {
  build_plans();
  const auto& L = layer();
  const int only = hand_of(info.subactionPath);
  if (auto sources = L.quest.sources.find(info.action); sources != L.quest.sources.end())
    for (const auto& source : sources->second)
      if (kInputs[source.input].type == kVibration && (only < 0 || source.hand == only)) return true;
  for (const auto& [profile, bindings] : L.suggestions)
    if (quest_rank(name_of(profile)) < 0)
      for (const auto& binding : bindings)
        if (binding.action == info.action) return true;
  return false;
}
XrResult create_action_space(XrSession session, const XrActionSpaceCreateInfo* info, XrSpace* out) {
  auto& L = layer();
  const auto it = L.actions.find(info->action);
  if (!valid_handle(info->action, "XrAction") || it == L.actions.end()) return XR_ERROR_HANDLE_INVALID;
  const auto& action = *it->second;
  if (action.type != kPose) return XR_ERROR_ACTION_TYPE_MISMATCH;
  if (info->subactionPath && std::ranges::find(action.subactions, info->subactionPath) == action.subactions.end())
    return XR_ERROR_PATH_UNSUPPORTED;
  if (auto result = ensure_fixed(); XR_FAILED(result)) return result;
  XrActionSpaceCreateInfo ci = *info;
  ci.next = nullptr;
  ci.action = L.host[unbound_pose];
  const int only = hand_of(info->subactionPath);
  ci.subactionPath = only >= 0 ? L.hands[std::size_t(only)] : XR_NULL_PATH;
  // Bound to nothing until the game's bindings give it an input: never located, as on a Quest.
  if (auto result = ::xrCreateActionSpace(session, &ci, out); XR_FAILED(result)) return result;
  L.spaces[*out] = {session, info->action, info->subactionPath, info->poseInActionSpace, {}, {}};
  return XR_SUCCESS;
}
XrSpace located_space(XrSpace space) {
  auto& L = layer();
  const auto it = L.spaces.find(space);
  if (it == L.spaces.end()) return space;
  auto& entry = it->second;
  // Not active since the last sync (its set not synced, nothing bound): not located.
  const auto value = L.values.find({entry.action, entry.subaction});
  if (value == L.values.end() || !value->second.active) return space;  // its own host space is never located
  const auto action = L.actions.find(entry.action);
  if (action == L.actions.end()) return space;
  auto make = [&](XrAction host, XrPath subaction, XrSpace& out, int hand = -1, bool grip = false) {
    if (out) return out;
    const XrPosef pose =
        hand >= 0 ? venture::xr::offset_pose(name_of(L.profiles[std::size_t(hand)]), hand, entry.pose, grip)
                  : entry.pose;
    XrActionSpaceCreateInfo ci{XR_TYPE_ACTION_SPACE_CREATE_INFO, nullptr, host, subaction, pose};
    if (XR_FAILED(::xrCreateActionSpace(entry.session, &ci, &out))) out = {};
    return out;
  };
  const int only = hand_of(entry.subaction);
  for (int hand = 0; hand < 2; ++hand) {
    if (only == -2 || (only >= 0 && only != hand)) continue;
    const auto& plan = plan_for(hand);
    if (plan.covered.contains(entry.action)) continue;  // read from the game's own host action
    const auto sources = plan.sources.find(entry.action);
    if (sources == plan.sources.end()) continue;
    for (const auto& source : sources->second)
      if (source.hand == hand && kInputs[source.input].type == kPose && L.inputs[std::size_t(hand)][source.input].active)
        if (auto found = make(L.host[source.input], L.hands[std::size_t(hand)],
                              entry.fixed[{source.hand, source.input, L.profiles[std::size_t(hand)]}], hand,
                              source.input == grip_pose))
          return found;
  }
  if (action->second->host)
    if (auto found = make(action->second->host, entry.subaction, entry.own)) return found;
  return space;
}
void destroy_space(XrSpace space) {
  auto& L = layer();
  if (auto it = L.spaces.find(space); it != L.spaces.end()) {
    for (auto& [key, host] : it->second.fixed)
      if (host) ::xrDestroySpace(host);
    if (it->second.own) ::xrDestroySpace(it->second.own);
    L.spaces.erase(it);
  }
}
void profiles_changed() { layer().profiles_stale = true; }
void reset_session(XrSession session) {
  auto& L = layer();
  // The session's action spaces go with it, whether or not it attached actions.
  for (auto it = L.spaces.begin(); it != L.spaces.end();) {
    if (it->second.session != session) {
      ++it;
      continue;
    }
    for (auto& [key, host] : it->second.fixed)
      if (host) ::xrDestroySpace(host);
    if (it->second.own) ::xrDestroySpace(it->second.own);
    it = L.spaces.erase(it);
  }
  if (L.session != session) return;
  for (auto& space : L.keyboard_aim)
    if (space) ::xrDestroySpace(std::exchange(space, XrSpace{}));
  L.session = {};
  L.values.clear();
  L.inputs = {};
  L.profiles = {};
  L.profiles_stale = true;
}
XrResult destroy_instance() {
  auto& L = layer();
  for (auto& [handle, set] : L.sets)
    if (set->host) {
      venture::xr::forget_menu_action_set(set->host);
      ::xrDestroyActionSet(set->host);
    }
  if (L.set) {
    venture::xr::forget_menu_action_set(L.set);
    venture::xr::forget_grip_action(L.host[squeeze]);
    venture::xr::forget_grip_action(L.host[squeeze_press]);
    ::xrDestroyActionSet(L.set);
  }
  L = {};
  return XR_SUCCESS;
}
}  // namespace venture::hle::openxr::controls

// ---- Guest entry points -----------------------------------------------------------------
namespace venture::hle::openxr::guest {
using namespace venture::hle::openxr::controls;
using controls::layer;
XrResult xrCreateActionSet(XrInstance instance, const XrActionSetCreateInfo* info, XrActionSet* out) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(instance, "XrInstance")) return XR_ERROR_HANDLE_INVALID;
  if (!info || XR_FAILED(validate_struct(info, XR_TYPE_ACTION_SET_CREATE_INFO)) || !out) return XR_ERROR_VALIDATION_FAILURE;
  auto& L = layer();
  const auto name = bounded(info->actionSetName, sizeof(info->actionSetName));
  const auto localized = bounded(info->localizedActionSetName, sizeof(info->localizedActionSetName));
  if (name.empty()) return XR_ERROR_NAME_INVALID;
  if (!valid_name(name)) return XR_ERROR_PATH_FORMAT_INVALID;
  if (localized.empty()) return XR_ERROR_LOCALIZED_NAME_INVALID;
  for (const auto& [handle, set] : L.sets) {
    // A repeated display name is let through: it never reaches the host, and
    // a game that repeats one ran on the runtimes that accepted it.
    if (set->name == name) return XR_ERROR_NAME_DUPLICATED;
  }
  if (auto result = ensure_fixed(); XR_FAILED(result)) return result;
  auto record = std::make_unique<GuestSet>();
  record->name = name;
  record->localized = localized;
  record->priority = info->priority;
  *out = reinterpret_cast<XrActionSet>(record.get());
  L.sets[*out] = std::move(record);
  track_handle(*out, "XrActionSet", instance);
  return XR_SUCCESS;
}
XrResult xrDestroyActionSet(XrActionSet handle) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(handle, "XrActionSet")) return XR_ERROR_HANDLE_INVALID;
  venture::xr::session().finish_host_end();
  auto& L = layer();
  if (const auto it = L.sets.find(handle); it != L.sets.end()) {
    for (const auto action : std::vector<XrAction>(it->second->actions)) forget_action(action);
    if (it->second->host) {
      venture::xr::forget_menu_action_set(it->second->host);
      ::xrDestroyActionSet(it->second->host);
    }
    L.sets.erase(it);
  }
  forget_handle(handle);
  return XR_SUCCESS;
}
XrResult xrCreateAction(XrActionSet set, const XrActionCreateInfo* info, XrAction* out) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(set, "XrActionSet")) return XR_ERROR_HANDLE_INVALID;
  if (!info || XR_FAILED(validate_struct(info, XR_TYPE_ACTION_CREATE_INFO)) || !out) return XR_ERROR_VALIDATION_FAILURE;
  if (info->countSubactionPaths && !info->subactionPaths) return XR_ERROR_VALIDATION_FAILURE;
  auto& L = layer();
  const auto parent = L.sets.find(set);
  if (parent == L.sets.end()) return XR_ERROR_HANDLE_INVALID;
  for (const auto& [session, sets] : state().attached_sets)
    if (sets.contains(key(set))) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
  switch (info->actionType) {
    case kBool: case kFloat: case kVector: case kPose: case kVibration: break;
    default: return XR_ERROR_VALIDATION_FAILURE;
  }
  const auto name = bounded(info->actionName, sizeof(info->actionName));
  const auto localized = bounded(info->localizedActionName, sizeof(info->localizedActionName));
  if (name.empty()) return XR_ERROR_NAME_INVALID;
  if (!valid_name(name)) return XR_ERROR_PATH_FORMAT_INVALID;
  if (localized.empty()) return XR_ERROR_LOCALIZED_NAME_INVALID;
  for (const auto action : parent->second->actions) {
    if (L.actions.at(action)->name == name) return XR_ERROR_NAME_DUPLICATED;  // display names may repeat
  }
  std::vector<XrPath> subactions;
  for (std::uint32_t i = 0; i < info->countSubactionPaths; ++i) {
    const auto path = info->subactionPaths[i];
    const auto& text = name_of(path);
    if (text.empty()) return XR_ERROR_PATH_INVALID;
    // A user path, each once.
    if (!text.starts_with("/user/") || text.find("/input/") != std::string::npos ||
        text.find("/output/") != std::string::npos || std::ranges::find(subactions, path) != subactions.end())
      return XR_ERROR_PATH_UNSUPPORTED;
    subactions.push_back(path);
  }
  auto record = std::make_unique<GuestAction>();
  record->set = set;
  record->type = info->actionType;
  record->name = name;
  record->localized = localized;
  record->subactions = subactions;
  *out = reinterpret_cast<XrAction>(record.get());
  L.actions[*out] = std::move(record);
  parent->second->actions.push_back(*out);
  track_handle(*out, "XrAction", set);
  auto& entry = state().actions[*out];
  entry.type = info->actionType;
  entry.subactions = subactions;
  if (info->actionType == kVibration && subactions.empty()) {
    // Mobile SDKs use hand-specific output on an aggregate vibration action.
    for (const char* root : {"/user/head", "/user/hand/left", "/user/hand/right", "/user/gamepad"})
      if (const auto path = to_path(root)) entry.native_haptic_subactions.push_back(path);
  }
  L.plans_valid = false;
  return XR_SUCCESS;
}
XrResult xrDestroyAction(XrAction handle) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(handle, "XrAction")) return XR_ERROR_HANDLE_INVALID;
  venture::xr::session().finish_host_end();
  forget_action(handle);
  forget_handle(handle);
  return XR_SUCCESS;
}
XrResult xrSuggestInteractionProfileBindings(XrInstance instance, const XrInteractionProfileSuggestedBinding* info) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(instance, "XrInstance")) return XR_ERROR_HANDLE_INVALID;
  if (XR_FAILED(validate_struct(info, XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING)) ||
      (info->countSuggestedBindings && !info->suggestedBindings))
    return XR_ERROR_VALIDATION_FAILURE;
  auto& L = layer();
  for (std::uint32_t i = 0; i < info->countSuggestedBindings; ++i)
    if (!valid_handle(info->suggestedBindings[i].action, "XrAction") || !L.actions.contains(info->suggestedBindings[i].action))
      return XR_ERROR_HANDLE_INVALID;
  if (L.ever_attached) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
  const auto profile = name_of(info->interactionProfile);
  if (profile.empty()) return XR_ERROR_PATH_INVALID;
  for (std::uint32_t i = 0; i < info->countSuggestedBindings; ++i)
    if (name_of(info->suggestedBindings[i].binding).empty()) return XR_ERROR_PATH_INVALID;
  venture::xr::clear_haptic_bindings(session().handle);
  {
    std::set<XrAction> actions;
    for (std::uint32_t i = 0; i < info->countSuggestedBindings; ++i) actions.insert(info->suggestedBindings[i].action);
    log::info("xr", "Game suggests {}: {} bindings, {} actions", profile, info->countSuggestedBindings, actions.size());
  }
  std::optional<std::vector<XrActionSuggestedBinding>> previous;
  if (auto it = L.suggestions.find(info->interactionProfile); it != L.suggestions.end()) previous = it->second;
  L.suggestions[info->interactionProfile].assign(info->suggestedBindings,
                                                 info->suggestedBindings + info->countSuggestedBindings);
  L.plans_valid = false;
  auto restore = [&] {
    if (previous) L.suggestions[info->interactionProfile] = std::move(*previous);
    else L.suggestions.erase(info->interactionProfile);
    L.plans_valid = false;
  };
  const auto targets = touch_targets();
  const bool target = std::ranges::find(targets, profile) != targets.end();
  XrResult result = XR_SUCCESS;
  if (quest_rank(profile) >= 0) {
    // The fixed inputs carry it: the host bindings change only for a new optional input.
    const auto optional = optional_inputs();
    const bool changed = optional != L.optional;
    L.optional = optional;
    result = suggest_targets(changed);
    refresh_menus();
  } else if (target) {
    // The game binds this host controller itself, beside the fixed inputs.
    result = suggest_targets(false);
    if (XR_SUCCEEDED(result)) result = suggest_target(info->interactionProfile, profile);
    if (XR_FAILED(result)) {
      restore();
      suggest_target(info->interactionProfile, profile);
    }
  } else {
    // A profile Venture translates nothing onto (hand interaction, eye gaze): the
    // game's bindings as they are, on its own host actions.
    if (auto fixed = suggest_targets(false); XR_FAILED(fixed)) log::warn("xr", "Controller bindings failed: {}", int(fixed));
    std::vector<XrActionSuggestedBinding> own;
    for (std::uint32_t i = 0; i < info->countSuggestedBindings; ++i)
      if (const auto host = own_action(info->suggestedBindings[i].action))
        own.push_back({host, info->suggestedBindings[i].binding});
      else
        result = XR_ERROR_PATH_UNSUPPORTED;
    if (XR_SUCCEEDED(result)) {
      XrInteractionProfileSuggestedBinding copy = *info;
      copy.next = nullptr;
      copy.countSuggestedBindings = static_cast<std::uint32_t>(own.size());
      copy.suggestedBindings = own.data();
      result = venture::xr::suggest_with_menu_chord(instance, &copy);
      if (XR_SUCCEEDED(result)) {
        venture::xr::remember_menu_bindings(instance, info->interactionProfile, &copy);
        venture::xr::remember_grip_bindings(instance, info->interactionProfile, &copy);
      }
    }
    if (XR_FAILED(result)) restore();
  }
  return result;
}
XrResult xrAttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo* info) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(session, "XrSession")) return XR_ERROR_HANDLE_INVALID;
  if (!info || XR_FAILED(validate_struct(info, XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO)) ||
      (info->countActionSets && !info->actionSets))
    return XR_ERROR_VALIDATION_FAILURE;
  auto& L = layer();
  for (std::uint32_t i = 0; i < info->countActionSets; ++i)
    if (!valid_handle(info->actionSets[i], "XrActionSet") || !L.sets.contains(info->actionSets[i]))
      return XR_ERROR_HANDLE_INVALID;
  if (!state().attached_sets[session].empty()) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
  if (auto result = suggest_targets(false); XR_FAILED(result)) return result;
  std::vector<XrActionSet> sets{L.set};
  for (std::uint32_t i = 0; i < info->countActionSets; ++i)
    if (const auto host = L.sets.at(info->actionSets[i])->host) sets.push_back(host);
  XrSessionActionSetsAttachInfo copy{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr,
                                     static_cast<std::uint32_t>(sets.size()), sets.data()};
  const auto result = venture::xr::attach_with_menu_chord(session, &copy);
  if (XR_FAILED(result)) return result;
  venture::xr::clear_haptic_bindings(session);
  for (std::uint32_t i = 0; i < info->countActionSets; ++i) state().attached_sets[session].insert(key(info->actionSets[i]));
  L.ever_attached = true;
  L.session = session;
  L.values.clear();
  L.profiles_stale = true;
  L.plans_valid = false;
  build_plans();
  return result;
}
// The system keyboard takes the controllers while it is open: each hand's aim
// ray and trigger work its keys, and the game's actions read inactive, as on a
// Quest where the keyboard has input focus.
void point_keyboard(XrSession session, XrTime now) {
  auto& L = layer();
  if (!L.set || !L.host[aim_pose] || !L.host[trigger]) return;
  for (int hand = 0; hand < 2; ++hand) {
    auto& aim = L.keyboard_aim[std::size_t(hand)];
    if (!aim) {
      XrActionSpaceCreateInfo ci{XR_TYPE_ACTION_SPACE_CREATE_INFO, nullptr, L.host[aim_pose],
                                 L.hands[std::size_t(hand)], {{0, 0, 0, 1}, {0, 0, 0}}};
      if (XR_FAILED(::xrCreateActionSpace(session, &ci, &aim))) aim = {};
    }
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, L.host[trigger], L.hands[std::size_t(hand)]};
    XrActionStateFloat value{XR_TYPE_ACTION_STATE_FLOAT};
    const float pulled = XR_SUCCEEDED(::xrGetActionStateFloat(session, &gi, &value)) && value.isActive
                             ? value.currentState : 0.f;
    if (venture::xr::keyboard::point(hand, aim, pulled, now))
      venture::xr::keyboard::tick("xr_core", session, L.host[haptic], L.hands[std::size_t(hand)]);
  }
}
XrResult xrSyncActions(XrSession session, const XrActionsSyncInfo* info) {
  xr_call_trace::Call trace("guest.xrSyncActions");
  return trace.finish([&]() -> XrResult {
    std::lock_guard lock(state().mutex);
    if (!valid_handle(session, "XrSession")) return XR_ERROR_HANDLE_INVALID;
    if (!info || XR_FAILED(validate_struct(info, XR_TYPE_ACTIONS_SYNC_INFO)) ||
        (info->countActiveActionSets && !info->activeActionSets))
      return XR_ERROR_VALIDATION_FAILURE;
    auto& L = layer();
    auto& attached = state().attached_sets[session];
    std::map<XrActionSet, ActiveSet> active;
    std::vector<XrActiveActionSet> host_sets;
    if (L.set) host_sets.push_back({L.set, XR_NULL_PATH});
    for (std::uint32_t i = 0; i < info->countActiveActionSets; ++i) {
      const auto& entry = info->activeActionSets[i];
      const auto set = L.sets.find(entry.actionSet);
      if (!valid_handle(entry.actionSet, "XrActionSet") || set == L.sets.end()) return XR_ERROR_HANDLE_INVALID;
      if (!attached.contains(key(entry.actionSet))) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
      unsigned hands = 3;
      if (entry.subactionPath) {
        if (name_of(entry.subactionPath).empty()) return XR_ERROR_PATH_INVALID;
        bool declared = false;
        for (const auto action : set->second->actions)
          declared |= std::ranges::find(L.actions.at(action)->subactions, entry.subactionPath) !=
                      L.actions.at(action)->subactions.end();
        if (!declared) return XR_ERROR_PATH_UNSUPPORTED;
        const int hand = hand_of(entry.subactionPath);
        hands = hand >= 0 ? 1u << hand : 0u;
      }
      auto& a = active[entry.actionSet];
      a.hands |= hands;
      a.priority = set->second->priority;
      if (set->second->host) host_sets.push_back({set->second->host, entry.subactionPath});
    }
    // XR_EXT_active_action_set_priority: the game's overrides for its sets, here and on the host.
    XrActiveActionSetPrioritiesEXT host_priorities{XR_TYPE_ACTIVE_ACTION_SET_PRIORITIES_EXT};
    std::vector<XrActiveActionSetPriorityEXT> overrides;
    if (enabled("XR_EXT_active_action_set_priority"))
      for (auto* node = static_cast<const XrBaseInStructure*>(info->next); node; node = node->next)
        if (node->type == XR_TYPE_ACTIVE_ACTION_SET_PRIORITIES_EXT) {
          const auto* p = reinterpret_cast<const XrActiveActionSetPrioritiesEXT*>(node);
          for (std::uint32_t i = 0; p->actionSetPriorities && i < p->actionSetPriorityCount; ++i) {
            const auto& o = p->actionSetPriorities[i];
            if (auto a = active.find(o.actionSet); a != active.end()) a->second.priority = o.priorityOverride;
            if (auto set = L.sets.find(o.actionSet); set != L.sets.end() && set->second->host)
              overrides.push_back({set->second->host, o.priorityOverride});
          }
        }
    XrActionsSyncInfo copy{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, static_cast<std::uint32_t>(host_sets.size()), host_sets.data()};
    if (!overrides.empty() && host_xr().enabled_extensions.contains("XR_EXT_active_action_set_priority")) {
      host_priorities.actionSetPriorityCount = static_cast<std::uint32_t>(overrides.size());
      host_priorities.actionSetPriorities = overrides.data();
      copy.next = &host_priorities;
    }
    const auto result = host_sets.empty() ? XR_SUCCESS : venture::xr::sync_with_menu_chord(session, &copy);
    if (XR_FAILED(result)) return result;
    if (result != XR_SUCCESS) active.clear();  // not focused: every action inactive
    ++L.syncs;
    if (result == XR_SUCCESS && (L.profiles_stale || L.syncs % 128 == 1)) {
      L.profiles_stale = false;
      refresh_profiles(session);
    }
    XrTime now = 0;
    if (XR_FAILED(venture::xr::monotonic_to_time(venture::monotonic_ns(), &now))) now = 0;
    if (result == XR_SUCCESS) {
      read_inputs(session);
      split_pads();
      shape_inputs(now);
    }
    else L.inputs = {};
    if (venture::xr::keyboard::shown()) [[unlikely]] {
      if (result == XR_SUCCESS) point_keyboard(session, now);
      active.clear();
    }
    compute_values(session, active, now);
    return result;
  }());
}
XrResult xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo* info, XrActionStateBoolean* out) {
  std::lock_guard lock(state().mutex);
  if (!out || XR_FAILED(validate_struct(out, XR_TYPE_ACTION_STATE_BOOLEAN))) return XR_ERROR_VALIDATION_FAILURE;
  XrResult result;
  const auto* value = stored(session, info, kBool, result);
  if (!value) return result;
  out->currentState = value->active && value->value > 0.5f;
  out->changedSinceLastSync = value->changed;
  out->lastChangeTime = value->time;
  out->isActive = value->active;
  return XR_SUCCESS;
}
XrResult xrGetActionStateFloat(XrSession session, const XrActionStateGetInfo* info, XrActionStateFloat* out) {
  std::lock_guard lock(state().mutex);
  if (!out || XR_FAILED(validate_struct(out, XR_TYPE_ACTION_STATE_FLOAT))) return XR_ERROR_VALIDATION_FAILURE;
  XrResult result;
  const auto* value = stored(session, info, kFloat, result);
  if (!value) return result;
  out->currentState = value->active ? value->value : 0;
  out->changedSinceLastSync = value->changed;
  out->lastChangeTime = value->time;
  out->isActive = value->active;
  return XR_SUCCESS;
}
XrResult xrGetActionStateVector2f(XrSession session, const XrActionStateGetInfo* info, XrActionStateVector2f* out) {
  std::lock_guard lock(state().mutex);
  if (!out || XR_FAILED(validate_struct(out, XR_TYPE_ACTION_STATE_VECTOR2F))) return XR_ERROR_VALIDATION_FAILURE;
  XrResult result;
  const auto* value = stored(session, info, kVector, result);
  if (!value) return result;
  out->currentState = value->active ? value->vector : XrVector2f{};
  out->changedSinceLastSync = value->changed;
  out->lastChangeTime = value->time;
  out->isActive = value->active;
  return XR_SUCCESS;
}
XrResult xrGetActionStatePose(XrSession session, const XrActionStateGetInfo* info, XrActionStatePose* out) {
  std::lock_guard lock(state().mutex);
  if (!out || XR_FAILED(validate_struct(out, XR_TYPE_ACTION_STATE_POSE))) return XR_ERROR_VALIDATION_FAILURE;
  XrResult result;
  const auto* value = stored(session, info, kPose, result);
  if (!value) return result;
  out->isActive = value->active;
  return XR_SUCCESS;
}
XrResult xrEnumerateBoundSourcesForAction(XrSession handle, const XrBoundSourcesForActionEnumerateInfo* info,
                                          std::uint32_t capacity, std::uint32_t* count, XrPath* out) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(handle, "XrSession")) return XR_ERROR_HANDLE_INVALID;
  if (!count || (capacity && !out) || XR_FAILED(validate_struct(info, XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO)))
    return XR_ERROR_VALIDATION_FAILURE;
  auto& L = layer();
  const auto action = L.actions.find(info->action);
  if (!valid_handle(info->action, "XrAction") || action == L.actions.end()) return XR_ERROR_HANDLE_INVALID;
  if (!state().attached_sets[handle].contains(key(action->second->set))) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
  // The game sees its own Touch paths, where the host controller of that hand carries them.
  if (L.profiles_stale) {
    L.profiles_stale = false;
    refresh_profiles(handle);
  }
  std::vector<XrPath> sources;
  for (int hand = 0; hand < 2; ++hand) {
    const auto profile = L.profiles[std::size_t(hand)];
    const auto bound = L.bound.find(profile);
    if (!profile || bound == L.bound.end()) continue;
    const auto& plan = plan_for(hand);
    if (plan.covered.contains(info->action)) continue;
    if (auto list = plan.sources.find(info->action); list != plan.sources.end())
      for (const auto& source : list->second)
        if (source.hand == hand && ((bound->second[source.input] & (1u << hand)) ||
                                    (source.input == menu && venture::xr::menu_chord_enabled())) &&
            std::ranges::find(sources, source.guest) == sources.end())
          sources.push_back(source.guest);
  }
  if (action->second->host) {
    XrBoundSourcesForActionEnumerateInfo host_info{XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO, nullptr, action->second->host};
    std::uint32_t size = 0;
    if (XR_SUCCEEDED(::xrEnumerateBoundSourcesForAction(handle, &host_info, 0, &size, nullptr)) && size) {
      std::vector<XrPath> own(size);
      if (XR_SUCCEEDED(::xrEnumerateBoundSourcesForAction(handle, &host_info, size, &size, own.data())))
        for (std::uint32_t i = 0; i < size && i < own.size(); ++i)
          if (std::ranges::find(sources, own[i]) == sources.end()) sources.push_back(own[i]);
    }
  }
  *count = static_cast<std::uint32_t>(sources.size());
  if (capacity && capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT;
  if (capacity) std::copy(sources.begin(), sources.end(), out);
  return XR_SUCCESS;
}
XrResult xrGetCurrentInteractionProfile(XrSession handle, XrPath path, XrInteractionProfileState* out) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(handle, "XrSession")) return XR_ERROR_HANDLE_INVALID;
  if (XR_FAILED(validate_struct(out, XR_TYPE_INTERACTION_PROFILE_STATE))) return XR_ERROR_VALIDATION_FAILURE;
  const auto result = ::xrGetCurrentInteractionProfile(handle, path, out);
  if (XR_SUCCEEDED(result) && out->interactionProfile) {
    build_plans();
    const auto& plans = layer().plans;
    if (auto it = plans.find(out->interactionProfile); it != plans.end() && it->second.reported)
      out->interactionProfile = it->second.reported;
  }
  return result;
}
}  // namespace venture::hle::openxr::guest
