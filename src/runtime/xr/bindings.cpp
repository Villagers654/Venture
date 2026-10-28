#include "xr/bindings.h"
namespace venture::xr {
bool is_touch_profile(std::string_view p) {
  return p == "/interaction_profiles/oculus/touch_controller" ||
         p == "/interaction_profiles/facebook/touch_controller_pro" ||
         p == "/interaction_profiles/meta/touch_controller_plus";
}
std::optional<std::string> touch_binding(std::string_view path, std::string_view profile) {
  bool left = path.starts_with("/user/hand/left/");
  if (!left && !path.starts_with("/user/hand/right/"))
    return {};
  auto offset = path.find('/', 11); // slash after left/right
  if (offset == std::string_view::npos)
    return {};
  auto component = path.substr(offset);
  std::string root(path.substr(0, offset));
  if (profile == "/interaction_profiles/khr/generic_controller") {
    if (component == "/input/a/click" || component == "/input/x/click")
      return root + "/input/primary/click";
    if (component == "/input/b/click" || component == "/input/y/click")
      return root + "/input/secondary/click";
    for (auto allowed : {"/input/grip/pose", "/input/aim/pose", "/output/haptic",
                         "/input/thumbstick", "/input/thumbstick/x", "/input/thumbstick/y",
                         "/input/thumbstick/click", "/input/trigger/value", "/input/squeeze/value"})
      if (component == allowed)
        return std::string(path);
    return {};
  }
  if (profile == "/interaction_profiles/htc/vive_controller") {
    if (component == "/input/squeeze/value") return root + "/input/squeeze/click";
    if (component.starts_with("/input/thumbstick"))
      return root + "/input/trackpad" + std::string(component.substr(17));
    for (auto allowed : {"/input/grip/pose", "/input/aim/pose", "/input/trigger/value",
                         "/input/menu/click", "/output/haptic"})
      if (component == allowed) return std::string(path);
    return {};
  }
  if (profile == "/interaction_profiles/khr/simple_controller") {
    if (component == "/input/trigger/value" || component == "/input/trigger/click")
      return root + "/input/select/click";
    if (component == "/input/grip/pose" || component == "/input/aim/pose" ||
        component == "/output/haptic")
      return std::string(path);
    return {};
  }
  if (profile == "/interaction_profiles/valve/index_controller") {
    if (component == "/input/menu/click" || component == "/input/system/click")
      return {};
    if (component == "/input/x/click")
      return root + "/input/a/click";
    if (component == "/input/y/click")
      return root + "/input/b/click";
    if (component == "/input/x/touch")
      return root + "/input/a/touch";
    if (component == "/input/y/touch")
      return root + "/input/b/touch";
    if (component == "/input/thumbrest/touch")
      return root + "/input/trackpad/touch";
  }
  // Registry-defined common Touch components. Pro pinch/force/stylus are not
  // promoted to ordinary Touch controls merely by changing the profile name.
  for (auto allowed : {"/input/grip/pose",        "/input/aim/pose",      "/input/trigger/value",
                       "/input/trigger/touch",    "/input/squeeze/value", "/input/thumbstick",
                       "/input/thumbstick/x",     "/input/thumbstick/y",  "/input/thumbstick/click",
                       "/input/thumbstick/touch", "/input/a/click",       "/input/a/touch",
                       "/input/b/click",          "/input/b/touch",       "/input/x/click",
                       "/input/x/touch",          "/input/y/click",       "/input/y/touch",
                       "/input/thumbrest/touch",  "/input/menu/click",    "/output/haptic"}) {
    if (component == allowed)
      return std::string(path);
  }
  return {};
}
} // namespace venture::xr
