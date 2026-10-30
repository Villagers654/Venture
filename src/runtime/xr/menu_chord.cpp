#include "common/environment.h"
#include "xr/menu_chord.h"
#include "xr/haptic_output.h"
#include "xr/bindings.h"
#include "xr/host_xr.h"
#include "xr/clock.h"
#include "core/wait.h"
#include "common/log.h"
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>
namespace venture::xr {
namespace {
std::recursive_mutex mutex;
XrActionSet chord_set{};
XrAction clicks{};
std::array<XrPath, 2> hands{};
std::map<XrAction, XrActionSet> parents;
std::map<XrPath, std::map<XrAction, unsigned>> menus;
std::map<XrPath, std::map<XrAction, std::array<XrPath, 2>>> menu_paths;
// Host profiles on which the game binds a stick (or trackpad) click itself.
std::set<XrPath> stick_profiles;
bool conflict_warned = false;
struct Merged { bool value = false; XrTime changed = 0; std::uint64_t sync = 0; XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN}; };
struct Snapshot {
  MenuChord chord;
  bool pulse = false, active = false, attached = false;
  bool profiles_stale = true;
  std::uint64_t sync = 0;
  XrTime time = 0;
  std::array<XrPath, 2> profiles{};
  std::array<bool, 2> forced_hands{};
  std::array<bool, 2> chord_hands{};
  std::map<XrAction, unsigned> unbound_menus;
  std::map<XrActionSet, unsigned> active_sets;
  std::map<std::pair<XrAction, XrPath>, Merged> states;
};
std::map<XrSession, Snapshot> sessions;
std::string path_string(XrInstance instance, XrPath path) {
  char value[XR_MAX_PATH_LENGTH]{}; std::uint32_t size = 0;
  return XR_SUCCEEDED(::xrPathToString(instance, path, sizeof(value), &size, value)) ? value : "";
}
XrResult ensure(XrInstance instance) {
  if (chord_set) return XR_SUCCESS;
  XrActionSetCreateInfo ci{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strcpy(ci.actionSetName, "venture_menu_chord");
  std::strcpy(ci.localizedActionSetName, "Venture optional menu chord");
  ci.priority = 0; // Never outrank a game's higher-priority action set.
  auto result = ::xrCreateActionSet(instance, &ci, &chord_set);
  if (XR_FAILED(result)) return result;
  for (int hand = 0; hand < 2; ++hand) {
    result = ::xrStringToPath(instance, hand ? "/user/hand/right" : "/user/hand/left", &hands[hand]);
    if (XR_FAILED(result)) break;
  }
  if (XR_SUCCEEDED(result)) {
    XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
    std::strcpy(ai.actionName, "stick_clicks");
    std::strcpy(ai.localizedActionName, "Menu chord stick clicks");
    ai.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    ai.countSubactionPaths = 2; ai.subactionPaths = hands.data();
    result = ::xrCreateAction(chord_set, &ai, &clicks);
  }
  if (XR_FAILED(result)) { ::xrDestroyActionSet(chord_set); chord_set = {}; }
  return result;
}
unsigned mask(XrPath path) {
  return path == XR_NULL_PATH ? 3u : (path == hands[0] ? 1u : (path == hands[1] ? 2u : 0u));
}
// Each distinct bound source of a game menu action, once, for debug reports.
void log_bound_source(const std::string &path) {
  static std::set<std::string> logged;
  if (logged.insert(path).second) log::info("facts", "Game menu action bound to {}", path);
}
// Query only after a successful focused sync. A zero count before the first
// xrSyncActions is normal and must not become a cached fallback decision.
// Unknown/error answers keep the native binding; absence must be established.
std::optional<unsigned> native_source_hands(XrSession session, XrAction action) {
  XrBoundSourcesForActionEnumerateInfo info{XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO,
                                           nullptr, action};
  std::uint32_t count = 0;
  auto result = host_call<PFN_xrEnumerateBoundSourcesForAction, "xrEnumerateBoundSourcesForAction">(
      session, &info, 0, &count, nullptr);
  if (XR_FAILED(result) || count > 256) return {};
  if (!count) return 0u;
  std::vector<XrPath> sources(count);
  result = host_call<PFN_xrEnumerateBoundSourcesForAction, "xrEnumerateBoundSourcesForAction">(
      session, &info, count, &count, sources.data());
  if (XR_FAILED(result) || count > sources.size()) return {};
  unsigned available = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto path = path_string(host_xr().instance, sources[i]);
    if (path.empty()) return {};
    log_bound_source(path);
    // Runtimes keep the system button for themselves (SteamVR's dashboard on
    // Index controllers), so a menu bound there never reaches the game.
    if (path.ends_with("/input/system/click") || path.ends_with("/input/system/touch")) continue;
    if (path.starts_with("/user/hand/left/")) available |= 1u;
    if (path.starts_with("/user/hand/right/")) available |= 2u;
  }
  return available;
}
unsigned fallback_hands(const Snapshot &snapshot, XrAction action) {
  auto found = snapshot.unbound_menus.find(action);
  unsigned allowed = found == snapshot.unbound_menus.end() ? 0 : found->second;
  for (int hand = 0; hand < 2; ++hand)
    if (snapshot.forced_hands[hand]) allowed |= 1u << hand;
  return allowed;
}
enum class Mode : int { off, automatic, forced };
Mode mode() {
  static std::atomic<std::uint64_t> configured_generation{0};
  static std::atomic<int> configured{int(Mode::automatic)};
  const auto generation = host_procedure_generation();
  if (configured_generation.load(std::memory_order_acquire) != generation) {
    std::lock_guard lock(mutex);
    if (configured_generation.load(std::memory_order_relaxed) != generation) {
      const auto* value = venture::environment::getenv("VENTURE_MENU_CHORD");
      const std::string_view setting = value ? value : "";
      configured.store(int(setting == "off" ? Mode::off : setting == "stick-clicks" ? Mode::forced : Mode::automatic),
                       std::memory_order_relaxed);
      configured_generation.store(generation, std::memory_order_release);
    }
  }
  return Mode(configured.load(std::memory_order_relaxed));
}
// Host profiles are re-read on a profile-change event, and every 128 syncs in
// case a runtime does not report one.
constexpr std::uint64_t profile_refresh_syncs = 128;
}
bool menu_chord_enabled() { return mode() != Mode::off; }
bool menu_chord_forced() { return mode() == Mode::forced; }
void note_menu_profiles_changed(XrSession session) {
  if (!menu_chord_enabled()) return;
  std::lock_guard lock(mutex);
  auto it = sessions.find(session);
  if (it != sessions.end()) it->second.profiles_stale = true;
}
void remember_menu_bindings(XrInstance instance, XrPath profile, const XrInteractionProfileSuggestedBinding* info) {
  if (!menu_chord_enabled()) return;
  std::lock_guard lock(mutex);
  menus[profile].clear();
  menu_paths[profile].clear();
  stick_profiles.erase(profile);
  for (std::uint32_t i = 0; i < info->countSuggestedBindings; ++i) {
    const auto& binding = info->suggestedBindings[i];
    auto path = path_string(instance, binding.binding);
    if (path.ends_with("/input/thumbstick/click") || path.ends_with("/input/trackpad/click")) stick_profiles.insert(profile);
    for (int hand = 0; hand < 2; ++hand) {
      const std::string root = hand ? "/user/hand/right" : "/user/hand/left";
      if (path == root + "/input/menu/click") {
        menus[profile][binding.action] |= 1u << hand;
        menu_paths[profile][binding.action][hand] = binding.binding;
      }
    }
    if (!conflict_warned && (path.ends_with("/input/thumbstick/click") || path.ends_with("/input/trackpad/click"))) {
      conflict_warned = true;
      log::warn("xr", "Optional menu chord overlaps game stick clicks; ordinary clicks remain visible to the game");
    }
  }
}
namespace {
XrResult suggest(XrInstance instance, const XrInteractionProfileSuggestedBinding* info) {
  if (!menu_chord_enabled()) return ::xrSuggestInteractionProfileBindings(instance, info);
  std::lock_guard lock(mutex);
  auto result = ensure(instance); if (XR_FAILED(result)) return result;
  std::vector<XrActionSuggestedBinding> bindings;
  if (info->countSuggestedBindings) bindings.assign(info->suggestedBindings, info->suggestedBindings + info->countSuggestedBindings);
  auto profile = path_string(instance, info->interactionProfile);
  for (const auto& binding : bindings) {
    auto path = path_string(instance, binding.binding);
    if (!conflict_warned && (path.ends_with("/input/thumbstick/click") || path.ends_with("/input/trackpad/click"))) {
      conflict_warned = true;
      log::warn("xr", "Optional menu chord overlaps game stick clicks; ordinary clicks remain visible to the game");
    }
  }
  for (int hand = 0; hand < 2; ++hand) {
    auto source = touch_binding(std::string(hand ? "/user/hand/right" : "/user/hand/left") + "/input/thumbstick/click", profile);
    XrPath path{};
    if (source && XR_SUCCEEDED(::xrStringToPath(instance, source->c_str(), &path))) bindings.push_back({clicks, path});
  }
  auto copy = *info;
  copy.countSuggestedBindings = static_cast<std::uint32_t>(bindings.size()); copy.suggestedBindings = bindings.data();
  return ::xrSuggestInteractionProfileBindings(instance, &copy);
}
}  // namespace
XrResult suggest_with_menu_chord(XrInstance instance, const XrInteractionProfileSuggestedBinding* info) {
  auto result = suggest(instance, info);
  if (XR_SUCCEEDED(result)) note_haptic_suggestions(instance, *info);
  return result;
}
XrResult attach_with_menu_chord(XrSession session, const XrSessionActionSetsAttachInfo* info) {
  if (!menu_chord_enabled()) return host_call<PFN_xrAttachSessionActionSets, "xrAttachSessionActionSets">(session, info);
  if (info->countActionSets && !info->actionSets) return XR_ERROR_VALIDATION_FAILURE;
  std::lock_guard lock(mutex);
  auto result = ensure(host_xr().instance); if (XR_FAILED(result)) return result;
  std::vector<XrActionSet> sets;
  if (info->countActionSets) sets.assign(info->actionSets, info->actionSets + info->countActionSets);
  sets.push_back(chord_set);
  auto copy = *info; copy.countActionSets = static_cast<std::uint32_t>(sets.size()); copy.actionSets = sets.data();
  result = host_call<PFN_xrAttachSessionActionSets, "xrAttachSessionActionSets">(session, &copy);
  if (XR_SUCCEEDED(result)) sessions[session].attached = true;
  return result;
}
XrResult sync_with_menu_chord(XrSession session, const XrActionsSyncInfo* info) {
  if (!menu_chord_enabled()) return host_call<PFN_xrSyncActions, "xrSyncActions">(session, info);
  if (info->countActiveActionSets && !info->activeActionSets) return XR_ERROR_VALIDATION_FAILURE;
  std::lock_guard lock(mutex);
  auto& snapshot = sessions[session];
  if (!snapshot.attached) return host_call<PFN_xrSyncActions, "xrSyncActions">(session, info);
  std::vector<XrActiveActionSet> sets;
  if (info->countActiveActionSets) sets.assign(info->activeActionSets, info->activeActionSets + info->countActiveActionSets);
  sets.push_back({chord_set, XR_NULL_PATH});
  auto copy = *info; copy.countActiveActionSets = static_cast<std::uint32_t>(sets.size()); copy.activeActionSets = sets.data();
  auto result = host_call<PFN_xrSyncActions, "xrSyncActions">(session, &copy);
  if (XR_FAILED(result)) return result;
  ++snapshot.sync;
  snapshot.active_sets.clear();
  for (std::uint32_t i = 0; i < info->countActiveActionSets; ++i)
    snapshot.active_sets[info->activeActionSets[i].actionSet] |= mask(info->activeActionSets[i].subactionPath);
  if (result == XR_SUCCESS && (snapshot.profiles_stale || snapshot.sync % profile_refresh_syncs == 1)) {
    snapshot.profiles_stale = false;
    const bool forced = menu_chord_forced();
    for (int hand = 0; hand < 2; ++hand) {
      XrInteractionProfileState profile{XR_TYPE_INTERACTION_PROFILE_STATE};
      snapshot.profiles[hand] = XR_SUCCEEDED(::xrGetCurrentInteractionProfile(session, hands[hand], &profile)) ? profile.interactionProfile : XR_NULL_PATH;
      // Automatic also gives the chord to a game that never reads stick clicks: nothing can
      // collide with it, and a runtime may report the menu bound to a button players don't
      // know (SteamVR maps Touch's menu to the Steam Frame controller's View button).
      snapshot.forced_hands[hand] = forced || (menus.contains(snapshot.profiles[hand]) &&
                                               !stick_profiles.contains(snapshot.profiles[hand]));
    }
    snapshot.chord_hands = snapshot.forced_hands;
    snapshot.unbound_menus.clear();
    for (const auto &[action, parent] : parents) {
      unsigned requested = 0;
      for (int hand = 0; hand < 2; ++hand) {
        const auto bindings = menus.find(snapshot.profiles[hand]);
        if (snapshot.forced_hands[hand] || bindings == menus.end()) continue;
        const auto binding = bindings->second.find(action);
        if (binding != bindings->second.end()) requested |= binding->second & (1u << hand);
      }
      if (!requested) continue;
      const auto native = native_source_hands(session, action);
      if (!native) continue;
      const auto missing = requested & ~*native;
      if (!missing) continue;
      snapshot.unbound_menus[action] = missing;
      for (int hand = 0; hand < 2; ++hand)
        if (missing & (1u << hand)) snapshot.chord_hands[hand] = true;
    }
  }
  XrActionStateBoolean values[2]{{XR_TYPE_ACTION_STATE_BOOLEAN}, {XR_TYPE_ACTION_STATE_BOOLEAN}};
  const bool chord = snapshot.chord_hands[0] || snapshot.chord_hands[1];
  for (int hand = 0; hand < 2 && XR_SUCCEEDED(result) && chord; ++hand) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, clicks, hands[hand]};
    if (XR_FAILED(::xrGetActionStateBoolean(session, &gi, &values[hand]))) values[hand].isActive = false;
  }
  const auto now = venture::monotonic_ns();
  snapshot.active = result == XR_SUCCESS && chord && values[0].isActive && values[1].isActive;
  snapshot.pulse = snapshot.chord.update(now, values[0].currentState, values[1].currentState, snapshot.active);
  if (XR_FAILED(monotonic_to_time(now, &snapshot.time))) { snapshot.pulse = false; snapshot.active = false; }
  // Update every mapped menu at each sync, even if the app skips a state query.
  for (const auto& [action, parent] : parents) {
    bool mapped = false;
    for (int hand = 0; hand < 2; ++hand) {
      auto bindings = menus.find(snapshot.profiles[hand]);
      if ((fallback_hands(snapshot, action) & (1u << hand)) && bindings != menus.end() && bindings->second.contains(action)) mapped = true;
    }
    if (!mapped) continue;
    for (auto path : std::array<XrPath, 3>{XR_NULL_PATH, hands[0], hands[1]}) {
      XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, action, path};
      XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
      if (XR_SUCCEEDED(::xrGetActionStateBoolean(session, &gi, &value))) menu_action_state(session, &gi, &value);
    }
  }
  return result;
}
void register_menu_action(XrActionSet set, XrAction action) {
  if (!menu_chord_enabled()) return;
  std::lock_guard lock(mutex); parents[action] = set;
}
void forget_menu_object(std::uintptr_t object) {
  std::lock_guard lock(mutex);
  for (auto it = parents.begin(); it != parents.end();) {
    if (reinterpret_cast<std::uintptr_t>(it->first) == object || reinterpret_cast<std::uintptr_t>(it->second) == object) {
      for (auto& [profile, actions] : menus) actions.erase(it->first);
      for (auto& [profile, actions] : menu_paths) actions.erase(it->first);
      for (auto& [session, snapshot] : sessions) std::erase_if(snapshot.states, [&](auto& entry) { return entry.first.first == it->first; });
      for (auto& [session, snapshot] : sessions) snapshot.unbound_menus.erase(it->first);
      it = parents.erase(it);
    } else ++it;
  }
}
void reset_menu_session(XrSession session) { std::lock_guard lock(mutex); sessions.erase(session); }
bool menu_chord_pulse(XrSession session) {
  if (!menu_chord_enabled()) return false;
  std::lock_guard lock(mutex); auto it = sessions.find(session);
  return it != sessions.end() && it->second.active && it->second.pulse && it->second.chord_hands[0];
}
void menu_action_state(XrSession session, const XrActionStateGetInfo* info, XrActionStateBoolean* value) {
  if (!menu_chord_enabled()) return;
  std::lock_guard lock(mutex);
  auto it = sessions.find(session); auto parent = parents.find(info->action);
  if (it == sessions.end() || parent == parents.end()) return;
  auto& snapshot = it->second;
  const auto allowed = fallback_hands(snapshot, info->action);
  unsigned eligible = 0;
  for (int hand = 0; hand < 2; ++hand) {
    auto profile = menus.find(snapshot.profiles[hand]);
    if (!(allowed & (1u << hand)) || profile == menus.end()) continue;
    auto action = profile->second.find(info->action);
    if (action != profile->second.end()) eligible |= action->second & (1u << hand);
  }
  eligible &= mask(info->subactionPath) & snapshot.active_sets[parent->second];
  if (!eligible) { snapshot.states.erase({info->action, info->subactionPath}); return; }
  auto& merged = snapshot.states[{info->action, info->subactionPath}];
  if (!snapshot.active && !value->isActive) {
    merged = {};
    return;
  }
  if (merged.sync != snapshot.sync) {
    const bool current = (value->isActive && value->currentState) || (snapshot.active && snapshot.pulse);
    const bool changed = current != merged.value;
    if (changed) merged.changed = snapshot.time;
    merged.state = *value;
    merged.state.currentState = current;
    merged.state.isActive = value->isActive || snapshot.active;
    merged.state.changedSinceLastSync = changed;
    merged.state.lastChangeTime = changed ? snapshot.time : (merged.changed ? merged.changed : value->lastChangeTime);
    merged.value = current; merged.sync = snapshot.sync;
  }
  value->currentState = merged.state.currentState;
  value->isActive = merged.state.isActive;
  value->changedSinceLastSync = merged.state.changedSinceLastSync;
  value->lastChangeTime = merged.state.lastChangeTime;
}
std::vector<XrPath> menu_bound_sources(XrSession session, XrAction action) {
  std::vector<XrPath> sources;
  if (!menu_chord_enabled()) return sources;
  std::lock_guard lock(mutex);
  if (!clicks || !sessions.contains(session) || !sessions[session].attached) return sources;
  const auto allowed = fallback_hands(sessions[session], action);
  XrBoundSourcesForActionEnumerateInfo bound{XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO, nullptr, clicks};
  std::uint32_t count = 0;
  if (XR_FAILED(::xrEnumerateBoundSourcesForAction(session, &bound, 0, &count, nullptr)) || !count) return sources;
  for (int hand = 0; hand < 2; ++hand) {
    XrInteractionProfileState profile{XR_TYPE_INTERACTION_PROFILE_STATE};
    if (!(allowed & (1u << hand)) || XR_FAILED(::xrGetCurrentInteractionProfile(session, hands[hand], &profile))) continue;
    auto p = menu_paths.find(profile.interactionProfile);
    if (p == menu_paths.end()) continue;
    auto a = p->second.find(action);
    if (a != p->second.end() && a->second[hand]) sources.push_back(a->second[hand]);
  }
  return sources;
}

} // namespace venture::xr
