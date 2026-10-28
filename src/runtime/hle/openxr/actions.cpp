#include "xr/menu_chord.h"
#include <algorithm>
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
XrResult xrSuggestInteractionProfileBindings(XrInstance instance,
                                             const XrInteractionProfileSuggestedBinding *info) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(instance, "XrInstance"))
    return XR_ERROR_HANDLE_INVALID;
  if (XR_FAILED(validate_struct(info, XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING)) ||
      (info->countSuggestedBindings && !info->suggestedBindings))
    return XR_ERROR_VALIDATION_FAILURE;
  venture::xr::clear_haptic_bindings(session().handle);
  auto profile = path_string(instance, info->interactionProfile);
  bool declares_menu = false;
  for (std::uint32_t i = 0; i < info->countSuggestedBindings; ++i) {
    if (!valid_handle(info->suggestedBindings[i].action, "XrAction"))
      return XR_ERROR_HANDLE_INVALID;
    const auto path = path_string(instance, info->suggestedBindings[i].binding);
    declares_menu |= path == "/user/hand/left/input/menu/click" ||
                     path == "/user/hand/right/input/menu/click";
  }
  if (!is_touch_profile(profile)) {
    auto result = venture::xr::suggest_with_menu_chord(instance, info);
    if (XR_SUCCEEDED(result)) venture::xr::remember_menu_bindings(instance, info->interactionProfile, info);
    return result;
  }
  std::vector<std::string> targets{"/interaction_profiles/oculus/touch_controller",
                                   "/interaction_profiles/valve/index_controller",
                       "/interaction_profiles/htc/vive_controller",
                                   "/interaction_profiles/khr/simple_controller"};
  if (host_xr().enabled_extensions.contains("XR_KHR_generic_controller"))
    targets.emplace_back("/interaction_profiles/khr/generic_controller");
  XrResult first = XR_ERROR_PATH_UNSUPPORTED;
  bool accepted = false;
  for (auto &target : targets) {
    std::vector<XrActionSuggestedBinding> bindings;
    std::map<std::pair<XrAction, XrPath>, XrPath> sources;
    for (std::uint32_t i = 0; i < info->countSuggestedBindings; ++i) {
      auto binding = info->suggestedBindings[i];
      auto mapped = touch_binding(path_string(instance, binding.binding), target);
      if (!mapped)
        continue;
      if (XR_SUCCEEDED(::xrStringToPath(instance, mapped->c_str(), &binding.binding))) {
        bindings.push_back(binding);
        sources[{binding.action, binding.binding}] = info->suggestedBindings[i].binding;
      }
    }
    if (bindings.empty() && !venture::xr::menu_chord_forced() &&
        !(declares_menu && venture::xr::menu_chord_enabled()))
      continue;
    XrInteractionProfileSuggestedBinding copy{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    if (XR_FAILED(::xrStringToPath(instance, target.c_str(), &copy.interactionProfile)))
      continue;
    copy.countSuggestedBindings = static_cast<std::uint32_t>(bindings.size());
    copy.suggestedBindings = bindings.data();
    auto result = venture::xr::suggest_with_menu_chord(instance, &copy);
    log::debug("xr", "Bindings {} -> {}: {}", profile, target, int(result));
    if (target == targets.front())
      first = result;
    if (XR_SUCCEEDED(result)) {
      accepted = true;
      venture::xr::remember_menu_bindings(instance, copy.interactionProfile, info);
      state().guest_profiles[copy.interactionProfile] = info->interactionProfile;
      for (auto &[key, path] : sources)
        state().guest_sources[key] = path;
    }
  }
  return accepted ? XR_SUCCESS : first;
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
  if (venture::xr::menu_chord_enabled()) {
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
  auto r = ::xrEnumerateBoundSourcesForAction(handle, info, capacity, count, out);
  if (r == XR_SUCCESS && capacity)
    for (std::uint32_t i = 0; i < *count; ++i) {
      auto it = state().guest_sources.find({info->action, out[i]});
      if (it != state().guest_sources.end())
        out[i] = it->second;
    }
  return r;
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
