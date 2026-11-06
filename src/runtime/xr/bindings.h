#pragma once
#include "common/controls.h"
#include "xr/api.h"
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace venture::xr {
// The host path a game's Touch path reaches on host profile `profile`, under the
// player's controls (VENTURE_CONTROLS, else the defaults in common/controls.h).
// Empty result means the profile has no corresponding physical input. Never
// steal runtime system/menu input or pretend advanced Touch sensors exist.
std::optional<std::string> touch_binding(std::string_view path, std::string_view profile);
bool is_touch_profile(std::string_view profile);
// The host profiles a Touch game's bindings are suggested for, Touch first;
// those the host does not define (a missing extension) are left out.
std::vector<std::string> touch_targets();
// The player's controls in effect on a host profile, or none for one Venture
// does not remap (khr/simple_controller, profiles a game binds itself).
std::optional<controls::Resolved> resolved_controls(std::string_view profile);
// The host binds trackpad and stick halves (XR_EXT_dpad_binding).
bool host_dpad();
// Suggests translated Touch bindings for host profile `profile`. A host that
// refuses a path gets the suggestion again without touch sensors, then with
// only the controller's listed inputs, so one unknown sensor never costs a
// whole controller.
XrResult suggest_translated(XrInstance instance, XrPath profile, std::vector<XrActionSuggestedBinding> bindings);
// The controller the runtime reports, for the launcher's Controls page
// (VENTURE_CONTROLLER_REPORT names the file). Written when it changes.
void report_controllers(XrInstance instance, XrPath left, XrPath right);
} // namespace venture::xr
