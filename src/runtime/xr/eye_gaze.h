#pragma once
// Venture's own eye gaze (XR_EXT_eye_gaze_interaction on the host runtime), for
// eye-tracked foveated rendering (gfx/foveation_vulkan.h) with any runtime:
// SteamVR, Virtual Desktop, VDXR, Monado. Used when foveated rendering is
// Dynamic (VENTURE_FOVEATION=dynamic or unset, the default).
//
// The gaze action lives in Venture's own action set, attached and synced with
// the game's (menu_chord.cpp). Its binding joins the game's on the eye gaze
// profile when the game suggests one, and is suggested alone at attach otherwise.
#include "xr/api.h"
#include <optional>
#include <string_view>
#include <vector>
namespace venture::xr::eye_gaze {
// Dynamic foveated rendering: ask the host instance for XR_EXT_eye_gaze_interaction.
bool requested();
// Whether the host gives the gaze: its runtime reports eye gaze interaction,
// or Virtual Desktop shares its eye tracking. False until the host instance
// exists; decided once after that.
bool available();
// Venture's gaze action set once the host supports eye gaze (null otherwise). Call with
// the host instance before the game's action sets are attached.
XrActionSet action_set(XrInstance instance);
// The game suggests bindings for `profile`: Venture's gaze binding joins them on the eye gaze profile.
void add_binding(XrInstance instance, std::string_view profile, std::vector<XrActionSuggestedBinding>& bindings);
// Before the game's action sets are attached: suggest the eye gaze profile when the game did not.
void before_attach(XrInstance instance);
// After a successful attach: the gaze space of `session`.
void attached(XrSession session);
void forget_session(XrSession session);
// The gaze direction in the head's frame (x right, y up, -z ahead) at `time`, when tracked.
std::optional<XrVector3f> direction(XrTime time);
}  // namespace venture::xr::eye_gaze
