#include "xr/menu_chord.h"
#include "xr/grip_toggle.h"
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
std::string words(std::string_view identifier) {
  std::string out;
  bool start = true;
  for (char c : identifier) {
    if (c == '_') { out += ' '; start = true; continue; }
    out += start && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    start = false;
  }
  return out;
}
std::string profile_name(std::string_view profile) {
  if (profile == "/interaction_profiles/oculus/touch_controller") return "Oculus Touch Controller";
  if (profile == "/interaction_profiles/meta/touch_controller_plus") return "Meta Quest Touch Plus Controller";
  if (profile == "/interaction_profiles/facebook/touch_controller_pro") return "Meta Quest Touch Pro Controller";
  if (profile == "/interaction_profiles/khr/simple_controller") return "Simple Controller";
  auto slash = profile.rfind('/');
  return slash == std::string_view::npos ? std::string{} : words(profile.substr(slash + 1));
}
std::string component_name(std::string_view identifier) {
  if (identifier.size() == 1 && identifier[0] >= 'a' && identifier[0] <= 'z')
    return std::string(1, static_cast<char>(identifier[0] - 'a' + 'A')) + " Button";
  if (identifier == "menu" || identifier == "system") return words(identifier) + " Button";
  if (identifier == "squeeze") return "Grip";
  if (identifier == "thumbrest") return "Thumb Rest";
  if (identifier == "grip" || identifier == "aim") return words(identifier) + " Pose";
  if (identifier == "haptic") return "Haptics";
  return words(identifier);
}
} // namespace
// A Quest runtime names a source by hand, controller and component. Answered from the
// guest's own source paths (xrEnumerateBoundSourcesForAction returns guest Touch paths),
// which the host never bound and need not name; Delightex aborted on the mock's
// XR_ERROR_FUNCTION_UNSUPPORTED while logging its bindings.
XrResult xrGetInputSourceLocalizedName(XrSession handle, const XrInputSourceLocalizedNameGetInfo* info,
                                       std::uint32_t capacity, std::uint32_t* count, char* buffer) {
  std::lock_guard lock(state().mutex);
  if (!valid_handle(handle, "XrSession"))
    return XR_ERROR_HANDLE_INVALID;
  if (!count || (capacity && !buffer) ||
      XR_FAILED(validate_struct(info, XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO)))
    return XR_ERROR_VALIDATION_FAILURE;
  constexpr XrInputSourceLocalizedNameFlags all = XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT |
      XR_INPUT_SOURCE_LOCALIZED_NAME_INTERACTION_PROFILE_BIT | XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT;
  if (!info->whichComponents || (info->whichComponents & ~all))
    return XR_ERROR_VALIDATION_FAILURE;
  const auto instance = host_xr().instance;
  const auto source = path_string(instance, info->sourcePath);
  if (source.empty())
    return XR_ERROR_PATH_INVALID;
  struct User { std::string_view path, name; };
  constexpr User users[]{{"/user/hand/left", "Left Hand"}, {"/user/hand/right", "Right Hand"},
                         {"/user/head", "Head"}, {"/user/gamepad", "Gamepad"}};
  const User* user = nullptr;
  for (const auto& u : users)
    if (source.starts_with(u.path) && source.size() > u.path.size() && source[u.path.size()] == '/')
      user = &u;
  // A source is a component under a top-level user path: /input/<id>[/<part>] or /output/<id>.
  std::string_view rest = user ? std::string_view(source).substr(user->path.size()) : std::string_view{};
  std::string_view identifier;
  for (std::string_view kind : {"/input/", "/output/"})
    if (rest.starts_with(kind)) identifier = rest.substr(kind.size());
  identifier = identifier.substr(0, identifier.find('/'));
  if (!user || identifier.empty())
    return XR_ERROR_PATH_UNSUPPORTED;
  std::vector<std::string> parts;
  if (info->whichComponents & XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT)
    parts.emplace_back(user->name);
  if (info->whichComponents & XR_INPUT_SOURCE_LOCALIZED_NAME_INTERACTION_PROFILE_BIT) {
    XrPath user_path{};
    XrInteractionProfileState profile{XR_TYPE_INTERACTION_PROFILE_STATE};
    if (XR_SUCCEEDED(::xrStringToPath(instance, std::string(user->path).c_str(), &user_path)) &&
        XR_SUCCEEDED(guest::xrGetCurrentInteractionProfile(handle, user_path, &profile)) &&
        profile.interactionProfile != XR_NULL_PATH) {
      // The guest's xrGetCurrentInteractionProfile: the profile the game is told.
      auto name = profile_name(path_string(instance, profile.interactionProfile));
      if (!name.empty()) parts.push_back(std::move(name));
    }
  }
  if (info->whichComponents & XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT)
    parts.push_back(component_name(identifier));
  std::string name;
  for (const auto& part : parts) name += (name.empty() ? "" : " ") + part;
  *count = static_cast<std::uint32_t>(name.size() + 1);
  if (!capacity) return XR_SUCCESS;
  if (capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT;
  std::copy(name.begin(), name.end(), buffer);
  buffer[name.size()] = '\0';
  return XR_SUCCESS;
}
} // namespace venture::hle::openxr::guest
