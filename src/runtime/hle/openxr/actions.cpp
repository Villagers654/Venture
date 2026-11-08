#include "xr/menu_chord.h"
#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include "xr/haptic_output.h"
#include "common/log.h"
#include "hle/openxr/state.h"
#include "xr/bindings.h"

namespace venture::hle::openxr::guest {
namespace {
std::string path_string(XrInstance instance, XrPath path) {
  char text[XR_MAX_PATH_LENGTH]{};
  std::uint32_t count = 0;
  if (XR_FAILED(::xrPathToString(instance, path, sizeof(text), &count, text)))
    return {};
  return text;
}
} // namespace
XrResult create_action_for_host(XrActionSet set, const XrActionCreateInfo* info, XrAction* out) {
  if (info->actionType != XR_ACTION_TYPE_VIBRATION_OUTPUT || info->countSubactionPaths)
    return host_call<PFN_xrCreateAction, "xrCreateAction">(set, info, out);
  // An aggregate output still targets all its bound devices with XR_NULL_PATH.
  // Explicit native subpaths additionally support SDKs that select a hand at
  // delivery time. Retain all core roots, including head and gamepad; changing
  // this to a hand-only list would restrict otherwise valid aggregate bindings.
  std::array<XrPath, 4> roots{};
  constexpr const char* names[]{"/user/head", "/user/hand/left", "/user/hand/right", "/user/gamepad"};
  for (std::size_t i = 0; i < roots.size(); ++i) {
    const auto result = ::xrStringToPath(host_xr().instance, names[i], &roots[i]);
    if (XR_FAILED(result)) return result;
  }
  auto native = *info;
  native.countSubactionPaths = static_cast<std::uint32_t>(roots.size());
  native.subactionPaths = roots.data();
  const auto result = host_call<PFN_xrCreateAction, "xrCreateAction">(set, &native, out);
  if (result == XR_ERROR_PATH_UNSUPPORTED) {
    // Preserve the original aggregate contract if a host cannot declare these
    // standard roots. Never turn an API error into successful vibration.
    return host_call<PFN_xrCreateAction, "xrCreateAction">(set, info, out);
  }
  if (XR_SUCCEEDED(result))
    state().actions[*out].native_haptic_subactions.assign(roots.begin(), roots.end());
  return result;
}
namespace {
// The order a Quest 3 answers in for its Touch Plus controllers: Touch Plus where
// the game binds it, else Touch, which every Touch controller also answers to.
// Pro last: a Quest 3's controllers never report it. Venture presents a Quest 3
// whatever controller the host has.
int quest_rank(std::string_view profile) {
  if (profile == "/interaction_profiles/meta/touch_controller_plus") return 0;
  if (profile == "/interaction_profiles/oculus/touch_controller") return 1;
  return 2;
}
// What Venture suggests on one host profile, rebuilt from every guest suggestion.
struct TargetPlan {
  bool native = false;          // the game suggested this host profile itself
  XrPath reported = XR_NULL_PATH;  // the Touch profile the game is told is current
  std::size_t native_count = 0;
  std::vector<XrActionSuggestedBinding> host;        // host paths
  std::vector<XrPath> guest;                         // each host binding's guest path
  std::vector<XrActionSuggestedBinding> menu_source; // the game's paths, for the menu plan
  bool declares_menu = false;
};
TargetPlan plan_target(XrInstance instance, const std::string &target, XrPath target_path) {
  TargetPlan plan;
  std::vector<std::tuple<int, XrPath, const std::vector<XrActionSuggestedBinding> *>> touch;
  for (const auto &[profile, bindings] : state().guest_suggestions) {
    const auto name = path_string(instance, profile);
    if (is_touch_profile(name)) touch.emplace_back(quest_rank(name), profile, &bindings);
  }
  std::sort(touch.begin(), touch.end(), [](const auto &a, const auto &b) { return std::get<0>(a) < std::get<0>(b); });
  std::set<std::pair<XrAction, XrPath>> seen;
  auto add = [&](const XrActionSuggestedBinding &binding, XrPath guest) {
    if (seen.insert({binding.action, binding.binding}).second) {
      plan.host.push_back(binding);
      plan.guest.push_back(guest);
    }
  };
  // The game's own suggestion for this profile, else its first Touch profile in
  // Quest order, decides each action it binds; the others only add the actions
  // it leaves out, so every action the game made stays bound whichever profile
  // the host picks, and the profile the game is told is one whose bindings are live.
  std::set<XrAction> covered;
  auto native = state().guest_suggestions.find(target_path);
  if (native != state().guest_suggestions.end() && !is_touch_profile(target)) {
    plan.native = true;
    for (const auto &binding : native->second)
      if (valid_handle(binding.action, "XrAction")) add(binding, binding.binding);
    plan.native_count = plan.host.size();
    for (const auto &binding : native->second) covered.insert(binding.action);
  } else if (!touch.empty()) {
    plan.reported = std::get<1>(touch.front());
  }
  for (const auto &[rank, profile, bindings] : touch) {
    std::set<XrAction> mine;
    for (const auto &binding : *bindings) {
      if (covered.contains(binding.action) || !valid_handle(binding.action, "XrAction")) continue;
      mine.insert(binding.action);
      const auto path = path_string(instance, binding.binding);
      plan.declares_menu |= path == "/user/hand/left/input/menu/click" || path == "/user/hand/right/input/menu/click";
      plan.menu_source.push_back(binding);
      auto mapped = touch_binding(path, target);
      XrActionSuggestedBinding host{binding.action, XR_NULL_PATH};
      if (mapped && XR_SUCCEEDED(::xrStringToPath(instance, mapped->c_str(), &host.binding)))
        add(host, binding.binding);
    }
    covered.insert(mine.begin(), mine.end());
  }
  return plan;
}
// Suggests one host profile from every guest suggestion. No result when the
// game gave nothing that reaches it.
std::optional<XrResult> suggest_target(XrInstance instance, const std::string &target, XrPath target_path) {
  auto plan = plan_target(instance, target, target_path);
  XrInteractionProfileSuggestedBinding copy{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
  copy.interactionProfile = target_path;
  if (plan.native) {
    copy.countSuggestedBindings = static_cast<std::uint32_t>(plan.host.size());
    copy.suggestedBindings = plan.host.data();
    auto result = venture::xr::suggest_with_menu_chord(instance, &copy);
    if (XR_FAILED(result) && plan.host.size() > plan.native_count) {
      // The game's own bindings alone, and the host's verdict on them.
      plan.host.resize(plan.native_count);
      plan.guest.resize(plan.native_count);
      copy.countSuggestedBindings = static_cast<std::uint32_t>(plan.native_count);
      result = venture::xr::suggest_with_menu_chord(instance, &copy);
    }
    if (XR_SUCCEEDED(result)) {
      // The game's menu plan and profile are its own here.
      copy.countSuggestedBindings = static_cast<std::uint32_t>(plan.native_count);
      venture::xr::remember_menu_bindings(instance, target_path, &copy);
      state().guest_profiles.erase(target_path);
      for (std::size_t i = plan.native_count; i < plan.host.size(); ++i)
        state().guest_sources[{plan.host[i].action, plan.host[i].binding}] = plan.guest[i];
    }
    return result;
  }
  if (!plan.reported)
    return std::nullopt;
  // A game that binds only its menu still gets it where Venture makes one.
  if (plan.host.empty() && !venture::xr::menu_chord_forced() && !plan.declares_menu)
    return std::nullopt;
  auto result = venture::xr::suggest_translated(instance, target_path, plan.host);
  log::debug("xr", "Bindings {} -> {}: {}", path_string(instance, plan.reported), target, int(result));
  if (XR_SUCCEEDED(result)) {
    XrInteractionProfileSuggestedBinding guest{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    guest.interactionProfile = plan.reported;
    guest.countSuggestedBindings = static_cast<std::uint32_t>(plan.menu_source.size());
    guest.suggestedBindings = plan.menu_source.data();
    venture::xr::remember_menu_bindings(instance, target_path, &guest);
    state().guest_profiles[target_path] = plan.reported;
    for (std::size_t i = 0; i < plan.host.size(); ++i)
      state().guest_sources[{plan.host[i].action, plan.host[i].binding}] = plan.guest[i];
  }
  return result;
}
} // namespace
XrResult xrSuggestInteractionProfileBindings(XrInstance instance,
                                             const XrInteractionProfileSuggestedBinding *info) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(instance, "XrInstance"))
    return XR_ERROR_HANDLE_INVALID;
  if (XR_FAILED(validate_struct(info, XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING)) ||
      (info->countSuggestedBindings && !info->suggestedBindings))
    return XR_ERROR_VALIDATION_FAILURE;
  venture::xr::clear_haptic_bindings(session().handle);
  const auto profile = path_string(instance, info->interactionProfile);
  for (std::uint32_t i = 0; i < info->countSuggestedBindings; ++i)
    if (!valid_handle(info->suggestedBindings[i].action, "XrAction"))
      return XR_ERROR_HANDLE_INVALID;
  const auto targets = venture::xr::touch_targets();
  const bool touch = is_touch_profile(profile);
  if (!touch && std::find(targets.begin(), targets.end(), profile) == targets.end()) {
    // A profile Venture translates nothing onto: the game's bindings as they are.
    auto result = venture::xr::suggest_with_menu_chord(instance, info);
    if (XR_SUCCEEDED(result)) venture::xr::remember_menu_bindings(instance, info->interactionProfile, info);
    return result;
  }
  // OpenXR replaces a profile's earlier suggestion; the host profiles are
  // rebuilt from the game's current suggestion for every profile it named.
  auto &stored = state().guest_suggestions;
  std::optional<std::vector<XrActionSuggestedBinding>> previous;
  if (auto it = stored.find(info->interactionProfile); it != stored.end()) previous = it->second;
  stored[info->interactionProfile].assign(info->suggestedBindings,
                                          info->suggestedBindings + info->countSuggestedBindings);
  XrResult result = XR_ERROR_PATH_UNSUPPORTED;
  if (!touch) {
    result = suggest_target(instance, profile, info->interactionProfile).value_or(XR_ERROR_PATH_UNSUPPORTED);
  } else {
    // Every controller the host defines, each under the player's controls.
    bool accepted = false;
    for (const auto &target : targets) {
      XrPath target_path{};
      if (XR_FAILED(::xrStringToPath(instance, target.c_str(), &target_path)))
        continue;
      const auto suggested = suggest_target(instance, target, target_path);
      if (!suggested)
        continue;
      if (target == targets.front())
        result = *suggested;
      accepted |= XR_SUCCEEDED(*suggested);
    }
    if (accepted)
      result = XR_SUCCESS;
  }
  if (XR_FAILED(result)) {
    if (previous) stored[info->interactionProfile] = std::move(*previous);
    else stored.erase(info->interactionProfile);
  }
  return result;
}
XrResult xrEnumerateBoundSourcesForAction(XrSession handle,
                                          const XrBoundSourcesForActionEnumerateInfo *info,
                                          std::uint32_t capacity, std::uint32_t *count,
                                          XrPath *out) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(handle, "XrSession"))
    return XR_ERROR_HANDLE_INVALID;
  if (!count || (capacity && !out) ||
      XR_FAILED(validate_struct(info, XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO)))
    return XR_ERROR_VALIDATION_FAILURE;
  if (!valid_handle(info->action, "XrAction"))
    return XR_ERROR_HANDLE_INVALID;
  // The game sees its own Touch paths, and its menu bound where Venture makes one.
  {
    std::uint32_t size = 0;
    auto result = ::xrEnumerateBoundSourcesForAction(handle, info, 0, &size, nullptr);
    if (XR_FAILED(result)) return result;
    std::vector<XrPath> sources(size);
    if (size) {
      result = ::xrEnumerateBoundSourcesForAction(handle, info, size, &size, sources.data());
      if (XR_FAILED(result)) return result;
      for (auto& path : sources) {
        auto it = state().guest_sources.find({info->action, path});
        if (it != state().guest_sources.end()) path = it->second;
      }
    }
    for (auto path : venture::xr::menu_bound_sources(handle, info->action))
      if (std::find(sources.begin(), sources.end(), path) == sources.end()) sources.push_back(path);
    *count = static_cast<std::uint32_t>(sources.size());
    if (capacity && capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT;
    if (capacity) std::copy(sources.begin(), sources.end(), out);
    return XR_SUCCESS;
  }
}
XrResult xrGetCurrentInteractionProfile(XrSession handle, XrPath path,
                                        XrInteractionProfileState *out) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(handle, "XrSession"))
    return XR_ERROR_HANDLE_INVALID;
  if (XR_FAILED(validate_struct(out, XR_TYPE_INTERACTION_PROFILE_STATE)))
    return XR_ERROR_VALIDATION_FAILURE;
  auto result = ::xrGetCurrentInteractionProfile(handle, path, out);
  if (XR_SUCCEEDED(result)) {
    auto it = state().guest_profiles.find(out->interactionProfile);
    if (it != state().guest_profiles.end())
      out->interactionProfile = it->second;
  }
  return result;
}
} // namespace venture::hle::openxr::guest
