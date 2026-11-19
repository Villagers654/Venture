#pragma once
#include "common/controls.h"
#include "xr/api.h"
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace venture::xr {
// The host paths a game's Touch path reaches on host profile `profile`, under the
// player's controls (VENTURE_CONTROLS, else the defaults in common/controls.h):
// one per host input the player gave its Quest input, each suggested for the
// game's action (OpenXR combines them). Empty means the profile has no
// corresponding physical input. Never steal runtime system/menu input or
// pretend advanced Touch sensors exist.
std::vector<std::string> touch_bindings(std::string_view path, std::string_view profile);
bool is_touch_profile(std::string_view profile);
// The host profiles a Touch game's bindings are suggested for, Touch first;
// those the host does not define (a missing extension) are left out.
std::vector<std::string> touch_targets();
// The player's controls in effect on a host profile, or none for one Venture
// does not remap (khr/simple_controller, profiles a game binds itself). Where
// the host runtime's render model names another controller with the same
// inputs (controls::evident_profile), that controller's controls.
std::optional<controls::Resolved> resolved_controls(std::string_view profile);
// The host binds trackpad and stick halves (XR_EXT_dpad_binding).
bool host_dpad();
// Without XR_EXT_dpad_binding: the host paths of the pads one hand's rows use
// halves of on `profile` ("/user/hand/left/input/trackpad"), and their presses,
// for a pad action and a press action that split_pad reads.
std::vector<std::string> pad_bindings(std::string_view profile, int hand, bool press);
// The Quest rows of one hand a pad's halves press: the pad at `at`, pressed or
// not. All false with XR_EXT_dpad_binding (the host binds the halves).
std::array<bool, controls::kQuestCount> split_pad(std::string_view profile, int hand, XrVector2f at, bool pressed);
// The player's offset for the hand's controller (`profile`), placed before a
// pose in an action space: the controller moved and turned in the hand, then `pose`.
XrPosef offset_pose(std::string_view profile, int hand, const XrPosef& pose);
// Suggests translated Touch bindings for host profile `profile`. A host that
// refuses a path gets the suggestion again without touch sensors, then with
// only the controller's listed inputs, so one unknown sensor never costs a
// whole controller.
XrResult suggest_translated(XrInstance instance, XrPath profile, std::vector<XrActionSuggestedBinding> bindings);
// The controller in each hand, for the launcher's Controls page
// (VENTURE_CONTROLLER_REPORT names the file), written when it changes: the
// host's current profile, or the listed controller with the same inputs that
// the host runtime's render model for the device names (controls::evident_profile;
// SteamVR reports Touch for Touch Plus and names the model). The game is told
// its own Quest 3 profile either way, and the player controls of the evident
// controller apply on the host profile (resolved_controls).
void report_controllers(XrInstance instance, XrPath left, XrPath right);
// Tests: the render models the host runtime gives the controllers (left, right)
// in place of the runtime's own answer; nullopt reads the runtime again.
void override_controller_models(std::optional<std::array<std::string, 2>> models);
} // namespace venture::xr
